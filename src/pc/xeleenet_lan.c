/* SPDX-License-Identifier: GPL-3.0-or-later */
/* XeleeNet on this network (docs/lan-lobbies-plan.md L2): the lobby
 * service's calls (xeleenet_backend.h) with the host's game for the service.
 *
 * The host opens a lobby socket (LOBBY_PORT, or any if that's taken) and
 * announces its lobby over mDNS (net_mdns.c, `_xeleenet._udp`): the lobby's
 * id, name and settings summary, its seats, the host, the build. A guest
 * finds it there, or is given its address, and sends a signed JOIN with its
 * key, name, build and punch addresses; the host answers with a signed
 * WELCOME (the lobby id, the guest's port, everyone already in and where to
 * reach them, the settings) or a REFUSE. That is what the service's join
 * reply carries, so the punch goes on as on XeleeNet: guests who joined
 * earlier hear of a newcomer from the host (its intros), and the host keeps
 * the member list, dropping one its game no longer links to
 * (pc_xn_set_roster), as the service does. A guest leaving sends LEAVE; a
 * host closing says goodbye on mDNS.
 *
 * Every socket and the mDNS state live on one thread; the game reads the
 * results under s_lock, as with the service's client. */
#include "compat.h"
#include "pc/net_internal.h"
#include "pc/net_mdns.h"
#include "pc/pc.h"
#include "pc/xr_scene.h"
#include "xeleenet_backend.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void pc_log_line(const char* fmt, ...);

#define SERVICE "_xeleenet._udp.local."
#define LOBBY_PORT 42424        /* a host's lobby socket, so an address alone finds it */
#define JOIN_RESEND_MS 300
#define JOIN_WAIT_MS 6000       /* no answer by then: nobody's hosting there */
#define FIND_WAIT_MS 4000       /* a listed lobby's announcement, when we haven't one yet */
#define ROSTER_GRACE_MS 20000   /* a member the host's game doesn't link to by then has gone */
#define BROWSE_KEEP_MS 15000    /* browsing goes on this long after the list last asked */
#define TICK_MS 20

/* ---- wire: byte arrays only, so no packing or byte order matters ---------- */

enum { L_JOIN = 'J', L_WELCOME = 'W', L_REFUSE = 'X', L_LEAVE = 'L' };

typedef struct LHead {
    uint8_t magic[3]; /* "XNL" */
    uint8_t version;
    uint8_t type;
    uint8_t reserved[3];
} LHead;

typedef struct LJoin {
    LHead h;
    uint8_t key[32];
    uint8_t nonce[8]; /* the WELCOME or REFUSE echoes it */
    char name[16], tag[12];
    char rev[48], disc[12];
    uint8_t proto, ncands, reserved[2];
    uint8_t lobby[16]; /* the lobby it saw listed; all zero: whichever is here */
    uint8_t cands[PC_XN_CANDS][6];
    uint8_t sig[64];
} LJoin;

typedef struct LMember {
    uint8_t key[32];
    char name[16], tag[12];
    uint8_t port, ncands, reserved[2];
    uint8_t cands[PC_XN_CANDS][6];
    uint8_t joined[4]; /* seconds into the lobby, a stay's stamp */
} LMember;

typedef struct LWelcome {
    LHead h;
    uint8_t nonce[8];
    uint8_t lobby[16];
    uint8_t port, seats, nmembers, reserved;
    LMember m[3];
    char lobby_name[24], host[16], host_tag[12];
    char settings[320];
    uint8_t sig[64]; /* the host's (m[] holds its key, port 0) */
} LWelcome;

typedef struct LRefuse {
    LHead h;
    uint8_t nonce[8];
    char reason[16];
} LRefuse;

typedef struct LLeave {
    LHead h;
    uint8_t key[32];
    uint8_t lobby[16];
    uint8_t sig[64];
} LLeave;

static void head(LHead* h, uint8_t type) {
    memset(h, 0, sizeof *h);
    memcpy(h->magic, "XNL", 3);
    h->version = 1;
    h->type = type;
}

static void put_cand(uint8_t out[6], uint32_t ip, uint16_t udp) {
    memcpy(out, &ip, 4); /* network order already */
    out[4] = (uint8_t)(udp >> 8);
    out[5] = (uint8_t)udp;
}

static void get_cand(const uint8_t in[6], uint32_t* ip, uint16_t* udp) {
    memcpy(ip, in, 4);
    *udp = (uint16_t)(in[4] << 8 | in[5]);
}

static void copy_str(char* dst, size_t cap, const char* src, size_t src_cap) {
    size_t n = strnlen(src, src_cap);
    n = n < cap - 1 ? n : cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void hex(const uint8_t* b, size_t n, char* out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = '\0';
}

static bool unhex(const char* s, uint8_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) {
            return false;
        }
        b[i] = (uint8_t)v;
    }
    return true;
}

/* A listed lobby's code: the first six of its id, upper case (it is only
 * ever an internal handle on LAN: lobbies there have no codes to share). */
static void code_of(const char* id_hex, char code[8]) {
    for (int i = 0; i < 6; i++) {
        char c = id_hex[i];
        code[i] = (char)(c >= 'a' && c <= 'f' ? c - 32 : c);
    }
    code[6] = '\0';
}

/* ---- state ------------------------------------------------------------------ */

typedef enum { M_OFF, M_HOST, M_JOIN } LanMode;

/* Given to the thread at the start; read-only after. */
static struct {
    PcNetIdentity id;
    char name[16], tag[12];
    int seats;
    LanMode mode;
    char target[64]; /* a join: a listed code, or "@address[:port]" */
    uint32_t ip[PC_XN_CANDS];
    uint16_t udp[PC_XN_CANDS];
    int ncands;
} s_args;

