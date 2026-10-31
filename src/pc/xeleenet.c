/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xeleenet.h"
#include "lang.h"
#include "xeleenet_backend.h"
#include "net.h"
#include "pc.h"
#include "pc/slippi/slp_json.h"
#include "pc/slippi/slp_user.h"
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

extern void pc_log_line(const char* fmt, ...);

#define HEARTBEAT_MS 2000          /* the host's, at the CSS */
#define HEARTBEAT_PLAYING_MS 10000 /* in a game: nobody joins until the CSS, and each beat is billed */
#define JOIN_RETRIES 10 /* "pending" answers on Ask before joining (XN3) */
#define REPLY_CAP 16384

/* Everything the thread is given at the start; read-only after. */
static struct {
    char url[256];
    PcNetIdentity id;
    char name[16];
    char code[8];
    int seats;
    char cands[PC_XN_CANDS][24];
    int ncands;
} s_args;

/* The thread's results (s_lock). */
static SDL_Mutex* s_lock;
static SDL_Thread* s_thread;
static SDL_AtomicInt s_stop;
static SDL_AtomicInt s_playing; /* pc_xn_set_playing */
static PcXnStatus s_status;
static char s_reason[24], s_error[160];
static char s_lobby[33], s_lobby_code[8];
static int s_port;
static PcXnMember s_members[3];
static int s_nmembers;
static uint8_t s_roster[3][32];
static int s_nroster;
static uint8_t s_extra; /* ports other pads hold (docs/lan-multipad-plan.md) */

static char s_token[256]; /* the thread's own */
static PcXnListing s_info;  /* the lobby we're in (s_lock) */
static struct {
    char name[40];
    bool open;
    char settings[512];
} s_host_opts = {"", false, ""};
/* Browse's own thread and results (s_lock). */
static SDL_Thread* s_browse_thread;
static int s_browse_state; /* 0 fetching, 1 done, -1 failed */
static bool s_browse_full;
static PcXnListing s_rows[PC_XN_LIST_MAX];
static int s_nrows, s_others;
static char s_last[160];  /* the start of the last reply, for the log */
static char s_pubkey[48];

static void b64url(const uint8_t* in, size_t n, char* out) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) |
                     (i + 2 < n ? in[i + 2] : 0);
        out[o++] = a[v >> 18 & 63];
        out[o++] = a[v >> 12 & 63];
        if (i + 1 < n) {
            out[o++] = a[v >> 6 & 63];
        }
        if (i + 2 < n) {
            out[o++] = a[v & 63];
        }
    }
    out[o] = '\0';
}

static int b64val(char c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' :
           c >= 'a' && c <= 'z' ? c - 'a' + 26 :
           c >= '0' && c <= '9' ? c - '0' + 52 :
           c == '-'             ? 62 :
           c == '_'             ? 63 :
                                  -1;
}

/* Exactly `n` bytes from unpadded base64url, or false. */
static bool unb64url(const char* in, size_t len, uint8_t* out, size_t n) {
    if (len != (n * 4 + 2) / 3) {
        return false;
    }
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64val(in[i]);
        if (v < 0) {
            return false;
        }
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < n) {
                out[o++] = (uint8_t)(acc >> bits);
            }
        }
    }
    return o == n;
}

static void fail(const char* reason, const char* error) {
    SDL_LockMutex(s_lock);
    s_status = PC_XN_FAILED;
    snprintf(s_reason, sizeof s_reason, "%s", reason);
    snprintf(s_error, sizeof s_error, "%s", error);
    SDL_UnlockMutex(s_lock);
    pc_log_line("xeleenet: %s (%s)", error, reason);
}

/* One call. GET when `body` is NULL. The parsed reply in *out (free it
 * before the next call) on any answer, 2xx or a refusal; false when the
 * service was not reached. */
