/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net_stun.h"
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define COOKIE 0x2112A442u
#define BINDING_REQUEST 0x0001
#define BINDING_SUCCESS 0x0101
#define ATTR_MAPPED 0x0001
#define ATTR_XOR_MAPPED 0x0020

static uint16_t get16(const uint8_t* p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void pc_stun_request(uint8_t out[PC_STUN_REQUEST_BYTES], const uint8_t txid[12]) {
    out[0] = 0;
    out[1] = BINDING_REQUEST;
    out[2] = out[3] = 0; /* no attributes */
    out[4] = (uint8_t)(COOKIE >> 24);
    out[5] = (uint8_t)(COOKIE >> 16);
    out[6] = (uint8_t)(COOKIE >> 8);
    out[7] = (uint8_t)COOKIE;
    memcpy(out + 8, txid, 12);
}

bool pc_stun_is(const void* buf, size_t n) {
    const uint8_t* b = buf;
    return n >= 20 && (b[0] & 0xC0) == 0 && get32(b + 4) == COOKIE;
}

bool pc_stun_parse(const void* buf, size_t n, const uint8_t txid[12], uint32_t* ip, uint16_t* port) {
    const uint8_t* b = buf;
    if (!pc_stun_is(buf, n) || get16(b) != BINDING_SUCCESS || memcmp(b + 8, txid, 12) != 0) {
        return false;
    }
    size_t len = get16(b + 2);
    if (len + 20 > n) {
        return false;
    }
    bool found = false;
    for (size_t at = 20; at + 4 <= 20 + len;) {
        uint16_t type = get16(b + at), alen = get16(b + at + 2);
        const uint8_t* v = b + at + 4;
        if (at + 4 + alen > 20 + len) {
            return false;
        }
        /* family 1 is IPv4: 1 reserved byte, family, port, 4 address bytes */
        if ((type == ATTR_XOR_MAPPED || (type == ATTR_MAPPED && !found)) && alen >= 8 && v[1] == 1) {
            uint16_t p = get16(v + 2);
            uint32_t a = get32(v + 4);
            if (type == ATTR_XOR_MAPPED) {
                p ^= (uint16_t)(COOKIE >> 16);
                a ^= COOKIE;
            }
            *port = p;
            *ip = htonl(a);
            found = true;
            if (type == ATTR_XOR_MAPPED) {
                break; /* the one a NAT rewriting payloads cannot spoil */
            }
        }
        at += 4 + (size_t)((alen + 3) & ~3);
    }
    return found;
}

bool pc_stun_local_ipv4(uint32_t* ip) {
    /* The source address of a connected UDP socket that never sends: the
     * kernel picks it from the route, nothing goes on the wire. */
    struct sockaddr_in to, self;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808u); /* any public address will do */
    socklen_t len = sizeof self;
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    bool ok = s != INVALID_SOCKET && connect(s, (const struct sockaddr*)&to, sizeof to) == 0 &&
              getsockname(s, (struct sockaddr*)&self, &len) == 0;
    if (s != INVALID_SOCKET) {
        closesocket(s);
    }
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    bool ok = s >= 0 && connect(s, (const struct sockaddr*)&to, sizeof to) == 0 &&
              getsockname(s, (struct sockaddr*)&self, &len) == 0;
    if (s >= 0) {
        close(s);
    }
#endif
    if (ok && self.sin_addr.s_addr != 0) {
        *ip = self.sin_addr.s_addr;
        return true;
    }
    return false;
}
