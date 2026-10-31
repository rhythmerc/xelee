// Lobbies and the directory. A lobby knows who is in it and how to reach
// them, and it is the only place a join is decided. Only the host polls (the
// heartbeat); everyone else hears about joins and leaves from the host over
// the players' own links (docs/xeleenet-plan.md §3.1).
//
// Everything lives in memory behind one lock: the service holds a few
// hundred small records at most, and every call is a few map operations.
// A restart keeps the lobbies when -state names a file (main.go); the
// "Ask before joining" queue is never kept.

package main

import (
	"log"
	"math"
	"sort"
	"time"
)

// How long a decided "Ask before joining" request keeps its answer, for a
// joiner whose wait timed out just before the host answered.
const decidedKeep = 60 * time.Second

const listMax = 50

// Lobbies this close together sort as equally near; within a band, newest first.
const bandKm = 1000

type Geo struct {
	Country string   `json:"country"`
	Lat     *float64 `json:"lat"`
	Lon     *float64 `json:"lon"`
}

type Member struct {
	Key        string   `json:"key"`
	Name       string   `json:"name"`
	Tag        string   `json:"tag"`
	XR         bool     `json:"xr"`
	Candidates []string `json:"candidates"`
	Country    string   `json:"country"`
	Port       int      `json:"port"`
	Joined     int64    `json:"joined"` // ms; 0 for the host
}

type Lobby struct {
	ID       string   `json:"id"`
	Code     string   `json:"code"`
	Name     string   `json:"name"`
	Public   bool     `json:"public"`
	Ask      bool     `json:"ask"`
	Compat   Compat   `json:"compat"`
	Settings Settings `json:"settings"`
	Host     Member   `json:"host"`
	HostGeo  Geo      `json:"host_geo"`
	Members  []Member `json:"members"`
	// Seats a machine's other pads hold (docs/lan-multipad-plan.md), as
	// the host's heartbeat names them: taken, and never given to a joiner.
	Extra []int `json:"extra"`
	// Whose machine each Extra seat plays from, by that machine's port
	// (0 the host's): the list names the seat after its owner, "RHYTHM 2".
	ExtraOwner []int    `json:"extra_owner"`
	Banned     []string `json:"banned"`
	State      string   `json:"state"`
	Score      []int    `json:"score"`
	Created    int64    `json:"created"` // ms
	Beat       int64    `json:"beat"`    // ms, the last heartbeat

	pending map[string]*pending // joiner key -> request
	closed  bool
}

// A joiner waiting on "Ask before joining".
type pending struct {
	joiner  Joiner
	since   int64
	decided *answer
	done    chan struct{} // closed once decided
}

type Joiner struct {
	Member
	Compat Compat
	Geo    Geo
}

// answer is what a lobby call returns: a 200 body or a refusal.
type answer struct {
	body    any
	refusal *Refusal
}

func okBody(body any) answer            { return answer{body: body} }
func refused(r *Refusal) answer         { return answer{refusal: r} }
func okTrue() answer                    { return okBody(map[string]any{"ok": true}) }
func nowMs() int64                      { return time.Now().UnixMilli() }
func closedLobby() answer               { return refused(refuse(404, "closed", "The lobby closed")) }
func (l *Lobby) taken() int             { return 1 + len(l.Members) + len(l.Extra) }
func (l *Lobby) isHost(key string) bool { return key == l.Host.Key }

func (l *Lobby) public(m Member) map[string]any {
	// `joined` tells one stay in a seat from the next: a player who left and
	// joined again has the same key, and often the same port.
	return map[string]any{"key": m.Key, "name": m.Name, "tag": m.Tag, "xr": m.XR, "port": m.Port,
		"candidates": m.Candidates, "joined": m.Joined}
}

// --- the directory ---------------------------------------------------------------

// lobby finds an open lobby by id; nil if it closed or never was.
func (s *Service) lobby(id string) *Lobby {
	return s.lobbies[id]
}

// open adds a lobby with a fresh code. A host has one lobby at a time: the
// previous one closes (a game that crashed and hosts again leaves no ghost).
func (s *Service) open(l *Lobby) answer {
	if old := s.byHost[l.Host.Key]; old != nil {
		s.close(old, "host opened another lobby")
	}
	for l.Code == "" || s.codes[l.Code] != nil {
		l.Code = newCode()
	}
	l.ID = hexID()
	l.Created, l.Beat = nowMs(), nowMs()
	l.State = "open"
	l.Members, l.Banned = []Member{}, []string{}
	s.add(l)
	return okBody(map[string]any{"id": l.ID, "code": l.Code})
}

