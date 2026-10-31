/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_H
#define PC_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Netplay prototype: rollback lockstep between two instances over UDP
 * (src/pc/net.c, docs/netcode-plan.md §4). Enabled at boot by
 * MELEE_NET=<peer host:port> or at runtime by pc_net_connect()
 * (below); see net.c for the other knobs. */

/* Wire protocol version; a peer with another one is refused (both sides
 * report PEER_INCOMPATIBLE). Bump on any change to the packet layouts,
 * Rules or the handshake. */
/* Version 6 requires sequenced scene exits and acknowledged LAN election.
 * Version 7 adds the sender's frame advantage to every input packet; the
 * phase controller acts on the difference of the two, so a peer that does
 * not send one cannot be synchronised against.
 * Version 8 appends a truncated keyed-BLAKE2b tag to every datagram, so a
 * peer that does not authenticate what it sends cannot be talked to at all
 * once the session key exists (src/pc/net_wire.c).
 * Version 9 widens the input packet to 32 unacked frames and folds each
 * fighter's self and knockback velocity into the frame checksum, so a peer
 * on 8 would read every fight frame as a desync. READY names the start
 * frame it took, since a host whose RULES went stale re-issues one, and
 * input packets carry their pads delta-coded (pads_encode, net_wire.c).
 * A fight reseeds the RNG every tick (fight_reseed) and forces UCF on and
 * free camera off, which an older peer does not. */
/* Version 10 computes sinf, cosf, tanf and atanf as the GameCube does
 * (src/pc/libm/pc_msl_trig.c) instead of musl's correctly rounded ones, so
 * a peer on 9 parts from it on the first diagonal air dodge. */
/* Version 11 rounds as the disc does: fused multiply-adds where the DOL has
 * them (pc/gekko_fp.h), the SDK's paired-single vector and matrix calls,
 * MSL's sqrtf from the hardware's frsqrte estimate; and grabs can be mashed
 * out of again. A peer on 10 parts from it on the first DI. */
/* Version 12 links three or four players: 'F'/'G' pass another player's
 * input on (relayed by the host, or collected after a drop), and reliable
 * types 0x30-0x34 carry routes, relayed messages and drops (net.c). A 1v1
 * sends none of them, but a peer on 11 does not know them. */
/* Version 13 lets one machine play several controller ports: an input packet
 * gains an xports byte, and the pads of those ports follow the primary
 * port's (docs/lan-multipad-plan.md). A peer on 12 reads every packet as
 * malformed. */
#define PC_NET_PROTO_VERSION 13
void pc_net_init(void);
void pc_net_set_input_delay(int frames);
bool pc_net_active(void);
/* The player whose leaving voids the match, on the tick of the frame the
 * session agreed to end it on (in a fight); -1 on every other tick. The VS
 * scene ends the match as a no contest then (gmvs.c). */
int pc_net_void_port_now(void);
/* Presentation-only quick chat is available in connected noncombat scenes. */
bool pc_net_chat_available(void);
/* XeleeNet turns quick-chat off (not in its v1); every other mode has it. */
void pc_net_chat_set_enabled(bool enabled);
/* True when the simulation must be reproducible elsewhere: netplay,
 * record, replay or sync test. Guards machine-seeded retail behaviour. */
bool pc_net_deterministic(void);
bool pc_net_pure_load(const char* filename); /* served from memory, no barrier */

/* Netplay scene hand-off: true while the scene that asked to end must keep
 * ticking, so both peers leave it on the same frame however long their loads
 * took (src/melee/gm/gmscene.c, docs/netcode-plan.md section 5.2). */
bool pc_net_scene_hold(void);
/* Controller port the local player drives (0 = P1/host, 1 = P2/guest). */
int pc_net_local_player(void);

/* Frame of the tick being simulated (-1 before the first); identical on both
 * peers, so a scene change scheduled for a given frame lands in sync. */