static bool call(const char* path, const char* body, bool auth, int* status, PcSlippiJson** out) {
    /* The parsed tree points into the text, so the text outlives the call:
     * one buffer, the thread's own, reused by the next call. */
    static char reply[REPLY_CAP];
    char url[384];
    *out = NULL;
    snprintf(url, sizeof url, "%s%s", s_args.url, path);
    const char* bearer = auth ? s_token : NULL;
    bool ok = body != NULL ? pc_slippi_http_post_json(url, body, bearer, reply, REPLY_CAP, status) :
                             pc_slippi_http_get(url, bearer, reply, REPLY_CAP, status);
    snprintf(s_last, sizeof s_last, "%s", ok ? reply : "");
    if (ok && *status > 0) {
        char err[64];
        if (!pc_slippi_json_parse(reply, strlen(reply), out, err, sizeof err)) {
            pc_log_line("xeleenet: %s answered %d with no JSON (%s)", path, *status, err);
            ok = false;
        }
    }
    return ok && *out != NULL;
}

static void refusal(const PcSlippiJson* r, int status) {
    char reason[24], error[160];
    if (!pc_slippi_json_string(pc_slippi_json_get(r, "reason"), reason, sizeof reason)) {
        snprintf(reason, sizeof reason, "http_%d", status);
    }
    if (!pc_slippi_json_string(pc_slippi_json_get(r, "error"), error, sizeof error)) {
        snprintf(error, sizeof error, "XeleeNet answered %d", status);
        pc_log_line("xeleenet: unexpected reply %d: %s", status, s_last);
    }
    fail(reason, error);
}

static bool sign_in(void) {
    char body[512], nonce[96], msg[256], sig_text[96];
    int status = 0;
    PcSlippiJson* r;
    snprintf(body, sizeof body, "{\"pubkey\":\"%s\"}", s_pubkey);
    if (!call("/v1/session", body, false, &status, &r)) {
        fail("offline", "Couldn't reach XeleeNet");
        return false;
    }
    bool got = status == 200 && pc_slippi_json_string(pc_slippi_json_get(r, "nonce"), nonce, sizeof nonce);
    if (!got) {
        refusal(r, status);
    }
    pc_slippi_json_free(r);
    if (!got) {
        return false;
    }
    int n = snprintf(msg, sizeof msg, "xeleenet-session-v1\n%s\n%s", nonce, s_pubkey);
    uint8_t sig[64];
    pc_identity_sign(&s_args.id, sig, msg, (size_t)n);
    b64url(sig, sizeof sig, sig_text);
    snprintf(body, sizeof body, "{\"pubkey\":\"%s\",\"nonce\":\"%s\",\"sig\":\"%s\"}", s_pubkey,
        nonce, sig_text);
    if (!call("/v1/session/verify", body, false, &status, &r)) {
        fail("offline", "Couldn't reach XeleeNet");
        return false;
    }
    got = status == 200 && pc_slippi_json_string(pc_slippi_json_get(r, "token"), s_token, sizeof s_token);
    if (!got) {
        refusal(r, status);
    }
    pc_slippi_json_free(r);
    return got;
}

/* A call that signs in again once if the token ran out (an hour). */
static bool call_auth(const char* path, const char* body, int* status, PcSlippiJson** out) {
    if (!call(path, body, true, status, out)) {
        return false;
    }
    if (*status == 401) {
        pc_slippi_json_free(*out);
        *out = NULL;
        return sign_in() && call(path, body, true, status, out);
    }
    return true;
}

/* "a.b.c.d:port"; IPv6 ones are skipped (the socket is IPv4). */
static bool parse_cand(const char* s, uint32_t* ip, uint16_t* udp) {
    unsigned a, b, c, d, p;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &p, &tail) != 5 || a > 255 || b > 255 ||
        c > 255 || d > 255 || p == 0 || p > 65535)
    {
        return false;
    }
    *ip = htonl(a << 24 | b << 16 | c << 8 | d);
    *udp = (uint16_t)p;
    return true;
}

