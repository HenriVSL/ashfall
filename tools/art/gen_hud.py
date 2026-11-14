"""gen_hud.py - A4/A4b HUD stroke font + symbol sheet + minimap frame (DESIGN 10, 8.8; HUD.md 4.1; TASKS A4).

One atlas SET per screen height so the HUD is drawn 1:1 (never bilinear-scaled) at the common modes:

  set   screen       scale  pages (256x256 each)   small font   large font   symbol stroke
  480   640x480      1.00   hud_atlas_480          cap  9       cap 13       2 px
  600   800x600      1.25   hud_atlas_600          cap 11       cap 16       2 px
  768   1024x768     1.60   hud_atlas_768, _p1     cap 14       cap 21       3 px
  1050  1400x1050    2.19   hud_atlas_1050,_p1,_p2 cap 20       cap 28       4 px

Outputs (data/tex/), per set L: hud_atlas_<L>.bmp + _a.bmp (page 0), hud_atlas_<L>_p<n>.bmp + _a.bmp (further
pages where the set needs them) and hud_atlas_<L>.txt. Every page is 256x256 (DESIGN 8.8 / validator limit,
Voodoo3-safe); each font lives entirely on one page (packed as one block) and every page has its own `white`,
`line` and `line_sh`, so text + lines of one font need no texture switch. hud_atlas.bmp/_a/.txt = an exact copy
of the 480 set (kept for older code). Glyph and sprite NAMES are identical in every set; metrics are in pixels of
that set. The txt format is documented in its header (and docs/HANDOFF.md "A4" / "A4b" / "A4c").

Texels are PREMULTIPLIED and the drop shadow is BAKED (A4c): every glyph/sprite cell holds the white shape plus a
black shadow offset (+o,+o) (o = 1 px at 480/600, 2 px at 768/1050) at 60 % alpha behind it; colour = white x
shape coverage, alpha = shape + shadow. Drawn once with RB_PREMUL (ONE, INVSRCALPHA) the vertex colour tints only
the shape and the shadow stays black, so each glyph/sprite is ONE quad (no shadow pass). `white` (solid) and `line`
(plain stroke profile) have no shadow; `line_sh` is the stroke profile with its shadow below. Glyph strokes are
pixel-hinted (stem centre lines on pixel centres or edges depending on the stroke width) so text is crisp at
whole-pixel positions.

Contents: stroke font in two sizes, monospaced (every digit, capital and most symbols share one advance),
characters 0-9 A-Z . - + : / ' % ( ) ? c and '@' (code 64 = the AOA alpha), space; 3D-view contact marker
(hollow diamond, corner down, solid + dashed) and the TD square enclosing it (solid + dashed), velocity vector,
ghost VV, waterline W, minimap HAFU arch/chevron solid + dashed, selection / pointer triangles, locator arrow head,
threat arrow, off-screen triangle, breakaway X, seeker circle, dots, Raero diamond, aim cross, heading caret,
minimap jet, minimap frame 9-slice, a white texel block and an anti-aliased line profile.

Previews (tools/art/preview/hud_*.png): per set the atlas pages and a readability sheet (every DESIGN 10.2-10.6
string over sky / sea / cloud-white / terrain), mock HUDs at 640x480, 800x600 (cockpit + chase) and 1400x1050
built only from the atlas of that set, composited like the engine (one RB_PREMUL quad per glyph/sprite with the
baked shadow, lines with line_sh; textures quantised to 4444; 480/600 frames to 565).

Run: python3 tools/build_assets.py --only hud   (or python3 tools/art/gen_hud.py [--check]).
Deterministic: own sample-based rasteriser (pure Python), Pillow ops, assetkit rng only.
"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "tools"))
from PIL import Image, ImageChops  # noqa: E402

from assetkit import image as I  # noqa: E402
from assetkit.rng import noise_grid  # noqa: E402

SEED = 404
ORDER = 40
SHADOW_A = 0.6               # baked drop shadow alpha (A4c)
PAGE_W = PAGE_H = 256
SET_LINES = (480, 600, 768, 1050)
SET_PAGES = {480: 1, 600: 1, 768: 2, 1050: 3}    # fixed so ASSETS is static; build() checks the packer agrees


def _assets():
    out = ["tex/hud_atlas.bmp", "tex/hud_atlas_a.bmp", "tex/hud_atlas.txt"]
    for L in SET_LINES:
        for p in range(SET_PAGES[L]):
            stem = "tex/hud_atlas_%d%s" % (L, "" if p == 0 else "_p%d" % p)
            out += [stem + ".bmp", stem + "_a.bmp"]
        out.append("tex/hud_atlas_%d.txt" % L)
    return out


ASSETS = _assets()

SS = 5                       # rasteriser samples per pixel per axis (26 coverage levels; the VRAM copy is 4444)

# HUD colours of DESIGN 10.1 (for previews only; the game takes them from params)
GREEN = (64, 255, 96)
YELLOW = (255, 224, 64)
RED = (255, 48, 48)
SHADOW = 0.5
DIM = (-1, -1, -1)       # preview layer key: translucent black panel
SHL = (-2, -2, -2)       # preview layer key: shadow alpha (darkening) of baked / line shadows


def fl(v):
    return int(math.floor(v))


def rnd(v):
    return int(math.floor(v + 0.5))


# ---------------------------------------------------------------------------------------------------- rasteriser
class Canvas(object):
    """Coverage canvas w x h px. Shapes are given in anchor-relative pixel coordinates (x right, y down; integer
    coordinates are pixel EDGES, pixel (i, j) spans [i, i+1) x [j, j+1)); the anchor sits at canvas (ax, ay).
    Coverage = fraction of SS x SS sample points inside any shape (union)."""

    def __init__(self, w, h, ax=0.0, ay=0.0, clip=None):
        self.w, self.h, self.ax, self.ay = w, h, float(ax), float(ay)
        self.W, self.H = w * SS, h * SS
        self.m = bytearray(self.W * self.H)
        self.clip = clip                       # (y0, y1): ink only for y0 <= y <= y1 (anchor-relative)

    def _fill(self, x0, y0, x1, y1, test):
        ss = SS
        i0 = max(0, fl((x0 + self.ax) * ss - 0.5))
        i1 = min(self.W - 1, int(math.ceil((x1 + self.ax) * ss - 0.5)))
        if self.clip:
            y0 = max(y0, self.clip[0])
            y1 = min(y1, self.clip[1])
        j0 = max(0, fl((y0 + self.ay) * ss - 0.5))
        j1 = min(self.H - 1, int(math.ceil((y1 + self.ay) * ss - 0.5)))
        m = self.m
        W = self.W
        c = self.clip
        for j in range(j0, j1 + 1):
            y = (j + 0.5) / ss - self.ay
            if c and not (c[0] <= y <= c[1]):
                continue
            row = j * W
            for i in range(i0, i1 + 1):
                if m[row + i]:
                    continue
                x = (i + 0.5) / ss - self.ax
                if test(x, y):
                    m[row + i] = 1

    def capsule(self, a, b, hw, cap_a="round", cap_b="round"):
        ax, ay = a
        bx, by = b
        dx, dy = bx - ax, by - ay
        ln = math.hypot(dx, dy)
        if ln < 1e-9:
            if "round" in (cap_a, cap_b):
                self.disc(a, hw)
            return
        ux, uy = dx / ln, dy / ln
        if cap_a == "square":
            ax, ay = ax - ux * hw, ay - uy * hw
        if cap_b == "square":
            bx, by = bx + ux * hw, by + uy * hw
        ln = math.hypot(bx - ax, by - ay)
        hw2 = hw * hw
        ra, rb = cap_a == "round", cap_b == "round"

        def test(x, y):
            px, py = x - ax, y - ay
            t = px * ux + py * uy
            if t < 0.0:
                return ra and px * px + py * py < hw2
            if t > ln:
                qx, qy = x - bx, y - by
                return rb and qx * qx + qy * qy < hw2
            d = px * uy - py * ux
            return d * d < hw2
        self._fill(min(ax, bx) - hw, min(ay, by) - hw, max(ax, bx) + hw, max(ay, by) + hw, test)

    def disc(self, c, r):
        cx, cy = c
        r2 = r * r
        self._fill(cx - r, cy - r, cx + r, cy + r, lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 < r2)

    def polyline(self, pts, hw, closed=False, cap="round"):
        """Stroke through pts. Joins: square (mitre) for 75-105 degree turns, round otherwise; ends: cap."""
        pts = dedupe(pts)
        n = len(pts)
        if n == 1:
            self.disc(pts[0], hw)
            return
        if closed and n > 2 and dist(pts[0], pts[-1]) < 1e-6:
            pts = pts[:-1]
            n -= 1
        elif closed and n <= 2:
            closed = False

        def join(i):
            if not closed and (i == 0 or i == n - 1):
                return cap
            a, b, c = pts[(i - 1) % n], pts[i], pts[(i + 1) % n]
            d1 = unit(b[0] - a[0], b[1] - a[1])
            d2 = unit(c[0] - b[0], c[1] - b[1])
            turn = math.degrees(math.acos(max(-1.0, min(1.0, d1[0] * d2[0] + d1[1] * d2[1]))))
            return "square" if 75.0 <= turn <= 105.0 else "round"
        kinds = [join(i) for i in range(n)]
        nseg = n if closed else n - 1
        for i in range(nseg):
            j = (i + 1) % n
            self.capsule(pts[i], pts[j], hw, kinds[i], kinds[j])

    def polygon(self, pts):
        """Filled polygon (even-odd)."""
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        n = len(pts)

        def test(x, y):
            inside = False
            k = n - 1
            for i in range(n):
                xi, yi = pts[i]
                xk, yk = pts[k]
                if (yi > y) != (yk > y) and x < (xk - xi) * (y - yi) / (yk - yi) + xi:
                    inside = not inside
                k = i
            return inside
        self._fill(min(xs), min(ys), max(xs), max(ys), test)

    def coverage(self):
        """-> 'L' image w x h (box filtered samples)."""
        big = Image.frombytes("L", (self.W, self.H), bytes(self.m).replace(b"\x01", b"\xff"))
        return big.resize((self.w, self.h), Image.BOX)


def dist(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def unit(dx, dy):
    ln = math.hypot(dx, dy)
    return (dx / ln, dy / ln) if ln > 1e-12 else (1.0, 0.0)


def dedupe(pts):
    out = []
    for p in pts:
        if not out or dist(out[-1], p) > 1e-6:
            out.append((float(p[0]), float(p[1])))
    return out


def arc_pts(cx, cy, rx, ry, a0, a1, step=7.5):
    n = max(2, int(math.ceil(abs(a1 - a0) / step)))
    out = []
    for k in range(n + 1):
        a = math.radians(a0 + (a1 - a0) * k / float(n))
        out.append((cx + rx * math.cos(a), cy + ry * math.sin(a)))
    return out


def dash_path(pts, closed, n, gap):
    """Split a polyline into n equal dashes with gaps `gap` (pattern symmetric; closed: n dashes, n gaps, the
    first dash centred on the start point)."""
    pts = dedupe(pts)
    if closed and dist(pts[0], pts[-1]) > 1e-6:
        pts = pts + [pts[0]]
    segs = []
    acc = [0.0]
    for i in range(len(pts) - 1):
        acc.append(acc[-1] + dist(pts[i], pts[i + 1]))
    total = acc[-1]

    def at(s):
        s = s % total if closed else max(0.0, min(total, s))
        for i in range(len(pts) - 1):
            if s <= acc[i + 1] or i == len(pts) - 2:
                t = (s - acc[i]) / max(1e-12, acc[i + 1] - acc[i])
                return (pts[i][0] + (pts[i + 1][0] - pts[i][0]) * t, pts[i][1] + (pts[i + 1][1] - pts[i][1]) * t)
        return pts[-1]

    def piece(s0, s1):
        out = [at(s0)]
        if closed:
            # walk through the vertices between s0 and s1 (may wrap)
            k = 0
            s = s0
            while k < 4 * len(pts):
                k += 1
                nxt = None
                base = math.floor(s / total) * total
                for a in acc[1:]:
                    if base + a > s + 1e-9:
                        nxt = base + a
                        break
                if nxt is None or nxt >= s1:
                    break
                out.append(at(nxt))
                s = nxt
        else:
            for a in acc[1:-1]:
                if s0 < a < s1:
                    out.append(at(a))
        out.append(at(s1))
        return out
    if closed:
        d = (total - n * gap) / float(n)
        for k in range(n):
            s0 = -d / 2.0 + k * (d + gap)
            segs.append(piece(s0, s0 + d))
    else:
        d = (total - (n - 1) * gap) / float(n)
        for k in range(n):
            s0 = k * (d + gap)
            segs.append(piece(s0, s0 + d))
    return segs


# ---------------------------------------------------------------------------------------------------- stroke font
# Design grid: cap top y = 0, baseline y = 10 (stroke CENTRE lines), glyph box x = 0..6 (every digit and capital;
# narrow glyphs '.' ':' "'" use x = 0 only). Strokes: ("P", [items], closed) where items are points (x, y) or arcs
# ("A", x0, y0, x1, y1, a0, a1) = elliptical arc inside that box from angle a0 to a1 in degrees (0 = right,
# 90 = down, i.e. increasing = clockwise on screen); ("D", x, y) = round dot. Coordinates are snapped per font to
# its pixel grid (Font.x / Font.y) before the curves are sampled, so stems are crisp.

def A(x0, y0, x1, y1, a0, a1):
    return ("A", x0, y0, x1, y1, a0, a1)


def P(*items):
    return ("P", list(items), False)


def C(*items):
    return ("P", list(items), True)


def D(x, y):
    return ("D", x, y)


def stadium(x0, y0, x1, y1):
    r = (x1 - x0) / 2.0
    return C(A(x0, y0, x1, y0 + 2 * r, 180, 360), (x1, y1 - r), A(x0, y1 - 2 * r, x1, y1, 0, 180), (x0, y0 + r))


def ellipse(x0, y0, x1, y1):
    return C(A(x0, y0, x1, y1, 0, 360))


def rrect(x0, y0, x1, y1, r):
    return C(A(x0, y0, x0 + 2 * r, y0 + 2 * r, 180, 270), (x1 - r, y0), A(x1 - 2 * r, y0, x1, y0 + 2 * r, 270, 360),
             (x1, y1 - r), A(x1 - 2 * r, y1 - 2 * r, x1, y1, 0, 90), (x0 + r, y1),
             A(x0, y1 - 2 * r, x0 + 2 * r, y1, 90, 180), (x0, y0 + r))


def xform(strokes, fx, fy, arc_angle):
    """Map strokes through point functions; arcs: box corners mapped, angles through arc_angle(a)."""
    out = []
    for st in strokes:
        if st[0] == "D":
            out.append(("D", fx(st[1]), fy(st[2])))
            continue
        items = []
        for it in st[1]:
            if it[0] == "A":
                xa, xb = sorted((fx(it[1]), fx(it[3])))
                ya, yb = sorted((fy(it[2]), fy(it[4])))
                items.append(("A", xa, ya, xb, yb, arc_angle(it[5]), arc_angle(it[6])))
            else:
                items.append((fx(it[0]), fy(it[1])))
        out.append(("P", items, st[2]))
    return out


def rot180(strokes, w=6.0):
    return xform(strokes, lambda x: w - x, lambda y: 10.0 - y, lambda a: a + 180.0)


def mirror_x(strokes, w=6.0):
    return xform(strokes, lambda x: w - x, lambda y: y, lambda a: 180.0 - a)


def _glyph_defs():
    g = {}
    six = [P(A(0, 0, 6, 6, 300, 180), (0, 7)), ellipse(0, 4, 6, 10)]
    g["0"] = (6, [stadium(0, 0, 6, 10)])
    g["1"] = (6, [P((1.2, 2.2), (3, 0), (3, 10))])
    g["2"] = (6, [P(A(0, 0, 6, 6, 165, 395), (0, 10), (6, 10))])
    g["3"] = (6, [P(A(0.2, 0, 5.8, 5, 200, 450)), P(A(0, 5, 6, 10, 270, 520))])
    g["4"] = (6, [P((4.5, 10), (4.5, 0), (0, 7), (6, 7))])
    g["5"] = (6, [P((5.6, 0), (0.4, 0), (0.2, 4.5), A(0, 3.6, 6, 10, 210, 505))])
    g["6"] = (6, six)
    g["7"] = (6, [P((0, 0), (6, 0), (2.2, 10))])
    g["8"] = (6, [ellipse(0.6, 0, 5.4, 4.8), ellipse(0, 4.8, 6, 10)])
    g["9"] = (6, rot180(six))
    g["A"] = (6, [P((0, 10), (3, 0), (6, 10)), P((1.0, 6.6), (5.0, 6.6))])
    g["B"] = (6, [P((0, 4.8), (3.2, 4.8), A(0.8, 0, 5.6, 4.8, 90, -90), (0, 0), (0, 10), (3.4, 10),
                    A(0.8, 4.8, 6, 10, 90, -90), (3.4, 4.8))])
    g["C"] = (6, [P(A(0, 0, 6, 6, 330, 180), (0, 7), A(0, 4, 6, 10, 180, 30))])
    g["D"] = (6, [C((0, 0), (3, 0), A(0, 0, 6, 6, 270, 360), (6, 7), A(0, 4, 6, 10, 0, 90), (0, 10))])
    g["E"] = (6, [P((6, 0), (0, 0), (0, 10), (6, 10)), P((0, 4.8), (4.6, 4.8))])
    g["F"] = (6, [P((6, 0), (0, 0), (0, 10)), P((0, 4.8), (4.6, 4.8))])
    g["G"] = (6, [P(A(0, 0, 6, 6, 330, 180), (0, 7), A(0, 4, 6, 10, 180, 0), (6, 5.4), (3.4, 5.4))])
    g["H"] = (6, [P((0, 0), (0, 10)), P((6, 0), (6, 10)), P((0, 4.8), (6, 4.8))])
    g["I"] = (6, [P((3, 0), (3, 10)), P((1.2, 0), (4.8, 0)), P((1.2, 10), (4.8, 10))])
    g["J"] = (6, [P((6, 0), (6, 7), A(0, 4, 6, 10, 0, 160))])
    g["K"] = (6, [P((0, 0), (0, 10)), P((6, 0), (0, 6.4)), P((2.2, 4.2), (6, 10))])
    g["L"] = (6, [P((0, 0), (0, 10), (6, 10))])
    g["M"] = (6, [P((0, 10), (0, 0), (3, 6), (6, 0), (6, 10))])
    g["N"] = (6, [P((0, 10), (0, 0), (6, 10), (6, 0))])
    g["O"] = (6, [rrect(0, 0, 6, 10, 2.2)])
    g["P"] = (6, [P((0, 10), (0, 0), (3.4, 0), A(0.8, 0, 6, 5.2, 270, 450), (0, 5.2))])
    g["Q"] = (6, [rrect(0, 0, 6, 10, 2.2), P((3.6, 7.0), (6, 10))])
    g["R"] = (6, [P((0, 10), (0, 0), (3.4, 0), A(0.8, 0, 6, 5.2, 270, 450), (0, 5.2)), P((3.2, 5.2), (6, 10))])
    g["S"] = (6, [P(A(0, 0, 6, 5, 335, 90), A(0, 5, 6, 10, 270, 515))])
    g["T"] = (6, [P((0, 0), (6, 0)), P((3, 0), (3, 10))])
    g["U"] = (6, [P((0, 0), (0, 7), A(0, 4, 6, 10, 180, 0), (6, 0))])
    g["V"] = (6, [P((0, 0), (3, 10), (6, 0))])
    g["W"] = (6, [P((0, 0), (0.9, 10), (3, 4.4), (5.1, 10), (6, 0))])
    g["X"] = (6, [P((0, 0), (6, 10)), P((6, 0), (0, 10))])
    g["Y"] = (6, [P((0, 0), (3, 5.2)), P((6, 0), (3, 5.2)), P((3, 5.2), (3, 10))])
    g["Z"] = (6, [P((0, 0), (6, 0), (0, 10), (6, 10))])
    g["."] = (0, [D(0, 10)])
    g[":"] = (0, [D(0, 3), D(0, 9)])
    g["'"] = (0, [P((0, 0), (0, 3))])
    g["-"] = (6, [P((1, 5), (5, 5))])
    g["+"] = (6, [P((1, 5), (5, 5)), P((3, 2), (3, 8))])
    g["/"] = (6, [P((0.5, 10), (5.5, 0))])
    g["%"] = (6, [P((0.5, 10), (5.5, 0)), ellipse(0, 0, 2.6, 3.6), ellipse(3.4, 6.4, 6, 10)])
    g["("] = (6, [P((4.2, 0), (2.6, 2.6), (2.6, 7.4), (4.2, 10))])
    g[")"] = (6, mirror_x(g["("][1]))
    g["?"] = (6, [P(A(0, 0, 6, 5.6, 190, 410), (3, 6.4), (3, 7.2)), D(3, 10)])
    g["c"] = (6, [P(A(0.4, 4, 5.6, 10, 320, 40))])
    g["@"] = (6, [P((6, 3), (3.5, 8.5), A(0, 3, 4.2, 10, 60, 300), (3.5, 4.5), (6, 10))])   # AOA alpha
    g[" "] = (6, [])
    return g


GLYPHS = _glyph_defs()
FONT_CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.:'-+/%()?c@ "

class Font(object):
    """One pixel-hinted size. cap = ink rows of a capital; sw = rendered stroke width; gs = grid stroke (the stem
    width the hinting assumes: centre lines at gs/2 + whole px, so gs 1 = pixel centres, 2 = pixel edges ...);
    inkw = ink columns of a full-width glyph (inkw - gs even keeps centred strokes symmetric); gap = px between
    two glyph ink boxes."""

    def __init__(self, fid, name, cap, sw, gs, inkw, gap):
        self.fid, self.name, self.cap, self.sw, self.gs, self.inkw, self.gap = fid, name, cap, sw, gs, inkw, gap
        self.hw = sw / 2.0
        self.c0 = gs / 2.0
        self.sx = (inkw - gs) / 6.0
        self.sy = (cap - gs) / 10.0
        self.narrow = int(math.ceil(gs))                        # ink columns of '.', ':', "'"
        self.adv = inkw + gap
        self.margin = 2                                         # soft edge px + 1 transparent px around the ink
        self.cell_h = cap + 2 * self.margin
        self.oy = -self.margin

    def x(self, u):
        return self.c0 + rnd(u * self.sx)

    def y(self, v):
        return self.c0 + rnd(v * self.sy)

    def glyph_adv(self, ch):
        w = GLYPHS[ch][0]
        return (self.narrow + self.gap) if w == 0 and ch != " " else self.adv


class HudSet(object):
    """One atlas set: screen height `lines`, scale k = lines / 480 against the 640x480 virtual HUD layout."""

    def __init__(self, lines, fonts, ssw, msw, flw):
        self.lines = lines
        self.k = lines / 480.0
        self.fonts = fonts
        self.ssw = ssw            # symbol / line stroke px (the code's ui_line width at this set)
        self.msw = msw            # minimap symbol stroke px
        self.flw = flw            # minimap frame line px
        self.tex = "hud_atlas_%d" % lines

    def page_tex(self, p):
        return self.tex if p == 0 else "%s_p%d" % (self.tex, p)


SETS = [
    HudSet(480, [Font(0, "small", 9, 1.5, 1, 5, 2), Font(1, "large", 13, 2.0, 2, 8, 3)], 2, 1.2, 1),
    HudSet(600, [Font(0, "small", 11, 2.0, 2, 6, 3), Font(1, "large", 16, 2.0, 2, 10, 4)], 2, 1.5, 1),
    HudSet(768, [Font(0, "small", 14, 2.0, 2, 8, 3), Font(1, "large", 21, 3.0, 3, 13, 4)], 3, 2.0, 2),
    HudSet(1050, [Font(0, "small", 20, 3.0, 3, 11, 4), Font(1, "large", 28, 4.0, 4, 18, 5)], 4, 2.5, 2),
]
SET_BY_LINES = dict((s.lines, s) for s in SETS)


def strokes_px(strokes, f):
    """Design strokes -> [(kind, pts, closed)] in pixel coordinates relative to the pen (ink box top-left)."""
    out = []
    for st in strokes:
        if st[0] == "D":
            out.append(("D", [(f.x(st[1]), f.y(st[2]))], False))
            continue
        pts = []
        for it in st[1]:
            if it[0] == "A":
                X0, X1 = f.x(it[1]), f.x(it[3])
                Y0, Y1 = f.y(it[2]), f.y(it[4])
                pts.extend(arc_pts((X0 + X1) / 2.0, (Y0 + Y1) / 2.0, (X1 - X0) / 2.0, (Y1 - Y0) / 2.0, it[5], it[6]))
            else:
                pts.append((f.x(it[0]), f.y(it[1])))
        closed = st[2]
        out.append(("P", pts, closed))
    return out


def render_glyph(ch, f):
    """-> (coverage 'L' cell image or None, ox) ; cell top = cap top + f.oy, cell left = pen + ox."""
    w, strokes = GLYPHS[ch]
    if not strokes:
        return None, 0
    m = f.margin
    cw = (f.inkw if w else f.narrow) + 2 * m + 2
    ch_ = f.cell_h
    cv = Canvas(cw, ch_, m, m, clip=(f.c0 - f.hw, f.y(10) + f.hw))
    for kind, pts, closed in strokes_px(strokes, f):
        if kind == "D":
            cv.disc(pts[0], f.hw * 1.25)
        else:
            cv.polyline(pts, f.hw, closed=closed, cap="round")
    img = cv.coverage()
    bb = img.getbbox()
    x0 = max(0, bb[0] - 1)
    x1 = min(cw, bb[2] + 1)
    return img.crop((x0, 0, x1, ch_)), x0 - m



# ---------------------------------------------------------------------------------------------------- symbols
# Symbols are designed in 480-line px around the anchor (0, 0) = the hot spot, an integer pixel EDGE point, and
# scaled by the set's k. Straight-line coordinates are snapped (q) so strokes stay crisp: even stroke widths put
# centre lines on pixel edges, odd ones on pixel centres (lines through the anchor itself stay on the edge,
# symmetric). Shapes: ("L", pts, hw, closed, cap) polyline, ("F", pts) filled polygon, ("O", (cx, cy), r) disc,
# ("R", (cx, cy), r, hw) ring.

def ring_pts(r, a0=0.0, a1=360.0, cx=0.0, cy=0.0):
    return arc_pts(cx, cy, r, r, a0, a1, step=3.0)


def sym_defs(S):
    k = S.k
    sw = S.ssw
    hw = sw / 2.0
    hs = S.msw / 2.0

    def q(v):
        v *= k
        if abs(v) < 1e-9:
            return 0.0
        s = 1.0 if v > 0 else -1.0
        a = abs(v)
        return s * float(rnd(a)) if sw % 2 == 0 else s * (fl(a) + 0.5)

    def Q(pts):
        return [(q(x), q(y)) for x, y in pts]

    def K(pts):
        return [(x * k, y * k) for x, y in pts]

    def line(pts):
        return ("L", Q(pts), hw, False, "butt")
    out = []
    # velocity vector: circle r 5 (centre line), wing ticks 7 px, top tick 4 px (DESIGN 10.2 #7)
    out.append(("vv", [("R", (0, 0), 5.0 * k, hw), line([(-6, 0), (-13, 0)]), line([(6, 0), (13, 0)]),
                       line([(0, -6), (0, -10)])]))
    ghost = [("L", ring_pts(5.0 * k, a - 15, a + 15), hw, False, "butt") for a in (270, 330, 30, 90, 150, 210)]
    ghost += [line([(-6, 0), (-8.5, 0)]), line([(-10.5, 0), (-13, 0)]), line([(6, 0), (8.5, 0)]),
              line([(10.5, 0), (13, 0)]), line([(0, -6), (0, -10)])]
    out.append(("vv_ghost", ghost))
    # waterline W, 28 px wide, anchor = centre peak = boresight (DESIGN 10.2 #6)
    out.append(("waterline", [line([(-14, 0), (-8, 0), (-4, 6), (0, 0), (4, 6), (8, 0), (14, 0)])]))
    # 3D-view contact marker (A4b): hollow diamond, corner down, ~10 px across at 480 (12 at 600); memory = dashed
    r = 3.8 * k
    dm = [(0, -r), (r, 0), (0, r), (-r, 0)]
    out.append(("tgt_diamond", [("L", dm, hw, True, "butt")]))
    edge = r * math.sqrt(2.0)
    out.append(("tgt_diamond_dash", [sh for sh in corner_dash(dm, 0.36 * edge, 0.0, hw) if len(sh[1]) == 3]))
    # TD (designated): square enclosing the diamond, 20 px outer at 480; memory = corner brackets + edge dashes
    h = q(9)
    sq = [(-h, -h), (h, -h), (h, h), (-h, h)]
    out.append(("tgt_box", [("L", sq, hw, True, "butt")]))
    side = 2 * h
    out.append(("tgt_box_dash", corner_dash(sq, rnd(0.28 * side), rnd(0.22 * side), hw)))
    # minimap HAFU 8 x 4 px at 480 (A4b: HAFU only on the minimap); anchor = contact, feet on the anchor row
    a = 3.5 * k
    rr = 1.5 * k
    arch_s = [(-a, 0), (-a, -a + rr)] + arc_pts(-a + rr, -a + rr, rr, rr, 200, 270, 10) + \
        arc_pts(a - rr, -a + rr, rr, rr, 270, 340, 10) + [(a, -a + rr), (a, 0)]
    chev_s = [(-a, 0), (0, -a), (a, 0)]
    cl = (-a - 3 * hs - 2, 0.0)
    out.append(("map_arch", [("L", arch_s, hs, False, "butt")], cl))
    out.append(("map_arch_dash", [("L", p, hs, False, "butt") for p in dash_path(arch_s, False, 3, 1.5 * k)], cl))
    out.append(("map_chev", [("L", chev_s, hs, False, "butt")], cl))
    out.append(("map_chev_dash", [("L", p, hs, False, "butt") for p in dash_path(chev_s, False, 2, 2.0 * k)], cl))
    # pointers and arrows (filled; anchors at the tips except the off-screen triangle)
    out.append(("sel_tri", [("F", K([(-4.5, -6), (4.5, -6), (0, 0)]))]))          # selection, points down, 9 px
    out.append(("ptr_tri", [("F", K([(-3.5, -5), (3.5, -5), (0, 0)]))]))          # bank / range pointer
    out.append(("loc_arrow", [line([(-6, -5), (0, 0), (-6, 5)])]))                  # points +x
    out.append(("threat_arrow", [("F", K([(0, 0), (-18, -8), (-12, 0), (-18, 8)]))]))   # points +x, 18 px
    out.append(("offscreen_tri", [("F", K([(5, 0), (-5, -5), (-5, 5)]))]))         # points +x, 10 px
    out.append(("breakaway_x", [line([(-29, -29), (29, 29)]), line([(29, -29), (-29, 29)])]))
    out.append(("seeker", [("R", (0, 0), 26.0 * k, hw)]))                            # 3 deg
    out.append(("dot", [("O", (0, 0), 1.2 * k)]))                                    # 2 x 2 at 480
    out.append(("dot4", [("O", (0, 0), 2.1 * k)]))                                   # steering dot
    out.append(("raero", [("L", K([(0, -3), (3, 0), (0, 3), (-3, 0)]), 0.75 * hw, True, "butt")]))
    out.append(("aim_cross", [line([(-7, 0), (-3, 0)]), line([(3, 0), (7, 0)]), line([(0, -7), (0, -3)]),
                              line([(0, 3), (0, 7)])]))
    out.append(("caret", [line([(-4, 4), (0, 0), (4, 4)])]))                        # heading caret, points up
    jet = [(0, -7), (0.9, -5), (0.9, -1.5), (5.5, 2), (5.5, 3.4), (0.9, 2), (0.9, 4.6), (2.8, 6), (2.8, 7),
           (0, 6.4), (-2.8, 7), (-2.8, 6), (-0.9, 4.6), (-0.9, 2), (-5.5, 3.4), (-5.5, 2), (-0.9, -1.5), (-0.9, -5)]
    out.append(("map_jet", [("F", K(jet))]))                                         # nose up, 11 px at 480
    return out


def corner_dash(poly, leg, mid, hw):
    """Dashed closed polygon: an L bracket of `leg` px at every corner + one `mid` px dash centred on every edge."""
    out = []
    n = len(poly)
    for i in range(n):
        a, b, c = poly[(i - 1) % n], poly[i], poly[(i + 1) % n]
        u1 = unit(a[0] - b[0], a[1] - b[1])
        u2 = unit(c[0] - b[0], c[1] - b[1])
        out.append(("L", [(b[0] + u1[0] * leg, b[1] + u1[1] * leg), b, (b[0] + u2[0] * leg, b[1] + u2[1] * leg)],
                    hw, False, "butt"))
        mx, my = (b[0] + c[0]) / 2.0, (b[1] + c[1]) / 2.0
        out.append(("L", [(mx - u2[0] * mid / 2, my - u2[1] * mid / 2), (mx + u2[0] * mid / 2, my + u2[1] * mid / 2)],
                    hw, False, "butt"))
    return out


def shape_bounds(shapes):
    x0 = y0 = 1e9
    x1 = y1 = -1e9
    for sh in shapes:
        if sh[0] in ("L", "F"):
            pad = sh[2] if sh[0] == "L" else 0.0
            pad *= 1.5
            for p in sh[1]:
                x0, y0, x1, y1 = min(x0, p[0] - pad), min(y0, p[1] - pad), max(x1, p[0] + pad), max(y1, p[1] + pad)
        elif sh[0] == "O":
            (cx, cy), r = sh[1], sh[2]
            x0, y0, x1, y1 = min(x0, cx - r), min(y0, cy - r), max(x1, cx + r), max(y1, cy + r)
        elif sh[0] == "R":
            (cx, cy), r, hw = sh[1], sh[2], sh[3]
            x0, y0, x1, y1 = min(x0, cx - r - hw), min(y0, cy - r - hw), max(x1, cx + r + hw), max(y1, cy + r + hw)
    return x0, y0, x1, y1


def render_symbol(shapes, clip=None):
    """-> (coverage image, ox, oy): anchor at (ox, oy) inside the image (integer edge coordinates)."""
    x0, y0, x1, y1 = shape_bounds(shapes)
    if clip:
        y0, y1 = max(y0, clip[0]), min(y1, clip[1])
    ox = -fl(x0) + 2
    oy = -fl(y0) + 2
    w = int(math.ceil(x1)) + ox + 2
    h = int(math.ceil(y1)) + oy + 2
    cv = Canvas(w, h, ox, oy, clip=clip)
    for sh in shapes:
        if sh[0] == "L":
            cv.polyline(sh[1], sh[2], closed=sh[3], cap=sh[4])
        elif sh[0] == "F":
            cv.polygon(sh[1])
        elif sh[0] == "O":
            cv.disc(sh[1], sh[2])
        elif sh[0] == "R":
            (cx, cy), r, hw = sh[1], sh[2], sh[3]
            ro2, ri2 = (r + hw) ** 2, (r - hw) ** 2
            cv._fill(cx - r - hw, cy - r - hw, cx + r + hw, cy + r + hw,
                     lambda x, y: ri2 < (x - cx) ** 2 + (y - cy) ** 2 < ro2)
    img = cv.coverage()
    bb = img.getbbox()
    l, t = max(0, bb[0] - 1), max(0, bb[1] - 1)
    r_, b = min(w, bb[2] + 1), min(h, bb[3] + 1)
    return img.crop((l, t, r_, b)), ox - l, oy - t



def map_corner(S):
    return rnd(8 * S.k)


def map_frame(S):
    """Minimap frame 9-slice (DESIGN 10.4), 3 x corner px square: S.flw px outer line + S.ssw px corner brackets."""
    c = map_corner(S)
    n = 3 * c
    cv = Canvas(n, n, 0, 0)
    lw = float(S.flw)
    e0, e1 = lw / 2.0, n - lw / 2.0
    cv.polyline([(e0, e0), (e1, e0), (e1, e1), (e0, e1)], lw / 2.0, closed=True)
    bw = float(S.ssw)
    for sx, sy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
        cx = bw / 2.0 if sx > 0 else n - bw / 2.0
        cy = bw / 2.0 if sy > 0 else n - bw / 2.0
        leg = c - 1 - bw / 2.0
        cv.polyline([(cx, cy + sy * leg), (cx, cy), (cx + sx * leg, cy)], bw / 2.0, cap="butt")
    return cv.coverage()


def line_profile(S):
    """Anti-aliased line cross-section for the set's stroke (S.ssw px): rows 0, 1/4, 1 x ssw, 1/4, 0; 4 wide."""
    vals = [0, 64] + [255] * S.ssw + [64, 0]
    img = Image.new("L", (4, len(vals)), 0)
    px = img.load()
    for y, v in enumerate(vals):
        for x in range(4):
            px[x, y] = v
    return img