int32_t pc_net_frame(void);
/* Scheduled lobby exit, including a still-pending host handshake. */
int32_t pc_net_start_frame(void);
/* Service transport without advancing simulation (lobby start fence). */
void pc_net_poll(void);
/* Internet rendezvous transfers its already-bound IPv4 socket. Ownership
 * transfers on success only; no new NAT mapping is created. */
bool pc_net_connect_socket(
    intptr_t socket, const char* ip, uint16_t port, int player, uint32_t seed);
/* One link of a session XeleeNet's punch set up (net_punch.c). */
typedef struct PcNetLink {
    uint32_t ip; /* network order */
    uint16_t port;
    int player;       /* its controller port */
    uint8_t xports;   /* the other ports that machine plays (docs/lan-multipad-plan.md) */
    uint8_t key[32];  /* this link's MAC key; the handshake does not rekey it */
    bool via_host;    /* never punched: its input and messages come through the host */
} PcNetLink;
/* A session of n + 1 machines on an already-punched IPv4 socket, this side on
 * controller port `local` (and `local_xports` too, from its other pads:
 * local_phys[port] names the pad, NULL for pads 1.. in port order).
 * Ownership of the socket moves on success only. */
bool pc_net_connect_links(intptr_t socket, const PcNetLink* links, int n, int local,
    uint8_t local_xports, const int* local_phys, uint32_t seed);
/* End the session without a goodbye and hand its socket back (-1 when there
 * is none): XeleeNet restarts the session on it when a player joins. */
intptr_t pc_net_release_socket(void);
/* A planned leave (XeleeNet's hold B on the CSS, docs/xeleenet-ready-plan.md
 * P4), which every player sees on the same lockstep frame `last` from the
 * shared pads. The others call pc_net_port_leave(port, last) on that frame:
 * nothing waits on that port past it, and it reads as released from the next
 * frame on, with no goodbye, collection or void. Whoever leaves the session
 * calls pc_net_leave_quietly(last), which first waits (up to `ms`) until
 * every peer holds its input up to that frame (and, on the host, what it
 * passes on for others), then ends the session with no goodbye: a goodbye
 * would read as a drop to a peer that hasn't reached the frame yet. */
void pc_net_port_leave(int port, int32_t last);
/* The desync check compares frames from `frame` on (INT32_MAX: none until
 * told again). XeleeNet's roster reload: from the agreed frame to the CSS's
 * entry the players run different scenes (the CSS, the Joining page). */
void pc_net_check_from(int32_t frame);
void pc_net_leave_quietly(int32_t last, int ms);
/* Wait (up to `ms`) until every peer holds our input up to `last` (and, on
 * the host, what it passes on for others): before a session ends with no
 * goodbye, so nobody is left a frame short. False if some peer may not. */
bool pc_net_settle(int32_t last, int ms);
/* The controller ports in the session, ours included (bit per port); 0 offline. */
int pc_net_port_mask(void);
/* The ports this machine plays (pc_net_local_player()'s and any more its other
 * pads drive, docs/lan-multipad-plan.md); 0 offline. */
int pc_net_local_mask(void);
/* The physical pad (PADRead index) one of our ports reads; -1 for a port that
 * isn't ours. */
int pc_net_port_phys(int port);
/* Physical pad `phys` as the last fresh tick read it (whether it plays or
 * not); false when nothing is connected there or no session runs. */
struct PADStatus;
bool pc_net_phys_pad(int phys, struct PADStatus* out);
/* The primary port of the machine that plays `port` (its own for a primary
 * port), -1 for a port nobody plays. */
int pc_net_port_owner(int port);
/* Connect over the Slippi transport (MELEE_NET_STACK=slippi,
 * docs/slippi-migration-plan.md). Uses the Slippi ENet protocol; the game-side
 * rollback/barrier/ring machinery is unchanged. `local_udp_port` 0 lets ENet
 * pick one (a direct dial); a matchmaking hole punch passes the lobby's port.
 * `ip` of NULL or empty listens only. */
bool pc_net_connect_slippi(
    const char* ip, uint16_t port, int player, uint32_t seed, uint16_t local_udp_port);
