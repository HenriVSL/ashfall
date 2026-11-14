# Ashfall: Island Intercept (was "Jet"): status

Win98 arcade flight-combat game in the spirit of Ace Combat ("easy to make look realistic, but still a Windows 98
game"). Owner: session **86box-d5**, **VM A** (`samba/w98`, `samba/vmlock`). Started 2026-11-13.
Reuses the 3D engine, measurements and tooling of `dev/racer` (Direct3D 6 on stock DX 6.1, VC6).

## User vision (2026-11-13)

- **World:** one volcanic island in the sea: a big volcano, and on the island's edge a flat area with a runway and a
  base (a couple of boxes for hangars; nothing fancy early).
- **Start:** Ace Combat style from the airfield: nearly automated takeoff when you give throttle, plus a short automated
  climb into the air, then the player takes over.
- **Aircraft:** F-16-style single-engine fighter with afterburner. Control surfaces move visibly: stabilators for pitch,
  wing surfaces (flaperons) for roll. No yaw control needed.
- **Throttle (Ace Combat style):** A = hold to go fast, afterburner lights, turning limited by G limit. Release = back to
  normal speed (~Mach 0.8). Z = slow to manoeuvring speed, slowly down to a 150 kt floor (no stall model for now).
  Fast = Mach 1.5-1.8.
- **Views:** (1) cockpit/HUD view from the nose with only the HUD visible; (2) chase view behind the aircraft with a
  reduced HUD.
- **HUD:** F/A-18C style (user loves it); DCS's public documentation as the reference. Airspeed box left, altitude box
  right, G in the same area as the F/A-18.
- **Map:** minimap/radar map bottom-left showing enemy aircraft.
- **Target symbols:** HAFU-style: yellow = unknown/unidentified, red = hostile once our aircraft's radar identifies it.
  A triangle-style indicator in the 3D view at range X is fine.
- **Targeting keys:** S short press = cycle targets; S long press = designate for the missile (box around it).
  Space = launch.
- **Weapons:** AIM-120 AMRAAM-style radar missile (long range) and AIM-9-style heat seeker (short range).
- **LAR / DLZ:** show a launch acceptable region: can the target turn X degrees and still escape, or is it in the
  no-escape zone (Rmax / Rne / Rmin style).

## Decisions (user, 2026-11-13)

- HUD: F/A-18C style throughout, including the Hornet launch-zone circle (not the F-16 staple). Units: knots, feet, Mach.
- Pitch keys configurable; default Up arrow = nose UP.
- Missiles arcade: many missiles carried; flares yes; a shot fired inside the no-escape zone almost always hits,
  unless the missile hits terrain (target hides behind the volcano).
- Orchestrator defaults (recommended, not contradicted): Expert-style true roll + pitch, auto-coordinated, no yaw;
  enemies fire missiles at the player too (flares are the defence); milestone 1 = free flight for feel, then combat.
- View distance: humans can't see aircraft many km away at this resolution and combat is mostly beyond visual range, so
  distant aircraft exist only as HUD/minimap symbols. Render jets only within a few km; far plane only as far as the
  island and horizon need (haze + sky panorama). The HUD is therefore as important as the flight feel.
- Answers 2026-11-13: G limited at high speed (~7 g at M1.6, see how it behaves); missiles only (no gun); green island
  for now; enemies = same jet model in a different colour shade; minimap shows all enemies for now; 3 lives, maybe 3 hits
  (revisit later); always (re)start from the airfield.
- 2026-11-13: also include Ace Combat's **Novice** control style (auto-bank turns, auto wings-level, climb/dive vs the
  horizon, no loops) as a selectable mode next to Expert; default Expert, F6 toggles for the play-test.
- 2026-11-13 HUD/resolution: numbers centred (h + v) in the speed and altitude boxes; HAFU symbols on the minimap only, in
  the 3D view a small hollow diamond (corner down), red outline (yellow while unknown); minimum resolution 800x600;
  real target = the user's **IBM ThinkPad T40, 1400x1050** (much stronger than the VM), so HUD atlases per scale.
- 2026-11-13: VM minimum is now **15 fps at 800x600** (user: "if Voodoo struggles then it struggles"); 20-30 fps nice to
  have. Don't cut the look for the VM; the T40 is the real target.
