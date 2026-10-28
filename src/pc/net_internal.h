/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Internals shared by the netplay modules (src/pc/net*.c); callers use
 * pc/net.h and pc/net_lan.h. Modules:
 *   net.c           session lifecycle, receive dispatch, input send/ack,
 *                   stall/barrier, rollback and the per-tick entry points
 *   net_wire.c      byte order, wire codecs, headers, pad conversion, and
 *                   the session key and per-datagram MAC
 *   net_sim.c       link simulator (loss/delay/jitter/reorder/dup/burst);
 *                   every outgoing datagram goes through tx()
 *   net_reliable.c  stop-and-wait reliable channel ('R'/'K')
 *   net_handshake.c RULES/READY match handshake and the rules in force
 *   net_sync.c      time sync (offset/jitter rings, skip/advance, auto delay)
 *   net_snapshot.c  snapshots, frame checksum, state ring, sync test,
 *                   record/replay
 *
 * Threading
 * ---------
 * Everything runs on the game thread except two helpers in net.c:
 *   - tx_timer, a 4 ms SDL timer that resends the newest input packet,
 *     releases held (simulated) datagrams and retransmits the reliable
 *     message in flight;
 *   - rx_main, the receive thread: it drains the socket, runs the
 *     authentication gate, acks each input packet and times each ack the
 *     moment it lands, and queues everything else for recv_inputs, which
 *     applies it on the game thread. Its gate state, counters and queue are
 *     under s_rx_lock (net.c), taken before tx_lock and never inside it.
 * The two helpers and the game thread share, under net.tx_lock only:
 *   - the socket: net.active flips under the lock too, so the timer never
 *     sends on a closed socket, and pc_net_disconnect removes the timer and
 *     joins the receive thread before taking the lock, so neither outlives
 *     the socket;
 *   - each link's frozen packet copy (Peer.last_pkt, net.c), the held queue
 *     (s_held, net_sim.c) and the sim knobs tx() reads, net.tx_pkts/
 *     tx_inputs;
 *   - each link's reliable transmit queues (s_rel_tx, net_reliable.c): the
 *     head of a lane is the message in flight, resent until its 'K' arrives.
 *     The reliable receive queue is game thread only;
 *   - the link keys (net_wire.c), each link's RTT ring, and net.session and
 *     each Peer.addr, which the receive thread alone changes once a session
 *     is up (the guest learning its id, a dual-stack peer answering from
 *     another address).
 * All simulation state (frames, input rings, snapshots, rollback, time
 * sync, handshake, record/replay) is game thread only; pc_net_note_io
 * ignores other threads for that reason.
 *
 * Invariants
 * ----------
 *   - s_port_have[p] (net.c) is the newest contiguous real frame of remote
 *     port p: every frame <= it holds real input in s_remote_ring[p], every
 *     simulated frame above it holds the prediction it ran on. A gap is never
 *     stored; the ack tells the peer what to resend. s_remote_have is the
 *     lowest of them over the remote ports.
 *   - a snapshot is valid only for its exact frame in the scene it was
 *     taken in: Snapshot.frame is -1 when it holds nothing usable and
 *     snapshot_unusable() must pass before a restore.
 *   - nothing at or below net.rb_barrier is rolled back to and no snapshot
 *     is taken for such a frame; the barrier only rises within a session.
 *   - net.frame is the next fresh frame; net.tick_frame is the frame of the
 *     tick being run (frame - 1 outside a rollback, older during one).
 *   - a datagram is authenticated before anything reads it: once one tag
 *     has verified, the receive gate accepts nothing that does not, so no state
 *     below it (peer address, session id, rings, reliable lane) can be
 *     moved by a datagram that is not the peer's.
 *   - net.tx_pkts/tx_inputs are counted under tx_lock and cleared by the
 *     game thread's report without it (a lost increment is a stat, not
 *     state). */
#ifndef PC_NET_INTERNAL_H
#define PC_NET_INTERNAL_H

