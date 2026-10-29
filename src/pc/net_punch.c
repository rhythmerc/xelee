/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compat.h"
#include "pc/net_internal.h"
#include "pc/net_identity.h"
#include "pc/net_punch.h"
#include "pc/net_stun.h"
#include "pc/xeleenet.h"
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <monocypher.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char* pc_get_net_name(void);

#define STUN_MS 1500       /* how long the public address may take */
#define STUN_RESEND_MS 300
#define PROBE_MS 100       /* a link not punched yet: every candidate this often */
#define KEEP_MS 500        /* a punched one: keep its mapping and the echo going */
#define TELL_MS 250        /* a guest's report, the host's introductions */
#define START_MS 200       /* the host's start, until every guest answers */
#define PAIR_WAIT_MS 8000  /* two guests not through by then go via the host */
#define HOST_WAIT_MS 20000 /* a guest that cannot reach the host cannot play */
#define SWITCH_LEAD 90     /* frames between naming the switch and making it */

#define DEFAULT_STUN "stun.l.google.com:19302,stun.cloudflare.com:3478"

/* ---- wire ------------------------------------------------------------------
 * Every punch datagram is a head, a fixed body per type, and the sender's
 * Ed25519 signature over both; the lobby service gave everyone everyone's
 * key. Byte arrays only, so nothing depends on packing or byte order. */
enum { T_PROBE = 'P', T_INTRO = 'I', T_REPORT = 'R', T_START = 'S', T_ACK = 'K' };

typedef struct XnHead {
    uint8_t magic[3]; /* "XNP": never the first bytes of an engine datagram or STUN */
    uint8_t version;
    uint8_t type;
    uint8_t from, to; /* controller ports */
    uint8_t reserved;
    uint8_t lobby[8]; /* the first half of the lobby's id */
} XnHead;

typedef struct XnProbe {
    XnHead h;
    uint8_t nonce[8]; /* the sender's, for this lobby run */
    uint8_t echo[8];  /* the receiver's, once the sender has heard it; else 0 */
    uint8_t sig[64];
} XnProbe;

typedef struct XnIntro { /* host -> guest: a player who joined after it */
    XnHead h;
    uint8_t port, ncands, reserved[2];
    uint8_t key[32];
    uint8_t cands[PC_XN_CANDS][6]; /* IPv4 then port, network order */
    char name[16];                 /* NUL-padded */
    uint8_t sig[64];
} XnIntro;

typedef struct XnReport { /* guest -> host: how its links are going (port masks) */
    XnHead h;
    uint8_t known, up, gave, reserved;
    uint8_t sig[64];
} XnReport;

typedef struct XnStart { /* host -> guest */
    XnHead h;
    uint8_t ports;     /* who plays */
    uint8_t reserved[3];
    uint8_t direct[4]; /* per port: the ports it has its own link to */
    uint8_t seed[4];
    uint8_t nonces[4][8];
    uint8_t at[4]; /* players already in a session switch on this frame; ~0: not yet */
    uint8_t sig[64];
} XnStart;

typedef struct XnAck {
    XnHead h;
    uint8_t seed[4];
    uint8_t sig[64];
} XnAck;

/* ---- state ---------------------------------------------------------------- */

typedef enum { PH_OFF, PH_STUN, PH_SERVICE, PH_PUNCH, PH_STARTING, PH_DONE, PH_FAILED } Phase;

typedef struct Who { /* another player, by controller port */
    bool known;      /* key and candidates */
    uint8_t key[32];
    char name[16];
    int ncands;
    uint32_t ip[PC_XN_CANDS];
    uint16_t udp[PC_XN_CANDS];
    uint64_t known_ms;
    uint64_t nonce;           /* theirs, from their probes */
    bool up;                  /* they echoed ours: both ways work */
    bool gave;                /* a guest pair that ran out of time */
    struct sockaddr_in addr;  /* where they answered from */
    struct sockaddr_in seen;  /* where their last valid probe came from, up or not */
    bool seen_set;
    uint64_t next_probe;
    uint8_t known_m, up_m, gave_m; /* host: this guest's last report */
    bool acked;                    /* host: it answered the start */
    double joined;                 /* the service's stamp for their seat, 0 if introduced */
} Who;

static Phase s_phase;
static char s_why[160];
static sock_t s_sock = SOCK_INVALID;
static uint16_t s_bound;
static PcNetIdentity s_id;
static char s_url[256], s_name[16], s_code[64]; /* a LAN join's may be "@address:port" */
static int s_seats;
static uint64_t s_t0, s_stun_next, s_tell_next, s_start_next, s_in_ms;
static struct {
    struct sockaddr_in to;
    uint8_t txid[12];
    bool got;
    uint32_t ip;
    uint16_t udp;
} s_stun[2];
static int s_nstun;
static int s_local = -1;
static uint8_t s_lobby[8];
static uint64_t s_nonce;
static Who s_who[4];
static uint16_t s_block;
static char s_lobby_code[8];
static bool s_start_req; /* host: start without waiting for every seat */
static XnStart s_start; /* the host's, sent or accepted */
static int s_session_seq; /* sessions started this run: a new one is a roster change */
static uint32_t s_running_seed; /* the session running's start, 0 none */
static int32_t s_switch_at = -1; /* in a session: the frame the next one starts on */

/* Once a session runs, the engine owns the socket: its receive thread hands
 * punch datagrams over (ride_datagram) and the game thread drains them
 * (receive), sending through the engine (pc_net_send_datagram). That keeps
 * the lobby going in the CSS: a player who joins is found, linked and keyed
 * the same way, and the host starts the next session with them in it. */
#define RIDE_SLOTS 32
static struct {
    uint8_t buf[512];
    int n;
    struct sockaddr_in from;
} s_ride[RIDE_SLOTS];
static int s_ride_head, s_ride_n;
static SDL_Mutex* s_ride_lock;
static bool s_riding;
static XnAck s_ack;     /* a guest's answer to it, for the late repeats */
/* Players who left by holding B (pc_xn_punch_left): the member list goes on
 * naming them until the service hears (a guest's own list never changes), and
 * learn_members must not take them back. The same key with another `joined`
 * is the same player in a new seat. */