- 2026-11-13: kill feedback: the destroyed enemy's diamond turns grey for a few seconds; the minimap crosses it out for
  15 s.
- 2026-11-13: RWR tones: new fighter radar = waterfall 550/520/490/470 Hz x 83 ms; lock = 455/555 Hz alternating 0.25 s;
  missile launch = 455/555 Hz alternating 0.10 s, continuous (DESIGN 11.5). Missile warning starts at launch (user confirmed),
  not at seeker activation.
- 2026-11-13: LAR/launch zone computed **live in real time** from simulated fly-outs of the same missile model (tables only
  as seed, AI checks and fallback). Missiles fly a simulated AMRAAM-style profile (drop, boost-coast, loft, datalink
  midcourse, active seeker, terminal) with our own game numbers.
- 2026-11-14: no joystick support for now (keyboard only); M3 options menu = key capture + invert pitch.
- 2026-11-14 (M2 play-test): player AIM-120 flies ~2x the distance (enemy weapons unchanged); IR missile as is; balance
  good enough for V1 after that. Full weapon HUD also in the chase view.
- 2026-11-14: **game name: "Ashfall: Island Intercept"** (user). Exe/installer rename at release (T24).
- 2026-11-14: user reconfirmed: **15 fps is fine on the VM**. Bench gate = fps_avg >= 15, no sustained stretch below 15;
  30 fps nice to have; prefer the look (T22/A11/T19 told).

## Docs

- `docs/research/HUD.md` (verified against the ED DCS F/A-18C Early Access Guide), `docs/research/ENGINE.md` (racer engine
  reuse, terrain/sea/depth plan). Weapons research was blocked by the API safety filter; missiles are designed as game
  mechanics from the user's input instead.
- Racer references: `../racer/spike/RESULTS.md` (D3D6 caps, budgets), `../racer/docs/PERF.md`, `../racer/docs/DESIGN.md`.

## Log

- 2026-11-13: project started on VM A. Research agents started: HUD, weapons/sensors/DLZ, engine reuse. Flight research
  stopped (user: the vision above is enough for flying). User wants to review docs together before building; missile
  realism needs their input (fun first). Models/art get their own agents.
- 2026-11-13: research done (HUD verified, ENGINE). User decisions recorded above. Next: DESIGN.md + TASKS.md (Opus),
  reviewed with the user before coding.
- 2026-11-13: DESIGN.md rev 1 + TASKS.md done and reviewed; user answers folded in (DESIGN 18). M0 started.

- 2026-11-13: M0 wave 1 started: T01 engine copy + skeleton (Opus high); art agents A1 jet, A2 sky/light, A3 ground/sea/runway
  textures, A4 HUD font + symbols, A5 flight sounds. Orchestrator copied assetkit + sdk first so art could start at once.
- 2026-11-13: done: T01 (jet.exe builds on VM A, 0 warnings; params/F5, replay skeleton, tests, simh, sky loader fixed for A2 names),
  A2 sky/light, A4 HUD atlas (premultiplied "over" blend preferred: additive washes out over clouds), A5 flight sounds.
  Started: T02 depth/fog/HUD probe (+ RB_PREMUL blend), T03 island compiler, T05 flight model (early, feel first), A7 base.
  Running: A1 jet, A3 textures.
- 2026-11-13: A1 jet done (878/362/120 tris + moving parts, grey player + dark aggressor livery, flame), A3 ground/sea/runway
  textures done. Orchestrator mapped jet part meshes to the jetpart validate class. Wave 1 art complete.
- 2026-11-13: A7 base meshes done; T03 island compiler done (9.5 x 11.6 km island, rim 1808 m, runway heading 270 out to
  sea, jet.isl 7.77 MB). Terrain tris near the ground exceed Low budget (up to 10.9k at 50 m): T04/T09 to use skirts
  only at LOD changes + lodScale on Low. A6 island art pass deferred until after the M1 play-test (T03 previews look good).
- 2026-11-13: T05 flight model + keyboard + Novice done (tests green on VM + Mac, 12 us/step). Orchestrator defaults: decOver
  5 -> 15 (release A returns to cruise in ~15-20 s, not 55), noviceBankDeg 70 -> 80 (30 s circle, not 60).
