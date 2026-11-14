# Jet: reusing the racer engine and tooling (research, 2026-11-13)

Scope: what in `../racer` (read only) can be reused for an Ace Combat style flight game on the VM
(Pentium II 233, Voodoo3, DX 6.1, 640x480x16, 30 fps), how to render a flight world within the measured
budgets, what the asset pipeline can generate, build layout, testing patterns. Paths are relative to
`~/Documents/86box/Shared/dev/racer/` unless stated. Numbers marked UNVERIFIED must be measured on the VM
before they are trusted (the racer spike measured nothing beyond 600 m).

## 0. Verdict in five lines

- The engine is clean and mostly game-agnostic: core, rend, plat, tex, mesh, image, prof and the Python
  assetkit are reusable as they are. `phys/` (cars) is dropped, `coll/` stays only because `e_world` links it.
- `e_world` (128 m cells, 2 LODs, a track-shaped `.trk`) is the one big mismatch for a 16-40 km world.
  Keep it for the airbase (sky, instances, buildings, runway) and write a small new `e_terrain` module
  for the island heightmap chunks with 5 LODs and for the sea.
- Depth range is the main technical risk: 16-bit Z cannot cover 1 m to 40 km. W-buffer is already on by
  default in racer and the Voodoo3 HAL lists it. Verify with a depth/fog probe first; fallback is a two-pass
  split (needs a new `r_clear_z`, one line in the D3D6 backend).
- Triangle budget works: ~4-5k terrain, ~2k aircraft, ~1k effects, HUD lines ~0.3k = about 8k at 30 fps
  (Section 2.7). The island terrain at 16 px triangle size needs only ~1-4k tris with ring LODs.
- The art toolkit is directly reusable: assetkit mesh lofts for the jet, fBm/ridged noise for the island,
  gen_ground.py texture functions for rock/sand/grass/sea, gen_sky.py structure for the sky panorama.

---------------------------------------------------------------------------------------------------------

## 1. Module by module

Legend: COPY = as is, CHANGE = small edits (listed), DROP = not needed.

### 1.1 engine/core (all portable, VC6 + clang -std=c++98)  COPY all

| File | Notes |
|---|---|
| `e_base.h/.cpp` | types, `E_PI`, `E_CLAMP`, xorshift `ERand`, FNV/CRC32, `e_snprintf`, `e_ftoi`. COPY |
| `e_math.h/.cpp` (290 lines) | Vec3/Quat/Mat3/Mat4, frustum, AABB. Left-handed, +Y up, +Z forward, row vectors, yaw about +Y (e_math.h:1-8). `q_*` + `q_to_mat3` + `q_slerp` are what a flight model wants. COPY. Missing for flight (add to a game file, not the engine): ray-sphere, segment-sphere, quat from euler/roll-pitch-yaw. `m4_persp_lh(out, fovY, aspect, zn, zf)` takes any near/far. |
| `e_log.h/.cpp` | `log_printf(LOG_INFO, "tag: key=value ...")` grep-friendly lines. COPY (file name is chosen by main) |
| `e_cfg.h/.cpp` | ini parser + `-switch` args (`arg_has`, `arg_int`, `arg_str`, `arg_unknown`). COPY |
| `e_file.h/.cpp` | `file_load`, chunk container (`chunk_open/chunk_find`, CRC), `data_path("tex/x.bmp")` -> `<exe>\data\...`. COPY. Chunk header tag is a parameter, but `e_world.cpp` insists on tag `RTRK` (see 1.4) |
| `e_image.h/.cpp` | BMP 24/8 load, magenta key, `_a.bmp` alpha, mip generation. COPY |
| `e_prof.h/.cpp` | `prof_section/begin/end/count`, `-bench` report writer. COPY |

### 1.2 engine/coll  KEEP FILES, little use

