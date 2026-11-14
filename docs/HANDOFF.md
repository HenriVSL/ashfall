# Jet: handoff notes (append only)

## Design

The contract is `docs/DESIGN.md` (rev 1) and the task list `docs/TASKS.md`; user vision and decisions in `STATUS.md`.
Each task appends a `## Txx/Axx <title> (date)` section below with the facts a later task needs.

## T01 (early note) assetkit ready for art tasks (2026-11-13)

- `engine/tools/assetkit/` (copied by the orchestrator) now has the jet budget classes of DESIGN 8.8 in
  `validate.py` (`python3 engine/tools/assetkit/validate.py --list-classes`): `jet` (LOD0 600-950, LOD1 220-380,
  LOD2 70-140, exactly 3 LODs, the 17 tags of 8.8 required), `jetpart` (4-60 tris; warning if the origin/hinge is
  outside the mesh box), `missile` (LOD0 30-120, LOD1 1-20), `bldg` (12-300). Name prefixes: `jetpart*` -> jetpart,
  `jet*` -> jet, `missile*`/`msl_` -> missile, `hangar tower tank bunker bldg_` -> bldg. Texture rule `fx_*` = 128x128
  or 256x256. Racer classes unchanged (except the prefix `bldg_` now maps to `bldg`, 12-300). Or declare the class
  in the generator's `ASSETS` dict form. selftest: 119/119 pass.
- `tools/build_assets.py` is in place (racer wrapper + the island hook: generator "island" = `island/island.txt`
  through `tools/islandc/islandc.py` `build_island(src, out_dir, kit) -> [written relative paths]`, same shape as the
  kit's trackc protocol; skipped while islandc doesn't exist).

## A5 Sound pack v1, flight (2026-11-13)

- Generator `tools/sfx/gen_sfx.py` (assetkit protocol, `ASSETS` = 16 WAVs + `sfx/README.md`); run
  `python3 tools/sfx/gen_sfx.py [--check]` (= `tools/build_assets.py --only sfx`). About 7 s, deterministic
  (assetkit `Rng(5005, ...)`), `--check` identical; validate 16 files 0 errors. Output 1.3 MB.
- Files in `data/sfx/` (mono 22050 Hz 16-bit): loops `eng_idle eng_mil eng_ab` (3.0 s), `wind` (4.0 s), `brake`
  `roll` (2.0 s), `pullup` (1.0 s); one-shots `ab_light` (1.3 s) `gear_up` (3.0 s) `gear_clunk` (0.6 s)
  `expl_near` (3.5 s) `expl_far` (4.0 s) `ui_move ui_ok ui_back beep`. Every loop passes the seam check
  (printed per build). Loops start at sample 0 with DSBPLAY_LOOPING.
- **Mix law for T08's `snd.cpp`** is in `data/sfx/README.md`; `mix_engine()`, `mix_wind()`, `mix_brake()`,
  `mix_roll()`, `spool_step()` in the generator are the reference. Short version: lagged spool `N` (targets READY
  0.65, SLOW 0.74, NORMAL 0.90, FAST 1.0; tau 0.8 s up / 1.5 s down); idle/mil equal-power crossfade by
  `t = log(N/0.65)/log(1/0.65)`; `w_ab = sin(ab*pi/2)` with `w_mil *= 1 - 0.35*ab`; pitch idle `N/0.65*kv`, mil
  and ab `N*kv`, `kv = 1 + 0.06*(clamp(Mach,0.2,1.8) - 0.8)`. The engine loops share one baked gain (idle -10 dB,
  mil -4.5 dB re ab): no per-loop gain table. `ab_light` once when `ab` starts rising; `gear_up` at gear-up start,
  `gear_clunk` 3.0 s later; `pullup` loops while PULL UP is shown.
- Keep the sound master around 0.7: AB + wind + a pull sums to ~1.4x full scale (the preview is normalised).
- `tools/sfx/preview_flight.wav` (48 s, written on normal runs, not under `--check`): ground idle, AB takeoff
  roll + gear, NORMAL, AB to Mach 1.36 with a 5 g pull, NORMAL, speed brake, NORMAL, all with the README law.