func (s *Service) add(l *Lobby) {
	l.pending = map[string]*pending{}
	s.lobbies[l.ID] = l
	s.codes[l.Code] = l
	s.byHost[l.Host.Key] = l
}

func (s *Service) close(l *Lobby, reason string) {
	if l.closed {
		return
	}
	l.closed = true
	for _, p := range l.pending {
		if p.decided == nil {
			s.decide(l, p, closedLobby())
		}
	}
	delete(s.lobbies, l.ID)
	delete(s.codes, l.Code)
	if s.byHost[l.Host.Key] == l {
		delete(s.byHost, l.Host.Key)
	}
	log.Printf("lobby %s closed: %s", l.ID, reason)
}

// expire closes every lobby whose host stopped heartbeating.
func (s *Service) expire() {
	s.mu.Lock()
	defer s.mu.Unlock()
	cutoff := nowMs() - s.cfg.Expire.Milliseconds()
	for _, l := range s.lobbies {
		if l.Beat <= cutoff {
			s.close(l, "expired")
		}
	}
}

// list answers the browser: the public lobbies a player can join on the same
// build, not full unless asked for, nearest first. Lobbies on other builds
// are only counted.
func (s *Service) list(c Compat, full bool, from Geo) map[string]any {
	type row struct {
		l    *Lobby
		band int
	}
	var rows []row
	others := 0
	for _, l := range s.lobbies {
		if !l.Public {
			continue
		}
		if l.Compat != c {
			others++
			continue
		}
		if !full && l.taken() >= l.Settings.Seats {
			continue
		}
		band := 0
		if d, ok := km(from, l.HostGeo); ok {
			band = int(d / bandKm)
		}
		rows = append(rows, row{l, band})
	}
	sort.Slice(rows, func(i, j int) bool {
		if rows[i].band != rows[j].band {
			return rows[i].band < rows[j].band
		}
		return rows[i].l.Created > rows[j].l.Created
	})
	out := []map[string]any{}
	for i, r := range rows {
		if i == listMax {
			break
		}
		l := r.l
		xr := l.Host.XR
		for _, m := range l.Members {
			xr = xr || m.XR
		}
		out = append(out, map[string]any{
			"code": l.Code, "name": l.Name, "host": map[string]any{"name": l.Host.Name, "tag": l.Host.Tag},
			"country": l.HostGeo.Country, "taken": l.taken(), "settings": l.Settings, "state": l.State,
			"xr": xr, "created": l.Created, "players": l.players(),
		})
	}
	return map[string]any{"lobbies": out, "other_versions": others}
}

// players is who sits in the lobby, in port order, for the browser's Details
// page (docs/xeleenet-lobby-details-plan.md): names, tags and XR only, never
// keys or addresses. A seat a machine's other pad holds is marked `extra`,
// with the port of the machine it plays from.
func (l *Lobby) players() []map[string]any {
	out := []map[string]any{{"port": 0, "name": l.Host.Name, "tag": l.Host.Tag, "xr": l.Host.XR}}
	for _, m := range l.Members {
		out = append(out, map[string]any{"port": m.Port, "name": m.Name, "tag": m.Tag, "xr": m.XR})
	}
	for i, q := range l.Extra {
		owner := 0
		if i < len(l.ExtraOwner) {
			owner = l.ExtraOwner[i]
		}
		out = append(out, map[string]any{"port": q, "extra": true, "owner": owner})
	}
	sort.Slice(out, func(a, b int) bool { return out[a]["port"].(int) < out[b]["port"].(int) })
	return out
}

func km(a, b Geo) (float64, bool) {
	if a.Lat == nil || a.Lon == nil || b.Lat == nil || b.Lon == nil {
		return 0, false
	}
	r := math.Pi / 180
	h := math.Pow(math.Sin((*b.Lat-*a.Lat)*r/2), 2) +
		math.Cos(*a.Lat*r)*math.Cos(*b.Lat*r)*math.Pow(math.Sin((*b.Lon-*a.Lon)*r/2), 2)
	return 12742 * math.Asin(math.Sqrt(h)), true
}

// --- the host ----------------------------------------------------------------------