`e_coll.*` (627 + 69 lines): triangle buckets on a 4 m grid, one-sided walls, `CollBox` (declared, "unused in
1.0"), 2D OBB SAT, `coll_ray`, `coll_ground`. It is generic but sized for a road: a 4 m bucket grid over a 16 km island
is 16M buckets. `e_world.h` includes `e_coll.h` and `World` owns a `CollWorld*`, so the file stays in the
build. Flight does not need it: ground collision = `terrain_height(x, z)` (bilinear on the height grid, Section 2.2),
plus flat runway/apron rectangles, plus sphere tests for aircraft/missiles/hangar boxes in game code. The runway
area alone (1 km x 1 km) could use `coll_create_tris` + `coll_ground` if exact touchdown on the runway mesh
is wanted, but a heightmap function is simpler and deterministic.

### 1.3 engine/phys  DROP

`veh.*` (1261 + 205 lines, one car), `phys.*` (PhysScene steps cars + walls), `surf.*` (SURF_ENUM table for
tyres): car-only. Remove `engine\obj\veh.obj phys.obj surf.obj` from `EOBJS` and from `tools/physh/Makefile`.
`e_coll.h` defines `SURF_*` and `CollTri.surface`; harmless, keep. The flight model lives in `src/` as portable
code (same rule as racer DESIGN 2.2: portable sim, Windows-only render), so it runs on the Mac under clang.

### 1.4 engine/world

| File | Verdict | Details |
|---|---|---|
| `e_tex.*` (123 lines) | COPY | `tex_get("rock_a", IMG_KEYED/IMG_ALPHA, RT_CLAMP/RT_NOMIP)` refcounted cache by name, `tex_add(name, img, flags)` for generated images (e.g. radar blips), `tex_release_all()` between missions. e_tex.h:1-12 |
| `e_mesh.*` (412 lines) | COPY (+ small notes) | `.msh` load (4 LODs max: `Mesh.lod[4]`, 24 tags max, `MESH_LIT_MAX 4096` verts/LOD for `mesh_draw_lit`), per-LOD VB, `mesh_lod(m, dist, lodScale)`, `mesh_tag("name")`. Static `mesh_draw`/`mesh_draw_ex` (pre-lit vertex colours, optional tint and pass mask) and `mesh_draw_lit` (CPU-lit per vertex with sun+ambient, e_mesh.h:21-26, used for cars). Aircraft want `mesh_draw_lit`. Wheel-style sub-meshes (own `.msh`, tag on the body for the hub) are the pattern for stabilators/flaperons/rudder/nozzle (Section 3.4). `MM_WHEEL/MM_GLASS/MM_LIGHT/MM_TINT` flags are car vocabulary; they are just bits |
| `e_world.*` (1110 lines) | CHANGE or SPLIT | see below |

**Racer-specific assumptions baked into `e_world`:**

1. `enum { WORLD_CELL = 128 }` (e_world.h:25), a compile-time constant used by the cell grid (`floor_div(...,
   WORLD_CELL)` at e_world.cpp:387-390) and the `CELL` chunk. Culling loops over every grid cell in the square
   `eye +- viewDist`: viewDist 40 km = 625 x 625 = 390k cells per frame. Unusable. Fine up to ~4 km (63 x 63).
2. Exactly 2 LODs per cell (`WCell.batch0[2]`, `lod = d < lodDist ? 0 : 1`, e_world.cpp:397) with one
   `lodDist`. Terrain needs 5.
3. `world_view_lookat` hard-codes near 1.0 m and far `viewDist + 50` (e_world.cpp:350). A flight game builds its own
   `WView` (the struct is plain data, e_world.h:53) or adds near/far parameters.
4. `WORLD_MAX_VISIBLE 4096` batches/instances; `world_draw_sky` assumes panorama elevations -6..+28 degrees
   (defaults in `world_draw_sky`, e_world.cpp:922; the SKY chunk can override), a 100 m dome (scaled to `viewDist + 25` with
   `world_set_sky_late(1)`), 32 segments, a -30 degree fog band below and fog colour at the nadir (e_world.cpp:910-1082).
   The dome rotates with the full view matrix (translation removed), so it already works for pitch and roll.
5. `world_load` requires container tag `RTRK`, INFO, optional SKY, TEXL, MATL, MSHL, CELL, VPOL/BTCH/INDX, INST,
   COLT/COLG/WALL/BOXS. `world_chunk(w, "SLCE")` etc. let a game read its own chunks. Track-specific content
   (SLCE/CHKP/GRID/MAPI) is read by `src/track.cpp`, not the engine, so it does not leak in.
6. World size note in DESIGN 2.3: "worlds fit in +-4 km, float resolution <= 0.5 mm". An island +-16 km has 1-2 mm
   resolution at the edge: fine. Keep the island centred on the origin and never move the world origin.
7. Fog: `world_draw_view` derives fog from `World.fogStart/fogEnd` clamped to `min(fogEnd, viewDist)` and
   `start = min(fogStart, 0.9 end)`; the fog colour comes from the SKY chunk. Fine for any scale.

**Recommendation:** keep `e_world` unchanged for the airbase and for everything that is already "static objects in
cells": runway, taxiways, hangars (boxes), tower, trees, fuel tanks, AA sites, radar dishes, ships (static), plus
the sky (`world_sky_load`, `world_draw_sky`, `world_set_sky_late`). The airbase is ~3 x 3 km, which is 24 x 24
cells. Load a `base.trk` produced by a new island compiler (Section 3.6) with `viewDist` ~4 km. Write the island
terrain as a separate module `e_terrain.*` (Section 2.2). If one engine had to be general, the cheapest change is
`WORLD_CELL` -> `World.cellSize` read from INFO plus `nLods` per cell; that touches world_load, world_visible and
the compiler, so prefer the split.

### 1.5 engine/rend  COPY, three small additions

`e_rend.h` (84 lines), `e_rend.cpp` (95), `r_backend.h`, `r_null.cpp`, `r_d3d6.cpp` (1692). Only `r_d3d6.cpp`
includes `d3d.h`. Key API (e_rend.h):

```c
int   r_init(void *hwnd, const RInit *ri);   int r_init_null(const RInit *ri);   void r_shutdown(void);
int   r_begin_frame(u32 clearRGB /* | R_ZONLY */);  void r_end_frame(void);  void r_present(void);
void  r_set_viewport(int x,int y,int w,int h);
void  r_set_proj(const Mat4*); void r_set_view(const Mat4*); void r_set_world(const Mat4*);   /* NULL = identity */
void  r_set_fog(int on, u32 rgb, float start, float end);       /* W table fog, eye distances in metres */
void  r_set_material(const RMaterial *m);   /* tex0, tex1, blend (RB_OPAQUE/ALPHATEST/BLEND/ADD/SHADOW), stage1,
                                               cullNone, zWrite, zTest, fog, clampUV, zBias 0..2, lodBias */
RTex  r_tex_create(const EImage*, int flags /* RT_CLAMP|RT_NOMIP */);   void r_tex_free(RTex);
RVBuf r_vb_create(int fmt /* RFMT_V1 RVert1 24 B, RFMT_V2 32 B */, const void *verts, int count);
void  r_draw_vb(RVBuf, const u16 *idx, int nIdx);                          /* static, indexed triangle list */
void  r_draw_mem(int fmt, const void *verts, int nVerts, const u16 *idx, int nIdx);   /* dynamic: effects, sea, trails */
void  r_draw_2d(const RVert2D*, int nVerts, const u16 *idx, int nIdx, RTex tex, int blend);  /* screen px, no Z/fog */
int   r_save_bmp(const char *path);  void r_stats(RStats*, int reset);  RCaps *r_caps();
```

How the backend really works (important for budgets): `draw_tris` (r_d3d6.cpp:851-974) transforms with its own
float code (world*view*proj collapsed to one matrix), clips against near and the four screen edges (no guard
band), then `DrawIndexedPrimitive` of D3DTLVERTEX from system memory with `DONOTCLIP`. "Vertex buffers" are only
malloc copies. `r_draw_vb` transforms the contiguous vertex range `[min idx, max idx]` the index list touches
(r_d3d6.cpp:990-1000), so **every LOD of every chunk needs its own contiguous vertex block**, never one shared
vertex array indexed with a stride (a far chunk would transform all of the fine vertices).

Depth behaviour: `sz = cz * rhw` (z/w), vertices beyond the far plane (code bit 2) are not clipped but get
`sz = 1`, only the near plane clips in depth (r_d3d6.cpp:884-935). W-buffer is selected by `RInit.wantWBuffer`
(racer main.cpp:551 sets it to 1 by default, r_d3d6.cpp:1336: `D3DZB_USEW`); `d_set_proj` also calls
`SetTransform(PROJECTION)` so the driver knows the W range (r_d3d6.cpp:628-632).

Additions needed for jet:

1. `void r_clear_z(void)`: `s_vp->Clear2(1, &rect, D3DCLEAR_ZBUFFER, 0, 1.0f, 0)` mid-frame (copy the call from
   `d_begin_frame`, r_d3d6.cpp:1466). Needed only for the two-pass depth fallback (2.4).
2. No change needed for decals: `RMaterial.zBias` (0..2) already exists for runway markings over terrain.
3. Optional `r_draw_2d` with RB_ADD already works (blend is a parameter), but see e_ui below.

Other facts: `RInit.fpsTarget 30/60` gives the blank divisor (4 blanks of 119.96 Hz = 30 fps) and honest
pacing (spin + queue flip + wait, docs/PERF.md section 4.3); `r_pace_stats` reports missed slots;
`MAX_TEX 1024`, `MAX_VB 4096` (r_d3d6.cpp:32); texture budget accounting `r_tex_kb()`; `RT_NOMIP`; formats chosen from
the image flags: opaque 565, keyed 1555, `IMG_ALPHA` 4444.

### 1.6 engine/plat

| File | Verdict | Notes |
|---|---|---|
| `e_plat.*` | COPY | window class "RacerWnd" / title "Racer" are only defaults (`PlatInit.title/.wndClass`); the error box caption is "Racer" (e_plat.cpp:191), change it. **FPU: `plat_fpu_setup()` sets PC24 + round-to-nearest (e_plat.cpp:9-11).** This also truncates `double` math in your own code to 24-bit mantissa. A flight model that uses doubles must either use `float` consistently or define `PLAT_FPU_PC` as `_PC_53` (RESULTS.md: FPUSETUP gave no speedup). |
| `e_input.*` (927) | COPY + small additions | see below |
| `e_audio.*` (563) | COPY | DirectSound, mono PCM, up to 32 voices, `au_play(snd, vol, pan, pitch, loop)`, `au_set` cached + `au_update` once per frame. No 3D: do distance/pan/doppler in game code (pitch = rate multiplier). `AU_MAX_SOUNDS 64`. Never PlaySound. |
| `e_ui.*` (338) | COPY + `ui_line` | GDI glyph atlas -> ARGB4444 texture, `font_create(&f, "Arial", px, bold, outlinePx)`, `ui_begin(640,480)`, `ui_text(f,x,y,argb,align,"s")`, `ui_rect`, `ui_image(tex, x,y,w,h,u0,v0,u1,v1,argb)`, `ui_end()`. All 2D is RB_BLEND (e_ui.h:11-12). Needed: `ui_line(x0,y0,x1,y1,width,argb)` (rotated quad, 2 tris, clipped to the screen or a HUD rectangle) and an additive option (call `r_draw_2d(..., RB_ADD)` directly). `r_draw_2d` clamps vertices to the screen (no guard band), which distorts a line crossing the edge: clip lines against the HUD box (Cohen-Sutherland) before building quads. |

**Input API (e_input.h) and short vs long press.** `in_poll()` once per 120 Hz tick reads `GetAsyncKeyState` for
watched keys only. Per key it keeps `s_down`, `s_prev` and a "tapped between polls" bit (e_input.cpp:765-782).
Queries: `in_key(vk)` held, `in_key_hit(vk)` pressed since the last poll, `in_action(id)` 0..1,
`in_action_hit(id)`, `in_axis(id)` -1..1 (joystick/rudder), `in_joy(i)->axis[8]/buttons/pov`. **There is no
release event and no hold duration.** The S key (short = cycle, long = designate) is therefore game code: keep
`heldTicks[]` per action, count while `in_action(a) > 0.5`, fire "long" the moment `heldTicks == LONG_TICKS`
(for example 36 ticks = 0.3 s at 120 Hz, gives immediate feedback instead of waiting for release), fire
"short" on release if the long threshold was never reached. Edge cases: a tap shorter than one poll is covered
by the `r & 1` bit, and the focus gate zeroes keys. A cleaner engine option is to append
`in_action_held_ticks(a)` and `in_action_released(a)` next to `in_action_hit` (the previous state is already stored).
The sim tick is 120 Hz, so timing is exact and replayable.

Bindings: `in_action_define(id, "iniName")` makes game-specific actions (the default `IA_*` set is driving: steer,
throttle, brake, handbrake...; call `in_bind_defaults()` once, then define `IA_PITCH_UP`, `IA_ROLL_LEFT`,
`IA_FAST` (A), `IA_SLOW` (Z), `IA_TARGET` (S), `IA_FIRE` (Space)...). Slots: 0 key, 1 alt key, 2 joystick.
`in_bind_load/in_bind_save` persist to the ini `[controls]` (e.g. `fire=key SPACE, j0 b0`). Keyboard flight needs input
shaping (ramped virtual stick); racer's `player.cpp` + `docs/research/KEYBOARD.md` document the lessons (full lock
in 0.05 s was undrivable): copy the idea, not the code.