def white_block():
    return Image.new("RGBA", (4, 4), (255, 255, 255, 255))


def shadow_off(S):
    return max(1, rnd(S.k))


def bake(cov, ox, oy):
    """Coverage 'L' -> premultiplied RGBA with the drop shadow: the cell grows by (ox, oy) to the right/bottom (the
    top-left and therefore every anchor stays put). RGB = shape coverage c, A = c + SHADOW_A * s * (1 - c)."""
    w, h = cov.size
    W, H = w + ox, h + oy
    c = Image.new("L", (W, H), 0)
    c.paste(cov, (0, 0))
    sh = Image.new("L", (W, H), 0)
    sh.paste(cov, (ox, oy))
    cd, sd = list(c.getdata()), list(sh.getdata())
    a = [min(255, rnd(cv + SHADOW_A * sv * (255 - cv) / 255.0)) for cv, sv in zip(cd, sd)]
    al = Image.new("L", (W, H))
    al.putdata(a)
    return Image.merge("RGBA", (c, c, c, al))


def plain(cov):
    return Image.merge("RGBA", (cov, cov, cov, cov))


# ---------------------------------------------------------------------------------------------------- atlas
def font_block(grp):
    """Pack one font's glyph cells into a single block (PAGE_W - 2 wide, minimal height, 1 px gutters) so the
    whole font lands on one page and packs as densely as the uniform cell height allows."""
    sizes = [im.size for _, im in grp]
    bw = PAGE_W - 2
    h = max(sz[1] for sz in sizes) + 2
    while True:
        try:
            pos = I.pack_rects(sizes, bw, h, 1)
            break
        except ValueError:
            h += 1
            if h > PAGE_H - 2:
                raise
    hmax = max(y + sz[1] for (x, y), sz in zip(pos, sizes)) + 1
    blk = Image.new("RGBA", (bw, hmax), (0, 0, 0, 0))
    offs = {}
    for (key, im), (x, y) in zip(grp, pos):
        blk.paste(im, (x, y))
        offs[key] = (x, y, im.size[0], im.size[1])
    return blk, offs


