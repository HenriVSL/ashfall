# Jet: implementation tasks (rev 1, 2026-11-13)

Read `docs/DESIGN.md` first; section numbers below refer to it. Each task is one focused agent session.
**T-tasks** are code (C++ in the VM, Python tools); **A-tasks** are art/models/sound made with the asset toolkit and
run by separate art agents, in parallel with code. Every A-task has file-format inputs/outputs fixed by DESIGN 8.8,
so code and art never wait on each other's internals (code uses placeholders until the art lands).

## Rules for every task

- **Project**: `~/Documents/86box/Shared/dev/jet/` (= `M:\dev\jet`). Read-only for tasks: `sdk/`, `probe/` (after
  T02), `docs/research/`, `docs/DESIGN.md`, `docs/TASKS.md`, `STATUS.md`, and **every other project** in
  `Shared/dev/` (reading `../racer/` for patterns is fine and expected; never edit it, never `#include` from it).
- **File ownership**: edit only the files your task owns (listed per task). Need a change elsewhere? Describe it in
  your report (file, function, why). Appending functions/fields at the **end** of a header you own is fine; changing a
  signature that DESIGN fixes (8.6 terrain API, 12.2 `missile_step`, 5.1 `PilotInput`) needs orchestrator approval.
  Generated files belong to the task that owns the generator. Tunables go through `params` (DESIGN 2.5) in your own
  module's table; `jet.ini` is regenerated with `-dumpini` by whoever integrates (T08, T17, T19).
- **Keep the tree building.** Every saved file compiles (stubs are fine). Portable files: check on the Mac first:
  `clang++ -std=c++98 -fsyntax-only -Wall -Iengine/core -Iengine/coll -Iengine/world -Isrc <file>` and
  `make -C tools/simh`. If the VM build fails in a file you don't own, wait a few minutes and retry, then report it.
  Never fix someone else's file.
- **VM A only**, tools `~/Documents/86box/samba/w98` and `~/Documents/86box/samba/vmlock`. VM A is shared with
  other sessions (see CLAUDE.md section 1): **every VM use takes the lock, builds included**. Lock name =
  `86box-d5/<task id>`:
  ```sh
  L=~/Documents/86box/samba/vmlock; W=~/Documents/86box/samba/w98
  $L wait 86box-d5/T05 "T05 build+test" 1800
  $W build jet                                    # VCVARS32 + nmake; prints compiler output; exit 1 on failure
  $W run 'M:\dev\jet\jet.exe -test flight -log' -t 120 -C 'M:\dev\jet'; echo "rc=$?"
  $L release 86box-d5/T05
  ```
  Hold the lock only for a build/test cycle; release before editing; `vmlock touch` during long runs. If the holder
  is not `86box-d5/...` (the user play-testing, or another session), wait; don't use agent98 at all meanwhile. Before
  releasing: kill every program you started (`w98 ps`, kill by the **first** column only) and make sure fullscreen
  restored the desktop.
- `jet.exe` must not be running when you build. Prefer `w98 run` with `-exitafter`/`-headless`/`-test`. `w98 run`
  hides windows unless `--show`: **benchmarks and screenshots need `--show`**. Screenshots:
  `$W run 'M:\dev\jet\jet.exe -window -start air -shot 4 shot.bmp -exitafter 4.5' --show -t 120 -C 'M:\dev\jet'`, then
  `sips -s format png ~/Documents/86box/Shared/dev/jet/shot.bmp --out <scratch>/shot.png` and look. VNC only for
  windowed checks (slows rendering ~7x; never measure fps with it); DirectInput/fullscreen ignores VNC input.
- No interactive DOS commands through `w98 sh`. Text files for Win98 tools in CRLF.
- **Coding rules**: DESIGN 2.9, `/W3` clean, `-Wall` clean on clang for portable files. **Python**: Python 3 +
  Pillow only (no numpy), deterministic (run twice, compare md5); generators use the assetkit (no own writers,
  exporters or validators).
- **Weapons are game mechanics** (DESIGN 12): do not look up or reproduce real-world weapon performance; tune to the
  gameplay targets in DESIGN 12.7.
- **Verification is by text output**: exit codes, `test:`/`bench:`/`hud:` log lines, report files. Screenshots only
  for visual tasks. Every acceptance item names the command that proves it.
- **Report** (final message): what you built; files; how you verified it (commands + key output lines); deviations
  from DESIGN (with reason); open issues; numbers worth recording. Also **append** `## Txx/Axx <title> (date)` to
  `docs/HANDOFF.md` (append only) with the facts a later task needs.

Difficulty: **M** = medium, **H** = high. `||` = can run in parallel with the other tasks of its wave.

## Dependency graph and waves

```
M0  Wave 1 (||):  T01 engine copy + skeleton
                  A1 jet | A2 sky+clouds+light | A3 ground/sea/runway textures | A4 HUD font+symbols | A5 sound v1
                  (A-tasks need only the assetkit copy: T01 does that copy FIRST, within its first hour)
    Wave 2 (||):  T02 depth/fog/HUD probe (T01) | T03 island compiler v1 (T01 assetkit)
    -> M0 review (orchestrator): probe/RESULTS.md decision, island + jet previews
M1  Wave 3 (||):  T04 terrain+sea renderer (T01,T02,T03) | T05 flight model + keyboard (T01) | T07 flight HUD + minimap (T01,T02; A4 when ready)
                  A6 island art pass (T03,A3) | A7 base buildings (T01)
    Wave 4:       T06 takeoff, crash, bounds, PULL UP (T05; T04's portable part)
    Wave 5:       T08 M1 integration (T04-T07, A1-A3, A5; A6/A7 when ready)
    Wave 6:       T09 M1 VM performance pass (T08)
    -> M1 FEEL PLAY-TEST (user) -> T10 feel rounds (repeat until accepted)
M2  Wave 7 (||):  T11 radar+targeting (T05,T04) | T12 missiles+flares (T05,T04) | A8 weapons+effects art | A9 sound v2
    Wave 8 (||):  T13 launch-zone solver (T12) | T15 target symbology HUD (T07,T11)
    Wave 9 (||):  T14 enemy AI (T11,T12,T13) | T16 weapon HUD + warnings (T07,T12,T13)
    Wave 10:      T17 combat integration (T11-T16, A8, A9) -> T18 M2 performance pass
    -> M2 COMBAT PLAY-TEST (user)
M3  Wave 11 (||): T19 frontend (T17, A10) | T20 sortie + score (T17) | T22 visual polish (T17) | A10 frontend art | A11 island detail | A12 enemy model (if Q3)
    Wave 12:      T21 balance pass (play-test notes) | T23 real-hardware pass (user) 
    Wave 13:      T24 release (all)
    -> M3 PLAY-TEST -> 1.0
```