### 1.7 Game-level files worth copying as patterns (not engine)

`src/main.cpp` (683 lines: WinMain, init order, integer QPC accumulator for the 120 Hz fixed tick, clamp 0.25 s,
max 8 ticks/frame, `-headless`, `-bench`, `-test`, `-shot`, `-exitafter`), `src/bench.cpp` (676 lines: zone
breakdown, 1%-low, pace stats), `src/replay.cpp` (input record + state hash), `src/tests.cpp` (dispatcher:
`{ "math", test_math, 1 }, ...` tests.cpp:12-21), `src/camera.cpp`, `src/hud.cpp`, `src/snd.cpp`, `src/scene.cpp`
(presets, fx pools). Copy `main.cpp`, `bench.cpp`, `tests.cpp`, `replay.cpp` skeletons and rewrite the content.

---------------------------------------------------------------------------------------------------------

## 2. Rendering the flight world within budget

Budgets (spike/RESULTS.md "Budgets for the VM", docs/PERF.md): own transform + submit ~1.2 ms per 1000 tris
(0.63 us/vertex, 0.8 us/triangle); **~8k tris/frame at 30 fps** (~10 ms CPU); ~115 Mpix/s fill (about 8x screen
overdraw at 30 fps, 640x480); multitexture ~1.7x fill; alpha blend ~1.2x; table fog 0-15%; textures <= 9 MB
resident (VRAM cliff: 120 -> 3 fps if exceeded); texture upload 2.9 ms each (never during flight); 256x256 max
size; 16-bit Z, no stencil, guard band 0. Racer's measured frame: sim 8.3 ms, world 8.1 ms (3000 tris),
cars 5.5 ms, sky 1.5 ms, HUD 0.75 ms (PERF.md section 3). Aim for 30 fps with 4 blanks; flight reads fine at 30.

### 2.1 Coordinates and projection

- Units metres, +Y up, island centred on the origin, sea level y = 0. All positions within +-20 km.
- FOV 70-75 degrees horizontal. Focal length at 640 px ~ 457 px (hfov 70). A triangle edge `e` at distance `d` covers
  `457 e / d` pixels: this drives every LOD distance below.
- Fog: W-based linear table fog, never vertex fog (3x submit cost). Fog colour = the sky's horizon colour.
  Start ~ 0.25 x end, end ~ 35 km; far plane 40 km. UNVERIFIED: how the Voodoo3 driver builds its 64-entry fog table
  at W range 1 m..40 km (resolution is probably fine for a linear ramp; exponential haze (`D3DFOG_EXP`) untested).

### 2.2 Island terrain (new module `e_terrain.*`)

Data (from the island compiler, Section 3.6): height field `H` on a 32 m grid, 512 x 512 samples = 16.4 km square
(uint16 heights, 0.1 m units, 512 KB), plus precomputed per-vertex colour (baked lighting incl. cast shadows) and a
class id per quad.

Chunks: 512 m x 512 m (32 x 32 = 1024 chunks, only those not entirely under water, ~300-450 for a 10 km island). Each
chunk has 5 LODs generated from the same field: 32 m, 64 m, 128 m, 256 m, 512 m grids (16x16, 8x8, 4x4, 2x2, 1x1
quads = 512 / 128 / 32 / 8 / 2 triangles; 289 / 81 / 25 / 9 / 4 vertices). Each LOD has its own VB (contiguous
vertices, Section 1.5). Storage ~11 KB per chunk, ~4-5 MB total: fine.