def build_set(S):
    """-> (list of RGBA page images, info). Each font goes whole onto one page (packed as one block); symbols fill
    the remaining space, first page first; every page has its own white block and line / line_sh profiles."""
    o = shadow_off(S)
    groups = []
    glyphs = {}
    blocks = {}
    for f in S.fonts:
        grp = []
        for ch in FONT_CHARS:
            img, ox = render_glyph(ch, f)
            key = "g%d_%03d" % (f.fid, ord(ch))
            if img is not None:
                grp.append((key, bake(img, o, o)))
            glyphs[(f.fid, ch)] = (key if img is not None else None, ox, f.glyph_adv(ch))
        blk, offs = font_block(grp)
        blocks["font%d" % f.fid] = offs
        groups.append(("font%d" % f.fid, [("font%d" % f.fid, blk)]))
    anchors = {}
    for d in sym_defs(S):
        name, shapes = d[0], d[1]
        clip = d[2] if len(d) > 2 else None
        img, ox, oy = render_symbol(shapes, clip)
        anchors[name] = (ox, oy)
        groups.append((name, [(name, bake(img, o, o))]))
    anchors["map_frame"] = (0, 0)
    groups.append(("map_frame", [("map_frame", bake(map_frame(S), o, o))]))
    lp = line_profile(S)
    common = [("white", white_block()), ("line", plain(lp)), ("line_sh", bake(lp, 0, o))]
    # page layout: font blocks stacked full width from the top, then the rest of the page shelf-packed with
    # symbols (tallest first) + this page's white/line/line_sh; symbols that don't fit go to the next page.
    fonts = [(g[0], g[1][0][1]) for g in groups if g[0].startswith("font")]
    syms = [g[1][0] for g in groups if not g[0].startswith("font")]
    order = sorted(range(len(syms)), key=lambda i: (-syms[i][1].size[1], -syms[i][1].size[0], i))
    todo = [syms[i] for i in order]
    pages = []                      # [(placements [(name, img, x, y)], ...)]
    gpage = {}
    fi = 0
    while fi < len(fonts) or todo:
        place = []
        y = 1
        while fi < len(fonts) and y + fonts[fi][1].size[1] + 1 + 12 <= PAGE_H:
            name, blk = fonts[fi]
            place.append((name, blk, 1, y))
            gpage[name] = len(pages)
            y += blk.size[1] + 1
            fi += 1
        region = PAGE_H - y + 1
        chosen = list(common)
        rest = []
        for it in todo:
            trial = chosen + [it]
            try:
                I.pack_rects([im.size for _, im in trial], PAGE_W, region, 1)
                chosen = trial
            except ValueError:
                rest.append(it)
        pos = I.pack_rects([im.size for _, im in chosen], PAGE_W, region, 1)
        for (name, im), (x, yy) in zip(chosen, pos):
            place.append((name, im, x, y - 1 + yy))
            if name not in ("white", "line", "line_sh"):
                gpage[name] = len(pages)
        if len(chosen) == len(common) and not any(n.startswith("font") for n, _, _, _ in place):
            raise ValueError("set %d: symbols do not fit on an empty page" % S.lines)
        todo = rest
        pages.append(place)
    rects = {}                      # (name, page) -> (x, y, w, h)
    images = []
    used = 0
    for pi, place in enumerate(pages):
        img = Image.new("RGBA", (PAGE_W, PAGE_H), (0, 0, 0, 0))
        for name, im, x, y in place:
            img.paste(im, (x, y))
            if name in blocks:              # a font block: its glyph cells
                for key, (cx, cy, cw, ch_) in blocks[name].items():
                    rects[(key, pi)] = (x + cx, y + cy, cw, ch_)
                    used += cw * ch_
            else:
                rects[(name, pi)] = (x, y, im.size[0], im.size[1])
                used += im.size[0] * im.size[1]
        images.append(img)
    info = {"set": S, "shadow": o, "glyphs": glyphs, "anchors": anchors, "rects": rects, "gpage": gpage,
            "pages": len(pages), "used": used}
    return images, info


