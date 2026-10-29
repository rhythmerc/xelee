/* SPDX-License-Identifier: GPL-3.0-or-later */
/* XeleeNet on this network (docs/lan-lobbies-plan.md): the lobby service's
 * calls (xeleenet_backend.h) with the host's game for the service. It
 * announces its lobby over mDNS and answers joins on its punch socket; a
 * guest browses the announcements and joins the host directly.
 *
 * L1 is the seam only: nothing is announced yet, the list is empty and a
 * start is refused. L2 fills it in. */
#include "xeleenet_backend.h"

#include <stdio.h>
#include <string.h>

extern void pc_log_line(const char* fmt, ...);

static PcXnStatus s_status = PC_XN_OFF;
static char s_reason[24], s_error[160];

static bool lan_start(const char* url, const PcNetIdentity* id, const char* name, const char* code, int seats,
    const uint32_t* ip, const uint16_t* udp, int ncands) {
    (void)url, (void)id, (void)name, (void)code, (void)seats, (void)ip, (void)udp, (void)ncands;
    snprintf(s_reason, sizeof s_reason, "unavailable");
    snprintf(s_error, sizeof s_error, "LAN lobbies aren't ready yet");
    s_status = PC_XN_FAILED;
    pc_log_line("xeleenet: LAN: %s", s_error);
    return true; /* started, and failed: the punch reports it as a refusal */
}

static PcXnStatus lan_status(void) {
    return s_status;
}

static const char* lan_error(const char** reason) {
    if (reason != NULL) {
        *reason = s_reason;
    }
    return s_error;
}

static bool lan_lobby(char id[33], char code[8], int* port) {
    (void)id, (void)code, (void)port;
    return false;
}

static int lan_members(PcXnMember* out, int max) {
    (void)out, (void)max;
    return 0;
}

static void lan_set_roster(const uint8_t (*keys)[32], int n) {
    (void)keys, (void)n;
}

static void lan_stop(void) {
    s_status = PC_XN_OFF;
    s_reason[0] = s_error[0] = '\0';
}

static void lan_host_options(const char* name, bool open, const char* settings_json) {
    (void)name, (void)open, (void)settings_json;
}

static bool lan_lobby_info(PcXnListing* out) {
    (void)out;
    return false;
}

static bool lan_browse(const char* url, const PcNetIdentity* id, bool full) {
    (void)url, (void)id, (void)full;
    return true;
}

static int lan_browse_poll(PcXnListing* out, int max, int* n, int* others) {
    (void)out, (void)max;
    *n = *others = 0;
    return 1; /* nothing announced yet */
}

const PcXnBackend pc_xn_lan_backend = {
    lan_start, lan_status, lan_error, lan_lobby, lan_members, lan_set_roster, lan_stop,
    lan_host_options, lan_lobby_info, lan_browse, lan_browse_poll,
};