#include "pc/net.h"
#include "pc/net_lan.h"
#include "pc/pc.h"
#include "pc/slippi/slp_checksum.h"

#include <dolphin/pad.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wscalar-storage-order" /* disc-struct unions in lb/types.h */
#include <melee/gm/types.h>
#pragma GCC diagnostic pop

#include <SDL3/SDL_mutex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- platform socket shim --------------------------------------------- */

#if defined(_WIN32)
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define getpid _getpid
static inline bool sock_nonblock(sock_t s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}
static inline void sock_startup(void) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
static inline bool sock_nonblock(sock_t s) {
    int f = fcntl(s, F_GETFL, 0);
    return f != -1 && fcntl(s, F_SETFL, f | O_NONBLOCK) != -1;
}
static inline void sock_startup(void) {}
#define sock_close close
#endif

/* ---- constants -------------------------------------------------------- */

#define RING 64 /* frames of history kept per side; power of two */
/* Unacked frames an input packet can carry. Delivery runs at one packet's
 * worth per round trip (net.c send_inputs), so 32 holds 60 Hz to ~530 ms. */
#define REDUNDANCY 32
#define WINDOW 7 /* predicted frames allowed before a hard stall */
#define SNAPS 8  /* snapshot ring, one per predicted frame; > WINDOW */
#define FRAME_US ((int32_t)(pc_sim_period_ns() / 1000)) /* the boundary's pacing target */
#define STALL_TIMEOUT_MS 3000
#define CONNECT_TIMEOUT_MS 60000
/* The same wait for a session matchmaking just paired (net.connect_timeout_ms):
 * the peer signed an offer moments ago, so silence now means a dead address. */
#define MATCH_CONNECT_TIMEOUT_MS 10000
#define SYNC_INTERVAL 30 /* frames between time-sync decisions (Slippi) */
#define IO_QUIET 120     /* frames a disc request keeps the barrier ahead */
/* A session ends before its frame counter can get anywhere near the end of
 * its range. Frames are absolute int32 on the wire and in every ring index,
 * and the protocol has no wrap strategy: rather than invent serial-number
 * comparisons for a case no match reaches, the session is bounded so the
 * counter provably cannot get there. 100 million frames is nineteen days of
 * continuous play, and the peers part cleanly with a reason when it lands
 * instead of overflowing into undefined behaviour. */
#define SESSION_MAX_FRAMES 100000000
/* Remote players a session links to: up to three, one per other controller
 * port (docs/xeleenet-plan.md §3.2). A 1v1 has one. Everything about one link
 * -- its address, key, sequence spaces, reliable lanes -- is indexed by its
 * peer number, 0..net.npeers-1, in every module. */
#define MAX_PEERS 3

/* ---- wire format ------------------------------------------------------ */

/* Multi-byte fields are big-endian on the wire (wire_* in net_wire.c); the
 * packed structs are the exact wire image with host-order fields. */
#define WIRE_VERSION PC_NET_PROTO_VERSION

/* Every datagram carries NET_MAC_LEN trailing bytes of keyed BLAKE2b over
 * everything in front of them, header included (net_wire.c). Eight bytes is
 * the tradeoff: a blind forgery costs 2^-64 per try against a receiver that
 * answers nothing it rejects, while a wider tag would cost another 8 bytes
 * on every one of the ~120 datagrams a second a session sends. The field is
 * present from the first datagram -- zero-filled until the key exists -- so
 * the length of each message type is one number rather than two. */
#define NET_MAC_LEN 8

/* 8-byte pad, same fields Slippi puts on the wire. */
typedef struct WirePad {
    uint16_t button;
    int8_t stickX, stickY, substickX, substickY;
    uint8_t triggerLeft, triggerRight;
} WirePad;

/* Every datagram starts with this; one from another version, session or
 * address is dropped before its body is looked at. */
typedef struct Hdr {
    uint8_t magic;
    uint8_t version;  /* WIRE_VERSION */
    uint32_t session; /* host picks it at connect, the guest learns it */
    uint8_t player;
} __attribute__((packed)) Hdr;