| ID | Title | Depends on | Diff | Wave |
|---|---|---|---|---|
| T01 | Engine copy, skeleton, params/F5, simh, engine additions | - | H | 1 |
| A1 | Jet: mesh, moving parts, LODs, liveries, flame | T01 (assetkit) | H | 1 |
| A2 | Sky panorama, light set, cloud textures, sun glow | T01 (assetkit) | M | 1 |
| A3 | Ground, sea, foam, runway/apron textures | T01 (assetkit) | M | 1 |
| A4 | HUD stroke font + symbol sheet + minimap frame | T01 (assetkit) | M | 1 |
| A5 | Sound pack v1 (flight) | - | M | 1 |
| T02 | Depth/fog/HUD-line probe, decision | T01 | M | 2 |
| T03 | Island compiler v1 (`tools/islandc`) + placeholder island | T01 | H | 2 |
| T04 | `e_terrain`: island load/queries + LOD terrain + sea render | T01, T02, T03 | H | 3 |
| T05 | Flight model + keyboard shaping + feel tests | T01 | H | 3 |
| T07 | Flight HUD + minimap (top priority) | T01, T02 (A4) | H | 3 |
| A6 | Island art pass (`island/island.txt`) | T03, A3 | M | 3 |
| A7 | Base buildings + props | T01 | M | 3 |
| T06 | Takeoff/climb-out, ground roll, crash, bounds, PULL UP | T05, T04 | M | 4 |
| T08 | M1 integration: scene, cameras, flow, sound, replays, F10 | T04-T07, A1-A3, A5 | H | 5 |
| T09 | M1 VM performance pass + presets | T08 | M | 6 |
| T10 | Feel rounds after the M1 play-test | M1 play-test | M | after M1 |
| T11 | Radar, contacts, HAFU states, targeting input, RWR | T05, T04 | H | 7 |
| T12 | Missiles, guidance, flares, terrain hits, stores | T05, T04 | H | 7 |
| A8 | Weapons + effects art (missiles, trails, explosions, flares, smoke) | T01 | M | 7 |
| A9 | Sound pack v2 (combat) | A5 | M | 7 |
| T13 | Launch-zone solver + AI tables + honesty tests | T12 | H | 8 |
| T15 | Target symbology HUD (HAFU, triangle, TD, locator, arrows, map contacts) | T07, T11 | H | 8 |
| T14 | Enemy AI + `-aifight` | T11, T12, T13 | H | 9 |
| T16 | Weapon HUD: launch-zone circle, SHOOT, ACT/TTG, seeker, warnings | T07, T12, T13 | H | 9 |
| T17 | Combat integration: modes, damage, respawn, fx, combat sound | T11-T16, A8, A9 | H | 10 |
| T18 | M2 performance pass (combat bench) | T17 | M | 10 |
| T19 | Frontend: menus, options, key capture, pause, results | T17, A10 | H | 11 |
| T20 | 3-wave sortie, messages, score, records | T17 | M | 11 |
| T22 | Visual polish (vapour, contrails, shadow, clouds, explosions) | T17 | M | 11 |
| A10 | Frontend art: logo, title backdrop, panels, icon | T01 | M | 11 |
| A11 | Island detail: vegetation, rocks, extra base props | A6, A7 | M | 11 |
| A12 | Enemy aircraft model (DEFERRED: user chose the same model in a different shade, Q3) | A1 | H | 11 |
| T21 | Balance pass from play-test notes | M2/M3 play-tests | M | 12 |
| T23 | Real-hardware pass (High, 60 fps) with the user | T18 | M | 12 |
| T24 | Release: version, README, Setup98 installer, zip | all | M | 13 |

---------------------------------------------------------------------------------------------------------------

## M0: engine copy, probe, island compiler, first art

## T01 - Engine copy, skeleton, params/F5, simh, engine additions  (H)

**Goal**: a building, running `jet.exe` with the final layout, so every later task only fills in its own files.

**Owns/creates**: `engine/**` (the copy; after T01 only `e_terrain*` changes hands to T04, other engine files need
orchestrator approval), `engine/tools/assetkit/**` (copy; edit `validate.py` classes per DESIGN 8.8), `sdk/**`
(copy), `Makefile`, `src/game.h`, `src/params.*`, `src/main.cpp`, `src/tests.h/.cpp` (dispatcher + `math`, `params`),
`src/replay.*` (skeleton: header + param snapshot + hash records), **stub `.h/.cpp` for every module in DESIGN 2.4**,
`tools/simh/Makefile` + `tools/simh/simh.cpp`, `tools/build_assets.py` (copied/adapted discovery), `jet.ini`,
`docs/ENGINE_COPY.md`, `docs/HANDOFF.md` (create with a "Design" section pointing to DESIGN/TASKS).

**May read**: DESIGN 2, 3, 5.3, 16; `../racer/engine/**`, `../racer/src/main.cpp tests.cpp replay.cpp bench.cpp`,
`../racer/Makefile`, `../racer/tools/physh/`, `../racer/sdk/README.md`, `docs/research/ENGINE.md` 1, 4, 5.

