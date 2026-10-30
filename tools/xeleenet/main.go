// XeleeNet, the lobby service (README.md; docs/xeleenet-plan.md §3.1). One
// static binary with no outside services: it runs the same on a laptop, a
// $4 VPS or any container host.
//
//	XELEENET_TOKEN_KEY=... xeleenet -listen :8787
//	XELEENET_TOKEN_KEY=... xeleenet -tls-domain xn.example.com -state /var/lib/xeleenet/state.json
//
// Secrets come from the environment: XELEENET_TOKEN_KEY (signs session
// tokens; any long random string) and, optionally, XELEENET_NAME_BLOCKLIST
// (comma-separated words refused in player and lobby names).

package main

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/oschwald/maxminddb-golang/v2"
	"golang.org/x/crypto/acme/autocert"
)

type Config struct {
	TokenKey    []byte
	Blocklist   []string
	Expire      time.Duration // without a host heartbeat before a lobby closes
	JoinWait    time.Duration // longest a joiner waits on "Ask before joining" before "pending"
	RosterGrace time.Duration // a member the host doesn't list yet is kept this long
	PerIP       int           // calls a minute per address, every call
	Hosts       int           // lobbies opened a minute per player key
	Joins       int           // joins by code a minute per address, so codes can't be guessed
	IPHeader    string
	GeoHeaders  bool
}

type Service struct {
	cfg     Config
	mu      sync.Mutex
	lobbies map[string]*Lobby // id -> lobby
	codes   map[string]*Lobby // code -> lobby
	byHost  map[string]*Lobby // host key -> lobby
	limits  limiter
	geodb   *maxminddb.Reader
}

func newService(cfg Config) *Service {
	return &Service{cfg: cfg, lobbies: map[string]*Lobby{}, codes: map[string]*Lobby{}, byHost: map[string]*Lobby{}}
}

func main() {
	var cfg Config
	listen := flag.String("listen", "127.0.0.1:8787", "HTTP address (ignored with -tls-domain)")
	tlsDomain := flag.String("tls-domain", "", "serve HTTPS on :443 for this domain with a Let's Encrypt certificate")
	tlsCache := flag.String("tls-cache", "/var/lib/xeleenet/certs", "where the certificates are kept")
	state := flag.String("state", "", "file the lobbies are saved to on shutdown and read back on start")
	stun := flag.String("stun", "", "also answer STUN on this UDP address (e.g. :3478)")
	geoip := flag.String("geoip", "", "MaxMind-format city database for the browser's country and distance sort")
	flag.BoolVar(&cfg.GeoHeaders, "geo-headers", false, "take country and coordinates from Cloudflare's visitor location headers")
	flag.StringVar(&cfg.IPHeader, "ip-header", "", "header a trusted proxy puts the client address in (CF-Connecting-IP, X-Forwarded-For)")
	flag.DurationVar(&cfg.Expire, "expire", 30*time.Second, "close a lobby after this long without a heartbeat")
	flag.DurationVar(&cfg.JoinWait, "join-wait", 20*time.Second, "hold an \"Ask before joining\" request this long")
	flag.DurationVar(&cfg.RosterGrace, "roster-grace", 20*time.Second, "keep a member the host doesn't list yet this long")
	flag.IntVar(&cfg.PerIP, "limit-ip", 120, "calls a minute per address (0: no limit)")
	flag.IntVar(&cfg.Hosts, "limit-host", 6, "lobbies opened a minute per player (0: no limit)")
	flag.IntVar(&cfg.Joins, "limit-join", 20, "joins by code a minute per address (0: no limit)")
	flag.Parse()

	cfg.TokenKey = []byte(os.Getenv("XELEENET_TOKEN_KEY"))
	if len(cfg.TokenKey) == 0 {
		log.Fatal("set XELEENET_TOKEN_KEY (any long random string; changing it signs everyone out)")
	}
	for _, w := range strings.Split(os.Getenv("XELEENET_NAME_BLOCKLIST"), ",") {
		if w = notAlnum.ReplaceAllString(strings.ToLower(strings.TrimSpace(w)), ""); w != "" {
			cfg.Blocklist = append(cfg.Blocklist, w)
		}
	}

	s := newService(cfg)
	if *geoip != "" {
		db, err := openGeoDB(*geoip)
		if err != nil {
			log.Fatalf("geoip: %v", err)
		}
		s.geodb = db
	}
	if *state != "" {
		if err := s.load(*state); err != nil {
			log.Fatalf("state: %v", err)
		}
	}
	if *stun != "" {
		if err := serveSTUN(*stun); err != nil {
			log.Fatalf("stun: %v", err)
		}
	}
	go func() {
		for range time.Tick(time.Second) {
			s.expire()
			s.limits.forget()
		}
	}()

	srv := &http.Server{Handler: s, ReadHeaderTimeout: 10 * time.Second, IdleTimeout: 120 * time.Second}
	errs := make(chan error, 2)
	if *tlsDomain != "" {
		m := &autocert.Manager{Prompt: autocert.AcceptTOS, HostPolicy: autocert.HostWhitelist(*tlsDomain),
			Cache: autocert.DirCache(*tlsCache)}
		srv.Addr = ":443"
		srv.TLSConfig = &tls.Config{GetCertificate: m.GetCertificate, NextProtos: []string{"h2", "http/1.1", "acme-tls/1"}}
		go func() { errs <- http.ListenAndServe(":80", m.HTTPHandler(nil)) }()
		go func() { errs <- srv.ListenAndServeTLS("", "") }()
		log.Printf("xeleenet on https://%s", *tlsDomain)
	} else {
		srv.Addr = *listen
		go func() { errs <- srv.ListenAndServe() }()
		log.Printf("xeleenet on http://%s", *listen)
	}

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	select {
	case err := <-errs:
		log.Fatal(err)
	case <-stop:
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	srv.Shutdown(ctx)
	if *state != "" {
		if err := s.save(*state); err != nil {
			log.Fatalf("state: %v", err)
		}
	}
}

// --- keeping lobbies across a restart ------------------------------------------------

// save writes the open lobbies. Pending "Ask before joining" requests are
// dropped: their joiners ask again and get a fresh one.
func (s *Service) save(path string) error {
	s.mu.Lock()
	list := make([]*Lobby, 0, len(s.lobbies))
	for _, l := range s.lobbies {
		list = append(list, l)
	}
	data, err := json.Marshal(list)
	s.mu.Unlock()
	if err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o600); err != nil {
		return err
	}
	log.Printf("saved %d lobbies", len(list))
	return os.Rename(tmp, path)
}

// load reads the lobbies back, each with a fresh heartbeat window: a host
// that's still there keeps its lobby, one that left lets it expire as usual.
func (s *Service) load(path string) error {
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var list []*Lobby
	if err := json.Unmarshal(data, &list); err != nil {
		return err
	}
	for _, l := range list {
		l.Beat = nowMs()
		s.add(l)
	}
	log.Printf("loaded %d lobbies", len(list))
	return os.Remove(path)
}