def entry(info, name):
    """-> (page, x, y, w, h) of a glyph key or sprite (white/line: page 0's copy)."""
    gp = info["gpage"]
    if name.startswith("g") and name[1:2].isdigit() and "_" in name:
        p = gp["font" + name[1]]
    else:
        p = gp.get(name, 0)
    x, y, w, h = info["rects"][(name, p)]
    return p, x, y, w, h


SPRITE_NOTES = {
    "white": "solid white texels for lines/boxes/backgrounds: sample the centre of this rect (u,v = x+1, y+1)",
    "line": "AA line profile of the set's stroke, NO shadow: map v across the stroke (quad = rect h tall, centred "
            "on the line: centre = oy = h/2), u anywhere inside",
    "line_sh": "line profile + baked shadow below (shadow offset o rows): quad = rect h tall with the stroke centre "
               "(oy) on the line and +v pointing screen-down (horizontal lines) / screen-right (vertical lines); for "
               "diagonals point +v to the down-right side. One quad per line incl. its shadow",
    "vv": "velocity vector: circle r5 + wing ticks 7 + top tick 4 (x scale); anchor = flight path point",
    "vv_ghost": "ghost VV (caged): dashed circle, broken wings",
    "waterline": "waterline W 28 px (x scale); anchor = centre peak = boresight",
    "tgt_diamond": "3D-view contact marker: hollow diamond, corner down, 10 px (x scale; 12 at 600); tint red "
                   "hostile / yellow unknown; anchor = contact",
    "tgt_diamond_dash": "contact marker, memory (four corner pieces)",
    "tgt_box": "TD box (designated): square enclosing tgt_diamond, 20 px outer (x scale); anchor = contact",
    "tgt_box_dash": "TD box, lock in memory (corner brackets + edge dashes)",
    "map_arch": "minimap HAFU unknown 8x4 (x scale); anchor = contact; feet end on the anchor row",
    "map_arch_dash": "minimap unknown, memory",
    "map_chev": "minimap HAFU hostile 8x4 (x scale); anchor = contact",
    "map_chev_dash": "minimap hostile, memory",
    "sel_tri": "selection triangle 9 px (x scale), points down; anchor = tip (6 px x scale above the marker top)",
    "ptr_tri": "small pointer triangle 7 px, points down; anchor = tip (bank pointer; rotate for the range pointer)",
    "loc_arrow": "locator line arrow head (open V), points +x; anchor = tip; rotate",
    "threat_arrow": "threat arrow (filled dart) 18 px, points +x; anchor = tip; rotate, scale <= 1 for far ones",
    "offscreen_tri": "off-screen contact triangle 10 px, points +x; anchor = centre; rotate",
    "breakaway_x": "breakaway X 60 px; anchor = centre",
    "seeker": "SRM seeker circle r26 (3 deg); anchor = centre",
    "dot": "contact / missile dot, 2x2 at 480; anchor = centre",
    "dot4": "steering dot 4 px at 480; anchor = centre",
    "raero": "Raero diamond 6 px; anchor = centre",
    "aim_cross": "chase aim cross 14 px (6 px open centre); anchor = centre",
    "caret": "heading caret, points up; anchor = tip",
    "map_jet": "minimap own jet, nose up, 11 px at 480; anchor = centre",
    "map_frame": "minimap frame 9-slice: corner = (rect w - shadowOff) / 3; left/top slices = corner, right/bottom "
                 "slices = corner + shadowOff (they carry the shadow); corners 1:1, edges stretched, centre empty",
}


