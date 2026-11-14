# e_terrain: island terrain, queries, LOD terrain and sea (T04)

Files: `e_terrain.h` (API), `e_terrain.cpp` (portable: load, queries, LOS march, LOD selection; simh links it),
`e_terrain_draw.cpp` (render: terrain batches, sea ring, shallow sea, foam), `e_terrain_view.cpp` +
`e_terrain_view.mak` (stand-alone viewer for screenshots/bench; not part of jet.exe). Tests: `src/tests_terrain.cpp`
(`jet.exe -test terrain`, `simh test terrain`, `simh island`). File format: docstring of `tools/islandc/isl.py`.

## Use from the game

```c
Terrain t;                                   /* big-ish struct: make it static */
terrain_load(&t, path);                      /* 0 = missing/bad file: open sea at y = 0, default runway (logged) */
terrain_gpu_init(&t);                        /* render side, once after r_init (also without an island: sea only) */
/* per frame, after r_begin_frame and r_set_fog: */
TerrainDrawCfg *c = terrain_draw_cfg();      /* set once from the preset / params */
fog = terrain_fog_565fix(sky.fog);           /* see "Fog colour on the Voodoo3" below (also World.sky.fog, skyBot) */
c->fogRGB = fog; c->fogStart = fs; c->fogEnd = fe;   /* what you passed to r_set_fog (foam + fog band use it) */
terrain_draw(&t, &view, TERRAIN_LODSCALE_LOW, farM); /* opaque terrain, first */
/* ... base (e_world), aircraft ... */
sea_draw(&t, &view, seconds);                /* shallow sea + opaque ring + foam; before the late sky */
const TerrainStats *s = terrain_stats();     /* tris, skirtTris, foamTris, chunksByLod[5], draws, seaTris ... */
terrain_free(&t);                            /* frees the GPU side too */
```
`WView` is e_world's (`world_view_lookat` or your own eye/view/proj/frustum; `viewDist` = far plane: the sea ring
reaches the far plane's corners, from `view->proj`). **Keep viewDist + ~2 km below 65.5 km on the Voodoo3** (fog/W wrap). `farDist` of terrain_draw culls chunks whose nearest point is farther: pass the far plane's
corner distance (scene_far_radial), the frustum's planar far plane culls the rest (a radius = the far plane cut a
circle out of the ground seen from altitude).

## Query rules

| Function | Rule |
|---|---|
| `terrain_height(x, z)` | LOD0 render mesh: each 32 m quad split SW-NE into (SW, NW, NE) + (SW, NE, SE), planar per triangle. Matches the mesh within 0.025 m (bilinear would differ by up to 5.2 m on steep quads). Outside the grid: -60 |
| `terrain_surface` | max(height, seaLevel 0) |
| `terrain_class` | CLAS quad: `TCLASS_WATER SAND GRASS ROCK ASH LAVA RUNWAY APRON` (0-7); outside = water |
| `terrain_normal` | unit normal of that LOD0 triangle |
| `terrain_los(a, b)` | 1 = clear: segment strictly above the mesh everywhere and both ends above sea level |
| `terrain_segment_hit(a, b, hit)` | first contact with the mesh **or the sea plane** (missiles splash); hit = point (y = 0 for the sea) |
| `terrain_block_max(x, z)`, `terrain_area_max(rect)` | HMAX (256 m blocks) maxima: cheap look-ahead for PULL UP / AI |
| `terrain_base` | BASE chunk (runway, threshold spawn, 4 air spawns, 9 collision boxes) |

LOS/segment march: grid clip, then a 2D DDA over 2048 m super blocks -> 256 m HMAX blocks -> 32 m cells; a level is
entered only where the ray is below its maximum (5 cm margin), and a cell is tested exactly (ray height minus the
two planes, piecewise linear, no sampling). The test compares it with `terrain_segment_ref` (every cell the path
can touch) on 1000 random rays.

## Rendering

- **Selection** (`terrain_select`, portable): 4 x 4-chunk groups (2 km) are culled first (distance, frustum
  in/out/intersecting), then chunks. Only chunks in the frustum get a LOD (others `TERRAIN_NOLOD`): an edge shared
  with an invisible chunk is outside the frustum too, so skirts / restriction / fan stitching toward it never show.
  Water chunks use an AABB raised to sea level + 1 m, so their shallow-sea fan is not culled with the seabed.
- **LOD**: chunk LOD k ends at `lodScale * lodK * lodEdge[k]` (lodK 28.6; with lodScale 1: 0.9 / 1.8 / 3.7 /
  7.3 km), LOD4 to farDist. Distance = eye to the chunk AABB centre (`distMode 1`, default) or nearest point
  (`distMode 0`, DESIGN). 10 % hysteresis frame to frame (squared distances). A restriction pass then makes
  neighbours differ by at most one LOD (T03 sized the skirts for +-1). Underwater-only chunks select LOD0/1 but draw
  their LOD2 mesh (alias).
- **lodScale convention**: DESIGN 8.2's formula (distances x lodScale: larger = finer). DESIGN 9.3 lists High as
  "0.5 (8 px)", which only fits the inverse convention; here High = `TERRAIN_LODSCALE_HIGH` 1.5 (~11k tris, High
  budget 12k), Low = `TERRAIN_LODSCALE_LOW` 0.85.
