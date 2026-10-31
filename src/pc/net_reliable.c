/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay reliable channel: two independent stop-and-wait lanes so a full
 * or slow caller lane never holds up RULES/READY. Lane 0 carries the match
 * handshake (types < 0x10, consumed inline by net_handshake.c), lane 1 the
 * caller's messages (types >= 0x10, queued for pc_net_recv_reliable) except
 * REL_RESUME and REL_DELAY, which net.c and net_sync.c consume inline the
 * same way (REL_RESUME arrives while the game thread is parked in a stall
 * and REL_DELAY mid-match, where no caller is draining). Each lane has one
 * 'R' in flight, resent every 250 ms until its 'K' arrives,
 * 4 queued behind it and its own sequence space.
 *
 * Lane 1 mixes messages a session depends on -- REL_RESUME, REL_SCENE,
 * REL_DELAY, the lobby's ready barrier, a stage pick -- with two kinds a
 * player generates at will: quick chat and the ranked-set exchange. Only
 * the first kind cannot be refused (a resume that is not queued misses the
 * window the peer is waiting out, a scene hand-off that is not queued
 * stalls the scene on both peers, a refused ready barrier fails the
 * lobby), and quick chat is live in the online lobby, so without a rule a
 * player mashing phrases could take all four slots from any of them. So
 * the two player-driven kinds share at most REL_LOW_QUEUE slots and
 * everything else keeps two, which is as many as are outstanding at once
 * in practice: a resume overlaps a scene hand-off or a delay change, but
 * all three want a stall, a scene exit and the host's delay pick on one
 * frame. Both capped senders retry -- the ranked exchange from
 * pc_rank_session_poll(), a phrase by the player, who is rate limited to
 * one per 2 s anyway -- so the cap costs a frame, not a message. A type
 * that is neither is control by default: the starvable set is named here,
 * not the protected one.
 *
 * Wire: Rel.seq / RelAck.seq bit 7 is the lane, bits 0-6 the lane's 7-bit
 * sequence number, so the receiver demultiplexes without a new field.
 * Sequence arithmetic is modulo 128: the next expected seq is processed
 * and acked, one of the 8 before it is re-acked (the ack was lost), any
 * other is dropped. Transmit side under tx_lock, receive side game thread
 * only.
 *
 * Every link has its own pair of lanes and sequence spaces. A message from
 * pc_net_send_reliable goes to every peer: it is queued on all of their
 * lanes or, when any of them is full, on none. */
#include "compat.h"
#include "pc/net_internal.h"
#include "pc/net_chat.h"
#include "pc/xeleenet_menu.h"

#include <SDL3/SDL_timer.h>
#include <string.h>

#define REL_QUEUE 4
#define REL_RESEND_NS 250000000ull
#define REL_LANES 2
#define REL_LANE_BIT 0x80
#define REL_SEQ_MASK 0x7f
#define REL_REACK 8     /* already-accepted seqs still re-acked */
#define REL_LOW_QUEUE 2 /* lane 1 slots low-priority traffic may hold */

typedef struct RelMsg {
    uint8_t type;
    uint16_t len;
    uint8_t payload[REL_MAX];
} RelMsg;

typedef struct RelTx {
    RelMsg q[REL_QUEUE]; /* [head] is the one in flight */
    int head, n;
    uint8_t seq;      /* lane seq of the message in flight */
    uint64_t sent_ns; /* 0: not sent yet */
    int resends;
} RelTx;

static RelTx s_rel_tx[MAX_PEERS][REL_LANES]; /* under tx_lock */
static RelMsg s_rel_rx[REL_QUEUE];           /* lane 1 messages waiting for the caller */
static int s_rel_rx_head, s_rel_rx_n;
static uint8_t s_rel_expect[MAX_PEERS][REL_LANES]; /* next lane seq accepted */
static bool s_rel_unexpected_logged;

static int lane_of(uint8_t type) {
    return type < 0x10 ? 0 : 1;
}

/* The two kinds a player can produce faster than the lane drains: a quick
 * chat phrase, and the ranked-set exchange (net_rank_session.c owns every
 * type from 0x60 up). Naming these rather than the control types is what
 * keeps a new message type safe by default. */
static bool rel_low_priority(uint8_t type) {
    return type == REL_CHAT || type >= 0x60;
}

/* Low-priority messages a lane currently holds (caller holds tx_lock). The
 * queue is four entries, so counting beats keeping a second counter in step
 * with head/n across enqueue, ack and reset. */
static int low_queued(const RelTx* t) {
    int n = 0;
    for (int i = 0; i < t->n; i++) {
        if (rel_low_priority(t->q[(t->head + i) % REL_QUEUE].type)) {
            n++;
        }
    }
    return n;
}

