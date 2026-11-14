# Jet: Design Document (rev 1, 2026-11-13)

Working folder: `~/Documents/86box/Shared/dev/jet/` (= `M:\dev\jet` in Win98). Game name **Ashfall: Island Intercept** (user, 2026-11-14; was working title "Jet"). Executable
`jet.exe`, config `jet.ini`, log `jet.log`. Owner session **86box-d5**, **VM A** (`samba/w98`, `samba/vmlock`).

This document is the contract between all implementation and art agents; `TASKS.md` refers to its section numbers.
Binding inputs: `STATUS.md` (user vision + decisions), `docs/research/HUD.md` (HUD look), `docs/research/ENGINE.md`
(engine reuse plan). `docs/research/WEAPONS.md` is background for HAFU/radar/launch-zone *concepts* only: all weapon
numbers in this game are our own invented, tunable game numbers (section 12). If something here is ambiguous, pick
the simplest interpretation, write it at the top of your file and in your report.

**Spec rule:** minimum = 86Box VM A (Pentium II 233, Voodoo3, DX 6.1): **Low preset, 640x480x16, 30 fps**. Real target
= Pentium M + Radeon 9000: **High preset, 60 fps**. The VM is the only dev machine; don't cut High to fit the VM.

**Two top priorities (user):** (1) **flight feel on the keyboard**, (2) **HUD quality**: readable, clear, stable
symbols. The game is BVR-first: beyond a few km the HUD and minimap symbols are the *only* picture of an enemy.

---------------------------------------------------------------------------------------------------------------

## 0. Summary (one page, for review)

**What you fly.** An original F-16-style single-engine fighter with afterburner. Its tail planes (stabilators) and
wing flaperons move visibly when you pitch and roll. One volcanic island in the open sea: a big volcano with a
crater, beaches, and on the coast a flat airbase with a 2.8 km runway, a few hangars and a tower.

**How it starts.** You sit on the runway. Press **A** and the jet rolls, lifts off, raises the gear and climbs out
by itself to about 1500 ft. "YOU HAVE CONTROL" and it's yours (you can take over earlier by steering).

**How it controls (keyboard, Ace Combat "Expert" style).**
- **Up/Down arrows** = nose up/down (default Up = nose UP; invertible). **Left/Right** = roll. No rudder: the jet
  is auto-coordinated, so you turn by rolling and pulling, like Ace Combat.
- A tap gives a tiny, precise nudge (for aiming); holding builds up to the full rate in about half a second. Let
  go and the jet holds its attitude: the bank stays where you left it (it snaps to wings-level within 4 degrees).
- **A held** = afterburner, fast (towards Mach 1.6), but turning is limited by the 9 G limit, so wide turns.
  **Nothing** = cruise at Mach 0.8. **Z held** = speed brake: down to manoeuvring speed (350 kt) quickly, the
  tightest turns, then slowly further to the 150 kt floor. No stall.
- Climbing costs speed, diving gains it, hard turns bleed it. All feel numbers live in `jet.ini`; **F5 reloads
  them live** while you fly, so tuning sessions are quick.

**What the screen shows.**
- **Cockpit view:** only the green F/A-18C-style HUD over the world: heading tape on top, airspeed box left,
  altitude box right, AOA/Mach/G under the speed box, pitch ladder, velocity vector, bank scale, weapon line.
- **Chase view (C toggles):** the jet from behind with a reduced HUD (speed, altitude, G, aim cross, velocity
  vector, horizon, target symbols, weapon block).
- **Minimap** bottom-left in both views: heading-up radar map with the island, the runway, contacts and missiles.
- **Targets:** each radar contact gets a HAFU-style symbol at its position: **yellow** = unknown, **red** = hostile
  once our radar has identified it (takes about 1.5 s). Range is written under it. Enemy aircraft are only drawn
  as 3D models within about 2.5 km (beyond that they would be 1-2 pixels anyway); further out the symbol *is* the
  enemy, so symbols are big, steady and never flicker.

**How combat works.**
- **S tap** = select the next contact (a small triangle marks it). **S hold (0.3 s)** = designate it: the radar
  locks and a target box goes around it. **Space** = fire. **D** = switch missile type. **F** = flares.
- Two missiles, many of each (Ace Combat style counts): a long-range radar missile (16) with an "ACT/TTG" countdown
  after launch, and a short-range heat seeker (40) with a seeker circle and a growl/lock tone.
- With a missile selected the HUD shows the Hornet launch-zone circle: ticks for minimum range, no-escape range and
  maximum range, a pointer for the target's range, "SHOOT" (steady inside max range, flashing inside the no-escape
  zone). These numbers come from simulating *our own* missile against the target, so what the HUD says is what
  happens: inside the no-escape zone a shot almost always hits, unless the target hides behind the volcano and the
  missile hits the ground.
- Enemies (2-5 per wave) fly the same flight model, lock you, fire missiles at you, defend with turns and flares and
  try to hide behind terrain. A warning tone and "MISSILE" with a direction arrow tell you when to break and flare.
  Three hits and you are down (tunable; to be revisited); you start again from the runway (3 lives per sortie).

**Milestones (each ends with a user play-test).**
- **M0** (no play-test): engine copied from the racer and building; depth/fog probe on the VM decides the depth
  setup; island compiler and first art in parallel.
- **M1: free flight** around the island with the full flight HUD, both views, takeoff, crash/respawn, 30 fps on
  the VM. **Feel play-test: the most important gate**; feel-tuning rounds until the user is happy.
- **M2: combat**: radar, contacts, symbology, both missiles, launch zone, flares, enemy AI, a training mode and one
  combat sortie. Combat play-test.
- **M3: polish**: menus, the 3-wave mission with score and records, options and key rebinding, visual polish,
  real-hardware High pass, Setup98 installer + zip.

---------------------------------------------------------------------------------------------------------------

## 1. Scope, non-goals, lessons

### 1.1 In scope for 1.0
One island, one player aircraft type, one enemy type (the same jet in a different colour shade; user 2026-11-13), two missile types, flares,
radar/HAFU/launch zone, enemy AI with missiles, takeoff automation, cockpit + chase views, free flight + training +
one 3-wave sortie, score/records, menus, keyboard only (no joystick for now, user 2026-11-14), Setup98 installer.

### 1.2 Non-goals for 1.0 (cut on purpose)
Landing; stall/spin; yaw control; fuel; gun (user: missiles only); ground targets; wingmen/friendlies; chaff, notching, ECM
(flares decoy both seeker types instead, arcade); radar modes/antenna elevation; datalink; weather/night; multiple
islands; campaign; multiplayer. Each one is a line in `docs/` if the user asks for it later.

### 1.3 Lessons from the racer (why this design looks the way it does)
- The racer modelled real tyres first and shaped the keyboard on top; it never felt right after 3 rounds. **Here the
  model is arcade from the start**: the player commands *rates*, the model turns them into G within a G limit, and
  every feel number is an ini key with live reload (F5) and an on-screen feel overlay (F10).
- Robots and scripted tests catch regressions but **do not judge feel**: the M1 gate is the user's hands. M1 is kept
  lean so the user gets the controls early.
- Test logs, replays (`last.rpl` of every flight) and text stat dumps are the debugging tools; screenshots only for
  visuals (user preference).

---------------------------------------------------------------------------------------------------------------

## 2. Architecture

### 2.1 Folder layout

```
jet/
  Makefile                 NMAKE (VC6): engine\engine.lib, then jet.exe (adapted from racer, 2.8)
  jet.ini                  defaults + every tunable (generated by `jet.exe -dumpini`, 2.5)
  play.bat                 user launcher (fullscreen, Low preset on the VM)
  engine/                  COPY of racer/engine (2026-11-13) minus phys/; see docs/ENGINE_COPY.md
    core/  coll/  world/  rend/  plat/
    world/e_terrain.*      NEW: island heightmap chunks (5 LODs), height/LOS queries, sea (8)
    tools/assetkit/        COPY (validate.py budget classes edited for jet)
  sdk/                     COPY of racer/sdk (DX6 headers + OWPL licence). Read-only for tasks
  probe/                   M0 depth/fog/HUD-line probe (T02) + RESULTS.md. Read-only after T02
  src/                     GAME (2.4)
  data/                    shipped: meshes/*.msh tex/*.bmp sky/*.bmp sfx/*.wav island/jet.isl island/base.trk
  island/island.txt        island source (A6 owns the values, T03 owns the format)
  tools/                   Mac side: build_assets.py, islandc/ (island compiler), art/gen_*.py, sfx/gen_*.py,
                           simh/ (Mac headless sim harness)
  tests/                   scripts (.scr), expected outputs, user replays
  docs/                    DESIGN.md TASKS.md HANDOFF.md ENGINE_COPY.md PLAYTEST_*.md research/
  dist/                    Setup98 manifest, package.sh (M3)
```

### 2.2 Layers and the portable-core rule (same rule as racer DESIGN 2.2)

```
 game (src/)     main scene camera hud* fx snd menu bench | flight pilot takeoff world radar target missile
                                                          | weapons dlz ai mission params replay tests
                 ---------------- Windows ---------------- | -------------------- portable --------------------
 engine          plat input audio ui | rend (d3d6, null) | world mesh tex | e_terrain (load+queries portable) | core coll
```
Portable files include only `e_base.h`, other portable headers and the C library. They compile with VC6 `/W3` and
with `clang++ -std=c++98 -Wall` on the Mac. **Everything that changes game state is portable**; Windows files read
state (render, sound, HUD) and feed input through `PilotInput` (5.1). Headless runs = rendered runs; Mac tests;
deterministic replays.

### 2.3 What is copied, what is new

| Part | From racer | Change |
|---|---|---|
| engine/core (base, math, log, cfg, file, image, prof) | COPY | `e_cfg`: add `cfg_reload` (re-read file, report changed keys) |
| engine/coll | COPY | unused but linked (e_world includes it) |
| engine/phys | DROP | |
| engine/world (e_tex, e_mesh, e_world) | COPY | none; e_world is used for the airbase `.trk` + sky only |
| engine/world/e_terrain | NEW | section 8 |
| engine/rend | COPY | add `r_clear_z()` (only if the probe picks Plan B, 9.1) |
| engine/plat (e_plat, e_input, e_audio, e_ui) | COPY | e_plat title/class/error caption "Jet"; e_input: append `in_action_held_ticks(a)`, `in_action_released(a)`; e_ui: append `ui_line(x0,y0,x1,y1,w,argb)` + `ui_set_blend(RB_ADD/RB_BLEND)` + `ui_clip(x,y,w,h)` |
| sdk/ | COPY | none |
| assetkit | COPY | `validate.py` classes `jet`, `jetpart`, `missile`, `bldg`, `fx` (8.8) |
| src/main.cpp, bench.cpp, replay.cpp, tests.cpp | pattern | skeletons copied, content rewritten |
| tools/art/gen_car.py, gen_ground.py, gen_sky.py, gen_ui.py, sfx/gen_sfx.py | pattern | generators rewritten for jet (A-tasks) |
| tools/trackc terrain.py, light.py | pattern | ideas for islandc (T03) |

Record the copy in `docs/ENGINE_COPY.md` (date, racer files, every jet edit). Port fixes by hand both ways when they
matter. Never `#include` anything from `../racer`.

### 2.4 Module table