static struct {
    uint8_t key[32];
    double joined;
} s_departed[4];
static int s_ndeparted;

static uint64_t now_ms(void) {
    return SDL_GetTicks();
}

static void put64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; i--, v >>= 8) {
        p[i] = (uint8_t)v;
    }
}

static uint64_t get64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = v << 8 | p[i];
    }
    return v;
}

static void fail(const char* why) {
    if (s_phase == PH_FAILED) {
        return;
    }
    snprintf(s_why, sizeof s_why, "%s", why);
    s_phase = PH_FAILED;
    pc_log_line("xeleenet: %s", why);
    if (s_riding) {
        /* The lobby is gone (the service, a refused start), not the session:
         * the game goes on with whoever is in it. */
        pc_net_set_datagram_handler(NULL);
        s_riding = false;
    }
    if (s_sock != SOCK_INVALID) {
        sock_close(s_sock);
        s_sock = SOCK_INVALID;
    }
    pc_xn_stop();
}

static const char* ip_text(uint32_t ip, uint16_t port) {
    static char buf[4][24];
    static int k;
    char* b = buf[k++ & 3];
    uint32_t h = ntohl(ip);
    snprintf(b, 24, "%u.%u.%u.%u:%u", h >> 24, h >> 16 & 255, h >> 8 & 255, h & 255, port);
    return b;
}

static void head(XnHead* h, uint8_t type, int to) {
    memcpy(h->magic, "XNP", 3);
    h->version = 1;
    h->type = type;
    h->from = (uint8_t)s_local;
    h->to = (uint8_t)to;
    h->reserved = 0;
    memcpy(h->lobby, s_lobby, sizeof s_lobby);
}

static void send_to(const void* buf, size_t n, uint32_t ip, uint16_t port, int to) {
    if (to >= 0 && (s_block & (1u << (s_local * 4 + to)))) {
        return; /* MELEE_NET_SIM_BLOCK: as two guests behind strict NATs */
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = htons(port);
    if (s_sock != SOCK_INVALID) {
        sendto(s_sock, (const char*)buf, (int)n, 0, (const struct sockaddr*)&a, sizeof a);
    } else if (s_riding) {
        pc_net_send_datagram(buf, n, ip, port);
    }
}

/* Signs the datagram (its last 64 bytes are the signature) and sends it to
 * that player: where they answered from, or every candidate before that. */
static void send_signed(void* buf, size_t n, int to) {
    pc_identity_sign(&s_id, (uint8_t*)buf + n - 64, buf, n - 64);
    const Who* w = &s_who[to];
    if (w->up) {
        send_to(buf, n, w->addr.sin_addr.s_addr, ntohs(w->addr.sin_port), to);
        return;
    }
    bool seen_is_cand = !w->seen_set;
    for (int c = 0; c < w->ncands; c++) {
        send_to(buf, n, w->ip[c], w->udp[c], to);
        seen_is_cand |= w->ip[c] == w->seen.sin_addr.s_addr && htons(w->udp[c]) == w->seen.sin_port;
    }
    /* Their probe came from an address none of their candidates name: a NAT
     * that maps every destination to its own port (a phone's carrier, often)
     * showed STUN one port and shows us another. Answer where it came from;
     * that mapping is open toward us. */
    if (!seen_is_cand) {
        send_to(buf, n, w->seen.sin_addr.s_addr, ntohs(w->seen.sin_port), to);
    }
}

/* The key for the link between two players: both nonces and both public
 * keys, lower port first, bound to the lobby. Both ends feed it the same
 * bytes, and nobody off the path has seen the nonces. */
static void link_key(int a, int b, const uint8_t nonces[4][8], uint8_t out[32]) {
    if (a > b) {
        int t = a;
        a = b;
        b = t;
    }
    static const char label[] = "xeleenet link key v1";
    const uint8_t* ka = a == s_local ? s_id.public_key : s_who[a].key;
    const uint8_t* kb = b == s_local ? s_id.public_key : s_who[b].key;
    uint8_t pa = (uint8_t)a, pb = (uint8_t)b;
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 32);
    crypto_blake2b_update(&ctx, (const uint8_t*)label, sizeof label - 1);
    crypto_blake2b_update(&ctx, s_lobby, sizeof s_lobby);
    crypto_blake2b_update(&ctx, &pa, 1);
    crypto_blake2b_update(&ctx, nonces[a], 8);
    crypto_blake2b_update(&ctx, ka, 32);
    crypto_blake2b_update(&ctx, &pb, 1);
    crypto_blake2b_update(&ctx, nonces[b], 8);
    crypto_blake2b_update(&ctx, kb, 32);
    crypto_blake2b_final(&ctx, out);
}

/* ---- after the start: the engine owns the socket -------------------------- */

/* The engine's receive thread: a punch datagram is queued for the game
 * thread; anything else is the engine's. */
static bool ride_datagram(const void* buf, size_t n, uint32_t ip, uint16_t port) {
    if (n < 3 || memcmp(buf, "XNP", 3) != 0) {
        return false;
    }
    if (n <= sizeof s_ride[0].buf) {
        SDL_LockMutex(s_ride_lock);
        if (s_ride_n < RIDE_SLOTS) {
            int i = (s_ride_head + s_ride_n++) % RIDE_SLOTS;
            memcpy(s_ride[i].buf, buf, n);
            s_ride[i].n = (int)n;
            memset(&s_ride[i].from, 0, sizeof s_ride[i].from);
            s_ride[i].from.sin_family = AF_INET;
            s_ride[i].from.sin_addr.s_addr = ip;
            s_ride[i].from.sin_port = htons(port);
        }
        SDL_UnlockMutex(s_ride_lock);
    }
    return true;
}

/* The next datagram: off our own socket before the session, else out of the
 * engine's hand-over queue. -1 when there is none. */
