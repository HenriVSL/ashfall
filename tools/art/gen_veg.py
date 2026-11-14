"""gen_veg.py - Jet island vegetation, rocks and village sprites for the scatter renderer (task A11).

    python3 tools/build_assets.py --only veg [--check]          # or: python3 tools/art/gen_veg.py [--check]

Outputs:
  tex/veg_atlas.bmp   256x256 keyed atlas (exact RGB(255,0,255) = transparent; RB_ALPHATEST, IMG_KEYED at load,
                      lodBias -2 like every alpha-tested material), sprites drawn at 4x and box-filtered
  tex/veg_atlas.txt   CRLF sprite table, read by islandc (tools/islandc/scatter.py) into data/island/veg.sct:
                      name x y w h world_w world_h sink kind   (pixels; metres; kind 0 = camera-facing, 1 = fixed
                      orientation along the instance yaw, double-sided: hedges, walls)

The sprites are what the 80 m terrain cells cannot show from 300 m: tree groves on the forest texture, dark
cryptomeria (cedar) plantations, palm groups along the coast, single field trees, hedgerows and black basalt stone
walls along the plot borders (the farmland patchwork of classes.py), shrubs on the heath, boulders on the ash and
rock, and white-walled village houses with terracotta roofs. Green Atlantic volcanic island look (STATUS: green
island; Azores / Madeira mood). Origin = bottom centre on the ground; the image bottom row sits SINK metres below
the ground so slopes never show a floating base. Lighting is painted for a camera-facing card (top-lit, light from
the upper left, the sun side of A2's 230/40 sun for a viewer facing north-east, the usual heading over the island);
the scatter renderer multiplies the baked terrain light of the spot (islandc) on top.

  sprite   px (content)  metres          habitat (islandc scatter.py)
  grove    124 x 60      26 x 12.6      forest quads (laurel / broadleaf groups)
  cedar     60 x 92      9.0 x 13.8     plantation / conifer forest quads
  palms     60 x 92      10 x 15.3      coast < 30 m (canary / coconut palm pairs)
  tree      60 x 60      9.0 x 9.0      single trees on pasture, plot corners, forest edges
  shrub     60 x 28      5.6 x 2.6      heath, scrub, upland pasture, under the treeline (hydrangea / heather)
  hedge    124 x 22      24 x 4.3       plot borders (fixed orientation)
  wall     124 x 12      24 x 1.25      plot borders, upland pastures (fixed orientation): black basalt stones
  rock      60 x 28      6.4 x 3.0      ash, rock, outcrops, cooled lava (scaled 0.7-2.6)
  houses   124 x 60      34 x 16.5      villages in the farmland near the coast
"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "tools"))

from PIL import Image, ImageDraw  # noqa: E402

from assetkit import image as I  # noqa: E402
from assetkit import mesh as M  # noqa: E402
from assetkit import preview as V  # noqa: E402
from assetkit.rng import Rng  # noqa: E402

SEED = 1111
ORDER = 28              # before the island (islandc reads tex/veg_atlas.txt)
SS = 4                  # supersampling
ATLAS = (256, 256)

ASSETS = ["tex/veg_atlas.bmp", "tex/veg_atlas.txt"]

# name: (content w, h px, world w, world h, sink m, kind)
SPRITES = [
    ("grove", 124, 60, 26.0, 12.6, 0.6, 0),
    ("houses", 124, 60, 34.0, 16.5, 0.8, 0),
    ("cedar", 60, 92, 9.0, 13.8, 0.5, 0),
    ("palms", 60, 92, 10.0, 15.3, 0.5, 0),
    ("tree", 60, 60, 9.0, 9.0, 0.4, 0),
    ("shrub", 60, 28, 5.6, 2.6, 0.3, 0),
    ("rock", 60, 28, 6.4, 3.0, 0.5, 0),
    ("hedge", 124, 22, 24.0, 4.3, 0.4, 1),
    ("wall", 124, 12, 24.0, 1.25, 0.25, 1),
]

# palettes (sRGB): shadow, mid, lit, tip
LAUREL = ((22, 38, 20), (40, 68, 30), (68, 102, 42), (104, 134, 60))
BEECH = ((28, 44, 22), (52, 80, 34), (88, 118, 50), (128, 152, 74))
CEDAR = ((16, 32, 26), (30, 56, 44), (50, 84, 62), (78, 112, 80))
PALM = ((40, 58, 26), (74, 102, 38), (116, 146, 58), (168, 186, 92))
SHRUB = ((30, 46, 24), (56, 84, 38), (92, 124, 54), (136, 160, 76))
HYDRANGEA = [(112, 132, 214), (150, 160, 230), (160, 150, 214), (226, 228, 240)]
TRUNK = (64, 52, 42)
TRUNK_LIT = (104, 88, 70)
PALM_TRUNK = (118, 100, 78)
BASALT = ((26, 26, 28), (52, 50, 52), (82, 80, 80), (112, 110, 106))
WALL_WHITE = (232, 228, 216)
ROOF = ((128, 52, 34), (170, 74, 46), (200, 98, 62))


def lerp(a, b, t):
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    return tuple(int(a[k] + (b[k] - a[k]) * t + 0.5) for k in range(3))


def ramp(r, l):
    stops = (0.0, 0.38, 0.72, 1.0)
    l = 0.0 if l < 0.0 else (1.0 if l > 1.0 else l)
    for i in range(3):
        if l <= stops[i + 1]:
            return lerp(r[i], r[i + 1], (l - stops[i]) / (stops[i + 1] - stops[i]))
    return r[3]


class Pt(object):
    """Painter on a SS x canvas in final-pixel coordinates (y down)."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.img)

    def ell(self, x, y, rx, ry, col):
        rx, ry = max(rx, 0.25), max(ry, 0.25)
        self.d.ellipse([(x - rx) * SS, (y - ry) * SS, (x + rx) * SS, (y + ry) * SS], fill=col + (255,))

    def poly(self, pts, col):
        self.d.polygon([(x * SS, y * SS) for x, y in pts], fill=col + (255,))

    def line(self, pts, width, col):
        self.d.line([(x * SS, y * SS) for x, y in pts], fill=col + (255,), width=max(1, int(round(width * SS))))

    def taper(self, pts, w0, w1, col):
        n = len(pts)
        left, right = [], []
        for i in range(n):
            a = pts[max(0, i - 1)]
            b = pts[min(n - 1, i + 1)]
            dx, dy = b[0] - a[0], b[1] - a[1]
            ln = math.hypot(dx, dy) or 1.0
            nx, ny = -dy / ln, dx / ln
            wv = (w0 + (w1 - w0) * i / float(max(1, n - 1))) * 0.5
            left.append((pts[i][0] + nx * wv, pts[i][1] + ny * wv))
            right.append((pts[i][0] - nx * wv, pts[i][1] - ny * wv))
        self.poly(left + right[::-1], col)

    def final(self):
        """Box-filtered to 1x with premultiplied colour (no dark fringes), alpha thresholded later by the kit."""
        small = self.img.convert("RGBa").resize((self.w, self.h), Image.BOX).convert("RGBA")
        return small


