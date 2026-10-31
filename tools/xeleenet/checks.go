// The checks every field from a client goes through, lobby codes and player
// tags. A failed check is a refusal: `reason` is the stable word the game
// switches on, `error` the line it may show as is.

package main

import (
	"crypto/rand"
	"crypto/sha1"
	"encoding/base32"
	"fmt"
	"math"
	"regexp"
	"strconv"
	"strings"
)

// Refusal is an answer other than 200: the status and {error, reason}.
type Refusal struct {
	Status int
	Reason string
	Error  string
}

func (r *Refusal) Body() map[string]any {
	return map[string]any{"error": r.Error, "reason": r.Reason}
}

func refuse(status int, reason, error string) *Refusal {
	return &Refusal{status, reason, error}
}

// fail ends a handler with a refusal (recovered in ServeHTTP).
func fail(status int, reason, error string) {
	panic(refuse(status, reason, error))
}

func randomBytes(n int) []byte {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return b
}

// The tag after a player's name: the first 40 bits of SHA-1(public key) in
// RFC 4648 base32, exactly as net_identity.c makes a connect code's suffix.
// Names can repeat; the tag is what tells two players apart.
func tagOf(pubkey []byte) string {
	sum := sha1.Sum(pubkey)
	return base32.StdEncoding.EncodeToString(sum[:5])
}

// Lobby codes: six characters with no 0/O or 1/I, so one read off a stream or
// a headset is typed back right. Shown as K7M-Q2D; accepted in any case, with
// or without the dash.
const codeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

var codeRe = regexp.MustCompile(`^[A-HJ-NP-Z2-9]{6}$`)

func newCode() string {
	b := randomBytes(6)
	for i := range b {
		b[i] = codeAlphabet[b[i]&31]
	}
	return string(b)
}

func normalizeCode(v any) string {
	text, ok := v.(string)
	if !ok {
		return ""
	}
	code := strings.Map(func(r rune) rune {
		if r == '-' || r == ' ' || r == '\t' || r == '\n' || r == '\r' {
			return -1
		}
		return r
	}, strings.ToUpper(text))
	if !codeRe.MatchString(code) {
		return ""
	}
	return code
}

// --- JSON fields ---------------------------------------------------------------
//
// Bodies are decoded into map[string]any and read with JavaScript's
// strictness: a flag is on only if it is `true`, a count must be an integer.

func isTrue(v any) bool {
	b, ok := v.(bool)
	return ok && b
}

func integer(v any) (int, bool) {
	f, ok := v.(float64)
	if !ok || f != math.Trunc(f) || math.Abs(f) > 1<<31 {
		return 0, false
	}
	return int(f), true
}

func str(v any) string {
	s, _ := v.(string)
	return s
}

// --- names ---------------------------------------------------------------------

// What Name Entry can type: letters, digits, a space and a few symbols.
var nameRe = regexp.MustCompile(`^[A-Za-z0-9 .!?&'#+\-]+$`)
var notAlnum = regexp.MustCompile(`[^a-z0-9]`)

func (s *Service) blocked(text string) bool {
	flat := notAlnum.ReplaceAllString(strings.ToLower(text), "")
	for _, w := range s.cfg.Blocklist {
		if strings.Contains(flat, w) {
			return true
		}
	}
	return false
}

func (s *Service) checkName(v any, max int, what string) string {
	text := strings.Join(strings.Fields(str(v)), " ")
	if text == "" || len(text) > max || !nameRe.MatchString(text) {
		fail(400, "bad_name", fmt.Sprintf("%s must be 1-%d letters, digits or simple symbols", what, max))
	}
	if s.blocked(text) {
		fail(400, "bad_name", "Pick a different "+strings.ToLower(what))
	}
	return text
}

// TODO: make the cap 15; the stage select's draw already fits 15 (xeleenet-sss.rcss).
func (s *Service) playerName(v any) string { return s.checkName(v, 12, "Name") }
func (s *Service) lobbyName(v any) string  { return s.checkName(v, 20, "Lobby name") }