static int next_datagram(uint8_t* buf, size_t cap, struct sockaddr_in* from) {
    if (s_sock != SOCK_INVALID) {
        for (;;) {
            socklen_t len = sizeof *from;
            int n = (int)recvfrom(s_sock, (char*)buf, (int)cap, 0, (struct sockaddr*)from, &len);
#if defined(_WIN32)
            /* Winsock hands an unconnected UDP socket the ICMP unreachable from a
             * candidate nobody listens on as a recvfrom error; the queue goes on. */
            if (n < 0 && WSAGetLastError() == WSAECONNRESET) {
                continue;
            }
#endif
            return n > 0 ? n : -1;
        }
    }
    int n = -1;
    if (s_riding && s_ride_lock != NULL) {
        SDL_LockMutex(s_ride_lock);
        if (s_ride_n > 0) {
            n = s_ride[s_ride_head].n;
            memcpy(buf, s_ride[s_ride_head].buf, (size_t)n);
            *from = s_ride[s_ride_head].from;
            s_ride_head = (s_ride_head + 1) % RIDE_SLOTS;
            s_ride_n--;
        }
        SDL_UnlockMutex(s_ride_lock);
    }
    return n;
}

static void begin_session(uint32_t seed) {
    PcNetLink links[3];
    int n = 0;
    for (int p = 0; p < 4; p++) {
        if (p == s_local || !(s_start.ports & (1u << p))) {
            continue;
        }
        const Who* w = &s_who[p];
        PcNetLink* l = &links[n++];
        memset(l, 0, sizeof *l);
        l->player = p;
        l->ip = w->up ? w->addr.sin_addr.s_addr : w->ip[0];
        l->port = w->up ? ntohs(w->addr.sin_port) : w->udp[0];
        l->via_host = !(s_start.direct[s_local] & (1u << p));
        link_key(s_local, p, (const uint8_t(*)[8])s_start.nonces, l->key);
        pc_log_line("xeleenet: P%d %s at %s", p + 1, l->via_host ? "through the host" : "direct",
            ip_text(l->ip, l->port));
    }
    /* The first session takes our socket; a later one (someone joined) takes
     * it back from the running session, with no goodbye: every peer restarts.
     * First every peer must hold our input up to the switch: once we stop,
     * nothing resends what was lost, and a peer a frame short waits there
     * until its silence timeout (an 80 ms one-way delay showed it). */
    if (s_sock == SOCK_INVALID && pc_net_active()) {
        pc_net_settle(pc_net_frame(), 1500); /* the switch frame, host or guest */
    }
    sock_t sock = s_sock != SOCK_INVALID ? s_sock : (sock_t)pc_net_release_socket();
    s_sock = SOCK_INVALID;
    if (sock == SOCK_INVALID || !pc_net_connect_links((intptr_t)sock, links, n, s_local, seed)) {
        if (sock != SOCK_INVALID) {
            sock_close(sock);
        }
        s_riding = false;
        fail("Couldn't start the session");
        return;
    }
    crypto_wipe(links, sizeof links);
    if (s_ride_lock == NULL) {
        s_ride_lock = SDL_CreateMutex();
    }
    s_ride_head = s_ride_n = 0;
    s_riding = true;
    pc_net_set_datagram_handler(ride_datagram);
    s_phase = PH_DONE;
    s_session_seq++;
    s_running_seed = seed;
    s_switch_at = -1;
    pc_log_line("xeleenet: session %d up as P%d with %d others after %.1f s", s_session_seq,
        s_local + 1, n, (now_ms() - s_t0) / 1000.0);
}

/* ---- receiving ------------------------------------------------------------ */

static void on_probe(const XnProbe* m, const struct sockaddr_in* from) {
    Who* w = &s_who[m->h.from];
    w->nonce = get64(m->nonce);
    if (!w->seen_set || w->seen.sin_addr.s_addr != from->sin_addr.s_addr ||
        w->seen.sin_port != from->sin_port)
    {
        bool cand = false;
        for (int c = 0; c < w->ncands; c++) {
            cand |= w->ip[c] == from->sin_addr.s_addr && htons(w->udp[c]) == from->sin_port;
        }
        if (!cand && !w->up) {
            pc_log_line("xeleenet: P%d probes from %s, not a candidate; answering there too",
                m->h.from + 1, ip_text(from->sin_addr.s_addr, ntohs(from->sin_port)));
        }
        w->seen = *from;
        w->seen_set = true;
    }
    if (get64(m->echo) == s_nonce && !w->up) {
        w->up = true;
        w->addr = *from;
        w->next_probe = 0; /* echo theirs at once */
        pc_log_line("xeleenet: P%d punched at %s after %.1f s", m->h.from + 1,
            ip_text(from->sin_addr.s_addr, ntohs(from->sin_port)), (now_ms() - w->known_ms) / 1000.0);
    } else if (w->next_probe > now_ms() + PROBE_MS) {
        w->next_probe = now_ms(); /* they have not heard us: answer now */
    }
}

static void on_intro(const XnIntro* m) {
    int p = m->port;
    if (p < 1 || p > 3 || p == s_local || s_who[p].known || m->ncands < 1 ||
        m->ncands > PC_XN_CANDS)
    {
        return;
    }
    Who* w = &s_who[p];
    memcpy(w->key, m->key, 32);
    w->ncands = m->ncands;
    for (int c = 0; c < w->ncands; c++) {
        memcpy(&w->ip[c], m->cands[c], 4);
        w->udp[c] = (uint16_t)(m->cands[c][4] << 8 | m->cands[c][5]);
    }
    memcpy(w->name, m->name, sizeof w->name);
    w->name[sizeof w->name - 1] = '\0';
    w->known = true;
    w->known_ms = now_ms();
    pc_log_line("xeleenet: the host introduced P%d (%s)", p + 1, w->name);
}