static int parse_members(const PcSlippiJson* list, PcXnMember* out, int max) {
    int n = 0;
    for (int i = 0; list != NULL && i < list->child_count && n < max; i++) {
        const PcSlippiJson* m = pc_slippi_json_at(list, i);
        PcXnMember x;
        memset(&x, 0, sizeof x);
        const PcSlippiJson* key = pc_slippi_json_get(m, "key");
        if (key == NULL || key->type != PC_SLIPPI_JSON_STRING ||
            !unb64url(key->str, key->str_len, x.key, sizeof x.key))
        {
            continue;
        }
        pc_slippi_json_string(pc_slippi_json_get(m, "name"), x.name, sizeof x.name);
        pc_slippi_json_string(pc_slippi_json_get(m, "tag"), x.tag, sizeof x.tag);
        x.port = pc_slippi_json_int(pc_slippi_json_get(m, "port"), -1);
        x.joined = pc_slippi_json_num(pc_slippi_json_get(m, "joined"), 0);
        const PcSlippiJson* cands = pc_slippi_json_get(m, "candidates");
        for (int c = 0; cands != NULL && c < cands->child_count && x.ncands < PC_XN_CANDS; c++) {
            char text[64];
            if (pc_slippi_json_string(pc_slippi_json_at(cands, c), text, sizeof text) &&
                parse_cand(text, &x.ip[x.ncands], &x.udp[x.ncands]))
            {
                x.ncands++;
            }
        }
        if (x.port >= 0 && x.port <= 3 && x.ncands > 0) {
            out[n++] = x;
        }
    }
    return n;
}

static void str_of(const PcSlippiJson* obj, const char* key, char* out, size_t cap) {
    if (!pc_slippi_json_string(pc_slippi_json_get(obj, key), out, cap)) {
        out[0] = '\0';
    }
}

/* A summary (the service's settings object) into a listing. */
static void parse_settings(const PcSlippiJson* st, PcXnListing* x) {
    char v[16];
    str_of(st, "preset", x->preset, sizeof x->preset);
    x->seats = pc_slippi_json_int(pc_slippi_json_get(st, "seats"), 2);
    str_of(st, "mode", v, sizeof v);
    x->teams = strcmp(v, "teams") == 0;
    str_of(st, "win", v, sizeof v);
    x->stock = strcmp(v, "time") != 0;
    x->stocks = pc_slippi_json_int(pc_slippi_json_get(st, "stocks"), 4);
    x->time_min = pc_slippi_json_int(pc_slippi_json_get(st, "time_min"), 8);
    str_of(st, "items", x->items, sizeof x->items);
    str_of(st, "stage_list", x->stage_list, sizeof x->stage_list);
    str_of(st, "fighter_list", x->fighter_list, sizeof x->fighter_list);
    str_of(st, "format", v, sizeof v);
    x->first_to = strcmp(v, "first_to") == 0;
    x->first_to_n = pc_slippi_json_int(pc_slippi_json_get(st, "first_to"), 2);
}

bool pc_xn_listing_settings(const char* json, PcXnListing* x) {
    PcSlippiJson* st = NULL;
    char err[64];
    if (json == NULL || !pc_slippi_json_parse(json, strlen(json), &st, err, sizeof err)) {
        return false;
    }
    parse_settings(st, x);
    pc_slippi_json_free(st);
    return true;
}

static void parse_listing(const PcSlippiJson* row, PcXnListing* x) {
    char v[16];
    memset(x, 0, sizeof *x);
    str_of(row, "code", x->code, sizeof x->code);
    str_of(row, "name", x->name, sizeof x->name);
    const PcSlippiJson* host = pc_slippi_json_get(row, "host");
    str_of(host, "name", x->host, sizeof x->host);
    str_of(host, "tag", x->tag, sizeof x->tag);
    str_of(row, "country", x->country, sizeof x->country);
    x->taken = pc_slippi_json_int(pc_slippi_json_get(row, "taken"), 1);
    str_of(row, "state", v, sizeof v);
    x->playing = strcmp(v, "playing") == 0;
    x->xr = pc_slippi_json_bool(pc_slippi_json_get(row, "xr"), false);
    parse_settings(pc_slippi_json_get(row, "settings"), x);
}

static void set_members(const PcSlippiJson* list) {
    PcXnMember m[3];
    int n = parse_members(list, m, 3);
    SDL_LockMutex(s_lock);
    memcpy(s_members, m, sizeof m);
    s_nmembers = n;
    SDL_UnlockMutex(s_lock);
}

