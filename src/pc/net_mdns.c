/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mDNS/DNS-SD for one service type (net_mdns.h): our record announced as an
 * unsolicited multicast answer every second (PTR + SRV + TXT + A/AAAA) and
 * on every PTR question for the service, and a PTR query every two seconds;
 * other instances' TXT records kept by instance name until silent for 5 s
 * or a goodbye (ttl 0). The socket, interface and address choices are the
 * LAN lobby's (net_lan.c), which this replaces (docs/lan-lobbies-plan.md
 * L4). */
#include "pc/net_mdns.h"
#include "pc/android_hooks.h"

#include <SDL3/SDL_timer.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#define sock_close close
#endif
#include "mdns/mdns.h"

extern void pc_log_line(const char* fmt, ...);
void net_addr_text(const struct sockaddr* sa, char* out, size_t cap); /* net_wire.c */

#define ANNOUNCE_MS 1000
#define QUERY_MS 2000
#define LOST_MS 5000
#define MAX_IFACES 16
/* clang-format off */
#define MSTR(s) { (s), strlen(s) }
/* clang-format on */

typedef struct Held {
    PcMdnsRecord r;
    uint64_t seen;
} Held;

typedef struct Iface { /* as net_lan.c's */
    char name[32];
    unsigned index;
    struct in_addr a;
    struct in6_addr aaaa;
    bool link_local6;
} Iface;

static bool s_open;
static int s_sock4 = -1, s_sock6 = -1;
static char s_service[64];
static char s_hostname[48];
static struct in_addr s_self4;
static struct in6_addr s_self6;
static char s_self_ip[46] = "0.0.0.0";
static Held s_held[PC_MDNS_RECORDS];
static int s_n;
static bool s_heard;
/* MELEE_NET_SIM_NO_MCAST: a network that drops multicast (hotel and campus
 * Wi-Fi). The sockets open and send, and nothing is ever heard, not even our
 * own query; tests the blocked state and Join by address. */
static bool s_sim_blocked;
static uint64_t s_opened, s_announced, s_queried;
static uint32_t s_rx[1024]; /* mdns.h wants 32-bit aligned buffers */
static uint32_t s_tx[1024];

/* Ours. */
static bool s_announcing;
static char s_instance[64], s_fqdn[128];
static uint16_t s_port;
static int s_ntxt;
static char s_key[PC_MDNS_TXT_MAX][16];
static char s_val[PC_MDNS_TXT_MAX][256];

static uint64_t now_ms(void) {
    return SDL_GetTicks();
}

/* ---- interfaces (net_lan.c's choice) ---------------------------------- */

static bool prefix_ci(const char* s, const char* lower) {
    while (*lower && tolower((unsigned char)*s) == *lower)
        s++, lower++;
    return *lower == '\0';
}

static bool iface_skipped(const char* name) {
    static const char* const virt[] = {"docker", "veth", "br-", "virbr", "tun", "tap", "wg", "utun",
        "zt", "zerotier", "tailscale", "radmin"};
    for (size_t i = 0; i < sizeof virt / sizeof virt[0]; i++) {
        if (prefix_ci(name, virt[i])) {
            return true;
        }
    }
    return false;
}

static void iface_add(Iface* v, int* n, const char* name, unsigned index, const struct sockaddr* sa) {
    int i = 0;
    while (i < *n && strcmp(v[i].name, name) != 0) {
        i++;
    }
    if (i == *n) {
        if (*n == MAX_IFACES) {
            return;
        }
        memset(&v[i], 0, sizeof v[i]);
        snprintf(v[i].name, sizeof v[i].name, "%s", name);
        v[i].index = index;
        (*n)++;
    }
    if (sa->sa_family == AF_INET) {
        if (v[i].a.s_addr == 0) {
            v[i].a = ((const struct sockaddr_in*)sa)->sin_addr;
        }
    } else if (sa->sa_family == AF_INET6) {
        const struct in6_addr* a6 = &((const struct sockaddr_in6*)sa)->sin6_addr;
        bool ll = IN6_IS_ADDR_LINKLOCAL(a6);
        if (IN6_IS_ADDR_UNSPECIFIED(&v[i].aaaa) || (v[i].link_local6 && !ll)) {
            v[i].aaaa = *a6;
            v[i].link_local6 = ll;
        }
    }
}