typedef struct Packet {
    Hdr h;            /* 'M' */
    uint16_t seq;     /* per-session tx sequence: dedup, reorder, RTT match */
    int32_t newest;   /* newest local frame the sender holds */
    int32_t first;    /* frame of pads[0]; pads[i] is frame first+i */
    int32_t ck_frame; /* frame the checksum was taken before */
    uint32_t ck;
    uint8_t count;
    /* GGPO's frame advantage: how many frames the sender's simulation is
     * ahead of the newest input it has from us. Both peers send it, and the
     * phase controller acts on the DIFFERENCE, which is the one form of the
     * measurement with no clock, no round trip and no symmetric-path
     * assumption in it (net_sync.c time_sync). Clamped to a byte; the window
     * it can legitimately reach is WINDOW + delay. */
    int8_t adv;
    WirePad pads[REDUNDANCY];
} __attribute__((packed)) Packet;

typedef struct Ack {
    Hdr h;         /* 'A' */
    uint16_t seq;  /* seq of the input packet being acked (RTT sample) */
    int32_t frame; /* newest contiguous frame the sender now holds */
} __attribute__((packed)) Ack;

/* 'F' (proto 12): another player's input, passed on by a peer that holds it
 * (net.c, "forwarding"): the host relays it to a guest that cannot reach that
 * player, and after a drop everyone passes on what they hold of the player
 * who left. The bytes after `origin` are an 'M' packet's after its header --
 * the origin's frames, pads and checksum report; seq and adv are unused. */
typedef struct FwdHdr {
    Hdr h;          /* 'F', from the peer passing it on */
    uint8_t origin; /* the controller port whose input this is */
} __attribute__((packed)) FwdHdr;

/* 'G' (proto 12): the ack for 'F'. */
typedef struct FwdAck {
    Hdr h;          /* 'G' */
    uint8_t origin; /* whose input */
    int32_t frame;  /* newest contiguous frame of it the sender now holds */
} __attribute__((packed)) FwdAck;

/* Reliable lobby message (stop-and-wait, net_reliable.c). */
#define REL_MAX 256
#define REL_RULES 0x01  /* host -> guest {seed, start_frame, nonce} (net_handshake.c) */
#define REL_READY 0x02  /* guest -> host {nonce, echo} (net_handshake.c) */
#define REL_NONCES 0x03 /* host -> guests, 3+ players: every nonce (net_handshake.c) */
#define REL_RESUME 0x12 /* net.c's resume exchange, dispatched by on_rel */
#define REL_DELAY 0x13  /* the host's input-delay pick, dispatched by on_rel */
#define REL_CHAT 0x15   /* fixed quick-chat phrase, consumed before caller queue */
#define REL_SCENE 0x14  /* the scene-exit hand-off, dispatched by on_rel */
/* Links between three or four players (net.c, "routes and drops"). */
#define REL_ROUTE 0x30 /* guest -> host {ports it cannot hear}: relay them to it */
#define REL_RELAY 0x31 /* host -> guest {origin port, type, payload}: a relayed message */
#define REL_GONE 0x32  /* guest -> host {port}: that player has gone silent or left */
#define REL_DROP 0x33  /* host -> guests {port}: pass on everything you hold of it */
#define REL_VOID 0x34  /* host -> guests {port, last, void}: its input ends, the match is void */
typedef struct Rel {
    Hdr h; /* 'R' */
    uint8_t seq;
    uint8_t type; /* < 0x10 the handshake, REL_RESUME net.c, else the caller */
    uint16_t len;
    uint8_t payload[REL_MAX];
} __attribute__((packed)) Rel;

typedef struct RelAck {
    Hdr h; /* 'K' */
    uint8_t seq;
} __attribute__((packed)) RelAck;

typedef struct Bye {
    Hdr h;          /* 'B' */
    uint8_t reason; /* a pc_net_peer_status() value */
} __attribute__((packed)) Bye;