/* The parts of the request every lobby call shares. */
static void common_fields(char* out, size_t cap) {
    char rev[48], name[40], cands[160];
    size_t r = 0;
    for (const char* p = pc_app_rev(); *p && r + 1 < sizeof rev; p++) {
        char c = *p;
        rev[r++] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                           c == '.' || c == '_' || c == '+' || c == '-' ?
                       c :
                       '-';
    }
    rev[r] = '\0';
    pc_slippi_json_escape(s_args.name, name, sizeof name);
    size_t o = 0;
    for (int i = 0; i < s_args.ncands; i++) {
        o += (size_t)snprintf(cands + o, sizeof cands - o, "%s\"%s\"", i ? "," : "", s_args.cands[i]);
    }
    snprintf(out, cap,
        "\"player\":\"%s\",\"xr\":false,\"candidates\":[%s],"
        "\"compat\":{\"proto\":%d,\"rev\":\"%s\",\"disc\":\"%s\"}",
        name, cands, PC_NET_PROTO_VERSION, rev[0] ? rev : "unknown", pc_net_disc_id());
}

static bool host(void) {
    char common[512], body[1536], lobby_name[64];
    int status = 0;
    PcSlippiJson* r;
    common_fields(common, sizeof common);
    char display[40];
    if (s_host_opts.name[0] != '\0') {
        snprintf(display, sizeof display, "%s", s_host_opts.name);
    } else {
        snprintf(display, sizeof display, "%.12s's lobby", s_args.name);
    }
    pc_slippi_json_escape(display, lobby_name, sizeof lobby_name);
    int seats = s_args.seats < 2 ? 2 : s_args.seats > 4 ? 4 : s_args.seats;
    char fallback[256];
    snprintf(fallback, sizeof fallback,
        "{\"preset\":\"custom\",\"seats\":%d,\"mode\":\"ffa\",\"win\":\"stock\","
        "\"stocks\":4,\"time_min\":8,\"items\":\"off\",\"stage_list\":\"competitive\","
        "\"pick\":\"random\",\"format\":\"endless\",\"first_to\":2,\"place_secs\":15}",
        seats);
    char big[1536];
    snprintf(big, sizeof big, "{\"name\":\"%s\",\"visibility\":\"%s\",\"ask\":false,%s,\"settings\":%s}",
        lobby_name, s_host_opts.open ? "public" : "private", common,
        s_host_opts.settings[0] ? s_host_opts.settings : fallback);
    snprintf(body, sizeof body, "%s", big);
    if (!call_auth("/v1/lobbies", body, &status, &r)) {
        fail("offline", "Couldn't reach XeleeNet");
        return false;
    }
    char id[33] = "", code[8] = "";
    bool got = status == 200 && pc_slippi_json_string(pc_slippi_json_get(r, "id"), id, sizeof id) &&
               pc_slippi_json_string(pc_slippi_json_get(r, "code"), code, sizeof code);
    if (!got) {
        refusal(r, status);
    }
    pc_slippi_json_free(r);
    if (!got) {
        return false;
    }
    SDL_LockMutex(s_lock);
    memcpy(s_lobby, id, sizeof s_lobby);
    memcpy(s_lobby_code, code, sizeof s_lobby_code);
    memset(&s_info, 0, sizeof s_info);
    snprintf(s_info.code, sizeof s_info.code, "%s", code);
    snprintf(s_info.name, sizeof s_info.name, "%s", display);
    snprintf(s_info.host, sizeof s_info.host, "%s", s_args.name);
    {
        PcSlippiJson* st = NULL;
        const char* text = s_host_opts.settings[0] ? s_host_opts.settings : fallback;
        if (pc_slippi_json_parse(text, strlen(text), &st, NULL, 0)) {
            parse_settings(st, &s_info);
            pc_slippi_json_free(st);
        }
    }
    s_port = 0;
    s_status = PC_XN_IN;
    SDL_UnlockMutex(s_lock);
    pc_log_line("xeleenet: hosting lobby %s, code %s, %d seats (build %s, disc %s)", id, code, seats,
        pc_app_rev(), pc_net_disc_id());
    return true;
}