static SDL_Mutex* s_lock;
static SDL_Thread* s_thread;
static SDL_AtomicInt s_stop;
static SDL_AtomicInt s_alive; /* the thread hasn't returned */
/* The thread's results (s_lock). */
static PcXnStatus s_status = PC_XN_OFF;
static char s_reason[24], s_error[160];
static uint8_t s_lobby[16];
static char s_lobby_hex[33], s_code[8];
static int s_port = -1;
static PcXnMember s_members[3];
static int s_nmembers;
static uint8_t s_roster[3][32];
static int s_nroster;
/* Ports machines' other pads hold (docs/lan-multipad-plan.md): taken seats
 * that no joiner is given. The host's game names them with the roster. */
static uint8_t s_extra, s_extra_announced;
static uint8_t s_owners; /* each port's machine, two bits a port (pc_xn_set_roster) */
static PcXnListing s_info;
static bool s_info_known;
static char s_address[64]; /* the host's lobby socket, as the banner shows it */
static struct {
    char name[40];
    char settings[320];
    int seats;     /* the Lobby Settings' (pc_xn_update_settings); 0: s_args.seats */
    int result;    /* its answer: 0 none yet, 1 taken, -1 refused */
    bool announce; /* the thread announces the change at once */
} s_opts;
/* Browsing (s_lock): rows as heard, and when the list last asked. */
static PcXnListing s_rows[PC_XN_LIST_MAX];
static int s_nrows, s_others;
static uint64_t s_browse_asked;
static bool s_browse_full;
static bool s_unavailable;
/* The thread's own. */
static sock_t s_sock = SOCK_INVALID;
static uint16_t s_lobby_port;
static uint64_t s_t0;
static uint64_t s_joined_ms[3]; /* host: when each member (by s_members index) joined */
static char s_host_addr[64];    /* guest: where the host's lobby socket is */

static uint64_t now_ms(void) {
    return SDL_GetTicks();
}

static void set_failed(const char* reason, const char* error) {
    SDL_LockMutex(s_lock);
    snprintf(s_reason, sizeof s_reason, "%s", reason);
    snprintf(s_error, sizeof s_error, "%s", error);
    s_status = PC_XN_FAILED;
    SDL_UnlockMutex(s_lock);
    pc_log_line("xeleenet: LAN: %s (%s)", error, reason);
}

static bool same_build(int proto, const char* rev, const char* disc) {
    return proto == PC_NET_PROTO_VERSION && strcmp(rev, pc_app_rev()) == 0 && strcmp(disc, pc_net_disc_id()) == 0;
}

static bool open_socket(uint16_t want) {
    sock_startup();
    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock == SOCK_INVALID) {
        return false;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(want);
    if (bind(s_sock, (struct sockaddr*)&a, sizeof a) != 0) {
        a.sin_port = 0; /* another host on this machine has the usual port */
        if (bind(s_sock, (struct sockaddr*)&a, sizeof a) != 0) {
            sock_close(s_sock);
            s_sock = SOCK_INVALID;
            return false;
        }
    }
    socklen_t len = sizeof a;
    getsockname(s_sock, (struct sockaddr*)&a, &len);
    s_lobby_port = ntohs(a.sin_port);
    return sock_nonblock(s_sock);
}

static int recv_one(uint8_t* buf, size_t cap, struct sockaddr_in* from) {
    for (;;) {
        socklen_t len = sizeof *from;
        int n = (int)recvfrom(s_sock, (char*)buf, (int)cap, 0, (struct sockaddr*)from, &len);
#if defined(_WIN32)
        if (n < 0 && WSAGetLastError() == WSAECONNRESET) {
            continue;
        }
#endif
        return n;
    }
}

static void send_to(const void* buf, size_t n, const struct sockaddr_in* to) {
    sendto(s_sock, (const char*)buf, (int)n, 0, (const struct sockaddr*)to, sizeof *to);
}

static uint16_t lobby_port_default(void) {
    const char* p = getenv("MELEE_XN_LAN_PORT");
    return (uint16_t)(p != NULL && atoi(p) > 0 ? atoi(p) : LOBBY_PORT);
}

/* The lobby's address for the banner, under s_lock: the port only when it
 * isn't the one a typed address assumes. */
static void set_address(const char* ip, uint16_t port) {
    if (port == LOBBY_PORT) {
        snprintf(s_address, sizeof s_address, "%s", ip);
    } else {
        snprintf(s_address, sizeof s_address, "%s:%u", ip, port);
    }
}

/* ---- the host --------------------------------------------------------------- */

/* One seat in the announcement's `p`, with nothing to escape: its port, XR
 * (0/1), its tag in 8 characters, its name's length in a hex digit and the
 * name. A machine's other pad is its port, its machine's port and
 * "--------0" (docs/xeleenet-lobby-details-plan.md). */
static size_t seat_txt(char* out, size_t cap, int port, bool xr, const char* tag, const char* name) {
    const size_t len = strnlen(name, 15);
    return (size_t)snprintf(out, cap, "%d%d%-8.8s%zx%.*s", port, xr ? 1 : 0, tag, len, (int)len, name);
}