def foliage(pt, r, cx, cy, rx, ry, pal, tuft=(1.0, 2.2), density=1.0, core=0.78, top_light=0.55, side=0.25,
            flowers=None, clamp=None):
    """A foliage mass: dark core + many tufts back to front; light from the top-left; optional flower dots."""
    if core > 0:
        pt.ell(cx, cy + ry * 0.1, rx * core, ry * core, pal[0])
    area = math.pi * rx * ry
    ta = (tuft[0] + tuft[1]) * 0.5
    n = int(area / (ta * ta) * 1.4 * density) + 6
    tufts = []
    for _ in range(n):
        a = r.range(0.0, math.pi * 2.0)
        d = math.sqrt(r.random())
        x = cx + math.cos(a) * rx * d
        y = cy + math.sin(a) * ry * d
        if clamp:
            x = max(clamp[0], min(clamp[1], x))
            y = max(clamp[2], min(clamp[3], y))
        tufts.append((r.random(), x, y, r.range(tuft[0], tuft[1]), r.gauss(0.0, 0.10)))
    tufts.sort()
    for z, x, y, sz, nz in tufts:
        ny = (y - cy) / max(0.5, ry)
        nx = (x - cx) / max(0.5, rx)
        l = 0.45 - top_light * ny - side * nx + 0.40 * (z - 0.5) + nz
        pt.ell(x, y, sz * 1.2, sz, ramp(pal, l))
    if flowers:
        nf = int(n * flowers)
        for _ in range(nf):
            a = r.range(0.0, math.pi * 2.0)
            d = math.sqrt(r.random()) * 0.9
            x = cx + math.cos(a) * rx * d
            y = cy + math.sin(a) * ry * d * 0.9
            if y < cy - ry * 0.7:
                continue
            c = HYDRANGEA[int(r.random() * len(HYDRANGEA)) % len(HYDRANGEA)]
            l = 0.85 + 0.25 * (-(y - cy) / max(0.5, ry))
            pt.ell(x, y, tuft[0] * 0.9, tuft[0] * 0.75, lerp((0, 0, 0), c, l))