static uint32_t get32b(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put32b(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* A start. Not in a session yet (the first, or a newcomer): begin now. In
 * one, a roster change: answer at once, then switch on the frame the host
 * names, together with everyone else in it -- leaving first would stall the
 * session the others still tick, and with it their answers (a deadlock seen
 * at the second join). */
static void on_start(const XnStart* m) {
    if ((s_phase != PH_PUNCH && s_phase != PH_DONE) || !(m->ports & (1u << s_local)) ||
        get64(m->nonces[s_local]) != s_nonce)
    {
        return;
    }
    const uint32_t seed = get32b(m->seed);
    const bool riding = s_riding && pc_net_active();
    if (riding && seed == s_running_seed) {
        send_signed(&s_ack, sizeof s_ack, 0); /* the session we're in: answered already */
        return;
    }
    if (riding && memcmp(m->seed, s_start.seed, 4) == 0 && get32b(m->at) == get32b(s_start.at)) {
        send_signed(&s_ack, sizeof s_ack, 0); /* our answer was lost: the same one again */
        return;
    }
    for (int p = 1; p < 4; p++) {
        if ((m->ports & (1u << p)) && p != s_local && !s_who[p].known) {
            return; /* cannot happen: the host waits for our report to know them */
        }
    }
    s_start = *m;
    head(&s_ack.h, T_ACK, 0);
    memcpy(s_ack.seed, m->seed, 4);
    for (int i = 0; i < 3; i++) {
        send_signed(&s_ack, sizeof s_ack, 0);
    }
    if (riding) {
        const uint32_t at = get32b(m->at);
        if (at != 0xFFFFFFFFu) {
            s_switch_at = (int32_t)at;
            pc_log_line("xeleenet: the next session (players %02x) starts at frame %d", m->ports, s_switch_at);
        }
        return;
    }
    begin_session(seed);
}

static void receive(void) {
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        int n = next_datagram(buf, sizeof buf, &from);
        if (n <= 0) {
            return;
        }
        if (from.sin_family != AF_INET) {
            continue;
        }
        for (int i = 0; i < s_nstun; i++) {
            if (!s_stun[i].got && pc_stun_parse(buf, (size_t)n, s_stun[i].txid, &s_stun[i].ip, &s_stun[i].udp)) {
                s_stun[i].got = true;
            }
        }
        const XnHead* h = (const XnHead*)buf;
        if (s_local < 0 || n < (int)sizeof(XnHead) + 64 || memcmp(h->magic, "XNP", 3) != 0 ||
            h->version != 1 || h->to != s_local || h->from > 3 || h->from == s_local ||
            memcmp(h->lobby, s_lobby, sizeof s_lobby) != 0 || !s_who[h->from].known)
        {
            continue; /* an engine datagram from a peer already started arrives here too */
        }
        size_t want = h->type == T_PROBE  ? sizeof(XnProbe) :
                      h->type == T_INTRO  ? sizeof(XnIntro) :
                      h->type == T_REPORT ? sizeof(XnReport) :
                      h->type == T_START  ? sizeof(XnStart) :
                      h->type == T_ACK    ? sizeof(XnAck) :
                                            0;
        if ((size_t)n != want || !pc_identity_verify(s_who[h->from].key, buf + n - 64, buf, (size_t)n - 64)) {
            continue;
        }
        bool from_host = h->from == 0;
        switch (h->type) {
        case T_PROBE:
            on_probe((const XnProbe*)buf, &from);
            break;
        case T_INTRO:
            if (from_host) {
                on_intro((const XnIntro*)buf);
            }
            break;
        case T_REPORT:
            if (s_local == 0) {
                const XnReport* r = (const XnReport*)buf;
                Who* w = &s_who[h->from];
                w->known_m = r->known;
                w->up_m = r->up;
                w->gave_m = r->gave;
            }
            break;
        case T_START:
            if (from_host) {
                on_start((const XnStart*)buf);
            }
            break;
        case T_ACK:
            if (s_local == 0 && memcmp(((const XnAck*)buf)->seed, s_start.seed, 4) == 0) {
                s_who[h->from].acked = true;
            }
            break;
        }
    }
}

/* ---- the phases ----------------------------------------------------------- */

static bool resolve(const char* host, const char* port, struct sockaddr_in* out) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL) {
        return false;
    }
    memcpy(out, res->ai_addr, sizeof *out);
    freeaddrinfo(res);
    return true;
}

