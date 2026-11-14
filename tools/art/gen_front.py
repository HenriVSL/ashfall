"""gen_front.py - A10 front-end art: title logo, menu panel kit, program icon (DESIGN 14.5, 10.1; TASKS A10).

"Ashfall: Island Intercept". Everything is premultiplied for RB_PREMUL (ONE, INVSRCALPHA), like the HUD (A4c)
and the fire fx (A8). Text in menus uses the existing HUD stroke fonts (hud_atlas_<L>); no new font.

Outputs (data/):
  tex/logo_lo_<i>.bmp + _a   logo tiles, native at 600 lines (800x600); use for screen heights < 700
  tex/logo_hi_<i>.bmp + _a   logo tiles, native at 1050 lines (1400x1050); use for screen heights >= 700
  tex/logo.bmp + _a          the whole logo in ONE 256x64 texture (pause / records headers, loading)
  tex/logo.txt               tile layout + placement (CRLF; format in its header)
  tex/front_<L>.bmp + _a     menu kit atlas per screen-height set L = 480 600 768 1050 (one 256x256 page each):
                             9-slice panel, title strip, selection bar, key caps, tabs, arrows, marker, check
                             boxes, volume segments, divider, white
  tex/front_<L>.txt          sprite / slice records (CRLF; format in its header, HUD-atlas compatible)
  ashfall.ico                program icon: 32x32 + 16x16, 16-colour and 256-colour (Win98)

Logo: "ASHFALL" in own chamfered extended italic letters (no system font), basalt-steel face (sky-reflection steel
above a chrome horizon, dark basalt below) with glowing lava cracks rising from the baseline, a chisel bevel lit
cool from above and warm (lava) from below, dark outline, soft drop shadow and an ember glow; "ISLAND INTERCEPT"
in silver beneath, flanked by ember streaks. Rendered natively per set at 2x and box-filtered (crisp at 1:1).

Previews (tools/art/preview/front_*.png): title at 640x480, 800x600, 1024x768, 1400x1050 over an in-engine island
shot, options (controls + video/sound), results, pause, the atlas pages, logo tiles and the icon; composited the
engine way (textures quantised to 4444 like r_d3d6.cpp, RB_PREMUL, frame to 565 with dither).

Run: python3 tools/build_assets.py --only front   (or python3 tools/art/gen_front.py [--check]).
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "tools"))
from PIL import Image, ImageChops, ImageDraw, ImageFilter  # noqa: E402

from assetkit import image as I  # noqa: E402
from assetkit.rng import Rng, noise_grid  # noqa: E402

SEED = 1010
ORDER = 70                  # after gen_hud (40): previews read the HUD font atlases
WS = 2                      # logo work scale (rendered at 2x, box-filtered down)
SS = 4                      # polygon supersampling on top of the work scale

# ---------------------------------------------------------------------------------------------------- constants
LOGO_W, LOGO_H = 460, 100   # logo canvas, virtual 640x480 units
LOGO_X, LOGO_Y = 90, 30     # title placement (top-left of the canvas)
LOGO_SETS = (("lo", 600), ("hi", 1050))
LOGO_HI_MIN_H = 700         # screen height from which the hi set is used
SMALL_TEX = (256, 64)       # tex/logo.bmp
SMALL_S = 256.0 / LOGO_W    # px per virtual unit of the small logo

MAIN_TEXT, MAIN_TOP, MAIN_H, MAIN_W0, MAIN_T, MAIN_TRACK = "ASHFALL", 8.0, 62.0, 59.0, 15.5, 6.5
SUB_TEXT, SUB_TOP, SUB_H, SUB_W0, SUB_T, SUB_TRACK = "ISLAND INTERCEPT", 79.0, 12.0, 12.5, 3.3, 4.6
SHEAR = 0.16                # italic: x += (baseline - y) * SHEAR

UI_SETS = (480, 600, 768, 1050)
STROKE = {480: 2, 600: 2, 768: 3, 1050: 4}       # = HUD set stroke
SHOFF = {480: 1, 600: 1, 768: 2, 1050: 2}        # = HUD baked shadow offset
SHADOW_A = 0.6

GREEN = (64, 255, 96)       # HUD green 0xFF40FF60
EMBER = (255, 140, 40)      # logo ember accent 0xFFFF8C28
EMBER_HOT = (255, 200, 96)
WHITE = (240, 244, 240)
GREY = (120, 136, 128)
RED = (255, 48, 48)
PANEL_FILL = (0, 20, 20, 204)   # premultiplied (0, 16, 16, 204): exact in 4444


def pow2ceil(n):
    p = 8
    while p < n:
        p *= 2
    return p


def split_axis(n):
    """[(offset, content, texSize)]: 254 content texels per 256 tile (1 texel apron on each side)."""
    out, x = [], 0
    while x < n:
        c = min(254, n - x)
        out.append((x, c, pow2ceil(c + 2)))
        x += c
    return out


def logo_px(lines):
    s = lines / 480.0
    return int(round(LOGO_W * s)), int(round(LOGO_H * s))


def logo_tiles(lines):
    w, h = logo_px(lines)
    tiles = []
    for (y, ch, th) in split_axis(h):
        for (x, cw, tw) in split_axis(w):
            tw2, th2 = max(tw, th // 8), max(th, tw // 8)        # aspect <= 8:1
            tiles.append({"off": (x, y), "size": (cw, ch), "tex": (tw2, th2)})
    return tiles


def _assets():
    out = []
    for name, lines in LOGO_SETS:
        for i in range(len(logo_tiles(lines))):
            out += ["tex/logo_%s_%d.bmp" % (name, i), "tex/logo_%s_%d_a.bmp" % (name, i)]
    out += ["tex/logo.bmp", "tex/logo_a.bmp", "tex/logo.txt"]
    for L in UI_SETS:
        out += ["tex/front_%d.bmp" % L, "tex/front_%d_a.bmp" % L, "tex/front_%d.txt" % L]
    out.append("ashfall.ico")
    return out


ASSETS = _assets()


# ---------------------------------------------------------------------------------------------------- helpers
def clamp8(v):
    return 0 if v < 0 else (255 if v > 255 else int(v + 0.5))


def lerp(a, b, t):
    return a + (b - a) * t


def lerpc(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def smooth(e0, e1, x):
    t = (x - e0) / (e1 - e0)
    t = 0.0 if t < 0 else (1.0 if t > 1 else t)
    return t * t * (3 - 2 * t)


def chamfer_poly(x0, y0, x1, y1, tl=0, tr=0, br=0, bl=0):
    pts = []
    pts += [(x0, y0 + tl), (x0 + tl, y0)] if tl else [(x0, y0)]
    pts += [(x1 - tr, y0), (x1, y0 + tr)] if tr else [(x1, y0)]
    pts += [(x1, y1 - br), (x1 - br, y1)] if br else [(x1, y1)]
    pts += [(x0 + bl, y1), (x0, y1 - bl)] if bl else [(x0, y1)]
    return pts


def inset_chamfer(x0, y0, x1, y1, d, tl=0, tr=0, br=0, bl=0):
    """chamfer_poly inset by d (45 degree chamfers shrink by d * (2 - sqrt 2))."""
    f = 0.5858 * d
    return chamfer_poly(x0 + d, y0 + d, x1 - d, y1 - d, max(0, tl - f) if tl else 0, max(0, tr - f) if tr else 0,
                        max(0, br - f) if br else 0, max(0, bl - f) if bl else 0)


def corner_cuts(x0, y0, x1, y1, tl=0, tr=0, br=0, bl=0, e=0.6):
    """Triangles removing 45-degree corners of the box (extended by e outside so no AA residue stays)."""
    cuts = []
    if tl:
        cuts.append([(x0 - e, y0 - e), (x0 + tl + e, y0 - e), (x0 - e, y0 + tl + e)])
    if tr:
        cuts.append([(x1 - tr - e, y0 - e), (x1 + e, y0 - e), (x1 + e, y0 + tr + e)])
    if br:
        cuts.append([(x1 + e, y1 - br - e), (x1 + e, y1 + e), (x1 - br - e, y1 + e)])
    if bl:
        cuts.append([(x0 - e, y1 - bl - e), (x0 + bl + e, y1 + e), (x0 - e, y1 + e)])
    return cuts


def R(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


def raster(size, items, scale, ss=SS):
    """Exact sample rasteriser (even-odd, sample centres, painter's order) for the small UI sprites: a polygon
    edge on a pixel edge gives a crisp pixel edge (Pillow's polygon fill includes both span ends)."""
    W, H = size[0] * ss, size[1] * ss
    buf = bytearray(W * H)
    k = scale * ss
    for poly, val in items:
        pts = [(x * k, y * k) for x, y in poly]
        n = len(pts)
        ys = [p[1] for p in pts]
        j0, j1 = max(0, int(math.floor(min(ys)))), min(H - 1, int(math.ceil(max(ys))))
        fillb = bytes([val])
        for j in range(j0, j1 + 1):
            yc = j + 0.5
            xs = []
            for e in range(n):
                (x0, y0), (x1, y1) = pts[e], pts[(e + 1) % n]
                if (y0 <= yc < y1) or (y1 <= yc < y0):
                    xs.append(x0 + (yc - y0) * (x1 - x0) / (y1 - y0))
            xs.sort()
            for a, b in zip(xs[0::2], xs[1::2]):
                i0, i1 = max(0, int(math.ceil(a - 0.5))), min(W, int(math.ceil(b - 0.5)))
                if i1 > i0:
                    buf[j * W + i0:j * W + i1] = fillb * (i1 - i0)
    return Image.frombytes("L", (W, H), bytes(buf)).reduce(ss)


def poly_mask(size, items, scale, ss=SS, exact=None):
    """items [(poly, 255|0)] in units; scale = px per unit -> 'L' coverage (drawn at ss x, box-filtered).
    Small images use the exact rasteriser, large ones (logo) Pillow's polygon fill."""
    w, h = size
    if exact is None:
        exact = w * h * ss * ss <= 400000
    if exact:
        return raster(size, items, scale, ss)
    im = Image.new("L", (w * ss, h * ss), 0)
    d = ImageDraw.Draw(im)
    k = scale * ss
    for poly, val in items:
        d.polygon([(px * k - 0.49, py * k - 0.49) for px, py in poly], fill=val)   # Pillow: integer = px centre
    return im.reduce(ss) if ss > 1 else im


def _cross(m, op):
    out = m
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        out = op(out, ImageChops.offset(m, dx, dy))
    return out


def grow(m, r):
    """Dilate a coverage mask by r px (alternating square / cross steps = octagonal; fraction blended)."""
    n = int(r)
    for i in range(n):
        m = m.filter(ImageFilter.MaxFilter(3)) if i % 2 == 0 else _cross(m, ImageChops.lighter)
    f = r - n
    if f > 0.02:
        m2 = m.filter(ImageFilter.MaxFilter(3)) if n % 2 == 0 else _cross(m, ImageChops.lighter)
        m = Image.blend(m, m2, f)
    return m


def scale_l(m, a):
    return m.point(lambda v, a=a: clamp8(v * a))


def layer(cov, col, a=1.0):
    """Straight RGBA layer of one colour with alpha = coverage x a."""
    im = Image.new("RGBA", cov.size, tuple(int(c) for c in col) + (0,))
    im.putalpha(cov if a == 1.0 else scale_l(cov, a))
    return im


def premultiply(img):
    r, g, b, a = img.split()
    return Image.merge("RGBA", (ImageChops.multiply(r, a), ImageChops.multiply(g, a), ImageChops.multiply(b, a), a))


def reduce_bands(img, f):
    return Image.merge(img.mode, [b.reduce(f) for b in img.split()])


def hgrad(w, h, a0, a1, x0=0, x1=None):
    """'L' horizontal ramp a0 -> a1 (0..255) between x0 and x1 (clamped outside)."""
    x1 = w if x1 is None else x1
    row = bytes(clamp8(lerp(a0, a1, min(1.0, max(0.0, (x + 0.5 - x0) / float(max(1, x1 - x0)))))) for x in range(w))
    return Image.frombytes("L", (w, h), row * h)


def vgrad(w, h, a0, a1, y0=0, y1=None):
    y1 = h if y1 is None else y1
    data = b"".join(bytes([clamp8(lerp(a0, a1, min(1.0, max(0.0, (y + 0.5 - y0) / float(max(1, y1 - y0))))))]) * w
                    for y in range(h))
    return Image.frombytes("L", (w, h), data)


# ---------------------------------------------------------------------------------------------------- letters
def glyph(ch, H, W0, t):
    """Chamfered extended sans (own design). -> (advance, fills, cuts) in glyph-local units (y down, 0..H).
    Motif: big 45-degree cuts on the top-left and bottom-right corners, small softening elsewhere."""
    ts, th = t, t * 0.88
    c, k = t * 0.95, t * 0.22
    ym = (H - th) / 2.0
    fills, cuts, extra = [], [], []
    corners = (0, 0, 0, 0)
    if ch == " ":
        return W0 * 0.55, [], []
    if ch == "A":
        W = W0
        cA, yb = 0.42 * W, H * 0.52
        ci = max(0.0, cA - 0.586 * t)
        fills = [R(0, 0, W, H)]
        cuts = [chamfer_poly(ts, th, W - ts, yb, tl=ci, tr=ci), R(ts, yb + th, W - ts, H + 1)]
        corners = (cA, cA, k, k)
    elif ch == "S":
        W = W0 * 0.96
        fills = [R(0, 0, W, th), R(0, 0, ts, ym + th), R(0, ym, W, ym + th), R(W - ts, ym, W, H), R(0, H - th, W, H)]
        corners = (c, k, c, k)
    elif ch == "H":
        W = W0
        fills = [R(0, 0, ts, H), R(W - ts, 0, W, H), R(0, ym, W, ym + th)]
        corners = (c, k, c, k)
    elif ch == "F":
        W = W0 * 0.86
        fills = [R(0, 0, ts, H), R(0, 0, W, th), R(0, ym, W * 0.82, ym + th)]
        corners = (c, k, 0, k)
        extra = corner_cuts(0, ym, W * 0.82, ym + th, br=th * 0.8)
    elif ch == "E":
        W = W0 * 0.86
        fills = [R(0, 0, ts, H), R(0, 0, W, th), R(0, ym, W * 0.82, ym + th), R(0, H - th, W, H)]
        corners = (c, k, c, k)
        extra = corner_cuts(0, ym, W * 0.82, ym + th, br=th * 0.8)
    elif ch == "L":
        W = W0 * 0.84
        fills = [R(0, 0, ts, H), R(0, H - th, W, H)]
        corners = (c, 0, c, k)
    elif ch == "I":
        W = ts
        fills = [R(0, 0, ts, H)]
        corners = (0.6 * t, 0, 0.6 * t, 0)
    elif ch == "N":
        W = W0
        dw = ts * 1.22
        fills = [R(0, 0, ts, H), R(W - ts, 0, W, H), [(0, 0), (dw, 0), (W, H), (W - dw, H)]]
        corners = (c, k, c, k)
    elif ch == "D":
        W = W0
        cD = 0.30 * W
        ci = max(0.0, cD - 0.586 * t)
        fills = [R(0, 0, W, H)]
        cuts = [chamfer_poly(ts, th, W - ts, H - th, tr=ci, br=ci)]
        corners = (c, cD, cD, k)
    elif ch == "T":
        W = W0 * 0.94
        xs = (W - ts) / 2.0
        fills = [R(0, 0, W, th), R(xs, 0, xs + ts, H)]
        corners = (c, k, 0, 0)
        extra = corner_cuts(xs, 0, xs + ts, H, br=c * 0.8)
    elif ch == "R":
        W = W0
        yb, cR = H * 0.50, 0.30 * W
        ci = max(0.0, cR - 0.586 * t)
        xa, dw = W * 0.30, ts * 1.25
        fills = [R(0, 0, W, yb + th), R(0, 0, ts, H), [(xa, yb), (xa + dw, yb), (W, H), (W - dw, H)]]
        cuts = [chamfer_poly(ts, th, W - ts, yb, tr=ci)]
        corners = (c, cR, k, k)
        extra = corner_cuts(0, 0, W, yb + th, br=th * 0.9)
    elif ch == "C":
        W = W0 * 0.92
        cC = 0.32 * W
        ci = max(0.0, cC - 0.586 * t)
        fills = [R(0, 0, W, H)]
        cuts = [chamfer_poly(ts, th, W + 1, H - th, tl=ci, bl=ci)]
        corners = (cC, k, c, cC)
    elif ch == "P":
        W = W0 * 0.94
        yb, cR = H * 0.52, 0.30 * W
        ci = max(0.0, cR - 0.586 * t)
        fills = [R(0, 0, W, yb + th), R(0, 0, ts, H)]
        cuts = [chamfer_poly(ts, th, W - ts, yb, tr=ci)]
        corners = (c, cR, 0, k)
        extra = corner_cuts(0, 0, W, yb + th, br=cR * 0.8) + corner_cuts(0, 0, ts, H, br=c * 0.8)
    else:
        raise ValueError("no logo glyph for %r" % ch)
    cuts = cuts + corner_cuts(0, 0, W, H, *corners) + extra
    return W, fills, cuts


def word_layout(text, top, H, W0, t, track):
    """-> (advance list, total unsheared width)."""
    advs = [glyph(ch, H, W0, t)[0] for ch in text]
    return advs, sum(advs) + track * (len(text) - 1)


def word_polys(text, x, top, H, W0, t, track, shear):
    """Polygons of a word (left pen x, cap top), italic about the baseline. -> (items, letter boxes, width)."""
    items, boxes = [], []
    pen = x
    base = top + H
    for i, ch in enumerate(text):
        adv, fills, cuts = glyph(ch, H, W0, t)

        def tr(p, pen=pen):
            return [(pen + px + (H - py) * shear, top + py) for (px, py) in p]
        items += [(tr(p), 255) for p in fills] + [(tr(p), 0) for p in cuts]
        if ch != " ":
            boxes.append((pen, pen + adv))
        pen += adv + (track if i < len(text) - 1 else 0)
    return items, boxes, (pen - x) + H * shear, base


def centred_x(text, H, W0, t, track, shear):
    w = word_layout(text, 0, H, W0, t, track)[1] + H * shear
    return (LOGO_W - w) / 2.0, w


# ---------------------------------------------------------------------------------------------------- logo
def crack_items(boxes, top, base, shear, rng):
    """Lava crack polylines rising from the baseline inside each letter (unsheared letter space -> sheared).
    -> list of (p0, p1, width) in virtual units."""
    H = base - top
    segs = []

    def walk(x, y, ang, length, w0, depth):
        step = 2.2
        n = max(2, int(length / step))
        px, py = x, y
        for i in range(n):
            u = i / float(n)
            ang += rng.gauss(0, 0.30) + (-math.pi / 2 - ang) * 0.25
            nx, ny = px + math.cos(ang) * step, py + math.sin(ang) * step
            w = w0 * (1.0 - u) ** 0.7 + 0.35
            segs.append(((px + (base - py) * shear, py), (nx + (base - ny) * shear, ny), w))
            if depth < 2 and rng.chance(0.11):
                walk(nx, ny, ang + rng.sign() * rng.range(0.6, 1.0), length * (1 - u) * rng.range(0.35, 0.6),
                     w * 0.75, depth + 1)
            px, py = nx, ny

    for (x0, x1) in boxes:
        n = 2 if (x1 - x0) < 40 else 3
        for j in range(n):
            x = lerp(x0 + 2, x1 - 2, (j + rng.range(0.15, 0.85)) / float(n))
            walk(x, base + 1.0, -math.pi / 2 + rng.range(-0.45, 0.45), H * rng.range(0.35, 0.72),
                 rng.range(1.1, 1.7), 0)
    return segs


def draw_segs(size, segs, scale, ss=SS):
    w, h = size
    im = Image.new("L", (w * ss, h * ss), 0)
    d = ImageDraw.Draw(im)
    k = scale * ss
    for (p0, p1, wd) in segs:
        lw = max(1, int(round(wd * k)))
        a, b = (p0[0] * k - 0.49, p0[1] * k - 0.49), (p1[0] * k - 0.49, p1[1] * k - 0.49)
        d.line([a, b], fill=255, width=lw)
        r = lw / 2.0
        d.ellipse([b[0] - r, b[1] - r, b[0] + r, b[1] + r], fill=255)
    return im.reduce(ss)


def bevel_shade(cov, nb):
    """Chisel bevel: distance-to-edge by nb erosions -> (hy, hx) gradients centred on 128 (+ = facing up/left)."""
    binm = cov.point(lambda v: 255 if v >= 128 else 0)
    acc = binm.point(lambda v: 1 if v else 0)
    m = binm
    for i in range(nb):
        m = m.filter(ImageFilter.MinFilter(3)) if i % 2 == 0 else _cross(m, ImageChops.darker)
        acc = ImageChops.add(acc, m.point(lambda v: 1 if v else 0))
    h = acc.point(lambda c, n=nb: clamp8(c * 255.0 / (n + 1)))
    h = h.filter(ImageFilter.GaussianBlur(0.8))
    grad = 2 * 255.0 / (nb + 1)
    sc = grad / 110.0
    hy = ImageChops.subtract(ImageChops.offset(h, 0, -1), ImageChops.offset(h, 0, 1), sc, 128)
    hx = ImageChops.subtract(ImageChops.offset(h, -1, 0), ImageChops.offset(h, 1, 0), sc, 128)
    return hy, hx


def render_logo(s):
    """Logo canvas at s px per virtual unit -> premultiplied RGBA (round(460 s) x round(100 s))."""
    cw, chh = int(round(LOGO_W * s)), int(round(LOGO_H * s))
    ws = s * WS
    size = (cw * WS, chh * WS)
    W, H = size
    rng = Rng(SEED, "logo")

    # --- main word
    mx, _ = centred_x(MAIN_TEXT, MAIN_H, MAIN_W0, MAIN_T, MAIN_TRACK, SHEAR)
    items, boxes, _, base = word_polys(MAIN_TEXT, mx, MAIN_TOP, MAIN_H, MAIN_W0, MAIN_T, MAIN_TRACK, SHEAR)
    cov = poly_mask(size, items, ws)
    ow = max(1.0, 1.7 * ws)                  # outline width
    outl = grow(cov, ow)

    # --- subtitle
    sx, sw = centred_x(SUB_TEXT, SUB_H, SUB_W0, SUB_T, SUB_TRACK, SHEAR)
    sitems, _, _, sbase = word_polys(SUB_TEXT, sx, SUB_TOP, SUB_H, SUB_W0, SUB_T, SUB_TRACK, SHEAR)
    scov = poly_mask(size, sitems, ws)
    sout = grow(scov, max(1.0, 1.05 * ws))

    # --- streaks beside the subtitle (taper toward the outer ends)
    cy = SUB_TOP + SUB_H * 0.5 + 0.5
    gap = 7.0
    lx0, lx1 = 20.0, sx - gap
    rx0, rx1 = sx + sw + gap, LOGO_W - 20.0
    th0, th1 = 0.35, 1.25
    streak_items = [([(lx0, cy - th0), (lx1, cy - th1), (lx1 - 1.2, cy + th1), (lx0, cy + th0)], 255),
                    ([(rx0 + 1.2, cy - th1), (rx1, cy - th0), (rx1, cy + th0), (rx0, cy + th1)], 255)]
    stk = poly_mask(size, streak_items, ws)
    ramp = Image.new("L", size, 0)
    ramp.paste(hgrad(int(lx1 * ws) - int(lx0 * ws), H, 0, 255), (int(lx0 * ws), 0))
    ramp.paste(hgrad(int(rx1 * ws) - int(rx0 * ws), H, 255, 0), (int(rx0 * ws), 0))
    stk = ImageChops.multiply(stk, ramp)
    stk_out = ImageChops.multiply(grow(stk, max(1.0, 0.9 * ws)), ramp)

    # --- lava cracks
    segs = crack_items(boxes, MAIN_TOP, base, SHEAR, rng)
    crack = ImageChops.multiply(draw_segs(size, segs, ws), cov)
    halo = crack.filter(ImageFilter.GaussianBlur(2.4 * ws)).point(lambda v: clamp8(v * 2.6))
    halo = ImageChops.multiply(halo, cov)

    # --- bevel + noise
    nb = max(2, int(round(2.6 * ws)))
    hy, hx = bevel_shade(cov, nb)
    ng = noise_grid(max(8, W // 6), max(8, H // 6), cells=6, octaves=4, seed=SEED + 3, tile=False)
    nimg = Image.frombytes("L", (len(ng[0]), len(ng)), bytes(clamp8(v * 255) for row in ng for v in row))
    nimg = nimg.resize(size, Image.BICUBIC)

    # --- face colour (per pixel, inside the main mask)
    covb, hyb, hxb = cov.tobytes(), hy.tobytes(), hx.tobytes()
    crb, hab, nzb = crack.tobytes(), halo.tobytes(), nimg.tobytes()
    face = bytearray(W * H * 4)
    top_px, base_px = MAIN_TOP * ws, base * ws
    for y in range(H):
        v = (y + 0.5 - top_px) / (base_px - top_px)
        v = 0.0 if v < 0 else (1.0 if v > 1 else v)
        if v < 0.46:
            col0 = lerpc((206, 213, 223), (112, 121, 136), (v / 0.46) ** 1.2)
            rough = 0.10
        else:
            u = (v - 0.46) / 0.54
            col0 = lerpc((62, 58, 60), (30, 24, 22), u)
            rough = 0.30
        e = smooth(0.50, 1.0, v) ** 1.4 * 0.62
        col0 = (col0[0] + 210 * e, col0[1] + 62 * e, col0[2] + 12 * e)
        heat = 0.45 + 0.55 * smooth(0.25, 1.0, v)
        under = smooth(0.30, 1.0, v)
        row = y * W
        for x in range(W):
            i = row + x
            c = covb[i]
            if not c:
                continue
            nz = 1.0 + rough * (nzb[i] / 255.0 - 0.5) * 2.0
            r, g, b = col0[0] * nz, col0[1] * nz, col0[2] * nz
            d = ((hyb[i] - 128) * 0.82 + (hxb[i] - 128) * 0.50) / 110.0
            if d > 0:
                d = 1.0 if d > 1 else d
                r += 230 * d * 0.80
                g += 238 * d * 0.80
                b += 255 * d * 0.80
            elif d < 0:
                d = -1.0 if d < -1 else d
                m = 1.0 + 0.72 * d
                r, g, b = r * m - 255 * d * 0.55 * under, g * m - 112 * d * 0.55 * under, b * m - 30 * d * 0.55 * under
            hl = hab[i] / 255.0 * heat
            if hl:
                r += 255 * hl * 0.85
                g += 78 * hl * 0.85
                b += 14 * hl * 0.85
            ct = crb[i] / 255.0 * (0.55 + 0.45 * heat)
            if ct:
                r, g, b = lerp(r, 255, ct), lerp(g, 214, ct), lerp(b, 120, ct)
            j = i * 4
            face[j] = clamp8(r)
            face[j + 1] = clamp8(g)
            face[j + 2] = clamp8(b)
            face[j + 3] = c
    face = Image.frombytes("RGBA", size, bytes(face))

    # --- subtitle face: silver, slightly darker toward the baseline
    sface = Image.merge("RGB", [vgrad(W, H, a0, a1, int(SUB_TOP * ws), int(sbase * ws))
                                for a0, a1 in ((240, 166), (243, 172), (248, 186))])
    sface.putalpha(scov)

    # --- composite (straight alpha)
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    allout = ImageChops.lighter(ImageChops.lighter(outl, sout), stk_out)
    sh = ImageChops.offset(allout, int(round(1.6 * ws)), int(round(2.4 * ws))).filter(
        ImageFilter.GaussianBlur(2.2 * ws))
    out = Image.alpha_composite(out, layer(sh, (0, 0, 0), 0.62))
    glow = outl.filter(ImageFilter.GaussianBlur(6.0 * ws))
    glow = ImageChops.multiply(glow, vgrad(W, H, 0, 255, int((MAIN_TOP + MAIN_H * 0.3) * ws), int(base * ws)))
    out = Image.alpha_composite(out, layer(glow, (255, 84, 18), 0.50))
    out = Image.alpha_composite(out, layer(outl, (16, 11, 10)))
    out = Image.alpha_composite(out, face)
    out = Image.alpha_composite(out, layer(sout, (16, 11, 10)))
    out = Image.alpha_composite(out, sface)
    out = Image.alpha_composite(out, layer(stk_out, (16, 11, 10), 0.85))
    out = Image.alpha_composite(out, layer(stk, EMBER_HOT))
    return reduce_bands(premultiply(out), WS)


def cut_tiles(canvas, lines):
    """Premultiplied canvas -> [(tile dict, RGBA tile image)] with a 1-texel apron of neighbour content."""
    w, h = canvas.size
    out = []
    for t in logo_tiles(lines):
        (ox, oy), (cw, ch), (tw, th) = t["off"], t["size"], t["tex"]
        img = Image.new("RGBA", (tw, th), (0, 0, 0, 0))
        x0, y0 = max(0, ox - 1), max(0, oy - 1)
        x1, y1 = min(w, ox + cw + 1), min(h, oy + ch + 1)
        img.paste(canvas.crop((x0, y0, x1, y1)), (1 - (ox - x0), 1 - (oy - y0)))
        out.append((t, img))
    return out


def logo_txt(tilesets, small_size):
    L = []
    L.append("# logo.txt - Ashfall title logo layout. GENERATED by tools/art/gen_front.py (A10): edit the generator.")
    L.append("# Textures are PREMULTIPLIED (RGB = colour x alpha): load <name>.bmp + <name>_a.bmp with image flags 0")
    L.append("# and combine them (like hud_font.cpp load_page; IMG_ALPHA's bleed breaks premultiplied texels),")
    L.append("# RT_CLAMP | RT_NOMIP, bilinear. Draw with RB_PREMUL (ONE, INVSRCALPHA), vertex colour white; fade with")
    L.append("# (k, k, k, k) (scale RGB and alpha together).")
    L.append("# Records (integers, single spaces; ignore unknown records and extra trailing fields):")
    L.append("#   place <x> <y> <w> <h>        logo canvas rect in virtual 640x480 units (title screen)")
    L.append("#   set <name> <nativeLines> <pxW> <pxH> <tiles> <minScreenH>")
    L.append("#        the canvas is pxW x pxH texels; drawn 1:1 when screen h = nativeLines. Pick the set with the")
    L.append("#        largest minScreenH <= screen h (lo below %d lines, hi from %d)." % (LOGO_HI_MIN_H, LOGO_HI_MIN_H))
    L.append("#   tile <set> <i> <texture> <texW> <texH> <u0> <v0> <w> <h> <offX> <offY>")
    L.append("#        the w x h texels at (u0, v0) of the texture cover canvas texels (offX, offY)..(+w, +h).")
    L.append("#        Each tile carries a 1-texel apron of neighbour content around that rect, so bilinear")
    L.append("#        filtering at non-native scales shows no seams. UV = (u0 / texW, v0 / texH) ..")
    L.append("#        ((u0 + w) / texW, (v0 + h) / texH); no half-texel inset.")
    L.append("#   small <texture> <texW> <texH> <u0> <v0> <w> <h> <scale1000>")
    L.append("#        the whole logo in one texture (pause / records headers): canvas = w x h texels at (u0, v0);")
    L.append("#        scale1000 = texels per virtual unit x 1000 (draw at w x 1000 / scale1000 virtual units for")
    L.append("#        a 1:1 look at 480 lines, or any size).")
    L.append("# Drawing (k = screen h / 480, f = screen h / nativeLines): X = round(placeX * k) (+ the horizontal")
    L.append("# centring offset of non-4:3 modes), Y = round(placeY * k); tile screen rect = (X + offX * f,")
    L.append("# Y + offY * f, w * f, h * f). At the native height f = 1 and every texel is one pixel (D3D TL: subtract")
    L.append("# 0.5 from x and y); at 640x480 the lo set draws at f = 0.8, at 1024x768 the hi set at f = 0.73.")
    L.append("place %d %d %d %d" % (LOGO_X, LOGO_Y, LOGO_W, LOGO_H))
    for name, lines, (pw, ph), tiles, tnames in tilesets:
        mn = 0 if name == "lo" else LOGO_HI_MIN_H
        L.append("set %s %d %d %d %d %d" % (name, lines, pw, ph, len(tiles), mn))
        for i, t in enumerate(tiles):
            (ox, oy), (cw, ch), (tw, th) = t["off"], t["size"], t["tex"]
            L.append("tile %s %d %s %d %d 1 1 %d %d %d %d" % (name, i, tnames[i], tw, th, cw, ch, ox, oy))
    sw, sh = small_size
    L.append("small logo %d %d 0 %d %d %d %d" % (SMALL_TEX[0], SMALL_TEX[1], (SMALL_TEX[1] - sh) // 2, sw, sh,
                                                  int(round(SMALL_S * 1000))))
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------------------------------- menu kit
class UISet(object):
    def __init__(self, lines):
        self.lines = lines
        self.s = lines / 480.0
        self.st = STROKE[lines]
        self.fw = max(1, self.st // 2)       # thin frame line
        self.o = SHOFF[lines]
        self.tex = "front_%d" % lines

    def px(self, v):
        return int(round(v * self.s))


def bake_tint(cov, o):
    """White shape (premultiplied) + black drop shadow at (+o, +o), 60 % (same as the HUD, A4c)."""
    w, h = cov.size
    c = Image.new("L", (w + o, h + o), 0)
    c.paste(cov, (0, 0))
    sh = Image.new("L", (w + o, h + o), 0)
    sh.paste(cov, (o, o))
    a = ImageChops.add(c, ImageChops.multiply(scale_l(sh, SHADOW_A), ImageChops.invert(c)))
    return Image.merge("RGBA", (c, c, c, a))


def ring(outer, inner):
    return [(outer, 255), (inner, 0)]


def compose(size, layers):
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    for cov, col, a in layers:
        out = Image.alpha_composite(out, layer(cov, col, a))
    return premultiply(out)


def ui_elements(U):
    """-> list of (name, premul RGBA, ox, oy, kind, l, t, r, b, tint). kind 0 sprite, 3 = 3-slice, 9 = 9-slice."""
    s, st, fw, o = U.s, U.st, U.fw, U.o
    els = []

    # panel 9-slice: dark translucent fill, black rim, thin green frame, bright green corner brackets,
    # chamfered top-left / bottom-right corners (the logo motif)
    C = U.px(14)
    ch = U.px(8)
    P = 2 * C + 4
    full = chamfer_poly(0, 0, P, P, tl=ch, br=ch)
    fill = poly_mask((P, P), [(full, 255)], 1)
    rim = poly_mask((P, P), ring(full, inset_chamfer(0, 0, P, P, 1, tl=ch, br=ch)), 1)
    frame = poly_mask((P, P), ring(inset_chamfer(0, 0, P, P, 1, tl=ch, br=ch),
                                   inset_chamfer(0, 0, P, P, 1 + fw, tl=ch, br=ch)), 1)
    brk = poly_mask((P, P), ring(inset_chamfer(0, 0, P, P, 1, tl=ch, br=ch),
                                 inset_chamfer(0, 0, P, P, 1 + st, tl=ch, br=ch)), 1)
    lb = C - 1
    corners = poly_mask((P, P), [(R(0, 0, lb, lb), 255), (R(P - lb, 0, P, lb), 255), (R(P - lb, P - lb, P, P), 255),
                                 (R(0, P - lb, lb, P), 255)], 1)
    brk = ImageChops.multiply(brk, corners)
    els.append(("panel", compose((P, P), [(fill, PANEL_FILL[:3], PANEL_FILL[3] / 255.0), (rim, (0, 0, 0), 0.85),
                                          (frame, GREEN, 0.55), (brk, GREEN, 1.0)]), 0, 0, 9, C, C, C, C, 0))

    # title strip 3-slice (panel header): green tint, green base line, ember notches at the left
    Th, Lc, Rc = U.px(20), U.px(30), U.px(18)
    Wt = Lc + 4 + Rc
    tfill = Image.new("L", (Wt, Th), 255)
    tfill = ImageChops.multiply(tfill, hgrad(Wt, Th, 255, 0, Lc + 4, Wt))
    tline = poly_mask((Wt, Th), [(R(0, Th - fw, Wt, Th), 255)], 1)
    m0, nw, sk = U.px(5), max(2, U.px(5)), U.px(4)
    y0, y1 = U.px(4), Th - U.px(4) - fw
    n1 = poly_mask((Wt, Th), [([(m0 + sk, y0), (m0 + nw + sk, y0), (m0 + nw, y1), (m0, y1)], 255)], 1)
    m1 = m0 + nw + max(2, U.px(3))
    n2 = poly_mask((Wt, Th), [([(m1 + sk, y0), (m1 + nw * 0.6 + sk, y0), (m1 + nw * 0.6, y1), (m1, y1)], 255)], 1)
    els.append(("title", compose((Wt, Th), [(tfill, GREEN, 0.16), (tline, GREEN, 0.9), (n1, EMBER, 1.0),
                                             (n2, (200, 70, 20), 1.0)]), 0, 0, 3, Lc, 0, Rc, 0, 0))

    # selection bar 3-slice: bright chamfered green edge, translucent green body, lines, fades out to the right
    Bh, Lb, Rb = U.px(19), U.px(10), U.px(56)
    Wb = Lb + 4 + Rb
    ramp = hgrad(Wb, Bh, 255, 0, Lb + 4, Wb)
    ramp2 = hgrad(Wb, Bh, 255, 0, Lb + 4 + Rb // 3, Wb)
    bshape = chamfer_poly(0, 0, Wb, Bh, tl=U.px(5))
    body = ImageChops.multiply(poly_mask((Wb, Bh), [(bshape, 255)], 1), ramp)
    lines = poly_mask((Wb, Bh), [(R(0, 0, Wb, fw), 255), (R(0, Bh - fw, Wb, Bh), 255)], 1)
    lines = ImageChops.multiply(ImageChops.multiply(lines, poly_mask((Wb, Bh), [(bshape, 255)], 1)), ramp2)
    eb = max(2, U.px(3))
    edge = poly_mask((Wb, Bh), [(chamfer_poly(0, 0, eb, Bh, tl=min(eb, U.px(5))), 255)], 1)
    els.append(("bar", compose((Wb, Bh), [(body, GREEN, 0.30), (lines, GREEN, 0.75), (edge, GREEN, 1.0)]),
                0, 0, 3, Lb, 0, Rb, 0, 0))

    # key caps 9-slice: normal (green frame) and hot (capturing: ember)
    Kc = max(4, U.px(5))
    P = 2 * Kc + 4
    kch = max(2, U.px(3))
    kshape = chamfer_poly(0, 0, P, P, tl=kch, br=kch)
    kfill = poly_mask((P, P), [(kshape, 255)], 1)
    kfr = poly_mask((P, P), ring(kshape, inset_chamfer(0, 0, P, P, fw, tl=kch, br=kch)), 1)
    els.append(("keycap", compose((P, P), [(kfill, (0, 0, 0), 0.55), (kfr, GREEN, 0.85)]), 0, 0, 9, Kc, Kc, Kc, Kc, 0))
    kfr2 = poly_mask((P, P), ring(kshape, inset_chamfer(0, 0, P, P, max(fw, st - 1), tl=kch, br=kch)), 1)
    els.append(("keycap_hot", compose((P, P), [(kfill, (255, 110, 20), 0.30), (kfr2, EMBER, 1.0)]),
                0, 0, 9, Kc, Kc, Kc, Kc, 0))

    # tabs 9-slice
    Tc = max(4, U.px(6))
    P = 2 * Tc + 4
    tch = max(2, U.px(4))
    tshape = chamfer_poly(0, 0, P, P, tl=tch)
    tf = poly_mask((P, P), [(tshape, 255)], 1)
    tfr = poly_mask((P, P), ring(tshape, inset_chamfer(0, 0, P, P, fw, tl=tch)), 1)
    tfr2 = poly_mask((P, P), ring(tshape, inset_chamfer(0, 0, P, P, st, tl=tch)), 1)
    els.append(("tab", compose((P, P), [(tf, (0, 0, 0), 0.35), (tfr, GREEN, 0.45)]), 0, 0, 9, Tc, Tc, Tc, Tc, 0))
    els.append(("tab_on", compose((P, P), [(tf, GREEN, 0.26), (tfr2, GREEN, 1.0)]), 0, 0, 9, Tc, Tc, Tc, Tc, 0))

    # tintable sprites (white + baked shadow)
    mh = 2 * U.px(5.5)
    mw = int(round(mh * 0.75))
    cov = poly_mask((mw, mh), [([(0, 0), (mw, mh / 2.0), (0, mh), (mw * 0.38, mh / 2.0)], 255)], 1)
    els.append(("marker", bake_tint(cov, o), mw, mh // 2, 0, 0, 0, 0, 0, 1))
    ah = 2 * U.px(4.5)
    aw = int(round(ah * 0.62))
    cov = poly_mask((aw, ah), [([(aw, 0), (0, ah / 2.0), (aw, ah)], 255)], 1)
    els.append(("arrow_l", bake_tint(cov, o), aw // 2, ah // 2, 0, 0, 0, 0, 0, 1))
    cov = poly_mask((aw, ah), [([(0, 0), (aw, ah / 2.0), (0, ah)], 255)], 1)
    els.append(("arrow_r", bake_tint(cov, o), aw // 2, ah // 2, 0, 0, 0, 0, 0, 1))
    cov = poly_mask((ah, aw), [([(0, aw), (ah / 2.0, 0), (ah, aw)], 255)], 1)
    els.append(("arrow_u", bake_tint(cov, o), ah // 2, aw // 2, 0, 0, 0, 0, 0, 1))
    cov = poly_mask((ah, aw), [([(0, 0), (ah, 0), (ah / 2.0, aw)], 255)], 1)
    els.append(("arrow_d", bake_tint(cov, o), ah // 2, aw // 2, 0, 0, 0, 0, 0, 1))
    n = U.px(11)
    cov = poly_mask((n, n), ring(R(0, 0, n, n), R(st, st, n - st, n - st)), 1)
    els.append(("check_off", bake_tint(cov, o), n // 2, n // 2, 0, 0, 0, 0, 0, 1))
    g = st + max(1, U.px(1.5))
    cov = poly_mask((n, n), ring(R(0, 0, n, n), R(st, st, n - st, n - st)) + [(R(g, g, n - g, n - g), 255)], 1)
    els.append(("check_on", bake_tint(cov, o), n // 2, n // 2, 0, 0, 0, 0, 0, 1))
    sw_, sh_ = U.px(6), U.px(11)
    cov = poly_mask((sw_, sh_), [(R(0, 0, sw_, sh_), 255)], 1)
    els.append(("seg", bake_tint(cov, o), 0, 0, 0, 0, 0, 0, 0, 1))
    cov = poly_mask((sw_, sh_), ring(R(0, 0, sw_, sh_), R(fw, fw, sw_ - fw, sh_ - fw)), 1)
    els.append(("seg_off", bake_tint(cov, o), 0, 0, 0, 0, 0, 0, 0, 1))
    Dc = U.px(24)
    Wd = 2 * Dc + 4
    cov = ImageChops.multiply(ImageChops.multiply(Image.new("L", (Wd, fw), 255), hgrad(Wd, fw, 0, 255, 0, Dc)),
                              hgrad(Wd, fw, 255, 0, Dc + 4, Wd))
    els.append(("divider", bake_tint(cov, o), 0, 0, 3, Dc, 0, Dc + o, 0, 1))
    els.append(("white", Image.new("RGBA", (4, 4), (255, 255, 255, 255)), 0, 0, 0, 0, 0, 0, 0, 1))
    return els


def snap4444(img, boxes):
    """Make the texels in boxes exact under the engine's 4444 conversion (r_d3d6.cpp: RGB + Bayer 0..15 then >> 4,
    alpha >> 4): premultiplied RGB = 16 m (m <= alpha level), alpha = 17 m. Stretched 9/3-slice middles would
    otherwise magnify the dither pattern into blocks (and the corners must match the stretched middle)."""
    px = img.load()
    done = set()
    for (x0, y0, x1, y1) in boxes:
        for y in range(y0, y1):
            for x in range(x0, x1):
                if (x, y) in done:
                    continue
                done.add((x, y))
                r, g, b, a = px[x, y]
                ma = (a + 8) // 17
                px[x, y] = (min(ma, (r + 8) // 17) * 16, min(ma, (g + 8) // 17) * 16, min(ma, (b + 8) // 17) * 16,
                            ma * 17)
    return img


def snap_slices(els):
    out = []
    for e in els:
        img, kind, l, t, r, b = e[1], e[4], e[5], e[6], e[7], e[8]
        w, h = img.size
        if kind == 9:                                   # whole frame: corners match the stretched parts
            img = snap4444(img.copy(), [(0, 0, w, h)])
        elif kind == 3:                                 # left cap + middle; the fading right cap keeps dither
            img = snap4444(img.copy(), [(0, 0, w - r, h)])
        out.append((e[0], img) + tuple(e[2:]))
    return out


def build_ui_set(U):
    at = I.Atlas(256, 256, gutter=2)
    els = snap_slices(ui_elements(U))
    order = sorted(range(len(els)), key=lambda i: (-els[i][1].size[1], -els[i][1].size[0], els[i][0]))
    for i in order:
        at.add(els[i][0], els[i][1])
    page = at.image()
    recs = []
    for e in els:
        x, y, w, h = at.rect(e[0])
        recs.append((e[0], x, y, w, h) + tuple(e[2:]))
    used = sum(r[3] * r[4] for r in recs)
    return page, recs, used


SPRITE_NOTES = {
    "panel": "9-slice panel: corners 1:1, edges/centre stretched; chamfered TL/BR corners, green brackets",
    "title": "3-slice panel title strip (draw across the panel's inner width at its top); text starts after l",
    "bar": "3-slice selection bar (row highlight): l = bright edge, r = fade-out; text white on top",
    "keycap": "9-slice key name box (controls list)",
    "keycap_hot": "9-slice key box while capturing ('PRESS A KEY')",
    "tab": "9-slice tab (inactive)",
    "tab_on": "9-slice tab (active)",
    "marker": "selection chevron, points +x, anchor = tip",
    "arrow_l": "value arrow (option cycling), anchor = centre",
    "arrow_r": "value arrow, anchor = centre",
    "arrow_u": "list scroll arrow, anchor = centre",
    "arrow_d": "list scroll arrow, anchor = centre",
    "check_off": "check box, anchor = centre",
    "check_on": "check box ticked, anchor = centre",
    "seg": "volume segment (on), anchor = top-left",
    "seg_off": "volume segment (off, outline), anchor = top-left",
    "divider": "3-slice horizontal rule fading at both ends (tint green)",
    "white": "solid white 4x4 (sample the centre for plain rectangles / veils)",
}


def front_txt(U, recs):
    L = []
    L.append("# %s.txt - front-end (menu) kit for %d screen lines. GENERATED by tools/art/gen_front.py (A10):" % (
        U.tex, U.lines))
    L.append("# edit the generator. Sets 480 600 768 1050 (same names, metrics in px of the set): pick the largest")
    L.append("# set <= screen height (like hud_atlas_<L>) and place at virtual 640x480 coordinates x (h / 480).")
    L.append("# Texture: %s.bmp + _a.bmp (256x256), PREMULTIPLIED: load both with image flags 0 and combine" % U.tex)
    L.append("# (hud_font.cpp load_page), RT_CLAMP | RT_NOMIP. Draw with RB_PREMUL (ONE, INVSRCALPHA), 1:1 texel =")
    L.append("# pixel at whole-pixel positions (D3D TL: subtract 0.5). Text: the HUD fonts (hud_atlas_<L>).")
    L.append("# tint 1 = white shape with the black drop shadow baked (+shadowOff px, 60 %, like the HUD A4c): the")
    L.append("#   vertex colour (premultiplied) tints the shape, the shadow stays black.")
    L.append("# tint 0 = colours baked (green / ember / dark fill): draw with vertex colour white, fade with (k,k,k,k).")
    L.append("# Records (integers, single spaces; '#' = comment; ignore unknown records / extra trailing fields):")
    L.append("#   set <lines> <pages> <pageW> <pageH> <scale1000> <stroke> <thin> <shadowOff> <shadowA100>")
    L.append("#   page <n> <texture>")
    L.append("#   <name> <x> <y> <w> <h> <ox> <oy> <page> <kind> <l> <t> <r> <b> <tint>")
    L.append("#        same first 8 fields as hud_atlas sprites (x y w h = rect incl. any baked shadow, (ox, oy) =")
    L.append("#        anchor). kind 0 = sprite: draw the rect with its top-left at (Ax - ox, Ay - oy).")
    L.append("#        kind 9 = 9-slice: l t r b = border widths in texels; corners drawn 1:1, edges stretched along")
    L.append("#        their length, the centre both ways (the centre and edge middles are uniform, so any size")
    L.append("#        >= l + r by t + b works). kind 3 = horizontal 3-slice: l / r cap widths 1:1, middle")
    L.append("#        stretched, height drawn 1:1 (h).")
    L.append("set %d 1 256 256 %d %d %d %d %d" % (U.lines, int(round(U.s * 1000)), U.st, U.fw, U.o,
                                               int(SHADOW_A * 100)))
    L.append("page 0 %s" % U.tex)
    for r in recs:
        L.append("# %s" % SPRITE_NOTES[r[0]])
        L.append("%s %d %d %d %d %d %d 0 %d %d %d %d %d %d" % r)
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------------------------------- icon
VGA16 = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128), (0, 128, 128),
         (192, 192, 192), (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0), (0, 0, 255), (255, 0, 255),
         (0, 255, 255), (255, 255, 255)]


VGA_USE = (0, 1, 4, 7, 8, 9, 11, 15)    # black maroon navy silver grey red yellow white: no olive/green/teal shades


def jet_poly(cx, cy, length, ang_deg):
    """F-16-like top-view silhouette, nose toward ang (0 = up, + = clockwise), in icon units."""
    half = [(0.0, 0.52), (0.035, 0.42), (0.05, 0.26), (0.08, 0.08), (0.34, -0.14), (0.34, -0.20), (0.09, -0.22),
            (0.08, -0.34), (0.21, -0.45), (0.21, -0.50), (0.06, -0.49), (0.045, -0.53)]
    pts = half + [(-x, y) for (x, y) in reversed(half)]
    a = math.radians(ang_deg)
    ca, sa = math.cos(a), math.sin(a)
    out = []
    for (x, y) in pts:
        x, y = x * length, -y * length              # y up -> screen down
        out.append((cx + x * ca - y * sa, cy + x * sa + y * ca))
    return out


def _cross_pad(m):
    """Cross dilation without wrap-around."""
    w, h = m.size
    big = Image.new("L", (w + 2, h + 2), 0)
    big.paste(m, (1, 1))
    return _cross(big, ImageChops.lighter).crop((1, 1, w + 1, h + 1))


ICON_JET = {32: (16.0, 9.5, 17.0, 0.0), 16: (16.0, 9.0, 18.0, 0.0)}   # centre x, y, length (units of 32), heading


def draw_icon(n):
    """n x n RGBA (hard 1-bit alpha) program icon: basalt volcano with a glowing crater, jet climbing away."""
    k = 8
    S = n * k
    u = S / 32.0
    cm = Image.new("L", (S, S), 0)
    ImageDraw.Draw(cm).polygon([(0.0, 31.0 * u), (11.0 * u, 21.0 * u), (21.0 * u, 21.0 * u), (32.0 * u, 31.0 * u),
                                (32.0 * u, 32.0 * u), (0.0, 32.0 * u)], fill=255)
    sh = Image.new("RGB", (S, S))
    px = []
    for y in range(S):
        for x in range(S):
            t = (x / float(S) - 0.28) / 0.50
            t = 0.0 if t < 0 else (1.0 if t > 1 else t)
            px.append(tuple(clamp8(v) for v in lerpc((150, 140, 134), (52, 48, 48), t)))
    sh.putdata(px)
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    img.paste(sh, (0, 0), cm)
    d = ImageDraw.Draw(img)
    d.line([(15.0 * u, 21.5 * u), (13.6 * u, 24.5 * u), (15.2 * u, 27.0 * u), (13.4 * u, 31.5 * u)],
           fill=(236, 70, 16, 255), width=max(1, int(1.7 * u)))
    d.ellipse([10.4 * u, 19.6 * u, 21.6 * u, 22.4 * u], fill=(255, 100, 16, 255))
    d.ellipse([13.0 * u, 20.2 * u, 19.0 * u, 21.8 * u], fill=(255, 232, 96, 255))
    jx, jy, jl, ja = ICON_JET[n]
    tx = jx * u - math.sin(math.radians(ja)) * jl * 0.5 * u
    ty = jy * u + math.cos(math.radians(ja)) * jl * 0.5 * u
    if n >= 32:
        d.polygon([(tx - 0.9 * u, ty), (tx + 0.9 * u, ty), (tx + 0.5 * u, 19.6 * u), (tx - 0.5 * u, 19.6 * u)],
                  fill=(238, 242, 248, 255))
    d.polygon(jet_poly(jx * u, jy * u, jl * u, ja), fill=(198, 204, 214, 255))
    cx = jx * u + math.sin(math.radians(ja)) * jl * 0.27 * u
    cy = jy * u - math.cos(math.radians(ja)) * jl * 0.27 * u
    d.ellipse([cx - 1.1 * u, cy - 1.1 * u, cx + 1.1 * u, cy + 1.1 * u], fill=(40, 72, 128, 255))
    small = reduce_bands(premultiply(img), k)
    out = []
    for (rr, gg, bb, aa) in small.getdata():
        if aa >= 110:
            f = 255.0 / aa
            out.append((clamp8(rr * f), clamp8(gg * f), clamp8(bb * f), 255))
        else:
            out.append((0, 0, 0, 0))
    res = Image.new("RGBA", (n, n))
    res.putdata(out)
    al = res.split()[3]
    ring_ = ImageChops.subtract(_cross_pad(al), al)       # 1 px dark outline (reads on teal and white desktops)
    res.paste(Image.new("RGBA", (n, n), (24, 20, 20, 255)), (0, 0), ring_)
    return res


def nearest(pal, c):
    best, bi = None, 0
    for i, p in enumerate(pal):
        dd = (p[0] - c[0]) ** 2 * 3 + (p[1] - c[1]) ** 2 * 4 + (p[2] - c[2]) ** 2 * 2
        if best is None or dd < best:
            best, bi = dd, i
    return bi


def icon_indexed(rgba, bpp):
    """-> (palette list, index list, mask list) for a 4- or 8-bit ICO image. 4-bit: the standard VGA 16 colours
    with a 4x4 ordered dither (the classic Win9x icon look); 8-bit: the icon's own colours (<= 256)."""
    n = rgba.size[0]
    data = list(rgba.getdata())
    if bpp == 4:
        pal = list(VGA16)
    else:
        cols = sorted(set((p[0], p[1], p[2]) for p in data if p[3]))
        pal = [(0, 0, 0)] + [c for c in cols if c != (0, 0, 0)]
        if len(pal) > 256:
            raise ValueError("icon has more than 256 colours")
        pal += [(0, 0, 0)] * (256 - len(pal))
    idx, mask = [], []
    for i, p in enumerate(data):
        if p[3]:
            c = (p[0], p[1], p[2])
            if bpp == 4:
                dth = (_BAYER[(i // n & 3) * 4 + (i % n & 3)] + 0.5) / 16.0 - 0.5
                c = tuple(clamp8(v + dth * 48) for v in c)
                idx.append(VGA_USE[nearest([VGA16[j] for j in VGA_USE], c)])
            else:
                idx.append(nearest(pal, c))
            mask.append(0)
        else:
            idx.append(0)
            mask.append(1)
    return pal, idx, mask


def ico_bytes(images):
    """images [(n, bpp, palette, indices, mask)] -> Win98 .ico (BMP DIBs: XOR bitmap + AND mask, no PNG).
    (assetkit has no ICO writer; small packer kept here, proposed for assetkit.image in the A10 report.)"""
    blobs = []
    for (n, bpp, pal, idx, mask) in images:
        row = ((n * bpp + 31) // 32) * 4
        xor = bytearray()
        for y in range(n - 1, -1, -1):
            r = bytearray(row)
            for x in range(n):
                v = idx[y * n + x]
                if bpp == 8:
                    r[x] = v
                else:
                    r[x >> 1] |= (v << 4) if (x & 1) == 0 else v
            xor += r
        mrow = ((n + 31) // 32) * 4
        andm = bytearray()
        for y in range(n - 1, -1, -1):
            r = bytearray(mrow)
            for x in range(n):
                if mask[y * n + x]:
                    r[x >> 3] |= 0x80 >> (x & 7)
            andm += r
        ncol = 1 << bpp
        hdr = struct.pack("<IiiHHIIiiII", 40, n, 2 * n, 1, bpp, 0, len(xor) + len(andm), 0, 0, ncol, 0)
        palb = b"".join(struct.pack("<4B", c[2], c[1], c[0], 0) for c in pal[:ncol])
        blobs.append((n, bpp, hdr + palb + bytes(xor) + bytes(andm)))
    out = struct.pack("<HHH", 0, 1, len(blobs))
    off = 6 + 16 * len(blobs)
    for (n, bpp, b) in blobs:
        out += struct.pack("<BBBBHHII", n if n < 256 else 0, n if n < 256 else 0, 16 if bpp == 4 else 0, 0, 1, bpp,
                           len(b), off)
        off += len(b)
    return out + b"".join(b for (_, _, b) in blobs)


# ---------------------------------------------------------------------------------------------------- previews
_BAYER = (0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5)


def _bayer_img(w, h, scale=1.0, bias=0.0):
    row = [None] * 4
    for yy in range(4):
        row[yy] = bytes(clamp8(_BAYER[yy * 4 + (x & 3)] * scale + bias) for x in range(w))
    return Image.frombytes("L", (w, h), b"".join(row[y & 3] for y in range(h)))


def eng4444(img):
    """What r_d3d6.cpp pack_px does to a premultiplied RGBA texture in 4444: RGB + Bayer (0..15), truncate to 4 bits;
    alpha truncated (no dither); expanded x 17 on the card."""
    w, h = img.size
    d = _bayer_img(w, h)
    r, g, b, a = img.split()
    q = lambda v: (v >> 4) * 17  # noqa: E731
    return Image.merge("RGBA", [ImageChops.add(c, d).point(q) for c in (r, g, b)] + [a.point(q)])


def frame565(img):
    """16-bit frame buffer look (565 with ordered dither)."""
    w, h = img.size
    bands = []
    for c, bits in zip(img.split(), (5, 6, 5)):
        mx = (1 << bits) - 1
        step = 255.0 / mx
        dimg = _bayer_img(w, h, step / 16.0, step / 32.0)
        c2 = ImageChops.add(c, dimg, 1.0, -int(step / 2))
        bands.append(c2.point(lambda v, mx=mx: int(int(v * mx / 255.0 + 0.5) * 255.0 / mx + 0.5)))
    return Image.merge("RGB", bands)


def tint(src, col, k=1.0):
    """Premultiplied white(ish) sprite x straight colour col x fade k."""
    r, g, b, a = src.split()
    ch = [ImageChops.multiply(x, Image.new("L", src.size, clamp8(cc * k))) for x, cc in zip((r, g, b), col)]
    return Image.merge("RGBA", ch + [scale_l(a, k) if k != 1.0 else a])


def scale_premul(img, f):
    """GPU-like bilinear scaling of a premultiplied texture (bands independently, no prefilter)."""
    w, h = img.size
    nw, nh = max(1, int(round(w * f))), max(1, int(round(h * f)))
    inv = 1.0 / f
    data = (inv, 0, 0.5 * inv - 0.5, 0, inv, 0.5 * inv - 0.5)
    return Image.merge("RGBA", [b.transform((nw, nh), Image.AFFINE, data, resample=Image.BILINEAR)
                                for b in img.split()])


class HudFont(object):
    """Reads a hud_atlas_<L> set (txt + pages) to draw text in the previews, the engine way."""

    def __init__(self, kit, lines):
        self.ok = False
        p = kit.find("tex/hud_atlas_%d.txt" % lines)
        if not p:
            return
        self.fonts, self.glyphs, self.pages = {}, {}, {}
        with open(p, "rb") as f:
            for raw in f.read().decode("ascii").splitlines():
                t = raw.split()
                if not t or t[0].startswith("#"):
                    continue
                if t[0] == "page":
                    tp = kit.find("tex/%s.bmp" % t[2])
                    rgb, _ = I.read_bmp(tp)
                    al, _ = I.read_bmp(I.alpha_path(tp))
                    rgb = rgb.convert("RGBA")
                    rgb.putalpha(al.convert("L"))
                    self.pages[int(t[1])] = eng4444(rgb)
                elif t[0] == "font":
                    v = [int(x) for x in t[1:]]
                    self.fonts[v[0]] = {"cap": v[1], "oy": v[3], "adv": v[4], "gap": v[6], "page": v[10]}
                elif t[0] == "g":
                    v = [int(x) for x in t[1:]]
                    self.glyphs[(v[0], v[1])] = v[2:]
        self.ok = True

    def width(self, f, s):
        if not s:
            return 0
        tot = 0
        for ch in s:
            g = self.glyphs.get((f, ord(ch))) or self.glyphs.get((f, ord("?")))
            tot += g[5]
        return tot - self.fonts[f]["gap"]

    def draw(self, fr, x, y, f, col, s, align=0, k=1.0):
        if align == 1:
            x -= self.width(f, s) // 2
        elif align == 2:
            x -= self.width(f, s)
        F = self.fonts[f]
        page = self.pages[F["page"]]
        for ch in s:
            g = self.glyphs.get((f, ord(ch))) or self.glyphs.get((f, ord("?")))
            gx, gy, gw, gh, gox, adv = g
            if gw:
                fr.over(tint(page.crop((gx, gy, gx + gw, gy + gh)), col, k), x + gox, y + F["oy"])
            x += adv


class Frame(object):
    def __init__(self, bg):
        self.img = bg.convert("RGB").copy()

    def over(self, src, x, y):
        x, y = int(x), int(y)
        W, H = self.img.size
        w, h = src.size
        x0, y0, x1, y1 = max(0, x), max(0, y), min(W, x + w), min(H, y + h)
        if x1 <= x0 or y1 <= y0:
            return
        s = src.crop((x0 - x, y0 - y, x1 - x, y1 - y))
        d = self.img.crop((x0, y0, x1, y1))
        r, g, b, a = s.split()
        inv = ImageChops.invert(a)
        d = ImageChops.add(ImageChops.multiply(d, Image.merge("RGB", (inv, inv, inv))), Image.merge("RGB", (r, g, b)))
        self.img.paste(d, (x0, y0))


class Screen(object):
    """Preview composer: virtual 640x480 layout -> screen px, front kit + HUD fonts of the matching set."""

    def __init__(self, kit, w, h, bg, ui_pages, ui_recs, logo_sets):
        self.w, self.h = w, h
        self.k = h / 480.0
        self.L = max(L for L in UI_SETS if L <= h)
        self.fr = Frame(bg.resize((w, h), Image.BICUBIC))
        self.font = HudFont(kit, self.L)
        self.page = eng4444(ui_pages[self.L])
        self.recs = dict((r[0], r) for r in ui_recs[self.L])
        self.logo_sets = logo_sets

    def px(self, v):
        return int(round(v * self.k))

    def crop(self, name):
        r = self.recs[name]
        return self.page.crop((r[1], r[2], r[1] + r[3], r[2] + r[4])), r

    def text(self, x, y, f, col, s, align=0, k=1.0):
        self.font.draw(self.fr, self.px(x), self.px(y), f, col, s, align, k)

    def text_w(self, f, s):
        return self.font.width(f, s) / self.k

    def sprite(self, name, ax, ay, col=WHITE):
        img, r = self.crop(name)
        if r[12]:
            img = tint(img, col)
        self.fr.over(img, self.px(ax) - r[5], self.px(ay) - r[6])

    def sprite_px(self, name, x, y, col=WHITE):
        img, r = self.crop(name)
        if r[12]:
            img = tint(img, col)
        self.fr.over(img, x, y)

    def nine(self, name, x0, y0, x1, y1, col=WHITE):
        img, r = self.crop(name)
        if r[12]:
            img = tint(img, col)
        l, t, rr, b = r[8], r[9], r[10], r[11]
        X0, Y0, X1, Y1 = self.px(x0), self.px(y0), self.px(x1), self.px(y1)
        w, h = img.size
        xs = [(0, l, X0, X0 + l), (l, w - rr, X0 + l, X1 - rr), (w - rr, w, X1 - rr, X1)]
        ys = [(0, t, Y0, Y0 + t), (t, h - b, Y0 + t, Y1 - b), (h - b, h, Y1 - b, Y1)]
        for (sx0, sx1, dx0, dx1) in xs:
            for (sy0, sy1, dy0, dy1) in ys:
                if dx1 > dx0 and dy1 > dy0:
                    piece = img.crop((sx0, sy0, sx1, sy1)).resize((dx1 - dx0, dy1 - dy0), Image.NEAREST)
                    self.fr.over(piece, dx0, dy0)

    def three(self, name, x0, y, x1, col=WHITE, yc=None):
        """3-slice from x0 to x1 (virtual); top at y (or vertically centred on yc)."""
        img, r = self.crop(name)
        if r[12]:
            img = tint(img, col)
        l, rr = r[8], r[10]
        w, h = img.size
        X0, X1 = self.px(x0), self.px(x1)
        Y = self.px(yc) - h // 2 if yc is not None else self.px(y)
        for (sx0, sx1, dx0, dx1) in ((0, l, X0, X0 + l), (l, w - rr, X0 + l, X1 - rr), (w - rr, w, X1 - rr, X1)):
            if dx1 > dx0:
                self.fr.over(img.crop((sx0, 0, sx1, h)).resize((dx1 - dx0, h), Image.NEAREST), dx0, Y)

    def rect(self, x0, y0, x1, y1, col, a):
        X0, Y0, X1, Y1 = self.px(x0), self.px(y0), self.px(x1), self.px(y1)
        c = tuple(clamp8(v * a) for v in col)
        self.fr.over(Image.new("RGBA", (X1 - X0, Y1 - Y0), c + (clamp8(255 * a),)), X0, Y0)

    def logo(self, x=LOGO_X, y=LOGO_Y):
        name = "hi" if self.h >= LOGO_HI_MIN_H else "lo"
        lines, canvas = self.logo_sets[name]
        f = self.h / float(lines)
        img = canvas if abs(f - 1.0) < 1e-6 else scale_premul(canvas, f)
        self.fr.over(img, self.px(x), self.px(y))

    def done(self):
        return frame565(self.fr.img)


def load_bg(kit, rel, fallback_top=(70, 120, 190), fallback_bot=(30, 60, 100)):
    p = os.path.join(kit.root, rel)
    if os.path.exists(p):
        return Image.open(p).convert("RGB")
    g = I.gradient(800, 600, fallback_top, fallback_bot)
    return g.convert("RGB")


MENU_ITEMS = ("FLY", "OPTIONS", "RECORDS", "QUIT")


def title_menu(sc, sel=0):
    """Title layout (virtual): logo at (90,30) 460x100; menu panel 232..408 x 268..392; items centred at x 320,
    cap tops 284 + 24 i (large font); bar 240..400 centred on the item; marker tip 8 px inside the bar."""
    sc.logo()
    sc.nine("panel", 232, 268, 408, 392)
    for i, it in enumerate(MENU_ITEMS):
        y = 284 + 24 * i
        if i == sel:
            sc.three("bar", 240, 0, 400, yc=y + 6.5)
            sc.sprite("marker", 254, y + 6.5, EMBER)
            sc.text(320, y, 1, WHITE, it, 1)
        else:
            sc.text(320, y, 1, GREEN, it, 1)
    sc.text(320, 452, 0, GREY, "ARROWS SELECT    ENTER OK    ESC BACK", 1)
    sc.text(620, 452, 0, GREY, "V1.0", 2)


def options_controls(sc):
    """Options panel: 96..544 x 52..428, title strip, tabs, 13 rows of 19 (cap tops 124 + 19 i)."""
    sc.rect(0, 0, 640, 480, (0, 0, 0), 0.35)
    sc.nine("panel", 96, 52, 544, 428)
    sc.three("title", 100, 56, 540)
    sc.text(100 + sc.recs["title"][8] / sc.k + 4, 59.5, 1, WHITE, "OPTIONS")
    tabs = (("CONTROLS", 112, 212, True), ("VIDEO", 218, 290, False), ("SOUND", 296, 368, False))
    for name, x0, x1, on in tabs:
        sc.nine("tab_on" if on else "tab", x0, 86, x1, 104)
        sc.text((x0 + x1) / 2.0, 91, 0, WHITE if on else GREEN, name, 1)
    rows = (("PITCH UP", "UP"), ("PITCH DOWN", "DOWN"), ("ROLL LEFT", "LEFT"), ("ROLL RIGHT", "RIGHT"),
            ("FAST - AFTERBURNER", "A"), ("SLOW - BRAKE", "Z"), ("TARGET / LOCK", "S"), ("FIRE", "SPACE"),
            ("NEXT WEAPON", "D"), ("FLARES", "F"), ("VIEW", "C"), ("LOOK AT TARGET", "V"))
    sel, hot = 4, 7
    for i, (lab, key) in enumerate(rows):
        y = 116 + 19 * i
        if i == sel:
            sc.three("bar", 104, 0, 536, yc=y + 6.5)
            sc.sprite("marker", 116, y + 6.5, EMBER)
            sc.text(124, y, 1, WHITE, lab)
        else:
            sc.text(124, y, 1, GREEN, lab)
        if i == hot:
            sc.nine("keycap_hot", 400, y - 2, 520, y + 15)
            sc.text(460, y + 2, 0, EMBER_HOT, "PRESS A KEY", 1)
        else:
            sc.nine("keycap", 400, y - 2, 520, y + 15)
            sc.text(460, y, 1, WHITE, key, 1)
    sc.sprite("arrow_d", 528, 338, GREEN)
    y = 116 + 19 * 12 + 10
    sc.three("divider", 112, y - 7, 528, GREEN)
    sc.text(124, y, 1, GREEN, "INVERT PITCH")
    sc.sprite("arrow_l", 416, y + 6.5, GREEN)
    sc.text(460, y, 1, WHITE, "OFF", 1)
    sc.sprite("arrow_r", 504, y + 6.5, GREEN)
    sc.three("divider", 112, 396, 528, GREEN)
    sc.text(320, 406, 0, GREY, "ENTER CHANGE    DEL DEFAULT    ESC BACK", 1)


def options_video_sound(sc):
    sc.rect(0, 0, 640, 480, (0, 0, 0), 0.35)
    sc.nine("panel", 96, 52, 544, 428)
    sc.three("title", 100, 56, 540)
    sc.text(100 + sc.recs["title"][8] / sc.k + 4, 59.5, 1, WHITE, "OPTIONS")
    tabs = (("CONTROLS", 112, 212, False), ("VIDEO", 218, 290, False), ("SOUND", 296, 368, True))
    for name, x0, x1, on in tabs:
        sc.nine("tab_on" if on else "tab", x0, 86, x1, 104)
        sc.text((x0 + x1) / 2.0, 91, 0, WHITE if on else GREEN, name, 1)
    sc.text(124, 120, 0, EMBER, "VIDEO")
    vrows = (("PRESET", "LOW"), ("RESOLUTION", "800X600"), ("DEFAULT VIEW", "COCKPIT"))
    for i, (lab, val) in enumerate(vrows):
        y = 136 + 22 * i
        sc.text(124, y, 1, GREEN, lab)
        sc.sprite("arrow_l", 396, y + 6.5, GREEN)
        sc.text(460, y, 1, WHITE, val, 1)
        sc.sprite("arrow_r", 524, y + 6.5, GREEN)
    sc.three("divider", 112, 210, 528, GREEN)
    sc.text(124, 222, 0, EMBER, "SOUND")
    srows = (("MASTER", 8), ("EFFECTS", 10), ("ENGINE", 6), ("WARNING TONES", 9))
    for i, (lab, n) in enumerate(srows):
        y = 238 + 22 * i
        if i == 1:
            sc.three("bar", 104, 0, 536, yc=y + 6.5)
            sc.sprite("marker", 116, y + 6.5, EMBER)
            sc.text(124, y, 1, WHITE, lab)
        else:
            sc.text(124, y, 1, GREEN, lab)
        for j in range(10):
            sc.sprite("seg" if j < n else "seg_off", 400 + 12 * j, y + 1, WHITE if (i == 1 and j < n) else GREEN)
    y = 238 + 22 * 4 + 6
    sc.text(124, y, 1, GREEN, "MUTE WHEN PAUSED")
    sc.sprite("check_on", 405, y + 6.5, GREEN)
    sc.three("divider", 112, 396, 528, GREEN)
    sc.text(320, 406, 0, GREY, "LEFT RIGHT CHANGE    ESC BACK", 1)


def results_panel(sc):
    """Results panel: 120..520 x 40..440."""
    sc.rect(0, 0, 640, 480, (0, 0, 0), 0.30)
    sc.nine("panel", 120, 40, 520, 440)
    sc.three("title", 124, 44, 516)
    sc.text(124 + sc.recs["title"][8] / sc.k + 4, 47.5, 1, WHITE, "MISSION COMPLETE")
    sc.text(500, 49.5, 0, GREEN, "SORTIE - NORMAL", 2)
    rows = (("TIME", "07:42"), ("KILLS", "6"), ("AIM-120  HITS / FIRED", "5 / 8"), ("AIM-9  HITS / FIRED", "1 / 3"),
            ("DEATHS", "1"))
    for i, (lab, val) in enumerate(rows):
        y = 82 + 21 * i
        sc.text(144, y, 1, GREEN, lab)
        sc.text(496, y, 1, WHITE, val, 2)
    sc.three("divider", 136, 190, 504, GREEN)
    score = (("KILLS  6 X 1000", "6000"), ("BVR BONUS  3 X 300", "900"), ("DEATHS  1 X -1500", "-1500"),
             ("TIME BONUS", "690"), ("ACCURACY  6 / 11", "1090"))
    for i, (lab, val) in enumerate(score):
        y = 202 + 18 * i
        sc.text(144, y, 0, GREEN, lab)
        sc.text(496, y, 0, WHITE, val, 2)
    sc.three("divider", 136, 300, 504, GREEN)
    sc.text(144, 314, 1, GREEN, "SCORE")
    sc.text(496, 314, 1, WHITE, "7180", 2)
    sc.text(144, 338, 1, GREEN, "BEST - NORMAL")
    sc.text(496, 338, 1, WHITE, "7180", 2)
    sc.text(496, 358, 0, EMBER, "NEW RECORD", 2)
    sc.three("bar", 248, 0, 392, yc=404 + 6.5)
    sc.sprite("marker", 262, 404 + 6.5, EMBER)
    sc.text(320, 404, 1, WHITE, "CONTINUE", 1)


def pause_panel(sc, small):
    """Pause: veil, panel 176..464 x 128..356 with the small logo (260 virtual wide) on top."""
    sc.rect(0, 0, 640, 480, (0, 0, 0), 0.40)
    sc.nine("panel", 176, 128, 464, 356)
    img, (u0, v0, w, h) = small
    lw = 260.0
    f = lw * sc.k / w
    sc.fr.over(scale_premul(img.crop((u0, v0, u0 + w, v0 + h)), f), sc.px(320 - lw / 2.0), sc.px(140))
    items = ("RESUME", "RESTART ON RUNWAY", "OPTIONS", "QUIT TO MENU")
    for i, it in enumerate(items):
        y = 230 + 26 * i
        if i == 0:
            sc.three("bar", 196, 0, 444, yc=y + 6.5)
            sc.sprite("marker", 210, y + 6.5, EMBER)
            sc.text(320, y, 1, WHITE, it, 1)
        else:
            sc.text(320, y, 1, GREEN, it, 1)


def preview_atlas(page, recs, L):
    bg = I.gradient(512, 512, (40, 70, 110), (8, 10, 14))
    fr = Frame(bg)
    big = page.resize((512, 512), Image.NEAREST)
    fr.over(big, 0, 0)
    out = Image.new("RGB", (512 + 512, 512), (0, 0, 0))
    out.paste(fr.img, (0, 0))
    chk = I.checker(512, 512, 16, (150, 150, 150), (110, 110, 110))
    fr2 = Frame(chk)
    fr2.over(big, 0, 0)
    out.paste(fr2.img, (512, 0))
    return out


def preview_logo_tiles(tilesets, small_img):
    rows = []
    for name, lines, (pw, ph), tiles in tilesets:
        imgs = [eng4444(img) for (_, img) in tiles]
        w = sum(i.size[0] for i in imgs) + 8 * (len(imgs) + 1)
        h = max(i.size[1] for i in imgs) + 16
        sheet = Image.new("RGB", (w, h), (0, 0, 0))
        x = 8
        for im in imgs:
            chk = I.checker(im.size[0], im.size[1], 8, (150, 150, 150), (110, 110, 110))
            fr = Frame(chk)
            fr.over(im, 0, 0)
            sheet.paste(fr.img, (x, 8))
            x += im.size[0] + 8
        rows.append(sheet)
    chk = I.checker(small_img.size[0], small_img.size[1], 8, (150, 150, 150), (110, 110, 110))
    fr = Frame(chk)
    fr.over(eng4444(small_img), 0, 0)
    rows.append(fr.img)
    W = max(r.size[0] for r in rows)
    H = sum(r.size[1] + 8 for r in rows)
    out = Image.new("RGB", (W, H), (20, 20, 20))
    y = 0
    for r in rows:
        out.paste(r, (0, y))
        y += r.size[1] + 8
    return out


def preview_icon(icons):
    """icons {(n, bpp): rgba as stored} -> sheet: per depth on teal (Win98 desktop) and white, 1x + 6x zoom."""
    out = Image.new("RGB", (4 * 330, 240), (0, 0, 0))
    x = 0
    for bpp in (4, 8):
        for bgc in ((0, 128, 128), (255, 255, 255)):
            panel = Image.new("RGB", (330, 240), bgc)
            im = icons[(32, bpp)]
            big = im.resize((192, 192), Image.NEAREST)
            panel.paste(big, (8, 8), big)
            im = icons[(16, bpp)]
            big = im.resize((96, 96), Image.NEAREST)
            panel.paste(big, (210, 8), big)
            panel.paste(icons[(32, bpp)], (220, 150), icons[(32, bpp)])
            panel.paste(icons[(16, bpp)], (270, 158), icons[(16, bpp)])
            out.paste(panel, (x, 0))
            x += 330
    return out


# ---------------------------------------------------------------------------------------------------- build
def build(out_dir, kit):
    # logo sets
    tilesets, logo_canvas = [], {}
    for name, lines in LOGO_SETS:
        canvas = render_logo(lines / 480.0)
        assert canvas.size == logo_px(lines), (canvas.size, logo_px(lines))
        tiles = cut_tiles(canvas, lines)
        tnames = []
        for i, (t, img) in enumerate(tiles):
            tn = "logo_%s_%d" % (name, i)
            kit.save_bmp("tex/%s.bmp" % tn, img, alpha=True, alpha_bleed=0)
            tnames.append(tn)
        tilesets.append((name, lines, canvas.size, [t for t, _ in tiles], tnames))
        logo_canvas[name] = (lines, canvas, tiles)
        kit.log("logo %s: canvas %dx%d at %d lines, %d tiles %s" % (
            name, canvas.size[0], canvas.size[1], lines, len(tiles),
            " ".join("%dx%d" % t["tex"] for t, _ in tiles)))
    sm = render_logo(SMALL_S)
    small = Image.new("RGBA", SMALL_TEX, (0, 0, 0, 0))
    v0 = (SMALL_TEX[1] - sm.size[1]) // 2
    small.paste(sm, (0, v0))
    kit.save_bmp("tex/logo.bmp", small, alpha=True, alpha_bleed=0)
    kit.save_text("tex/logo.txt", logo_txt(tilesets, sm.size), crlf=True)

    # menu kit
    ui_pages, ui_recs = {}, {}
    for L in UI_SETS:
        U = UISet(L)
        page, recs, used = build_ui_set(U)
        kit.save_bmp("tex/%s.bmp" % U.tex, page, alpha=True, alpha_bleed=0)
        kit.save_text("tex/%s.txt" % U.tex, front_txt(U, recs), crlf=True)
        ui_pages[L], ui_recs[L] = page, recs
        kit.log("front %4d: %d sprites, %d%% of the page" % (L, len(recs), 100 * used // (256 * 256)))

    # icon
    icons, ico_imgs = {}, []
    for n in (32, 16):
        rgba = draw_icon(n)
        for bpp in (4, 8):
            pal, idx, mask = icon_indexed(rgba, bpp)
            ico_imgs.append((n, bpp, pal, idx, mask))
            im = Image.new("RGBA", (n, n))
            im.putdata([(pal[i] + (0 if m else 255,)) for i, m in zip(idx, mask)])
            icons[(n, bpp)] = im
    order = [(32, 4), (16, 4), (32, 8), (16, 8)]
    ico_imgs.sort(key=lambda e: order.index((e[0], e[1])))
    with open(kit.out("ashfall.ico"), "wb") as f:
        f.write(ico_bytes(ico_imgs))

    if not kit.previews:
        return
    logo_sets = dict((name, (lines, canvas_from_tiles(tiles, logo_px(lines)))) for name, (lines, canvas, tiles)
                     in logo_canvas.items())
    small_info = (eng4444(small), (0, v0, sm.size[0], sm.size[1]))
    bg = load_bg(kit, "shots/t10b/new_o03.png")
    for (w, h) in ((640, 480), (800, 600), (1024, 768), (1400, 1050)):
        sc = Screen(kit, w, h, bg, ui_pages, ui_recs, logo_sets)
        title_menu(sc)
        kit.save_preview("front_title_%d.png" % w, sc.done())
    sc = Screen(kit, 800, 600, bg, ui_pages, ui_recs, logo_sets)
    options_controls(sc)
    kit.save_preview("front_options_800.png", sc.done())
    sc = Screen(kit, 800, 600, bg, ui_pages, ui_recs, logo_sets)
    options_video_sound(sc)
    kit.save_preview("front_options_sound_800.png", sc.done())
    sc = Screen(kit, 1024, 768, bg, ui_pages, ui_recs, logo_sets)
    options_controls(sc)
    kit.save_preview("front_options_1024.png", sc.done())
    bg2 = load_bg(kit, "shots/t10b/new_sea50.png")
    sc = Screen(kit, 800, 600, bg2, ui_pages, ui_recs, logo_sets)
    results_panel(sc)
    kit.save_preview("front_results_800.png", sc.done())
    bg3 = load_bg(kit, "shots/m2fix_chase_slots.png")
    sc = Screen(kit, 800, 600, bg3, ui_pages, ui_recs, logo_sets)
    pause_panel(sc, small_info)
    kit.save_preview("front_pause_800.png", sc.done())
    for L in UI_SETS:
        kit.save_preview("front_atlas_%d.png" % L, preview_atlas(eng4444(ui_pages[L]), ui_recs[L], L))
    kit.save_preview("front_logo_tiles.png", preview_logo_tiles(
        [(n, l, s, [(t, img) for t, img in logo_canvas[n][2]]) for n, l, s, _, _ in tilesets], small))
    kit.save_preview("front_icon.png", preview_icon(icons))


def canvas_from_tiles(tiles, size):
    """Rebuild the canvas from the tiles (as the engine sees them: 4444) - proves the layout."""
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    for t, img in tiles:
        (ox, oy), (cw, ch) = t["off"], t["size"]
        out.paste(eng4444(img).crop((1, 1, 1 + cw, 1 + ch)), (ox, oy))
    return out


if __name__ == "__main__":
    from assetkit import build as B
    sys.exit(B.main_for(__file__))