def broadleaf(pt, r, cx, yb, h, hw, pal, trunk=0.28):
    """One round broadleaf tree: short trunk, 2-4 overlapping crown lobes."""
    th = h * trunk
    pt.taper([(cx, yb + 2), (cx + r.range(-1, 1), yb - th)], max(1.4, hw * 0.16), max(0.9, hw * 0.10), TRUNK)
    pt.taper([(cx - hw * 0.03, yb), (cx - hw * 0.03, yb - th * 0.9)], max(0.6, hw * 0.05), 0.4, TRUNK_LIT)
    top = yb - h
    cy = top + (h - th) * 0.52
    lobes = 2 + int(r.random() * 3)
    for k in range(lobes):
        ox = r.range(-0.45, 0.45) * hw
        oy = r.range(-0.15, 0.25) * (h - th)
        foliage(pt, r, cx + ox, cy + oy, hw * r.range(0.55, 0.75), (h - th) * r.range(0.38, 0.5), pal,
                tuft=(max(0.8, hw * 0.07), max(1.6, hw * 0.16)), density=0.9)
    foliage(pt, r, cx, cy, hw, (h - th) * 0.5, pal, tuft=(max(0.8, hw * 0.08), max(1.8, hw * 0.18)), core=0.7)


def s_grove(r):
    w, h = 124, 60
    pt = Pt(w, h)
    yb = h - 3.0
    trees = [(18, 30, 15, BEECH), (44, 44, 19, LAUREL), (68, 38, 16, BEECH), (92, 47, 18, LAUREL), (110, 30, 12, BEECH),
             (32, 24, 10, LAUREL), (80, 22, 10, LAUREL)]
    order = sorted(range(len(trees)), key=lambda i: -trees[i][1])          # tall (back) first
    for i in order:
        x, hh, hw, pal = trees[i]
        broadleaf(pt, r, x + r.range(-2, 2), yb - (1.0 if hh > 35 else 0.0), hh + r.range(-2, 2), hw, pal)
    # undergrowth band along the ground
    for x in range(4, w - 4, 6):
        foliage(pt, r, x + r.range(-2, 2), yb - 3.0, r.range(4.0, 7.0), r.range(2.5, 4.0), SHRUB, tuft=(0.9, 1.7),
                core=0.6, density=0.8)
    return pt.final()


def s_cedar(r):
    w, h = 60, 92
    pt = Pt(w, h)
    yb = h - 3.0
    for cx, hh, hw in ((16, 70, 11), (44, 84, 13), (30, 60, 9)):
        top = yb - hh
        pt.taper([(cx, yb + 2), (cx, yb - hh * 0.2)], 2.4, 1.6, TRUNK)
        tiers = int(hh / 3.0)
        for i in range(tiers):
            t = (i + 0.5) / tiers
            y = top + hh * 0.04 + (hh * 0.86) * t
            ww = hw * (0.15 + 0.85 * t ** 0.9)
            foliage(pt, r, cx + r.range(-0.6, 0.6), y, ww, 3.2, CEDAR, tuft=(0.9, 1.8), core=0.7, density=0.8,
                    top_light=0.35, side=0.35)
        pt.taper([(cx, top + 6), (cx, top)], 1.6, 0.5, CEDAR[1])
    return pt.final()


