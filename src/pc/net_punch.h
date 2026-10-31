/* SPDX-License-Identifier: GPL-3.0-or-later */
/* XeleeNet's connect phase (docs/xeleenet-plan.md section 3.3, XN2): from a
 * lobby code to a running N-player session.
 *
 * One UDP socket does all of it, and then becomes the session's:
 *   1. STUN tells us our public address; with our LAN address those are the
 *      candidates the lobby service passes to the others (xeleenet.c).
 *   2. Everyone probes everyone else's candidates with signed probes until a
 *      probe comes back echoing ours: that link is punched, both ways. The
 *      host introduces each guest to the guests who joined after it (only the
 *      host polls the service).
 *   3. Every guest must reach the host. Two guests that have not punched
 *      through in PAIR_WAIT_MS go through the host instead (net.c, routes).
 *   4. With the seats filled and every pair decided, the host sends the start
 *      (seed, routes, every player's nonce) and everyone hands the socket to
 *      the engine with a key per link. */
#ifndef PC_NET_PUNCH_H
#define PC_NET_PUNCH_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The service to use: MELEE_XN_URL, else the one the build names
 * (XELEENET_URL at configure time), else "". */
const char* pc_xn_url(void);
/* Host (`code` NULL, for up to `seats` players) or join by code. */
bool pc_xn_punch_start(const char* url, const char* name, const char* code, int seats);
/* Drive it (game thread, often), session or not: the lobby goes on while
 * one runs. 1 while a session is up, 0 before, -1 it failed
 * (pc_xn_punch_error). */
int pc_xn_punch_poll(void);
const char* pc_xn_punch_error(void);
void pc_xn_punch_cancel(void);
/* The host starts the session now, without waiting for the seats to fill:
 * as soon as someone has joined and every link is decided. */
void pc_xn_punch_request_start(void);
/* Sessions started this run. A new one is a roster change: everyone runs the
 * handshake again and reloads the CSS (gmonlinemode.c). */
int pc_xn_punch_session_seq(void);
/* Leave the lobby (a host closes it) and forget this run, session or not.
 * The session itself is pc_net_disconnect's. */
void pc_xn_punch_end(void);
/* A guest left by holding B (every player still in the lobby calls this on
 * the frame they see it): forget them, and the host starts the next session
 * without them. True when that leaves the host alone: the session has ended
 * and the lobby waits for the next to join, as it did when it opened. */
bool pc_xn_punch_left(int port);
/* A spare pad of ours (physical pad `phys`, 1-3) asked for a seat (START on
 * the CSS): the host gives it the lowest free port at the next session, as
 * for a joiner, and that pad plays it (docs/lan-multipad-plan.md). Up to
 * two. A claim the host doesn't fill within a few seconds is dropped. */
void pc_xn_punch_want_seat(int phys);
/* Our pads waiting for a seat, oldest first: their count, and them in phys. */
int pc_xn_punch_claims(int* phys);
/* True once after claims were dropped for want of a seat. */
bool pc_xn_punch_claim_refused(void);
/* Another seat of a machine's (its other pad) gave up its seat: every player
 * calls this on the frame they see it, as pc_xn_punch_left. False for a
 * machine's own seat. */
bool pc_xn_punch_seat_left(int port);
/* Host: the lobby's seats now, which caps the seats other pads get. */
void pc_xn_punch_set_seats(int seats);

/* What the waiting room shows (gmonlinemode.c). */
enum {
    PC_XN_PUNCH_OFF,
    PC_XN_PUNCH_SERVICE,  /* finding our address, signing in, opening or joining */
    PC_XN_PUNCH_LOBBY,    /* in: players come and links go up */
    PC_XN_PUNCH_STARTING, /* the host sent the start */
    PC_XN_PUNCH_UP,       /* the session is running */
    PC_XN_PUNCH_FAILED,   /* `error` says why */
};
typedef struct PcXnPunchView {
    int phase;
    bool host;
    char code[8]; /* the lobby's, once in */
    int nplayers; /* us included, port order */
    struct {
        int port;
        char name[16];
        bool local;
        bool linked;   /* punched (us: always) */
        bool via_host; /* a guest we gave up reaching directly */
        int owner;     /* the machine that plays it, by primary port: its own, or
                        * another seat's machine (docs/lan-multipad-plan.md) */
    } players[4];
    int want_x; /* seats we asked for our other pads */
    bool can_start; /* host: someone joined and every link is decided */
    const char* error;
} PcXnPunchView;
void pc_xn_punch_view(PcXnPunchView* out);

/* The player's XeleeNet name (docs/xeleenet-plan.md ID-2): its own, never
 * Slippi's, kept in xeleenet-name.txt in the profile. "" until chosen. */
const char* pc_xn_name(void);
void pc_xn_set_name(const char* name);
/* The first characters of this install's key tag, for "NAME#TAG": two
 * players with the same name are told apart by it. "" without a key. */
const char* pc_xn_tag(void);
/* A file in the profile folder (MELEE_XN_IDENTITY's, for tests): `out`, or
 * NULL without one. */
char* pc_xn_profile_file(const char* leaf, char* out, size_t cap);
/* This install's key, for the service calls made outside a run (Browse). */
struct PcNetIdentity;
bool pc_xn_identity(struct PcNetIdentity* out);
/* MELEE_XN=host or join:CODE (with MELEE_XN_URL, MELEE_XN_NAME, MELEE_XN_SEATS):
 * the connect phase at boot, blocking until the session is up, as
 * MELEE_NET_PEERS is up from boot. False when MELEE_XN is unset. */
bool pc_xn_punch_from_env(void);

#ifdef __cplusplus
}
#endif
#endif