static bool heartbeat(void) {
    char body[256], path[96];
    int status = 0;
    PcSlippiJson* r;
    size_t o = (size_t)snprintf(body, sizeof body, "{\"state\":\"%s\",\"xr\":false,\"roster\":[",
                                SDL_GetAtomicInt(&s_playing) ? "playing" : "open");
    SDL_LockMutex(s_lock);
    for (int i = 0; i < s_nroster; i++) {
        char k[48];
        b64url(s_roster[i], 32, k);
        o += (size_t)snprintf(body + o, sizeof body - o, "%s\"%s\"", i ? "," : "", k);
    }
    o += (size_t)snprintf(body + o, sizeof body - o, "],\"extra\":[");
    for (int q = 1, k = 0; q < 4; q++) {
        if (s_extra & (1u << q)) {
            o += (size_t)snprintf(body + o, sizeof body - o, "%s%d", k++ ? "," : "", q);
        }
    }
    SDL_UnlockMutex(s_lock);
    snprintf(body + o, sizeof body - o, "]}");
    snprintf(path, sizeof path, "/v1/lobbies/%s/heartbeat", s_lobby);
    if (!call_auth(path, body, &status, &r)) {
        return false; /* a lost beat; the lobby lives 30 s without one */
    }
    if (status == 200) {
        set_members(pc_slippi_json_get(r, "members"));
    } else {
        refusal(r, status); /* closed under us, or not ours */
    }
    pc_slippi_json_free(r);
    return status == 200;
}

static bool join(void) {
    char common[512], body[768];
    int status = 0;
    PcSlippiJson* r;
    common_fields(common, sizeof common);
    snprintf(body, sizeof body, "{\"code\":\"%s\",%s}", s_args.code, common);
    if (!call_auth("/v1/join", body, &status, &r)) {
        fail("offline", "Couldn't reach XeleeNet");
        return false;
    }
    for (int tries = 0;; tries++) {
        char st[16] = "", id[33] = "";
        pc_slippi_json_string(pc_slippi_json_get(r, "status"), st, sizeof st);
        pc_slippi_json_string(pc_slippi_json_get(r, "id"), id, sizeof id);
        if (status != 200) {
            refusal(r, status);
            pc_slippi_json_free(r);
            return false;
        }
        if (strcmp(st, "joined") == 0) {
            set_members(pc_slippi_json_get(r, "members"));
            SDL_LockMutex(s_lock);
            memcpy(s_lobby, id, sizeof s_lobby);
            pc_slippi_json_string(pc_slippi_json_get(r, "code"), s_lobby_code, sizeof s_lobby_code);
            s_port = pc_slippi_json_int(pc_slippi_json_get(r, "port"), -1);
            memset(&s_info, 0, sizeof s_info);
            snprintf(s_info.code, sizeof s_info.code, "%s", s_lobby_code);
            str_of(r, "name", s_info.name, sizeof s_info.name);
            parse_settings(pc_slippi_json_get(r, "settings"), &s_info);
            s_status = PC_XN_IN;
            SDL_UnlockMutex(s_lock);
            pc_slippi_json_free(r);
            pc_log_line("xeleenet: joined lobby %s as P%d with %d others (build %s, disc %s)", id,
                s_port + 1, s_nmembers, pc_app_rev(), pc_net_disc_id());
            return true;
        }
        pc_slippi_json_free(r);
        char path[96];
        snprintf(path, sizeof path, "/v1/lobbies/%s/join", id);
        if (strcmp(st, "pending") != 0 || tries >= JOIN_RETRIES || SDL_GetAtomicInt(&s_stop) ||
            !call_auth(path, NULL, &status, &r))
        {
            fail("offline", "Couldn't reach XeleeNet");
            return false;
        }
    }
}

static PcXnStatus svc_status(void);

static void wait_ms(int ms) {
    for (int t = 0; t < ms && !SDL_GetAtomicInt(&s_stop); t += 50) {
        SDL_Delay(50);
    }
}

/* Until the host's next heartbeat. In a game, every HEARTBEAT_PLAYING_MS,
 * unless a member the service lists isn't linked yet (one who joined during
 * the game: the host learns of them from a beat, then has the service's
 * roster grace to reach them). The game starting or ending beats at once. */
