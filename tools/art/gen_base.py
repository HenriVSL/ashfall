"""gen_base.py - Jet airbase buildings (task A7): hangar, control tower, fuel tank, radar dome. Outputs: ASSETS.

    python3 tools/build_assets.py --only base [--check]       # or: python3 tools/art/gen_base.py [--check]

Simple shapes that read from the air ("a couple of boxes for hangars, no need to be too fancy this early"), modest
military palette: olive-grey corrugated hangars with grey arched roofs, concrete tower with a dark glass cab,
off-white fuel tanks, a white radome on a concrete block. Pattern: ../racer/tools/art/gen_buildings.py (read-only).

Every mesh (DESIGN 8.5, assetkit conventions): metres, origin = ground centre of the footprint (y = 0 = ground),
the FRONT (hangar door, tower door) faces -Z. Class bldg (12-300 tris per LOD), two LODs: LOD0 below LOD_DIST m
(x preset lodScale), LOD1 beyond. Pre-lit with prelight(mode="object") using A2's light colours
(data/sky/light.txt sun/ambient/hemi) and the toolkit key direction in mesh space; islandc/e_world multiply the
per-instance sun on top as the racer does. Footprints match the DESIGN 8.1 boxes, so islandc's BASE collision AABBs
(DESIGN 8.6) can use these sizes directly (yaw 0; swap x/z for yaw 90/270):

    mesh     footprint x * z   height  shape
    hangar   40 x 30           14.0    arched roof (spring 8 m, crown 14 m), 32 x 10 m sliding door at -Z
    tower    8 x 8 (5 x 5 shaft) 25.0  concrete shaft to 20 m, glass cab 21.2-24 m, roof slab, mast (tag beacon)
    tank     14 x 14 (r 7)     11.6    fuel tank, 10 m wall + cone roof, ladder on the -Z side
    radome   10 x 10           16.2    concrete block 5 m + collar + white dome r 6 (centre y 10.2)

Textures (24-bit, opaque, no key):
    base_metal   128x128 tiling corrugated sheet, 4 m per repeat (ribs along v), NEUTRAL grey (mean ~200):
                 the vertex colour gives the hue (hangar walls olive grey, roof grey)
    base_concrete 128x128 tiling concrete with 2 m formwork joints, 8 m per repeat, NEUTRAL grey (tinted)
    base_atlas   256x128 opaque atlas, cells carry their own colour: hangar door, personnel door, clerestory
                 window, tower cab glass, tower door + window, tank wall, tank roof, radome panels, plain white
"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "tools"))

from PIL import Image, ImageDraw  # noqa: E402

from assetkit import image as I  # noqa: E402
from assetkit import mesh as M  # noqa: E402
from assetkit import palette as P  # noqa: E402
from assetkit import preview as V  # noqa: E402
from assetkit.rng import Rng, fbm  # noqa: E402

SEED = 707
ORDER = 45

MESHES = ["hangar", "tower", "tank", "radome"]
TEXTURES = ["base_metal", "base_concrete", "base_atlas"]
ASSETS = (["tex/%s.bmp" % t for t in TEXTURES]
          + [{"file": "meshes/%s.msh" % n, "class": "bldg"} for n in MESHES])

LOD_DIST = 1200.0        # LOD0 below this distance (x lodScale); the base is drawn to 6 km (DESIGN 8.5)
METAL_TILE = 4.0         # metres per repeat of base_metal
CONC_TILE = 8.0          # metres per repeat of base_concrete
WRAP = M.WRAP_SOFT       # soft wrap: back walls keep their hue (racer A4 buildings)
WHITE = (255, 255, 255, 255)

# Target colours (what the surface should average to after the texture; light comes on top).
COL = {
    "hangar_wall": (126, 132, 112),     # olive grey corrugated walls
    "hangar_roof": (122, 126, 124),     # neutral mid grey roof (reads on green grass and on the light apron)
    "hangar_trim": (88, 94, 84),
    "tower_conc": (196, 192, 182),      # light concrete
    "tower_frame": (70, 74, 78),        # dark cab frame band
    "tower_roof": (150, 152, 150),
    "mast_red": (200, 40, 32),
    "mast_white": (235, 235, 230),
    "radome_base": (178, 176, 168),
}

# Fallback light colours (= data/sky/light.txt by A2 at the time of writing); the file wins when present.
LIGHT_DEFAULT = {"sun": (255, 244, 226), "ambient": (132, 150, 178), "hemi": 0.35, "fog": (196, 212, 228),
                 "zenith": (40, 86, 172), "top": (50, 98, 182), "horizon": (194, 211, 231),
                 "sunDir": (-0.586824, 0.642788, -0.492404), "fogStart": 3600.0, "fogEnd": 18000.0}


def read_light(kit):
    """Numeric keys of A2's light set (data/sky/light.txt); defaults for anything missing."""
    out = dict(LIGHT_DEFAULT)
    path = kit.find("sky/light.txt") if kit is not None else None
    if not path or not os.path.exists(path):
        return out
    with open(path, "r") as f:
        for line in f:
            parts = line.split("#", 1)[0].split()
            if len(parts) < 2 or parts[0] not in out:
                continue
            try:
                vals = [float(x) for x in parts[1:]]
            except ValueError:
                continue
            cur = out[parts[0]]
            if isinstance(cur, tuple):
                out[parts[0]] = tuple(int(v) if isinstance(cur[0], int) else v for v in vals[:len(cur)])
            else:
                out[parts[0]] = vals[0]
    return out


