# assetkit: the shared asset toolkit

**Version 1.2** (`assetkit.__version__`; history in [section 15](#15-version-history)). Mac-side Python package of
the reusable engine (DESIGN 8.1). **Every** asset generator (car, textures,
vegetation, buildings, sky, HUD art, sounds) and the track compiler are built on it. Python 3 (3.9+) and
Pillow only; no numpy. Same inputs give the same bytes.

Rules:
- Asset tasks own only their generator `tools/art/gen_<name>.py` and its outputs. **Never write your own
  exporter, BMP writer, mesh format code or validator.** Need a toolkit change? Ask the toolkit owner (or
  propose it in your task report).
- Outputs go into `data/` only through `build(out_dir, kit)`; preview PNGs into `tools/art/preview/`.
- Check every generator with `python3 tools/build_assets.py --only <name>` (validates) and `--check`
  (determinism) before any VM test.

Contents: [1 Quick start](#1-quick-start) · [2 Conventions](#2-conventions) · [3 Generator template](#3-generator-template) ·
[4 Budgets](#4-budgets-and-classes) · [5 API: rng](#5-rng) · [6 palette](#6-palette) · [7 mesh](#7-mesh) ·
[8 image](#8-image) · [9 export](#9-export-binary-formats) · [10 validate](#10-validate) · [11 preview](#11-preview) ·
[12 build + Kit](#12-build-and-kit) · [13 Recipes](#13-recipes) · [14 Pitfalls](#14-pitfalls) ·
[15 Version history](#15-version-history)

## 1. Quick start

```sh
cd ~/Documents/86box/Shared/dev/racer
python3 engine/tools/assetkit/selftest.py            # toolkit self-test, exit code = failures
python3 tools/build_assets.py                        # rebuild everything into data/, validate data/
python3 tools/build_assets.py --only car,sfx         # some generators (gen_<name>.py, a track id, or a kind: art sfx sample tracks)
python3 tools/build_assets.py --check                # rebuild into a temp dir, compare md5 with data/ (writes nothing)
python3 tools/build_assets.py --list                 # discovered generators; also -v, --no-previews, --keep-temp
python3 engine/tools/assetkit/validate.py data/meshes/v10gt.msh data/tex/car_*.bmp   # validator, exit = errors
PYTHONPATH=engine/tools python3 -m assetkit.validate data/                            # same, module form
make -C tools assets | assets-check | selftest | validate                            # Mac make targets
```
The worked example is `samples/gen_sample.py` (box house with a keyed sign, crossed-billboard tree, keyed atlas,
previews). Its outputs live in `samples/out/` (never in `data/`); look at `samples/out/preview/*.png`.

## 2. Conventions

**Axes and units (DESIGN 2.3).** Metres. Left-handed, D3D: **+X right/east, +Y up, +Z forward/north.**
Yaw positive turns +Z toward +X (clockwise seen from above). Angles in the API are **degrees**.

**Winding.** Triangles are **clockwise seen from the front** (D3D culls counter-clockwise). With the plain cross
product the outward normal of triangle (a, b, c) is `cross(b - a, c - a)`. Every primitive below produces
outward/front-facing results; the rules for the sweep primitives are:
- `quad(p0, p1, p2, p3)`: corners clockwise from the front = bottom-left, top-left, top-right, bottom-right.
- `loft`/grid rule: front = `cross(station direction, point direction)`. Stations running toward -Z (front to
  back of a car) with points running bottom -> top on the +X side face outward. Capped lofts fix themselves.
- `strip_along`/`extrude` (open shapes): front = the profile's walking direction rotated 90 degrees
  counter-clockwise in the (lat, h) plane: a road profile walked left -> right faces up; a wall profile walked
  bottom -> top on the right side faces left (toward the road). Closed `extrude` shapes always face outward.
- `lathe`: profile from bottom to top faces outward.
- `mirror`, negative `scale` and mirroring `matrix` flip the winding automatically.

**Facing and origins (DESIGN 8.3).**
- Objects (buildings, signs, props, trees): origin at the **ground centre of the footprint** (y = 0 is the
  ground). The side meant to be seen (house front, sign face) **faces -Z**: a viewer at negative Z looking north
  sees it; trackc turns instances so signs face oncoming traffic.
- Cars: origin at the CoG projected to the ground (y = 0 = ground under static ride height), **nose toward +Z**.
  Tags `wheel_fl wheel_fr wheel_rl wheel_rr` (hub centres), `brake_l brake_r`, `cam_bumper`.
- Wheels: separate left/right meshes, spin axis X, centred on the hub.

**Textures (DESIGN 3.3).** Power-of-two sides, max 256 x 256, aspect <= 8:1; 24-bit uncompressed bottom-up
BMP (BI_RGB). Texture coordinate v = 0 is the **top** row of the image. Transparency: **exact magenta
(255, 0, 255)** key for cut-outs (foliage, fences, signs; ALPHATEST materials), never anti-aliased against the
key (`save_bmp(keyed=True)` thresholds and bleeds edges); smooth alpha = an extra 8-bit grey `<name>_a.bmp`
(`save_bmp(alpha=True)`; smoke, shadow blob, glow, glyphs). Material texture names = file name without path or
extension, `[a-z0-9_]`, <= 31 characters; the engine loads `data/tex/<name>.bmp`.

**Vertex colours.** `RVert1.color` is 0xAARRGGBB and multiplies the texture (stage 0 MODULATE); the engine never
lights static geometry. Objects: bake with `prelight(g, mode="object")`. Cars: white (`mode="white"`), lit on the
CPU at run time. trackc: `mode="world"` with the setting's sun.

**Naming.** Lowercase `snake_case` file names. Meshes `data/meshes/<name>.msh`, textures `data/tex/<name>.bmp`,
sounds `data/sfx/<name>.wav`, sky tiles `data/sky/`. The file-name prefix picks the budget class (section 4).

**Determinism.** Only `assetkit.rng` (or `random.Random(int)`) for randomness; never `hash()`, time, sets or
`os.listdir` order without sorting. One `Rng(seed, "thing", i)` per independent stream.

## 3. Generator template

Save as `tools/art/gen_<name>.py`. `python3 tools/build_assets.py --only <name>` runs it. (This exact file
passes the validator; a bare 10-triangle box house would fail the `building` budget of 60-300 triangles.)

```python
"""gen_bales.py - silage bales (A4). Outputs: see ASSETS."""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "tools"))
from assetkit import image as I, mesh as M, palette as P, preview as V
from assetkit.rng import Rng, noise_grid

SEED = 31
ASSETS = [                                   # every output, relative to the data root
    "tex/bale_white.bmp",
    {"file": "meshes/bale_round.msh", "class": "prop"},     # dict form declares the budget class
]
ORDER = 50                                   # optional: lower runs first (default 50)

def bale_texture():
    img = I.new(64, 64, P.C["bale_white"])
    return I.apply_noise(img, noise_grid(64, 64, cells=4, seed=SEED), amount=0.06)

def bale():
    mb = M.MeshBuilder("bale_round")
    plastic = mb.material("plastic", tex="bale_white")
    g = M.cylinder(0.6, 1.2, seg=10, pos=(-0.6, 0.6, 0), axis="x", mat=plastic)   # 40 tris, lying on the ground
    M.prelight(g, mode="object")
    mb.lod(120.0); mb.add(g)                 # LOD0 to 120 m; add mb.lod() + a simpler Geo for a LOD1
    return mb

def build(out_dir, kit):                     # out_dir = data root of this run (a temp dir); use kit.* paths
    kit.save_bmp("tex/bale_white.bmp", bale_texture())
    mb = bale()
    kit.save_msh("meshes/bale_round.msh", mb)
    if kit.previews:                         # False during --check: skip slow previews
        kit.save_preview("bale_views.png", V.views(mb, 512, tex_dirs=kit.tex_dirs, title="bale_round"))

if __name__ == "__main__":
    from assetkit import build as B
    sys.exit(B.main_for(__file__))           # python3 tools/art/gen_bales.py [--check]
```
What happens on a run: the runner imports the module (only the selected ones with `--only`; an import error
fails just that generator), calls `build(tmp, kit)` with a fresh temp dir, checks that
every `ASSETS` file was written (undeclared extra files are a warning), validates all outputs (format, budgets,
textures referenced by meshes exist in `tmp/tex` or `data/tex`), then copies changed files into `data/`
(`--check`: compares md5 instead and writes nothing). Exit code 1 on any error. Keep module top level free of
side effects (the module is imported once per run). Read other generators' outputs with `kit.find("tex/x.bmp")`.
Sounds (`tools/sfx/gen_*.py`) follow the same protocol (`ASSETS` like `"sfx/eng_on_1000.wav"`).

**Your checklist:** outputs declared in `ASSETS`; `--only <name>` shows `budget ok`; `--check` shows `identical`;
look at the preview PNGs (views from all sides, turntable, LOD sheet, contact sheet of textures, `to16` preview
for gradients); write the numbers (tris per LOD, texture sizes) in your report.

## 4. Budgets and classes

Validator defaults (`validate.BUDGETS`, DESIGN 8.1). Triangles per LOD; LODs beyond the listed ones must not
have more triangles than the previous LOD.

| Class | LOD triangles | Notes |
|---|---|---|
| `car` | LOD0 600-800, LOD1 220-300, LOD2 70-110 | exactly 3 LODs; tags `wheel_fl wheel_fr wheel_rl wheel_rr brake_l brake_r cam_bumper` |
| `wheel` | LOD0 8-64, LOD1 4-24 | shared 128x128 texture |
| `tree` | 4-6 | crossed billboard 2-3 planes (cull_none ALPHATEST) |
| `bush` | 2-4 | |
| `treeline` | 2-16 | forest-edge cards |
| `building` | 60-300 | barn, house, church, sauna, shop |
| `prop` | 8-40 | post, sign, bale, mailbox, pole, chevron, boulder |
| `free` | none | samples, tests (`sample*`, `test*`, `dbg_*`) |
| `jet` | LOD0 600-950, LOD1 220-380, LOD2 70-140 | jet DESIGN 8.8; exactly 3 LODs; tags `cam_cockpit cam_chase nozzle hinge_stab_l hinge_stab_r hinge_flap_l hinge_flap_r rail_0..rail_7 flare_l flare_r` |
| `jetpart` | 4-60 | stabilators, flaperons, nozzle, gear: origin at the hinge, hinge axis = local X (warning if the origin is outside the box) |
| `missile` | LOD0 30-120, LOD1 1-20 | on rails and in flight |
| `bldg` | 12-300 | jet base buildings (hangar, tower, tank, bunker, `bldg_`) |

Class lookup: the `ASSETS` dict `"class"` wins, else the file-name prefix (`validate.CLASS_PREFIXES`:
`v10gt`/`car_` car, `wheel` wheel, `tree`, `bush`, `treeline`, `barn farmhouse house church sauna shop
building shelter jetty` building, `jetpart*` jetpart, `jet*` jet, `missile* msl_` missile, `hangar tower tank bunker
bldg_` bldg, `post sign bale hay mailbox pole chevron kmpost boulder rock prop_ fence rail`
prop), else a warning "no budget class". CLI: `--class prop` forces one; `--list-classes` prints the table.
Texture rules: all textures power of two, <= 256, aspect <= 8; `car_*` exactly 256x256; `wheel*`, `env_*` <= 128;
everything under a `sky/` folder 256x256 (High tiles) or 256x128 (Low tiles); `fx_*` (jet effect atlases)
128x128 or 256x256.

## 5. rng

```python
from assetkit import rng
r = rng.Rng(11, "tree", "pine", 3)   # random.Random seeded by stable_hash(11, "tree", "pine", 3)
r.random(); r.uniform(2, 3); r.gauss(0, 1); r.choice(seq); r.shuffle(lst)   # standard methods (stable)
r.range(2.0, 3.0)                    # float in [2, 3)
r.int(1, 6)                          # int in [1, 6] inclusive
r.chance(0.25)                       # True 25% of the time
r.pick(["pine", "spruce"])           # one element
r.weighted([("pine", 3), ("birch", 1)])  # weighted choice
r.jitter(10.0, 0.5)                  # 10 +- 0.5
r.sign(); r.unit2()                  # -1/+1; random unit (x, z)
r.child("bark")                      # independent sub-stream Rng(11, "tree", "pine", 3, "bark")
rng.stable_hash(1, "a")              # 64-bit FNV-1a of the parts (str/int/float/bytes/tuples); stable_hash32()
rng.hash2(ix, iy, seed)              # lattice hash in [0, 1) (hash2i: u32, hash1: 1D)
rng.value_noise(x, y, seed=4, period=8)          # smooth noise [0, 1]; period (int or (px, py)) makes it tile
rng.value_noise_1d(x, seed=4, period=None)       # 1D (skylines, profiles)
rng.fbm(x, y, octaves=4, lacunarity=2, gain=0.5, seed=4, period=8)   # fractal sum [0, 1]
rng.ridged(x, y, octaves=4, seed=4)              # crests [0, 1] (rock, cracks, mountain skylines)
rng.noise_grid(256, 256, cells=8, octaves=4, gain=0.5, seed=4, tile=True, kind="fbm")  # rows of [0,1], stretched, tileable
```

## 6. palette

```python
from assetkit import palette as P
P.C["punamulta"]            # named colours (r, g, b): sun ambient fog horizon zenith asphalt asphalt_worn line_white
                            # line_yellow gravel dirt sand_shore grass grass_dry field rye forest_floor moss rock_granite
                            # rock_dark lake lake_deep spruce pine pine_bark birch_bark birch_mark birch_leaf bush
                            # punamulta punamulta_dark trim_white wall_yellow roof_tin roof_tile window wood_grey
                            # wood_dark concrete bale_white sign_yellow sign_red sign_black sign_white sign_blue glass
P.KEY                       # (255, 0, 255): the transparency key; never a real colour
P.SUN_DIR                   # unit vector toward the low NW evening sun (elev P.SUN_ELEV_DEG 6, azimuth P.SUN_AZIM_DEG -45)
P.KEY_DIR                   # object 'studio' light (front-left-above in mesh space) used by prelight(mode="object")
P.dir_from_angles(-45, 6)   # vector toward a light at compass yaw (0 = +Z north, 90 = +X east) and elevation
P.lerp(a, b, 0.3); P.shade(c, 0.8); P.mul(a, b); P.add(a, b)   # colour math (ints, clamped)
P.desaturate(c, 0.5); P.warm(c, 0.3); P.luma(c)                # grey mix, evening warmth, brightness 0..255
P.jitter(c, rng, 0.08)      # +-8% brightness with an Rng
P.argb(c, a=255)            # pack to u32 0xAARRGGBB (vertex colour); P.from_argb(u) -> (r, g, b, a)
P.q565(c); P.q555(c); P.q4444(c)   # colour after 16-bit conversion
P.is_near_key(c)            # True if it would read as magenta-tinted; P.clamp8(v); P.rgb(c); P.hexstr(c)
P.FOG_START_FRAC            # 0.40 (Low preset)
```
The sky task (A5) owns the final light set (`data/sky/<set>.txt`); palette values are the defaults.

**1.2 additions** (new names only; every 1.1 value is unchanged, so old outputs stay identical):
```python
P.C["spruce_tip"]           # vegetation (A3): spruce_shadow spruce_lit spruce_tip pine_lit pine_tip pine_bark_grey
                            # pine_bark_orange birch_lit birch_tip juniper forest_dark
P.C["kerb_red"]             # road/ground (A2): asphalt_dust asphalt_new bitumen aggregate kerb_red kerb_white
                            # granite_pink lichen blueberry needles mud flower_white flower_yellow flower_purple water_sky
P.C["church_cream"]         # buildings/props (A4): church_cream sauna_brown plinth_grey granite_boulder lichen_pale
                            # galvanised straw pole_brown door_brown glass_top glass_low reflector roof_dark brass
                            # (A4's local "granite_pink"/"lichen" are granite_boulder/lichen_pale here: A2 had the names)
P.C["fi_evening_fog"]       # A5's sky: fi_evening_zenith _top _horizon _fog _skyfill ("fog"/"horizon"/"zenith" unchanged)
P.LIGHT_SETS["fi_evening"]["fogEnd"]   # the whole fi_evening.txt light set as a dict (the txt stays authoritative)
P.UI["accent"]              # HUD/frontend colours (A6 gen_ui UI): ink panel panel_hi edge accent red lake logo_* ...
```

## 7. mesh

```python
from assetkit import mesh as M
```
Constants (mirroring the engine headers): `M.RB_OPAQUE RB_ALPHATEST RB_BLEND RB_ADD RB_SHADOW`, `M.RS1_NONE
RS1_DETAIL RS1_ENVADD`, `M.MM_TINT MM_ENV MM_WHEEL MM_GLASS MM_LIGHT`, `M.RFMT_V1 RFMT_V2`, `M.SURF_NONE
SURF_ASPHALT SURF_PAINT SURF_KERB SURF_GRAVEL SURF_GRASS SURF_DIRT SURF_WATER`, `M.MSH_NO_LIMIT`.
Vector helpers on tuples: `M.vadd vsub vscale vdot vcross vlen vnorm vlerp`, `M.face_normal(a, b, c)`.

### MeshBuilder (one .msh)
```python
mb = M.MeshBuilder("barn_red")
wall = mb.material("wall", tex="wall_red_board")                       # RB_OPAQUE, culled
leaf = mb.material("leaf", tex="tree_pine", blend=M.RB_ALPHATEST, cull_none=True)   # keyed foliage
paint = mb.material("paint", tex="car_0", stage1=M.RS1_ENVADD, tex1="env_sky", flags=M.MM_ENV)  # car paint
mb.lod(25.0)              # start LOD0: drawn while distance < 25 m (x preset lodScale); returns LOD index
mb.add(geo)               # add a Geo to the current LOD (or mb.add(geo, lod=1)); materials by object or name
mb.lod()                  # last LOD: no limit (written as MSH_NO_LIMIT = 100000)
mb.tag("wheel_fl", (-0.755, 0.326, 1.22), (0, 0, 1))   # name <= 15 chars, position, direction
mb.tri_counts()           # [702, 258, 92]
mb.geo(0)                 # the live Geo of LOD0
m = mb.save(path)         # write .msh, returns export.MshFile;  mb.to_msh() builds it without writing
```
Up to 4 LODs and 24 tags. On export each LOD's identical vertices are shared, triangles sorted by material into
parts, and each LOD may have at most 65535 vertices.

### Geo (triangles with attributes)
Per vertex `P` position, `N` normal, `UV`, `UV2`, `C` (r, g, b, a); per triangle `T` (i0, i1, i2) clockwise,
`TM` material (Material object or name), `TS` surface id. Methods that change geometry return the Geo, so chain.
```python
g = M.Geo(mat=wall)                     # empty; mat = default material for tri()
i = g.vert((x, y, z), n=(0, 1, 0), uv=(0, 0), c=(255, 255, 255, 255), uv2=(0, 0))   # -> index
g.tri(a, b, c, mat=None, surf=0)        # clockwise from the front
g.quad_idx(a, b, c, d)                  # two tris, a b c d clockwise
g.nverts; g.ntris; g.copy(); g.merge(other, ...); M.merge(g1, g2)      # counts, copy, append
g.translate(0, 1, 0); g.scale(2) ; g.scale(1, 2, 1)                     # negative scale keeps faces outward
g.rotate_y(30)                          # yaw: +Z toward +X (D3DXMatrixRotationY)
g.rotate_x(10)                          # +Y toward +Z: positive pitches +Z DOWN (D3DXMatrixRotationX)
g.rotate_z(10)                          # +X toward +Y (D3DXMatrixRotationZ)
g.matrix([[1,0,0],[0,1,0],[0,0,1],[tx,ty,tz]])   # row-vector 4x3: p' = p*M + row3; normals inverse-transposed
g.place(pos=(5, 0, 2), yaw=90, scale=1.2)        # scale, yaw, translate
g.mirror("x"); g2 = g.mirrored("x")    # mirror across x = 0, winding flipped (mirror the half car body)
g.flip()                                # reverse winding and normals
g.weld(eps=1e-5)                        # merge identical vertices (attrs=False: by position only); drops degenerate tris
g.remove_degenerate(1e-9); g.compact()  # drop zero-area tris; drop unused vertices
g.smooth(crease=50)                     # smoothing-group normals (use this): faces joined by edges <= crease share
                                        # one normal, transitive -> one vertex per position on smooth areas
g.smooth(55, soft=lambda t: g.TM[t] == "trim")   # soft(t) True: triangle t's edges never split (hidden parts)
M.smooth_normals(g, 50, soft=None)      # same, returns a NEW welded Geo (g unchanged)
g.normals(crease=50)                    # LEGACY per-corner smoothing (not transitive, over-splits vertices);
                                        # the primitives still use it so existing outputs stay identical
g.flat()                                # = normals(0)
g.set_normals(lambda p: (0, 1, 0))      # normals from a function of position (foliage)
g.uv_planar("-z", scale=0.5)            # planar seen from a face: '-z' front (u = x, v = -y), '+x' side, '+y' top...
g.uv_box(scale=0.25)                    # box projection per triangle, upright and unmirrored on every side
g.uv_cylinder("y", center=(0,0,0), u_repeat=2, v_scale=0.5)   # around an axis; seams split
g.uv_transform(su, sv, ou, ov); g.uv_fit((0, 0, 1, 1))        # scale/offset; stretch current bounds into a rect
g.uv_remap(atlas.uv("sign_moose"))      # map [0,1] UVs into an atlas rectangle
g.uv2_from_uv(8.0)                      # second UV set (detail texture, RVert2)
g.set_mat(roof); g.set_mat(roof, where="wall")   # all / only triangles with material "wall"
g.set_surface(M.SURF_GRAVEL); g.set_color(P.C["trim_white"]); g.tint((200, 200, 255))
g.bounds(); g.stats(); g.tri_area(t); g.tri_normal(t)
g.signed_volume()                       # > 0 for a closed outward surface
g.edge_report()                         # {'boundary', 'nonmanifold', 'flipped', 'closed'} (position-welded)
g.batches(M.RFMT_V1)                    # [(mat_key, engine vertex tuples, indices)] for export.PoolEncoder
```

### Primitives (all return a new Geo)
```python
M.box(sx, sy, sz, pos=(0,0,0), base=True, mat=wall, uv_scale=None, skip=("-y",))
    # base=True: pos = bottom centre; uv_scale None = each face 0..1, number = box projection (repeats per m)
M.quad(p0, p1, p2, p3, mat=m, uv=(u0, v0, u1, v1))       # bottom-left, top-left, top-right, bottom-right
M.vquad(w, h, pos=(0,0,0), yaw=0, mat=sign, uv=atlas.uv("sign"))   # vertical quad standing on pos, faces -Z
M.cylinder(r, h, seg=12, pos=(0,0,0), axis="y", caps=True, r_top=None)   # axis "x" for wheels (from pos toward +X)
M.lathe([(r0, y0), (r1, y1), ...], seg=16, caps=True, crease=60)   # revolve about +Y, profile bottom -> top
M.sphere(r, seg=12, rings=6, pos=(0,0,0))
M.loft(stations, closed=False, cap_start=False, cap_end=False, crease=50, u=None, v=None)
    # stations: list of point lists (same length). Car: 20 stations x 16 points per half, then mirror
M.extrude(shape2d, path3d, closed=True, caps=True, frames=None)   # tube/prism; frames="horizontal" keeps walls upright
M.strip_along(path, [(lat, h), ...], closed=False, frames=None, mats=None, surfs=None, v_scale=0.1)
    # roads, verges, walls, rails, fences along a polyline; mats/surfs per profile segment; v repeats per metre
M.strip_rows(rows, uv=None, mats=None, surfs=None, group=None, skip=None, color=WHITE, crease=40)
    # cross-section varying per row: rows[i] = points across the strip at station i (same count; repeat a point
    # to collapse a segment). mats/surfs: list per segment k or f(i, k); uv(i, j) (default u across 0..1, v per m);
    # group(i, k) -> key: quads share seam vertices only with equal keys (then uv(i, j, key), color(i, j, key));
    # skip(i, k) leaves quads out; crease=None keeps (0, 1, 0) normals. Front face rule = strip_along
M.strip_rows(rows, mats=lambda i, k: roles[i][k], uv=lambda i, j: (j / 3.0, i * 0.5))   # e.g. a widening road
M.frames_along(path, closed=False, mode="horizontal")   # per-point (right, up); mode "rmf" for 3D tubes
M.crossed_billboard(w, h, planes=2, mat=leaf, uv=atlas.uv("pine"), normals="sphere")   # 2 planes = 4 tris
M.cap(ring_points, target_dir)          # flat fan facing target_dir (used by loft/lathe)
M.cap(ring, (0, 0, 1), center="bbox", bulge=0.035)   # convex fan: centre pushed 3.5 cm along target_dir (car nose/
    # tail, domes; negative = dished); center = point | 'centroid' | 'bbox'; each tri oriented on its own
```

### Walls as cell grids (`facade`, from A4's buildings)
A planar wall is a grid of cells: plain tiling boards everywhere, and each window/door/sign = one atlas rectangle
mapped exactly onto its own cell (no decals, nothing coplanar, no T-junctions inside the wall). Gable triangles:
a convex outline (clipped cells), usually above a trim board that hides the seam.
```python
g = M.facade([(-4, 0), (-4, 3), (0, 5), (4, 3), (4, 0)],   # convex outline, clockwise in wall-local (x right, y up)
             (0, 0, -3), (1, 0, 0),                        # origin of local (0, 0); right = local +x -> faces -Z
             [(0, 0, 1.0, 2.1, "door_house"), (-2.5, 1.0, 1.2, 1.2, "win_house", glass)],  # (cx, y0, w, h, key[, mat])
             wall, win, atlas, tile=2.0)                   # plain mat, element mat, Atlas | dict | fn key -> uv rect
M.facade(outline, origin, right, mat=wall)                 # no elements: one fan, tiling UVs (u = x/tile + uoff, v = -y/tile)
M.clip_convex(poly, convex); M.poly_area2(poly)            # the 2D helpers it uses (clockwise, x right, y up)
```
Elements outside the outline raise ValueError; the result is welded (prelight it afterwards). An element key can
also be a `(u0, v0, u1, v1)` tuple.

### Lighting and LOD helpers
```python
M.prelight(g, mode="object", hemi=0.35, ao_height=1.2, ao_min=0.7)   # bake into vertex colours (multiplies C)
M.prelight(g, mode="world", sun_dir=P.SUN_DIR, shadow=0.5)           # trackc, in world space
M.prelight(g, mode="white")                                          # cars (CPU-lit at run time)
M.pick_even(seq, 8, keep=(3,))          # 8 items spread evenly, first/last/keep always kept
M.reduce_stations(stations, 10)         # fewer loft stations (LOD1 car)
M.reduce_points(stations, 8, keep=(4,)) # fewer points per station, same indices on every station
M.decimate(g, 260, crease=50)           # edge-collapse to ~260 tris; seams/boundaries locked; returns new Geo
M.prelight(g, mode="object", wrap=M.WRAP_SOFT)                      # 0.9: light walls keep their hue on the back
```
`prelight` light = `ambient * (1 - hemi + hemi*(n.y*0.5+0.5)) + sun * max(0, (n.L + wrap)/(1 + wrap)) * shadow`
(`wrap` `M.WRAP_OBJECT` = 0.5 for objects so their far side isn't flat ambient, 0 for world; `M.WRAP_SOFT` = 0.9 is
the named softer option A4/T23 use for buildings: not the default, because it would change every object mesh),
scaled so full sun = 1.0, times a ground AO ramp (`ao_min` at y = `ground_y` to 1 at `ao_height`), times the existing colour.

## 8. image

```python
from assetkit import image as I
img = I.new(128, 128, P.C["asphalt"]); rgba = I.new_rgba(128, 256)    # RGB / transparent RGBA (Pillow images)
art = I.supersample(96, 192, lambda d, im, s: d.polygon([(48*s, 0), (96*s, 190*s), (0, 190*s)], fill=(55,80,45,255)))
    # anti-aliased drawing at 4x on a transparent canvas, box-filtered down (edges clean for keying)
I.gradient(256, 64, top, bottom); I.multiply(img, (200, 180, 160))
I.apply_noise(img, rng.noise_grid(128, 128, 8, seed=3), amount=0.1)   # brightness modulation
I.noise_image(128, 128, lo, hi, cells=8, octaves=4, seed=3, tile=True, kind="fbm")
I.make_tileable(img, border=32); I.wrap_offset(img); I.seam_error(img)    # seamless tiling helpers
I.save_bmp("data/tex/x.bmp", img)                     # RGB: as is.  Raises on size-rule violations
I.save_bmp(path, rgba, keyed=True)                    # RGBA -> exact magenta key (bleed + threshold at alpha 128)
I.save_bmp(path, rgba, alpha=True)                    # x.bmp (colour bled under transparency) + x_a.bmp (8-bit grey)
I.save_bmp(path, glow, alpha=True, alpha_bleed=0)     # pair without the colour bleed: premultiplied/additive (glow)
I.save_bmp(path, rgba, bg=(128, 128, 128))            # opaque: RGBA flattened over bg, no key (opaque atlases)
I.save_bmp8("data/tex/x_a.bmp", grey)                 # 8-bit grey BMP ('L' as is; RGBA/LA -> its alpha band)
I.flatten(rgba, (128, 128, 128))                      # RGBA over an opaque colour -> RGB
I.save_bmp(path, big, check=False)                    # non-texture image (e.g. a 2048 px panorama source)
rgb, info = I.read_bmp(path)                          # own reader: 24-bit -> RGB, 8-bit grey -> 'L'; info dict
I.load_texture(path)                                  # RGBA as the engine sees it (key or _a.bmp -> alpha)
I.key_threshold(rgba); I.bleed(rgba, 4); I.keyed_to_rgba(rgb); I.alpha_path(path)
I.bmp_bytes(rgb); I.bmp8_bytes(grey); I.bmp_info(data)
I.is_pow2(128); I.size_problems(w, h)                 # texture rule checks
at = I.Atlas(256, 256, gutter=2); at.add("pine", img1); at.add("birch", img2)
sheet = at.image()                                    # packed RGBA, gutters filled with edge texels
opaque = at.image(bg=(128, 128, 128))                 # opaque atlas (RGB, no key): save with save_bmp as is
at.rect("pine") -> (x, y, w, h); at.uv("pine") -> (u0, v0, u1, v1) (half-texel inset) for Geo.uv_remap / quads
at.write_header("src/hud_rects.h", "HUD")             # C #defines (CRLF) for 2D sprites
I.pack_rects([(w, h), ...], 256, 256, gutter=2)       # positions only (shelf packer, deterministic)
I.mip_chain(img); I.mip_sheet(img)                    # engine-like key-safe mips; side-by-side preview
I.to16(img, "565", dither=True)                       # what 16-bit does to it ("555", "1555", "4444")
I.checker(w, h); I.over_checker(rgba); I.tex_kb(256, 256)   # backdrop; VRAM estimate (16-bit + mips) in KB
```

### Baking 3D features into an atlas (`bake`)
Rasterise a mesh in UV space and keep, per texel, the 3D position, normal and a region name; then paint
features by asking "where on the model is this texel?" (panel lines, lights, window frames, moss on north
faces, dirt near the ground). Coordinates are final-texture texels (x right, y down = UV * size); rects
`(x0, y0, w, h)`; buffers are flat lists of `W*H = (w*ss)*(h*ss)`, index `iy*W + ix`. From A1's car bake.
```python
from assetkit import bake as B
bk = B.Baker(256, 256, ss=2, overlap=0.02)         # bake at 2x; overlap closes hairline gaps between triangles
bk.add_geo(geo, key=lambda p, n: p[0], region="body", clip=(0, 0, 256, 128))   # texel = UV * (w, h)
bk.add_tri([(x, y)] * 3, [p0, p1, p2], [n0, n1, n2], keys=(k0, k1, k2), region="front", clip=rect)  # one triangle
    # overlaps (mirrored halves sharing texels, front + back projections): the larger key wins; keys None = first wins
bk.dilate((0, 0, 256, 128), steps=6, region="body")   # grow into empty texels (mip/filter safety)
bk.pos[i]; bk.nrm[i]; bk.reg[i]; bk.index(ix, iy); bk.texel(ix, iy); bk.covered(rect)   # None = empty texel
dist = bk.edge_distance([p is not None and is_glass(p) for p in bk.pos], cap=4, rects=[rect])   # trims/frames
img = bk.paint(lambda p, n, reg, ix, iy: (200, 40, 40) if p[1] > 0.8 else (90, 90, 90), bg=(0, 0, 0))
    # -> w x h RGB, box-filtered from the bake (downsample=False: W x H; empty=True: fn also gets empty texels)
```

## 9. export (binary formats)

The only writers/readers of engine formats; layouts are documented at the top of `export.py`.
```python
from assetkit import export as E
m = E.read_msh("data/meshes/v10gt.msh")        # MshFile: mats, parts, lods, tags, verts, normals, idx, box, radius
m.tri_counts(); m.lod_triangles(0)             # per-LOD counts; (mat, i0, i1, i2) absolute indices
E.write_msh(path, m); E.msh_bytes(m)           # round trip is byte-identical
E.trk_bytes([("INFO", b), ("TEXL", E.pack_names(["road_a"]))])   # .trk container: header + crc32 + padded chunks
E.write_trk(path, chunks); t = E.read_trk(path); t.get("SLCE"); t.problems
enc = E.PoolEncoder()                          # VPOL/BTCH/INDX shared encoding (trackc)
enc.add_geo(cell, lod, geo, {"road_a": 0}, E.RFMT_V1)   # one batch per material; mat key -> MATL index
enc.add_batch(cell, lod, mat, E.RFMT_V2, verts, idx)     # raw: verts (x, y, z, argb, u, v[, u2, v2]), idx into verts
vpol, btch, indx = enc.encode()                # chunk payloads; E.decode_pools(...) / E.encode_pools(pd) / E.check_pools(pd)
E.pack_name("koivujarvi", 32); E.unpack_name(b); E.latin1("Koivujärvi"); E.crc32(b); E.f32(x); E.pad4(b)
```
.msh (DESIGN 8.3): 60-byte header; `MeshMatFile` 68 B, `MeshPart` 12 B (firstIdx absolute into idx[]),
`MeshLod` 20 B (maxDist, firstPart, numParts, firstVert, numVerts), `MeshTag` 40 B, `RVert1` 24 B, normals,
u16 indices **relative to the LOD's firstVert**; parts per LOD sorted by material; `radius` = max distance of
any vertex from the mesh origin; last LOD maxDist 100000. Pools: one per (cell, format) holding both LODs'
batches of that cell, split at 65535 vertices; `TrkBatch` 24 B = s32 cell, lod, mat, pool; u32 firstIdx, numIdx.

## 10. validate

```python
from assetkit import validate as VA
probs = VA.check_file("data/meshes/barn_red.msh")          # [Problem(path, code, msg, level)]
probs = VA.check_paths(["data/"], classes={"meshes/v10gt.msh": "car"}, tex_dirs=["data/tex"])
VA.errors(probs); VA.class_for("tree_pine")                 # errors only; -> "tree"
VA.check_msh(path, cls="prop"); VA.check_bmp(path); VA.check_wav(path); VA.check_trk(path)
```
Checks. **.msh**: header/counts/file size, LOD and part ranges, indices < LOD vertex count, <= 65535 vertices
per LOD, LOD distances increasing, material names/enums, referenced textures exist (`<msh dir>/../tex` and
`tex_dirs`), unit normals, NaN/inf, box/radius cover the vertices, tag names (required tags per class),
**winding** (face normal vs vertex normals on culled materials, inconsistent shared edges, closed meshes not
inside out), degenerate tris (warning), budgets. **.bmp**: BITMAPINFOHEADER, BI_RGB, bottom-up, 24-bit (8-bit
grey for `_a`), power of two, <= 256, aspect, class sizes, **key purity** (near-magenta pixels that aren't the
exact key), **magenta fringes** (magenta-tinted pixels next to the key), `_a` pairing and size. **.wav**:
RIFF PCM mono 8/16-bit (rate != 22050 warns). **.trk**: tag, version, chunk count, crc32, chunk bounds and
zero padding, duplicates, INFO present, VPOL/BTCH/INDX consistency, and the trackc (T03) layouts: INFO 136 B,
SKY 312 B, TEXL/MSHL 32 B, MATL 20 B (texture indices < TEXL count), INST 40 B, **COLT CollTri 56 B**, **SLCE
TrkSlice 92 B**, GRID 16 B (8 slots, warning), MAPI 8 B (<= 256, warning), CELL 20 + 52 * numCells (cells inside
the grid, batch/instance ranges inside BTCH/INST, BTCH.cell < numCells), COLG = one GridBlock (exact size, refs <
COLT count), **WALL = s32 count + WallSeg 60 B * count** (+ optional GridBlock, exact; prev/next in range), BOXS
s32 count + CollBox 32 B, CHKP 12 + 4 * numCp (slice indices < SLCE count, last = finish). Sizes:
`VA.COLLTRI_SIZE WALLSEG_SIZE TRKSLICE_SIZE COLLBOX_SIZE TRK_FIXED TRK_RECORD`. Codes:
`format winding normals uv degenerate bounds tags material missing-tex budget lod npot size aspect key fringe
alpha wav trk pools`. Other extensions are ignored. CLI exit code = number of errors (max 255).
`VA.TEX_RULES` = `(prefix, exact size or list of sizes or None, max side)`: name rules match the file stem (first
match wins); a prefix ending in `/` is a folder rule (`("sky/", [(256, 256), (256, 128)], 256)`: every texture in a
`sky/` folder).

## 11. preview

```python
from assetkit import preview as V
V.save(V.views(mb, 512, tex_dirs=kit.tex_dirs, title="barn_red"), kit.preview_path("barn_views.png"))
    # 3x2 sheet: ortho from +Z, -Z, +X, top; 3/4 perspective; 3/4 wireframe; tri counts. facing="+z" for cars
V.turntable(mb, 8, 256, elev=20, facing="-z")    # 8 views around (4 per row)
V.lods(mb, 320)                                  # every LOD side by side + wireframe row; labels "LOD0 264 tris" /
                                                 # "to 100 m" ("last (no limit)"), two lines so 160 px panels fit
V.scene([(road_geo, (0, 0, 0)), (mb, (12, 0, 30), -20), (tree, (8, 0, 40), 0, 1.2)],
        eye=(0, 1.2, -8), target=(0, 1, 20), size=(640, 480), fog=(60, 300), ground=(P.C["grass"], 300))
V.contact_sheet(["data/tex/a.bmp", ("custom", pil_img)], cols=4, cell=160, mips=True)
V.render([(obj, pos, yaw, scale)], V.Camera(eye, target, fov=45 | ortho=half_h), size=(512, 512),
         light=dir, fog=(start, end, colour), wire=False, q16=False, lighting="auto", mats=None)   # -> PIL Image
V.render([road_geo], cam, mats=setting.MATERIALS)  # Geo TM = material NAMES + a table (trackc): {name: Material |
    # record dict | (name, tex, blend, cull_none)} or a list of those; also views/turntable/scene(mats=...)
V.material_table(mats)                           # -> {name: record dict} (what mats= is normalised to)
V.render(items, cam, mips=True, mip_bias=-0.5)   # per-pixel mip selection (nearest level, point sampled, image.
    # mip_chain levels): distant textures/cut-outs as the engine minifies them; also V.scene(..., mips=True)
V.label(img, "chase camera", xy=(4, 3), color=(240, 240, 240), size=12)   # text on a sheet; returns img
V.sky_image(w, h); V.bounds_of(obj); V.clear_cache(); V.save(img, path)
```
`obj` = MeshBuilder, MshFile, Geo or a `.msh` path. `tex_dirs` default `data/tex`; pass `kit.tex_dirs` in
generators (this run's output first). Lighting `auto`: meshes whose vertex colours are all white (cars) get
lambert + ambient from the palette, pre-lit meshes show their baked colours. Alpha test on ALPHATEST/BLEND
materials (key / `_a` alpha < 128); missing textures show a magenta/grey checker. 512 px views take < 1 s.

## 12. build and Kit

`tools/build_assets.py` = `assetkit.build.main`. **Imports**: `--only` imports only the requested generators, and
an import error (syntax error, half-saved file) fails only that generator (`FAILED (import error)` + traceback; the
others still run, exit code 1); the same for trackc (`FAILED (trackc import error)` on the tracks).
`B.discover(root, only=None, load=True)` -> Gens (`g.module`, `g.load_error`); `load=False` imports nothing.
Discovery, in this order: `tools/art/gen_*.py`,
`tools/sfx/gen_*.py`, `engine/tools/assetkit/samples/gen_*.py` (each group by `(ORDER, name)`), then
`tracks/<id>/track.txt` through trackc. **trackc protocol** (T03): `tools/trackc/trackc.py` exposes
`build_track(track_txt_path, out_dir, kit)` that writes under `out_dir` (e.g. `tracks/<id>.trk`,
`tracks/<id>_preview.png`, `tracks/<id>_report.txt`) and returns those relative paths; until it exists tracks
show `skipped`. A full build (no `--only`, no `--check`) also validates all of `data/` at the end.

Summary table columns: asset, kind, files, tris (sum of LOD0 of its meshes), KB, budget (`ok` or error count),
result (`N written, M unchanged`, or `identical` / `DIFF` under `--check`).

`kit` (passed to `build`):
```python
kit.out("meshes/x.msh")        # absolute path in this run's out_dir (folders created)
kit.save_msh("meshes/x.msh", mb); kit.save_bmp("tex/x.bmp", img, keyed=True)   # write outputs
kit.save_text("sky/fi_evening.txt", txt)                 # text output, bytes = txt.encode("ascii"), no newline change
kit.save_text("tex/hud_font.txt", txt, encoding="latin-1", crlf=True)   # crlf: \n -> \r\n (Win98 tools)
kit.save_bmp8("tex/glow_a.bmp", glow)  # 8-bit grey (image.save_bmp8)
kit.save_preview("x_views.png", img)   # PNG into the preview dir (no-op when kit.previews is False)
kit.preview_path("x.png")      # path in tools/art/preview/ (temp dir under --check)
kit.find("tex/road_a.bmp")     # this run's output, else data/ (read other generators' results)
kit.rng(seed, "name", i)       # = rng.Rng(seed, "name", i)
kit.tex_dirs                   # [out_dir/tex, data/tex] for preview/validate
kit.validate_files(["meshes/x.msh"])   # validate own outputs early -> [Problem]
kit.log("note"); kit.check; kit.previews; kit.name; kit.root; kit.data_dir
kit.mesh, kit.image, kit.palette, kit.export, kit.validate, kit.preview, kit.rng_module   # the modules
```

## 13. Recipes

**Keyed sign on a post (prop).**
```python
at = I.Atlas(256, 256); at.add("moose", moose_rgba)                    # RGBA art from I.supersample
mb = M.MeshBuilder("sign_moose")
post = mb.material("post", tex="posts"); face = mb.material("face", tex="signs", blend=M.RB_ALPHATEST, cull_none=True)
g = M.box(0.08, 2.0, 0.08, mat=post, uv_scale=1.0, skip=("-y",))
g.merge(M.vquad(0.9, 0.8, (0, 1.6, -0.05), 0, face, at.uv("moose")))  # faces -Z, toward oncoming traffic
M.prelight(g); mb.lod(); mb.add(g)
kit.save_bmp("tex/signs.bmp", at.image(), keyed=True)
```
**Car body half by loft + mirror** (A1): build 18-24 stations front -> back, each 16 points from the bottom
centre line over the +X side to the roof centre line (x = 0 at both ends), `half = M.loft(st, cap_start=True,
cap_end=True)`, then `body = half.copy().merge(half.mirrored("x")).weld()`; LOD1 from `M.reduce_stations` +
`M.reduce_points` on the same stations; LOD2 by `M.decimate` or a box hull. Check `body.edge_report()["closed"]`.
**Road ribbon** (trackc): `M.strip_along(centre_pts, profile, frames=banked_frames, mats=[...], surfs=[...],
v_scale=0.1)`, then `M.prelight(g, mode="world")`, cut per cell, `PoolEncoder.add_geo(cell, lod, g, mat_index)`.
When the cross-section changes along the road (width, kerbs, verges), build the rows yourself and use
`M.strip_rows(rows, group=role_of_quad, mats=..., surfs=..., uv=..., crease=None)`.
**Painting a mesh's atlas from 3D** (car liveries, building facades): give the mesh UVs in atlas regions, `bake.Baker`
+ `add_geo`/`add_tri` per region with a depth key, `dilate` each region, then `paint(fn)` where fn looks at the
texel's position/normal; `edge_distance` for frames around windows and lights.
**Tileable ground texture**: `I.noise_image(..., tile=True)` or draw freely and `I.make_tileable`; check with
`I.seam_error` and a 2x2 tiled preview (`I.wrap_offset`).

## 14. Pitfalls

- For new meshes use `g.smooth(crease)` (smoothing groups), not `g.normals(crease)`: the legacy function's
  per-corner test isn't transitive and splits vertices on curved surfaces (A1's car LOD0: 831 vs 496 vertices).
  The primitives (`loft`, `lathe`, `strip_along`, ...) still call `normals()` so old outputs stay byte-identical;
  call `.smooth(crease)` on their result (after merging caps/parts) to get the cleaner normals.
- A crossed billboard or any double-sided quad needs a `cull_none=True` material, otherwise the validator
  reports winding errors (its 'sphere' normals don't match one side) and the engine culls the back.
- `rotate_x` positive pitches +Z **down** (D3D convention). Test with `views()`.
- Don't draw anti-aliased shapes on a magenta background; draw on transparent RGBA (`I.supersample`) and save
  with `keyed=True`. The validator catches fringes.
- uv_box/box UVs run past [0,1] (they repeat); for atlas entries use per-face UVs (`uv_scale=None`) + `uv_remap`.
- `MeshBuilder.add` copies the Geo: changing the Geo afterwards doesn't change the mesh.
- An empty LOD is legal but pointless; LOD maxDist must increase.
- Generators must not depend on files written by a later generator; use `ORDER` (lower first) and `kit.find`.
- Keep generators fast (< ~30 s); `kit.previews` is False under `--check`, so skip previews there.

## 15. Version history

- **1.2** (T02c, 2026-11-13): `--only` imports only the requested generators; import errors fail per generator
  (also trackc); `discover(root, only, load)`. Palette: the M2/M3 art tasks' colours as new names, `P.UI`,
  `P.LIGHT_SETS`. Validator sky rule (folder rules in `TEX_RULES`, size lists). `kit.save_text`, `kit.save_bmp8` /
  `I.save_bmp8`, `save_bmp(alpha_bleed=0)`, `save_bmp(bg=)`, `I.flatten`, `Atlas.image(bg=)`. `M.facade`,
  `M.clip_convex`, `M.poly_area2`, `M.WRAP_OBJECT` / `M.WRAP_SOFT`. Preview mipmaps (`mips=`, `mip_bias=`), two-line
  `V.lods` labels. No default changed: every generator and track builds md5-identical.
- **1.1** (T02b, 2026-11-12): `g.smooth()` / `M.smooth_normals`, `M.cap(center=, bulge=)`, `M.strip_rows`,
  `assetkit.bake`, `V.label`, `V.render(mats=)`, `validate.check_trk` record layouts.
- **1.0** (T02, 2026-11-12): first version.