- For A9: DSP/WAV helpers (`rng`, `lp/hp/bp/bp_vary`, `steady`, `tones`, `thump`, `noise_burst`,
  `grains_decaying`, `add_pulse`, `seam_check`, `put_wav(kit, name, pcm, info, conv, loop, log)`, `fades`,
  `fade_out`, `SOFT_SQ`, `tone_seg`) are importable without side effects; put the combat sounds in
  `tools/sfx/gen_sfx_combat.py` with its own `ASSETS`, and its README as a separate file or section (the A5
  build rewrites `data/sfx/README.md` on every run, so don't append to that file from A9).
- The assetkit has no WAV writer: WAVs are written with the stdlib `wave` module (as the racer's gen_sfx) and
  validated by the kit's `check_wav` through the build runner.

## A2 Sky panorama, light set, clouds, sun glow (2026-11-13)

- Generator `tools/art/gen_sky.py` (assetkit protocol, ORDER 20); run `python3 tools/build_assets.py --only sky`
  (`--check`: identical, ~16 s; with previews ~28 s). Validate: 18 files, 0 errors, 0 warnings.
- **Outputs**: `data/sky/sky_hi_0..7.bmp` (High 2048x256 as 8 x 256x256), `data/sky/sky_lo_0..3.bmp` (Low 1024x128 as
  4 x 256x128, BOX downsample), `data/sky/light.txt` (CRLF), `data/tex/cloud_puffs.bmp` + `_a` (256x256, 2x2 atlas of
  128 px cumulus: cell i at x = (i % 2) * 128, y = (i / 2) * 128; 0 wide, 1 tower, 2 flat, 3 round),
  `data/tex/cloud_deck.bmp` + `_a` (256 tileable), `data/tex/sun_glow.bmp` (128x128 RGB, premultiplied for RB_ADD,
  black border, no `_a`). Previews: `tools/art/preview/sky_strip.png sky_view_{sun,anti,high,puffs,low}.png
  sky_tiles.png cloud_puffs.png cloud_deck.png sun_glow.png`.
- **Light set** (`light.txt`, racer `fi_evening.txt` format + extra keys; numeric `key a b c` lines): sun azimuth 230
  elevation 40, `sunDir -0.586824 0.642788 -0.492404` (toward the sun, x east, y up, z north), `sun 255 244 226`,
  `ambient 132 150 178`, `skyfill 116 150 204`, `hemi 0.35`, `zenith 40 86 172`, `top 50 98 182`,
  `horizon 194 211 231`, **`fog 196 212 228`**, `fogStart 3600 fogEnd 18000` (Low defaults; presets override),
  `vHorizon 0.9375 elevLo -10 elevHi 55`. Cloud hints: `cloud_puff_size 450 1100` m, `cloud_alt 1800 4000`,
  `deck_alt 6000`, `deck_tile_m 3000`, `puff_tint`, `deck_tint`. Word-valued keys (`set`, `tile_prefix sky_hi`,
  `tile_prefix_low sky_lo`) are skipped by the engine's sscanf parser.
- **Deviation**: `elevHi` is **55**, not 40 (DESIGN 8.4): with the sun at 40 deg and the panorama ending at 40, half
  the disc and the aureole would fall into the vertex-coloured zenith cap. The engine reads elevLo/elevHi/vHorizon
  from the set, so nothing else changes. Resolution: High 4.4 rows/deg above the horizon (Low 2.2), plenty for
  the smooth gradient; the horizon cloud banks are soft anyway.
- **Fog join**: every panorama row below the horizon (High rows 238-255, Low 119-127) is exactly `fog`; the haze
  ramp reaches `fog` at 0 deg and fades out by +6 deg; horizon band (-0.75..+0.25 deg) average = fog, column luma
  deviation 0.00. The fogged sea (fog end = far plane) meets the panorama seamlessly at any altitude (preview
  `sky_view_high.png`: 4000 m, pitch -8). Top row exactly `top`. Brightest column above the horizon = az 230.2
  (engine `sun_check` should report diff < 1 deg). 360/0 wrap seam 0.02 vs adjacent 0.13.
- **Engine naming (T08/T04 action needed)**: the copied `world_sky_load(w, maxTiles, prefix)` loads `<prefix>_0..7`,
  Low `<prefix>_low_0..3` and the set file `sky/<prefix>.txt`; TASKS fixes the names `sky_hi_*`, `sky_lo_*`,
  `light.txt`. With `prefix = "sky_hi"` High works but Low falls back to halving the High tiles (works, just ignores
  the dedicated Low files) and the set file is not found. Fix in the engine/scene: read `light.txt` explicitly (or
  fill `WSky` from it) and look for `tile_prefix_low`. Likewise `world_set_sun_glow` loads `tex/glow.bmp`, else a
  procedural glow: point it at `tex/sun_glow.bmp`. The engine scales the glow by sun * 56/255 (subtle, the disc +
  aureole are painted), 11 deg half size: fine with this texture.
- **Clouds**: puffs are lit white (light from above, toward the viewer), grey-blue flat bases, soft edges, alpha 0
  at every cell border (bilinear/mips safe). Draw RB_BLEND, fogged (colour to `fog`, alpha kept), 4444 (preview
  shows the engine's Bayer dither: fine). Deck: broken ~26 % coverage (alpha >= 128), soft edges, periodic noise
  so it tiles exactly (rgb seam 0.94 vs adjacent 0.91, alpha 6.5 vs 5.6).
- Texture memory (16-bit): sky High 1 MB, Low 256 KB, puffs 128 KB, deck 128 KB (+ mips), glow 32 KB.

## T01 Engine copy, skeleton, params/F5, simh, engine additions (2026-11-13)

- **Builds**: `w98 build jet` (and `clean` + build) with 0 warnings; Mac `make -C tools/simh` clang -Wall clean.
  Engine copy and every edit: `docs/ENGINE_COPY.md`. jet.exe loop/app flow: header of `src/main.cpp`.
- **Layout = DESIGN 2.4**: every module has a `.h/.cpp` (stubs marked "STUB by T01 (owner Txx)"): replace the
  whole file. Stub test files `src/tests_<area>.cpp` (flight pilot takeoff world terrain radar target missile dlz ai
  mission) are already in the Makefile, simh and the `-test` table: owners just replace them. Also stubbed:
  `feel_card_print` (T05, tests_flight.cpp), `simh_shots`/`simh_dlztable` (T13, tests_dlz.cpp), `simh_aifight` (T14,
  tests_ai.cpp), `simh_island` (T04, tests_terrain.cpp): simh already dispatches them (no simh.cpp edit needed).
- **Contracts fixed in stubs**: `PilotInput` (DESIGN 5.1) is in `pilot.h` (with `PilotKeys`, `PilotState`, THR_*);
  `FlightState`/`FlightVisual` (4.2/4.8) in `flight.h`; the terrain API (8.6) in `engine/world/e_terrain.h`;
  `missile_step` (12.2) in `missile.h`. `pilot.h`/`flight.h` must not include `game.h` (game.h includes them).
- **game.h**: the sim container is `Game g_game` (DESIGN says World; `World` is already e_world's type). `Aircraft`
  {id, side, flags AC_*, livery, fs, prev, in, hp}; per-aircraft state of other modules lives in their own arrays
  indexed by `Aircraft.id` (player = 0). Units/consts: SIM_HZ, SIM_DT, KT2MS, FT2M, NM2M, GRAV, `speed_of_sound(h)`.
  `Args g_args` + `args_parse` live in `src/game.cpp` (portable, so simh has them); not-yet switches log
  `args: not yet -x (Txx)` from the `s_notYet` table in game.cpp: remove your switch's row when you implement it.
- **params** (`src/params.h`, DESIGN 2.5): each module registers a static `Param` table from `xxx_params()`
  (`params_add`); portable modules are called by `params_register_all()` (params.cpp), Windows ones by main.cpp
  (`cam_params hud_params snd_params scene_params`). Types P_FLOAT/P_INT/P_BOOL/P_TABLE/P_STR, flag P_RESTART
  (`[video]`, registered in game.cpp). `ptable_eval` for tables. F5 = `params_reload` (logs `params: reload sec.key
  old= new=` + `params: reload changed=N`, on screen "INI RELOADED (N)" 2 s); `params_snapshot/restore/hash` for
  replays. `[dev] keys`, `[dev] testValue` (F5 check key). `jet.ini` = `jet.exe -dumpini jet.ini` output (only
  [video] + [dev] so far): **whoever integrates (T08, T17, T19) regenerates it**.
- **main.cpp**: tick order of DESIGN 2.6 wired with the stubs (flight_tick), `-headless` (null renderer, 120 s cap),
  `-exitafter`, `-log [N]`, `-logfile`, `-seed`, `-start air|runway`, `-shot T f.bmp`, `-test`, `-dumpini`, `-fps`/F11,
  F5, Esc = quit (T08: pause menu). Live keys are read with `in_key` in `read_live_keys` (T05/T08: define jet actions
  with `in_action_define` and use bindings; S needs an action for the short/long counters). Logs: jet.log for play;
  `-test -headless -exitafter -script -replay -dumpini -hudtest` -> **jet_test.log**; `-bench` -> jet_bench.log.
  A debug overlay (Arial e_ui font) shows "JET skeleton", fps, INI RELOADED and a boresight cross (ui_line, RB_ADD):
  T07/T08 remove it when the HUD exists.
- **replay** (`src/replay.*`): working skeleton (header + params snapshot, I/R/H/P/E records, quantise, hash every
  120 ticks, `-test replay` passes). Not wired into main yet (T08: -record/-replay/last.rpl, F5 already calls
  `replay_record_params` while recording). `test_determinism` = T08 stub.
- **Engine additions**: `cfg_reload`, `in_action_held_ticks`, `in_action_released` (returns ticks held on the
  release poll; sub-poll tap = 1), `ui_line`, `ui_set_blend`, `ui_clip`, `r_clear_z`. A2 sky names: `world_sky_load`
  reads `data/sky/light.txt` + `tile_prefix(_low)`, sun glow `tex/sun_glow.bmp` (ENGINE_COPY.md). Untested on screen:
  scene (T08) is the first caller of the sky; `r_clear_z` is for T02.
- **Exit codes**: agent98 shows a process exit code of -1 as **255** (`-test nonsense` -> rc=255).

## A4 HUD stroke font + symbol sheet + minimap frame (2026-11-13)

- **Generator** `tools/art/gen_hud.py` (`python3 tools/build_assets.py --only hud`, `--check` identical, 0.3 s
  without previews, ~15 s with). Outputs `data/tex/hud_atlas.bmp` + `hud_atlas_a.bmp` (256x256, 41 % of the texels
  used, 128 KB as 4444 without mips) and `data/tex/hud_atlas.txt` (CRLF, format documented in its header).
  Load with `tex_get("hud_atlas", IMG_ALPHA, RT_CLAMP | RT_NOMIP)`.
- **Texels are premultiplied white**: RGB = alpha = coverage (white shapes on transparent black; tint with the vertex
  colour). Shadow pass: RB_SHADOW (ZERO, INVSRCALPHA) or RB_BLEND with black at vertex alpha 0x80, offset (+1,+1).
  HUD pass: RB_ADD (ONE, ONE) as DESIGN 10.1 says, **or premultiplied over (ONE, INVSRCALPHA)** (needs a new blend
  mode in the backend). Finding for T02/T07: additive green washes out over cloud-white and bright sky (white +
  green = white); "over" keeps it saturated everywhere and looks the same over dark sea/terrain. Compare
  `tools/art/preview/hud_readability_1x.png` (additive) with `hud_readability_over_1x.png`, and
  `hud_mock_cockpit.png` with `hud_mock_cockpit_over.png`. Yellow (unknown HAFU) on cloud-white is weak in both
  modes; a stronger shadow (alpha ~0.7) helps it. `PREMULT = False` in the generator gives straight alpha
  (RGB white) if the HUD ever draws only with RB_BLEND.
- **txt records** (all integers): `font f cap cellH oy adv spaceAdv gap inkW stroke10`; `g f code x y w h ox adv`
  (draw the cell at (penX + ox, capTopY + oy), pen += adv; text ink width = sum(adv) - gap); sprites
  `name x y w h ox oy` where (ox, oy) is the ANCHOR, a texel-edge point inside the rect: draw the rect's top-left at
  (Ax - ox, Ay - oy), rotate about the anchor. Comment lines start with `#`.
- **Fonts** (1:1 at 480 lines, monospaced: every digit, capital and symbol shares one advance; space = digit width):
  small `f 0`: cap 9 px, 1.5 px strokes on pixel centres (1 bright px + soft sides), ink 5 x 9, advance 7, cell 13 px
  tall, oy -2. Large `f 1`: cap 13 px, 2 px strokes on pixel edges (crisp), ink 8 x 13, advance 11, cell 17, oy -2.
  Narrow glyphs `. : '` advance 3 (small) / 5 (large). Characters: `0-9 A-Z . : ' - + / % ( ) ? c`, **`@` (64) =
  the AOA alpha**, space. Lowercase other than `c` ("Vc") is not in the font: draw it as capitals; missing -> `?`.
  Draw text at whole pixels (D3D TL: subtract 0.5) or the hinted stems blur. Higher resolutions: scale by h/480 with
  bilinear (soft but fine); the atlas has room for a 1.6x size set if 1024x768 text needs to be sharper.
- **Sprites** (sizes outer, at 640x480): `white` (2x2 inside a 4x4 white block: sample its centre for ui_line /
  boxes / panels), `line2` (4x6 AA 2 px line profile 0, 1/4, 1, 1, 1/4, 0: map v across a 6 px wide quad for
  sub-pixel smooth lines, optional), `vv` (circle r5 + wings 7 + top tick 4), `vv_ghost`, `waterline` (W 28 px,
  anchor = centre peak), `td_box` 20 px / `td_box_dash`, `td_diamond` 24 px / `td_diamond_dash`, `hafu_arch` /
  `hafu_chev` 16x8 + `_dash` (anchor = contact; the frame is the upper half, its feet end on the anchor row; put
  `dot` on the anchor), `map_arch` / `map_chev` 8x4 + `_dash` (minimap, 1.2 px strokes; 8 px instead of 7 so they
  are symmetric about a pixel edge), `sel_tri` 9 px (points down, anchor = tip: place it 6 px above the symbol top),
  `ptr_tri` 7 px (bank pointer / launch-zone range pointer; rotate), `loc_arrow` (open V, points +x, anchor tip),
  `threat_arrow` 18 px dart (points +x, anchor tip; scale <= 1 for the farther ones), `offscreen_tri` 10 px
  (points +x, anchor centre), `breakaway_x` 60 px, `seeker` r26 (3 deg), `dot` 2x2, `dot4` steering dot, `raero`
  diamond 6 px, `aim_cross` 14 px (open centre), `caret` (heading caret, points up), `map_jet` 11 px (nose up),
  `map_frame` 24x24 9-slice (corners 8 px 1:1, edges stretched, centre empty; 1 px line + 2 px corner brackets).
  The launch-zone circle (r66), VISUAL 20 deg circle, ladder, tape, boxes and bank arc are lines (`white`/`line2`).
- **Previews** `tools/art/preview/hud_*.png`: `atlas_1x/3x` (dark + bright sky), `specimen_1x/3x` (font + symbols
  composited the engine way over dark/sky/sea/cloud/terrain: 50 % shadow at +1,+1, texture quantised to 4444,
  frame to 565 with dither), `readability_1x/3x` + `readability_over_1x` (every DESIGN 10.2-10.6 string on sky, sea,
  cloud-white and terrain bands), `mock_cockpit` (+ `_2x`, `_over`) and `mock_chase` (640x480 HUDs built only from
  this atlas + lines; backgrounds and the minimap island are placeholders).
- Not done / for others: no GDI fallback here (T07). Positions in the mocks follow DESIGN 10.2-10.6 literally
  (text y = cap top); T07's HUD_LAYOUT.md is authoritative for the final layout.

## A1 Jet: mesh, moving parts, LODs, liveries, flame (2026-11-13)

- **Generator** `tools/art/gen_jet.py` (`python3 tools/build_assets.py --only jet [--check]`, ~10 s; deterministic,
  `--check` = identical). Previews `tools/art/preview/jet_*.png`: `views_grey/aggr`, `turn_grey/aggr`, `lods`,
  `deflect` (stab +20, flaperons +-20, nozzle open, AB, gear down), `chase` (DESIGN 7 chase camera mock + enemy),
  `ground` (gear down, side), `parts`, `tex`.
- **Frame**: metres, +Z nose, +X right wing, +Y up, origin = CG. Nose tip z +8.12, nozzle exit z -7.15 (15.27 m),
  span 9.33 m over the tip rails, fin top y +3.05. Gear down: wheel bottoms at **y = -2.047** (CG 2.05 m above the
  runway: T06 ground roll / crash height). Lowest airframe point with the gear up: intake/belly y -1.32 (ventral fin
  tips -1.36); radius 8.12 m.
- **jet.msh** (class `jet`): LOD0 **878** tris (< 300 m), LOD1 **362** (< 900 m), LOD2 **120** (last; stop drawing
  at `acMeshDist`). **LOD0 has no stabilators, flaperons, nozzle or gear**: draw the parts at their tags (chase view
  and any LOD0 jet). LOD1/LOD2 are self-contained (fixed stabilators, fixed nozzle, wing without the flaperon notch,
  no pylons/ventrals on LOD1+2, no tip rails on LOD2); draw no parts with them. Materials: `skin` (tex `jet_grey`)
  and `canopy` (tex `jet_grey`, `MM_GLASS`; the dark tinted canopy is painted in the atlas). Vertex colours white:
  `mesh_draw_lit`. **Enemy livery = swap texture `jet_grey` -> `jet_aggr`** on every material of jet.msh AND of the
  four surface parts (racer car_0 -> car_n pattern); nozzle/gear (`jet_parts`) and flame are shared.
- **Tags** (all 17 of DESIGN 8.8 + 4): `cam_cockpit` (0, 0.98, 4.35) pilot eye under the canopy; `cam_chase`
  (0, 0.55, -0.60) = visual centre, chase pivot/look-at base; `nozzle` (0, 0.05, -7.15) dir -Z = nozzle exit centre
  (flame origin, glow billboard); `hinge_stab_l/r` (-+0.62, -0.08, -5.75); `hinge_flap_l/r` (-+2.10, -0.02, -1.42);
  `rail_0..7` = missile centreline points, dir +Z: **0/1 = left/right wingtip rails** (y -0.18, z -0.55; SRM),
  2/3 = outer pylons (x -+3.62), 4/5 = middle (-+2.80), 6/7 = inner (-+1.95) (y ~ -0.44; LRM); even = left, odd =
  right; `flare_l/r` (-+0.50, -0.72, -4.90) dir down/out/aft; extra: `hinge_nozzle` (0, 0.05, -6.20), `gear_n`
  (0, -1.25, 2.55) dir -Z, `gear_l/r` (-+0.62, -1.25, -0.85) dir +Z. Hinge tag dir = hinge axis (+X).
- **Parts** (class `jetpart`, origin = hinge, axis local X): `world = rotX(angle) * translate(tag) * body`.
  **Angle sign: positive = trailing edge UP** (assetkit `rotate_x` = D3DXMatrixRotationX: a point at -Z goes to
  +Y), same sign on both sides. So nose-up pitch = stabs positive; roll right = right flaperon positive (up), left
  negative. Map DESIGN 4.8 `FlightVisual` signs accordingly. `jet_stab_l/r` 20 tris, `jet_flap_l/r` 12 tris
  (nose 5 cm ahead of the hinge hides inside the wing's blunt trailing edge: +-20 deg shows no gap).
  `jet_nozzle` 48 tris: 12 petals, origin = hinge-ring centre (tag `hinge_nozzle`), petals toward -Z; **opening has
  no hinge axis: scale x and y about the origin by `1 + 0.18 * open`** (hinge ring stays inside the fuselage end up
  to 1.18). `jet_gear` 50 tris: one leg (strut + oleo + 8-sided wheel), drawn at `gear_n`, `gear_l`, `gear_r`;
  retract = rotate about X so the wheel swings toward the tag dir: angle = `(1 - gearPos) * 90` for gear_n (aft),
  `-(1 - gearPos) * 90` for the mains (forward); don't draw it at gearPos 0.
- **Flame** `jet_flame.msh` (16 tris, declared class `jetpart`): two nested 8-sided cones, origin = nozzle exit,
  pointing -Z: outer 4.5 m / r 0.40, core 1.7 m / r 0.27; material `flame` RB_ADD, cull none, tex `flame`
  (+ `flame_a`, premultiplied: RGB already fades to black). UV: u around (tiles: flicker = random u offset), v along
  (v 0 at the nozzle). Vertex colours fade to black at the tips: draw it unlit with its vertex colours (no
  `mesh_draw_lit`), no Z write, fog off. Previews scale x/y with the nozzle (1 + 0.18 open) and z by
  `0.3 + 0.7 * ab`. `glow_nozzle.bmp` 128x128 additive radial glow (black = nothing) for the billboard at `nozzle`.
- **Textures**: `jet_grey.bmp`, `jet_aggr.bmp` 256x256 (one UV layout: fuselage strip top 64 rows with both halves
  sharing texels, wings L/R plan views, fins L/R side views, stabs, flaperons, intake mouth, solid patches; table
  `LAYOUT`/`PATCH` in the generator); `jet_parts.bmp` 128x128; `flame.bmp`+`flame_a.bmp` 128x128;
  `glow_nozzle.bmp` 128x128. 16-bit VRAM with mips ~0.46 MB for all of them. Player: two-tone grey, low-vis invented roundel
  (ring + island-peak triangle) on the intake sides and the left wing, fin "07". Enemy: dark two-tone aggressor
  camouflage, red-ringed invented star insignia, red fin-tip band, fin "21".
- **Needs a validate.py change (T01's file, not edited by A1)**: `CLASS_PREFIXES` maps `jet*` -> `jet`, so the
  standalone validator and the **full-build final validation of data/** (`assetkit/build.py` ~line 460, no
  classes) check `jet_stab_l` etc. against the `jet` budget and fail (133 errors). `--only jet` builds pass (ASSETS
  declares `jetpart`). Fix: add `("jet_stab", "jetpart"), ("jet_flap", "jetpart"), ("jet_nozzle", "jetpart"),
  ("jet_gear", "jetpart"), ("jet_flame", "jetpart")` before `("jet", "jet")`. Until then:
  `validate.py --class jetpart data/meshes/jet_*_*.msh data/meshes/jet_gear.msh ...` passes (0 errors).

## A3 Ground, sea, foam, runway/apron textures (2026-11-13)

- Generator `tools/art/gen_ground.py` (assetkit protocol, ORDER 30): `python3 tools/build_assets.py --only ground`
  (~25 s; previews +15 s), `--check` identical; two runs md5-identical; `validate.py` 15 files, 0 errors, 0 warnings.
  The docstring at the top of the generator is the mapping reference (sizes, periods, UV rules).
- **Outputs** (`data/tex/`): `t_sand t_grass t_rock t_ash t_lava` (256x256 tileable), `sea` (256x256), `foam` +
  `foam_a` (256x64), `runway rwy_end rwy_thr taxi` (256x64), `apron` (128x128), `rwy_marks` (256x128 keyed atlas) +
  `rwy_marks.txt` (CRLF rect list). 16-bit VRAM with mips ~1.56 MB total. Seam ratio (edge step / neighbour step,
  racer threshold 1.6): all 0.13-0.97 except apron 1.48 (a slab joint sits on the wrap by design).
- **Texture periods for island.txt (m per repeat, all divide the 512 m chunk):** `t_sand 32`, `t_grass 64`,
  `t_rock 64`, `t_ash 64`, `t_lava 64`; sea 128 m. Planar chunk-local UVs u = x / P, v = z / P (baked bush shadows,
  crack lips and wave facets assume the 230/40 sun with v = +z north; mirrored v still looks fine).
- **Terrain textures are near-neutral**: average texel exactly grey (sand 216, grass 184, rock 168, ash 184), detail
  = luminance + small warm/cool hue shifts; the class tint carries the colour. Average screen colour on flat ground
  = tint x mean / 255 x bake light. **Suggested class tints** (flat sunlit ground light normalised to 1.0; if
  islandc's bake gives flat ground L, use tint / L): `sand 253 235 189`, `grass 116 169 75`, `rock 134 131 124`,
  `ash 164 155 147`, `lava 230 222 214`, shelf (t_sand under the 0.65 sea blend) `102 198 189`. **t_lava is NOT
  neutral** (dark basalt crust, mean 62 51 47, with orange glowing fissures): give it a near-white tint or the glow
  dies. A6 owns the final values. Keep macro variation in islandc's tint (vertex noise): the textures keep their
  tile-scale variation weak on purpose so the 64 m repeat doesn't grid from altitude.
- **Sea** (`sea.bmp`, T04): carries the sea colour (mean 29 72 109; vertex colour only darkens with distance). Swell
  = tileable wave trains travelling toward azimuth 240 (wind from ENE): scroll UV toward (-0.87, -0.50) in (u, v)
  at ~0.012 per s (~1.5 m/s). Baked facet light, sky reflection, rare glints, few small whitecaps.
- **Foam** (`foam.bmp` + `foam_a`, T03 batches / T04 draws): PREMULTIPLIED for RB_ADD (ONE/ONE: colour already x
  alpha, black where none; `foam_a` for RB_BLEND). u along the coast, 1 repeat / 64 m; v across the strip clamped,
  v = 0 landward edge (6 m inland of the 0 m contour), v = 1 seaward edge (34 m offshore) = a 40 m strip; waterline
  at v = 0.15. Alpha 0 at both v edges.
- **Runway ribbons** (all u = 0..1 across the full 45 m width, v = s / 50 m): from EACH runway end `rwy_thr` 0..50 m
  (piano keys 6-34.8 m, opaque so they don't vanish in the mips like keyed stripes; v = 0 at the end, flip v at the
  other end), `rwy_end` 50..100 m (no centre line), then `runway` (centre dashes 30 m paint at v 0.2-0.8, 20 m gap
  across the wrap, so every switch at whole v is seamless). Side stripes 0.9 m at 1.0-1.9 m from each edge.
  `taxi`: u across a **23 m** taxiway, v = s / 25 m; yellow centre line + double edge lines 1.5 m in, shoulders.
  `apron`: u = x / 32, v = z / 32 (4 m slabs). Ribbon textures carry their own colour (vertex colour = light).
- **rwy_marks** (keyed, ALPHATEST quads 0.05-0.1 m over the ribbon + zBias): `bar` (solid paint: TDZ bars 1.8 x
  22.5 m and aiming points 9 x 45 m by stretching), digits `d0..d9` (rect 3.6 x 9.6 m = glyph 3 x 9 m + margin),
  `hold` (yellow holding position, 23 x 3 m, dashed side = image top = runway). Image up = landing direction, image
  left = pilot's left. Placement suggestions (digits 46 m, aiming point 400 m, TDZ 150-900 m, hold 75 m from the
  centre line) and the rects are in `rwy_marks.txt` (`name x y w h world_w world_h`). The digits depend on the
  runway heading A6 picks (e.g. 233 deg -> "23" / "05").
- Previews (`tools/art/preview/`): `ground_sheet.png` (all + mips), `ground_tiled.png` (3x3 tiled + RGB565
  dithered), `ground_tinted.png` (terrain x suggested tints, shelf under sea, 565), mock renders with the kit
  renderer (point + mips, A2 fog/light, 565): `ground_airfield_{roll,thr,approach,600m}.png`,
  `ground_island_{15km,slopes,low}.png` (crude cone island with per-quad classes; not islandc).
- Deviation from TASKS: one extra output `rwy_thr.bmp` (opaque piano keys; the keyed version faded out beyond a few
  hundred metres in the mip preview), and `rwy_marks` is 256x128 instead of a full 256 (fits; half the VRAM).

## A7 Base buildings: hangar, tower, tank, radome (2026-11-13)

- Generator `tools/art/gen_base.py` (assetkit protocol, ORDER 45); run `python3 tools/build_assets.py --only base`
  (`--check`: identical, < 1 s; with previews ~7 s). Validate (class `bldg`, declared in `ASSETS`): 7 files, 0 errors,
  0 warnings. Note: `radome` has no `bldg` name prefix in `validate.CLASS_PREFIXES`; `ASSETS` declares it, but a bare
  `validate.py data/` warns "no budget class" for it unless run with `--class bldg` (harmless; T01 may add the prefix).
- **Meshes** (`data/meshes/`, names = the islandc placeholder instance names of TASKS T03: `hangar tower tank radome`).
  Origin = ground centre of the footprint, y = 0 = ground (place at the flattened terrain height; the apron ribbon is
  0.2 m above it, so a building on the apron just sinks 0.2 m into it, which is fine). The FRONT faces -Z (yaw 90 puts
  it toward -X). Two LODs each: LOD0 below 1200 m (x lodScale), LOD1 beyond (the base is drawn to 6 km).

  | mesh | tris LOD0/LOD1 | AABB at yaw 0 (x * z * height) | notes |
  |---|---|---|---|
  | `hangar` | 222 / 36 | 40 x 30 x 14.0 | arched roof (walls 8 m, crown 14 m), 32 x 10 m door on -Z; tag `door` (0, 0, -15) |
  | `tower` | 238 / 50 | 8 x 8 x 25.0 | 5 x 5 m concrete shaft to 20 m, dark frame, glass cab 21.2-24 m, roof slab, mast; tags `beacon` (2.6, 25, 2.6) for a blinking light, `door` (0, 0, -2.5) |
  | `tank` | 112 / 56 | 14 x 14 x 11.6 | r 7 m, wall 10 m, cone roof, ladder on -Z |
  | `radome` | 200 / 108 | 12 x 12 x 16.2 | 10 x 10 x 5 m concrete block, dome r 6 m (overhangs the block), door on -Z |

  These are the DESIGN 8.1 sizes (hangar 40 x 30 x 14, tower 8 x 8 x 25), so the islandc `BASE` collision boxes can
  use the table directly (swap x/z for yaw 90/270). Radome and tank sizes were free; take them from the table.
- Materials: `metal` (tex `base_metal`), `conc` (`base_concrete`), `atlas` (`base_atlas`); all RB_OPAQUE, culled,
  no key, no alpha. Vertex colours = tint x baked object light (`prelight(mode="object")`, toolkit key direction in
  mesh space, **A2's `sun`/`ambient`/`hemi` colours read from `data/sky/light.txt`**, soft wrap 0.9, ground AO).
  The engine/islandc may multiply a per-instance sun/shade on top as the racer does; nothing else is needed.
- **Textures** (`data/tex/`, 24-bit, opaque): `base_metal` 128x128 corrugated sheet, 4 m per repeat, neutral grey
  (vertex-tinted: hangar walls olive grey, roof mid grey); `base_concrete` 128x128, 8 m per repeat, neutral;
  `base_atlas` 256x128, cells carry their colour (hangar door, personnel door, clerestory window, cab glass, tower
  door/window, tank wall, tank roof, radome panels, plain white). VRAM (16-bit + mips) ~43 + 43 + 85 KB.
- Previews `tools/art/preview/base_*.png`: `base_{hangar,tower,tank,radome}_views.png`, `base_lods.png`,
  `base_tex.png`, and the base layout with these meshes on a flat mock airfield (A3's textures, A2's light/fog, kit
  renderer, 565): `base_scene_{low,apron,600m,top,far}.png`. The mock placement (`base_items()`: 4 hangars side by
  side on the apron's far edge, doors toward the apron, tower at the apron's end, 3 tanks behind, radome off to the
  side, two jets on the apron for scale) is only a suggestion; islandc/A6 own the real positions.
- From the air: the grey hangar roofs read well on grass and on the apron; the tower is a thin landmark; tanks and
  radome are white dots beyond ~2 km. Nothing here is taller than 25 m (DESIGN 8.1 climb-out rule unaffected).

## T03 Island compiler v1 + placeholder island (2026-11-13)

- **Run**: `python3 tools/islandc/islandc.py island/island.txt` (~15 s, + ~15 s previews), `--check` (builds twice
  into temp dirs: md5 run A == run B == data/, reader round trip, structural check, assetkit validate; exit 0),
  `--no-previews`; or `python3 tools/build_assets.py --only island [--check]` (hook works, `identical`). Python 3 +
  Pillow, assetkit only (container = `export.trk_bytes` with the tag swapped to RISL; base.trk = assetkit pools).
- **Outputs**: `data/island/jet.isl` (7.77 MB), `data/island/base.trk` (45 KB), `data/tex/map_island.bmp` + `_a`
  (128x128), `data/island/report.txt` (CRLF: chunks, tris per LOD, batches, heights, coast rule, runway, view triangle
  counts, chunk list), previews `tools/islandc/out/*.png`. Modules: `islandc` (source format SPEC + CLI + report),
  `height`, `classes`, `bake`, `chunks`, `isl`, `base`, `preview`.
- **Format reference = the docstring of `tools/islandc/isl.py`** (byte offsets of every chunk; `isl.decode()` is a
  Python reader T04 can mirror). Both files load with the engine's own C code: `world_load("island/base.trk",
  WL_NO_RENDER)` -> 63 cells, 134 batches, 4 meshes, 9 inst; `chunk_open(.., E_TAG('R','I','S','L'))` on jet.isl OK
  (CRC checked). Chunk order INFO HGHT HMAX CLAS MATL CHNK BTCH VERT INDX BASE MAPI. Facts T04 needs:
  - Grid 513 x 513, 32 m, x0 = z0 = -8192, h = -200 + u16 * 0.05. HGHT = the LOD0 mesh heights rounded (<= 0.025 m).
    Each quad is split SW-NE: triangles (SW, NW, NE) + (SW, NE, SE); bilinear `terrain_height` differs from the mesh
    by up to a few metres on steep non-planar quads (use the triangle rule if ground contact must match the mesh).
  - Coast rule holds per sample: below sea level <= -4 m, land >= +1 m (74 chunk-border samples changed by the LOD0
    snap lie in between, listed in report). Floor -60 m. Chunks not stored are deeper than -59.5 m everywhere.
  - 500 stored chunks; 127 all-underwater ones keep LOD2-4 only and their LOD0/LOD1 CHNK records **alias** LOD2
    (same vFirst/bFirst): don't assume disjoint ranges. Vertex blocks per chunk-LOD are contiguous, <= 65535.
  - **BTCH `pad` is now `skirtTris`** (deviation, layout unchanged): the last skirtTris triangles of each batch are
    skirts. Draw them only for chunks with a neighbour at a different LOD; at altitude this cuts LOD4 from 10 to 2
    tris per chunk. Skirts face outward, depth = max(4 m, 0.15 x edge, measured mismatch to LOD k+-1 + 1 m) (up to
    129 m on the volcano at LOD3/4). LOD0 has no skirts; its border is crack-free against LOD1 (snapped in HGHT).
    Adjacent chunks may differ by more than 1 LOD only with lodScale < ~0.8 (High): skirts were sized for +-1.
  - MATL (6): `t_sand t_grass t_rock t_ash t_lava` opaque; `foam` = RB_ADD | MF_CLAMPV 0x200 | MF_NOCULL 0x800
    (flags low byte = RB_* value). MATL tint is informational: **vertex colours are final** (class colour x baked
    light), don't multiply again. Terrain UVs chunk-local, 0..16. Foam strip y = 0.25 m (+ the beach rise on the
    landward row), v 0 / 0.15 / 1, u < 6.
  - Classes (CLAS, `terrain_class`): 0 water 1 sand 2 grass 3 rock 4 ash 5 lava 6 runway 7 apron.
  - Shallows: the shelf is turquoise (tint 102 198 189 on t_sand) fading to `deep` = the sea texture's mean colour
    by 40 m depth, so a sea blended (0.65) over every stored chunk and opaque elsewhere shows no seam at the
    stored/not-stored boundary (checked in `view_high.png`). That is the simplest shallows rule for T04.
  - BASE: runway centre (-2250, -3980), heading **270** (takeoff west, out to sea), 2800 x 45 m, plateau 12.0 m;
    threshold spawn (-890, 12.2, -3980) hdg 270 (40 m past the east end, y = ribbon top); air spawns 12 km N/E/S/W of
    the island centre (320, 512) at 2000 m heading inward; 9 boxes (A7 mesh AABBs at yaw 0 since the hangars face the
    runway: hangar 40 x 30 x 14, tower 8 x 8 x 25, tank 14 x 14 x 11.6, radome 12 x 12 x 16.2). Climb-out corridor
    (5 km past the end, +-600 -> 1500 m) max terrain 29 m; coast 270 m past the departure end; sea 610 m south of the
    centre line. Designators 27 (east end) / 09.
  - MAPI: `map_island`, rect x0 -6576 z0 -6368 x1 7248 z1 7456 (108 m/px), u = (x - x0)/w, v = (z1 - z)/h (top =
    north). Land opaque (relief, shading), coast line light, runway white, sea alpha 0.
- **base.trk** (for e_world, T08): INFO SKY (from light.txt) TEXL MATL MSHL CELL VPOL BTCH INDX INST, no COLT (the
  loader warns "no COLT"; collision = BASE boxes). Ribbons 0.2 m above the plateau, zBias 1, white vertex colour:
  runway (rwy_thr/rwy_end/runway per A3, both ends, 50 m segments), parallel taxiway at +200 m, 3 links, apron 300 x
  150 m; rwy_marks quads 0.27 m up, ALPHATEST zBias 2 (digits, TDZ bars, aiming points, hold lines; LOD0 only).
  LOD1 = ribbons only. INST: `hangar` x4, `tower`, `tank` x3, `radome` (A7 names), drawDist 6000 m, yaw from
  island.txt. If a mesh is missing islandc adds a placeholder box batch (not the case now).
- **Triangle counts** (report `[views]`, engine LOD rule, 70 deg, far 18 km, frustum + LOD): 50 m over the runway
  max 10.9k tris (6.4k without skirts), on the threshold 10.5k (6.0k), 1500 m over the crater 3.2k (1.1k), 8000 m
  over the sea 3.5k (0.7k). With lodScale 0.7: 7.9k (4.6k) low, 1.8k (0.5k) crater. **Budget risk for T04/T09:**
  DESIGN 9.3 Low says terrain typ. 1.5-3k, max 4k. Near the ground the whole island is in view (250+ chunks at
  LOD3/4) and 512 m chunks make LOD0 cost 5-6 chunks x 512 tris even though the 0.9 km ring is small. Levers: draw
  skirts only at LOD transitions (skirtTris), lodScale ~0.6-0.7 on Low, distance to chunk centre instead of the
  AABB, a shorter LOD3/4 far range in fog. ENGINE 2.2's 4k estimate ignored chunk granularity and skirts.
- **For A6 (island.txt)**: every key is documented in the file; class tints use A3's convention (tint x texture
  mean / 255 = flat sunlit colour; vertex colour = smooth mean of adjacent quads' colour targets, so borders are soft
  while the texture switches per quad); macro variation (forest/dry/coast grass, crater rock, red ash) comes from the
  `tint` variants. Look notes from the previews: green lowlands and slopes up to ~850 m, bare summit cone (rock/ash)
  and crater read well from 15 km and the runway; A3's `t_rock` shows a visible repeating motif at 2-3 km
  (`view_crater.png`); beaches are thin (1-2 quads); lava channels are only a dark vertex-colour smear at LOD >= 1
  (by design, no 128 m lava blocks). The plateau blend zone is forced to grass (no grey embankment).
  Compiler changes A6 may want: tell the T03 owner/orchestrator.
- **Deviations**: (1) LOD border snapping only LOD0 -> LOD1 (in the height grid itself); LOD >= 1 use measured-depth
  skirts instead of snapping, because snapping LOD k to k+1 opens uncovered cracks against the skirt-less LOD0 and
  can't be consistent for all LOD pairs. (2) BTCH pad = skirtTris. (3) LOD0/1 aliasing of underwater chunks (file
  size: 7.77 MB < 8 MB; saves ~1.7 MB). (4) INFO `lodEdge[5]` = grid edge per LOD in metres (32..512). (5) MATL
  flag bits defined here (MF_CLAMPU 0x100, MF_CLAMPV 0x200, MF_ZBIAS1 0x400, MF_NOCULL 0x800). (6) Previews: 7 views
  (+ minimap) instead of 3. (7) Summit (rim max) 1808 m, mean rim 1750 m; island 9.5 x 11.6 km.

## T05 Flight model, keyboard shaping, feel tests (2026-11-13)

- **Files** (all portable, replaced the T01 stubs): `src/flight.h/.cpp`, `src/pilot.h/.cpp`, `src/tests_flight.cpp`,
  `src/tests_pilot.cpp`, `docs/FLIGHT_TUNING.md` (plain-words key guide for the user). `simh feel` was already
  dispatched by T01 (no simh.cpp edit). `simh test flight|pilot`, `jet.exe -test flight|pilot` exit 0.
- **Model** = DESIGN 4.3, with every 4.6 key in `[flight]` (+ `vManBandKt ceilDrag abOnSec abOffSec brakeSec`) and every
  5.2 key in `[pilot]` (+ `rollFine rollRateFine flareRepeatMs` and the Novice keys). Rotation is one rotation vector
  per tick in the path frame (nose-up = negative angle about +X, gravity side pull = + about +Y, roll right = - about
  +Z), renormalised every tick. `nHold = up.y` of qPath, bank = atan2(-right.y, up.y), heading = atan2(f.x, f.z)
  (-pi..pi; HUD adds 360).
- **FlightState additions** (end of struct): `nLim nHold Vt vs turnRate pitchRate capped` (bits: 1 pitch-rate cap,
  2 floor/ceiling nose drop, 4 at the speed floor). `pitch` = NOSE pitch (gamma + alpha in the vertical), `gamma` =
  path. FlightVisual signs: every surface + = trailing edge UP; pull = both stabs up; roll right = right flaperon up /
  left down, right stab more up. `nozzle = ab`; `vapour` 0..1 for n > 6 below ~650 kt.
- **API for others**: `flight_set_path(fs, hdg, gamma, bank)` (also sets n = nHold) and `flight_path_quat` (T06
  rotate/air spawn, T14 tests), `flight_g_limit(V)`, `flight_roll_rate_max(V)`, `flight_neg_g_limit()`,
  `flight_tau_g()`, `flight_speed_floor()`, `flight_speed_target(thr, h)` (HUD/T13/T14), `flight_reset_peak` (HUD
  peak G timeout), `flight_derive` (after editing qPath/V by hand). **onGround = 1**: flight_step does not move the
  jet (T06 owns pos/V/qPath on the ground) but updates ab/brake/surfaces/derived; after liftoff clear onGround and
  set V >= 150 kt (the step clamps V to the floor).
- **Pilot**: `pilot_update(ps, keys, out)` = Expert. `pilot_update_fs(ps, keys, &ac->fs, out)` = mode-aware (Expert or
  Novice): **main/T08 should call pilot_update_fs for the player** so Novice works. Edges: fire/wpnNext/view = press
  pulses; `flare` = 1 on the press tick and every 500 ms held (one burst per pulse, T12); `look` = held.
  `pilot_seed_axes(ps, up, ur)` for the T06 handover (in Novice the axes are intents: seed 0).
  `pilot_axis_step(...)` is the generic ramp (T06 override can reuse it).
- **Short/long classifier** (DESIGN 5.3, for T11): `PressClass` + `press_classify(pc, heldTicks, releasedTicks,
  longTicks)` -> PRESS_NONE/SHORT/LONG, fed with `in_action_held_ticks` / `in_action_released`;
  `pilot_long_press_ticks()` = 36. Scripts/replays without e_input: `press_counter_feed(c, down, &held, &released)`.
  PilotKeys.tgtHeld/tgtReleased carry the S counters; pilot_update already turns them into tgtShort/tgtLong.
- **Novice mode** (user scope addition 2026-11-13): `[pilot] mode = expert|novice` (default expert, P_STR, F5
  re-applies the ini). Left/Right = bank to `noviceBankDeg` 70 with a level turn (n = cos(gamma)/cos(bank) within the
  G limit), release = wings level (0.94 s from 70 deg); Up/Down = flight-path angle rate (`noviceGammaRateDeg` 15)
  into a held reference limited to +-75 deg (no loops, no inverted); released = hold the climb angle
  (`noviceAutoLevelPitch` 1 = ease to level); Up + Left = tighter climbing turn (6 -> 21 deg/s at 500 kt). A lead term
  cancels the model's tauG so the nose stops where the key leaves it (< 0.3 deg bump). It only produces
  PilotInput.up/ur: flight physics untouched. **F6 hookup for T08** (main.cpp, next to F5):
  `if (in_key_hit(VK_F6)) { int m = pilot_toggle_mode(); hud_message(pilot_mode_name(m), 2.0f); if (replay_active() == 1) replay_record_params(g_game.tick); }`
  (the mode is a param, so replays need the PARAMS record as after F5).
- **-telemetry** writer for T08: `flight_telemetry_open(path)` at flight start, `flight_telemetry_row(g_game.tick,
  &keys, &pl->in, &pl->fs)` after flight_step, `flight_telemetry_close()` at exit (CSV, CRLF, header line; columns
  tick..capped). game.cpp's s_notYet row for -telemetry stays T08's.
- **Key bindings** (T08): main's read_live_keys still uses in_key; S needs an action (`in_action_define`) so
  `in_action_held_ticks/in_action_released` fill PilotKeys.tgtHeld/tgtReleased. Joystick: fill joyValid/joyX/joyY
  (DirectInput Y + = stick back = nose up).
- **jet.ini** has no [flight]/[pilot] yet: the integrator regenerates it with -dumpini (66+ keys).
- **Tests**: -test flight/pilot run against the CODE defaults (snapshot, empty ini, restore), so jet.ini edits never
  fail them; `simh feel [-ini FILE]` measures the card with the loaded values and prints the % vs target.
  T10: when the user accepts new numbers, change the defaults in flight.cpp/pilot.cpp and the targets in s_card.
- **Deviations from DESIGN 4** (also in flight.cpp's header): `decBrake` 28 (DESIGN 15 gives FAST -> 350 kt in 24 s,
  the card says 13); `pitchRateCapDeg` 40 (30 clipped the card's own 150 kt line and kept turnBoost from scaling
  low-speed turns); SLOW brake fades over `vManBandKt`; ceilingFade scales only positive thrust; new `ceilDrag` + nose
  drop above ceilHardM (an M1.6 zoom otherwise coasts ~11 km past 15 km; now tops out at 15.8 km, settles below 15 km);
  floor nose drop applied along the pilot's "down" (works inverted); one rotation vector per tick. Card line
  "30 deg climb loses ~2 kt/s" is now defined as the 10 s average = 2.9 kt/s (the Mach target falls with altitude);
  the tap line is a range check 0.3-0.6 deg (measured 0.38).
- **Measured** (VM = Mac, identical): turns 19.14 / 27.90 / 31.90 / 7.41 deg/s (510 kt 9 g / 350 kt / 150 kt / M1.6
  7 g), roll 90 deg 0.56 s, 360 1.83 s, full pitch 0.48 s, tap 0.38 deg, NORMAL->FAST 13.9 s, FAST->350 13.3 s,
  350->155 kt 40.9 s; turnBoost=1.2 -> all four turn lines +20.2..20.6 %. Novice: 70.0 deg bank, 0.0 m altitude
  change over the 360 (60.8 s), release to level 0.94 s, climb/dive capped at exactly +-75. flight_step 12.1 us on
  the VM (`-test flight` timing line; 16.4 before removing trig from the hot path).

## T02 Depth/fog/HUD-line probe and decision (2026-11-13)

- **Decision (probe/RESULTS.md):** Plan A, single pass, **W-buffer**; near 1.0 m cockpit / 1.5 m chase; far 18 km
  Low / 28 km High; linear W table fog **3.6-16.2 km Low, 5.6-25.2 km High (fogEnd = 0.9 x far)**: the Voodoo fog
  table only reaches ~93-96 % at fogEnd, which leaves a visible line where the sea meets the panorama's fog band;
  with the trim it reaches the 565 fog colour and the join is seamless. HUD: 2 px strokes (1 px for ticks/box
  outlines), black shadow (+1,+1) 50 % (70 % optional for yellow), HUD pass **RB_PREMUL**. Low resolution
  **800x600x16**, pacing divisor 4 (30 fps) realistic, fallback 6, gate 8 (15 fps).
- **Engine change (orchestrator-approved):** `RB_PREMUL` appended to the blend enum in `engine/rend/e_rend.h`
  (src ONE, dest INVSRCALPHA, case in `blend_mode()` of `r_d3d6.cpp`); `ui_set_blend` accepts RB_BLEND / RB_ADD /
  RB_PREMUL (`e_ui.cpp/.h`). r_null needed no change (it ignores blend). Blend check on the Voodoo3 exact.
  ENGINE_COPY.md's edit table does not list it yet (T01's file: orchestrator please add a row).
- **HUD atlas loading bug to avoid (T07):** `tex_get("hud_atlas", IMG_ALPHA, ..)` breaks the premultiplied atlas
  (`img_load` IMG_ALPHA runs `img_bleed`: alpha-0 texels become white, glyph cells render as boxes). Load RGB and
  `_a` with flags 0, alpha = red of `_a`, `flags = IMG_ALPHA`, `tex_add(...)` (probe.cpp `atlas_load`).
- **HUD cost:** ~7-8 us CPU per HUD quad on the VM (ui_line or atlas glyph, any blend); a cockpit-sized HUD with the
  shadow pass (304 quads, 28 draws) = 1.6-2.2 ms, over the 1.5 ms budget. Levers: lines from the atlas `white`
  texel (one texture, one draw per pass; needs a textured ui_line in e_ui), a pre-shadowed premultiplied atlas
  (one quad per glyph; A4), fewer shadowed lines.
- **Other rules:** runway ribbons 0.2 m + **zBias 2** (1 fails at 4 km on the glide path); separate surfaces >= 2 m
  apart are safe with W to 17 km (0.5 m fails from 5 km; 16-bit Z fails 2-5 m from 1 km); coastline under W = at most
  a 1-px shoreline offset, no shimmer; **keep |uv| per vertex below ~16-32** (large uv values make the emulated
  Voodoo choose coarser mips: blurry from 32, flat at 2048) = chunk-relative terrain UVs, camera-relative sea UVs.
- **Aircraft:** A1 LOD2 jet at 640x480 = 10 / 3 / 2 / 1 px at 1 / 2.5 / 4 / 6 km broadside, tail-on 0 px from
  2.5 km. Keep acMeshDist 2.5 km / acDotDist 4 km, but draw the 2x2 dot from 1.5 km (under the mesh, alpha ~0.7
  fading out to 4 km) so tail-on jets never vanish.
- **Budgets (emulator):** Low test frame (5.4k tris, 63 draws, sky + terrain + sea + 16 puffs + 2 jets + HUD) =
  14.7 ms at 640x480 and 14.8 ms at 800x600 fullscreen, CPU-bound. Full-screen layer fill ~1.7 ms at 640, ~4 ms at
  800; fog 0-15 %; blending +0.2-0.4 ms per layer (ADD = BLEND = PREMUL); `r_clear_z` 0.12-0.21 ms. Texture memory
  after front+back+Z: 12.8 MB at 640x480, 11.3 MB at 800x600.
- `world_sky_load(w, 4, "light")` + `world_draw_sky` work on screen (first caller). The probe: `probe/probe.exe
  -scene 1..8|all [-wbuf 0|1] [-shot] [-fs] [-res 640|800|1024]`, tables via `python3 probe/summarize.py`.

## A4b HUD atlas sets per resolution, contact diamond, centred box numbers (2026-11-13)

User follow-up: 800x600 minimum (640x480 fallback), real target ThinkPad T40 at 1400x1050; HAFU only on the
minimap; 3D-view contacts are hollow diamonds; numbers centred in the speed/altitude boxes. Same generator
(`tools/art/gen_hud.py`), deterministic (two full runs md5-identical incl. previews, `--check` identical), validator
0 errors / 0 warnings on all 14 BMPs.
- **Sets** (draw 1:1, never scale the atlas; pick the set = screen height, else the next smaller one; place at
  virtual 640x480 coordinates x h/480):

  | set | mode | pages (256x256 + `_a`) | small cap / adv | large cap / adv | stroke (symbols, ui_line) |
  |---|---|---|---|---|---|
  | 480 | 640x480 | `hud_atlas_480` | 9 / 7 | 13 / 11 | 2 |
  | 600 | 800x600 | `hud_atlas_600` | 11 / 9 | 16 / 14 | 2 |
  | 768 | 1024x768 | `hud_atlas_768`, `hud_atlas_768_p1` | 14 / 11 | 21 / 17 | 3 |
  | 1050 | 1400x1050 | `hud_atlas_1050`, `hud_atlas_1050_p1` | 20 / 15 | 28 / 23 | 4 |

  Metrics file `hud_atlas_<L>.txt`. Pages stay 256x256 everywhere (DESIGN 8.8 + the validator's 256 limit; also fine
  on the Radeon): 768/1050 use **two pages** instead of one 512 texture. Each font is entirely on one page (page 0
  in every set), each page has its own `white` + `line`; symbols fill page 0 first, the rest go to page 1 (e.g.
  at 1050 `tgt_box`, the big circles/X, arrows and `map_frame` are on page 1): the HUD then needs at most two
  texture batches. VRAM (4444, no mips): 128 KB (480, 600), 256 KB (768, 1050). Only the active set is loaded.
- **Legacy names**: `hud_atlas.bmp/_a.bmp/.txt` = exact copy of the 480 set (txt identical except
  `page 0 hud_atlas`). New code should load `hud_atlas_<L>`.
- **txt format changes vs A4** (read the file header): new records `set <lines> <pages> <pageW> <pageH> <scale1000>
  <stroke> <mapStroke10> <frameLine>` and `page <n> <texture>`; `font` gained two trailing fields `<baseline>
  <page>` (baseline = cap = baseline offset below the cap top); sprite lines gained a trailing `<page>`; `white`/
  `line` appear once per page. The A4 sprite `line2` is now **`line`** (profile 0, 1/4, 1 x stroke, 1/4, 0; centre
  = h/2). Removed: the 16 px `hafu_*` sprites and `td_box*` / `td_diamond*` (replaced below). `map_frame` corner =
  rect w / 3 (8, 10, 13, 18 px).
- **New 3D contact sprites** (all sets): `tgt_diamond` hollow diamond, corner down, outline only (10 px at 480, 12
  at 600, 21 at 1050; tint red hostile / yellow unknown), `tgt_diamond_dash` (memory: four corner pieces),
  `tgt_box` TD square enclosing it (20 px outer at 480, 24 at 600, 44 at 1050), `tgt_box_dash`. Anchors = the contact
  point; `sel_tri` sits 6 px (x scale) above the diamond top. Minimap keeps `map_arch` / `map_chev` (+ `_dash`).
- **Centring numbers in boxes**: text ink width W = sum(adv) - gap, ink height = cap (a capital covers rows
  [capTop, capTop + cap) exactly, faint 1 px soft edge around). For a box interior [x0, x1) x [y0, y1):
  X = x0 + (x1 - x0 - W) / 2, Y = y0 + (y1 - y0 - cap) / 2 (integers). The mocks draw the box stroke inside the
  box rectangle and centre in the remaining interior (`Frame.box` + `Frame.text_in_box` in the generator).
- **Shadow**: the previews offset the dark shadow by 1 px at 480/600 and 2 px at 768/1050 (1 px looks too thin at
  1050); suggest T07 does the same (`max(1, round(h / 480))`).
- **Previews** `tools/art/preview/`: `hud_atlas_<L>.png` (pages on dark / sky), `hud_readability_<L>.png` and
  `_over.png` (every DESIGN 10.2-10.6 string, additive vs premultiplied over, per set), `hud_mock_cockpit_480.png`,
  `hud_mock_cockpit_600.png` (+ `_zoom` 2x, `_over`), `hud_mock_chase_600.png`, `hud_mock_cockpit_1050_over.png`
  (+ `_crop` 1:1 centre), `hud_mock_chase_1050.png`. The A4 preview names (`hud_mock_cockpit.png` etc.) are gone.
  The additive-vs-over finding of A4 holds at every set.

## A4c Baked drop shadow: one quad per glyph/sprite (2026-11-13)

Reason: T02 measured ~7-8 us CPU per HUD quad on the VM; a separate shadow pass doubled the quad count. HUD blend is
RB_PREMUL (ONE, INVSRCALPHA), confirmed by T02 on the Voodoo3.
- **Every glyph and sprite cell in every set now has the shadow baked in**: white shape (RGB = alpha = coverage)
  over a black shadow offset (+o, +o), **o = 1 px at 480/600 and 2 px at 768/1050**, 60 % alpha (RGB 0, alpha
  only; alpha = c + 0.6 s (1 - c)). Draw each glyph/sprite as ONE quad with RB_PREMUL and the tint as vertex colour:
  the shape takes the tint and the shadow stays black. Vertex alpha fades shape + shadow (scale vertex RGB by the
  same alpha). **No shadow pass.** There are no plain (unshadowed) glyph variants any more.
- **No shadow** on `white` (solid 4x4, panels/boxes/minimap background) and `line` (plain stroke profile). New
  **`line_sh`**: the stroke profile with its shadow o rows below (4 wide, h = stroke + 4 + o, record oy = the
  stroke centre row). One textured quad draws a line with its shadow: put the stroke centre (oy) on the line and
  point +v screen-down for horizontal lines, screen-right for vertical lines, toward the down-right side for
  diagonals (exact for H/V lines, close enough for the rest).
- **Metrics**: cells and sprite rects grow by o to the right and bottom only, so every `ox`, anchor (ox, oy), `adv`,
  `cap`, `inkW` and the centring recipe are unchanged and describe the SHAPE (layout ignores the shadow). `font`
  `cellH` now includes o. `set` record gained `<shadowOff> <shadowA100>`. `map_frame`: corner = (w - o) / 3, the
  right/bottom slices are corner + o wide (they carry the shadow). Rotated sprites rotate their shadow too (fine
  for the small arrows/pointers).
- **Pages**: fonts are now packed as one block each (denser). 480 and 600: 1 page; 768: 2 pages (both fonts on page
  0); **1050: 3 pages** (`hud_atlas_1050`, `_p1`, `_p2`: small font on page 0, large font on page 1, symbols spread
  over the free space; the `page` field of every record says where). VRAM (4444): 128 / 128 / 256 / 384 KB.
- **Loading**: keep the two-file format; per T02, load `hud_atlas_<L>.bmp` and `_a.bmp` with image flags 0 and
  combine them (`IMG_ALPHA` fills transparent texels with neighbour colour and breaks premultiplied texels; see
  `atlas_load` in `probe/probe.cpp`). The probe's parser keeps working: all new fields are trailing.
- Checks: `--check` identical, two full runs md5-identical (incl. previews), validator 16 BMPs 0 errors /
  0 warnings, `hud_atlas.*` = 480 set. Every texel verified premultiplied (RGB <= alpha; shadow-only texels RGB 0,
  alpha <= 153).
- **Previews** (all RB_PREMUL now; the additive variants are gone): `hud_atlas_<L>.png`, `hud_readability_<L>.png`
  (now readable on cloud-white too), `hud_mock_cockpit_480.png`, `hud_mock_cockpit_600.png` (+ `_zoom`),
  `hud_mock_chase_600.png`, `hud_mock_cockpit_1050.png` (+ `_crop`), `hud_mock_chase_1050.png`.

## T04 (early note) portable terrain queries ready (2026-11-13)

- `engine/world/e_terrain.cpp` now loads `data/island/jet.isl` for real (headless, simh, jet.exe): `terrain_height`
  (LOD0 mesh rule: SW-NE diagonal, matches the rendered mesh within 0.025 m; bilinear would be off by up to 5.2 m),
  `terrain_surface`, `terrain_class` (TCLASS_*), `terrain_los`, `terrain_segment_hit` (mesh **or sea plane**, first
  contact), `terrain_base` (real runway: centre -2250,-3980, heading 270, 12 m; threshold spawn -890,12.2,-3980),
  plus `terrain_normal`, `terrain_block_max`, `terrain_area_max` (HMAX max over a rect: PULL UP / AI look-ahead).
  `simh test terrain` passes (1000 rays vs the exact reference: 0 mismatches). T06/T11/T12 can use it now; the render
  part follows in the full T04 section.

## T06 Takeoff/climb-out, ground roll, crash, bounds, PULL UP (2026-11-13)

- **Files** (portable, replaced the T01 stubs): `src/takeoff.h/.cpp`, `src/world.h/.cpp`, `src/tests_takeoff.cpp`,
  `src/tests_world.cpp`, script `tests/t06/takeoff.scr` (CRLF). `-test takeoff` (69 checks) and `-test world` (34)
  exit 0 on VM A and in simh, **on the real island** (T04's loader landed during T06) and on the T01 stub. Header
  comments of takeoff.h / world.h are the API reference. New params: `[takeoff]` 26 keys, `[world]` 12 keys
  (integrator: regenerate jet.ini).
- **Takeoff** (DESIGN 6.1): READY (parked at the BASE threshold, CG `gearHeightM` 2.05 above the ribbon = y 14.25 on
  the island; "A: TAKEOFF") -> ROLL on A (7 / 11 m/s^2 with A held) -> ROTATE at 145 kt -> liftoff at 160 kt +
  rotation complete -> CLIMB (ap_steer: wings level on the extended centre line, path 12 deg, gear up from 50 ft in
  3 s) -> HANDOVER at 1500 ft AGL + 300 kt or on override ("YOU HAVE CONTROL" 2 s) -> DONE (takeoff_tick returns 0).
  Measured: tap A: liftoff 12.05 s after A, roll 518 m (2760 m runway left), handover 30.8 s; A held: 8.3 s / 378 m /
  20.5 s; Novice same as Expert; centre-line error 0.00 m (20 m displacement recovers to < 2 m in 6 s, 0.6 m
  overshoot); min clearance after liftoff 2.29 m (crash at 1.5); override (key held > 0.25 s) hands over exactly at
  200 ft AGL, a 0.1 s tap does nothing. On the real island the climb-out track clears the terrain by >= 7.7 m right
  after liftoff and climbs out over the sea (checked offline against jet.isl).
- **Main wiring for T08** (main.cpp is not T06's; current main already calls takeoff_reset/takeoff_tick/world_check,
  so it parks and logs READY, but nothing more):
  1. `start_flight`: `world_reset(&g_game);` then `takeoff_bind_pilot(&s_takeoff, &s_pilot[PLAYER_ID]);` before
     `takeoff_reset` (bind = override reads the KEY axes, needed in Novice; the handover resets Novice's climb-angle
     reference so it holds the climb). Runway start: keep `fs.onGround = 1` before takeoff_reset (it calls
     takeoff_park, which places the jet itself); air start: `world_air_start(&g_game, pl, from)` (nearest spawn,
     NORMAL speed, wings level, heading to the island, gear up).
  2. Tick step 1: `pilot_update_fs` (T05). Step 4: `if (takeoff_tick(...)) in = own;` then
     `world_override_input(&g_game, PLAYER_ID, &in);` (area turn-back; works for any aircraft id).
  3. Crash flow (M1, DESIGN 6.3): world_check clears AC_ALIVE, hp = 0, wreck on the surface, event CRASH_*. Wreck
     camera while `!world_respawn_due(&g_game, 0)`; then `pilot_reset(&s_pilot[0]); takeoff_park(&s_takeoff, pl,
     &g_game);` (also the pause menu's "restart on runway"). No more air respawns (user rule).
  4. HUD (T07/T08): `takeoff_message(&s_takeoff, &g_game)` (NULL = none); `world_status(0)`: `pullUp` (flash PULL UP
     + tone loop), `areaWarn` ("RETURN TO AREA" + minimap arrow towards `brgCentre`, valid within 2 km of the warn
     ring and beyond), `turnback`, `distCentre`, `agl`. Sounds from the event ring (keep your own read index, see
     world.h): TO_ROLL (roll rumble starts; volume by V while takeoff_active and fs.onGround), GEAR_UP (gear_up.wav),
     GEAR_LOCKED (gear_clunk), HANDOVER (beep), CRASH_* (expl_near), RETURN_AREA (beep). READY = engine idle.
  5. `-script` (DESIGN 16.2): `tests/t06/takeoff.scr` uses `T keydown|keyup KEY` (KEY = UP DOWN LEFT RIGHT A Z SPACE D
     F C V) and `T expect FIELD OP VALUE` with FIELD = phase (READY..DONE) kt agl_ft alt_ft gear lat_m onground alive
     pullup gamma pitch. tests_takeoff.cpp has a small static runner (`t_script`) that T08 may copy. Until -script
     exists, `-test takeoff [file.scr]` runs a script and logs the full phase timeline; the acceptance command
     `-headless -start runway -script tests\t06\takeoff.scr -exitafter 40 -log` today logs `args: not yet -script` and
     READY only.
- **World** (DESIGN 6.2-6.4): crash = clearance < 1.5 m (GROUND / SEA by the terrain under it), sphere 5 m vs BASE
  boxes (union-box quick reject), aircraft vs aircraft swept spheres 6 m over the tick (no tunnelling; both crash,
  `other` = the partner). Area centre = **mean of the 4 air spawns** (= the INFO island centre 320, 512 on the real
  file; the DESIGN API has no centre accessor). Warning >= 26 km (off < 25.5), turn-back >= 32 km while not heading in:
  G-limited level turn (bank <= 75 deg) towards the centre, A ignored (NORMAL), player keys ignored; when the heading
  is within 30 deg it rolls wings level and hands back (bank < 10). Max distance reached: 34.1 km at M0.8, 37.0 km
  from a 950 kt run (coordinates stay within ~+-38 km; outside the height grid = sea). PULL UP (player, 2 Hz):
  straight-line prediction sampled every 0.5 s to 5 s, interpolated; on <= 4 s, off > 4.5 s; suppressed during
  takeoff phases (until HANDOVER) and below 200 ft over the runway (+150 m). Measured: 30 deg dive at the sea warns
  3.86 s before impact; level into the volcano warns 3.67 s before impact; level 100 ft over the sea is quiet.
- **Costs (VM A)**: world_check 2.8 us/tick for 1 aircraft, 14.4 (6 over the sea), 18.0 (6 over the island);
  world_crash_test 1.6 us, terrain_height 0.86 us, world_pullup_predict 15 us (2 Hz, player only).
- **Generic autopilot** `ap_steer(fs, hdg, gamma, maxBank, out)` (takeoff.h): heading + flight-path angle through
  the flight model's own up -> nCmd mapping; used by the climb-out and the turn-back, usable by bench routes.
- **Deviations**: rotatePitchDeg = NOSE attitude (path = nose - the model's 1 g alpha read from `[flight]` by name),
  so the flight model takes over with no nose jump; liftoff needs vLiftKt AND the full rotateSec. HANDOVER blends
  autopilot -> player axes over 0.5 s (an axis the player moves is his at once) instead of pilot_seed_axes. Climb
  tracks the extended centre line (`climbHdgGain` 8, `trackGainDeg` 0.15). PULL UP samples every 0.5 s (not 1 s).
  Turn-back ends wings level and does not re-trigger while heading in. Crash ignores AC_INVULN (terrain always
  kills); world sets hp 0 / takeoff_park sets hp 100 (mission may override).
- **Requests**: T04/orchestrator: `locate()` in e_terrain.cpp uses `(int)fx` (VC6 `_ftol`, slow on the VM) -
  `e_ftoi`-style floor would help every height query; a portable island-centre accessor in the 8.6 API (T06 uses
  the air-spawn mean). T05 (optional): `flight_alpha_1g(V)` so takeoff need not read `[flight]` keys by name.
- **Side effect on `-test replay` (T01/T08's file)**: its `repeat_compression` check (`bytes < 4500` for 1000 ticks)
  counts the two params snapshots (header + the PARAMS record), which grow with every module's params; with T06's
  38 keys the snapshot is 2.47 KB and the file 5854 B, so the check FAILs (inputs/params/hashes all still match).
  Fix in replay.cpp: compare the record bytes without the snapshots (e.g. `bytes - 2 * snapBytes`). Not edited by T06.

## T07 Flight HUD + minimap (2026-11-13)

- **Files** (replaced the T01 stubs): `src/hud.h/.cpp` (HudState, cockpit + chase HUD, number caches, messages,
  `-hudtest`), `src/hud_font.h/.cpp` (HUD graphics layer `hg_*`: atlas sets, text, sprites, textured lines, keep-out
  boxes, GDI fallback), `src/hud_map.h/.cpp` (minimap + contact queue for T15), `docs/HUD_LAYOUT.md` (layout as
  built, authoritative), screenshots `shots/*.png`, test logs `shots/logs/`. Approved edits outside: `engine/plat/e_ui.h/.cpp`
  append (`ui_quad4_px`, `ui_line_tex`, `ui_quad_count`; row in ENGINE_COPY.md); `src/main.cpp` (-hudtest dispatch after
  init, before APP_LOADING, quits without the loop; `plat_set_no_dialogs` also for -hudtest); `src/game.cpp` (removed
  the `-hudtest` row of s_notYet).
- **Atlas** (hfont_init, after r_init): set = largest of 480/600/768/1050 <= screen height (`[hud] atlasSet`
  overrides, restart), pages loaded colour + `_a` with flags 0 and combined (DESIGN 9.1b), `tex_add("<page>_premul")`.
  Uses the **A4c baked shadows** only (no shadow pass anywhere): glyphs/sprites one quad, lines one `line_sh` quad,
  `white` for solid rects/triangles. Everything is drawn with RB_PREMUL; **colours passed to hg_* must be
  premultiplied: use `hg_col(argb, alpha)`**. Without atlas files: GDI fallback (Courier New, untextured lines, boxes
  for sprites, RB_BLEND), logged `hud: atlas missing, GDI fallback`.
- **hg_* layer (for T15/T16)**: screen pixels; layout in virtual 640x480 units via `hg_px(v)` (k = h/480). Frame:
  `hg_begin()` .. calls .. `hg_end()`; calls are recorded and emitted sorted by atlas page (1 draw per page; the
  1050 set has 3 pages, 768 two). `hg_text(x, y=cap top, font, col, align 0/1/2, s)` (whole px, monospaced digits,
  lowercase drawn as capitals except `c`, `@` = alpha glyph), `hg_text_w/cap/adv/gap`, `hg_sprite(id, ax, ay, col)`
  (anchor rounded), `hg_sprite_rot` (+ = clockwise), `hg_sprite_scaled`, `hg_sprite_id("tgt_diamond")` (once at
  init), `hg_line` (set stroke, ends extended by stroke/2), `hg_line_w` (thinner profile), `hg_line_ext`, `hg_box`
  (crisp outline inside the rect), `hg_rect`, `hg_tri`, `hg_nine` (map_frame), `hg_clip(px box)` (lines: centre
  line clipped with the profile margin; text/sprites cut with uv / dropped when rotated), `hg_exclude` (keep-out
  boxes: lines are cut around them, text/sprites touching them skipped; cleared by hg_begin), `hg_flush` + 
  `hg_image_quad` for a foreign texture. `hud_project_dir/point(hs, ...)` = the HUD's conformal projection.
- **HudState** (hud.h) = everything the HUD reads; `hud_state_fill(hs, g, cam, alpha)` interpolates player 0
  (slerped qPath, nose = qPath * rotX(-alpha), V, n, mach, vs, heading of the path, bank). `hud_draw(g, c, alpha)`
  (what main already calls) = fill + `hud_set_extras(warn, clockSec, weapon, flares)` values + `hud_draw_state`.
  `map_draw(g, alpha)` likewise. Both work in main today (verified only through -hudtest; scene/camera are stubs).
- **For T08**: (1) the cockpit camera must look along the NOSE (`flight_nose`/qPath*rotX(-alpha)) so the waterline
  = boresight and the VV sits alpha below it (camera.cpp stub uses qPath); the HUD reads `Camera.view/proj` (65 deg
  horizontal at 4:3, same as the stub). (2) Call `hud_set_extras(bits, clock, weapon, flares)` each frame:
  `HUDW_PULLUP` / `HUDW_RETURN` from world_check's WEV_PULLUP / WEV_RETURN_AREA, clock < 0 = hidden, weapon "" in M1,
  flares < 0 hides "F40". (3) `hud_message(text, secs)` for TAKEOFF / YOU HAVE CONTROL / INI RELOADED (n) / the F6
  mode name (queue of 4, sim time); `hud_reset()` at flight start. (4) Tab = `map_cycle_range()`. (5) Remove main's
  T01 debug overlay (JET skeleton text + RB_ADD boresight cross) when the HUD is on. (6) `jet.ini` gains `[hud]`
  keys via -dumpini (bright aseRadius aseOuter aseGapMin600 boxPadX boxPadY chaseAlpha ladderWindow chaseHeading
  chaseMach atlasSet mapRange mapShowAll mapX mapY mapSize mapBgAlpha mapWedgeAlpha mapIslandAlpha mapImgX0..Z1
  mapAreaX/Z/R).
- **For T15**: `map_draw_contact(&MapContact{x, z, headingRad, MAPC_UNKNOWN|HOSTILE|MSL_OWN|MSL_HOSTILE,
  MAPF_MEMORY|DESIGNATED})` queues for this frame (main draws hudtgt before the map; map_draw draws the queue on
  top and clears it); `map_draw_rwr(bearing)`; call `map_set_auto_contacts(0)` at init (until then map_draw shows
  every live enemy as a hostile chevron when `mapShowAll` = 1). `map_to_screen` for custom map marks. The -hudtest
  table is in `hud_test()` (hud.cpp): add `else if (!e_stricmp(name, "contacts")) r = hudtgt_test();` there. Use
  `hud_layout()` for the frame (x 170..470, **y 152..384**: top moved below the heading caret).
- **For T16**: ASE circle centre = waterline (320, 240), radius `[hud] aseRadius` **60** (DESIGN said 66), ticks and
  pointer within `aseOuter` 8 beyond; user rule: clear gap to both boxes, checked for every set by `-hudtest
  layout` (gap 8/10/13/17 px at 480/600/768/1050, minimum 6 px at 600 scaled). If T16 changes the radius or tick
  reach, change the params and rerun -hudtest layout. Readouts right of the circle start at x 396 = the altitude box
  left edge, below it (box bottom y ~239).
- **Stability rules as built**: numbers at 10 Hz (speed 1 kt, altitude 10 ft) and 5 Hz (VVI 10 ft/min, Mach 0.01,
  G 0.1, AOA 0.1); dead band 0.5 + 0.2 quanta; a change in the same direction as the previous update's change
  applies at once, a first move or reversal needs 2 consecutive updates; strings + ink widths rebuilt only on
  change. Speed is true airspeed (no CAS model). Peak G shown above 4 g, cleared after 5 s below 2 g (HUD-side, not
  fs.peakG). Flashing from HudState.time: VV clamped 3 Hz, PULL UP 4 Hz, hostile missile dots 4 Hz.
- **Ladder**: rung centres = projection of directions at elevation theta on the great circle through the VV and the
  zenith (heading from the lift vector at the vertical), rung direction = projected horizontal right vector, so it
  is exactly conformal at any attitude incl. inverted; hooks towards the horizon; lines cut around the boxes, VVI
  and AOA/M/G stack (keep-out boxes) for readability.
- **Tests (VM A, `-hudtest NAME`, exit = failures)**: `ladder` (1720 conformal checks: max error 0.075 px, max
  frame-to-frame motion deviation 0.133 px, VV 0.000 px off boresight at alpha 0; 12 attitudes read back from the
  back buffer: 0 lit pixels outside the frame; timing) | `numbers` (48 s sweeps incl. 9.9 G and 4/5-digit
  altitudes: max changes/s 10/10/1/5/5/5 vs limits 10/10/5/5/5/5, 0 flicker, 0 changes under jitter in the hold,
  box size constant, centring within 0.5 px, lag within limits) | `layout` (ASE gaps for all four sets + 10 shots +
  a dummy ASE circle in the cockpit level shot) | `all`. Commands: `jet.exe -fullscreen -res 800 600 -hudtest
  ladder -log` (also 640x480 `-window`, 1024x768 and 1600x1200 fullscreen for the layout; 1400x1050 is not a mode
  on the VM's Voodoo3: INVALIDMODE). All exit 0.
- **Cost** (VM, 800x600, 600 set, CPU, 120 frames): cockpit HUD 0.955 + minimap 0.360 = **1.315 ms** (max 1.38),
  180 quads; chase 0.526 + 0.360 = 0.886 ms. ~7.3 us per quad. Draws: 1 per atlas page for the HUD, 3 for the map
  (atlas, island texture with RB_BLEND, atlas). Little headroom under 1.5 ms: T15/T16 symbology adds quads (its own
  budget 0.5 ms in DESIGN 10.5).

## T04 e_terrain: island load/queries, LOD terrain, sea (2026-11-13)

- **Files**: `engine/world/e_terrain.h/.cpp` (portable: loader, queries, LOS march, LOD selection),
  `e_terrain_draw.cpp` (render), `e_terrain.md` (API notes, rules, fog quirk, viewer usage: read it before using
  the renderer), `e_terrain_view.cpp` + `e_terrain_view.mak` (stand-alone viewer exe for shots/bench; not in
  jet.exe), `src/tests_terrain.cpp`. DESIGN 8.6 API unchanged; everything else appended (TerrainDrawCfg,
  TerrainStats, terrain_select, terrain_stats, terrain_normal, terrain_block_max, terrain_area_max,
  terrain_fog_565fix, terrain_segment_ref, TERRAIN_LODSCALE_LOW/HIGH, TCLASS_*).
- **Queries** (portable, usable now): `terrain_height` = LOD0 mesh rule (SW-NE diagonal; bilinear differs from the
  mesh by up to 5.2 m), outside grid -60; `terrain_segment_hit` = first contact with mesh **or sea plane**;
  `terrain_los` = 1 when strictly above the mesh and both ends above sea. LOS march: grid clip -> 2 km super blocks
  -> 256 m HMAX blocks -> exact per-cell triangle test. **VM: 11.0 us average for 30 km rays** (classes: mixed
  300-3000 m 10.9, low 30-400 m 19.6, volcano rim 2.4; worst single ray 92 us); terrain_height ~1 us per call.
- **Verification**: `jet.exe -test terrain` rc=0 on VM A (30 checks, ~11 s incl. load) and `simh test terrain` exit 0
  (29 checks; the timing gate is VM-only): height = HGHT at all 263k samples, = mesh at 573k points of the LOD0
  triangles (worst 0.025 m), LOS vs exact reference on 1000 random rays 0 mismatches (636 clear / 364 blocked),
  segment hit points on the surface (dt 0.001 m), dense 2 m sampling never finds a missed contact, hysteresis no
  flips, skirts only at LOD changes. `simh island [lodScale]` prints the view table below.
- **LOD / budget** (orchestrator asks): skirts only on the sides that face another mesh LOD (per-side skirt ranges,
  sorted at gpu_init), `lodScale` param, `distMode` 0 nearest point / 1 centre (default 1), restriction to +-1 LOD
  between neighbours, group culling, LOD only for visible chunks. **Low defaults: lodScale 0.85, distMode 1, far
  18 km** -> T03's `[views]` max over 8 headings (terrain + skirts, foam separately):
  50 m over the runway **4632** (skirt 390, foam 328; LODs 4/7/21/66/49), 1500 m over the crater **918**, 8000 m
  over the sea **702**, threshold **4720** (T03 measured 10.9k / 3.2k / 3.5k / 10.5k with lodScale 1, all skirts).
  lodScale 0.7: runway 3.4k (viewer). High: `TERRAIN_LODSCALE_HIGH` 1.5 -> runway/threshold ~11.3k (High 12k).
- **lodScale convention (DESIGN deviation, please fix 9.3)**: DESIGN 8.2's formula multiplies the switch distances
  by lodScale (larger = finer, same as e_world's WView.lodScale); DESIGN 9.3's "High 0.5 (8 px)" only fits the
  inverse. Here Low 0.85, High 1.5 (2.0 would be the 8 px rule but ~16-18k tris).
- **VM bench** (e_terrain_view, 800x600x16 fullscreen, Low defaults, CPU ms per frame, worst heading of each
  view; `-vp 8` gives the same numbers, so it is all CPU transform/clip/submit, no fill wait):
  runway 50 m: terrain 8.9 + sea 1.7 = **10.6 ms** (4.5k tris, 4.2k verts, 4 draws); threshold 9.4 + 1.8 = **11.2**;
  volcano slope 10.1; coast 40 m 9.7; crater 1500 m 4.1; sea 8000 m 5.5. Selection 0.3-0.7 ms of that.
  **The TASKS gate "terrain + sea <= 6 ms at the worst viewpoint" is NOT met** at the accepted ~4.5-5k tris: the
  renderer floor on the VM is ~0.8 us/tri submit + 0.63 us/vertex transform (racer spike), i.e. ~6 ms for terrain
  alone at 4.5k tris / 4.2k verts (measured 1.8 us per tri all-in).
  6 ms needs ~2-2.5k tris (lodScale ~0.6 and/or a coarser far LOD: e.g. a 2 km merged LOD for whole groups would
  need an islandc change). Levers for T09: lodScale (0.7: runway 9.25 ms), farDist (= fogEnd 16.2 km drops ~15 %
  of the LOD4 chunks from altitude). Merging must stay on: ~38 us per draw call (342 draws = 22 ms).
- **Looks (screenshots, `-all`)**: no cracks (`-view slope -skirts 2` shows the sky through LOD seams, default
  shows none), coast: two shots 5 cm apart differ by 13 / 5 / 0 strongly changed pixels at coast_low / crater / sea
  (no z-fight between terrain, shallow fans, ring and foam), fog joins the panorama exactly with the 565 fix below.
  PNGs: `<orchestrator scratchpad>/t04/tv_*.png` (runway, runway_sea, crater, sea3 = fixed fog, coast_low,
  threshold, slope, slope_noskirt).
- **Shallows (DESIGN 8.3 deviation, same look)**: not a blended ring "inside 1.5 km of the coast" but flat fans over
  every water chunk (256/512 m lattices, T-junction free), blended 0.65 with Z write + zBias 1, alpha -> 1 from 30 to
  58 m depth; the opaque ring is drawn after them and Z-masked by them. This shows T03's shelf colours at full mesh
  detail and meets the opaque sea without a seam. Foam: additive, black-fog fade, zBias 2.
- **For T08 (scene)**: `terrain_gpu_init` after r_init (sea works without an island too); per frame
  `r_set_fog(...)`, set `terrain_draw_cfg()->fogRGB/fogStart/fogEnd` to the same values, `terrain_draw(t, view,
  TERRAIN_LODSCALE_LOW, far)` first, base/aircraft, then **one** `sea_draw(t, view, seconds)` (shallow + ring + fog
  band + foam, in that order) before the late sky. `terrain_stats()` has tris/verts/skirt/foam/sea/shallow/draws
  for the "terrain: tris=" bench line. **Fog colour**: on the Voodoo3 fog/vertex colours come out one 565 level
  darker than texels: pass `terrain_fog_565fix(light fog)` to r_set_fog and cfg, and after world_sky_load set
  `w->sky.fog` and every `w->skyBot[0..31]` through it as well, else the far sea and e_world's lower sky band show a
  visible line against the panorama (measured (189,210,222) vs (197,214,230); with the fix all equal).
  Load: jet.isl 7.3 s from M: (Samba) + gpu_init 0.6-1.3 s; memory ~8 MB file (kept, VERT/INDX reordered in place).
  Low horizon from 50 m: the fogged part of the sea (beyond 3.6 km) is only ~10 px tall, so the sea-sky line looks
  crisp at low altitude; a closer fog start would soften it (T09/scene).
- **Other notes**: base.trk ribbons are zBias 1 (T03) but T02's decision is zBias 2 (T03/T08). jet.ini keys: none
  registered by T04 (the engine can't see params): T08 maps its `[scene]`/preset params onto TerrainDrawCfg.

## T08 M1 integration: scene, cameras, flow, sound, replays, overlays (2026-11-13)

- **Files** (replaced the T01 stubs): `src/scene.*`, `src/camera.*`, `src/snd.*`, `src/fx.*` (T08 section: flame +
  nozzle glow; T17/T22 append their own sections), `src/bench.*`, `src/main.cpp`, `src/replay.*` (complete),
  `play.bat`, `jet.ini` (regenerated: 242 keys + `[controls]`), `docs/PLAYTEST_M1.md`, scripts `tests/t08/*.scr`,
  screenshots `shots/t08/*.bmp`. **`src/game.h/.cpp` (T08 owns them with main after T01)**: appended the portable
  session (`session_*`) and the `-script` runner (`script_*`), `Args.benchView`, `[video]` default 800x600, removed
  the implemented switches from `s_notYet`, added `-benchview`. No engine file changed.
- **Portable session (game.h)**: the whole DESIGN 2.6 tick lives in `session_tick(g, keys, cmd)` (pilot_update_fs ->
  target -> ai -> takeoff_tick + world_override_input + bench autopilot hook -> flight_step -> world_check ->
  weapons/radar/mission -> crash flow: `world_respawn_due` -> pilot_reset + takeoff_park). `session_start` does
  world_reset, takeoff_bind_pilot, runway park (or `-start air` = world_air_start), resets every module; g->tick
  starts at **1**. `cmd` = `SES_CMD_RUNWAY` (pause menu restart) / `SES_CMD_AIR` (dev F9) is part of the tick input.
  main.cpp, `-test determinism/replay` and simh all fly through it, so everything a replay needs is keys + cmd.
  T17: add enemies to `g->numAircraft` in `mission_start` / spawn code; the tick already steps them.
- **Replays record PilotKeys, not PilotInput (format v2, replay.h)**: 'K' + 12 bytes (key bits, S held/released
  counters, joystick s16, session cmd in bits 12-15), 'R' repeats, 'H' hash before the tick, 'P' params snapshot,
  'E'. Reason: the takeoff override and Novice read the shaped key axes (PilotState), so replaying PilotInput could
  not reproduce them; keys also compress better (a 60 s flight = ~1.8 KB of records + 2 x 2.5 KB snapshots). Every
  interactive flight writes `last.rpl`; `-record FILE`; `-replay FILE` (rendered or `-headless`; exit 1 on hash
  diffs; logs `replay: done ... diff=N`). F5, F6 and script `param` lines write a PARAMS record (`params_changed`
  in main). The final 'H' record carries the next tick number (session ticks start at 1).
  The T06 size issue is fixed: the check compares record bytes without the snapshots (`ReplayStats.snapBytes`).
- **-script FILE** (game.cpp): DESIGN 16.2 lines plus `S` key, `param sec.key=value`, `restart`, `air`, `end`;
  expects for phase kt agl_ft alt_ft gear lat_m onground alive pullup gamma pitch bank respawns mode. Works headless
  and rendered (script keys replace the live keys; F5 etc. still work). Exit code = failed expects. T06's
  acceptance command works now: `-headless -start runway -script tests\t06\takeoff.scr -exitafter 40 -log`.
- **Input**: jet actions 0-15 defined in main (`JA_*`, names = `[controls]` keys: pitchUp pitchDown rollLeft
  rollRight fast slow target fire weapon flare view look mapRange pause stickX stickY); `jet_input_setup` runs after
  `in_init` (which binds the racer defaults) and for `-dumpini` (writes `[controls]`). S fills
  `tgtHeld/tgtReleased` through `in_action_held_ticks/released`; joystick = `j0 x` / `j0 y` full axes (stick back =
  +y = nose up). Live (not recorded) keys: Esc/P pause menu, F5, F6, F9/F10 (dev), F11, F12 (`shots\jet_NNN.bmp`),
  Tab. C (view) comes from `PilotInput.view`, so replays switch views too.
- **Scene** (DESIGN 9.4): r_set_fog with `terrain_fog_565fix(light.txt fog)` (also terrain cfg, `World.sky.fog`,
  every `skyBot[]`), terrain_draw, base.trk via e_world (`baseDist` 6 km, cells LOD0 within `baseLodDist` 1.5 km),
  aircraft, one sea_draw, late sky (`world_sky_load(.., "light")`, `world_set_sky_late(1)`, clear = fog | R_ZONLY),
  cloud deck (High), puffs (clusters of 4, 3-15 km from the island, 2000-3600 m, sorted, RB_BLEND fogged, fade within
  `cloudFadeM` 300 m), then fx, dots (2x2, 1.5-4 km, alpha 0.7, terrain_los occlusion), HUD, minimap, overlays.
  Presets: `s_presets[]` in scene.cpp (Low far 18 km, fog 3.6-16.2 km, lodScale 0.85, 4 sky tiles, 16 puffs; High 28
  km, 5.6-25.2 km, 1.5, 8 tiles, 48 puffs + deck + sun glow); `[scene]` keys override live (0/-1 = preset).
  T09 tunes `s_presets` and the `[scene]` defaults; `scene_stats()` has per-frame tris and ms per section.
- **Jet drawing** (A1 rules): `mesh_draw_lit` with the light set's sun/ambient, LOD by distance x `acLodScale`,
  meshes to `acMeshDist` 2.5 km; LOD0 adds stabs/flaperons (`rotX(angle) * translate(hinge tag) * body`, angle =
  FlightVisual, + = trailing edge up), nozzle (scale x/y 1 + 0.18 nozzle), gear legs (nose +(1-gear)*90 deg, mains -)
  only while gearPos > 0. Enemy livery = `jet_aggr` override (not on nozzle/gear). Own jet: chase view only, drawn
  last. M1 props (render only, `[scene] props`): 2 aggressor jets parked 30 m in front of the first two hangar
  boxes, 1 circling the island (r 7 km, 1100 m, 200 m/s); `scene_prop_pose` (T17 can delete them).
- **fx (T08 part)**: flame = `jet_flame.msh` via `mesh_append_part` (world space, tint = brightness, u + random per
  frame), RB_ADD, no Z write, fog off, scale x/y with the nozzle and z by 0.3 + 0.7 ab; nozzle glow billboard at the
  `nozzle` tag (0.4 m aft), size `glowSize` x (0.55 + 0.45 ab); both fade from `fadeFrom` 1.2 km to acMeshDist.
- **Cameras** ([camera]): cockpit = `cam_cockpit` tag, orientation qNose (waterline = boresight, T07 rule), 65 deg,
  near 1.0; chase = lagging frame (tauCamRot 0.10 / tauCamRoll 0.16), 24 m behind / 6 up, look 40 m ahead along the
  nose, FOV 70 + 5 x ab, near 1.5, eye >= 3 m above the surface; wreck orbit while dead; `-camat` fixed (HUD hidden);
  small shake above 7 g / in AB (render-only noise). Start view `[camera] startView` = 1 (chase). Far = scene preset.
- **Sound** (A5 law, `[sound]` master 0.7): loops idle/mil/ab/wind/brake/roll started at the -74 dB floor and kept
  running; spool N lag (READY 0.65 / SLOW .74 / NORMAL .90 / FAST 1.0, tau 0.8 up 1.5 down); ab_light on the ab rise;
  gear_up / gear_clunk / beep / expl_near from the world event queue (own index); PULL UP loop while warned;
  `cockpitEngine` 0.8. snd_ui(0) = beep (F5, F6).
- **HUD wiring**: `hud_set_extras(PULLUP | RETURN, -1, "", -1)` from `world_status(0)`; takeoff messages re-queued
  every second while `takeoff_message` returns text ("A: TAKEOFF", "YOU HAVE CONTROL"); "INI RELOADED (n)" and the
  F6 mode name via hud_message; `hud_reset` + camera snap on every respawn (`session_respawns` changes). HUD and
  minimap are hidden while dead (red "CRASH / RESTART ON THE RUNWAY"). The T01 debug overlay is gone; overlays (F10
  feel, F11 fps, pause menu, REPLAY) use the HUD font (hg_*).
- **Log level**: plays now log at info (`jet.log`: takeoff timeline, events, F5 lines; ~1 line/s while a takeoff
  message is up). Benches log to jet_bench.log, scripts/replays/tests to jet_test.log (or `-logfile`).
- **Verified (VM A, 800x600x16 fullscreen unless noted)**: `-test replay` 11/11 (60 s session with 2 param changes:
  61 hashes, 0 diffs; a different turnBoost is detected: 26 diffs) and `-test determinism` 3/3 (two 90 s sessions
  with takeoff, manoeuvres, a crash into the sea, the runway restart and an air restart: identical hashes; same on the
  Mac, crash at tick 8077 on both); simh `test all` green. T06 command: 12/12 expects. **F5 live**: windowed scripted
  flight recorded with `-record`, jet.ini turnBoost 1.0 -> 1.3 edited from the Mac and F5 sent by vncdo during a
  full pull: `params: reload flight.turnBoost old=1 new=1.3` at tick 4427, telemetry nLim 9.0 -> 11.7 and n 9.0 ->
  10.3 within 6 ticks; `-replay` of that file: `replay: done ticks=9000 hashes=76 diff=0 params=3` (evidence in
  `tests/t08/` + `tests/t08/logs/`). Screenshots `shots/t08/` (ready, roll, climb-out, cockpit over sea and island,
  chase AB, PULL UP at the volcano, crash/wreck view, respawn READY, apron with the parked props).
- **Bench** (`-bench 60 -benchroute R -preset low`, chase view, `tests/t08/logs/bench2_*.txt`): all four routes
  29.97-29.98 fps, missed 0.11-0.22 %, 1%-low 29.99 (30 fps cap, divisor 4): draw CPU avg low 10.7 / volcano 9.1 /
  high 7.8 / clouds 12.0 ms (worst 27 / 18 / 12 / 17), present (vblank wait) is the rest. Terrain tris avg/max low
  1150/4514, volcano 651/2234, high 145/476, clouds 964/1318; total tris max 6.5k (low). Per section avg: terrain
  1.9-3.7 ms, sea 0.9-2.0, sky 1.4, HUD+map 0.7, clouds 0.1, **aircraft 3.4 ms** (own LOD0 jet + 6 parts with CPU
  lighting in mesh_draw_lit: the biggest single item; T09 lever: cache the lit vertices per frame/part, or LOD1 for
  the own jet beyond ~40 m, or fewer parts).
- **Open / for later**: no explosion effect (T17); props are render-only; `ARGS_MAX_SHOTS` is 8 per run; puff
  clouds are sparse billboards (16 on Low); the user's play-test decides `startView` and the chase numbers. jet.ini
  must be regenerated (`jet.exe -dumpini jet.ini`) after any params change.

## T10a M1 feel round 1: altitude ceiling -> thrust lapse, speed truth (2026-11-13)

- **Files**: `src/flight.cpp/.h` (lapse + above-ceiling terms; appended `flight_thrust_lapse(h)`, `flight_ceiling()`),
  `src/tests_flight.cpp` (new checks + replay runner), `docs/FLIGHT_TUNING.md` (Altitude section), `jet.ini`
  (regenerated with `-dumpini`), `tests/user/m1_ceiling.rpl` + `.log` (the user's flight, copied from last.rpl/jet.log),
  `tests/user/m1_ceiling_vm_old.csv/.log` (exact telemetry of that flight with the M1 exe: replay diff=0).
- **The bug, exactly** (VM replay with the M1 build, diff=0): t=214-252 s the user zoomed vertically in AB to 57 300 ft
  (17.5 km); from t=258 to t=301 s, A held, nose 35-88 deg down, the speed sat at 150 kt for 43 s (4412 ticks at the
  floor while descending) from 56 800 to 51 000 ft. Cause: T05's `ceilDrag` = 8 x (h-12 km)/3 km m/s^2 (14 m/s^2 at
  17.2 km) is more than gravity (9.81) can give, and `ceilingFade` = 0 above 15 km; the floor clamp held V at 150.
  It released only below 15.7 km where the drag fell under g.
- **New model** ([flight]; `ceilSoftM ceilHardM ceilDrag` removed): positive throttle acceleration x
  `flight_thrust_lapse(h)` = clamp((sigma^k - sigmaC^k)/(sigmaF^k - sigmaC^k), 0, 1), ISA density ratio sigma,
  `thrustFullM` 6000 (factor 1 below: **nothing changes below 6 km**), `ceilingM` 15500 (factor 0: AB climb rate fades
  to 0), `thrustLapseExp` 0.7. Table of 161 entries (200 m), rebuilt when a key changes (F5); no pow/exp per tick.
  Above `ceilingM` only: `thinAirDecel` 0.5 m/s^2 (ramps in over 1 km and with speed: 0 at the floor speed, full 200 kt
  above; max 1.5) so the jet can't cruise above the ceiling; `zoomDrag` 3 (extra cost of climbing, x(h-ceiling)/1 km
  x g x sin(gamma), **only while climbing**: zooms top out smoothly, dives and level flight untouched); gentle nose drop
  towards a 5 deg descent from ceiling+1 km (half floorDropDeg, smooth over 1.5 km). Speed targets unchanged (Mach x
  a(h)); AB top speed is not reduced, it just takes longer to reach as thrust lapses (0 at the ceiling).
- **Measured** (VM = Mac): every feel card line unchanged (19.14/27.90/31.90/7.41 deg/s, 13.88 s, 13.32 s, 40.87 s,
  2.94 kt/s); throttle acceleration at 0/3/5.9 km exactly 18.00 AB / 6.00 NORMAL. The user's replay hashes match the
  old model until tick 26400 = the first time above 6 km (proof that below 6 km nothing changed). Dives: 80 cases
  (3-20 km, 10-90 deg, AB and NORMAL, from 150 kt) all gain speed on the first tick (worst +1.70 m/s^2 = 10 deg dive);
  user case 17.2 km 150 kt 35 deg AB dive: 183/204 kt after 3/5 s, 500 kt after 31 s; vertical: 207/244 kt after 3/5 s.
  Zooms (path held): AB 30 deg climb from 3 km tops out 17.5 km, M1.6 at 15 km + 4 s pull 17.7 km, the user's vertical
  1000 kt zoom from 4 km 17.8 km (58 300 ft; old model 57 300); all come back below the ceiling, 0 floor-pinned
  descending ticks, max accel step 0.07 m/s^2 per tick. Climb rate (best steady, ft/min) AB / NORMAL: 0 km
  180k/31k, 6 km 166k/29k, 8 km 122k/21k, 10 km 83k/14k, 12 km 49k/8.4k, 14 km 18.6k/3.2k, 15 km 5.9k/1.0k, 15.5 km 0/0
  (arcade numbers: AB T/W ~1.8 at any speed below target is the M1 feel the user likes). flight_step 11.9 us.
- **Speed truth** (user: "1000 knots doesn't feel like 1800 kph"): the numbers are right. HUD speed = true airspeed
  (V x MS2KT); 1000 kt = 1852.0 km/h; flown path length per second = HUD speed within 0.02 % over 479 one-second
  windows of the user's whole session (fastest second: 1079 kt, 555 m flown = 1997 km/h); sim time = real time (user's
  jet.log: simtime 899.68 s vs realtime 899.71 s); speed_of_sound within 0.23 % of ISA up to 15 km. World units are
  metres (runway 2800 x 45 m). Mach 1.6 = 1058 kt at sea level, 983 kt at 6 km, 920 kt above 11 km (TAS). So the
  "slow" feeling is perception (few near references, 65-70 deg FOV): T10b's cues are the right lever.
- **Tests**: `-test flight` 63 checks (new: `climb_rate_falls_with_altitude thrust_unchanged_below_6km
  lapse_1_low_0_at_ceiling climb_fades_to_0_at_ceiling dive_always_gains_speed user_case_ab_dive_17km
  user_case_vertical_dive ceiling_ab_climb_tops_out ceiling_settles ceiling_climb_smooth zoom_m16_15km_limited
  zoom_m16_comes_back zoom_vertical_1000kt_4km speed_truth_distance_1s speed_truth_kt_to_kmh
  speed_truth_sound_vs_isa`; the old `ceiling_zoom_limited` is replaced), plus climb table lines. `simh feel` now also
  prints the climb table and the three zoom tops for the loaded jet.ini. pilot/replay/determinism/takeoff/world green
  on VM A; simh `test all` green.
- **Replay runner** (new): `jet.exe -test flight FILE.rpl` / `simh test flight FILE.rpl` replays a recording headless
  through the session and writes `FILE.rpl.csv` (telemetry columns) + a 5 s summary + the session speed-truth line;
  exit code = hash differences (a replay recorded by an older flight model diverges where the physics changed).
  Mac vs VM: the M1 recording replays with diffs from tick 1200 on the Mac (float differences vs x87), exactly on
  the VM; use the VM for exact reproduction of user flights.
- **jet.ini**: the old file had no user edits (every value = code default); regenerated, with T10b's four in-progress
  `[scene]` keys (hazeScaleM farMaxM skyFirst baseAltK) left out on purpose so their defaults stay live: T10b/the
  integrator should regenerate after the render changes land.
- **For the orchestrator**: DESIGN 4.3/4.5/4.6 still describe ceilingFade/ceilSoftM/ceilHardM (read-only for me):
  replace with the lapse keys above. Facts not changed (feel at normal altitude): at NORMAL throttle a dive is held
  near cruise (decOver 15 > g: V settles at most ~48 kt above the target in a vertical dive) and Z brakes in a dive;
  from below the target every dive accelerates. VM build warnings C4305 in camera.cpp(42), fx.cpp(189), snd.cpp(37)
  are in T10b's in-progress files.

## T03b Triple the world: 3x island and flying area (2026-11-13)

- **What changed**: new `data/island/jet.isl` (9.46 MB, format unchanged, version 1), `base.trk`, `map_island.bmp` +
  `_a` (now **256x256**), `report.txt`, previews `tools/islandc/out/*.png`. Island **34 x 34 km** land bbox (T03:
  9.5 x 11.6), 665 km2 land, crater rim mean 3000 m, **summit 3077 m** (at 560, 640), crater rim radius 1250 m, bowl
  400 m, cone base radius 10.8 km (exp 1.6) inside a lowland ring with 190 m hills, 6 satellite cones (380-820 m), 6
  lava channels, 44 ridges, 5 bays/headlands + a west cape, ENE cliffs (130 m), patchy 340 m beaches, 2 islets
  (17600,-6600 and -7400,17800). Green with a bare rock/ash summit cone (grass < 1450 m, ash > 1900 m).
- **Grid: 513 x 80 m** (41 km square, x0 = z0 = -20480), chunks 16 cells = **1280 m** (32 x 32), lodEdge 80/160/320/
  640/1280. Why 80 m and not finer: e_terrain refuses gridN > 513 (`superMax[64]`, `nSuperX <= 8` in e_terrain.h/.cpp
  terrain_load), so 513 samples must span island + shelf: 64 m = 32.8 km (too small for a 3x island), 72 m = 36.9 km
  (~1 km sea margin), 80 m = 41 km (3-4.5 km margin). File size also rules out finer cells: ~82 bytes per land LOD0
  quad -> 64 m cells for this island ~15 MB. **No engine/loader change needed.** All texture periods 64 m (t_sand was
  32 m), so chunk-local UVs reach **20** (T02 rule 16-32; foam u < 9.1). LOD switch distances scale with the cell
  (d_k = lodScale x 28.6 x edge_k: Low 1.95 / 3.9 / 7.8 / 15.6 km), so triangles per screen stay the same rule; LOD0
  quads near the jet are 80 m (T03: 32 m).
- **Airbase** (same layout/rules): runway centre **(-13850, -2200)**, heading 270, 2800 x 45 m, plateau 12 m (blend now
  800 m: flat coastal base area), threshold spawn (-12490, 12.2, -2200); on the island's west cape, coast **570 m**
  past the departure end, climb-out corridor max terrain 46 m (limit 150), hills 100-200 m start ~3 km east (inland)
  of the threshold. Designators 27/09. Boxes as before (moved with the base).
- **Island centre (INFO) (1200, 1800), radius 19.2 km**; air spawns 26 km N/E/S/W of it at 2000 m (~9 km off the
  coast; beyond the height grid = open sea, so tests' sea_point still works). Area centre = spawn mean = (1200, 1800).
- **Area / minimap defaults** (edited in the registering files + the same 9 keys in `jet.ini`): `[world] areaWarnKm 75`,
  `areaTurnKm 90` (T03: 26 / 32); `[hud] mapAreaR 75000` (circle = the warning ring), `mapAreaX/Z 1200/1800`,
  `mapImgX0 -18512 mapImgZ0 -17912 mapImgX1 20912 mapImgZ1 21512` (= MAPI, 154 m/px). Range steps unchanged (Tab
  10/20/40 NM, default 20 NM = the whole island with sea around; 40 NM shows the warning ring when near it).
  hud_map still takes the image rect from params, not from `Terrain.mapRect`: if the island changes again, update
  both (or make hud_map read `mapRect` when `mapTex[0]`).
- **islandc (T03b additions, all optional; defaults rebuild the T03 island byte-identically, checked)**: island.txt
  keys `grid N cell` (N = 64k+1 <= 513), `feature_scale` (internal noise wavelengths/fades of height.py), `noise_scale`
  (class border noise), `coast_detail lo hi` (angular coast noise lattice), `bay az halfwidth depth` (* ; < 0 =
  headland), `beach width share`, `islet x z radius height` (*), `edge_fade m` (sea forced to the -60 m floor near
  the grid border), `minimap_size`. Macro tint height bands scale with summit/1750. Report: budget line 16 MB,
  [views] now LOD to the chunk centre with lodScale 1.0/0.85 + a mid-slope viewpoint (upper bound: all skirts);
  exact engine numbers: `tools/simh/simh island 0.85`. Previews: view_sea15km/40km/60km = 15/40/60 km off the coast
  (40/60 rendered with a longer far plane, labelled), view_high (6000 m, High's 28 km far), view_slopes (250 m over
  the cone's SW slopes). `--check`: OK (A == B == data, reader round trip identical, 0 problems); `build_assets.py
  --only island --check`: identical. Compile ~22 s, + ~25 s previews.
- **Numbers** (report / simh / VM A): 597 stored chunks (130 underwater, 247 with water), 272k vertices, 1.23 M
  indices, 4029 batches. Engine LOD view table (simh island, Low lodScale 0.85, dist centre, skirts at changes, far
  18 km; T04 numbers in brackets): runway 50 m **4086** (4632), threshold **4458** (4720), sea 8000 m 758 (702),
  1500 m over the summit 2602 (918: the whole big island is in view now). lodScale 1.5 (High) runway 9.0k
  (~11.3k). **VM A**: `-test terrain` 30/30, `-test takeoff` 69/69 (liftoff 12.05 s, min clear 2.29 m, same as
  T06), `-test world` 34/34 (bounds max 92.0 km / 94.9 km fast, turn-back 20-25 s); LOS 30 km rays avg **17.9 us**
  (gate 30; T04 11.0), terrain_height 1.06 us. **Load: `boot: terrain loaded ... ms=8905`** (T04: 7.3 s for 7.77 MB).
  Bench (`-bench 60 -preset low`, 800x600, chase; reports `tests/t03b/bench_*.txt`): low 29.97 fps, terrain tris
  avg/max 1073/4120, draw 10.6 ms avg (24.7 worst); volcano 29.97 fps, 3106/5190, draw 14.0 ms (20.7); high 29.98
  fps, 573/916, draw 10.1 ms (16.4). All 30 fps, missed <= 0.28 %.
- **Edits outside islandc (data-driven, needed for the new island)**: `src/tests_terrain.cpp` (outside-grid / sea
  points from x0/z0, los_high_clear at hTop + 1000, crater view over the highest HMAX block, runway view from BASE,
  file_facts nChunks > 100 and nBlocksX = (gridN-1)/8), `src/tests_world.cpp` (t_bounds starts 1 km inside
  areaWarnKm and allows turn + 4 / 8 km, read via params_find), `src/bench.cpp` (route low: the start 3 km inland at
  62 m was inside a 105 m hill -> crash at tick 1, the bench then measured the parked jet: start y = max(62,
  area max + minAgl)). All pass on the T03 island too.
- **Left for others (island-size assumptions, not edited)**: `src/scene.cpp` props_init/clouds_init: the circling
  prop (r 7 km, 1100 m around the island centre) now flies over the cone flank/satellite cones (tops ~1250 m), and
  the puff clusters 3-15 km from the centre at 2000-3600 m sit over the island interior near the 3077 m cone
  (suggest r ~20 km / 1500 m and clusters 8-35 km). `src/bench.cpp` routes volcano (cx +-6 km at 2300 m: below the
  rim now, the AP climbs to 2285-3204 m) and clouds (2900 m over the summit) were sized for T03 (suggest volcano
  3600 m +-12 km). `src/hud_map.cpp` map_init runway defaults (-2250, -3980) are overwritten from BASE every frame.
  DESIGN 8.1/8.2/8.6/8.7/9.1 numbers (513 x 32 m, 512 m chunks, 128x128 map, area 30 km). On Low (far 18 km) the
  volcano is not visible from the air spawns (26 km) and the island is only a fog line 15 km off the coast
  (`view_sea15km.png`): T10b's altitude-aware far plane decides how far the island shows.

## T10b M1 play-test round 1: the "ball" view, view distance for the 3x world, speed sensation (2026-11-13)

- **Files**: `src/scene.cpp/.h` (altitude-aware distances, sky first, radial culls, presets, props/clouds for the 3x
  world), `src/camera.cpp/.h` (altitude far plane, speed FOV, buffet), `src/fx.cpp/.h` (T10b section: air streaks,
  `fx` prof section), `src/snd.cpp` (wind boost), `src/bench.cpp` (routes for the 3x world + `far_eff` line),
  `engine/world/e_terrain_draw.cpp` + `e_terrain.md` (sea ring / fog band / sea shade only: **no change to
  e_terrain.h/.cpp, the loader or terrain_select**), `jet.ini` (regenerated: only the 35 new keys added, every
  existing value incl. T03b's identical), `tests/t10b/*.scr` + `tests/t10b/logs/bench_*.txt`, `shots/t10b/*.png`.
  Minimap: unchanged (the user's "triple the radius" = the world, done by T03b; no `mapScale` added).
- **The "ball", exact cause** (reproduced with `-camat` straight down / 35 deg down from 3-15 km, `before_*.png`):
  1. **Late sky dome** (main cause): `world_set_sky_late(1)` draws the panorama after the geometry, Z-tested, scaled
     to a sphere of radius far+25 m whose bottom is a cone from the -30 deg ring to the nadir. From altitude the ground
     lies farther away than that dome, so the dome painted fog colour over all ground beyond
     h x 0.35 horizontally (15 km: everything beyond 5.2 km = a 16-sided disc; diagnosis `diag_sky_dome_grid.png`:
     e_terrain_view with `-nosky` shows the whole sea). Fix: **sky drawn first** without Z (`[scene] skyFirst 1`),
     geometry always covers it. Cost: same CPU (sky 1.36-1.39 ms), the extra fill is hidden behind the CPU.
  2. **Radial vs planar**: the W fog and the far plane are planar (view depth w) but the sea fog band (alpha by
     horizontal radius), the ring's outer radius (1.06 x far), the sea shade (3D distance incl. eye height) and the
     terrain cull (sphere of radius far) were radial: rings / circles / an arc horizon from altitude. Fix: band = a
     strip between the lines w = 0.85 fogEnd and w = far (alpha linear in w, zBias 2, also fogs the shallow fans);
     ring reaches the far plane's corner distance; shade by horizontal distance; terrain/clouds culled at the corner
     distance (`scene_far_radial`) and the frustum's far plane does the rest.
  3. **Fixed 18 km far / fog from 10-15 km altitude** (ground 90 % fogged looking down). Fix: far, fog start and fog
     end x `scene_alt_factor(h)` = h / (H (1 - exp(-h/H))), H = `[scene] hazeScaleM` 8000 (optical depth of an
     exponential atmosphere along a slant path to the sea): x1.0 at sea level, 1.2 at 3 km, 1.24 at 3.6 km, 1.58 at
     8 km; capped by `farMaxM` 62000 (Low reaches it at ~8.3 km). Camera far plane = `scene_far_at(eye.y)`.
- **Voodoo3 hard limit, new and important: nothing may reach a view depth of ~65.5 km.** The fog table / W wrap there:
  far 80 km from 15 km altitude shows an unfogged dark sea band above the horizon, 62 km is clean (`cap*_o15.png`).
  The renderer only rejects triangles wholly beyond far (r_d3d6 clips near + screen edges), so the sea ring is now
  clipped to w <= 0.998 far on the CPU, puffs past the far plane are culled, and `SCENE_FAR_LIMIT` 64000 clamps
  far (terrain chunks overhang far by <= ~1.8 km). **High (56 km at sea level) can grow only to 62 km** on this
  hardware; T23 checks whether the T40's Radeon has the same limit (then raise `farMaxM` there).
- **View distance for the 3x world (orchestrator)**: presets Low **far 38 km, fog 7.6-34.2 km**, High 56 km /
  11.2-50.4 km at sea level (fogEnd 0.9 x far kept, T02). With altitude: 3 km 45.6 km, 8 km 60 km, >= 8.3 km 62 km
  (fog 12.4-55.8). Low horizon from 50-300 m: crisp sea/sky line, island + volcano visible from the 26 km air spawns
  (`new_lvl0.png`, `new_sea50.png`, `new_o03.png`).
- **W-buffer check at the new ranges**: coast + runway from 8 km altitude, two shots 1 m apart: **9 changed pixels**
  of 480k (`fin_rw8a/b.png`): no coast shimmer, no runway z-fight; separated-surface rules of T02 unchanged near the
  camera (W precision did not depend on far in T02). After shots: `fin_*.png`, `new_*.png` (3x world), `after1_*.png`
  (T03 island) next to `before_*.png`.
- **scene (3x world)**: circling prop r 20 km / 1500 m; puff clusters 8-35 km from the centre (2000-3600 m); base
  drawn to `baseDist + baseAltK x altitude` (6 km + h: the runway doesn't vanish from altitude).
  `SceneStats.farM/fogStart/fogEnd` = the frame's effective values.
- **bench routes (bench.cpp)**: volcano +-12 km at 3600 m; high **12000 m** +-25 km (the altitude-scaled 62 km far =
  worst case); clouds 3000 m on a 20 km square around the centre (through the cloud ring). New line
  `bench: far_eff avg= max=`.
- **Speed cues** (render only, sim and replays untouched; master `[fx] speedCues` 1):
  - chase FOV 70 + `fovAB` 5 x ab + **`fovSpeed` 10 x smoothstep(350..1050 kt)**, both smoothed `tauFov` 0.6 s
    (85 deg at full AB + M1.6 down low). Cockpit `fovSpeedCockpit` 0 (keeps the HUD's 65 deg scale; the HUD reads
    Camera.proj so it would stay conformal if raised).
  - **buffet**: smooth noise (3 sines, `buffetHz` 9, no per-frame jitter), mostly vertical: `buffetSpeed` 0.10 deg x
    ((kt-550)/500)^2 + `buffetTransonic` 0.22 deg x bell(Mach 1.0, `buffetMachW` 0.05), x (1 + `buffetLowAlt` 0.8)
    near the ground (fades by 400 m), cockpit x 0.6, off on the ground. The T08 shakeG/shakeAB stay.
  - **air streaks** (`[fx] streak*`): static air points on a 150 m world lattice around a box ahead of the camera
    (true parallax), drawn as 1.6 px camera-facing streaks of length V x 0.030 s, alpha 0.40 fading to the tail,
    near fade 8-16 m; count 32 x linear ramp 250..900 kt, x2 near the ground (fades by 600 m); one draw.
  - **wind sound**: A5 law below 800 kt unchanged; with cues `u` clamps at 1.6 (not 1.25), cap 0.95 (not 0.8),
    x (1 + 0.35) near the ground (fades by 500 m), transonic flutter 25 % around Mach 1 (`[sound] wind*`).
  - Ground clutter/detail texture: not done (terrain is already the biggest cost down low; streaks get the low
    boost instead).
- **Cost (VM A, 800x600 Low, `-bench 60`, chase, reports `tests/t10b/logs/`)**: low 29.96 fps (missed 0.22 %), volcano
  29.96 (0.33), high 29.97 (0.28, far_eff 62000), clouds 29.98 (0.33). Draw CPU avg/worst: low 10.5/24.2 ms, volcano
  13.8/23.7 (terrain 6.0 ms avg: the big island's cone in view), high 11.1/17.4, clouds 10.6/15.6. Speed cues on the
  low route: `fx` section 0.10 ms off / 0.38 ms default (~500 kt, 60 m) / 0.66 ms with all 64 streaks
  (`maxcues.scr`); draw 10.20 -> 10.53 ms.
- **For others**: DESIGN 9.1/9.3/9.4 numbers changed (far/fog, sky first, Voodoo 65.5 km limit) - orchestrator please
  update. Late sky is still available (`skyFirst 0`) but brings the ball back from altitude. `-camat` uses fovChase
  without speed FOV. Screenshots taken with `-shot` stall one frame ~2 s (shows as a fps dip in a bench).

## T11 Radar, contacts, HAFU states, targeting input, RWR (2026-11-13)

- **API note for T12/T14/T15 (stable since the start of T11; full rules in the header comment of `src/radar.h`):**
  `radar_designated(obs)` (aircraft id or -1), `radar_lock(obs)` (`LOCK_NONE / LOCK_TRACK / LOCK_MEMORY`),
  `radar_track(g, obs, id, &pos, &vel)` (0 none/dead, 1 tracked = true pos/vel, 2 MEMORY = extrapolated to g->tick),
  `radar_designate(g, obs, id)` (-1 clears; AI designates with it), `radar_locked_by(id)` (bitmask of observers with
  LOCK_TRACK on id), `radar_views(g, obs, ContactView *out, max)` + `radar_view_pos(g, &v, alpha, &pos)` (HUD:
  latched display states/flags, render position), `radar_rwr(g, PLAYER_ID, &RwrView)`, `radar_rwr_launch(g,
  shooter, target, type)` (**T12: call it at every launch at an aircraft**, type 0 LRM / 1 SRM -> REV_LAUNCH event),
  `radar_rwr_missile_warn(type, launchDist, dist)` (11.5 MISSILE rule), event ring `radar_event_seq/get` (REV_LOCK =
  lock tone, REV_DESIGNATE/UNDESIGNATE, REV_IDENT, REV_DEAD, REV_LAUNCH). Player selection: `target_selected()`.
  Datalink rule for T12: guided while `radar_designated(shooter) == target && radar_lock(shooter) == LOCK_TRACK`,
  else inertial; guided LRM launch needs LOCK_TRACK at launch (12.6).

## A9 Sound pack v2, combat (2026-11-13)

- Generator `tools/sfx/gen_sfx_combat.py` (assetkit protocol, generator name `sfx_combat`, ORDER 51, own seed 9009;
  `ASSETS` = 16 WAVs + `sfx/README_combat.md`). Run `python3 tools/sfx/gen_sfx_combat.py [--check]` or
  `tools/build_assets.py --only sfx` (both sound generators; `--only sfx_combat` for this one). About 3 s,
  deterministic: `--check` identical twice (data md5 sum unchanged), validate 17 files 0 errors / 0 warnings,
  876.9 KB. Imports A5's helpers from `gen_sfx.py` (no edits to A5's files; A5's `--check` still identical).
- Files in `data/sfx/` (mono 22050 Hz 16-bit, every file peaks at -1 dBFS):
  loops `srm_growl` (2.0 s), `srm_lock` (1.0 s), `rwr_lock` (0.8 s), `rwr_missile` (1.0 s), `msl_motor` (1.5 s), all
  pass A5's seam check (tones whole cycles, noise steady-state periodic, pulse loops have the seam in silence);
  one-shots `msl_launch` (2.2 s) `msl_pass` (1.6 s, closest approach at 0.45 s) `lock_beep` `rwr_search`
  `rwr_launch` (0.5 s) `flare` (0.55 s = one burst of 2) `hit` (1.8 s) `kill` (1.0 s) `jet_pass` (3.0 s, closest
  approach at 0.6 s) `expl_air` (2.5 s) `no_msl`. Additions beyond DESIGN 15: `rwr_search`, `rwr_launch`,
  `expl_air`, `no_msl` (12.6 "NO MSL click"); all optional for T17. v1 + v2 = 32 sounds (au limit 64).
- **Everything T17 needs is in `data/sfx/README_combat.md`**: per-file suggested e_audio volumes (computed from a
  measured perceived level towards designed targets on A5's scale: cruise engine ~-18 dB, AB ~-11.5 dB, PULL UP
  ~-9 dB), the trigger rules, the growl/lock law, distance/doppler laws, priorities/ducking. Short version:
  - SRM: keep `srm_growl` + `srm_lock` looping at the -74 dB floor from init (as snd.cpp's engine); seeker signal
    s 0..1 -> growl vol `0.65 * db(-6 * (1 - s))`, pitch `1 + 0.25 s`; on lock growl -> floor, srm_lock 0.25.
  - Own radar lock (T11 `REV_LOCK` for the player): `lock_beep` 0.45 (no beep for the SRM lock: the tone is the cue).
  - RWR (T11 `radar_rwr(g, PLAYER_ID, &v)`): `v.nLocks > 0` -> `rwr_lock` loop 0.45 from position 0, stop at 0.
    First homing hostile missile passing `radar_rwr_missile_warn` -> `rwr_launch` 0.55 once, then `rwr_missile` loop
    0.60 0.5 s later; it replaces rwr_lock (muted meanwhile). Anti-fatigue: full for 2 s, then -4 dB, back to
    full when the nearest homing missile is < 2 km or a new one launches. PULL UP + rwr_missile: missile -3 dB.
  - `msl_motor`: nearest 3 missiles, vol `0.65 * min(1, 100 m / d)`, pitch `clamp(340 / (340 + range_rate), 0.6,
    1.6)`. `jet_pass`: trigger at time-to-closest-approach <= 0.6 s, miss < 600 m. `msl_pass`: hostile miss within
    200 m. `expl_air` 0.95 within 1 km then x 1 km / d, delayed d / 340 m/s (A5 `expl_far` for ground/sea hits);
    `kill` 0.75 0.35 s after a kill, `hit` 1.0, `flare` 1.0 per burst, `msl_launch` 0.95 (SRM pitch 1.04).
- Distinctness by design (register + rhythm): growl low continuous rough, srm_lock 1600 Hz continuous clean,
  rwr_lock 784 Hz slow pulse 2.5/s, rwr_missile 1568/1175 Hz warble bursts (6 notes + 0.14 s gap, 2/s),
  rwr_launch falling sweeps 2000 -> 1200 Hz (PULL UP rises), lock_beep 3 pips + held 2093 Hz, rwr_search two soft
  698 Hz blips. Soft timbres (sine + weak low harmonics), raised-cosine edges, consonant pitch set.
- Tone-to-masker check (build log, `mask ...` lines; 1/3-octave band vs A5's engine + wind at the suggested
  volumes): over cruise / over full afterburner at 700 kt: rwr_missile +16.6 / +12.5 dB, rwr_launch +18.1 / +14.1,
  rwr_lock +12.8 / +9.2, srm_lock +15.3 / +11.3, lock_beep +19.9 / +16.8, rwr_search +11.8 / +8.0, kill +16.6 /
  +12.6, srm_growl (near lock) +11.2 / +5.5 (searching floor 6 dB lower: just at the edge in full AB, intended);
  A5's PULL UP for reference +14.0 / +10.0.
- `tools/sfx/preview_combat.wav` (44 s, normal runs only, needs A5's files in data/sfx): A5 engine/wind with A5's mix
  law, master 0.7, plus search blip, lock beep, LRM launch, growl -> lock tone -> SRM launch, RWR lock, launch
  warning + missile warning with AB, 5 g break and flares, missile pass-by, stray missile motor with doppler,
  jet pass, air kill + SPLASH, hit, NO MSL (script in README_combat.md). Mix peak 1.21 before normalising.
- Not verified by ear (no listening on this side): the user/T17 should judge the growl character and the
  warning volumes in the first combat play-test; volumes are one table (`TARGET_DB`) in the generator.

## T11 (final) Radar, contacts, HAFU states, targeting input, RWR (2026-11-13)

(The API note for T12/T14/T15 is in the first `## T11` section above; it did not change.)
- **Files** (portable, replaced the T01 stubs): `src/radar.h/.cpp` (contacts, scan, ID, memory, dead, designation +
  lock, display latch, ContactView, RWR, events), `src/target.h/.cpp` (player's S short/long logic),
  `src/tests_radar.cpp`, `src/tests_target.cpp`. The header comments of radar.h / target.h are the reference.
  **No edit outside**: T08's session already calls `target_update` (step 2), `radar_tick` (step 8), `radar_reset` +
  `target_reset` (session_start), so game.cpp is untouched. New params: 20 keys, all in **`[radar]`** (target's
  selectRangeNm cycleSec autoReselect autoRedesignate too) -> integrator regenerates jet.ini.
- **Model** (DESIGN 11 numbers unchanged): pair (obs, id) scanned at 10 Hz on ticks with `(tick + obs*5 + id*7) % 12
  == 0` (the 10 player/enemy pairs of a 1 + 5 fight all have their own tick: <= 1 terrain_los per tick). Body frame
  = `flight_nose` (same as the HUD waterline). Volume +-60/+-45, Rdet 20 NM x (1 + 0.25 sin^2 aspect) (beam 25 NM),
  LOS via terrain_los. ID 1.5 s within 15 NM (timer from the first seen scan inside 15 NM, broken by leaving 15 NM or
  by MEMORY, not by single missed scans), instant within 5 NM. MEMORY after 0.5 s unseen, anchored at the TRUE
  position/velocity at that moment (the radar coasts 0.5 s: no symbol jump), NONE 6 s later (ID forgotten). DEAD the
  tick after AC_ALIVE clears (crash or kill), listed 2 s (`splashSec`) for SPLASH, its designations cleared at once.
  Same-side aircraft are never contacts (enemies don't track each other).
- **Lock**: designation + lock live in radar.* (shares the scan's LOS). Designate inside gimbal +-70 / 1.2 Rdet / LOS
  -> LOCK_TRACK at once (REV_LOCK = lock tone), else LOCK_MEMORY at once (TD dashed, dropped after 3 s). Lock test
  failing 1 s since the last good scan -> LOCK_MEMORY; 3 s more -> designation dropped (REV_UNDESIGNATE arg 1); good
  again in LOCK_MEMORY -> TRACK. A held lock keeps the contact tracked outside the scan volume (STT, up to 70 deg).
  `radar_designate` with the already designated id is a no-op (AI may call it every think).
- **Display latch (10.7.4)**: ContactView.state/flags are latched copies (>= 0.3 s per displayed value); appearance
  (NONE ->) and DEAD apply at once; flag changes from the player's S presses apply at once (target.cpp calls
  `radar_view_kick`), so the triangle/TD box answer the key without delay. AI/weapons read the sim values
  (`simState`, `radar_contact_state`, `radar_track`). `CVF_IDENTIFIED` switches together with the displayed state.
- **For T15**: `n = radar_views(g, PLAYER_ID, cv, 8)` per frame, `radar_view_pos(g, &cv[i], alpha, &p)` for the symbol
  position (tracked = the same lerp as the jet, MEMORY = extrapolated to the render time, DEAD = anchor), state ->
  diamond colour (DETECTED yellow, HOSTILE red, MEMORY dashed in the colour of CVF_IDENTIFIED), CVF_SELECTED =
  triangle, CVF_DESIGNATED = TD box (CVF_LOCKMEM = dashed), `stateAge` for SPLASH (state CS_DEAD) and fades,
  `range/altitude/closure/heading/bearing/offNose` for labels/locator (computed at the last tick; recompute from the
  render position if you need sub-tick smoothness). `hud.mapShowAll=1`: enemies the radar has not detected are not
  in radar_views; draw them from `g->ac` (alive, side red) with your own style.
- **For T14**: `radar_contact_state(i, PLAYER_ID)`, `radar_track(g, i, PLAYER_ID, &p, &v)` (1 tracked / 2 memory),
  `radar_geom(g, i, PLAYER_ID, doLos, &G)` (inVolume/inRange/inGimbal/inLockRange/los + angles; doLos costs one LOS),
  `radar_designate(g, i, PLAYER_ID)`; DEFEND cue for the player's lock on an AI: `radar_locked_by(i) & 1`.
- **For T17 (sound/HUD warnings)**: lock tone = REV_LOCK with obs 0; RWR "LOCK" + tone while `radar_locked_by(0)`
  != 0 (or `radar_rwr(g, 0, &rv)`: bearings for the map ticks, `map_draw_rwr(rv.lockBearing[k])`); launches at the
  player = REV_LAUNCH with id 0 (T12 calls `radar_rwr_launch`); `radar_rwr_missile_warn(type, launchDist, dist)` per
  homing missile for "MISSILE". Events log at info for DEAD/DESIGNATE/UNDESIGNATE/LOCK/LOCK_MEMORY/LAUNCH, debug for
  DETECT/IDENT/MEMORY/REACQUIRE/DROP; target.cpp logs `target: select|designate|break lock|kill ...` at info.
- **Verified**: `jet.exe -test radar` 36/36 rc=0 and `-test target` 28/28 rc=0 on VM A; `simh test radar|target` same,
  `simh test all` green for every area except T12's missile test (in progress, not T11's). Key lines: volume
  +-59 in / +-61 out, el +-44 in / +-46 out; aspect head-on/tail 19.9 in / 20.1 out, beam 24.9 / 25.1 (rdet 46300 m);
  volcano (summit 560,640, observer 16 km W, target running S 8 km E of it at 1500 m, 600 m/s): MEMORY exactly 60
  ticks after the last seen scan and 57 ticks after the real LOS loss, NONE 720 ticks later; ID 10 NM = 180 ticks
  after detection, 4.9 NM same scan, 16 NM never, closing through 15 NM 190 ticks; memory extrapolation error 1.95 m
  after 2 s (radar_track is 1 tick ahead by definition), view position 0.31 m; reacquire keeps HOSTILE; dead next
  tick + undesignate arg 2, removed after 240 ticks; latch: sim hostile +12 ticks shown +36, stress 60 s min display
  gap 36 ticks, AI-style designation flicker (233 changes) shown as 90 changes, min gap 36; RWR lock/bearing/timeout/
  launch/MISSILE rule; select order 2-3-1-4-2 by angle, frozen cycle across a 25 deg turn, rebuild after 2.5 s, dead
  skipped, 40 NM select limit; designate/move/break/designate-without-selection; gimbal loss: lock MEMORY 120 ticks
  after the last good scan (119 after the turn), dropped 360 ticks later, re-lock from lock MEMORY, lock held at 65 deg
  (outside the volume); kills with autoReselect / autoRedesignate / neither; player death clears; S end to end
  through pilot_update: tap = 1 short, hold = 1 long at the 36th held tick, no short on release.
- **Cost (VM A)**: radar tick with 6 aircraft over the island (every scan passes the cheap tests and runs a LOS):
  **51.5 us average per tick**, worst tick 233 us (gate 300); ~0.83 LOS per tick, ~60 us per scan, dominated by
  terrain_los on low rays across the island. No per-tick allocation; trig of the volume params cached per tick.
- **World size (T03b)**: no range changed. Radar ranges are not tied to the island: 20 NM (37 km) detection covers
  the 34 km island and stays well beyond the LRM head-on Rmax target (16-22 km); 40 NM select = the 75 km warning
  ring. If the play-test wants earlier contacts in the bigger area, `detRangeNm` is one key.
- **Deviations / interpretations**: (1) designating outside the lock limits gives LOCK_MEMORY at once (DESIGN only
  defines losing a held lock); (2) STT keeps the track outside the scan volume; (3) MEMORY anchor at the coast end
  (true position after 0.5 s) instead of the last seen scan, for symbol continuity (10.7.1); (4) DESIGN 11.4's
  "autoRedesignate=0 makes it re-lock" read as "autoRedesignate=1 re-locks" (default 0); (5) flag changes from S
  presses bypass the 0.3 s latch (user feedback; all automatic changes latch); (6) a crash also produces DEAD/SPLASH
  data (T15/T17 decide whether a crash shows SPLASH).

## A8 Weapons and effects art (2026-11-13)

- **Generator** `tools/art/gen_fx.py` (ORDER 60, after gen_jet): `python3 tools/build_assets.py --only fx [--check]`
  (~7 s, previews included). `--check` identical twice; outputs deleted + regenerated = md5-identical; validate 14
  files 0 errors (`lrm`/`srm` need `--class missile` standalone, see open issues). Previews `tools/art/preview/fx_*.png`:
  `lrm_views srm_views missile_lods rails tex over_bg blend_compare explosion_seq mock_sea mock_land` (mocks are drawn
  over T10b game frames `shots/t10b/new_lvl0.png` / `new_o03.png` with the engine's blend equations, 565 dithered).
- **Missiles** (original, no real markings; class `missile`; vertex colours white -> `mesh_draw_lit` like the jet;
  materials `body` + `fin` culled, `fin2` cull-none on LOD1; one texture `msl`):
  - `lrm.msh` (HUD "AIM-120"): 3.65 m, body r 0.090, off-white with a warm radome, invented steel-teal double band,
    long low mid-body strakes + small swept tail fins. `srm.msh` ("AIM-9"): 2.85 m, r 0.064, light grey, dark glass
    seeker dome, ochre band, small nose canards + cropped-delta tail fins with rolleron discs.
  - LOD0 **112 tris** (< 120 m), LOD1 **20 tris** (4-sided body + crossed tail planes; draw to ~600 m, DESIGN 9.1).
  - **Origin = rail point** = centreline at mid-length, nose +Z: draw at A1's `rail_N` tag with the jet's body matrix
    (`world = translate(rail tag) * body`, no rotation). rail_0/1 = SRM (wingtips), rail_2..7 = LRM. Fins in an X
    (45 deg): nothing touches a pylon/rail; SRM inboard tail-fin tip clears the wing tip underside by ~12 mm,
    LRM top gap to the pylon 3 cm (`fx_rails.png`). Tags: `nozzle` (0, 0, -L/2) dir -Z (motor glow + trail start),
    `nose` (0, 0, +L/2). Rail missiles only with jet LOD0 (< 300 m); 8 x 112 lit tris is a CPU cost (T18: LOD1 for
    rail missiles beyond ~60 m, or skip the 6 LRMs on enemy jets).
- **Textures, blend, loading** (all `data/tex/`, alpha 0 on every atlas cell border):

  | file | size | content | blend | load |
  |---|---|---|---|---|
  | `msl` | 128 | missile atlas (opaque) | opaque, lit | `tex_get(name, 0, 0)` |
  | `fx_fire` + `_a` | 256, 2x2 | **premultiplied** fire frames 0 flash, 1 fireball, 2 breakup, 3 dying + soot | **RB_PREMUL**, Z test, no Z write, **fog off** + manual distance fade | colour + `_a` without bleed, like `hud_font.cpp load_page` (`tex_get(IMG_ALPHA)` would bleed RGB into alpha-0 texels = halo); `RT_CLAMP`, `RT_NOMIP` (the engine's alpha-weighted mips are wrong for premul) |
  | `fx_smoke` + `_a` | 256, 2x2 | straight-alpha puffs 0 dense, 1 lumpy, 2 flat wisp, 3 thin torn; near-white, soft top-left light | RB_BLEND, fogged, no Z write | `tex_get(name, IMG_ALPHA, RT_CLAMP)` (mips fine) |
  | `trail` + `_a` | 64x256 | straight-alpha smoke ribbon, u across (alpha 0 at u = 0/1), v along, tiles in v | RB_BLEND, fogged, no Z write, cull none | `IMG_ALPHA`, wrap v, keep mips (far trails need them) |
  | `splash` + `_a` | 128 | straight-alpha water plume; bottom centre = water point | RB_BLEND, fogged | `IMG_ALPHA`, `RT_CLAMP` |
  | `flare` | 64 | white-hot core, yellow halo, 6 faint rays; black = nothing | RB_ADD, fog off | `tex_get(name, 0, RT_CLAMP)` |
  | `debris` | 64, 4x4 x 16 px | keyed metal shards (cells have a 1 px clear gutter) | RB_ALPHATEST, vertex-lit | `IMG_KEYED` |
  | `dot` | 16 | soft white point (additive) | RB_ADD, fog off | `RT_CLAMP` |

  Why premul for fire: `fx_blend_compare.png` - RB_ADD washes the fireball to white over sky/cloud and yellow over
  terrain; RB_PREMUL keeps orange fire + dark soot on every background (same T02 finding as the HUD). The fire
  texture also works as a pure glow with RB_ADD (RGB already fades to black). **Fading a premul sprite = multiply
  RGB and alpha by the same factor** (vertex colour (k, k, k, k), modulate both stages). Fog must stay off on
  premul/add fx (fog would add fog colour through ONE); fade them by hand: `1 - smoothstep(fogStart, fogEnd, w)`.
  Smoke/trail/splash textures are near-white: **the vertex colour sets the smoke colour and light**, e.g.
  `c = base * (ambient*0.75 + sun*0.55) / 1.05` with the light set (`lit_smoke()` in the generator).
  Atlas cells: cell i at x = (i % 2) * 128, y = (i / 2) * 128 (cloud_puffs convention), UV inset half a texel.
- **Recipes** (what the previews use; all render-only, own RNG; sizes are an arcade x1.5 `EXPL_SCALE`):
  - **Explosion** (1.5 s fire, smoke to ~6 s; 7 fire + 6 smoke quads + flash, within the Low 20-quad budget if
    the smoke count is 4): back to front = smoke, fire, flash.
    Fire billboards: one core (9 m) at the centre + 6 (5-8 m) delayed 0.02-0.25 s, offset 3-6 m, each life
    1.1-1.5 s; `f = age/life * 4`: draw frame floor(f) with weight 1-frac and frame floor(f)+1 with weight frac
    (two quads during the cross-fade); size = s0 * (1 + 1.4 * g), g = 0.35 + 0.65 * (1 - (1-u)^2); fade weight by
    1 - smoothstep(0.72, 1, u); random roll 0-360 deg and random u-mirror per quad (fire has no baked light).
    Smoke: 4-6 puffs from 0.25-0.6 s, life 5-7 s, size 8 -> 30 m (ease-out), rise 3 m/s and drift outward,
    colour (52,48,44) -> (118,116,114) over the first 60 %, alpha 0.88 * fade-in 0.35 s * (1-u)^1.3; roll only
    +-25 deg (baked top light), mirror u for variety. Flash: `flare` RB_ADD 42 -> 16 m over 0.16 s.
    Debris: 8-12 shards, 25-55 m/s, gravity, spin +-720 deg/s, 0.6-1.6 m, RB_ALPHATEST lit grey; optional tiny smoke
    puff behind 2-3 of them. Far rule: when the core fire quad is < ~5 px, draw one `dot` (4 px, orange) instead.
    Near hits: + the short screen flash of DESIGN 9.5.
  - **Missile trail** (camera-facing ribbon from the ring buffer): width 0.8 m at the head growing to 3.5 m
    (`w = 0.8 + 2.7 * (1 - exp(-age/1.2))`), alpha `0.85 * (1 - age/life)^1.5` (life 7 s LRM, 5 s SRM), **at least
    2 px wide on screen** (else far trails flicker/vanish), v = distance from launch / 12 m (texel-square at 3 m
    width), u 0..1 across, colour lit near-white (240,240,236) x light. Trail only while the motor burns, then the
    points age out. Motor glow: `flare` 1.4 m RB_ADD tinted (255,220,170) at the `nozzle` tag, `dot` 3 px when
    smaller (DESIGN: missiles mesh < 600 m, trails to 6 km).
  - **Flares**: `flare` 4.5 m (min 3 px), brightness flicker +-15 % per frame, fade out over the last 0.4 s of the
    3 s burn; short trail ribbon (9 points / 1.2 s, width 0.6 -> 2.8 m, alpha 0.8 -> 0) in light grey.
  - **Water splash**: 2-3 upright billboards (rotate about world Y only) 26-38 m tall, width 0.9 x height, bottom
    6 % below the sea plane, height ease-out 0 -> 1 over 0.6 s (scale y), alpha 0.95 fading 1.0-3.5 s, alternate
    u-mirror; plus one white `fx_smoke` cell 2 puff (vapour) rising. Colour lit white.
  - **Damaged jet**: `fx_smoke` puffs from the nozzle every ~0.1 s, (80,78,76) lit, 4 -> 10 m over 1.6 s, alpha 0.86
    fading; burning wreck = explosion fire frames 2/3 looped at low weight + the smoke recipe.
- **VRAM** (16-bit): fire 128 KB (no mips), smoke 171 KB, trail/splash/msl 43 KB each, flare/debris 11 KB, dot 1 KB:
  **~0.45 MB** total. Fire/smoke are 4444 on the Voodoo3: 16 alpha levels, the device dither hides the banding
  (`fx_over_bg.png` is 565-dithered; edges stay soft).
- **Gotcha for other art tasks**: `image.save_bmp(alpha=True)` with the default `alpha_bleed=8` overwrites the RGB of
  every texel with alpha < 255 near an opaque one (meant for cut-outs): it puts seams into soft straight-alpha art.
  gen_fx saves soft textures with `alpha_bleed=0` and fills only alpha-0 texels with the edge colour (`fill_clear`).
  Value-noise fBm with lacunarity 2 shows straight creases where all octaves' lattice lines meet; `fbm_r()` (rotated,
  lacunarity 2.13) avoids them for non-tiling textures.
- **Open / for others**: (1) T01/validate owner: add `("lrm", "missile"), ("srm", "missile")` to
  `validate.CLASS_PREFIXES` so the full-build validation of `data/` doesn't warn "no budget class" (warning only;
  `--only fx` uses the ASSETS class). (2) T17: premul loader + RB_PREMUL as above; the missile/fx section goes in
  `src/fx.cpp` (T17 part). (3) If far fire quads shimmer without mips, ask for a premul-correct (plain box) mip
  option in the engine instead of the alpha-weighted one.

## T12 Missiles, guidance, flares, terrain hits, stores (2026-11-13)

- **Files** (portable, replaced the T01 stubs): `src/missile.h/.cpp` (model + guidance, `missile_step`),
  `src/weapons.h/.cpp` (stores, firing, flares, live missiles, events, views), `src/tests_missile.cpp`, script
  `tests/t12/fire.scr`. **No game.cpp edit was needed**: T08's session already calls `weapons_reset` (session_start)
  and `weapons_tick` (step 7). Header comments of missile.h / weapons.h are the API reference. New params: `[lrm]` 33,
  `[srm]` 31, `[weapons]` 24 keys (every type gets the full key set; help text says which type uses a key):
  **integrator regenerates jet.ini**. `-test missile` 61/61 on VM A and in simh, table bit-identical on both;
  `-test determinism` 3/3 and `-test replay` 11/11 on VM A; `simh test all` green.
- **Model** (missile.h header): DESIGN 12.2/12.3 with one guidance law everywhere: PN on the missile's own aim
  estimate, `a = navN (omega x vm) + apnK navN/2 a_target_perp`, |a| <= gMax g min(1,(V/gRefSpeed)^2); gravity only
  in the speed equation (no path sag, nothing to compensate, as DESIGN's pseudo code); motor impulse integrated
  exactly per step and RK2 speed, so dt 1/30 and 1/120 agree (Rmax head-on 19389 vs 19376 m, SRM tail 2758 vs 2758).
  Phases LAUNCH -> MIDCOURSE (datalink or inertial) -> ACTIVE (LRM: est. distance < activeM; maddog after 1.5 s;
  SRM no-lock search 2 s in a 12 deg cone) -> TERMINAL -> LOST (memory 1.5 s, reacquire) -> unguided. Seeker LOS =
  `terrain_los` every losSec 0.1 s staggered by launch slot; gimbal/range checked every step; lost after lostSec 0.3.
  Fuze = closest approach of the step's relative segment < fuzeM after armSec. Ground = `terrain_segment_hit`
  (mesh or sea) behind a cheap HMAX reject (`missile_ground_hit`, usable by anyone). MF_MISSED = opening 0.5 s after
  arming **and** after the boost (a fleeing FAST target can open briefly during the boost). Deterministic, no RNG.
- **For T13 (fly-outs, honest numbers)** - the recipe `tests_missile.cpp:flyout()` uses:
  `missile_launch(&m, type, 0, 1, shooterPos, shooterNose, shooterV, &tv, MF_GUIDED, 0.0f)`; every step move your
  target model, fill `tv` = {valid 1, pos, vel (end of step), datalink 1 (lock assumed held), ab (1 while escaping in
  AB: SRM heat range x1.2), id 1}, `missile_step(&m, mp, &tv, terrain, 1/30)`, stop at `m.phase == MPH_DEAD`
  (`m.result`: MRES_HIT / GROUND / EXPIRED) or `m.flags & MF_MISSED`. TOF = steps x dt. The SRM seeker tracks only
  inside `missile_heat_range x trackMul` (aspect: 5 km nose-on .. 8 km tail, x1.05), so the fly-out itself caps SRM
  Rmax near the lock range (plus ~1.2 km of memory flight) - no separate clamp needed. **Cost on VM A: missile_step
  5.35 us/step at sea, 8.6 us over the island** (10 Hz terrain_los in TERMINAL + segment checks; Mac 0.04): DESIGN
  12.4's "1500 steps ~ 1.5 ms" does not hold on the VM (1500 steps = 8-13 ms). Already optimised once (inline math,
  cos memo, cached density: was 10.8/13.3). Suggest dlzStepsLow ~200-300, or keep 1500 only for High/T40.
  `simh test missile probe:TYPE:ASPECT:ESC:HZ` sweeps R and prints result/miss/lost per range (ESC 0 straight,
  1/2 break L/R, 3/4 beam L/R), `simh test missile table [-ini F]` prints the gameplay table with loaded params.
- **For T14 (AI)**: weapons reads `Aircraft.in.fire/flare/wpnNext` for every aircraft, or call
  `weapons_select(id, MSL_*)`, `weapons_fire(g, id)` (returns slot or -1), `weapons_flares(g, id)`. Guided LRM needs
  `radar_designate` first (weapons uses T11's radar as the sensor: TD in LOCK_TRACK = datalink, LOCK_MEMORY =
  inertial launch from `radar_track`, no TD = maddog). SRM seeker lock: `weapons_state(id)->srmLock` (refreshed every
  0.1 s, staggered). Stores: `weapons_set_stores(id, lrm, srm, flares)` (also the refill amount; default enemy 2+2,
  20 flares from `[weapons] enemy*`). Incoming threats: `weapons_threats(id, slots, n)` + `weapons_missile(slot)`.
  AI flares decoy at p x `aiFlareFactor` 0.25 (non-player dispenser).
- **For T15/T16 (HUD)**: `weapons_missiles(MissileView *, n)` (pos/prevPos for interpolation, `act`/`ttg` estimates:
  LRM ACT until ACTIVE then TTG, SRM TTG; -1 = none; checked: ACT 8.2 vs real 7.9 s, TTG 7.9 vs 8.8 s),
  `weapons_latest_at(PLAYER_ID, td)` = the slot for the ACT/TTG line, `weapons_state(PLAYER_ID)`: `sel`, `lrm/srm/
  flares`, `srmLock` (lock tone, circle slaved when `srmLockTd`), `srmNear` (growl), `railEmpty[8]` (rail shows a
  missile when 0; 0/1 SRM tips, 2..7 LRM pylons, A1 tags). MISSILE warning: `weapons_threats(PLAYER_ID, ...)` and
  per missile `radar_rwr_missile_warn(type, view.launchDist, dist)`.
- **For T17 (integration)**: event ring `weapons_event_count()/weapons_event(i)` (own read index, like world.h):
  WPE_LAUNCH, HIT (victim, damage = `[lrm|srm] damageEnemy/damagePlayer` 100/55, **weapons never touches hp or
  AC_ALIVE: mission applies DESIGN 14.1** - its `missileDamage` 34 can simply override), GROUND, EXPIRED, FLARE_HIT,
  SELFDESTRUCT (target lost AC_ALIVE: 14.2 "missiles on the old player self-destruct" is automatic), FLARES, DECOY,
  EMPTY ("NO MSL" click), ACQUIRED, LOST, PASS (hostile miss within passM 200 m: msl_pass sound), SELECT. One `wpn:`
  log line per event except SELECT (`[weapons] logEvents`). Refill: automatic when an aircraft comes back alive and
  when `session_respawns()` changes (runway restart). fx: `weapons_flares_view`, missile trails keyed by
  `MissileView.serial`, rails via `rail`. `tests/t12/fire.scr` = headless session smoke (maddog LRM, flares, D,
  SRM search): `-headless -start air -script tests\t12\fire.scr -exitafter 14 -log`.
- **Tuned values (vs 12.7 start)**: LRM dragK 0.9e-4 (1.1e-4), armSec 1.3 (0.8), fuzeM 25 (15), minSpeed 390 (250);
  SRM dragK 1.4e-4 (2.5e-4), inducedK 0.3 (0.05), armSec 0.65 (0.4), fuzeM 20 (10), minSpeed 470 (220), trackMul
  1.05. New knobs: apnK 1, launchSec 0.3/0.1, maddogSec 1.5, searchSec 2, searchConeDeg 12, lostSec 0.3, memSec 1.5,
  losSec 0.1, loftG 0 (off), trackMul, abLockMul 1.2; `[weapons] flareDragK` 0.0015 (so a flare falls behind the jet).
  Why: minSpeed ("out of energy") is what separates head-on from tail Rmax; the fuze radius must stay well above the
  guidance discretisation (33 m per fly-out step) or Rne becomes end-game noise and live/fly-out disagree.
- **Gameplay table** (co-alt 3000 m, both NORMAL 263 m/s, fly-out dt 1/30, escapes at gUse 0.80 to FAST from
  launch): LRM head Rmax **19.4 km** (16-22), tail **6.8** (5-8), Rne head **7.2** (6-9, worst break), Rmin **0.76**
  (0.6-1.2), beam 10.5, tail Rne 5.4, head Rmax at 8 km alt 32.7 / at 1 km 15.8; SRM head Rmax **6.6** (5-7), tail
  **2.76** (2.5-3.5), **Rne head 4.6 (target 1.8-3: deviation)**, Rmin **0.35** (0.3-0.5), beam 3.0. World size
  (T03b, 3x): ranges kept as 12.7 - they are sized against radar detection 37 km / ID 28 km and aircraft speeds, not
  the island; the 34 km island and 75 km area leave room for 20-30 km BVR approaches and 3 km-high volcano masking.
- **Deviations from DESIGN 12**: (1) APN term (apnK; 0 = pure PN): pure PN at its limit let the miss distance creep
  past the fuze vs a breaking target. (2) An unguided (gave-up) missile cannot fuze (ground/expiry only), otherwise a
  lost missile on an exactly head-on straight target still "hit" (SRM Rmax showed 12.6 km). (3) **SRM head-on Rne 4.6
  km, not 1.8-3**: with this model a breaking target stays roughly beam-on for the SRM's 4-7 s flight, so Rne(head)
  sits near the beam range; every lever that pushed it under 3 km (inducedK 1.0, gimbal 15-20, more drag) also
  killed crossing shots (beam Rmax 0-1.1 km) or the tail range. Balance consequence: nose-on SRM shots inside the 5 km
  lock range are mostly no-escape (flares are the defence) - T21 may shorten lockHeadM. (4) Decoyed missiles never
  reacquire; a decoy roll needs the flare inside that missile's gimbal (12.5). (5) Flare drag added (flareDragK).
  (6) TD in MEMORY launches an inertial guided LRM (12.6 sentence), T11's note says LOCK_TRACK needed: DESIGN wins.
- **Costs (VM A)**: weapons_tick **128 us** for 12 live missiles + 7 aircraft over the island (gate 500); flare
  rates over 2000 trials: SRM tail 0.692 (p 0.70), beam 0.550 (0.55), LRM beam 0.324 (0.30), LRM tail 0.456 (0.45),
  AI SRM tail 0.189 (0.175); all within +-0.05 absolute.
- **Open**: the full key set per missile type in jet.ini is verbose (could register per-type tables later). Final VM
  run (after T15 fixed a `near` build break in hud_tgt.cpp): build 0 warnings, -test missile 61/61, missile_step
  5.35 / 8.64 us per step (sea / island), weapons_tick 127.5 us.

## T15 Target symbology HUD + minimap contacts + kill feedback (2026-11-13)

- **Files**: `src/hud_tgt.h/.cpp` (replaced the T01 stub: symbology, minimap contact queue, kill records, `-hudtest
  contacts`), `docs/HUD_LAYOUT.md` (appended "Target symbology": every position, size, hysteresis band), screenshots
  `shots/tgt_<cockpit|chase>_<in|behind>_<600|768>.png` + `shots/tgt_pipe_<view>.png`, logs
  `shots/logs/hud_contacts600.log` / `768.log`. **Edits outside (allowed / to report)**: `src/hud.cpp` one dispatch line
  in `hud_test()`: `else if (!e_stricmp(name, "contacts")) { extern int hudtgt_test(void); r = hudtgt_test(); }`;
  `src/hud_map.h` appended `MAPC_KILL = 4` (kind) and `MAPF_NORADAR = 8` (flag); `src/hud_map.cpp` `draw_contact`:
  `MAPC_KILL` = grey-red X (two `hg_line_w` strokes, 7k wide) and `MAPF_NORADAR` = 40 % alpha.
- **Not yet wired (integrator, T17)**: `hudtgt_params()` must be called from main's `register_params()` after
  `hud_params()` (14 `[hud]` keys: tgtHideM tgtArrowNm tgtArrowMax tgtArrowInset tgtLocatorLen tgtSplashSec tgtFadeSec
  killDiamondSec killMapSec tgtOverlapPx tgtLabelHz tgtLabelOthers tgtMemAlpha mapNoRadar mapMissiles mapRwr), then
  `-dumpini`. Until then the code defaults apply (statics are initialised to the defaults). main already calls
  `hudtgt_draw(&g_game, &s_cam, alpha)` between hud_draw and map_draw: nothing else to do.
- **Live path** `hudtgt_draw(g, c, alpha)`: `map_set_auto_contacts(0)` (T15 owns the map contacts now), `hud_state_fill`,
  `hudtgt_build` = `radar_views(g, PLAYER_ID)` + `radar_view_pos(alpha)` -> `TgtSym` list, minimap extras (undetected
  enemies when `mapShowAll`, live missiles from `weapons_missiles` interpolated prevPos->pos, RWR ticks from
  `radar_rwr` -> `map_draw_rwr`), then the core `hudtgt_draw_list(hs, list, n, toMap)` (also used by the tests).
  **T17: the RWR map ticks are already drawn here** (don't add them again); "LOCK"/tones are still yours.
- **Rules as built** (DESIGN 10.5 + user decisions): hollow diamond (yellow unknown / red hostile / dashed 50 % in
  MEMORY in the colour of CVF_IDENTIFIED) drawn anywhere on screen in both views (cockpit too: game symbols, not
  clipped to the HUD frame), never inside the minimap square (an arrow at the square's edge points at it instead);
  TD box on the diamond; cockpit TD limited to the HUD frame (clamped on the frame edge + flashing 3 Hz + locator line
  from the waterline when outside), chase TD clamped to the screen edge + locator from the aim cross; selection
  triangle at a CONSTANT offset above the TD box size (DESIGN said 6 px above the diamond; constant so it never jumps
  when designated); labels at a constant offset below the TD box size; declutter 4 labels; overlap 12k/15k; arrows for
  20 NM, max 6; hidden within 400 m except the TD box. Every on/off decision has hysteresis (bands in HUD_LAYOUT.md).
- **Kill feedback (user request 2026-11-13)**: T11's DEAD entry starts a per-aircraft-id kill record (position, time =
  now - stateAge); the record, not the radar listing, draws: grey diamond (0xFFB4B4B4) at the kill position for
  `killDiamondSec` 3 s, fading over `tgtFadeSec` 0.8 s, "SPLASH" for `tgtSplashSec` 1.5 s (on screen only), and the
  minimap X for `killMapSec` 15 s. If the id becomes a live contact again (respawn) the grey diamond stops at once,
  the map X stays. A crash also produces DEAD in radar.cpp, so a crashed enemy also gets SPLASH/grey/X (T17 can filter
  by not passing those if it wants). The wreck is not followed (DEAD anchor is static; enemies' fs stops when dead).
- **Tests** (`jet.exe -fullscreen -res 800 600 -hudtest contacts -log`, also 1024x768; exit = failures, **0 at both**):
  overlap rule with hysteresis (8/11/14 px -> right, 16 -> below, back to 14 -> below, 11 -> right, 20 -> below),
  declutter (8 contacts -> labels = designated + selected + 2 nearest = 4); **boundary jitter** (12 cases: a symbol on
  each hysteresis boundary with +-1.9 px or range noise for 300 frames: screen edge enter/leave, minimap edge, HUD
  frame TD clamp, overlap 12/15, 400 m hide, 20 NM arrow range, range text at 9.45 NM, unit switches at 1 and 10 NM,
  17.5 NM: **0 changes each**); **radar pipeline** (real radar.cpp + target.cpp on a local Game, no terrain: player
  rolling/pulling with flight_step, 7 enemies: head-on ID, one at the 60 deg scan edge, a formation contact fixed in
  the player's nose frame, closing then killed at 12 s, a world-static point, a crossing one going to MEMORY then
  NONE, one behind; S taps/holds; 1500 frames at 50 fps over the 120 Hz sim = varying alpha; cockpit and chase):
  state flicker 0 (state aspects latched >= 0.3 s; geometric aspects only change with >= 3 px motion), range text
  never faster than 5 Hz and no A-B-A, max 4 labels, nothing outside the screen, **formation contact max 0.0022 px**
  (cockpit) / 0.010 px (chase) from its first position, integer centre never changed (no interpolation jitter);
  **world-static contact vs an independent pinhole projection: drawn centre within 0.500 px (rounding), float
  0.0001 px, frame-to-frame motion deviation 0.97-0.99 px (< 1)**; kill: grey diamond still drawn after the radar's
  2 s listing, gone after 3.8 s, map X present 10-15 s, gone after 15 s; coverage (hostile, memory, SPLASH, TD,
  triangle, locator, arrows) all seen. Review scenes: 4 shots per resolution (see HUD_LAYOUT.md).
- **Cost (VM A, 800x600, 600 set, CPU, 200 frames)**: 6 contacts: symbology 0.165 ms (18 quads) + minimap contacts
  +0.082 ms = **+0.25 ms**; 8 contacts: 0.222 ms (26 quads) + 0.106 = **+0.33 ms** (DESIGN gate 0.5 ms). Chase same.
  1024x768: 0.26 / 0.34 ms. With T07's 1.315 ms the HUD + map total is ~1.56-1.65 ms with 6-8 contacts, slightly over
  DESIGN 10.7.8's 1.5 ms (T18: the levers are T07's quads; T16 adds more).
- **Found, not mine (camera.cpp, T08 owner)**: the cockpit view matrix is built as `m4_look_lh(eye, eye + f, u)` with a
  UNIT forward (camera.cpp line ~191; also line 150). At world coordinates of 5-60 km the float sum eye + f
  quantises the direction to ~5e-4 .. 4e-3 rad, i.e. **0.3 to 2.5 px of frame-to-frame wobble of the whole 3D view and
  every conformal symbol against the fixed waterline/boxes** (found when my test camera copied the pattern: a contact
  fixed in the nose frame moved 0.18 px at 5 km altitude near the origin). Fix: `at = eye + f * 1000` (and scale the
  shake/buffet offsets added to `at` on lines ~194/199 by the same 1000), or build the view matrix from the
  quaternion directly. The chase camera (at 40 m ahead, eye 24 m behind) is fine.
- **For T16**: `hudtgt_td(&x, &y, &clamped, &half)` returns the TD box drawn by the last `hudtgt_draw_list` (rounded
  centre px, clamped flag, half size px; 0 = none). main draws hudtgt before hudwpn, so it is this frame's box. Off-nose degrees for the locator are computed from the render state (5 Hz cache).

## T13 Launch-zone solver (LIVE fly-outs), AI tables, honesty tests (2026-11-13)

- **Files** (portable, replaced the T01 stubs): `src/dlz.h/.cpp`, `src/tests_dlz.cpp` (`-test dlz`, `simh shots N
  [seed]`, `simh dlztable [-out FILE | probe T IA IS ID IV IH]`), **`data/dlz.tab`** (AI tables, 10.8 KB, generated by
  `tools/simh/simh dlztable -out data/dlz.tab`). simh.cpp already dispatched shots/dlztable (no edit). main.cpp
  unchanged: its `dlz_work(&g_game, 1500)` is clamped by `dlz_budget()` to the preset (Low 200 steps, High 1500).
  New params: 17 keys in **`[dlz]`** (integrator regenerates jet.ini). Header comment of dlz.h = the reference.
- **User request (2026-11-13) done: the zone is computed LIVE** from T12's `missile_step` (unchanged, dt 1/30, lock
  held, terrain every step) for the designated target, continuously, time-sliced per frame. One cycle = one geometry
  snapshot (radar track of the target: memory = the estimate), predicted ahead by `leadFrac` 0.5 x the last cycle time:
  **NOW** (one fly-out at the current range vs the straight target: hit now? + TOF + intercept point) -> **NOWESC**
  (one fly-out at the current range vs the worst escape: "can it still escape by turning now"; skipped when NOW
  missed or range > 1.3 x Rne) -> **RMAX** -> **RNE** (worst escape solved, other escapes checked with one fly-out at
  that range; one check rests per cycle, rotating) -> **RMIN** (every 2nd cycle). Each value is a bracket search seeded
  with the previous answer: a hit estimates the extra range from the coast law (12.2 speed equation), a miss the
  shortfall (closest approach / early-out), the next fly-out aims 0.6 % past it, so a converged value costs ~2 fly-outs;
  sure misses stop early (missile cannot close the gap even coasting, or gave up). Tables seed new targets and stand in
  (flagged `DLZV_TABLE`) until the first live Rmax.
- **Definitions / target model** (dlz.h): Rmax straight at current velocity; Rne = min over E = {break L/R: level turn
  that way until flying away from the shooter (launch LOS), then run; beam L/R: turn until perpendicular to the LOS}
  at `escapeGUse` x `flight_g_limit(V)` with flight.cpp's speed law to FAST (AB, induced bleed, lapse) and a **roll-in
  dead time 0.85 x bank/rollRate + tauG** (matched to flight_step flown by a pilot; without it SRM beam escapes beat
  the zone because under PN a later turn costs the missile more); Rne <= Rmax; Rmin straight. **Guided SRM needs the
  seeker lock** (weapons.cpp rule): beyond the aspect lock range / outside the gimbal a fly-out is a miss, so SRM Rmax
  is usually the lock range (head 5.0 km, tail ~2.75 km energy).
- **Published values**: Rmax = middle of the final bracket (<= 2.5 %; the verified hit if it stayed wide), Rne = the
  verified hit side (conservative), Rmin = smallest verified hit; smaller values publish at once, larger ones move 50 %.
- **API for T16 (weapon HUD)**: `const DlzView *v = dlz_view();` after main's per-frame `dlz_work`. Fields: `valid`,
  `target`, `type` (the selected weapon; switching weapon restarts), `range`, `closure` (this frame), `rmax/rne/rmin`,
  `srcMax/srcNe/srcMin` (`DLZV_LIVE` / `DLZV_TABLE` estimate), **`ready`** (first live Rmax: SHOOT allowed, 10.6;
  measured 0.13 s on the VM at 30 fps: the first cycle gets 2x budget and skips NOW), `inRmax/inRne/inRmin`
  (rmin < range <= rmax etc.), `nowValid/nowHit` (a shot at the current range hits a straight target), `nowEscValid/
  nowNoEscape/nowEscFlown` (still hits vs the worst escape; flown or implied), `tof` (NOW fly-out time of flight, -1 =
  out of range) for the "23 TOF" line, `leadDir` (unit world vector to the predicted intercept, recomputed every frame
  from the current state with the missile's average speed measured by the NOW fly-outs) for the steering dot,
  `worstEsc`, `cycles`, `cycleSec` (sim seconds of the last full cycle = live update period), `ageSec`, `memory`.
  Suggested SHOOT: `ready && inRmax` (optionally `&& nowHit`), flashing when `inRne` (or `nowNoEscape`).
- **API for T14 (AI)**: `dlz_ai_zone_ac(g, shooter, target, type, &z)` or `dlz_ai_zone(type, sPos, sVel, tPos, tVel,
  &z)` -> `z.rmax/rne/rmin/tof/worstEsc` (deterministic table lookup, ~32 node reads; aspect from the target's velocity,
  LOS elevation, shooter speed/altitude, target speed between NORMAL and FAST; SRM capped by the lock range of this
  geometry). Tables assume the nose on the target and the target escaping at `tableGUse` 1.0 (the player at full G).
  For a full answer of one geometry (tests, tools): `dlz_geom_from(g, shooter, target, type, &geom)` + `dlz_solve(&geom,
  warm, &z)` (synchronous, ~3-5k steps = 25-40 ms on the VM: not per tick). `dlz_set_escape_guse(gUse)`: T14/T17 set
  the enemy difficulty's gUse for the player's live zone (default `[dlz] escapeGUse` 0.80 = Normal); it does not
  touch the tables. DEFEND can use `z.worstEsc` (the escape that defines Rne = the best one for the target).
- **Tables**: aspect {0,45,90,135,180} x shooter {300,500,800 kt} x LOS elevation {-7,0,+7 deg} x target {NORMAL, FAST}
  x shooter altitude {1500,4500,8000 m}, 270 nodes per type, from the same solver. Loaded at session start from
  `data/dlz.tab` (**VM 4-18 ms**); key = the [lrm]/[srm]/[flight] + solver params text and a solver version
  (`"solver v3"` in tab_key: **bump it when the solver code changes**, then regenerate the file). Params changed ->
  `dlz_cache.tab` next to jet.ini, else the first lookup rebuilds synchronously (**VM 13.1 s**, Mac 0.08 s; 1.68 M
  steps) and writes the cache; never mid-session (F5: stale until the next session start, logged once). Accuracy vs a
  full solve at random off-node geometries (nose on target): Rmax mean |err| 6.2 % (92/98 within 15 %), Rne 12.9 %.
- **Verified**: `-test dlz` 39/39 rc=0 on VM A and in simh (`simh test all` green; VM `-test determinism` 3/3, `replay`
  11/11, `missile` 61/61 after the change). Honesty with LIVE shots (weapons.cpp at 1/120, target flown by flight_step
  with an escape pilot; 40 random geometries per type, over the sea; VM = Mac): 0.97 Rmax straight hits LRM 39/40 SRM
  34/34; 1.05 Rmax misses 39/40, 20/22 (guided launches; SRM shots beyond the lock range launch unguided search: 14/24
  of those still hit - no tone = no SHOOT, a bonus not a lie); **0.95 Rne vs each escape 100 %** (LRM 4x38, SRM 4x29);
  inside 0.9 Rne vs a Normal-AI-style defence (best escape after 0.7 s + one flare burst with p 0.5 at 3 km) 38/38,
  29/29; 1.1 Rmax vs the best escape misses 36/40, 20/22. Gameplay geometry (co-alt 3000 m, NORMAL): LRM Rmax head 19.4
  km, tail 6.8, Rne head 7.5-7.7 (roll-in; T12's instant-turn helper said 7.2), Rmin 0.80; SRM Rmax head 5.0 (lock),
  tail 2.75, Rne head 4.66, Rmin 0.35. Volcano: target 4.5 km behind the summit at 1500 m, range 11.5 km: open-sea Rmax
  16.1 km (SHOOT), with terrain 1.75 km (VM) / 4.05 km (Mac) (no SHOOT). Game bytes unchanged by 20 dlz_work calls.
- **Live performance (VM A, Low, LRM head-on 10 km, 30 fps = 4 ticks/frame)**: 200 steps/frame + 1.4 ms time cap ->
  **steady 1.45 ms per frame** (~8.3 us per step incl. target model; worst 3.1 ms = the 2x first-result frames);
  cycle 11 fly-outs / ~3.2-3.7k steps -> **full live refresh 0.66 s at 30 fps, 1.33 s at the 15 fps floor**; NOW/Rmax
  values are therefore at most one cycle old. **Lag behind a target breaking away** (published vs a synchronous solve of
  the same instant, every 0.5 s): VM Rmax avg 0.64 s (worst 1.3), Rne avg 1.2 s (worst 1.8), cycle 1.13 s there
  (longer fly-outs); Mac-speed (unbounded) 0.48 / 0.79 s. **T40 estimate** (Pentium M ~8-10x the VM CPU, High preset
  1500 steps/frame at 60 fps = ~1.4 ms/frame): ~90k steps/s -> a full cycle every ~0.04-0.05 s (20+ Hz), lag ~0.1 s.
- **Deviations from DESIGN 12.4**: live solver = coast-law-guided brackets + early-out instead of fixed 6-step
  bisections (same answers, ~3x fewer steps); Low budget 200 steps (+ time cap), not 1500 (missile_step is 5-9 us on the
  VM); NOW/NOWESC fly-outs added (user request); escapes with roll-in and "turn away from the shooter" (not a literal
  180); SRM lock rule in the fly-out; tables: 5 axes (+ target speed, shooter altitude) shipped as a data file instead
  of a 3-axis build at load (DESIGN's in-load build would be ~2.4 s on the VM even for 3 axes); table escape gUse 1.0
  (the player), live escape gUse 0.80 (the enemy).
- **Open / for others**: (1) **Flares vs the 90 % promise (T14/T21/T12)**: with one burst per missile the hit rate inside
  0.9 Rne is 100 % (and 0 decoys in ~33 bursts: p x aiFlareFactor ~0.08-0.2, worth a look by T12), but with flares every
  0.5 s inside 3 km it drops to LRM 22/38 (58 %), SRM 13-14/28-29 (~48 %): T14's flare doctrine / aiFlareFactor decides
  whether DESIGN 12.7's ">= 90 % vs Normal AI" holds. (2) T14's AI rolls with keyboard ramp rates (slower roll-in than
  the test pilot): re-run the honesty shots with the real DEFEND once T14 lands (`live_shot` in tests_dlz.cpp is the
  harness). (3) A SLOW target is not "straight at constant speed" (it bleeds to 150 kt): the zone is slightly
  pessimistic for it. (4) Live-vs-Mac float paths differ slightly (x87 PC24): e.g. volcano Rmax 1.75 vs 4.05 km; display
  only, tables come from the file. (5) Dev tools: `-test dlz cmp:T:G:E` (fly-out vs live shot for geometry G of the
  honesty set), `[dlz] trace=1` logs every fly-out.

## A9b RWR tones to the user's spec (2026-11-13)

- **What changed**: only the four RWR files of the A9 pack, same names and roles (generator
  `tools/sfx/gen_sfx_combat.py`, README `data/sfx/README_combat.md` regenerated; the other 12 combat WAVs and A5's pack
  are byte-identical). User spec (binding): waterfall 550/520/490/470 Hz 0.083 s steps 0.335 s once (new fighter
  radar in the threat table); lock 455/555 Hz alternating 0.25 s each, continuous; missile 455/555 Hz 0.10 s each,
  continuous. Near-pure sines (3rd harmonic -24 dB, 5th -36 dB), phase-continuous FSK with exact integer phase.

  | file | kind | samples | s | content | level | suggested vol |
  |---|---|---|---|---|---|---|
  | `rwr_search` | one-shot | 7387 | 0.335 | 550/520/490/470 Hz, 1847/1847/1847/1846 smp (0.0838 s) each, 5 ms ramps | -3.7 dB | 0.25 |
  | `rwr_lock` | loop | 22050 | 1.000 | 455/555/455/555 Hz, 5513/5512/5512/5513 smp (2 cycles = 505 whole sine cycles) | -3.6 dB | 0.30 |
  | `rwr_missile` | loop | 4410 | 0.200 | 455 then 555 Hz, 2205 smp each (1 cycle = 101 whole sine cycles), starts on 455 | -3.6 dB | 0.45 |
  | `rwr_launch` | one-shot | 11025 | 0.500 | 555/455/555/455/555 Hz, 2205 smp each, 4 ms attack, no end fade, ends on a whole cycle | -3.6 dB | 0.45 (= rwr_missile) |

  0.335 s = 7386.75 samples, so 7387 (0.33501 s); 0.25 s = 5512.5 samples, so rwr_lock alternates 5513/5512 such that
  each pitch gets 11025 samples per loop; one 0.5 s cycle alone would hold 252.5 sine cycles (not seamless), hence the
  1.0 s loop. rwr_launch is scaled with rwr_missile's gain (not normalised itself) so the joined tone has one level.
- **For T17** (all in README_combat.md "How to play them"; changes vs A9):
  - `rwr_search`: once for **every new enemy fighter radar** that first detects the player, rate limit 1 per 1 s
    (was optional, 1 per 4 s), skipped (not queued) while rwr_lock or rwr_missile play.
  - `rwr_lock`: unchanged rule (loop from position 0 while any enemy has an STT lock), vol 0.30 (was 0.45).
  - `rwr_launch` + `rwr_missile`: same sequence (rwr_launch once, rwr_missile loop from position 0 0.5 s later), both
    at vol 0.45 (was 0.55 / 0.60) and both -3 dB under PULL UP. au has no sample-exact chaining: start the loop when
    `au_playing(launchVoice)` turns 0 or at 0.5 s, whichever first; both ends sit at zero crossings, so a frame of
    gap is a tiny dropout, no click. Equivalent and simpler: skip rwr_launch and start rwr_missile at once.
    **New**: a second missile during the warning must NOT replay rwr_launch (two out-of-step copies would beat);
    keep the loop, reset the anti-fatigue level to full. Anti-fatigue (-4 dB after 2 s) unchanged.
  - The RWR tones are now continuous (no silent seam): stop rwr_lock / rwr_missile with 2-3 frames of falling
    `au_set` volume before `au_stop` to avoid a tick.
- **Distinctness**: all RWR tones at 455-555 Hz, below the engine fan band (750-1220 Hz); lock vs missile differ only
  by rhythm (2 vs 5 tones/s), the waterfall is one falling 4-step run; srm_growl (400-500 Hz) shares the register but
  is a raspy buzz. Targets raised (missile -10.5, lock -14, waterfall -15 dB; the 455-555 Hz band has more engine/wind
  energy) to keep A9's margins: `mask` lines over cruise / full AB at 700 kt: rwr_missile +18.1 / +12.9 dB, rwr_launch
  +17.8 / +12.6, rwr_lock +14.5 / +9.4, rwr_search +12.3 / +7.2 (PULL UP +14.0 / +10.0).
- **Verified** (Mac only, no VM): `python3 tools/sfx/gen_sfx_combat.py` twice: data md5 sum and preview md5 identical,
  `--check` identical; `python3 tools/build_assets.py --only sfx --check`: sfx + sfx_combat identical, validation ok
  (`python3 -m assetkit.validate` on the 4 WAVs + README: 0 errors, 0 warnings). Zero-crossing frequency per segment
  read back from the WAVs: exactly 455.00/555.00, 550.00/520.00/490.00/470.00 Hz. Click check (build log `click`
  lines, LSB): rwr_lock seam N-1 -> 0 d1 5980 (pcm[N-1] -5980, pcm[0] 0), switches max d1 5954, steady sine max d1
  6047; rwr_missile seam d1 5980, switches 5980, steady 6046; rwr_launch -> rwr_missile join d1 5980; d2 at the
  switches 1054 vs steady 941 (the slope change of a phase-continuous switch); A5 seam check ok on both loops. The
  preview now starts loops on their exact sample (ticks split at loop on/off times) and plays a one-shot's last
  sample: launch -> missile join in `preview_combat.wav` max step = steady max. Preview: 2 waterfalls (1.0, 2.2 s).
- Not verified by ear on this side: the user should listen to `tools/sfx/preview_combat.wav` (1.0/2.2 s waterfalls,
  17-21 s lock, 21.0 s on the missile tone in full burner).

## T14 Enemy AI + robot player + `simh aifight` (2026-11-13)

- **Files** (portable, replaced the T01 stubs): `src/ai.h/.cpp` (brains, profiles, robot hook), `src/tests_ai.cpp`
  (`-test ai`, `simh aifight N [seed] [diff]`, `simh aifight shots N [seed]`). simh.cpp already dispatched `aifight`
  (no edit). No other file changed (no weapons/missile fix needed, see flare decoy below). Header comments of ai.h /
  ai.cpp are the reference (states, steering law, terrain rules, flare doctrine).
- **Params** (integrator regenerates jet.ini): `[ai]` 20 keys + `[ai_easy] [ai_normal] [ai_hard]` 14 keys each = 62
  keys. **Sections are `ai_easy` etc., not `ai.easy`**: params snapshots (replays) split `sec.key` at the FIRST dot, so
  a dotted section name broke `params_restore` ("unknown key=ai.robot.crank"). Minor tuning constants and the robot
  profile are fixed in ai.cpp, because the registry is close to `PARAMS_MAX` 512: portable 299 + ~155 Windows keys
  (camera 29, scene 23, hud_wpn 22, fx 16, hud_tgt 16, hud 15, hud_map 15, snd 14, main 5) = ~455. **T17/T01: raise
  PARAMS_MAX in params.h before adding many more keys** (params_add silently stops registering when full).
- **Model** (DESIGN 13): think 10 Hz (perception from own radar/RWR/MAW + attack order, threats with one defendProb
  roll each, state machine PATROL / INTERCEPT / ATTACK_BVR(+CRANK) / MERGE / EXTEND / DEFEND (+ CRUISE for tests,
  DRONE for TRAINING), terrain look-ahead, collision check, watchdog); steer every tick: goal -> desired direction ->
  acceleration demand (V x kTurn x angle + gravity compensation) -> bank error + load factor <= gCap x nLim -> inverse
  of flight.cpp's up -> nCmd mapping -> **[pilot] keyboard ramps** (read live) -> PilotInput. Fire = one-tick
  `PilotInput.fire` after `weapons_select`; flares = `PilotInput.flare`. LRM: designate (radar_designate), LOCK_TRACK,
  nose within 20 deg, R < table Rmax x lrmFireFrac (> 1.2 Rmin); SRM: seeker lock on the target and R < table Rne x
  srmFireFrac (> 1.1 Rmin). Tables from `dlz_ai_zone` with the RADAR track (knowledge rule); every launch is also
  checked against the truth table (AiStats.envViolations, must stay 0).
- **Terrain/AVOID**: steering floor floorAglM 450 m over the surface sampled 1.5-10 s ahead along the velocity and the
  goal direction (climb angle demanded on the fly); AVOID = dive recovery check (pull at 0.9 x G limit after rolling
  upright must end recoverMarginM 250 m above the highest surface on the recovery path) or AGL < 300 m, full G limit,
  >= 1 s; masking (Hard) lowers the floor to 150 m. Area: beyond 55 km from the centre the goal turns to the centre
  (world_override_input is only applied to the player by the session). Ceiling 12 km. Mid-air collision avoidance
  (closest approach < 200 m within 4 s -> steer away, dead-on: lower id up / higher id down).
- **Flare doctrine (DESIGN 12.7 promise)**: one burst per defended missile (`flareBursts` 1) when it comes within 3 km,
  with ONE flareProb roll; a second burst (profiles with flareBursts > 1, the robot) only 1.5 s later (= rollSec).
  Measured with the real DEFEND as the target (`simh aifight shots 300 77`, Rne computed for the difficulty's gUse):

  | target | LRM inside 0.9 Rne | LRM Rne..Rmax | SRM inside 0.9 Rne | SRM Rne..Rmax |
  |---|---|---|---|---|
  | Easy   | 99.0 % (294/297) | 50.7 % | 99.0 % (296/299) | 66.9 % |
  | Normal | **93.9 %** (279/297) | 20.7 % | **96.0 %** (286/298) | 41.9 % |
  | Hard   | 92.6 % (275/297) | 3.9 %  | 88.5 % (261/295) | 25.3 % |

  Every miss inside 0.9 Rne was a flare decoy (Normal 18 LRM / 12 SRM): the escape never beats the zone.
- **Flare decoy check (T13 open issue 1)**: not a weapons bug. `tests_dlz.cpp` `g_start()` reseeds `s_g.rng` with 4242
  for EVERY shot, so the first burst's decoy roll is always the 7th draw of that seed = **0.803** (> any AI chance
  0.075-0.23): 0 decoys by construction; the stress variant's later bursts use later draws, hence its ~45 % loss.
  With per-shot seeds (tests_ai.cpp) the decoy rate matches the configured chance (Normal: LRM 18/141 bursts = 12.8 %
  vs p 0.075-0.11 + tail bonus; SRM 12/115 = 10.4 % vs 0.14-0.18; Hard SRM 34/245 = 13.9 %). Suggested fix for T13's
  harness (T13's file, not edited): seed s_g.rng per shot (e.g. from the geometry index).
- **Robot player** = the same AI with a fixed profile (ai.cpp `s_robot`: reactSec 0.5, gUse 0.9, LRM only inside 1.2
  x table Rne (a player who waits for the flashing SHOOT) and < 0.95 Rmax, SRM inside Rne, cooldown 3 s, 16+40+40
  stores, defend 1.0, 2 flare bursts, crank, knowsAll = the minimap). `simh aifight` flies it through the real
  session (`session_start(START_AIR)`, `session_set_autopilot(ai_robot_hook)`), waves of the difficulty (2-3-3 /
  2-3-4 / 3-4-5) spawned 22-28 km out at 4-6 km with the attack order, next wave 8 s after a clear, player missile
  damage 34 (3 hits), missile death -> SES_CMD_RUNWAY after 2.5 s (crash -> the session's own runway restart, takeoff
  flown by takeoff.cpp with A held), 3 lives, 900 s cap.
- **Kill/death table** (`simh aifight 100 1 D`, robot vs waves, 100 sorties each):

  | difficulty | complete / failed / timeout | robot kills / spawned | robot deaths | K/D | enemy hits / launches | avg enemy LRM launch |
  |---|---|---|---|---|---|---|
  | Easy   | 97 / 1 / 2   | 793 / 800  | 83  | 9.55 | 24+310 / 780+1344 LRM+SRM | 18.8 km |
  | Normal | 83 / 10 / 7  | 862 / 900  | 156 | 5.53 | 180+377 / 1378+1218       | 11.4 km |
  | Hard   | 26 / 50 / 24 | 892 / 1140 | 235 | 3.80 | 221+525 / 1415+1615       | 9.3 km  |

  All three: 0 terrain/sea crashes (enemies and robot), 0 stuck (longest without progress 40 s; watchdog 60 s), 0
  launches outside the zone (AI check and harness truth check), DEFEND on every reacted missile (Easy 565/565, Normal
  1037/1037, Hard 1487/1487 due/in DEFEND), react rates 0.51 / 0.83 / 1.00 vs defendProb 0.5 / 0.85 / 1.0; mid-air
  collisions 0 / 4 / 0 enemy-enemy, 0 with the robot. Easy fires at long range (18.8 km avg LRM), Hard waits (9.3 km).
- **Verified**: VM A `w98 build jet` 0 warnings; `jet.exe -test ai -log` **24/24, rc=0** (twice);
  `simh test ai` 24/24 and `simh test all` green. Key VM lines: turns 90 deg 5.9 s / 180 deg 11.3 s (0 overshoot,
  alt +-18 m); ramps 28800 axis steps, 0 outside the [pilot] keyboard ramps; n never above the G limit, max n/nLim
  0.952 Hard / 0.902 robot outside AVOID; LRM launches easy 22.0 / normal 17.3 / hard 13.7 km (truth Rmax 23 km x
  0.95/0.75/0.60), guided, Normal crank 46 deg; DEFEND 0.44 s after launch (reactSec 0.35 + think phase), FAST, one
  burst at 2988 m; defendProb 32/40 (0.85); terrain: level into the volcano, 60 deg sea dive from 2100 m, inverted 45
  deg dive from 1500 m, 30 deg dive at the volcano: no crash, min AGL 422-473 m; Hard masking entered, LOS blocked,
  missile expired; bounds 58 -> 55.9 km; sortie determinism (hash 6ecd5ec6 twice on the VM; Mac 0138690d: x87 vs SSE
  float paths, as T13 noted). **Timing (VM, 5 Normal enemies + robot over the island, 60 s): think tick (think + steer)
  avg 81 us, worst 269 us (3 of 3000 think ticks > 200 us), steer-only tick 21 us; average per enemy per tick 26 us**
  (gate 200 us). `simh aifight 100 1 D` ~10-15 s per difficulty on the Mac.
- **Not yet wired (T17)**:
  1. **Think stagger bug in game.cpp session_tick**: `((g->tick + (u32)i * 12u) % 12u) == 0` is `tick % 12 == 0` for
     every i (DESIGN's formula is degenerate): all enemies think on the same tick. Use `(g->tick + (u32)i * 5u) % 12u`
     (phases 5,10,3,8,1 for ids 1-5; the robot hook uses the same formula for its id).
  2. Spawning (mission_start / waves): place the aircraft in `g->ac[id]` (id, side SIDE_RED, flags AC_ALIVE, livery 1,
     hp 100, `flight_init(&fs, pos, hdg, flight_speed_target(THR_NORMAL, y))`, fs.onGround 0, gearPos 0, thr NORMAL,
     prev = fs, in.thr NORMAL), `world_reset_aircraft(id)`, raise `g->numAircraft`, then
     `ai_spawn(g, id, g->diff, 1, g->ac[PLAYER_ID].fs.pos)` (sets the difficulty stores with weapons_set_stores; the
     brain resets, so call it again when a slot is reused). `spawn_wave()` in tests_ai.cpp is a working example.
     TRAINING drones: `ai_spawn_drone(g, id, cx, cz, radiusM, altM)` (circuits clockwise, never fires or defends).
  3. Kills: mission clears AC_ALIVE / applies damage from WPE_HIT (tests_ai.cpp `fight()` shows the 34-damage rule).
     Nothing to tell the AI (dead brains are skipped by the session loop).
  4. Player's live zone vs this difficulty (T13 note): at session start `dlz_set_escape_guse(ai_profile(g->diff)->gUse)`.
  5. `-aifight N` in main.cpp (and remove the `{ "-aifight", "T14" }` row of s_notYet in game.cpp):
     `char n[16], s[16], d[16]; char *av[3] = { n, s, d }; e_snprintf(n, 16, "%d", g_args.aifight); e_snprintf(s, 16,
     "%u", g_args.haveSeed ? g_args.seed : 1u); e_snprintf(d, 16, "%d", g_args.diff >= 0 ? g_args.diff : 1);
     rc = simh_aifight(3, av);` (headless; loads data/island/jet.isl itself; exit code = failed checks).
  6. Headless robot sortie (T17 acceptance 2): after `session_start`, `ai_spawn_robot(g, PLAYER_ID)` and
     `session_set_autopilot(ai_robot_hook)`; when `session_respawns()` changes call `ai_spawn_robot` again; hold A
     (`keys.fast = 1`) while `takeoff_active(session_takeoff())`.
  7. Sound/HUD need nothing new: enemy locks come from radar_designate (REV_LOCK on the player = RWR), launches are
     WPE_LAUNCH + REV_LAUNCH, flares WPE_FLARES.
- **Debug**: `[ai] logStates=1` logs every state change (`ai: state id=.. A->B why=..`), `=2` adds a 1 Hz brain trace
  per AI (`ai: trace ...`: state, target, knowledge, lock, range, nose angle, table Rmax, altitude, speed, throttle,
  AVOID, floor angle, threats, stores, SRM lock). `simh aifight` prints `aifight: fight=...` per sortie, crash lines
  with state/masking, `aifight: left ...` for enemies alive at a timeout, summaries and `aifight: check` lines.
- **Deviations from DESIGN 13**: (1) section names `ai_easy` etc. (snapshot format, above); (2) DEFEND's escape class
  from the table's worstEsc, side = the shorter turn, heading relative to the MISSILE position (tables are symmetric:
  their L/R is arbitrary); (3) a missile triggers DEFEND once (again only within 6 km after a timeout); (4) MERGE
  overshoot -> EXTEND only when the target is not pointing at us (extending from a target on our tail is suicide);
  (5) INTERCEPT breaks off a hopeless tail chase after chaseSec 40 s (PATROL, re-engage when close/closing or after
  20 s) - without it a chase of an equally fast target tripped the 60 s watchdog; (6) RWR lock bearing steers a PATROL
  brain towards the locker (radar then finds him; no position knowledge); (7) mid-air collision avoidance and a
  steering floor added to AVOID; (8) AVOID may use the full G limit (gUse elsewhere); (9) enemy stores flares 20 per
  life (`flares` key); (10) the "match altitude within 1500 m" rule is a target altitude clamp, climbs/dives limited to
  +-34 deg.
- **Open**: (1) Hard SRM inside-Rne hit rate 88.5 % (flareProb 0.8): DESIGN's 90 % target is vs Normal only; T21 can
  lower Hard flareProb or aiFlareFactor if the user wants the promise on Hard too. (2) The robot is a strong player
  (K/D 3.8-9.6, ~66 % SRM hits); the user's play-test decides the real difficulty. (3) Sorties take ~650-750 s of sim
  time (takeoffs after deaths, 22-28 km wave starts): T17's `-exitafter 600` robot sortie may end before wave 3.

## T16 Weapon HUD: launch-zone circle, SHOOT, ACT/TTG, seeker, warnings (+ cockpit camera precision fix) (2026-11-13)

- **Files**: `src/hud_wpn.h/.cpp` (replaced the T01 stub: weapon HUD, `-hudtest zone`), `docs/HUD_LAYOUT.md` (appended
  "Weapon HUD": every position, band and the cost table), screenshots `shots/wpn_<cockpit|chase>_<case>_<600|768>.png`
  (32), logs `shots/logs/hud_zone600.log` / `hud_zone768.log`. **Edits outside (allowed)**: `src/hud.cpp` one dispatch line
  in `hud_test()`: `else if (!e_stricmp(name, "zone")) { extern int hudwpn_test(void); r = hudwpn_test(); }`;
  **`src/camera.cpp`** (orchestrator-approved, T15 finding): new static `look_dir(out, eye, dir, up)` (= m4_look_lh with
  at - eye = dir); the cockpit view and the `-camat` view are built from the forward DIRECTION (shake/buffet offsets
  are added to that direction exactly as they were added to `eye + f`, so their size is unchanged). Chase and wreck
  cameras untouched. Verified in `-hudtest zone` part "camera": real `cam_update` at 71 km from the origin, 600 frames
  of a manoeuvring jet with varying alpha: nose-frame directions project within **0.0001 px** (the old `eye + unit f`
  matrix: **1.71 px at 600, 2.19 px at 768**), a point 5 km out 0.0005 px.
- **API** (hud_wpn.h header comment = reference): `hudwpn_draw(g, c, alpha)` (main already calls it after hudtgt_draw) =
  `hud_state_fill` + `hudwpn_build(g, alpha, &hs, &in)` (radar TD + view pos, `hudtgt_td` box, `dlz_view` copy,
  `weapons_latest_at` missile act/ttg, `weapons_state` stores/sel/srmLock/srmNear, `radar_locked_by(0)`,
  `weapons_threats` filtered by `radar_rwr_missile_warn`) + `hudwpn_draw_in(&hs, &in)` (the core; tests feed a
  synthetic `WpnIn`). `hudwpn_tones(&t)`: `growl`, `srmLock`, `rwrLock`, `missile` = this frame's displayed states (sound
  requests). `hudwpn_reset()` new flight, `hudwpn_init()` optional prewarm, `hudwpn_ms_last()`.
- **Rules as built**: see HUD_LAYOUT.md. SHOOT = only what dlz publishes: `ready && Rmin < R <= Rmax && !(nowValid &&
  !nowHit)` (+ SRM: `srmLock == TD`); flashing additionally `R <= Rne && !(nowEscValid && !nowNoEscape)` and not in
  memory. Entering needs a 3 % band + 0.2 s + one completed dlz cycle; leaving is immediate. Breakaway X conservative
  (on at Rmin x 1.03, off at x 1.06 after 0.3 s). ACT/TTG: follows ceil(MissileView estimate) DOWN only, max 2 Hz, up
  only on a > 2 s re-estimate (T12's ACT estimate runs ~2.4x real time in the boost and ~0.7x in the coast: a real-time
  clock went 2 -> 3 in the live run); the ACT -> TTG switch happens on the frame the missile goes active.
- **Tests** (`jet.exe -fullscreen -res 800 600 -hudtest zone -log`, also 1024x768; exit = failures, **0 at both**):
  approach LRM/SRM through Rmax -> Rne -> Rmin with 2 % published-value noise and NOW/NOWESC one cycle late (0 frames of
  SHOOT/flashing outside the published zone; 3 changes, min hold 7.7-9.5 s; SHOOT 1.4-1.6 s after Rmax, flashing 1.7-1.8
  s after Rne, X before Rmin); hovering +-5 % around Rmax / Rne / Rmin for 40 s (0 violations, 10 / 9 / 3 changes, min
  hold 2.0 / 1.95 / 0.42 s, no SHOOT shorter than 0.25 s); countdown (noisy estimate +-0.3 s: 18 ACT changes 0.51-1.42 s
  apart, 0 increases, within +-1 s of the truth, TTG on the activation frame; TOF never faster than 2 Hz); SRM seeker
  (10 Hz raw lock flicker -> 3 displayed changes, min hold 0.98 s; tones = display; slaved within 0.08 px; outside the
  frame clamped + flashing); warnings (MISSILE on/off timing, 4 Hz duty 71/151, arrow at the right bearing +-1 px, LOCK
  5 Hz input -> 2 changes); geometry (ASE reach = r + aseOuter exactly, box gaps 10/13 px >= 7/8, readouts below the
  altitude box and clear of the circle, steering dot within 0.5 px in 5 cases, SHOOT side / chase bar side jitter <= 1
  switch); **live pipeline** (real radar + designate + `dlz_work` + `weapons_tick` + hudtgt + hudwpn on a local Game, no
  terrain, own `weapons_set_sensor` from radar): 0 violations (shortest displayed SHOOT state 0.20-0.47 s: an "off"
  segment, which the 0.2 s entry hold bounds); engagement 1: launch on steady SHOOT at 20.3 km (Rmax
  23.9) vs a straight target -> "17ACT" .. "1ACT" -> "9TTG" on the activation tick -> HIT; engagement 2: launch on
  flashing SHOOT at 7.5 km (Rne 7.9) with the target breaking (AB, roll + pull) -> HIT; third line back to TOF after.
  `-hudtest layout` (all four sets ok) and `-hudtest contacts` still exit 0.
- **Cost (VM A, 800x600, 600 set)**: cockpit worst (circle, ticks, SHOOT, readouts, MISSILE, LOCK, 3 arrows) **0.29 ms**
  avg (single-frame max 0.40), 46 quads, 2 draws; cockpit LRM in Rne 0.22 ms / 33 quads; chase 0.16-0.23 ms; 1024x768
  0.22-0.29 ms (3 draws: two atlas pages). The ring texture is generated once per set: **79 ms (600) / 88 ms (768)** on
  the VM, i.e. a one-off hitch at the first designation unless `hudwpn_init()` is called at startup (item 5 below). HUD total with T07 1.315 + T15 0.25-0.33
  + T16 0.22-0.29 = **~1.8-1.95 ms** (DESIGN 10.7.8 says 1.5: T18).
- **Not yet wired (T17)**:
  1. `hudwpn_params()` in main's `register_params()` after `hud_params()` (and T15's `hudtgt_params()`): 12 `[hud]`
     keys (wpnShootBand wpnShootOnSec wpnLatchSec aseDegLrm aseDegSrm aspectPointer wpnThreatRing wpnThreatMax
     wpnThreatAlpha wpnRingTex wpnNameLrm wpnNameSrm), then `-dumpini`. Code defaults apply until then. (PARAMS_MAX is
     512: check the count.)
  2. Sounds from `hudwpn_tones()` each frame in `sndc_frame`: `growl` -> srm_growl loop, `srmLock` -> srm_lock loop,
     `rwrLock` -> rwr_lock, `missile` -> rwr_missile (DESIGN 15). The radar lock beep on designate stays REV_LOCK (radar
     events), launches/hits from weapons events.
  3. **`weapons_set_sensor` must be wired to the radar** (T12 still uses its fallback "nearest in the nose cone"
     sensor): otherwise weapons may launch at a different target / lock state than the TD the HUD and dlz show.
     `live_sensor()` in hud_wpn.cpp (tests) is a ready template (TD = `radar_designated`, lock from `radar_lock`,
     memory from `radar_track`).
  4. Keep `hud_set_extras(.., weapon "", ..)`: hud_wpn draws the weapon line / chase weapon block itself. Pass the flare
     count (`weapons_state(0)->flares`) there if the minimap "F40" is wanted.
  5. Call `hudwpn_reset()` where `hud_reset()` is called (respawn) and **`hudwpn_init()` after `hud_init()`** (prewarms
     the ring texture: 79-88 ms on the VM, otherwise paid on the frame of the first designation).
- **Deviations from DESIGN 10.6**: VISUAL circle radius = aseRadius (a 20 deg circle is r ~183 and would cross both
  boxes); MISSILE at (320, 155) and LOCK at (320, 318) (DESIGN's 186 / 200 lie on the circle); threat arrows on a 160 ring
  (140 clipped the weapon line for a missile astern) and "largest" = full alpha, others 60 % (hg_* has no
  rotate+scale); SHOOT entry needs band + 0.2 s + a completed dlz cycle, flashing is suppressed while the TD is in
  memory, SRM SHOOT needs the seeker lock on the TD; ACT/TTG estimate-driven (see above); chase shows only the third
  line under the DLZ bar (range is in T15's TD label, Vc dropped); RNG below 1 NM in hundreds of ft; Vc in 10 kt steps;
  aspect pointer drawn thin (distinct from the ticks); SHOOT moves below the TD box near the top of the HUD frame;
  readout numbers in a right-aligned 4-digit column starting at x 396.
- **Open / for others**: (1) T07's right-hand pitch-ladder labels end at x ~395 and can sit next to the readout block
  (x 396..452, y 290..327 virtual) with ~1-2 px gap; a keep-out box for the block in hud.cpp's `exclude_boxes` (while a
  zone is shown) would make it clean (T07 successor / T18). (2) SHOOT overlaps the ring when the TD is near the
  boresight (same in the Hornet). (3) At 1050 (T40) the ring does not fit a 256 texture at r 60 (needs 268): it is
  drawn as 50 chords there (~+50 quads, fine on the T40 CPU; T18 could split the ring into 4 quadrant textures).
  (4) The HUD total is ~1.8-1.95 ms on the VM (T18 budget decision).

## T17 Combat integration: the playable M2 build (TRAINING + 1-wave SORTIE) (2026-11-14)

- **Files**: `src/mission.h/.cpp` (director), `src/tests_mission.cpp` (`-test mission`, 21 checks), `src/snd_combat.h/.cpp`,
  `src/fx.cpp/.h` (T17 section + 4 hook lines in T08's fx_init/fx_free/fx_tick/fx_draw), `src/scene.cpp/.h` (rail and
  in-flight missiles, props only in FREE, `scene_light`, `scene_draw_wreck`), `src/main.cpp`, `src/bench.cpp` (route
  `combat`), `jet.ini` (regenerated: 451 params + [controls], 0 value changes vs the old file), `play.bat` (comment),
  new `training.bat` + `sortie.bat [easy|hard]`, `tests/t17/waves3.scr`, `docs/PLAYTEST_M2.md`, `shots/m2_*.png`,
  bench reports `tests/t17/bench_combat_{chase,cockpit}.txt`. **Approved edits outside**: `src/params.h` PARAMS_MAX
  512 -> 768; `src/game.cpp` think stagger `(tick + i*5) % 12` (phases 5,10,3,8,1), `s_notYet` emptied (-aifight,
  -diff, -mode done), and an appended `session_runway(g, why)` (= the static ses_runway; declared at the end of game.h)
  so a missile death respawns through the same path as the crash flow (respawn counter: camera/HUD snap, weapons
  refill, robot); `src/tests_dlz.cpp` per-shot RNG seed (`4242 + 7919 * shot`).
- **Mission director** (mission.h header = the reference): runs only after `mission_set_director(1)` (main.cpp for every
  flight; `-test mission`). Off = mission_tick does nothing, so T14's `simh aifight` / `-test ai fight` (own spawns + own
  34-damage rule inside the harness loop) are unchanged: **T20: when the harness moves onto the director, drop its own
  damage/spawn code or the player takes double damage.** FREE never spawns. TRAINING: 3 drones (ai_spawn_drone) on
  circuits ahead of the runway threshold along the takeoff heading (12 km/1500 m r 3 km, 20 km +7 km right/3500 m r 4 km,
  30 km 7 km left/6000 m r 5 km), respawn 10 s after death, player stores topped up with weapons_set_stores (NOT
  weapons_refill: that resets the cooldown). SORTIE: wave after the takeoff handover (air start: tick 1), spawn = T14's
  spawn_wave recipe with g->rng, message "WAVE n: k BANDITS BRG xxx", next wave 8 s after the clear, MISSION COMPLETE 3 s
  after the last kill; waves = `[mission] sortieWaves` (default 1, read live). Damage: WPE_HIT enemy = destroyed; player
  -34 (`missileDamage`) unless AC_INVULN; hp <= 50 -> "DAMAGED" + `MissionStats.damaged` (smoke); hp <= 0 -> dead,
  runway 2.5 s later via session_runway. AC_INVULN while the takeoff runs + 3 s after. Lives 3 (0 = unlimited): the 3rd
  death = MISSION FAILED (the player still respawns: free flight with the remaining enemies; main offers RESTART
  MISSION). Score per 14.4. `dlz_set_escape_guse(ai_profile(diff)->gUse)` at the start (director on, not FREE).
  `MissionStats` (counters for sound/fx: `playerHits`, `kills`), `mission_msg*` ring (main -> hud_message),
  `mission_clock` (HUD clock, SORTIE), `mission_log_result` ("mission: result ..." at the end; main logs one at exit
  for a still-running combat flight).
- **Radar sensor (T16 item 3)**: nothing to wire: weapons.cpp's default sensor (`weapons_set_sensor(NULL)`) already IS
  the radar designation (`sensor_radar`; the weapons.h header text about a "fallback nose-cone sensor" is outdated, T12
  please fix the comment). Verified end to end: `-test mission sensor_radar_td` (aircraft 1 dead ahead, TD on aircraft 2
  15 deg right -> the LRM launches at 2, LOCK_TRACK).
- **main.cpp**: params `hudtgt_params` + `hudwpn_params` after hud_params, `sndc_params` after snd_params;
  `hudwpn_init()` after hud_init; `hudtgt_reset/hudwpn_reset/fx_reset/sndc_reset` at every flight start, `hudwpn_reset`
  with every hud_reset; `hud_set_extras(.., mission_clock, "", flares in TRAINING/SORTIE)`; mission messages forwarded;
  `-mode training|sortie -diff 0|1|2`; headless + combat mode = **robot sortie** (session_set_autopilot(ai_robot_hook),
  A held through every takeoff, ai_spawn_robot again on every respawn, quits "mission_end" when the mission ends);
  `-aifight N [-seed S] [-diff D]` = simh_aifight (log jet_test.log); pause menu RESUME / RESTART ON RUNWAY / RESTART
  MISSION (TRAINING/SORTIE: fresh session_start, last.rpl restarted) / QUIT; results panel 1.5-16.5 s after the end;
  **V look** (`look_camera`, post-processes cam_update's matrix, camera.cpp untouched): cockpit head turns to the TD
  target up to 120 deg (flight HUD + weapon HUD hidden while the blend > 0.3, target symbols stay), chase eye 30 m behind
  the jet on the target line, 0.15 s blend both ways. Dev: `-benchview side` (camera beside the jet looking back;
  screenshots of trails/flares/smoke; HUD off).
- **fx (T17 section)**: everything from the weapons/world event rings + mission counters, timed in sim ticks (pause
  freezes, replays identical), own RNG. Trails per MissileView.serial (points every 0.2 s while the motor burns, Low 16 /
  High 24 points, life 7/5 s, >= 2 px wide, within 6 km, faded within 80 m of the camera and when seen end-on - without
  that the own launch smoke stacked into a white cloud over the jet in chase view); motor glows; explosions (A8 recipe x1.5:
  core + 6 satellites with the 4-frame premul cross-fade, 4 smoke puffs, flash, 8-10 debris shards, far = orange dot,
  screen flash < 400 m; cap Low 4 / High 8, oldest replaced); kill = explosion + a falling burning wreck
  (scene_draw_wreck, smoke trail, impact explosion/splash); player hit = small burst + red flash; missile into
  sea/ground = splash/small explosion; self-destruct = air burst; flares (sprite + short ribbon); damaged smoke from the
  nozzle every 4 ticks with 85 % of the jet's velocity (smoke left still in the air passes the chase camera 24 m back
  within 0.1 s at 500 kt and was invisible). Smoke colour = A8's lit() with `scene_light`. `fx_combat_stats()` for the bench.
- **snd_combat**: README_combat rules: growl/srm_lock from `hudwpn_tones` (growl signal = nearest enemy angle / 12 deg),
  rwr_lock (muted under the missile warning), rwr_launch -> rwr_missile join (loop starts when the onset ends or at
  0.5 s; no second onset; -4 dB after 2 s, full when a missile < 2 km or REV_LAUNCH at the player; -3 dB under PULL UP),
  rwr_search on REV_DETECT(obs enemy, id player) max 1/s and never during lock/missile, lock_beep on REV_LOCK(obs 0)
  (not for the same target within 2 s), positional launches/flares/pass-bys (WPE_PASS)/explosions (delayed d/340, expl_far
  beyond 4 km, ground impacts, enemy crashes), hit / expl_near from MissionStats.playerHits, kill chime 0.35 s after a
  player kill, 3 msl_motor loops (nearest 3, doppler from the range rate, own missiles faded in after 2 s), jet_pass.
  `[sound] warn` / `combat` group volumes; continuous tones fade over 3 frames before au_stop.
- **Verified**: VM `-test all` result=0 (math..replay incl. mission 21/21, ai 24/24, dlz 39/39), simh `test all` rc=0,
  clang -Wall 0 warnings, VC6 /W3 0 warnings. Headless robot sorties on the VM: `-mode sortie -headless -exitafter 600`
  -> complete at 161 s sim (2 kills, 0 deaths, score 5305); 3 waves (`-script tests\t17\waves3.scr -exitafter 900`):
  Normal complete 670 s (9/9 kills, 1 death), Easy complete 598 s (8/8, 0 deaths), Hard FAILED 805 s (7/12, 3 deaths);
  `-aifight 3 -seed 7 -diff 1` rc=0. dlz honesty with per-shot seeds (VM): LRM 0.97 Rmax 39/40, inside 0.9 Rne vs AI
  defence 37/38 (1 flare decoy now happens), SRM 34/34 and 28/28; stress (flares every 0.5 s) LRM 31/38, SRM 18/28.
  simh aifight 100 x 3 after the stagger fix: K/D Easy 8.76 / Normal 5.22 / Hard 3.30 (T14: 9.55 / 5.53 / 3.80), 0
  crashes, 0 stuck, 0 out-of-envelope launches.
- **Bench** (`-bench 60 -benchroute combat -preset low`, 800x600, VM A; reports `tests/t17/bench_combat_*.txt`): chase
  **29.94 fps, missed 0.67 %**, draw 16.6 ms avg (aircraft 5.6, fx 3.7, hud 1.15, terrain 1.6, sea 1.6), sim 1.4;
  cockpit **29.93 fps, missed 0.78 %**, draw 12.0 ms (fx 3.7, hud 1.77). Both gate30 pass. Load: 7.7 missiles avg / 9 max,
  fx ~215 quads avg / 296 max, 4 explosions max, 2 contacts. The route (bench.cpp header): circle r 6 km at 3000 m over
  the sea, 4 drone-brain enemies (0.5 / 1.3 km ahead, 1.3 / 2.5 km behind), player fires SRM/LRM alternately at the two
  ahead, the two behind fire LRMs at the player, flares every 2 s, hits destroy + respawn 3 s later, the player "damaged"
  (hp 40, smoke) from 20 s; FREE mode, so player hits do nothing.
- **Screenshots** (`shots/`): `m2_hud_cockpit.png` (ASE circle, SHOOT, TD + slaved seeker, readouts, MISSILE, LOCK,
  trail, falling wreck), `m2_hud_cockpit_splash.png`, `m2_hud_chase.png` (SHOOT, TD + DLZ bar, LOCK, SPLASH + explosion),
  `m2_explosion_cockpit.png` (fireball), `m2_hostile_missile_chase.png` (enemy missile trail from behind, threat arrows),
  `m2_trail.png`, `m2_flares_smoke.png`, `m2_damaged_smoke.png` (side camera: trail, flares, damaged smoke plume).
- **Open / for others**: (1) T18: aircraft section 5.6 ms in chase (own LOD0 jet + parts + rail missiles, LOD1 rail
  missiles beyond 10 m already) and fx 3.7 ms are the biggest combat items; HUD cockpit 1.77 ms. (2) `-test ai` timing
  on the VM now shows worst think tick 1415 us (9 of 3000 > 200 us, avg 26 us per enemy per tick unchanged): T14/T18 may
  look at the rare spikes. (3) weapons.h header still describes a fallback sensor (T12 comment). (4) T20: T14's harness
  vs the director (above). (5) After MISSION FAILED the remaining enemies keep fighting while the player respawns (no
  despawn: clearing AC_ALIVE would show SPLASH); T19's results screen should end the flight instead. (6) Rail missiles
  use LOD1 beyond 10 m (crossed tail planes read as small X shapes at the wingtips in chase view; A8 could add a nicer
  4-fin LOD1). (7) The look camera is in main.cpp because camera.cpp is T08's; T22 may move it.

## T18 M2 performance pass + small HUD fixes (2026-11-14)

- **Files**: `docs/PERF.md` (new: method, M2 bench table, budgets, HUD decision, AI spikes, levers), `src/hud_wpn.cpp/.h`
  (MISSILE / LOCK slots, keep-out export, threat-arrow push, chase SHOOT-below under the bar text, `-hudtest zone`
  part keepout + shot scene `visual_warn`), `src/hud.cpp/.h` (ladder keep-outs merged + weapon-HUD boxes,
  `hud_keepout`), `src/hud_tgt.cpp/.h` (`hudtgt_td_label`), `src/tests_ai.cpp` (timing part), `src/bench.cpp` (combat
  route stores), `src/main.cpp` (profiler section `dlz` around `dlz_work`, 3 lines), `docs/HUD_LAYOUT.md` (appended
  "T18 overlap rules"), bench reports `tests/t18/{base,t18}_*.txt`, logs `tests/t18/logs/`, screenshots
  `shots/m2fix_*.png`, `shots/wpn_*_visual_warn_*.bmp`. **Outside the approved list (report)**: tests_ai.cpp (T14),
  bench.cpp (T17), main.cpp (T17), hud.h / hud_tgt.h / hud_wpn.h (appended declarations only). No ai.cpp change.
- **Bench (VM A, Low, 800x600, 60 s; PERF.md has the full table)**: low 29.97 fps / 0.22 % missed, volcano 29.97 /
  0.28, high 29.97 / 0.28, clouds 29.98 / 0.17, combat chase 29.94 / 0.61, combat cockpit 29.94 / 0.61: all pass
  (start of T18: 29.89-29.98, worst 1.11 % in combat chase). DLZ 1.06-1.15 ms avg (cap 1.4). HUD: flight routes
  0.77-0.81 ms, combat chase 1.20, **combat cockpit 1.80 ms > DESIGN's 1.5: accepted** (fps gate holds with draw
  12.2 ms in the cockpit; reasons in PERF.md). Sim 1.44 ms in combat. New bench section line `section name=dlz`.
- **Store counts 96/98/97 explained**: the screenshots came from the combat bench route, which set every aircraft's
  stores to 99 (`weapons_set_stores(id, 99, 99, 99)`). TRAINING was already right (mission.cpp tops the player up to
  `[weapons] lrmCount/srmCount/flareCount` 16/40/40 after weapons_tick in the same tick, so the HUD always shows the
  normal loadout); SORTIE shows the real loadout (weapons_refill: 16 / 40 / 40 per life). The bench now keeps the
  player at the loadout with the same kind of top-up (param pointers cached: a per-tick `params_find` cost 0.2 ms of
  sim), enemies keep 99 (not displayed). New screenshots read "AIM-120 16", "AIM-9 40", "FLR 40".
- **MISSILE / LOCK vs the TD cluster (both views)**: three slots each (home / left / right, HUD_LAYOUT.md "T18 overlap
  rules"); a text leaves home when the TD cluster (TD box, SHOOT incl. its gap to the box whenever the zone is shown,
  the TD label from `hudtgt_td_label`, the chase DLZ bar + third line) is within 2 px, takes the free side away from
  the TD (8 px clear) and returns once home is 8 px clear. `-hudtest zone` keepout: TD box over a 20-unit grid of the
  whole screen, 1653 frames per view, **0 overlaps**; LOCK boundary +-1.9 px noise 300 frames: **0 changes**.
- **Chase SHOOT below the TD box** (near the top) now sits under the DLZ bar's third line (it overlapped "3 TOF" in
  the first T18 shots): 174 frames below, 0 on the bar text.
- **Ladder keep-outs (cockpit)**: hg_exclude holds 6 boxes, so speed box + AOA/M/G stack and altitude box + VVI are
  one box each; the other four come from `hudwpn_keepout` (last frame, same view): readout block (fixed width, while
  the zone is shown), weapon line + VISUAL, MISSILE (while warned, not by flash phase), LOCK. Chase: speed + G,
  altitude, weapon block, MISSILE, LOCK. Ladder numbers touching them are skipped, lines cut. T16's "ladder labels
  1-2 px from the readouts" is covered by the readout box.
- **Threat arrows**: an arrow whose dart rectangle touches a keep-out (flight HUD boxes of this frame via
  `hud_keepout(box, max, view, time)`, the weapon texts, MISSILE / LOCK at their slots) moves outward along its
  bearing to the first clear radius (on at 0 margin, off at 4, per serial; max +60). Sweep every 0.5 deg, both views,
  zone and VISUAL cases: **0 hits** (exact dart triangle), push-boundary jitter 1 change. The 160 ring touched the
  altitude box/VVI (71-97 deg), readouts (109-136), weapon line/VISUAL (162-200), speed stack (241-255): those
  bearings are now pushed out a few px. T16's warn test (90 deg arrow at the ring) unchanged.
- **AI think spikes**: not AI work. The slow thinks were the ones where an enemy designates the player
  (`radar_designate` logs DESIGNATE + LOCK at INFO) and `-test` runs with log autoflush on, i.e. two `fflush`es to the
  Samba share inside the timed think; the rest were emulator stalls (not repeated by an identical second run).
  Found with a nanosecond-clock simh build (Mac: designation thinks 2-3.6 us with the log lines, ordinary without).
  The game loop never autoflushes. `-test ai timing` now times with autoflush off and runs the scenario twice (min per
  think): VM **avg 81.3 us, worst 154 us, 0 of 3000 > 200 us** (single runs 205 / 226 us); gate avg 200 / worst 500 us.
  `simh aifight 100 1 1`: **K/D 5.22 before and after** (845 kills, 162 deaths, 0 crashes / stuck / out-of-zone).
- **Verified**: VM build 0 warnings; `-hudtest layout|contacts|zone` exit 0 at 800x600 and 1024x768
  (`tests/t18/logs/hud_*`); VM `-test all` result=0 (`tests/t18/logs/test_all.log`); `tools/simh/simh test all`
  rc=0; clang -Wall clean on the portable files.
- **Open / for others**: (1) radar.cpp (T11) logs every DESIGNATE / LOCK / LAUNCH of every enemy at INFO: in play
  that is buffered (flushed once per sim second on the main thread) - consider LOG_DEBUG for enemy observers.
  (2) Biggest frame items for T22: own jet in chase 4.9-5.5 ms (CPU lighting of LOD0 + parts), combat fx 3.8 ms.
  (3) The MISSILE / LOCK slot rule considers only the designated target's cluster; other contacts' diamonds/labels
  can still pass under them (they move, the overlap is brief). (4) The bench's 30 s screenshot runs only reach
  shots up to ~20 s (the bench ends at its measuring time).

## A10 Front-end art (2026-11-14)

- **Generator** `tools/art/gen_front.py` (ORDER 70, after gen_hud): `python3 tools/build_assets.py --only front
  [--check]` (~3-4 s incl. previews). build_assets discovers `tools/art/gen_*.py` by itself (no registry to edit).
  Validator: 24 BMPs 0 errors / 0 warnings, no class prefix needed. `--check` identical; outputs + previews deleted
  and regenerated = md5-identical (45 files). Every texel premultiplied (RGB <= alpha, checked).
- **Everything is premultiplied, RB_PREMUL (ONE, INVSRCALPHA)**. Load colour + `_a` with image flags 0 and combine
  (exactly `hud_font.cpp load_page`), `RT_CLAMP | RT_NOMIP`, bilinear on. Vertex colour white for baked art; fade =
  (k, k, k, k). Text = the HUD fonts (`hud_atlas_<L>`, large font for items/titles, small for hints); no new font.
- **Logo** (`data/tex/logo.txt`, CRLF, format in its header). Canvas = **460 x 100 virtual**, title placement
  **(90, 30)** (top-left, 640x480 virtual; centred). Two tile sets drawn as quads, plus a one-texture small copy:

  | set | use when | native lines | canvas texels | tiles (texture size; used w x h; canvas offset) |
  |---|---|---|---|---|
  | `lo` | screen h < 700 | 600 | 575 x 125 | `logo_lo_0` 256x128 (254x125 @0,0), `logo_lo_1` 256x128 (254x125 @254,0), `logo_lo_2` 128x128 (67x125 @508,0) |
  | `hi` | screen h >= 700 | 1050 | 1006 x 219 | `logo_hi_0..3` 256x256 (254x219 @0/254/508,0; last 244x219 @762,0) |
  | small | pause / records header | - | 256 x 56 in `logo.bmp` 256x64 at v0 = 4 | 0.557 texels per virtual unit |

  Every tile: the used rect starts at texel (1, 1) and carries a 1-texel apron of neighbour content (no seams at
  non-native scales); UV = (1/texW, 1/texH)..((1+w)/texW, (1+h)/texH), no half-texel inset. Screen rect of a tile:
  k = h/480, f = h/nativeLines: (round(90k) + offX f, round(30k) + offY f, w f, h f). 1:1 at 800x600 (lo) and
  1400x1050 (hi); lo at f 0.8 for 640x480, hi at f 0.73 for 1024x768 (both look fine in the previews). VRAM (4444):
  lo 160 KB, hi 512 KB, small 32 KB; load only the menu's set and free it in flight.
  Look: own chamfered extended italic letters, basalt-steel face (chrome horizon), lava cracks rising from the
  baseline, chisel bevel (cool light above, warm lava light below), dark outline, soft shadow + bottom ember glow;
  "ISLAND INTERCEPT" silver with ember streaks. Reads over sky, cloud and sea.
- **Menu kit** `data/tex/front_<L>.bmp` + `_a` + `.txt` for L = 480 / 600 / 768 / 1050 (pick the largest <= screen h,
  like the HUD; one 256x256 page each, 128 KB in 4444). Sprite lines keep the hud_atlas sprite format (`name x y w h
  ox oy page`) + trailing `kind l t r b tint` (kind 0 sprite, 9 = 9-slice with border widths l t r b, 3 =
  horizontal 3-slice with caps l / r, height 1:1). tint 1 = white shape + baked 60 % black shadow (+1 px at 480/600,
  +2 at 768/1050), vertex colour tints it; tint 0 = baked colours. Margins per set (480 / 600 / 768 / 1050):
  - `panel` 9-slice, border **14 / 18 / 22 / 31** (source 2b+4 square): fill dark teal-black 80 % (premul 0,16,16,204),
    1 px black rim, thin green frame (55 %), bright green corner brackets, chamfered top-left / bottom-right corners.
  - `title` 3-slice (panel header strip), h 20 / 25 / 32 / 44, caps l 30/38/48/66 (ember notches; start the title
    text 4 virtual px after l), r 18/22/29/39 (fill fades out; base line continues).
  - `bar` 3-slice (selection highlight), h **19 / 24 / 30 / 42**, l 10/12/16/22 (bright chamfered edge), r 56/70/90/122
    (fade-out). Centre it vertically on the item (cap middle).
  - `keycap` / `keycap_hot` 9-slice border 5/6/8/11 (green frame / ember while capturing), `tab` / `tab_on` 9-slice
    border 6/8/10/13, `divider` 3-slice (tint green; fades at both ends).
  - tint sprites: `marker` (chevron, points +x, anchor = tip; ember), `arrow_l/r/u/d` (anchor centre), `check_off/on`
    (anchor centre), `seg` / `seg_off` (volume blocks 6x11 virtual, anchor top-left; 10 per slider, 12 apart),
    `white` 4x4.
  - 9-slices are 4444-exact (premul RGB = 16 m, alpha = 17 m) so the engine's Bayer dither can't turn the stretched
    middle into blocks and the corners match the middle; 3-slices the same except the fading right cap.
- **Colours** (straight ARGB): HUD green 0xFF40FF60 (unselected items, labels, frames), selected item text white
  0xFFF0F4F0 on `bar`, marker + section headers + "NEW RECORD" ember 0xFFFF8C28, capture text 0xFFFFC860, hints grey
  0xFF788880, panel fill baked.
- **Layouts used in the previews** (virtual 640x480; suggestions for T19, change freely): title: logo (90,30);
  menu `panel` 232..408 x 268..392; items FLY / OPTIONS / RECORDS / QUIT large font centred at x 320, cap tops
  284 + 24 i; `bar` 240..400; marker tip x 254; hint line small font y 452 grey. Options: veil black 35 %, `panel`
  96..544 x 52..428, `title` 100..540 at y 56 ("OPTIONS" large white), tabs y 86..104, rows cap tops 116 + 19 i
  (labels x 124, keycaps 400..520 x y-2..y+15, key names large white centred), INVERT PITCH row with
  `arrow_l` 416 / value 460 / `arrow_r` 504, hint y 406. Results: `panel` 120..520 x 40..440, stat rows large font
  (labels x 144, values right at 496) at 82 + 21 i, score breakdown small font at 202 + 18 i, SCORE / BEST large,
  CONTINUE on a `bar`. Pause: veil 40 %, `panel` 176..464 x 128..356, small logo 260 virtual wide at y 140, items at
  230 + 26 i.
- **Icon** `data/ashfall.ico` (named for the game; T24 needs no rename; TASKS' `data/jet.ico` is not produced):
  32x32 + 16x16, each 4-bit (standard VGA 16 colours, ordered dither, the classic Win9x look) and 8-bit; BMP DIBs
  with AND masks (no PNG entries, Win98-safe). Entry order 32/4, 16/4, 32/8, 16/8. Design: F-16-style jet climbing
  straight up out of a basalt volcano with a glowing crater. `rc`: `IDI_ICON1 ICON "data\\ashfall.ico"`.
- **Previews** `tools/art/preview/`: `front_title_640/800/1024/1400.png` (title over the T10b island shot
  `shots/t10b/new_o03.png`), `front_options_800.png`, `front_options_1024.png`, `front_options_sound_800.png`,
  `front_results_800.png`, `front_pause_800.png`, `front_atlas_<L>.png`, `front_logo_tiles.png`, `front_icon.png`.
  Composited like the engine: textures through r_d3d6's 4444 packing (RGB + Bayer, alpha truncated), RB_PREMUL,
  GPU-style bilinear for the scaled logo, frame to 565 with dither.
- **Open / for others**: (1) assetkit has no ICO writer: `ico_bytes()` in gen_front.py is a ~40-line packer kept
  local; toolkit owner may move it into `assetkit.image` (and teach validate.py `.ico`). (2) T19: the menus need
  HUD font + `front_<L>` + logo pages: at 1050 lines that is 3 + 1 + 4 textures; batch by page like hg_end.

## T16b Full weapon HUD in the chase view (2026-11-14)

- **User request** (M2 play-test): weapon HUD in the chase cam too. Done: the chase view now draws the cockpit's
  launch-zone circle with Rmax / Rne / Rmin ticks, range + aspect pointers, steering dot, breakaway X, Vc / RNG /
  TOF -> ACT -> TTG readouts, SHOOT (steady / flashing, same honesty rules), SRM seeker, VISUAL circle, weapon block,
  MISSILE / LOCK / threat arrows. `[hud] chaseAse` 1 (default; **new key, for T21a's `-dumpini`**); 0 = T16's compact
  DLZ bar (the bar is removed from chase by default: redundant with the circle).
- **Anchor**: the circle centre is the **steady-state aim cross** (`hud_chase_ase` in hud.cpp): where the 1 km nose point
  sits with the camera's `chaseDist/chaseUp/chaseLook` and no lag/shake at this frame's projection = x 320, y 205 at FOV 70
  .. ~214 at 85 (virtual), clamped 200..216, sticky whole pixel. It never moves with the camera lag, roll lag, shake or
  buffet (test: 0 moves in 600 frames with +-3 deg lag, +-6 deg roll lag, +-0.4 deg shake), only with the smoothed FOV in
  1 px steps (FOV sweep 70..85..70: max step 1, 1 reversal, within 0.75 px of the lag-free aim cross). The live aim cross
  wanders inside the circle in manoeuvres (up to 18 px at 600 in the test). Steering dot = angle NOSE -> leadDir (not the
  camera frame) on the screen axes. SRM seeker stays on the live aim cross (conformal).
- **Layout** (HUD_LAYOUT.md "T16b"): readouts at x 396 from 44 below the centre (clear of the chase altitude box and the
  right wing); chase MISSILE / LOCK home slots moved up to y 98 / 116 (were 120 / 138, inside the circle's reach); weapon
  line stays the chase block bottom right. The circle passes just above the jet (ring bottom ~y 265 virtual, fin tip ~268):
  `shots/chase_wpn_game*_*.png`.
- **Files**: `src/hud_wpn.cpp/.h` (shared `draw_ase`, chase path, chase readout keep-out, tests), `src/hud.cpp/.h`
  (`HudLayout.chAseX/Y0/Y1` appended, `hud_chase_ase`, chase gap check in `-hudtest layout`), `docs/HUD_LAYOUT.md`, shots
  `shots/chase_wpn_*`, logs `tests/t16b/logs/`, bench `tests/t16b/bench_combat_chase.txt`. Nothing outside the owned files.
- **Verified** (VM A): build 0 warnings; `-hudtest layout|contacts|zone` exit 0 at 800x600 and 1024x768; chase circle gap
  to the chase boxes over the whole clamp range 54 / 67 / 86 / 118 px at sets 480 / 600 / 768 / 1050 (min 5 / 7 / 8 / 11);
  zone part `chase` (stability, FOV sweep, geometry at both clamp ends: reach 85.00 = limit, readouts / slots 0 overlaps,
  steering dot 0.50 px, long range), chase runs of approach LRM/SRM (0 violations, 3 changes, min hold 7.7-9.5 s), hover
  rmax/rne, countdown (ACT -> TTG on the activation frame, third line drawn every frame), keepout (0 overlaps / 0 arrow
  hits in chase incl. the readouts); `-test all` result=0. Combat bench chase 29.94 fps, missed 0.56 %, hud 1.26 ms.
- **Cost**: chase weapon HUD worst case 0.364 ms avg (0.39 max), 51 quads (was 0.229 with the bar): within the 0.4 ms cap.
- **Longer ranges (T21a)**: the {2..40 NM} scale copes: Rmax 39 km -> scale 30, R 38 NM pointer pinned + flashing at
  355 until R < 29.4 NM; Rmax 70 km -> scale 40, Rmax tick 340 deg. A target beyond the scale (e.g. 35 NM with Rmax
  20 NM) keeps the pinned flashing pointer until it closes (the Hornet rule: scale from Rmax, not from range).
- **Open**: (1) SHOOT can overlap the ring when the TD is near the aim cross (same as the cockpit, T16 open 2). (2) T07's
  messages (y 168) and RETURN TO AREA (186) cross the circle's top in chase as in the cockpit (transient). (3) At the
  start of the run a stale play-test jet.exe was still running on VM A (blocked the link); it was gone before I built.

## T21a Balance from the M2 play-test: the player's AIM-120 flies ~2x as far (2026-11-14)

- **User verdict (binding)**: IR fine; the player's radar missile should go double distance; enemy weapons unchanged.
- **Missile kinds** (`missile.h`): `MSLK_LRM` (= MSL_LRM, `[lrm]`, the enemy LRM), `MSLK_SRM` (`[srm]`, everyone),
  `MSLK_LRM_PLAYER` (new `[lrm_player]`, full key set). `missile_params_of()` accepts a type or a kind;
  `missile_kind(type, player)`, `missile_kind_type(kind)`, `missile_kind_name()`. `missile_launch()` accepts a kind
  (m->type = the plain type, new `Missile.kind`); `missile_step` signature unchanged. **Rule everywhere: an aircraft
  with AC_PLAYER fires MSLK_LRM_PLAYER** (weapons_fire; `MissileView.kind` appended; weapons uses
  `missile_params_of(m->kind)` for stepping, decoy gimbal, ACT/TTG). fx.cpp keys trails/glows on
  `missile_params_of(m->type)` -> [lrm] dropSec/boostSec: identical on purpose ([lrm_player] keeps 0.15 / 3.0 s).
- **[lrm_player] vs [lrm]**: boostAcc 200 -> 400, dragK 0.9e-4 -> 0.7e-4, maxSec 45 -> 60 (the rest equal).
  Fly-out table (co-alt 3000 m, both NORMAL 263 m/s, `simh test missile table`), enemy LRM -> player LRM:
  head Rmax 19.4 -> **38.3 km** (tof 24 -> 40 s), Rne head 7.2 -> **14.0** (live solver with roll-in 7.6 -> 13.3),
  tail Rmax 6.8 -> 17.2, beam Rmax 10.5 -> 24.0, Rmin 0.76 -> 0.89 (solver 0.80 -> 0.93), tail Rne 5.4 -> 14.0,
  head Rmax at 8 km 32.7 -> 59.3 (maxSec-limited), at 1 km 15.8 -> 30.9. `-test missile` has 5 new checked rows
  (lrmp_*), `-test dlz` 4 new gameplay rows.
- **dlz**: DlzGeom.type may be a kind; `dlz_geom_from` / `dlz_ai_zone_ac` map an AC_PLAYER shooter's LRM to the
  player kind themselves; `dlz_ai_zone(kind, ...)` takes the kind. The live HUD zone flies the player's kind
  (DlzView.type stays MSL_LRM/MSL_SRM). **Tables: 3 kinds x 270 nodes** (data/dlz.tab 16216 bytes, key "solver v4",
  tab_key includes `lrm_player.*`); enemy LRM + SRM nodes are **bit-identical** to T13's file (540/540 compared). The
  player kind gets 12 Rmax fly-outs per node in table mode (6 left its end-game-limited misses unbracketed:
  12.2 km nodes that 1.03 x still hit) + a re-solve while no miss bracketed it. VM table rebuild after a params
  change 27.4 s (was 13.1), load 20.5 ms. **Delete `dlz_cache.tab` in the jet folder after tuning dlz code**: its key
  is the params text, so a cache from older code with the same params still loads.
- **AI**: robot (AC_PLAYER) LRM decisions + trace use the player kind tables; DEFEND's escape choice uses the
  incoming missile's kind (Threat.kind); enemy fire decisions unchanged (`-test ai` enemy launches 22.0 / 17.3 / 13.7 km
  as T14).
- **Radar**: new `[radar] playerDetRangeNm` 40 (head/tail; beam 50; lock 1.2x = 48 NM) and `playerIdRangeNm` 25 for
  an AC_PLAYER observer; enemies keep detRangeNm 20 / idRangeNm 15. selectRangeNm stays 40 NM.
- **Mission**: `[mission] spawnMinKm/spawnMaxKm` 22/28 -> **45/55** (spawn still kept within 60 km of the centre;
  area warn 75 / turn 90). TRAINING unchanged. tests_ai's aifight harness reads the same two keys.
- **Verified**: Mac `tools/simh/simh test all` rc=0, clang -Wall clean; VM A build 0 warnings, `jet.exe -test all`
  result=0 (missile 66, dlz 53, radar 44, target 28, ai 24, mission 21). Honesty (VM = Mac, 40 geometries, player LRM):
  0.97 Rmax hits 40/40, 1.05 Rmax misses 39/40, 1.1 Rmax vs best escape misses 40/40, 0.95 Rne 40/40 vs each of the 4
  escapes, inside 0.9 Rne vs AI defence 40/40; enemy LRM / SRM rows unchanged. Volcano still collapses the player
  zone (open sea 31.8 km -> 1.4 km VM / 3.5 km Mac). `simh aifight shots 300 77` player LRM inside 0.9 Rne vs the real
  DEFEND: Easy 99.3 / Normal 97.6 / Hard 93.2 % (was 99.0 / 93.9 / 92.6).
- **DLZ cost (VM, Low 200 steps + 1.4 ms cap)**: steady 1.45 ms/frame (unchanged), cycle ~5.9k steps / 8-13 fly-outs,
  **full refresh 1.00 s at 30 fps, 2.0 s at 15 fps** (was 0.66 / 1.33; test gate 0.7 -> 1.2 s), first live Rmax
  0.23 s, lag behind a breaking target Rmax avg 0.98 s (worst 1.94), Rne avg 1.56 s (worst 2.63), cycle 1.5 s.
- **aifight** (`simh aifight 100 1 D`, 900 s cap, robot uses the player LRM, waves at 45-55 km): K/D Easy 8.76 -> 9.48,
  Normal 5.22 -> 5.87, Hard 3.30 -> 6.20; complete/failed/timeout 98/1/1 -> 99/0/1, 84/13/3 -> 82/8/10,
  22/61/17 -> 31/8/61; avg sortie 644 / 692 / 715 -> 638 / 692 / 837 s; robot LRM shots 87/55/46 -> 157/215/718.
  0 crashes, 0 stuck, 0 out-of-zone launches.
- **Bench** (VM, `-bench 60 -benchroute combat -preset low`, --show): chase 29.91 fps / 0.95 % missed, cockpit 29.94 /
  0.61 %, dlz 1.03 / 0.85 ms (reports tests/t21a/).
- **Open**: (1) Hard sorties often outlast aifight's 900 s cap (longer approaches): a harness limit, not a game one.
  (2) Player-kind table Rne is optimistic vs full solves in some off-node geometries (+30 %; same as the enemy
  tables); only the robot's fire window and the first HUD frames (DLZV_TABLE, no SHOOT) use it. (3) HUD (T16b
  checked the circle scales): minimap default 20 NM shows a fresh wave (24-30 NM) only at the edge, and `[hud]
  tgtArrowNm` 20 gives no edge arrows for 20-40 NM contacts; consider mapRange 40 / tgtArrowNm 40. (4) DESIGN 11/14.3
  still say 20 NM / 22-28 km (orchestrator's file).

## T20 (early note) API for the menus: results + records (2026-11-14)

- All in `src/mission.h` (portable, no new files; the block "added by T20" at the end is the reference). No main.cpp
  edit by T20.
- **Results screen**: `MissionResults r; if (mission_results(&g_game, &r)) { ...; records_submit(&r); }` -> `r.valid`
  (1 = SORTIE ended), `outcome` (MIS_COMPLETE / MIS_FAILED), `timeSec` (mission clock), `wave/waves`, `kills/spawned`,
  `killsBvr`, `fired[MSL_LRM] hits[MSL_LRM]` (AIM-120), `fired[MSL_SRM] hits[MSL_SRM]` (AIM-9), `deaths`, `hitsTaken`,
  `parts.kill/bvr/death/time/accuracy` (score breakdown lines), `score`, `newRecord`, `prevBest` (-1 = none).
  Call `records_submit(&r)` once when the results screen opens (it saves records.ini on a new record and sets
  `r.newRecord`; repeated calls for the same mission are harmless). Never call it from tests/headless/replays.
- **RECORDS screen**: `records_load()` (re-reads records.ini; missing/corrupt = no records), then per difficulty
  `RecordRow row; if (records_get(d, &row)) ...` -> `score, outcome, date ("YYYY-MM-DD HH:MM"), timeSec, waves, kills,
  spawned, killsBvr, deaths, fired[2], hits[2]`. `records_diff_name(d)` = "easy"/"normal"/"hard".
  Optional "reset records": `records_reset(-1); records_save();`.
- Score is computed by `mission_score(const ScoreIn*, ScoreParts*)`; the live value is `mission_stats()->score`.

## T20 3-wave sortie, messages, score, records (2026-11-14)

- **Files**: `src/mission.h/.cpp` (sortie, score, results, records.ini; T20 block at the end of mission.h = the API
  reference), `src/tests_mission.cpp` (+ parts `score`, `waves`, `records`, `sortie_results`; 42 checks),
  `src/tests_ai.cpp` (harness part only: `fight()` now runs on the mission director). No new source files, so no
  Makefile edits; no main.cpp edit.
- **Sortie**: `[mission] sortieWaves` default **1 -> 3**. Wave sizes DESIGN 13.3 (Easy 2,3,3 / Normal 2,3,4 / Hard
  3,4,5; max 5 alive), "WAVE n: k BANDITS BRG xxx" (4 s), new **"WAVE n CLEARED"** (3 s, not after the last wave),
  next wave 8 s after the clear, MISSION COMPLETE 3 s after the last kill, MISSION FAILED on the last life (T17).
  New keys `spawnLaterMinKm/spawnLaterMaxKm` (waves 2+, default **45/55 = same as wave 1**): spawning later waves at
  35-45 km saved only ~5 % of robot sortie time (Easy 824 -> 766 s, Normal 847 -> 813, Hard ~1000 both), and 35 km
  would be inside the player's head-on AIM-120 Rmax (38 km) on spawn. Sortie length comes from the robot's BVR
  dance (enemy LRM -> DEFEND -> EXTEND -> re-approach), not from the approach. Attack order was already there (ai_spawn).
- **Score** (`mission_score(const ScoreIn*, ScoreParts*)`, pure): kill 1000, BVR +300 (launch > bvrKm 10 km), death
  -1500, accuracy hits/fired x 2000 (hits = player missiles that killed), time bonus max(0, ref - s) x 5 only on
  COMPLETE. New key **`timeBonusRefSec` 750** (DESIGN said 600 for 22-28 km waves; T14 robot sorties were 650-750 s
  then, now 820-1010 s, i.e. +~150 s for the 45-55 km approaches: 600 + 150). A good human (fires at 30+ km) should
  finish in ~450-600 s (estimate) -> 750-1500 bonus. **Deviation**: the total is clamped at 0. `mission: score ...`
  line (breakdown) before every `mission: result` line.
- **Results/records API**: see "## T20 (early note)" above (mission_results, records_load/get/submit/save/reset,
  records_set_path for tests). records.ini = base_path("records.ini"), CRLF, sections `[easy] [normal] [hard]`, keys
  score, date ("YYYY-MM-DD HH:MM" local, C `time/localtime/strftime`), outcome (complete/failed), timeTenths, waves,
  kills, spawned, bvr, deaths, lrmFired, lrmHits, srmFired, srmHits. Every key must parse and be in range (hits <=
  fired, kills <= spawned), otherwise the row is ignored (warn line); garbage files load as "no records". The best
  score of a SORTIE that ENDED (complete or failed) counts; a quit mission is never recorded. A new record rewrites
  the whole file. **T19**: menu.cpp's `menu_records_best()` stub reads `[sortie] easy=...`, which is NOT this format:
  replace its body with `RecordRow r; return records_get(diff, &r) ? r.score : -1;`; on the results screen call
  `mission_results(&g_game, &res); records_submit(&res);` once (res.newRecord / res.prevBest for "NEW RECORD").
- **T17 open issue 1 fixed**: `simh aifight` / `-aifight` / `-test ai fight` fly through the director
  (mission_set_director(1) around the fight, old value restored; sortieWaves and lives forced to 3, with_params
  restores them): no own spawns, no own 34-damage rule, no own runway command (the director's missile-death respawn).
  The harness only flies the robot and counts launches/hits/crashes/envelope; kills/deaths/waves/score come from
  `mission_stats()`. Cap 900 -> **1800 s** (`AIFIGHT_CAP_SEC`); fight lines and the summary now have `mission=` /
  `score=` and a "summary complete" line (avg/min/max mission time, avg score). Hits on the robot (`eHits`) now
  include hits ignored while invulnerable.
- **Robot sorties, Mac** (`simh aifight 50 1 D`, jet.ini as in the folder; sortieWaves forced 3):

  | diff | complete / failed / timeout | mission time avg (min-max) of completes | avg score | robot K/D | deaths (all missile) |
  |---|---|---|---|---|---|
  | Easy   | 50 / 0 / 0 | 824 s (579-1064) | 9024  | 12.9 | 31 |
  | Normal | 48 / 2 / 0 | 848 s (602-1184) | 9345  | 9.9  | 45 |
  | Hard   | 45 / 5 / 0 | 1013 s (635-1701) | 11568 | 8.0  | 73 |

  0 crashes, 0 stuck, 0 launches outside the zone, DEFEND on every reacted missile. (T21a with the old harness at a
  900 s cap: Hard 31/8/61.)
- **Robot sorties, VM** (`jet.exe -mode sortie -headless -diff D -seed S -script tests\t17\waves3.scr -exitafter
  1800`; the folder's jet.ini still has sortieWaves=1 until the integrator regenerates it): all 6 COMPLETE. Easy
  661 s / 940 s (scores 10198 / 7054), Normal 834 / 945 s (9136 / 8747), Hard 1180 / 1124 s (10427 / 11274); 14-27 s
  wall each. `-aifight 5 -seed 1 -diff D` on the VM: Easy 5/0/0 avg 879 s, Normal 5/0/0 avg 798 s, Hard 4/1/0 (completes avg 1172 s); all checks ok, rc=0.
- **Params to add to jet.ini** (integrator): `[mission] sortieWaves=3` (value change), new `spawnLaterMinKm=45`,
  `spawnLaterMaxKm=55`, `timeBonusRefSec=750`.
- **Verified**: Mac `simh test mission` 42/42, `simh test all` rc=0 at the time (it now fails `terrain file_facts`
  after A11's new jet.isl, see below), clang -Wall clean; VM build 0 warnings, `-test mission` 42/42 rc=0, `-test ai`
  24/24, `-test all` = 14/15 (only terrain file_facts).
- **Open**: (1) `-test terrain file_facts` fails on VM + Mac since A11 regenerated data/island/jet.isl (03:49; now
  597 chunks / 8 mats): T04's test expectation, not T20's. (2) Robot sorties are long (820-1010 s avg, max 1701 s on
  Hard): enemies spend up to 265-529 s in PATROL/INTERCEPT (an enemy that lost the robot); a human with the minimap
  can hunt them, but T21 may want the AI to re-acquire the player faster (e.g. give the attack order again when a
  wave has been alive > N s). (3) The waves test kills by clearing AC_ALIVE (0 kills, score from the time bonus
  only); real kills are covered by sortie_complete and the robot runs.

## A11 Island detail + the deferred A6 colour pass (2026-11-14)

- **What**: the island's look (docs/ISLAND.md: concept, landmarks, why) + vegetation / rock / village sprites with a
  small scatter renderer + base props. Wind from ENE drives it: dark windward laurel forest with cedar plantations,
  lighter leeward pastures and scrub, a farmland patchwork (plots, hedgerows, basalt walls) below 300 m, upland
  pastures, a brown heath belt, ash/scoria/sulphur on the cone, glowing lava only above 1700 m (cooled black flows
  below), black-sand beaches, palms on the low coast, 8 villages. Review sheet `tools/islandc/out/review_*.png`
  (top, classes, takeoff, base, low fields/forest/pasture/coast, 3 km, 8 km, slopes, hide-behind-the-volcano);
  in-game `shots/a11_*.png` (low_fields, low_forest, low_coast, base, takeoff, slopes, 3km, 8km; high_forest,
  high_fields = High preset).
- **Files (A11)**: new `tools/art/gen_veg.py` (veg_atlas.bmp 256x256 keyed + veg_atlas.txt), `tools/art/gen_props.py`
  (truck tug jeep lightmast windsock .msh, props.bmp, fence.bmp keyed), `tools/islandc/scatter.py` (-> data/island/
  veg.sct, format in its docstring), `src/scatter.h/.cpp`, `docs/ISLAND.md`, `tests/a11/` (bench reports + camat
  logs). Edited: `tools/art/gen_ground.py` (+ t_forest, t_scrub; other textures byte-identical),
  `tools/islandc/{classes,chunks,islandc,base,preview}.py`, `island/island.txt` (A11 keys, tints, scatter, props,
  fences). **Outside my files (minimum, please note)**: `src/scene.cpp` 2 lines (include "scatter.h" + the hook
  `scatter_draw(c);` right after `s_st.terrainTris = terrain_stats()->tris;` in scene_draw), `Makefile`
  (obj\scatter.obj in GOBJS), `src/tests_terrain.cpp` file_facts expects `nMats == 8` (orchestrator-approved).
- **islandc changes (A11 owns them)**: grass gets a material sub-class (classes.mcls: 8 forest -> t_forest, 9 scrub
  -> t_scrub; CLAS still says grass), pyramid over material classes, MATL now 8 = TERRAIN_MAX_MATS (foam stays 5,
  new materials 6/7 after it: no engine change). Vertex colour window of LOD >= 1 halved (chunks.vcolour
  `max(1, s >> 1)`) so plot/forest edges survive to LOD1-2. New keys (defaults = off): `moisture`, `forest`,
  `fields`, `scrub_belt`, `lava_cool`, `outcrops`, `sand_black`, `scatter`, `scatter_range`, `prop` (INST without
  collision box, drawDist 2.5 km), `fence` (runway-frame polyline -> double-sided keyed 2.4 m ribbon in the base
  LOD0 cells). New output `island/veg.sct` (in OUTPUTS, md5-checked by --check). The T03 "old island.txt rebuilds
  byte-identically" property no longer holds (the half colour window applies always).
- **Numbers**: jet.isl 10.0 MB (T03b 9.46; load +~0.5 s), 5372 batches (4029); land: grass 44.7 %, forest 29.6 %,
  scrub 8.9 %, rock 9.5 %, ash 4.9 %, lava 0.7 %, sand 1.8 %. veg.sct 1.2 MB, 74 365 sprites in 32 x 32 cells of
  1280 m (max 318/cell): grove 17 350, cedar 6 231, palms 1 234, tree 13 007, shrub 3 170, rock 3 507, hedge 20 290,
  wall 9 526, houses 50. base.trk: 19 instances (+5 prop meshes, 10 props), fence 3860 m = 340 tris. Climb-out
  corridor max terrain 46 m (unchanged; no sprite in the corridor or within 350 m of the paved base).
- **Scatter renderer** (`src/scatter.cpp`, header has the rules): lazy init on the first scatter_draw (file +
  `tex_get("veg_atlas", IMG_KEYED)`), one ALPHATEST fogged double-sided material (lodBias -2), batches of 1024
  sprites per r_draw_mem, cells by distance + frustum, density LOD by an instance hash (thinned sprites shrink out),
  grow-in over the outer 15 % of the range, camera-facing cards lean back by 0.5 x view elevation (max 35 deg),
  hedges/walls fixed along their yaw. Presets: Low 1600 m / 600 max / far density 0.35, High 3500 m / 2400 / 0.6.
  Own prof section + count `scatter` (bench lines `section name=scatter`, `count name=scatter`). In -camat mode it
  logs `scatter: camat frames=150 avg_ms= sprites=` (cost probe).
- **For T19 (main.cpp)**: optional one line next to `fx_params();`: `scatter_params();` (+ `#include "scatter.h"`)
  so `[scatter] on rangeM maxSprites nearFrac farDensity fadeFrac tiltK bright` reach jet.ini / F5. Without it the
  built-in defaults apply. `scatter_free()` may be called at shutdown (frees file + texture); not required.
- **Costs (VM A, 800x600 Low, --show, no VNC)**: benches (`tests/a11/bench_*.txt`): low 29.97 fps / 0.28 % missed
  (final binary 29.96 / 0.44), volcano 29.94 / 0.61, high 29.96 / 0.33, clouds 29.97 / 0.28, combat (chase) 29.94 /
  0.67: all pass gate15 and gate30. Scatter section avg 0.21-0.23 ms on low (max 150 sprites), <= 0.07 ms on the
  others (nothing within 1.6 km of the camera). Worst-case probes (-camat, `tests/a11/logs/camat_*.log`): 120-150 m
  AGL over forest / fields / coast / base = 127-173 sprites, 1.5-1.9 ms (~11 us per sprite, mostly the engine's CPU
  transform + 2 tris); High preset on the VM 752-912 sprites = 7.7-8.3 ms (T40 target). Terrain section +0.05-0.15
  ms (2 more materials: low 1.80 vs T18 1.65, volcano 6.20 vs 6.13); base section +0.5 ms on the low route (props,
  fences). Textures +~560 KB (t_forest, t_scrub, veg_atlas, props, fence).
- **Verified**: `python3 tools/build_assets.py --only ground,veg,props,base,island --check` identical;
  `islandc --check` OK (reader round trip identical, 0 problems); `validate.py data/` 151 files 0 errors (5 warnings:
  prop meshes + radome have no bldg name prefix, declared in ASSETS); VM build 0 warnings; VM `-test all` result=0
  (terrain file_facts mats=8, takeoff 69/69 liftoff 12.05 s min clear 2.29 m = T03b); `tools/simh/simh test all`
  rc=0.
- **Open**: (1) From 3-10 km the terrain shows only broad colour zones: the 80 m grid's coarse LODs (320-1280 m cells)
  can't carry 160-480 m plots; real detail at altitude would need a macro/detail texture pass in e_terrain (engine
  change, not done). (2) Groves read a little lighter than the canopy texture under them at 150 m. (3) The minimap
  still colours by game class (forest/scrub = grass). (4) A dedicated scatter-heavy bench route (e.g. 150 m AGL over
  the windward forest) would make the scatter cost visible in benches (bench.cpp is not mine). (5) Sprites have no
  collision / LOS (by design, <= 18 m).

## T22 Visual polish: vapour, contrails, shadows, cloud fly-through, explosions, smoke columns + aircraft/fx speedups (2026-11-14)

- **Files**: `src/fx.cpp/.h` (T22 section at the end + hooks in T08's flame and T17's combat code), `src/scene.cpp/.h`
  (T22 fast aircraft path, shadows, cloud fly-through factor, wreck LOD), `src/camera.cpp` (explosion shake only),
  `docs/PERF.md` (appended), scripts `tests/t22/*.scr`, bench reports `tests/t22/*.txt`, screenshots `shots/t22_*.png`.
  No art changed (shadow planform is rasterised from jet.msh LOD1 at init: `t22_jet_shadow` texture, no generator).
  **No main.cpp change needed.** Chase geometry (`chaseDist/chaseUp/chaseLook`) unchanged, `look_dir()` untouched.
- **jet.ini**: 42 new keys (defaults are the tuned values; regenerate with `-dumpini` when integrating):
  `[scene] acFast shadow shadowAglM shadowAlpha shadowSeaK shadowSunK shadowDistM wreckLodScale shadowLiftM`,
  `[fx] vapour vapourGFrom vapourGFull vapourLife vapourAlpha vapourWidth vapourWidthEnd vapourDistM vapourHumidAltM
  vapourSheet vapourCone vapourConeMachW contrail contrailAltM contrailAlpha contrailWidthEnd contrailDistM
  contrailSecs explScale explLayers burnDebris column columnSecs columnRise windDeg windMs cloudHaze cloudWisps
  wispAlpha flameDiamonds diamondBright shakeExplDeg`, `[camera] shakeExpl`. Low/High differences are built in
  (fx_hi() = scene preset "high"); keys with 0/-1 defaults mean "preset".
- **Effects** (all render-only, own RNG, sim-tick timing):
  - Wingtip vapour: ribbons from both tips (rail_0/1 tags, 1.2 m aft) at n 5 -> 8 g (or the sim's vis.vapour), x
    humidity (x0.4 at 9 km), life 0.6 s, Low 12 / High 24 points, jets within 1.5 km; faded near the camera only
    within 3-12 m so the chase view sees them stream past. Wing-top vapour sheets (6 -> 8.5 g, a blanket rising aft
    over each wing) and the transonic vapour cone (bell around Mach 1.0, half width 0.035) use the trail texture
    with a random v offset per frame (flowing). `shots/t22_vapour*.png`, `t22_vapour_cone*.png`.
  - Contrails above 8 km (fade-in 600 m below): from the nozzle with a ~0.15 s gap, Low 24 pts / 6 s, High 48 / 12 s,
    1 -> 12 m wide, drawn to 15 km, sunlit white (not lit(): that matched the haze colour and vanished). In the chase
    view the own contrail is behind the camera (correct). `t22_contrail_side.png` (bench high route, side camera),
    `t22_contrail_demo.png` (combat route with `-script tests\t22\condemo.scr` = contrails from 2 km, demo only).
  - Shadows: planform of jet.msh LOD1 (64x128 alpha texture), 3x5 grid projected **along the sun** onto the real
    surface (two iterations vs world_surface), RB_SHADOW (dest x (1-a)), zBias 2, 0.5 m lift, alpha 0.6 fading out
    by 300 m AGL, x0.45 on the sea, within 2.5 km. All alive jets + FREE props. With the sun at az 230 / el 40 the
    shadow lies ~1.2 x height to the NE: in the chase view it shows on the ground roll / at liftoff and leaves the
    screen above ~10 m (realistic; `[scene] shadowSunK 0` = straight down if the user wants it as a height cue).
    `t22_shadow_runway_parked.png`, `t22_shadow_liftoff1/2.png`.
  - Cloud fly-through: scene.cpp computes `scene_cloud_near()` (0..1 from h + 0.8 x cloudFadeM to 0.45 h, puffs
    treated as 0.8-high ellipsoids); fx draws a full-screen haze (up to 0.7, after all 3D fx, before the HUD) and
    6 / 12 cloud wisps on a 140 m world lattice (parallax). The puff itself still fades out near the camera (T08).
    `t22_cloud1..4.png` (-camat 1000 / 600 / 380 / 150 m from puff 9 at -12134, 2435, 4276).
  - Explosions: `[fx] explScale` 1.7 (T17 constant 1.5); full kills/crashes add 3 / 5 secondary blooms 0.25-0.6 s late,
    every first-fireball quad turns into a dark soot puff where it dies (Low 4 / High 7; "fireball -> smoke"), and
    3 / 5 burning debris pieces (fire head + dark smoke ribbon arcing down, stop at the ground). Camera shake from
    near explosions (0.8 deg at 0 m, gone by 700 m, decays in 0.6 s) and 1.2 x on hits to the own jet:
    `fx_cam_shake()` -> camera.cpp (`[camera] shakeExpl`). `t22_expl1..5.png` (crash on land, wreck camera).
  - Smoke columns: where a falling wreck hits the ground / sea and at every crash site: puffs every 0.8 s (High
    0.5) rising 11 m/s and drifting with `windDeg/windMs`, 14 -> 70 m (High 85), life 12 s (High 15), dark at the
    base, for 20 s (High 35; half on the sea, lighter steam colours there, no fire), fire at the base on land for the
    first ~half; caps Low 2 / High 4 columns. `t22_column1..3.png`.
  - Afterburner: 3 shock diamonds along the flame (glow texture, in the same draw as the nozzle glow), only above
    25 % AB. `t22_vapour_cone2.png` shows the flame.
- **Speedups** (details + numbers in PERF.md T22): `[scene] acFast` (own CPU lighting of camera-facing triangles only,
  one batch per material for jet + parts + rail missiles: aircraft 5.27 -> 2.84 ms on the low route); fx frustum
  out-codes + per-ribbon conservative pre-pass + puff cos/sin/lit colours at spawn (combat fx 3.89 -> 2.46 ms);
  wrecks at coarser LODs (`wreckLodScale` 0.3). `ribbon()` (T17) is now a wrapper of `ribbon2()` (per-point alpha,
  near-fade range, fade-in, min px, culling); `qb_bill()` culls before its trig; `PUFF_MAX` 256 with `s_puffCap` Low
  128 / High 256. bench `ac_tris` now counts only the drawn (camera-facing) triangles.
- **Verified** (VM A, 800x600 Low, `--show`, no VNC): VC6 /W3 **0 warnings**; `jet.exe -test all -log` **result=0**;
  benches all ~29.9 fps (gate 15 and the old 29.8 both pass): low 29.96 / 0.28 %, volcano 29.96 / 0.33, high 29.97 /
  0.22, clouds 29.98 / 0.11, combat chase 29.91 / 1.00, combat cockpit 29.94 / 0.61. High preset combat 20 s runs
  (fx 2.5 ms). clang -fsyntax-only -Wall clean on fx/camera/scene (scene with a stub windows.h).
- **Open / for others**: (1) T19/main: `-camat` runs with `-script` did not crash the jet with a held DOWN key
  (no CRASH event; the same script without -camat crashes at 50 s) - camat seems to drop the pilot keys; only matters
  for screenshots. (2) During T22 the T40 laptop had `jet.exe` open over the share for a while (link
  LNK1104); I linked a temporary jet22.exe from a throw-away makefile and deleted both afterwards. (3) Puff count is
  still the combat-fx driver: damaged smoke (30 puffs/s) + 6 burning wrecks x ~30 puffs saturate Low's 128; if T23
  wants more fx headroom on the VM, halve the damaged-smoke rate with bigger puffs. (4) A11 added its `scatter_draw(c)` line
  (+ include) to scene.cpp during T22; both edits coexist and were in the benched build.

## T19 Frontend: title, menus, options + key capture, pause, results (2026-11-14)

- **Files**: `src/menu.h/.cpp` (new, ~1850 lines), `src/main.cpp` (app states), `tests/t19/{flow,keys,reload,failed,
  complete}.scr` (CRLF) + logs `tests/t19/log_*.log`, `docs/PLAY_HOWTO.md`, new `ashfall.bat` (title, settings from
  jet.ini), `play.bat` (+ `-mode free` so it still starts free flight directly), `jet.ini` regenerated (-dumpini,
  incl. T20's [mission] keys and A11's [scatter]), screenshots `shots/t19_*_{800,1024}.png`. No other file edited.
- **App flow (main.cpp)**: plain `jet.exe` (or `-mscript`) -> loading screen (`menu_draw_loading`, small logo) ->
  `go_title()`: a FREE session_start as the backdrop, drawn every frame with `menu_backdrop_camera` (fixed-camera orbit
  27-31 km from the volcano over the sea, 1.05-1.55 km up, one lap ~10 min; only g_game.tick advances for clouds /
  props, the sim is never ticked) -> APP_TITLE. The menu returns APP_LOADING with `g_args.mode/diff` -> start_flight.
  **Any of `-mode -start -diff -script -replay -bench -camat -record -telemetry -exitafter -shot -headless` keeps the
  old direct start** (benches, tests, batch launchers unchanged). Pause = `menu_pause_*` (RESUME / RESTART ON RUNWAY /
  RESTART MISSION (not FREE) / OPTIONS / QUIT TO MENU -> go_title). A menu change while paused calls
  `params_changed()` (replay PARAMS record). Window caption "Ashfall" (`PlatInit.title`, no engine edit).
  A screen entered inside a tick gets its keys from the next tick (`appBefore` in sim_tick): the Enter of QUIT TO MENU
  pressed FLY before that fix.
- **T17 open issue 5 fixed**: a live SORTIE that ends (COMPLETE / FAILED) goes to APP_RESULTS 3 s after
  `MissionStats.endTick` (`results_due/go_results`): the sim stops, so the remaining enemies stop fighting; last.rpl is
  closed there; the frozen flight (scene, fx, dots, no HUD) is drawn under the results. Replays, -script, -bench and
  headless runs keep T17's behaviour (headless robot still quits "mission_end"). T17's in-flight results panel is gone.
- **Results / records (T20 API)**: `menu_enter(APP_RESULTS)` calls `mission_results(&g_game, &r)` and, if valid,
  `records_submit(&r)` exactly once; shows `parts.*` breakdown, `score`, BEST = new score or `prevBest`, NEW RECORD =
  `r.newRecord`. RECORDS screen: `records_load()` + `records_get(d)` (score, date, outcome, kills, time).
  `records_load()` also at menu_init. A -mscript sets `records_set_path("t19_records.ini")`.
- **Settings**: applied live with `params_restore("sec.key=value")` (or `in_bind`) and saved at once by
  `ini_write_key()` in menu.cpp: a line-based rewrite of one key (first `key=` in the section replaced, missing key
  after the section's last key, missing section appended), comments / order / unknown keys kept, CRLF. **Why not
  cfg_save**: e_cfg holds max 1024 lines (comments + blanks count) and the generated jet.ini is ~1040+ lines: the
  tail ([controls]!) would be dropped. **ENGINE BUG for the orchestrator**: the regenerated jet.ini is now **1147
  lines** (~1120 entries without headers) > `CFG_MAX_ENT` 1024 in engine/core/e_cfg.cpp, so `cfg_load` silently drops
  the tail: the end of `[scatter]` and all of `[controls]` (the warn is logged before log_open, so nothing shows).
  Effects: those params read as defaults at start and on F5; custom key bindings would not load. **Workaround in
  T19's files**: `menu_load_controls(path)` reads `[controls]` straight from the file (main calls it after
  in_bind_load; keys.scr/reload.scr prove bindings survive a restart with the 1147-line ini). Proper fix: raise
  CFG_MAX_ENT to 2048 (Cfg is malloc'd, ~300 B per entry -> ~600 KB) - engine file, not edited. Keys written: [controls] <action>
  (all 3 slots), pilot.invertPitch, pilot.mode, video.preset/width/height/fullscreen (next start; the VIDEO tab says
  so, orange when different from the running game; `-res/-preset/-window/-fullscreen` switches still override),
  camera.startView, sound.master/engine/warn/combat (0..1 in 10 steps; ini values > 1 show as full).
- **Key capture**: keyboard only (`in_capture`, joystick kinds ignored, `in_watch_all` while capturing). Esc cancels;
  reserved: Esc, F5, F6, F9-F12 and the pause action's keys (Esc, P). A key used by another action (slot 0 or 1) is
  swapped into that action ("SWAPPED: NEXT WEAPON IS NOW G"). Del = the defaults captured by `menu_input_defaults()`
  (main calls it after jet_input_setup, before in_bind_load). Actions found by their [controls] names, so main's JA_*
  ids stay private. Menus use fixed keys (arrows / Enter / Space / Esc / Backspace / Del), so rebinding can't lock
  you out.
- **Look**: A10 kit + logo exactly per A10's notes (premul pages via an own copy of load_page, 1:1 9-slices /
  3-slices, `ui_quad4_px` batches by texture, HUD fonts for text, layout in 640x480 virtual units centred
  horizontally). Logo tile set loaded on the title and freed in flight (`menu_enter`). The HUD font has no ','
  (draws '?'): keep commas out of menu strings.
- **-mscript FILE** (command list in menu.cpp "menu script"): wait, key, bind, setcfg, arg start, fly KEY on|off,
  robot on|off, reload, expect screen|tab|item|cfg|param|bind|outcome|fps|msg, waitfor, shot (`%W` = screen width),
  log, quit; "test: mscript ..." lines in jet_test.log, exit code = failures. Settings go to `t19_jet.ini` (a copy of
  the running ini), never jet.ini. `-ini FILE` (new, main) starts with another ini.
  `warn: unknown switch -mscript` / `-ini` in the log: harmless; they need adding to `g_argSwitches` in game.cpp
  (T08's file, not edited).
- **Verified (VM A)**: VC6 /W3 0 warnings. `-mscript tests\t19\flow.scr` 28/28 at 800x600 and `-res 1024 768`
  (title -> OPTIONS 3 tabs -> RECORDS -> FLY -> free flight -> pause -> options -> quit to menu -> QUIT);
  `keys.scr` 39/39 (capture G, conflict swap D<->G, reserved F5, cancel, Del defaults, LOOK -> B, invert pitch, style,
  preset, default view, volumes, saved values in t19_jet.ini, `reload`); `-ini t19_jet.ini -mscript reload.scr` 8/8
  (a fresh start reads every saved value, incl. the High preset); `failed.scr` 10/10 at both sizes (lives 1, air start,
  dive into the sea -> MISSION FAILED -> results 3 s later -> CONTINUE -> title); `complete.scr` 2/2 (robot, Easy,
  1 wave: COMPLETE at 337 s, score 3895, NEW RECORD -> CONTINUE). Title fly-by **30.0 fps at 800x600, 29.9 at
  1024x768** (the 30 fps cap; gate 15). Direct starts checked: `-window -start air -shot ... -exitafter 4.5` and
  `-mode sortie -headless` go straight to the flight. `-test all` result=0 (`tests/t19/log_test_all.log`).
- **Open**: (1) CFG_MAX_ENT engine bug above (scatter tail keys + F5 affected until fixed). (2) `-mscript`/`-ini` in g_argSwitches. (3) scene_draw_loading (T08/T22's scene.cpp)
  still says "JET M2 COMBAT" but is no longer called by main. (4) Results show the frozen flight from the camera the
  player had (chase or cockpit; no HUD). (5) Resolution list = 4:3 16-bit modes >= 800x600 only (the HUD layout is
  4:3); the 86Box Voodoo3 lists 9 (800x600 .. 1920x1440).

## T21d NIRD gauge tests + gauge fixes + dlz live refresh (2026-11-14)
- **`-hudtest zone` now checks the T21c NIRD** (src/hud_wpn.cpp). The spec is written separately from the drawing
  code (`nird_exp`, `ase_exp`). New part `test_nird` (cockpit with a head-on-like Raero, chase with a tail-like Raero)
  runs a whole approach with the player LRM numbers (Rmax 40.4 / Rne 13.5 / Rmin 0.95 km), from 2.3x Rmax down to
  0.5x Rmin. At every frame it checks: caret = spec (max error 0.000 deg). The caret never rises and moves at most
  0.39 deg per frame, without jumping at Rmax (180.09 -> 179.99). It is pinned at 355 beyond 2x Rmax (2 % release
  hysteresis) and flashes there (drawn 268 of 540 frames); it is always drawn when not pinned. Ticks: Rmax 180,
  Rmin 75, Rne between them, Raero where the spec puts it. Steering dot on the dynamic edge (0.00 px). "IN LAR" is
  never shown outside Rmin < R <= Rmax, comes on 0.48 s after Rmax and changes exactly twice. Plus a no-Rne case.
  Updated parts: geometry (sweep to 2.3x Rmax: pin seen; dot clamp), the steering dot in both views (25 cases =
  5 directions x 5 ranges with the dynamic edge), and chase long range (6 cases for Rmax 39 / 70 / 40.4 km: caret,
  pin + flashing, tick angles, reach). Also: IN LAR + caret-monotonic checks in the approach tests, IN LAR flicker
  checks in the hover tests, IN LAR placement checks (cockpit: circle reach, all 3 LOCK slots, readouts, weapon line,
  bank pointer at every bank -47..47; chase: inside the readout keep-out, right of the circle), and an expected IN LAR
  state in every shot scene. `SynZone.raeroT` publishes a Raero. New shot scene `lar_edge` (10.3 NM, just inside
  Rmax), added at the end of `s_cases` so test_cost's indices stay the same.
- **Real bugs fixed in the T21c gauge code**:
  1. `nird_ang` went up to 360 deg (12:00) just inside 2x Rmax, then jumped back to 355 when the pin engaged. 12:00 is
     also the R = 0 position. Out-of-range is now clamped at 355 (the pin angle); a 0/0 guard was added for Rmax = Rmin.
  2. "IN LAR" overlaps: the cockpit text at y 334 touched or overlapped the bank pointer (at banks 0-10 deg). It is
     now at `LAR_Y_CP` 330.5: 3 px from LOCK's home and 3.4 px from the pointer at 800x600 (4 / 3.5 px at 1024x768).
     The chase text at y 132 sat on the circle's top (the ring at the FOV-40 clamp, the aspect pointer in a tail
     chase); it is now the 4th readout row (left at x 396, under TOF/ACT/TTG), inside the chase readout keep-out rect.
  3. Ladder keep-out overflow (T18): the cockpit IN LAR rect was the 3rd fixed rect, which pushed LOCK out of the
     4-slot `hudwpn_keepout` list whenever MISSILE + LOCK were up. IN LAR now goes after MISSILE / LOCK (the threat
     arrows still see all rects; the chase IN LAR is inside the readout rect).
  4. "IN LAR" had no hysteresis (raw `inRmax`): it could flicker on a noisy boundary. It now uses SHOOT's entry rule
     (held `wpnShootOnSec` + one live publication) and still leaves at once, so it is never shown outside the LAR.
  5. Dynamic ASE without an Rne (no no-escape zone) stayed wide (aseDeg) all the way out to Rmax. It now widens towards
     Rmin (`ase_deg`). There was no division by zero.
  Not changed: when Raero = Rmax (beam aspect) the Raero tick lies under the longer Rmax tick (same value, nothing
  wrong is shown). The Raero tick (0.45 aseOuter, 0.6 profile) looks like the aspect pointer (0.55, 0.6); the
  `raero` diamond sprite in the atlas is unused (user's call).
- **dlz live refresh** (src/dlz.cpp): new `[dlz] coastFarM` 6000 (0 = off). Live and sync fly-outs against a
  straight target step 2 x dt after the motor burn while the target is farther than 6 km (one step's cost). Escape
  fly-outs (Rne / checks) and the AI tables are untouched, so Rne is bit-identical, the table key and data/dlz.tab are
  unchanged, and Rmax moves <= 0.5 % with no bias. Cycle 6800 -> 5560 steps (Mac). VM live_refresh 1.33 -> **1.14 s at
  30 fps**. Gate 1.2 -> 1.4 s in tests_dlz.cpp, with a comment (user accepts 15 fps on the VM; T40 ~0.2 s).
  `simh shots 500`: all "inside Rne" rows 100 % (player defence+flares 18/19 as before). **jet.ini needs `-dumpini`**
  for the new key (the default works without it).
- **Verified (VM A)**: VC6 /W3 0 warnings. `-fullscreen -res 800 600` and `-res 1024 768`: `-hudtest zone`, `layout` and
  `contacts` all rc=0 (zone failures=0). `-test all` rc=0. Mac: `make -C tools/simh` clean, `simh test all` rc=0.
  Screenshots: shots/nird_cockpit_rmax.png (just inside Rmax), nird_cockpit_rne_rmax.png (between Rne and Rmax),
  nird_cockpit_inside_rne.png, nird_cockpit_out_of_range.png, nird_chase_rmax.png.
- **Docs to update (not this task's files)**: DESIGN 10.6 "IN LAR below the circle" -> chase = 4th readout row;
  HUD_LAYOUT.md weapon table (IN LAR positions, NIRD instead of the range scale); DESIGN 12.4 mentions `coastFarM`.