/* Signed distance a - b in the 7-bit lane sequence space. */
static int seq_diff(uint8_t a, uint8_t b) {
    int d = (a - b) & REL_SEQ_MASK;
    return d < 64 ? d : d - 128;
}

/* Resend #1, #4, #16, #64...: a bad link logs a few lines, not one per 250 ms. */
static bool log_resend(unsigned n) {
    return (n & (n - 1)) == 0 && (n & 0x55555555u) != 0;
}

/* Send or resend each lane's message in flight on every link (caller holds
 * tx_lock). */
void rel_service(void) {
    uint64_t now = 0;
    for (int peer = 0; peer < net.npeers; peer++) {
    for (int lane = 0; lane < REL_LANES; lane++) {
        RelTx* t = &s_rel_tx[peer][lane];
        if (t->n == 0 || net.link_dead[peer]) {
            continue;
        }
        if (now == 0) {
            now = SDL_GetTicksNS();
        }
        if (t->n == 0 || (t->sent_ns != 0 && now - t->sent_ns < REL_RESEND_NS)) {
            continue;
        }
        const RelMsg* m = &t->q[t->head];
        Rel r = {hdr('R'), (uint8_t)(lane << 7 | t->seq), m->type, m->len, {0}};
        memcpy(r.payload, m->payload, m->len);
        wire_rel(&r);
        tx(peer, &r, offsetof(Rel, payload) + m->len);
        if (t->sent_ns != 0) {
            t->resends++;
            if (log_resend((unsigned)t->resends)) {
                pc_log_line("net: reliable seq %u type %02x len %u to peer %d resend #%d", r.seq,
                    m->type, m->len, peer, t->resends);
            }
        }
        t->sent_ns = now;
    }
    }
}

static bool lane_has_room(int peer, uint8_t type) {
    const RelTx* t = &s_rel_tx[peer][lane_of(type)];
    return t->n < REL_QUEUE && (!rel_low_priority(type) || low_queued(t) < REL_LOW_QUEUE);
}

/* Queue on one link, or on every live link when `only` is -1 (all or none).
 * A dead link (net.link_dead) is skipped: what it would carry reaches that
 * peer through the host instead (REL_RELAY). */
static bool rel_queue(int only, uint8_t type, const void* payload, int len) {
    int from = only < 0 ? 0 : only;
    int to = only < 0 ? net.npeers : only + 1;
    SDL_LockMutex(net.tx_lock);
    bool ok = from < to && to <= net.npeers;
    for (int peer = from; ok && peer < to; peer++) {
        ok = net.link_dead[peer] ? only < 0 : lane_has_room(peer, type);
    }
    if (ok) {
        for (int peer = from; peer < to; peer++) {
            if (net.link_dead[peer]) {
                continue;
            }
            RelTx* t = &s_rel_tx[peer][lane_of(type)];
            RelMsg* m = &t->q[(t->head + t->n++) % REL_QUEUE];
            m->type = type;
            m->len = (uint16_t)len;
            if (len > 0) {
                memcpy(m->payload, payload, (size_t)len);
            }
        }
        rel_service();
    }
    SDL_UnlockMutex(net.tx_lock);
    return ok;
}

bool rel_send_to(int peer, uint8_t type, const void* payload, int len) {
    if (!net.active || peer < 0 || len < 0 || len > REL_MAX || (len > 0 && payload == NULL) ||
        pc_net_slippi_active())
    {
        return false;
    }
    return rel_queue(peer, type, payload, len);
}

bool pc_net_send_reliable(uint8_t type, const void* payload, int len) {
    if (!net.active || len < 0 || len > REL_MAX || (len > 0 && payload == NULL)) {
        return false;
    }
    /* The Slippi transport has no custom reliable channel: lane-1 traffic
     * (scene hand-off, delay, chat) is tunnelled on ENet's reliable channel
     * instead, so it never touches the (absent) custom socket. Lane 0 is the
     * RULES/READY handshake, which the Slippi path replaces with
     * PC_SLIPPI_MSG_SETUP; refuse it rather than send it into the void. */
    if (pc_net_slippi_active()) {
        if (type < 0x10) {
            return false; /* lane 0: the custom RULES/READY handshake only */
        }
        return pc_net_slippi_send_rel(type, payload, len);
    }
    return rel_queue(-1, type, payload, len);
}

int pc_net_recv_reliable(uint8_t* type, void* payload, int max) {
    if (s_rel_rx_n == 0 || type == NULL || payload == NULL || max < 0) {
        return -1;
    }
    RelMsg* m = &s_rel_rx[s_rel_rx_head];
    int n = m->len > max ? max : m->len;
    *type = m->type;
    memcpy(payload, m->payload, (size_t)n);
    s_rel_rx_head = (s_rel_rx_head + 1) % REL_QUEUE;
    s_rel_rx_n--;
    return n;
}

