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

/* Host (`code` NULL, waiting for `seats` players) or join by code. */
bool pc_xn_punch_start(const char* url, const char* name, const char* code, int seats);
/* Drive it (game thread, often): 0 still going, 1 the session is up, -1 it
 * failed (pc_xn_punch_error). */
int pc_xn_punch_poll(void);
const char* pc_xn_punch_error(void);
void pc_xn_punch_cancel(void);
/* MELEE_XN=host or join:CODE (with MELEE_XN_URL, MELEE_XN_NAME, MELEE_XN_SEATS):
 * the connect phase at boot, blocking until the session is up, as
 * MELEE_NET_PEERS is up from boot. False when MELEE_XN is unset. */
bool pc_xn_punch_from_env(void);

#ifdef __cplusplus
}
#endif
#endif
