# Jet M0 probe: depth, fog, HUD lines, costs (T02, VM A, 2026-11-13)

`probe.exe` (this folder, built on the copied engine, `engine\engine.lib`) runs the 8 scenes of DESIGN 9.2 on the
86Box VM (Pentium II 233, Voodoo3, DX 6.1) with W-buffer and 16-bit Z, at far 18 / 28 km and near 1.0 / 1.5 m, plus
800x600 fullscreen runs (user decision 2026-11-13: 800x600 is the minimum resolution; 15 fps is the VM gate).
Every number below comes from the `probe: ...` lines in `probe_*.log`; full tables: `TABLES.md`; screenshots:
`shots/*.png` (suffix `_w` = W-buffer, `_z` = Z, `_w800s6/_w800s7/_w800` = 800x600 fullscreen W).

## Decision line

**Plan A: one pass, W-buffer (`wantWBuffer=1`). Near 1.0 m cockpit / 1.5 m chase (near has no effect on W
precision; far 18 vs 28 km only moves where the 1-px shoreline offset appears). Far 18 km Low / 28 km High. Fog: linear W table fog, start 3.6 km / 5.6 km, END
TRIMMED TO 0.9 x far = 16.2 km Low / 25.2 km High (the Voodoo fog table reaches only ~93-96 % at the far plane
otherwise, which leaves a visible step against the panorama's fog-coloured bottom). HUD: 2 px strokes (1 px only
for tape ticks and box outlines), 1 px black shadow at (+1,+1) with 50 % alpha (70 % optional for yellow text),
HUD pass blended with the new `RB_PREMUL` (ONE, INVSRCALPHA): the Voodoo3 accepts it (blend check exact) and it
costs the same fill as ADD/BLEND; ADD is rejected (green turns pure white over haze and cloud). Low resolution
800x600x16 on the VM, pacing divisor 4 (30 fps) is realistic for the planned Low scene (measured Low frame
14.8 ms at 800x600, CPU-bound), divisor 6 as the fallback, the 15 fps gate (8 blanks) has a 2x margin. Other
rules from the probe: runway decals 0.2 m + `zBias 2`; keep separate surfaces >= 2 m apart (islandc's 4 m under
the sea is fine); keep |uv| < ~16-32 per vertex (camera/chunk-relative UVs); acMeshDist 2.5 km / acDotDist 4 km
confirmed, but start the dot at 1.5 km under the mesh so tail-on jets never vanish.**

## Environment and method

| Item | Value |
|---|---|
| Machine | VM A, 86Box, P2-233, Voodoo3 3000 (`3dfx32v3.dll` 4.11.1.441), DX 6.1, 16-bit Z (no stencil), WBUFFER / WFOG / FOGTABLE / ZBIAS caps |
| Blend caps | src: zero one srcalpha invsrcalpha destcolor invdestcolor; dest: zero one srccolor invsrccolor srcalpha invsrcalpha (so ONE/INVSRCALPHA is legal) |
| Modes | scenes 1-7: windowed 640x480 on the 800x600x16 desktop (scene 2 rerun fullscreen: identical numbers, `probe_wfs.log`); costs: windowed + fullscreen 640x480 and 800x600 |
| Grabs | windowed: GDI copy of the client area after `r_present`, verified by a 16-bit frame-number marker in the top and bottom rows (the windowed Blt is asynchronous; without the marker the bottom rows were stale). Fullscreen: `r_save_bmp`. `grab_check` GDI vs back buffer: 2 px differ (the marker) |
| Depth error metric | scenes 1-3 draw each case twice: the test frame (Z on, game draw order) and a reference with Z off in painter's order (exact visibility, same triangles). err = pixels that differ. Flat 565-exact colours, no fog |
| Timing | QPC. The windowed Blt present and the fullscreen Flip don't show fill cost (the first never blocks, the second is locked to 120 Hz), so GPU-inclusive frame times are N frames between two `gpu_sync()` (= `r_save_bmp`, which locks the back buffer with DDLOCK_WAIT; its own cost, 450 / 700 ms, is measured on an idle card and subtracted). Fullscreen benches render without presenting. Run-to-run noise about +-0.5 ms per frame (VM B was running) |
| Reproduce | `w98 build jet`, `w98 build jet\probe`, then `w98 run 'M:\dev\jet\probe\probe.exe -scene all -wbuf 1 -shot' --show -t 1500 -C 'M:\dev\jet\probe'` (`-wbuf 0` for Z; `-fs -res 800 -scene 8` etc.), convert `shots/*.bmp` with sips, `python3 probe/summarize.py` |