func visibility(v any) string {
	if s := str(v); s == "public" || s == "private" {
		return s
	}
	fail(400, "bad_request", "Visibility is public or private")
	return ""
}

// --- the build -------------------------------------------------------------------

// Compat is the build a player runs. Lobbies only match the exact same build:
// the native wire version, the app revision and the disc image.
type Compat struct {
	Proto int    `json:"proto"`
	Rev   string `json:"rev"`
	Disc  string `json:"disc"`
}

var revRe = regexp.MustCompile(`^[A-Za-z0-9._+\-]{1,40}$`)
var discRe = regexp.MustCompile(`^[A-Za-z0-9]{1,32}$`)

func compat(v any) Compat {
	m, _ := v.(map[string]any)
	proto, ok := integer(m["proto"])
	rev, disc := str(m["rev"]), str(m["disc"])
	if !ok || proto <= 0 || proto >= 65536 || !revRe.MatchString(rev) || !discRe.MatchString(disc) {
		fail(400, "bad_request", "Missing or malformed build info")
	}
	return Compat{proto, rev, disc}
}

// --- addresses ---------------------------------------------------------------------

// Where a player can be reached: up to four "ip:port" (IPv6 in brackets),
// their public mapping from STUN and their LAN address.
var addrRe = regexp.MustCompile(`^(?:(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})|\[[0-9A-Fa-f:.]{2,45}\]):(\d{1,5})$`)

func candidates(v any) []string {
	list, _ := v.([]any)
	if len(list) < 1 || len(list) > 4 {
		fail(400, "bad_request", "Send 1-4 addresses")
	}
	out := make([]string, 0, len(list))
	for _, a := range list {
		text, isStr := a.(string)
		m := addrRe.FindStringSubmatch(text)
		ok := isStr && m != nil
		if ok {
			port, _ := strconv.Atoi(m[5])
			ok = port >= 1 && port <= 65535
			for _, o := range m[1:5] {
				if n, _ := strconv.Atoi(o); n > 255 {
					ok = false
				}
			}
		}
		if !ok {
			shown := fmt.Sprint(a)
			if len(shown) > 60 {
				shown = shown[:60]
			}
			fail(400, "bad_request", "Malformed address "+shown)
		}
		out = append(out, text)
	}
	return out
}

// --- lobby settings -----------------------------------------------------------------

// Settings is the summary the browser shows. The full set travels between
// the players' games (the host owns it); the service keeps this only.
type Settings struct {
	Preset    string `json:"preset"`
	Seats     int    `json:"seats"`
	Mode      string `json:"mode"`
	Win       string `json:"win"`
	Stocks    int    `json:"stocks"`
	TimeMin   int    `json:"time_min"`
	Items     string `json:"items"`
	StageList string `json:"stage_list"`
	// The fighter list (docs/xeleenet-fighter-list-plan.md): all, high, low
	// or custom. Missing reads as all, from games before it.
	FighterList string `json:"fighter_list"`
	Pick        string `json:"pick"`
	Format      string `json:"format"`
	FirstTo     int    `json:"first_to"`
	PlaceSecs   int    `json:"place_secs"`
	// The lists themselves, for a lobby's Details page
	// (docs/xeleenet-lobby-details-plan.md): a bit per CharacterKind, and a
	// bit per Random Stage Switch id. Missing reads as the named list's.
	FighterMask uint32 `json:"fighter_mask"`
	StageMask   uint32 `json:"stage_mask"`
}

// The named lists' masks, as the game has them: src/pc/xeleenet_fighters.h
// (XN_FIGHTERS_*) and src/pc/xeleenet_menu.cpp (kStages, kCompetitive).
const (
	fighterBits       = 26
	stageBits         = 29
	fighterZelda      = 0x12
	fighterSheik      = 0x13
	fightersAll       = uint32(1)<<fighterBits - 1
	fightersHigh      = uint32(1)<<0x02 | 1<<0x09 | 1<<0x0F | 1<<0x14 | 1<<fighterZelda | 1<<fighterSheik | 1<<0x00 | 1<<0x0C | 1<<0x0E | 1<<0x0D | 1<<0x11 | 1<<0x10 | 1<<0x07 | 1<<0x16
	fightersLow       = fightersAll &^ fightersHigh
	stagesAll         = uint32(1)<<stageBits - 1
	stagesCompetitive = uint32(1)<<24 | 1<<25 | 1<<26 | 1<<4 | 1<<5 | 1<<7
)