/* Our announcement: who we are and what the list shows. */
static void host_announce(void) {
    char proto[8], seats[4], taken[4], port[8], instance[32];
    char name[40], settings[320], fm[12] = "", sm[12] = "", seat_list[128];
    SDL_LockMutex(s_lock);
    snprintf(name, sizeof name, "%s", s_opts.name[0] ? s_opts.name : s_args.name);
    snprintf(settings, sizeof settings, "%s", s_opts.settings);
    snprintf(taken, sizeof taken, "%d", 1 + s_nmembers + __builtin_popcount(s_extra));
    snprintf(seats, sizeof seats, "%d", s_opts.seats ? s_opts.seats : s_args.seats);
    s_extra_announced = s_extra;
    /* Every seat in port order: us, the guests, other pads. */
    size_t o = seat_txt(seat_list, sizeof seat_list, 0, pc_xr_presenting(), s_args.tag, s_args.name);
    for (int q = 1; q < 4; q++) {
        for (int i = 0; i < s_nmembers; i++) {
            if (s_members[i].port == q) {
                o += seat_txt(seat_list + o, sizeof seat_list - o, q, s_members[i].xr, s_members[i].tag,
                              s_members[i].name);
            }
        }
        if (s_extra & (1u << q)) {
            o += (size_t)snprintf(seat_list + o, sizeof seat_list - o, "%d%d--------0", q, (s_owners >> (2 * q)) & 3);
        }
    }
    s_opts.announce = false;
    SDL_UnlockMutex(s_lock);
    /* The masks don't fit in `s` (a TXT value is 255 bytes at most): they
     * come last in it (xeleenet_menu.cpp settings_json), cut off here and
     * sent as keys of their own. */
    char* masks = strstr(settings, ",\"fighter_mask\":");
    unsigned f = 0, st = 0;
    if (masks != NULL && sscanf(masks, ",\"fighter_mask\":%u,\"stage_mask\":%u", &f, &st) == 2) {
        snprintf(fm, sizeof fm, "%x", f);
        snprintf(sm, sizeof sm, "%x", st);
        masks[0] = '}';
        masks[1] = '\0';
    }
    snprintf(proto, sizeof proto, "%d", PC_NET_PROTO_VERSION);
    snprintf(port, sizeof port, "%u", s_lobby_port);
    snprintf(instance, sizeof instance, "xn-%.16s", s_lobby_hex);
    const char* keys[] = {"v", "rev", "disc", "id", "name", "host", "tag", "seats", "taken", "port", "s", "fm", "sm",
                          "g", "p"};
    const char* values[] = {proto, pc_app_rev(), pc_net_disc_id(), s_lobby_hex, name, s_args.name, s_args.tag,
                            seats, taken, port, settings, fm, sm, pc_xn_playing() ? "1" : "0", seat_list};
    static bool logged;
    if (!logged) {
        logged = true;
        pc_log_line("xeleenet: LAN announcement: s %zu bytes, p %zu, fm %s, sm %s", strlen(settings),
                    strlen(seat_list), fm, sm);
    }
    pc_mdns_announce(instance, s_lobby_port, keys, values, (int)(sizeof keys / sizeof keys[0]));
}

static void refuse(const struct sockaddr_in* to, const uint8_t nonce[8], const char* reason) {
    LRefuse r;
    head(&r.h, L_REFUSE);
    memcpy(r.nonce, nonce, 8);
    memset(r.reason, 0, sizeof r.reason);
    snprintf(r.reason, sizeof r.reason, "%s", reason);
    send_to(&r, sizeof r, to);
}

static void member_wire(LMember* out, const PcXnMember* m, uint32_t joined_s) {
    memset(out, 0, sizeof *out);
    memcpy(out->key, m->key, 32);
    copy_str(out->name, sizeof out->name, m->name, sizeof m->name);
    copy_str(out->tag, sizeof out->tag, m->tag, sizeof m->tag);
    out->port = (uint8_t)m->port;
    out->ncands = (uint8_t)m->ncands;
    for (int c = 0; c < m->ncands && c < PC_XN_CANDS; c++) {
        put_cand(out->cands[c], m->ip[c], m->udp[c]);
    }
    for (int i = 0; i < 4; i++) {
        out->joined[i] = (uint8_t)(joined_s >> (24 - 8 * i));
    }
}

static void on_join(const LJoin* j, const struct sockaddr_in* from) {
    if (!pc_identity_verify(j->key, j->sig, j, offsetof(LJoin, sig))) {
        return;
    }
    char rev[48], disc[12];
    copy_str(rev, sizeof rev, j->rev, sizeof j->rev);
    copy_str(disc, sizeof disc, j->disc, sizeof j->disc);
    static const uint8_t none[16];
    if (memcmp(j->lobby, none, 16) != 0 && memcmp(j->lobby, s_lobby, 16) != 0) {
        refuse(from, j->nonce, "not_found");
        return;
    }
    if (!same_build(j->proto, rev, disc)) {
        refuse(from, j->nonce, "version");
        return;
    }
    SDL_LockMutex(s_lock);
    int at = -1;
    for (int i = 0; i < s_nmembers; i++) {
        if (memcmp(s_members[i].key, j->key, 32) == 0) {
            at = i; /* asked again (our answer was lost): the same seat */
        }
    }
    if (at < 0) {
        if (1 + s_nmembers + __builtin_popcount(s_extra) >= (s_opts.seats ? s_opts.seats : s_args.seats)) {
            SDL_UnlockMutex(s_lock);
            refuse(from, j->nonce, "full");
            return;
        }
        int port = 1;
        for (bool used = true; used && port < 4; port += used) {
            used = (s_extra & (1u << port)) != 0;
            for (int i = 0; i < s_nmembers; i++) {
                used |= s_members[i].port == port;
            }
        }
        at = s_nmembers++;
        PcXnMember* m = &s_members[at];
        memset(m, 0, sizeof *m);
        memcpy(m->key, j->key, 32);
        copy_str(m->name, sizeof m->name, j->name, sizeof j->name);
        copy_str(m->tag, sizeof m->tag, j->tag, sizeof j->tag);
        m->port = port;
        m->xr = (j->reserved[0] & 1) != 0;
        m->ncands = j->ncands < PC_XN_CANDS ? j->ncands : PC_XN_CANDS;
        for (int c = 0; c < m->ncands; c++) {
            get_cand(j->cands[c], &m->ip[c], &m->udp[c]);
        }
        m->joined = (double)(now_ms() - s_t0);
        s_joined_ms[at] = now_ms();
        pc_log_line("xeleenet: LAN: %s#%s joins as P%d", m->name, m->tag, port + 1);
    }
    LWelcome w;
    memset(&w, 0, sizeof w);
    head(&w.h, L_WELCOME);
    memcpy(w.nonce, j->nonce, 8);
    memcpy(w.lobby, s_lobby, 16);
    w.port = (uint8_t)s_members[at].port;
    w.seats = (uint8_t)(s_opts.seats ? s_opts.seats : s_args.seats);
    /* Us first, then everyone who joined before them. */
    PcXnMember self;
    memset(&self, 0, sizeof self);
    memcpy(self.key, s_args.id.public_key, 32);
    snprintf(self.name, sizeof self.name, "%s", s_args.name);
    snprintf(self.tag, sizeof self.tag, "%s", s_args.tag);
    self.port = 0;
    self.ncands = s_args.ncands;
    memcpy(self.ip, s_args.ip, sizeof self.ip);
    memcpy(self.udp, s_args.udp, sizeof self.udp);
    member_wire(&w.m[w.nmembers++], &self, 0);
    for (int i = 0; i < s_nmembers; i++) {
        if (i != at && s_members[i].joined < s_members[at].joined) {
            member_wire(&w.m[w.nmembers++], &s_members[i], (uint32_t)(s_members[i].joined / 1000));
        }
    }
    snprintf(w.lobby_name, sizeof w.lobby_name, "%s", s_opts.name[0] ? s_opts.name : s_args.name);
    snprintf(w.host, sizeof w.host, "%s", s_args.name);
    snprintf(w.host_tag, sizeof w.host_tag, "%s", s_args.tag);
    snprintf(w.settings, sizeof w.settings, "%s", s_opts.settings);
    SDL_UnlockMutex(s_lock);
    pc_identity_sign(&s_args.id, w.sig, &w, offsetof(LWelcome, sig));
    send_to(&w, sizeof w, from);
    host_announce(); /* the seats taken */
}