static void wait_beat(void) {
    const int playing = SDL_GetAtomicInt(&s_playing);
    SDL_LockMutex(s_lock);
    const bool linking = s_nmembers > s_nroster;
    SDL_UnlockMutex(s_lock);
    const int ms = playing && !linking ? HEARTBEAT_PLAYING_MS : HEARTBEAT_MS;
    for (int t = 0; t < ms && !SDL_GetAtomicInt(&s_stop) && SDL_GetAtomicInt(&s_playing) == playing; t += 50) {
        SDL_Delay(50);
    }
}

static int thread_main(void* unused) {
    (void)unused;
    bool hosting = s_args.code[0] == '\0';
    if (!sign_in() || !(hosting ? host() : join())) {
        return 0;
    }
    while (hosting && !SDL_GetAtomicInt(&s_stop)) {
        wait_beat();
        if (!SDL_GetAtomicInt(&s_stop) && !heartbeat() && svc_status() == PC_XN_FAILED) {
            return 0;
        }
    }
    wait_ms(1 << 30); /* a guest has nothing to poll (only the host does, §3.1) */
    char path[96];
    int status = 0;
    PcSlippiJson* r;
    snprintf(path, sizeof path, "/v1/lobbies/%s/%s", s_lobby, hosting ? "close" : "leave");
    if (call_auth(path, "{}", &status, &r)) {
        pc_slippi_json_free(r);
    }
    return 0;
}

static bool svc_start(const char* url, const PcNetIdentity* id, const char* name, const char* code,
    int seats, const uint32_t* ip, const uint16_t* udp, int ncands) {
    if (s_lock == NULL) {
        s_lock = SDL_CreateMutex();
    }
    if (s_thread != NULL) {
        return false;
    }
    if (s_browse_thread != NULL) {
        SDL_WaitThread(s_browse_thread, NULL); /* it shares the arguments and token */
        s_browse_thread = NULL;
    }
    memset(&s_args, 0, sizeof s_args);
    snprintf(s_args.url, sizeof s_args.url, "%s", url);
    size_t ul = strlen(s_args.url);
    while (ul > 0 && s_args.url[ul - 1] == '/') {
        s_args.url[--ul] = '\0';
    }
    s_args.id = *id;
    /* TODO: 15, with the service's cap (tools/xeleenet/src/common.js playerName). */
    snprintf(s_args.name, sizeof s_args.name, "%.12s", name);
    snprintf(s_args.code, sizeof s_args.code, "%s", code != NULL ? code : "");
    s_args.seats = seats;
    for (int i = 0; i < ncands && i < PC_XN_CANDS; i++) {
        uint32_t h = ntohl(ip[i]);
        snprintf(s_args.cands[i], sizeof s_args.cands[i], "%u.%u.%u.%u:%u", h >> 24, h >> 16 & 255,
            h >> 8 & 255, h & 255, udp[i]);
        s_args.ncands++;
    }
    b64url(id->public_key, 32, s_pubkey);
    s_token[0] = '\0';
    SDL_LockMutex(s_lock);
    s_status = PC_XN_WORKING;
    s_reason[0] = s_error[0] = s_lobby[0] = s_lobby_code[0] = '\0';
    s_port = -1;
    s_nmembers = s_nroster = 0;
    SDL_UnlockMutex(s_lock);
    SDL_SetAtomicInt(&s_stop, 0);
    SDL_SetAtomicInt(&s_playing, 0);
    s_thread = SDL_CreateThread(thread_main, "xeleenet", NULL);
    if (s_thread == NULL) {
        fail("thread", "Couldn't start the XeleeNet thread");
        return false;
    }
    return true;
}

static PcXnStatus svc_status(void) {
    if (s_lock == NULL) {
        return PC_XN_OFF;
    }
    SDL_LockMutex(s_lock);
    PcXnStatus s = s_status;
    SDL_UnlockMutex(s_lock);
    return s;
}

static const char* svc_error(const char** reason) {
    if (reason != NULL) {
        *reason = s_reason;
    }
    return s_error;
}