static void stun_begin(void) {
    const char* list = getenv("MELEE_XN_STUN");
    list = list != NULL ? list : DEFAULT_STUN;
    s_nstun = 0;
    /* On this network, the LAN address is all anyone needs (and the
     * internet may not be there at all). */
    if (strcmp(list, "off") == 0 || pc_xn_network() == PC_XN_NET_LAN) {
        return;
    }
    char copy[256];
    snprintf(copy, sizeof copy, "%s", list);
    for (char *t = copy, *next; t != NULL && *t != '\0' && s_nstun < 2; t = next) {
        char* comma = strchr(t, ',');
        next = comma != NULL ? comma + 1 : NULL;
        if (comma != NULL) {
            *comma = '\0';
        }
        char* colon = strrchr(t, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        if (!resolve(t, colon + 1, &s_stun[s_nstun].to)) {
            pc_log_line("xeleenet: cannot resolve STUN server %s", t);
            continue;
        }
        pc_identity_random(s_stun[s_nstun].txid, sizeof s_stun[s_nstun].txid);
        s_stun[s_nstun].got = false;
        s_nstun++;
    }
}

static void stun_send(void) {
    for (int i = 0; i < s_nstun; i++) {
        if (!s_stun[i].got) {
            uint8_t req[PC_STUN_REQUEST_BYTES];
            pc_stun_request(req, s_stun[i].txid);
            sendto(s_sock, (const char*)req, sizeof req, 0, (const struct sockaddr*)&s_stun[i].to,
                sizeof s_stun[i].to);
        }
    }
}

/* STUN is in (or out of time): hand the candidates to the service. */
static void stun_done(void) {
    uint32_t ip[PC_XN_CANDS];
    uint16_t udp[PC_XN_CANDS];
    int n = 0;
    uint32_t lan;
    if (pc_stun_local_ipv4(&lan)) {
        ip[n] = lan;
        udp[n++] = s_bound;
    }
    for (int i = 0; i < s_nstun; i++) {
        if (!s_stun[i].got) {
            continue;
        }
        bool dup = false;
        for (int c = 0; c < n; c++) {
            dup |= ip[c] == s_stun[i].ip && udp[c] == s_stun[i].udp;
        }
        if (!dup && n < PC_XN_CANDS) {
            ip[n] = s_stun[i].ip;
            udp[n++] = s_stun[i].udp;
        }
    }
    if (s_nstun == 2 && s_stun[0].got && s_stun[1].got && s_stun[0].udp != s_stun[1].udp) {
        /* The NAT maps us to a new port for every destination, so the one
         * STUN saw is not the one another player will: punching straight
         * to another guest will likely fail, and that pair goes via the host. */
        pc_log_line("xeleenet: our NAT maps each destination to its own port (%u, %u)",
            s_stun[0].udp, s_stun[1].udp);
    }
    if (n == 0) {
        ip[n] = htonl(0x7f000001u);
        udp[n++] = s_bound; /* no route at all: only this machine can reach us */
    }
    for (int c = 0; c < n; c++) {
        pc_log_line("xeleenet: reachable at %s", ip_text(ip[c], udp[c]));
    }
    if (!pc_xn_start(s_url, &s_id, s_name, s_code[0] ? s_code : NULL, s_seats, ip, udp, n)) {
        fail("Couldn't start the XeleeNet client");
        return;
    }
    s_phase = PH_SERVICE;
}

static void learn_members(void) {
    PcXnMember m[3];
    int n = pc_xn_members(m, 3);
    for (int i = 0; i < n; i++) {
        Who* w = &s_who[m[i].port];
        if (m[i].port == s_local || w->known) {
            continue;
        }
        bool departed = false;
        for (int d = 0; d < s_ndeparted; d++) {
            departed |= memcmp(s_departed[d].key, m[i].key, 32) == 0 && s_departed[d].joined == m[i].joined;
        }
        if (departed) {
            continue;
        }
        w->joined = m[i].joined;
        memcpy(w->key, m[i].key, 32);
        snprintf(w->name, sizeof w->name, "%s", m[i].name);
        w->ncands = m[i].ncands;
        memcpy(w->ip, m[i].ip, sizeof w->ip);
        memcpy(w->udp, m[i].udp, sizeof w->udp);
        w->known = true;
        w->known_ms = now_ms();
        pc_log_line("xeleenet: P%d is %s#%s", m[i].port + 1, w->name, m[i].tag);
    }
}

static uint8_t mask_of(bool (*test)(const Who*)) {
    uint8_t m = 0;
    for (int p = 0; p < 4; p++) {
        if (p != s_local && test(&s_who[p])) {
            m |= (uint8_t)(1u << p);
        }
    }
    return m;
}

static bool is_known(const Who* w) {
    return w->known;
}

static bool is_up(const Who* w) {
    return w->up;
}

static bool is_gave(const Who* w) {
    return w->gave;
}

static void punch_tick(void) {
    uint64_t t = now_ms();
    for (int p = 0; p < 4; p++) {
        Who* w = &s_who[p];
        if (p == s_local || !w->known) {
            continue;
        }
        if (s_local != 0 && p != 0 && !w->up && !w->gave && t - w->known_ms > PAIR_WAIT_MS) {
            w->gave = true;
            pc_log_line("xeleenet: no direct link to P%d after %d s; it goes through the host",
                p + 1, PAIR_WAIT_MS / 1000);
        }
        if (t >= w->next_probe && !w->gave) {
            XnProbe m;
            head(&m.h, T_PROBE, p);
            put64(m.nonce, s_nonce);
            put64(m.echo, w->nonce);
            send_signed(&m, sizeof m, p);
            w->next_probe = t + (w->up ? KEEP_MS : PROBE_MS);
        }
    }
    if (t < s_tell_next) {
        return;
    }
    s_tell_next = t + TELL_MS;
    if (s_local != 0) {
        if (!s_who[0].up && t - s_in_ms > HOST_WAIT_MS) {
            fail("Couldn't connect to the host");
            return;
        }
        if (s_who[0].known) {
            XnReport r;
            memset(&r, 0, sizeof r);
            head(&r.h, T_REPORT, 0);
            r.known = mask_of(is_known);
            r.up = mask_of(is_up);
            r.gave = mask_of(is_gave);
            send_signed(&r, sizeof r, 0);
        }
        return;
    }
    /* Host: introduce every guest to those it has not heard of. */
    uint8_t keys[3][32];
    int nkeys = 0;
    for (int g = 1; g < 4; g++) {
        Who* w = &s_who[g];
        if (!w->known || !w->up) {
            continue;
        }
        memcpy(keys[nkeys++], w->key, 32);
        for (int o = 1; o < 4; o++) {
            const Who* x = &s_who[o];
            if (o == g || !x->known || (w->known_m & (1u << o))) {
                continue;
            }
            XnIntro m;
            memset(&m, 0, sizeof m);
            head(&m.h, T_INTRO, g);
            m.port = (uint8_t)o;
            m.ncands = (uint8_t)x->ncands;
            memcpy(m.key, x->key, 32);
            memcpy(m.name, x->name, sizeof m.name);
            for (int c = 0; c < x->ncands; c++) {
                memcpy(m.cands[c], &x->ip[c], 4);
                m.cands[c][4] = (uint8_t)(x->udp[c] >> 8);
                m.cands[c][5] = (uint8_t)x->udp[c];
            }
            send_signed(&m, sizeof m, g);
        }
    }
    pc_xn_set_roster((const uint8_t(*)[32])keys, nkeys);
}

/* Host: someone has joined, every guest reaches us and every pair of guests
 * has decided its route; and the seats are filled or the host said start. */
static bool links_decided(int* players_out) {
    int players = 1;
    for (int g = 1; g < 4; g++) {
        const Who* w = &s_who[g];
        if (!w->known) {
            continue;
        }
        players++;
        if (!w->up) {
            return false;
        }
        for (int o = 1; o < 4; o++) {
            if (o != g && s_who[o].known && !((w->up_m | w->gave_m) & (1u << o))) {
                return false;
            }
        }
    }
    *players_out = players;
    return players >= 2;
}

static bool ready_to_start(void) {
    int players = 0;
    return links_decided(&players) && (players >= s_seats || s_start_req);
}

static void start_build(void) {
    memset(&s_start, 0, sizeof s_start);
    uint32_t seed;
    pc_identity_random(&seed, sizeof seed);
    seed |= 1;
    s_start.seed[0] = (uint8_t)(seed >> 24);
    s_start.seed[1] = (uint8_t)(seed >> 16);
    s_start.seed[2] = (uint8_t)(seed >> 8);
    s_start.seed[3] = (uint8_t)seed;
    s_start.ports = 1;
    put64(s_start.nonces[0], s_nonce);
    for (int g = 1; g < 4; g++) {
        const Who* w = &s_who[g];
        if (!w->known) {
            continue;
        }
        s_start.ports |= (uint8_t)(1u << g);
        put64(s_start.nonces[g], w->nonce);
        s_start.direct[0] |= (uint8_t)(1u << g);
        s_start.direct[g] |= 1;
        for (int o = 1; o < 4; o++) {
            /* Direct only when both ends have punched it. */
            if (o != g && s_who[o].known && (w->up_m & (1u << o)) && (s_who[o].up_m & (1u << g))) {
                s_start.direct[g] |= (uint8_t)(1u << o);
            }
        }
    }
    for (int g = 1; g < 4; g++) {
        for (int o = g + 1; o < 4; o++) {
            if ((s_start.ports & (1u << g)) && (s_start.ports & (1u << o)) &&
                !(s_start.direct[g] & (1u << o)))
            {
                pc_log_line("xeleenet: P%d and P%d go through the host", g + 1, o + 1);
            }
        }
    }
    put32b(s_start.at, 0xFFFFFFFFu);
    for (int g = 1; g < 4; g++) {
        s_who[g].acked = false;
    }
    s_start_next = 0;
    s_phase = PH_STARTING;
    pc_log_line("xeleenet: starting, players %02x, seed %u", s_start.ports, seed);
}

/* The host's start, until everyone has answered. Then, with nobody in a
 * session yet, it begins at once. With one running, the second step: the
 * frame everyone in it switches on, sent until it comes. */
static void start_tick(void) {
    const uint64_t t = now_ms();
    const bool riding = s_riding && pc_net_active();
    const uint32_t seed = get32b(s_start.seed);
    if (riding && get32b(s_start.at) != 0xFFFFFFFFu) {
        if (pc_net_frame() >= (int32_t)get32b(s_start.at)) {
            begin_session(seed);
            return;
        }
        if (t >= s_start_next) {
            s_start_next = t + START_MS;
            for (int g = 1; g < 4; g++) {
                if (s_start.ports & (1u << g)) {
                    head(&s_start.h, T_START, g);
                    send_signed(&s_start, sizeof s_start, g);
                }
            }
        }
        return;
    }
    if (t < s_start_next) {
        return; /* its own clock: punch_tick's runs alongside while a session does */
    }
    s_start_next = t + START_MS;
    bool all = true;
    for (int g = 1; g < 4; g++) {
        if ((s_start.ports & (1u << g)) && !s_who[g].acked) {
            all = false;
            head(&s_start.h, T_START, g);
            send_signed(&s_start, sizeof s_start, g);
        }
    }
    if (!all) {
        return;
    }
    if (!riding) {
        begin_session(seed);
        return;
    }
    put32b(s_start.at, (uint32_t)(pc_net_frame() + SWITCH_LEAD));
    s_start_next = 0; /* the named frame goes out on the next tick */
    pc_log_line("xeleenet: everyone answered; the next session (players %02x) starts at frame %d",
        s_start.ports, (int)get32b(s_start.at));
}

/* Host, with a session running: a guest the session doesn't have yet, linked
 * to everyone, or one it has who left, means the next session. */
static bool roster_changed(void) {
    uint8_t guests = 0;
    for (int g = 1; g < 4; g++) {
        guests |= s_who[g].known ? (uint8_t)(1u << g) : 0;
    }
    int players = 0;
    return guests != (s_start.ports & ~1u) && links_decided(&players);
}

int pc_xn_punch_poll(void) {
    switch (s_phase) {
    case PH_OFF:
    case PH_FAILED:
        return -1;
    default:
        break;
    }
    receive();
    uint64_t t = now_ms();
    switch (s_phase) {
    case PH_STUN: {
        bool all = true;
        for (int i = 0; i < s_nstun; i++) {
            all &= s_stun[i].got;
        }
        if (all || t - s_t0 > STUN_MS) {
            stun_done();
        } else if (t >= s_stun_next) {
            stun_send();
            s_stun_next = t + STUN_RESEND_MS;
        }
        break;
    }
    case PH_SERVICE: {
        PcXnStatus st = pc_xn_status();
        if (st == PC_XN_FAILED) {
            const char* reason;
            const char* err = pc_xn_error(&reason);
            fail(err[0] ? err : "XeleeNet refused");
            break;
        }
        char id[33], code[8];
        if (st != PC_XN_IN || !pc_xn_lobby(id, code, &s_local)) {
            break;
        }
        for (int i = 0; i < 8; i++) {
            unsigned v = 0;
            sscanf(id + 2 * i, "%2x", &v);
            s_lobby[i] = (uint8_t)v;
        }
        s_in_ms = t;
        memcpy(s_lobby_code, code, sizeof s_lobby_code);
        s_phase = PH_PUNCH;
        if (s_local == 0) {
            pc_log_line("xeleenet: lobby code %s", code); /* tools/net_test_n.py reads it */
        }
        break;
    }
    case PH_PUNCH:
        learn_members();
        if (pc_xn_status() == PC_XN_FAILED) {
            fail(pc_xn_error(NULL));
            break;
        }
        punch_tick();
        if (s_local == 0 && s_phase == PH_PUNCH && ready_to_start()) {
            start_build();
        }
        break;
    case PH_STARTING:
        if (s_riding) {
            learn_members();
            punch_tick(); /* the running session's links stay probed meanwhile */
        }
        start_tick();
        break;
    case PH_DONE:
        /* The lobby goes on in the CSS: newcomers are found and linked. */
        learn_members();
        if (pc_xn_status() == PC_XN_FAILED) {
            fail(pc_xn_error(NULL));
            break;
        }
        punch_tick();
        if (s_local != 0 && s_switch_at >= 0 && pc_net_frame() >= s_switch_at) {
            begin_session(get32b(s_start.seed)); /* the frame the host named */
        } else if (s_local == 0 && roster_changed()) {
            start_build();
        }
        break;
    default:
        break;
    }
    if (s_phase == PH_FAILED) {
        return -1;
    }
    return s_session_seq > 0 && pc_net_active() ? 1 : 0;
}

int pc_xn_punch_session_seq(void) {
    return s_session_seq;
}

bool pc_xn_punch_start(const char* url, const char* name, const char* code, int seats) {
    if (s_phase != PH_OFF && s_phase != PH_DONE && s_phase != PH_FAILED) {
        return false;
    }
    memset(s_who, 0, sizeof s_who);
    memset(&s_start, 0, sizeof s_start);
    s_ndeparted = 0;
    s_session_seq = 0;
    s_lobby_code[0] = '\0';
    s_start_req = false;
    s_phase = PH_STUN;
    s_why[0] = '\0';
    s_local = -1;
    s_seats = seats;
    snprintf(s_url, sizeof s_url, "%s", url);
    snprintf(s_name, sizeof s_name, "%.12s", name);
    snprintf(s_code, sizeof s_code, "%s", code != NULL ? code : "");
    s_block = net_sim_block_mask();
    /* MELEE_XN_IDENTITY: another key directory, so several instances on one
     * machine are several players (tools/net_test_n.py --xn). */
    const char* dir_env = getenv("MELEE_XN_IDENTITY");
    char* pref = dir_env == NULL ? SDL_GetPrefPath(NULL, "melee-pc") : NULL;
    char label[9] = "";
    for (size_t i = 0, o = 0; s_name[i] != '\0' && o < 8; i++) {
        char c = s_name[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            label[o++] = c;
            label[o] = '\0';
        }
    }
    /* The label only names the key's connect code; the key is the identity. */
    bool ok = pc_identity_load(&s_id, dir_env != NULL ? dir_env : pref, label[0] ? label : "PLAYER");
    SDL_free(pref);
    if (!ok) {
        fail("No identity key for XeleeNet");
        return false;
    }
    do {
        pc_identity_random(&s_nonce, sizeof s_nonce);
    } while (s_nonce == 0);
    sock_startup();
    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    const char* lport = getenv("MELEE_NET_PORT");
    a.sin_port = htons((uint16_t)(lport != NULL ? atoi(lport) : 0));
    socklen_t len = sizeof a;
    if (s_sock == SOCK_INVALID || bind(s_sock, (struct sockaddr*)&a, sizeof a) != 0 ||
        !sock_nonblock(s_sock) || getsockname(s_sock, (struct sockaddr*)&a, &len) != 0)
    {
        fail("Couldn't open a UDP socket");
        return false;
    }
    s_bound = ntohs(a.sin_port);
    s_t0 = now_ms();
    s_stun_next = 0;
    s_tell_next = 0;
    stun_begin();
    pc_log_line("xeleenet: %s on UDP %u as %s", code != NULL ? "joining" : "hosting", s_bound,
        s_id.code);
    return true;
}

const char* pc_xn_punch_error(void) {
    return s_why;
}

void pc_xn_punch_cancel(void) {
    if (s_phase == PH_DONE || s_phase == PH_OFF) {
        return;
    }
    fail("cancelled");
}

const char* pc_xn_url(void) {
#ifndef XELEENET_URL
#define XELEENET_URL ""
#endif
    const char* env = getenv("MELEE_XN_URL");
    return env != NULL && env[0] != '\0' ? env : XELEENET_URL;
}

bool pc_xn_punch_from_env(void) {
    const char* xn = getenv("MELEE_XN");
    if (xn == NULL || xn[0] == '\0') {
        return false;
    }
    const char* url = getenv("MELEE_XN_URL");
    const char* name = getenv("MELEE_XN_NAME");
    const char* seats = getenv("MELEE_XN_SEATS");
    const char* wait = getenv("MELEE_XN_WAIT_S");
    const char* net_env = getenv("MELEE_XN_NET");
    bool join = strncmp(xn, "join:", 5) == 0;
    /* MELEE_XN_NET=lan: the lobby is on this network, with no service. */
    bool lan = net_env != NULL && strcmp(net_env, "lan") == 0;
    pc_xn_set_network(lan ? PC_XN_NET_LAN : PC_XN_NET_SERVICE);
    if ((!lan && (url == NULL || url[0] == '\0')) || (!join && strcmp(xn, "host") != 0)) {
        pc_log_line("xeleenet: MELEE_XN wants host or join:CODE, with MELEE_XN_URL (or MELEE_XN_NET=lan)");
        return true;
    }
    url = url != NULL ? url : "";
    if (name == NULL || name[0] == '\0') {
        name = pc_get_net_name();
    }
    if (!pc_xn_punch_start(url, name != NULL && name[0] ? name : "PLAYER", join ? xn + 5 : NULL,
            seats != NULL ? atoi(seats) : 2))
    {
        return true;
    }
    /* Boot waits here, as a MELEE_NET_PEERS session is up from boot. */
    uint64_t until = now_ms() + (uint64_t)(wait != NULL ? atoi(wait) : 300) * 1000;
    int r;
    /* pc_exit_requested: Ctrl-C or SIGTERM, which only set the flag. */
    while ((r = pc_xn_punch_poll()) == 0 && now_ms() < until && !pc_exit_requested) {
        SDL_Delay(5);
    }
    if (r != 1) {
        pc_log_line("xeleenet: no session (%s)", r < 0 ? s_why : "timed out");
        pc_xn_punch_cancel();
    } else {
        net.direct = true; /* up from boot: the session agrees its own match */
    }
    return true;
}

void pc_xn_punch_request_start(void) {
    if (s_code[0] == '\0' && !s_start_req) {
        s_start_req = true; /* a host: start as soon as anyone has joined */
        pc_log_line("xeleenet: the host starts the session with the first to join");
    }
}

bool pc_xn_punch_left(int port) {
    if (port < 1 || port > 3 || port == s_local || !s_who[port].known) {
        return false;
    }
    Who* w = &s_who[port];
    const int d = s_ndeparted < 4 ? s_ndeparted++ : 3;
    memcpy(s_departed[d].key, w->key, 32);
    s_departed[d].joined = w->joined;
    pc_log_line("xeleenet: P%d (%s) left the lobby", port + 1, w->name);
    memset(w, 0, sizeof *w);
    if (s_local != 0) {
        return false; /* the host's next start names who plays */
    }
    for (int g = 1; g < 4; g++) {
        if (s_who[g].known) {
            return false; /* the others go on: roster_changed starts the next session */
        }
    }
    /* Nobody left with the host: no session at all. Its socket comes back to
     * the punch, and the lobby waits for the next to join, as when it opened. */
    if (s_riding) {
        pc_net_set_datagram_handler(NULL);
        s_riding = false;
    }
    sock_t sock = (sock_t)pc_net_release_socket();
    if (sock != SOCK_INVALID) {
        s_sock = sock;
    }
    memset(&s_start, 0, sizeof s_start);
    s_running_seed = 0;
    s_switch_at = -1;
    if (s_sock == SOCK_INVALID) {
        fail("Couldn't keep the lobby's socket");
        return true;
    }
    s_phase = PH_PUNCH;
    pc_log_line("xeleenet: the host is alone in the lobby again");
    return true;
}

void pc_xn_punch_end(void) {
    if (s_phase == PH_OFF) {
        return;
    }
    if (s_riding) {
        pc_net_set_datagram_handler(NULL);
        s_riding = false;
    }
    if (s_sock != SOCK_INVALID) {
        sock_close(s_sock);
        s_sock = SOCK_INVALID;
    }
    pc_xn_stop(); /* leave the lobby; a host closes it */
    s_phase = PH_OFF;
    s_local = -1;
    crypto_wipe(&s_id, sizeof s_id);
}

void pc_xn_punch_view(PcXnPunchView* v) {
    memset(v, 0, sizeof *v);
    v->phase = s_phase == PH_OFF                           ? PC_XN_PUNCH_OFF :
               s_phase == PH_STUN || s_phase == PH_SERVICE ? PC_XN_PUNCH_SERVICE :
               s_phase == PH_PUNCH                         ? PC_XN_PUNCH_LOBBY :
               s_phase == PH_STARTING                      ? PC_XN_PUNCH_STARTING :
               s_phase == PH_DONE                          ? PC_XN_PUNCH_UP :
                                                             PC_XN_PUNCH_FAILED;
    v->host = s_code[0] == '\0';
    v->error = s_why;
    memcpy(v->code, s_lobby_code, sizeof v->code);
    for (int p = 0; p < 4; p++) {
        bool me = p == s_local;
        if (!me && !s_who[p].known) {
            continue;
        }
        if (me && s_local < 0) {
            continue;
        }
        int i = v->nplayers++;
        v->players[i].port = p;
        v->players[i].local = me;
        v->players[i].linked = me || s_who[p].up;
        v->players[i].via_host = !me && s_who[p].gave;
        snprintf(v->players[i].name, sizeof v->players[i].name, "%s", me ? s_name : s_who[p].name);
    }
    int players = 0;
    v->can_start = s_local == 0 && s_phase == PH_PUNCH && links_decided(&players);
}

/* ---- the player's name and tag ------------------------------------------ */

static char s_xn_name[16];
static bool s_xn_name_read;

char* pc_xn_profile_file(const char* leaf, char* out, size_t cap) {
    const char* dir_env = getenv("MELEE_XN_IDENTITY");
    char* pref = dir_env == NULL ? SDL_GetPrefPath(NULL, "melee-pc") : NULL;
    const char* dir = dir_env != NULL ? dir_env : pref;
    if (dir == NULL) {
        return NULL;
    }
    snprintf(out, cap, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] == '/' ? "" : "/", leaf);
    SDL_free(pref);
    return out;
}