/* ---- Slippi matchmaking sessions -----------------------------------------
 * The flow is Slippi's: the character is picked first, the search runs, and
 * once the ENet link is up the two sides swap *selections* (character, color,
 * stage, rngOffset). Only then does the match begin. So the link is opened on
 * its own (nothing of the rollback engine runs, and a Dolphin peer never sees
 * our scene hand-offs), the selections cross it, and pc_net_slippi_adopt()
 * starts the engine on it when the VS scene starts. */
struct PcSlippiSelections;
bool pc_net_slippi_link_open(
    const char* ip, uint16_t port, int local_player, uint16_t local_udp_port);
void pc_net_slippi_link_pump(void);
/* 0 connecting, 1 connected, 2 failed or closed. */
int pc_net_slippi_link_state(void);
bool pc_net_slippi_link_send_selections(const struct PcSlippiSelections* s);
bool pc_net_slippi_link_remote_selections(struct PcSlippiSelections* out);
void pc_net_slippi_link_close(void);
/* Apply Slippi's fixed rules (4 stocks, 8 minutes, ...) to this process;
 * `frozen_stadium` is the match's alt stage mode (Direct only). */
void pc_net_slippi_rules_force(bool frozen_stadium);
/* Pin / release the all-unlocked save image outside a session (the Slippi
 * CSS); the player's own progress is kept aside and put back on release. */
void pc_net_unlock_pin(void);
void pc_net_unlock_release(void);
/* Start the rollback engine on the open link for a match; `rng_offset` is the
 * decider's value. Call before the VS scene's first tick. */
bool pc_net_slippi_adopt(int local_player, uint32_t rng_offset);
/* The match is over and the peer is still there: stop the engine and keep the
 * ENet peer as the link (Direct's lobby, where the next game is agreed).
 * False, with nothing changed, when there is no healthy match to give back. */
bool pc_net_slippi_unadopt(void);
/* Which peer sends the Slippi direct-session setup (seed + start frame). A
 * MELEE_NET session leaves it unset and the caller's controller port decides
 * (P1 sends); a matchmaking session sets it from the ticket's decider flag,
 * since the decider is not necessarily P1. */
void pc_net_slippi_set_setup_host(bool host);
bool pc_net_slippi_setup_host(void);
/* Offered every IPv4 datagram on the session socket before the session sees
 * it; true consumes it. Runs on net.c's receive thread, and
 * pc_net_set_datagram_handler() returns only once no call into the previous
 * handler is still running, so what one reads must stay fixed while it is
 * installed. */
typedef bool (*PcNetDatagramHandler)(const void*, size_t, uint32_t, uint16_t);
void pc_net_set_datagram_handler(PcNetDatagramHandler handler);
bool pc_net_send_datagram(const void* data, size_t size, uint32_t address, uint16_t port);

/* RNG seed agreed for the session (pc_net_connect / match handshake). */
uint32_t pc_net_seed(void);
/* The Slippi session's rngOffset (see slippiSyncRng in gmvs.c). */
uint32_t pc_net_rng_offset(void);

/* Match handshake progress: 0 idle, 1 pending, 2 done, 3 failed (15 s). */
int pc_net_handshake_state(void);

/* Match rules in force from the RULES handshake until disconnect: unlock-all
 * is on for both peers, frozen stadium is the host's setting. False when no
 * rules are in force (use the local prefs).
 *
 * While a session is in force the unlock masks in the save data are actually
 * written all-unlocked (net_handshake.c), so this flag now agrees with them
 * instead of overriding them; the direct mask readers agree too. */
bool pc_net_rules(bool* unlock_all, bool* frozen_stadium);
/* XeleeNet's Teams: the host sets what its RULES carry; every player's CSS
 * opens with what the session agreed (the host's own setting before one). */
void pc_net_set_teams(bool teams);
/* The host's lobby Frozen Stadium for RULES: 0 or 1, -1 for the player's own. */
void pc_net_set_frozen_stadium(int frozen);
bool pc_net_rules_teams(void);