static bool svc_lobby(char id[33], char code[8], int* port) {
    if (svc_status() != PC_XN_IN) {
        return false;
    }
    SDL_LockMutex(s_lock);
    memcpy(id, s_lobby, 33);
    memcpy(code, s_lobby_code, 8);
    *port = s_port;
    SDL_UnlockMutex(s_lock);
    return true;
}

static int svc_members(PcXnMember* out, int max) {
    if (s_lock == NULL) {
        return 0;
    }
    SDL_LockMutex(s_lock);
    int n = s_nmembers < max ? s_nmembers : max;
    memcpy(out, s_members, (size_t)n * sizeof *out);
    SDL_UnlockMutex(s_lock);
    return n;
}

static void svc_set_roster(const uint8_t (*keys)[32], int n, uint8_t extra) {
    if (s_lock == NULL) {
        return;
    }
    SDL_LockMutex(s_lock);
    s_extra = (uint8_t)(extra & 14u);
    s_nroster = n < 3 ? n : 3;
    memcpy(s_roster, keys, (size_t)s_nroster * 32);
    SDL_UnlockMutex(s_lock);
}

static void svc_stop(void) {
    if (s_thread == NULL) {
        return;
    }
    SDL_SetAtomicInt(&s_stop, 1);
    SDL_WaitThread(s_thread, NULL);
    s_thread = NULL;
    SDL_LockMutex(s_lock);
    s_status = PC_XN_OFF;
    SDL_UnlockMutex(s_lock);
}

static void svc_host_options(const char* name, bool open, const char* settings_json) {
    snprintf(s_host_opts.name, sizeof s_host_opts.name, "%s", name != NULL ? name : "");
    s_host_opts.open = open;
    snprintf(s_host_opts.settings, sizeof s_host_opts.settings, "%s", settings_json != NULL ? settings_json : "");
}

static bool svc_lobby_info(PcXnListing* out) {
    if (s_lock == NULL || svc_status() != PC_XN_IN) {
        return false;
    }
    SDL_LockMutex(s_lock);
    *out = s_info;
    SDL_UnlockMutex(s_lock);
    return true;
}

static int browse_main(void* unused) {
    (void)unused;
    char path[160], rev[48];
    int status = 0;
    PcSlippiJson* r;
    size_t n = 0;
    for (const char* p = pc_app_rev(); *p && n + 1 < sizeof rev; p++) {
        char c = *p;
        rev[n++] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                           c == '_' || c == '+' || c == '-' ?
                       c :
                       '-';
    }
    rev[n] = '\0';
    snprintf(path, sizeof path, "/v1/lobbies?proto=%d&rev=%s&disc=%s%s", PC_NET_PROTO_VERSION,
        rev[0] ? rev : "unknown", pc_net_disc_id(), s_browse_full ? "&full=1" : "");
    bool ok = (s_token[0] != '\0' || sign_in()) && call_auth(path, NULL, &status, &r);
    if (ok && status == 200) {
        const PcSlippiJson* list = pc_slippi_json_get(r, "lobbies");
        SDL_LockMutex(s_lock);
        s_nrows = 0;
        for (int i = 0; list != NULL && i < list->child_count && s_nrows < PC_XN_LIST_MAX; i++) {
            parse_listing(pc_slippi_json_at(list, i), &s_rows[s_nrows++]);
        }
        s_others = pc_slippi_json_int(pc_slippi_json_get(r, "other_versions"), 0);
        s_browse_state = 1;
        SDL_UnlockMutex(s_lock);
    } else {
        if (ok) {
            refusal(r, status);
        }
        SDL_LockMutex(s_lock);
        s_browse_state = -1;
        SDL_UnlockMutex(s_lock);
    }
    if (ok) {
        pc_slippi_json_free(r);
    }
    return 0;
}

