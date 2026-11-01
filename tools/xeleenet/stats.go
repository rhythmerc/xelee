// Public stats for a status page: how many players are on XeleeNet and the
// lobbies they are in. No sign-in, and open to any origin, so a static page
// can poll it. It shows no more than the browser already does (public
// lobbies' names, settings and players, never codes, keys or addresses);
// private lobbies are only counted.
//
// "Online" is everyone seated in a lobby plus everyone whose game called the
// service lately (the lobby list refreshes while open; hosts heartbeat)
// without a seat: players browsing. Guests don't call once seated, which is
// why seats are counted from the lobbies themselves.

package main

import (
	"sort"
	"sync"
)

// How long a signed-in call counts its player as online.
const seenWindowMs = 60_000

// seen: player key -> the time of their last signed-in call (ms). Its own
// lock, as authorize runs outside s.mu.
type seenSet struct {
	mu   sync.Mutex
	last map[string]int64
}

func (t *seenSet) mark(key string) {
	t.mu.Lock()
	if t.last == nil {
		t.last = map[string]int64{}
	}
	t.last[key] = nowMs()
	t.mu.Unlock()
}

// recent is the keys seen within the window; older ones are dropped.
func (t *seenSet) recent() map[string]bool {
	cutoff := nowMs() - seenWindowMs
	out := map[string]bool{}
	t.mu.Lock()
	for k, at := range t.last {
		if at < cutoff {
			delete(t.last, k)
		} else {
			out[k] = true
		}
	}
	t.mu.Unlock()
	return out
}

// stats runs under s.mu.
func (s *Service) stats() map[string]any {
	browsing := s.seen.recent()
	seated, xr, open, playing, private := 0, 0, 0, 0, 0
	versions := map[string]int{}
	type row struct {
		l   *Lobby
		out map[string]any
	}
	var rows []row
	for _, l := range s.lobbies {
		delete(browsing, l.Host.Key)
		for _, m := range l.Members {
			delete(browsing, m.Key)
		}
		seated += l.taken()
		if l.Host.XR {
			xr++
		}
		for _, m := range l.Members {
			if m.XR {
				xr++
			}
		}
		if l.State == "playing" {
			playing++
		} else {
			open++
		}
		versions[l.Compat.Rev]++
		if !l.Public {
			private++
			continue
		}
		rows = append(rows, row{l, map[string]any{
			"name": l.Name, "host": map[string]any{"name": l.Host.Name, "tag": l.Host.Tag},
			"country": l.HostGeo.Country, "taken": l.taken(), "settings": l.Settings, "state": l.State,
			"version": l.Compat.Rev, "created": l.Created, "players": l.players(),
		}})
	}
	sort.Slice(rows, func(i, j int) bool { return rows[i].l.Created > rows[j].l.Created })
	public := []map[string]any{}
	for i, r := range rows {
		if i == listMax {
			break
		}
		public = append(public, r.out)
	}
	return map[string]any{
		"now":      nowMs(),
		"online":   seated + len(browsing),
		"seated":   seated,
		"browsing": len(browsing),
		"xr":       xr,
		"lobbies": map[string]any{
			"total": len(s.lobbies), "open": open, "playing": playing,
			"public": len(s.lobbies) - private, "private": private,
		},
		"versions": versions,
		"public":   public,
	}
}
