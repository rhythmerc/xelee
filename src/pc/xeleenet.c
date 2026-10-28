/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xeleenet.h"
#include "net.h"
#include "net_lan.h"
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

#define HEARTBEAT_MS 2000
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
static PcXnStatus s_status;
static char s_reason[24], s_error[160];
static char s_lobby[33], s_lobby_code[8];
static int s_port;
static PcXnMember s_members[3];
static int s_nmembers;
static uint8_t s_roster[3][32];
static int s_nroster;

static char s_token[256]; /* the thread's own */
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
        name, cands, PC_NET_PROTO_VERSION, rev[0] ? rev : "unknown", pc_lan_disc_id());
}

static bool host(void) {
    char common[512], body[1024], lobby_name[40];
    int status = 0;
    PcSlippiJson* r;
    common_fields(common, sizeof common);
    char display[24];
    snprintf(display, sizeof display, "%.12s's lobby", s_args.name);
    pc_slippi_json_escape(display, lobby_name, sizeof lobby_name);
    int seats = s_args.seats < 2 ? 2 : s_args.seats > 4 ? 4 : s_args.seats;
    snprintf(body, sizeof body,
        "{\"name\":\"%s\",\"visibility\":\"private\",\"ask\":false,%s,"
        "\"settings\":{\"preset\":\"custom\",\"seats\":%d,\"mode\":\"ffa\",\"win\":\"stock\","
        "\"stocks\":4,\"time_min\":8,\"items\":\"off\",\"stage_list\":\"competitive\","
        "\"pick\":\"random\",\"format\":\"endless\",\"first_to\":2,\"place_secs\":15}}",
        lobby_name, common, seats);
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
    s_port = 0;
    s_status = PC_XN_IN;
    SDL_UnlockMutex(s_lock);
    pc_log_line("xeleenet: hosting lobby %s, code %s, %d seats", id, code, seats);
    return true;
}

static bool heartbeat(void) {
    char body[256], path[96];
    int status = 0;
    PcSlippiJson* r;
    size_t o = (size_t)snprintf(body, sizeof body, "{\"state\":\"open\",\"xr\":false,\"roster\":[");
    SDL_LockMutex(s_lock);
    for (int i = 0; i < s_nroster; i++) {
        char k[48];
        b64url(s_roster[i], 32, k);
        o += (size_t)snprintf(body + o, sizeof body - o, "%s\"%s\"", i ? "," : "", k);
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
            s_status = PC_XN_IN;
            SDL_UnlockMutex(s_lock);
            pc_slippi_json_free(r);
            pc_log_line("xeleenet: joined lobby %s as P%d with %d others", id, s_port + 1, s_nmembers);
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

static void wait_ms(int ms) {
    for (int t = 0; t < ms && !SDL_GetAtomicInt(&s_stop); t += 50) {
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
        wait_ms(HEARTBEAT_MS);
        if (!SDL_GetAtomicInt(&s_stop) && !heartbeat() && pc_xn_status() == PC_XN_FAILED) {
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

bool pc_xn_start(const char* url, const PcNetIdentity* id, const char* name, const char* code,
    int seats, const uint32_t* ip, const uint16_t* udp, int ncands) {
    if (s_lock == NULL) {
        s_lock = SDL_CreateMutex();
    }
    if (s_thread != NULL) {
        return false;
    }
    memset(&s_args, 0, sizeof s_args);
    snprintf(s_args.url, sizeof s_args.url, "%s", url);
    size_t ul = strlen(s_args.url);
    while (ul > 0 && s_args.url[ul - 1] == '/') {
        s_args.url[--ul] = '\0';
    }
    s_args.id = *id;
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
    s_thread = SDL_CreateThread(thread_main, "xeleenet", NULL);
    if (s_thread == NULL) {
        fail("thread", "Couldn't start the XeleeNet thread");
        return false;
    }
    return true;
}

PcXnStatus pc_xn_status(void) {
    if (s_lock == NULL) {
        return PC_XN_OFF;
    }
    SDL_LockMutex(s_lock);
    PcXnStatus s = s_status;
    SDL_UnlockMutex(s_lock);
    return s;
}

const char* pc_xn_error(const char** reason) {
    if (reason != NULL) {
        *reason = s_reason;
    }
    return s_error;
}

bool pc_xn_lobby(char id[33], char code[8], int* port) {
    if (pc_xn_status() != PC_XN_IN) {
        return false;
    }
    SDL_LockMutex(s_lock);
    memcpy(id, s_lobby, 33);
    memcpy(code, s_lobby_code, 8);
    *port = s_port;
    SDL_UnlockMutex(s_lock);
    return true;
}

int pc_xn_members(PcXnMember* out, int max) {
    if (s_lock == NULL) {
        return 0;
    }
    SDL_LockMutex(s_lock);
    int n = s_nmembers < max ? s_nmembers : max;
    memcpy(out, s_members, (size_t)n * sizeof *out);
    SDL_UnlockMutex(s_lock);
    return n;
}

void pc_xn_set_roster(const uint8_t (*keys)[32], int n) {
    if (s_lock == NULL) {
        return;
    }
    SDL_LockMutex(s_lock);
    s_nroster = n < 3 ? n : 3;
    memcpy(s_roster, keys, (size_t)s_nroster * 32);
    SDL_UnlockMutex(s_lock);
}

void pc_xn_stop(void) {
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