def frond(pt, r, x0, y0, ang, ln, droop, pal, lit):
    """A palm frond: curved midrib with leaflets hanging off it."""
    pts = []
    n = 9
    for k in range(n + 1):
        u = k / float(n)
        a = ang + droop * u * u
        pts.append((x0 + math.cos(a) * ln * u, y0 + math.sin(a) * ln * u))
    for k in range(1, n + 1):
        u = k / float(n)
        px, py = pts[k]
        l = max(0.8, ln * 0.36 * (1.0 - 0.5 * u))
        for s in (-1, 1):
            a = ang + droop * u * u + s * 1.25 + 0.55
            ex, ey = px + math.cos(a) * l, py + math.sin(a) * l + l * 0.35
            pt.line([(px, py), (ex, ey)], 1.3, ramp(pal, lit * (0.9 - 0.3 * u) + r.range(-0.1, 0.1)))
    pt.line(pts, 1.4, ramp(pal, lit * 0.7))


def palm(pt, r, cx, yb, h, lean):
    top = (cx + lean, yb - h)
    pts = []
    for k in range(9):
        u = k / 8.0
        pts.append((cx + lean * (u ** 1.6), yb - h * u))
    pt.taper(pts, 3.8, 2.6, PALM_TRUNK)
    for k in range(1, 8):                                    # ring scars
        x, y = pts[k]
        pt.line([(x - 1.2, y), (x + 1.2, y)], 0.5, (90, 76, 60))
    pt.taper([(p[0] - 0.6, p[1]) for p in pts], 0.8, 0.5, (150, 132, 104))
    nf = 11
    for k in range(nf):
        a = -math.pi * 0.5 + (k - (nf - 1) * 0.5) / (nf - 1) * math.pi * 1.6 + r.range(-0.12, 0.12)
        ln = r.range(16.0, 20.0)
        droop = 0.9 if abs(math.cos(a)) > 0.3 else 0.3
        droop *= 1 if math.cos(a) > 0 else -1
        lit = 0.85 if math.sin(a) < 0 else 0.55
        frond(pt, r, top[0], top[1], a, ln, droop, PALM, lit)
    pt.ell(top[0], top[1] + 1.0, 2.2, 1.6, PALM[0])


def s_palms(r):
    w, h = 60, 92
    pt = Pt(w, h)
    yb = h - 3.0
    palm(pt, r, 22, yb, 60, 5)
    palm(pt, r, 38, yb, 70, -4)
    for x in (10, 30, 50):
        foliage(pt, r, x, yb - 2.0, 6.0, 3.0, SHRUB, tuft=(0.9, 1.6), core=0.6, density=0.8)
    return pt.final()


def s_tree(r):
    w, h = 60, 60
    pt = Pt(w, h)
    broadleaf(pt, r, 30, h - 3.0, 54, 25, BEECH, trunk=0.30)
    return pt.final()


def s_shrub(r):
    w, h = 60, 28
    pt = Pt(w, h)
    yb = h - 2.0
    for x, rx, ry, fl in ((12, 10, 9, 0.10), (30, 13, 11, 0.16), (48, 10, 8, 0.08)):
        foliage(pt, r, x, yb - ry * 0.9, rx, ry, SHRUB, tuft=(0.9, 1.8), core=0.75, flowers=fl,
                clamp=(1, w - 1, 1, h - 1))
    return pt.final()


def s_hedge(r):
    w, h = 124, 22
    pt = Pt(w, h)
    yb = h - 1.5
    pt.poly([(0, yb), (0, yb - 12), (w, yb - 12), (w, yb)], SHRUB[0])
    for x in range(0, w + 6, 5):
        foliage(pt, r, x + r.range(-1.5, 1.5), yb - 9.0 + r.range(-1.5, 1.5), r.range(4.5, 6.5), r.range(6.0, 8.5),
                SHRUB, tuft=(0.9, 1.6), core=0.7, density=0.9, flowers=0.12, clamp=(0, w, 0.5, h))
    return pt.final()