static void drop_member(int i, const char* why) {
    pc_log_line("xeleenet: LAN: P%d (%s) %s", s_members[i].port + 1, s_members[i].name, why);
    s_members[i] = s_members[s_nmembers - 1];
    s_joined_ms[i] = s_joined_ms[s_nmembers - 1];
    s_nmembers--;
}

static void on_leave(const LLeave* l) {
    if (memcmp(l->lobby, s_lobby, 16) != 0 || !pc_identity_verify(l->key, l->sig, l, offsetof(LLeave, sig))) {
        return;
    }
    SDL_LockMutex(s_lock);
    for (int i = 0; i < s_nmembers; i++) {
        if (memcmp(s_members[i].key, l->key, 32) == 0) {
            drop_member(i, "left");
            break;
        }
    }
    SDL_UnlockMutex(s_lock);
    host_announce();
}

/* As the service does: a member the host's game hasn't linked to within the
 * grace (or no longer links to) has gone. */
static void host_roster(void) {
    bool changed = false;
    SDL_LockMutex(s_lock);
    changed = s_extra != s_extra_announced; /* the list's seats taken */
    for (int i = 0; i < s_nmembers;) {
        bool linked = false;
        for (int r = 0; r < s_nroster; r++) {
            linked |= memcmp(s_roster[r], s_members[i].key, 32) == 0;
        }
        if (!linked && now_ms() - s_joined_ms[i] > ROSTER_GRACE_MS) {
            drop_member(i, "isn't linked: gone");
            changed = true;
        } else {
            i++;
        }
    }
    SDL_UnlockMutex(s_lock);
    if (changed) {
        host_announce();
    }
}

static bool host_begin(void) {
    if (!open_socket(lobby_port_default())) {
        set_failed("socket", "Couldn't open the LAN lobby's socket");
        return false;
    }
    pc_identity_random(s_lobby, sizeof s_lobby);
    SDL_LockMutex(s_lock);
    hex(s_lobby, 16, s_lobby_hex);
    code_of(s_lobby_hex, s_code);
    s_port = 0;
    s_nmembers = 0;
    s_info_known = false;
    s_status = PC_XN_IN;
    SDL_UnlockMutex(s_lock);
    pc_mdns_open(SERVICE);
    SDL_LockMutex(s_lock);
    set_address(pc_mdns_self_ip(), s_lobby_port);
    SDL_UnlockMutex(s_lock);
    host_announce();
    pc_log_line("xeleenet: hosting lobby %s, code %s, %d seats on LAN %s:%u (build %s, disc %s)", s_lobby_hex,
                s_code, s_args.seats, pc_mdns_self_ip(), s_lobby_port, pc_app_rev(), pc_net_disc_id());
    return true;
}

/* ---- the guest -------------------------------------------------------------- */

/* Where a listed lobby (by code) is: its announcement's address and port,
 * and its id. Waits for the announcement a while: a join can come before the
 * list has heard it (a headless run, or straight after opening). */
static bool find_listed(const char* code, struct sockaddr_in* to, uint8_t lobby[16]) {
    const uint64_t until = now_ms() + FIND_WAIT_MS;
    pc_mdns_open(SERVICE);
    do {
        pc_mdns_poll();
        PcMdnsRecord r[PC_MDNS_RECORDS];
        int n = pc_mdns_records(r, PC_MDNS_RECORDS);
        for (int i = 0; i < n; i++) {
            const char* id = pc_mdns_txt(&r[i], "id");
            char c[8];
            if (id == NULL || strlen(id) != 32 || strchr(r[i].ip, ':') != NULL) {
                continue;
            }
            code_of(id, c);
            if (strcmp(c, code) == 0 && unhex(id, lobby, 16) && r[i].port != 0) {
                memset(to, 0, sizeof *to);
                to->sin_family = AF_INET;
                to->sin_port = htons(r[i].port);
                inet_pton(AF_INET, r[i].ip, &to->sin_addr);
                snprintf(s_host_addr, sizeof s_host_addr, "%s:%u", r[i].ip, r[i].port);
                return true;
            }
        }
        SDL_Delay(50);
    } while (now_ms() < until && !SDL_GetAtomicInt(&s_stop));
    return false;
}