**Details**:
- **First** copy `engine/tools/assetkit/` (+ README) and `tools/build_assets.py` so the A-tasks can start; tell the
  orchestrator in HANDOFF the moment it is in place. Then copy engine core/coll/world/rend/plat + sdk; drop `phys/`;
  no `obj/`, `.lib`, `.pyc`, logs. Record the racer source date and every edit in `docs/ENGINE_COPY.md`.
- Engine edits (DESIGN 2.3): window class/title/error caption "Jet"; `cfg_reload`; `in_action_held_ticks`,
  `in_action_released`; `ui_line`, `ui_set_blend`, `ui_clip`; `r_clear_z` (cheap, lets T02 test Plan B).
- `params` (DESIGN 2.5): registry with float/int/bool/table types, `params_load`, reload with change log,
  `-dumpini` writer (CRLF, help comments), snapshot/restore for replays.
- main.cpp: loop of DESIGN 2.6, all switches of 2.7 parsed into `Args` (unimplemented ones log `args: not yet`),
  app states as stubs, `-headless`, `-exitafter`, `-log`, `-seed`, `-test`, `-fps`, F5 hook calling `params` reload.
- simh: compiles every portable file with clang (`-DE_NO_RENDER`); `simh test math` works.

**Acceptance**:
1. `w98 build jet` succeeds with no warnings; `w98 build jet clean` then build works.
2. `jet.exe -headless -exitafter 5 -log` exits by itself; `jet.log` shows `boot:` lines and `ticks=600`.
3. `jet.exe -test math` and `-test params` exit 0, `-test nonsense` exits -1; Mac: `make -C tools/simh &&
   tools/simh/simh test math` exit 0.
4. `jet.exe -dumpini x.ini` writes a CRLF ini; editing a test key and sending F5 in a windowed run (lock, `--show`,
   VNC key press allowed for this one check) logs `params: reload ...`.
5. `python3 engine/tools/assetkit/selftest.py` exit 0 in the copy.

## T02 - Depth/fog/HUD-line probe and decision  (M)

**Goal**: measure on the VM what DESIGN 9.1 assumes and write the decision the renderer and HUD build on.

**Owns/creates**: `probe/**` (`probe/Makefile`, `probe/probe.cpp` linking `engine\engine.lib`, `probe/RESULTS.md`,
`probe/shots/*.png`).
**May read**: DESIGN 9, 10.1; `../racer/spike/` (probe3d patterns, RESULTS.md), `docs/research/ENGINE.md` 2.4.
**Details**: implement the 8 test scenes of DESIGN 9.2 with W-buffer on and off, at far 18 km and 28 km, near 1.0 /
1.5 m; automate (`probe -scene N -shot`), screenshots compared in pairs (pixel-diff counts for shimmer), costs timed
with QPC over 300 frames. All runs exit by themselves and restore the desktop.
**Acceptance**: 1. `probe/RESULTS.md` has a table per scene (setting, result, screenshot path) and the **decision
line**: Plan A or B, near/far, fog start/end, HUD stroke width + shadow + blend; 2. the 15 m jet pixel counts at
1/2.5/4/6 km are recorded (confirms `acMeshDist`/`acDotDist` or proposes new values); 3. orchestrator review.

## T03 - Island compiler v1 + placeholder island  (H)

**Goal**: `tools/islandc` turns `island/island.txt` into `data/island/jet.isl` (DESIGN 8.6), `data/island/base.trk`,
the minimap image and previews, deterministically.