func (s *Service) hostOnly(l *Lobby, key string) *answer {
	if l == nil {
		a := closedLobby()
		return &a
	}
	if !l.isHost(key) {
		a := refused(refuse(403, "not_host", "Only the host can do that"))
		return &a
	}
	return nil
}

type beat struct {
	State  string
	Roster []string
	Extra  []int // ports other pads hold
	Owners []int // each one's machine, by its port (missing: the host's)
	Score  []int // nil: unchanged
	XR     bool
}

// heartbeat: the host is alive. `roster` lists the guests its game is linked
// to; anyone else who joined a while ago has gone. Answers with every
// member's addresses (a new one means "start reaching them") and the pending
// joins.
func (s *Service) heartbeat(l *Lobby, key string, b beat) answer {
	if a := s.hostOnly(l, key); a != nil {
		return *a
	}
	l.Beat = nowMs()
	l.State = b.State
	l.Host.XR = b.XR
	if b.Score != nil {
		l.Score = b.Score
	}
	grace := nowMs() - s.cfg.RosterGrace.Milliseconds()
	kept := []Member{}
	for _, m := range l.Members {
		if contains(b.Roster, m.Key) || m.Joined > grace {
			kept = append(kept, m)
		}
	}
	l.Members = kept
	l.Extra, l.ExtraOwner = []int{}, []int{}
	for i, q := range b.Extra {
		held := q < 1 || q > 3
		for _, m := range l.Members {
			held = held || m.Port == q
		}
		for _, e := range l.Extra {
			held = held || e == q
		}
		if !held {
			owner := 0
			if i < len(b.Owners) && b.Owners[i] >= 0 && b.Owners[i] <= 3 && b.Owners[i] != q {
				owner = b.Owners[i]
			}
			l.Extra = append(l.Extra, q)
			l.ExtraOwner = append(l.ExtraOwner, owner)
		}
	}
	members := []map[string]any{}
	for _, m := range l.Members {
		members = append(members, l.public(m))
	}
	waiting := []map[string]any{}
	for _, p := range l.pending {
		if p.decided == nil {
			waiting = append(waiting, map[string]any{"key": p.joiner.Key, "name": p.joiner.Name,
				"tag": p.joiner.Tag, "xr": p.joiner.XR, "country": p.joiner.Geo.Country, "since": p.since})
		}
	}
	sort.Slice(waiting, func(i, j int) bool { return waiting[i]["since"].(int64) < waiting[j]["since"].(int64) })
	return okBody(map[string]any{"state": l.State, "members": members, "pending": waiting})
}

func (s *Service) setSettings(l *Lobby, key, name, vis string, ask bool, st Settings) answer {
	if a := s.hostOnly(l, key); a != nil {
		return *a
	}
	if st.Seats < l.taken() {
		return refused(refuse(409, "seats", itoa(l.taken())+" players are in the lobby; remove someone first"))
	}
	l.Name, l.Public, l.Ask, l.Settings = name, vis == "public", ask, st
	return okTrue()
}

func (s *Service) admit(l *Lobby, key, target string, accept bool) answer {
	if a := s.hostOnly(l, key); a != nil {
		return *a
	}
	p := l.pending[target]
	if p == nil || p.decided != nil {
		return refused(refuse(404, "not_pending", "Nobody is waiting to join with that key"))
	}
	result := refused(refuse(403, "declined", "The host declined"))
	if accept {
		result = s.seat(l, p.joiner)
	}
	s.decide(l, p, result)
	word := "joined"
	if result.refusal != nil {
		word = result.refusal.Reason
	}
	return okBody(map[string]any{"result": word})
}

// kick removes a player and keeps them out for the rest of the lobby's life.
func (s *Service) kick(l *Lobby, key, target string) answer {
	if a := s.hostOnly(l, key); a != nil {
		return *a
	}
	if l.isHost(target) {
		return refused(refuse(400, "bad_request", "The host can't remove themselves"))
	}
	if !contains(l.Banned, target) {
		l.Banned = append(l.Banned, target)
	}
	l.Members = without(l.Members, target)
	if p := l.pending[target]; p != nil && p.decided == nil {
		s.decide(l, p, refused(refuse(403, "banned", "The host removed you from this lobby")))
	}
	return okTrue()
}

