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
    double joined; /* when they took the seat (service ms): a rejoin is a new stay */
} PcXnMember;

/* A lobby as the browser lists it (and as joining learns it): the
 * service's summary (tools/xeleenet/src/common.js settings()). */
typedef struct PcXnListing {
    char code[8];
    char name[24];
    char host[16];
    char tag[12];
    char country[4];
    int taken, seats;
    char preset[10];
    bool teams;
    bool stock;
    int stocks, time_min;
    char items[12];
    char stage_list[12];
    bool first_to; /* else endless */
    int first_to_n;
    bool playing, xr;
} PcXnListing;
/* The most lobbies one listing holds (the service sends 50 at most). */
#define PC_XN_LIST_MAX 50

typedef enum PcXnStatus {
    PC_XN_OFF,     /* nothing started, or closed */
    PC_XN_WORKING, /* signing in, opening the lobby, joining */
    PC_XN_IN,      /* hosting, or seated in a lobby */
    PC_XN_FAILED,  /* pc_xn_error says why */
} PcXnStatus;

/* Which network the lobbies are on: the XeleeNet service, or this local
 * network, where the host's game is the lobby service (xeleenet_lan.c,
 * docs/lan-lobbies-plan.md). Every call below goes to the one set. Refused
 * (false) while a run on the other is working or in. */
typedef enum PcXnNet {
    PC_XN_NET_SERVICE,
    PC_XN_NET_LAN,
} PcXnNet;
bool pc_xn_set_network(PcXnNet net);
PcXnNet pc_xn_network(void);
/* LAN: nothing at all heard on mDNS for a while, not even our own query:
 * this network blocks discovery, and joining is by address. */
bool pc_xn_lan_unavailable(void);
/* LAN: the lobby's address on this network ("192.168.0.57", with ":port" off
 * the usual one), host or guest, once in. */
bool pc_xn_lan_address(char* out, size_t size);

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
/* A game is running (VS to the results) or not: the host's heartbeat says so
 * (the browser's "in game") and slows down meanwhile. LAN has no heartbeat. */
void pc_xn_set_playing(bool playing);
/* The app is closing: a LAN lobby says goodbye (the host's announcement, a
 * guest's LEAVE) at once. The service's lobby is left to its own timeouts, as
 * a lost connection would be. */
void pc_xn_exit(void);

/* What the next host run opens: its name, visibility and the settings
 * summary the browser shows (JSON, the service's settings object). */
void pc_xn_host_options(const char* name, bool open, const char* settings_json);
/* The lobby we're in, as far as the service told us (name and summary). */
bool pc_xn_lobby_info(PcXnListing* out);

/* Browse: the public lobbies on our build, fetched on a thread of its own
 * (`full` includes the full ones). Poll: 0 still fetching, 1 done (rows in
 * `out`, the count in *n, lobbies on other builds in *others), -1 failed
 * (pc_xn_error). Joining waits for a fetch still running. */
bool pc_xn_browse(const char* url, const PcNetIdentity* id, bool full);
int pc_xn_browse_poll(PcXnListing* out, int max, int* n, int* others);

#ifdef __cplusplus
}
#endif
#endif