static int iface_list(Iface* v) {
    int n = 0;
#if defined(_WIN32)
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG len = 16384;
    IP_ADAPTER_ADDRESSES* list = malloc(len);
    if (list != NULL && GetAdaptersAddresses(AF_UNSPEC, flags, NULL, list, &len) == ERROR_BUFFER_OVERFLOW) {
        free(list);
        list = malloc(len);
    }
    if (list != NULL && GetAdaptersAddresses(AF_UNSPEC, flags, NULL, list, &len) == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES* a = list; a != NULL; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                continue;
            }
            char name[32];
            if (WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof name, NULL, NULL) == 0) {
                snprintf(name, sizeof name, "%s", a->AdapterName);
            }
            for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u != NULL; u = u->Next) {
                iface_add(v, &n, name, a->Ipv6IfIndex, u->Address.lpSockaddr);
            }
        }
    }
    free(list);
#else
    struct ifaddrs* list;
    if (getifaddrs(&list) == 0) {
        for (struct ifaddrs* a = list; a != NULL; a = a->ifa_next) {
            unsigned want = IFF_UP | IFF_RUNNING;
            if (a->ifa_addr != NULL && (a->ifa_flags & (want | IFF_LOOPBACK)) == want) {
                iface_add(v, &n, a->ifa_name, if_nametoindex(a->ifa_name), a->ifa_addr);
            }
        }
        freeifaddrs(list);
    }
#endif
    return n;
}

static bool route_to(const struct sockaddr_in* to, struct in_addr* out) {
    struct sockaddr_in self;
    socklen_t len = sizeof self;
    int probe = (int)socket(AF_INET, SOCK_DGRAM, 0);
    bool ok = probe >= 0 && connect(probe, (const struct sockaddr*)to, sizeof *to) == 0 &&
              getsockname(probe, (struct sockaddr*)&self, &len) == 0;
    if (probe >= 0) {
        sock_close(probe);
    }
    if (ok) {
        *out = self.sin_addr;
    }
    return ok;
}

static void mcast_iface(const Iface* f) {
    if (s_sock4 >= 0 && f->a.s_addr != 0) {
        struct ip_mreq req;
        memset(&req, 0, sizeof req);
        req.imr_multiaddr.s_addr = htonl(0xE00000FBu);
        req.imr_interface = f->a;
        setsockopt(s_sock4, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&f->a, sizeof f->a);
        setsockopt(s_sock4, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&req, sizeof req);
    }
    if (s_sock6 >= 0 && f->index != 0) {
        struct ipv6_mreq req;
        unsigned index = f->index;
        memset(&req, 0, sizeof req);
        req.ipv6mr_multiaddr.s6_addr[0] = 0xFF;
        req.ipv6mr_multiaddr.s6_addr[1] = 0x02;
        req.ipv6mr_multiaddr.s6_addr[15] = 0xFB;
        req.ipv6mr_interface = index;
        setsockopt(s_sock6, IPPROTO_IPV6, IPV6_MULTICAST_IF, (const char*)&index, sizeof index);
        setsockopt(s_sock6, IPPROTO_IPV6, IPV6_JOIN_GROUP, (const char*)&req, sizeof req);
    }
}

/* The interface on the route to the mDNS group unless it's a container or
 * VPN one and a plain one exists (net_lan.c's pick_iface). */