func (s *Service) closeBy(l *Lobby, key string) answer {
	if l == nil {
		return okTrue()
	}
	if !l.isHost(key) {
		return refused(refuse(403, "not_host", "Only the host can close the lobby"))
	}
	s.close(l, "closed by host")
	return okTrue()
}

// --- joiners -------------------------------------------------------------------------

// join decides a join by code. With "Ask before joining" it returns the
// request to wait on instead.
func (s *Service) join(l *Lobby, j Joiner) (answer, *pending) {
	if l.Compat != j.Compat {
		return refused(refuse(409, "version", "This lobby is on a different Xelee version; both need the same one")), nil
	}
	if contains(l.Banned, j.Key) {
		return refused(refuse(403, "banned", "The host removed you from this lobby")), nil
	}
	if l.isHost(j.Key) {
		return refused(refuse(400, "own_lobby", "That's your own lobby")), nil
	}
	for i := range l.Members {
		if m := &l.Members[i]; m.Key == j.Key {
			// Joining again (a retry, a lost answer): same seat, fresh addresses.
			m.Candidates = j.Candidates
			return okBody(l.joined(*m)), nil
		}
	}
	if l.taken() >= l.Settings.Seats {
		return refused(refuse(409, "full", "The lobby is full")), nil
	}
	if !l.Ask {
		return s.seat(l, j), nil
	}
	p := l.pending[j.Key]
	if p == nil || p.decided != nil {
		p = &pending{joiner: j, since: nowMs(), done: make(chan struct{})}
		l.pending[j.Key] = p
	} else {
		p.joiner = j
	}
	return answer{}, p
}

// joinStatus: a joiner on "Ask before joining" asking again after "pending".
func (s *Service) joinStatus(l *Lobby, key string) (answer, *pending) {
	if l == nil {
		return closedLobby(), nil
	}
	for _, m := range l.Members {
		if m.Key == key {
			return okBody(l.joined(m)), nil
		}
	}
	p := l.pending[key]
	if p == nil {
		return refused(refuse(404, "not_pending", "Not waiting to join this lobby")), nil
	}
	return answer{}, p
}

func (s *Service) leave(l *Lobby, key string) answer {
	if l == nil {
		return closedLobby()
	}
	if l.isHost(key) {
		s.close(l, "host_left")
		return okTrue()
	}
	l.Members = without(l.Members, key)
	if p := l.pending[key]; p != nil && p.decided == nil {
		delete(l.pending, key)
	}
	return okTrue()
}

// seat gives a joiner the lowest free port (the host is port 0) and answers
// with everyone they need to reach.
func (s *Service) seat(l *Lobby, j Joiner) answer {
	if l.closed {
		return closedLobby()
	}
	if l.taken() >= l.Settings.Seats {
		return refused(refuse(409, "full", "The lobby is full"))
	}
	port := 1
	for used := true; used; {
		used = false
		for _, m := range l.Members {
			if m.Port == port {
				used = true
			}
		}
		for _, e := range l.Extra {
			if e == port {
				used = true
			}
		}
		if used {
			port++
		}
	}
	m := j.Member
	m.Country, m.Port, m.Joined = j.Geo.Country, port, nowMs()
	l.Members = append(l.Members, m)
	sort.Slice(l.Members, func(a, b int) bool { return l.Members[a].Port < l.Members[b].Port })
	return okBody(l.joined(m))
}

func (l *Lobby) joined(m Member) map[string]any {
	host := l.public(l.Host)
	host["port"] = 0
	others := []map[string]any{host}
	for _, o := range l.Members {
		if o.Key != m.Key {
			others = append(others, l.public(o))
		}
	}
	return map[string]any{"status": "joined", "id": l.ID, "code": l.Code, "name": l.Name, "port": m.Port,
		"state": l.State, "settings": l.Settings, "members": others}
}

func (s *Service) decide(l *Lobby, p *pending, result answer) {
	p.decided = &result
	close(p.done)
	time.AfterFunc(decidedKeep, func() {
		s.mu.Lock()
		defer s.mu.Unlock()
		if l.pending[p.joiner.Key] == p {
			delete(l.pending, p.joiner.Key)
		}
	})
}

// --- helpers --------------------------------------------------------------------------

func contains(list []string, key string) bool {
	for _, k := range list {
		if k == key {
			return true
		}
	}
	return false
}

func without(members []Member, key string) []Member {
	out := []Member{}
	for _, m := range members {
		if m.Key != key {
			out = append(out, m)
		}
	}
	return out
}