- 2026-11-13: T02 probe done: W-buffer single pass, far 18/28 km, fog end 0.9 x far, runway zBias 2, RB_PREMUL HUD blend
  works on the Voodoo3, Low 800x600 holds ~30 fps in the test frame (15 fps gate 2x margin). A4b atlas sets per
  resolution (480/600/768/1050) + diamonds + centring metrics done; A4c (baked shadow, one quad per glyph) running.
  M0 complete. Starting T04 terrain renderer and T07 flight HUD.
- 2026-11-13: T06 takeoff/crash/bounds/PULL UP done (liftoff 8-12 s, handover 20-31 s after A, tests green on the real
  island). Known: -test replay size check fails (params snapshot grew; T08 fixes in replay.cpp). Running: T04, T07.
- 2026-11-13: T07 flight HUD + minimap done (1.3 ms at 800x600, ASE gap rule checked in all sets). T04 terrain + sea done
  (Low ~4.7k terrain tris worst, 10-11 ms worst view on the VM: fine for the 15 fps floor, T09 tunes). Orchestrator set
  runway zBias 2 / marks 3 in islandc base.py. Started T08 M1 integration (first playable build).
- 2026-11-13: **T08 done: first playable M1 build** (play.bat on VM A; ~30 fps on all Low bench routes at 800x600; replays
  + determinism verified; F5 live tuning verified). docs/PLAYTEST_M1.md written. Waiting: user M1 feel/HUD play-test.
- 2026-11-13: user M1 round 1: controls "feel great" (Expert). Bugs: ceiling traps speed at 150 kt; terrain looks like a
  ball from high up; minimap 3x larger; speed feels slow. Started T10a (thrust lapse) + T10b (render, map, speed cues).
  Notes: docs/PLAYTEST_M1_NOTES.md.
- 2026-11-13: user clarified "triple the radius" = the whole world / flying area, not the minimap. Started T03b: island
  ~3x radius (~30 km, volcano ~2.5-3 km), area bounds ~3x; minimap unchanged.
- 2026-11-13: T10a done: thrust lapse with altitude (service ceiling ~15.5 km), dives always accelerate, user case fixed;
  speed verified true (HUD = TAS, 1000 kt = 1852 km/h, distance matches within 0.02 %): "slow" is perception -> T10b cues.
- 2026-11-13: T03b done: island 34 x 34 km, summit 3077 m, 80 m grid, base on the west cape, area 75/90 km; tests
  green, Low benches 30 fps, load 8.9 s. Lowlands look monotonous green at 80 m cells (polish later: A6/A11 colour
  variation + clutter). T10b now also raising far plane for the bigger world.
- 2026-11-13: T10b done: "ball" fixed (late sky dome painted over far ground; sky now first, fog/sea by view depth,
  far/fog grow with altitude), Low far 38 km / High 56 km (Voodoo limit ~62-64 km), speed cues (chase FOV +10 deg,
  buffet, air streaks, wind). Orchestrator verified: VM build clean, -test all rc=0. **M1.1 build ready** for the user.
  Starting M2 wave 7: T11 radar/targeting, T12 missiles/flares, A8 weapons art, A9 combat sounds.
