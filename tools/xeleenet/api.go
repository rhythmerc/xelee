// The HTTP API. It lists public lobbies, turns a join code into a seat, and
// passes on the addresses players need to reach each other directly. It
// never carries match traffic and never decides game state.
//
//   POST   /v1/session                 {pubkey}               -> {nonce}
//   POST   /v1/session/verify          {pubkey, nonce, sig}   -> {token, expires, tag}
//   GET    /v1/lobbies?proto=&rev=&disc=[&full=1]            -> {lobbies, other_versions}
//   POST   /v1/lobbies                 host a lobby           -> {id, code}
//   POST   /v1/lobbies/<id>/heartbeat  host, every few s      -> {state, members, pending}
//   PUT    /v1/lobbies/<id>/settings   host
//   POST   /v1/lobbies/<id>/admit      host {key, accept}     (Ask before joining)
//   POST   /v1/lobbies/<id>/kick       host {key}
//   DELETE /v1/lobbies/<id>            host (or POST /v1/lobbies/<id>/close)
//   POST   /v1/join                    {code, ...}            -> joined | pending | refusal
//   GET    /v1/lobbies/<id>/join       joiner, after "pending" -> joined | pending | refusal
//   POST   /v1/lobbies/<id>/leave      member
//   GET    /healthz                    -> ok
//
// Everything but the two session calls carries "Authorization: Bearer <token>".
// A refusal is {error, reason}: `reason` is a stable word for the game,
// `error` a line it can show.

package main

import (
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"regexp"
	"strconv"
	"time"
)

const maxBody = 4096

var lobbyPath = regexp.MustCompile(`^/v1/lobbies/([0-9a-f]{32})(?:/(heartbeat|settings|admit|kick|join|leave|close))?$`)

func (s *Service) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	defer func() {
		if v := recover(); v != nil {
			if rf, ok := v.(*Refusal); ok {
				reply(w, rf.Status, rf.Body())
				return
			}
			if v == http.ErrAbortHandler {
				panic(v)
			}
			log.Printf("panic: %v", v)
			reply(w, 500, map[string]any{"error": "XeleeNet had a problem; try again", "reason": "server"})
		}
	}()
	if r.URL.Path == "/healthz" {
		w.Write([]byte("ok\n"))
		return
	}
	ip := s.clientIP(r)
	s.limit("ip", ip, s.cfg.PerIP, "Too many requests; wait a minute")
	answerWith(w, s.route(w, r, ip))
}

func (s *Service) route(w http.ResponseWriter, r *http.Request, ip string) answer {
	path, method := r.URL.Path, r.Method
	switch {
	case path == "/v1/session" && method == "POST":
		return s.session(r)
	case path == "/v1/session/verify" && method == "POST":
		return s.verify(r)
	case path == "/v1/lobbies" && method == "GET":
		return s.listCall(r)
	case path == "/v1/lobbies" && method == "POST":
		return s.hostCall(r)
	case path == "/v1/join" && method == "POST":
		return s.joinCall(r, ip)
	}
	m := lobbyPath.FindStringSubmatch(path)
	if m == nil {
		fail(404, "not_found", "Not found")
	}
	id, action := m[1], m[2]
	me := s.authorize(r)
	var b map[string]any
	if method == "POST" || method == "PUT" {
		b = body(r)
	}
	switch method + " " + action {
	case "POST heartbeat":
		roster := []string{}
		list, _ := b["roster"].([]any)
		for _, k := range list {
			if key, ok := k.(string); ok && len(roster) < 3 {
				roster = append(roster, key)
			}
		}
		state := "open"
		if b["state"] == "playing" {
			state = "playing"
		}
		var score []int
		if list, ok := b["score"].([]any); ok {
			score = []int{}
			for i, n := range list {
				if i == 4 {
					break
				}
				f, _ := n.(float64)
				score = append(score, int(int32(f)))
			}
		}
		hb := beat{State: state, Roster: roster, Score: score, XR: isTrue(b["xr"])}
		return s.locked(func() answer { return s.heartbeat(s.lobby(id), me.Pubkey, hb) })
	case "PUT settings":
		name, vis, ask, st := s.lobbyName(b["name"]), visibility(b["visibility"]), isTrue(b["ask"]), settings(b["settings"])
		return s.locked(func() answer { return s.setSettings(s.lobby(id), me.Pubkey, name, vis, ask, st) })
	case "POST admit":
		target, accept := str(b["key"]), isTrue(b["accept"])
		return s.locked(func() answer { return s.admit(s.lobby(id), me.Pubkey, target, accept) })
	case "POST kick":
		target := str(b["key"])
		return s.locked(func() answer { return s.kick(s.lobby(id), me.Pubkey, target) })
	case "DELETE ", "POST close": // the same, for clients that only speak GET and POST (the game)
		return s.locked(func() answer { return s.closeBy(s.lobby(id), me.Pubkey) })
	case "GET join":
		s.mu.Lock()
		a, p := s.joinStatus(s.lobby(id), me.Pubkey)
		s.mu.Unlock()
		if p != nil {
			return s.wait(r, id, p)
		}
		return a
	case "POST leave":
		return s.locked(func() answer { return s.leave(s.lobby(id), me.Pubkey) })
	}
	fail(405, "bad_request", "Not allowed")
	return answer{}
}