/* Payload of the RULES handshake message (net_handshake.c). */
typedef struct Rules {
    uint32_t seed;
    int32_t start_frame;
    uint64_t nonce; /* the host's per-session nonce, from the platform CSPRNG */
    GameRules game;
    uint8_t item_freq;
    uint64_t item_mask;
    uint32_t stage_mask;
    uint8_t frozen_stadium;
    uint32_t unlock_hash; /* unlock_hash_now() after the sender forced its masks */
    uint32_t hash;        /* rules_hash() of the wire image above; the guest recomputes it */
} __attribute__((packed)) Rules;

/* Payload of the READY reply (net_handshake.c): the guest's own nonce and
 * the host's echoed back, so the host can tell its live peer from a replay
 * of an older session's READY. Its unlock_hash lets the host refuse a
 * mismatch at once instead of waiting out the 15 s timeout. */
typedef struct Ready {
    uint64_t nonce;       /* the guest's */
    uint64_t echo;        /* Rules.nonce as the guest received it */
    uint32_t unlock_hash; /* the guest's forced unlock state */
    int32_t start_frame;  /* the RULES start_frame it took: the host may re-issue */
    uint32_t hash;        /* ready_hash() of the wire image above */
} __attribute__((packed)) Ready;

/* Payload of REL_NONCES (net_handshake.c): every player's handshake nonce by
 * controller port (0 for a seat nobody holds), from the host to each guest
 * once every guest is ready. Two guests never exchange RULES/READY, so this
 * is how each pair of them learns the two values its link key is derived
 * from. Only sessions of three or more players send it. */
typedef struct Nonces {
    uint64_t nonce[4];
    uint32_t hash; /* nonces_hash() of the wire image above */
} __attribute__((packed)) Nonces;

/* Payload of the RESUME message (reliable REL_RESUME, net.c): what the
 * sender still holds after an interruption. Every field is 32-bit, so the
 * big-endian conversion is one loop over the image. */
typedef struct Resume {
    uint32_t session; /* the sender's session id: a restarted peer's differs */
    uint32_t seed;    /* the agreed seed: another match cannot be resumed into */
    int32_t newest;   /* newest frame of its own input it still holds */
    int32_t have;     /* newest contiguous frame it holds of OURS */
    int32_t frame;    /* the frame its game thread is parked on (diagnostics) */
} __attribute__((packed)) Resume;

/* Payload of REL_DELAY (net_sync.c): the host's input delay and the frame
 * both peers switch to it on. Two 32-bit fields, so htonl/ntohl is the
 * whole codec. */
typedef struct DelayMsg {
    uint32_t delay;
    uint32_t frame;
} __attribute__((packed)) DelayMsg;

/* Payload of REL_SCENE (net.c): the frame the sender's scene asked to end
 * on. Both peers leave on max(theirs, ours) + SCENE_HANDOFF, so a load that
 * costs one peer more ticks than the other cannot put the next scene on
 * different frames (docs/netcode-plan.md section 5.2). */
typedef struct SceneMsg {
    uint32_t seq;   /* exits the sender has completed: pairs the two halves */
    uint32_t frame; /* the frame its scene asked to end on */
} __attribute__((packed)) SceneMsg;

_Static_assert(sizeof(WirePad) == 8, "wire layout");
_Static_assert(sizeof(Hdr) == 7, "wire layout");
_Static_assert(sizeof(Packet) == 27 + REDUNDANCY * 8, "wire layout");
_Static_assert(sizeof(Ack) == 13, "wire layout");
_Static_assert(sizeof(FwdHdr) == 8 && sizeof(FwdAck) == 12, "wire layout");
_Static_assert(sizeof(Rel) == 11 + REL_MAX, "wire layout");
_Static_assert(sizeof(RelAck) == 8 && sizeof(Bye) == 8, "wire layout");
_Static_assert(sizeof(Rules) == 16 + sizeof(GameRules) + 22, "wire layout");
_Static_assert(sizeof(Ready) == 28, "wire layout");
_Static_assert(sizeof(Nonces) == 36, "wire layout");
_Static_assert(sizeof(Resume) == 20 && sizeof(Resume) % 4 == 0, "wire layout");
_Static_assert(sizeof(DelayMsg) == 8, "wire layout");
_Static_assert(sizeof(SceneMsg) == 8, "wire layout");

