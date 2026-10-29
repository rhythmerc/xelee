/* SPDX-License-Identifier: GPL-3.0-or-later */
/* One DNS-SD service type on the local network over mDNS, IPv4 and IPv6
 * (src/pc/net_mdns.c): our own record, announced every second, and the
 * others', as heard. XeleeNet's LAN lobbies announce and browse with it
 * (xeleenet_lan.c, docs/lan-lobbies-plan.md).
 *
 * Single-threaded: open, poll, announce, read and close from one thread. */
#ifndef PC_NET_MDNS_H
#define PC_NET_MDNS_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_MDNS_TXT_MAX 16
#define PC_MDNS_RECORDS 16

/* Another instance's record: its TXT pairs whole (values up to 255 bytes,
 * printable ASCII), from where it came (IPv4 preferred). */
typedef struct PcMdnsRecord {
    char instance[64]; /* the instance label, without the service */
    char ip[46];
    uint16_t port; /* its SRV port */
    int ntxt;
    char key[PC_MDNS_TXT_MAX][16];
    char value[PC_MDNS_TXT_MAX][256];
} PcMdnsRecord;

/* `service` like "_xeleenet._udp.local.". False when no socket opened at
 * all (pc_mdns_unavailable says so too). */
bool pc_mdns_open(const char* service);
/* Says goodbye to our record, if any, and closes. */
void pc_mdns_close(void);
/* Our record from now on (`instance` NULL: none, with a goodbye for the
 * last). Announced at once and every second after. */
void pc_mdns_announce(const char* instance, uint16_t port, const char* const* keys, const char* const* values, int n);
/* Receive, expire records silent for 5 s, re-announce. Often. */
void pc_mdns_poll(void);
/* The records heard, ours left out. */
int pc_mdns_records(PcMdnsRecord* out, int max);
/* The value of `key` in a record, or NULL. */
const char* pc_mdns_txt(const PcMdnsRecord* r, const char* key);
/* Nothing at all heard for 5 s since opening, not even our own query or
 * announce looped back: multicast is blocked here. */
bool pc_mdns_unavailable(void);
/* Our address on the network the group is on (dotted IPv4, else IPv6). */
const char* pc_mdns_self_ip(void);

#ifdef __cplusplus
}
#endif
#endif