static bool svc_browse(const char* url, const PcNetIdentity* id, bool full) {
    if (s_lock == NULL) {
        s_lock = SDL_CreateMutex();
    }
    if (s_thread != NULL) {
        return false; /* a run owns the arguments */
    }
    if (s_browse_thread != NULL) {
        SDL_WaitThread(s_browse_thread, NULL);
        s_browse_thread = NULL;
    }
    if (strncmp(s_args.url, url, sizeof s_args.url) != 0 ||
        memcmp(s_args.id.public_key, id->public_key, 32) != 0)
    {
        memset(&s_args, 0, sizeof s_args);
        snprintf(s_args.url, sizeof s_args.url, "%s", url);
        size_t ul = strlen(s_args.url);
        while (ul > 0 && s_args.url[ul - 1] == '/') {
            s_args.url[--ul] = '\0';
        }
        s_args.id = *id;
        b64url(id->public_key, 32, s_pubkey);
        s_token[0] = '\0';
    }
    s_browse_full = full;
    SDL_LockMutex(s_lock);
    s_browse_state = 0;
    SDL_UnlockMutex(s_lock);
    s_browse_thread = SDL_CreateThread(browse_main, "xeleenet browse", NULL);
    return s_browse_thread != NULL;
}

static int svc_browse_poll(PcXnListing* out, int max, int* n, int* others) {
    if (s_lock == NULL) {
        return -1;
    }
    SDL_LockMutex(s_lock);
    int st = s_browse_state;
    if (st == 1) {
        *n = s_nrows < max ? s_nrows : max;
        memcpy(out, s_rows, (size_t)*n * sizeof *out);
        *others = s_others;
    }
    SDL_UnlockMutex(s_lock);
    return st;
}

/* ---- the network: the service above, or this network (xeleenet_lan.c) ---- */

const PcXnBackend pc_xn_service_backend = {
    svc_start, svc_status, svc_error, svc_lobby, svc_members, svc_set_roster, svc_stop,
    svc_host_options, svc_lobby_info, svc_browse, svc_browse_poll,
};

static PcXnNet s_net = PC_XN_NET_SERVICE;

static const PcXnBackend* backend(void) {
    return s_net == PC_XN_NET_LAN ? &pc_xn_lan_backend : &pc_xn_service_backend;
}

bool pc_xn_set_network(PcXnNet net) {
    if (net == s_net) {
        return true;
    }
    PcXnStatus st = backend()->status();
    if (st == PC_XN_WORKING || st == PC_XN_IN) {
        return false; /* a run on the other network owns the client */
    }
    backend()->stop();
    s_net = net;
    /* XeleeNet's pages and menus read LAN's lines where they differ. */
    pc_lang_overlay("xeleenet.", net == PC_XN_NET_LAN ? "xeleenet.lan." : NULL);
    pc_log_line("xeleenet: network %s", net == PC_XN_NET_LAN ? "LAN" : "service");
    return true;
}

PcXnNet pc_xn_network(void) {
    return s_net;
}

bool pc_xn_start(const char* url, const PcNetIdentity* id, const char* name, const char* code,
    int seats, const uint32_t* ip, const uint16_t* udp, int ncands) {
    return backend()->start(url, id, name, code, seats, ip, udp, ncands);
}

PcXnStatus pc_xn_status(void) {
    return backend()->status();
}

const char* pc_xn_error(const char** reason) {
    return backend()->error(reason);
}

bool pc_xn_lobby(char id[33], char code[8], int* port) {
    return backend()->lobby(id, code, port);
}

int pc_xn_members(PcXnMember* out, int max) {
    return backend()->members(out, max);
}

void pc_xn_set_roster(const uint8_t (*keys)[32], int n, uint8_t extra) {
    backend()->set_roster(keys, n, extra);
}

void pc_xn_stop(void) {
    backend()->stop();
}

void pc_xn_set_playing(bool playing) {
    SDL_SetAtomicInt(&s_playing, playing ? 1 : 0);
}

void pc_xn_exit(void) {
    if (s_net == PC_XN_NET_LAN) {
        backend()->stop();
    }
}

void pc_xn_host_options(const char* name, bool open, const char* settings_json) {
    backend()->host_options(name, open, settings_json);
}

bool pc_xn_lobby_info(PcXnListing* out) {
    return backend()->lobby_info(out);
}

bool pc_xn_browse(const char* url, const PcNetIdentity* id, bool full) {
    return backend()->browse(url, id, full);
}

int pc_xn_browse_poll(PcXnListing* out, int max, int* n, int* others) {
    return backend()->browse_poll(out, max, n, others);
}
