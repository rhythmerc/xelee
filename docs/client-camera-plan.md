# Client-specific cameras in netplay

Status: researched 2026-10-28, deferred. Each player's game would frame the
action for its own player instead of every peer drawing the same synced
camera. The use case is big stages like Hyrule Temple, and future custom
maps, where the shared "fit everyone" camera zooms far out.

## What we found

### Melee's camera runs inside the simulation

- **The state:** `static Camera game_camera` (`src/melee/cm/camera.c:61`), plus
  the game's camera object at `cm_804D6464`.
- **The update:** `fn_8002F360`, registered at priority 0x12
  (`camera.c:4153`). It runs in every sim tick, rollback re-runs included.
- **The default mode:** `Camera_8002B3D4` (`:1579`) fits every framed subject
  inside the stage's camera limits.
- **Its inputs:**
  - the fighters' camera boxes (`ftcamera.c:51`);
  - camera boxes from items and stages;
  - the stage's camera limits;
  - screen-shake requests;
  - pad input in pause, training and 1P only.
- **The camera object is written only at draw time:** `fn_800301D0` calls
  `Camera_8002A4AC` (`:1052`).
- **Snapshot and hashes:** `game_camera` and the camera object are both in
  the rollback snapshot. The per-frame desync checksum doesn't include
  the camera; the full-state hash every 180 frames does.

### The simulation reads the camera back

These would desync if each player's camera differed:

- **Corneria** (`grcorneria.c:1747`, `:2385`): the camera's focus picks the
  background section, and that choice gates a `HSD_Randi(3)` draw.
- **Onett's cars** (`gronett.c:547`): they time themselves from the screen
  edges, read from the **draw-time** camera object (`Camera_800307D0`).
- **Unown** (`itunknown.c:144`): flies toward the camera's focus.
- **Chansey and Scizor leaving** (`itkyasarin.c:544` → `itzako.c:224`,
  `ithassam.c:335`): their velocity points at the camera.
- **Star KO or screen KO** (`ft_0D31.c:64`): depends on the camera mode.

Some reads change the simulation's state without changing gameplay:

- Mute City's background cars (`grmutecity.c:1559`).
- Dream Land 64's background (`groldpupupu.c:389`) and Kongo 64's bird
  (`groldkongo.c:481`). Both read the draw-time camera object.

Some things that look camera-dependent are safe, because they use only the
stage's own bounds:

- blast zones;
- respawn height;
- the stage-limit checks in CPU AI and Rainbow Cruise;
- offscreen damage, which in netplay goes through `slippiOffscreen` against
  stage limits. Slippi fixes are forced on there.

### A risk that exists today

Onett, Dream Land 64 and Kongo 64 read a camera object that's only updated
when a frame is drawn. A rollback re-run draws nothing. So those reads can
differ between peers that drew different frames, and Onett's car timing is a
possible desync now. It's being fixed separately by moving those reads onto
`game_camera.transform`.

## The design

1. **Keep the synced camera** exactly as it is, inside the sim tick.
2. **Make the simulation independent of draw-time camera state.** Read
   `game_camera.transform` everywhere the sim reads the camera object today
   (`Camera_800307D0`, `Camera_80030CFC`, `Camera_80030BBC`). That's the
   risk fix above.
3. **Add a presentation pass.** At draw time:
   - compute this player's framing: the local fighter's camera box, plus
     others nearby, clamped to the stage's camera limits;
   - write it to the camera object for drawing only;
   - afterwards put the synced transform back.

   Keep this camera's state in `src/pc` statics, outside the snapshot, as
   the XR follow window does (`xr_scene.c:496`, `follow_update`). Use a
   read-only variant of the camera-box update, since `Camera_8002928C`
   advances timers (`camera.c:384`).
4. **Check draw-time writes into fighters.** The magnifier writes into fighter
   memory while drawing (`ftlib.c:596`). With per-player cameras that would
   differ between peers, so confirm the full-state hash masks it, or move it.
5. **Optionally, an allow-list of stages** if some stage can't be made safe.

Making `game_camera` itself per-player isn't viable. Corneria's random draw,
Unown and the items that leave would diverge.

## Testing

Run `tools/net_test.py` and `tools/net_test_n.py` with the per-player camera
switched on for **one** peer only.

It passes when:

- no `DESYNC` appears;
- the `net: fullhash` lines are identical on every peer;
- the `.slp` replays are identical;
- `MELEE_NET_RENDER_AUDIT=1` stays clean.

Cover Corneria, Onett, Mute City, Dream Land 64, Kongo 64 and Temple. Add a
run with items on that forces Unown, Chansey and Scizor, and a run with heavy
rollback (delay and loss).

## Precedent to reuse

- **The XR eye views** already ignore Melee's camera.
  `pc_xr_world_camera` (`camera.c:4047`) hands over the game camera's view,
  and aurora draws each eye from the head pose.
- **The XR follow window** is the model for a camera kept outside the
  snapshot. It already has `.follow_player`.
- **Melee's 1P camera** leans toward P1 (`Camera_8002B1F8`, `:1444`), the
  local-player focus we'd want.
