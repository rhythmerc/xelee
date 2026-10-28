/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay prototype: two instances exchange one PADStatus per frame over UDP
 * and run in rollback lockstep modelled on Slippi (docs/netcode-plan.md §4):
 * a remote input that has not arrived yet is predicted as "repeat last", a
 * snapshot is taken before every predicted tick, and when the real input
 * turns out different the state is restored and the frames since re-run.
 * Both peers must boot with the same disc and no memory card; every session
 * agrees the rest -- seed, rules and unlock state -- in the match handshake,
 * the env path below included (net_handshake.c's handshake_direct). Sessions
 * can also be opened at runtime through pc_net_connect() (src/pc/net_lan.h).
 *
 *   MELEE_NET=host:port          peer address (enables netplay at boot)
 *   MELEE_NET_PORT=n             local UDP port (default 41000)
 *   MELEE_NET_PLAYER=0|1         which controller port the local player drives
 *   MELEE_NET_DELAY=n|auto       input delay in frames (default auto: from ping and jitter)
 *   MELEE_NET_RECONNECT_MS=ms    resume an interrupted session for this long (default 3000, 0 off)
 *   MELEE_NET_SIM_LOSS=percent   drop that share of outgoing packets
 *   MELEE_NET_SIM_DELAY_MS=ms    hold every outgoing packet that long
 *   MELEE_NET_SIM_DELAY_RX_MS=ms hold every incoming packet that long (asymmetric links)
 *   MELEE_NET_SIM_JITTER_MS=ms   uniform +-ms on the outgoing delay (reorders when > delay)
 *   MELEE_NET_SIM_REORDER=pct    hold that share of packets behind the next one
 *   MELEE_NET_SIM_DUP=pct        send that share of packets twice
 *   MELEE_NET_SIM_BURST=n        every 5 s drop n consecutive outgoing packets
 *   (the simulator's PRNG is seeded from MELEE_NET_PORT, so runs repeat)
 *
 * This file is the session, the socket and the rollback loop; the wire
 * codec, link simulator, reliable channel, handshake, time sync and
 * snapshots are the net_*.c modules listed in net_internal.h. */
#include "compat.h"
#include "pc/file_cache.h"
#include "pc/slp.h"
#include "pc/net_internal.h"
#include "pc/net_sfx.h"
#include "pc/net_punch.h"
#include "pc/slippi/slp_adapter.h"
#include "pc/slippi/slp_checksum.h"

#include <dolphin/ar.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include <melee/gm/gmvs.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/lb/lb_0195.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/random.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <poll.h>
#endif

/* ---- state ------------------------------------------------------------ */

struct NetSession net = {
    .sock = SOCK_INVALID, .tick_frame = -1, .rb_barrier = -1, .start_frame = -1};

static WirePad s_local_ring[RING]; /* indexed by frame & (RING-1) */
/* Remote input, one ring per controller port: real input, or the prediction
 * in use. s_remote_mask has a bit for every port a remote player drives (one
 * in a 1v1); each of those ports keeps its own contiguous mark. s_remote_have
 * and s_remote_newest are the lowest of them: every remote input up to
 * s_remote_have is real, which is what the rollback loop waits and checks on.
 * remote_marks_update() recomputes them after any port moves. */
#define PORTS 4
static uint8_t s_remote_mask;
static WirePad s_remote_ring[PORTS][RING];
static int32_t s_port_have[PORTS] = {-1, -1, -1, -1};   /* newest contiguous real frame */
static int32_t s_port_newest[PORTS] = {-1, -1, -1, -1}; /* newest frame its sender reported */
static int32_t s_remote_have = -1;                      /* lowest s_port_have of the remote ports */
static int32_t s_remote_newest = -1;                    /* lowest s_port_newest of them */
/* A player who left (net.c "routes and drops"): its last real frame, -1
 * otherwise. Nothing waits on that port past it, and it reads as released. */
static int32_t s_port_end[PORTS] = {-1, -1, -1, -1};
static uint8_t s_gone; /* ports of players who left (their input is being collected) */
static int32_t s_end_frame = -1; /* game_over_note: first tick with a result */
static bool s_game_over;         /* the match end can no longer be rolled back */

/* One link per remote player (net.npeers of them, net.peer_port[i] its
 * controller port): everything this file keeps about talking to one peer.
 * Each group's comment says which thread owns it; the locks are the ones the
 * whole-session versions of these fields had. */
typedef struct Peer {
    int port; /* the controller port it drives */
    /* Its address: the receive thread follows a dual-stack peer to the address
     * it answers from (rx_datagram), under tx_lock. */
    struct sockaddr_storage addr;
    socklen_t addr_len;
    /* Receive gate (receive thread, s_rx_lock). */
    bool heard;          /* any datagram from it yet */
    uint64_t last_rx_ns; /* when its last accepted datagram arrived */
    /* One authenticated datagram has arrived, so the peer holds the link's
     * key and nothing unauthenticated is accepted from it from here on. Until
     * then mac_quiet counts what was let through for want of one, so a pair
     * that never agrees on a key says so instead of looking protected. */
    bool mac_seen;
    int mac_quiet;
    bool seq_init;
    uint16_t seq_top;  /* highest seq seen */
    uint64_t seq_bits; /* bit k: (top-k) received; bit 0 = top */
    int32_t rx_have;   /* newest contiguous frame of its input queued (acked) */
    /* Transmit (tx_lock). */
    uint16_t tx_seq; /* next tx seq */
    struct {
        uint16_t seq;
        uint64_t send_ns;
    } rtt_ring[64];  /* last 64 sends */
    Packet last_pkt; /* the newest input packet, for tx_timer's resend */
    bool last_valid;
    uint64_t last_send_ns;
    /* Game thread. */
    int32_t last_acked; /* newest local frame it holds */
    uint32_t ping_us;   /* smoothed RTT of this link */
    /* Its newest checksum report, which the host passes on with its input to
     * a guest that cannot hear it (send_forwarded). */
    int32_t ck_last_frame;
    uint32_t ck_last;
    bool left; /* it said goodbye in a session of three or more (receive thread) */
    /* Its checksums by frame. Each input packet reports one frame, the newest
     * it has confirmed, and that is often ahead of ours; a single slot
     * overwritten by every packet meant those frames were never compared from
     * this side. Kept by frame instead, each is compared once we confirm it
     * too (GGRS's pending-checksum map, bounded here by the ring). */
    struct {
        int32_t frame; /* -1: empty */
        uint32_t ck;
    } rck[RING];
    int32_t ck_checked; /* newest frame compared against its */
} Peer;
static Peer s_peer[MAX_PEERS];
static int32_t s_rb_frame = -1;      /* oldest mispredicted frame not rolled back yet */

static int peer_index(const Peer* p) {
    return (int)(p - s_peer);
}

int net_host_peer(void) {
    for (int i = 0; i < net.npeers; i++) {
        if (net.peer_port[i] == 0) {
            return i;
        }
    }
    return -1;
}

static void remote_marks_update(void) {
    int32_t have = INT32_MAX, newest = INT32_MAX;
    for (int p = 0; p < PORTS; p++) {
        if ((s_remote_mask & (1u << p)) && s_port_end[p] < 0) {
            have = s_port_have[p] < have ? s_port_have[p] : have;
            newest = s_port_newest[p] < newest ? s_port_newest[p] : newest;
        }
    }
    /* Every remote player gone: nothing left to wait on. */
    s_remote_have = s_remote_mask ? (have == INT32_MAX ? INT32_MAX / 2 : have) : -1;
    s_remote_newest = s_remote_mask ? (newest == INT32_MAX ? INT32_MAX / 2 : newest) : -1;
}

/* The remote players' ports, at connect: a 1v1 has the one port that is not
 * the local player's. */
static void remote_ports_set(uint8_t mask) {
    s_remote_mask = mask;
    for (int p = 0; p < PORTS; p++) {
        s_port_have[p] = s_port_newest[p] = -1;
    }
    remote_marks_update();
}

/* Slippi transport (MELEE_NET_STACK=slippi, docs/slippi-migration-plan.md
 * section 6). When s_slippi_on is set, the socket send/receive/wait paths go
 * through the Slippi ENet driver and its adapter; the rollback loop, snapshot
 * ring, barrier, pad handling and time-sync estimator stay exactly as they
 * are, because the Slippi frame callback below feeds the same s_remote_ring /
 * s_remote_have / s_rb_frame state the custom transport's on_inputs does. */
static bool s_slippi_on;
/* A Slippi matchmaking session runs the engine for the match only, with no
 * scene hand-offs: a Dolphin peer has no such messages (pc_net_slippi_adopt). */
static bool s_slippi_match_only;
static PcSlippiAdapter s_slippi;
static bool s_slippi_ever_heard;
static uint64_t s_slippi_now_us(void);
/* Defined below, after simulated_upto()/s_local_ring are in scope; forward
 * declared here because send_inputs() (earlier in the file) branches to one. */
static void s_slippi_send_inputs(void);
static bool s_slippi_recv(void);
static bool s_slippi_wait_remote(int32_t need);
static bool s_slippi_ck_checked_once;
static uint32_t s_ck_ring[RING];  /* our checksum entering each frame */
/* Per-frame Slippi wire checksum, computed the same place s_ck_ring is. The
 * wire carries Slippi's formula, not our FNV one, so the desync check under
 * Slippi compares against this ring (see check_desync). */
static uint32_t s_wire_ck_ring[RING];
/* When the contiguous remote mark last moved, 0 before the first time. The
 * no-progress bound is measured from here, not from the start of a wait:
 * wait_remote's own clock restarts on every call, so a peer that advanced one
 * frame just under the bound each time kept the game frozen for ever. */
static uint64_t s_progress_ns;
static bool s_no_progress; /* the wait ended on a talking peer that never advanced */
/* Session-local seed history must not survive a reconnect. */
static uint32_t s_seed_after_tick;
static bool s_seed_have;
static bool s_lockstep;      /* snapshot failure: stop predicting, retain older corrections */
static PADStatus s_raw_last; /* newest physical sample (port 0) */
static int s_status;         /* pc_net_peer_status(); kept until the next connect */
static bool s_peer_left;     /* BYE received or version mismatch: stop waiting */
static bool s_warn_src, s_warn_sess, s_warn_bad, s_warn_mac; /* one reject line each per session */
#define MAC_QUIET_MAX 120 /* ~2 s of a peer's traffic at 60 Hz */

static unsigned s_stalls, s_advances, s_rollbacks, s_rb_lost;
static int s_rb_depth_max;
static uint64_t s_stall_ns_max;
static uint64_t s_ping_sum;
static unsigned s_ping_n;
static uint32_t s_rtt_min, s_rtt_max; /* this stats window */
static unsigned s_rx_pkts;            /* datagrams this stats window */
static unsigned s_rx_acks;            /* every input packet earns one ack: the gap is loss */
static int s_loss_pct;                /* round-trip loss, one-second window */
static unsigned s_loss_tx_mark, s_loss_ack_mark; /* counters at that window's start */
static int s_rb_depth_cur, s_rb_depth_recent;    /* deepest rollback this second / the last */
static int32_t s_stall_frame = -1000;            /* frame that ended a stall > 500 ms */

/* Reconnect phase; the machine and the numbers are down in "resume after an
 * interruption", these live here because send_inputs and fresh_tick read
 * them. MELEE_NET_RECONNECT_MS overrides the window at connect. */
#define RECONNECT_MS 3000
/* RSM_, not RC_: mingw-w64's wingdi.h defines RC_NONE as an empty macro
 * (raster capabilities), so RC_NONE broke the whole enum on Windows. */
enum { RSM_NONE, RSM_ACTIVE, RSM_FAILED };
static int s_rc;         /* reconnect phase */
static uint64_t s_rc_ns; /* when the phase opened */
static bool s_rc_sent;   /* our RESUME is out for this phase */
static int32_t s_rc_window_ms = RECONNECT_MS;

static int32_t s_wrote = -1; /* newest local frame written to s_local_ring */
static uint64_t s_send_ns;   /* when the newest local frame first went out */
static int32_t s_send_frame = -1;

static SDL_ThreadID s_game_thread;

/* Outgoing side: the game thread and a 4 ms SDL timer (mid-frame resend +
 * release of held packets + reliable retransmit) share the socket under
 * net.tx_lock. net.active flips under it too, so the timer never sends on
 * a closed socket. */
static SDL_TimerID s_timer;

/* Receive side: rx_main, a thread of its own, drains the socket, runs the
 * authentication gate, acks every input packet and times the round trip of
 * every ack the moment the datagram lands; recv_inputs applies what it
 * queued on the game thread (the "receive" section says why). s_rx_lock
 * guards the queue and everything the gate owns: each Peer's receive group
 * (heard, MAC grace state, seq window, rx_have), s_rx_held, the sticky
 * "peer gone" pair and the rx counters. It is taken before net.tx_lock,
 * never while holding it. */
static SDL_Thread* s_rx_thread; /* NULL: recv_inputs drains the socket itself */
static atomic_bool s_rx_run;
static SDL_Mutex* s_rx_lock;
static atomic_int s_frame_pub; /* net.frame as the receive thread may read it */
/* send_inputs' verdict on whether the link wants tx_timer's mid-frame
 * resend. Loss, jitter and rollback depth are game-thread state; the timer
 * used to read them bare from its own thread. */
static atomic_bool s_want_resend;
/* send_inputs' adv for each link, recomputed every tick (tx_timer stamps it
 * on resends) */
static atomic_int s_adv_pub[MAX_PEERS];
static bool s_rx_left;           /* BYE or protocol mismatch seen: s_peer_left to be */
static int s_rx_why;             /* the s_status that goes with it */
static unsigned s_rx_full;       /* datagrams dropped on a full queue, this window */
static Held s_rx_held[HELD_MAX]; /* incoming, receive thread */

/* Sequence numbers (proto 3): every input packet carries its link's
 * tx_seq++, assigned in send_packet under tx_lock. The receiver dedups exact
 * copies (RTP-style anti-replay: a 64-bit window behind the highest seq) and
 * counts reorders but still processes them (their pads may fill a gap). */
static unsigned s_rx_dups, s_rx_reorders; /* this stats window */

/* Socket errors: transient ones (EAGAIN/EINTR/ICMP unreachable) are ignored,
 * a hard one is logged once per session and counted. */
static unsigned s_sock_err;
static bool s_warn_sock;
static unsigned s_rx_malformed, s_rx_bad_src, s_rx_bad_sess, s_rx_bad_player, s_tx_would_block;
static unsigned s_rx_bad_mac; /* datagrams refused by the authentication gate */

/* Unacked frames the newest input packet carried, reported as `red`: every
 * one the peer has not acked, up to REDUNDANCY (send_inputs says why). */
static int s_red_target = REDUNDANCY;

/* Rollback re-run cost: every re-run tick of a rollback happens inside the
 * one present that started it, so a deep correction lands as a long frame.
 * The count is reported (snap_stats_report's "resim/present max") so that
 * cost is visible rather than assumed. A full pad queue during a re-run
 * forces reuse of a live sample (s_resim_eat). */
static int s_resim_run;         /* re-run ticks in the present now running */
static unsigned s_resim_eat;    /* live-sample reuses this window */
static bool s_resim_eat_logged; /* one log per session */
static bool s_timer_tried;      /* SDL_AddTimer attempted this session */

/* Rollback barrier: no frame <= net.rb_barrier can be rolled back to, so
 * fresh_tick predicts nothing up to it either (lockstep). Raised by
 *   - a scene change (heaps are torn down and rebuilt around it);
 *   - a game-thread disc request, to IO_QUIET frames ahead: the tick that
 *     issued it cannot be re-run, the completion lands on a worker thread
 *     some frames later, and the match's own loads trail into its first
 *     frames. aurora's in-flight counters cannot narrow that window: the
 *     audio thread streams music through the same path all match;
 *   - a lost rollback (rb_lost), to the newest simulated frame.
 *
 * Snapshot failure uses s_lockstep instead: earlier predicted frames must
 * remain eligible for correction. */
static int s_scene_last = -1; /* scene_kind() at the last fresh tick */
/* Lockstep frames a fight's first loads get before it predicts. */
#define FIGHT_ENTRY_LEAD 10
static bool s_rb_lost_logged; /* rb_lost: one log per session */

static void barrier_raise(int32_t f) {
    if (f > net.rb_barrier) {
        net.rb_barrier = f;
    }
}

/* Disc requests issued by the game thread. The audio thread streams music
 * through the same path into ARAM and its own statics, none of which a
 * snapshot covers, so those do not count. */
void pc_net_note_io(void) {
    if (SDL_GetCurrentThreadID() == s_game_thread) {
        int lead = in_fight() ? 2 : IO_QUIET;
        barrier_raise(net.frame + lead);
    }
}

/* Files a netplay fight may load mid-match without raising the barrier.
 * Every one is served synchronously from the file cache (pinned there, and
 * fetched at fight entry), so the load completes inside the tick that asks
 * for it on both peers, copies pristine bytes over whatever its buffer held,
 * and a re-simulated tick re-runs exactly the same copy. The barrier used to
 * be raised instead, and every rollback behind it was refused: a Pokemon
 * Stadium transformation that landed while a remote input was still
 * predicted locked the misprediction in and desynced the match (net_test
 * --stage 3, "cannot roll back to frame 5818 (behind the barrier)"). Slippi
 * preloads the same four archives for the same reason. A file joins this
 * list only once whatever parses it tolerates a re-run: Stadium's parse
 * refreshes its buffer first (grStadium_801D42B8), since that buffer may sit
 * in memory no snapshot covers. */
static const char* const s_pure_loads[] = {"GrPs1.dat", "GrPs2.dat", "GrPs3.dat", "GrPs4.dat"};

bool pc_net_pure_load(const char* filename) {
    if (!pc_net_deterministic() || filename == NULL) {
        return false;
    }
    const char* base = strrchr(filename, '/');
    base = base != NULL ? base + 1 : filename;
    for (size_t i = 0; i < sizeof s_pure_loads / sizeof s_pure_loads[0]; i++) {
        if (strcmp(base, s_pure_loads[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Fight entry, behind the barrier: have every pure load cached before the
 * first frame that can predict. Machines that prewarm have them already. */
static void pure_loads_fetch(void) {
    static uint32_t fetched_session;
    if (fetched_session == net.session) {
        return;
    }
    fetched_session = net.session;
    for (size_t i = 0; i < sizeof s_pure_loads / sizeof s_pure_loads[0]; i++) {
        if (!pc_file_cache_require(s_pure_loads[i])) {
            pc_log_line("net: %s could not be cached; its mid-match load will stop rollback",
                s_pure_loads[i]);
        }
    }
}

extern struct GameSceneInfo* gm_804D6720; /* current scene, gmscene.c */

int scene_kind(void) {
    return gm_804D6720 != NULL ? gm_804D6720->scene_kind : -1;
}

bool pc_net_chat_available(void) {
    int scene = scene_kind();
    return net.active && (scene == GS_CSS || scene == GS_RESULTS || scene == GS_ONLINE_LOBBY);
}

bool in_fight(void) {
    int k = scene_kind();
    return k == GS_VS || k == GS_SUDDEN_DEATH;
}

/* Newest frame ticked in the current timeline. */
static int32_t simulated_upto(void) {
    return net.resim ? net.tick_frame : net.frame - 1;
}

/* Newest frame whose checksum is final: simulated with real inputs on both
 * sides and not awaiting a rollback. */
static int32_t confirmed_frame(void) {
    int32_t f = simulated_upto();
    if (f > s_remote_have) {
        f = s_remote_have;
    }
    if (s_rb_frame >= 0 && f >= s_rb_frame) {
        f = s_rb_frame - 1;
    }
    return f;
}

/* ---- transmit --------------------------------------------------------- */

static size_t packet_len(const Packet* pk) {
    return offsetof(Packet, pads) + pk->count * sizeof(WirePad);
}

#if defined(_WIN32)
#define sock_last_err() WSAGetLastError()
#else
#define sock_last_err() errno
#endif

/* "Drained": the non-blocking socket has nothing more (recv stops). */
static bool sock_would_block(int e) {
#if defined(_WIN32)
    return e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

/* Transient and ignorable: an interrupted call or an ICMP unreachable queued
 * against a connectionless UDP socket. Not a would-block, so recv keeps
 * draining and send just drops the datagram. */
static bool sock_transient(int e) {
#if defined(_WIN32)
    return e == WSAEINTR || e == WSAECONNRESET;
#else
    return e == EINTR || e == ECONNREFUSED;
#endif
}

/* Block until the socket has a datagram or ms pass. select() is the wait
 * Winsock has; a POSIX descriptor at or above FD_SETSIZE would overflow its
 * fd_set, so poll() there. */
static void sock_wait(sock_t s, int ms) {
#if defined(_WIN32)
    fd_set r;
    FD_ZERO(&r);
    FD_SET(s, &r);
    struct timeval tv = {0, ms * 1000};
    select(0, &r, NULL, NULL, &tv);
#else
    struct pollfd p = {s, POLLIN, 0};
    poll(&p, 1, ms);
#endif
}

/* One hard socket error logged per session, counted for the report. */
static void sock_err_note(const char* what, int e) {
    s_sock_err++;
    if (!s_warn_sock) {
        s_warn_sock = true;
        pc_log_line("net: %s error %d at frame %d", what, e, net.frame);
    }
}

/* One sendto with error translation (caller holds tx_lock). */
int net_sendto(int peer, const void* buf, size_t len) {
    const Peer* p = &s_peer[peer];
    int r = (int)sendto(
        net.sock, (const char*)buf, len, 0, (const struct sockaddr*)&p->addr, p->addr_len);
    if (r < 0) {
        int e = sock_last_err();
        if (sock_would_block(e)) {
            s_tx_would_block++;
        } else if (!sock_transient(e)) {
            sock_err_note("sendto", e);
        }
    }
    return r;
}

/* Wire copy of a host-order input packet out (caller holds tx_lock). Stamps
 * the next tx seq and remembers when it went out, so the matching ack yields
 * a clean RTT sample regardless of resends or delayed acks. */
static void send_packet(Peer* p, Packet* pk) {
    pk->seq = p->tx_seq++;
    int slot = pk->seq & 63;
    p->rtt_ring[slot].seq = pk->seq;
    p->rtt_ring[slot].send_ns = SDL_GetTicksNS();
    Packet w = *pk;
    wire_packet(&w);
    uint8_t out[PACKET_WIRE_MAX];
    memcpy(out, &w, offsetof(Packet, pads));
    size_t n = offsetof(Packet, pads) + pads_encode(w.pads, w.count, out + offsetof(Packet, pads));
    tx(peer_index(p), out, n);
}

/* Slippi's second send: the newest input packet goes out again mid-frame,
 * so a lost packet costs half a frame instead of a whole one. Runs on SDL's
 * timer thread; it only touches the frozen packet copy, the held queue and
 * the reliable transmit queue. Removes itself once the session is gone. */
static Uint32 SDLCALL tx_timer(void* ud, SDL_TimerID id, Uint32 interval) {
    (void)ud;
    (void)id;
    SDL_LockMutex(net.tx_lock);
    if (!net.active) {
        s_timer = 0;
        SDL_UnlockMutex(net.tx_lock);
        return 0;
    }
    tx_flush();
    rel_service();
    uint64_t now = SDL_GetTicksNS();
    /* Mid-frame resend pacing:
     * On clean links (low loss, low jitter, low rollback depth), disable redundant
     * mid-frame retransmissions to prevent bufferbloat and network congestion.
     * Re-enable immediately when packet loss, jitter spikes, or rollbacks occur.
     * If the game thread is stalled in an asset load or DVD read (now - last_send_ns >= 16 ms),
     * keep sending keepalives every 7 ms so neither peer times out. */
    bool need_resend = atomic_load_explicit(&s_want_resend, memory_order_relaxed);
    for (int i = 0; i < net.npeers; i++) {
        Peer* p = &s_peer[i];
        if (s_gone & (1u << p->port)) {
            continue;
        }
        bool game_thread_quiet = (now - p->last_send_ns >= 16000000ull);
        if (p->last_valid && (now - p->last_send_ns >= 7000000ull) &&
            (game_thread_quiet || need_resend))
        {
            /* The pads are frozen, but the advantage is not: a resend
             * carrying the adv from when the packet was built would feed the
             * peer's phase ring a sample up to a frame old (longer across a
             * load). */
            p->last_pkt.adv = (int8_t)atomic_load_explicit(&s_adv_pub[i], memory_order_relaxed);
            send_packet(p, &p->last_pkt);
            p->last_send_ns = now;
        }
    }
    int32_t seen = atomic_load_explicit(&s_frame_pub, memory_order_relaxed);
    SDL_UnlockMutex(net.tx_lock);
    /* Outside the lock: the log write must not hold up the sender, and the
     * stuck thread it signals may itself be waiting on this mutex. */
    net_watchdog_tick(seen);
    return interval;
}

/* ---- routes and drops ----------------------------------------------------
 * Three or four players are a mesh: every player links to every other. Two
 * things that a 1v1 never meets come out of that (docs/xeleenet-plan.md
 * section 3.3 and 3.5).
 *
 * Routes. Two guests may not reach each other at all (a strict NAT on either
 * side), while both reach the host -- which every guest must. A guest that
 * has heard nothing on a link for ROUTE_WAIT_MS after the handshake, or
 * nothing for ROUTE_SILENT_MS on one that was working, while its other links
 * are fresh, marks that link dead (net.link_dead) and tells the host which
 * ports it cannot hear (REL_ROUTE). The host then passes those players' input
 * on to it ('F', acked with 'G'), straight out of its own ring of real input,
 * with their latest checksum report so the desync check still covers the
 * pair, and relays their reliable messages (REL_RELAY: the scene hand-off,
 * chat, the caller's types). No reliable message goes out on a dead link, but
 * input packets still do, so a link that comes back is heard: it revives, and
 * the host stops relaying that pair.
 *
 * "While its other links are fresh" is what tells a dead link from a starved
 * process. Four games booting on one machine stalled whole processes for
 * seconds (tools/net_test_n.py, cold caches), and every link looked silent at
 * once from inside the stalled one.
 *
 * Drops. A guest who leaves -- a BYE, or silence on the host's link -- would
 * otherwise end the session for everyone. Instead the host tells every guest
 * to pass on what it holds of that player's input (REL_DROP); between them the
 * remaining peers hold everything the player ever sent anyone, and the host
 * collects it and passes it on. Once the collected input has stopped growing
 * for DROP_SETTLE_MS and every guest holds all of it, the host announces where
 * it ends and when the match is void (REL_VOID). From the frame after the
 * last one, that port reads as a released pad on every peer; anyone who
 * predicted past it rolls back as for any misprediction, and the match ends
 * as a no contest on the void frame (pc_net_void_port_now, gmvs.c). The host
 * leaving still ends the session: it leads every agreement.
 *
 * None of this runs in a 1v1. */
#define ROUTE_WAIT_MS 8000   /* a guest link never heard this long after the handshake */
#define ROUTE_SILENT_MS 3000 /* a working one silent this long */
#define FRESH_MS 1000        /* "its other links are fresh": heard this recently */
#define ROUTE_BACK_MS 500    /* a dead route heard this recently is back */
#define DROP_SETTLE_MS 500   /* the collected input stopped growing this long */
#define VOID_LEAD 60         /* frames past the last input the match is void on */

static uint8_t s_fwd[PORTS];                  /* peers (bit = peer index) port o's input goes on to */
static int32_t s_fwd_acked[PORTS][MAX_PEERS]; /* what each of them holds of it (tx_lock) */
static uint8_t s_route_mask;                  /* guest: ports it cannot hear */
static bool s_route_told;                     /* the host has that mask */
static uint64_t s_link_t0;                    /* when the handshake finished, 0 before */
static uint8_t s_gone_told;     /* guest: ports it has reported gone to the host */
static uint64_t s_drop_quiet;   /* host: when the collected input last grew */
static int32_t s_drop_mark;     /* host: its mark for that port when it last grew */
static int s_void_port = -1;    /* the player whose leaving voids the match */
static int32_t s_void_frame = -1;

typedef struct VoidMsg {
    uint8_t port;
    int32_t last; /* its last real frame; from the next one it is released */
    int32_t at;   /* the frame the match ends on as a no contest, -1 outside a fight */
} __attribute__((packed)) VoidMsg;

static void exit_if_test_done(void);

static Peer* peer_by_port(int port) {
    for (int i = 0; i < net.npeers; i++) {
        if (s_peer[i].port == port) {
            return &s_peer[i];
        }
    }
    return NULL;
}

static void links_reset(void) {
    memset(s_fwd, 0, sizeof s_fwd);
    for (int o = 0; o < PORTS; o++) {
        s_port_end[o] = -1;
        for (int i = 0; i < MAX_PEERS; i++) {
            s_fwd_acked[o][i] = -1;
        }
    }
    memset(net.link_dead, 0, sizeof net.link_dead);
    s_route_mask = 0;
    s_route_told = true;
    s_link_t0 = 0;
    s_gone = s_gone_told = 0;
    s_drop_quiet = 0;
    s_drop_mark = -1;
    s_void_port = -1;
    s_void_frame = -1;
}

/* Stop using the link to peer i: nothing more goes out on it (game thread). */
static void link_kill(int i, const char* why) {
    SDL_LockMutex(net.tx_lock);
    if (!net.link_dead[i]) {
        net.link_dead[i] = true;
        rel_link_drop(i);
        pc_log_line("net: link to P%d is dead (%s) at frame %d", s_peer[i].port + 1, why,
            net.frame);
    }
    SDL_UnlockMutex(net.tx_lock);
}

/* Pass on what this side holds of each forwarded port's real input, from what
 * each recipient has acked of it (send_inputs, tx_lock held). */
static void send_forwarded(void) {
    for (int o = 0; o < PORTS; o++) {
        if (s_fwd[o] == 0) {
            continue;
        }
        int32_t have = s_port_have[o];
        const Peer* op = peer_by_port(o);
        if (have < 0 || op == NULL) {
            continue;
        }
        for (int i = 0; i < net.npeers; i++) {
            if (!(s_fwd[o] & (1u << i)) || net.link_dead[i]) {
                continue;
            }
            int64_t first = (int64_t)s_fwd_acked[o][i] + 1;
            if (first <= (int64_t)have - RING) {
                first = (int64_t)have - RING + 1;
            }
            first = first < 0 ? 0 : first;
            int64_t count = (int64_t)have - first + 1;
            if (count <= 0) {
                continue;
            }
            count = count > REDUNDANCY ? REDUNDANCY : count;
            Packet pk;
            memset(&pk, 0, sizeof pk);
            pk.first = (int32_t)first;
            pk.newest = (int32_t)(first + count - 1);
            /* The origin's own checksum report, while it is one this
             * recipient can place (it must not name a frame past the pads). */
            bool ck = op->ck_last_frame >= 0 && op->ck_last_frame <= pk.newest;
            pk.ck_frame = ck ? op->ck_last_frame : -1;
            pk.ck = ck ? op->ck_last : 0;
            pk.count = (uint8_t)count;
            for (int32_t f = 0; f < count; f++) {
                pk.pads[f] = s_remote_ring[o][(first + f) & (RING - 1)];
            }
            wire_packet(&pk);
            uint8_t out[HELD_BYTES];
            FwdHdr fh = {hdr('F'), (uint8_t)o};
            memcpy(out, &fh, sizeof fh);
            size_t body = offsetof(Packet, pads) - sizeof(Hdr);
            memcpy(out + sizeof fh, (const uint8_t*)&pk + sizeof(Hdr), body);
            size_t n = sizeof fh + body + pads_encode(pk.pads, (int)count, out + sizeof fh + body);
            tx(i, out, n);
        }
    }
}

static void send_fwd_ack(const Peer* to, int origin, int32_t frame) {
    FwdAck a = {hdr('G'), (uint8_t)origin, (int32_t)htonl((uint32_t)frame)};
    SDL_LockMutex(net.tx_lock);
    tx(peer_index(to), &a, sizeof a);
    SDL_UnlockMutex(net.tx_lock);
}

/* The host's REL_RELAY: a message from a guest some other guest cannot hear. */
void net_relay_out(int peer, uint8_t type, const void* payload, int len) {
    if (net.local != 0 || peer < 0 || peer >= net.npeers || len > REL_MAX - 2) {
        return;
    }
    int o = s_peer[peer].port;
    if (s_fwd[o] == 0) {
        return;
    }
    uint8_t m[REL_MAX];
    m[0] = (uint8_t)o;
    m[1] = type;
    if (len > 0) {
        memcpy(m + 2, payload, (size_t)len);
    }
    for (int i = 0; i < net.npeers; i++) {
        if ((s_fwd[o] & (1u << i)) && i != peer && !net.link_dead[i] &&
            !rel_send_to(i, REL_RELAY, m, len + 2))
        {
            pc_log_line("net: relay of type %02x from P%d to P%d refused (lane full)", type, o + 1,
                s_peer[i].port + 1);
        }
    }
}

/* Host: start collecting a player who left, from everyone. */
static void drop_begin(int port, const char* why) {
    Peer* x = peer_by_port(port);
    if (net.local != 0 || x == NULL || (s_gone & (1u << port))) {
        return;
    }
    s_gone |= (uint8_t)(1u << port);
    link_kill(peer_index(x), why);
    s_drop_quiet = SDL_GetTicksNS();
    s_drop_mark = s_port_have[port];
    uint8_t p = (uint8_t)port;
    pc_net_send_reliable(REL_DROP, &p, 1);
    SDL_LockMutex(net.tx_lock);
    s_fwd[port] = 0;
    for (int i = 0; i < net.npeers; i++) {
        if (!net.link_dead[i]) {
            s_fwd[port] |= (uint8_t)(1u << i);
            s_fwd_acked[port][i] = -1;
        }
    }
    SDL_UnlockMutex(net.tx_lock);
    pc_log_line("net: P%d left (%s) at frame %d; collecting its input (we hold %d)", port + 1,
        why, net.frame, s_port_have[port]);
}

/* Everyone, on the host's REL_VOID (and the host as it sends it): the
 * player's input ends at `last`, and the match is a no contest at `at`. */
static void drop_end(int port, int32_t last, int32_t at) {
    if (s_port_end[port] >= 0) {
        return;
    }
    if (s_port_have[port] < last) {
        pc_log_line("net: P%d's input ends at %d but we hold only %d; leaving netplay", port + 1,
            last, s_port_have[port]);
        s_status = PC_NET_PEER_TIMEOUT;
        s_peer_left = true;
        return;
    }
    static const WirePad released;
    int32_t upto = simulated_upto();
    for (int32_t f = last + 1; f <= upto && f > upto - RING; f++) {
        WirePad* slot = &s_remote_ring[port][f & (RING - 1)];
        if (memcmp(slot, &released, sizeof released) != 0 && (s_rb_frame < 0 || f < s_rb_frame)) {
            s_rb_frame = f;
        }
        *slot = released;
    }
    s_port_end[port] = last;
    s_gone |= (uint8_t)(1u << port);
    SDL_LockMutex(net.tx_lock);
    s_fwd[port] = 0;
    SDL_UnlockMutex(net.tx_lock);
    remote_marks_update();
    s_void_port = at >= 0 ? port : s_void_port;
    s_void_frame = at >= 0 ? at : s_void_frame;
    pc_log_line("net: P%d's input ends at frame %d%s (we are at %d)", port + 1, last,
        at >= 0 ? "; the match is void" : "", net.frame);
    if (at >= 0) {
        pc_log_line("net: match void at frame %d", at);
    }
}

void net_link_rel(int peer, uint8_t type, const void* payload, int len) {
    const uint8_t* b = payload;
    if (peer < 0 || peer >= net.npeers) {
        return;
    }
    bool from_host = peer == net_host_peer();
    switch (type) {
    case REL_ROUTE: /* a guest's ports it cannot hear */
        if (net.local != 0 || len != 1) {
            return;
        }
        SDL_LockMutex(net.tx_lock);
        for (int o = 0; o < PORTS; o++) {
            if (o == net.local || (s_gone & (1u << o))) {
                continue; /* a player who left goes to everyone anyway */
            }
            bool want = (b[0] & (1u << o)) != 0, have = (s_fwd[o] & (1u << peer)) != 0;
            if (want && !have) {
                s_fwd[o] |= (uint8_t)(1u << peer);
                s_fwd_acked[o][peer] = -1;
                pc_log_line("net: P%d cannot hear P%d; relaying", s_peer[peer].port + 1, o + 1);
            } else if (!want && have) {
                s_fwd[o] &= (uint8_t)~(1u << peer);
                pc_log_line("net: P%d hears P%d again; no longer relaying", s_peer[peer].port + 1,
                    o + 1);
            }
        }
        SDL_UnlockMutex(net.tx_lock);
        return;
    case REL_RELAY: { /* the host passing on another guest's message */
        Peer* o = len >= 2 && from_host ? peer_by_port(b[0]) : NULL;
        if (o != NULL && o != &s_peer[peer]) {
            rel_dispatch(peer_index(o), b[1], b + 2, len - 2);
        }
        return;
    }
    case REL_GONE: /* a guest saying a player left */
        if (net.local == 0 && len == 1 && b[0] < PORTS) {
            drop_begin(b[0], "a guest says so");
        }
        return;
    case REL_DROP: { /* the host: pass on what we hold of that player */
        Peer* x = from_host && len == 1 && b[0] < PORTS ? peer_by_port(b[0]) : NULL;
        if (x == NULL || x == &s_peer[peer]) {
            return;
        }
        s_gone |= (uint8_t)(1u << b[0]);
        link_kill(peer_index(x), "the host says it left");
        SDL_LockMutex(net.tx_lock);
        s_fwd[b[0]] = (uint8_t)(1u << peer);
        s_fwd_acked[b[0]][peer] = -1;
        SDL_UnlockMutex(net.tx_lock);
        pc_log_line("net: P%d left; passing on its input (we hold %d)", b[0] + 1,
            s_port_have[b[0]]);
        return;
    }
    case REL_VOID: {
        VoidMsg v;
        if (!from_host || len != (int)sizeof v) {
            return;
        }
        memcpy(&v, payload, sizeof v);
        if (v.port < PORTS && peer_by_port(v.port) != NULL) {
            drop_end(v.port, (int32_t)ntohl((uint32_t)v.last), (int32_t)ntohl((uint32_t)v.at));
        }
        return;
    }
    }
}

/* Routes, drops and their timers: every fresh tick and every turn of an input
 * wait (game thread). */
static void links_poll(void) {
    if (!net.active || net.npeers < 2 || s_slippi_on) {
        return;
    }
    int host = net_host_peer();
    /* Each link's receive clock, under the receive thread's lock. */
    bool heard[MAX_PEERS], left[MAX_PEERS];
    uint64_t last[MAX_PEERS];
    SDL_LockMutex(s_rx_lock);
    for (int i = 0; i < net.npeers; i++) {
        heard[i] = s_peer[i].heard;
        last[i] = s_peer[i].last_rx_ns;
        left[i] = s_peer[i].left;
    }
    SDL_UnlockMutex(s_rx_lock);
    /* After the stamps, never before: one the receive thread wrote between
     * the two reads would be later than `now`, and `now - last` would wrap to
     * centuries of silence (it did: "silent 18446744073.7 s"). */
    uint64_t now = SDL_GetTicksNS();
    if (s_link_t0 == 0 && net.hs == HS_DONE) {
        s_link_t0 = now;
    }
    /* Before the handshake a game may still be booting, and a booting game
     * says nothing on its own (the resend timer repeats an input packet, and
     * there is none before the first frame): silence means nothing yet. The
     * connect and stall waits cover that stretch, as in a 1v1. */
    bool judge = s_link_t0 != 0;
    /* Is there a fresh link other than i? (FRESH_MS) */
    int fresh = 0;
    for (int i = 0; i < net.npeers; i++) {
        fresh += heard[i] && !(s_gone & (1u << s_peer[i].port)) &&
                 now - last[i] < FRESH_MS * 1000000ull;
    }
    for (int i = 0; i < net.npeers; i++) {
        int port = s_peer[i].port;
        bool fresh_i = heard[i] && now - last[i] < FRESH_MS * 1000000ull;
        bool others_fresh = fresh - (fresh_i ? 1 : 0) > 0;
        if (s_gone & (1u << port)) {
            continue;
        }
        if (net.link_dead[i]) {
            /* A dead route that is heard again is back. */
            if (net.local != 0 && heard[i] && now - last[i] < ROUTE_BACK_MS * 1000000ull) {
                SDL_LockMutex(net.tx_lock);
                net.link_dead[i] = false;
                SDL_UnlockMutex(net.tx_lock);
                s_route_mask &= (uint8_t)~(1u << port);
                s_route_told = false;
                pc_log_line("net: link to P%d is back at frame %d", port + 1, net.frame);
            }
            continue;
        }
        if (left[i]) {
            exit_if_test_done(); /* tools/net_test*.py ends a run with a goodbye */
            if (net.local == 0) {
                drop_begin(port, "it said goodbye");
            } else if (!(s_gone_told & (1u << port))) {
                uint8_t p = (uint8_t)port;
                if (rel_send_to(host, REL_GONE, &p, 1)) {
                    s_gone_told |= (uint8_t)(1u << port);
                }
            }
            continue;
        }
        if (net.local == 0) {
            /* The host's link to a guest is the one every route needs: a
             * guest silent on it as long as a 1v1 waits before giving up
             * (the stall timeout, then the resume window), while the other
             * guests are heard, has left. */
            uint64_t limit = (uint64_t)(STALL_TIMEOUT_MS + s_rc_window_ms) * 1000000ull;
            if (judge && heard[i] && now - last[i] > limit && others_fresh) {
                char why[48];
                snprintf(why, sizeof why, "silent %.1f s", (now - last[i]) / 1e9);
                drop_begin(port, why);
            }
            continue;
        }
        if (i == host) {
            continue; /* the host going silent ends the session (wait_remote) */
        }
        bool host_fresh = host >= 0 && heard[host] && now - last[host] < FRESH_MS * 1000000ull;
        bool never = !heard[i] && s_link_t0 != 0 && now - s_link_t0 > ROUTE_WAIT_MS * 1000000ull;
        bool quiet = heard[i] && now - last[i] > ROUTE_SILENT_MS * 1000000ull;
        if (judge && (never || quiet) && host_fresh) {
            char why[48];
            snprintf(why, sizeof why, never ? "never heard" : "silent %.1f s",
                (now - last[i]) / 1e9);
            link_kill(i, why);
            s_route_mask |= (uint8_t)(1u << port);
            s_route_told = false;
        }
    }
    if (!s_route_told && host >= 0) {
        s_route_told = rel_send_to(host, REL_ROUTE, &s_route_mask, 1);
    }
    /* Host: a player's collected input has settled and every guest holds it. */
    if (net.local == 0) {
        for (int port = 0; port < PORTS; port++) {
            if (!(s_gone & (1u << port)) || s_port_end[port] >= 0) {
                continue;
            }
            if (s_port_have[port] != s_drop_mark) {
                s_drop_mark = s_port_have[port];
                s_drop_quiet = now;
            }
            bool all = now - s_drop_quiet > DROP_SETTLE_MS * 1000000ull;
            SDL_LockMutex(net.tx_lock);
            for (int i = 0; i < net.npeers && all; i++) {
                if (!net.link_dead[i] && s_fwd_acked[port][i] < s_port_have[port]) {
                    all = false;
                }
            }
            SDL_UnlockMutex(net.tx_lock);
            if (!all) {
                continue;
            }
            int32_t lastf = s_port_have[port];
            int32_t from = net.frame > lastf ? net.frame : lastf;
            VoidMsg v = {(uint8_t)port, (int32_t)htonl((uint32_t)lastf),
                (int32_t)htonl((uint32_t)(in_fight() ? from + VOID_LEAD : -1))};
            if (pc_net_send_reliable(REL_VOID, &v, sizeof v)) {
                drop_end(port, lastf, in_fight() ? from + VOID_LEAD : -1);
            }
        }
    }
}

int pc_net_void_port_now(void) {
    return net.active && s_void_frame >= 0 && net.tick_frame == s_void_frame && in_fight() ?
               s_void_port :
               -1;
}

static void send_inputs(void) {
    if (s_slippi_on) {
        s_slippi_send_inputs();
        return;
    }
    /* The local sample taken at frame f is the input for frame f + delay. */
    int32_t newest = s_wrote;
    if (newest < 0) {
        return; /* do not send input packets until the first local frame exists */
    }
    atomic_store_explicit(&s_want_resend,
        s_loss_pct >= 2 || jitter_us() >= 4000 || s_rb_depth_recent >= 2, memory_order_relaxed);
    uint64_t now = SDL_GetTicksNS();
    int red = 0;
    SDL_LockMutex(net.tx_lock);
    tx_flush();
    rel_service();
    for (int i = 0; i < net.npeers; i++) {
        Peer* p = &s_peer[i];
        if (s_gone & (1u << p->port)) {
            continue; /* a dead route still gets input, so it is heard if it comes back */
        }
        Packet pk;
        memset(&pk, 0, sizeof pk);
        pk.h = hdr('M');
        /* Everything this peer has not acked yet, oldest first so its
         * contiguous mark can always advance; the cap only bites when acks
         * are starved.
         *
         * Computed in 64 bits. The values are all frame numbers, and while a
         * session is bounded well below the point where a frame number could
         * overflow (see SESSION_MAX_FRAMES), `last_acked + 1` and
         * `newest - first + 1` are the expressions that would do it first,
         * and signed overflow is undefined rather than merely wrong.
         * Widening costs nothing here and removes the class. */
        int64_t newest64 = newest;
        int64_t first = (int64_t)p->last_acked + 1;
        if (first < 0) {
            first = 0;
        }
        if (first <= newest64 - RING) {
            first = newest64 - RING + 1;
        }
        /* Every unacked frame, up to the packet's REDUNDANCY slots. The peer
         * keeps contiguous data only, so a packet moves its mark at most
         * `count` frames past what it had acked a round trip ago: delivery is
         * count frames per round trip, and 60 Hz needs 60 * RTT of them. This
         * used to ship only as many as loss and rollback depth asked for (4
         * on a clean link), which starved every link over ~66 ms of round
         * trip: the remote fell behind, the window filled, the rollbacks
         * deepened, and the depth was the one thing that widened the packet
         * again -- the session held 60 Hz only by rolling back 8 frames deep.
         * GGPO, GGRS and Slippi all send the whole unacked span; loss costs
         * nothing extra because every packet already repeats everything
         * unacked, and a reconnect's refill (a whole ring's gap) goes out at
         * the full width with no special case. */
        int64_t count = newest64 - first + 1;
        if (count > REDUNDANCY) {
            count = REDUNDANCY;
        }
        if (count < 1) {
            count = 0;
            first = newest64;
        }
        red = (int)count > red ? (int)count : red;
        for (int32_t f = 0; f < count; f++) {
            pk.pads[f] = s_local_ring[(first + f) & (RING - 1)];
        }
        pk.count = (uint8_t)count;
        pk.first = (int32_t)first;
        pk.newest = newest;
        pk.ck_frame = confirmed_frame();
        pk.ck = pk.ck_frame >= 0 ? s_ck_ring[pk.ck_frame & (RING - 1)] : 0;
        /* How far this simulation has run past the newest input this peer
         * has given us. The peer compares it with its own for us and only
         * the side further ahead gives a frame back (net_sync.c time_sync). */
        int32_t adv = net.frame - 1 - s_port_have[p->port];
        pk.adv = (int8_t)(adv < -127 ? -127 : adv > 127 ? 127 : adv);
        atomic_store_explicit(&s_adv_pub[i], pk.adv, memory_order_relaxed);
        send_packet(p, &pk);
        p->last_pkt = pk;
        p->last_valid = true;
        p->last_send_ns = now;
    }
    send_forwarded();
    SDL_UnlockMutex(net.tx_lock);
    s_red_target = red;
    if (newest != s_send_frame) {
        s_send_frame = newest;
        s_send_ns = now;
    }
}

static void send_ack(const Peer* p, int32_t frame, uint16_t seq) {
    Ack a = {hdr('A'), seq, frame};
    wire_ack(&a);
    SDL_LockMutex(net.tx_lock);
    tx(peer_index(p), &a, sizeof a);
    SDL_UnlockMutex(net.tx_lock);
}

static void send_bye(int peer, uint8_t reason) {
    Bye b = {hdr('B'), reason};
    tx(peer, &b, sizeof b);
}

/* ---- receive -----------------------------------------------------------
 * Reception runs on a thread of its own (rx_main below), not on the game
 * thread. On the game thread a datagram waited in the socket until the frame
 * loop next drained it, so every freeze of that thread -- a load, a shader
 * compile, a phone descheduling it -- was measured as network latency by
 * both peers: the frozen side acked late, and timed the acks it received
 * late. Phone<->PC on one Wi-Fi network (~20 ms ping) logged ping max
 * 150-450 ms per window, matching the phone's frame freezes exactly, and
 * auto delay and the jitter mean (net_sync.c) consume that number. The
 * frozen side also stopped acking, so its peer counted its own packets as
 * lost and widened their redundancy.
 *
 * So the receive thread does the part that is about the link the moment a
 * datagram lands: drain the socket, authenticate, ack every input packet
 * and time every ack of ours. What is about the game goes through a queue
 * (rx_push) that recv_inputs applies on the game thread: the input rings,
 * s_remote_have, frame advantage, each link's last_acked, the RTT statistics, the
 * reliable lanes and through them the handshake, resume, delay and scene
 * hand-off. A BYE or a protocol mismatch is a sticky flag instead, so a full
 * queue cannot lose it.
 *
 * Measured with tools/net_test.py --delay 10 --load-stall 0.3 (20 ms round
 * trip, one peer's game thread parked 300 ms): the other peer's ping max for
 * that window went from 339 ms to 27 ms, jitter from 7.1 to 0.9 ms. Its
 * steady ping went from 40-47 ms to 23-24 ms too, because the old figure also
 * carried most of a frame of drain delay on each end.
 *
 * The ack carries the link's rx_have, the receive thread's contiguous mark over what
 * it has queued. on_inputs applies the same packets in the same order under
 * the same rule, so the next drain brings s_remote_have to that mark or past
 * it: an ack never claims a frame the game thread will not get, and the peer
 * drops a frame from its redundancy window (which starts at last_acked + 1)
 * only once it is queued here. A packet the full queue refuses is still
 * acked, with the unchanged mark, so the peer's RTT and loss stay honest.
 *
 * A thread rather than tx_timer: the timer polls every 4 ms, which would
 * put up to 4 ms of quantisation back into every sample on each side (the
 * error this removes, only smaller), and SDL runs every timer on one shared
 * thread. A thread parked in poll() wakes when the datagram lands. */

/* One datagram's worth of game-thread work, host order. */
typedef struct RxMsg {
    int peer;   /* the link it came in on */
    int len;    /* message bytes */
    int rtt_us; /* 'A': round trip timed as it landed, -1 for none;
                 * 'M': RX_REORDERED if it arrived behind a later seq */
    union {
        Hdr h;
        Packet pk;
        Ack ack;
        Rel rel;
        RelAck rack;
    } u;
} RxMsg;
/* While this side is frozen the peer keeps sending: its input packet every
 * 16 ms from wait_remote(), every 8 ms once its tx_timer resends as well,
 * and an ack for each resend our own tx_timer makes (16 ms apart, 8 when the
 * link wants resends) -- 120 to 250 datagrams a second. 256 is a second of
 * that, past every freeze the phone sessions logged (<= 450 ms); a 2 s
 * --load-stall on loopback dropped none (rx_full 0). A longer freeze
 * drops the surplus and counts it (rx_full): input packets and reliable
 * messages are resent, and a later ack supersedes a lost one. */
#define RXQ 256
#define RX_REORDERED (-2)
static RxMsg s_rxq[RXQ]; /* under s_rx_lock */
static int s_rxq_head, s_rxq_n;

/* Queue a message for the game thread (receive thread, s_rx_lock held). */
static bool rx_push(const Peer* p, const void* body, int len, int rtt_us) {
    if (s_rxq_n == RXQ) {
        s_rx_full++;
        return false;
    }
    RxMsg* m = &s_rxq[(s_rxq_head + s_rxq_n++) % RXQ];
    m->peer = peer_index(p);
    m->len = len;
    m->rtt_us = rtt_us;
    memcpy(&m->u, body, (size_t)len);
    return true;
}

/* RTP/IPsec-style anti-replay over the per-datagram seq: SEQ_DUP is an exact
 * copy already seen (drop it), SEQ_REORDER is older but not a duplicate
 * (process it, its pads may fill a gap), SEQ_NEW advances the window. */
enum { SEQ_NEW, SEQ_DUP, SEQ_REORDER };
static int seq_check(Peer* p, uint16_t seq) {
    if (!p->seq_init) {
        p->seq_init = true;
        p->seq_top = seq;
        p->seq_bits = 1; /* bit 0 marks the top as received */
        return SEQ_NEW;
    }
    int16_t d = (int16_t)(seq - p->seq_top);
    if (d > 0) {
        p->seq_bits = d >= 64 ? 0 : p->seq_bits << d;
        p->seq_bits |= 1;
        p->seq_top = seq;
        return SEQ_NEW;
    }
    uint32_t back = (uint32_t)(-d);
    if (back >= 64) {
        return SEQ_REORDER; /* too old to remember: process, cannot dedup */
    }
    uint64_t bit = 1ull << back;
    if (p->seq_bits & bit) {
        return SEQ_DUP;
    }
    p->seq_bits |= bit;
    return SEQ_REORDER;
}

/* An input packet off the wire (receive thread, s_rx_lock held): checked,
 * queued for on_inputs and acked. The ack goes out here rather than after
 * the game thread applies the packet because its seq is what the peer times
 * its round trip by; its frame is rx_have, final once the packet is queued
 * (see the top of this section). */
static void rx_input(Peer* p, const Packet* pk, int n, bool reordered, const Peer* via) {
    int32_t frame = atomic_load(&s_frame_pub);
    int count = pk->count > REDUNDANCY ? REDUNDANCY : pk->count;
    /* A peer can be at most window + delays ahead of us; anything else is
     * garbage (a corrupt packet, or a stale process at the peer's address).
     * All arithmetic is in int64 so first/count/newest at INT32 extremes
     * cannot overflow the comparisons. */
    int64_t first = pk->first, newest = pk->newest, ckf = pk->ck_frame;
    int64_t last = first + count - 1;

    /* A keepalive packet sent before local frame 0 exists has count 0 and newest -1. */
    bool is_keepalive = (count == 0 && newest == -1 && first == 0 && ckf == -1);

    if (!is_keepalive && (n < (int)(offsetof(Packet, pads) + (size_t)count * sizeof(WirePad)) ||
                             first < 0 || newest < first || last > newest ||
                             newest > (int64_t)frame + RING / 2 || ckf < -1 || ckf > newest))
    {
        s_rx_malformed++;
        if (!s_warn_bad) {
            s_warn_bad = true;
            pc_log_line("net: dropped malformed input packet (len %d first %d count %d newest %d "
                        "ck_frame %d at frame %d)",
                n, pk->first, count, pk->newest, pk->ck_frame, frame);
        }
        return;
    }
    if (is_keepalive) {
        return;
    }
    /* Only contiguous data is taken; the ack below tells the peer what to
     * resend, so a gap never has to be tracked. */
    int64_t have = p->rx_have;
    if (count > 0 && first <= have + 1 && last > have && last < (int64_t)frame + RING / 2) {
        have = last;
    }
    if (rx_push(p, pk, n, reordered ? RX_REORDERED : -1)) {
        p->rx_have = (int32_t)have;
    }
    if (via != NULL) {
        send_fwd_ack(via, p->port, p->rx_have); /* passed on: ack whoever passed it */
    } else {
        send_ack(p, p->rx_have, pk->seq);
    }
}

/* The game thread's half of a packet rx_input queued. rx_input's rule over
 * the same packets in the same order, against a net.frame never behind the
 * one it checked, so a drain leaves the port's mark at or above rx_have. */
static void on_inputs(Peer* p, const Packet* pk, bool reordered) {
    const int port = p->port;
    if (s_port_end[port] >= 0) {
        return; /* the player left and everyone agreed where its input ends */
    }
    int count = pk->count > REDUNDANCY ? REDUNDANCY : pk->count;
    int64_t last = (int64_t)pk->first + count - 1;
    if (count > 0 && pk->first <= s_port_have[port] + 1 && last > s_port_have[port] &&
        last < (int64_t)net.frame + RING / 2)
    {
        int32_t upto = simulated_upto();
        for (int32_t f = s_port_have[port] + 1; f <= last; f++) {
            WirePad pad = pk->pads[f - pk->first];
            WirePad* slot = &s_remote_ring[port][f & (RING - 1)];
            /* A frame already simulated holds the prediction it ran on. */
            if (f <= upto && memcmp(slot, &pad, sizeof pad) != 0 &&
                (s_rb_frame < 0 || f < s_rb_frame))
            {
                s_rb_frame = f;
            }
            *slot = pad;
        }
        s_port_have[port] = (int32_t)last;
        remote_marks_update();
        s_progress_ns = SDL_GetTicksNS();
    }
    if (pk->ck_frame >= 0) {
        p->rck[pk->ck_frame & (RING - 1)].frame = pk->ck_frame;
        p->rck[pk->ck_frame & (RING - 1)].ck = pk->ck;
        if (pk->ck_frame > p->ck_last_frame) {
            p->ck_last_frame = pk->ck_frame;
            p->ck_last = pk->ck;
        }
    }
    /* The peer's frame advantage against ours. Both are measured the same
     * way against data that actually arrived, so their DIFFERENCE is the
     * phase error with nothing estimated in it -- no shared clock, no round
     * trip, and no assumption that the two directions are equally fast.
     *
     * The estimator this replaces asked "what time was it when the peer
     * sent this", which needs the one-way trip, and the only handles on that
     * are round trips measured through a socket this thread drains once per
     * frame. That quantisation inflates every sample by most of a frame on
     * BOTH peers, so the error is common-mode: it survives the trimmed mean
     * and moves both peers the same way. Measured here, an averaged trip put
     * both peers 7 ms behind each other (nobody corrects, and the pair keeps
     * whatever phase it started with -- 6 rollbacks on one side against 724
     * on the other), and a minimum-filtered trip put both 16 ms ahead of
     * each other (both slow down, and the pair runs at 59.4 Hz).
     *
     * A reordered packet is not sampled: its adv was measured before one
     * that already landed, and pairing it with our current adv would read
     * the reorder as phase error. (tx_timer restamps its resends, so those
     * carry an adv as fresh as any.) */
    if (!reordered) {
        adv_note(peer_index(p), pk->adv, net.frame - 1 - s_port_have[port]);
    }
    if (pk->newest > s_port_newest[port]) {
        s_port_newest[port] = pk->newest;
        remote_marks_update();
    }
}

/* ---- Slippi transport (MELEE_NET_STACK=slippi) ------------------------
 * The adapter delivers every newly-arrived contiguous remote frame, oldest
 * first, exactly the loop on_inputs runs over a legacy packet's pads. This
 * applies one frame to the same ring state, so the rollback loop above is
 * unchanged. It is the ONLY place the Slippi transport writes remote input. */
static void slp_frame_cb(void* user, int remote_slot, int32_t frame,
                         const uint8_t pad[PC_SLIPPI_PAD_DATA_SIZE]) {
    (void)user;
    /* The local peer occupies one port, so in a 1v1 there is a single remote
     * pad per frame; WirePad and the 8 wire bytes are the same layout. */
    WirePad wp;
    memcpy(&wp, pad, sizeof wp);
    /* Slippi's pad bytes are the console's PADStatus: the button half-word is
     * big-endian. (Read as host order, a Dolphin peer's Y reads as D-pad up
     * and Kirby taunts instead of jumping.) */
    wp.button = (uint16_t)__builtin_bswap16(wp.button);
    const int port = net.remote;
    WirePad* slot = &s_remote_ring[port][frame & (RING - 1)];
    int32_t upto = simulated_upto();
    if (frame <= upto && memcmp(slot, &wp, sizeof wp) != 0 &&
        (s_rb_frame < 0 || frame < s_rb_frame)) {
        s_rb_frame = frame;
    }
    *slot = wp;
    s_port_have[port] = frame;
    s_progress_ns = SDL_GetTicksNS();
    /* The remote reported this frame as delivered, so it is the newest we can
     * use; the Slippi "newest" is the highest delivered frame. */
    if (frame > s_port_newest[port]) {
        s_port_newest[port] = frame;
    }
    remote_marks_update();
    /* The peer stamps every pad packet with its newest finalized Slippi
     * checksum. Record it under its own frame so check_desync can compare it
     * with our s_wire_ck_ring entry for that frame (the legacy path does the
     * same from each packet's ck_frame/ck). */
    int32_t ckf = -1;
    uint32_t ckv = 0;
    if (pc_slippi_adapter_remote_checksum(&s_slippi, remote_slot, &ckf, &ckv) && ckf >= 0) {
        int i = ckf & (RING - 1);
        Peer* p = &s_peer[0];
        if (p->rck[i].frame < ckf) { /* never let an older report overwrite */
            p->rck[i].frame = ckf;
            p->rck[i].ck = ckv;
        }
    }
}

/* Monotonic microseconds for the Slippi clock-offset sample. */
static uint64_t s_slippi_now_us(void) {
    return SDL_GetTicksNS() / 1000ull;
}

/* The Slippi direct-session setup (seed + start frame) from the peer. The host
 * sends PC_SLIPPI_MSG_SETUP; the guest applies it and marks the handshake done.
 * Interim: M4's selections flow replaces this for Dolphin interop. */
static void slp_setup_cb(void* user, uint32_t seed, int32_t start_frame) {
    (void)user;
    if (net.hs == HS_DONE) {
        return;
    }
    net.seed = seed;
    net.rng_offset = seed; /* until the selections flow supplies the decider's */
    *HSD_RandSeedPtr = seed;
    net.start_frame = start_frame;
    net.hs = HS_DONE;
    net.hs_host = false;
    net.ck_from = start_frame;
    net.desync_reported = false;
    pc_log_line("net: Slippi setup from host: seed=%u start_frame=%d (frame %d)", seed,
        start_frame, net.tick_frame);
}

/* A tunnelled engine reliable message from the peer (scene hand-off, delay,
 * chat): hand it to the same consumer the custom transport's lane 1 uses. */
static void slp_rel_cb(void* user, uint8_t type, const void* payload, int len) {
    (void)user;
    rel_dispatch(0, type, payload, len);
}

bool pc_net_slippi_send_rel(uint8_t type, const void* payload, int len) {
    if (!s_slippi_on) {
        return false;
    }
    return pc_slippi_net_send_rel(s_slippi.net, type, payload, len);
}

/* One Slippi pump: service ENet and deliver any new contiguous frames through
 * slp_frame_cb. No-op unless the Slippi transport is in force. */
static bool s_slippi_pump(void) {
    if (!s_slippi_on) {
        return false;
    }
    return pc_slippi_adapter_pump(&s_slippi, s_slippi_now_us());
}

/* Newest local frame handed to the Slippi session. The session sends its whole
 * unacked window each call, so we queue every new frame since the last send
 * (normally one) and let it assemble the packet. The engine's delay convention
 * is already inside s_wrote (write_head writes frame + delay), which matches
 * Slippi's handleSendInputs (sample f is sent as frame f + delay). */
static int32_t s_slippi_sent_upto = -1;

static void s_slippi_send_inputs(void) {
    if (s_wrote < 0 || !pc_slippi_adapter_connected(&s_slippi)) {
        return;
    }
    /* The finalized-frame checksum the peer compares (Slippi's formula, not
     * our frame_checksum). Taken from the ring, not sampled live: the value
     * for a frame must be the one computed when that frame was simulated,
     * or prediction makes the two peers label different states with the
     * same frame. */
    int32_t cf = confirmed_frame();
    if (cf >= 0) {
        pc_slippi_adapter_set_checksum(&s_slippi, cf, s_wire_ck_ring[cf & (RING - 1)]);
    }
    uint64_t now = s_slippi_now_us();
    for (int32_t f = s_slippi_sent_upto + 1; f <= s_wrote; f++) {
        WirePad w = s_local_ring[f & (RING - 1)];
        w.button = (uint16_t)__builtin_bswap16(w.button); /* big-endian on the wire */
        if (!pc_slippi_adapter_send_local(&s_slippi, f, &w, now)) {
            break;
        }
        s_slippi_sent_upto = f;
    }
}

/* Service the Slippi transport and fold its liveness into the same state the
 * custom receive path maintains, so wait_remote/diagnostics need no Slippi
 * special case beyond the wait loop. */
static bool s_slippi_recv(void) {
    if (!s_slippi_on) {
        return false;
    }
    bool got = s_slippi_pump();
    int st = pc_slippi_adapter_status(&s_slippi);
    if (st == PC_SLIPPI_NET_DISCONNECTED || st == PC_SLIPPI_NET_FAILED) {
        if (!s_peer_left) {
            s_peer_left = true;
            /* ENet reports a failure to connect separately from a peer that
             * left; both map to the engine's statuses, and the peer's own
             * reason byte (if any) does not change what the game does. */
            s_status = st == PC_SLIPPI_NET_FAILED ? PC_NET_PEER_TIMEOUT : PC_NET_PEER_LEFT;
        }
        return got;
    }
    if (st == PC_SLIPPI_NET_CONNECTED || s_slippi_ever_heard) {
        s_slippi_ever_heard = true;
        s_peer[0].heard = true;
        s_peer[0].last_rx_ns = SDL_GetTicksNS();
        uint32_t ping = 0;
        PcSlippiNetStats stats;
        pc_slippi_net_stats(s_slippi.net, &stats);
        ping = stats.ping_us;
        if (ping != 0) {
            net.ping_us = ping;
        }
    }
    return got;
}

/* Slippi's clock-offset estimate (CalcTimeOffsetUs): positive = we run ahead
 * of the peer. A Slippi session measures phase this way instead of from the
 * frame advantages the custom packets carry. */
int32_t pc_net_slippi_time_offset_us(void) {
    return s_slippi_on ? pc_slippi_adapter_time_offset_us(&s_slippi) : 0;
}

bool pc_net_slippi_active(void) {
    return s_slippi_on;
}

/* Which peer sends the Slippi setup. Set at connect (matchmaking passes the
 * ticket's decider); a MELEE_NET session leaves it unset and the handshake
 * falls back to controller port. */
static bool s_slippi_setup_host;
void pc_net_slippi_set_setup_host(bool host) {
    s_slippi_setup_host = host;
}
bool pc_net_slippi_setup_host(void) {
    return s_slippi_setup_host;
}

bool pc_net_slippi_send_setup(uint32_t seed, int32_t start_frame) {
    if (!s_slippi_on) {
        return false;
    }
    return pc_slippi_adapter_send_setup(&s_slippi, seed, start_frame);
}

int pc_net_slippi_conn_state(void) {
    if (!s_slippi_on) {
        return 2;
    }
    int st = pc_slippi_adapter_status(&s_slippi);
    if (st == PC_SLIPPI_NET_CONNECTED) {
        return 1;
    }
    if (st == PC_SLIPPI_NET_DISCONNECTED || st == PC_SLIPPI_NET_FAILED) {
        return 2;
    }
    return 0;
}

/* The Slippi wait: pump until the needed frame is delivered or the peer goes
 * away/silent. ENet owns reliability and connection state, so the custom
 * transport's reconnect phase has no analogue here; a connected session simply
 * keeps pumping until the frame arrives or the connect timeout elapses. */
static bool s_slippi_wait_remote(int32_t need) {
    uint64_t t0 = SDL_GetTicksNS();
    for (;;) {
        s_slippi_recv();
        if (s_remote_have >= need) {
            return true;
        }
        if (s_peer_left) {
            return false;
        }
        /* Resend our unacked inputs, as the legacy wait does: the peer may be
         * waiting on us, and before the first send actually went out (ENet was
         * still connecting when fresh_tick called send_inputs) this is what
         * puts frame 0 on the wire at all. Two calls: send_inputs queues any
         * NEW local frame, then resend_local rebuilds the whole unacked window
         * (send_inputs alone sends nothing once s_wrote stops advancing, which
         * is exactly when a stalled peer most needs the window). */
        s_slippi_send_inputs();
        pc_slippi_adapter_resend_local(&s_slippi);
        if (pc_slippi_adapter_status(&s_slippi) == PC_SLIPPI_NET_FAILED) {
            s_status = PC_NET_PEER_TIMEOUT;
            return false;
        }
        uint64_t limit = (uint64_t)net.connect_timeout_ms * 1000000ull;
        uint64_t since = s_progress_ns ? s_progress_ns : t0;
        if (SDL_GetTicksNS() - since > limit) {
            s_status = PC_NET_PEER_TIMEOUT;
            return false;
        }
        SDL_Delay(1);
    }
}

/* The round trip of our input packet `seq`, timed as its ack lands (receive
 * thread); -1 unless the ack echoes a seq we actually sent and have not
 * sampled yet. Consumed, so a delayed duplicate ack cannot skew the
 * estimate. The ring is written by both senders under tx_lock. */
static int rtt_sample(Peer* p, uint16_t seq) {
    int rtt = -1;
    SDL_LockMutex(net.tx_lock);
    int slot = seq & 63;
    if (p->rtt_ring[slot].seq == seq && p->rtt_ring[slot].send_ns != 0) {
        uint64_t now = SDL_GetTicksNS();
        if (now > p->rtt_ring[slot].send_ns && now - p->rtt_ring[slot].send_ns < 5000000000ull) {
            rtt = (int)((now - p->rtt_ring[slot].send_ns) / 1000);
        }
        p->rtt_ring[slot].send_ns = 0;
    }
    SDL_UnlockMutex(net.tx_lock);
    return rtt;
}

/* The game thread's half of an ack: rtt is rtt_sample()'s. */
static void on_ack(Peer* p, const Ack* a, int rtt) {
    /* The peer cannot hold a frame we never sent: a bogus ack above s_wrote
     * would leave send_inputs() with first > newest, i.e. shipping no pads at
     * all for the rest of the session (and INT32_MAX overflows first). */
    if (a->frame > p->last_acked && a->frame <= s_wrote) {
        p->last_acked = a->frame;
    }
    if (rtt >= 0) {
        p->ping_us = p->ping_us == 0 ? (uint32_t)rtt : (uint32_t)((p->ping_us * 3 + rtt) / 4);
        /* The session's ping is its slowest link's: that link sets the delay
         * and the time sync's slack. */
        uint32_t worst = 0;
        for (int i = 0; i < net.npeers; i++) {
            worst = s_peer[i].ping_us > worst ? s_peer[i].ping_us : worst;
        }
        net.ping_us = worst;
        s_ping_sum += (uint64_t)rtt;
        s_ping_n++;
        if (s_rtt_min == 0 || (uint32_t)rtt < s_rtt_min) {
            s_rtt_min = (uint32_t)rtt;
        }
        if ((uint32_t)rtt > s_rtt_max) {
            s_rtt_max = (uint32_t)rtt;
        }
        jitter_note((uint32_t)rtt);
    }
}

/* One datagram from the peer, header already in host order and checked
 * (receive thread, s_rx_lock held). */
static void rx_dispatch(Peer* p, void* buf, int n) {
    union {
        Hdr h;
        Packet pk;
        Ack ack;
        Rel rel;
        RelAck rack;
        Bye bye;
    }* u = buf;
    switch (u->h.magic) {
    case 'M': {
        /* Decoded out of the datagram into its host-order struct: the pads
         * are delta-coded on the wire (pads_decode), so the body is no
         * longer the struct's image. */
        Packet pk;
        if (n < (int)offsetof(Packet, pads)) {
            return;
        }
        memcpy(&pk, buf, offsetof(Packet, pads));
        if (pk.count > REDUNDANCY || !pads_decode((const uint8_t*)buf + offsetof(Packet, pads),
                                         n - (int)offsetof(Packet, pads), pk.count, pk.pads))
        {
            return;
        }
        wire_packet(&pk);
        n = (int)packet_len(&pk);
        switch (seq_check(p, pk.seq)) {
        case SEQ_DUP:
            /* An exact copy means the peer has not seen an ack for it. The
             * original was processed, but the ack itself may be what went
             * missing, and dropping the duplicate silently leaves the peer
             * resending until the redundancy in a later packet happens to
             * cover the gap. Re-ack it: the ack carries our contiguous mark,
             * so it is correct whenever it arrives and costs one datagram. */
            s_rx_dups++;
            send_ack(p, p->rx_have, pk.seq);
            break;
        case SEQ_REORDER:
            s_rx_reorders++; /* older but new: still process, its pads may fill a gap */
            rx_input(p, &pk, n, true, NULL);
            break;
        default:
            rx_input(p, &pk, n, false, NULL);
            break;
        }
        break;
    }
    case 'F': {
        /* Another player's input, passed on by this peer: decoded as its
         * own 'M' and applied to that player's port, acked back here. */
        const FwdHdr* fh = buf;
        Peer* o = peer_by_port(fh->origin);
        size_t body = offsetof(Packet, pads) - sizeof(Hdr);
        if (o == NULL || o == p || n < (int)(sizeof *fh + body)) {
            return;
        }
        Packet pk;
        memcpy(&pk.h, &u->h, sizeof pk.h);
        pk.h.magic = 'M';
        pk.h.player = (uint8_t)o->port;
        memcpy((uint8_t*)&pk + sizeof(Hdr), (const uint8_t*)buf + sizeof *fh, body);
        int pads_at = (int)(sizeof *fh + body);
        if (pk.count > REDUNDANCY ||
            !pads_decode((const uint8_t*)buf + pads_at, n - pads_at, pk.count, pk.pads))
        {
            return;
        }
        wire_packet(&pk);
        rx_input(o, &pk, (int)packet_len(&pk), true, p);
        break;
    }
    case 'G': {
        const FwdAck* g = buf;
        int32_t f = (int32_t)ntohl((uint32_t)g->frame);
        if (n != (int)sizeof *g || g->origin >= PORTS) {
            return;
        }
        SDL_LockMutex(net.tx_lock);
        int32_t* acked = &s_fwd_acked[g->origin][peer_index(p)];
        if (f > *acked && f <= s_port_have[g->origin]) {
            *acked = f;
        }
        SDL_UnlockMutex(net.tx_lock);
        break;
    }
    case 'A':
        if (n != (int)sizeof(Ack)) {
            return;
        }
        wire_ack(&u->ack);
        s_rx_acks++;
        rx_push(p, &u->ack, n, rtt_sample(p, u->ack.seq));
        break;
    case 'R':
        if (n < (int)offsetof(Rel, payload)) {
            return;
        }
        wire_rel(&u->rel);
        rx_push(p, &u->rel, n, -1);
        break;
    case 'K':
        if (n != (int)sizeof(RelAck)) {
            return;
        }
        rx_push(p, &u->rack, n, -1);
        break;
    case 'B':
        /* Only a peer we have already heard from can end the session. Before
         * pinning, the source address is not a usable filter (a dual-stack
         * peer answers from either family), so without this any host that
         * knows the published port could kill a connecting session with one
         * forged 8-byte datagram. */
        if (n != (int)sizeof(Bye) || s_rx_left || !p->heard) {
            return;
        }
        /* A guest leaving a session of three or more is a drop, not the end
         * of it (links_poll); only the host leaving ends it for everyone. */
        if (net.npeers > 1 && p->port != 0) {
            if (!p->left) {
                p->left = true;
                pc_log_line("net: P%d said goodbye (reason %d) at frame %d", p->port + 1,
                    u->bye.reason, atomic_load(&s_frame_pub));
            }
            break;
        }
        s_rx_left = true;
        s_rx_why = u->bye.reason <= PC_NET_PEER_RESUME ? u->bye.reason : PC_NET_PEER_LEFT;
        pc_log_line("net: peer left (reason %d) at frame %d", s_rx_why, atomic_load(&s_frame_pub));
        break;
    default:
        return;
    }
    p->heard = true;
    p->last_rx_ns = SDL_GetTicksNS();
}

/* Reject unknown/truncated datagrams before they can select a peer or fail
 * compatibility. Body fields remain in wire order here. */
static bool packet_shape(const void* buf, int n) {
    const Hdr* h = buf;
    switch (h->magic) {
    case 'M': {
        const Packet* p = buf;
        int body = n - (int)offsetof(Packet, pads);
        return body >= 0 && p->count <= REDUNDANCY &&
               pads_wire_len((const uint8_t*)buf + offsetof(Packet, pads), body, p->count) == body;
    }
    case 'A':
        return n == (int)sizeof(Ack);
    case 'R': {
        const Rel* r = buf;
        return n >= (int)offsetof(Rel, payload) && ntohs(r->len) <= REL_MAX &&
               n == (int)(offsetof(Rel, payload) + ntohs(r->len));
    }
    case 'K':
        return n == (int)sizeof(RelAck);
    case 'F': {
        int head = (int)(sizeof(FwdHdr) + offsetof(Packet, pads) - sizeof(Hdr));
        const Packet* q = (const Packet*)((const uint8_t*)buf + sizeof(FwdHdr) - sizeof(Hdr));
        return n >= head && q->count <= REDUNDANCY &&
               pads_wire_len((const uint8_t*)buf + head, n - head, q->count) == n - head;
    }
    case 'G':
        return n == (int)sizeof(FwdAck);
    case 'B':
        return n == (int)sizeof(Bye);
    default:
        return false;
    }
}

/* Hardware output is deliberately outside game snapshots. Compare against
 * the last command actually emitted, not the interpreter's rewound status. */
void pc_net_rumble_command(unsigned port, unsigned command) {
    static int motor[4] = {-1, -1, -1, -1};
    if (port >= 4 || net.resim || motor[port] == (int)command)
        return;
    pc_pad_rumble_command(port, command);
    motor[port] = (int)command;
}

static PcNetDatagramHandler s_datagram_handler;
/* The handler runs on the receive thread, inside s_rx_lock; taking the lock
 * here means that once this returns no call into the old one is running. */
void pc_net_set_datagram_handler(PcNetDatagramHandler handler) {
    SDL_LockMutex(s_rx_lock);
    s_datagram_handler = handler;
    SDL_UnlockMutex(s_rx_lock);
}

bool pc_net_send_datagram(const void* data, size_t size, uint32_t address, uint16_t port) {
    if (!net.tx_lock || !data || size > 512)
        return false;
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = address;
    to.sin_port = htons(port);
    SDL_LockMutex(net.tx_lock);
    int n = net.active ? (int)sendto(net.sock, (const char*)data, (int)size, 0,
                             (struct sockaddr*)&to, sizeof to) :
                         -1;
    SDL_UnlockMutex(net.tx_lock);
    return n == (int)size;
}

/* One call's worth of datagrams (rx_pump) or of queued messages
 * (recv_inputs). The peer sends an input packet per frame, a mid-frame
 * resend, an ack per packet received and the odd reliable message: a couple
 * of dozen per frame at the very worst. Without a cap a drain is a livelock
 * -- anything that fills it faster than the loop empties it parks the
 * draining thread there for as long as it keeps arriving, and on the game
 * thread that is the frame loop and wait_remote(). The remainder stays
 * queued for the next call. */
#define RX_BUDGET 128

/* The link a datagram belongs to: once a peer has been heard, its address
 * picks it; before that, the player the header names (still in wire order,
 * but it is one byte). NULL when neither matches. */
static Peer* rx_peer(const Hdr* h, const struct sockaddr_storage* from) {
    for (int i = 0; i < net.npeers; i++) {
        if (s_peer[i].heard && addr_eq(from, &s_peer[i].addr)) {
            return &s_peer[i];
        }
    }
    for (int i = 0; i < net.npeers; i++) {
        if (s_peer[i].port == h->player) {
            return &s_peer[i];
        }
    }
    return NULL;
}

/* One datagram off the socket (receive thread, s_rx_lock held). It is
 * dropped, with one log line per session and reason, unless it comes from
 * a peer's address, carries our protocol version and session id (the guest
 * learns the id from the host's first packet) and names that peer's player. */
static void rx_datagram(void* buf, int n, const struct sockaddr_storage* from, socklen_t from_len) {
    Hdr* h = buf;
    if (s_datagram_handler && from->ss_family == AF_INET) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)from;
        if (s_datagram_handler(buf, (size_t)n, a->sin_addr.s_addr, ntohs(a->sin_port)))
            return;
    }
    s_rx_pkts++;
    if (n < (int)(sizeof(Hdr) + NET_MAC_LEN)) {
        s_rx_malformed++;
        return;
    }
    /* Everything downstream measures the message, not the datagram: the
     * tag is checked here and then left behind, so every length test and
     * every body parser below stays the one it was. */
    int body = n - NET_MAC_LEN;
    if (!packet_shape(buf, body)) {
        s_rx_malformed++;
        return;
    }
    Peer* p = rx_peer(h, from);
    if (p == NULL) {
        s_rx_bad_player++;
        return;
    }
    const int pi = peer_index(p);
    /* The authentication gate, ahead of every other check: a datagram
     * that cannot prove it belongs to this session must not pin the peer
     * address, teach us the session id, fail our protocol version or
     * reach rx_dispatch.
     *
     * A handshake-derived key does not exist until the handshake, and
     * the two peers get it one leg apart -- the guest when it accepts
     * RULES, the host when it accepts the READY that answers it. Until a
     * datagram has actually verified, one that does not is taken as a
     * peer that has not keyed yet rather than as an attack; from the
     * first tag that checks out, nothing unauthenticated is accepted
     * again for the rest of the session. That upgrade is one round trip
     * wide and it is never given back, so the window is the bootstrap
     * one that already existed rather than a downgrade an attacker can
     * reopen.
     *
     * A key pinned from MELEE_NET_KEY gets no such grace: both peers
     * hold it from connect, so there is no leg to wait out and the first
     * untagged datagram is already a forgery. Without this the knob
     * would authenticate nothing at all -- an attacker who never sends a
     * valid tag would simply keep the grace open forever.
     *
     * The key is read under tx_lock: the handshake installs it on the
     * game thread while this runs on the receive thread. */
    SDL_LockMutex(net.tx_lock);
    bool keyed = net_key_ready(pi);
    bool mac_ok = keyed && net_mac_ok(pi, buf, (size_t)body);
    bool pinned = net_key_pinned();
    SDL_UnlockMutex(net.tx_lock);
    if (mac_ok) {
        p->mac_seen = true;
    } else if (p->mac_seen || pinned) {
        s_rx_bad_mac++;
        if (!s_warn_mac) {
            s_warn_mac = true;
            pc_log_line("net: dropped a datagram with a bad MAC (magic %c len %d) at frame %d",
                h->magic >= 32 && h->magic < 127 ? h->magic : '?', n, atomic_load(&s_frame_pub));
        }
        return;
    } else if (keyed && ++p->mac_quiet == MAC_QUIET_MAX) {
        /* We hold a key and two seconds of the peer's traffic has gone by
         * without one tag of it verifying. That is not the handshake leg
         * the upgrade above allows for; it is two peers holding different
         * keys -- a stale MELEE_NET_KEY on one side is how -- and the
         * session is running unauthenticated. Said once, out loud,
         * because the alternative is a session that looks protected in
         * the log and is not. */
        pc_log_line("net: %d datagrams from the peer and none of them authenticate; this "
                    "session is UNAUTHENTICATED (the two sides hold different keys)",
            MAC_QUIET_MAX);
    }
    wire_hdr(h);
    bool same_addr = addr_eq(from, &p->addr);
    /* Once pinned, unrelated traffic cannot change any session state.
     *
     * Before pinning, the address is NOT a usable filter: the two sides
     * pick their own for each other, and a dual-stack LAN peer routinely
     * answers from a different family than the one the lobby handed us
     * (measured PC<->tablet: we dialled its IPv6 link-local, it answered
     * from 192.168.1.109). Requiring a match there breaks the handshake
     * outright. What IS gated on the address is the one datagram that
     * needs no session state to do damage -- see the BYE in rx_dispatch:
     * before pinning, a forged one from anywhere would otherwise end the
     * session on its own. */
    if (p->heard && !same_addr) {
        s_rx_bad_src++;
        if (!s_warn_src) {
            char got[80] = "?", want[80] = "?";
            net_addr_text((const struct sockaddr*)from, got, sizeof got);
            net_addr_text((const struct sockaddr*)&p->addr, want, sizeof want);
            s_warn_src = true;
            pc_log_line("net: dropped a datagram from %s (the peer is %s)", got, want);
        }
        return;
    }
    if (h->player != p->port) {
        s_rx_bad_player++;
        return;
    }
    /* Learning the session id is how the guest finds the host without the
     * lobby having told it one -- and it is also the one piece of session
     * state a stranger could set with a single well-shaped datagram,
     * because the MAC gate above is a grace window while no key exists.
     * A 16-byte RelAck carrying someone else's session id used to win that
     * race, after which every genuine host datagram failed the session
     * check for the rest of the session (learn_session requires
     * net.session == 0, so there is no second chance) and the match died
     * on the handshake timeout. Only the message that carries the session
     * in the first place -- a well-formed RULES -- may establish it.
     * "Well-formed" here means the Rel frame declares a full Rules
     * payload: the genuine host always sends sizeof(Rules) (on_rules
     * drops anything else), so a one-packet forgery has to shape a
     * payload that also survives the guest's validation, and the
     * session is still only learned -- no address pin, no key. */
    bool is_rules;
    {
        /* packet_shape has already proved this is a 'R' whose declared
         * length matches the datagram, so the Rel view is in bounds and
         * r->len can be trusted here. */
        const Rel* rel = buf;
        is_rules = h->magic == 'R' && rel->type == REL_RULES && ntohs(rel->len) == sizeof(Rules);
    }
    bool learn_session =
        net.session == 0 && net.local != 0 && p->port == 0 && h->session != 0 && is_rules;
    bool guest_preamble = !p->heard && net.local == 0 && h->session == 0;
    if (h->session != net.session && !learn_session && !guest_preamble) {
        s_rx_bad_sess++;
        if (!s_warn_sess) {
            s_warn_sess = true;
            pc_log_line("net: dropped a datagram for session %08x player %u (ours %08x, peer %d)",
                h->session, h->player, net.session, p->port);
        }
        return;
    }
    if (h->version != WIRE_VERSION) {
        if (same_addr && !s_rx_left) {
            s_rx_left = true;
            s_rx_why = PC_NET_PEER_INCOMPATIBLE;
            pc_log_line("net: peer speaks protocol %u, we speak %u", h->version, WIRE_VERSION);
        }
        return;
    }
    if (learn_session) {
        SDL_LockMutex(net.tx_lock);
        net.session = h->session;
        SDL_UnlockMutex(net.tx_lock);
    }
    if (!p->heard && !same_addr) {
        char got[80] = "?", want[80] = "?";
        net_addr_text((const struct sockaddr*)from, got, sizeof got);
        net_addr_text((const struct sockaddr*)&p->addr, want, sizeof want);
        pc_log_line("net: peer %d answers from %s, not %s; following it", pi, got, want);
        SDL_LockMutex(net.tx_lock);
        memcpy(&p->addr, from, sizeof p->addr);
        p->addr_len = from_len;
        SDL_UnlockMutex(net.tx_lock);
    }
    if (net.sim_rx_delay_ns == 0) {
        rx_dispatch(p, buf, body);
    } else {
        /* A full simulator queue is one more loss. */
        int slot = held_put(s_rx_held, buf, (size_t)body, SDL_GetTicksNS() + net.sim_rx_delay_ns);
        if (slot >= 0) {
            s_rx_held[slot].peer = (uint8_t)pi;
        }
    }
}

/* Drain the socket through the gate, then release what the link simulator
 * held back. Runs on the receive thread, or on the game thread when there
 * is none. False on a hard socket error (sock_err_note logged it). */
_Static_assert(HELD_BYTES + NET_MAC_LEN <= 512, "a full input packet must fit rx_pump's buffer");
static bool rx_pump(void) {
    for (int budget = RX_BUDGET; budget > 0; budget--) {
        union {
            Hdr h;
            uint8_t raw[512];
        } u;
        struct sockaddr_storage from;
        socklen_t from_len = sizeof from;
        int n = (int)recvfrom(net.sock, (char*)&u, sizeof u, 0, (struct sockaddr*)&from, &from_len);
        if (n < 0) {
            int e = sock_last_err();
            if (sock_would_block(e)) {
                break; /* nothing more queued */
            }
            if (sock_transient(e)) {
                continue; /* EINTR / ICMP unreachable: more may still be queued */
            }
            sock_err_note("recvfrom", e);
            return false;
        }
        if (n == 0) {
            continue; /* empty datagram: nothing to parse */
        }
        SDL_LockMutex(s_rx_lock);
        rx_datagram(&u, n, &from, from_len);
        SDL_UnlockMutex(s_rx_lock);
    }
    if (net.sim_rx_delay_ns != 0) {
        SDL_LockMutex(s_rx_lock);
        uint64_t now = SDL_GetTicksNS();
        for (Held* h; (h = held_due(s_rx_held, now)) != NULL;) {
            h->release_ns = 0;
            rx_dispatch(&s_peer[h->peer], h->buf, h->len);
        }
        SDL_UnlockMutex(s_rx_lock);
    }
    return true;
}

/* The longest the receive thread waits between looks at s_rx_run: all it
 * bounds is how long pc_net_disconnect() waits for the thread to notice. */
#define RX_WAIT_MS 5

static int SDLCALL rx_main(void* ud) {
    (void)ud;
    /* The simulator's receive hold is released from here too, and a 1 ms
     * turn keeps its delay within a millisecond of the knob. */
    int ms = net.sim_rx_delay_ns != 0 ? 1 : RX_WAIT_MS;
    while (atomic_load(&s_rx_run)) {
        sock_wait(net.sock, ms);
        if (!rx_pump()) {
            SDL_DelayNS(RX_WAIT_MS * 1000000ull); /* a hard error is logged, not spun on */
        }
    }
    return 0;
}

/* When the peer was last heard from, 0 before its first datagram: the
 * receive thread's clock, read under its lock (game thread). */
static uint64_t rx_heard_ns(void) {
    SDL_LockMutex(s_rx_lock);
    uint64_t t = 0;
    if (net.npeers == 1) {
        t = s_peer[0].heard ? s_peer[0].last_rx_ns : 0;
    } else if (net.local != 0) {
        /* A guest's session lives on its link to the host. Another guest
         * going quiet is a route (links_poll), not the end of the session. */
        int h = net_host_peer();
        t = h >= 0 && s_peer[h].heard ? s_peer[h].last_rx_ns : 0;
    } else {
        /* The host's, on its freshest guest: one guest going quiet is a drop
         * (links_poll), and only every guest going quiet ends the session.
         * Taking the quietest instead opened the resume window under a
         * dropped guest and timed the session out at the very moment the
         * drop was declared. */
        for (int i = 0; i < net.npeers; i++) {
            uint64_t heard = s_peer[i].heard && !net.link_dead[i] ? s_peer[i].last_rx_ns : 0;
            t = heard > t ? heard : t;
        }
    }
    SDL_UnlockMutex(s_rx_lock);
    return t;
}

/* Apply what the receive thread queued, in arrival order (game thread): the
 * frame loop calls this once per fresh tick and wait_remote() once a turn. */
void recv_inputs(void) {
    if (s_slippi_on) {
        /* The legacy path publishes the frame for the watchdog and the tx
         * timer before draining; keep that the same under Slippi. */
        atomic_store(&s_frame_pub, net.frame);
        s_slippi_recv();
        return;
    }
    atomic_store(&s_frame_pub, net.frame);
    if (s_rx_thread == NULL) {
        rx_pump();
    }
    for (int budget = RX_BUDGET; budget > 0; budget--) {
        RxMsg m;
        SDL_LockMutex(s_rx_lock);
        bool got = s_rxq_n > 0;
        if (got) {
            m = s_rxq[s_rxq_head];
            s_rxq_head = (s_rxq_head + 1) % RXQ;
            s_rxq_n--;
        }
        SDL_UnlockMutex(s_rx_lock);
        if (!got) {
            break;
        }
        Peer* p = &s_peer[m.peer];
        switch (m.u.h.magic) {
        case 'M':
            on_inputs(p, &m.u.pk, m.rtt_us == RX_REORDERED);
            break;
        case 'A':
            on_ack(p, &m.u.ack, m.rtt_us);
            break;
        case 'R':
            on_rel(m.peer, &m.u.rel, m.len);
            break;
        default: /* 'K' */
            on_rel_ack(m.peer, &m.u.rack);
            break;
        }
    }
    SDL_LockMutex(s_rx_lock);
    bool left = s_rx_left;
    int why = s_rx_why;
    SDL_UnlockMutex(s_rx_lock);
    if (left && !s_peer_left) {
        s_peer_left = true;
        s_status = why;
    }
}

/* Frames the last 16 rollbacks restored to, newest last: a desync with an
 * uncorrected misprediction in it has no rollback to the frame that went
 * wrong, and that is otherwise invisible. */
static int32_t s_rb_recent[16];
static unsigned s_rb_recent_n;

/* On a desync: what the input rings and the checksum ring finally held
 * around the frame, after every rollback. The two peers' logs together are
 * the only thing that separates "we simulated different inputs" (a
 * misprediction nobody corrected, a delay mismatch, a lost packet taken as
 * contiguous) from "the same inputs built different state" (an impure
 * tick): the state dump beside it is the first pass only. */
static uint8_t s_sim_n[RING]; /* times each frame index was simulated */

static void dump_rings_around(int32_t f) {
    for (int32_t i = f - 16; i <= f + 1; i++) {
        if (i < 0 || i > net.frame || i <= net.frame - RING) {
            continue;
        }
        const WirePad* m = &s_local_ring[i & (RING - 1)];
        char theirs[PORTS * 24] = "";
        size_t n = 0;
        for (int p = 0; p < PORTS && n < sizeof theirs; p++) {
            if (s_remote_mask & (1u << p)) {
                const WirePad* t = &s_remote_ring[p][i & (RING - 1)];
                int w = snprintf(theirs + n, sizeof theirs - n, " p%d %04x/%d,%d%s", p + 1,
                    t->button, t->stickX, t->stickY, i > s_port_have[p] ? "?" : "");
                n += w > 0 ? (size_t)w : 0;
            }
        }
        pc_log_line("net: ring f%d p%d %04x/%d,%d%s ck %08x x%u%s", i, net.local + 1, m->button,
            m->stickX, m->stickY, theirs, s_ck_ring[i & (RING - 1)], s_sim_n[i & (RING - 1)],
            i > s_remote_have ? " predicted" : "");
    }
    /* Sized for the worst case (16 frames of up to 10 digits, SESSION_MAX_FRAMES
     * is 1e8) and still guarded: snprintf returns the length it WOULD have
     * written, so accumulating its return value past the end makes the next
     * size argument wrap and the next write land outside the buffer. */
    char rb[16 * 11 + 1];
    int n = 0;
    for (unsigned i = s_rb_recent_n > 16 ? s_rb_recent_n - 16 : 0; i < s_rb_recent_n; i++) {
        int w = snprintf(rb + n, sizeof rb - (size_t)n, "%d ", s_rb_recent[i % 16]);
        if (w < 0 || w >= (int)(sizeof rb - (size_t)n)) {
            n = (int)sizeof rb - 1;
            break;
        }
        n += w;
    }
    rb[n > 0 ? n - 1 : 0] = '\0';
    pc_log_line("net: ring have %d newest %d wrote %d delay %d barrier %d rb_frame %d resim %d, "
                "last rollbacks %s",
        s_remote_have, s_remote_newest, s_wrote, net.delay, net.rb_barrier, s_rb_frame, net.resim,
        rb);
}

/* Every frame both peers have confirmed and the peer reported a checksum
 * for, oldest first, each once. A frame the peer never reported is left for
 * the next call until a later one is compared or it ages out of the rings. */
static void check_desync(void) {
    if (net.desync_reported || net.hs == HS_PENDING) {
        return;
    }
    int32_t upto = confirmed_frame();
    if (s_game_over && upto > s_end_frame) {
        upto = s_end_frame; /* the GAME! frames after it ran on predictions */
    }
    for (int pi = 0; pi < net.npeers; pi++) {
    Peer* p = &s_peer[pi];
    int32_t from = p->ck_checked + 1 > net.ck_from ? p->ck_checked + 1 : net.ck_from;
    if (from <= net.frame - RING) {
        from = net.frame - RING + 1; /* aged out of s_ck_ring */
    }
    for (int32_t f = from; f <= upto; f++) {
        int i = f & (RING - 1);
        if (p->rck[i].frame != f) {
            continue;
        }
        p->ck_checked = f;
        /* Under Slippi the wire carries Slippi's checksum formula (Dolphin's),
         * not our FNV frame_checksum, so compare that ring instead; using
         * s_ck_ring here would flag every frame. The Slippi hard check has a
         * +/-1 tolerance on the float-sum half, exactly as the ASM does. */
        bool bad;
        uint32_t local;
        if (pc_net_slippi_active()) {
            local = s_wire_ck_ring[i];
            bad = pc_slippi_checksum_is_hard_desync(local, p->rck[i].ck);
            /* One-shot proof the gate is live and comparing real values, not
             * two zeros (an all-absent checksum set is also 0, and frame 0
             * before the fight is exactly that). Fire on the first frame the
             * fight makes both nonzero. */
            if (!s_slippi_ck_checked_once && (local != 0 || p->rck[i].ck != 0)) {
                s_slippi_ck_checked_once = true;
                pc_log_line(
                    "net: first Slippi desync check at frame %d (local %08x remote %08x)", f, local,
                    p->rck[i].ck);
            }
        } else {
            local = s_ck_ring[i];
            bad = local != p->rck[i].ck;
        }
        if (bad) {
            net.desync_reported = true;
            pc_log_line("net: DESYNC at frame %d with peer %d (P%d) (local %08x remote %08x)", f,
                pi, p->port + 1, local, p->rck[i].ck);
            dump_rings_around(f);
            dump_states_around(f);
            return;
        }
    }
    }
}

/* ---- resume after an interruption --------------------------------------
 * A silence longer than STALL_TIMEOUT_MS used to end the session outright,
 * so a two-second Wi-Fi hiccup killed a match both peers could have
 * finished: the game thread is merely parked in wait_remote(), every byte
 * of game state is intact and both input rings still hold their last RING
 * frames. The stall timeout now opens a reconnect phase instead. The
 * socket, the 4 ms transmit timer and the 16 ms resends of the wait loop
 * stay exactly as they are, so the peer keeps being probed; on top of that
 * each side that opens a phase states what it holds in exactly one reliable
 * REL_RESUME (a statement, never answered: see net_resume_rel). Both sides
 * refill the gap through the ordinary input path (send_inputs' unacked
 * window, which always carries up to REDUNDANCY frames) and the wait ends
 * at the frame it was waiting for, the frames that were predicted before
 * the interruption being rolled back as usual.
 *
 * The rings bound what can be resumed and both sides' rings are frozen
 * during the phase: whoever is not parked can only run WINDOW + delay
 * frames past the other before it parks too, so one check at exchange time
 * cannot go stale.
 *
 * None of this runs in the common case and the frame loop grows no branch
 * for it: the phase is entered from the stall path, the refill rides on the
 * unacked window send_inputs already sends, and a refused exchange is
 * noticed by the very next wait (which is at most WINDOW frames away, since
 * a parked peer stops producing input).
 *
 * MELEE_NET_RECONNECT_MS bounds the phase. The default is RECONNECT_MS: a
 * Wi-Fi roam or a hotspot handover completes well inside it, and the whole
 * outage (STALL_TIMEOUT_MS to notice plus this window) stays near 6 s, which
 * is about as long as a player waits before quitting anyway. Both were 7 s
 * and 15 s until faf888914 shortened them, trading a longer outage tolerance
 * for much faster "the peer is gone" detection; 0 disables resume, i.e. the
 * hard disconnect this file did before. */

/* Resume payload in wire order (big-endian, like every wire field). The
 * codec's be32() is private to net_wire.c and every field here is 32 bits,
 * so the image swaps as one array; that is its own inverse, so this serves
 * both send and receive. */
static void wire_resume(Resume* r) {
    uint8_t* p = (uint8_t*)r;
    for (size_t i = 0; i < sizeof *r; i += 4) {
        uint8_t b0 = p[i], b1 = p[i + 1];
        p[i] = p[i + 3];
        p[i + 1] = p[i + 2];
        p[i + 2] = b1;
        p[i + 3] = b0;
    }
}

/* What we hold, so the peer can decide whether the gap is coverable. Queued
 * once per phase: the reliable channel resends it every 250 ms until its ack
 * arrives, so a lossy link needs no help here. */
static void resume_send(void) {
    /* Each link gets its own numbers: what we hold of that peer's input. */
    static bool sent[MAX_PEERS];
    if (!s_rc_sent) {
        memset(sent, 0, sizeof sent);
    }
    bool all = true;
    for (int i = 0; i < net.npeers; i++) {
        if (!sent[i]) {
            Resume r = {net.session, net.seed, s_wrote, s_port_have[s_peer[i].port], net.frame};
            wire_resume(&r);
            sent[i] = rel_send_to(i, REL_RESUME, &r, sizeof r);
        }
        all = all && sent[i];
    }
    s_rc_sent = all;
}

/* Refuse to resume: end the session with a status of its own rather than
 * run on with a gap neither ring can fill. The BYE pc_net_disconnect()
 * sends carries the same status, so the other side reports it too. */
static void resume_fail(void) {
    s_rc = RSM_FAILED;
    s_status = PC_NET_PEER_RESUME;
}

/* Only an established session can be resumed, and waiting on one that is not
 * actively harms the caller: the lobby reports a failure from pc_lan_poll(),
 * which runs on the very thread parked in wait_remote(), so holding the
 * phase open through a handshake that will never finish turns a 7 s "connect
 * failed" into a 22 s hang (tools/net_lan_test.py host_dies, where the host
 * is killed while the guest is still connecting at frame 3). Before the
 * handshake there is also nothing to resume: no agreed seed, no agreed start
 * frame, and rings holding a couple of menu frames.
 *
 * HS_DONE is "established" everywhere now (net_handshake.c's hs_done() pins
 * seed, start_frame and ck_from); HS_PENDING, HS_FAILED and HS_IDLE all mean
 * the parameters are still unagreed, whether or not the guest has adopted
 * the host's session id. This used to need a grace period, because a direct
 * MELEE_NET session ran no handshake and stayed HS_IDLE for its whole life,
 * so idleness could not be read as "not yet"; it runs the same handshake as
 * the lobby now (handshake_direct), so the state means one thing again. */
static bool session_established(void) {
    return net.hs == HS_DONE;
}

/* The peer's RESUME (net_reliable.c dispatches it here; game thread).
 *
 * One-way by design: a RESUME is a statement, never a request, and this
 * never sends anything. The receiver has both sides' numbers in front of it
 * -- s_wrote - r.have is what the peer needs from us, r.newest -
 * s_remote_have what we need from it -- so one message clears or refuses the
 * whole exchange, and when both peers stall (the usual case) each opens its
 * own phase and states its own numbers anyway. An earlier version answered
 * a RESUME whenever no phase was open on this side; both sides then answered
 * each other's answers for the rest of the match, ~60 logged lines a second
 * inside the frame loop (Main's integration run: 2686 copies of the last
 * line below from a handful of interruptions). */
void net_resume_rel(int peer, const void* payload, int len) {
    Resume r;
    if (peer < 0 || peer >= net.npeers) {
        return;
    }
    Peer* p = &s_peer[peer];
    const int port = p->port;
    if (len != (int)sizeof r) {
        pc_log_line("net: RESUME of %d bytes ignored (expected %d)", len, (int)sizeof r);
        return;
    }
    /* Not established yet: there is nothing to resume, and the seed we would
     * compare below is not the agreed one, so a mismatch here would fail the
     * session for the wrong reason. Our own wait ends it fast anyway. */
    if (!session_established()) {
        pc_log_line("net: RESUME at frame %d ignored, the session is not established (hs %d)",
            net.frame, net.hs);
        return;
    }
    memcpy(&r, payload, sizeof r);
    wire_resume(&r);
    /* A restarted peer or a different match cannot be resumed into: every
     * frame we would refill was simulated from another seed. */
    if (r.session != net.session || r.seed != net.seed) {
        pc_log_line("net: cannot resume, the peer answers for session %08x seed %u (ours %08x "
                    "seed %u)",
            r.session, r.seed, net.session, net.seed);
        resume_fail();
        return;
    }
    /* Each side can only serve frames still in its RING-frame ring. In
     * int64 so a corrupt payload at the int32 extremes cannot overflow the
     * comparison. */
    if ((int64_t)s_wrote - r.have > RING || (int64_t)r.newest - s_port_have[port] > RING) {
        pc_log_line("net: cannot resume at frame %d, the gap outruns the %d-frame ring (P%d "
                    "holds our %d of %d, we hold its %d of %d)",
            net.frame, RING, port + 1, r.have, s_wrote, s_port_have[port], r.newest);
        resume_fail();
        return;
    }
    /* The peer may hold frames whose acks died with the link: starting the
     * refill above them saves a round trip. Same guard as on_ack, it cannot
     * hold a frame we never sent. */
    if (r.have > p->last_acked && r.have <= s_wrote) {
        p->last_acked = r.have;
    }
    if (r.newest > s_port_newest[port]) {
        s_port_newest[port] = r.newest;
        remote_marks_update();
    }
    pc_log_line("net: P%d resumes from frame %d, holds our %d, newest %d (we are at %d, hold "
                "its %d, newest %d)",
        port + 1, r.frame, r.have, r.newest, net.frame, s_port_have[port], s_wrote);
}

/* The stall timeout fired: open the phase instead of dropping the session.
 * False when resume is off or the session was never established, and the
 * caller then times out exactly as it did before this file grew a phase --
 * same line, same status, same 7 s. */
static bool resume_begin(uint64_t now) {
    if (s_rc_window_ms <= 0 || !session_established()) {
        return false;
    }
    s_rc = RSM_ACTIVE;
    s_rc_ns = now;
    s_rc_sent = false;
    pc_log_line("net: interrupted at frame %d (peer silent %d ms), reconnecting for up to %d ms",
        net.frame, STALL_TIMEOUT_MS, s_rc_window_ms);
    resume_send();
    return true;
}

/* Once per wait-loop turn while the phase is open. False ends the session:
 * the window expired, or the exchange was refused. */
static bool resume_poll(uint64_t now) {
    if (s_rc == RSM_FAILED) {
        return false;
    }
    if (!s_rc_sent) {
        resume_send(); /* the reliable queue was full when the phase opened */
    }
    if (now - s_rc_ns > (uint64_t)s_rc_window_ms * 1000000ull) {
        pc_log_line("net: resume window of %d ms expired at frame %d", s_rc_window_ms, net.frame);
        s_status = PC_NET_PEER_TIMEOUT; /* the session ends as it always did */
        return false;
    }
    return true;
}

/* The peer's inputs are flowing again. */
static void resume_end(uint64_t now) {
    pc_log_line("net: resumed at frame %d after %.1f s (hold remote %d, its newest %d)", net.frame,
        (now - s_rc_ns) / 1e9, s_remote_have, s_remote_newest);
    s_rc = RSM_NONE;
    s_rc_sent = false;
}

/* Block until the remote input for `need` is here. False when the session is
 * over: the peer announced it is gone, the reconnect window expired or the
 * interruption could not be resumed (s_status says which).
 *
 * An established session that falls silent past the stall timeout enters the
 * reconnect phase above rather than ending; before the peer's first packet
 * there is nothing to resume, so that wait keeps its own timeout.
 *
 * "Silent" is measured from the last datagram the peer sent, NOT from the
 * start of this wait. A peer whose game thread is inside a load -- a stage,
 * a character, a first-time shader compile on a phone -- stops advancing for
 * seconds while tx_timer keeps its input window flowing every 7 ms. Timing
 * that out as silence killed the session on every slow load and froze the
 * transition screen ("NOW LOADING") until the reconnect window expired.
 * A peer that talks but never advances still has to end somewhere, so a
 * no-progress cap bounds the wait; it is minutes, not seconds, because a
 * cold-cache load on a phone is legitimately tens of seconds. */
#define NO_PROGRESS_TIMEOUT_MS 120000
/* The connect wait, with a floor: a session that never went through
 * connect_impl (a fixture, a future caller) must not read as "wait zero". */
static int connect_timeout_ms(void) {
    return net.connect_timeout_ms > 0 ? net.connect_timeout_ms : CONNECT_TIMEOUT_MS;
}
static bool wait_remote(int32_t need) {
    if (s_slippi_on) {
        return s_slippi_wait_remote(need);
    }
    if (s_remote_have >= need) {
        return true;
    }
    uint64_t t0 = SDL_GetTicksNS();
    uint64_t last_send = t0;
    s_stalls++;
    for (;;) {
        recv_inputs();
        links_poll();
        if (s_remote_have >= need) {
            break;
        }
        if (s_peer_left) {
            return false;
        }
        net_watchdog_heartbeat();
        /* Before now, so no stamp the receive thread writes is later than it. */
        uint64_t heard = rx_heard_ns();
        uint64_t now = SDL_GetTicksNS();
        if (s_rc != RSM_NONE) {
            if (!resume_poll(now)) {
                return false;
            }
        } else {
            /* Before the first packet there is no rx clock, so the connect
             * wait is still measured from t0. */
            uint64_t quiet = heard ? now - heard : now - t0;
            uint64_t limit = (heard ? STALL_TIMEOUT_MS : connect_timeout_ms()) * 1000000ull;
            /* Measured from the last forward progress, not from the start of
             * this wait: t0 restarts with every wait_remote() call, so a peer
             * that advanced the contiguous mark one frame just under the bound
             * each time kept the game frozen for ever while looking healthy.
             * Before the first advance there is nothing to measure from, and
             * t0 is the connect wait's own clock anyway. */
            uint64_t since = s_progress_ns ? s_progress_ns : t0;
            bool stuck = now - since > NO_PROGRESS_TIMEOUT_MS * 1000000ull;
            if (quiet > limit || stuck) {
                s_no_progress = stuck && quiet <= limit;
                if (!heard || s_no_progress || !resume_begin(now)) {
                    s_status = PC_NET_PEER_TIMEOUT;
                    return false;
                }
            }
        }
        if (now - last_send > 16000000ull) {
            send_inputs(); /* peer may be waiting on us, or lost our packets */
            last_send = now;
            /* A stall and the resume behind it can hold the game thread for
             * seconds, and a window whose messages go unread that long is
             * marked "not responding" (Windows at ~5 s) and cannot be
             * moved. Pumping only fills SDL's queue: nothing is dispatched
             * or drawn here, mid-tick, and the next frame boundary reads the
             * input and window events as usual. */
            if (now - t0 > 100000000ull) {
                SDL_PumpEvents();
            }
        }
        /* The receive thread is draining the socket meanwhile; half a
         * millisecond is how late its queue can be seen here. */
        SDL_DelayNS(500000);
    }
    if (s_rc != RSM_NONE) {
        resume_end(SDL_GetTicksNS());
    }
    uint64_t dt = SDL_GetTicksNS() - t0;
    if (dt > s_stall_ns_max) {
        s_stall_ns_max = dt;
    }
    if (dt > 500000000ull) {
        s_stall_frame = net.frame;
    }
    return true;
}

/* ---- public ----------------------------------------------------------- */

bool pc_net_active(void) {
    return net.active;
}

/* True when this run's simulation has to be reproducible on another machine:
 * a netplay session, a recording, a replay, or a sync test. Game code uses
 * it to suppress the handful of retail behaviours that are deliberately
 * seeded from the local machine (see gmTitle_801A165C). Offline play keeps
 * every one of them. */
bool pc_net_deterministic(void) {
    return net.active || record_active() || net.synctest;
}

int pc_net_local_player(void) {
    return net.local;
}

/* ---- scene hand-off -------------------------------------------------
 *
 * A scene ends when its own code asks to, and how many TICKS it takes to
 * get there depends on the machine: the loads a scene waits on are polled
 * once per tick and the pad queue turns the wait into simulated frames, so
 * the peer that reads an archive faster leaves the scene earlier. Both then
 * run the next scene from different frames, which is a real divergence and
 * not an arithmetic one (docs/netcode-plan.md section 5.2).
 *
 * So the exit frame is agreed instead of inferred: each peer announces the
 * frame its scene asked to end on, and both leave on the later of the two
 * plus a lead that covers the reliable round trip. This is the pattern the
 * lobby already uses to hand over to the CSS (pc_lan_start_frame). */
#define SCENE_HANDOFF 20 /* frames of lead: ~330 ms, menus are lockstep */
#define SCENE_SLOTS 8    /* announcements kept: the peer may be a scene ahead */
/* How long a scene may wait for the other half of a hand-off before the
 * session is declared dead. Five seconds is far longer than any real
 * hand-off (the announcement is one reliable round trip on top of
 * SCENE_HANDOFF) and short enough that a peer which never answers does not
 * leave the player in a scene they cannot leave. */
#define SCENE_HOLD_MAX 300

/* Defined below; pc_net_scene_hold() is the one caller ahead of it. */
void pc_net_disconnect(void);

/* Every hand-off carries the count of scene exits the sender has made, and a
 * peer's frame is only ever paired with the exit of the same number. Without
 * it a late retransmit or an early announcement is indistinguishable from
 * this scene's answer: one peer then agrees a frame from the PREVIOUS scene,
 * releases immediately while the other is still waiting, and the sims part
 * on the frame that peer enters the next scene (measured tablet<->PC, "peer
 * 121" against an exit asked at 5775). */
static uint32_t s_scene_seq;                     /* exits we have completed */
static int32_t s_scene_exit_local = -1;          /* frame our scene asked to end on */
static int32_t s_scene_exit_at = -1;             /* agreed frame, once both are in */
static int32_t s_scene_wait_since = -1;          /* frame the incomplete wait began */
static int32_t s_scene_exit_remote[MAX_PEERS][SCENE_SLOTS]; /* each peer's, by its own seq */

void net_scene_rel(int peer, const void* payload, int len) {
    SceneMsg m;
    if (len != (int)sizeof m) {
        pc_log_line("net: REL_SCENE of %d bytes ignored", len);
        return;
    }
    memcpy(&m, payload, sizeof m);
    int64_t f = (int32_t)ntohl(m.frame);
    /* A correct peer announces its own frame, which tracks ours to within the
     * few frames the hand-off is built for. Anything else is a corrupt or
     * hostile message, and pc_net_scene_hold() would turn it into a scene the
     * game can never leave (a far-future frame) or an exit taken alone (a
     * frame near INT32_MAX, where later + SCENE_HANDOFF overflows negative
     * and this side walks out while the peer is still waiting). */
    if (f < 0 || f > (int64_t)net.frame + RING) {
        pc_log_line("net: REL_SCENE for frame %d ignored (we are at %d)", (int32_t)f, net.frame);
        return;
    }
    /* The sequence is the peer's count of completed exits, so the only ones
     * that can be about a scene either side still cares about are the one we
     * are in and its immediate neighbours: the peer may be a scene ahead if
     * it left first, and a retransmit of the previous one may still arrive.
     * Anything further is stale or invented, and taking it would alias into
     * a live slot through the modulo -- an exit frame from eight scenes ago,
     * or a far-future one planted deliberately, landing in the slot the
     * current hand-off reads. */
    uint32_t seq = ntohl(m.seq);
    int64_t ahead = (int64_t)seq - (int64_t)s_scene_seq;
    if (ahead < -1 || ahead > 1) {
        pc_log_line("net: REL_SCENE seq %u ignored (we are on %u)", seq, s_scene_seq);
        return;
    }
    if (peer >= 0 && peer < MAX_PEERS) {
        s_scene_exit_remote[peer][seq % SCENE_SLOTS] = (int32_t)f;
    }
}

static void scene_handoff_reset(void) {
    s_scene_seq = 0;
    s_scene_exit_local = s_scene_exit_at = -1;
    s_scene_wait_since = -1;
    for (int p = 0; p < MAX_PEERS; p++) {
        for (int i = 0; i < SCENE_SLOTS; i++) {
            s_scene_exit_remote[p][i] = -1;
        }
    }
}

/* True while the caller must keep ticking the scene it has asked to leave.
 * Offline, and once the peer is gone, it never holds. */
bool pc_net_scene_hold(void) {
    if (net.active && s_slippi_match_only) {
        return false; /* scenes end locally; only the match frames are synced */
    }
    if (!net.active) {
        /* Replaying a netplay recording: the scene has to end where the
         * recorded pair agreed, not where this run's own code asks to. */
        return record_replay_scene_hold(net.frame);
    }
    /* Every peer's exit frame for this scene; the latest of them, or -1 while
     * any is still missing. All of them announce to all of them, so each
     * computes the same exit on its own. */
    const int slot = s_scene_seq % SCENE_SLOTS;
    int32_t remote = -2;
    for (int i = 0; i < net.npeers; i++) {
        if (s_gone & (1u << s_peer[i].port)) {
            continue;
        }
        int32_t f = s_scene_exit_remote[i][slot];
        remote = f < 0 ? -1 : remote == -1 ? -1 : f > remote ? f : remote;
    }
    if (s_scene_exit_local < 0) {
        SceneMsg m = {htonl(s_scene_seq), htonl((uint32_t)net.frame)};
        /* Only latch the frame if the announcement actually went out. The
         * lane is stop-and-wait four deep and shares itself with the resume,
         * delay, chat, stage-pick and ranked messages, so it can be full at
         * the instant a scene ends; latching anyway drops the announcement
         * for good, the peer waits on an exit frame it never gets, and the
         * two sims end up in different scenes. Retry on the next tick, the
         * way resume_send() already does. */
        if (pc_net_send_reliable(REL_SCENE, &m, sizeof m)) {
            s_scene_exit_local = net.frame;
        }
    }
    /* A hand-off that never completes is otherwise silent: the scene simply
     * never ends and the session stays healthy around it, which is
     * indistinguishable from a scene that has not asked yet. Say so once a
     * second while waiting, naming which half is missing -- and stop waiting
     * eventually. The peer keeps sending inputs, so neither the stall timeout
     * nor the watchdog notices a peer that has simply stopped answering this
     * exchange, and the player is left in a scene they cannot leave. */
    if (s_scene_exit_local < 0 || remote < 0) {
        static int32_t said;
        if (s_scene_wait_since < 0) {
            s_scene_wait_since = net.frame;
        }
        if (net.frame - s_scene_wait_since >= SCENE_HOLD_MAX) {
            pc_log_line("net: scene %u hand-off never completed in %d frames (ours %d, peer "
                        "%d); leaving netplay",
                s_scene_seq, SCENE_HOLD_MAX, s_scene_exit_local, remote);
            s_status = PC_NET_PEER_TIMEOUT;
            pc_net_disconnect();
            return false; /* the session is over: the scene ends here */
        }
        if (net.frame - said >= 60) {
            said = net.frame;
            pc_log_line("net: scene %u hand-off waiting at frame %d (ours %d, peer %d)",
                s_scene_seq, net.frame, s_scene_exit_local, remote);
        }
        return true; /* the peer is still in the scene: wait for its frame */
    }
    s_scene_wait_since = -1;
    if (s_scene_exit_at < 0) {
        int32_t later = remote > s_scene_exit_local ? remote : s_scene_exit_local;
        s_scene_exit_at = later + SCENE_HANDOFF;
        /* Nothing may still be predicted when the scene goes: the next
         * scene's first tick raises the barrier past every outstanding
         * frame, and snapshot_unusable() rejects those snapshots anyway, so
         * a correction arriving then is dropped and the peers finish the
         * match on different remote inputs. Running the last frames of the
         * scene lockstep costs nothing -- they are the frames after the
         * match has already been decided -- and it is the same thing the
         * entry side does. */
        barrier_raise(s_scene_exit_at);
        pc_log_line("net: scene %u ends at frame %d (asked %d, peer %d)", s_scene_seq,
            s_scene_exit_at, s_scene_exit_local, remote);
    }
    if (net.frame < s_scene_exit_at) {
        return true;
    }
    for (int i = 0; i < MAX_PEERS; i++) {
        s_scene_exit_remote[i][slot] = -1;
    }
    s_scene_exit_local = s_scene_exit_at = -1;
    s_scene_seq++;
    return false;
}

/* True when the newest local sample holds no stick deflection and no button.
 * A whole-frame correction taken while the player is mid-input costs them
 * that frame of it; taken at rest it costs nothing anyone can feel. The
 * sample is the one already quantised for the wire, so idle stick noise
 * (at_rest) does not read as movement. */
bool net_local_idle(void) {
    if (s_wrote < 0) {
        return true;
    }
    const WirePad* p = &s_local_ring[s_wrote & (RING - 1)];
    return p->button == 0 && p->stickX == 0 && p->stickY == 0 && p->substickX == 0 &&
           p->substickY == 0 && p->triggerLeft == 0 && p->triggerRight == 0;
}

static void audio_journal_reset(void);
static void pad_stats_reset(void);
static void seed_stats_reset(void);
static void audit_reset(void);
static bool s_seen_remote;

static void session_reset(void) {
    s_slippi_match_only = false;
    scene_handoff_reset();
    s_seed_have = false;
    s_lockstep = snapshot_state_region_missing() != NULL;
    s_seen_remote = false;
    memset(s_local_ring, 0, sizeof s_local_ring);
    memset(s_remote_ring, 0, sizeof s_remote_ring);
    memset(s_ck_ring, 0, sizeof s_ck_ring);
    memset(s_wire_ck_ring, 0, sizeof s_wire_ck_ring);
    memset(s_sim_n, 0, sizeof s_sim_n);
    memset(s_rb_recent, 0, sizeof s_rb_recent);
    s_rb_recent_n = 0;
    sim_reset();
    memset(s_rx_held, 0, sizeof s_rx_held);
    net.frame = 0;
    net.tick_frame = -1;
    net.resim = false;
    links_reset();
    remote_ports_set(s_remote_mask);
    s_rb_frame = -1;
    s_end_frame = -1;
    s_game_over = false;
    /* Every link starts over; its port and address are what connect set. */
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer* p = &s_peer[i];
        Peer keep = *p;
        memset(p, 0, sizeof *p);
        p->port = keep.port;
        p->addr = keep.addr;
        p->addr_len = keep.addr_len;
        p->rx_have = p->last_acked = p->ck_checked = p->ck_last_frame = -1;
        for (int f = 0; f < RING; f++) {
            p->rck[f].frame = -1;
        }
    }
    net.desync_reported = s_peer_left = s_no_progress = false;
    s_slippi_ck_checked_once = false;
    s_progress_ns = 0;
    s_warn_src = s_warn_sess = s_warn_bad = s_warn_mac = false;
    /* A fresh session starts with no key and trusting nothing it has not
     * yet authenticated; the key arrives with the handshake. */
    net_key_clear();
    s_status = PC_NET_PEER_OK;
    s_rc = RSM_NONE;
    s_rc_sent = false;
    s_stalls = net.skips = s_advances = s_rollbacks = s_rb_lost = 0;
    s_rb_depth_max = s_rb_depth_cur = s_rb_depth_recent = 0;
    s_stall_ns_max = 0;
    s_stall_frame = -1000;
    net.ping_us = 0;
    s_ping_sum = 0;
    s_ping_n = 0;
    s_rtt_min = s_rtt_max = 0;
    net.tx_pkts = s_rx_pkts = net.tx_inputs = s_rx_acks = 0;
    s_rx_dups = s_rx_reorders = 0;
    s_sock_err = 0;
    s_warn_sock = false;
    s_rx_malformed = s_rx_bad_src = s_rx_bad_sess = s_rx_bad_player = s_tx_would_block = 0;
    s_rx_bad_mac = s_rx_full = 0;
    s_rx_left = false;
    s_rx_why = 0;
    s_rxq_head = s_rxq_n = 0;
    atomic_store(&s_frame_pub, 0);
    for (int i = 0; i < MAX_PEERS; i++) {
        atomic_store(&s_adv_pub[i], 0);
    }
    atomic_store(&s_want_resend, false);
    s_red_target = REDUNDANCY;
    s_resim_run = 0;
    s_resim_eat = 0;
    s_resim_eat_logged = false;
    s_timer_tried = false;
    s_loss_pct = 0;
    s_loss_tx_mark = s_loss_ack_mark = 0;
    sync_reset();
    audio_journal_reset();
    pad_stats_reset();
    seed_stats_reset();
    audit_reset();
    s_wrote = -1;
    s_send_ns = 0;
    s_send_frame = -1;
    net.rb_barrier = -1;
    s_scene_last = -1;
    s_rb_lost_logged = false;
    rel_reset();
    net.hs = HS_IDLE;
    net.hs_host = false;
    net.direct = false; /* pc_net_init() sets it back for a MELEE_NET session */
    net.seed = 0;
    s_slippi_setup_host = false;
    net.rng_offset = 0;
    net.start_frame = -1;
    net.ck_from = 0;
    snaps_free();
}

void pc_net_disconnect(void) {
    if (s_slippi_on) {
        /* Tear the Slippi transport down first: it owns the ENet host, and a
         * disconnected peer needs no custom BYE (ENet signals it). */
        if (s_slippi.net != NULL) {
            pc_slippi_net_disconnect(s_slippi.net, PC_SLIPPI_DISCONNECT_UNSPECIFIED);
            pc_slippi_adapter_pump(&s_slippi, s_slippi_now_us());
            pc_slippi_net_destroy(s_slippi.net);
            s_slippi.net = NULL;
        }
        s_slippi_on = false;
        net.slippi = false;
        net.active = false;
        net.resim = false;
        net.hs = HS_IDLE;
        rules_restore();
        HSD_PadLibData.qtype = 0;
        pc_log_line("net: Slippi disconnected at frame %d (status %d)", net.tick_frame, s_status);
        snaps_free();
        return;
    }
    if (net.sock == SOCK_INVALID) {
        return;
    }
    /* Timer and receive thread first: a removed timer cannot fire again, and
     * taking tx_lock below waits out a callback already running; a joined
     * thread reads the socket no more, and what it queued dies with the
     * session. Nothing touches the socket after this returns. */
    if (s_timer) {
        SDL_RemoveTimer(s_timer);
        s_timer = 0;
    }
    if (s_rx_thread != NULL) {
        atomic_store(&s_rx_run, false);
        SDL_WaitThread(s_rx_thread, NULL);
        s_rx_thread = NULL;
    }
    s_rxq_head = s_rxq_n = 0;
    if (!s_peer_left) {
        /* Tell the peer why so it need not wait out the silence; sent
         * as a 5-packet burst so NAT/firewall drops are tolerated. */
        uint8_t why = s_status == PC_NET_PEER_OK ? PC_NET_PEER_LEFT : (uint8_t)s_status;
        net.sim_hold = false; /* straight out: the held queue dies with the socket */
        for (int i = 0; i < 5; i++) {
            SDL_LockMutex(net.tx_lock);
            for (int peer = 0; peer < net.npeers; peer++) {
                send_bye(peer, why);
            }
            SDL_UnlockMutex(net.tx_lock);
            SDL_Delay(4);
        }
        if (s_status == PC_NET_PEER_OK) {
            s_status = PC_NET_PEER_LEFT;
        }
    }
    SDL_LockMutex(net.tx_lock);
    net.active = false;
    net.resim = false;
    sock_close(net.sock);
    net.sock = SOCK_INVALID;
    net.hs = HS_IDLE;
    /* After the BYE burst above, which still needed the key to be accepted:
     * the nonces it was derived from are dead with the session, so the key
     * does not outlive it in this process's memory either. */
    net_key_clear();
    for (int i = 0; i < MAX_PEERS; i++) {
        s_peer[i].mac_seen = false;
    }
    rules_restore();
    HSD_PadLibData.qtype = 0;
    SDL_UnlockMutex(net.tx_lock);
    pc_log_line("net: disconnected at frame %d (status %d)", net.tick_frame, s_status);
    snaps_free();
}

/* MELEE_NET_EXIT_AFTER_FRAMES=n (tools/net_test.py): the instance that reaches
 * n first sends BYE, and the other one is a frame or two behind (the clocks
 * differ by the time offset), so its own check would never fire — it would sit
 * at the title until the harness killed it. A BYE this close to the target ends
 * the test as done; a BYE for a real reason still logs DESYNC etc. first. */
static int32_t s_exit_after; /* 0 = knob unset */
static void exit_if_test_done(void) {
    if (s_exit_after > 0 && net.frame >= s_exit_after - 16) {
        pc_log_line("net: test done at frame %d", net.frame);
        pc_net_disconnect(); /* BYE goes out, the peer need not wait out the timeout */
        exit(0);
    }
}

int pc_net_peer_status(void) {
    return s_status;
}

bool pc_net_desync(void) {
    return net.active && net.desync_reported;
}

void pc_net_peer_status_clear(void) {
    s_status = PC_NET_PEER_OK;
}

int pc_net_quality(void) {
    if (!net.active) {
        return 0;
    }
    if (s_rc == RSM_ACTIVE) {
        return 3; /* reconnecting; RSM_FAILED is one tick from the disconnect */
    }
    /* A desync is as bad as a stall and never recovers, so it reads as at
     * least 2 however healthy the link looks around it. */
    if (s_peer_left || net.desync_reported || net.frame - s_stall_frame < 120) {
        return 2;
    }
    if (s_loss_pct >= 5 || s_rb_depth_recent >= 4 || jitter_us() >= 8000) {
        return 1;
    }
    return 0;
}

static int s_configured_delay = -1;
void pc_net_set_input_delay(int frames) {
    s_configured_delay = frames >= 0 && frames <= 4 ? frames : -1;
}

/* Is this a destination a peer could answer from? A network, broadcast or
 * multicast address is never a host, and a session pointed at one can only
 * time out -- which, with the connect wait parking the render thread, used to
 * present as a crash rather than as a failure to connect. Seen in the wild
 * (issue #87): a pairing dialled a /8 network base -- the peer's first octet
 * with the other three zeroed -- instead of the address its datagrams had
 * actually arrived from. */
static bool addr_is_host(const struct sockaddr* sa) {
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)sa;
        uint32_t ip = ntohl(a->sin_addr.s_addr);
        return a->sin_port != 0 && ip != 0 && ip != 0xffffffffu && (ip >> 28) < 14 &&
               (ip & 0xffffffu) != 0;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6* a = (const struct sockaddr_in6*)sa;
        static const uint8_t any[16] = {0};
        return a->sin6_port != 0 && memcmp(&a->sin6_addr, any, sizeof any) != 0 &&
               a->sin6_addr.s6_addr[0] != 0xff;
    }
    return false;
}

/* One remote player to link to: its address and its controller port. */
typedef struct PeerSpec {
    const char* ip;
    uint16_t port;
    int player;         /* its controller port, 0-3 */
    const uint8_t* key; /* the link's MAC key from XeleeNet's punch; NULL: the handshake's */
    bool dead;          /* never punched: through the host from the start */
} PeerSpec;

/* A session with n peers (1..MAX_PEERS), this side on controller port
 * `local`. Port 0 hosts: it names the session and leads the handshake. */
static bool connect_impl(
    sock_t supplied, const PeerSpec* peers, int n, int local, uint32_t seed) {
#ifdef __EMSCRIPTEN__
    /* A page has no UDP: Emscripten's sockets are WebSocket proxies. Every
     * session path (MELEE_NET, the lobby's DHT socket) comes through here, so
     * refusing here keeps netplay inert and its receive thread unstarted. */
    (void)supplied, (void)peers, (void)n, (void)local, (void)seed;
    pc_log_line("net: netplay is unavailable in the browser");
    return false;
#endif
    if (net.tx_lock == NULL) {
        net.tx_lock = SDL_CreateMutex();
        s_rx_lock = SDL_CreateMutex();
        sock_startup();
    }
    pc_net_disconnect();
    if (n < 1 || n > MAX_PEERS || local < 0 || local > 3) {
        pc_log_line("net: %d peers with local port %d is not a session", n, local);
        return false;
    }
    struct sockaddr_storage peer_addr[MAX_PEERS];
    socklen_t peer_len[MAX_PEERS];
    uint8_t mask = 0;
    int family = AF_UNSPEC;
    for (int i = 0; i < n; i++) {
        const PeerSpec* ps = &peers[i];
        if (ps->player < 0 || ps->player > 3 || ps->player == local || (mask & (1u << ps->player))) {
            pc_log_line("net: peer %d's port P%d is taken or out of range", i, ps->player + 1);
            return false;
        }
        mask |= (uint8_t)(1u << ps->player);
        char portstr[8];
        snprintf(portstr, sizeof portstr, "%u", ps->port);
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = supplied == SOCK_INVALID ? (family != AF_UNSPEC ? family : AF_UNSPEC) :
                                                     AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(ps->ip, portstr, &hints, &res) != 0 || res == NULL) {
            pc_log_line("net: cannot resolve %s:%u", ps->ip, ps->port);
            return false;
        }
        if (!addr_is_host(res->ai_addr)) {
            pc_log_line(
                "net: refusing to connect to %s:%u -- not a unicast host address", ps->ip, ps->port);
            freeaddrinfo(res);
            return false;
        }
        memset(&peer_addr[i], 0, sizeof peer_addr[i]);
        memcpy(&peer_addr[i], res->ai_addr, res->ai_addrlen);
        peer_len[i] = (socklen_t)res->ai_addrlen;
        family = res->ai_family; /* one socket: every peer resolves in the first one's family */
        freeaddrinfo(res);
    }

    sock_t sock = supplied == SOCK_INVALID ? socket(family, SOCK_DGRAM, 0) : supplied;
    if (sock == SOCK_INVALID) {
        pc_log_line("net: socket() failed");
        return false;
    }
    const char* lport = getenv("MELEE_NET_PORT");
    unsigned short bind_port = (unsigned short)(lport ? atoi(lport) : 41000);
    struct sockaddr_storage bind_addr;
    memset(&bind_addr, 0, sizeof bind_addr);
    socklen_t bind_len;
    if (family == AF_INET6) {
        struct sockaddr_in6* a = (struct sockaddr_in6*)&bind_addr;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(bind_port);
        bind_len = sizeof *a;
    } else {
        struct sockaddr_in* a = (struct sockaddr_in*)&bind_addr;
        a->sin_family = AF_INET;
        a->sin_port = htons(bind_port);
        bind_len = sizeof *a;
    }
    if (supplied == SOCK_INVALID && bind(sock, (struct sockaddr*)&bind_addr, bind_len) != 0) {
        /* The reason matters: "address already in use" is another instance
         * (or one still tearing down) holding the port, and the session then
         * silently does not happen at all -- which looks exactly like two
         * peers playing unconnected games side by side. */
        pc_log_line("net: bind(%u) failed, error %d (in use by another instance?)", bind_port,
            sock_last_err());
        sock_close(sock);
        return false;
    }
    if (!sock_nonblock(sock)) {
        pc_log_line("net: could not put the socket in non-blocking mode");
        if (supplied == SOCK_INVALID)
            sock_close(sock);
        return false;
    }
    if (supplied != SOCK_INVALID) {
        struct sockaddr_in bound;
        socklen_t length = sizeof bound;
        if (getsockname(sock, (struct sockaddr*)&bound, &length) == 0)
            bind_port = ntohs(bound.sin_port);
    }
    /* Ask the network for expedited forwarding (DSCP EF / 46): low-latency
     * queueing where the path honours it, harmless where it does not. */
#if defined(_WIN32)
    /* ponytail: Windows ignores IP_TOS for DSCP; real marking there needs the
     * qWAVE API (QOSAddSocketToFlow). Skipped in the prototype. */
#else
    {
        int tos = 0xB8; /* DSCP 46 << 2, ECN 0 */
        int qr = family == AF_INET6 ?
                     setsockopt(sock, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof tos) :
                     setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
        if (qr != 0) {
            pc_log_line("net: could not set DSCP EF on the socket (non-fatal)");
        }
    }
#endif
    session_reset();
    /* A full raw queue (any stall) with qtype 0 makes the pad alarm shift
     * qread, dropping the head write_head() just filled: the tick then eats
     * a raw sample (remote port = no controller) and the peers diverge. 2
     * drops the new raw sample instead; the local input is read from the
     * head anyway. */
    HSD_PadLibData.qtype = 2;
    net.local = local;
    net.remote = peers[0].player;
    remote_ports_set(mask);
    net.npeers = n;
    for (int i = 0; i < n; i++) {
        net.peer_port[i] = s_peer[i].port = peers[i].player;
        s_peer[i].addr = peer_addr[i];
        s_peer[i].addr_len = peer_len[i];
    }
    /* A hand-typed address may be a peer the user has not started yet, so it
     * keeps the long wait. One handed over from matchmaking (pc_net_connect_
     * socket, the only caller that passes a socket) belongs to a peer that
     * answered a signed offer seconds ago: if it says nothing now, the
     * destination is dead, and 60 s of a parked render thread looks exactly
     * like the crash in issue #87. */
    net.connect_timeout_ms =
        supplied == SOCK_INVALID ? CONNECT_TIMEOUT_MS : MATCH_CONNECT_TIMEOUT_MS;
    const char* delay = getenv("MELEE_NET_DELAY");
    net.delay_auto = delay ? strcmp(delay, "auto") == 0 : s_configured_delay < 0;
    net.delay = net.delay_auto ? 2 : delay ? atoi(delay) : s_configured_delay;
    if (net.delay < 0 || net.delay >= RING / 2) {
        net.delay = 2;
    }
    net.delay_next = net.delay;
    s_wrote = net.delay - 1; /* frames 0..delay-1 stay neutral on both sides */
    /* MELEE_NET_ROLLBACK=off: keep the barrier ahead of every frame, which
     * is the lockstep the out-of-memory path already falls back to (nothing
     * is predicted, nothing is snapshotted, nothing is re-run). The session
     * then runs at the link's round trip, so this is a diagnostic: a row
     * that desyncs with rollback on and survives with it off has its fault
     * in the re-simulation, not in the input pipeline. */
    const char* rb = getenv("MELEE_NET_ROLLBACK");
    if (rb != NULL && strcmp(rb, "off") == 0) {
        barrier_raise(INT32_MAX);
        pc_log_line("net: rollback off (MELEE_NET_ROLLBACK), lockstep at the link's round trip");
    }
    /* 0 means "no resume phase at all", so it must survive the parse; a
     * negative or unparseable value falls back to the default rather than
     * silently disabling the feature. */
    const char* rc_ms = getenv("MELEE_NET_RECONNECT_MS");
    char* rc_end = NULL;
    int64_t rc_want = rc_ms != NULL ? strtoll(rc_ms, &rc_end, 10) : RECONNECT_MS;
    /* Anything unparseable, negative or wider than the field falls back to the
     * default; 0 is legal and means "no reconnect phase, drop as before". */
    bool rc_bad =
        rc_ms != NULL && (rc_end == rc_ms || *rc_end != '\0' || rc_want < 0 || rc_want > INT32_MAX);
    s_rc_window_ms = rc_bad ? RECONNECT_MS : (int32_t)rc_want;
    sim_env(bind_port);
    /* The host names the session; the guest takes it from the first packet. */
    net.session = net.local == 0 ?
                      (uint32_t)(SDL_GetPerformanceCounter() ^ (uint64_t)getpid() << 20) | 1u :
                      0;
    net.seed = seed;
    if (seed != 0) {
        *HSD_RandSeedPtr = seed;
    }
    /* MELEE_NET_KEY: one secret typed by whoever starts both instances. The
     * handshake derives a key of its own from the two nonces, but only once
     * it completes, so this is what covers the bootstrap window before that
     * -- and it is all a peer has if the two never agree. It pins the key
     * for the whole session -- a handshake later on will not replace it --
     * because rekeying in flight would drop every datagram that straddled
     * the change, and the two peers cannot make that change on the same
     * frame. */
    const char* key = getenv("MELEE_NET_KEY");
    if (key != NULL && key[0] != '\0') {
        net_key_direct(key);
        pc_log_line("net: datagrams authenticated from MELEE_NET_KEY (the handshake will not "
                    "rekey this session)");
    }
    /* XeleeNet's punch keyed every link already, and decided which guest
     * pairs never got through: those start out dead, and the first
     * links_poll tells the host, so no 8 s wait finds it out again. */
    for (int i = 0; i < n; i++) {
        if (peers[i].key != NULL) {
            net_key_link(i, peers[i].key);
        }
        if (peers[i].dead && local != 0 && peers[i].player != 0) {
            net.link_dead[i] = true;
            s_route_mask |= (uint8_t)(1u << peers[i].player);
            s_route_told = false;
        }
    }
    SDL_LockMutex(net.tx_lock);
    net.sock = sock;
    net.active = true;
    SDL_UnlockMutex(net.tx_lock);
    for (int i = 0; i < n; i++) {
        pc_log_line("net: peer %d is P%d at %s:%u", i, peers[i].player + 1, peers[i].ip,
            peers[i].port);
    }
    pc_log_line(
        "net: rollback with %d peer%s (P%d first at %s:%u), local port %u, player P%d, delay "
        "%s%d, window %d, seed %u, session %08x, proto %d, sim loss %d%% delay %d/%d ms jitter "
        "%d reorder %d%% dup %d%% burst %d",
        n, n == 1 ? "" : "s", peers[0].player + 1, peers[0].ip, peers[0].port, bind_port,
        net.local + 1, net.delay_auto ? "auto " : "", net.delay, WINDOW, seed,
        net.session, WIRE_VERSION, net.sim_loss, (int)(net.sim_delay_ns / 1000000),
        (int)(net.sim_rx_delay_ns / 1000000), net.sim_jitter_ms, net.sim_reorder, net.sim_dup,
        net.sim_burst);
    atomic_store(&s_rx_run, true);
    s_rx_thread = SDL_CreateThread(rx_main, "net rx", NULL);
    if (s_rx_thread == NULL) {
        /* Still a working session, only without the fix: recv_inputs drains
         * the socket itself, and a stall of this thread reads as latency. */
        pc_log_line("net: no receive thread; receiving on the game thread");
    }
    /* The FP control word, from inside the shipped process. Flush-to-zero or
     * a non-default rounding mode on one peer and not the other silently
     * changes every result; it was measured IEEE-default on both targets,
     * but only ever in a shell process, because a release APK cannot be
     * ptraced. Two peers' logs now answer it directly. */
    {
        uint64_t fp = 0;
#if defined(__aarch64__)
        __asm__ volatile("mrs %0, fpcr" : "=r"(fp));
#elif defined(__x86_64__) || defined(__i386__)
        uint32_t mxcsr = 0;
        __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
        fp = mxcsr;
#endif
        pc_log_line("net: fp control word %08" PRIx64 " (0 = IEEE default on aarch64; "
                    "x86 default 1f80)",
            fp);
    }
    return true;
}

bool pc_net_connect(const char* ip, uint16_t port, int player, uint32_t seed) {
    PeerSpec peer = {ip, port, player ? 0 : 1};
    return connect_impl(SOCK_INVALID, &peer, 1, player ? 1 : 0, seed);
}

/* Connect over the Slippi transport (MELEE_NET_STACK=slippi). The game-visible
 * session setup (local/remote ports, delay, seed, barrier, rings) is shared
 * with the custom path; only the transport differs. `local_udp_port` of 0 lets
 * ENet pick a port (fine for a direct dial); a matchmaking hole punch passes
 * the lobby's port. */
bool pc_net_connect_slippi(
    const char* ip, uint16_t port, int player, uint32_t seed, uint16_t local_udp_port) {
#ifdef __EMSCRIPTEN__
    (void)ip, (void)port, (void)player, (void)seed, (void)local_udp_port;
    pc_log_line("net: the Slippi transport is unavailable in the browser");
    return false;
#else
    if (net.tx_lock == NULL) {
        net.tx_lock = SDL_CreateMutex();
        s_rx_lock = SDL_CreateMutex();
        sock_startup();
        pc_slippi_net_global_init();
    }
    pc_net_disconnect();
    session_reset();
    HSD_PadLibData.qtype = 2;
    net.local = player ? 1 : 0;
    net.remote = 1 - net.local;
    remote_ports_set((uint8_t)(1u << net.remote));
    net.npeers = 1;
    net.peer_port[0] = s_peer[0].port = net.remote;
    /* Default: P1 sends the setup. A matchmaking session overrides this from
     * the ticket's decider flag (pc_net_slippi_set_setup_host). */
    s_slippi_setup_host = net.local == 0;
    /* Test hook: MELEE_SLIPPI_SETUP_PORT=1|2 names the port that sends the
     * setup (both peers set the same value), to exercise a non-P1 decider. */
    const char* setup_port = getenv("MELEE_SLIPPI_SETUP_PORT");
    if (setup_port && (setup_port[0] == '1' || setup_port[0] == '2')) {
        s_slippi_setup_host = net.local == setup_port[0] - '1';
    }
    net.connect_timeout_ms = MATCH_CONNECT_TIMEOUT_MS;
    const char* delay = getenv("MELEE_NET_DELAY");
    net.delay_auto = delay ? strcmp(delay, "auto") == 0 : s_configured_delay < 0;
    net.delay = net.delay_auto ? 2 : delay ? atoi(delay) : s_configured_delay;
    if (net.delay < 0 || net.delay >= RING / 2) {
        net.delay = 2;
    }
    net.delay_next = net.delay;
    s_wrote = net.delay - 1;
    net.seed = seed;
    net.direct = true;

    PcSlippiSessionConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.local_port = (uint8_t)net.local;
    cfg.remote_count = 1;
    cfg.remote_ports[0] = (uint8_t)net.remote;
    s_slippi.net = pc_slippi_net_create(&cfg, local_udp_port, NULL);
    if (s_slippi.net == NULL) {
        pc_log_line("net: cannot create the Slippi ENet host");
        return false;
    }
    pc_slippi_adapter_init(&s_slippi, s_slippi.net, slp_frame_cb, NULL, NULL);
    /* setup/selection/chat/connection events arrive through the driver's own
     * callback struct, not the adapter's poll, so register them here. The
     * adapter's on_setup/on_frame are separate (poll-driven) and stay as set. */
    {
        PcSlippiNetCallbacks cb;
        memset(&cb, 0, sizeof cb);
        cb.on_setup = slp_setup_cb;
        cb.on_rel = slp_rel_cb;
        pc_slippi_net_set_callbacks(s_slippi.net, &cb);
    }
    pc_slippi_adapter_start_game(&s_slippi, s_slippi_now_us());
    s_slippi_sent_upto = -1;
    s_slippi_ever_heard = false;
    s_slippi_on = true;
    net.slippi = true;
    net.sock = SOCK_INVALID; /* there is no custom socket under Slippi */
    pc_slippi_net_listen(s_slippi.net); /* accept if the peer dials us */
    if (ip != NULL && ip[0] != '\0') {
        if (!pc_slippi_net_connect(s_slippi.net, ip, port)) {
            pc_log_line("net: cannot dial Slippi peer %s:%u", ip, port);
            pc_slippi_net_destroy(s_slippi.net);
            s_slippi.net = NULL;
            s_slippi_on = false;
            net.slippi = false;
            return false;
        }
    }
    net.active = true;
    pc_log_line("net: Slippi transport: peer %s:%u, local port P%d, delay %s%d, seed %u",
        ip != NULL && ip[0] ? ip : "(listen)", port, net.local + 1, net.delay_auto ? "auto " : "",
        net.delay, seed);
    return true;
#endif
}

/* ---- Slippi link and adopt (see net.h) ---------------------------------- */
static PcSlippiNet* s_link;

bool pc_net_slippi_link_open(
    const char* ip, uint16_t port, int local_player, uint16_t local_udp_port) {
#ifdef __EMSCRIPTEN__
    (void)ip, (void)port, (void)local_player, (void)local_udp_port;
    return false;
#else
    if (net.tx_lock == NULL) {
        net.tx_lock = SDL_CreateMutex();
        s_rx_lock = SDL_CreateMutex();
        sock_startup();
        pc_slippi_net_global_init();
    }
    pc_net_slippi_link_close();
    PcSlippiSessionConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.local_port = (uint8_t)(local_player ? 1 : 0);
    cfg.remote_count = 1;
    cfg.remote_ports[0] = (uint8_t)(local_player ? 0 : 1);
    s_link = pc_slippi_net_create(&cfg, local_udp_port, NULL);
    if (s_link == NULL) {
        pc_log_line("net: cannot create the Slippi ENet host");
        return false;
    }
    pc_slippi_net_listen(s_link);
    if (!pc_slippi_net_connect(s_link, ip, port)) {
        pc_log_line("net: cannot dial Slippi peer %s:%u", ip, port);
        pc_net_slippi_link_close();
        return false;
    }
    pc_log_line("net: Slippi link: dialling %s:%u from port %u as P%d", ip, port, local_udp_port,
        local_player + 1);
    return true;
#endif
}

void pc_net_slippi_link_pump(void) {
    if (s_link != NULL) {
        pc_slippi_net_poll(s_link, s_slippi_now_us());
    }
}

int pc_net_slippi_link_state(void) {
    if (s_link == NULL) {
        return 2;
    }
    int st = pc_slippi_net_status(s_link);
    if (st == PC_SLIPPI_NET_CONNECTED) {
        return 1;
    }
    if (st == PC_SLIPPI_NET_DISCONNECTED || st == PC_SLIPPI_NET_FAILED) {
        return 2;
    }
    return 0;
}

bool pc_net_slippi_link_send_selections(const struct PcSlippiSelections* s) {
    return s_link != NULL && pc_slippi_net_send_selections(s_link, s);
}

bool pc_net_slippi_link_remote_selections(struct PcSlippiSelections* out) {
    return s_link != NULL && pc_slippi_net_remote_selections(s_link, 0, out);
}

void pc_net_slippi_link_close(void) {
    if (s_link == NULL) {
        return;
    }
    pc_slippi_net_disconnect(s_link, PC_SLIPPI_DISCONNECT_UNSPECIFIED);
    pc_slippi_net_poll(s_link, s_slippi_now_us());
    pc_slippi_net_destroy(s_link);
    s_link = NULL;
}

bool pc_net_slippi_adopt(int local_player, uint32_t rng_offset) {
#ifdef __EMSCRIPTEN__
    (void)local_player, (void)rng_offset;
    return false;
#else
    if (s_link == NULL || pc_net_slippi_link_state() != 1) {
        pc_log_line("net: no connected Slippi link to start a match on");
        return false;
    }
    PcSlippiNet* link = s_link;
    s_link = NULL;
    session_reset();
    HSD_PadLibData.qtype = 2;
    net.local = local_player ? 1 : 0;
    net.remote = 1 - net.local;
    remote_ports_set((uint8_t)(1u << net.remote));
    net.npeers = 1;
    net.peer_port[0] = s_peer[0].port = net.remote;
    net.connect_timeout_ms = MATCH_CONNECT_TIMEOUT_MS;
    const char* delay = getenv("MELEE_NET_DELAY");
    /* Each Slippi client picks its own input delay (2 by default) and labels
     * its pads with the frame they apply to, so nothing is negotiated: auto
     * delay announces changes over the tunnelled lane, which a Dolphin peer
     * does not have. */
    net.delay_auto = false;
    net.delay = delay ? atoi(delay) : s_configured_delay;
    if (net.delay < 0 || net.delay >= RING / 2) {
        net.delay = 2;
    }
    net.delay_next = net.delay;
    s_wrote = net.delay - 1;
    net.seed = rng_offset;
    net.rng_offset = rng_offset;
    net.direct = true;
    s_slippi.net = link;
    pc_slippi_adapter_init(&s_slippi, link, slp_frame_cb, NULL, NULL);
    /* No setup message and no tunnelled engine lanes: a Dolphin peer sends
     * neither, and the match is already agreed. */
    PcSlippiNetCallbacks cb;
    memset(&cb, 0, sizeof cb);
    pc_slippi_net_set_callbacks(link, &cb);
    pc_slippi_adapter_start_game(&s_slippi, s_slippi_now_us());
    /* Slippi numbers the online frames from 1 (the engine's counter, which is
     * our net.frame): frame 0 is never exchanged, and a peer's first packet
     * carries 1 .. delay as neutral pads plus the first real one. */
    s_slippi_sent_upto = 0;
    s_slippi_ever_heard = false;
    s_slippi_on = true;
    net.slippi = true;
    net.sock = SOCK_INVALID;
    /* Agreed already: nothing to handshake, no scene hand-offs in this
     * session, the engine counts frames from here. */
    net.hs = HS_DONE;
    net.hs_host = false;
    net.start_frame = 0;
    /* A peer's checksum for frame 0 is not stamped before it has run it, so
     * frame 0 reads as zero and would be reported as a desync. */
    net.ck_from = 1;
    s_slippi_match_only = true;
    net.active = true;
    pc_log_line("net: Slippi match: P%d, rngOffset %u, delay %s%d", net.local + 1, rng_offset,
        net.delay_auto ? "auto " : "", net.delay);
    return true;
#endif
}

bool pc_net_slippi_unadopt(void) {
#ifdef __EMSCRIPTEN__
    return false;
#else
    if (!s_slippi_on || s_slippi.net == NULL || s_status != PC_NET_PEER_OK ||
        pc_slippi_net_status(s_slippi.net) != PC_SLIPPI_NET_CONNECTED) {
        return false;
    }
    /* The engine stops; the ENet peer stays up and goes back to being the
     * link the next game's selections cross (Slippi's Direct lobby). */
    s_link = s_slippi.net;
    s_slippi.net = NULL;
    s_slippi_on = false;
    s_slippi_match_only = false;
    net.slippi = false;
    net.active = false;
    net.resim = false;
    net.hs = HS_IDLE;
    rules_restore();
    HSD_PadLibData.qtype = 0;
    snaps_free();
    pc_log_line("net: Slippi game over at frame %d; the link stays up for the next one",
        net.tick_frame);
    return true;
#endif
}

bool pc_net_connect_socket(
    intptr_t socket, const char* ip, uint16_t port, int player, uint32_t seed) {
    if (socket == -1)
        return false;
    PeerSpec peer = {ip, port, player ? 0 : 1};
    return connect_impl((sock_t)socket, &peer, 1, player ? 1 : 0, seed);
}

bool pc_net_connect_links(
    intptr_t socket, const PcNetLink* links, int n, int local, uint32_t seed) {
    if (socket == -1 || n < 1 || n > MAX_PEERS) {
        return false;
    }
    static char ips[MAX_PEERS][INET_ADDRSTRLEN];
    PeerSpec peers[MAX_PEERS];
    for (int i = 0; i < n; i++) {
        struct in_addr a;
        a.s_addr = links[i].ip;
        inet_ntop(AF_INET, &a, ips[i], sizeof ips[i]);
        peers[i] = (PeerSpec){ips[i], links[i].port, links[i].player, links[i].key,
            links[i].via_host};
    }
    if (!connect_impl((sock_t)socket, peers, n, local, seed)) {
        return false;
    }
    net.direct = true; /* the session agrees its own match, as MELEE_NET_PEERS's */
    return true;
}

/* MELEE_NET_PEERS="0=host:port,1=host:port,2=host:port": a direct session of
 * up to four players, every one listed by controller port (this side's own
 * entry, MELEE_NET_PLAYER's, is skipped). False when unset or malformed. */
static bool peers_from_env(uint32_t seed) {
    const char* spec = getenv("MELEE_NET_PEERS");
    if (spec == NULL || spec[0] == '\0') {
        return false;
    }
    const char* player = getenv("MELEE_NET_PLAYER");
    int local = player != NULL ? atoi(player) : 0;
    static char hosts[4][256];
    PeerSpec peers[MAX_PEERS];
    int n = 0;
    for (const char* q = spec; *q != '\0';) {
        const char* end = strchr(q, ',');
        size_t len = end != NULL ? (size_t)(end - q) : strlen(q);
        const char* eq = memchr(q, '=', len);
        const char* colon = NULL;
        for (const char* c = q; c < q + len; c++) {
            colon = *c == ':' ? c : colon;
        }
        int port = eq != NULL ? atoi(q) : -1;
        if (eq == NULL || colon == NULL || colon < eq || port < 0 || port > 3 ||
            (size_t)(colon - eq - 1) >= sizeof hosts[0])
        {
            pc_log_line("net: MELEE_NET_PEERS wants port=host:port,..., not %s", spec);
            return false;
        }
        if (port != local) {
            if (n == MAX_PEERS) {
                pc_log_line("net: MELEE_NET_PEERS lists more than %d other players", MAX_PEERS);
                return false;
            }
            memcpy(hosts[port], eq + 1, (size_t)(colon - eq - 1));
            hosts[port][colon - eq - 1] = '\0';
            peers[n].ip = hosts[port];
            peers[n].port = (uint16_t)atoi(colon + 1);
            peers[n].player = port;
            n++;
        }
        q = end != NULL ? end + 1 : q + len;
    }
    return connect_impl(SOCK_INVALID, peers, n, local, seed);
}

void pc_net_init(void) {
    s_game_thread = SDL_GetCurrentThreadID(); /* pc_platform_init runs on it */
    if (pc_xn_punch_from_env()) {
        return; /* XeleeNet: a lobby code instead of addresses */
    }
    const char* peer = getenv("MELEE_NET");
    const char* many = getenv("MELEE_NET_PEERS");
    if (many != NULL && many[0] != '\0') {
        const char* seed = getenv("MELEE_SEED");
        if (!peers_from_env(seed ? (uint32_t)strtoul(seed, NULL, 0) : 0)) {
            return;
        }
        net.direct = true; /* the session agrees its own match, as below */
        return;
    }
    if (peer == NULL || peer[0] == '\0') {
        return;
    }
    char host[256];
    const char* colon = strrchr(peer, ':');
    if (colon == NULL || (size_t)(colon - peer) >= sizeof host) {
        pc_log_line("net: MELEE_NET must be host:port");
        return;
    }
    memcpy(host, peer, (size_t)(colon - peer));
    host[colon - peer] = '\0';
    const char* player = getenv("MELEE_NET_PLAYER");
    const char* seed = getenv("MELEE_SEED");
    const int is_p2 = player != NULL && player[0] == '1';
    const uint32_t seed_val = seed ? (uint32_t)strtoul(seed, NULL, 0) : 0;
    /* MELEE_NET_STACK=slippi selects the Slippi ENet transport (Slppi online
     * services), legacy (default) the custom UDP path. The game-visible
     * session is the same either way. */
    const char* stack = getenv("MELEE_NET_STACK");
    if (stack != NULL && strcmp(stack, "slippi") == 0) {
        const char* lport = getenv("MELEE_NET_PORT");
        uint16_t udp_port = (uint16_t)(lport ? atoi(lport) : 41000);
        if (!pc_net_connect_slippi(host, (uint16_t)atoi(colon + 1), is_p2, seed_val, udp_port)) {
            return;
        }
        return;
    }
    if (!pc_net_connect(host, (uint16_t)atoi(colon + 1), is_p2, seed_val)) {
        return;
    }
    /* No lobby agreed this match, so the session agrees it itself: the same
     * RULES/READY exchange a lobby session runs, hosted by player 1, fired
     * from the frame loop once the game has filled its own rules in
     * (handshake_direct, net_handshake.c). Everything match-affecting is
     * therefore validated rather than assumed, including the seed -- the
     * host's wins, so MELEE_SEED no longer has to be set, or set equal, on
     * both peers. */
    net.direct = true;
    /* Said plainly rather than left to be inferred from a missing line: the
     * key arrives with the handshake, so until it completes there is nothing
     * to authenticate these datagrams with and anything that can reach this
     * port can inject input, acks, a delay change or a BYE. A shared secret
     * closes that window from the first packet. */
    const char* key = getenv("MELEE_NET_KEY");
    if (key == NULL || key[0] == '\0') {
        pc_log_line("net: this MELEE_NET session is UNAUTHENTICATED until its handshake "
                    "completes; set the same MELEE_NET_KEY on both peers to authenticate its "
                    "datagrams from the first one");
    }
}

int32_t pc_net_start_frame(void) {
    return net.active ? net.start_frame : -1;
}

void pc_net_poll(void) {
    if (!net.active) {
        return;
    }
    recv_inputs();
    send_inputs(); /* includes reliable retransmits while simulation is parked */
    if (s_peer_left) {
        pc_net_disconnect();
    }
}

int32_t pc_net_frame(void) {
    return net.tick_frame;
}

bool pc_net_stats(int* ping_ms, int* delay_frames, unsigned* rollbacks) {
    if (!net.active) {
        return false;
    }
    *ping_ms = (int)(net.ping_us / 1000);
    *delay_frames = net.delay;
    *rollbacks = s_rollbacks;
    return true;
}

/* ---- audio answers the simulation is allowed to see --------------------
 * pc/net.h has the why. One journal per frame in a RING-deep ring, replayed
 * in call order: the order of audio questions inside a tick is a function
 * of the state and the inputs, so the answer sequence is too. Journalling
 * is armed only between the start of a tick's frame and the end of that
 * tick, so a query from the render phase or the audio thread cannot shift
 * the sequence. */
#define AUDIO_J 32
static struct AudioJournal {
    int32_t frame;
    uint8_t n;
    int32_t v[AUDIO_J];
} s_aj[RING];
static uint8_t s_aj_i;                   /* answers replayed/recorded so far this tick */
static bool s_aj_on;                     /* a tick is running */
static unsigned s_aj_over, s_aj_replays; /* answers that did not fit / were replayed */
static int s_aj_off = -1;                /* MELEE_NET_AUDIO_JOURNAL=off */

static bool audio_journal_live(void) {
    if (s_aj_off < 0) {
        const char* e = getenv("MELEE_NET_AUDIO_JOURNAL");
        s_aj_off = e != NULL && strcmp(e, "off") == 0;
    }
    return s_aj_off == 0 && s_aj_on && net.active && SDL_GetCurrentThreadID() == s_game_thread;
}

/* MELEE_NET_SFX_LOG=off: sound starts go back to the journal below. */
static int s_sfx_off = -1;

bool pc_net_sfx_on(void) {
    if (SDL_GetCurrentThreadID() != s_game_thread) {
        return false; /* the audio thread keys voices off too */
    }
    if (s_sfx_off < 0) {
        const char* e = getenv("MELEE_NET_SFX_LOG");
        s_sfx_off = e != NULL && strcmp(e, "off") == 0;
    }
    return s_sfx_off == 0 && net.active;
}

/* A frame's simulation begins (a fresh tick or a re-run of it). */
static void audio_journal_begin(int32_t f) {
    s_aj_i = 0;
    s_aj_on = true;
    if (net.active) {
        net_sfx_begin(f, net.resim); /* a re-run of an older frame starts a rollback */
    }
    struct AudioJournal* j = &s_aj[f & (RING - 1)];
    if (j->frame != f) {
        j->frame = f;
        j->n = 0;
    }
}

bool pc_net_audio_replay(int32_t* out) {
    if (!audio_journal_live()) {
        return false;
    }
    struct AudioJournal* j = &s_aj[net.tick_frame & (RING - 1)];
    if (j->frame != net.tick_frame || s_aj_i >= j->n) {
        return false;
    }
    *out = j->v[s_aj_i++];
    s_aj_replays++;
    return true;
}

int32_t pc_net_audio_record(int32_t v) {
    if (!audio_journal_live()) {
        return v;
    }
    struct AudioJournal* j = &s_aj[net.tick_frame & (RING - 1)];
    if (j->frame == net.tick_frame && j->n == s_aj_i && s_aj_i < AUDIO_J) {
        j->v[s_aj_i] = v;
        j->n = ++s_aj_i;
    } else {
        s_aj_over++;
    }
    return v;
}

/* "Is that voice still playing" during a session: the simulation is told no,
 * because the true answer is a wall-clock fact and the two peers' audio
 * clocks are not the same clock (axdriver.c AXDriver_8038D9D8 has the why).
 * False outside a session, so offline play keeps the real answer.
 * MELEE_NET_AUDIO_DEAF=off restores it and is the regression test for the
 * desync it causes. s_deaf_true counts how often the engine would have said
 * "still playing", which is the size of the behaviour change. */
static int s_deaf_off = -1;
static unsigned s_deaf_asks, s_deaf_true;

bool pc_net_audio_deaf(bool* answer) {
    if (s_deaf_off < 0) {
        const char* e = getenv("MELEE_NET_AUDIO_DEAF");
        s_deaf_off = e != NULL && strcmp(e, "off") == 0;
    }
    if (s_deaf_off != 0 || !net.active || SDL_GetCurrentThreadID() != s_game_thread) {
        return false;
    }
    s_deaf_asks++;
    *answer = false;
    return true;
}

/* What the engine would have answered, for the report: called by axdriver.c
 * only when the choke is on and only to measure it. */
void pc_net_audio_deaf_note(bool live) {
    if (live) {
        s_deaf_true++;
    }
}

/* The sound log's numbers for the 600-frame report: session totals. */
static void sfx_stats_report(void) {
    NetSfxStats st;
    net_sfx_stats(&st);
    pc_log_line("net: sfx played %u, resim deduped %u, late %u, killed %u, cancelled %u "
                "(revived %u), shielded %u, outside ticks %u, audio-private %u, overflow %u",
        st.played, st.deduped, st.late, st.killed, st.cancelled, st.revived, st.shielded,
        st.outside, st.private_starts, st.overflow);
}

static void audio_journal_reset(void) {
    net_sfx_reset();
    memset(s_aj, 0, sizeof s_aj);
    s_aj_i = 0;
    s_aj_on = false;
    s_aj_over = 0;
    s_aj_replays = 0;
    s_deaf_asks = 0;
    s_deaf_true = 0;
}

/* Only the sound path asks (axdriver.c AXDriver_8038CFF4, lbaudio_ax.c
 * lbAudioAx_80023F28): a re-simulated frame must not start its sounds a
 * second time. That choke is the one thing a re-simulated frame does
 * differently from a fresh one, so MELEE_NET_RESIM_SFX=1 lifts it: a row
 * that desyncs with the choke and survives without it has its divergence
 * inside the sound path, not in the physics. */
bool pc_net_resim(void) {
    static int choke = -1;
    if (choke < 0) {
        const char* e = getenv("MELEE_NET_RESIM_SFX");
        choke = e != NULL && e[0] == '1' ? 0 : 1;
    }
    return choke != 0 && (net.resim || net.synctest);
}

/* Every input wait, including a failed snapshot, has the same disconnect
 * handling. A failed snapshot must not allow a speculative tick to run. */
static bool wait_input(int32_t need, const char* reason) {
    int32_t have = s_remote_have;
    uint64_t t0 = SDL_GetTicksNS();
    bool ready = wait_remote(need);
    uint64_t waited = SDL_GetTicksNS() - t0;
    if (waited >= 33000000ull && getenv("MELEE_NET_DEBUG") != NULL) {
        pc_log_line("net timing: input wait %.1f ms at frame %d need %d have %d -> %d (%s)",
            waited / 1e6, net.frame, need, have, s_remote_have, reason);
    }
    if (ready) {
        net.waited_ns += waited; /* not lateness: pc_net_catch_up_ns */
        return true;
    }
    /* A refused resume logged its own reason, and the peer was not
     * silent at all: its answer was simply unusable. */
    if (!s_peer_left && s_rc != RSM_FAILED) {
        if (s_no_progress) {
            pc_log_line("net: peer still sending but stuck at frame %d for %d ms, leaving netplay",
                s_remote_newest, NO_PROGRESS_TIMEOUT_MS);
        } else {
            pc_log_line("net: peer silent for %d ms at frame %d, leaving netplay",
                rx_heard_ns() ? STALL_TIMEOUT_MS : connect_timeout_ms(), net.frame);
        }
    }
    /* The BYE can arrive while the game thread is parked in
     * wait_remote() rather than in recv_inputs() above, and that is the
     * usual case once one-way delay is high enough to stall every
     * frame. Without this the stalling side never ends the test and the
     * harness kills it, which reads as a netplay failure. */
    if (s_peer_left) {
        exit_if_test_done();
    }
    pc_net_disconnect();
    return false;
}

/* ---- rollback ---------------------------------------------------------
 * One snapshot per predicted frame (Slippi cadence), taken right before the
 * tick that consumes the prediction. When the real input for such a frame
 * arrives and differs, that frame's snapshot is restored and every frame
 * since is ticked again from the input rings with the resim flag on. */

/* Slippi's "game confirmed over" (StartEngineLoop.asm): once the match result
 * has held for more than its rollback limit, no rollback can undo it, and
 * from then on Dolphin neither stalls nor rolls back nor needs our inputs for
 * the GAME! frames that remain. It stops sending its own as well, since a
 * peer at its rollback limit keeps advancing without one. Waiting for those
 * inputs here deadlocked the end of a match whenever Dolphin reached the end
 * already at its limit (a 90 ms link: both sides froze and the set dropped
 * on the silence timeout). So the end is confirmed here by the same rule, and
 * the frames after it run on predictions with nothing to wait for.
 *
 * One condition stricter than Slippi's: the result's own frame must be backed
 * by the peer's real inputs with no rollback pending, so everything up to it
 * is final (check_desync still compares up to there). The peer sent those:
 * it simulated that frame itself, and a frame it simulates is a frame whose
 * input it sends. */
static void game_over_note(int32_t f) {
    if (!s_slippi_on || s_game_over) {
        return;
    }
    if (!in_fight() || gmVs_GetSceneState()->match_result == 0) {
        s_end_frame = -1;
        return;
    }
    if (s_end_frame < 0) {
        s_end_frame = f;
    }
    if (f - s_end_frame > WINDOW && s_remote_have >= s_end_frame && s_rb_frame < 0) {
        s_game_over = true;
        pc_log_line("net: game confirmed over at frame %d (result from frame %d)", f,
            s_end_frame);
    }
}

/* Only the fight it was confirmed in: a scene after it waits as usual. */
static bool game_over(void) {
    return s_game_over && in_fight();
}

/* Every remote port without real input for frame f repeats its newest real
 * one (neutral before it has any). */
static void predict(int32_t f) {
    static const WirePad neutral;
    for (int p = 0; p < PORTS; p++) {
        if ((s_remote_mask & (1u << p)) && s_port_have[p] < f && s_port_end[p] < 0) {
            s_remote_ring[p][f & (RING - 1)] =
                s_port_have[p] >= 0 ? s_remote_ring[p][s_port_have[p] & (RING - 1)] : neutral;
        }
    }
}

/* Snapshot before a predicted frame. Behind the barrier nothing is taken
 * (the frame cannot be rolled back to anyway); when the buffer cannot be
 * grown, disable further prediction without invalidating older snapshots.
 * The caller must wait for real input before running this frame. */
static bool snap_predicted(int32_t f) {
    Snapshot* s = snap_slot(f);
    if (s_lockstep || f <= net.rb_barrier) {
        return false;
    }
    if (!snapshot_take(s, f)) {
        /* A DVD/ARQ transfer in flight: this frame waits for the real input
         * and the next one predicts again. Taking the session to lockstep
         * for that (and calling it out of memory) turned one refused take
         * into a whole match of full-length stalls, in 2 of 4
         * tools/net_test.py --hitch runs. */
        if (snapshot_refused_io()) {
            return false;
        }
        /* Out of memory, or a platform whose linker cannot bracket the
         * decomp's statics at all (net_snapshot.c). */
        const char* why = snapshot_state_region_missing();
        s_lockstep = true;
        pc_log_line("net: %s at frame %d, lockstep from here",
            why != NULL ? why : "out of memory for snapshots", f);
        return false;
    }
    return true;
}

/* MELEE_NET_RESIM_AUDIT_POISON=1: the audit's discarded pass runs with a
 * deliberately wrong remote input, which is what a real misprediction is.
 * The true re-run after it must still land exactly where the first pass
 * did; anything the wrong pass left behind that the snapshot does not
 * cover shows up there and nowhere else. */
static bool s_audit_poison;      /* a wrong-input pass is running now */
static bool s_audit_poison_want; /* and the knob that asks for one */

/* The queue slot HSD_PadRenewMasterStatus will consume next. */
static PADStatus* pad_head(void) {
    PadLibData* p = &HSD_PadLibData;
    return &p->queue->stat[p->qread * 4];
}

/* The raw pad queue must never move its read cursor under the inputs a tick
 * is about to consume. With qtype 0 a full queue makes HSD_PadRenewRawStatus
 * shift qread, merge the dropped sample's buttons into the next one and
 * overwrite the slot write_head just filled (controller.c:77-105); the tick
 * then simulates a raw local sample -- the remote port reads as "no
 * controller", the local port as the undelayed physical pad -- while the
 * frame's checksum still reports the synced inputs. One such frame on one
 * peer is a permanent divergence.
 *
 * pc_net_connect sets qtype 2 (drop the new sample instead), but in
 * MELEE_NET mode it connects from pc_platform_init, and the game's own
 * gmMain_8015FD24 later runs HSD_PadInit, which copies default_libinfo_data
 * over the whole PadLibData and puts qtype back to 0. So the invariant is
 * re-asserted every tick: one byte, and it cannot be lost by a re-init.
 * MELEE_NET_PAD_QTYPE=0 restores the shifting queue and is the regression
 * test for the desync it causes. */
static void pad_qtype_hold(void) {
    static int want = -1;
    if (want < 0) {
        const char* e = getenv("MELEE_NET_PAD_QTYPE");
        want = e != NULL ? atoi(e) : 2;
        if (want != 2) {
            pc_log_line(
                "net: raw pad queue type %d (MELEE_NET_PAD_QTYPE), expect input slips", want);
        }
    }
    HSD_PadLibData.qtype = (uint8_t)want;
}

/* Detector for the same hazard, kept as the regression test: what
 * write_head put in the queue head for the frame about to be simulated,
 * against what the tick actually consumed. Both must agree on every tick of
 * a session; a mismatch is the divergence above, and the counters below go
 * into the periodic report so a run can be believed. */
static uint16_t s_head_button[4];
static int32_t s_head_frame = -1;
static const PADStatus* s_head_ptr; /* slot write_head filled */
static uint8_t s_head_qread, s_head_qwrite, s_head_qcount;
static uint32_t s_head_retrace; /* frame boundaries seen at the write */
static unsigned s_pad_slips, s_pad_full, s_tick_idle;
static uint8_t s_qdepth_max;

static void pad_stats_reset(void) {
    s_pad_slips = 0;
    s_pad_full = 0;
    s_tick_idle = 0;
    s_qdepth_max = 0;
    s_head_frame = -1;
    net.pad_reuse = 0;
    net.pad_empty = 0;
}

static void head_note(const PADStatus* head, int32_t f) {
    const PadLibData* p = &HSD_PadLibData;
    for (int i = 0; i < 4; i++) {
        s_head_button[i] = head[i].button;
    }
    s_head_frame = f;
    s_head_ptr = head;
    s_head_qread = p->qread;
    s_head_qwrite = p->qwrite;
    s_head_qcount = p->qcount;
    s_head_retrace = VIGetRetraceCount();
    if (p->qcount >= p->qnum) {
        s_pad_full++;
    }
    if (p->qcount > s_qdepth_max) {
        s_qdepth_max = p->qcount;
    }
}

static void head_check(void) {
    if (s_head_frame < 0 || !net.active) {
        return;
    }
    const PadLibData* p = &HSD_PadLibData;
    for (int port = 0; port < PORTS; port++) {
        if (port != net.local && !(s_remote_mask & (1u << port))) {
            continue;
        }
        /* HSD_PadADConvert ORs synthetic direction bits above 0xffff into
         * button (controller.c), so only the real pad bits can be compared. */
        uint32_t saw = HSD_PadMasterStatus[port].button & 0xffffu;
        if (saw != s_head_button[port]) {
            s_pad_slips++;
            if (s_pad_slips <= 8) {
                /* The slot the tick consumed is one behind the read cursor it
                 * left behind; if that is not the slot write_head filled,
                 * something moved the queue in between. */
                int read_slot = (p->qread + p->qnum - 1) % p->qnum;
                pc_log_line("net: pad slip at frame %d port %d: wrote %04x, tick consumed %04x "
                            "(wrote slot %d of %d at qcount %d/%d qwrite %d; tick read slot %d, "
                            "now qread %d qcount %d qwrite %d; retrace %u -> %u)",
                    s_head_frame, port, s_head_button[port], saw, s_head_qread, p->qnum,
                    s_head_qcount, p->qnum, s_head_qwrite, read_slot, p->qread, p->qcount,
                    p->qwrite, s_head_retrace, VIGetRetraceCount());
            }
        }
    }
    /* And did the tick advance the simulation at all? gm_RunSimTick only
     * runs gm_EvaluateAllControllerInputs and the scene's frame proc when
     * lb_80019A30(0) is set, which lb_80019900 recomputes from an
     * accumulator in lb_0195.c -- a TU the snapshot deliberately excludes.
     * At 60 Hz it is true every tick; if a re-run tick ever lands on a
     * false one, that frame was counted but never simulated. */
    if (!lb_80019A30(0)) {
        s_tick_idle++;
        if (s_tick_idle <= 4) {
            pc_log_line(
                "net: tick at frame %d did not advance the sim (lb_80019A30 false)", s_head_frame);
        }
    }
    s_head_frame = -1;
}

/* Slippi's SyncRNG (InitOnlinePlay.asm): in an online fight the seed
 * entering every tick is a function of the agreed seed and the frame alone.
 * Otherwise one extra draw on one peer -- an out-of-tick call, a path only
 * one timeline took -- leaves the two streams a draw apart for the rest of
 * the match, which is what the Windows desync in plan section 5.1 was
 * (fighters identical, seed one draw apart). Reseeded, a divergence that
 * touched nothing but the seed heals on the next tick, and one that did
 * touch state still shows in the fighter fields and the full-state hash.
 * Applied before the checksum and the recording read the seed, on fresh and
 * re-run ticks alike, so a replay restoring the recorded seed agrees. The
 * mix is murmur3's finalizer: consecutive frames get unrelated seeds. */
static void fight_reseed(int32_t f) {
    if (!net.active || net.hs != HS_DONE || f <= net.start_frame || !in_fight()) {
        return;
    }
    if (pc_net_slippi_active()) {
        return; /* Slippi's own SyncRNG process does this (gmvs.c) */
    }
    uint32_t x = net.seed ^ ((uint32_t)f * 0x9e3779b9u);
    x ^= x >> 16;
    x *= 0x85ebca6bu;
    x ^= x >> 13;
    x *= 0xc2b2ae35u;
    x ^= x >> 16;
    *HSD_RandSeedPtr = x;
}

/* Ports 0-3 of the queue head become the synced inputs for frame f. */
static void write_head(PADStatus* head, int32_t f) {
    static const WirePad neutral;
    const WirePad* mine = f >= net.delay ? &s_local_ring[f & (RING - 1)] : &neutral;
    for (int p = 0; p < PORTS; p++) {
        if (p == net.local) {
            from_wire(&head[p], mine);
            continue;
        }
        if (!(s_remote_mask & (1u << p))) {
            memset(&head[p], 0, sizeof head[p]);
            head[p].err = PAD_ERR_NO_CONTROLLER;
            continue;
        }
        static const WirePad released;
        const WirePad* theirs =
            s_port_end[p] >= 0 && f > s_port_end[p] ? &released : &s_remote_ring[p][f & (RING - 1)];
        WirePad wrong;
        if (s_audit_poison) {
            wrong = *theirs;
            wrong.stickX = (int8_t)-wrong.stickX;
            wrong.button ^= 0x0100; /* A */
            theirs = &wrong;
        }
        from_wire(&head[p], theirs);
        if (!s_seen_remote && theirs->button != 0) {
            s_seen_remote = true;
            pc_log_line("net: first remote button press (%04x) at frame %d", theirs->button, f);
        }
    }
    head_note(head, f);
}

/* Re-expose the slot the last tick consumed so the next tick has one to
 * consume; the raw sample in it was already captured. Fails when full. */
static PADStatus* unconsume(void) {
    PadLibData* p = &HSD_PadLibData;
    if (p->qcount >= p->qnum) {
        return NULL;
    }
    p->qread = (uint8_t)((p->qread + p->qnum - 1) % p->qnum);
    p->qcount++;
    return &p->queue->stat[p->qread * 4];
}

/* Set up the re-run of frame f: refresh its snapshot if it is still
 * predicted (the old one holds the discarded timeline), then feed it. */
static bool resim_prepare(int32_t f) {
    if (f > s_remote_have) {
        if (!snap_predicted(f) && !wait_input(f, "replay snapshot")) {
            return false;
        }
        if (f > s_remote_have) {
            predict(f);
        }
    }
    PADStatus* head = unconsume();
    if (head == NULL) {
        /* ponytail: the pad queue is full (a hitch queued several samples
         * during the rollback) and the game reads the queue head directly
         * through HSD_PadRenewMasterStatus (controller.c), not a pointer we
         * own, so a private re-run buffer would never be read. This re-run
         * therefore reuses the live head, eating one real sample; time sync
         * pays it back. Counted (resim_eat) so the report shows how often. */
        PadLibData* p = &HSD_PadLibData;
        head = &p->queue->stat[p->qread * 4];
        s_resim_eat++;
        if (!s_resim_eat_logged) {
            s_resim_eat_logged = true;
            pc_log_line("net: pad queue full during a rollback at frame %d, re-run reused a live "
                        "sample",
                f);
        }
    }
    write_head(head, f);
    fight_reseed(f);
    s_ck_ring[f & (RING - 1)] = frame_checksum(head);
    if (net.active && pc_net_slippi_active()) {
        s_wire_ck_ring[f & (RING - 1)] = pc_slippi_wire_checksum();
    }
    /* The state ring is the last simulation of each frame, which is the
     * confirmed timeline (the desync dump and the audit's field-level diff
     * both read it); without this it held first-pass values only. The
     * recording wants the same treatment: this re-run replaces the staged
     * record for f, so what reaches the file is the pass that stood. */
    record_state(head, f);
    record_frame(head, s_ck_ring[f & (RING - 1)], f);
    if (s_sim_n[f & (RING - 1)] < 255) {
        s_sim_n[f & (RING - 1)]++;
    }
    net.tick_frame = f;
    audio_journal_begin(f); /* the re-run replays this frame's audio answers */
    return true;
}

static bool rollback_to(int32_t f) {
    Snapshot* s = snap_slot(f);
    const char* why = s->buf == NULL || s->frame != f ? "no snapshot" :
                      f <= net.rb_barrier             ? "behind the barrier" :
                                                        snapshot_unusable(s);
    if (why != NULL) {
        /* The misprediction stays in the timeline: the peers have diverged
         * unless the inputs happened to match. Everything simulated so far
         * is now behind the barrier so it is not rolled back to later. */
        s_rb_lost++;
        barrier_raise(simulated_upto());
        if (!s_rb_lost_logged) {
            s_rb_lost_logged = true;
            pc_log_line("net: cannot roll back to frame %d (%s), expect a desync", f, why);
        }
        return false;
    }
    int depth = simulated_upto() - f + 1;
    /* The pad queue and its bookkeeping sit inside the snapshot but belong
     * to the present: keep the live copy so raw samples queued since the
     * snapshot survive, and the re-run ticks consume slots unconsume() hands
     * them. Interrupts stay off so the pad alarm cannot land in between. */
    bool intr = OSDisableInterrupts();
    PadLibData pad = HSD_PadLibData;
    HSD_PadData queue[8];
    int qn = pad.qnum > 8 ? 8 : pad.qnum;
    memcpy(queue, pad.queue, qn * sizeof *queue);
    snapshot_restore(s);
    memcpy(pad.queue, queue, qn * sizeof *queue);
    /* Rumble nodes and active heads were rewound: their free-list head must
     * come from that same snapshot, not the discarded live timeline. */
    pad.rumble_info = HSD_PadLibData.rumble_info;
    HSD_PadLibData = pad;
    OSRestoreInterrupts(intr);
    s_rollbacks++;
    s_rb_recent[s_rb_recent_n++ % 16] = f;
    if (depth > s_rb_depth_max) {
        s_rb_depth_max = depth;
    }
    if (depth > s_rb_depth_cur) {
        s_rb_depth_cur = depth;
    }
    net.resim = true;
    return resim_prepare(f);
}

/* ---- per-tick entry --------------------------------------------------- */

/* Draws of random.c's LCG between two seeds, -1 past the bound. A
 * re-simulation that took a different branch usually shows up here first:
 * same fighter positions, a seed a few draws apart. */
static int seed_steps(uint32_t from, uint32_t to) {
    for (int i = 0; i <= 4096; i++) {
        if (from == to) {
            return i;
        }
        from = from * 214013u + 2531011u;
    }
    return -1;
}

/* Seed at the end of the last tick of a present, and how far it had moved
 * by the start of the next one: everything between those two points is
 * outside every tick (the render phase, the audio thread), so a draw there
 * is state the rollback re-run can never reproduce. */
static unsigned s_seed_out_draws, s_seed_out_frames;

static void seed_stats_reset(void) {
    s_seed_out_draws = 0;
    s_seed_out_frames = 0;
}

/* Called at the top of a tick. The seed is folded into the netplay checksum
 * (net_snapshot.c:148) and outside a fight the checksum is ONLY the pads and
 * this seed, so a draw between two ticks that happens on one peer and not
 * the other is by itself a desync.
 *
 * Two separate causes, both measured, both fixed:
 *
 * 1. The wall clock. gmtitle.c:161-170 stirred the RNG once per
 *    seconds-of-minute, so two machines that reached the title eight
 *    wall-seconds apart stirred 41 times against 49 (phone<->PC). Fixed at
 *    the source, because no repair here can invent the draws the other peer
 *    made.
 *
 * 2. Scene timing. The rest are scene-enter draws -- stage init, CPU AI init
 *    inside Fighter_Create -- backtraced across three 9900-frame sessions
 *    and never paced by rendering. Two peers of similar speed make them on
 *    the same frame, but a slow device does not: an SM-T505 against this PC
 *    drew at frame 232 where the PC drew later, and desynced there. The seed
 *    a tick starts from must therefore be the seed the last tick ended with,
 *    whatever the gap between them contained.
 *
 * Netplay only (`net.active`). A recording leg must NOT do this: whether a
 * draw falls inside or outside a tick depends on load timing, so rewriting
 * the seed makes a recording that only replays on a machine of the same
 * speed -- measured, it moved the replay divergence to frame 1. Netplay is
 * the opposite case: both peers run one frame stream and the repair is
 * applied identically on each, which is what makes them agree. */
static void seed_out_of_tick_check(void) {
    if (!s_seed_have) {
        return;
    }
    uint32_t now = *HSD_RandSeedPtr;
    if (now == s_seed_after_tick) {
        return;
    }
    int steps = seed_steps(s_seed_after_tick, now);
    s_seed_out_frames++;
    s_seed_out_draws += steps > 0 ? (unsigned)steps : 0;
    if (s_seed_out_frames <= 3) {
        pc_log_line("net: seed moved outside the tick at frame %d: %08x -> %08x (%d draws)%s",
            net.frame, s_seed_after_tick, now, steps, net.active ? ", restoring" : "");
    }
    if (net.active) {
        *HSD_RandSeedPtr = s_seed_after_tick;
    }
}

/* How many TICKS a load costs must not depend on this machine's disc speed.
 * The game polls a read's completion once per tick, so a peer whose file
 * cache is warm finishes in one tick while the other spends hundreds --
 * measured tablet<->PC on a real match: the tablet entered the fight at
 * frame 39470 and this PC at 40353, 883 frames (~15 s) later, with an
 * identical seed and identical pads, and the session desynced on the first
 * frame `frame_checksum` folded fighter fields for (it folds them only while
 * in_fight()). Draining the queue before the tick makes the read complete
 * inside the tick that issued it, so the poll succeeds on the same tick for
 * both peers.
 *
 * Netplay only, and bounded: a wedged read must not hang the game, and while
 * this blocks, the peer simply sees the stall that wait_remote() is built to
 * ride out. */
static void dvd_settle(void) {
    if (aurora_dvd_inflight() <= 0 && aurora_arq_inflight() <= 0) {
        return;
    }
    const uint64_t t0 = SDL_GetTicksNS();
    static bool warned;
    while (aurora_dvd_inflight() > 0 || aurora_arq_inflight() > 0) {
        if (SDL_GetTicksNS() - t0 > 5000000000ull) {
            /* The transfer is not going to settle in time. Letting the tick
             * run here was the dangerous half of this: past this point the
             * two peers can be simulating the same frame with different I/O
             * completed, and nothing downstream would notice until the
             * checksum happened to cover whatever the load changed. Run on,
             * because parking the game thread indefinitely is worse, but
             * take rollback off the table for a while: a lockstep frame is
             * still the same frame on both sides, and it cannot be re-run
             * from a snapshot that may predate the completion. */
            barrier_raise(net.frame + IO_QUIET);
            if (!warned) {
                warned = true;
                pc_log_line("net: disc/ARAM transfer still in flight after 5 s at frame %d; "
                            "running on in lockstep to frame %d",
                    net.frame, net.rb_barrier);
            }
            return;
        }
        /* This thread is alive and spinning, not wedged: say so, or the
         * watchdog's 5 s threshold is exactly this loop's own 5 s cap and a
         * disc drain reports a healthy thread (and dumps a stack from inside
         * whatever it happens to hold). */
        net_watchdog_heartbeat();
        SDL_DelayNS(200000);
    }
}

/* MELEE_NET_STALL_TEST=frame[:ms] (fixture): park the game thread mid-match,
 * standing in for a load the netcode cannot shorten -- a stage, a character,
 * a first-time shader compile. Only the guest does it, so one exported value
 * stalls exactly one side of a two-process run. tx_timer keeps sending
 * throughout, which is the condition wait_remote() has to tell apart from a
 * peer that is actually gone. */
static void stall_test(void) {
    static bool parsed;
    static int32_t at = -1;
    static int ms = 10000;
    if (!parsed) {
        parsed = true;
        const char* s = getenv("MELEE_NET_STALL_TEST");
        if (s != NULL) {
            const char* colon = strchr(s, ':');
            at = (int32_t)atoi(s);
            if (colon != NULL) {
                ms = atoi(colon + 1);
            }
        }
    }
    if (at >= 0 && net.frame == at && net.local == 1) {
        pc_log_line("net: stall test: game thread asleep %d ms at frame %d", ms, at);
        SDL_Delay((Uint32)ms);
    }
}

/* MELEE_NET_HITCH_TEST=ms:every (fixture): park THIS peer's game thread for
 * `ms` every `every` frames of a fight, standing in for the frame freezes a
 * phone shows mid-match (100-450 ms each, measured in phone<->PC LAN logs).
 * Unlike stall_test it is not gated on the player number: the harness sets
 * it on one instance only. tx_timer keeps resending the newest input
 * throughout, as it does on a real freeze. */
static void hitch_test(void) {
    static int ms = -1, every;
    if (ms < 0) {
        const char* s = getenv("MELEE_NET_HITCH_TEST");
        const char* colon = s != NULL ? strchr(s, ':') : NULL;
        ms = colon != NULL ? atoi(s) : 0;
        every = colon != NULL ? atoi(colon + 1) : 0;
    }
    if (ms > 0 && every > 0 && net.frame % every == 0 && in_fight()) {
        pc_log_line("net: hitch test: game thread asleep %d ms at frame %d", ms, net.frame);
        SDL_Delay((Uint32)ms);
    }
}

/* Read the latest published hardware state immediately before input is
 * sent. Extra time-sync ticks and rollback must reuse their recorded sample;
 * they must never sample hardware a second time. Physical port zero is the
 * local controller even when matchmaking assigned this peer player two. */
static void capture_local_sample(bool fresh) {
    if (fresh) {
        PADStatus pads[4];
        PADRead(pads);
        s_raw_last = pads[0];
    }
}

/* A fresh frame: capture the local sample, exchange inputs, predict or
 * stall, and feed the queue head. `raw` is false for the extra tick a
 * time-sync advance adds, which reuses the last physical sample. */
static void fresh_tick(PADStatus* head, bool raw) {
    if (net.active) {
        /* Part cleanly rather than run the frame counter toward the end of
         * its range; every ring index and every wire field is an absolute
         * int32 frame, and there is no wrap strategy behind them. */
        if (net.frame >= SESSION_MAX_FRAMES) {
            pc_log_line("net: session frame limit reached at %d, disconnecting", net.frame);
            pc_net_disconnect();
            return;
        }
        stall_test();
        hitch_test();
        static int timing_debug = -1;
        static uint64_t sample_send_total, sample_send_max;
        static unsigned sample_send_count;
        if (timing_debug < 0)
            timing_debug = getenv("MELEE_NET_DEBUG") != NULL;
        uint64_t sampled_at = timing_debug && raw ? SDL_GetTicksNS() : 0;
        capture_local_sample(raw);
        /* One slot per tick; a delay change (delay_auto) leaves a gap to fill
         * with the same sample, or already-sent frames that must not move. */
        int64_t target_w = (int64_t)net.frame + net.delay;
        for (int32_t w = s_wrote + 1; (int64_t)w <= target_w; w++) {
            to_wire(&s_local_ring[w & (RING - 1)], &s_raw_last);
            s_wrote = w;
        }
        send_inputs();
        if (sampled_at) {
            uint64_t elapsed = SDL_GetTicksNS() - sampled_at;
            sample_send_total += elapsed;
            if (elapsed > sample_send_max)
                sample_send_max = elapsed;
            if (++sample_send_count == 600) {
                pc_log_line("net timing: input poll-to-send mean %.3f max %.3f ms at frame %d",
                    sample_send_total / 600000000.0, sample_send_max / 1000000.0, net.frame);
                sample_send_count = 0;
                sample_send_total = sample_send_max = 0;
            }
        }
        recv_inputs();
        links_poll();
        if (s_peer_left) {
            exit_if_test_done();
            pc_net_disconnect();
            return;
        }
        /* A scene change tears heaps down and rebuilds them: nothing across
         * it can be restored, and its loads trail into the next frames. */
        int scene = scene_kind();
        if (scene != s_scene_last) {
            /* Both peers must enter a scene on the SAME frame; when they do
             * not, the sims are running different code and the checksum
             * says so a frame later (docs/netcode-plan.md section 5.2). Two
             * peers' logs diffed on this line give the gap directly, which
             * is the measurement any fix for it has to move. */
            pc_log_line("net: scene %d -> %d at frame %d", s_scene_last, scene, net.frame);
            s_scene_last = scene;
            int lead = in_fight() ? FIGHT_ENTRY_LEAD : IO_QUIET;
            barrier_raise(net.frame + lead);
        }
        /* Predict at most WINDOW frames past the remote, and only in a
         * fight past the barrier (plan §10.4: menus lockstep, rollback
         * armed in GS_VS once the match's own loads have gone quiet).
         *
         * Slow (or fast) motion is lockstep as well. A re-run tick calls
         * lb_80019900 again, and its frame-skip accumulator lives in
         * lb_0195.c, which the snapshot deliberately excludes (melee_state.ld)
         * because rewinding the pad alarm makes it queue catch-up ticks. The
         * accumulator is only idempotent per tick while the tick period
         * equals the step, i.e. at game speed 1.0; Slo-Mo Melee halves it
         * (gmslomo.c sets game_speed 0.5 and declares itself GS_VS), so
         * after a rollback of odd depth the two peers advance the scene on
         * opposite ticks -- one simulates a frame the other skips. */
        bool speed_1 = gmVsMelee_StartData.rules.game_speed == 1.0F;
        /* The tick that voids the match (a player left, net.c "routes and
         * drops") must run once, on real input: ending the match is not pure
         * under a re-run (a peer that re-ran it after a misprediction parted
         * from one that ran it once, DESYNC on the next frame). Lockstep on
         * the frame before settles every correction below it, and lockstep on
         * the frame itself leaves nothing that can roll back across it. */
        bool voiding = s_void_frame >= 0 && net.frame >= s_void_frame - 1 && net.frame <= s_void_frame;
        bool lockstep =
            s_lockstep || !in_fight() || net.frame <= net.rb_barrier || !speed_1 || voiding;
        /* The last lockstep frames before this fight can predict: size and
         * page in the rollback ring now, a slot a frame, while the wait below
         * is for the peer anyway (net_snapshot.c snaps_reserve). A barrier
         * further off than the fight's entry lead is rollback switched off
         * (MELEE_NET_ROLLBACK) or a long load, and reserves nothing yet. */
        if (in_fight() && !s_lockstep && speed_1 && net.frame <= net.rb_barrier &&
            net.rb_barrier - net.frame <= FIGHT_ENTRY_LEAD)
        {
            snaps_reserve();
            pure_loads_fetch();
        }
        int32_t need = lockstep ? net.frame : (net.frame >= WINDOW ? net.frame - WINDOW : 0);
        if (game_over()) {
            /* The GAME! frames: nothing left to wait for or roll back. */
            if (s_remote_have < net.frame) {
                predict(net.frame);
            }
        } else if (!wait_input(need, lockstep ? "lockstep" : "window")) {
            return;
        }
        if ((net.frame % SYNC_INTERVAL) == 0 && net.frame > 0 && !game_over()) {
            time_sync();
        }
        if (s_remote_have < net.frame && !game_over()) {
            if (!snap_predicted(net.frame) && !wait_input(net.frame, "snapshot fallback")) {
                return;
            }
            if (s_remote_have < net.frame) {
                predict(net.frame);
            }
        }
        /* Re-resolve the slot the next HSD_PadRenewMasterStatus will read.
         * The pointer this tick started with was taken before wait_remote
         * and snap_predicted, and every OSRestoreInterrupts in between
         * delivers the pad alarms that came due during the stall (src/pc/os.c
         * deliver_pending); with the raw queue full and qtype back to 0 those
         * move qread, and the inputs would then be written into a slot no
         * tick ever reads. pad_qtype_hold() keeps qtype at 2 so the cursor
         * cannot move at all; this is the second lock on the same door. */
        head = pad_head();
        write_head(head, net.frame); /* after anything that can run the pad alarm */
    }
    replay_feed(head);
    pc_slp_play_feed(head); /* MELEE_SLP_PLAY: a Slippi replay's pads (slp_play.c) */

    if (net.start_frame == net.frame && net.hs != HS_FAILED) {
        *HSD_RandSeedPtr = net.seed; /* both peers enter the match from the agreed seed */
    }
    fight_reseed(net.frame);
    uint32_t ck = frame_checksum(head);
    s_ck_ring[net.frame & (RING - 1)] = ck;
    /* Slippi's wire checksum, taken at the same instant as ours, so the
     * desync check under Slippi compares like for like (the two formulas
     * never agree). Only when it can be sent: a legacy session has no wire
     * consumer for it and the gather is not free. */
    if (net.active && pc_net_slippi_active()) {
        s_wire_ck_ring[net.frame & (RING - 1)] = pc_slippi_wire_checksum();
    }
    s_sim_n[net.frame & (RING - 1)] = 1;
    /* No-op unless netplay is running or MELEE_NET_STATE_LOG is set: a solo
     * replay is how a cross-platform divergence is localised. */
    record_state(head, net.frame);
    if (net.active) {
        check_desync(); /* reports and dumps both rings on the first mismatch */
    }
    record_frame(head, ck, net.frame);
    /* Everything the peer has acknowledged can no longer be rolled back, so
     * its staged record is final and can go to the file in frame order. */
    record_confirm(confirmed_frame());

    if ((net.frame % 60) == 0) {
        s_rb_depth_recent = s_rb_depth_cur; /* pc_net_quality: deepest rollback last second */
        s_rb_depth_cur = 0;
        /* Loss over the last second, smoothed over three. Both consumers of
         * this react to a burst -- send_inputs() widens the redundancy and
         * tx_timer() turns the mid-frame resend back on -- so the ten-second
         * window this replaces could answer a burst after it was over.
         *
         * The smoothing is symmetric on purpose. A one-second window is only
         * ~60 acks, so its binomial spread is wide; taking the rise
         * immediately and decaying slowly latches onto the top of that
         * spread instead of its mean, which measured 6% on a link losing 1%
         * and is enough on its own to make pc_net_quality() call a good
         * connection bad. */
        int sent = (int)(net.tx_inputs - s_loss_tx_mark);
        s_loss_tx_mark = net.tx_inputs;
        SDL_LockMutex(s_rx_lock); /* the receive thread counts the acks */
        int acked = (int)(s_rx_acks - s_loss_ack_mark);
        s_loss_ack_mark = s_rx_acks;
        SDL_UnlockMutex(s_rx_lock);
        if (sent > 0) {
            int now_pct = sent > acked ? 100 * (sent - acked) / sent : 0;
            s_loss_pct = (s_loss_pct * 2 + now_pct) / 3;
        }
    }
    if ((net.frame % 600) == 0 && net.frame > 0 && net.active) {
        /* The rx counters are the receive thread's: read and cleared under
         * its lock, which it waits out for one log line every 10 s. */
        SDL_LockMutex(s_rx_lock);
        pc_log_line("net: frame %d, rollbacks %u (max depth %d, lost %u), stalls %u (worst "
                    "%.1f ms), skips %u, advances %u, ping %u ms (avg %.0f, min %u, max %u, "
                    "jitter %.1f), loss %d%% (%u tx %u rx), offset %+.1f ms, remote behind %d, "
                    "barrier %d, quality %d, dup %u reorder %u sock_err %u resim_eat %u red %d, "
                    "drops (bad_src %u bad_sess %u bad_player %u bad_mac %u malformed %u "
                    "would_block %u rx_full %u), "
                    "pad reuse %u empty %u, audio replayed %u over %u, seed out-of-tick %u draws "
                    "in %u frames, pad slips %u (queue worst %u, full at write %u), idle ticks %u, "
                    "audio liveness asked %u (engine would say yes %u)",
            net.frame, s_rollbacks, s_rb_depth_max, s_rb_lost, s_stalls, s_stall_ns_max / 1e6,
            net.skips, s_advances, net.ping_us / 1000,
            s_ping_n ? s_ping_sum / 1000.0 / s_ping_n : 0.0, s_rtt_min / 1000, s_rtt_max / 1000,
            jitter_us() / 1000.0, s_loss_pct, net.tx_pkts, s_rx_pkts, net.offset_last / 1000.0,
            net.frame - 1 - s_remote_have, net.rb_barrier, pc_net_quality(), s_rx_dups,
            s_rx_reorders, s_sock_err, s_resim_eat, s_red_target, s_rx_bad_src, s_rx_bad_sess,
            s_rx_bad_player, s_rx_bad_mac, s_rx_malformed, s_tx_would_block, s_rx_full,
            net.pad_reuse, net.pad_empty, s_aj_replays, s_aj_over, s_seed_out_draws,
            s_seed_out_frames, s_pad_slips, s_qdepth_max, s_pad_full, s_tick_idle, s_deaf_asks,
            s_deaf_true);
        s_stall_ns_max = 0;
        s_ping_sum = 0;
        s_ping_n = 0;
        s_rtt_min = s_rtt_max = 0;
        net.tx_pkts = s_rx_pkts = net.tx_inputs = s_rx_acks = 0;
        s_loss_tx_mark = s_loss_ack_mark = 0; /* the marks index those counters */
        s_rx_dups = s_rx_reorders = s_sock_err = s_resim_eat = 0;
        s_rx_bad_src = s_rx_bad_sess = s_rx_bad_player = s_rx_malformed = s_tx_would_block = 0;
        s_rx_bad_mac = s_rx_full = 0;
        SDL_UnlockMutex(s_rx_lock);
        snap_stats_report();
        sfx_stats_report();
    }
#ifdef MELEE_FP_PERTURB_NAME
    if ((net.frame % 600) == 0 && net.frame > 0) {
        extern unsigned pc_fp_perturb_calls; /* src/pc/libm/pc_perturb.c */
        pc_log_line("net: fp perturb " MELEE_FP_PERTURB_NAME " fired %u times by frame %d",
            pc_fp_perturb_calls, net.frame);
    }
#endif
    net.tick_frame = net.frame;
    audio_journal_begin(net.tick_frame);
    net.frame++;
}

/* ---- re-simulation audit ----------------------------------------------
 * MELEE_NET_RESIM_AUDIT=K: every AUDIT_EVERY frames, roll back K frames and
 * re-run them from the same input rings. Nothing about the inputs changed,
 * so every byte of the state must come back as it was; a block that does
 * not is the re-simulation diverging from the fresh pass on its own, which
 * is a desync waiting for a peer that re-ran a different set of frames.
 *
 * Each audit re-runs the range twice and reports two comparisons: the first
 * re-run against the first pass, and the second re-run against the first
 * re-run. The pair separates the two faults that both look like "the
 * re-simulation diverged": a tick that is not a pure function of (state,
 * inputs) breaks both, while a first pass that did something a re-run never
 * does -- rendered, loaded, ran a frame twice, read a clock -- breaks only
 * the first. Off unless the variable is set. */
#define AUDIT_EVERY 120
static int s_audit_k;
static Snapshot s_audit_after; /* state after the first pass of the frames */
static Snapshot s_audit_run1;  /* and after the first re-run of them */
static Snapshot s_audit_now;   /* region list of the state being compared */
static bool s_audit_running;
static int s_audit_pass; /* 1 = first re-run, 2 = second */
static int32_t s_audit_from;
static unsigned s_audit_runs, s_audit_ck_bad, s_audit_pure_bad;
static uint32_t s_audit_ck[RING], s_audit_ck1[RING]; /* checksums of pass 1 / re-run 1 */
static char s_audit_state[SNAPS][320]; /* their state lines, for the field-level diff */
static char s_audit_state1[SNAPS][320];
static int32_t s_audit_done = -1; /* frame the last audit ran at */

static void audit_reset(void) {
    memset(&s_audit_after, 0, sizeof s_audit_after);
    memset(&s_audit_run1, 0, sizeof s_audit_run1);
    memset(&s_audit_now, 0, sizeof s_audit_now);
    s_audit_running = false;
    s_audit_pass = 0;
    s_audit_done = -1;
    s_audit_runs = 0;
    s_audit_ck_bad = 0;
    s_audit_pure_bad = 0;
}

void pc_net_sync(void) {
    static bool opened;
    if (!opened) {
        opened = true;
        record_open();
        net.synctest = getenv("MELEE_NET_SYNCTEST") != NULL;
        if (net.synctest) {
            pc_log_line("net: synctest on (every tick simulated twice, sound off)");
        }
#ifdef MELEE_FP_PERTURB_NAME
        /* A determinism-harness build (CMakeLists.txt MELEE_FP_PERTURB): say
         * so on every single run, because it desyncs against every other
         * build by design. src/pc/libm/pc_perturb.c owns the counter. */
        pc_log_line("net: FP PERTURB " MELEE_FP_PERTURB_NAME " +1 ULP (determinism test build, "
                    "never ship)");
#endif
        const char* ea = getenv("MELEE_NET_EXIT_AFTER_FRAMES");
        s_exit_after = ea != NULL ? (int32_t)strtol(ea, NULL, 10) : 0;
        const char* au = getenv("MELEE_NET_RESIM_AUDIT");
        s_audit_k = au != NULL ? atoi(au) : 0;
        if (s_audit_k > SNAPS) {
            s_audit_k = SNAPS; /* one snapshot per predicted frame is all there is */
        }
        const char* po = getenv("MELEE_NET_RESIM_AUDIT_POISON");
        s_audit_poison_want = po != NULL && po[0] == '1';
        if (s_audit_k > 0) {
            pc_log_line("net: resim audit on, %d frames every %d%s (MELEE_NET_RESIM_AUDIT)",
                s_audit_k, AUDIT_EVERY,
                s_audit_poison_want ? ", with a mispredicted pass first" : "");
        }
    }
    if (net.active && s_exit_after > 0 && net.frame >= s_exit_after) {
        exit_if_test_done();
    }
    if (net.active) {
        SDL_LockMutex(net.tx_lock);
        if (s_timer == 0) {
            s_timer = SDL_AddTimer(4, tx_timer, NULL);
        }
        SDL_UnlockMutex(net.tx_lock);
    }
    if (net.active) {
        net_watchdog_arm(); /* this is the thread that must keep moving */
        pad_qtype_hold();   /* HSD_PadInit wipes it; the tick depends on it */
        dvd_settle();
    }
    if (net.active || record_active()) {
        /* Also on the record/replay legs: a recording made with a seed that
         * drifts with the frame rate cannot replay identically anywhere,
         * which is the M0 claim (docs/netcode-plan.md section 5). */
        seed_out_of_tick_check();
    }
    if (net.synctest) {
        synctest_before_tick();
    }
    if (!net.active && !record_active()) {
        net.frame++;
        return;
    }
    /* Every tick must consume a queue entry: HSD_PadRenewMasterStatus renews
     * the game's inputs only when the queue is non-empty, so a tick that
     * finds it empty runs on the previous frame's inputs while write_head
     * and the checksum below say it ran on this frame's -- a divergence on
     * one side only. pc_net_pace_adjust_ns pins one entry there; this counts
     * the invariant (pad_empty must stay 0) and repairs it if some flush got
     * in between. The local sample is read back out of the head only when
     * this present really queued one. */
    PadLibData* p = &HSD_PadLibData;
    bool fresh = p->qcount > 0 && !net.pad_reused;
    if (p->qcount == 0) {
        net.pad_empty++;
    }
    if (net.sync_mode == SYNC_LEGACY) {
        fresh_tick(&p->queue->stat[p->qread * 4], true); /* pre-fix path, net_sync.c */
        return;
    }
    /* SYNC_OFF measures the link and never acts on it (net_sync.c), so it
     * keeps the normal input path: a control run has to differ from the run
     * under test in the time sync alone, not in how pads reach the tick. */
    PADStatus* head = p->qcount > 0 ? &p->queue->stat[p->qread * 4] : unconsume();
    fresh_tick(head != NULL ? head : &p->queue->stat[p->qread * 4], fresh);
}

/* Compare the live state against what an earlier run of the same frames
 * left, by region and by per-frame checksum. `label` names the pair being
 * compared; returns the number of frames whose checksum did not come back. */
static int audit_diff(const Snapshot* want_s, const uint32_t* want_ck,
    const char (*want_state)[320], const char* label) {
    Snapshot* n = &s_audit_now;
    if (!snapshot_take(n, net.frame)) {
        pc_log_line("net: audit could not snapshot the re-run state");
        return -1;
    }
    int blocks = 0, ranges = 0;
    const uint8_t* want = want_s->buf;
    for (int i = 0; i < want_s->nregions; i++) {
        const Region* r = &want_s->regions[i];
        const Region* q = i < n->nregions ? &n->regions[i] : NULL;
        if (q == NULL || q->ptr != r->ptr || q->len != r->len) {
            /* Report and skip: a heap whose extent moved says nothing about
             * the regions after it, and stopping here hid them. */
            pc_log_line("net: audit f%d+%d %s region %s moved (%p/%zu -> %p/%zu)", s_audit_from,
                s_audit_k, label, r->name, r->ptr, r->len, q ? q->ptr : NULL, q ? q->len : 0);
            want += r->len;
            continue;
        }
        /* Runs of adjacent differing 16-byte blocks, merged: a re-run
         * differs in a few hundred blocks that belong to a handful of
         * objects, and one line per object is what `nm` can be pointed at. */
        size_t run_from = 0;
        bool in_run = false;
        for (size_t off = 0; off <= r->len; off += 16) {
            size_t len = off < r->len ? (r->len - off < 16 ? r->len - off : 16) : 0;
            bool diff = len != 0 && memcmp((const uint8_t*)r->ptr + off, want + off, len) != 0;
            if (diff) {
                blocks++;
                if (!in_run) {
                    in_run = true;
                    run_from = off;
                }
            } else if (in_run) {
                in_run = false;
                ranges++;
                if (ranges <= 40) {
                    pc_log_line("net: audit f%d+%d %s differs %s+0x%zx..0x%zx at %p (%zu bytes)",
                        s_audit_from, s_audit_k, label, r->name, run_from, off,
                        (const uint8_t*)r->ptr + run_from, off - run_from);
                }
            }
        }
        want += r->len;
    }
    /* The blocks above include state no checksum looks at (the HUD, the
     * debug font, render-phase scratch). What decides a desync is whether
     * the re-run reproduced each frame's checksum: inputs, the RNG seed and
     * every fighter's position, facing, percent, action and stocks. */
    int ck_bad = 0;
    for (int32_t f = s_audit_from; f < net.frame; f++) {
        if (s_ck_ring[f & (RING - 1)] != want_ck[f & (RING - 1)]) {
            if (ck_bad < 4) {
                pc_log_line("net: audit f%d %s checksum %08x -> %08x", f, label,
                    want_ck[f & (RING - 1)], s_ck_ring[f & (RING - 1)]);
                /* Which field moved: the state line the earlier run wrote
                 * against the one this run wrote (net_snapshot.c). */
                pc_log_line("net: audit f%d was %s", f, want_state[(f - s_audit_from) % SNAPS]);
                pc_log_line("net: audit f%d now %s", f, state_line(f));
            }
            ck_bad++;
        }
    }
    /* A branch that only changed how many random numbers the frame drew is
     * the commonest shape of an unfaithful re-run: same positions, a seed a
     * few draws along. */
    uint32_t seed_was = want_s->seed_val, seed_now = *HSD_RandSeedPtr;
    if (seed_was != seed_now) {
        pc_log_line("net: audit f%d+%d %s seed %08x -> %08x (%d draws on)", s_audit_from, s_audit_k,
            label, seed_was, seed_now, seed_steps(seed_was, seed_now));
    }
    pc_log_line("net: audit f%d+%d %s: %d blocks in %d ranges differ, %d checksums differ, "
                "seed %s",
        s_audit_from, s_audit_k, label, blocks, ranges, ck_bad,
        seed_was == seed_now ? "same" : "MOVED");
    return ck_bad;
}

/* The discarded mispredicted pass is done: drop the wrong input and re-run
 * the range with the true one, which is exactly what a real rollback does. */
static bool audit_after_poison(void) {
    s_audit_poison = false;
    s_audit_pass = 1;
    return rollback_to(s_audit_from);
}

/* The first re-run is done: compare it with the first pass, then keep its
 * own result and re-run the range a second time. Returns true when that
 * second re-run is under way (the caller must let the frame loop tick). */
static bool audit_after_run1(void) {
    int ck_bad = audit_diff(&s_audit_after, s_audit_ck, s_audit_state, "vs pass1");
    if (ck_bad > 0) {
        s_audit_ck_bad++;
    }
    /* The same range again, from the same snapshot: whatever differs
     * between two re-runs is the tick itself reading something outside the
     * state, and whatever differs only against pass 1 is something the
     * first pass did that a re-run does not. */
    if (!snapshot_take(&s_audit_run1, net.frame)) {
        return false;
    }
    memcpy(s_audit_ck1, s_ck_ring, sizeof s_audit_ck1);
    for (int i = 0; i < s_audit_k; i++) {
        snprintf(s_audit_state1[i], sizeof s_audit_state1[0], "%s", state_line(s_audit_from + i));
    }
    s_audit_pass = 2;
    return rollback_to(s_audit_from);
}

static void audit_after_run2(void) {
    int ck_bad = audit_diff(&s_audit_run1, s_audit_ck1, s_audit_state1, "vs re-run1");
    if (ck_bad > 0) {
        s_audit_pure_bad++;
    }
    s_audit_runs++;
    pc_log_line("net: audit f%d+%d done (%u first-pass / %u re-run mismatches of %u audits)",
        s_audit_from, s_audit_k, s_audit_ck_bad, s_audit_pure_bad, s_audit_runs);
}

bool pc_net_after_tick(bool scene_ending) {
    s_aj_on = false; /* the tick is over: later audio queries are not its own */
    net_sfx_end();
    head_check(); /* did this tick consume the inputs write_head wrote? */
    if (net.synctest) {
        return synctest_after_tick();
    }
    if (!net.active) {
        /* The record/replay leg never reaches the arming at the bottom, so
         * without this s_seed_have stays false and the check above is a
         * permanent no-op there -- a recording whose seed was stirred by the
         * wall clock could never replay identically anywhere, which is the
         * whole M0 claim (docs/netcode-plan.md section 5). */
        if (record_active()) {
            s_seed_after_tick = *HSD_RandSeedPtr;
            s_seed_have = true;
        }
        return false;
    }
    game_over_note(net.tick_frame);
    recv_inputs();
    if (game_over()) {
        s_rb_frame = -1; /* Slippi stops rolling back once the game is over */
    }
    if (s_rb_frame >= 0) {
        int32_t f = s_rb_frame;
        s_rb_frame = -1;
        if (rollback_to(f)) {
            s_resim_run = 0;
            return true;
        }
    }
    if (net.resim) {
        if (net.tick_frame + 1 < net.frame) {
            int32_t g = net.tick_frame + 1;
            /* The snapshot of a frame this rollback re-runs was taken on the
             * timeline being replaced. A frame still predicted gets a fresh
             * one in resim_prepare; a confirmed one does not, so its old
             * slot would restore the mispredicted state. No real rollback
             * can target a confirmed frame, but the resim audit does, and it
             * carried the stale state into the live session (a DESYNC at
             * f4676 under --delay 50 --loss 3 --jitter, one frame after a
             * rollback to 4675). Drop it instead. */
            Snapshot* stale = snap_slot(g);
            if (g <= s_remote_have && stale->frame == g) {
                stale->frame = -1;
            }
            s_resim_run++;
            return resim_prepare(g);
        }
        net.resim = false;
        net_sfx_rollback_done();     /* the re-run caught up: settle its sounds */
        resim_note(s_resim_run + 1); /* the tick that just ran was a re-run too */
        s_resim_run = 0;
        if (s_audit_running) {
            bool more = s_audit_pass == 0 ? audit_after_poison() :
                        s_audit_pass == 1 ? audit_after_run1() :
                                            (audit_after_run2(), false);
            if (more) {
                return true; /* the next pass of this audit is running */
            }
            s_audit_running = false;
            s_audit_poison = false;
        }
    }
    /* The audit's own rollback: same rings, so the re-run must reproduce the
     * state it replaces (MELEE_NET_RESIM_AUDIT). */
    if (s_audit_k > 0 && !s_audit_running && s_rb_frame < 0 && in_fight() &&
        (net.frame % AUDIT_EVERY) == 0 && net.frame != s_audit_done)
    {
        int32_t f = net.frame - s_audit_k;
        Snapshot* s = f > 0 ? snap_slot(f) : NULL;
        if (s != NULL && s->frame == f && f > net.rb_barrier && snapshot_unusable(s) == NULL &&
            snapshot_take(&s_audit_after, net.frame))
        {
            memcpy(s_audit_ck, s_ck_ring, sizeof s_audit_ck);
            for (int i = 0; i < s_audit_k; i++) {
                snprintf(s_audit_state[i], sizeof s_audit_state[0], "%s", state_line(f + i));
            }
            s_audit_from = f;
            s_audit_done = net.frame;
            s_audit_running = true;
            s_audit_pass = s_audit_poison_want ? 0 : 1;
            s_audit_poison = s_audit_poison_want;
            if (rollback_to(f)) {
                return true;
            }
            s_audit_running = false;
            pc_log_line("net: audit could not roll back to frame %d", f);
        }
    }
    check_desync();
    handshake_direct();
    if (net.direct && net.hs == HS_FAILED) {
        /* A direct session has no lobby to report a refusal to, so it ends
         * itself. Whichever check failed has already logged why; what this
         * adds is that the session is over rather than about to play on and
         * desync a minute later. INCOMPATIBLE rides the BYE, so the peer
         * reports the same class even when its own side of the exchange was
         * still waiting. */
        s_status = PC_NET_PEER_INCOMPATIBLE;
        pc_log_line("net: refusing this session at frame %d, the peers did not agree on the "
                    "match parameters",
            net.frame);
        pc_net_disconnect();
        return false;
    }
    if (!scene_ending && net.advance_left > 0 && (net.frame % 5) == 0) {
        /* Behind the peer: one extra tick this present, once per 5 frames. */
        PADStatus* head = unconsume();
        if (head != NULL) {
            net.advance_left--;
            s_advances++;
            fresh_tick(head, false);
            return true;
        }
    }
    s_seed_after_tick = *HSD_RandSeedPtr; /* seed_out_of_tick_check reads it next present */
    s_seed_have = true;
    return false;
}
