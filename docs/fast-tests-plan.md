# Faster-than-real-time tests, with frame-exact input

Plan, 2026-10-30.

**Goal:** the desktop test harnesses (tools/net_test*.py, xn_preview.py) run the
game uncapped, with every press landing on an exact frame, as a TAS does. Today
they're bound to real time:
- the game sleeps to 60 Hz;
- the harness holds keys for milliseconds and settles with `time.sleep`.

So a lobby run takes 4-6 minutes, and it gets flaky when the machine is busy.
That's why parallel agents had to queue on a lock.

## What the experiment showed (2026-10-30)

The run was `MELEE_UNPACED=1 MELEE_VSYNC=0` plus `net_test_n.py --players 3
--minutes 1`, with a one-line change in vi.c `unpaced_playback()`.

| | Paced (earlier run) | Unpaced |
|---|---|---|
| Wall time to frame 12000, 3 instances | ~213 s | **34 s** |
| fps per instance | 60 | ~370 in menus, ~390 in the match |
| Desyncs / rollbacks | 0 / normal | 0 / 0 |
| Harness | pass | **fail: "the match never loaded"** |

**The engine copes with running uncapped.**
- Time sync works on frame advantage, not on clocks.
- Offline, the pad read happens once per frame boundary (lb_pc_pad_alarm_to_frame); netplay pins one sample per boundary.
- The lobby's own timers count frames: the countdown, the SSS pick timer, the agreement leads.
- The engine's millisecond timeouts (drop silence, resends, connect waits) only bite on real stalls.

**The harness doesn't cope.** Its wall-clock retries pressed Start too late: the
match loaded at frame 6625 instead of about 1458.

## Design

### T1. The switch
- **`MELEE_UNPACED=1`:** `unpaced_playback()` (vi.c) returns true, so the frame boundary never sleeps to 60 Hz. This is the experiment's change, kept.
- **The harness sets `MELEE_UNPACED=1` and `MELEE_VSYNC=0`** for runs that use T2's input. Unported tests stay paced until they're ported, so nothing mixes.
- **Log wording:** the `fps` line keeps working, but "STALL" lines at the boundary (vi.c) are worded for paced play. Unpaced, a frame over 100 ms is still a stall worth logging, so they stay.

### T2. Input scripts that run inside the game
The key FIFOs (MELEE_KEY_FIFO, _P2.._P4, keyboard.c) keep their file paths and
gain frame commands. A line ending in `f` is a frame command; anything else is
the old millisecond form, unchanged for unported tests.

- **The FIFO thread only queues lines.** Each frame boundary (`pc_keyboard_apply`, the game thread), each FIFO's queue runs, so presses start and end on boundaries. The same script from the same starting frame gives the same frames.
- **Commands:**
  - `X+Down 7f`: hold these keys for 7 frames, then release.
  - `wait 30f`: nothing for 30 frames.
  - `until <text> [600f]`: run nothing more until a log line containing `<text>` is logged after this command started.
    - `pc_log_line` checks the oldest pending `until` of each queue: one substring compare per line, under a lock.
    - The optional frame cap fails the script with `script: timeout at <text>`, so a broken run ends instead of hanging.
  - `mark <id>`: logs `script: mark <id>` when reached. The harness waits on marks instead of sleeping.
  - `tap X 3f`: X for 3 frames, then 3 frames released; the menus' cursor needs a release between taps.
- **Every command logs `script: <command> at frame N`** (behind MELEE_INPUT_TRACE), so a failed run shows where each instance was.
- **Netplay:** commands run at the boundary of every presented frame. Rollback re-runs don't consume script frames, since the boundary is the one place time advances. Held netplay waits (the frozen scene redrawn) don't either. So a `wait 30f` is 30 simulated fresh frames.

### T3. The harness
In tools/net_test.py:
- `press(insts, key, frames, settle)` writes `KEY {frames}f` and `wait {settle_frames}f`, then `mark <n>`, and waits for the mark in every instance's log.
- `press_until(...)` becomes `tap` plus `until <scene line>`. The game retries in frames, not seconds.
- `settle()` becomes a frame wait.
- `wait_log` and every wall-clock timeout stay, as safety nets, with timeouts no longer than they are now.
- A `--paced` flag runs the old way, for comparison and for watching.