def s_wall(r):
    w, h = 124, 12
    pt = Pt(w, h)
    yb = h - 1.0
    # dry-stone wall of black basalt blocks, two courses + capping, grass at the foot
    x = 0.0
    while x < w:
        bw = r.range(4.0, 7.5)
        bh = r.range(4.0, 5.5)
        l = r.range(0.15, 0.6)
        pt.poly([(x, yb), (x + 0.4, yb - bh), (x + bw - 0.4, yb - bh - r.range(-0.4, 0.4)), (x + bw, yb)],
                ramp(BASALT, l))
        pt.line([(x + 0.8, yb - bh + 0.6), (x + bw - 0.8, yb - bh + 0.6)], 0.7, ramp(BASALT, l + 0.35))
        x += bw + r.range(-0.3, 0.4)
    x = 1.0
    while x < w - 1:
        bw = r.range(3.0, 6.0)
        y0 = yb - 4.2
        bh = r.range(3.0, 4.6)
        l = r.range(0.2, 0.7)
        pt.ell(x + bw * 0.5, y0 - bh * 0.5, bw * 0.55, bh * 0.55, ramp(BASALT, l))
        pt.ell(x + bw * 0.45, y0 - bh * 0.7, bw * 0.3, bh * 0.22, ramp(BASALT, l + 0.3))
        x += bw
    for x in range(0, w, 2):
        pt.line([(x, yb + 1), (x + r.range(-0.8, 0.8), yb - r.range(0.5, 2.0))], 0.6, ramp(SHRUB, r.range(0.4, 0.9)))
    return pt.final()


def s_rock(r):
    w, h = 60, 28
    pt = Pt(w, h)
    yb = h - 1.5
    for cx, rx, ry in ((14, 11, 8), (36, 15, 12), (50, 8, 6), (24, 7, 5)):
        pts = []
        n = 9
        for k in range(n):
            a = math.pi + k / float(n - 1) * math.pi
            rr = r.range(0.8, 1.1)
            pts.append((cx + math.cos(a) * rx * rr, yb + math.sin(a) * ry * rr * 1.6))
        pt.poly(pts + [(cx + rx, yb + 1), (cx - rx, yb + 1)], BASALT[1])
        # facets: lit top-left, shadow right/bottom
        pt.poly([(cx - rx * 0.7, yb - ry * 0.6), (cx - rx * 0.1, yb - ry * 1.45), (cx + rx * 0.2, yb - ry * 0.9),
                 (cx - rx * 0.3, yb - ry * 0.3)], BASALT[2])
        pt.poly([(cx - rx * 0.4, yb - ry * 1.0), (cx - rx * 0.05, yb - ry * 1.4), (cx + rx * 0.05, yb - ry * 1.15)],
                BASALT[3])
        pt.poly([(cx + rx * 0.3, yb - ry * 0.8), (cx + rx * 0.95, yb - ry * 0.2), (cx + rx, yb), (cx + rx * 0.4, yb)],
                BASALT[0])
        for _ in range(4):
            x = cx + r.range(-0.7, 0.7) * rx
            pt.line([(x, yb - ry * r.range(0.3, 1.0)), (x + r.range(-2, 2), yb - ry * r.range(0.0, 0.3))], 0.5,
                    BASALT[0])
    for x in range(1, w - 1, 3):                                   # lichen / grass tufts at the foot
        pt.ell(x + r.range(-1, 1), yb, r.range(0.8, 1.6), 0.8, ramp(SHRUB, r.range(0.3, 0.8)))
    return pt.final()


def house(pt, r, x0, yb, ww, wh, rh, gable_front, chimney=True):
    """White house: walls ww x wh px, terracotta roof rh px tall; gable_front = roof seen from the gable end."""
    x1 = x0 + ww
    pt.poly([(x0, yb), (x0, yb - wh), (x1, yb - wh), (x1, yb)], WALL_WHITE)
    pt.poly([(x0 + ww * 0.62, yb), (x0 + ww * 0.62, yb - wh), (x1, yb - wh), (x1, yb)], (196, 192, 182))   # shade side
    pt.line([(x0, yb - 0.6), (x1, yb - 0.6)], 1.2, (70, 70, 72))                                         # basalt plinth
    if gable_front:
        pt.poly([(x0 - 0.8, yb - wh), (x0 + ww * 0.5, yb - wh - rh), (x1 + 0.8, yb - wh)], ROOF[1])
        pt.poly([(x0 + ww * 0.5, yb - wh - rh), (x1 + 0.8, yb - wh), (x0 + ww * 0.62, yb - wh)], ROOF[0])
    else:
        pt.poly([(x0 - 1.0, yb - wh), (x0 + 2.0, yb - wh - rh), (x1 - 2.0, yb - wh - rh), (x1 + 1.0, yb - wh)], ROOF[1])
        pt.line([(x0 + 2.0, yb - wh - rh), (x1 - 2.0, yb - wh - rh)], 0.8, ROOF[2])
    # windows + door with dark basalt frames (Azorean style)
    nwin = max(1, int(ww / 7))
    for k in range(nwin):
        wx = x0 + (k + 0.5) * ww / nwin
        pt.poly([(wx - 1.2, yb - wh * 0.45), (wx - 1.2, yb - wh * 0.8), (wx + 1.2, yb - wh * 0.8),
                 (wx + 1.2, yb - wh * 0.45)], (52, 60, 72))
    if chimney:
        cx = x0 + ww * r.range(0.2, 0.75)
        pt.poly([(cx, yb - wh - rh * 0.5), (cx, yb - wh - rh - 2), (cx + 2.2, yb - wh - rh - 2),
                 (cx + 2.2, yb - wh - rh * 0.5)], WALL_WHITE)


