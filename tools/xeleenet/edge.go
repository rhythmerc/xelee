// What the service knows about a caller beyond the request: their address,
// where it is, and how often they have called. None of it needs a particular
// host or proxy; each comes from a flag (main.go).

package main

import (
	"net"
	"net/http"
	"net/netip"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/oschwald/maxminddb-golang/v2"
)

// clientIP is the caller's address: the connection's, or the first hop of
// a header a trusted proxy sets (-ip-header, e.g. CF-Connecting-IP or
// X-Forwarded-For). Without a proxy in front, never trust the header.
func (s *Service) clientIP(r *http.Request) string {
	if h := s.cfg.IPHeader; h != "" {
		if v := strings.TrimSpace(strings.Split(r.Header.Get(h), ",")[0]); v != "" {
			return v
		}
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

// --- rate limits --------------------------------------------------------------------

// A one-minute window per (kind, key), from that key's first call. Coarse
// on purpose: these only stop floods and code guessing.
type limiter struct {
	mu   sync.Mutex
	keys map[string]*window
}

type window struct {
	start int64 // unix seconds
	count int
}

func (s *Service) limit(kind, key string, perMinute int, message string) {
	if perMinute <= 0 {
		return
	}
	l := &s.limits
	l.mu.Lock()
	now := time.Now().Unix()
	if l.keys == nil {
		l.keys = map[string]*window{}
	}
	k := kind + "|" + key
	w := l.keys[k]
	if w == nil || now-w.start >= 60 {
		w = &window{start: now}
		l.keys[k] = w
	}
	w.count++
	over := w.count > perMinute
	l.mu.Unlock()
	if over {
		fail(429, "rate_limited", message)
	}
}

// forget drops windows that have run out (every second, from main).
func (l *limiter) forget() {
	l.mu.Lock()
	defer l.mu.Unlock()
	now := time.Now().Unix()
	for k, w := range l.keys {
		if now-w.start >= 60 {
			delete(l.keys, k)
		}
	}
}

// --- geography ----------------------------------------------------------------------

// geo places a caller for the browser: the country is shown, the
// coordinates only sort the list and are never sent out. From a MaxMind-format
// city database (-geoip, e.g. DB-IP's free IP-to-City Lite), else from a
// proxy's location headers (-geo-headers), else nothing (the list is then
// newest first).
func (s *Service) geo(r *http.Request) Geo {
	if s.geodb != nil {
		addr, err := netip.ParseAddr(s.clientIP(r))
		if err == nil {
			var rec struct {
				Country struct {
					ISOCode string `maxminddb:"iso_code"`
				} `maxminddb:"country"`
				Location struct {
					Latitude  *float64 `maxminddb:"latitude"`
					Longitude *float64 `maxminddb:"longitude"`
				} `maxminddb:"location"`
			}
			if s.geodb.Lookup(addr.Unmap()).Decode(&rec) == nil {
				return Geo{rec.Country.ISOCode, rec.Location.Latitude, rec.Location.Longitude}
			}
		}
		return Geo{}
	}
	if s.cfg.GeoHeaders {
		g := Geo{Country: r.Header.Get("CF-IPCountry")}
		if g.Country == "XX" || g.Country == "T1" {
			g.Country = ""
		}
		lat, e1 := strconv.ParseFloat(r.Header.Get("CF-IPLatitude"), 64)
		lon, e2 := strconv.ParseFloat(r.Header.Get("CF-IPLongitude"), 64)
		if e1 == nil && e2 == nil {
			g.Lat, g.Lon = &lat, &lon
		}
		return g
	}
	return Geo{}
}

func openGeoDB(path string) (*maxminddb.Reader, error) {
	return maxminddb.Open(path)
}
