# XeleeNet lobby service

The server half of XeleeNet lobbies (`docs/xeleenet-plan.md` §3.1). A
Cloudflare Worker with two kinds of Durable Object:

- **`Lobby`**, one per lobby. It holds the members, their addresses, the
  "Ask before joining" queue and the ban list, and it decides every join.
- **`Directory`**, one in all. It hands out codes, resolves them, and keeps
  the browser's summary rows.

The service only introduces players: it passes each one the others'
addresses, and the games then connect directly. Match traffic never touches
it, and neither do the full lobby settings or the roster's day-to-day changes;
the host's game owns those and tells the others over the players' own links.

## The API

`src/index.js` has the full list. In short:

| Call | Who | |
|---|---|---|
| `POST /v1/session`, `/v1/session/verify` | everyone | Sign a nonce with the install's Ed25519 key, get an hour's token |
| `GET /v1/lobbies?proto&rev&disc[&full=1]` | browser | Public lobbies on the same build, nearest first |
| `POST /v1/lobbies` | host | Open a lobby; closes the host's previous one |
| `POST /v1/lobbies/<id>/heartbeat` | host | Every 2 s at the CSS, 10 s in a game. Returns members (with addresses) and pending joins |
| `PUT /v1/lobbies/<id>/settings` | host | The settings summary the browser shows |
| `POST /v1/lobbies/<id>/admit`, `/kick`; `DELETE /v1/lobbies/<id>` | host | |
| `POST /v1/join` | joiner | By code; answers joined (with everyone's addresses), pending, or a refusal |
| `GET /v1/lobbies/<id>/join` | joiner | Asks again after "pending" |
| `POST /v1/lobbies/<id>/leave` | member | |

A refusal is `{"error", "reason"}`. The `reason` is a stable word for the game
to switch on: `not_found`, `closed`, `full`, `version`, `banned`, `declined`,
`seats`, `bad_name`, `bad_settings`, `rate_limited`, `unauthorized`, … and
`error` is a line it can show as is.

**Only the host polls.** Durable Objects bill wall-clock time while a request
is open, so long-polling by every member would keep every lobby running all
the time. Instead the host's heartbeat is short and carries the joins; a
joiner gets everyone's addresses in its join reply; the others hear about a
newcomer from the host's game. The one request that waits is a joiner on "Ask
before joining" (at most `JOIN_WAIT_SECS`, then "pending").

**What's public.** Joining a lobby gives you its members' addresses (as in
any peer-to-peer game, Slippi included). The browser shows names, tags,
country and rules, never addresses. Coordinates from Cloudflare only sort the
list.

**Names** can repeat: the key is the identity, and the 8-character tag
(SHA-1 of the key, as `net_identity.c` makes a connect code) tells two players
with the same name apart. Names and lobby names are checked against
`NAME_BLOCKLIST`, a secret, so the word list isn't in the repository.

## Running it locally

```sh
cd tools/xeleenet
npm ci
npx wrangler dev --var TOKEN_KEY:dev                    # http://127.0.0.1:8787
python3 tools/xn_cli.py keygen                          # from the repo root
python3 tools/xn_cli.py host --seats 4                  # another terminal
python3 tools/xn_cli.py --key /tmp/b.key --name PIXEL list
```

`tools/xn_cli.py` is a headless client for the whole API. The end-to-end test
starts its own `wrangler dev` with short timeouts and plays every flow:

```sh
python3 tools/xeleenet/test_service.py
```

`wrangler dev` doesn't enforce the rate limits, so those are only checked
deployed.

## Deploying

`.github/workflows/xeleenet.yml` tests on every push that touches the
service and deploys from `main`. It uses the same Cloudflare secrets as the log
drop (`CLOUDFLARE_API_TOKEN`, `CLOUDFLARE_ACCOUNT_ID`), plus:

| Secret | What |
|---|---|
| `XELEENET_TOKEN_KEY` | Signs session tokens; becomes the Worker's `TOKEN_KEY`. Changing it signs everyone out. |
| `XELEENET_NAME_BLOCKLIST` | Optional: comma-separated words refused in names. |

```sh
openssl rand -hex 32 | gh secret set -R rhythmerc/xelee XELEENET_TOKEN_KEY
gh secret set -R rhythmerc/xelee XELEENET_NAME_BLOCKLIST
```

The game will take the service URL the way it takes the log drop's: from a
repository secret compiled in at build time (not wired up yet; XN3).

## Cost

Rough numbers from Cloudflare's published limits; check the current ones
before relying on them. The free plan allows about 100,000 Durable Object
requests a day. A lobby at the CSS costs ~1,800 requests an hour (the host's
2 s heartbeat) and one in a game ~360, so the free plan covers somewhere
around 50-100 lobby-hours a day. The $5 Workers paid plan includes a million a
month and charges cents per million after. If XeleeNet grows past that, the
next step is hibernating WebSockets for the host, which bill almost nothing
while idle.