var fighterLists = map[string]uint32{"all": fightersAll, "high": fightersHigh, "low": fightersLow}
var stageLists = map[string]uint32{"all": stagesAll, "competitive": stagesCompetitive}

// mask checks a list's mask against its name: not empty, within `bits`, and
// the named list's own mask unless the list is custom. Missing reads as the
// named list's; a custom list must send one.
func mask(v any, bits int, named map[string]uint32, list, what string) uint32 {
	want, isNamed := named[list]
	if v == nil {
		if !isNamed {
			fail(400, "bad_settings", "A custom "+what+" needs its mask")
		}
		return want
	}
	n, ok := integer(v)
	if !ok || n <= 0 || n >= 1<<bits || (isNamed && uint32(n) != want) {
		fail(400, "bad_settings", "Bad "+what+" mask")
	}
	return uint32(n)
}

func pick(v any, allowed []string, what string) string {
	s, ok := v.(string)
	for _, a := range allowed {
		if ok && s == a {
			return s
		}
	}
	fail(400, "bad_settings", "Bad "+what)
	return ""
}

func intIn(v any, lo, hi int, what string) int {
	n, ok := integer(v)
	if !ok || n < lo || n > hi {
		fail(400, "bad_settings", "Bad "+what)
	}
	return n
}

func settings(v any) Settings {
	m, _ := v.(map[string]any)
	firstTo := m["first_to"]
	if firstTo == nil {
		firstTo = float64(2)
	}
	fighters := m["fighter_list"]
	if fighters == nil {
		fighters = "all"
	}
	out := Settings{
		Preset:      pick(m["preset"], []string{"1v1", "doubles", "ffa", "custom"}, "preset"),
		Seats:       intIn(m["seats"], 2, 4, "seat count"),
		Mode:        pick(m["mode"], []string{"ffa", "teams"}, "mode"),
		Win:         pick(m["win"], []string{"stock", "time"}, "win condition"),
		Stocks:      intIn(m["stocks"], 1, 99, "stock count"),
		TimeMin:     intIn(m["time_min"], 0, 99, "time limit"),
		Items:       pick(m["items"], []string{"off", "very_low", "low", "medium", "high", "very_high"}, "item frequency"),
		StageList:   pick(m["stage_list"], []string{"competitive", "all", "custom"}, "stage list"),
		FighterList: pick(fighters, []string{"all", "high", "low", "custom"}, "fighter list"),
		Pick:        pick(m["pick"], []string{"loser", "everyone", "random", "host"}, "stage pick"),
		Format:      pick(m["format"], []string{"endless", "first_to"}, "set format"),
		FirstTo:     intIn(firstTo, 2, 5, "set length"),
	}
	out.FighterMask = mask(m["fighter_mask"], fighterBits, fighterLists, out.FighterList, "fighter list")
	if (out.FighterMask>>fighterZelda)&1 != (out.FighterMask>>fighterSheik)&1 {
		fail(400, "bad_settings", "Zelda and Sheik come as one")
	}
	out.StageMask = mask(m["stage_mask"], stageBits, stageLists, out.StageList, "stage list")
	out.PlaceSecs = intIn(m["place_secs"], 10, 60, "placement time")
	switch out.PlaceSecs {
	case 10, 15, 20, 30, 45, 60:
	default:
		fail(400, "bad_settings", "Bad placement time")
	}
	// Loser picks needs a loser: one side in a 1v1, or a team.
	if out.Pick == "loser" && out.Mode == "ffa" && out.Seats > 2 {
		fail(400, "bad_settings", "Loser picks needs two players or Teams")
	}
	return out
}