static void pick_iface(void) {
    Iface v[MAX_IFACES];
    int n = iface_list(v);
    struct sockaddr_in group;
    struct in_addr route;
    memset(&group, 0, sizeof group);
    group.sin_family = AF_INET;
    group.sin_port = htons(MDNS_PORT);
    group.sin_addr.s_addr = htonl(0xE00000FBu);
    int on_route = -1;
    if (route_to(&group, &route)) {
        for (int i = 0; i < n && on_route < 0; i++) {
            if (v[i].a.s_addr == route.s_addr) {
                on_route = i;
            }
        }
        if (on_route < 0 && n < MAX_IFACES) {
            memset(&v[n], 0, sizeof v[n]);
            strcpy(v[n].name, "?");
            v[n].a = route;
            on_route = n++;
        }
    }
    int pick = -1, best = -1;
    for (int i = 0; i < n; i++) {
        int score = !iface_skipped(v[i].name) * 4 + (i == on_route) * 2 + (v[i].a.s_addr != 0);
        if (score > best) {
            best = score;
            pick = i;
        }
    }
    memset(&s_self4, 0, sizeof s_self4);
    memset(&s_self6, 0, sizeof s_self6);
    strcpy(s_self_ip, "0.0.0.0");
    if (pick < 0) {
        pc_log_line("mdns: no usable network interface");
        return;
    }
    s_self4 = v[pick].a;
    s_self6 = v[pick].aaaa;
    mcast_iface(&v[pick]);
    char a[16] = "-", aaaa[46] = "-";
    if (s_self4.s_addr != 0) {
        inet_ntop(AF_INET, &s_self4, a, sizeof a);
    }
    if (!IN6_IS_ADDR_UNSPECIFIED(&s_self6)) {
        inet_ntop(AF_INET6, &s_self6, aaaa, sizeof aaaa);
    }
    snprintf(s_self_ip, sizeof s_self_ip, "%s", s_self4.s_addr != 0 ? a : aaaa);
    pc_log_line("mdns: interface %s %s %s", v[pick].name, a, aaaa);
}

/* ---- ours ---------------------------------------------------------------- */

static void announce_on(int sock, bool goodbye) {
    if (!s_announcing) {
        return;
    }
    mdns_record_t ptr = {.name = MSTR(s_service), .type = MDNS_RECORDTYPE_PTR, .data.ptr.name = MSTR(s_fqdn)};
    mdns_record_t extra[PC_MDNS_TXT_MAX + 3];
    size_t n = 0;
    memset(extra, 0, sizeof extra);
    extra[n].name = (mdns_string_t)MSTR(s_fqdn);
    extra[n].type = MDNS_RECORDTYPE_SRV;
    extra[n].data.srv.port = s_port;
    extra[n++].data.srv.name = (mdns_string_t)MSTR(s_hostname);
    for (int i = 0; i < s_ntxt; i++) {
        extra[n].name = (mdns_string_t)MSTR(s_fqdn);
        extra[n].type = MDNS_RECORDTYPE_TXT;
        extra[n].data.txt.key = (mdns_string_t)MSTR(s_key[i]);
        extra[n++].data.txt.value = (mdns_string_t)MSTR(s_val[i]);
    }
    if (s_self4.s_addr != 0) {
        extra[n].name = (mdns_string_t)MSTR(s_hostname);
        extra[n].type = MDNS_RECORDTYPE_A;
        extra[n].data.a.addr.sin_family = AF_INET;
        extra[n++].data.a.addr.sin_addr = s_self4;
    }
    if (!IN6_IS_ADDR_UNSPECIFIED(&s_self6)) {
        extra[n].name = (mdns_string_t)MSTR(s_hostname);
        extra[n].type = MDNS_RECORDTYPE_AAAA;
        extra[n].data.aaaa.addr.sin6_family = AF_INET6;
        extra[n++].data.aaaa.addr.sin6_addr = s_self6;
    }
    (goodbye ? mdns_goodbye_multicast : mdns_announce_multicast)(sock, s_tx, sizeof s_tx, ptr, NULL, 0, extra, n);
}

static void announce(bool goodbye) {
    if (s_sock4 >= 0) {
        announce_on(s_sock4, goodbye);
    }
    if (s_sock6 >= 0) {
        announce_on(s_sock6, goodbye);
    }
    s_announced = now_ms();
}

static void query(void) {
    const size_t len = strlen(s_service);
    if (s_sock4 >= 0) {
        mdns_query_send(s_sock4, MDNS_RECORDTYPE_PTR, s_service, len, s_tx, sizeof s_tx, 0);
    }
    if (s_sock6 >= 0) {
        mdns_query_send(s_sock6, MDNS_RECORDTYPE_PTR, s_service, len, s_tx, sizeof s_tx, 0);
    }
    s_queried = now_ms();
}

/* ---- theirs -------------------------------------------------------------- */