def s_houses(r):
    w, h = 124, 60
    pt = Pt(w, h)
    yb = h - 3.0
    # back row
    house(pt, r, 6, yb - 6, 22, 11, 7, True)
    house(pt, r, 64, yb - 7, 26, 12, 6, False)
    # church tower + nave (village landmark)
    pt.poly([(96, yb - 4), (96, yb - 22), (118, yb - 22), (118, yb - 4)], WALL_WHITE)
    pt.poly([(96, yb - 22), (107, yb - 30), (118, yb - 22)], ROOF[1])
    pt.poly([(99, yb - 4), (99, yb - 40), (106, yb - 40), (106, yb - 4)], WALL_WHITE)
    pt.poly([(103, yb - 4), (103, yb - 40), (106, yb - 40), (106, yb - 4)], (196, 192, 182))
    pt.poly([(98.5, yb - 40), (102.5, yb - 47), (106.5, yb - 40)], (60, 60, 64))
    pt.poly([(101, yb - 36), (101, yb - 32), (104, yb - 32), (104, yb - 36)], (40, 40, 44))
    # front row
    house(pt, r, 26, yb, 24, 12, 8, False)
    house(pt, r, 48, yb - 1, 18, 10, 7, True)
    house(pt, r, 2, yb, 16, 9, 6, True, chimney=False)
    # garden trees and hedges between the houses
    for x, rr in ((20, 5.0), (62, 4.0), (90, 5.5)):
        foliage(pt, r, x, yb - rr, rr, rr * 0.9, BEECH, tuft=(0.8, 1.5), core=0.7)
    for x in range(2, w - 2, 5):
        foliage(pt, r, x, yb - 1.0, 2.6, 1.6, SHRUB, tuft=(0.7, 1.2), core=0.5, density=0.6, flowers=0.1)
    return pt.final()


PAINTERS = {"grove": s_grove, "cedar": s_cedar, "palms": s_palms, "tree": s_tree, "shrub": s_shrub,
            "hedge": s_hedge, "wall": s_wall, "rock": s_rock, "houses": s_houses}


def sprites():
    out = []
    for name, pw, ph, ww, wh, sink, kind in SPRITES:
        img = PAINTERS[name](Rng(SEED, "veg", name))
        if img.size != (pw, ph):
            raise ValueError("%s is %dx%d, expected %dx%d" % ((name,) + img.size + (pw, ph)))
        # threshold the alpha (no anti-aliasing against the key, CLAUDE.md 8)
        px = img.load()
        for y in range(ph):
            for x in range(pw):
                c = px[x, y]
                px[x, y] = (c[0], c[1], c[2], 255) if c[3] >= 112 else (0, 0, 0, 0)
        out.append((name, img))
    return out


def atlas():
    at = I.Atlas(ATLAS[0], ATLAS[1], gutter=2)
    imgs = sprites()
    for name, img in imgs:
        at.add(name, img)
    at.pack()
    lines = ["# veg_atlas.txt - A11 sprite table for islandc scatter.py (GENERATED by tools/art/gen_veg.py)",
             "# name x y w h (pixels in veg_atlas.bmp 256x256) world_w world_h sink (metres) kind (0 facing, 1 fixed)"]
    for name, pw, ph, ww, wh, sink, kind in SPRITES:
        x, y, w, h = at.rect(name)
        lines.append("%s %d %d %d %d %.2f %.2f %.2f %d" % (name, x, y, w, h, ww, wh, sink, kind))
    return at, imgs, "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------------------------------- previews