/* "address" or "address:port", IPv4. */
static bool parse_address(const char* text, struct sockaddr_in* to) {
    char host[48];
    snprintf(host, sizeof host, "%s", text);
    char* colon = strchr(host, ':');
    uint16_t port = lobby_port_default();
    if (colon != NULL) {
        *colon = '\0';
        port = (uint16_t)atoi(colon + 1);
    }
    memset(to, 0, sizeof *to);
    to->sin_family = AF_INET;
    to->sin_port = htons(port);
    return port != 0 && inet_pton(AF_INET, host, &to->sin_addr) == 1;
}

static void join_fill(LJoin* j, const uint8_t lobby[16], const uint8_t nonce[8]) {
    memset(j, 0, sizeof *j);
    head(&j->h, L_JOIN);
    memcpy(j->key, s_args.id.public_key, 32);
    memcpy(j->nonce, nonce, 8);
    snprintf(j->name, sizeof j->name, "%s", s_args.name);
    snprintf(j->tag, sizeof j->tag, "%s", s_args.tag);
    snprintf(j->rev, sizeof j->rev, "%s", pc_app_rev());
    snprintf(j->disc, sizeof j->disc, "%s", pc_net_disc_id());
    j->proto = PC_NET_PROTO_VERSION;
    j->ncands = (uint8_t)s_args.ncands;
    j->reserved[0] = pc_xr_presenting() ? 1 : 0; /* bit 0: in a headset, for the list's XR tag */
    memcpy(j->lobby, lobby, 16);
    for (int c = 0; c < s_args.ncands; c++) {
        put_cand(j->cands[c], s_args.ip[c], s_args.udp[c]);
    }
    pc_identity_sign(&s_args.id, j->sig, j, offsetof(LJoin, sig));
}

static void on_welcome(const LWelcome* w) {
    SDL_LockMutex(s_lock);
    memcpy(s_lobby, w->lobby, 16);
    hex(s_lobby, 16, s_lobby_hex);
    code_of(s_lobby_hex, s_code);
    s_port = w->port;
    s_nmembers = 0;
    for (int i = 0; i < w->nmembers && i < 3; i++) {
        const LMember* lm = &w->m[i];
        PcXnMember* m = &s_members[s_nmembers++];
        memset(m, 0, sizeof *m);
        memcpy(m->key, lm->key, 32);
        copy_str(m->name, sizeof m->name, lm->name, sizeof lm->name);
        copy_str(m->tag, sizeof m->tag, lm->tag, sizeof lm->tag);
        m->port = lm->port;
        m->ncands = lm->ncands < PC_XN_CANDS ? lm->ncands : PC_XN_CANDS;
        for (int c = 0; c < m->ncands; c++) {
            get_cand(lm->cands[c], &m->ip[c], &m->udp[c]);
        }
        m->joined = (double)((uint32_t)lm->joined[0] << 24 | lm->joined[1] << 16 | lm->joined[2] << 8 | lm->joined[3]) * 1000.0;
    }
    memset(&s_info, 0, sizeof s_info);
    copy_str(s_info.name, sizeof s_info.name, w->lobby_name, sizeof w->lobby_name);
    copy_str(s_info.host, sizeof s_info.host, w->host, sizeof w->host);
    copy_str(s_info.tag, sizeof s_info.tag, w->host_tag, sizeof w->host_tag);
    snprintf(s_info.code, sizeof s_info.code, "%s", s_code);
    char settings[sizeof w->settings + 1];
    copy_str(settings, sizeof settings, w->settings, sizeof w->settings);
    pc_xn_listing_settings(settings, &s_info);
    s_info.seats = w->seats;
    s_info.taken = 1 + w->nmembers;
    s_info_known = true;
    struct sockaddr_in at;
    if (parse_address(s_host_addr, &at)) {
        char ip[64];
        net_addr_text((const struct sockaddr*)&at, ip, sizeof ip);
        set_address(ip, ntohs(at.sin_port));
    }
    s_status = PC_XN_IN;
    SDL_UnlockMutex(s_lock);
    pc_log_line("xeleenet: joined lobby %s as P%d with %d others on LAN %s (build %s, disc %s)", s_lobby_hex,
                w->port + 1, w->nmembers, s_host_addr, pc_app_rev(), pc_net_disc_id());
}

static bool guest_join(void) {
    struct sockaddr_in to;
    uint8_t lobby[16] = {0};
    if (s_args.target[0] == '@') {
        if (!parse_address(s_args.target + 1, &to)) {
            set_failed("not_found", "That isn't an address on this network");
            return false;
        }
        snprintf(s_host_addr, sizeof s_host_addr, "%s", s_args.target + 1);
    } else if (!find_listed(s_args.target, &to, lobby)) {
        set_failed("not_found", "That lobby isn't on this network any more");
        return false;
    }
    if (!open_socket(0)) {
        set_failed("socket", "Couldn't open a socket");
        return false;
    }
    uint8_t nonce[8];
    pc_identity_random(nonce, sizeof nonce);
    LJoin j;
    join_fill(&j, lobby, nonce);
    const uint64_t until = now_ms() + JOIN_WAIT_MS;
    uint64_t next = 0;
    while (now_ms() < until && !SDL_GetAtomicInt(&s_stop)) {
        if (now_ms() >= next) {
            send_to(&j, sizeof j, &to);
            next = now_ms() + JOIN_RESEND_MS;
        }
        uint8_t buf[sizeof(LWelcome) + 16];
        struct sockaddr_in from;
        int n;
        while ((n = recv_one(buf, sizeof buf, &from)) > 0) {
            const LHead* h = (const LHead*)buf;
            if (n < (int)sizeof(LHead) || memcmp(h->magic, "XNL", 3) != 0 || h->version != 1) {
                continue;
            }
            if (h->type == L_REFUSE && n == (int)sizeof(LRefuse) &&
                memcmp(((const LRefuse*)buf)->nonce, nonce, 8) == 0)
            {
                char reason[17];
                copy_str(reason, sizeof reason, ((const LRefuse*)buf)->reason, 16);
                set_failed(reason, strcmp(reason, "full") == 0      ? "The lobby is full" :
                                   strcmp(reason, "version") == 0   ? "The lobby is on another Xelee version" :
                                                                      "That lobby isn't on this network any more");
                return false;
            }
            if (h->type == L_WELCOME && n == (int)sizeof(LWelcome)) {
                const LWelcome* w = (const LWelcome*)buf;
                bool host_key = false;
                for (int i = 0; i < w->nmembers && i < 3; i++) {
                    host_key |= w->m[i].port == 0 &&
                                pc_identity_verify(w->m[i].key, w->sig, w, offsetof(LWelcome, sig));
                }
                if (memcmp(w->nonce, nonce, 8) == 0 && host_key && w->port >= 1 && w->port <= 3) {
                    on_welcome(w);
                    return true;
                }
            }
        }
        SDL_Delay(TICK_MS);
    }
    set_failed("offline", "No lobby answered there");
    return false;
}