**Owns/creates**: `tools/islandc/*.py` (`islandc.py`, `height.py`, `classes.py`, `bake.py`, `chunks.py`, `isl.py`
(writer + reader + format doc block), `base.py`, `preview.py`), `island/island.txt` (format + placeholder values;
**A6 takes over the values**), `data/island/jet.isl`, `data/island/base.trk`, `data/tex/map_island.bmp` + `_a`,
`tools/islandc/out/*.png`, `data/island/report.txt`.
**May read**: DESIGN 8, 9.1, 9.3; `docs/research/ENGINE.md` 2.2, 2.3, 3.3, 3.4; `../racer/tools/trackc/terrain.py`,
`light.py`, `cells.py`, `trkfile.py`; assetkit README.
**Details**: height field recipe of DESIGN 8.1 / ENGINE 3.4 driven by `island.txt` keys (seed, summit, crater,
cone exponent, ridges, satellite cones, coast wobble, shelf, base position/heading, runway size, class thresholds,
class tints, texture periods, sun direction (default until A2's light set exists)); airbase flattening; coast rule
(terrain >= 4 m under sea level from 20 m offshore); classes; baked vertex light with height-field shadow march; 5
LOD chunks with border snapping + skirts, per-class batches, chunk-local UVs, foam batches; `HMAX`, `CLAS`, `BASE`,
`MAPI`; `base.trk` with runway/taxiway/apron ribbons and box hangars/tower/tanks as placeholder instances (A7
meshes replace them by name); `--check` (re-run + md5 + reader validation); hook into `tools/build_assets.py`.
**Acceptance**: 1. `python3 tools/islandc/islandc.py island/island.txt --check` exit 0, two runs identical md5;
2. `report.txt` lists chunks, tris per LOD, batches, height range, runway data, file size (< 8 MB);
3. previews: top view with classes + chunk grid, shaded relief, 3 perspective views (from the runway, over the
crater, from 15 km at sea) via the assetkit preview renderer; 4. the reader in `isl.py` round-trips every chunk.

## A1 - Jet: mesh, moving parts, LODs, liveries, flame  (H)

**Goal**: an original F-16-style single-engine fighter (not a replica: own proportions, no real markings), generated
with the assetkit.
**Inputs**: DESIGN 4.8, 7, 8.8 (class `jet`/`jetpart` budgets and tags), 9.5; assetkit README;
`../racer/tools/art/gen_car.py` (loft + mirror + livery bake pattern, read-only).
**Owns/creates (outputs)**: `tools/art/gen_jet.py`; `data/meshes/jet.msh` (LOD0-2, tags of DESIGN 8.8),
`jet_stab_l.msh jet_stab_r.msh jet_flap_l.msh jet_flap_r.msh jet_nozzle.msh jet_gear.msh` (origin at hinge, hinge axis
local X), `jet_flame.msh` (12-16 tris cone), `data/tex/jet_grey.bmp` (player), `jet_aggr.bmp` (enemy: dark
two-tone aggressor), `jet_parts.bmp`, `flame.bmp` + `_a`, `glow_nozzle.bmp`; `tools/art/preview/jet_*.png`.
**Details**: fuselage loft with chin intake, bubble canopy (own material, dark tinted), cropped-delta wing, single
fin, stabilators and flaperons as parts, nozzle petals, 8 rail tags (wingtips + under-wing), gear; livery bake with
panel lines, low-vis markings, an own invented roundel. Size ~15 m long, ~10 m span; CG at the origin; +Z forward.
**Acceptance**: 1. `python3 tools/build_assets.py --only jet --check` identical md5; 2. `validate.py` passes on every
output; 3. preview views + turntable + LOD sheet + a "surfaces deflected" view (stab 20 deg, flaperons +-20 deg) look
right; describe the silhouette in the report.

## A2 - Sky panorama, light set, clouds, sun glow  (M)

**Inputs**: DESIGN 8.4, 9.1 (fog colour = horizon), 9.3 (tile counts); `../racer/tools/art/gen_sky.py` (structure,
light set format, closed-form fog colour), assetkit README; Q2 answered: green island (was: default bright tropical/Mediterranean
afternoon).
**Owns/creates**: `tools/art/gen_sky.py`; `data/sky/sky_hi_0..7.bmp`, `sky_lo_0..3.bmp` (elevations -10..+40),
`data/sky/light.txt` (zenith/horizon/fog colours, sun direction azimuth ~230 deg elevation ~40 deg, sun colour,
ambient: islandc's bake and the jet lighting use it), `data/tex/cloud_puffs.bmp` + `_a` (2x2 atlas of 128 px
cumulus), `cloud_deck.bmp` + `_a` (tileable 256), `sun_glow.bmp`; previews.
**Acceptance**: 1. `--check` identical md5; 2. validate passes; 3. preview of the panorama unrolled + a 70 deg view at
the horizon with fog colour bands shown as swatches (bottom rows == fog colour exactly); 4. tileability seam check of
the deck.

## A3 - Ground, sea, foam, runway textures  (M)

**Inputs**: DESIGN 8.1 (classes), 8.3 (sea), 8.5 (runway), 8.8; `../racer/tools/art/gen_ground.py` (functions to
recolour), Q2 default.
**Owns/creates**: `tools/art/gen_ground.py`; `data/tex/t_sand.bmp t_grass.bmp t_rock.bmp t_ash.bmp t_lava.bmp`
(256 tileable, near-neutral so vertex colour carries the macro look), `sea.bmp` (256 tileable, baked swell + glitter),
`foam.bmp` + `_a`, `runway.bmp` (256x64: asphalt + centre line + edge lines), `rwy_marks.bmp` (threshold piano keys,
numbers, touchdown marks, keyed), `apron.bmp`, `taxi.bmp`; previews (tiled 3x3 + 16-bit quantised).
**Acceptance**: 1. `--check` identical md5; 2. validate passes and seam ratio < kit threshold; 3. preview sheet; list
texture periods (m) for `island.txt` in the report.

## A4 - HUD stroke font + symbol sheet + minimap frame  (M)

**Inputs**: DESIGN 10 (all sizes, symbols), `docs/research/HUD.md` 4.1 (atlas list); `../racer/tools/art/gen_ui.py`
(stroke font renderer, sprite sheet + rect file).
**Owns/creates**: `tools/art/gen_hud.py`; `data/tex/hud_atlas.bmp` + `_a` (256x256: stroke font 9 px and 13 px cap
height, monospaced digits, A-Z, `0-9 . - + : / ' %` and alpha; symbols: VV, ghost VV, waterline W, TD square/diamond
solid + dashed, HAFU arch/chevron solid + dashed (16 px and minimap 7 px), selection triangle, locator arrow head,
threat arrow, breakaway X, seeker circle, dot, minimap jet; a 2x2 white texel block for lines), `data/tex/hud_atlas.txt`
(rect list `name x y w h ox oy`, glyph advance widths), previews at 1x and 3x on dark and on bright-sky backgrounds.
**Details**: white glyphs on transparent (the code tints), 1 px soft edge baked for smoothness at 640x480, glyph cell
grid documented in the txt file header.
**Acceptance**: 1. `--check` identical md5; 2. validate passes; 3. readability preview: every string of DESIGN 10.2-10.6
rendered at 640x480 scale over sky/sea/cloud-white/terrain swatches.

## A5 - Sound pack v1 (flight)  (M)

**Inputs**: DESIGN 15 (v1 rows); `../racer/tools/sfx/gen_sfx.py` (synth framework, loop rules).
**Owns/creates**: `tools/sfx/gen_sfx.py`, `data/sfx/` v1 files of DESIGN 15, `data/sfx/README.md` (list, loop
lengths, base pitch, suggested mix law for the engine crossfade).
**Acceptance**: 1. two runs identical md5; 2. loops seamless (seam check printout); 3. `tools/sfx/preview_flight.wav`:
offline mix of idle -> mil -> AB -> mil -> brake with wind rising, using the README mix law.

---------------------------------------------------------------------------------------------------------------

## M1: free flight with HUD (feel play-test)

## T04 - `e_terrain`: island load/queries, LOD terrain and sea rendering  (H)

**Goal**: the island and sea on screen within budget, plus the portable queries every sim module needs.
**Owns/creates**: `engine/world/e_terrain.h`, `e_terrain.cpp` (portable: load, height, surface, class, LOS, segment
hit, base), `e_terrain_draw.cpp` (render: chunk LOD selection, culling, batches, sea rings, shallows, foam),
`src/tests_terrain.cpp`, `engine/world/e_terrain.md` (API notes).
**May read**: DESIGN 8, 9.1-9.4, `probe/RESULTS.md`; `tools/islandc/isl.py` (format doc block); racer `e_world.cpp`
(culling, sorting patterns), `docs/research/ENGINE.md` 1.5, 2.2, 2.3.
**Details**: the API of DESIGN 8.6 exactly; per-LOD contiguous vertex blocks; LOD by 3D distance with hysteresis and
`lodScale`; frustum + far culling; batches sorted by material across chunks; sea rings with snapped UVs and scroll;
blended shallow ring; foam additive. Expose stats (`tris`, `chunksByLod[5]`, `draws`). The portable part must be
usable from simh and from T06 before the render part exists: deliver it first and note it in HANDOFF.
**Acceptance**: 1. `-test terrain` (and `simh test terrain`) exit 0: height vs grid, `terrain_los` vs brute force on
1000 rays (0 mismatches), segment hits, `terrain_los` 30 km ray <= 30 us on the VM; 2. screenshots with `-camat` at
the three ENGINE 2.7 viewpoints (50 m over the runway, 1500 m over the volcano, 8000 m over the sea): no cracks, no
z-fight at the coast, fog blends into the sky; 3. `terrain: tris=` stays within DESIGN 9.3 Low at those viewpoints;
terrain + sea <= 6 ms at the worst one (bench line).

## T05 - Flight model, keyboard shaping, feel tests  (H)

**Goal**: DESIGN 4 and 5 in portable code, tunable live, with the feel card as a test. This is the heart of M1.
**Owns/creates**: `src/flight.*`, `src/pilot.*`, `src/tests_flight.cpp`, `src/tests_pilot.cpp`, `simh feel` command
(in `tools/simh/simh.cpp` via a function `feel_card_print()` in `tests_flight.cpp`; T01 owns simh.cpp: add one
dispatch line and report it), `docs/FLIGHT_TUNING.md` (what each key does, in plain words, for the user).
**May read**: DESIGN 3, 4, 5, 2.5; `../racer/docs/research/KEYBOARD.md` (axis model lessons), racer `player.cpp`
(pattern only).
**Details**: exact step of DESIGN 4.3 with every tunable of 4.6/5.2 registered in `params`; quaternion
renormalisation; derived heading/pitch/bank/gamma/Mach once per tick; `FlightVisual`; joystick path; short/long
press classifier (5.3) as a generic helper used for S by T11; `-telemetry` CSV writer for the player. Write
`FLIGHT_TUNING.md` so the user can tune alone (e.g. "turns too slow at cruise -> raise `gLimitTable` points above
330 kt or `turnBoost`").
**Acceptance**: 1. `-test flight` + `-test pilot` exit 0 on the VM and in simh (feel card within 10 %, path hold in a
60 deg bank with no input, floor nose drop, ceiling, detent, short/long press timelines); 2. `simh feel` prints the
card; 3. with `[flight] turnBoost=1.2` in the ini, `simh feel` shows the turn rates +20 % (proves reload path data);
4. flight step <= 15 us per aircraft on the VM (`-test flight` timing line).

## T07 - Flight HUD + minimap (top priority)  (H)

**Goal**: the cockpit and chase flight HUD of DESIGN 10.1-10.4 and 10.7 at full quality, before the 3D scene is done.
**Owns/creates**: `src/hud.*`, `src/hud_font.*` (atlas text from `hud_atlas.txt`, GDI fallback), `src/hud_map.*`
(minimap frame, island image, own jet, scan wedge, range; a `map_draw_contact()` API for T15), `-hudtest` scenes
`ladder` and `numbers` (in `hud.cpp`), `docs/HUD_LAYOUT.md` (screenshots + element table as built).
**May read**: DESIGN 10, `docs/research/HUD.md`, `probe/RESULTS.md` (stroke decision); racer `hud.cpp` (pattern).
**Details**: reads only a `HudState` struct filled by the caller (flight values, interpolated orientation, camera
matrices), so it works over a static test scene now and over the real scene in T08. Text caches, update rates and
hysteresis (10.7.3), pixel snapping, clipping to the HUD frame with `ui_clip`, flashing from sim time. Messages queue
API (`hud_message(text, secs)`). Both views.
**Acceptance**: 1. `jet.exe -hudtest ladder` and `-hudtest numbers` exit 0 (checks of 10.8: change rates, smooth
motion < 1 px deviation, nothing outside the frame); 2. screenshots of both views at level, 30 deg climb + 60 deg
bank, inverted, 9 G at 350 kt, 15 deg alpha (VV clamped and flashing) reviewed by the orchestrator against
HUD.md; 3. HUD + minimap <= 1.5 ms on the VM (`hud: ms=` line in the hudtest).

## A6 - Island art pass  (M)

**Goal**: a beautiful, readable island: the values in `island/island.txt` (not the compiler).
**Inputs**: T03's compiler and format, A3 textures and periods, A2 light set, DESIGN 8.1, question Q2.
**Owns/creates**: `island/island.txt` (values), `tools/islandc/out/review_*.png` (review sheet), `docs/ISLAND.md`
(the island's description, landmarks, runway heading, why).
**Details**: volcano silhouette that reads from 15 km, crater, lava channels, beaches, green lower slopes, cliffs on
the weather side, shelf colours, airbase placement with a clear climb-out over the sea; check flight-relevant rules
(nothing > 150 m within 5 km of the climb-out, runway flat). Ask the T03 owner (or report) for compiler changes.
**Acceptance**: 1. `islandc --check` exit 0; 2. review sheet: top view, relief, 6 perspective views incl. the takeoff
view and a "hide behind the volcano" view; 3. `report.txt` within budgets.

## A7 - Base buildings and props  (M)

**Inputs**: DESIGN 8.1, 8.5, 8.8 (`bldg`); racer `gen_buildings.py` (pattern).
**Owns/creates**: `tools/art/gen_base.py`; `data/meshes/hangar.msh tower.msh tank.msh radome.msh` (+ textures
`data/tex/base_*.bmp`), names matching T03's placeholder instance names; previews.
**Acceptance**: `--check` identical md5; validate passes; preview of the base layout with these meshes (assetkit
`scene()`).

## T06 - Takeoff/climb-out, ground roll, crash, bounds, PULL UP  (M)

**Owns/creates**: `src/takeoff.*`, `src/world.*`, `src/tests_takeoff.cpp`, `src/tests_world.cpp`.
**May read**: DESIGN 6, 4, 8.6 (`BASE`); `e_terrain.h`.
**Details**: phases of 6.1 producing `PilotInput` through the same shaping path; continuous handover; air spawns;
crash tests (terrain/sea/boxes/aircraft); area warnings and auto turn-back; PULL UP predictor (2 Hz); events for HUD
messages and sounds (`WorldEvent` queue).
**Acceptance**: 1. `-test takeoff` and `-test world` exit 0 (VM + simh): READY -> handover in 20-35 s, centre-line
error < 2 m, no ground contact after liftoff, override at 200 ft works, crash/bounds/PULL UP cases; 2. headless
`-start runway -script tests/t06/takeoff.scr -exitafter 40` logs the phase timeline.

## T08 - M1 integration: scene, cameras, flow, sound, replays, overlays  (H)

**Orchestrator note (from A5)**: engine/wind/brake mix law and reference code are in `data/sfx/README.md` and A5's `mix_*()` functions; full AB + wind + hard pull sums to ~1.4x full scale, so keep the sound master around 0.7. Volumes in the README are a starting point for the M1 play-test.

**Goal**: the playable M1 build.
**Owns/creates**: `src/scene.*`, `src/camera.*`, `src/snd.*`, `src/bench.*` (routes `low`, `volcano`, `high`,
`clouds`), `src/fx.*` (afterburner flame + nozzle glow only; T17 extends fx in its own section of the file after
T08 is done), `src/main.cpp` (from T01: app flow FREE mode, pause menu "resume / restart on runway / quit", F9-F12,
`last.rpl`), `src/replay.*` (complete), `play.bat`, `jet.ini` (regenerated), `docs/PLAYTEST_M1.md` (how to play,
keys, what to judge, how to tune with F5/F10, where replays go).
**May read**: everything in DESIGN; racer `scene.cpp camera.cpp snd.cpp bench.cpp main.cpp` (patterns).
**Details**: draw order 9.4; jet with parts + flame + glow; base via e_world; clouds; cockpit/chase cameras (7);
`HudState` fill; F10 feel overlay; engine/wind/AB/brake/roll/gear sounds; presets Low/High (9.3); `-start air`.
**Acceptance**: 1. `play.bat` on the VM: take off, fly, crash, respawn, switch views (screenshots of each, lock
held); 2. `-test replay` and `-test determinism` exit 0; a recorded 60 s flight replays with `hash ... ok`;
3. `-bench 60 -benchroute volcano -preset low` runs and writes `bench.txt` (numbers recorded; the 30 fps gate is T09);
4. F5 during a flight changes the feel live and the replay of that flight still verifies.

## T09 - M1 VM performance pass + presets  (M)

**Owns/creates**: the Low/High preset values (in `scene.cpp`'s preset table: coordinate with T08's final code, T08
is done), `docs/PERF.md`; small optimisations in files listed in its report and approved by the orchestrator.
**Acceptance**: all four bench routes on Low: fps_avg >= 29.8, missed slots <= 2 %, 1%-low >= 24, recorded per
section in `docs/PERF.md` (racer PERF.md format); High preset values documented (unmeasured until T23).

## T10 - Feel rounds after the M1 play-test  (M, repeatable)

**Goal**: turn the user's notes into numbers (and code only if numbers can't do it).
**Owns**: `jet.ini` defaults (via the params defaults in `flight.cpp`/`pilot.cpp`/`camera.cpp`, coordinate with
the orchestrator), `docs/PLAYTEST_M1.md` (round notes), `tests/user/*`.
**Details**: replay the user's `last.rpl` and telemetry first (DESIGN 16.5); change few keys per round; update the
feel card targets in `tests_flight.cpp` when the user accepts new numbers; never "fix" feel with assists the user did
not ask for.
**Acceptance**: the user's verdict; suites stay green.

---------------------------------------------------------------------------------------------------------------

## M2: combat (combat play-test)

## T11 - Radar, contacts, HAFU states, targeting input, RWR  (H)

**Owns/creates**: `src/radar.*`, `src/target.*`, `src/tests_radar.cpp`, `src/tests_target.cpp`.
**May read**: DESIGN 11, 5.3, 8.6 (`terrain_los`), 10.5 (what the HUD needs from you).
**Details**: per-observer contact tables (player + enemies), detection/ID/memory/lock of 11.1-11.3, S short/long logic
of 11.4 using T05's classifier, RWR events (11.5), a read-only `ContactView` list for HUD/minimap/AI with latched
states (10.7.4).
**Acceptance**: `-test radar` + `-test target` exit 0 (VM + simh): cone and aspect ranges, terrain mask with the real
island (contact behind the volcano drops to MEMORY after 0.5 s), ID at 1.5 s / instant < 5 NM, memory 6 s, select
order by angle with frozen cycle, designate/undesignate, lock gimbal loss; radar tick <= 0.3 ms for 6 aircraft.

## T12 - Missiles, guidance, flares, terrain hits, stores  (H)

**Owns/creates**: `src/missile.*`, `src/weapons.*`, `src/tests_missile.cpp`.
**May read**: DESIGN 12 (all), 11.3, 8.6. Not real-world weapon data.
**Details**: `missile_step` exactly as the shared function of 12.2 (no live-only shortcuts, so T13's fly-outs match),
phases 12.3, seeker LOS/gimbal/memory, flares 12.5 with the sim RNG, firing rules 12.6, stores 12.1, hit/ground
events, a `MissileView` list for HUD/fx (ACT/TTG estimates included). Tune the 12.7 starting values towards the
gameplay targets using a small fly-out helper of your own in the test file (T13 builds the real solver).
**Acceptance**: `-test missile` exit 0 (VM + simh): straight chase, crossing target, G limit respected, terrain impact
when the target is behind the volcano, seeker LOS loss, flare decoy rates within +-5 % of the configured p over
2000 trials, maddog acquisition; gameplay targets of 12.7 met within their ranges (printed table); 12 live missiles
<= 0.5 ms per tick on the VM.

## A8 - Weapons and effects art  (M)

**Inputs**: DESIGN 9.5, 8.8 (`missile`, `fx`), 12.1.
**Owns/creates**: `tools/art/gen_fx.py`; `data/meshes/lrm.msh srm.msh` (rail-mounted and flight, origin at the rail
point), `data/tex/msl.bmp`, `trail.bmp` + `_a` (soft ribbon), `fx_fire.bmp` + `_a` (2x2 atlas), `fx_smoke.bmp` + `_a`
(2x2), `flare.bmp`, `debris.bmp`, `splash.bmp` + `_a`, `dot.bmp` (distance dot); previews.
**Acceptance**: `--check` identical md5; validate passes; preview sheet incl. the missiles on the jet's rail tags
(assetkit `scene()` with A1's tags).

## A9 - Sound pack v2 (combat)  (M)

**Inputs**: DESIGN 15 (v2 rows), A5's generator (extend it: A9 owns the v2 functions + files; coordinate one shared
`gen_sfx.py` by adding a separate module `tools/sfx/gen_sfx_combat.py`).
**Owns/creates**: `tools/sfx/gen_sfx_combat.py`, `data/sfx/` v2 files, `data/sfx/README_combat.md` (own file in its `ASSETS`: A5's build rewrites `data/sfx/README.md`; A5's `put_wav`/`steady` helpers are importable; orchestrator 2026-11-13).
**Acceptance**: identical md5 twice; loop seams ok; `tools/sfx/preview_combat.wav` (lock, launch, growl -> lock tone,
RWR, missile warning, flares, explosion).

## T13 - Launch-zone solver, AI tables, honesty tests  (H)

**Owns/creates**: `src/dlz.*`, `src/tests_dlz.cpp`, `simh shots` and `simh dlztable` (functions in tests_dlz.cpp; one
dispatch line each in simh.cpp, report them).
**May read**: DESIGN 12.4, 12.7, 13.3 (`gUse` for the escape turn), 2.6 (per-frame budget).
**Details**: definitions, fly-out and solver of 12.4 calling T12's `missile_step` unchanged at dt 1/30; escape set E
using the flight model's G limit; warm-started bisections, time-sliced by step budget; smoothing; TOF + lead direction
for the HUD; AI tables at load with timing log.
**Acceptance**: 1. `-test dlz` exit 0: honesty targets of 12.7 (straight 0.97/1.05 Rmax, 0.95 Rne x each escape) on the
VM and in simh, 40 geometries each; 2. `simh shots 500` table in the report; 3. full refresh <= 0.7 s at the Low step
budget, budget per frame <= 1.5 ms on the VM; AI tables < 1 s on the VM; 4. a designated target behind the volcano
shows Rmax collapsing (test case).

## T15 - Target symbology HUD  (H)

**Goal**: DESIGN 10.5 + 10.7: the enemy's only representation beyond 2.5 km. Clear, readable, stable.
**Owns/creates**: `src/hud_tgt.*`, `-hudtest contacts` (scene in hud_tgt.cpp; one dispatch line in hud.cpp's hudtest
table: report it to T07's successor / orchestrator), `docs/HUD_LAYOUT.md` (append a section).
**May read**: DESIGN 10, 11; `ContactView` (T11); `hud_map.h` (T07).
**Details**: HAFU arch/chevron + centre dot, selection triangle, TD square/diamond with dashed/flash states, labels
with declutter, locator line, off-screen arrows, SPLASH, minimap contacts via `map_draw_contact`; works in both views.
**Acceptance**: 1. `-hudtest contacts` exit 0: no state flicker across 1000 frames of synthetic state changes,
world-static contact deviation < 1 px, overlap rule, labels limited to 4; 2. screenshots (cockpit + chase): 6 contacts
at 0.8-30 NM incl. two overlapping, one behind (locator), one in memory, designated hostile, over sky/sea/terrain,
reviewed by the orchestrator; 3. symbology <= 0.5 ms with 8 contacts.

## T14 - Enemy AI + `-aifight`  (H)

**Owns/creates**: `src/ai.*`, `src/tests_ai.cpp`, `simh aifight` (function in tests_ai.cpp, one dispatch line).
**May read**: DESIGN 13, 11, 12, 6.4; flight/radar/weapons/dlz headers.
**Details**: states 13.1, steering 13.2 obeying keyboard ramp rates, difficulty tables 13.3, DEFEND choosing from E,
terrain masking (Hard), AVOID override; a robot player (same AI with a "player" profile) for headless fights.
**Acceptance**: `-test ai` exit 0; `simh aifight 100 1 1` (robot player vs Normal waves): 0 AI terrain/sea crashes
outside deliberate masking manoeuvres, 0 stuck (no state > 60 s without progress), AI fires only inside its
envelope, DEFEND triggers on every inbound missile with defendProb, kill/death table per difficulty in the report;
AI think + steer <= 0.2 ms per enemy per tick on the VM.

## T16 - Weapon HUD: launch-zone circle, SHOOT, ACT/TTG, seeker, warnings  (H)

**User rule (2026-11-13)**: the launch-zone/ASE circle (with its ticks and range pointer) must never touch the speed and altitude boxes; use T07's layout constants (`hud.aseRadius`, centre) and keep its gap check passing.

**Owns/creates**: `src/hud_wpn.*`, `-hudtest zone` (scene in hud_wpn.cpp, one dispatch line), `docs/HUD_LAYOUT.md`
(append).
**May read**: DESIGN 10.6, 10.7, 12, `docs/research/HUD.md` 2; `dlz.h`, `weapons.h`, `radar.h`.
**Details**: circle with Rmin/Rne/Rmax ticks, range and aspect pointers, steering dot, SHOOT steady/flashing, breakaway
X, Vc/RNG/TOF->ACT->TTG readouts, chase DLZ bar, SRM seeker circle states + tone requests (`snd` events), VISUAL
maddog cue, weapon line + counts, MISSILE/threat arrows/LOCK warnings.
**Acceptance**: `-hudtest zone` exit 0 (SHOOT only when the published zone says so, ACT counts down whole seconds and
switches to TTG at activation, no flicker); screenshots of each state reviewed against HUD.md 2.1; <= 0.4 ms.

## T17 - Combat integration  (H)

**Goal**: the playable M2 build: TRAINING and a 1-wave SORTIE.
**Owns/creates**: `src/mission.*`, `src/tests_mission.cpp`, `src/fx.*` (combat section: trails, explosions, flares,
smoke, debris, dots), `src/snd_combat.*`, `src/main.cpp` (mode selection by switches, combat wiring), `src/scene.*`
(aircraft list, distance dots), `bench.cpp` route `combat`, `jet.ini` regenerated, `docs/PLAYTEST_M2.md`.
**May read**: everything.
**Details**: DESIGN 14.1-14.3 (damage, respawn, TRAINING drones, 1-wave SORTIE), fx of 9.5 within 9.3 caps, combat
sounds of 15 (positional), V look-at-target camera (7), aircraft mesh/dot distances of 9.1.
**Acceptance**: 1. `-test mission` exit 0; 2. headless robot sortie `-mode sortie -headless -exitafter 600` completes
or fails cleanly with a results line; 3. screenshots: missile launch + trail, explosion, flares, damaged smoke,
both views with the full HUD; 4. `-bench 60 -benchroute combat` runs (gate in T18).

## T18 - M2 performance pass  (M)

**Owns**: `docs/PERF.md` (M2 section), preset values; approved small optimisations.
**Acceptance**: combat route + the four M1 routes on Low: fps_avg >= 29.8, missed <= 2 % (superseded 2026-11-14 by
the user: gate = fps_avg >= 15, no sustained stretch below 15; 30 nice to have); DLZ and HUD within their
budgets (9.3).

---------------------------------------------------------------------------------------------------------------

## M3: polish and release

## T19 - Frontend: menus, options, key capture, pause, results  (H)
**Owns/creates**: `src/menu.*`, `src/main.cpp` (app states), `-mscript` flows, `docs/PLAY_HOWTO.md`.
**Details**: DESIGN 14.5; key capture for every action of 5.4 incl. invert pitch; settings saved to `jet.ini`; title
with an in-engine fly-by.
**Acceptance**: `-mscript tests/t19/*.scr` flows pass on the VM; screenshots of every screen.

## T20 - 3-wave sortie, messages, score, records  (M)
**Owns/creates**: `src/mission.*` (after T17), `records.ini` handling, `src/tests_mission.cpp` (extend).
**Acceptance**: `-test mission` exit 0 incl. scoring examples of 14.4; robot sorties on all difficulties complete.

## T21 - Balance pass from play-test notes  (M)
**Owns**: params defaults (flight, ai, weapons, mission) per the play-test notes; `docs/PLAYTEST_M2.md`/`_M3.md` notes.
**Acceptance**: the user's verdict; `simh shots` and `simh aifight` tables before/after in the report.

## T22 - Visual polish  (M)
**Owns/creates**: `src/fx.*` (polish section), `src/scene.*` (polish items), `src/camera.*` (shake/FOV polish).
**Details**: wingtip vapour at high G, contrails above 8 km, blob shadow of the jet on terrain/runway below 300 m,
cloud fly-through fade and haze, explosion/smoke polish, wreck smoke column.
**Acceptance**: Low still passes T18's gate; screenshots.

## A10 - Frontend art (M)
**Owns/creates**: `tools/art/gen_front.py`; `data/tex/logo.bmp` + `_a`, menu panels (9-slice), `data/jet.ico`;
previews. **Acceptance**: `--check`, validate, preview of the title layout at 640x480.

## A11 - Island detail (M)
**Owns/creates**: `tools/art/gen_veg.py` (palms/shrubs crossed billboards, rocks), `island/island.txt` scatter keys
(coordinate with A6's owner / orchestrator), base props (fences, lights, vehicles). **Acceptance**: `--check`; budgets
in `report.txt`; T18 gate still passes (measured by the orchestrator).

## A12 - Enemy aircraft model (only if Q3 = own model) (H)
**Owns/creates**: `tools/art/gen_bandit.py`, `data/meshes/bandit*.msh`, textures. Same budgets/tags as A1.
**Acceptance**: as A1.

## T23 - Real-hardware pass (High, 60 fps) with the user (M)
**Owns**: High preset values, `docs/PERF.md` (real hardware section). Needs the user to run builds on the Radeon PC.
**Acceptance**: High at 60 fps on the target PC (user's run, bench report), Low re-checked on the X41 if the user wants.

## T24 - Release (M)
**Owns/creates**: version resource, `README.md`, `dist/` (Setup98 manifest per `../setup98/AGENTS.md`, `package.sh`),
release notes. **Acceptance**: installer + portable zip install and run on the VM (`-test all` exit 0 from the
installed folder); user approval before any public repo/release.