def _sprite_geo(at, name, mat, ww, wh, sink, yaw):
    """One upright card (camera-facing in the game; here oriented by yaw) for the mock scene."""
    u0, v0, u1, v1 = at.uv(name)
    g = M.vquad(ww, wh, (0.0, -sink, 0.0), yaw, mat, (u0, v0, u1, v1))
    return g


def mock_scene(kit, at, eye, target, title):
    """Mock: a green field with groves, cedars, palms, hedges, walls, houses, rocks (kit renderer, 565)."""
    r = Rng(SEED, "mock")
    mat = M.Material("veg", tex="veg_atlas", blend=M.RB_ALPHATEST, cull_none=True)
    meta = dict((s[0], s) for s in SPRITES)
    items = []
    cam_yaw = math.degrees(math.atan2(target[0] - eye[0], target[2] - eye[2]))

    def add(name, x, z, scale=1.0, yaw=None):
        _, _, _, ww, wh, sink, kind = meta[name]
        y = cam_yaw if (yaw is None or kind == 0) else yaw
        g = _sprite_geo(at, name, mat, ww * scale, wh * scale, sink * scale, y)
        items.append((g, (x, 0.0, z)))
    for k in range(26):
        add("grove", r.range(-160, -40), r.range(80, 420), r.range(0.8, 1.2))
    for k in range(10):
        add("cedar", r.range(40, 110), r.range(200, 320), r.range(0.85, 1.15))
    for k in range(7):
        add("palms", r.range(-30, 30), r.range(30, 90), r.range(0.8, 1.2))
    for k in range(6):
        add("hedge", -20 + 24 * k, 140, 1.0, 0.0)
        add("wall", 30, 150 + 24 * k, 1.0, 90.0)
    for k in range(5):
        add("tree", r.range(-20, 140), r.range(60, 260))
        add("shrub", r.range(-20, 60), r.range(20, 120))
        add("rock", r.range(60, 160), r.range(40, 140), r.range(0.8, 2.2))
    add("houses", 90, 120)
    add("houses", 120, 160)
    items.sort(key=lambda it: -((it[1][0] - eye[0]) ** 2 + (it[1][2] - eye[2]) ** 2))
    img = V.scene(items, eye, target, size=(640, 480), fov=60.0, fog=(300.0, 2500.0),
                  ground=((86, 128, 56), 3000.0), tex_dirs=kit.tex_dirs, q16=True, lighting="vertex", mips=True,
                  mip_bias=-0.5)
    V.label(img, title, color=(255, 255, 255))
    return img


def previews(kit, at, imgs):
    sheet = V.contact_sheet([(n, img) for n, img in imgs] + [("veg_atlas", at.image())], cols=5, cell=150, mips=True,
                            title="A11 vegetation / rock / village sprites (keyed; mips below)")
    kit.save_preview("veg_sheet.png", sheet)
    kit.save_preview("veg_scene_low.png", mock_scene(kit, at, (-60.0, 12.0, -60.0), (40.0, 8.0, 200.0),
                                                     "mock: 12 m over a field edge"))
    kit.save_preview("veg_scene_300m.png", mock_scene(kit, at, (-200.0, 300.0, -500.0), (40.0, 0.0, 200.0),
                                                      "mock: 300 m, 750 m out"))


# ---------------------------------------------------------------------------------------------------- build

def build(out_dir, kit):
    at, imgs, txt = atlas()
    sheet = at.image()
    kit.save_bmp("tex/veg_atlas.bmp", sheet, keyed=True)
    kit.save_text("tex/veg_atlas.txt", txt, crlf=True)
    for name, pw, ph, ww, wh, sink, kind in SPRITES:
        x, y, w, h = at.rect(name)
        kit.log("%-7s %3dx%-3d at %3d,%3d  %.1f x %.1f m (%.1f px/m)  kind %d" % (name, w, h, x, y, ww, wh,
                                                                                 w / ww, kind))
    kit.log("veg_atlas 256x256 keyed (1555) %.0f KB with mips" % I.tex_kb(256, 256))
    if kit.previews:
        previews(kit, at, imgs)


if __name__ == "__main__":
    from assetkit import build as B
    sys.exit(B.main_for(__file__))
