# XeleeNet lobby service

The server half of XeleeNet lobbies (`docs/xeleenet-plan.md` §3.1). It's one
static Go binary that needs no outside services and no particular host: it
runs the same on a laptop, a $4 VPS or a container platform.

- **Lobbies** (`lobby.go`). Each one holds its members, their addresses, the
  "Ask before joining" queue and the ban list, and it decides every join.
- **The directory** (also `lobby.go`) hands out codes, resolves them, and
  answers the browser.
- **STUN** (`stun.go`, optional) tells a game its public address, so
  connecting doesn't need Google's or Cloudflare's STUN servers.

The service only introduces players. It passes each one the others'
addresses, and the games then connect directly. Match traffic never touches
it. Neither do the full lobby settings or the roster's day-to-day changes:
the host's game owns those and tells the others over the players' own links.

State is in memory behind one lock. With `-state FILE`, the open lobbies are
written on shutdown and read back on start, so a restart or upgrade doesn't
close them. Hosts get a fresh heartbeat window when the service comes back.

## The API

`api.go` has the full list. In short:

| Call | Who | |
|---|---|---|
| `POST /v1/session`, `/v1/session/verify` | everyone | Sign a nonce with the install's Ed25519 key, get an hour's token |
| `GET /v1/lobbies?proto&rev&disc[&full=1]` | browser | Public lobbies on the same build, nearest first |
| `POST /v1/lobbies` | host | Open a lobby; closes the host's previous one |
| `POST /v1/lobbies/<id>/heartbeat` | host | Every 2 s at the CSS, 10 s in a game (2 s while a member who joined isn't linked yet), with `state` open or playing. Returns members (with addresses) and pending joins |
| `PUT /v1/lobbies/<id>/settings` | host | The settings summary the browser shows |
| `POST /v1/lobbies/<id>/admit`, `/kick`; `DELETE /v1/lobbies/<id>` (or `POST …/close`, for the game) | host | |
| `POST /v1/join` | joiner | By code; answers joined (with everyone's addresses), pending, or a refusal |
| `GET /v1/lobbies/<id>/join` | joiner | Asks again after "pending" |
| `POST /v1/lobbies/<id>/leave` | member | |
| `GET /healthz` | monitoring | `ok` |

A refusal is `{"error", "reason"}`. The `reason` is a stable word for the game
to switch on: `not_found`, `closed`, `full`, `version`, `banned`, `declined`,
`seats`, `bad_name`, `bad_settings`, `rate_limited`, `unauthorized`, …. The
`error` is a line the game can show as is.

**Only the host polls.** Its heartbeat is short and carries the joins. A
joiner gets everyone's addresses in its join reply, and the others hear about
a newcomer from the host's game. The one request that waits is a joiner on
"Ask before joining" (at most `-join-wait`, then "pending").

**What's public.** Joining a lobby gives you its members' addresses, as in
any peer-to-peer game (Slippi included). The browser shows names, tags,
country and rules, never addresses. Coordinates only sort the list.

**Names** can repeat. The key is the identity, and the 8-character tag
(SHA-1 of the key, as `net_identity.c` makes a connect code) tells two players
with the same name apart. Names and lobby names are checked against
`XELEENET_NAME_BLOCKLIST`, so the word list isn't in the repository.

## Running it locally

Needs Go 1.27 or later (on `PATH`, `$GO`, or unpacked to `~/.local/opt/go`).

```sh
cd tools/xeleenet
go build -o xeleenet .
XELEENET_TOKEN_KEY=dev ./xeleenet                       # http://127.0.0.1:8787
python3 tools/xn_cli.py keygen                          # from the repo root
python3 tools/xn_cli.py host --seats 4                  # another terminal
python3 tools/xn_cli.py --key /tmp/b.key --name PIXEL list
```

`tools/xn_cli.py` is a headless client for the whole API. The end-to-end test
builds the service and runs every flow against it with short timeouts. It
then checks the rate limits, STUN, and a restart that keeps the lobbies:

```sh
python3 tools/xeleenet/test_service.py
```

`tools/net_test_n.py --xn` and `tools/net_test_xn_menu.py` start their own
copy the same way.

## Flags and secrets

| Flag | Default | |
|---|---|---|
| `-listen` | `127.0.0.1:8787` | HTTP address (plain HTTP: behind a proxy, or for tests) |
| `-tls-domain` | | Serve HTTPS on :443 with a Let's Encrypt certificate for this name (needs :80 too) |
| `-tls-cache` | `/var/lib/xeleenet/certs` | Where certificates are kept |
| `-state` | | Save lobbies here on shutdown, read them back on start |
| `-stun` | | Also answer STUN on this UDP address (`:3478`) |
| `-geoip` | | A MaxMind-format city database for country and nearest-first ([DB-IP's free IP to City Lite](https://db-ip.com/db/download/ip-to-city-lite), CC BY 4.0, monthly) |
| `-geo-headers` | off | Take location from Cloudflare's visitor location headers instead, if proxied by Cloudflare |
| `-ip-header` | | The header a trusted proxy puts the client address in. Only set this behind that proxy |
| `-expire`, `-join-wait`, `-roster-grace` | 30s, 20s, 20s | Lobby timings |
| `-limit-ip`, `-limit-host`, `-limit-join` | 120, 6, 20 | Per minute: calls per address, lobbies per player, joins per address (0 turns one off) |

| Environment | |
|---|---|
| `XELEENET_TOKEN_KEY` | Required. Signs session tokens: any long random string (`openssl rand -hex 32`). Changing it signs everyone out. |
| `XELEENET_NAME_BLOCKLIST` | Optional. Comma-separated words refused in names. |

The DB-IP Lite database is CC BY 4.0: anywhere the country shows, the credits
must say "IP Geolocation by DB-IP" with a link to https://db-ip.com.

Without `-geoip` or `-geo-headers`, the browser shows no country and lists
lobbies newest first.

## Running it on a server

CI (`.github/workflows/xeleenet.yml`) tests every push and builds
`xeleenet-linux-amd64` and `-arm64` as a workflow artifact. On a VPS with a
DNS name pointing at it:

```sh
scp xeleenet-linux-amd64 server:/usr/local/bin/xeleenet
scp deploy/xeleenet.service server:/etc/systemd/system/
ssh server 'printf "XELEENET_TOKEN_KEY=%s\nXELEENET_DOMAIN=xn.example.com\n" "$(openssl rand -hex 32)" > /etc/xeleenet.env &&
  chmod 600 /etc/xeleenet.env && systemctl enable --now xeleenet'
```

`deploy/provision.sh root@SERVER DOMAIN` does all of that on a fresh Ubuntu
server, plus the firewall (TCP 22, 80, 443; UDP 3478) and the city database
(`/usr/local/share/xeleenet/city.mmdb`, fetched from DB-IP). To upgrade,
copy the new binary and run `systemctl restart xeleenet`. The lobbies come
back from `-state`.

`deploy/Dockerfile` builds an image for container hosts. It serves plain HTTP
on 8787 and STUN on 3478/udp, and expects the platform to terminate TLS.

The game takes the service URL from `XELEENET_URL` at configure time (the
`XELEENET_URL` repository variable in CI) or `MELEE_XN_URL` at run time.