static void guest_leave(void) {
    LLeave l;
    struct sockaddr_in to;
    if (s_sock == SOCK_INVALID || !parse_address(s_host_addr, &to)) {
        return;
    }
    head(&l.h, L_LEAVE);
    memcpy(l.key, s_args.id.public_key, 32);
    memcpy(l.lobby, s_lobby, 16);
    pc_identity_sign(&s_args.id, l.sig, &l, offsetof(LLeave, sig));
    for (int i = 0; i < 3; i++) {
        send_to(&l, sizeof l, &to);
    }
}

/* ---- browsing ---------------------------------------------------------------- */

/* An announcement's `p` (seat_txt) into a listing's seats. */
static void parse_seats(const char* p, PcXnListing* x) {
    x->nplayers = 0;
    while (p != NULL && strlen(p) >= 11 && x->nplayers < 4 && p[0] >= '0' && p[0] <= '3') {
        PcXnSeat* s = &x->players[x->nplayers];
        memset(s, 0, sizeof *s);
        s->port = (uint8_t)(p[0] - '0');
        if (strncmp(p + 2, "--------", 8) == 0) {
            s->extra = true;
            s->owner = (uint8_t)((p[1] - '0') & 3);
            x->nplayers++;
            p += 11;
            continue;
        }
        s->xr = p[1] == '1';
        x->xr |= s->xr;
        copy_str(s->tag, sizeof s->tag, p + 2, 8);
        for (size_t n = strlen(s->tag); n > 0 && s->tag[n - 1] == ' '; n--) {
            s->tag[n - 1] = '\0';
        }
        const char c = p[10];
        const size_t len = c >= '0' && c <= '9' ? (size_t)(c - '0') : c >= 'a' && c <= 'f' ? (size_t)(c - 'a' + 10) : 0;
        if (strlen(p + 11) < len) {
            break;
        }
        copy_str(s->name, sizeof s->name, p + 11, len);
        x->nplayers++;
        p += 11 + len;
    }
}

static void browse_tick(void) {
    pc_mdns_open(SERVICE); /* a browse alone (M_OFF) has opened nothing yet */
    pc_mdns_poll();
    PcMdnsRecord r[PC_MDNS_RECORDS];
    int n = pc_mdns_records(r, PC_MDNS_RECORDS);
    PcXnListing rows[PC_XN_LIST_MAX];
    int nrows = 0, others = 0;
    SDL_LockMutex(s_lock);
    const bool full = s_browse_full;
    SDL_UnlockMutex(s_lock);
    for (int i = 0; i < n && nrows < PC_XN_LIST_MAX; i++) {
        const char* id = pc_mdns_txt(&r[i], "id");
        const char* v = pc_mdns_txt(&r[i], "v");
        const char* rev = pc_mdns_txt(&r[i], "rev");
        const char* disc = pc_mdns_txt(&r[i], "disc");
        if (id == NULL || strlen(id) != 32 || v == NULL || rev == NULL || disc == NULL) {
            continue;
        }
        if (!same_build(atoi(v), rev, disc)) {
            others++;
            continue;
        }
        PcXnListing* x = &rows[nrows];
        memset(x, 0, sizeof *x);
        code_of(id, x->code);
        const char* s;
        if ((s = pc_mdns_txt(&r[i], "name")) != NULL)
            snprintf(x->name, sizeof x->name, "%s", s);
        if ((s = pc_mdns_txt(&r[i], "host")) != NULL)
            snprintf(x->host, sizeof x->host, "%s", s);
        if ((s = pc_mdns_txt(&r[i], "tag")) != NULL)
            snprintf(x->tag, sizeof x->tag, "%s", s);
        pc_xn_listing_settings(pc_mdns_txt(&r[i], "s"), x);
        if ((s = pc_mdns_txt(&r[i], "seats")) != NULL)
            x->seats = atoi(s);
        x->taken = (s = pc_mdns_txt(&r[i], "taken")) != NULL ? atoi(s) : 1;
        /* The lists' masks (else the named lists', from `s`), in a game or
         * not, the seats, and where the host is. */
        if ((s = pc_mdns_txt(&r[i], "fm")) != NULL && strtoul(s, NULL, 16) != 0)
            x->fighter_mask = (uint32_t)strtoul(s, NULL, 16);
        if ((s = pc_mdns_txt(&r[i], "sm")) != NULL && strtoul(s, NULL, 16) != 0)
            x->stage_mask = (uint32_t)strtoul(s, NULL, 16);
        x->playing = (s = pc_mdns_txt(&r[i], "g")) != NULL && s[0] == '1';
        parse_seats(pc_mdns_txt(&r[i], "p"), x);
        if (r[i].port == LOBBY_PORT)
            snprintf(x->address, sizeof x->address, "%s", r[i].ip);
        else
            snprintf(x->address, sizeof x->address, "%s:%u", r[i].ip, r[i].port);
        if (x->taken >= x->seats && !full) {
            continue;
        }
        nrows++;
    }
    SDL_LockMutex(s_lock);
    memcpy(s_rows, rows, (size_t)nrows * sizeof rows[0]);
    s_nrows = nrows;
    s_others = others;
    s_unavailable = pc_mdns_unavailable();
    SDL_UnlockMutex(s_lock);
}

