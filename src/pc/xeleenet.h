/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The game's client for the XeleeNet lobby service (tools/xeleenet,
 * docs/xeleenet-plan.md section 3.1): sign in with the install's key, host or
 * join a lobby, and -- for the host -- keep it alive with a heartbeat that
 * brings back who joined and where they can be reached.
 *
 * Every call to the service runs on one thread of its own; the game reads its
 * results with pc_xn_status and pc_xn_members whenever it likes. */
#ifndef PC_XELEENET_H
#define PC_XELEENET_H
#include "net_identity.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PC_XN_CANDS 4

/* Somebody else in the lobby. */
typedef struct PcXnMember {
    uint8_t key[32];
    char name[16];
    char tag[12];
    int port; /* their controller port: 0 the host, 1-3 guests in join order */
    int ncands;
    uint32_t ip[PC_XN_CANDS]; /* network order */
    uint16_t udp[PC_XN_CANDS];
} PcXnMember;

typedef enum PcXnStatus {
    PC_XN_OFF,     /* nothing started, or closed */
    PC_XN_WORKING, /* signing in, opening the lobby, joining */
    PC_XN_IN,      /* hosting, or seated in a lobby */
    PC_XN_FAILED,  /* pc_xn_error says why */
} PcXnStatus;

/* Open a lobby (`code` NULL) or join one by its code, reachable at the given
 * addresses (network order). `url` is the service; `seats` only matters to a
 * host. False when a run is already going. */
bool pc_xn_start(const char* url, const PcNetIdentity* id, const char* name, const char* code,
    int seats, const uint32_t* ip, const uint16_t* udp, int ncands);
PcXnStatus pc_xn_status(void);
/* The service's reason word ("full", "not_found", ...) and its line to show. */
const char* pc_xn_error(const char** reason);
/* Once in: the lobby's id (32 hex digits), its code and our port. */
bool pc_xn_lobby(char id[33], char code[8], int* port);
/* Everyone else in the lobby, as last heard: a guest knows those seated before
 * it, the host everyone (refreshed every heartbeat). */
int pc_xn_members(PcXnMember* out, int max);
/* Host: the guests its game is linked to, sent with every heartbeat; the
 * service lets go of a member left out of it for a while. */
void pc_xn_set_roster(const uint8_t (*keys)[32], int n);
/* Leave the lobby (a host closes it) and stop the thread. */
void pc_xn_stop(void);

#ifdef __cplusplus
}
#endif
#endif
