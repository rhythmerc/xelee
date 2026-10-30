// Signing in. A client proves it holds its key once: POST /v1/session gives
// it a nonce, it signs "xeleenet-session-v1\n<nonce>\n<pubkey>", and
// /v1/session/verify answers with a token good for an hour. Neither the nonce
// nor the token is stored: both carry an HMAC under the token key, so they
// survive a restart and need no shared state.

package main

import (
	"crypto/ed25519"
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"
)

const (
	nonceSecs = 120
	tokenSecs = 3600
)

var b64 = base64.RawURLEncoding

func unixNow() int64 { return time.Now().Unix() }

func (s *Service) mac(text string) string {
	h := hmac.New(sha256.New, s.cfg.TokenKey)
	h.Write([]byte(text))
	return b64.EncodeToString(h.Sum(nil)[:16])
}

func sameText(a, b string) bool {
	return subtle.ConstantTimeCompare([]byte(a), []byte(b)) == 1
}

// parseKey decodes a base64url Ed25519 public key.
func parseKey(text string) ([]byte, bool) {
	if strings.ContainsAny(text, "+/=") {
		return nil, false
	}
	key, err := b64.DecodeString(text)
	return key, err == nil && len(key) == ed25519.PublicKeySize
}

func (s *Service) makeNonce(pubkey string) string {
	ts := unixNow()
	salt := b64.EncodeToString(randomBytes(9))
	return fmt.Sprintf("%d.%s.%s", ts, salt, s.mac(fmt.Sprintf("n|%s|%d|%s", pubkey, ts, salt)))
}

func (s *Service) checkNonce(pubkey, nonce string) bool {
	parts := strings.Split(nonce, ".")
	if len(parts) != 3 {
		return false
	}
	ts, err := strconv.ParseInt(parts[0], 10, 64)
	if err != nil {
		return false
	}
	if age := unixNow() - ts; age < 0 || age > nonceSecs {
		return false
	}
	return sameText(parts[2], s.mac(fmt.Sprintf("n|%s|%s|%s", pubkey, parts[0], parts[1])))
}

func sessionMessage(nonce, pubkey string) []byte {
	return []byte("xeleenet-session-v1\n" + nonce + "\n" + pubkey)
}

func (s *Service) makeToken(pubkey string, expires int64) string {
	return fmt.Sprintf("%s.%d.%s", pubkey, expires, s.mac(fmt.Sprintf("t|%s|%d", pubkey, expires)))
}

// Me is a signed-in caller: the key (base64url) and its tag.
type Me struct {
	Pubkey string
	Tag    string
}

func (s *Service) authorize(r *http.Request) Me {
	token, _ := strings.CutPrefix(r.Header.Get("Authorization"), "Bearer ")
	parts := strings.Split(token, ".")
	if len(parts) == 3 {
		key, ok := parseKey(parts[0])
		expires, err := strconv.ParseInt(parts[1], 10, 64)
		if ok && err == nil && expires > unixNow() &&
			sameText(parts[2], s.mac(fmt.Sprintf("t|%s|%s", parts[0], parts[1]))) {
			return Me{parts[0], tagOf(key)}
		}
	}
	fail(401, "unauthorized", "Sign in to XeleeNet again")
	return Me{}
}