// --- session -----------------------------------------------------------------

func (s *Service) session(r *http.Request) answer {
	b := body(r)
	pubkey := str(b["pubkey"])
	if _, ok := parseKey(pubkey); !ok {
		fail(400, "bad_request", "Malformed key")
	}
	return okBody(map[string]any{"nonce": s.makeNonce(pubkey)})
}

func (s *Service) verify(r *http.Request) answer {
	b := body(r)
	pubkey, nonce := str(b["pubkey"]), str(b["nonce"])
	key, ok := parseKey(pubkey)
	if !ok {
		fail(400, "bad_request", "Malformed key")
	}
	sig, err := b64.DecodeString(str(b["sig"]))
	if !s.checkNonce(pubkey, nonce) || err != nil || len(sig) != ed25519.SignatureSize ||
		!ed25519.Verify(key, sessionMessage(nonce, pubkey), sig) {
		fail(401, "unauthorized", "Couldn't sign in to XeleeNet")
	}
	expires := unixNow() + tokenSecs
	return okBody(map[string]any{"token": s.makeToken(pubkey, expires), "expires": expires, "tag": tagOf(key)})
}

// --- lobbies -------------------------------------------------------------------

func (s *Service) listCall(r *http.Request) answer {
	s.authorize(r)
	q := r.URL.Query()
	proto, _ := strconv.ParseFloat(q.Get("proto"), 64)
	c := compat(map[string]any{"proto": proto, "rev": q.Get("rev"), "disc": q.Get("disc")})
	from := s.geo(r)
	return s.locked(func() answer { return okBody(s.list(c, q.Get("full") == "1", from)) })
}

func (s *Service) hostCall(r *http.Request) answer {
	me := s.authorize(r)
	s.limit("host", me.Pubkey, s.cfg.Hosts, "You've opened a lot of lobbies; wait a minute")
	b := body(r)
	l := &Lobby{
		Name:     s.lobbyName(b["name"]),
		Public:   visibility(b["visibility"]) == "public",
		Ask:      isTrue(b["ask"]),
		Compat:   compat(b["compat"]),
		Settings: settings(b["settings"]),
		Host: Member{Key: me.Pubkey, Tag: me.Tag, Name: s.playerName(b["player"]), XR: isTrue(b["xr"]),
			Candidates: candidates(b["candidates"])},
		HostGeo: s.geo(r),
	}
	l.Host.Country = l.HostGeo.Country
	return s.locked(func() answer { return s.open(l) })
}

func (s *Service) joinCall(r *http.Request, ip string) answer {
	me := s.authorize(r)
	s.limit("join", ip, s.cfg.Joins, "Too many join attempts; wait a minute")
	b := body(r)
	code := normalizeCode(b["code"])
	if code == "" {
		fail(400, "bad_code", "Lobby codes are six letters and digits")
	}
	j := Joiner{
		Member: Member{Key: me.Pubkey, Tag: me.Tag, Name: s.playerName(b["player"]), XR: isTrue(b["xr"]),
			Candidates: candidates(b["candidates"])},
		Compat: compat(b["compat"]),
		Geo:    s.geo(r),
	}
	s.mu.Lock()
	l := s.codes[code]
	if l == nil {
		s.mu.Unlock()
		fail(404, "not_found", "No lobby with that code")
	}
	a, p := s.join(l, j)
	id := l.ID
	s.mu.Unlock()
	if p != nil {
		return s.wait(r, id, p)
	}
	return a
}

// wait holds a joiner on "Ask before joining" for the host's answer, up to
// JoinWait; then "pending", and the joiner asks again (GET …/join).
func (s *Service) wait(r *http.Request, id string, p *pending) answer {
	select {
	case <-p.done:
	case <-time.After(s.cfg.JoinWait):
	case <-r.Context().Done():
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if p.decided != nil {
		return *p.decided
	}
	if s.lobby(id) == nil {
		return okBody(map[string]any{"status": "pending", "id": nil})
	}
	return okBody(map[string]any{"status": "pending", "id": id})
}

// --- helpers ---------------------------------------------------------------------

func (s *Service) locked(f func() answer) answer {
	s.mu.Lock()
	defer s.mu.Unlock()
	return f()
}

func answerWith(w http.ResponseWriter, a answer) {
	if a.refusal != nil {
		reply(w, a.refusal.Status, a.refusal.Body())
		return
	}
	reply(w, 200, a.body)
}

func reply(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(body)
}

func body(r *http.Request) map[string]any {
	text, err := io.ReadAll(io.LimitReader(r.Body, maxBody+1))
	if err != nil {
		fail(400, "bad_request", "Couldn't read the request")
	}
	if len(text) > maxBody {
		fail(413, "bad_request", "Request too large")
	}
	if len(text) == 0 {
		return map[string]any{}
	}
	var v any
	if json.Unmarshal(text, &v) == nil {
		if m, ok := v.(map[string]any); ok {
			return m
		}
	}
	fail(400, "bad_request", "Malformed JSON")
	return nil
}

func hexID() string     { return hex.EncodeToString(randomBytes(16)) }
func itoa(n int) string { return strconv.Itoa(n) }