## Scene 1: coastline (1:8 terrain ramp through the sea plane, camera 300 m up, terrain drawn before the sea)

err A / err B = error pixels in two frames 1/30 s apart (250 m/s); flips = error pixels that changed. One coast
row across the ramp is ~347 px (`coast_px`).

| Depth | far / near | 1 km | 3 km | 6 km | 12 km | 17 km |
|---|---|---|---|---|---|---|
| **W** | 18 km / 1.0 (same for 1.5) | 266 / 0 | 350 / 0 | 0 / 0 | 351 / 0 | 0 / 0 |
| **W** | 28 km / 1.0 (same for 1.5) | 337 / 0 | 0 / 0 | 0 / 351 | 0 / 0 | 0 / 0 |
| Z | 18 km / 1.0 | 998 / 1341 | 691 / 1043 | 0 / 0 | 347 / 347 | 0 / 0 |
| Z | 28 km / 1.5 | 1000 / 1013 | 347 / 698 | 347 / 347 | 347 / 347 | 0 / 0 |

Result: with W the only error is **one pixel row exactly on the shoreline** (land shows where the sea should be:
the coastline shifts by at most 1 px, a whole row at once): no speckle, no band, at every distance. Z gives up to
3-4 rows at 1-3 km that flip between frames. Shots: `s1_coast_f18_d01..d17_{w,z}.png` + `_zoom` (red = error;
4x zoom at the coast centre), `s1_coast_f28_d17_*`.

## Scene 2: parallel quads (near quad drawn first, far quad second, LESSEQUAL), leak = % of the near quad lost

| Depth | separation | orientation | 100 m | 1 km | 5 km | 10 km | 17 km |
|---|---|---|---|---|---|---|---|
| **W** (all far/near) | 0.5 m | facing | 0 | 0 | 100 | 100 | 0 |
| **W** | 0.5 m | 70 deg oblique | 0 | 0 | 4 | 42 | 85 |
| **W** | 2 m / 5 m | both | 0 | 0 | 0 | 0 | 0 |
| Z 18 km / 1.0 | 2 m | facing | 0 | 100 | 100 | 100 | 100 |
| Z 18 km / 1.0 | 5 m | facing | 0 | 100 | 100 | 100 | 100 |
| Z 18 km / 1.0 | 5 m | oblique | 0 | 0 | 50 | 100 | 100 |

W keeps 2 m separations apart out to 17 km; 0.5 m fails from 5 km. Z fails 2-5 m from 1 km. The W numbers of this
scene are identical for near 1.0 / 1.5 and far 18 / 28 km. Fullscreen 640x480 gives byte-identical numbers (`probe_wfs.log`).
Shots: `s2_quads_d01000/d10000/d17000_{face,obl}_{w,z}.png`.

## Scene 3: runway decal 0.2 m over flat ground (err % of runway pixels; W, far 18 km, near 1.0)

| View | zBias | 0.5 km | 2 km | 4 km |
|---|---|---|---|---|
| 3 deg glide path | 0 | 0 | 0 | 5.0 |
| 3 deg glide path | 1 | 0 | 0 | 5.1 |
| 3 deg glide path | 2 | 0 | 0 | **0** |
| 600 m level | 0 | 0 | 3.4 | 16.7 |
| 600 m level | 1 | 0 | 0.5 | 0 |
| 600 m level | 2 | 0 | 0 | **0** |

All four far/near settings agree within a few pixels. **zBias 2** is clean everywhere (DESIGN 8.5 said zBias 1).
(With Z the decal never fails: Z quantises both surfaces to the same value at distance and LESSEQUAL lets the
later decal win the tie, but Z loses scene 2 badly.) Shots: `s3_rwy_d2000/d4000_zb0/zb1_{w,z}.png` + `_zoom`.

## Scene 4: fog

**(a) fog ramp** (black billboards at exact view depth W over the fog colour; measured fog fraction):