def clamp8(v):
    return max(0, min(255, int(round(v))))


def tint_of(target, mean):
    """Vertex colour that turns a texture of average `mean` into `target` (clamped to 255)."""
    return tuple(clamp8(255.0 * t / max(1.0, m)) for t, m in zip(target, mean)) + (255,)


def mean_colour(img):
    px = list(img.getdata())
    n = float(len(px))
    return tuple(sum(p[k] for p in px) / n for k in range(3))


# ==================================================================================================== textures

def _n(x, y, w, h, cx, cy, seed, octaves=3):
    """Tileable fBm in [0, 1] with cx x cy lattice cells over a w x h image."""
    return fbm(x * cx / float(w), y * cy / float(h), octaves=octaves, seed=seed, period=(cx, cy))


def tex_metal():
    """Corrugated sheet, 128 px = 4 m: 16 ribs of 0.25 m along v, sheet laps every 2 m, weathering streaks."""
    w = h = 128
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            rib = math.sin(2.0 * math.pi * (x + 0.5) / 8.0)          # lit from the left
            f = 1.0 + 0.085 * rib
            if y % 64 in (0, 1):
                f *= 0.86 if y % 64 == 0 else 1.05                    # sheet lap: shadow line + lit lip
            streak = _n(x, y, w, h, 16, 2, SEED + 1)                 # vertical runs
            blot = _n(x, y, w, h, 4, 4, SEED + 2)
            f *= 0.94 + 0.10 * streak
            f *= 0.96 + 0.08 * blot
            v = 200.0 * f
            px[x, y] = (clamp8(v * 0.99), clamp8(v), clamp8(v * 1.01))
    return img


def tex_concrete():
    """Concrete, 128 px = 8 m: formwork joints every 2 m (32 px) both ways, stains, faint rain streaks."""
    w = h = 128
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            f = 0.92 + 0.10 * _n(x, y, w, h, 8, 8, SEED + 3, 4)
            f *= 0.95 + 0.07 * _n(x, y, w, h, 32, 3, SEED + 4)
            if y % 32 == 31:
                f *= 0.84
            elif y % 32 == 0:
                f *= 1.03
            if x % 32 == 31:
                f *= 0.90
            v = 202.0 * f
            px[x, y] = (clamp8(v * 1.01), clamp8(v), clamp8(v * 0.97))
    return img


def noise_fill(img, amount, seed, cx=4, cy=4):
    px = img.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            k = 1.0 + amount * (_n(x, y, w, h, cx, cy, seed) - 0.5) * 2.0
            r, g, b = px[x, y][:3]
            px[x, y] = (clamp8(r * k), clamp8(g * k), clamp8(b * k), 255)
    return img