/* An input packet on the wire: the fields before pads as they are, then the
 * pads delta-coded (pads_encode, net_wire.c), at most 9 bytes each. */
#define PACKET_WIRE_MAX (offsetof(Packet, pads) + REDUNDANCY * (sizeof(WirePad) + 1))

/* The largest message tx() stamps and the link simulator holds: a full input
 * packet outgrew a reliable one when REDUNDANCY went to 32. */
#define HELD_BYTES (PACKET_WIRE_MAX + 1 > sizeof(Rel) ? PACKET_WIRE_MAX + 1 : sizeof(Rel))

/* Datagrams parked by the simulator; release_ns 0 marks a free slot. Sent in
 * release order, so plain delay stays FIFO and jitter reorders. */
typedef struct Held {
    uint64_t release_ns;
    uint16_t len;
    uint8_t peer; /* the link it goes out on (or came in on) */
    uint8_t buf[HELD_BYTES + NET_MAC_LEN];
} Held;
#define HELD_MAX 128

/* ---- snapshot ----------------------------------------------------------- */

#define MAX_HEAPS 8
/* Two halves of the heap-descriptor array, data, bss, then one per heap. */
#define MAX_REGIONS (4 + MAX_HEAPS)

typedef struct Region {
    const char* name;
    void* ptr;
    size_t len;
} Region;

typedef struct Snapshot {
    int32_t frame; /* -1: holds nothing usable */
    int scene;     /* scene_kind() when taken; another scene cannot take it back */
    uint8_t* buf;
    size_t cap;
    size_t used;
    size_t faulted; /* bytes of buf ever written, so already paged in */
    int nregions;
    Region regions[MAX_REGIONS];
    u32* seed_ptr;
    uint32_t seed_val; /* *seed_ptr when taken (diagnostics) */
    int32_t barrier;   /* net.rb_barrier when taken (diagnostics) */
} Snapshot;

/* ---- session state that crosses modules --------------------------------- */

enum { HS_IDLE, HS_PENDING, HS_DONE, HS_FAILED };

/* MELEE_NET_SYNC: "on" (default) is the time sync this file documents;
 * "off" measures a run with no skip/advance at all; "legacy" restores the
 * pre-fix skip that discarded a queued pad sample, which is the regression
 * test for the desync it caused. */
enum { SYNC_ON, SYNC_OFF, SYNC_LEGACY };

struct NetSession {
    /* session (net.c); active and sock flip under tx_lock */
    bool active;
    sock_t sock;
    int npeers;               /* links in this session, 1..MAX_PEERS */
    int peer_port[MAX_PEERS]; /* the controller port each peer drives */
    /* The link to that peer is dead (a guest that cannot reach another guest,
     * or a player who left): nothing is sent on it, and its input and reliable
     * messages come through the host instead (net.c, "routes and drops"). */
    bool link_dead[MAX_PEERS];
    int local, remote, delay; /* remote: peer 0's port (the only one in a 1v1) */
    uint32_t session; /* 0 on the guest until the host's first packet */
    /* How long to wait for the peer's first datagram, ms. CONNECT_TIMEOUT_MS
     * for a session the user dialled by hand (the other end may simply be
     * started later); much shorter for one matchmaking just paired, where the
     * peer proved it was running moments ago and a silent one is a dead
     * address, not a slow human. */
    int connect_timeout_ms;
    int32_t frame;      /* next fresh frame to simulate */
    int32_t tick_frame; /* frame the last prepared tick simulates */
    bool resim;         /* re-running frames after a rollback */
    bool desync_reported;
    int32_t rb_barrier; /* no frame <= this is rolled back to */

    /* transmit: timer thread + game thread under tx_lock */
    SDL_Mutex* tx_lock;
    unsigned tx_pkts, tx_inputs; /* datagrams / input packets this stats window */