| Preset (fog start-end, far) | W = start | 20 % | 40 % | 60 % | 80 % | W = 0.97 far | W = 0.995 far |
|---|---|---|---|---|---|---|---|
| Low 3.6-18 km, far 18 | 0.033 | 0.212 | 0.407 | 0.599 | 0.805 | 0.929 (exp 0.963) | 0.963 (exp 0.994) |
| High 5.6-28 km, far 28 | 0.009 | 0.213 | 0.412 | 0.596 | 0.798 | 0.918 (0.963) | 0.927 (0.994) |
| **LowTrim 3.6-16.2 km, far 18** | 0.036 | 0.215 | 0.407 | 0.599 | 0.797 | 0.985 (1.0) | 0.986 (1.0) |
| **HighTrim 5.6-25.2, far 28** | 0.009 | 0.212 | 0.411 | 0.599 | 0.797 | 0.985 (1.0) | 0.986 (1.0) |

The W table fog is linear and accurate (+-0.015) through 80 %, but its last entries never reach 100 % at fogEnd.
0.985 is the fog colour itself after RGB565 quantisation (192, 211, 224 for 196, 212, 228), so the trimmed presets
are effectively 100 % from 0.9 x far on. Fog is planar (view depth W, same as the far clip), not radial.

**(b) textured sea to beyond the far plane under the A2 panorama** (first on-screen use of `world_sky_load` +
`world_draw_sky`: works). max row step = largest change between adjacent rows around the sea's far edge (8-bit):

| Preset | 100 m | 1000 m | 4000 m | 4000 m, pitch -8 |
|---|---|---|---|---|
| Low (fogEnd = far) | 47.8 | 15.0 | 7.5, visible line | 7.5 |
| High (fogEnd = far) | 56.1 | 18.2 | 11.5, visible line | 11.5 |
| **LowTrim** | 48.3 | 15.0 | **5.2, seamless** | 4.8 |
| **HighTrim** | 58.8 | 12.2 | **7.5, seamless** | 7.3 |

At 100 m the sea's far edge is 1-2 rows under the horizon and the fog legitimately ramps over ~5 rows (big row
steps, no seam: the measured per-row fog matches the expected within 0.02-0.13, `s4 row` lines). At 4 km with
fogEnd = far the fogged sea stops at ~93 % against the 100 % panorama: a thin visible line
(`s4_seasky_High_a4000_p0_w.png`); with the trim it is gone (`s4_seasky_HighTrim_a4000_p0_w.png`,
`..._LowTrim_...`). Shots: `s4_fogramp_*`, `s4_seasky_*` (+ `_zoom` at the edge).

## Scene 5: UV magnitude (800 x 1200 m grass patch, 32 m period, integer offset M added to every uv, vs M = 0)

| M (max uv) | 8 (46) | 32 (70) | 64 (102) | 128 (166) | 256 (294) | 512 (550) | 2048 (2086) |
|---|---|---|---|---|---|---|---|
| px differing > 24 | 1.8 k | 11 k | 18 k | 29 k | 35 k | 40 k | 43 k |
| mean diff (8-bit) | 3.5 | 9.9 | 11.7 | 13.4 | 14.3 | 15.0 | 15.6 |

No swimming, but large uv values make the (emulated) Voodoo pick coarser mip levels: the texture goes blurry
from ~|uv| 32 on and is a flat average by 2048 (`s5_uv_off0000/0128/2048_w_zoom.png`; same with Z). **Rule for
T04: keep |uv| per vertex small**: terrain UVs relative to the chunk origin (chunk 512 m = 8-16 periods), sea UVs
relative to the camera snapped to the period (DESIGN 8.3 already); for the far sea ring |uv| grows but it is fogged.

## Scene 6: aircraft visibility (A1 jet, LOD2 120 tris, against bright haze at 1500 m, Low fog)

Pixels that differ from the background frame (> 10 and > 40 in any channel give the same counts: the jet is dark).

| Aspect | 1 km | 2.5 km | 4 km | 6 km | 800x600: 1 / 2.5 / 4 / 6 km |
|---|---|---|---|---|---|
| side | 10 | 3 | 2 | 1 | 13 / 3 / 3 / 1 |
| plan (90 deg bank) | 16 | 3 | 2 | 1 | 24 / 3 / 3 / 1 |
| tail | 4 | **0** | **0** | 0 | 5 / 1 / 1 / 1 |