### T4. Porting order
Each port is green at least 3 times in a row unpaced, and once paced. The
paced run checks the scripts didn't come to depend on speed.
1. **`net_test_n.py`**, the simplest: a boot, Start, a match, a workout. The workout's millisecond holds become frame holds, with offsets in frames.
2. **`net_test_xn_menu.py`**, the default and `--net lan`. The joins are the hard part: the host's code comes off its log, and a guest's script waits for it. Nothing on the CSS depends on wall time.
3. **The scenarios, one by one:** ready, leave-*, sss-*, multipad*, fighters, details, settings-*.
   - **sss-timeout:** the 15 s pick timer becomes 2.5 s of wall time, so the script doesn't pick and waits for the timer in frames.
   - **sss-repick:** its race keeps MELEE_NET_SIM_DELAY_MS, which is wall time, so it isn't a frame race. Check that it still races.
4. **`xn_preview.py`:** no input, but its waits ("fade in", `--settle`) become frame waits, so a batch of pages runs uncapped.

### T5. Skip drawing (done 2026-10-31)
With 3-4 instances, drawing is most of the cost.
- **Where a frame went, unpaced** (MELEE_FRAME_PROBE's once-a-second split):
  about 400 ms of each second in the game's draw (HSD's render callbacks and
  the GX calls), 350-650 ms at the frame boundary (waiting on Aurora's FIFO
  and render workers), the rest sim. Aurora's render worker was each
  instance's busiest thread; the GPU sat near 35%.
- **The game's render pass can't be skipped.** Fighter hurtboxes depend on it
  (HSD_JObjSetupMatrix), and the frame loop hangs off the XFB copy's
  draw-done: skipping the render half dropped the game to 6 fps.
- **`MELEE_DRAW_EVERY=N` (vi.c) skips in Aurora instead**
  (`aurora_set_frame_discard`). A discarded frame's GX commands all run
  (state, textures, EFB copies, the draw-done token); only each draw's
  vertices are skipped, and the frame isn't presented. The game reads
  nothing back from the GPU (no GXPeekZ/ARGB).
- **The harness defaults unpaced runs to N=10** (net_test.py DRAW_EVERY;
  MELEE_DRAW_EVERY in the environment overrides it, 1 draws every frame).
  Window shots still work: the window holds a frame at most N frames old.
- **Proof:** a 1-minute 3-player match at N=10, every peer skipping a
  different set of frames, ends with matching checksums. Frames run at
  1100-1800 fps against ~350; that match took 28 s against 41 s, and a lobby
  scenario 17-19 s against 24-25 s. What's left is wall time: boot, RmlUi
  page loads (two main-thread stalls of ~2 s each in `default`, at the Host
  page and the lobby overlay), the service, the joins.

### T6. Test slots instead of the lock (done 2026-10-31)
The machine-wide lock (`flock …/melee-game-test.lock`) was a convention for
agents, not code. The harness now takes a **test slot** itself
(net_test.py `claim_slot`), a flock under ~/.cache/melee_net_cache:
- A slot moves the run's UDP ports (+200 a slot) and so its Dawn caches,
  ~2 GB a port, kept run to run; and its window titles (`melee-net1-a`).
  The default work dir gets the slot as a suffix. The service already takes a
  free port.
- `MELEE_TEST_SLOTS` (2) is how many runs go side by side. Each one more is
  another set of caches on disk.
- A paced run takes every slot: it needs the machine to keep time.
- A LAN run also takes `lan.lock`: LAN lobbies are on every other LAN run's
  list, and the guest picks by row. Service lobbies aren't announced on the
  LAN, so service runs go beside a LAN run.
- No outer `flock` is needed around the harnesses any more.

## Expected result

Boot, menus and loads speed up along with the match. A lobby scenario of 4-6
minutes paced should take about 40-60 s unpaced, and less with T5. Two agents
could then run their tests side by side, without a queue.

## Steps and proof

| Step | Proof |
|---|---|
| T1 switch | The experiment, committed. |
| T2 scripts | A unit-style run: one instance boots unpaced. A script taps Start, waits for the CSS, and moves the hand 10 frames right twice. The log shows each command at its expected frame on two runs. |
| T3 harness helpers | `net_test_n.py --players 3` green unpaced 3 times in a row, under 60 s each. |
| T4 ports | Each scenario green unpaced 3 times in a row, and once paced. |
| T5 draw skip | The recorded match matches paced, and fps is measured per N. |
| T6 lock | Two agents' service runs side by side, both green. |