| Module | Files | Portable | Purpose | Task |
|---|---|---|---|---|
| game types | `src/game.h` | yes | constants, `Aircraft`, `World`, ids (append-only after T01) | T01 |
| params | `src/params.*` | yes | ini-backed tunable registry, F5 reload, `-dumpini` (2.5) | T01 |
| flight | `src/flight.*` | yes | arcade flight model (4) | T05 |
| pilot | `src/pilot.*` | yes | keys -> shaped axes, throttle state, short/long press classifier (5) | T05 |
| takeoff | `src/takeoff.*` | yes | ground roll, auto takeoff + climb-out, handover (6) | T06 |
| world | `src/world.*` | yes | island queries wrapper, bounds, crash test, spawn points, PULL UP predictor (6, 8) | T06 |
| radar | `src/radar.*` | yes | detection, ID, tracks, memory, RWR events (11) | T11 |
| target | `src/target.*` | yes | selection/designation from S short/long, lock (11.4) | T11 |
| missile | `src/missile.*` | yes | missile model + guidance, one step function shared by live and fly-out (12) | T12 |
| weapons | `src/weapons.*` | yes | stores, firing, flares, hits, terrain impacts (12) | T12 |
| dlz | `src/dlz.*` | yes | launch-zone solver (time-sliced fly-outs) + AI tables (12.4) | T13 |
| ai | `src/ai.*` | yes | enemy pilots (13) | T14 |
| mission | `src/mission.*` | yes | modes, waves, damage, respawn, score (14) | T17 |
| replay | `src/replay.*` | yes | input record/playback, param snapshot, state hashes (16) | T01, T08 |
| tests | `src/tests.cpp` + `src/tests_<area>.cpp` | yes | `-test` dispatcher; each task owns its tests file | T01 + each |
| main | `src/main.cpp` | no | WinMain, loop, args, app states | T01, T08 |
| camera | `src/camera.*` | no | cockpit + chase cameras (7) | T08 |
| scene | `src/scene.*` | no | draw order, sky, terrain, sea, base, aircraft, clouds, presets (9) | T08 |
| hud | `src/hud.*` (flight), `src/hud_font.*`, `src/hud_map.*` (minimap) | no | flight HUD, stroke text, minimap (10) | T07 |
| hud target | `src/hud_tgt.*` | no | HAFU symbols, selection, TD box, locator, off-screen arrows, minimap contacts (10.5) | T15 |
| hud weapons | `src/hud_wpn.*` | no | launch-zone circle, SHOOT, ACT/TTG, seeker, warnings (10.6) | T16 |
| fx | `src/fx.*` | no | trails, explosions, flares, smoke, afterburner flame (9.5) | T17 (flame: T08) |
| snd | `src/snd.*` (flight), `src/snd_combat.*` | no | sound logic over e_audio (15) | T08, T17 |
| menu | `src/menu.*` | no | frontend, pause, results (14.5) | T19 |
| bench | `src/bench.*` | no | `-bench` routes and report (16.4) | T08, T09 |
| terrain | `engine/world/e_terrain.cpp` (portable), `e_terrain_draw.cpp` (render) | load+query yes | 8 | T04 |

### 2.5 Tunables: `params` and live reload

- Each portable module declares its tunables in a static table: `{ "flight", "rollRateTable", P_TABLE, &s_rollTab,
  "150:110,250:180,400:220,700:200", min, max, "help" }`. Types: float, int, bool, table (`x:y,x:y` piecewise
  linear, up to 12 points). Units are in the key name or help text (kt, deg, deg/s, s, m, g).
- `params_load(cfg)` at start; **F5** = `cfg_reload` + `params_load` again; logs one line per changed key
  (`params: reload flight.rollRateTable old=... new=...`) and shows "INI RELOADED (n)" on screen for 2 s. Bad values
  are clamped and logged, never fatal. Sections reloadable live: `[pilot] [flight] [takeoff] [camera] [hud] [radar]
  [weapons] [dlz] [ai] [mission] [sound]`. `[video]` needs a restart.
- `jet.exe -dumpini file` writes every key with its default and help comment (CRLF). The shipped `jet.ini` is
  generated this way, so the docs never drift from the code.
- The replay header stores a snapshot of all params; an F5 during a recorded flight writes a PARAMS record at that
  tick, so replays stay exact.

### 2.6 Main loop and tick order

Same structure as racer DESIGN 2.6: integer QPC accumulator, **fixed 120 Hz sim tick** (`SIM_DT = 1/120`), clamp
0.25 s, max 8 ticks per frame, render interpolation with `alpha`, display-locked pacing (`RInit.fpsTarget` 30 Low /
60 High). `sim_tick()` order in APP_FLIGHT:

```
1 in_poll (or script/replay) -> pilot_update(&player.pilot, keys)        5.1-5.3 (short/long press classified here)
2 target_update (S events)                                               11.4
3 ai_think (staggered 10 Hz per enemy: (tick + id*12) % 12 == 0) ; ai_steer every tick -> enemy PilotInput  13
4 takeoff_tick for the player while in ground/auto phases                6
5 flight_step for every aircraft                                         4
6 world_check: ground/sea/box/bounds -> crash events; PULL UP predictor (2 Hz)   6.4
7 weapons_tick: fire requests, missiles step (missile_step), flares, hits, terrain impacts   12
8 radar_tick (10 Hz, staggered per contact; RWR events)                  11
9 mission_tick: damage, deaths, respawns, waves, score, messages         14
10 replay record / hash every 120 ticks
```
Per **frame** (not tick): `dlz_work(budgetSteps)` refines the player's launch zone (display only, deterministic in
result, 12.4), camera, scene, HUD, sound.

### 2.7 Command line (main.cpp; ini keys in brackets)

| Switch | Meaning |
|---|---|
| `-window` / `-fullscreen`, `-res W H`, `-preset low\|high` | video [`video.*`] |
| `-headless` | no window/D3D/sound/input; sim flat out; player flown by `-script` or the AI pilot |
| `-mode free\|training\|sortie`, `-diff 0\|1\|2`, `-seed N` | skip menus and start (M1: `free` is the only mode) |
| `-start runway\|air` | start on the runway (default) or in the air at 1500 m |
| `-log [N]`, `-exitafter T`, `-shot T file.bmp`, `-camat X Y Z HDG PITCH` | as racer |
| `-script file` | timed key presses/releases + `expect` checks (16.2) |
| `-record file` / `-replay file` | input record/playback + param snapshot + hashes; every flight writes `last.rpl` |
| `-telemetry file` | CSV per tick for the player: keys, axes, n, V, alpha, bank, pitch, heading, alt, throttle state |
| `-test NAME` | self-tests (16.1); exit code = failures |
| `-bench N [-benchroute low\|volcano\|high\|combat]` | autopilot bench route, writes `bench.txt` (16.4) |
| `-aifight N` | headless AI-vs-AI or AI-vs-robot fights, prints stats (16.3) |
| `-hudtest NAME` | HUD test scenes: scripted flight-state sweeps over a static scene, logs displayed strings (10.8) |
| `-dumpini file`, `-dev` (enables F9 respawn, F10 overlay without the ini flag) | |
| `-nocap`, `-fps`, `-nosound`, `-nojoy` | as racer |

### 2.8 Build
As racer DESIGN 2.8 with: no `engine\phys` in `/I` paths or `EOBJS`; `e_terrain.obj e_terrain_draw.obj` added;
`GOBJS` = jet's `src\*.obj`; output `jet.exe`. `w98 build jet`. Mac: `make -C tools/simh` builds `tools/simh/simh`
from every portable file (`clang++ -std=c++98 -O2 -Wall -Wno-unused-function -DE_NO_RENDER`).

### 2.9 Coding rules
Exactly racer DESIGN 2.9 (C-style C++, no STL/exceptions/RTTI/templates, no allocation per tick, loop variables at
the top of the function, `float` + `(float)sin()`, `e_snprintf`, no `<stdint.h>`, never name things
`near/far/min/max/small/interface`, `e_ftoi` in inner loops, `/W3` clean, ASCII sources). **FPU: PC24 everywhere**
(`plat_fpu_setup`), so all sim math is `float`, all time keeping is integer ticks.

---------------------------------------------------------------------------------------------------------------

## 3. Units and coordinates

- Internal SI: metres, seconds, m/s, radians. Degrees only in ini/UI. Left-handed D3D convention: **+X east, +Y up,
  +Z north**. Island centred on the origin, **sea level y = 0**. Everything stays within +-32 km (float resolution
  <= 2 mm). Never move the origin.