def art_door():
    """Hangar sliding door, 128 x 40 px = 32 x 10 m (4 px/m): 8 leaves of 4 m, panel ribs, top track beam,
    hazard stripes along the bottom, a pale band with dark stencil blocks at mid height."""
    w, h = 128, 40
    img = Image.new("RGBA", (w, h), (96, 104, 90, 255))
    d = ImageDraw.Draw(img)
    for k in range(8):
        x0 = k * 16
        shade = 1.0 + (0.04 if k % 2 else -0.03)
        d.rectangle([x0, 2, x0 + 15, h - 1], fill=tuple(clamp8(c * shade) for c in (96, 104, 90)) + (255,))
        for yy in (12, 24):                                            # horizontal stiffener ribs
            d.line([x0 + 1, yy, x0 + 14, yy], fill=(112, 120, 106, 255))
            d.line([x0 + 1, yy + 1, x0 + 14, yy + 1], fill=(78, 84, 74, 255))
        d.line([x0, 2, x0, h - 1], fill=(46, 50, 44, 255))             # leaf joint
        d.line([x0 + 15, 2, x0 + 15, h - 1], fill=(104, 112, 100, 255))
    d.rectangle([0, 0, w - 1, 1], fill=(60, 64, 60, 255))              # top track beam
    for x in range(w):                                                 # hazard stripes, 0.5 m
        for y in range(h - 2, h):
            on = ((x + (h - y)) // 3) % 2 == 0
            img.putpixel((x, y), (214, 176, 40, 255) if on else (36, 36, 34, 255))
    noise_fill(img, 0.05, SEED + 10, 16, 4)
    return img


def art_pdoor():
    """Personnel door 16 x 32 px (1.2 x 2.4 m incl. frame)."""
    img = Image.new("RGBA", (16, 32), (60, 64, 62, 255))
    d = ImageDraw.Draw(img)
    d.rectangle([2, 2, 13, 31], fill=(112, 118, 110, 255))
    d.rectangle([3, 4, 12, 12], fill=(98, 104, 96, 255))
    d.point([(11, 18), (11, 19)], fill=(200, 200, 190, 255))
    return img


def glass_px(t, lit=1.0):
    """Window glass colour at height fraction t (0 top .. 1 bottom): sky reflection on top, darker below."""
    top = (120, 150, 182)
    low = (34, 46, 60)
    return tuple(clamp8((top[k] + (low[k] - top[k]) * min(1.0, t * 1.3)) * lit) for k in range(3)) + (255,)


def art_hwin():
    """Clerestory window band piece 32 x 8 px (4 x 1 m): 4 panes."""
    w, h = 32, 8
    img = Image.new("RGBA", (w, h), (70, 74, 72, 255))
    for y in range(1, h - 1):
        for x in range(1, w - 1):
            if x % 8 == 0:
                continue
            img.putpixel((x, y), glass_px((y - 1) / float(h - 3), 0.95))
    return img


def art_cab():
    """Tower cab glass, one side, 64 x 24 px (~7.8 x 2.8 m): 5 panes, dark mullions, sky reflection."""
    w, h = 64, 24
    img = Image.new("RGBA", (w, h), (50, 54, 58, 255))
    for y in range(2, h - 2):
        for x in range(w):
            if x % 13 in (0, 1) or x >= w - 1:
                continue
            k = 1.0 + 0.10 * math.sin(x * 0.21 + y * 0.35)             # faint diagonal sheen
            img.putpixel((x, y), glass_px((y - 2) / float(h - 5), k))
    return img


def art_tdoor():
    """Tower door 16 x 24 px (1.4 x 2.2 m): steel blue-grey door with a glazed top."""
    img = Image.new("RGBA", (16, 24), (64, 66, 66, 255))
    d = ImageDraw.Draw(img)
    d.rectangle([2, 2, 13, 23], fill=(84, 100, 118, 255))
    for y in range(4, 10):
        for x in range(4, 12):
            img.putpixel((x, y), glass_px((y - 4) / 6.0))
    return img


def art_twin():
    """Tower slit window 8 x 12 px (0.8 x 1.2 m)."""
    img = Image.new("RGBA", (8, 12), (88, 88, 86, 255))
    for y in range(1, 11):
        for x in range(1, 7):
            img.putpixel((x, y), glass_px((y - 1) / 10.0))
    return img


def art_tank():
    """Fuel tank wall, 128 x 32 px wrapped once round the tank (44 m x 10 m). Off-white plate courses
    (every 2 m), staggered vertical plate joints, ladder + cage at u = 0.5 (the -Z side), a thin grey stripe near
    the top, dirt at the foot, faint rust runs."""
    w, h = 128, 32
    img = Image.new("RGBA", (w, h))
    px = img.load()
    base = (224, 224, 218)
    for y in range(h):
        course = int(y / 6.4)
        for x in range(w):
            f = 0.97 + 0.05 * _n(x, y, w, h, 8, 2, SEED + 20)
            f *= 0.97 + 0.05 * _n(x, y, w, h, 32, 2, SEED + 21)          # vertical runs
            yy = y - course * 6.4
            if 0.0 <= yy < 1.0 and y > 0:
                f *= 0.94                                                # plate course seam
            if (x + course * 9) % 32 == 0:
                f *= 0.975                                               # vertical joint (staggered, faint)
            if y >= h - 2:
                f *= 0.72 + 0.10 * (h - 1 - y)                            # dirt at the foot
            c = [base[k] * f for k in range(3)]
            if 3 <= y <= 4:
                c = [126.0 * f, 130.0 * f, 128.0 * f]                     # grey band
            px[x, y] = (clamp8(c[0]), clamp8(c[1]), clamp8(c[2]), 255)
    cx = w // 2
    for y in range(h):                                                   # ladder (rails + rungs) + cage
        for x in (cx - 2, cx + 1):
            px[x, y] = (110, 112, 110, 255)
        if y % 2 == 0:
            for x in range(cx - 1, cx + 1):
                px[x, y] = (140, 142, 140, 255)
        if y < h - 8:
            for x in (cx - 3, cx + 2):
                px[x, y] = (170, 172, 168, 255)
    return img


def art_troof():
    """Tank cone roof 32 x 32 px (u around, v from apex (top) to eave (bottom)): light grey, radial seams."""
    w, h = 32, 32
    img = Image.new("RGBA", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            f = 0.95 + 0.08 * _n(x, y, w, h, 4, 4, SEED + 30)
            if x % 8 == 0:
                f *= 0.92
            if y >= h - 2:
                f *= 0.85                                                # eave angle
            px[x, y] = (clamp8(196 * f), clamp8(198 * f), clamp8(196 * f), 255)
    return img


def art_radome():
    """Radome panels 64 x 32 px (u around, v top -> bottom): off-white, very faint diagonal panel lattice (a
    geodesic hint that disappears at range; strong seams made it read as an igloo)."""
    w, h = 64, 32
    img = Image.new("RGBA", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            f = 0.985 + 0.015 * _n(x, y, w, h, 4, 4, SEED + 40)
            if (x + 2 * y) % 16 == 0 or (x - 2 * y) % 16 == 0:
                f *= 0.965
            if y % 8 == 4:
                f *= 0.975
            px[x, y] = (clamp8(238 * f), clamp8(240 * f), clamp8(238 * f), 255)
    return img


def art_plain():
    return Image.new("RGBA", (8, 8), (255, 255, 255, 255))


CELLS = [("door", art_door), ("tank", art_tank), ("cab", art_cab), ("radome", art_radome),
         ("troof", art_troof), ("hwin", art_hwin), ("pdoor", art_pdoor), ("tdoor", art_tdoor),
         ("twin", art_twin), ("plain", art_plain)]


def make_atlas():
    at = I.Atlas(256, 128, gutter=2)
    for name, fn in CELLS:
        at.add(name, fn())
    return at


# ==================================================================================================== geometry

def oquad(p, uv, outward, mat, color=WHITE, n=None):
    """Quad from 4 corners in ring order with uv per corner; flipped if needed so it faces `outward`."""
    a, b, c = p[0], p[1], p[2]
    fn = M.face_normal(a, b, c)
    if M.vdot(fn, outward) < 0.0:
        p = [p[0], p[3], p[2], p[1]]
        uv = [uv[0], uv[3], uv[2], uv[1]]
        fn = M.face_normal(p[0], p[1], p[2])
    g = M.Geo(mat)
    ns = n if n is not None else [fn] * 4
    if n is not None and M.vdot(M.face_normal(a, b, c), outward) < 0.0:
        ns = [n[0], n[3], n[2], n[1]]
    ids = [g.vert(p[k], ns[k], uv[k], color) for k in range(4)]
    g.quad_idx(ids[0], ids[1], ids[2], ids[3], mat)
    return g


def box_uv(sx, sy, sz, pos, mat, tile, skip=(), color=WHITE):
    """Box with world-scaled tiling UVs (box projection, `tile` metres per repeat)."""
    g = M.box(sx, sy, sz, pos=pos, mat=mat, uv_scale=1.0 / tile, skip=skip, color=color)
    return g


def finish(g, light, ao_height=2.0, ao_min=0.72):
    g.weld()
    M.prelight(g, mode="object", sun=light["sun"], ambient=light["ambient"], hemi=light["hemi"],
               ao_height=ao_height, ao_min=ao_min, wrap=WRAP)
    return g


class Ctx(object):
    """Shared state: atlas, texture means (for tints), light."""

    def __init__(self, kit):
        self.metal = tex_metal()
        self.concrete = tex_concrete()
        self.at = make_atlas()
        self.atlas_img = self.at.image(bg=(128, 128, 128))
        self.metal_mean = mean_colour(self.metal)
        self.conc_mean = mean_colour(self.concrete)
        self.light = read_light(kit)

    def uv(self, key):
        return self.at.uv(key)

    def mats(self, mb):
        return {"metal": mb.material("metal", tex="base_metal"),
                "conc": mb.material("conc", tex="base_concrete"),
                "atlas": mb.material("atlas", tex="base_atlas")}

    def mtint(self, name):
        return tint_of(COL[name], self.metal_mean)

    def ctint(self, name):
        return tint_of(COL[name], self.conc_mean)


# ---------------------------------------------------------------------------------------------------- hangar

H_W, H_D, H_SPRING, H_CROWN = 40.0, 30.0, 8.0, 14.0
DOOR_W, DOOR_H = 32.0, 10.0


def arch_points(nseg):
    """Arch over x = -20..20 from the spring line (y 8) to the crown (y 14): circular segment -> [(x, y, nx, ny)]."""
    c, s = H_W * 0.5, H_CROWN - H_SPRING
    R = (c * c + s * s) / (2.0 * s)
    cy = H_CROWN - R
    a0 = math.asin(c / R)
    out = []
    for i in range(nseg + 1):
        a = -a0 + 2.0 * a0 * i / nseg
        x, y = R * math.sin(a), cy + R * math.cos(a)
        if i == 0:
            x, y = -c, H_SPRING
        if i == nseg:
            x, y = c, H_SPRING
        out.append((x, y, math.sin(a), math.cos(a)))
    return out


def hangar_geo(ctx, mats, lod):
    nseg = 10 if lod == 0 else 5
    arch = arch_points(nseg)
    hz = H_D * 0.5
    wall_c = ctx.mtint("hangar_wall")
    roof_c = ctx.mtint("hangar_roof")
    outline = [(-H_W * 0.5, 0.0)] + [(x, y) for x, y, _, _ in arch] + [(H_W * 0.5, 0.0)]
    g = M.Geo()
    # front gable (-Z) with the big door; back gable (+Z) with a personnel door on LOD0
    front_el = [(0.0, 0.0, DOOR_W, DOOR_H, "door")]
    back_el = []
    side_el = []
    if lod == 0:
        front_el.append((0.0, 11.0, 8.0, 1.0, "hwin"))
        back_el = [(-6.0, 0.0, 1.2, 2.4, "pdoor"), (6.0, 0.0, 1.2, 2.4, "pdoor")]
        side_el = [(x, 5.4, 4.0, 1.0, "hwin") for x in (-10.0, -4.5, 1.0, 6.5, 12.0)]
    g.merge(M.facade(outline, (0.0, 0.0, -hz), (1.0, 0.0, 0.0), front_el, mats["metal"], mats["atlas"], ctx.at,
                     tile=METAL_TILE, color=wall_c))
    g.merge(M.facade(outline, (0.0, 0.0, hz), (-1.0, 0.0, 0.0), back_el, mats["metal"], mats["atlas"], ctx.at,
                     tile=METAL_TILE, color=wall_c))
    rect = [(-hz, 0.0), (-hz, H_SPRING), (hz, H_SPRING), (hz, 0.0)]
    g.merge(M.facade(rect, (-H_W * 0.5, 0.0, 0.0), (0.0, 0.0, -1.0), side_el, mats["metal"], mats["atlas"], ctx.at,
                     tile=METAL_TILE, color=wall_c))
    g.merge(M.facade(rect, (H_W * 0.5, 0.0, 0.0), (0.0, 0.0, 1.0), side_el, mats["metal"], mats["atlas"], ctx.at,
                     tile=METAL_TILE, color=wall_c))
    # arched roof: one strip per segment, corrugations running over the arch (texture u along Z, v along the arc)
    arc = [0.0]
    for i in range(1, len(arch)):
        arc.append(arc[-1] + math.hypot(arch[i][0] - arch[i - 1][0], arch[i][1] - arch[i - 1][1]))
    for i in range(nseg):
        x0, y0, nx0, ny0 = arch[i]
        x1, y1, nx1, ny1 = arch[i + 1]
        p = [(x0, y0, -hz), (x0, y0, hz), (x1, y1, hz), (x1, y1, -hz)]
        uv = [(-hz / METAL_TILE, arc[i] / METAL_TILE), (hz / METAL_TILE, arc[i] / METAL_TILE),
              (hz / METAL_TILE, arc[i + 1] / METAL_TILE), (-hz / METAL_TILE, arc[i + 1] / METAL_TILE)]
        n0, n1 = (nx0, ny0, 0.0), (nx1, ny1, 0.0)
        mid = ((nx0 + nx1) * 0.5, (ny0 + ny1) * 0.5, 0.0)
        g.merge(oquad(p, uv, mid, mats["metal"], roof_c, n=[n0, n0, n1, n1]))
    return g


def hangar(ctx):
    mb = M.MeshBuilder("hangar")
    mats = ctx.mats(mb)
    for lod in (0, 1):
        g = finish(hangar_geo(ctx, mats, lod), ctx.light, ao_height=2.5)
        if lod == 0:
            mb.lod(LOD_DIST)
        else:
            mb.lod()
        mb.add(g)
    mb.tag("door", (0.0, 0.0, -H_D * 0.5), (0.0, 0.0, -1.0))
    return mb


# ---------------------------------------------------------------------------------------------------- tower

T_SHAFT, T_SHAFT_H = 5.0, 20.0
T_CAB, T_FRAME_TOP, T_GLASS_TOP, T_ROOF_TOP, T_MAST_TOP = 8.0, 21.2, 24.0, 24.5, 25.0
T_GLASS_B, T_GLASS_T = 3.7, 3.9      # half widths of the outward-leaning glass band, bottom / top


def rot_y(p, deg):
    a = math.radians(deg)
    s, c = math.sin(a), math.cos(a)
    return (p[0] * c + p[2] * s, p[1], -p[0] * s + p[2] * c)


def tower_geo(ctx, mats, lod):
    g = M.Geo()
    conc = ctx.ctint("tower_conc")
    h = T_SHAFT * 0.5
    rect = [(-h, 0.0), (-h, T_SHAFT_H), (h, T_SHAFT_H), (h, 0.0)]
    sides = [((0.0, 0.0, -h), (1.0, 0.0, 0.0)), ((h, 0.0, 0.0), (0.0, 0.0, 1.0)),
             ((0.0, 0.0, h), (-1.0, 0.0, 0.0)), ((-h, 0.0, 0.0), (0.0, 0.0, -1.0))]
    for k, (org, right) in enumerate(sides):
        el = []
        if k == 0:
            el.append((0.0, 0.0, 1.4, 2.2, "tdoor"))
        if lod == 0:
            ys = (7.0, 13.0) if k == 0 else (4.0, 10.0, 16.0)
            el += [(0.0 if k else 0.0, y, 0.8, 1.2, "twin") for y in ys]
        g.merge(M.facade(rect, org, right, el, mats["conc"], mats["atlas"], ctx.at, tile=CONC_TILE, color=conc,
                         uoff=0.25 * k))
    # cab frame (a dark band under the glass) and roof slab
    frame_c = tint_of(COL["tower_frame"], ctx.conc_mean)
    g.merge(box_uv(T_CAB, T_FRAME_TOP - T_SHAFT_H, T_CAB, (0.0, T_SHAFT_H, 0.0), mats["conc"], CONC_TILE,
                   color=frame_c))
    roof_c = tint_of(COL["tower_roof"], ctx.conc_mean)
    g.merge(box_uv(T_CAB, T_ROOF_TOP - T_GLASS_TOP, T_CAB, (0.0, T_GLASS_TOP, 0.0), mats["conc"], CONC_TILE,
                   color=roof_c))
    # glass band: four outward-leaning trapezoids (closed ring, no corner posts needed)
    u0, v0, u1, v1 = ctx.uv("cab")
    for k in range(4):
        p = [(-T_GLASS_B, T_FRAME_TOP, -T_GLASS_B), (-T_GLASS_T, T_GLASS_TOP, -T_GLASS_T),
             (T_GLASS_T, T_GLASS_TOP, -T_GLASS_T), (T_GLASS_B, T_FRAME_TOP, -T_GLASS_B)]
        p = [rot_y(q, 90.0 * k) for q in p]
        out = rot_y((0.0, -0.1, -1.0), 90.0 * k)
        g.merge(oquad(p, [(u0, v1), (u0, v0), (u1, v0), (u1, v1)], out, mats["atlas"]))
    if lod == 0:
        # antenna mast with a red top on the roof (the beacon tag sits on top of it)
        pu = ctx.uv("plain")
        for y0, y1, col in ((T_ROOF_TOP, T_MAST_TOP - 0.15, COL["mast_white"]), (T_MAST_TOP - 0.15, T_MAST_TOP,
                                                                                 COL["mast_red"])):
            b = M.box(0.2, y1 - y0, 0.2, pos=(2.6, y0, 2.6), mat=mats["atlas"], skip=("-y",),
                      color=col + (255,))
            b.uv_remap(pu)
            g.merge(b)
    return g


def tower(ctx):
    mb = M.MeshBuilder("tower")
    mats = ctx.mats(mb)
    mb.lod(LOD_DIST)
    mb.add(finish(tower_geo(ctx, mats, 0), ctx.light, ao_height=2.0))
    mb.lod()
    mb.add(finish(tower_geo(ctx, mats, 1), ctx.light, ao_height=2.0))
    mb.tag("beacon", (2.6, T_MAST_TOP, 2.6), (0.0, 1.0, 0.0))
    mb.tag("door", (0.0, 0.0, -T_SHAFT * 0.5), (0.0, 0.0, -1.0))
    return mb


# ---------------------------------------------------------------------------------------------------- tank

TK_R, TK_H, TK_ROOF = 7.0, 10.0, 11.6


def tank_geo(ctx, mats, lod):
    seg = 16 if lod == 0 else 8
    # u = 0.5 (the ladder in the texture) is at angle 180 deg = the -Z side
    wall = M.lathe([(TK_R, 0.0), (TK_R, TK_H)], seg=seg, mat=mats["atlas"], caps=False, v=[1.0, 0.0])
    wall.uv_remap(ctx.uv("tank"))
    roof = M.lathe([(TK_R, TK_H), (0.9, TK_ROOF - 0.08), (0.6, TK_ROOF)], seg=seg, mat=mats["atlas"],
                   caps="top", v=[1.0, 0.05, 0.0])
    roof.uv_remap(ctx.uv("troof"))
    g = wall.merge(roof)
    g.smooth(50)
    return g


def tank(ctx):
    mb = M.MeshBuilder("tank")
    mats = ctx.mats(mb)
    mb.lod(LOD_DIST)
    mb.add(finish(tank_geo(ctx, mats, 0), ctx.light, ao_height=1.5))
    mb.lod()
    mb.add(finish(tank_geo(ctx, mats, 1), ctx.light, ao_height=1.5))
    return mb


# ---------------------------------------------------------------------------------------------------- radome

RD_BASE, RD_BASE_H, RD_R, RD_CY = 10.0, 5.0, 6.0, 10.2


def radome_geo(ctx, mats, lod):
    g = M.Geo()
    seg, rings = (12, 6) if lod == 0 else (8, 4)
    base_c = ctx.ctint("radome_base")
    hb = RD_BASE * 0.5
    rect = [(-hb, 0.0), (-hb, RD_BASE_H), (hb, RD_BASE_H), (hb, 0.0)]
    sides = [((0.0, 0.0, -hb), (1.0, 0.0, 0.0)), ((hb, 0.0, 0.0), (0.0, 0.0, 1.0)),
             ((0.0, 0.0, hb), (-1.0, 0.0, 0.0)), ((-hb, 0.0, 0.0), (0.0, 0.0, -1.0))]
    for k, (org, right) in enumerate(sides):
        el = [(0.0, 0.0, 1.4, 2.2, "tdoor")] if k == 0 else []
        g.merge(M.facade(rect, org, right, el, mats["conc"], mats["atlas"], ctx.at, tile=CONC_TILE,
                         color=base_c, uoff=0.3 * k))
    top = M.quad((-hb, RD_BASE_H, -hb), (-hb, RD_BASE_H, hb), (hb, RD_BASE_H, hb), (hb, RD_BASE_H, -hb),
                 mat=mats["conc"], uv=(0.0, 0.0, RD_BASE / CONC_TILE, RD_BASE / CONC_TILE),
                 color=tint_of(COL["tower_roof"], ctx.conc_mean))
    g.merge(top)
    # collar + dome (truncated sphere, 140 deg of arc from the collar to the top)
    e0 = math.radians(-50.0)
    rc = RD_R * math.cos(e0)
    prof = [(rc, RD_BASE_H), (rc, RD_CY + RD_R * math.sin(e0))]
    for i in range(1, rings + 1):
        e = e0 + (math.radians(84.0) - e0) * i / rings
        prof.append((RD_R * math.cos(e), RD_CY + RD_R * math.sin(e)))
    vs = [1.0 - k / float(len(prof) - 1) for k in range(len(prof))]
    dome = M.lathe(prof, seg=seg, mat=mats["atlas"], caps="top", v=vs)
    dome.uv_remap(ctx.uv("radome"))
    dome.smooth(70)
    g.merge(dome)
    return g


def radome(ctx):
    mb = M.MeshBuilder("radome")
    mats = ctx.mats(mb)
    mb.lod(LOD_DIST)
    mb.add(finish(radome_geo(ctx, mats, 0), ctx.light, ao_height=1.5))
    mb.lod()
    mb.add(finish(radome_geo(ctx, mats, 1), ctx.light, ao_height=1.5))
    return mb


# ==================================================================================================== build

def all_meshes(ctx):
    return [("hangar", hangar(ctx)), ("tower", tower(ctx)), ("tank", tank(ctx)), ("radome", radome(ctx))]


def build(out_dir, kit):
    ctx = Ctx(kit)
    kit.save_bmp("tex/base_metal.bmp", ctx.metal)
    kit.save_bmp("tex/base_concrete.bmp", ctx.concrete)
    kit.save_bmp("tex/base_atlas.bmp", ctx.atlas_img)
    meshes = all_meshes(ctx)
    for name, mb in meshes:
        kit.save_msh("meshes/%s.msh" % name, mb)
        lo, hi = V.bounds_of(mb)
        kit.log("%-7s tris %s  box x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f" %
                (name, mb.tri_counts(), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
    if kit.previews:
        previews(out_dir, kit, ctx, meshes)


# ==================================================================================================== previews

GROUND_MATS = {"t_grass": ("t_grass", "t_grass"), "runway": ("runway", "runway"), "taxi": ("taxi", "taxi"),
               "apron": ("apron", "apron"), "rwy_thr": ("rwy_thr", "rwy_thr")}
GRASS_TINT = (150, 190, 110)     # preview only: A6/islandc own the real class tints


def _gquad(g, x0, z0, x1, z1, y, mat, uvf, col):
    pts = [(x0, y, z0), (x0, y, z1), (x1, y, z1), (x1, y, z0)]
    ids = [g.vert(p, (0.0, 1.0, 0.0), uvf(p[0], p[2]), col) for p in pts]
    g.quad_idx(ids[0], ids[1], ids[2], ids[3], mat)


def mock_ground():
    """Flat mock airbase (preview only): grass, runway along +Z (x = 0), parallel taxiway at x = 180, an apron
    150 x 300 m east of the taxiway (x 191.5..341.5, z 1100..1400). Same layout as A3's mock airfield."""
    g = M.Geo()
    k = 0.92
    gc = tuple(clamp8(c * k) for c in GRASS_TINT) + (255,)
    wc = (clamp8(255 * k),) * 3 + (255,)
    for xi in range(-8, 8):
        for zi in range(-2, 8):
            x0, z0 = xi * 500.0, zi * 500.0
            _gquad(g, x0, z0, x0 + 500.0, z0 + 500.0, 0.0, "t_grass", lambda x, z: (x / 64.0, -z / 64.0), gc)
    for kz in range(0, 56):
        z0 = kz * 50.0
        m = "rwy_thr" if kz == 0 else "runway"
        sv = 1.0 if kz == 0 else -1.0                      # rwy_thr: v = 0 at the runway end
        _gquad(g, -22.5, z0, 22.5, z0 + 50.0, 0.2, m,
               lambda x, z, z0=z0, sv=sv: ((x + 22.5) / 45.0, sv * (z - z0) / 50.0), wc)
    for kz in range(0, 112):
        z0 = kz * 25.0
        _gquad(g, 168.5, z0, 191.5, z0 + 25.0, 0.2, "taxi", lambda x, z, z0=z0: ((x - 168.5) / 23.0, -(z - z0) / 25.0),
               wc)
    for zz in (300.0, 1250.0, 2500.0):
        _gquad(g, 22.5, zz - 11.5, 168.5, zz + 11.5, 0.2, "apron", lambda x, z: (x / 32.0, -z / 32.0), wc)
    _gquad(g, 191.5, 1100.0, 341.5, 1400.0, 0.2, "apron", lambda x, z: (x / 32.0, -z / 32.0), wc)
    _gquad(g, 341.5, 1060.0, 420.0, 1440.0, 0.2, "apron", lambda x, z: (x / 32.0, -z / 32.0), wc)
    return g


def base_items(md, jet):
    """Mock placement (preview only; T03/islandc places the real instances): 4 hangars on the apron's east
    edge with their doors facing the apron (yaw 90 -> door faces -X), tower at the apron's south end, 3 tanks
    north-east, radome on the south-east corner."""
    it = []
    for z in (1175.0, 1225.0, 1275.0, 1325.0):
        it.append((md["hangar"], (362.0, 0.2, z), 90.0))
    it.append((md["tower"], (366.0, 0.2, 1080.0), 90.0))
    for x, z in ((420.0, 1480.0), (442.0, 1500.0), (464.0, 1480.0)):
        it.append((md["tank"], (x, 0.0, z), 0.0))
    it.append((md["radome"], (470.0, 0.0, 1020.0), 0.0))
    if jet:
        it.append((jet, (300.0, 2.25, 1250.0), -90.0))
        it.append((jet, (240.0, 2.25, 1180.0), 180.0))
    return it


def previews(out_dir, kit, ctx, meshes):
    td = list(kit.tex_dirs)
    md = dict(meshes)
    kit.save_preview("base_tex.png", V.contact_sheet([os.path.join(out_dir, "tex", n + ".bmp") for n in TEXTURES],
                                                     cols=3, cell=256, mips=True, title="A7 base textures"))
    for n in MESHES:
        kit.save_preview("base_%s_views.png" % n, V.views(md[n], 320, tex_dirs=td, title=n))
    sheet = Image.new("RGB", (4 * 2 * 200, 2 * 200), (90, 90, 96))
    for k, n in enumerate(MESHES):
        sheet.paste(V.lods(md[n], 200, tex_dirs=td, title=n), (k * 400, 0))
    kit.save_preview("base_lods.png", sheet)
    # base layout scene
    light = ctx.light
    jp = kit.find("meshes/jet.msh")
    jet = jp if jp and os.path.exists(jp) else None
    ground = mock_ground()
    items = [(ground, (0.0, 0.0, 0.0), 0.0)] + base_items(md, jet)
    vfov = math.degrees(2.0 * math.atan(math.tan(math.radians(35.0)) * 0.75))      # 70 deg horizontal, 4:3
    sky = I.gradient(640, 480, light["top"], light["fog"])
    fog = (light["fogStart"], light["fogEnd"], light["fog"])
    sun = tuple(light["sunDir"])
    views = [("base_scene_low.png", (150.0, 120.0, 700.0), (380.0, 0.0, 1250.0), 0, "low pass 120 m, 600 m out"),
             ("base_scene_apron.png", (230.0, 2.5, 1120.0), (380.0, 8.0, 1290.0), 0, "on the apron, eye 2.5 m"),
             ("base_scene_600m.png", (-700.0, 600.0, 300.0), (330.0, 0.0, 1250.0), 0, "from 600 m, 1.4 km out"),
             ("base_scene_top.png", (330.0, 900.0, 1240.0), (330.0, 0.0, 1241.0), 0, "top, 900 m up"),
             ("base_scene_far.png", (-2600.0, 900.0, -1200.0), (330.0, 0.0, 1250.0), 1, "4 km out, 900 m, LOD1")]
    for name, eye, tgt, lod, label in views:
        img = V.render(items, V.Camera(eye, tgt, fov=vfov, near=1.0), size=(640, 480), bg=sky, fog=fog,
                       tex_dirs=td, lighting="vertex", mats=GROUND_MATS, mips=True, q16=True, lod=lod, light=sun)
        V.label(img, "A7 mock base: %s (kit renderer, 565)" % label, (4, 3), (250, 250, 250))
        kit.save_preview(name, img)


if __name__ == "__main__":
    from assetkit import build as B
    sys.exit(B.main_for(__file__))