def set_txt(S, info, tex_names):
    k = S.k
    L = [
        "# hud_atlas_%d.txt - HUD stroke font + symbols for %d screen lines (%dx%d). GENERATED by" % (
            S.lines, S.lines, rnd(S.lines * 4 / 3.0), S.lines),
        "# tools/art/gen_hud.py (A4/A4b): edit the generator. Sets: 480 600 768 1050 (same names in each, metrics",
        "# in px of the set). Pick the set whose lines match the screen height (else the next smaller one) and",
        "# place everything at virtual 640x480 coordinates x scale (screen h / 480).",
        "# Textures: one per page (see 'page' records), each 256x256 + <name>_a.bmp: tex_get(name, IMG_ALPHA,",
        "# RT_CLAMP | RT_NOMIP). Every font is entirely on one page; each page has its own 'white' and 'line'.",
        "# Load the colour BMP and <name>_a.bmp with image flags 0 and combine them yourself (IMG_ALPHA's loader fills",
        "# transparent texels with neighbour colour, which breaks premultiplied texels; see probe.cpp atlas_load).",
        "# Texels are PREMULTIPLIED and the DROP SHADOW IS BAKED: every glyph/sprite cell = white shape (RGB = alpha",
        "# = coverage) over a black shadow offset (+o, +o) px (o = 'set' field shadowOff) at shadowA100 % alpha",
        "# (RGB 0, alpha only). Draw each glyph/sprite as ONE quad with RB_PREMUL (ONE, INVSRCALPHA), vertex colour",
        "# = HUD tint: it tints the shape and the shadow stays black; vertex alpha fades shape + shadow together",
        "# (scale vertex RGB by the same alpha). NO separate shadow pass. 'white' (solid, no shadow) is for panels /",
        "# boxes; 'line' (no shadow) and 'line_sh' (stroke + its shadow below) are the textured-line profiles.",
        "# Coordinates: texels, origin top-left (u = x / 256, v = y / 256). 1 texel = 1 screen px for this set:",
        "# draw unscaled at whole pixel positions (D3D TL vertices: subtract 0.5 from x and y) so the hinted stems",
        "# stay crisp.",
        "# Records (fields separated by single spaces, integers; '#' lines are comments; ignore unknown records and",
        "# extra trailing fields):",
        "#   set <lines> <pages> <pageW> <pageH> <scale1000> <stroke> <mapStroke10> <frameLine> <shadowOff>",
        "#       <shadowA100>",
        "#        scale1000 = lines / 480 x 1000. stroke = symbol / ui_line stroke px for this set (use it for the",
        "#        ladder, tape, boxes, circles). mapStroke10 = minimap symbol stroke x10. frameLine = minimap frame px.",
        "#   page <n> <texture name>",
        "#   font <f> <cap> <cellH> <oy> <adv> <spaceAdv> <gap> <inkW> <stroke10> <baseline> <page>",
        "#        f 0 = small, 1 = large. cap = cap height px: a capital's ink covers exactly the rows",
        "#        [capTop, capTop + cap) (plus a faint ~1 px soft edge). baseline = cap = baseline offset below the cap",
        "#        top (no descenders in this font). cellH = height of every glyph cell; oy = cell top relative to the",
        "#        cap top. adv = advance of every digit, capital and most symbols (MONOSPACED); narrow glyphs . : '",
        "#        have their own adv. gap = empty px inside adv after the ink box; inkW = ink box width of a full",
        "#        glyph. stroke10 = stroke x10. page = the page holding all glyphs of this font.",
        "#   g <f> <code> <x> <y> <w> <h> <ox> <adv>",
        "#        code = ASCII code (decimal). Cell x y w h on the font's page. For a string with its cap top at Y and",
        "#        pen at X: draw the cell at (X + ox, Y + oy), then X += adv. w = 0: nothing to draw (space).",
        "#        Missing characters: use '?'; lowercase other than 'c' is not in the font: draw it as capitals.",
        "#   <name> <x> <y> <w> <h> <ox> <oy> <page>",
        "#        sprite: rect on that page and its ANCHOR (ox, oy) = texel-edge point inside the rect (from the rect's",
        "#        top-left corner). Draw the rect with its top-left at (Ax - ox, Ay - oy) for an anchor at screen",
        "#        point (Ax, Ay) (a whole-pixel edge point: round the symbol centre, DESIGN 10.7.5). Rotated sprites",
        "#        rotate about the anchor (rotating also turns the baked shadow: fine for small angles; the rotated",
        "#        pointers/arrows are tiny). 'white', 'line', 'line_sh' appear once per page (use the bound page's).",
        "# Cells include the shadow: every glyph cell and sprite rect extends shadowOff px further right and down",
        "# than the shape; cellH includes it too. Advances, ox/oy, anchors, cap and inkW describe the SHAPE only:",
        "# layout, alignment and centring ignore the shadow.",
        "# Exact centring of a number in a box (speed/altitude boxes): ink width W = sum(adv) - gap, ink height =",
        "# cap. For a box whose inside spans [x0, x1) x [y0, y1) in whole px: X = x0 + (x1 - x0 - W) / 2,",
        "# Y = y0 + (y1 - y0 - cap) / 2 (integer division; centring is exact when the leftover is even, else the",
        "# extra px goes right/down). Right-aligned digit columns stay put because every digit has the same adv.",
        "# Glyph cell grid (design): cap top y = 0, baseline y = 10, glyph box x = 0..6, stroke centre lines.",
        "#   px x = g/2 + round(x * (inkW - g) / 6), px y = g/2 + round(y * (cap - g) / 10), g = hinting stem width",
        "#   (1 for the 480 small font: centres on pixel centres, 1.5 px strokes; else = stroke).",
        "# Character set: 0-9 A-Z . : ' - + / % ( ) ? c and '@' (code 64) = the AOA alpha symbol; space.",
    ]
    L.append("set %d %d %d %d %d %d %d %d %d %d" % (S.lines, info["pages"], PAGE_W, PAGE_H, rnd(k * 1000), S.ssw,
                                                    rnd(S.msw * 10), S.flw, info["shadow"], rnd(SHADOW_A * 100)))
    for p in range(info["pages"]):
        L.append("page %d %s" % (p, tex_names[p]))
    for f in S.fonts:
        L.append("font %d %d %d %d %d %d %d %d %d %d %d" % (f.fid, f.cap, f.cell_h + info["shadow"], f.oy, f.adv, f.adv, f.gap,
                                                            f.inkw, rnd(f.sw * 10), f.cap,
                                                            info["gpage"]["font%d" % f.fid]))
    for f in S.fonts:
        L.append("# %s font: cap %d px, stroke %.1f px, advance %d" % (f.name, f.cap, f.sw, f.adv))
        for ch in FONT_CHARS:
            key, ox, adv = info["glyphs"][(f.fid, ch)]
            if key is None:
                L.append("g %d %d 0 0 0 %d 0 %d" % (f.fid, ord(ch), f.cell_h, adv))
            else:
                p, x, y, w, h = entry(info, key)
                L.append("g %d %d %d %d %d %d %d %d" % (f.fid, ord(ch), x, y, w, h, ox, adv))
    L.append("# sprites")
    for p in range(info["pages"]):
        x, y, w, h = info["rects"][("white", p)]
        L.append("# " + SPRITE_NOTES["white"])
        L.append("white %d %d 2 2 1 1 %d" % (x + 1, y + 1, p))
        x, y, w, h = info["rects"][("line", p)]
        L.append("# " + SPRITE_NOTES["line"])
        L.append("line %d %d %d %d 2 %d %d" % (x, y, w, h, h // 2, p))
        x, y, w, h = info["rects"][("line_sh", p)]
        L.append("# " + SPRITE_NOTES["line_sh"])
        L.append("line_sh %d %d %d %d 2 %d %d" % (x, y, w, h, (h - info["shadow"]) // 2, p))
    for name in [d[0] for d in sym_defs(S)] + ["map_frame"]:
        p, x, y, w, h = entry(info, name)
        ox, oy = info["anchors"][name]
        L.append("# " + SPRITE_NOTES.get(name, name))
        L.append("%s %d %d %d %d %d %d %d" % (name, x, y, w, h, ox, oy, p))
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------------------------------- preview compositor
class Frame(object):
    """Mimics the engine for one atlas set: background RGB, translucent panels (DIM), then every glyph/sprite as one
    RB_PREMUL quad with its baked shadow, lines as line_sh quads. Order-independent approximation: all shadow
    alpha (SHL layer) darkens first, then each colour layer is composited over (ONE, INVSRCALPHA). Atlas texels are
    quantised to 4 bits (ARGB4444) as in VRAM."""

    def __init__(self, bg, S, info, pages):
        self.bg = bg.convert("RGB")
        self.w, self.h = self.bg.size
        self.S = S
        self.info = info
        q = lambda v: (rnd(v / 255.0 * 15.0) * 255 + 7) // 15
        self.tex_c = [pg.split()[0].point(q) for pg in pages]
        self.tex_a = [pg.split()[3].point(q) for pg in pages]
        self.layers = {}
        self.clip = None

    def layer(self, col):
        if col not in self.layers:
            self.layers[col] = Image.new("L", (self.w, self.h), 0)
        return self.layers[col]

    def add_img(self, cov, X, Y, col, alpha=1.0):
        """Add an 'L' coverage image with its top-left at pixel (X, Y), clipped to the frame and self.clip."""
        if alpha != 1.0:
            cov = cov.point(lambda v: rnd(v * alpha))
        x0, y0, x1, y1 = 0, 0, self.w, self.h
        if self.clip:
            c = self.clip
            x0, y0, x1, y1 = max(x0, c[0]), max(y0, c[1]), min(x1, c[2]), min(y1, c[3])
        w, h = cov.size
        bx0, by0 = max(X, x0), max(Y, y0)
        bx1, by1 = min(X + w, x1), min(Y + h, y1)
        if bx1 <= bx0 or by1 <= by0:
            return
        part = cov.crop((bx0 - X, by0 - Y, bx1 - X, by1 - Y))
        lay = self.layer(col)
        box = (bx0, by0, bx1, by1)
        lay.paste(ImageChops.add(lay.crop(box), part), box)

    def quad(self, p, rect, X, Y, col, alpha=1.0, rot=0.0, pivot=(0, 0)):
        """One textured quad: shape coverage into the colour layer, shadow alpha (A - C) into SHL."""
        x, y, w, h = rect
        c = self.tex_c[p].crop((x, y, x + w, y + h))
        a = self.tex_a[p].crop((x, y, x + w, y + h))
        if rot:
            m = max(w, h)
            out = []
            for im in (c, a):
                big = Image.new("L", (w + 2 * m, h + 2 * m), 0)
                big.paste(im, (m, m))
                out.append(big.rotate(-rot, resample=Image.BILINEAR, center=(m + pivot[0], m + pivot[1])))
            c, a = out
            bb = a.getbbox()
            if not bb:
                return
            c, a = c.crop(bb), a.crop(bb)
            X, Y = X - m + bb[0], Y - m + bb[1]
        self.add_img(c, X, Y, col, alpha)
        self.add_img(ImageChops.subtract(a, c), X, Y, SHL, alpha)

    def sprite(self, name, ax, ay, col, alpha=1.0, rot=0.0):
        p, x, y, w, h = entry(self.info, name)
        ox, oy = self.info["anchors"][name]
        self.quad(p, (x, y, w, h), rnd(ax) - ox, rnd(ay) - oy, col, alpha, rot, (ox, oy))

    def font(self, fid):
        return self.S.fonts[fid]

    def text_width(self, fid, s):
        f = self.font(fid)
        return sum(f.glyph_adv(ch if ch in GLYPHS else "?") for ch in s) - f.gap

    def text(self, fid, s, X, Y, col, align=0, alpha=1.0):
        """Cap top at Y; align 0 = X is the left ink edge, 1 = centre, 2 = right ink edge."""
        f = self.font(fid)
        w = self.text_width(fid, s)
        pen = X if align == 0 else (X - w / 2.0 if align == 1 else X - w)
        pen = rnd(pen)
        for ch in s:
            if ch not in GLYPHS:
                ch = ch.upper() if ch.upper() in GLYPHS else "?"
            key, ox, adv = self.info["glyphs"][(fid, ch)]
            if key:
                p, x, y, gw, gh = entry(self.info, key)
                self.quad(p, (x, y, gw, gh), pen + ox, rnd(Y) + f.oy, col, alpha)
            pen += adv

    def text_in_box(self, fid, s, x0, y0, x1, y1, col):
        """Centre a string exactly in the box interior [x0, x1) x [y0, y1) (the txt header recipe)."""
        f = self.font(fid)
        w = self.text_width(fid, s)
        X = x0 + (x1 - x0 - w) // 2
        Y = y0 + (y1 - y0 - f.cap) // 2
        self.text(fid, s, X, Y, col)

    def lines(self, segs, col, hw=None, alpha=1.0, cap="butt"):
        """Sub-pixel strokes as line_sh quads would draw them: stroke + SHADOW_A shadow offset (+o, +o)."""
        if hw is None:
            hw = self.S.ssw / 2.0
        allp = [p for s in segs for p in s]
        if not allp:
            return
        x0 = max(fl(min(p[0] for p in allp) - hw - 2), -4)
        y0 = max(fl(min(p[1] for p in allp) - hw - 2), -4)
        x1 = min(int(math.ceil(max(p[0] for p in allp) + hw + 2)), self.w + 4)
        y1 = min(int(math.ceil(max(p[1] for p in allp) + hw + 2)), self.h + 4)
        if x1 <= x0 or y1 <= y0:
            return
        cv = Canvas(x1 - x0, y1 - y0, -x0, -y0)
        for s in segs:
            cv.polyline(s, hw, closed=False, cap=cap)
        cov = cv.coverage()
        o = self.info["shadow"]
        self.add_img(cov, x0, y0, col, alpha)
        self.add_img(cov, x0 + o, y0 + o, SHL, alpha * SHADOW_A)

    def box(self, x0, y0, x1, y1, col):
        """Rectangle outline whose stroke lies INSIDE [x0, x1) x [y0, y1); returns the interior box."""
        s = self.S.ssw
        h = s / 2.0
        self.lines([[(x0 + h, y0 + h), (x1 - h, y0 + h), (x1 - h, y1 - h), (x0 + h, y1 - h), (x0 + h, y0)]], col)
        return x0 + s, y0 + s, x1 - s, y1 - s

    def rect_fill(self, x0, y0, x1, y1, col, alpha):
        self.add_img(Image.new("L", (x1 - x0, y1 - y0), rnd(alpha * 255)), x0, y0, col)

    def compose(self, q565=True):
        out = self.bg
        for key in (DIM, SHL):
            lay = self.layers.get(key)
            if lay:
                f = ImageChops.invert(lay)
                out = ImageChops.multiply(out, Image.merge("RGB", (f, f, f)))
        for col, lay in sorted(self.layers.items()):
            if col in (DIM, SHL):
                continue
            out = Image.composite(Image.new("RGB", (self.w, self.h), col), out, lay)
        if q565:
            out = I.to16(out, "565", dither=True)
        return out


def zoom(img, k):
    return img.resize((img.size[0] * k, img.size[1] * k), Image.NEAREST)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


# ---------------------------------------------------------------------------------------------------- backgrounds
SKY_TOP = (58, 104, 176)
SKY_HOR = (178, 202, 226)
SEA_NEAR = (22, 52, 82)
SEA_FAR = (84, 120, 150)
CLOUD = (236, 239, 243)
LAND = (78, 112, 52)
LAND_DRY = (128, 124, 78)
ROCK = (92, 84, 76)


def scene_bg(w, h, horizon_y, bank_deg, seed):
    """Synthetic flight background: sky gradient, a cumulus bank, sea, island with a volcano. Deterministic."""
    nz = noise_grid(80, 60, cells=6, octaves=4, gain=0.55, seed=seed, tile=False)
    cl = noise_grid(80, 60, cells=4, octaves=4, gain=0.5, seed=seed + 1, tile=False)

    def samp(grid, u, v):
        gx = max(0.0, min(79.0, u * 79.0))
        gy = max(0.0, min(59.0, v * 59.0))
        x0, y0 = int(gx), int(gy)
        x1, y1 = min(79, x0 + 1), min(59, y0 + 1)
        fx, fy = gx - x0, gy - y0
        a = grid[y0][x0] * (1 - fx) + grid[y0][x1] * fx
        b = grid[y1][x0] * (1 - fx) + grid[y1][x1] * fx
        return a * (1 - fy) + b * fy
    ca, sa = math.cos(math.radians(bank_deg)), math.sin(math.radians(bank_deg))
    img = Image.new("RGB", (w, h))
    px = img.load()
    cx = w / 2.0
    for y in range(h):
        for x in range(w):
            # rotate into the horizon frame: v = height above the horizon (px), u = along
            dx, dy = x - cx, y - horizon_y
            u = dx * ca - dy * sa
            v = -(dx * sa + dy * ca)
            n = samp(nz, x / float(w), y / float(h))
            if v >= 0:
                t = min(1.0, v / 260.0)
                c = lerp(SKY_HOR, SKY_TOP, t ** 0.7)
                # cumulus band 20..120 px above the horizon, patchy
                cv = samp(cl, x / float(w), y / float(h))
                band = math.exp(-((v - 70.0) / 55.0) ** 2)
                d = (cv - 0.45) * 4.0 * band
                if d > 0:
                    k = min(1.0, d)
                    shade = 0.82 + 0.18 * min(1.0, max(0.0, (v - 40) / 60.0))
                    c = lerp(c, tuple(int(cc * shade) for cc in CLOUD), k)
                c = lerp(c, SKY_HOR, max(0.0, 1.0 - v / 6.0) * 0.5)
            else:
                dep = -v
                t = min(1.0, dep / 220.0)
                c = lerp(SEA_FAR, SEA_NEAR, t ** 0.6)
                c = tuple(max(0, min(255, int(cc * (0.92 + 0.16 * n)))) for cc in c)
            if v > -2 and v < 140:
                # island silhouette above the sea (ground rising above the horizon)
                lt = 0.0
                if -260 < u < 90:
                    edge = min(u + 260, 90 - u) / 40.0
                    lt = min(1.0, edge) * (12.0 + 8.0 * samp(nz, (u + 320) / 640.0, 0.3))
                if -150 < u < 30:
                    cone = 1.0 - abs(u + 60) / 90.0
                    lt = max(lt, 84.0 * max(0.0, cone) ** 1.2)
                if v < lt:
                    t = v / max(1.0, lt)
                    base = lerp(LAND, LAND_DRY, min(1.0, max(0.0, (v - 18) / 40.0)))
                    if v > 50:
                        base = lerp(base, ROCK, min(1.0, (v - 50) / 25.0))
                    c = tuple(max(0, min(255, int(cc * (0.8 + 0.35 * n)))) for cc in base)
                    c = lerp(c, SKY_HOR, 0.35 * (1.0 - t * 0.5))
            px[x, y] = c
    return img


def bands(w, band, seed):
    """Four horizontal background bands of height `band`: sky, sea, cloud-white, terrain."""
    img = Image.new("RGB", (w, 4 * band))
    px = img.load()
    nz = noise_grid(64, 64, cells=8, octaves=4, gain=0.55, seed=seed, tile=True)
    for y in range(4 * band):
        k, yy = y // band, y % band
        for x in range(w):
            n = nz[(yy // 2) % 64][(x // 2) % 64]
            if k == 0:
                c = lerp((150, 186, 228), (92, 138, 204), yy / float(max(1, band - 1)))
            elif k == 1:
                c = tuple(int(cc * (0.85 + 0.3 * n)) for cc in (30, 66, 102))
            elif k == 2:
                c = tuple(min(255, int(cc * (0.96 + 0.06 * n))) for cc in CLOUD)
            else:
                c = tuple(int(cc * (0.7 + 0.6 * n)) for cc in lerp(LAND, LAND_DRY, n))
            px[x, y] = c
    return img


def scene_bg_fast(w, h, horizon_y, bank_deg, seed, k):
    """scene_bg rendered at <= 800 px wide and resized (it is only a backdrop)."""
    if w <= 800:
        return scene_bg(w, h, horizon_y, bank_deg, seed)
    f = 2
    small = scene_bg(w // f, h // f, horizon_y / f, bank_deg, seed)
    return small.resize((w, h), Image.BILINEAR)


# ---------------------------------------------------------------------------------------------------- previews
def preview_atlas(S, pages):
    n = len(pages)
    out = Image.new("RGB", (n * 2 * (PAGE_W + 8), PAGE_H), (40, 40, 40))
    sky = I.gradient(PAGE_W, PAGE_H, (120, 168, 226), (196, 214, 236))
    dark = Image.new("RGB", (PAGE_W, PAGE_H), (12, 16, 20))
    for i, pg in enumerate(pages):
        c, a = pg.split()[0], pg.split()[3]
        for j, (bg, col) in enumerate(((dark, (255, 255, 255)), (sky, GREEN))):
            f = ImageChops.invert(a)                                   # RB_PREMUL: col * c + bg * (1 - a)
            o = ImageChops.multiply(bg, Image.merge("RGB", (f, f, f)))
            o = ImageChops.add(o, Image.merge("RGB", [c.point(lambda v, k=k: rnd(v * k / 255.0)) for k in col]))
            out.paste(o, ((i * 2 + j) * (PAGE_W + 8), 0))
    return out


def design_strings():
    """(font, text, colour) of every string in DESIGN 10.2-10.6."""
    G, Y, R = GREEN, YELLOW, RED
    return [
        (0, "350  000  010  020    5  10  -5  -10", G),     # 10.2 heading tape, ladder numbers
        (0, "350   1500   12450   8750   99999", G),       # airspeed / altitude boxes
        (0, "-1250   70   -20", G),                          # VVI
        (0, "@ 2.6   M 0.84   G 1.0   9.2", G),              # AOA / Mach / G / peak
        (0, "AIM-120 16    AIM-9 40", G),                    # weapon line
        (1, "A: TAKEOFF", G),                                # messages
        (1, "YOU HAVE CONTROL", G),
        (1, "INI RELOADED (3)   WAVE 2", G),
        (1, "PULL UP", R),                                   # warnings
        (0, "RETURN TO AREA      03:27", G),                 # warning, clock
        (0, "G 4.5   M 1.62", G),                            # 10.3 chase
        (1, "AIM-120  16", G),                               # weapon block
        (0, "FLR 40   LOCK   SRM", G),
        (0, "20  10  40   F40", G),                          # 10.4 minimap
        (0, "12.4   17   8   12.4  17", Y),                  # 10.5 labels (unknown)
        (0, "12.4  17   135", R),                            # hostile label, locator degrees
        (1, "SPLASH", G),
        (1, "SHOOT", G),                                     # 10.6
        (0, "960 Vc   19.9 RNG   23 TOF", G),
        (0, "17ACT   11TTG   6TTG", G),
        (0, "VISUAL   AIM-120 16", G),
        (1, "MISSILE", R),
        (0, "LOCK", R),
    ]


def flow(fr, rows, x0, y0, width):
    """Lay strings out left to right, wrapping at width. -> y after the last line."""
    x, y, line_h = x0, y0, 0
    sp = rnd(18 * fr.S.k)
    for f, s, col in rows:
        w = fr.text_width(f, s)
        lh = fr.font(f).cap + rnd(6 * fr.S.k)
        if x > x0 and x + w > x0 + width:
            x, y = x0, y + line_h
            line_h = 0
        fr.text(f, s, x, y + 2, col)
        x += w + sp
        line_h = max(line_h, lh)
    return y + line_h


def preview_readability(S, info, pages):
    """Every DESIGN 10.2-10.6 string 1:1 for this set on four bands: sky, sea, cloud-white, terrain."""
    rows = design_strings()
    w = rnd(640 * S.k)
    probe = Frame(Image.new("RGB", (w, 8)), S, info, pages)
    band = flow(probe, rows, 6, 4, w - 12) + 6
    fr = Frame(bands(w, band, SEED + 11), S, info, pages)
    for k in range(4):
        flow(fr, rows, 6, k * band + 4, w - 12)
    return fr.compose(q565=S.lines <= 600)


# ---- mock HUDs ----
PXDEG = 8.8              # px per degree at 480 lines (65 deg hFOV, HUD.md 1.1)


def mock_cockpit(S, info, pages):
    k = S.k
    W, H = rnd(640 * k), rnd(480 * k)

    def v(a):
        return a * k
    pitch, bank, aoa = 4.0, 12.0, 3.0
    pxd = PXDEG * k
    cx, cy = v(320), v(240)
    bg = scene_bg_fast(W, H, cy + pitch * pxd, bank, SEED, k)
    fr = Frame(bg, S, info, pages)
    G = GREEN
    Sm, Lg = 0, 1
    fs = fr.font(Sm)
    clip = (rnd(v(170)), rnd(v(116)), rnd(v(470)), rnd(v(384)))
    vvx, vvy = cx + v(6), cy + aoa * pxd
    ca, sa = math.cos(math.radians(bank)), math.sin(math.radians(bank))
    fpa = pitch - aoa

    def rot(lx, ly):
        return (vvx + lx * ca + ly * sa, vvy - lx * sa + ly * ca)
    fr.clip = clip
    for deg in range(-15, 20, 5):
        ly = (fpa - deg) * pxd
        if abs(ly) > v(150):
            continue
        if deg == 0:
            fr.lines([[rot(v(-110), ly), rot(v(-12), ly)], [rot(v(12), ly), rot(v(110), ly)]], G)
            continue
        gap, ln = v(12), v(40)
        hk = v(5) if deg > 0 else -v(5)
        segs = []
        if deg > 0:
            segs = [[rot(-gap - ln, ly + hk), rot(-gap - ln, ly), rot(-gap, ly)],
                    [rot(gap, ly), rot(gap + ln, ly), rot(gap + ln, ly + hk)]]
        else:
            for side in (-1, 1):
                for i in range(5):
                    a0 = gap + i * v(9)
                    a1 = min(gap + ln, a0 + v(6))
                    segs.append([rot(side * a0, ly), rot(side * a1, ly)])
                segs.append([rot(side * (gap + ln), ly), rot(side * (gap + ln), ly + hk)])
        fr.lines(segs, G)
        for sx in (-1, 1):
            px_, py_ = rot(sx * (gap + ln + v(12)), ly)
            fr.text(Sm, "%d" % deg, px_, py_ - fs.cap / 2.0, G, align=1)
    fr.sprite("vv", vvx, vvy, G)
    # launch zone circle on the waterline (r 66), ticks, range pointer, steering dot
    R = v(66)
    fr.lines([arc_pts(cx, cy, R, R, -90, 270, 3)], G)
    rscale = 20.0
    for rng_nm, kind in ((14.0, "max"), (7.5, "ne"), (1.2, "min")):
        a = math.radians(-90 + 360.0 * rng_nm / rscale)
        ux, uy = math.cos(a), math.sin(a)
        if kind == "max":
            fr.lines([[(cx + ux * R, cy + uy * R), (cx + ux * (R + v(8)), cy + uy * (R + v(8)))]], G)
        elif kind == "ne":
            for off in (-v(2), v(2)):
                ox_, oy_ = -uy * off, ux * off
                fr.lines([[(cx + ux * R + ox_, cy + uy * R + oy_),
                           (cx + ux * (R + v(6)) + ox_, cy + uy * (R + v(6)) + oy_)]], G)
        else:
            fr.lines([[(cx + ux * R, cy + uy * R), (cx + ux * (R - v(6)), cy + uy * (R - v(6)))]], G)
    a = -90 + 360.0 * 11.2 / rscale
    fr.sprite("ptr_tri", cx + math.cos(math.radians(a)) * (R + 1), cy + math.sin(math.radians(a)) * (R + 1), G,
              rot=a + 90)
    fr.sprite("dot4", cx - v(16), cy + v(20), G)
    for i, s in enumerate(("960 Vc", "11.2 RNG", "23 TOF")):
        fr.text(Sm, s, v(396), v(290 + 14 * i), G)
    # contacts (A4b): hollow diamond markers; the designated one has the TD square + SHOOT; labels below
    tx, ty = cx + v(18), cy - v(26)
    fr.sprite("tgt_diamond", tx, ty, RED)
    fr.sprite("tgt_box", tx, ty, G)
    fr.text(Lg, "SHOOT", tx, ty - v(10) - v(14) - fr.font(Lg).cap, G, align=1)
    fr.text(Sm, "11.2  14", tx, ty + v(14), RED, align=1)
    fr.sprite("tgt_diamond", v(250), v(196), YELLOW)
    fr.sprite("sel_tri", v(250), v(196) - v(5) - v(6), G)
    fr.text(Sm, "18.5", v(250), v(196) + v(9), YELLOW, align=1)
    fr.sprite("tgt_diamond", v(420), v(186), RED)
    fr.text(Sm, "26", v(420), v(186) + v(9), RED, align=1)
    fr.sprite("tgt_diamond_dash", v(398), v(160), YELLOW, 0.5)
    fr.clip = None
    fr.sprite("waterline", cx, cy, G)
    # heading tape (320,132), 240 px wide, 8 px/deg, labels every 10 deg
    hdg = 47.0
    fr.clip = (rnd(v(200)), rnd(v(100)), rnd(v(440)), rnd(v(160)))
    tape = []
    for d in range(int(hdg) - 20, int(hdg) + 21):
        if d % 5:
            continue
        x = cx + (d - hdg) * v(8)
        tape.append([(x, v(132)), (x, v(132) - (v(6) if d % 10 == 0 else v(3)))])
        if d % 10 == 0:
            fr.text(Sm, "%03d" % (d % 360), x, v(132 - 9) - fs.cap, G, align=1)
    fr.lines(tape, G)
    fr.clip = None
    fr.sprite("caret", cx, v(135), G)
    # airspeed / altitude boxes, numbers centred (A4b)
    for (x0, x1, s) in ((206, 254, "350"), (386, 444, "12450")):
        inner = fr.box(rnd(v(x0)), rnd(v(220)), rnd(v(x1)), rnd(v(236)), G)
        fr.text_in_box(Sm, s, *(inner + (G,)))
    fr.text(Sm, "1250", v(444), v(206), G, align=2)
    for i, s in enumerate(("@ 3.0", "M 0.84", "G 1.0", "9.2")):
        fr.text(Sm, s, v(196), v(262 + 14 * i), G)
    tk = []
    for d in (-45, -30, -15, -5, 5, 15, 30, 45):
        a = math.radians(90 + d)
        ln = v(8 if abs(d) in (15, 45) else (11 if abs(d) == 30 else 5))
        tk.append([(cx + math.cos(a) * v(112), cy + math.sin(a) * v(112)),
                   (cx + math.cos(a) * (v(112) + ln), cy + math.sin(a) * (v(112) + ln))])
    fr.lines(tk, G)
    a = math.radians(90 + bank)
    fr.sprite("ptr_tri", cx + math.cos(a) * v(111), cy + math.sin(a) * v(111), G, rot=bank)
    fr.text(Sm, "AIM-120 16", cx, v(364), G, align=1)
    fr.text(Sm, "03:27", v(178), v(368), G)
    mock_minimap(fr)
    return fr.compose(q565=S.lines <= 600)


def mock_minimap(fr, heading=47.0):
    k = fr.S.k
    mx, my, n = rnd(6 * k), rnd(338 * k), rnd(136 * k)
    fr.rect_fill(mx, my, mx + n, my + n, DIM, 0.376)
    cx, cy = mx + n / 2.0, my + n / 2.0
    isl = []
    for i in range(24):
        a = math.radians(i * 15)
        r = (22 + 6 * math.sin(3 * a) + 3 * math.cos(5 * a)) * k
        isl.append((-20 * k + r * math.cos(a), 26 * k + r * 0.8 * math.sin(a)))
    ca, sa = math.cos(math.radians(-heading)), math.sin(math.radians(-heading))
    isl = [(cx + x * ca - y * sa, cy + x * sa + y * ca) for x, y in isl]
    cv = Canvas(n, n, -mx, -my)
    cv.polygon(isl)
    fr.add_img(cv.coverage(), mx, my, (40, 80, 30))
    cv = Canvas(n, n, -mx, -my)
    cv.polygon([(cx, cy)] + arc_pts(cx, cy, 68 * k, 68 * k, -150, -30, 6))
    fr.add_img(cv.coverage(), mx, my, GREEN, 0.15)
    p, x, y, w, h = entry(fr.info, "map_frame")
    o = fr.info["shadow"]
    tc = fr.tex_c[p].crop((x, y, x + w, y + h))
    ta = fr.tex_a[p].crop((x, y, x + w, y + h))
    c = (w - o) // 3                       # corner; right/bottom slices are c + o (they carry the shadow)
    xs = [(0, c, mx, c), (c, w - c - o, mx + c, n - 2 * c), (w - c - o, w, mx + n - c, c + o)]
    ys = [(0, c, my, c), (c, h - c - o, my + c, n - 2 * c), (h - c - o, h, my + n - c, c + o)]
    for i, (sx0, sx1, dx, dw) in enumerate(xs):
        for j, (sy0, sy1, dy, dh) in enumerate(ys):
            if i == 1 and j == 1:
                continue
            for img, col in ((tc, GREEN), (ImageChops.subtract(ta, tc), SHL)):
                piece = img.crop((sx0, sy0, sx1, sy1)).resize((dw, dh), Image.NEAREST)
                fr.add_img(piece, dx, dy, col)
    fr.text(0, "20", mx + rnd(5 * k), my + rnd(5 * k), GREEN)
    fr.text(0, "F40", mx + n - rnd(5 * k), my + rnd(5 * k), GREEN, align=2)
    fr.sprite("map_jet", cx, cy, GREEN)
    for (ux, uy, col, nm) in ((10, -40, RED, "map_chev"), (-30, -22, YELLOW, "map_arch"),
                              (36, -50, RED, "map_chev"), (24, 20, YELLOW, "map_arch_dash")):
        fr.sprite(nm, cx + ux * k, cy + uy * k, col)
    fr.lines([[(cx + 10 * k, cy - 40 * k), (cx + 6 * k, cy - 35.5 * k)]], RED, hw=fr.S.msw / 2.0)
    fr.sprite("dot", cx + 4 * k, cy - 18 * k, GREEN)
    hwm = fr.S.msw / 2.0
    fr.lines([[(cx + 5 * k, cy - 45 * k), (cx + 15 * k, cy - 45 * k), (cx + 15 * k, cy - 35 * k),
               (cx + 5 * k, cy - 35 * k), (cx + 5 * k, cy - 45 * k)]], GREEN, hw=hwm)


def mock_chase(S, info, pages):
    k = S.k
    W, H = rnd(640 * k), rnd(480 * k)

    def v(a):
        return a * k
    bank = -25.0
    bg = scene_bg_fast(W, H, v(268), bank, SEED + 3, k)
    jet = [(v(320), v(270)), (v(360), v(292)), (v(352), v(297)), (v(320), v(290)), (v(288), v(297)),
           (v(280), v(292))]
    cv = Canvas(W, H, 0, 0)
    cv.polygon(jet)
    bg = Image.composite(Image.new("RGB", (W, H), (70, 74, 80)), bg, cv.coverage())
    fr = Frame(bg, S, info, pages)
    G, Sm, Lg = GREEN, 0, 1
    ca, sa = math.cos(math.radians(bank)), math.sin(math.radians(bank))
    vvx, vvy = v(322), v(226)

    def rot(lx, ly):
        return (vvx + lx * ca + ly * sa, vvy - lx * sa + ly * ca)
    ly0 = 4 * PXDEG * k
    fr.lines([[rot(v(-110), ly0), rot(v(-12), ly0)], [rot(v(12), ly0), rot(v(110), ly0)]], G, alpha=0.5)
    for deg in (10, -10):
        ly = ly0 - deg * PXDEG * k
        hk = v(5) if deg > 0 else -v(5)
        fr.lines([[rot(v(-52), ly + hk), rot(v(-52), ly), rot(v(-12), ly)],
                  [rot(v(12), ly), rot(v(52), ly), rot(v(52), ly + hk)]], G, alpha=0.5)
    fr.sprite("aim_cross", v(320), v(214), G)
    fr.sprite("vv", vvx, vvy, G)
    for (x0, x1, s) in ((150, 198, "512"), (442, 500, "8750")):
        inner = fr.box(rnd(v(x0)), rnd(v(220)), rnd(v(x1)), rnd(v(236)), G)
        fr.text_in_box(Sm, s, *(inner + (G,)))
    fr.text(Sm, "G 4.5", v(150), v(252), G)
    fr.text(Sm, "AIM-9", v(620), v(410), G, align=2)
    fr.text(Lg, "38", v(620), v(424), G, align=2)
    fr.text(Sm, "FLR 40", v(620), v(442), G, align=2)
    # designated hostile with SRM seeker locked + chase DLZ bar
    tx, ty = v(470), v(170)
    fr.sprite("tgt_diamond", tx, ty, RED)
    fr.sprite("tgt_box", tx, ty, G)
    fr.sprite("seeker", tx, ty, G)
    fr.text(Lg, "SHOOT", tx, ty - v(30) - fr.font(Lg).cap, G, align=1)
    fr.text(Sm, "3.8  9", tx, ty + v(30), RED, align=1)
    bx, by = tx + v(36), ty - v(25)
    fr.lines([[(bx, by), (bx, by + v(50))]], G)
    for yy, ln in ((by + v(6), v(6)), (by + v(20), v(4)), (by + v(44), v(6))):
        fr.lines([[(bx, yy), (bx + ln, yy)]], G)
    fr.sprite("offscreen_tri", bx - v(2), by + v(30), G)
    fr.sprite("tgt_diamond", v(236), v(132), YELLOW)
    fr.text(Sm, "14.0", v(236), v(141), YELLOW, align=1)
    fr.sprite("tgt_diamond", v(400), v(250), RED)
    fr.text(Sm, "6.1", v(400), v(259), RED, align=1)
    fr.sprite("offscreen_tri", v(20), v(120), YELLOW, rot=200)
    fr.sprite("offscreen_tri", v(300), v(20), RED, rot=-90)
    fr.text(Lg, "MISSILE", v(320), v(180), RED, align=1)
    for ang in (140, 215):
        a = math.radians(ang)
        fr.sprite("threat_arrow", v(320) + math.cos(a) * v(140), v(240) + math.sin(a) * v(140), RED, rot=ang)
    fr.text(Sm, "LOCK", v(320), v(198), RED, align=1)
    mock_minimap(fr, heading=300.0)
    return fr.compose(q565=S.lines <= 600)


# ---------------------------------------------------------------------------------------------------- build
def build(out_dir, kit):
    results = {}
    for S in SETS:
        pages, info = build_set(S)
        if info["pages"] != SET_PAGES[S.lines]:
            raise ValueError("set %d packs into %d pages, SET_PAGES says %d: update SET_PAGES" % (
                S.lines, info["pages"], SET_PAGES[S.lines]))
        names = [S.page_tex(p) for p in range(info["pages"])]
        for p, pg in enumerate(pages):
            kit.save_bmp("tex/%s.bmp" % names[p], pg, alpha=True, alpha_bleed=0)
        kit.save_text("tex/%s.txt" % S.tex, set_txt(S, info, names), encoding="ascii", crlf=True)
        kit.log("set %4d: %d page(s), small cap %d adv %d, large cap %d adv %d, stroke %d, %d%% of %d page texels" % (
            S.lines, info["pages"], S.fonts[0].cap, S.fonts[0].adv, S.fonts[1].cap, S.fonts[1].adv, S.ssw,
            100 * info["used"] // (PAGE_W * PAGE_H * info["pages"]), info["pages"]))
        results[S.lines] = (info, pages)
        if S.lines == 480:          # legacy names = exact copy of the 480 set
            kit.save_bmp("tex/hud_atlas.bmp", pages[0], alpha=True, alpha_bleed=0)
            kit.save_text("tex/hud_atlas.txt", set_txt(S, info, ["hud_atlas"]), encoding="ascii", crlf=True)
    if kit.previews:
        for S in SETS:
            info, pages = results[S.lines]
            kit.save_preview("hud_atlas_%d.png" % S.lines, preview_atlas(S, pages))
            kit.save_preview("hud_readability_%d.png" % S.lines, preview_readability(S, info, pages))
        kit.save_preview("hud_mock_cockpit_480.png", mock_cockpit(SET_BY_LINES[480], *results[480]))
        S = SET_BY_LINES[600]
        mc = mock_cockpit(S, *results[600])
        kit.save_preview("hud_mock_cockpit_600.png", mc)
        kit.save_preview("hud_mock_cockpit_600_zoom.png", zoom(mc.crop((200, 125, 600, 475)), 2))
        kit.save_preview("hud_mock_chase_600.png", mock_chase(S, *results[600]))
        S = SET_BY_LINES[1050]
        mc = mock_cockpit(S, *results[1050])
        kit.save_preview("hud_mock_cockpit_1050.png", mc)
        kit.save_preview("hud_mock_cockpit_1050_crop.png", mc.crop((350, 262, 1050, 787)))
        kit.save_preview("hud_mock_chase_1050.png", mock_chase(S, *results[1050]))


if __name__ == "__main__":
    from assetkit import build as B
    sys.exit(B.main_for(__file__))
