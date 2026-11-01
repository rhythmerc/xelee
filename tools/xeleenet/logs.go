// The log drop: the launcher's Send logs row (src/pc/log_report.cpp) posts a
// bundle here and shows the player the code it gets back. The same API the
// Cloudflare Worker in tools/log-drop had; tools/fetch_logs.py reads it.
//
//   POST /logs             a bundle (text, "XELEE-LOGS 1" first) -> {"code": "XL-7K3QM2"}
//   GET  /logs             recent uploads, newest first (admin)
//   GET  /logs/XL-7K3QM2   one bundle (admin)
//
// Off unless -logs names a folder. Reads need XELEENET_LOGS_TOKEN; without it
// nothing can be read back. Anyone can pull the URL out of the APK, so the
// per-address limit, the size caps and the expiry keep junk cheap.

package main

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"
)

const (
	logMaxBytes   = 5 << 20
	logMagic      = "XELEE-LOGS 1\n"
	logKeep       = 30 * 24 * time.Hour
	logPerMinute  = 3
	logCodeLetter = "0123456789ABCDEFGHJKMNPQRSTVWXYZ" // Crockford: no I, L, O or U
)

var (
	logCode   = regexp.MustCompile(`^XL-[0-9A-HJKMNP-TV-Z]{6}$`)
	logHeader = regexp.MustCompile(`^(version|rev|platform|os): (.{0,120})$`)
	logStrip  = regexp.MustCompile("[`*_~|<>@]")
)

type LogDrop struct {
	Dir     string
	Token   string // admin reads; "" refuses them all
	Webhook string // optional Discord webhook: a line per upload
	Cap     int64  // bytes kept in all; uploads past it are refused until some expire
	mu      sync.Mutex
}

type logMeta struct {
	Code     string `json:"code"`
	Uploaded string `json:"uploaded"`
	Size     int    `json:"size"`
	Version  string `json:"version,omitempty"`
	Rev      string `json:"rev,omitempty"`
	Platform string `json:"platform,omitempty"`
	OS       string `json:"os,omitempty"`
}

// logsCall answers /logs and /logs/<code>; false when the path isn't one.
func (s *Service) logsCall(w http.ResponseWriter, r *http.Request, ip string) bool {
	if r.URL.Path != "/logs" && !strings.HasPrefix(r.URL.Path, "/logs/") {
		return false
	}
	d := s.logs
	if d == nil {
		fail(404, "not_found", "Not found")
	}
	switch {
	case r.URL.Path == "/logs" && r.Method == "POST":
		s.limit("logs", ip, logPerMinute, "Too many uploads; try again in a minute")
		reply(w, 200, map[string]any{"code": d.upload(r)})
	case r.URL.Path == "/logs" && r.Method == "GET":
		d.admin(r)
		reply(w, 200, map[string]any{"logs": d.list()})
	case logCode.MatchString(strings.TrimPrefix(r.URL.Path, "/logs/")) && r.Method == "GET":
		d.admin(r)
		data, err := os.ReadFile(filepath.Join(d.Dir, strings.TrimPrefix(r.URL.Path, "/logs/")+".txt"))
		if err != nil {
			fail(404, "not_found", "No logs with that code (they are kept 30 days)")
		}
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		w.Write(data)
	default:
		fail(404, "not_found", "Not found")
	}
	return true
}