Distance LOD with screen-space rule `d_k = 28.6 * edge_k` (16 px triangles at 640 px, 70 degrees):

| LOD | grid | switch distance (3D, to the nearest point of the chunk) |
|---|---|---|
| 0 | 32 m | < 0.9 km |
| 1 | 64 m | < 1.8 km |
| 2 | 128 m | < 3.7 km |
| 3 | 256 m | < 7.3 km |
| 4 | 512 m | < 14.6 km, beyond: skipped or merged into the far sea/haze |

Use hysteresis (10%) to stop LOD flicker. Distance is 3D, so it automatically gives coarser terrain when flying
high. Expected cost for a 70 degree wedge fully covered by land: ~960 + 720 + 780 + 740 + 740 = ~4k triangles (ring
area x 2 / edge^2); real scenes are less (half the view is sky/sea, most of the far rings are water, whose chunks
are dropped), so **~1.5-3.5k terrain tris typically, 5k worst case, ~2.5 ms transform + 3 ms submit.** Add frustum
culling per chunk AABB (e_world's `frustum_aabb` is reusable) and a view-distance cap.

Cracks between LODs: use skirts (a downward strip on each chunk edge, depth = 0.1 x grid size, <= 40 m on LOD4):
costs 4 x quads x 2 tris (LOD0 +128 tris per chunk, so only add them on LOD >= 1, or stitch LOD0 border vertices
to the coarser grid as racer's terrain.py does, 'odd vertices on cell borders take the average of neighbours',
terrain.py docstring lines 12-16). Skirts are simpler and neighbour-independent.

Material classes (rock/basalt, ash, grass/forest, sand, lava): with one tiled 256 texture per class the chunk mesh is
split into per-class batches (sorted by material across chunks, as `world_visible` does with a counting sort,
e_world.cpp:439-447); texture changes stay under the 40-per-frame Low budget with 5 classes. Hard class borders
look blocky at 32 m, so tints must be close and vertex colour carries the macro shading/lighting. Upgrade path:
macro colour map (e.g. 4 x 4 textures of 256x256 over the island, 16 m/texel, ~2.7 MB with mips) multiplied by a bright
tiling detail texture in the validated 3-stage layout (`stage0 SELECTARG1 tex`, `stage1 MODULATE tex x current`,
`stage2 MODULATE diffuse`; no MODULATE2X, so the detail texture must be near-white; RESULTS.md "ValidateDevice"),
at 1.7x fill on terrain pixels only. Do not start with it.

`float terrain_height(x, z)` (bilinear over the grid, portable) is used for ground collision, takeoff/landing,
missile terrain hits and AI terrain avoidance. A matching portable `terrain_los(a, b)` (ray-march over the grid)
gives radar masking and missile line-of-sight.

### 2.3 Sea

Flat, so cheap. A single dynamic mesh built each frame with `r_draw_mem` (~100-200 triangles), re-centred under the
camera: concentric rings with growing spacing (e.g. radii 0.5, 2, 6, 15, 40 km, a few dozen vertices each) at y = 0,
extending just beyond the far plane (vertices past far get `sz = 1`, fully fogged). Texturing: one 256x256 sea
texture, UV = world XZ / period, **offset by the camera position snapped to a multiple of the period** so UVs stay
small (huge UVs lose precision in the hardware interpolators; UNVERIFIED but cheap to avoid) plus a time offset for
scrolling waves. Dynamic mesh means the scroll costs nothing. Fog hides the horizon: fog colour = sky horizon colour.
Cost: fill only, about one screen of single-textured pixels minus what terrain covers; fog adds 0-15%.

Detail: Voodoo3 cannot do a MODULATE2X detail layer, a second pass doubles sea fill, so skip it. A far-LOD trick that
works with one texture: two overlaid ring meshes at different UV scales is also a second pass; skip. Instead bake
swell patterns and a sun glitter streak into the sea texture and use vertex colour to darken with distance/angle.
Shore: clamp terrain below sea level to about -3 m with turquoise vertex colours; draw the sea blended (`RB_BLEND`,
alpha ~0.6) only in an inner ring around the island (within ~6 km of the coast) and opaque outside, so shallows show
through; add a foam strip (2 tris per 32 m of coast, additive/blend, UV scrolling) along the 0 m contour, generated
by the island compiler (marching squares, ~300-600 tris). Wake/impact splashes use the effect billboards.

### 2.4 Depth precision: 16-bit Z, near plane, far plane

Z buffer (`z/w`) resolution at distance z is about `z^2 / (near x 65536)`. With near 1 m: 0.15 m at 100 m, 15 m at
1 km, 1.5 km at 10 km. Unusable beyond a few hundred metres. With near 10 m, far 40 km: 6 m at 2 km, 150 m at 10 km.
Still bad for coplanar overlays at distance. **W-buffer** (Voodoo-style, believed to be a 16-bit floating point
depth, i.e. relative precision ~1/2000, so ~5 cm at 100 m, 0.5 m at 1 km, 5 m at 10 km; UNVERIFIED) is supported:
spike caps list `WBUFFER`, RESULTS.md conclusion 4 says "renders the same", and racer already runs with
`wantWBuffer=1`. So:

1. **Plan A (single pass, W-buffer):** near 2 m (chase camera behind a 15 m aircraft; cockpit view has nothing closer
   than ~1 m except the canopy frame: use near 1 m only in the cockpit view or hide it), far 40 km. First task in the
   jet project: a small probe (like racer's `spike/probe3d`) that draws, with W-buffer on: (a) two overlapping quads
   0.5 m apart at 50 m / 500 m / 5 km / 20 km, (b) a flat sea quad to 40 km with fog 8-35 km, (c) a runway quad
   0.15 m over terrain at 1 km/3 km with `zBias` 0/1/2. Look at the screenshots.
2. **Plan B (two depth ranges):** pass A = `near 800 m, far 45 km`: sky, sea, far terrain chunks (distance > 1 km),
   distant aircraft. `r_clear_z()`. Pass B = `near 2 m, far 2.5 km`: close terrain chunks, base, own aircraft,
   effects, HUD. Everything in B is nearer than everything in A by construction, so no cross-pass occlusion errors;
   chunks straddling the boundary are drawn in both with overlapping ranges. Cost: one extra Z clear (~600 KB fill) and
   double frustum/culling setup. The projection matrix is set per pass with `r_set_proj` (the backend already forwards it
   to D3D). Distant chunks must not draw in B (their W is beyond far) and the late sky (`world_set_sky_late`,
   Z-tested after opaque) belongs at the end of pass A.
3. Runway/apron markings over terrain: flatten the terrain under the airbase in the compiler and put the runway mesh
   0.15-0.3 m above it with `zBias = 1..2` (racer does the same for decals, DESIGN 3.2: 3-5 cm lift plus zBias).

### 2.5 Sky, sun, clouds

- **Panorama dome:** reuse `world_draw_sky` + `world_sky_load` + `world_set_sky_late(1)`. Needs a new panorama
  (`gen_sky.py` for an open-sea day: warm horizon haze, blue gradient, distant cloud banks, sun disc) with
  `elevLo/elevHi` ~ -10..+40 degrees in the SKY chunk (the loader accepts them), bottom rows exactly the fog colour
  (gen_sky.py computes the fog colour in closed form from the horizon band, docstring "fog = the average colour of
  the horizon band"). Low preset: 4 tiles of 256x128. At altitude the geometric horizon dips (3 km: 1.9 degrees, 10 km:
  3.4 degrees), so make the band below 0 degrees haze coloured.
- Cost seen in racer: 1.5 ms for sky. It stays cheap because only visible segments are drawn and, with the late
  sky, only unfilled pixels shade.
- **Sun glow:** existing `world_set_sun_glow` additive billboard (needs `glow` texture + `glowTex` creation as in
  scene.cpp). Good for backlit chase views.
- **Cloud deck:** 1-2 camera-centred quads (2 tris each) at 5-6 km altitude textured with a tileable cloud 256x256
  (4444 alpha), `RB_BLEND`, UV scrolling, fogged, fade alpha with distance. Cheap haze layer and a strong sense of
  speed when you cross it. Fill is full screen x ~1.2 when looking at it, so keep the deck off in view directions
  that do not need it.
- **Cumulus puffs:** 20-40 billboards (2 tris each), 128x128 4444 texture variations in a 2x2 atlas, spread 2-6 km up
  and 3-20 km out, sorted far to near (insertion sort as racer, DESIGN 3.8, <= 128 particles), fade alpha below 400 m
  distance (to avoid fill blowups and pops). 80 tris + fill. Copy `fx_quad` (scene.cpp:595) and `fx_draw_puffs`
  (scene.cpp:615) for camera-facing quads with rotation and atlas cell selection.

### 2.6 Aircraft, runway, base, effects

**Player aircraft:** 700-900 tris LOD0 (same envelope as the car's 600-800), LOD1 ~300, LOD2 ~100. Control
surfaces (2 stabilators, 2 flaperons, rudder, nozzle petals, gear if drawn) as separate small `.msh` with their hinge
at the local origin; the body mesh carries tags (`hinge_stab_l`, `hinge_flap_r`, `nozzle`, `rail_l/r`, `cam_cockpit`,
`cam_chase`; `MeshTag` = 16-char name, pos, dir, max 24) and the game draws each part with
`world = rotation(about hinge axis) * body`. This mirrors wheel.msh handling in racer. Each part is an extra draw call
(~8) but a few tris each. Lighting: `mesh_draw_lit` with sun dir in body space (cars do exactly this, 1.06 ms for all
cars in racer). Afterburner flame: a 12-16 triangle cone (or two crossed planes) with a flame texture, `RB_ADD`, `zWrite
0`, `fog 0` (additive + fog would add the fog colour; racer fades by hand, scene.cpp:386 comment), length/width
scaled by afterburner level, UV scroll for flicker, plus an additive glow billboard at the nozzle tag (fx_add_glow
scene.cpp:649).

**Enemies:** the same mesh at LOD by distance (`mesh_lod(m, dist, lodScale)`); 15 m aircraft: LOD0 < 150 m (45 px),
LOD1 < 600 m (11 px), LOD2 < 2.5 km (2.7 px), beyond: a 2-triangle billboard silhouette or a coloured dot (HUD
symbols convey the rest) and a contrail. Typical frame: player 800 + 2 enemies LOD0/1 (700) + 4 LOD2 (400) = ~1.9k.
Per-aircraft lit draw ~0.3-0.7 ms at LOD0/1 (racer: player LOD0 996 tris = 3.2 ms CPU, AI car 0.7 ms),
so cap simultaneous LOD0 to 1-2.

**Runway / base:** gen from the compiler into the airbase `.trk`: flat apron + runway ribbon (a 16 m x 2.5 km strip
is 2 triangles per 64 m texture repeat: ~100 tris), taxiways, painted markings in a 256x64 runway texture (the
racer `road_*` textures 256x64 are the same idea), hangars as boxes (12 tris each + roof, building class 60-300
tris), tower, a few tanks and trees as instanced `.msh` (INST records with colour multipliers and merged draws for
meshes <= 64 verts, e_world.h T15 notes). A base of ~1500 tris visible is realistic: it is only drawn within ~4 km.

**Missile smoke trails:** camera-facing ribbon: each missile keeps a ring buffer of ~24-32 points sampled at 20-30
Hz; per frame build 2 vertices per point, offset by `normalize(cross(segDir, viewDir)) * halfWidth(age)` (width 0.3 ->
3 m, alpha fading, grey-white), `RB_BLEND`, Z test no Z write, fog on, `r_draw_mem`. 2 tris per segment: ~50 tris
per missile, 8 live missiles = 400 tris. Model it on the skid-mark ring buffer `fx_draw_skids` (scene.cpp:542)
and clamp ribbon width/alpha near the camera so close trails do not blow the fill budget. Same ribbon code serves
contrails and a burning-aircraft smoke column.

**Explosions:** 6-12 additive/blended billboards per explosion from the smoke/glow atlas (racer's `smoke.bmp` is a 2x2
atlas of 64x64 puffs; `glow.bmp` is RB_ADD ready), expanding and fading over ~1.5 s, plus a debris point spray
(billboards 0.5 m), plus a brief screen flash (full-screen additive quad, 2 layers of fill). <= 30 quads each =
60 tris.

**HUD:** vector lines + text in the 640x480 virtual space (`ui_begin(640,480)`); green `0xFF40FF60`-ish colour,
additive for glow (`r_draw_2d(..., RB_ADD)`). An F/A-18-style HUD has ~100-200 line segments (pitch ladder
rotated by roll, heading tape, speed/altitude boxes, flight path marker, targeting box, bracket corners, DLZ
bar); 2 tris each = 200-400 tris, ~0.3-0.5 ms (racer's whole HUD = 0.75 ms, gen_ui.py). Text through
`font_create` once (GDI is slow, never per frame). Use `tools/art/gen_ui.py`'s own stroke-font pipeline
(glyph definitions + `render_text`, gen_ui.py:110-365) if a clean monospace HUD font is wanted; otherwise a GDI
"Lucida Console"/"Courier New" atlas is enough. Minimap/radar: a small textured quad plus dots (`ui_rect`),
~50 tris.

### 2.7 Per-frame estimate against the VM budget (30 fps, 8k tris, ~10 ms CPU render)

| Element | Tris | Draw calls | Notes |
|---|---|---|---|
| Terrain (chunks, 5 LODs) | 1.5-3.5k (5k max) | 15-40 (per chunk x class) | frustum + LOD culling |
| Sea (dynamic rings) | 150 | 1 | one texture, fog |
| Sky dome + sun glow | ~500 (visible segments) | ~8 | late sky |
| Clouds (deck 2 + puffs 80) | ~100 | 2 | blended, fill |
| Airbase (when in range) | 0.5-1.5k | 10-20 | e_world cells + INST |
| Player jet (LOD0 + 8 parts) + flame | ~900 | 10 | `mesh_draw_lit` |
| Enemies (4 typical, mixed LOD) | 400-1000 | 6-10 | |
| Missiles (4 live) + trails | 300-600 | 6-8 | blended ribbons |
| Explosions / debris | 60-200 | 2-4 | additive |
| HUD (2D lines, text, radar) | 300-500 | 3-5 | `r_draw_2d`, no transform |
| **Total** | **~5.5-9.5k** | **~65-110** | at the 8k limit; cut terrain distance, cloud count or enemy detail first |

Texture memory (565 with mips unless noted): 5 terrain classes 5 x 171 KB = 0.86 MB; sea 171 KB; runway/base
atlas 2 x 171 KB; sky 8 tiles x 128 KB = 1 MB (Low 0.5); jet livery + parts 3 x 171 KB; clouds (4444, 128) 85 KB;
effects (smoke, glow, flame; 4444) ~130 KB; HUD font ~170 KB; ~4.5 MB total, inside the 9 MB cap with margin for a
macro colour map later.

Measure first: `-bench` flights at three fixed altitudes (50 m over the runway, 1500 m over the volcano, 8000 m
over the sea) with the `bench.cpp` zone report (Section 5).

---------------------------------------------------------------------------------------------------------

## 3. Asset pipeline reuse

### 3.1 assetkit (engine/tools/assetkit, Python 3 + Pillow, no numpy)  COPY whole package

Rules: deterministic (`rng.Rng(seed, names...)`, never `hash()`), outputs only via `build(out_dir, kit)`, `--check`
rebuilds into a temp dir and compares md5. Modules (README.md section 2-12):

- `rng.py`: `value_noise`, `fbm(x, y, octaves, lacunarity, gain, seed)`, `ridged(...)` (rng.py:145-226) = the
  height-field ingredients for a volcano; `noise_grid(w, h, cells, octaves, seed, tile=True, kind="fbm"|"ridged")`
  gives tileable noise images.
- `mesh.py` (1628 lines): `MeshBuilder` (materials, parts, LODs, tags), primitives `box`, `cylinder`, `lathe`, `loft`,
  `extrude`, `quad`, `strip_along`, `crossed_billboard`, `sphere`, `mirror`, `smooth_normals(crease)`, `prelight`,
  `decimate(geo, target_tris)` (edge collapse with seam/boundary locks, mesh.py:1371), `reduce_stations`,
  `reduce_points` for LODs.
- `image.py`: `save_bmp` (exact magenta key, `_a.bmp` alpha), `Atlas` + `pack_rects`, `make_tileable`, `seam_error`,
  `to16` (565/1555/4444 preview), `mip_chain`.
- `export.py`: the only writers of `.msh` (RMSH v1) and `.trk` (`PoolEncoder`, `encode_pools`, `write_trk`).
- `validate.py`: budgets per class (validate.py:31-46 `BUDGETS`, `CLASS_PREFIXES`). Needs jet classes:
  `jet` (LODs e.g. [(600,950),(220,380),(70,140)], tags `cam_cockpit cam_chase nozzle`), `jetpart` (4-40),
  `missile` (30-120), `bldg_base` reuse `building`. Since the toolkit is being copied, edit the jet copy.
- `preview.py`: software rasteriser (z-buffer, fog, 16-bit quantise) to PNG: `views`, `turntable`, `lods`, `scene`.
  Use it to inspect terrain chunks, the island from above, the jet's LODs, with no VM.
- `build.py` / `tools/build_assets.py`: discovers `tools/art/gen_*.py` (each exposes `ASSETS`, `build(out_dir, kit)`),
  runs trackc for tracks; for jet, run the island compiler in the same slot.

### 3.2 Generators: what each gives

| Racer file | Reuse for jet |
|---|---|
| `tools/art/gen_car.py` (1228 lines) | **The jet.** Procedural, no external models. Body = lofted half (22 stations x 9 points) + mirror via `stations3d`/`section(z)` cross-section functions with `pchip`/`curve2` interpolation (gen_car.py:72-200), `body_shell` (239), `smooth_normals` (270), LOD1/2 as subsets of the same stations/points via `reduce_stations/points`. Wheel/part meshes built separately (562-625) and tags written for hubs. Livery bake: `rasterise(bake)` (850) evaluates 3D features at each texel's surface point (panel lines, stripes, vents, lights) by rasterising LOD0 triangles in UV space at 2x and box filtering: reuse for camouflage + panel lines + roundel. Replace the car sections by an F-16-like fuselage loft (16 stations x 8-10 points half: ~420 tris both halves), wing = tapered airfoil loft or `box`-like strips (2 x ~16), vertical fin + 2 stabilators (~12 each), intake lip (~40), canopy (own material, ~40), nozzle (~24): ~650-800 tris. Control surfaces: separate Geo per part, origin at the hinge. |
| `tools/art/gen_ground.py` (1147) | Textures via `ng` (noise grids), `aniso`, `worley` (672), `_granite` (719), `rock_granite` (747), `sand_shore` (795), `water` (815), `grass_field` (615), `forest_floor` (650), `detail_noise` (834), road asphalt `_asphalt`/`_paint` (250-430) for runway/taxiway + markings. Recolour for basalt (dark grey), ash (brown grey), cooled lava (black with red cracks, worley cells), volcanic sand, tropical grass; sea texture from `water` plus swell and glitter. All 256x256 or 128x128, tileable (`seam_ratio` checks). |
| `tools/art/gen_sky.py` (1038) | Structure for the panorama: `paint_sky`, `fbm1d` ridges, cloud streak layers, `apply_mist`, `light_txt` which writes the SKY light set (zenith/horizon/fog colours, sun direction/colour, ambient) and the closed-form fog colour. Replace the Finnish landscape layers with open sea + cloud banks + distant island silhouettes. Tile split `tiles_of(img, 8)` and low 4-tile variants. Mapping: u = azimuth 0..360 (0 = +Z north, clockwise to +X), v rows linear in elevation (horizon row `V_HORIZON`). |
| `tools/art/gen_ui.py` (1635) | HUD pipeline: stroke font renderer, sprite sheet + rect text file (`ui_sprites.txt`), dial, 9-slice panels, `glow`, `smoke` (+ `_a`) effects. Reuse `smoke`, `glow`; build a HUD symbol sheet (HAFU symbols, brackets, markers) the same way; the rev dial's needle code is the model for gauges. |
| `tools/art/gen_vegetation.py`, `gen_buildings.py`, `gen_fx2.py` | Palm/tropical trees as crossed billboards (alpha-tested keyed atlas, budget 4-6 tris); buildings: hangars/tower (`gen_buildings` facade + atlas helpers, 60-300 tris); `gen_fx2` shows text-on-board texturing and "one atlas, all objects" batching. |
| `tools/sfx/gen_sfx.py` (1162) | Synth framework (WAV 22 kHz mono, loops of whole cycles). Add jet engine (turbine whine + noise roar, 3-4 RPM loops crossfaded like the car engine), afterburner roar, wind, missile launch/motor, explosion, RWR/lock tones, cockpit beeps. |

### 3.3 trackc: what is reusable

`tools/trackc/` is a track compiler (spline, cross-sections, walls, AI line, scatter); mostly not applicable.
Reusable ideas/code:

- `terrain.py` (407 lines): `smoothstep`, an fBm height function blended toward a target height near features with a
  smoothstep weight (`1 - smoothstep(0, infl, distance)`): use for the flat airbase plateau blend, a pure function
  of (x, z) so near and far meshes agree. `build_far` (306-407): regular grids, odd border vertices averaged to the
  coarser neighbour (anti-crack), skirts/"tuck", water quads where terrain dips below level. For jet: new
  `islandc.py` with the same style but 512 m chunks and 5 LODs.
- `light.py` (138 lines): baked lighting formula `albedoTint x (ambient x sky x ao + sun x max(0, n.Ld) x (1 -
  strength x shadow)) x exposure` with a shadow mask. For terrain, replace the tree-disc stamping with a height
  field shadow march toward the sun (512 x 512 samples x ~48 steps in pure Python: tens of seconds, acceptable).
- `cells.py`, `trkfile.py` + `export.py::PoolEncoder`: write cells/batches/pools into `.trk`. Use for the airbase file.
  Terrain chunks use a new simple chunk container ("RISL", e_file chunk API, CRC) read by `e_terrain` so the engine
  keeps one container reader.
- `preview.py` (trackc): top-view PNG with cell grid and per-cell stats: model for an island overview preview.

### 3.4 Volcanic island recipe (all deterministic Python with assetkit.rng)

1. Base radial shape `R(r, theta)`: cone `h0 * (1 - r/R0)^1.6`, crater rim and bowl (subtract a smooth bowl inside
   r < 0.12 R0, ridged noise on the rim), coastline wobble from `fbm(theta)` so the island is not a circle.
   Volcano ~2200 m (UNVERIFIED design choice, flyable height for Ace Combat style), base radius ~5 km, island
   ~10-12 km wide, shelf and sea floor at -3 .. -60 m (clamped, for the shallow turquoise colour).
2. Detail: `fbm` (low-frequency erosion) + `ridged` for ridges and lava channels (radial stripes, `ridged` of
   `(theta * k, r)` coordinates), small satellite cones, a secondary caldera lake or lava pool optional.
3. Airbase: flat plateau (target height 8-15 m) on the coast, 3 km x 1.5 km, `smoothstep` blend 300 m; runway
   2.5-3 km x 45 m aligned to the prevailing wind, a taxiway loop, apron, a hill/sea approach in line with the runway
   (nothing tall within 5 km of the approach) so the automated takeoff is safe.
4. Classes per quad from height + slope + noise: beach sand (< 4 m), grass/forest (4-300 m, slope < 25 degrees), rock
   (slope > 30 degrees), ash/basalt (> 600 m), lava fields (radial channels + crater floor). Vertex colours =
   class tint x baked light (+ AO from slope curvature + shadow march).
5. Export: chunk meshes (5 LODs x 512 m chunks, per-class batches, skirts), `terrain_height` grid (uint16),
   shoreline foam strip, airbase `.trk` (cells 128 m, INST for buildings/trees, runway ribbon), a top-view PNG,
   per-chunk triangle report (checks the budget table like trackc's `_report.txt`).

### 3.5 Texture/mesh budget reminders

256 x 256 maximum, power of two, aspect <= 8:1, 24-bit BMP, RGB(255,0,255) key, `_a.bmp` alpha, names `[a-z0-9_]`, <= 31
chars (README section 2). `.msh` vertex colour = baked light (static) or white (`mesh_draw_lit`), normals separate,
indices LOD-relative (e_mesh.h notes), max 4096 verts per LOD for lit drawing.

---------------------------------------------------------------------------------------------------------

## 4. Build setup and folder layout

**Racer Makefile** (66 lines, `Makefile`): NMAKE, run in the VM with `w98 build <project>`.
`CFLAGS = /nologo /W3 /O2 /G6 /DWIN32 /D_WINDOWS /Isdk /Iengine\core /Iengine\coll /Iengine\phys /Iengine\world
/Iengine\rend /Iengine\plat /Isrc`; `LIBS = user32 gdi32 winmm ddraw dsound dinput advapi32`. Stage 1: engine sources
-> `engine\obj\*.obj` (inference rules `{engine\core}.cpp{engine\obj}.obj:` etc., plus `{sdk}.cpp{engine\obj}.obj:` for
`dx6guid.cpp`) -> `lib /out:engine\engine.lib $(EOBJS)`. Stage 2: `src\*.cpp` -> `obj\*.obj`, then
`link /subsystem:windows /out:racer.exe $(GOBJS) engine\engine.lib $(LIBS)`. All objects depend on all headers
(`$(EOBJS) $(GOBJS): $(HDRS)`). To adapt: drop `engine\phys` from the include path and `veh/phys/surf` from `EOBJS`,
add `e_terrain.obj` (and `r_` changes), replace `GOBJS`.

**sdk/** (`sdk/README.md`): Open Watcom DX6/DX7 headers `d3d.h d3dcaps.h d3dtypes.h ddraw.h` with 5 small fixes
(D3DVERTEX was 28 bytes without `nx`; GUIDs defined in our own `dx6guid.cpp`; do not link `dxguid.lib`). Source files
define `DIRECTDRAW_VERSION 0x0600`, `DIRECT3D_VERSION 0x0600`, `_ENABLE_AUTODEPEND` before including (only
`r_d3d6.cpp` and a few plat files). Copy the whole folder including `OWPL-LICENSE.txt` and README (the headers are
file-level copyleft: keep the licence file with them).

**Recommended layout, copy (not reference):**

```
jet/
  Makefile                 (adapted from racer; engine.lib + jet.exe)
  engine/{core,coll,world,rend,plat}/     COPY of racer/engine minus phys/, minus engine/obj and engine.lib
  engine/world/e_terrain.*                NEW (terrain chunks, height query, sea)
  engine/tools/assetkit/                  COPY (edit validate.py budget classes), drop samples/out previews
  sdk/                                    COPY
  src/                                    game (portable: flight, ai, weapons, radar, mission; Windows: main, scene, hud, snd)
  tools/{build_assets.py, art/gen_*.py, islandc/, sfx/, physh/}
  data/ tests/ docs/ dist/
```

Reason to copy: racer is a different game that may still change the engine (its T28 is in progress as of
2026-11-13; STATUS.md log) and jet needs incompatible edits (terrain LODs, depth passes, HUD lines). A reference
(`..\racer\engine`) would let a racer fix break jet or vice versa; both projects also share one VM and Samba path
where a half-edited racer engine would break jet builds. Record in `jet/docs/` the racer revision copied (date, and
the list of engine files touched), and port fixes by hand both ways when they matter (r_d3d6.cpp pacing, e_input).
Do not copy `racer/engine/engine.lib`, `engine/obj/*.obj`, `.pyc`, `obj/`, replay/log files. Setup98 installer later
via `dev/setup98/AGENTS.md`.

Preset/ini: racer's `scene_preset_resolve` (scene.cpp:178) with Low/Medium/High overrides (viewDist, lodDist,
particles, texBudgetKB, sunGlow) is the model for jet presets (terrain LOD scale, cloud count, trail length). The
real-PC target (Pentium M, Radeon 9000, DX8) will accept 2-4x these numbers.

---------------------------------------------------------------------------------------------------------

## 5. Testing patterns worth keeping (DESIGN 9, PERF.md)

1. **Portable core + Mac headless build.** `tools/physh/Makefile`: `clang++ -std=c++98 -O2 -Wall -Wno-unused-function
   -DE_NO_RENDER` over every portable file plus `physh.cpp`, run `physh test <name>` in seconds. Keep that split:
   flight model, AI, weapons, radar/DLZ, missile guidance, mission logic are portable and unit-tested on the Mac;
   only `scene/hud/snd/main/camera` are Windows. Compiling with both VC6 `/W3` and clang catches dialect mistakes
   (VC6: `for (int i...)` leaks; no `<cstdint>`; declare variables at block top).
2. **`-test NAME` dispatcher** (`src/tests.cpp:12-21` table `{name, fn, flag}`; exit code = failures; `-test all`).
   For jet: `math`, `flight` (trim, climb rate, G limit, 150 kt floor), `missile` (pure pursuit vs lead, Rmax/Rne/Rmin
   numbers versus the DLZ tables), `terrain` (height query vs grid, LOS), `lod` (distance -> LOD tables), `radar`, `ai`,
   `determinism`, `replay`.
3. **Deterministic sim + replay.** Fixed 120 Hz tick, integer QPC accumulator (main.cpp:8), one RNG (`ERand`, xorshift),
   no wall-clock in sim, `-record f` stores quantised inputs + state hash every 120 ticks, `-replay f` verifies
   (`hash: tick=N ok|DIFF`). Every flight saves `last.rpl` (racer T27): a user bug report becomes a replay. Hold
   FPU state: `plat_fpu_ok()` check before each tick (see 1.6 on PC24 vs doubles).
4. **`-bench N`** (`bench.cpp`): autopilot flight along a scripted route, prints `fps_avg`, `1%-low`, `p99_ms`,
   missed slots, per-section ms (sim, terrain, sea, sky, aircraft, fx, HUD, present) and counters (tris, draws,
   texture changes) per route zone; `pass=` against the targets (>= 29.8 fps, <= 2% missed slots). Use `-seed` and
   same-seed back-to-back A/B runs; expect +-2% host noise. Windows must be visible (CLAUDE.md section 5). Run
   under the vmlock, no VNC.
5. **Log-based debugging**: one fact per line, `tag: key=value ...`, levels via `-log [N]`, separate log file per mode
   (racer_test.log, racer_bench.log...), never overwrite the player's log. Debug flight logic, balance, missile
   hit rates and AI from logs and stat dumps; screenshots only for visuals (user preference).
6. **Scripted input**: `-script file` (time command lines, DESIGN 9.2) and `-mscript` for menu flows; use for
   takeoff automation tests, "fire at target after t seconds", and regression of the S short/long press logic.
7. **Screenshots with fixed cameras**: `-shot T file.bmp` + `-exitafter` + `-cam`/`-camat` to place the camera, `sips
   -s format png` on the Mac. Use for the LOD/crack review at set viewpoints (high over the volcano, low over the
   runway, horizon at 8 km altitude).
8. **Asset determinism check**: `python3 tools/build_assets.py --check` (md5 of a rebuild vs `data/`),
   `validate.py` before any VM run, preview PNGs per generator to review models/terrain without the VM.
9. **AI-only regression** (racer `-airace`): jet equivalent `-aifight N` headless dogfights: kills, missile
   hits/misses, stuck/crash counts, terrain collisions; thresholds in a script, runs on the Mac too.
10. **Depth/fog probe first** (racer's `spike/probe3d`): before building the terrain, a small VM test of W-buffer
    precision at 50 m..30 km, 40 km fog, huge UV, texture repeat, `r_clear_z` cost. Spike results are the contract.

---------------------------------------------------------------------------------------------------------

## 6. Recommendations and open risks

1. Copy the engine now (Section 4), drop `phys/`, keep `coll/` compiled but unused; fix window title/error box;
   decide PC24 vs PC53 for the flight model.
2. First technical task: a depth/fog/UV probe at flight scales (2.4). It decides Plan A (W-buffer, one pass) vs
   Plan B (two depth ranges + `r_clear_z`). Everything else rests on it.
3. Keep `e_world` for sky + airbase instances; write `e_terrain` (5 LOD ring chunks of 512 m with per-LOD VBs and
   skirts) and a dynamic ring-mesh sea. Do not widen `WORLD_CELL` for the whole island.
4. Aircraft = `mesh_draw_lit` + separate part meshes with hinge tags; sprites/dots beyond ~2.5 km; HUD = new
   `ui_line` + additive `r_draw_2d`; trails/explosions/clouds = the racer fx quad/ring-buffer patterns.
5. Input: build short/long press on top of `in_action`/`in_key` (hold counter in game code, fire long at the threshold);
   optionally append `in_action_held_ticks/released` to e_input.
6. Reuse assetkit + gen_car.py approach for the jet, gen_ground.py textures, gen_sky.py panorama, plus a new
   `tools/islandc` (fBm/ridged height field, class + bake, chunk LODs, airbase `.trk`, foam strip).
7. Keep the racer discipline: portable sim/Mac tests, deterministic replay, `-bench` with section/zone logging,
   `-test` dispatcher, grep-friendly logs, one VM lock per visual test.
8. Risks: (a) W-buffer precision and far-fog behaviour at 40 km UNVERIFIED; (b) terrain fill/overdraw while the sea fills
   half the screen plus blended shallows and clouds (stay under ~6x overdraw); (c) CPU lighting of many aircraft;
   (d) texture seams on class borders; (e) `mesh_draw_lit` 4096-vertex cap and 4 LODs (fine for the jet);
   (f) real hardware will differ (spike numbers are emulator numbers: re-check on the real PCs).