/* Called once per simulation tick before the pad queue head is consumed.
 * Replaces the head sample's four ports with the synced inputs for this
 * frame, predicting the remote one when it has not arrived (stalling only
 * when the remote is more than the rollback window behind). */
void pc_net_sync(void);
void pc_net_record_boot_seed(void); /* gmmain.c, right after the RNG is seeded */

/* Called after each tick. Returns true when the tick must be run again
 * (rollback re-simulation or the MELEE_NET_SYNCTEST self-check). */
/* Finish rollback/bookkeeping, but defer fresh advances during scene exit. */
bool pc_net_after_tick(bool scene_ending);
void pc_net_render_audit(bool after); /* MELEE_NET_RENDER_AUDIT, net_snapshot.c */

/* Called by the frame boundary (src/pc/vi.c) after the pad alarm ran; the
 * returned ns are added to the next pacing wait. Time-sync corrections are
 * paid here rather than by sleeping inside a tick, and only ever lengthen a
 * frame: the peer that is behind is caught by the one ahead slowing down. */
uint64_t pc_net_pace_adjust_ns(void);

/* Called by the frame boundary before its pacing sleep with how late the
 * boundary is against its schedule; returns how much of that to run off by
 * skipping the sleep. The rest is dropped. */
uint64_t pc_net_catch_up_ns(uint64_t late_ns);

/* True while re-simulating: sound/music/rumble starts must be suppressed. */
bool pc_net_resim(void);
/* Reconcile physical motors after rollback; hardware state is not snapshotted. */
void pc_net_rumble_command(unsigned port, unsigned command);

/* The simulation asks the audio engine two questions whose answers live
 * outside every snapshot and move in real time: "did this sound start, and
 * with what voice id" (AXDriver_8038CFF4) and "is that voice still playing"
 * (AXDriver_8038D9D8 -> HSD_SynthSFXCheck, whose nodes sit in the excluded
 * HSD_Synth heap and whose voice state the audio engine clears when the
 * sample ends). A rollback re-runs a frame one round trip later, so on any
 * link with real latency the second answer differs from the first, the
 * simulation branches differently (src/melee/sfx/crowdsfx.c,
 * src/melee/gr/ground.c) and the frame's checksum diverges from the peer
 * that never re-ran it. So the answers are journalled per frame: the first
 * simulation of a frame asks the engine and records, every later
 * simulation of that frame replays the same answers and touches nothing.
 * MELEE_NET_AUDIO_JOURNAL=off restores the old behaviour.
 *
 * replay: true when this frame already has an answer at this point in its
 * call order, and *out is it. record: stores and returns v. Both are no-ops
 * outside a netplay tick on the game thread. */
bool pc_net_audio_replay(int32_t* out);
int32_t pc_net_audio_record(int32_t v);

/* The second question -- "is that voice still playing" (AXDriver_8038D9D8 ->
 * HSD_SynthSFXCheck) -- cannot be journalled into agreement, because each
 * peer's audio engine runs on its own wall clock and a peer that stalled for
 * a round trip has aged the voice further than the peer that did not. During
 * a session the simulation is therefore told "no, it finished" (true return,
 * *answer = false); outside one the real answer stands. That is a behaviour
 * change: crowd cheers no longer wait for the previous cheer to end and the
 * ground.c:3125 gate reads as "the stage sound is over".
 * MELEE_NET_AUDIO_DEAF=off restores the real answer.
 * pc_net_audio_deaf_note() records what the engine would have said, so the
 * periodic report can show how often that differs. */
bool pc_net_audio_deaf(bool* answer);
void pc_net_audio_deaf_note(bool live);

/* True in a netplay session on the game thread: sound starts then go through
 * pc/net_sfx.c, which hands the game virtual handles instead of engine voice
 * ids, logs each tick's starts and settles a rollback's sounds once it has
 * caught up. MELEE_NET_SFX_LOG=off falls back to the journal above. */
bool pc_net_sfx_on(void);

