/* SPDX-License-Identifier: GPL-3.0-or-later */
/* net_stun.c's codec against RFC 5769's sample IPv4 response, plus the
 * ways a reply must be refused. */
#include "../src/pc/net_stun.c"
#include <assert.h>
#include <stdio.h>

static const uint8_t TXID[12] = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};

/* RFC 5769 section 2.2: 192.0.2.1:32853, behind SOFTWARE, before
 * MESSAGE-INTEGRITY and FINGERPRINT. */
static const uint8_t SAMPLE[] = {
    0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86,
    0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63,
    0x74, 0x6f, 0x72, 0x20, 0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43,
    0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3, 0x8c, 0x74, 0x89, 0xf9,
    0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7, 0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96,
};

int main(void) {
    uint8_t req[PC_STUN_REQUEST_BYTES];
    pc_stun_request(req, TXID);
    assert(req[0] == 0 && req[1] == 1 && req[2] == 0 && req[3] == 0);
    assert(pc_stun_is(req, sizeof req));
    assert(memcmp(req + 8, TXID, 12) == 0);

    uint32_t ip = 0;
    uint16_t port = 0;
    assert(pc_stun_is(SAMPLE, sizeof SAMPLE));
    assert(pc_stun_parse(SAMPLE, sizeof SAMPLE, TXID, &ip, &port));
    assert(ntohl(ip) == 0xc0000201u && port == 32853);

    uint8_t other[12];
    memcpy(other, TXID, 12);
    other[11] ^= 1;
    assert(!pc_stun_parse(SAMPLE, sizeof SAMPLE, other, &ip, &port)); /* not our request */
    assert(!pc_stun_parse(SAMPLE, sizeof SAMPLE - 8, TXID, &ip, &port)); /* cut short */
    assert(!pc_stun_parse(req, sizeof req, TXID, &ip, &port));             /* a request */
    uint8_t bad[sizeof SAMPLE];
    memcpy(bad, SAMPLE, sizeof bad);
    bad[23] = 0x40; /* SOFTWARE claims to run past the message */
    assert(!pc_stun_parse(bad, sizeof bad, TXID, &ip, &port));
    const uint8_t engine[] = {'M', 12, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    assert(!pc_stun_is(engine, sizeof engine)); /* an engine datagram is never STUN */

    puts("net_stun: ok");
    return 0;
}