Expected length 15.3 m x 502 px / d: 7.7 / 3.1 / 1.9 / 1.3 px (640). Confirms DESIGN 9.1: at 2.5 km a jet is a
3 px dash, at 4 km 1-2 px; tail-on it disappears already at 2.5 km. Recommendation for T08/T15: keep
`acMeshDist` 2.5 km and `acDotDist` 4 km, but draw the 2x2 dot from 1.5 km on (under the mesh) and fade it from
alpha ~0.7, so a tail-on target never vanishes before the HUD symbol takes over; a full-alpha 2x2 dot is more
visible than the 3 px mesh dash (`s6_dots_w.png` + zoom). Shots: `s6_jets_{side,tail,plan}_w(800s6).png` + zoom.

## Scene 7: HUD (A4 atlas, 4444, in-engine) over haze sky, cloud white, sea, terrain

**Blend check** (known colour over grey 128, measured): BLEND a50 red -> 191.5,63.7,64 (exp 192,64,64); ADD
404000 -> 191.7,192,128 (exp 192,192,128); **PREMUL** a50 800000 -> 191.5,63.7,64 (exp 192,64,64), a0 404040 ->
191.7,192,191.7 (exp 192), a100 20C040 -> 32,191.5,63.7 (exp 32,192,64): **all ok, W and Z, 640 and 800**.

**2 px line core colour** (HUD green 40FF60, yellow FFE040; 640x480, shadow 50 %):

| Background | ADD | PREMUL (= BLEND for untextured lines) |
|---|---|---|
| haze sky 194,211,231 | 255,255,255 (chroma 0), yellow also white | 64,255,96 (chroma 191), yellow 255,224,64 |
| cloud 244,246,250 | 255,255,255 (chroma 0) | 64,255,96 |
| sea 30,70,110 | 96,255,208 (cyan cast) | 64,255,96 |
| terrain 70,100,50 | 136,255,144 | 64,255,96 |

ADD washes every HUD colour to white over haze and cloud (at 800x600 without shadow the cloud-band line is the
background colour exactly: invisible). PREMUL draws the exact HUD colour on every background; the 50 % shadow
outlines it on white. 70 % shadow makes yellow/red text clearly stronger on cloud (`s7_lines_premul_sh70_w.png`).
1 px lines read but are thin; 2 px is the stroke; 3 px looks heavy. Atlas text (small 9 px, large 13 px caps) is
crisp at 640x480 when drawn at whole pixels; at 800x600 through `ui_begin(640,480)` (x1.25, bilinear) it is
slightly soft but readable (`s7_mock_premul_sh50_w800s7_zoom.png`); drawn 1:1 it is crisp but 20 % smaller
(`s7_mock_premul_sh50_native1to1_w800s7_zoom.png`). The 3D contact markers are drawn as the new small hollow
diamonds (2 px, half-diagonal 6 px; red hostile / yellow unknown): readable over sky, cloud, sea and terrain.
Shots: `s7_lines_*` (6 variants), `s7_mock_{add,premul,blend}_sh50`, `s7_mock_premul_sh70` (+ zoom, 640 and 800).

## Scene 8: costs

GPU-inclusive ms per frame (method above). Fill layers = 4 full-screen textured quads (3D at 5 km with Z, or 2D).

| Case | windowed 640 W | windowed 640 Z | fullscreen 640 | fullscreen 800 |
|---|---|---|---|---|
| empty frame (clear) | 2.2-2.5 | 1.6-2.0 | 0.28-0.38 | 0.45-0.52 |
| `r_clear_z` each (x4 - x1) / 3 | 0.15 | 0.19 | 0.12 | 0.21 |
| 3D layers x4, fog off | 11.2 | 11.8 | 9.3 | 18.6 |
| 3D layers x4, fog on (W table) | 12.9 | 13.0 | 9.3 | 20.6 |
| 2D layers x4 opaque | 9.8 | 7.1 | 6.7 | 16.4 |
| 2D layers x4 BLEND | 11.2 | 8.4 | 8.1 | 17.2 |
| 2D layers x4 ADD | 10.7 | 8.3 | 7.5 | 16.6 |
| 2D layers x4 **PREMUL** | 10.7 | 8.4 | 7.5 | 16.8 |
| mock HUD (shadow + PREMUL, 304 quads, 28 draws, 608 tris) | 3.1 (CPU 2.2) | 2.3 (1.6) | 1.9 (1.9) | 1.9 (1.8) |
| **Low frame** (sky, 3200-tri terrain VB, 160-tri sea, 16 puffs, 2 jets LOD0, mock HUD: 5432 tris, 63 draws) | - | - | **14.7** (CPU 14.4) | **14.8** (CPU 14.8) |