/* ---- the thread ---------------------------------------------------------------- */

static int thread_main(void* unused) {
    (void)unused;
    LanMode mode;
    SDL_LockMutex(s_lock);
    mode = s_args.mode;
    SDL_UnlockMutex(s_lock);
    s_t0 = now_ms();
    if (mode == M_HOST && !host_begin()) {
        mode = M_OFF;
    } else if (mode == M_JOIN && !guest_join()) {
        mode = M_OFF;
    }
    uint64_t announced = now_ms();
    while (!SDL_GetAtomicInt(&s_stop)) {
        SDL_LockMutex(s_lock);
        const bool browsing = s_browse_asked != 0 && now_ms() - s_browse_asked < BROWSE_KEEP_MS;
        SDL_UnlockMutex(s_lock);
        if (mode == M_HOST) {
            uint8_t buf[sizeof(LWelcome)];
            struct sockaddr_in from;
            int n;
            while ((n = recv_one(buf, sizeof buf, &from)) > 0) {
                const LHead* h = (const LHead*)buf;
                if (n < (int)sizeof(LHead) || memcmp(h->magic, "XNL", 3) != 0 || h->version != 1) {
                    continue;
                }
                if (h->type == L_JOIN && n == (int)sizeof(LJoin)) {
                    on_join((const LJoin*)buf, &from);
                } else if (h->type == L_LEAVE && n == (int)sizeof(LLeave)) {
                    on_leave((const LLeave*)buf);
                }
            }
            host_roster();
            SDL_LockMutex(s_lock);
            const bool changed = s_opts.announce;
            SDL_UnlockMutex(s_lock);
            if (changed || now_ms() - announced > 3000) {
                host_announce(); /* the settings may have changed (the CSS's own) */
                announced = now_ms();
            }
        }
        if (mode == M_HOST || browsing) {
            browse_tick();
        } else if (mode == M_OFF && !browsing) {
            break; /* nothing to do: the next browse or start starts us again */
        }
        SDL_Delay(TICK_MS);
    }
    if (mode == M_JOIN) {
        guest_leave();
    }
    pc_mdns_close();
    if (s_sock != SOCK_INVALID) {
        sock_close(s_sock);
        s_sock = SOCK_INVALID;
    }
    SDL_SetAtomicInt(&s_alive, 0);
    return 0;
}

static void ensure_lock(void) {
    if (s_lock == NULL) {
        s_lock = SDL_CreateMutex();
    }
}

static void join_thread(void) {
    if (s_thread != NULL) {
        SDL_SetAtomicInt(&s_stop, 1);
        SDL_WaitThread(s_thread, NULL);
        s_thread = NULL;
    }
}

static bool run_thread(void) {
    SDL_SetAtomicInt(&s_stop, 0);
    SDL_SetAtomicInt(&s_alive, 1);
    s_thread = SDL_CreateThread(thread_main, "xeleenet lan", NULL);
    if (s_thread == NULL) {
        SDL_SetAtomicInt(&s_alive, 0);
    }
    return s_thread != NULL;
}

/* ---- the calls (xeleenet_backend.h) --------------------------------------------- */

static bool lan_start(const char* url, const PcNetIdentity* id, const char* name, const char* code, int seats,
    const uint32_t* ip, const uint16_t* udp, int ncands) {
    (void)url;
    ensure_lock();
    SDL_LockMutex(s_lock);
    const bool busy = s_status == PC_XN_WORKING || s_status == PC_XN_IN;
    SDL_UnlockMutex(s_lock);
    if (busy) {
        return false;
    }
    join_thread(); /* a browse's */
    memset(&s_args, 0, sizeof s_args);
    s_args.id = *id;
    snprintf(s_args.name, sizeof s_args.name, "%.12s", name);
    snprintf(s_args.tag, sizeof s_args.tag, "%s", pc_identity_code_suffix(id->code));
    s_args.seats = seats < 2 ? 2 : seats > 4 ? 4 : seats;
    s_args.mode = code == NULL ? M_HOST : M_JOIN;
    snprintf(s_args.target, sizeof s_args.target, "%s", code != NULL ? code : "");
    for (int i = 0; i < ncands && i < PC_XN_CANDS; i++) {
        s_args.ip[i] = ip[i];
        s_args.udp[i] = udp[i];
        s_args.ncands++;
    }
    SDL_LockMutex(s_lock);
    s_status = PC_XN_WORKING;
    s_reason[0] = s_error[0] = '\0';
    s_port = -1;
    s_nmembers = s_nroster = 0;
    s_info_known = false;
    SDL_UnlockMutex(s_lock);
    if (!run_thread()) {
        set_failed("thread", "Couldn't start the LAN lobby");
    }
    return true;
}

static PcXnStatus lan_status(void) {
    if (s_lock == NULL) {
        return PC_XN_OFF;
    }
    SDL_LockMutex(s_lock);
    PcXnStatus s = s_status;
    SDL_UnlockMutex(s_lock);
    return s;
}

static const char* lan_error(const char** reason) {
    if (reason != NULL) {
        *reason = s_reason;
    }
    return s_error;
}