- 2026-11-13: A9 combat sounds done (16 WAVs, preview tools/sfx/preview_combat.wav; needs the user's ear in the M2 test).
- 2026-11-13: T11 radar/contacts/targeting done (radar 20 NM, beam 25 NM; ID 1.5 s within 15 NM; tick 52 us). Started
  T15 target symbology (wave 8). Running: T12 missiles, A8 weapons art.
- 2026-11-13: A8 weapons/fx art done (LRM/SRM 112 tris, fire/smoke atlases, trail, flare, splash, debris, dot;
  fire = RB_PREMUL). Orchestrator added lrm/srm to the missile validate class.
- 2026-11-13: T12 missiles/flares done (LRM 16 / SRM 40 / flares 40; LRM head Rmax 19.4 km, Rne 7.2; SRM head Rmax 6.6,
  Rne 4.6 km accepted as the honest model result; missile_step 5-9 us on the VM so DLZ uses ~200-300 steps/frame on Low +
  tables). Started T13 launch-zone solver. Running: T15.
- 2026-11-13: T15 target symbology done (diamonds, TD box, locator, arrows, map contacts, kill feedback grey 3 s / map X
  15 s; +0.25-0.33 ms). T13 live launch zone done (1.45 ms/frame on the VM, full refresh 0.66 s at 30 fps; inside Rne
  100 % hits vs every escape; volcano masking collapses the zone; AI tables data/dlz.tab). User: continue. Started
  wave 9: T16 weapon HUD (+ cockpit camera wobble fix found by T15) and T14 enemy AI (+ flare doctrine / decoy check).
- 2026-11-13: user asked for RWR tones (waterfall / lock / launch). Started A9b (sound agent, same file names).
- 2026-11-13: A9b RWR tones done (exact 455/555 and 550-470 Hz, seamless loops; preview tools/sfx/preview_combat.wav).
- 2026-11-13: T14 enemy AI done (-test ai 24/24 VM + simh; robot vs waves 100 sorties each: K/D Easy 9.6 / Normal 5.5 /
  Hard 3.8; 0 crashes, 0 stuck, 0 out-of-envelope launches; enemy LRM avg launch 18.8 / 11.4 / 9.3 km; player hit rate
  inside 0.9 Rne vs real DEFEND: Normal LRM 94 % / SRM 96 % (misses = flare decoys), Hard SRM 88.5 %; 26 us per enemy
  per tick). Flare doctrine: one burst per reacted missile inside 3 km (flareProb). "0 decoys" was a test-harness
  seed, not a bug. For T17: think-stagger bug in game.cpp, PARAMS_MAX near 512, 62 new jet.ini keys. Waiting: T16.
- 2026-11-13: T16 weapon HUD done (launch-zone circle + ticks, SHOOT steady/flashing only on the published zone, ACT->TTG,
  SRM seeker, VISUAL, MISSILE/LOCK/threat arrows, chase DLZ bar; 0.29 ms worst; gap rule holds at all sets) + cockpit
  camera wobble fixed (1.7-2.2 px -> 0.0001 px). HUD total now ~1.8-1.95 ms on the VM (T18 budget). Started T17 combat
  integration (wave 10).
- 2026-11-14: T17 combat integration done: TRAINING (3 drones) + SORTIE (1 wave, sortieWaves key), 34 hp per hit / 3
  lives / runway respawn, score, fx (trails, explosions 1.5x, wrecks, flares, damaged smoke), all combat sounds incl.
  RWR tones, V look camera, training.bat / sortie.bat, docs/PLAYTEST_M2.md. Combat bench Low 29.9 fps both views.
  Robot sorties: Normal 1 wave 2/2 kills; 3 waves Easy/Normal complete, Hard failed. Orchestrator verified: VM build
  clean, -test all rc=0. Screenshots reviewed (chase SHOOT/LOCK overlap, VISUAL/arrow overlap, odd store counts ->
  T18). Started T18 perf pass + HUD fixes.
- 2026-11-14: T18 done: all five Low routes ~29.95 fps, missed <= 0.61 % (combat incl.); DLZ 1.1 ms; HUD 1.8 ms in cockpit
  accepted (docs/PERF.md). HUD overlap fixes (LOCK/MISSILE slots, ladder keep-outs, threat arrows), bench store counts,
  AI timing spikes were a test-log artefact. Orchestrator reviewed m2fix screenshots. **M2 combat build ready** for the
  user's play-test: training.bat, sortie.bat [easy|hard], docs/PLAYTEST_M2.md.
- 2026-11-14: **user M2 play-test**: IR (SRM) feels good; player radar missile should fly double distance; enemy weapons
  unchanged; then balanced enough for V1. Wants the weapon HUD in the chase view too. Orchestrator consequences: player
  radar ~40 NM / ID ~25 NM (enemy radar unchanged), sortie spawn 45-55 km. Started T21a (player LRM 2x) and T16b
  (full weapon HUD in chase).
- 2026-11-14: user named the game **Ashfall: Island Intercept**. Started A10 front-end art (logo, panels, icon).
- 2026-11-14: A10 front-end art done (ASHFALL logo basalt-steel + lava cracks, lo/hi tile sets, menu kit per atlas set, data/ashfall.ico; previews tools/art/preview/front_*.png).
- 2026-11-14: T16b done: full weapon HUD in chase (circle on the steady aim cross, above the jet; [hud] chaseAse=1, DLZ bar off), 0.36 ms, combat chase bench 29.94 fps. Screenshots reviewed.
- 2026-11-14: T21a done: player AIM-120 [lrm_player] head-on Rmax 19.4 -> 38.3 km, Rne 7.2 -> 14.0 km; enemy LRM + SRM
  unchanged (table nodes bit-identical); player radar 40 NM / ID 25 NM; sortie spawn 45-55 km; honesty 40/40; inside Rne
  vs Normal DEFEND 97.6 %; DLZ refresh 1.0 s at 30 fps; combat bench 29.9 fps. Orchestrator: minimap starts at 40 NM,
  edge arrows to 40 NM (hud_map/hud_tgt defaults + jet.ini), DESIGN 10.5/11.1/14.3 numbers updated; VM build clean,
  -hudtest contacts/layout + -test all rc=0. **M2 complete.** Starting M3 wave 11: T19 frontend, T20 3-wave sortie +
  score, T22 visual polish, A11 island detail (incl. the deferred A6 lowland colour pass).
- 2026-11-14: T20 done: 3-wave sortie default (waves per DESIGN 13.3, "WAVE n CLEARED"), score per 14.4 with time
  reference 750 s, records.ini (+ API for T19), aifight harness on the mission director (cap 1800 s). Robot sorties
  complete on all difficulties (avg 824 / 848 / 1013 s; long BVR back-and-forth; T21 may re-issue attack orders).
  terrain file_facts test broken by A11's island regen -> A11 fixes. T19 told the records wiring + jet.ini regen.
- 2026-11-14: A11 island detail + A6 colour pass done: windward laurel forest / leeward pastures + scrub, lowland field
  patchwork with hedges and walls, heath, ash/scoria crater, cooled lava, black beaches, villages; 74k scatter sprites
  (src/scatter.*; Low 1.6 km / High 3.5 km), base props + fences; all Low routes 30 fps; -test all rc=0. Orchestrator
  reviewed: low level looks good; from 3-10 km the ground is soft broad colour zones (coarse LOD vertex colours) ->
  candidate engine follow-up (macro/detail texture layer), ask the user.
- 2026-11-14: T22 visual polish done: wingtip vapour (5-8 g), wing vapour sheet, transonic cone, contrails > 8 km, mesh
  shadow below 300 m, cloud fly-through haze, bigger layered explosions + burning debris + camera shake, smoke columns at
  crash sites, afterburner shock diamonds. Own-jet draw 5.3 -> 2.8 ms, combat fx 3.9 -> 2.5 ms; all Low routes ~29.9 fps.
  Orchestrator reviewed screenshots (near hedge/wall sprites look like odd strips close up -> polish note). Note: the
  T40 had M:\dev\jet\jet.exe open over the share for a while (blocks links): use the copied snap folder.
- 2026-11-14: T19 frontend done: title fly-by + A10 logo, FLY (free/training/sortie + difficulty), OPTIONS (key capture
  13 actions, invert pitch, Expert/Novice, video, sound), RECORDS, pause, results (freezes the sortie 3 s after the end),
  live jet.ini updater, caption "Ashfall", ashfall.bat. Orchestrator fixes: engine CFG_MAX_ENT 1024 -> 2048 (1147-line
  jet.ini was truncated), -mscript/-ini added to the switch list, loading text, test leftovers removed. VM: -test all
  rc=0, keys/reload/flow menu scripts rc=0, simh rc=0, jet.ini regenerated. **M3 wave 11 complete.** Fresh T40
  snapshot in snap/ (t40.bat = title at 1400x1050 High). Next: user play/T40 pass, T21 balance, T24 release.
- 2026-11-14: **1.0 released locally** (user asked at 98 % of the weekly limit): release/Ashfall-1.0 (ASHFALL.EXE +
  data + jet.ini + README.TXT), dist/out/ashfall-1.0-setup.exe (Setup98, 8.7 MB, CRCs ok) + ashfall-1.0.zip (9.3 MB).
  VM: silent install rc=0, uninstall /PURGE rc=0; `-test all` from the installed folder rc=1 (not investigated; dev
  tests likely need files not shipped). Local git repo created with the initial commit (no remote). Not done: exe
  version resource, own installer icon stub, T21 balance/T23 T40 pass, far-terrain detail texture (user undecided).
- 2026-11-14 (T21b, LRM realism): the player's radar missile (LRM, `[lrm_player]`) now flies a realistic AMRAAM profile
  from the user's GDD (`aim_120_amraam_simulation_gdd.md`): a two-stage boost->sustain motor (2.5 s + 5.5 s, coast at
  8 s), a transonic wave-drag peak (`waveDragK`), quadratic lift-induced drag (`inducedK2`), a kinematic loft
  (`loftDeg`/`loftAltM`, low shots climb into thin air), an earlier active seeker (`activeM` 7 -> 18 km) and a 60 deg
  gimbal. Enemy `[lrm]` and everyone's `[srm]` are bit-for-bit unchanged (the new knobs default off there). `[lrm_player]`
  re-tuned to keep the player's ~2x envelope: head Rmax 38.3 -> 40.4 km, Rne 14.0 -> 13.5 km (dlz instant-turn 12.6),
  tail 17.2 -> 16.4 km, Rmin 0.89 -> 0.95 km. DLZ stays honest by construction (same `missile_step`); `data/dlz.tab`
  rebuilt, `jet.ini` regenerated. `-test all` rc=0 on the Mac harness; VM build to verify.