void on_rel(int peer, const Rel* r, int n) {
    if (r->len > REL_MAX || n < (int)(offsetof(Rel, payload) + r->len) || peer < 0 ||
        peer >= net.npeers)
    {
        return;
    }
    int lane = r->seq >> 7;
    uint8_t* expect = &s_rel_expect[peer][lane];
    int d = seq_diff(r->seq & REL_SEQ_MASK, *expect);
    if (d == 0) {
        if (lane == 0) {
            handshake_msg(peer, r->type, r->payload, r->len);
            *expect = (*expect + 1) & REL_SEQ_MASK;
        } else {
            /* Lane 1 is the engine's own reliable traffic. The Slippi transport
             * carries it directly on ENet's reliable channel and calls
             * rel_dispatch itself, so this path only runs for the custom
             * transport (which has already done the seq/ack bookkeeping). */
            if (!rel_dispatch(peer, r->type, r->payload, r->len)) {
                return; /* caller not draining: no ack, the peer resends later */
            }
            *expect = (*expect + 1) & REL_SEQ_MASK;
        }
    } else if (d < -REL_REACK || d > 0) {
        if (!s_rel_unexpected_logged) {
            s_rel_unexpected_logged = true;
            pc_log_line("net: reliable seq %u from peer %d unexpected (expect %u)", r->seq, peer,
                lane << 7 | *expect);
        }
        return;
    }
    RelAck k = {hdr('K'), r->seq};
    SDL_LockMutex(net.tx_lock);
    tx(peer, &k, sizeof k);
    SDL_UnlockMutex(net.tx_lock);
}

/* Deliver one lane-1 reliable message to whatever consumes it. Shared by
 * on_rel (custom transport) and the Slippi receive path (net.c). Returns
 * false when a caller-queued message had no room (the custom path then
 * withholds the ack so the peer resends; Slippi has already delivered it, so
 * the message is simply dropped there). */
bool rel_dispatch(int peer, uint8_t type, const void* payload, int len) {
    if (type >= REL_ROUTE && type <= REL_VOID) {
        net_link_rel(peer, type, payload, len);
        return true;
    }
    /* Everything a player broadcasts that every peer acts on (the scene
     * hand-off, chat, the caller's messages) also goes on, through the host,
     * to a guest that cannot hear that player. */
    if (type != REL_RESUME && type != REL_DELAY) {
        net_relay_out(peer, type, payload, len);
    }
    if (type == REL_RESUME) {
        net_resume_rel(peer, payload, len);
    } else if (type == REL_DELAY) {
        net_delay_rel(peer, payload, len);
    } else if (type == REL_SCENE) {
        net_scene_rel(peer, payload, len);
    } else if (type == REL_CHAT) {
        pc_net_chat_receive(payload, len);
    } else if (type == REL_XN_EDITING) {
        pc_xn_editing_receive(payload, len); /* the overlay's notice only: never queued */
    } else {
        if (s_rel_rx_n >= REL_QUEUE) {
            return false;
        }
        RelMsg* m = &s_rel_rx[(s_rel_rx_head + s_rel_rx_n++) % REL_QUEUE];
        m->type = type;
        m->len = (uint16_t)len;
        if (len > 0) {
            memcpy(m->payload, payload, (size_t)len);
        }
    }
    return true;
}

void on_rel_ack(int peer, const RelAck* k) {
    if (peer < 0 || peer >= net.npeers) {
        return;
    }
    RelTx* t = &s_rel_tx[peer][k->seq >> 7];
    SDL_LockMutex(net.tx_lock);
    if (t->n > 0 && (k->seq & REL_SEQ_MASK) == t->seq) {
        t->head = (t->head + 1) % REL_QUEUE;
        t->n--;
        t->seq = (t->seq + 1) & REL_SEQ_MASK;
        t->sent_ns = 0;
        t->resends = 0;
        rel_service(); /* next queued message goes out at once */
    }
    SDL_UnlockMutex(net.tx_lock);
}

/* A link that died: what it still had queued will never be acked, and a full
 * lane would refuse every broadcast after it. Caller holds tx_lock. */
void rel_link_drop(int peer) {
    for (int lane = 0; lane < REL_LANES; lane++) {
        RelTx* t = &s_rel_tx[peer][lane];
        t->head = t->n = t->resends = 0;
        t->sent_ns = 0;
    }
}

/* Session start: every queue empty, sequence numbers from 0 (timer parked). */
void rel_reset(void) {
    pc_net_chat_reset();
    for (int peer = 0; peer < MAX_PEERS; peer++) {
        for (int lane = 0; lane < REL_LANES; lane++) {
            RelTx* t = &s_rel_tx[peer][lane];
            t->head = t->n = t->resends = 0;
            t->seq = s_rel_expect[peer][lane] = 0;
            t->sent_ns = 0;
        }
    }
    s_rel_rx_head = s_rel_rx_n = 0;
    s_rel_unexpected_logged = false;
}