static bool printable(mdns_string_t s, size_t cap) {
    if (s.str == NULL || s.length >= cap) {
        return false;
    }
    for (size_t i = 0; i < s.length; i++) {
        if ((unsigned char)s.str[i] < 0x20 || (unsigned char)s.str[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

static int on_record(int sock, const struct sockaddr* from, size_t addrlen, mdns_entry_type_t entry, uint16_t query_id,
    uint16_t rtype, uint16_t rclass, uint32_t ttl, const void* data, size_t size, size_t name_offset,
    size_t name_length, size_t record_offset, size_t record_length, void* ud) {
    (void)addrlen, (void)query_id, (void)rclass, (void)name_length, (void)ud;
    if (s_sim_blocked) {
        return 0;
    }
    char namebuf[256];
    mdns_string_t name = mdns_string_extract(data, size, &name_offset, namebuf, sizeof namebuf);
    const size_t slen = strlen(s_service);
    if (entry == MDNS_ENTRYTYPE_QUESTION) {
        if ((rtype == MDNS_RECORDTYPE_PTR || rtype == MDNS_RECORDTYPE_ANY) && name.length == slen &&
            memcmp(name.str, s_service, slen) == 0)
        {
            s_heard = true; /* a question for the service: ours looped back, or a browser's */
            announce_on(sock, false);
        }
        return 0;
    }
    if (rtype != MDNS_RECORDTYPE_TXT || (from->sa_family != AF_INET && from->sa_family != AF_INET6) ||
        name.length <= slen + 1 || memcmp(name.str + name.length - slen, s_service, slen) != 0)
    {
        return 0;
    }
    s_heard = true;
    char instance[64];
    const size_t ilen = name.length - slen - 1; /* "<instance>." before the service */
    if (ilen == 0 || ilen >= sizeof instance) {
        return 0;
    }
    memcpy(instance, name.str, ilen);
    instance[ilen] = '\0';
    if (s_announcing && strcmp(instance, s_instance) == 0) {
        return 0; /* ours, looped back */
    }
    int i = 0;
    while (i < s_n && strcmp(s_held[i].r.instance, instance) != 0) {
        i++;
    }
    if (ttl == 0) { /* goodbye */
        if (i < s_n) {
            pc_log_line("mdns: %s left", instance);
            s_held[i] = s_held[--s_n];
        }
        return 0;
    }
    mdns_record_txt_t txt[PC_MDNS_TXT_MAX];
    size_t n = mdns_record_parse_txt(data, size, record_offset, record_length, txt, PC_MDNS_TXT_MAX);
    PcMdnsRecord r;
    memset(&r, 0, sizeof r);
    snprintf(r.instance, sizeof r.instance, "%s", instance);
    net_addr_text(from, r.ip, sizeof r.ip);
    for (size_t t = 0; t < n && r.ntxt < PC_MDNS_TXT_MAX; t++) {
        if (!printable(txt[t].key, sizeof r.key[0]) || !printable(txt[t].value, sizeof r.value[0])) {
            return 0; /* malformed: dropped whole */
        }
        memcpy(r.key[r.ntxt], txt[t].key.str, txt[t].key.length);
        memcpy(r.value[r.ntxt], txt[t].value.str, txt[t].value.length);
        r.ntxt++;
    }
    const char* port = pc_mdns_txt(&r, "port");
    r.port = (uint16_t)(port != NULL ? atoi(port) : 0);
    if (i == s_n) {
        if (s_n == PC_MDNS_RECORDS) {
            return 0;
        }
        s_n++;
        pc_log_line("mdns: found %s at %s", instance, r.ip);
    } else if (strchr(r.ip, ':') != NULL && strchr(s_held[i].r.ip, ':') == NULL) {
        memcpy(r.ip, s_held[i].r.ip, sizeof r.ip); /* seen on both families: keep IPv4 */
    }
    s_held[i].r = r;
    s_held[i].seen = now_ms();
    return 0;
}

/* ---- the calls ------------------------------------------------------------ */

bool pc_mdns_open(const char* service) {
    if (s_open) {
        return s_sock4 >= 0 || s_sock6 >= 0;
    }
    s_open = true;
    s_sim_blocked = getenv("MELEE_NET_SIM_NO_MCAST") != NULL;
    if (s_sim_blocked) {
        pc_log_line("mdns: MELEE_NET_SIM_NO_MCAST: hearing nothing");
    }
    pc_android_multicast_lock_acquire(); /* Android filters mDNS answers without it */
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    snprintf(s_service, sizeof s_service, "%s", service);
    struct sockaddr_in any4;
    struct sockaddr_in6 any6;
    memset(&any4, 0, sizeof any4);
    memset(&any6, 0, sizeof any6);
    any4.sin_family = AF_INET;
    any4.sin_port = htons(MDNS_PORT);
    any6.sin6_family = AF_INET6;
    any6.sin6_port = htons(MDNS_PORT);
    any6.sin6_addr = in6addr_any;
    s_sock4 = mdns_socket_open_ipv4(&any4);
    s_sock6 = mdns_socket_open_ipv6(&any6);
    if (s_sock4 < 0 && s_sock6 < 0) {
        pc_log_line("mdns: cannot open a socket on udp/%d: no discovery", MDNS_PORT);
    }
    pick_iface();
    s_n = 0;
    s_heard = false;
    s_announcing = false;
    s_opened = now_ms();
    query();
    return s_sock4 >= 0 || s_sock6 >= 0;
}

void pc_mdns_close(void) {
    if (!s_open) {
        return;
    }
    if (s_announcing) {
        announce(true);
        s_announcing = false;
    }
    if (s_sock4 >= 0) {
        mdns_socket_close(s_sock4);
    }
    if (s_sock6 >= 0) {
        mdns_socket_close(s_sock6);
    }
    s_sock4 = s_sock6 = -1;
    s_n = 0;
    s_open = false;
    pc_android_multicast_lock_release();
}

void pc_mdns_announce(const char* instance, uint16_t port, const char* const* keys, const char* const* values, int n) {
    if (!s_open) {
        return;
    }
    if (s_announcing && (instance == NULL || strcmp(instance, s_instance) != 0)) {
        announce(true); /* the last one goes */
    }
    s_announcing = instance != NULL;
    if (!s_announcing) {
        return;
    }
    snprintf(s_instance, sizeof s_instance, "%s", instance);
    snprintf(s_fqdn, sizeof s_fqdn, "%s.%s", s_instance, s_service);
    snprintf(s_hostname, sizeof s_hostname, "%.32s.local.", s_instance);
    s_port = port;
    s_ntxt = n < PC_MDNS_TXT_MAX ? n : PC_MDNS_TXT_MAX;
    for (int i = 0; i < s_ntxt; i++) {
        snprintf(s_key[i], sizeof s_key[i], "%s", keys[i]);
        snprintf(s_val[i], sizeof s_val[i], "%s", values[i]);
    }
    announce(false);
}

void pc_mdns_poll(void) {
    if (!s_open) {
        return;
    }
    for (int i = 0; i < 32 && s_sock4 >= 0; i++) {
        if (mdns_socket_listen(s_sock4, s_rx, sizeof s_rx, on_record, NULL) == 0) {
            break;
        }
    }
    for (int i = 0; i < 32 && s_sock6 >= 0; i++) {
        if (mdns_socket_listen(s_sock6, s_rx, sizeof s_rx, on_record, NULL) == 0) {
            break;
        }
    }
    const uint64_t t = now_ms();
    for (int i = 0; i < s_n;) {
        if (t - s_held[i].seen > LOST_MS) {
            pc_log_line("mdns: lost %s", s_held[i].r.instance);
            s_held[i] = s_held[--s_n];
        } else {
            i++;
        }
    }
    if (s_announcing && t - s_announced >= ANNOUNCE_MS) {
        announce(false);
    }
    if (t - s_queried >= QUERY_MS) {
        query();
    }
}

int pc_mdns_records(PcMdnsRecord* out, int max) {
    int n = s_n < max ? s_n : max;
    for (int i = 0; i < n; i++) {
        out[i] = s_held[i].r;
    }
    return n;
}

const char* pc_mdns_txt(const PcMdnsRecord* r, const char* key) {
    for (int i = 0; i < r->ntxt; i++) {
        if (strcmp(r->key[i], key) == 0) {
            return r->value[i];
        }
    }
    return NULL;
}

bool pc_mdns_unavailable(void) {
    return s_open && ((s_sock4 < 0 && s_sock6 < 0) || (!s_heard && now_ms() - s_opened > LOST_MS));
}

const char* pc_mdns_self_ip(void) {
    return s_self_ip;
}