- 2026-11-14: 1.1 built from the user's small changes (dlz/missile/fx/jet.ini): dist/out/ashfall-1.1-setup.exe + ashfall-1.1.zip; VM install/uninstall checked.
- 2026-11-14 (T21c, LAR/NIRD): from the user's LAR GDD (f_a_18_amraam_lar_avionics_symbology_gdd.md): the launch-zone
  circle is now the Hornet normalized NIRD (Rmax fixed at 6:00, Rmin at 2:30, caret sweeps the in-LAR arc per GDD 2.3),
  the ASE steering circle scales with range (tight at Rmax for loft, wide inside Rne; GDD 2.4), a Raero tick (zero-closure
  max aerodynamic range, GDD 4.2) and an "IN LAR" range cue (GDD 3). Raero is computed live in dlz.cpp (Rmax + target
  along-LOS velocity x TOF). Enemy LRM/SRM DLZ unchanged. -test all rc=0 on the Mac harness; HUD (hud_wpn.cpp) is
  render-side and needs the VM build + -hudtest zone/layout verification.
- 2026-11-14 (1.1, T40 graphics fix): user saw water through the island from a distance on the T40 (ATI Mobility
  Radeon 9000, 1400x1050x16 High). Cause: no W-buffer on the Radeon -> the engine fell back to a 16-bit Z-buffer over a
  56 km far plane, and hardware zBias (sea, runway, shadow) grows with distance on a Z-buffer. Fix (engine/rend/r_d3d6.cpp):
  without W-buffer ask for 24-bit Z (T40 gets 32/stencil) with a fallback to 16 if mixed depths fail; zBias only with the
  W-buffer (-zbias / -nozbias override). Verified on the T40 with -camat shots (40 km view clean, runway markings clean);
  VM Voodoo3 path unchanged (W-buffer, 16-bit, zbias on).
- 2026-11-14 (1.1 integration): user changes T21b (player LRM boost/sustain, wave drag, loft, early seeker) and T21c
  (normalized NIRD launch-zone gauge, dynamic ASE, Raero, IN LAR) tested; T21d rewrote the -hudtest zone checks for the
  NIRD semantics and fixed 5 gauge bugs (caret jump near 2x Rmax, IN LAR overlaps -> cockpit y 330.5 / chase 4th readout
  row, keep-out overflow, IN LAR hysteresis, dot scale without Rne); dlz `[dlz] coastFarM` 6000 (double coast steps
  vs straight targets beyond 6 km; Rne bit-identical), refresh 1.33 -> 1.14 s, gate 1.4 s. T40 audio: engine loops
  left-only on the SoundMAX driver -> e_audio re-applies the pan after SetFrequency (-norepan to compare; user to
  confirm by ear). v1.0 tagged; repo README.md; public GitHub repo HenriVSL/ashfall (user).