    /* link simulator knobs (net_sim.c), set at connect */
    int sim_loss; /* percent of outgoing packets dropped */
    uint64_t sim_delay_ns, sim_rx_delay_ns;
    int sim_jitter_ms, sim_reorder, sim_dup, sim_burst;
    bool sim_hold; /* delay/jitter/reorder/dup on: tx goes via s_held */

    /* match handshake (net_handshake.c) */
    int hs; /* HS_* */
    bool hs_host;
    bool direct;   /* MELEE_NET: no lobby, so the session agrees its own match */
    uint32_t seed; /* agreed RNG seed (0: none) */
    /* Slippi: the decider's rngOffset. It is the match's first seed and the
     * constant SyncRNG adds every frame (gmvs.c slippiSyncRng). */
    uint32_t rng_offset;
    int32_t start_frame;
    int32_t ck_from; /* checksums before this frame are not compared */

    /* time sync (net_sync.c) */
    uint32_t ping_us; /* smoothed RTT */
    bool delay_auto;
    int delay_next;   /* the host's pick, applied at delay_at */
    int32_t delay_at; /* frame both peers switch delay on (0: none) */
    int32_t offset_last;
    unsigned skips;
    int advance_left;
    int sync_mode;      /* SYNC_* from MELEE_NET_SYNC */
    bool pad_reused;    /* this present queued no new physical sample */
    unsigned pad_reuse; /* how often that happened */
    unsigned pad_empty; /* ticks that ran with an empty pad queue (a bug) */
    uint64_t waited_ns; /* game thread blocked on the peer since the last boundary */

    /* sync test (net_snapshot.c) */
    bool synctest;

    /* Slippi transport (MELEE_NET_STACK=slippi, src/pc/slippi/). When true,
     * the send/receive/wait paths below go through the Slippi ENet driver and
     * its adapter instead of the custom UDP socket, while the rollback loop,
     * snapshot ring, barrier and pad handling stay exactly as they are. `sock`
     * is SOCK_INVALID under Slippi (there is no custom socket). */
    bool slippi;
};
extern struct NetSession net;

/* ---- net.c ------------------------------------------------------------ */

/* Apply what the receive thread queued: inputs, acks, reliable messages
 * (game thread). */
void recv_inputs(void);
int scene_kind(void);
bool in_fight(void);

/* A REL_RESUME payload from the peer (on_rel dispatches it here instead of
 * queueing it for the caller); game thread, like everything recv_inputs
 * applies. */
void net_resume_rel(int peer, const void* payload, int len);

/* The link to the host (the peer on port 0), or -1 on the host itself. */
int net_host_peer(void);
/* REL_ROUTE..REL_VOID from `peer` (net_reliable.c dispatches them here). */
void net_link_rel(int peer, uint8_t type, const void* payload, int len);
/* The host passes a reliable message from `peer` on to the guests that cannot
 * hear it (REL_RELAY); a no-op anywhere else. */
void net_relay_out(int peer, uint8_t type, const void* payload, int len);

/* One sendto with errno/WSA translation and the sock_err counter; used by
 * the senders here and the link simulator's flush (net_sim.c). Caller holds
 * tx_lock. Returns bytes sent, or -1 on any error (transient or logged). */
int net_sendto(int peer, const void* buf, size_t len);

/* ---- net_wire.c ------------------------------------------------------- */

uint32_t fnv1a(uint32_t h, const void* data, size_t n);
void to_wire(WirePad* w, const PADStatus* p);
void from_wire(PADStatus* p, const WirePad* w);
void wire_hdr(Hdr* h);
void wire_packet(Packet* pk);
size_t pads_encode(const WirePad* pads, int count, uint8_t* out);
int pads_wire_len(const uint8_t* in, int len, int count);
bool pads_decode(const uint8_t* in, int len, int count, WirePad* pads);
void wire_ack(Ack* a);
void wire_rel(Rel* r);
void wire_rules(Rules* ru);
void wire_ready(Ready* rd);
void wire_nonces(Nonces* nc);
/* Handshake hashes: FNV over the payload's wire image with the session id
 * folded in, so a payload captured from one session cannot validate in
 * another. Rules' image carries the host nonce and Ready's carries both, so
 * between them the session id and both nonces are bound; RULES cannot bind
 * the guest's nonce because it does not exist yet when RULES is sent. */
