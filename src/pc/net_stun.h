/* SPDX-License-Identifier: GPL-3.0-or-later */
/* STUN (RFC 5389) binding requests, just enough for XeleeNet's hole punch
 * (docs/xeleenet-plan.md, XN2): ask a public server what our socket's address
 * looks like from outside. The punch sends and receives on its own socket, so
 * this is only the codec; it never touches a socket itself. */
#ifndef PC_NET_STUN_H
#define PC_NET_STUN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PC_STUN_REQUEST_BYTES 20

/* A binding request with this transaction id. */
void pc_stun_request(uint8_t out[PC_STUN_REQUEST_BYTES], const uint8_t txid[12]);
/* True when the datagram has a STUN header (the magic cookie): a socket that
 * also carries other traffic routes it here. */
bool pc_stun_is(const void* buf, size_t n);
/* A success response to `txid`: the mapped IPv4 address (network order) and
 * port. False for anything else, IPv6 mappings included. */
bool pc_stun_parse(const void* buf, size_t n, const uint8_t txid[12], uint32_t* ip, uint16_t* port);

/* Our IPv4 address on the route to the internet (network order): the LAN
 * address a player behind the same NAT reaches us at. False offline. */
bool pc_stun_local_ipv4(uint32_t* ip);

#ifdef __cplusplus
}
#endif
#endif