static bool lan_lobby(char id[33], char code[8], int* port) {
    if (lan_status() != PC_XN_IN) {
        return false;
    }
    SDL_LockMutex(s_lock);
    memcpy(id, s_lobby_hex, 33);
    memcpy(code, s_code, 8);
    *port = s_port;
    SDL_UnlockMutex(s_lock);
    return true;
}

static int lan_members(PcXnMember* out, int max) {
    if (s_lock == NULL) {
        return 0;
    }
    SDL_LockMutex(s_lock);
    int n = s_nmembers < max ? s_nmembers : max;
    memcpy(out, s_members, (size_t)n * sizeof *out);
    SDL_UnlockMutex(s_lock);
    return n;
}

static void lan_set_roster(const uint8_t (*keys)[32], int n, uint8_t extra, uint8_t owners) {
    if (s_lock == NULL) {
        return;
    }
    SDL_LockMutex(s_lock);
    s_extra = (uint8_t)(extra & 14u);
    s_owners = owners;
    s_nroster = n < 3 ? n : 3;
    memcpy(s_roster, keys, (size_t)s_nroster * 32);
    SDL_UnlockMutex(s_lock);
}

static void lan_stop(void) {
    join_thread();
    if (s_lock != NULL) {
        SDL_LockMutex(s_lock);
        s_status = PC_XN_OFF;
        s_nmembers = 0;
        SDL_UnlockMutex(s_lock);
    }
}

static void lan_host_options(const char* name, bool open, const char* settings_json) {
    (void)open; /* every LAN lobby is listed (docs/lan-lobbies-plan.md decision 4) */
    ensure_lock();
    SDL_LockMutex(s_lock);
    snprintf(s_opts.name, sizeof s_opts.name, "%s", name != NULL ? name : "");
    snprintf(s_opts.settings, sizeof s_opts.settings, "%s", settings_json != NULL ? settings_json : "");
    s_opts.seats = 0; /* the next lobby opens with its own */
    s_opts.result = 0;
    SDL_UnlockMutex(s_lock);
}

static bool lan_update_settings(const char* name, bool open, const char* settings_json) {
    (void)open; /* every LAN lobby is listed */
    if (s_lock == NULL || lan_status() != PC_XN_IN || s_args.mode != M_HOST) {
        return false;
    }
    PcXnListing x;
    memset(&x, 0, sizeof x);
    pc_xn_listing_settings(settings_json, &x);
    SDL_LockMutex(s_lock);
    const int taken = 1 + s_nmembers + __builtin_popcount(s_extra);
    if (x.seats != 0 && x.seats < taken) {
        s_opts.result = -1; /* no service to ask: its refusal, made here */
        SDL_UnlockMutex(s_lock);
        return true;
    }
    snprintf(s_opts.name, sizeof s_opts.name, "%s", name != NULL ? name : "");
    snprintf(s_opts.settings, sizeof s_opts.settings, "%s", settings_json != NULL ? settings_json : "");
    if (x.seats != 0) {
        s_opts.seats = x.seats;
    }
    s_opts.result = 1;
    s_opts.announce = true;
    SDL_UnlockMutex(s_lock);
    pc_log_line("xeleenet: LAN: settings taken (%d seats)", x.seats);
    return true;
}

static int lan_settings_result(char* why, size_t cap) {
    if (s_lock == NULL) {
        return 0;
    }
    SDL_LockMutex(s_lock);
    const int r = s_opts.result;
    const int taken = 1 + s_nmembers + __builtin_popcount(s_extra);
    s_opts.result = 0;
    SDL_UnlockMutex(s_lock);
    if (r < 0 && why != NULL) {
        snprintf(why, cap, "%d players are in the lobby; remove someone first", taken);
    }
    return r;
}

static bool lan_lobby_info(PcXnListing* out) {
    if (lan_status() != PC_XN_IN) {
        return false;
    }
    SDL_LockMutex(s_lock);
    bool known = s_info_known;
    if (known) {
        *out = s_info;
    }
    SDL_UnlockMutex(s_lock);
    return known;
}

static bool lan_browse(const char* url, const PcNetIdentity* id, bool full) {
    (void)url, (void)id;
    ensure_lock();
    SDL_LockMutex(s_lock);
    s_browse_asked = now_ms();
    s_browse_full = full;
    SDL_UnlockMutex(s_lock);
    if (SDL_GetAtomicInt(&s_alive)) {
        return true; /* browsing already (a run's thread browses too) */
    }
    join_thread(); /* a thread that has returned */
    SDL_LockMutex(s_lock);
    s_args.mode = M_OFF;
    SDL_UnlockMutex(s_lock);
    return run_thread();
}

static int lan_browse_poll(PcXnListing* out, int max, int* n, int* others) {
    if (s_lock == NULL) {
        return -1;
    }
    SDL_LockMutex(s_lock);
    s_browse_asked = now_ms();
    *n = s_nrows < max ? s_nrows : max;
    memcpy(out, s_rows, (size_t)*n * sizeof *out);
    *others = s_others;
    SDL_UnlockMutex(s_lock);
    return 1; /* live: whatever has been heard */
}

bool pc_xn_lan_address(char* out, size_t size) {
    if (s_lock == NULL || lan_status() != PC_XN_IN) {
        return false;
    }
    SDL_LockMutex(s_lock);
    snprintf(out, size, "%s", s_address);
    SDL_UnlockMutex(s_lock);
    return out[0] != '\0';
}

bool pc_xn_lan_unavailable(void) {
    if (s_lock == NULL) {
        return false;
    }
    SDL_LockMutex(s_lock);
    bool u = s_unavailable;
    SDL_UnlockMutex(s_lock);
    return u;
}

const PcXnBackend pc_xn_lan_backend = {
    lan_start, lan_status, lan_error, lan_lobby, lan_members, lan_set_roster, lan_stop,
    lan_host_options, lan_update_settings, lan_settings_result, lan_lobby_info, lan_browse, lan_browse_poll,
};