CPU-only (no sync, windowed): 500 `ui_line` quads 3.2-4.4 ms, 500 atlas glyph quads 3.6-4.2 ms (~7-8 us per quad
including `r_draw_2d`): the HUD is CPU-bound, independent of blend mode.

Read:
- **Plan B cost** would be ~0.15-0.2 ms per `r_clear_z` (not needed: Plan A).
- **Fog** costs 0-15 % fill (W table fog; 0 at fullscreen 640, ~0.5 ms per full-screen layer at 800).
- **Blending** costs ~0.2-0.4 ms per full-screen layer over opaque; ADD, BLEND and PREMUL are equal.
- **800x600 vs 640x480**: a full-screen layer costs ~4 ms at 800 vs ~1.7 ms at 640 in the emulator (more than
  the 1.56x pixel ratio; emulator figure). The Low frame is CPU-bound (14.4-14.8 ms of transform/lighting/submit)
  at both sizes, so its fill (~2.5-3 layers) runs in parallel and the frame time does not change. With sim
  (<= 5 ms) and audio that is ~20-25 ms: **divisor 4 (30 fps) at 800x600 is realistic**; overdraw is the risk at
  800 (keep it <= ~4 full-screen layers, i.e. no cloud deck on Low, puffs fade near the camera as planned).
- **Texture memory** left after front + back + Z (fullscreen): 12 834 KB at 640x480, **11 282 KB at 800x600**
  (spike: 8.9 MB at 1024x768). The Low budget of 6 MB fits with room.
- **HUD CPU**: the mock HUD (about the size of the M1 cockpit HUD) is 1.6-2.2 ms CPU with the shadow pass, above the
  1.5 ms budget of DESIGN 9.3 (the shadow pass doubles the quads; 28 draws because `ui_line` is untextured and every
  switch between lines and atlas quads flushes). See the T07 notes below.

## Notes for later tasks

- **T07 (HUD)**: (1) do NOT load the atlas with `tex_get("hud_atlas", IMG_ALPHA, ...)`: `img_load`'s IMG_ALPHA path
  runs `img_bleed`, which fills every alpha-0 texel with its neighbours' white and breaks the premultiplied atlas
  (glyph cells come out as solid boxes in ADD and PREMUL). Load `hud_atlas.bmp` and `hud_atlas_a.bmp` with flags 0,
  put the alpha file's red channel into alpha, set `flags = IMG_ALPHA` and `tex_add(name, &img, RT_CLAMP |
  RT_NOMIP)` (`atlas_load` in probe.cpp). (2) `ui_set_blend(RB_PREMUL)` for the HUD pass, `RB_BLEND` + black
  0x80 at (+1,+1) for the shadow. HUD colours with alpha FF need no premultiplying; a dimmed colour must scale RGB
  and alpha together. (3) Cut CPU: draw lines from the atlas `white` texel so the whole HUD is one texture (one draw
  per pass; needs a textured `ui_line` variant in e_ui), and/or ask A4 for a pre-shadowed atlas (shadow baked into
  the premultiplied texels: one quad per glyph instead of two; only possible with PREMUL). (4) At 800x600 draw
  the layout through `ui_begin(640,480)` (x1.25): slightly soft text; for crisp text ask A4 for a 1.25x font set
  (the atlas has room), or position at x1.25 and draw glyphs 1:1.
- **T04 (terrain/sea)**: W-buffer single pass; coast ok with the 4 m under-sea rule; UV rebasing rule (scene 5);
  fog end = 0.9 x far; the sea ring past the far plane is fine (partly clipped triangles are drawn, fully-beyond
  ones rejected, the sky's fog band fills the gap).
- **T08 (scene)**: presets: Low = 800x600, far 18 km, fog 3.6-16.2 km, divisor 4 (fallback 6, gate 8); High far
  28 km, fog 5.6-25.2 km. Runway ribbons `zBias 2`. `world_sky_load(w, 4, "light")` + `world_draw_sky` work.
- Probe caveat: emulator numbers; re-check fill on the T40 (Radeon) and the X41 later (T23).