- Heading 0 = north (+Z), 090 = east (+X), clockwise. Pitch + = nose up. Bank + = right wing down.
- Aircraft body frame: +Z forward, +X right wing, +Y up. Orientation = unit quaternion body -> world.
- HUD/UI units (user decision): **knots, feet, Mach, NM**. 1 kt = 0.514444 m/s, 1 ft = 0.3048 m, 1 NM = 1852 m.
  Speed of sound `a(h) = 340.3 - 0.00405 h` m/s (h <= 11 km), 295.7 above. **Airspeed shown = true airspeed** (no
  density model; the speed box reads like the game's speed). Altitude = MSL ("baro"), VVI ft/min.
- Time: sim ticks (120 Hz, u32 counter). Displayed clock from ticks.

---------------------------------------------------------------------------------------------------------------

## 4. Flight model (`src/flight.*`, portable) - arcade by design

### 4.1 Idea
A point mass with a speed scalar and an orientation. The player commands **pitch rate and roll rate** (shaped keys,
5). Pitch rate becomes **load factor n** (G) inside a **G limit that depends on speed**; turn rate follows from
physics (`omega = g*(n - n_hold)/V`), so fast = wide turns, slow = tight turns, without any special rule. Neutral
pitch holds the flight path (an auto-trim, like a fly-by-wire jet). No sideslip: gravity's sideways component in a
bank turns the jet (auto-coordination). Speed goes towards a target set by the throttle state, with energy trade
(climb/dive), induced drag in hard turns and a hard 150 kt floor. Enemies use the same model (fairness and honest
launch zones).

### 4.2 State (`FlightState`, per aircraft)

| Field | Meaning |
|---|---|
| `pos` (Vec3, m) | centre of mass |
| `qPath` (Quat) | flight-path frame: forward = velocity direction, up = lift direction, right = right wing |
| `V` (m/s) | true airspeed |
| `n`, `nCmd` (g) | load factor (shown as G), its command |
| `p` (rad/s) | roll rate (lagged) |
| `alpha` (rad) | cosmetic angle of attack: nose = path rotated up by alpha (HUD alpha, VV vs waterline, chase look) |
| `thr` | throttle state SLOW / NORMAL / FAST; `ab` 0..1 (afterburner visual/sound level, lagged) |
| `brake` 0..1 | speed brake visual |
| `onGround`, `gearPos` 0..1 | takeoff phases (6) |
| `peakG`, derived `heading/pitch/bank/gamma`, `mach` | for HUD; computed once per tick |

Render state = `qNose = qPath * rotX(-alpha)` (nose up), interpolated with `alpha`-blend between ticks.

### 4.3 Per-tick step (dt = 1/120 s)

Inputs from `PilotInput`: `up` (pitch axis -1..1, + = nose up after the invert option), `ur` (roll axis -1..1),
`thr` state. Let `gamma` = path pitch, `phi` = bank, `g` = 9.81.

```
nHold   = cos(gamma) * cos(phi)                         // lift that keeps the path straight (auto-trim)
nLim    = table(gLimitTable, V_kt) * turnBoost          // + G limit at this speed
nNeg    = negGLimit                                     // e.g. -3
if (up >= 0) nCmd = nHold + up * (nLim - nHold)
else         nCmd = nHold + (-up) * (nNeg - nHold)
nCmd    = clamp(nCmd, nHold - qCap*V/g, nHold + qCap*V/g)   // pitch-rate cap
nCmd    = clamp(nCmd, nNeg, nLim)
nCmd   -= floorDrop(V, gamma)                           // 4.5: at the speed floor the nose sinks towards the horizon
n      += (nCmd - n) * (1 - exp(-dt / tauG))            // G onset smoothing

pCmd    = ur * table(rollRateTable, V_kt) * DEG2RAD
if (ur == 0 && |phi| < detentDeg && |p| < 0.5 rad/s) pCmd = -phi * detentGain   // wings-level detent
p      += (pCmd - p) * (1 - exp(-dt / tauRoll))

qRate   = g * (n - nHold) / V                           // path pitch rate about path right axis
rRate   = latGrav * g * sin(phi) * cos(gamma) / V       // auto-coordination: gravity's side component turns the jet
qPath   = qPath * quat(right, qRate*dt) * quat(up, rRate*dt) * quat(forward, -p*dt); normalise every tick

// speed
Vt      = target(thr)                                   // 4.4
aThr    = (V < Vt) ? min(accMax(thr), (Vt - V)/tauV) : max(-decMax(thr), (Vt - V)/tauV)
aGrav   = -g * sin(gamma) * energyTrade
aInd    = -inducedPerG * max(0, |n| - 1)
V      += (aThr * flight_thrust_lapse(h) + aboveCeiling(h) + aGrav + aInd) * dt ; V = clamp(V, Vfloor, Vmax)

pos    += forward(qPath) * V * dt
alpha  += (clamp(alpha1g * n * (Vref/V)^2, alphaMin, alphaMax) - alpha) * (1 - exp(-dt/tauAlpha))
peakG   = max(peakG, n) ; reset when the HUD peak display times out (10.3)
```
Note: with `latGrav = 1` this is exact point-mass flight for the commanded lift. With no pitch input in a 60 degree
bank the jet slowly turns and the nose slowly falls; pulling gives the level turn (n = 1/cos(phi)), as in Ace Combat.

### 4.4 Throttle states (user: A = fast, nothing = normal, Z = slow)

| State | Key | Target `Vt` | Accel / decel |
|---|---|---|---|
| FAST | A held | `machFast` x a(h) (default Mach 1.6) | `accAB` up; afterburner lit (`ab` -> 1 in 0.3 s) |
| NORMAL | none | `machNormal` x a(h) (Mach 0.8) | `accMil` up, `decOver` down when above (after a dive or AB) |
| SLOW | Z held | while V > `vManKt` (350 kt): decel `decBrake` towards it; then `decSlow` towards `vFloorKt` | speed brake visual + sound |
Releasing Z returns to NORMAL. A and Z together = NORMAL. `Vmax` = Mach 2.0 hard clamp.

### 4.5 Floor and ceiling (no stall model)
- **Speed floor** 150 kt: `V` never drops below. So a vertical climb doesn't hang forever: within `floorBandKt` (20 kt)
  of the floor and with `gamma > 0`, `floorDrop` lowers `nCmd` so the path rotates towards the horizon at up to
  `floorDropDeg` deg/s (smoothstep in the band). Pushing the nose down stays possible.
- **Altitude (T10a, user M1 round 1, replaces the old ceiling):** throttle acceleration x `flight_thrust_lapse(h)`
  (density^0.7 curve: 100 % up to `thrustFullM` 6000 m, 0 at `ceilingM` 15 500 m; table rebuilt on F5). Above the
  ceiling only: `thinAirDecel` (0 at the floor speed, so dives never pin), `zoomDrag` (climbing only) and a gentle nose
  drop towards a 5 deg descent. Gravity always acts: a dive always gains speed. `ceilSoftM/ceilHardM/ceilDrag` are gone.

### 4.6 Tunables (`[flight]`, all live-reloadable; defaults are the M1 starting point)

| Key | Default | Meaning |
|---|---|---|
| `gLimitTable` | `150:4.5,200:6,250:7.5,300:8.5,330:9,700:9,1020:7,2000:6` | G limit (g) vs true airspeed (kt). User 2026-11-13: going fast limits G (about M1.1 -> 9 g, M1.6 -> 7 g) |
| `turnBoost` | 1.0 | multiplies the G limit (global "how tight") |
| `negGLimit` | -3.0 | push limit (g) |
| `pitchRateCapDeg` | 30 | max path pitch rate (deg/s) |
| `tauG` | 0.10 s | G onset lag |
| `rollRateTable` | `150:110,250:180,400:220,700:200` | max roll rate (deg/s) vs kt |
| `tauRoll` | 0.06 s | roll rate lag |
| `detentDeg`, `detentGain` | 4, 1.5 /s | wings-level detent (0 = off) |
| `latGrav` | 1.0 | gravity side component -> turn (0 = "floaty" bank) |
| `machNormal`, `machFast` | 0.80, 1.60 | throttle targets |
| `vManKt`, `vFloorKt` | 350, 150 | Z targets |
| `accMil`, `accAB` | 6, 18 m/s^2 | thrust acceleration caps |
| `decOver`, `decBrake`, `decSlow` | 5, 15, 2.5 m/s^2 | decelerations |
| `tauV` | 2.5 s | approach to target speed |
| `energyTrade` | 1.0 | climb/dive speed exchange (0 = none) |
| `inducedPerG` | 0.9 m/s^2 per g | speed bleed in hard turns |
| `vMaxMach` | 2.0 | hard speed clamp |
| `floorBandKt`, `floorDropDeg` | 20, 8 | 4.5 |
| `thrustFullM`, `ceilingM`, `thinAirDecel`, `zoomDrag` | 6000, 15500, 0.5, 3 | 4.5 (T10a) |
| `alpha1gDeg`, `alphaRefKt`, `alphaMinDeg`, `alphaMaxDeg`, `tauAlpha` | 2.0, 300, -6, 20, 0.15 | cosmetic AOA |
| `stabMaxDeg`, `stabRollDeg`, `flapMaxDeg`, `surfTau` | 20, 8, 20, 0.08 | control surface animation (4.8) |

### 4.7 Feel card (what the defaults give; `simh feel` prints this table, `-test flight` checks it +-10 %)

| Situation | Value |
|---|---|
| Level sustained turn rate at NORMAL (510 kt, 3000 m, 9 g) | ~19 deg/s, 360 in ~19 s (speed then bleeds towards corner) |
| At 350 kt (Z), 9 g | ~28 deg/s, 360 in ~13 s |
| At 150 kt floor, 4.5 g | ~32 deg/s |
| At FAST Mach 1.6 (~1020 kt), 7 g (G limited at speed) | ~7.4 deg/s, 360 in ~49 s |
| Roll 0 -> 90 deg bank, key held, 500 kt | ~0.6 s; full 360 roll ~1.8 s |
| Pitch key: full command after | ~0.5 s hold (5.2) |
| 0.1 s tap of Up at 500 kt | ~0.3-0.6 deg nose change (aiming nudge) |
| NORMAL -> FAST (sea level) | ~15 s to within 5 % |
| FAST -> 350 kt with Z | ~13 s; 350 -> 150 kt with Z held ~40 s |
| 30 deg climb at NORMAL power | loses ~2 kt/s |

### 4.8 Visual parameters for render
`FlightVisual` (computed in flight_step, interpolated by render): stabilator angle = `-stabMaxDeg * clamp((n - nHold)/
(nLim - nHold), -1, 1)` +- `stabRollDeg * ur` (left/right), flaperons = `+-flapMaxDeg * ur` (filtered with
`surfTau`), nozzle opening (0 normal, 1 AB), afterburner level `ab`, speed brake `brake`, gear `gearPos`, vapour
amount (`n > 6` and slow, for M3 polish).

---------------------------------------------------------------------------------------------------------------

## 5. Controls (`src/pilot.*`)

### 5.1 PilotInput
```c
typedef struct PilotInput { float up, ur; int thr; int fire, flare, wpnNext, tgtShort, tgtLong, view, look; } PilotInput;
```
Produced by `pilot_update` from keys (player), by `ai_steer` (enemies), or by the takeoff autopilot. Joystick:
analogue X/Y map straight to `ur/up` (deadzone 0.05, expo `joyExpo`), no ramps.

### 5.2 Keyboard shaping (`[pilot]`)
Axis model (racer KEYBOARD.md lesson: keys move an axis at a rate, never jump to full):
```
pitch: key held towards s:  |up| < pitchFine ? up += s*pitchRateFine*dt : up += s*pitchRate*dt   (towards s, no overshoot)
       key opposite to up:  up moves towards s at pitchCross
       no key:              up -> 0 at pitchReturn
roll:  same with rollRateIn, rollReturn, rollCross (one stage)
```

| Key | Default | Effect |
|---|---|---|
| `pitchFine` | 0.35 | first stage of the pitch axis (fine aiming) |
| `pitchRateFine` | 3.0 /s | 0 -> 0.35 in ~0.12 s |
| `pitchRate` | 1.8 /s | 0.35 -> 1 in ~0.36 s more (full at ~0.5 s) |
| `pitchReturn`, `pitchCross` | 6.0, 8.0 /s | release / reversal |
| `rollRateIn`, `rollReturn`, `rollCross` | 6.0, 10.0, 12.0 /s | full roll command in ~0.17 s |
| `invertPitch` | 0 | 0 = Up arrow nose UP (user default), 1 = inverted |
| `longPressMs` | 300 | S long-press threshold |
| `joyExpo`, `joyDead` | 0.3, 0.05 | joystick |

### 5.2b Novice control mode (user 2026-11-13: "include the novice style too")
Ace Combat's Novice/Normal scheme as a second mapping in `pilot.*` over the same flight model (no physics change):
Left/Right = turn (auto-bank toward `noviceBankDeg` 80 with level-turn lift, limited by the G limit; release =
auto wings-level); Up/Down = climb/dive relative to the horizon (gamma rate; gamma limited to +-`novicePitchLimitDeg`
75, so no loops or inverted flight); released = hold climb angle (`noviceAutoLevelPitch` 0). Up + Left = tighter
climbing turn. `[pilot] mode = expert|novice` (default expert, F5-reloadable); dev key F6 toggles in flight (T08 wires it).

### 5.3 Short vs long press (S key; generic classifier for any action)
Counted in sim ticks (exact, replayable): while the action is held, `held++`; **long fires the moment `held ==
longPressMs*120/1000` (36 ticks)**, so the lock feels immediate; on release, **short fires if long never fired**.
A tap shorter than one poll is still a short press (e_input's tapped bit). Focus loss resets `held` without events.
Built on the new `in_action_held_ticks/in_action_released` (2.3).

### 5.4 Default key map (all rebindable in `[controls]`, menu key capture in M3)

| Key | Action | Notes |
|---|---|---|
| Up / Down | pitch nose up / down | `invertPitch` swaps |
| Left / Right | roll left / right | |
| A (hold) | FAST: afterburner | also starts the takeoff roll |
| Z (hold) | SLOW: speed brake | |
| S tap / S hold 0.3 s | select next contact / designate (lock) selected; hold on the designated target = break lock | 11.4 |
| Space | fire selected weapon | 12.6 |
| D | next weapon (LRM <-> SRM) | |
| F | flares (one burst per press, repeat while held every 0.5 s) | |
| C | toggle cockpit / chase view | |
| V (hold) | look at designated target (chase: camera swings to keep jet + target in view) | M2 |
| Tab | minimap range 10 / 20 / 40 NM | |
| Esc / P | pause menu (M1: pause + "restart on runway" + quit) | |
| F5 | reload ini (live) | 2.5 |
| F9 | respawn in the air (dev, `-dev` or `[dev] keys=1`) | |
| F10 | feel overlay: axes, n, nLim, V/Vt, rates, alpha, small 4 s graph of up/ur/n | |
| F11 | fps overlay | |
| F12 | screenshot to `shots/jet_NNN.bmp` | |
Left hand rests on the home row: pinky A/Z (throttle), ring S (target), middle D (weapon), index F (flares), thumb
Space (fire); right hand on the arrows.

---------------------------------------------------------------------------------------------------------------

## 6. Takeoff, climb-out, ground, crash (`src/takeoff.*`, `src/world.*`)

### 6.1 Phases
| Phase | What happens | Ends when |
|---|---|---|
| READY | parked at the runway threshold, engine at idle, HUD message "A: TAKEOFF" | A pressed (any throttle-up) |
| ROLL | on-ground mode: position on the runway centre line, speed += `rollAccMil` (AB while A held: `rollAccAB`); nose wheel on | V >= `vRotateKt` (145) |
| ROTATE | path pitch eases to `rotatePitchDeg` (10) over 1.5 s | V >= `vLiftKt` (160): switch to the flight model (`qPath` from the current attitude, V) |
| CLIMB | autopilot drives `PilotInput` (wings level, path pitch `climbPitchDeg` 12, NORMAL or FAST if A held); gear up from 50 ft AGL (3 s) | AGL >= `handoverFt` (1500) and V >= 300 kt, or player override |
| HANDOVER | "YOU HAVE CONTROL" (2 s); autopilot's axes hand over continuously (the player's shaped axes start from the autopilot's values) | player flying |

Player override during CLIMB: a pitch/roll key held > 0.25 s above 200 ft AGL hands over at once. Ground roll is
the only non-flight-model motion. Defaults: `rollAccMil` 7, `rollAccAB` 11 m/s^2 (READY to airborne ~8-11 s,
handover ~25 s after pressing A). The runway, heading and threshold come from the island file (8.6 `BASE`).

### 6.2 Air start and respawn
`-start air`, F9 and mission respawns: at the nearest of 4 air spawn points (8.6), 1500-2500 m, NORMAL speed, wings
level, heading towards the island centre, gear up, 3 s invulnerable in combat modes (HUD blinks).

### 6.3 Crash and bounds
- Crash if `pos.y - surface(x, z) < crashClearM` (1.5 m) while not onGround; `surface = max(terrain_height, 0)`;
  also sphere (r 5 m) vs hangar/tower boxes; aircraft-aircraft sphere (r 6 m) = both crash.
- Crash -> explosion, 2.5 s wreck camera, then restart **on the runway** (user: always start from the airfield); the
  automated takeoff runs again, and steering takes over early. Air start stays for dev/tests only (`-start air`, F9).
- Area: radius 30 km from the island centre. At 26 km "RETURN TO AREA" + minimap arrow; at 32 km the autopilot
  turns the jet back (inputs overridden until heading within 30 degrees of the centre).

### 6.4 PULL UP
At 2 Hz sample `surface` at `pos + v*t` for t = 1, 2, 3, 4, 5 s (plus the straight-down AGL); impact predicted within
`pullUpSec` (4 s) -> "PULL UP" flashing + tone. Suppressed in takeoff phases and below 200 ft over the runway.

---------------------------------------------------------------------------------------------------------------

## 7. Cameras and views (`src/camera.*`, `[camera]`)

| | Cockpit (HUD-only) | Chase |
|---|---|---|
| Eye | `cam_cockpit` tag on the jet mesh (fallback 4.5 m ahead of CG, 1.0 m up) | behind/above: `chaseDist` 24 m, `chaseUp` 6 m in the camera frame |
| Orientation | `qNose` exactly (screen centre = boresight = HUD waterline) | slerp towards `qNose` with `tauCamRot` 0.10 s (roll `tauCamRoll` 0.16 s): the jet visibly leads the camera in manoeuvres |
| Look point | - | jet position + forward * `chaseLook` 40 m |
| Own aircraft drawn | **no** (user: only the HUD visible) | yes, LOD0 + moving surfaces + flame |
| FOV (horizontal) | `fovCockpit` 65 deg (HUD.md 1.1 scale) | `fovChase` 70 + `fovAB` 5 at full AB (speed feel) |
| Near plane | 1.0 m | 1.5 m (see 9.1) |
| Shake | `shakeG` small above 7 g, `shakeAB` tiny in AB | same |
| V (hold, M2) | head turns towards the TD target up to 120 deg | camera swings to frame jet + target |

Chase camera collision: keep the eye 3 m above `surface`. View switch is instant. Camera code is render-side and
reads only interpolated state.

---------------------------------------------------------------------------------------------------------------

## 8. World

> **T03b (2026-11-13, user: "triple the radius ... the whole world"):** the island is now 34 x 34 km (summit 3077 m,
> crater r 1250 m), grid 513 x **80 m** (41 km square, 1280 m chunks; the loader caps grids at 513), base on the west
> cape (runway centre (-13850,-2200), hdg 270), island centre (1200,1800), air spawns 26 km out at 2000 m, area warn
> 75 km / turn 90 km. Numbers in 8.1-8.7 and 9.1 below that say otherwise are superseded; see HANDOFF T03b.

### 8.1 The island (A6 designs it in `island/island.txt`, T03's compiler builds it)
- Height field 32 m grid, 513 x 513 samples = 16.4 km square, centred on the origin. Island about 12 x 10 km.
- Volcano: cone `h0*(1 - r/R0)^1.6` with summit ~1750 m (5700 ft), crater rim radius 450 m and bowl 220 m deep,
  ridged-noise lava channels running down, 2-3 small satellite cones, coastline wobble from fBm over the angle,
  beaches (< 4 m), shallow shelf (-4 .. -60 m) for turquoise water, sea floor clamped at -60 m.
- **Airbase** on the coast: flat plateau at 12 m (smoothstep blend 300 m), runway 2800 x 45 m with markings, a
  parallel taxiway, an apron 300 x 150 m, 4 hangars (40 x 30 x 14 m), a tower (8 x 8 x 25 m), 3 fuel tanks, a radar
  dome. Takeoff direction points out to sea; nothing taller than 150 m within 5 km of the climb-out path.
- Material classes per quad from height, slope and noise: `sand`, `grass` (green slopes < 25 deg, up to 600 m),
  `rock` (slope > 30 deg), `ash` (> 700 m), `lava` (channels + crater floor), plus `runway`/`apron` on the base.
  Vertex colour = class tint x baked light (sun + sky ambient + slope AO + height-field shadow march towards the sun
  of A2's light set). Climate/look (user 2026-11-13): **green** island for now: bright day, green vegetated slopes, rock/ash near the crater.

### 8.2 Terrain rendering (`e_terrain_draw.cpp`, ENGINE.md 2.2)
- Chunks 512 m (16 x 16 quads at LOD0); only chunks with land or shallows (~300-450). Five LODs per chunk:
  32/64/128/256/512 m grids (512/128/32/8/2 tris), each LOD with its **own contiguous vertex block** (r_draw_vb
  transforms the touched range).
- LOD by 3D distance from the eye to the chunk AABB: `d_k = lodScale * 28.6 * edge_k` (16 px triangles at 640 px
  wide, 70 deg): 0.9 / 1.8 / 3.7 / 7.3 km; LOD4 up to the far plane. 10 % hysteresis. `lodScale` 1.0 Low, 0.5 High.
- Cracks: LOD-k border vertices are snapped to the LOD-(k+1) edge interpolation (compiler), skirts (depth
  `max(4 m, 0.15 x grid)`) on LOD >= 1.
- Per-class batches, sorted by material across visible chunks (<= 6 texture changes for terrain).
- Frustum cull per chunk AABB; chunks beyond the far plane skipped.

### 8.3 Sea (`e_terrain_draw.cpp`)
- Dynamic ring mesh every frame (`r_draw_mem`, ~150 tris), centred under the camera (radii 0.4, 1.5, 4, 9, 18 km and
  far), y = 0, slightly beyond the far plane (fully fogged there).
- One 256x256 tileable sea texture (A3) with baked swell; UV = (world XZ - camera XZ snapped to the period) / period
  + time scroll. Vertex colour darkens with distance.
- Shallows: inside 1.5 km of the coast the sea ring is blended (alpha `seaShallowAlpha` 0.65) over the turquoise
  terrain shelf; opaque elsewhere. Foam strip along the 0 m contour (compiler, additive batch in chunk LODs 0-2).

### 8.4 Sky, sun, clouds
- Panorama dome via `world_sky_load/world_draw_sky/world_set_sky_late(1)` (A2: open sea, afternoon, sun at azimuth
  ~230 deg, elevation ~40 deg; elevations -10..+40; bottom rows = fog colour; light set file).
- Sun glow billboard (`world_set_sun_glow`) on High.
- Cumulus puffs: camera-facing billboards from a 2x2 atlas (A2), placed deterministically 1.8-4 km high over the area;
  sorted far to near; fade out within 300 m of the camera (no fill blow-up when flying through) and beyond the far
  plane. Count by preset (9.3). Cloud deck at 6 km: High only.

### 8.5 Base objects
`data/island/base.trk` (RTRK container written by islandc with assetkit `write_trk`): runway/taxiway/apron ribbons
(0.2 m above the flattened terrain + `zBias 1`), hangars/tower/tanks as INST meshes (A7; islandc writes box
placeholders until A7 lands). Drawn via e_world with `viewDist` 6 km. Collision boxes are in `BASE` (8.6).

### 8.6 Island file `data/island/jet.isl` (container tag `RISL`, e_file chunk API, little-endian, 4-byte aligned)

| Chunk | Content |
|---|---|
| `INFO` | u32 version 1; u32 gridN (513); f32 cell (32); f32 x0, z0 (south-west corner); f32 hMin (-200), hScale (0.05 m/unit); u32 chunkCells (16), nChunkX, nChunkZ (32, 32); u32 nLods (5); f32 seaLevel (0); f32 lodEdge[5]; u32 nMaterials; f32 islandCentre x, z, radius |
| `HGHT` | u16 h[gridN*gridN], row = z index (south to north), column = x; height = hMin + h*hScale |
| `HMAX` | u16 max height per 8x8-cell block (64 x 64), same encoding: coarse LOS march + culling |
| `CLAS` | u8 class per quad (512 x 512): impacts, minimap |
| `MATL` | per material: char tex[32]; u32 flags (blend/add/alphatest, clampUV); u32 tint ARGB |
| `CHNK` | per stored chunk: u16 cx, cz; f32 aabb[6]; per LOD: u32 vFirst, vCount, bFirst, bCount |
| `BTCH` | per batch: u16 material, u16 pad, u32 iFirst, iCount (indices relative to the chunk-LOD's vFirst) |
| `VERT` | RVert1 (x y z, colour ARGB baked, u v): **UVs chunk-local** (u = (x - chunkX0)/period; chunk size is a multiple of every period, so tiling is seamless and UVs stay < 16) |
| `INDX` | u16 indices |
| `BASE` | runway: centre x z, heading deg, length, width, height; threshold spawn x y z heading; 4 air spawns x y z heading; u32 nBoxes + AABB[n] (hangars, tower, tanks) |
| `MAPI` | minimap texture name char[32] + world rect x0 z0 x1 z1 |

Portable API (`e_terrain.h`, T04):
```c
int   terrain_load(Terrain *t, const char *path);          /* no render data needed (headless, simh) */
float terrain_height(const Terrain *t, float x, float z);  /* bilinear; outside grid = -60 */
float terrain_surface(const Terrain *t, float x, float z); /* max(height, seaLevel) */
int   terrain_class(const Terrain *t, float x, float z);
int   terrain_los(const Terrain *t, const Vec3 *a, const Vec3 *b); /* 1 = clear; HMAX blocks first, then 32 m steps only where needed */
int   terrain_segment_hit(const Terrain *t, const Vec3 *a, const Vec3 *b, Vec3 *hit); /* missiles */
const TerrainBase *terrain_base(const Terrain *t);
/* render side (e_terrain_draw.cpp): terrain_gpu_init, terrain_draw(t, view, lodScale, farDist), sea_draw(view, time, ...) */
```
`terrain_los` target cost: <= 30 us for a 30 km ray on the VM (HMAX march at 256 m, refine only blocks whose max is
above the ray).

### 8.7 Minimap image
islandc writes `data/tex/map_island.bmp` + `_a.bmp` (128x128, top view: land shaded by height, coast line, runway in
white, transparent sea). Rotated heading-up at runtime as a textured quad.

### 8.8 Asset formats and budgets (art tasks)
All from the racer pipeline: `.msh` RMSH v1 (assetkit `export.py`, max 4 LODs, 24 tags, 4096 verts per LOD for
lit drawing), 24-bit BMP, RGB(255,0,255) colour key, `_a.bmp` alpha companion, power-of-two, max 256x256, names
`[a-z0-9_]` <= 31 chars; WAV mono 22050 Hz 16-bit; deterministic Python 3 + Pillow (no numpy), `--check` md5.

| Class (validate.py) | Budget |
|---|---|
| `jet` | LOD0 600-950 tris, LOD1 220-380, LOD2 70-140; tags `cam_cockpit cam_chase nozzle hinge_stab_l hinge_stab_r hinge_flap_l hinge_flap_r rail_0..rail_7 flare_l flare_r` |
| `jetpart` (stabilators, flaperons, nozzle, gear) | 4-60 tris each, origin at the hinge, hinge axis = local X |
| `missile` | LOD0 30-120 tris (on rails and in flight), LOD1 <= 20 |
| `bldg` | 12-300 tris |
| `fx` atlases | 128x128 or 256x256 4444/keyed |
| Textures total | <= 6 MB on Low (VRAM cliff at ~9 MB) |

---------------------------------------------------------------------------------------------------------------

## 9. Rendering

### 9.1 View distance, depth and fog (user decision, 2026-11-13)
**User:** "Humans cannot see that far anyways... at that distance we must rely on HUD." So:
- **Aircraft are drawn only near:** jet LOD0 < 300 m, LOD1 < 900 m, LOD2 < `acMeshDist` 2.5 km (a 15 m jet is ~2.7 px
  long there at 640 px), then a 2x2 px dark dot sprite fading out by `acDotDist` 4 km, beyond that **nothing**: the
  HUD symbols are the enemy. Missiles: mesh < 600 m, trails within 6 km.
- **Far plane only as far as terrain and horizon need:** Low `farM` 18 km with linear W fog 3.6-18 km; High 28 km,
  fog 5.6-28 km. Fog colour = sky horizon colour, so the sea and island fade into the panorama; nothing beyond the
  far plane needs geometry. (The area is 30 km in radius: from far out the island is a minimap shape only.)
- **Depth plan:** Plan A = **one pass, W-buffer** (racer default `wantWBuffer=1`), near 1.0 m cockpit / 1.5 m chase,
  far = `farM`. The shorter far plane helps where it matters: if the Voodoo3 W is a linear 16-bit value, 18 km /
  65536 = 0.27 m everywhere (even 28 km = 0.43 m); fog table resolution also improves. (For a plain 16-bit z/w
  buffer the near plane dominates and far barely matters, so we don't rely on Z.)
- The racer's two-depth-range split (Plan B: far pass 0.8-18 km, `r_clear_z`, near pass 1.5 m-2.5 km) is **kept
  only as a fallback** if the probe shows W z-fighting at the coastline or under the runway. Expectation: not needed.
- Coplanar care regardless: islandc keeps the terrain at least 4 m under the sea plane from 20 m off the coast line;
  runway ribbons 0.2 m up + `zBias`.

### 9.1b Probe decision (T02, 2026-11-13; `probe/RESULTS.md`, binding)
- Depth: **Plan A**, one pass, **W-buffer**; near 1.0 m cockpit / 1.5 m chase; far 18 km Low / 28 km High. Keep
  separate surfaces >= 2 m apart (or zBias); 0.5 m gaps fail from 5 km.
- Fog: linear W table fog, **fog end = 0.9 x far** (3.6-16.2 km Low, 5.6-25.2 km High): the Voodoo table reaches only
  ~93-96 % at fog end, so ending it at the far plane leaves a line where sea meets the sky's fog-coloured bottom.
- Runway ribbons 0.2 m above ground need **zBias 2** (not 1).
- **UV rule**: keep every vertex |uv| below ~16-32 (terrain UVs chunk-local, sea UVs camera-relative); larger UVs make
  the emulated Voodoo pick coarse mips (blur from ~32, flat colour by 2048).
- Aircraft: mesh to 2.5 km, plus a 2x2 dark dot from **1.5 km** (under the mesh, alpha ~0.7) to 4 km, so tail-on jets
  (0 px from 2.5 km) never vanish.
- HUD: **RB_PREMUL** blend (ADD rejected: turns white over haze/cloud); strokes 2 px at 600 lines; shadow black
  (+1,+1) 50-70 %, baked into the atlas (A4c) so a glyph is one quad. HUD quads cost ~7-8 us each on the VM: draw lines
  textured from the atlas (one texture per page, one batch) to stay within 1.5 ms.
- Low = **800x600x16** on the VM; test frame (5.4k tris, 63 draws) 14.8 ms CPU-bound: 30 fps realistic, 15 fps gate
  has ~2x margin. Texture memory left at 800x600: 11.3 MB.
- Loader bug to avoid: `tex_get(name, IMG_ALPHA)` breaks premultiplied atlases; load colour + `_a` with flags 0 and
  build the alpha from `_a`'s red channel (`atlas_load` in `probe/probe.cpp`).

### 9.1c View distance after M1 round 1 (T10b, 2026-11-13; supersedes 9.1/9.1b/9.3 distances)
- Low far 38 km (fog 7.6-34.2 km), High far 56 km (fog 11.2-50.4 km) at sea level; far and fog grow with camera
  altitude (x h/(H(1-exp(-h/H))), H = `hazeScaleM` 8000 m), capped by `farMaxM` 62 km. **Voodoo3 limit:** fog/depth
  wrap beyond ~65.5 km, so far is clamped to 64 km in code (`SCENE_FAR_LIMIT`); raise on the T40 only if T23 shows
  the Radeon has no such limit.
- Sky is drawn **first** without Z (`[scene] skyFirst 1`); the old late sky dome painted fog over far ground (the
  user's "ball" from altitude). Sea fog band, ring edge, shading and culling all work by view depth / far corners.
- Bench routes: low, volcano (+-12 km at 3600 m), high (12 000 m, +-25 km), clouds (3000 m, 20 km square).
- Speed cues (render only, `[fx] speedCues`): chase FOV +10 deg 350-1050 kt, buffet, air streaks, wind sound.

### 9.2 The M0 probe (T02, `probe/`, built on the copied engine; results in `probe/RESULTS.md`)
Run at the **chosen** far planes (18 and 28 km) with W-buffer, and with Z for comparison; screenshots + numbers:
1. Coastline: a 1:8 terrain slope through the sea plane at 1, 3, 6, 12, 17 km: any z-fight shimmer while the camera
   moves (two shots 1/30 s apart, diff count of flipped pixels).
2. Parallel quads 0.5 / 2 / 5 m apart at 100 m, 1, 5, 10, 17 km.
3. Runway decal 0.2 m above flat ground at 0.5, 2, 4 km with zBias 0/1/2.
4. Fog: linear W table fog 3.6-18 km and 5.6-28 km; the sea's far edge vs the sky bottom colour (no seam), fog at
   the far plane edge = 100 %.
5. UV magnitudes 16 / 256 / 2048 on a tiled quad (swimming or blockiness).
6. Aircraft visibility: a 15 m box-jet at 1, 2.5, 4, 6 km: pixel count; the 2x2 dot sprite at 2.5-4 km.
7. HUD lines: `ui_line` quads 1/2/3 px wide, additive vs blend, with and without a 1 px dark shadow, over sky, sea,
   cloud-white and terrain; stroke font 7 and 9 px tall: readability shots (for T07).
8. Costs: `r_clear_z` ms (for Plan B), 500-quad ui_line batch ms, fog on/off fill ms.
**Decision line in RESULTS.md:** Plan A or B, near/far values, fog values, HUD stroke width + shadow choice.

### 9.3 Presets and budgets (`[video] preset`; each key overridable)

| Item | Low (VM, **800x600x16**, >= 15 fps required, 20-30 nice; 640x480 fallback) | High (**IBM ThinkPad T40**, Pentium M + Radeon, **1400x1050** native, 60 fps) |
|---|---|---|
| far plane / fog | 18 km / 3.6-18 km | 28 km / 5.6-28 km |
| terrain `lodScale` (larger = finer; T04) | 0.85 (T09 tunes) | 1.5 |
| terrain tris | typ. 1.5-3k, max 4k | max 12k |
| sea | 150 tris | 300 tris, extra ring |
| sky | 4 tiles 256x128 | 8 tiles |
| clouds | 16 puffs, no deck | 48 puffs + deck |
| aircraft mesh / dot range | 2.5 km / 4 km | same (it is a resolution/eye limit, not a budget) |
| max LOD0 aircraft at once | 2 | 6 |
| live missiles (cap) / trail points | 10 / 16 per missile | 24 / 32 |
| explosions (quads each) | 4 (<= 20) | 8 (30) |
| HUD + minimap | <= 500 tris, 4 draws | same |
| **total tris / draws** | **<= 7k / <= 90** | <= 20k / <= 200 |
| textures resident | <= 6 MB | <= 12 MB |
| CPU per frame (VM) | sim <= 5 ms (4 ticks), scene <= 15 ms, HUD <= 1.5 ms, DLZ <= 1.5 ms, audio <= 0.5 ms | |

Low gate (T09; user 2026-11-13: "I'm okay with 15 fps on Voodoo"): 800x600, a steady display-locked rate of at least
15 fps (8 blanks of 120 Hz; prefer 6 or 4 blanks if it holds), <= 2 % missed slots on all bench routes. Don't cut the look
only to reach 30 fps on the VM; the sim stays a fixed 120 Hz tick at any frame rate. High is checked only in T23.

### 9.4 Frame order (scene.cpp)
`r_begin_frame(fog | R_ZONLY)` -> terrain (opaque, by class) -> base cells/instances (e_world) -> aircraft
(`mesh_draw_lit`, own jet last in chase view) -> sea opaque ring -> sea shallow blended ring + foam -> late sky ->
clouds (sorted, blended, fogged) -> effects (trails blended, flame/explosions additive, fog off with manual fade) ->
dots (2x2 sprites) -> HUD (10) -> minimap -> overlays (F10/F11) -> `r_end_frame` -> sound -> `r_present`.

### 9.5 Aircraft and effects
- Jet: `mesh_draw_lit` with sun in body space; parts (2 stabilators, 2 flaperons, nozzle, gear) drawn with
  `world = rot(hinge axis, angle) * tag * body` (racer wheel pattern). Afterburner: 12-16 tri flame cone, `RB_ADD`,
  no Z write, fog off, length by `ab`, UV flicker; glow billboard at `nozzle`.
- Missile trails: camera-facing ribbon from a ring buffer (racer skid-mark pattern), width 0.3 -> 3 m, fading.
- Explosions: 6-12 billboards (fire + smoke atlas) over 1.5 s + debris points + a short screen flash for near ones.
- Flares: additive sprite + short smoke trail. Damaged jet: smoke puffs from the nozzle.

---------------------------------------------------------------------------------------------------------------

## 10. HUD (`hud.*` T07, `hud_tgt.*` T15, `hud_wpn.*` T16) - top priority with flight feel

**User HUD decisions (2026-11-13, override anything below that conflicts):**
- **Resolution:** minimum game resolution **800x600** (640x480 only a fallback); real target the **ThinkPad T40 at
  1400x1050**. The HUD must be sharp at every height: A4 provides glyph/symbol atlas sets per scale (480, 600, 768,
  1050 lines; picked at startup by screen height, nearest set, drawn at whole-pixel positions without bilinear
  up-scaling where possible); stroke widths scale with the set (2 px at 600, 3-4 px at 1050).
- **Speed and altitude boxes:** the numbers are **centred horizontally and vertically** inside their boxes (box size
  fixed for the max digit count, so the box never resizes; monospaced digits).
- **Kill feedback:** a destroyed enemy's 3D-view diamond turns **grey** for a few seconds (`killDiamondSec` ~3) with
  SPLASH; on the minimap it is **crossed out (X) for 15 s** (`killMapSec`).
- **ASE / launch-zone circle never touches the speed and altitude boxes** (clear gap, >= 6 px at 600 lines, scaled;
  radius/centre are layout constants owned by T07, used by T16).
- **HAFU symbols are for the minimap only.** In the 3D view (both cockpit and chase) a contact is a **small hollow
  diamond** (square rotated 45 deg, corner down), outline only: **red** for hostile, yellow while still unknown,
  dashed in memory. The TD box (designated) is a square around that diamond (no shape change on ID).

Reference: `docs/research/HUD.md` (F/A-18C, verified against the DCS EA guide). Everything is screen-space quads
(`ui_line` strokes + one stroke-font/symbol atlas from A4) in the 640x480 virtual space (`ui_begin(640,480)`, scaled
by height/480 at higher resolutions). No GDI per frame, no D3D lines.

### 10.1 Look and readability rules
- HUD green `hud.colour` 0xFF40FF60, strokes `hud.stroke` 2 px with a 1 px dark shadow offset (+1,+1) at 50 %
  alpha; blend mode additive over a shadow pass (final choice from the probe, 9.2 item 7). `hud.bright` 0..1.
- HAFU colours: unknown yellow 0xFFFFE040, hostile red 0xFFFF3030, both with the same dark shadow. Warnings that
  are game additions (MISSILE, threat arrows, LOCK) in red; everything else HUD green.
- Fonts: stroke font (A4) in two sizes, **small 9 px** and **large 13 px** cap height at 480 lines; digits monospaced
  (numbers don't wobble when they change). GDI fallback (`font_create("Courier New", ...)`) until A4 lands.
- Conformal items use the same view/projection as the 3D scene and the same interpolated state, so they sit
  exactly on the world.

### 10.2 Screen layout, cockpit view (640x480 virtual, centre (320,240) = boresight = waterline)
HUD frame (clip rectangle for conformal items): x 170..470, y 116..384.

| # | Element | Position / size | Behaviour (HUD.md ref) |
|---|---|---|---|
| 1 | Heading tape | centre (320,132), 240 px wide (8 px/deg, 30 deg window), ticks 5/10 deg, 3-digit labels every 10 deg, caret below centre | 1, moving scale |
| 2 | Airspeed box | x 206..254, y 220..236; right-aligned kt | 1 |
| 3 | Altitude box | x 386..444, y 220..236; ft MSL | 1 |
| 4 | Vertical velocity | right-aligned at (444,206), ft/min, rounded to 10 | 1 |
| 5 | AOA / Mach / G / peak G | left-aligned x 196, y 262 / 276 / 290 / 304 ("α 2.6", "M 0.84", "G 1.0", peak "9.2") | peak shown once > 4 g, cleared after 5 s below 2 g |
| 6 | Waterline symbol (W) | fixed at (320,240), 28 px wide | |
| 7 | Velocity vector | conformal; circle r 5 + wing ticks 7 + top tick 4; clamped to the frame and flashing when limited | 1 |
| 8 | Pitch ladder | conformal, rung every 5 deg, horizon line +-110 px with 24 px centre gap; rungs 2 x 40 px, gap 24 px; positive solid with 5 px hooks towards the horizon, negative dashed with hooks up; numbers at both ends; rotated by bank; centred laterally on the VV; clipped to the frame | 1 |
| 9 | Bank scale | arc centre (320,240), r 112 (bottom at y 352); ticks at 5/15/30/45 deg each side; pointer triangle inside the arc, pinned at 47 deg | 1 |
| 10 | Weapon line | centre (320,368): "AIM-120 16" / "AIM-9 40" (names in `[weapons]`) | M2 |
| 11 | Messages | large font centre (320,168), 2-3 s: "A: TAKEOFF", "YOU HAVE CONTROL", "INI RELOADED (3)", "WAVE 2" | |
| 12 | Warnings | "PULL UP" large at (320,300) flashing 4 Hz; "RETURN TO AREA" (320,186) | 6.3, 6.4 |
| 13 | Clock | (178,372), mission time mm:ss, sortie only | |
Weapon/target elements: 10.5, 10.6. Minimap: 10.4 (all views).

### 10.3 Chase view (reduced HUD, HUD.md section 3)
| Element | Position | Notes |
|---|---|---|
| Aim cross | projection of the point 1 km along the nose; 14 px cross | the nose reference in third person |
| Velocity vector | conformal | |
| Horizon + 10/-10 deg rungs | conformal, 50 % alpha, centred on the VV | attitude cue |
| Airspeed / altitude boxes | x 150..198 / 442..500, y 220..236 | further apart than in the cockpit, the jet sits in the middle |
| G | "G 4.5" under the speed box (150,252); Mach optional (`hud.chaseMach`) | G matters: it is our turn limit |
| Heading | small tape +-60 px at (320,24), optional `hud.chaseHeading=1` | |
| Weapon block | bottom-right (560, 410..450): weapon name, count large, "FLR 40", SRM lock state | AC-style corner |
| Target symbology | identical to the cockpit (10.5) | most important element in chase |
| Launch zone | compact vertical DLZ bar (50 px) right of the TD readout instead of the circle (HUD.md 2.2) | 10.6 |
| Messages, warnings, threat arrows | as cockpit | |

### 10.4 Minimap (`hud_map.*`, both views)
- Bottom-left square 136 x 136 at (6,338), dark translucent background (0x60000000), green 1 px frame, range text
  "20" top-left, flare count "F40" top-right.
- Heading-up, own jet symbol at the centre; range 10 / 20 / 40 NM (Tab) = half-width of the square.
- Island image (8.7) rotated with the heading; runway mark; area boundary circle (30 km) when in range.
- Radar scan wedge +-60 deg, 15 % alpha. Contacts (T15): HAFU mini-symbols 7 px + 6 px heading tick; designated with
  a green square; MEMORY dim/dashed; own missiles green dots, hostile missiles red blinking dots.
- What it shows: all enemies always for now (user 2026-11-13: `hud.mapShowAll=1` default; 0 = own radar contacts + memory only). Enemies that lock the player
  show as a red tick on the map frame at their bearing (RWR).

### 10.5 Target symbology (T15; user: must be clear, readable, stable)
| State (11.2) | 3D view marker (centred on the contact's projected position) | Minimap |
|---|---|---|
| DETECTED (unknown) | **yellow hollow diamond** (outline, corner down), ~12 px at 600 lines (user 2026-11-13; HAFU only on the map) | yellow mini arch |
| HOSTILE | **red hollow diamond** (outline, corner down), same size | red mini chevron |
| MEMORY | same shape, dashed, 50 % alpha, at the extrapolated position | dashed |
| SELECTED (S tap) | + green **triangle** (9 px, pointing down) 6 px above the symbol | - |
| DESIGNATED / locked (S hold) | + green **TD box**: square around the diamond (same shape unknown or hostile); dashed while lock is in memory; flashes 3 Hz when clamped to the HUD frame (cockpit) or screen edge (chase) | green square around |
Labels: small font centred under the dot: range in NM ("12.4" below 10 NM, "17" above; feet x100 below 1 NM:
"8"); for the designated contact also its altitude in thousands of ft ("12.4  17"). **Declutter**: labels only for
the designated, the selected and the 2 nearest other contacts; symbols for everything detected in view. Hidden
within 400 m except the TD box.
- **Locator line** (designated contact outside the HUD frame / screen): from the waterline (cockpit) or aim cross
  (chase), 60 px long towards the target (screen direction of the shortest rotation; targets behind use their
  projected direction), arrow head, degrees off the nose ("135") at the tip.
- **Off-screen arrows** for other contacts within 40 NM (`tgtArrowNm`, T21a/orchestrator 2026-11-14): 10 px triangles 20 px inside the screen edge pointing
  towards them, HAFU colour, max 6.
- Kill: "SPLASH" in large font at the target's last position for 1.5 s; symbol fades.

### 10.6 Weapon symbology (T16, T21c LAR)
- **Launch-zone circle** (cockpit, LRM or SRM selected with a TD): centred on the waterline, radius 66 px. T21c: the
  ring is the Hornet **normalized NIRD** (user LAR GDD): **Rmax fixed at 6:00**, **Rmin at 2:30**, **Rne** positioned
  between them, and the **range caret** sweeps counter-clockwise through the fixed in-LAR arc (GDD 2.3; out of range
  uses the left semicircle, pinned at 12:00 beyond 2x Rmax). Ticks: **Rmax** outward 8 px, **Rne** outward double tick
  6 px, **Rmin** inward 6 px, **Raero** (max aerodynamic range) thin outward tick. **Range pointer** = triangle on the
  ring pointing inward at the current range. Target aspect pointer: short outward line at the target's relative heading
  (`hud.aspectPointer=1`).
- **Steering dot** (4 px): offset from the circle centre = angle between the nose and the lead direction (towards
  the predicted intercept point from the solver). T21c: the edge is **dynamic** (`aseDegLrmMin` -> `aseDegLrm`, GDD 2.4)
  - tight at Rmax for loft steering, widening inside Rne. Aid only; the solver already accounts for off-boresight launch.
- **IN LAR** (small font, below the circle) while Rmin < R <= Rmax; **SHOOT** (large font) 14 px above the TD: steady
  when Rmin < R <= Rmax, **flashing 3 Hz when R <= Rne**; no SHOOT until the solver's first result for this target is
  ready (<= 0.25 s). **Breakaway X** (60 px) when R < Rmin.
- **Readouts** right of the circle, small font, x 396: "960 Vc" (y 290), "19.9 RNG" (y 304), third line: before
  launch "23 TOF" (solver's time of flight), after an LRM launch "17ACT" counting down, then "11TTG" once active
  (most recent live missile at this target), SRM "6TTG".
- **Chase DLZ bar**: 6 x 50 px vertical bar right of the TD readout: Rmin/Rne/Rmax ticks, caret for the range,
  same SHOOT rules.
- **SRM seeker circle**: 3 deg radius (26 px cockpit) at the boresight when no TD; slaved onto the TD when the TD is
  within the seeker gimbal and lock range; growl tone while searching with a target near, **high lock tone** + steady
  circle when locked; flashes when outside the HUD frame. "VISUAL"/maddog LRM: dashed 20 deg circle + "VISUAL" at the
  weapon line when LRM is selected with no TD.
- **Threat warnings** (game additions, red): "MISSILE" large at (320,186) flashing while any hostile missile is
  homing on the player; up to 3 **threat arrows** on a 140 px ring around the screen centre pointing at the missiles
  (nearest = largest); "LOCK" small at (320,200) + RWR tone while an enemy has the player locked.

### 10.7 Stability rules (binding for T07, T15, T16)
1. Positions come from interpolated render state and the 3D view/projection; no symbol is placed from 10 Hz data.
2. Tracked contacts use the true sim position (the radar knows it while tracking); MEMORY extrapolates linearly from
   the last track; the radar tick only changes *states*.
3. Numbers update at fixed rates (speed/alt/heading 10 Hz; Mach/G/AOA/VVI 5 Hz; range/Vc 5 Hz; ACT/TTG 2 Hz) and a
   value change needs 2 consecutive updates (no 1-digit flicker). Monospaced digits.
4. State changes (unknown -> hostile, select, designate, memory) latch for at least 0.3 s; MEMORY shows only after
   0.5 s of lost track.
5. Symbol centres rounded to whole pixels; text snapped to pixels; strokes sub-pixel.
6. Flash rates fixed (3 Hz SHOOT/TD clamped, 4 Hz warnings), phase from sim time (identical in replays).
7. Overlap: when two symbol centres are < 12 px apart, the second's label moves to its right; the designated contact
   draws last (on top).
8. Text strings are built only when the value changes (cached quads); HUD + minimap <= 1.5 ms on the VM.

### 10.8 HUD tests (`-hudtest NAME`, T07/T15/T16)
Scenes with scripted flight state over a static scene: `ladder` (pitch -90..+90, bank 0..360), `numbers` (speed,
altitude, VVI, G sweeps incl. 9.9 and 4-5 digit altitudes), `contacts` (synthetic contacts: 0.5-40 NM, behind, at
screen edges, overlapping pairs, state changes), `zone` (synthetic DLZ values and launches). Each logs `hud:` lines
(displayed strings, symbol pixel positions) and checks: text change rate <= the configured rate, a world-static
contact moves smoothly (frame-to-frame deviation from the expected screen motion < 1 px), no state flicker, nothing
drawn outside the frame. Screenshots at fixed times for the orchestrator/user review.

---------------------------------------------------------------------------------------------------------------

## 11. Radar, contacts, identification, targeting (`radar.*`, `target.*`)

All numbers are game numbers in `[radar]`. Every aircraft (player and enemies) has the same radar.

### 11.1 Detection (10 Hz, contacts staggered)
- Scan volume: body frame cone `|az| <= 60 deg`, `|el| <= 45 deg` (cheap test: b.z > 0, |b.x| < 1.732 b.z,
  |b.y| < 0.707 R).
- Range: `Rdet = detRangeNm (20 NM; the player: playerDetRangeNm 40 NM, ID playerIdRangeNm 25 NM, T21a) x (1 + 0.25
  sin^2(aspect))`; deterministic (no probability).
- Terrain masking: `terrain_los(own, contact)`; blocked = not detected. (Hiding behind the volcano works both ways.)
- No notching, no look-down penalty (cut, 1.2).

### 11.2 Contact state machine (per observer per aircraft)
| State | Enter | Leave |
|---|---|---|
| NONE | - | detected |
| DETECTED (unknown, yellow) | in volume, range, LOS | ID done -> HOSTILE; not seen 0.5 s -> MEMORY |
| HOSTILE (red) | 1.5 s of continuous track within `idRangeNm` 15 NM, or instantly within 5 NM | not seen 0.5 s -> MEMORY (stays red) |
| MEMORY (dashed) | lost track | seen again -> previous state; 6 s -> NONE |
| dead | target destroyed | removed after the SPLASH display |
SELECTED and DESIGNATED are flags on top of these states (11.4).

### 11.3 Lock (STT) for the designated contact
Lock gimbal +-70 deg, range <= 1.2 x Rdet, LOS required. Outside for > 1 s: lock goes to memory (TD dashed, missile
datalink stops: missiles go inertial) for 3 s, then the designation drops. Lock events feed the target's RWR.

### 11.4 Targeting input (S short/long, 5.3)
- **S short**: no selection -> select the contact nearest to the nose (angle) among DETECTED/HOSTILE/MEMORY within 40
  NM. Otherwise the next in order of increasing angle off the nose; the order list is frozen during a cycle (presses
  less than 2 s apart), so repeated taps walk through every contact.
- **S long**: designate the selected contact (if none: select the nearest to the nose and designate); lock tone. S
  long on the already designated contact = break lock.
- Designated contact destroyed: designation clears, selection moves to the next contact nearest the nose
  (`autoReselect=1`; `autoRedesignate=0` makes it re-lock automatically, Ace Combat style).
- Firing never requires HOSTILE (no ROE); ID is information and gates nothing.

### 11.5 RWR (player side)
- Enemy lock on player -> "LOCK" + steady tone + map tick (10.4, 10.6).
- Enemy missile launched at the player: "MISSILE" warning at launch for LRM (arcade), for SRM when within 5 km or
  at launch inside 5 km; threat arrows track each homing missile.
- **RWR tones (user, 2026-11-13; A9b):** new enemy fighter radar = "waterfall" 550 -> 520 -> 490 -> 470 Hz, 83 ms per
  step (0.335 s, once, `rwr_search`); enemy radar lock (STT) = 455/555 Hz alternating 0.25 s each, continuous
  (`rwr_lock`); missile launch / inbound = 455/555 Hz alternating 0.10 s each, continuous (`rwr_launch` onset +
  `rwr_missile` loop). Trigger rules as above (LRM at launch, SRM within 5 km); user confirmed 2026-11-13: warning at launch, not at
  seeker activation.

---------------------------------------------------------------------------------------------------------------

## 12. Weapons (`missile.*`, `weapons.*`, `dlz.*`) - game mechanics, invented numbers

Design intent: fun, readable and honest. The authentic part is the HUD look; the missiles are game objects with
tunable knobs in `[lrm]`, `[srm]`, `[weapons]`. Gameplay targets in 12.7 are what tuning aims at.

### 12.1 Loadout (Ace Combat-like counts)
LRM (radar, HUD name "AIM-120") 16; SRM (heat, "AIM-9") 40; flares 40 per life. Refilled on respawn. 8 rail tags show
missiles on the jet (visual only; they "reload" like Ace Combat). Fire cooldown 0.4 s, rails alternate.

### 12.2 Missile model (one function for live missiles and fly-outs)
```c
void missile_step(Missile *m, const MissileParams *mp, const TrackView *tgt, const Terrain *t, float dt);
/* state: pos, dir (unit), V, age, phase, guidance target (true track / last known / flare), flags */
thrust = (age > dropSec && age < dropSec + boostSec) ? boostAcc : 0
rho    = densityAlt ? exp(-pos.y / 8500) : 1
aLat   = PN: N * (omega x v), omega = (r x vr)/(r.r)   (pure PN; lead point during midcourse), |aLat| <= gMax*g*min(1,(V/gRefSpeed)^2)
V     += (thrust - dragK*rho*V*V - inducedK*|aLat| - g*dir.y) * dt
dir    = normalise(dir + aLat/V * dt) ; pos += dir*V*dt
hit    = after armSec: closest approach of this step's segment to the target < fuzeM
ground = terrain_segment_hit(prev, pos) (or y < 0 at sea)
dead   = age > maxSec or (V < minSpeed after the boost)
```
Seeker: gimbal limit (angle between dir and LOS), seeker range, terrain LOS (10 Hz staggered): blocked or outside the
gimbal for > 0.3 s = seeker lost -> fly to the last known point for 1.5 s, reacquire if possible, else unguided.
**Target behind the volcano**: either the LOS loss or `terrain_segment_hit` ends the missile; this is the one
designed way a no-escape shot misses.

### 12.3 Guidance phases
- **LRM**: LAUNCH (drop, 0.3 s straight) -> MIDCOURSE: lead towards the predicted intercept; target data from the
  shooter's lock ("datalink") while it holds, otherwise inertial (last known + extrapolation) -> ACTIVE when the
  estimated distance < `activeM`: own seeker searches +-`acqConeDeg`; acquires the target (or the nearest other
  aircraft in the cone except the shooter) -> TERMINAL: fire-and-forget; HUD ACT -> TTG. Fired with no TD (maddog):
  goes active after 1.5 s straight ahead.
- **SRM**: needs the seeker lock (tone) for a guided launch; TERMINAL from launch. Fired without lock: acquires the
  first heat target in its cone during the first 2 s, else unguided. Lock range by aspect: `lockTailM` (rear) to
  `lockHeadM` (head-on), x1.2 if the target is in afterburner.

### 12.4 Launch zone: honest numbers from our own model (`dlz.*`)
Definitions (same meaning as the Hornet's ticks, HUD.md 2.1):
- **Rmax**: largest launch range that hits a target that keeps flying straight at its current speed.
- **Rne**: largest launch range that hits for **every** escape in the set **E = {break left 180 + run, break right
  180 + run, beam left, beam right}**, flown at the target's max sustained turn (its flight-model G limit x its AI
  `gUse`) and accelerating to FAST.
- **Rmin**: smallest range that hits the straight-flying target (arming time + turn ability).
- **Raero** (T21c, user LAR GDD 4.2): the maximum aerodynamic range = **Rmax at zero target closure**, computed live as
  `Rmax + (target velocity along the LOS) x TOF`; it is the missile's absolute reach independent of the current aspect.
Fly-out: the real `missile_step` at dt = 1/30 s with the same phases (lock assumed held), launched along our current
nose from our current state, the target placed **along the current line of sight** at range R with its current
velocity (keeps aspect and off-boresight angle), simple target model (straight / turn-at-rate then straight), terrain
checked every step (so a volcano in between collapses the zone: the HUD tells the truth). Each fly-out ends at hit,
miss (opening after arming for 0.5 s), dead missile or ground.

Solver (player's designated target, display only): bisection per value with warm start (bracket [0.8, 1.25] x the
last result, 6 iterations); Rne = minimum over the 4 escapes; values cycle Rmax -> Rne(E1..E4) -> Rmin. **Time-sliced
by missile steps**, not time: `dlz_work(budget)` per frame, `dlzStepsLow` 1500 / `dlzStepsHigh` 6000 (~1.5 ms on the
VM, measure in T13). Full refresh target <= 0.7 s; published values smoothed (`v += 0.5*(new - v)`); new target:
nothing shown until the first Rmax (<= 0.25 s). Also yields TOF and the lead direction for the steering dot.

AI tables (12.4.2): at mission load, per missile type, the same solver synchronously: Rmax and Rne over aspect {0,
45, 90, 135, 180} x shooter speed {300, 500, 800 kt} x altitude difference {-3000, 0, +3000 m}, target at NORMAL
speed. Logged (`dlz: table ms=...`, VM target < 1 s). AI fire decisions use the tables; the player's HUD uses them for
the first frames of a new target.

### 12.5 Flares
F: burst of 2 flares (stock 40), hold repeats every 0.5 s. Flare: aircraft velocity - 20 m/s backwards + spread,
gravity 0.25 g, burns 3 s. Decoy roll at each burst for every hostile missile in TERMINAL homing on the dispenser
within `decoyM` 3 km with the flare inside its seeker gimbal: `p = pFlareSrm 0.55` or `pFlareLrm 0.30` (arcade:
flares work on both seekers; no chaff), +0.15 from the tail aspect, cap 0.9; one roll per missile per 1.5 s.
Success: the missile homes on the flare. AI flares use the same model x `aiFlareFactor` (0.25), which keeps the
promise "a shot inside Rne almost always hits" (target >= 90 % vs Normal AI, 12.7).

### 12.6 Firing rules (Space)
LRM: with TD -> guided launch (lock needed at launch); without TD -> maddog. SRM: with seeker lock -> guided; without
-> unguided search. Empty: "NO MSL" click. While the TD is in memory: LRM launches inertial (ACT still counts).

### 12.7 Starting values and gameplay targets (T12 tunes, T13 verifies; all `[lrm]`/`[srm]`)

| Key | LRM | SRM |
|---|---|---|
| `dropSec`, `boostSec`, `boostAcc` | 0.15, 3.0, 200 m/s^2 | 0.10, 2.0, 250 m/s^2 |
| `dragK` (x rho) | 1.1e-4 /m | 2.5e-4 /m |
| `inducedK`, `gMax`, `gRefSpeed` | 0.05, 30 g, 500 m/s | 0.05, 40 g, 450 m/s |
| `navN` | 4.0 | 4.0 |
| `gimbalDeg`, `acqConeDeg`, `activeM`, `seekM` | 50, 25, 7000, 9000 | 40, 3 (lock circle), -, - |
| `lockTailM`, `lockHeadM` | - | 8000, 5000 |
| `armSec`, `fuzeM`, `maxSec`, `minSpeed` | 0.8, 15, 45, 250 m/s | 0.4, 10, 20, 220 m/s |
| `damage` vs enemy / player | 100 / 55 | 100 / 55 |
Gameplay targets (co-altitude 3000 m, both at NORMAL): **LRM** head-on Rmax 16-22 km, tail-chase Rmax 5-8 km, Rne
6-9 km head-on, Rmin 0.6-1.2 km; **SRM** head-on Rmax 5-7 km, rear 2.5-3.5 km, Rne 1.8-3 km, Rmin 0.3-0.5 km.
Honesty targets (T13, `simh shots`): live shot at 0.97 Rmax vs a straight target hits >= 95 %; at 1.05 Rmax misses
>= 90 %; at 0.95 Rne vs each escape in E (no flares, no terrain) hits 100 %; vs the Normal AI's full defence (flares
included) inside 0.9 Rne >= 90 %.

**Player LRM `[lrm_player]` (T21a + T21b):** the player's radar missile has its own parameter set. T21a gave it ~2x the
enemy LRM's range (user M2 play-test). T21b re-tuned it to a realistic AMRAAM profile (see the user GDD
`aim_120_amraam_simulation_gdd.md`): a two-stage boost->sustain motor, transonic wave drag, quadratic induced drag, a
kinematic loft, an early active seeker and a 60 deg gimbal; the enemy `[lrm]` and all `[srm]` keep the arcade model
unchanged. Envelope (co-alt 3000 m, NORMAL): head Rmax ~40.4 km, Rne ~13.5 km, tail Rmax ~16.4 km, Rmin ~0.95 km.

---------------------------------------------------------------------------------------------------------------

## 13. Enemy AI (`ai.*`)

Enemies fly the same flight model and radar, carry the same missiles and get no cheats (no knowledge outside their
radar except the mission's "attack" order with the player's position at wave start).

### 13.1 States (think at 10 Hz staggered; steer every tick)
| State | Behaviour | Transitions |
|---|---|---|
| PATROL | waypoint loop around the island at 3-6 km, NORMAL | player detected or attack order -> INTERCEPT |
| INTERCEPT | lead pursuit, match altitude within 1500 m | R < table Rmax x `lrmFireFrac`, LRM left, nose within 20 deg -> ATTACK_BVR; R < 6 km -> MERGE |
| ATTACK_BVR | lock, fire one LRM, then CRANK (45 deg off the LOS keeping the lock) until its missile is active or 15 s | -> INTERCEPT / MERGE |
| MERGE | lead pursuit at `gUse`; SRM when its seeker locks and R < table Rne x `srmFireFrac`; overshoot -> EXTEND | |
| EXTEND | fly away at FAST for 5 s | -> INTERCEPT |
| DEFEND | missile inbound (RWR/MAW as 11.5) after `reactSec`: fly the best escape from **E** (the solver's set), FAST, flares with `flareProb` when the missile < 3 km; with `terrainMask` and the volcano within 6 km, turn so the volcano blocks the LOS | missile gone or 12 s -> EXTEND |
| AVOID (override) | terrain within 4 s (PULL UP predictor), below 300 m AGL (unless masking), area bounds | until clear |

### 13.2 Steering law (`ai_steer`)
Desired direction `d` in the path frame -> bank error to put `d` into the lift plane -> `ur = clamp(kRoll x
bankErr)`; `up = clamp(kPull x angle(d, forward) x cos(bankErr))`, load factor limited to `gUse x nLim`. AI axes
obey the same ramp rates as the keyboard (5.2): fair, and the AI looks like a pilot.

### 13.3 Difficulty (`[ai_easy] [ai_normal] [ai_hard]`; underscores: dotted names broke params snapshots, T14)
| Knob | Easy | Normal | Hard |
|---|---|---|---|
| `reactSec` | 1.2 | 0.7 | 0.35 |
| `gUse` (of G limit) | 0.65 | 0.80 | 0.95 |
| `lrmFireFrac` (of table Rmax) | 0.95 | 0.75 | 0.60 |
| `srmFireFrac` (of table Rne) | 1.0 | 0.85 | 0.70 |
| `fireCooldownSec` | 14 | 9 | 6 |
| missiles per enemy (LRM + SRM) | 1 + 2 | 2 + 2 | 2 + 4 |
| `defendProb` / `flareProb` | 0.5 / 0.2 | 0.85 / 0.5 | 1.0 / 0.8 |
| `crank`, `terrainMask` | 0, 0 | 1, 0 | 1, 1 |
| sortie waves (enemies) | 2, 3, 3 | 2, 3, 4 | 3, 4, 5 |
Easy fires at long range (dodgeable), Hard waits closer (deadly). Max 5 enemies alive at once.

---------------------------------------------------------------------------------------------------------------

## 14. Damage, respawn, modes, score (`mission.*`)

### 14.1 Damage
Player HP 100: missile hit 34 (three hits; user 2026-11-13, tunable `[mission] missileDamage`, to be revisited), crash/collision 100; >= 50 damage -> smoke trail and
"DAMAGED". Enemies: one missile hit destroys. Explosion + debris + wreck falling (fx).

### 14.2 Death and respawn
Explosion, 2.5 s wreck camera, restart on the runway (6.1 takeoff; user: always from the airfield) with full stores and HP, 3 s invulnerable after handover; hostile missiles
homing on the old player self-destruct. Sortie: `lives` 3 (0 = unlimited); out of lives -> "MISSION FAILED".

### 14.3 Modes
| Mode | Milestone | Content |
|---|---|---|
| FREE | M1 | runway start, no enemies, unlimited time |
| TRAINING | M2 | drones: 3 non-shooting enemies on circuits at different altitudes/ranges, respawn 10 s after a kill, unlimited missiles; for learning the HUD and zones |
| SORTIE | M2 (1 wave), M3 (3 waves) | runway start; after handover "WAVE 1: 2 BANDITS BRG 040"; enemies spawn 45-55 km out (T21a; player AIM-120 ~2x range) at 4-6 km altitude with the attack order; next wave 8 s after a wave is cleared; all waves -> "MISSION COMPLETE" |

### 14.4 Score and records
Kill 1000, +300 if the kill was beyond 10 km (BVR), -1500 per death, time bonus `max(0, 600 - s) x 5`, accuracy
bonus `hits/fired x 2000`. Results: time, kills, fired/hits per type, deaths, score. Best score per difficulty in
`records.ini`.

### 14.5 Menus (M3)
Title (logo over an in-engine island fly-by) -> FLY (Free / Training / Sortie, difficulty) | OPTIONS (controls with
key capture + invert pitch (no joystick: user 2026-11-14); video preset/resolution/default view; sound volumes) | RECORDS | QUIT. Pause:
resume, restart on runway, options, quit to menu. Results screen. Settings saved to `jet.ini`.

---------------------------------------------------------------------------------------------------------------

## 15. Audio (`snd.*`, `snd_combat.*`; A5 sound pack v1, A9 v2)

Mono WAV 22050 Hz 16-bit from `tools/sfx/gen_sfx.py` (racer pattern: deterministic synth, whole-cycle loops).
Positional sounds: volume/pan/pitch (doppler) computed in game code; `au_set` once per frame.

| Sound | File(s) | Trigger | Pack |
|---|---|---|---|
| Turbine idle / military / afterburner loops | `eng_idle eng_mil eng_ab` | crossfade by throttle state + `ab`, pitch by V | v1 |
| Afterburner light-up | `ab_light` | FAST pressed | v1 |
| Wind | `wind` | volume/pitch by V and G | v1 |
| Speed brake rumble | `brake` | SLOW | v1 |
| Runway roll rumble, gear motor + clunk | `roll gear_up gear_clunk` | takeoff | v1 |
| Crash / explosion near, far | `expl_near expl_far` | crashes, kills | v1 |
| UI move / ok / back, message beep | `ui_move ui_ok ui_back beep` | menus, messages, F5 | v1 |
| PULL UP tone | `pullup` | 6.4 | v1 |
| Missile launch + motor loop | `msl_launch msl_motor` | own launch (2 s), others positional | v2 |
| Missile pass-by | `msl_pass` | hostile missile misses within 200 m | v2 |
| SRM growl / lock tone | `srm_growl srm_lock` | 10.6 | v2 |
| Radar lock beep | `lock_beep` | designate | v2 |
| RWR lock tone / missile warning | `rwr_lock rwr_missile` | 11.5 | v2 |
| Flare pop | `flare` | F | v2 |
| Hit thump, kill chime | `hit kill` | own damage, SPLASH | v2 |
| Enemy jet pass-by | `jet_pass` | enemy within 1.5 km, doppler | v2 |

---------------------------------------------------------------------------------------------------------------

## 16. Testing

### 16.1 `-test NAME` (exit code = failures; `simh test NAME` runs the same portable tests on the Mac)
`math`, `params` (ini round trip, F5 diff), `flight` (feel card 4.7 within 10 %, path hold in a 60 deg bank with no
input, energy trade, floor nose drop, ceiling, detent, quaternion drift), `pilot` (axis ramps; short/long press with
scripted key timelines: 10 ms tap, 290 ms, 310 ms, 2 s hold, tap during focus loss), `takeoff` (READY -> handover
20-35 s, centre-line error < 2 m, no ground contact after liftoff, override), `terrain` (height vs grid,
`terrain_los` vs brute force on 1000 random rays, segment hits, timing), `world` (crash, bounds, PULL UP),
`radar` (cone, aspect range, terrain mask, ID timing, memory), `target` (S state machine), `missile` (straight chase,
crossing target, G limit, terrain impact, seeker LOS loss, flares), `dlz` (honesty targets 12.7), `ai` (50 fights: 0
terrain crashes outside DEFEND masking, fires only inside its envelope, defends), `mission` (waves, respawn, score),
`determinism` (two headless runs, same hashes), `replay` (record/replay incl. PARAMS records), `all`.
Windows-only: `-hudtest` checks (10.8), `-test input` (prints keys/joystick 10 s), `-test audio`.

### 16.2 Scripts
`-script file`: lines `T keydown NAME`, `T keyup NAME`, `T expect <field> <op> <value>`; used for takeoff, S
short/long, fire sequences, menu flows (M3 `-mscript` as racer).

### 16.3 Mac harness `tools/simh` (clang, all portable code, `data/island/jet.isl`)
`simh test NAME`, `simh feel` (prints the feel card from the current `jet.ini`), `simh shots N` (Monte Carlo shots at
fractions of Rmax/Rne against straight/escape/AI-defence targets -> hit-rate table), `simh aifight N seed diff`
(robot player vs a wave: kills, deaths, crashes, missiles fired/hit, terrain kills), `simh dlztable`, `simh island`.

### 16.4 Benchmarks (`-bench N -benchroute R`, racer bench.cpp pattern)
Autopilot routes: `low` (50 m over the runway and coast), `volcano` (1500 m around the crater), `high` (8000 m over
the sea looking at the island), `clouds` (through the puff layer), `combat` (scripted: 4 enemies, 8 missiles, 2
explosions near the player, HUD fully loaded). Report: fps_avg, 1%-low, p99, missed slots, zone/section ms (sim,
terrain, sea, sky, clouds, aircraft, fx, hud, dlz, present) and counters (tris, draws, texture changes); `pass=`
against 9.3. Window visible (`--show`), lock held, no VNC.

### 16.5 Feel play-test workflow (M1 and every feel round)
Every flight writes `last.rpl` (+ `-telemetry` CSV on request). The user's notes go to `docs/PLAYTEST_M1.md`, the
replay to `tests/user/`. Before changing numbers, the tuning task replays the user's flight and reads what the axes,
G and rates actually were. The user can also tune live: edit `jet.ini` (on the Mac via the share, or in Pica in the
VM), press F5, fly; the F10 overlay shows the numbers.

### 16.6 Logs
`jet.log` (play), `jet_test.log`, `jet_bench.log`; one fact per line `tag: key=value`; never overwrite the player's
log from a test.

---------------------------------------------------------------------------------------------------------------

## 17. Milestones

| Milestone | Contents | Gate |
|---|---|---|
| **M0** | T01 engine copy + skeleton builds (VM + simh); T02 probe -> `probe/RESULTS.md` decision (Plan A/B, near/far, fog, HUD stroke); T03 islandc v1 with a placeholder island; art starts in parallel (A1 jet, A2 sky, A3 textures, A4 HUD font/symbols, A5 sound v1) | orchestrator review of RESULTS.md and previews |
| **M1 free flight** | terrain + sea + sky + base, jet with moving surfaces + afterburner, cockpit + chase cameras, flight model + keyboard shaping, takeoff automation, crash/respawn, bounds, PULL UP, flight HUD (10.2-10.4 without contacts), minimap with island, flight sounds, F5/F10, replays, Low 30 fps on the VM, `play.bat`, `docs/PLAYTEST_M1.md` | **user feel play-test** (the key gate); feel rounds (T10) until accepted |
| **M2 combat** | radar + HAFU + targeting, target symbology, both missiles, launch zone, flares, weapon HUD + warnings, enemy AI with missiles, damage/respawn, TRAINING + 1-wave SORTIE, combat fx + sounds, combat bench 30 fps | **user combat play-test** |
| **M3 polish + release** | menus, 3-wave sortie, score/records, options + key capture, balance from play-tests, visual polish, island detail, real-hardware High pass, README, Setup98 installer + zip | user play-test of the whole game -> 1.0 |

M1 is deliberately lean: no menus, no enemies, simple clouds, placeholder base boxes are acceptable; what must be
right is the controls, the camera, the HUD and the frame rate.

---------------------------------------------------------------------------------------------------------------

## 18. User answers (2026-11-13)

| # | Question | Answer |
|---|---|---|
| Q1 | Gun or missiles only? | **Missiles only** |
| Q2 | Island look | **Green** island for now |
| Q3 | Enemy aircraft | **Same model in a different colour shade** (no A12 for now) |
| Q4 | Minimap | **Show all enemies** for now (`hud.mapShowAll=1`) |
| Q5 | Toughness | **3 lives**; maybe **three hits** to go down (tunable, revisit later); **always start from the airfield** (respawn = runway + auto takeoff) |
| - | G at speed | **Limit G at high speed** (~7 g at M1.6); "we'll see how it behaves" |