uint32_t rules_hash(Rules ru, uint32_t session);
uint32_t ready_hash(Ready rd, uint32_t session);
uint32_t nonces_hash(Nonces nc, uint32_t session);

/* Per-session datagram authentication. The key is BLAKE2b over the session
 * id and both handshake nonces, which is the one thing in the session an
 * off-path attacker cannot see: it can guess the session id, but not two
 * 64-bit CSPRNG draws that only ever travel inside the RULES/READY pair.
 * Both peers derive it from the same three values, so neither has to send
 * anything extra for it. net_key_direct() pins a key from a user-supplied
 * secret instead, for the direct MELEE_NET sessions that run no handshake;
 * a pinned key is never replaced by a derived one, so a session cannot rekey
 * mid-flight and lose the datagrams that straddle the change. */
void net_key_session(int peer, uint64_t host_nonce, uint64_t guest_nonce);
void net_key_direct(const char* secret);
/* A key agreed outside the session (XeleeNet's punch), pinned like MELEE_NET_KEY. */
void net_key_link(int peer, const uint8_t key[32]);
void net_key_clear(void);
bool net_key_ready(int peer);
/* True when the key came from MELEE_NET_KEY rather than the handshake. Both
 * peers then hold it from connect, so there is no leg of a handshake to wait
 * out and an unauthenticated datagram is a forgery from the first one. */
bool net_key_pinned(void);
/* Write the tag for the len bytes at buf into buf[len..len+NET_MAC_LEN); the
 * caller owns that room. Zeros while no key exists. */
void net_mac_stamp(int peer, void* buf, size_t len);
/* True when the tag after the len bytes at buf is this session's. False
 * whenever no key exists: the caller decides what an unauthenticated
 * datagram means at that point in the session. */
bool net_mac_ok(int peer, const void* buf, size_t len);
Hdr hdr(uint8_t magic);
bool addr_eq(const struct sockaddr_storage* a, const struct sockaddr_storage* b);
/* net_lan.c; text form of a datagram source, for logs and getaddrinfo(). */
void net_addr_text(const struct sockaddr* sa, char* out, size_t cap);

/* ---- net_sim.c -------------------------------------------------------- */

int held_put(Held* held, const void* buf, size_t len, uint64_t release_ns);
Held* held_due(Held* held, uint64_t now);
void tx(int peer, const void* buf, size_t len); /* caller holds tx_lock */
void tx_flush(void);                  /* caller holds tx_lock */
void sim_env(uint16_t bind_port);     /* MELEE_NET_SIM_* into net.sim_* */
/* MELEE_NET_SIM_BLOCK as bits (a * 4 + b), both ways per pair; the punch
 * phase (net_punch.c) honours it too. */
uint16_t net_sim_block_mask(void);
void sim_reset(void);

/* ---- net_reliable.c --------------------------------------------------- */

void rel_service(void); /* every peer's lanes; caller holds tx_lock */
void on_rel(int peer, const Rel* r, int n);
void on_rel_ack(int peer, const RelAck* k);
/* Deliver one lane-1 reliable message to its consumer (REL_RESUME, REL_DELAY,
 * REL_SCENE, REL_CHAT, or a caller-queued type). Shared by the custom
 * transport's on_rel and the Slippi receive path. False when a caller-queued
 * message had no room. */
bool rel_dispatch(int peer, uint8_t type, const void* payload, int len);
void rel_reset(void);
/* Queue a reliable message for one link only (the handshake's answers, a
 * resume's numbers, which differ per link). False when that lane is full. */
bool rel_send_to(int peer, uint8_t type, const void* payload, int len);
void rel_link_drop(int peer); /* caller holds tx_lock */