const char* pc_xn_name(void) {
    const char* env = getenv("MELEE_XN_NAME");
    if (env != NULL && env[0] != '\0') {
        return env;
    }
    if (!s_xn_name_read) {
        char path[1024];
        s_xn_name_read = true;
        FILE* f = pc_xn_profile_file("xeleenet-name.txt", path, sizeof path) ? fopen(path, "r") : NULL;
        if (f != NULL) {
            if (fgets(s_xn_name, sizeof s_xn_name, f) == NULL) {
                s_xn_name[0] = '\0';
            }
            s_xn_name[strcspn(s_xn_name, "\r\n")] = '\0';
            fclose(f);
        }
    }
    return s_xn_name;
}

void pc_xn_set_name(const char* name) {
    char path[1024];
    snprintf(s_xn_name, sizeof s_xn_name, "%.12s", name != NULL ? name : "");
    s_xn_name_read = true;
    FILE* f = pc_xn_profile_file("xeleenet-name.txt", path, sizeof path) ? fopen(path, "w") : NULL;
    if (f != NULL) {
        fprintf(f, "%s\n", s_xn_name);
        fclose(f);
    }
    pc_log_line("xeleenet: name is now %s", s_xn_name);
}

const char* pc_xn_tag(void) {
    static char tag[5];
    if (tag[0] == '\0') {
        PcNetIdentity id;
        const char* dir_env = getenv("MELEE_XN_IDENTITY");
        char* pref = dir_env == NULL ? SDL_GetPrefPath(NULL, "melee-pc") : NULL;
        if (pc_identity_load(&id, dir_env != NULL ? dir_env : pref, "PLAYER")) {
            const char* hash = strchr(id.code, '#');
            snprintf(tag, sizeof tag, "%.4s", hash != NULL ? hash + 1 : "");
        }
        crypto_wipe(&id, sizeof id);
        SDL_free(pref);
    }
    return tag;
}

bool pc_xn_identity(PcNetIdentity* out) {
    const char* dir_env = getenv("MELEE_XN_IDENTITY");
    char* pref = dir_env == NULL ? SDL_GetPrefPath(NULL, "melee-pc") : NULL;
    bool ok = pc_identity_load(out, dir_env != NULL ? dir_env : pref, "PLAYER");
    SDL_free(pref);
    return ok;
}