/* Called whenever the game issues a disc request: a tick that did I/O can
 * never be re-simulated (completions land on worker threads). */
void pc_net_note_io(void);

/* Live netplay numbers for the HUD. False when netplay is not active. */
bool pc_net_stats(int* ping_ms, int* delay_frames, unsigned* rollbacks);

/* Link quality for the HUD/lobby: 0 stable, 1 warning (loss, jitter or
 * deep rollbacks in the last second), 2 stalling (a stall over 500 ms in the
 * last two seconds, or the peer announced it is leaving), 3 reconnecting
 * (the peer has been silent past the stall timeout and the session is being
 * resumed, net.c's MELEE_NET_RECONNECT_MS). A reader that only knows 0-2
 * must treat anything above 2 as at least as bad as 2. */
int pc_net_quality(void);

/* True once this session's checksums have disagreed with the peer's. The
 * simulations have parted and no rollback will bring them back; the match is
 * no longer a match. Reported so the player can see it -- it used to be a
 * log line and nothing else, which left two players finishing a game that
 * only one of them was playing. */
bool pc_net_desync(void);

/* Why the last session ended (kept until the next connect): 0 still up /
 * never broke, 1 the peer left (BYE), 2 timeout, 3 desync, 4 incompatible
 * protocol version, 5 an interruption could not be resumed (the input gap
 * outran the rings, or the peer answered for another session or seed). */
enum {
    PC_NET_PEER_OK,
    PC_NET_PEER_LEFT,
    PC_NET_PEER_TIMEOUT,
    PC_NET_PEER_DESYNC,
    PC_NET_PEER_INCOMPATIBLE,
    PC_NET_PEER_RESUME
};
int pc_net_peer_status(void);
void pc_net_peer_status_clear(void);

/* Session control (src/pc/net.c). Connect at runtime instead of via
 * MELEE_NET; player 0 = P1 (host), 1 = P2. Returns false on socket error.
 * seed != 0 is applied to *HSD_RandSeedPtr at once (host); the guest passes
 * 0 and learns it from RULES. The next sim tick after connecting blocks in
 * the lockstep wait until the peer's first packet (60 s), so announce
 * before connecting. Game thread only. */
bool pc_net_connect(const char* ip, uint16_t port, int player, uint32_t seed);
void pc_net_disconnect(void);
/* The running game scene (GameSceneKind), -1 before the first (src/pc/net.c). */
int scene_kind(void);

/* Reliable lobby messages over the game socket (stop-and-wait, one in
 * flight). type is caller-defined (>= 0x10); payload <= 256 bytes. */
bool pc_net_send_reliable(uint8_t type, const void* payload, int len);
/* Returns payload length (>= 0) and fills *type when a message is pending,
 * -1 otherwise. */
int pc_net_recv_reliable(uint8_t* type, void* payload, int max);

/* Match handshake used by every lobby: host calls pc_net_host_match() once
 * both sides are connected; it sends RULES (seed, ruleset id) and waits for
 * READY; the guest's pc_net_guest_wait_match() returns true once RULES
 * arrived and READY was sent. Both return the frame at which GS_VS must be
 * entered so the transition happens on the same synced frame.
 * Non-blocking: call once per frame from the game thread; false means not
 * yet, or failed when pc_net_handshake_state() (net.h) is 3 (15 s timeout).
 * start_frame is 120 frames after the host's call; net.c re-applies the
 * seed on both peers entering that frame and compares frame checksums only
 * from it on. The first call with the same seed may be repeated. */
bool pc_net_host_match(uint32_t seed, int32_t* start_frame);
bool pc_net_guest_wait_match(uint32_t* seed, int32_t* start_frame);

/* Identity of the game image (8 hex digits, src/pc/net_disc.c): two peers on
 * different discs (region, revision, a modified ISO) are told apart before a
 * match instead of desyncing in it. "0" before a disc is open. */
const char* pc_net_disc_id(void);


#ifdef __cplusplus
}
#endif

#endif