func (d *LogDrop) upload(r *http.Request) string {
	if r.ContentLength > logMaxBytes {
		fail(413, "too_large", "The logs are too large to send")
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, logMaxBytes+1))
	if err != nil {
		fail(400, "bad_request", "Couldn't read the logs")
	}
	if len(body) > logMaxBytes {
		fail(413, "too_large", "The logs are too large to send")
	}
	if !bytes.HasPrefix(body, []byte(logMagic)) {
		fail(400, "bad_request", "Not a Xelee log bundle")
	}
	meta := logMeta{Uploaded: time.Now().UTC().Format(time.RFC3339), Size: len(body)}
	head := string(body[:min(len(body), 2048)])
	for i, line := range strings.Split(head, "\n") {
		if i == 0 || i > 11 {
			continue
		}
		if m := logHeader.FindStringSubmatch(line); m != nil {
			v := logStrip.ReplaceAllString(m[2], "")
			switch m[1] {
			case "version":
				meta.Version = v
			case "rev":
				meta.Rev = v
			case "platform":
				meta.Platform = v
			case "os":
				meta.OS = v
			}
		}
	}

	d.mu.Lock()
	defer d.mu.Unlock()
	if d.used()+int64(len(body)) > d.Cap {
		fail(503, "full", "The log server is full; try again tomorrow")
	}
	for i := 0; i < 5 && meta.Code == ""; i++ {
		c := newLogCode()
		if _, err := os.Stat(filepath.Join(d.Dir, c+".txt")); os.IsNotExist(err) {
			meta.Code = c
		}
	}
	if meta.Code == "" {
		fail(503, "busy", "The log server is busy; try again")
	}
	m, _ := json.Marshal(meta)
	if os.WriteFile(filepath.Join(d.Dir, meta.Code+".json"), m, 0o600) != nil ||
		os.WriteFile(filepath.Join(d.Dir, meta.Code+".txt"), body, 0o600) != nil {
		fail(500, "server", "The log server couldn't keep the logs")
	}
	log.Printf("logs: %s, %d bytes, %s %s", meta.Code, len(body), meta.Platform, meta.Version)
	if d.Webhook != "" {
		go d.notify(meta)
	}
	return meta.Code
}

func (d *LogDrop) notify(m logMeta) {
	line := "New logs **" + m.Code + "** · " + orQ(m.Version) + " · " + orQ(m.Platform) + " · " +
		itoa((m.Size+1023)/1024) + " KB"
	b, _ := json.Marshal(map[string]any{"content": line, "allowed_mentions": map[string]any{"parse": []string{}}})
	c := http.Client{Timeout: 10 * time.Second}
	if resp, err := c.Post(d.Webhook, "application/json", bytes.NewReader(b)); err == nil {
		resp.Body.Close()
	}
}

func (d *LogDrop) admin(r *http.Request) {
	got := sha256.Sum256([]byte(strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")))
	want := sha256.Sum256([]byte(d.Token))
	if d.Token == "" || subtle.ConstantTimeCompare(got[:], want[:]) != 1 {
		fail(401, "unauthorized", "Not authorized")
	}
}

func (d *LogDrop) list() []logMeta {
	d.mu.Lock()
	defer d.mu.Unlock()
	out := []logMeta{}
	names, _ := filepath.Glob(filepath.Join(d.Dir, "XL-*.json"))
	for _, n := range names {
		var m logMeta
		if data, err := os.ReadFile(n); err == nil && json.Unmarshal(data, &m) == nil {
			out = append(out, m)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Uploaded > out[j].Uploaded })
	return out
}

// used is the bytes the kept bundles take (under d.mu).
func (d *LogDrop) used() int64 {
	var n int64
	names, _ := filepath.Glob(filepath.Join(d.Dir, "XL-*.txt"))
	for _, name := range names {
		if st, err := os.Stat(name); err == nil {
			n += st.Size()
		}
	}
	return n
}

// prune drops bundles past logKeep (hourly, from main).
func (d *LogDrop) prune() {
	d.mu.Lock()
	defer d.mu.Unlock()
	names, _ := filepath.Glob(filepath.Join(d.Dir, "XL-*"))
	for _, name := range names {
		if st, err := os.Stat(name); err == nil && time.Since(st.ModTime()) > logKeep {
			os.Remove(name)
		}
	}
}

func newLogCode() string {
	b := make([]byte, 6)
	rand.Read(b)
	for i := range b {
		b[i] = logCodeLetter[b[i]&31]
	}
	return "XL-" + string(b)
}

func orQ(s string) string {
	if s == "" {
		return "?"
	}
	return s
}