/* ---- net_handshake.c -------------------------------------------------- */

void handshake_msg(int peer, uint8_t type, const uint8_t* payload, int len);
void rules_restore(void);
int32_t pc_net_slippi_time_offset_us(void);
void handshake_direct(void);

/* ---- net_sync.c ------------------------------------------------------- */

/* One phase sample: the peer's frame advantage and ours (net.c on_inputs). */
void adv_note(int peer, int remote_adv, int local_adv);
void jitter_note(uint32_t rtt); /* one RTT sample, for the |dRTT| mean */
uint32_t jitter_us(void);
void time_sync(void);
/* The host's REL_DELAY announcement (on_rel dispatches it here, like
 * REL_RESUME); game thread. */
void net_delay_rel(int peer, const void* payload, int len);

/* A REL_SCENE payload from the peer (on_rel dispatches it here, like
 * REL_DELAY): the frame its scene asked to end on. */
void net_scene_rel(int peer, const void* payload, int len);

/* A silent freeze: the transmit timer notices the game thread has stopped
 * ticking and asks it for a stack (src/pc/net_watchdog.c). */
void net_watchdog_arm(void);
void net_watchdog_tick(int32_t frame);
void net_watchdog_heartbeat(void);
void sync_reset(void);

/* ---- net_snapshot.c --------------------------------------------------- */

bool snapshot_take(Snapshot* s, int32_t frame);
/* True when the last snapshot_take failed only because a DVD/ARQ transfer
 * was in flight: that frame cannot be predicted, the next one can. */
bool snapshot_refused_io(void);
const char* snapshot_unusable(const Snapshot* s);
void snapshot_restore(const Snapshot* s);
/* Non-NULL for missing/invalid simulation ranges. Normal builds provide
 * validated ELF, PE or Mach-O sections; fixtures can exercise the fallback. */
const char* snapshot_state_region_missing(void);
Snapshot* snap_slot(int32_t f); /* rollback ring entry for frame f */
/* Size and page in every rollback slot for the state as it stands, so the
 * first predicted frame pays neither the allocation nor the faults. */
void snaps_reserve(void);
void snaps_free(void);
void snap_stats_report(void);
uint32_t frame_checksum(const PADStatus* head);
/* Slippi's exact wire desync checksum over the live game (net_snapshot.c),
 * used only on the Slippi transport. Distinct from frame_checksum. */
uint32_t pc_slippi_wire_checksum(void);

/* True when the Slippi transport is the active session's transport. */
bool pc_net_slippi_active(void);
/* Host: send the Slippi direct-session setup (seed + start frame) on ENet's
 * reliable channel; false until the peer is connected. Guest: false always.
 * Interim until M4's selections flow (net.c). */
bool pc_net_slippi_send_setup(uint32_t seed, int32_t start_frame);
/* Send a tunnelled engine reliable message (lane 1) on ENet's reliable channel
 * (net.c). */
bool pc_net_slippi_send_rel(uint8_t type, const void* payload, int len);
/* The peer connection state for the Slippi setup wait: 0 connecting, 1 up,
 * 2 gone. */
int pc_net_slippi_conn_state(void);
void record_state(const PADStatus* head, int32_t frame);
void dump_states_around(int32_t frame);
const char* state_line(int32_t frame); /* one frame's recorded state line, "" if gone */
void record_open(void);
bool record_active(void);
void replay_feed(PADStatus* head);
void record_frame(const PADStatus* head, uint32_t ck, int32_t f);
void record_confirm(int32_t upto);            /* write settled frames out in order */
bool record_replay_scene_hold(int32_t frame); /* replay: hold to the recorded exit */
bool net_local_idle(void); /* net.c; newest local sample has no stick or button */
void synctest_before_tick(void);
bool synctest_after_tick(void);

/* ---- Snapshot ---- */
const char* snapshot_describe(
    const Snapshot* s, char* buf, size_t n); /* one log line of metadata */
void resim_note(int ticks); /* re-run ticks the deepest rollback of a present cost */

#endif