- **Skirts** (`skirtMode 0`, default): only the sides whose 4-neighbour is at another mesh LOD. The loader counts
  each batch's skirt triangles per side (geometric: all three vertices on that chunk edge); terrain_gpu_init sorts
  them S E N W inside the batch, so a chunk draws only the needed sides (cuts skirts ~60 % vs "whole chunk" and
  ~90 % vs always). 1 = always, 2 = never (crack check: `e_terrain_view -view slope -skirts 2`).
- **Batches**: per material (5 terrain textures) one pass over the visible chunks. terrain_gpu_init reorders each
  chunk-LOD vertex block in place so a batch's vertices are contiguous (skirt bottoms last); chunks with mesh LOD >=
  `mergeLod` are copied into one `r_draw_mem` per material (default 0 = everything merged: 2-5 terrain draws per
  frame), finer ones draw straight from the block. Measured: per draw call ~38 us on the VM (342 draws = 22 ms vs
  4 draws = 9.3 ms for the same 4.5k tris), so keep merging on.
- **Sea** (`sea_draw`, after the opaque scene, before the late sky):
  1. shallow sea: flat fans at y = 0 over every visible water chunk (stored chunk with a sample below sea level),
     sea texture, RB_BLEND with **Z write** and `seaZBias` (1); vertex alpha `seaShallowAlpha` (0.65) where the water
     is shallower than `seaDeepFrom` (30 m), ramping to 1.0 at `seaDeepTo` (58 m) so it meets the opaque sea
     without a seam (non-stored chunks are deeper than 59.5 m). 256 m lattice (8-tri fan) for chunks at LOD <= 3 /
     `shallowLod`, 512 m (2 tris) beyond; a 256 m fan drops its edge midpoint toward a 512 m neighbour (no
     T-junctions); deep triangles are skipped and vertices are emitted only when used.
  2. opaque ring: radii 0.4 / 1.5 / 4 / 9 / 18 km (those below 0.85 x fogEnd), 0.85 x fogEnd, viewDist, and an
     outer ring at the far plane's corner distance / cos(pi / seaSegs) (T10b: covers the whole frustum, so no disc
     edge shows from altitude), `seaSegs` 16, per-quad frustum cull, and every quad **clipped on the CPU to view depth
     w <= 0.998 x viewDist** (the renderer only rejects triangles wholly beyond far, and the Voodoo3's fog/W wrap past
     ~65.5 km). Coplanar with the fans, so the fans' biased Z masks it and the shelf terrain shows through the fans only.
  3. fog completion band (when `cfg.fogEnd` > 0): a fog-coloured strip on the sea plane between the lines w = 0.85 x
     fogEnd and w = viewDist (w = depth along the view direction, what the W fog table uses), alpha linear in w, 4
     pieces, sideways +-outer radius, zBias 2 (over the ring and the shallow fans): the Voodoo fog table tops out at
     ~96 %. (T04 had alpha by horizontal radius on the ring vertices: from altitude that drew a fog ring = the "ball".)
  4. foam: the foam batches of visible chunks at mesh LOD 0-2, RB_ADD, `foamZBias` 2, drawn with a **black fog**
     (fades out with distance instead of adding fog colour), fog restored from `cfg.fogRGB/Start/End`.
  UVs: (world xz - camera xz snapped to `seaPeriod` 128 m) / period + scroll (`seaScrollU/V` = (-0.87, -0.50) x
  0.012 per s, wrapped). Vertex colour 1.0 at the eye -> `seaFarShade` 0.8 at the far plane (by horizontal distance squared; T10b dropped
  the eye height, which shaded a disc seen from altitude).
- **Fog colour on the Voodoo3 (important for the scene)**: texels are truncated to 565 but fog and vertex colours
  come out one 565 level darker, so with the light set's fog (196,212,228) the fogged sea and e_world's
  vertex-coloured lower sky band show (189,210,222) against the panorama's fog rows (197,214,230): a visible line
  where the sea meets the sky. Use `terrain_fog_565fix(fog)` for r_set_fog, `cfg.fogRGB`, `World.sky.fog` and every
  `World.skyBot[]` after world_sky_load (what e_terrain_view does): then all three match exactly (measured).

## Viewer (`e_terrain_view.exe`, screenshots and bench outside jet.exe)

Build `w98 build jet /f engine\world\e_terrain_view.mak` (after `w98 build jet`); run with `--show`:
`e_terrain_view.exe -all` (every preset: shot `tv_<name>.bmp` next to the exe + bench + 8-heading sweep),
`-view runway|runway_sea|crater|sea|coast_low|threshold|slope`, `-camat X Y Z HDG PITCH`, `-shot F`, `-bench N`,
`-sweep 1`, `-lodscale F`, `-far M`, `-dist 0|1`, `-skirts 0|1|2`, `-merge L`, `-shallowlod 3|4`, `-seabias N`,
`-foambias N`, `-vp N` (1/N viewport: CPU cost without fill), `-pair D` (two shots D m apart: z-fight diff),
`-fogfix 0|1`, `-nobase -nosky -nosea`, `-window`. Fullscreen 800x600x16 by default (Low). Log:
`engine\world\e_terrain_view.log` ("bench:" and "terrain:" lines). Frame order = DESIGN 9.4 (terrain, base.trk via
e_world, sea_draw, late sky).

## Numbers

See the T04 section of `docs/HANDOFF.md` (triangle counts at the ENGINE 2.7 viewpoints, bench, LOS timing).
