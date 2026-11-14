"""gen_ground.py - Jet ground, sea, foam and airfield textures (task A3). Outputs: see ASSETS.

    python3 tools/build_assets.py --only ground [--check]      # or: python3 tools/art/gen_ground.py [--check]

Structure, helpers and the seam test follow ../racer/tools/art/gen_ground.py (read-only pattern); the art is new.
All textures are 24-bit BMPs, 16-bit on the device (RGB565 + dither, mipmapped by the engine).

Terrain classes (DESIGN 8.1). Vertex colour = class tint x baked light (islandc), so these are NEAR-NEUTRAL: the
mean colour of each is grey (TERRAIN_MEAN), and the texture carries only relative detail (luminance + small warm /
cool hue shifts). The class tint gives the hue. Tint for a wanted average colour C: tint = C * 255 / mean
(SUGGESTED_TINT below; A6 owns the final values in island/island.txt). Exception: t_lava is NOT neutral (dark crust
with glowing fissures that must keep their colour): give it a light, near-white tint.

  name      size     period   mapping (islandc, chunk-local planar)        mean   content
  t_sand    256x256  32 m     u = x / P, v = z / P                         216    beach sand, wet patches, wrack
  t_grass   256x256  64 m     "                                            184    meadow, shrubs, trees + shadows
  t_rock    256x256  64 m     "                                            168    basalt blocks, cracks, moss
  t_ash     256x256  64 m     "                                            184    tephra/ash, scoria, rills
  t_lava    256x256  64 m     "                                            ~66    basalt crust, glowing fissures

Every period divides the 512 m chunk (DESIGN 8.6) and keeps chunk-local UVs <= 16.
Baked small-scale shading (bush shadows, crack sides, wave facets) assumes the DESIGN 8.4 sun: azimuth 230 deg,
elevation 40 deg, and u = +x (east), v = +z (north) (image down = north). Mirrored mappings still look fine.

Sea (DESIGN 8.3): sea.bmp 256x256, period 128 m (0.5 m texels), u = x / P, v = z / P (+ scroll). NOT neutral:
it is the sea colour (deep blue, mean ~ (27, 72, 108)); the vertex colour only darkens with distance. Swell is
a sum of tileable wave trains travelling toward azimuth 240 deg (wind from ENE): scroll the UV in that direction,
about (-0.87, -0.50) * 0.012 per second in u, v (~1.5 m/s; slow swell). Baked: facet shading for the sun,
sky reflection on tilted facets, sparse sun glints, scattered small whitecaps.

Foam: foam.bmp + foam_a.bmp 256x64, PREMULTIPLIED (colour = white x alpha, black where alpha = 0) for RB_ADD
(ONE/ONE ignores alpha; foam_a is there for RB_BLEND use). u = along the coast, 1 repeat per 64 m; v = across
the strip, 0 = landward edge, 1 = seaward edge (clamp v). Drawn for a strip 40 m wide from 6 m inland of the 0 m
contour (v = 0) to 34 m offshore (v = 1): swash lace on the beach, shore-break band at v ~ 0.15-0.25 (waterline
at v = 0.15), two broken breaker lines further out. Alpha is 0 at both v edges, so the strip edges never show.

Airfield (DESIGN 8.5, base.trk ribbons; textures carry their own colour, vertex colour = light only):
  runway    256x64   u = across the whole 45 m runway (0..1), v = along, 1 repeat per 50 m
                     asphalt + rubber, side stripes 0.9 m (1.0-1.9 m from each edge), centre line 0.9 m dashes:
                     30 m paint from v = 0.2 to 0.8, 20 m gap across the v wrap (switch materials at whole v).
  rwy_thr   256x64   same asphalt and side stripes, piano keys (12 x 1.8 m, 2.7 m centre gap) from 6 to 34.8 m
                     along, no centre line: 0..50 m from each runway end (v = 0 AT the end; flip v at the other end).
                     Opaque on purpose: keyed stripes average away in the mips, these stay a light patch.
  rwy_end   256x64   same asphalt and side stripes, NO centre line: 50..100 m from each end, where the numbers
                     are (ICAO: the centre line starts after the designation).
  taxi      256x64   u = across a 23 m taxiway (0..1), v = along, 1 repeat per 25 m; yellow centre line 0.3 m,
                     double yellow edge lines (0.25 m, 0.25 m apart) 1.5 m in from each edge, shoulders outside.
  apron     128x128  u = x / 32 m, v = z / 32 m: concrete slabs 4 m x 4 m (8 x 8 per repeat), joints, stains.
  rwy_marks 256x128  keyed atlas (exact magenta = transparent, ALPHATEST quads 0.05-0.1 m above the runway
                     ribbon): rects + world sizes + suggested placement in tex/rwy_marks.txt.
"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "tools"))
from PIL import Image  # noqa: E402

from assetkit import image as I  # noqa: E402
from assetkit import mesh as M  # noqa: E402
from assetkit import preview as V  # noqa: E402
from assetkit.rng import Rng, noise_grid, stable_hash32, value_noise_1d  # noqa: E402

SEED = 303
ORDER = 30

SUN_AZ, SUN_EL = 230.0, 40.0       # DESIGN 8.4 default sun (A2's data/sky/light.txt is authoritative for bakes)
WAVE_TO = 240.0                    # swell travels toward WSW (wind from ENE)

TERRAIN = ["t_sand", "t_grass", "t_rock", "t_ash", "t_lava", "t_forest", "t_scrub"]   # A11: + forest, scrub
SIZES = {"t_sand": (256, 256), "t_grass": (256, 256), "t_rock": (256, 256), "t_ash": (256, 256),
         "t_lava": (256, 256), "t_forest": (256, 256), "t_scrub": (256, 256), "sea": (256, 256), "foam": (256, 64),
         "runway": (256, 64), "rwy_end": (256, 64),
         "rwy_thr": (256, 64), "taxi": (256, 64), "apron": (128, 128), "rwy_marks": (256, 128)}
NAMES = TERRAIN + ["sea", "foam", "runway", "rwy_end", "rwy_thr", "taxi", "apron", "rwy_marks"]
ASSETS = ["tex/%s.bmp" % n for n in NAMES] + ["tex/foam_a.bmp", "tex/rwy_marks.txt"]

# metres per repeat (u, v) of each texture (for island.txt / islandc; airfield: u spans the ribbon width)
PERIOD = {"t_sand": (32, 32), "t_grass": (64, 64), "t_rock": (64, 64), "t_ash": (64, 64), "t_lava": (64, 64),
          "t_forest": (64, 64), "t_scrub": (64, 64),
          "sea": (128, 128), "foam": (64, 40), "runway": (45, 50), "rwy_end": (45, 50), "rwy_thr": (45, 50), "taxi": (23, 25),
          "apron": (32, 32)}

# neutral terrain means (grey level of the average texel; t_lava keeps its own colour)
TERRAIN_MEAN = {"t_sand": 216.0, "t_grass": 184.0, "t_rock": 168.0, "t_ash": 184.0, "t_forest": 160.0, "t_scrub": 176.0}

# average on-screen colour wanted on flat sunlit ground (= tint x mean / 255); tints below are derived from it.
# Green island (STATUS 2026-11-13): bright day, green vegetated slopes, rock/ash near the crater, beaches.
TARGET = {"t_sand": (214, 199, 160), "t_grass": (84, 122, 54), "t_rock": (88, 86, 82), "t_ash": (118, 112, 106),
          "t_lava": (230, 222, 214), "t_forest": (48, 82, 40), "t_scrub": (128, 124, 74)}   # t_lava: the tint itself (not neutral)
SHELF_TARGET = (86, 168, 160)            # t_sand under shallow water (seen through the 0.65 sea blend)

PAINT = (236, 236, 230)                  # runway white
YELLOW = (222, 178, 46)                  # taxiway yellow
SEAM_MAX = 1.6                           # seam ratio threshold (racer gen_ground convention)


def suggested_tints():
    out = {}
    for n in TERRAIN:
        if n == "t_lava":
            out[n] = TARGET[n]
        else:
            m = TERRAIN_MEAN[n]
            out[n] = tuple(min(255, int(round(c * 255.0 / m))) for c in TARGET[n])
    m = TERRAIN_MEAN["t_sand"]
    out["shelf"] = tuple(min(255, int(round(c * 255.0 / m))) for c in SHELF_TARGET)
    return out


# ---------------------------------------------------------------------------------------------------- helpers
# (patterns from ../racer/tools/art/gen_ground.py)

def _seed(*names):
    return stable_hash32(SEED, *names) & 0x7FFFFFFF


_NG_CACHE = {}      # callers never modify the returned lists


def ng(w, h, cells, octaves=3, seed=0, kind="fbm", gain=0.5):
    """Flat list of tileable noise in [0, 1] (assetkit.rng.noise_grid, `cells` lattice cells across w).
    Square low-frequency grids are evaluated at >= 4 px per finest lattice cell and upsampled (wrap-aware)."""
    key = (w, h, cells, octaves, seed, kind, gain)
    if key in _NG_CACHE:
        return _NG_CACHE[key]
    g = w
    if w == h:
        need = cells * (2 ** (octaves - 1)) * 4
        while g > 32 and g // 2 >= need:
            g //= 2
    flat = [v for row in noise_grid(g, g if w == h else h, cells, octaves, gain, seed, True, kind) for v in row]
    if g != w:
        flat = resample(flat, g, g, w, h)
    _NG_CACHE[key] = flat
    return flat


def resample(src, sw, sh, w, h):
    """Wrap-aware bilinear resample of a flat float list (keeps tiling)."""
    out = [0.0] * (w * h)
    for y in range(h):
        fy = (y + 0.5) * sh / float(h) - 0.5
        y0 = int(math.floor(fy))
        ty = fy - y0
        r0 = (y0 % sh) * sw
        r1 = ((y0 + 1) % sh) * sw
        o = y * w
        for x in range(w):
            fx = (x + 0.5) * sw / float(w) - 0.5
            x0 = int(math.floor(fx))
            tx = fx - x0
            a = x0 % sw
            b = (x0 + 1) % sw
            t = src[r0 + a] + (src[r0 + b] - src[r0 + a]) * tx
            u = src[r1 + a] + (src[r1 + b] - src[r1 + a]) * tx
            out[o + x] = t + (u - t) * ty
    return out


def aniso(w, h, cx, cy, octaves=3, seed=0, kind="fbm"):
    """Tileable noise with cx lattice cells across (image x) and cy along (image y): cx > cy = streaks along y."""
    if cx * h >= cy * w:
        gw, gh = w, max(2, int(round(w * cy / float(cx))))
    else:
        gw, gh = max(2, int(round(h * cx / float(cy)))), h
    g = ng(gw, gh, cx, octaves, seed, kind)
    if (gw, gh) == (w, h):
        return g
    return resample(g, gw, gh, w, h)


def sstep(e0, e1, x):
    if e0 == e1:
        return 1.0 if x >= e1 else 0.0
    t = (x - e0) / (e1 - e0)
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    return t * t * (3.0 - 2.0 * t)


def mixc(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def clamp8(v):
    return 0 if v < 0 else (255 if v > 255 else int(v + 0.5))


def to_img(w, h, R, G, B):
    out = bytearray(w * h * 3)
    for i in range(w * h):
        out[3 * i] = clamp8(R[i])
        out[3 * i + 1] = clamp8(G[i])
        out[3 * i + 2] = clamp8(B[i])
    return Image.frombytes("RGB", (w, h), bytes(out))


def fields(w, h, fn):
    """fn(i, x, y) -> (r, g, b) floats; returns flat lists R, G, B."""
    n = w * h
    R = [0.0] * n
    G = [0.0] * n
    B = [0.0] * n
    i = 0
    for y in range(h):
        for x in range(w):
            R[i], G[i], B[i] = fn(i, x, y)
            i += 1
    return R, G, B


def compose(w, h, fn):
    R, G, B = fields(w, h, fn)
    return to_img(w, h, R, G, B)


def neutral(w, h, R, G, B, mean):
    """Scale each channel so the texture's average is exactly grey `mean` (detail stays relative)."""
    n = float(w * h)
    out = []
    for ch in (R, G, B):
        m = sum(ch) / n
        k = mean / m
        out.append([v * k for v in ch])
    return to_img(w, h, out[0], out[1], out[2])


def cov(a, b, x0, x1):
    """Fraction of the pixel interval [x0, x1] covered by [a, b] (exact 1D box filter)."""
    lo = a if a > x0 else x0
    hi = b if b < x1 else x1
    return (hi - lo) / (x1 - x0) if hi > lo else 0.0


def cov_wrap(a, b, x0, x1, period):
    return min(1.0, cov(a, b, x0, x1) + cov(a - period, b - period, x0, x1) + cov(a + period, b + period, x0, x1))


def periodic1d(n, period_cells, seed, octaves=2):
    """n samples of smooth 1D noise in [0, 1] that wrap after n samples."""
    out = []
    for i in range(n):
        x = (i + 0.5) * period_cells / float(n)
        v = 0.0
        amp = 1.0
        norm = 0.0
        per = period_cells
        f = 1.0
        for o in range(octaves):
            v += amp * value_noise_1d(x * f, seed + o * 31, per)
            norm += amp
            amp *= 0.5
            f *= 2.0
            per *= 2
        out.append(v / norm)
    lo, hi = min(out), max(out)
    return [(v - lo) / ((hi - lo) or 1.0) for v in out]


def worley(w, h, n, seed, jitter=0.9, warp=None):
    """Tileable cellular noise on an n x n grid of jittered points -> flat lists (f1, edge = f2 - f1, cell value).
    Distances in cell units. warp = (dx, dy) flat lists in cell units."""
    r = Rng(seed, "worley")
    pts = []
    for gy in range(n):
        for gx in range(n):
            pts.append((gx + 0.5 + (r.random() - 0.5) * jitter, gy + 0.5 + (r.random() - 0.5) * jitter, r.random()))
    f1s = [0.0] * (w * h)
    eds = [0.0] * (w * h)
    vals = [0.0] * (w * h)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            fx = (x + 0.5) * n / float(w)
            fy = (y + 0.5) * n / float(h)
            if warp is not None:
                fx += warp[0][i]
                fy += warp[1][i]
            cx = int(math.floor(fx))
            cy = int(math.floor(fy))
            d1 = d2 = 1e9
            v1 = 0.0
            for oy in (-1, 0, 1):
                gy = cy + oy
                for ox in (-1, 0, 1):
                    gx = cx + ox
                    px, py, pv = pts[(gy % n) * n + (gx % n)]
                    dx = px + (gx - gx % n) - fx
                    dy = py + (gy - gy % n) - fy
                    d = dx * dx + dy * dy
                    if d < d1:
                        d2 = d1
                        d1 = d
                        v1 = pv
                    elif d < d2:
                        d2 = d
            f1s[i] = math.sqrt(d1)
            eds[i] = math.sqrt(d2) - f1s[i]
            vals[i] = v1
    return f1s, eds, vals


def shift(src, w, h, dx, dy):
    """Flat list sampled at (x + dx, y + dy), wrapping (integer offsets)."""
    out = [0.0] * (w * h)
    for y in range(h):
        sy = ((y + dy) % h) * w
        o = y * w
        for x in range(w):
            out[o + x] = src[sy + (x + dx) % w]
    return out


def sun_img_dir():
    """Unit (dx, dy) in image pixels pointing toward the sun (u = east, v = north = image down)."""
    a = math.radians(SUN_AZ)
    return math.sin(a), math.cos(a)


def seam_ratio(img, axes="uv"):
    """Colour step across the wrap edges / mean step between the neighbouring rows (columns): ~1.0 when
    seamless, >> 1 when the opposite edges don't match."""
    w, h = img.size
    px = img.convert("RGB").load()

    def d(a, b):
        return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
    edge = near = 0.0
    if "u" in axes:
        edge += sum(d(px[w - 1, y], px[0, y]) for y in range(h))
        near += sum(d(px[w - 2, y], px[w - 1, y]) + d(px[0, y], px[1, y]) for y in range(h)) * 0.5
    if "v" in axes:
        edge += sum(d(px[x, h - 1], px[x, 0]) for x in range(w))
        near += sum(d(px[x, h - 2], px[x, h - 1]) + d(px[x, 0], px[x, 1]) for x in range(w)) * 0.5
    return edge / max(1e-6, near)


def mean_rgb(img):
    w, h = img.size
    data = img.convert("RGB").tobytes()
    n = float(w * h)
    return tuple(sum(data[k::3]) / n for k in range(3))


# ---------------------------------------------------------------------------------------------------- terrain

def t_sand():
    """Beach sand, 32 m (0.125 m texels): low contrast, damp patches, wrack lines, shell grit."""
    w = h = 256
    s = _seed("t_sand")
    mot = ng(w, h, 6, 3, s + 1)
    rip = ng(w, h, 48, 2, s + 2, kind="ridged")
    gr = ng(w, h, 128, 1, s + 3)
    gr2 = ng(w, h, 64, 2, s + 4)
    wet = ng(w, h, 4, 3, s + 5)
    wr = ng(w, h, 20, 3, s + 6)
    sh = ng(w, h, 64, 1, s + 7)

    def fn(i, x, y):
        lum = 1.0 + 0.035 * (mot[i] - 0.5) * 2.0 + 0.03 * (rip[i] - 0.5) * 2.0
        lum += 0.05 * (gr[i] - 0.5) * 2.0 + 0.03 * (gr2[i] - 0.5) * 2.0
        c = (1.0, 1.0, 1.0)
        wt = 0.35 * sstep(0.55, 0.9, wet[i])                       # damp sand: darker, slightly cooler
        c = mixc(c, (0.86, 0.87, 0.90), wt)
        k = sstep(0.86, 0.95, wr[i]) * 0.55                        # wrack: seaweed / driftwood bits
        c = mixc(c, (0.62, 0.60, 0.50), k)
        c = mixc(c, (1.10, 1.09, 1.06), 0.6 * sstep(0.86, 0.95, sh[i]))   # shell grit
        return (c[0] * lum, c[1] * lum, c[2] * lum)
    R, G, B = fields(w, h, fn)
    return neutral(w, h, R, G, B, TERRAIN_MEAN["t_sand"])


def t_grass():
    """Green slopes from above, 64 m (0.25 m texels): meadow (lush/dry), shrub clusters, scattered trees with
    sun-side highlights and NE shadows, bare soil spots. Tile-scale variation kept weak (islandc varies tint)."""
    w = h = 256
    s = _seed("t_grass")
    patch = ng(w, h, 4, 3, s + 1)
    meadow = ng(w, h, 10, 3, s + 2)
    gr = ng(w, h, 128, 1, s + 3)
    gr2 = ng(w, h, 48, 2, s + 4)
    soil = ng(w, h, 14, 3, s + 5)
    dens = ng(w, h, 12, 2, s + 6)              # shrub density (small clusters; even at tile scale so the
    tdens = ng(w, h, 8, 2, s + 7)              # 64 m repeat doesn't show as a grid from altitude)
    bw = (ng(w, h, 8, 2, s + 8), ng(w, h, 8, 2, s + 9))
    warp = ([(v - 0.5) * 0.6 for v in bw[0]], [(v - 0.5) * 0.6 for v in bw[1]])
    bf1, _be, bv = worley(w, h, 24, s + 10, warp=warp)       # shrubs: 2.7 m cells
    tf1, _te, tv = worley(w, h, 9, s + 11, jitter=0.8)       # trees: 7 m cells
    leaf = ng(w, h, 96, 1, s + 12)
    canopy = [0.0] * (w * h)
    kind = [0] * (w * h)
    for i in range(w * h):
        rb = 0.22 + 0.22 * bv[i]                              # shrub radius in cells (0.6-1.2 m)
        b = sstep(rb, rb * 0.55, bf1[i]) * sstep(0.42, 0.62, dens[i] * 0.8 + bv[i] * 0.3)
        rt = 0.20 + 0.14 * tv[i]                              # tree crown radius (1.4-2.4 m)
        t = sstep(rt, rt * 0.6, tf1[i]) * sstep(0.5, 0.7, tdens[i] * 0.7 + tv[i] * 0.45)
        if t > b:
            canopy[i] = t
            kind[i] = 2
        else:
            canopy[i] = b
            kind[i] = 1 if b > 0.0 else 0
    sx, sy = sun_img_dir()
    toward = shift(canopy, w, h, int(round(sx * 4)), int(round(sy * 4)))      # canopy 1 m toward the sun
    away = shift(canopy, w, h, -int(round(sx * 2)), -int(round(sy * 2)))
    shad = shift(canopy, w, h, int(round(sx * 7)), int(round(sy * 7)))         # shadow caster 1.75 m sunward

    def fn(i, x, y):
        c = mixc((0.93, 1.00, 0.98), (1.10, 1.05, 0.84), sstep(0.3, 0.8, meadow[i]))   # lush <-> dry grass
        lum = 1.0 + 0.025 * (patch[i] - 0.5) * 2.0 + 0.07 * (gr[i] - 0.5) * 2.0 + 0.05 * (gr2[i] - 0.5) * 2.0
        sl = sstep(0.80, 0.92, soil[i]) * (1.0 - canopy[i])
        c = mixc(c, (1.42, 1.08, 0.86), sl * 0.8)
        lum *= 1.0 + 0.10 * sl
        lum *= 1.0 - 0.38 * max(0.0, shad[i] - canopy[i])                         # cast shadow on the ground
        cv = canopy[i]
        if cv > 0.0:
            if kind[i] == 2:
                cc, cl = (0.84, 1.0, 0.98), 0.55
            else:
                cc, cl = (0.90, 1.0, 0.92), 0.66
            lit = 1.0 + 0.45 * (away[i] - toward[i])                              # sun side of the crown brighter
            cl *= lit * (0.9 + 0.2 * leaf[i])
            c = mixc(c, cc, cv)
            lum = lum * (1.0 - cv) + cl * cv
        return (c[0] * lum, c[1] * lum, c[2] * lum)
    R, G, B = fields(w, h, fn)
    return neutral(w, h, R, G, B, TERRAIN_MEAN["t_grass"])


def t_rock():
    """Basalt slopes/cliffs, 64 m: fractured blocks (lit by facet), dark cracks with shadow side, moss and grass
    in cracks (green island), pale lichen. Isotropic (planar UVs land on slopes of every orientation)."""
    w = h = 256
    s = _seed("t_rock")
    wx = ng(w, h, 4, 3, s + 1)
    wy = ng(w, h, 4, 3, s + 2)
    warp = ([(v - 0.5) * 0.8 for v in wx], [(v - 0.5) * 0.8 for v in wy])
    _f1, ed, fv = worley(w, h, 10, s + 3, warp=warp)             # 6.4 m blocks
    _g1, ed2, gv = worley(w, h, 26, s + 4, jitter=0.95)           # 2.5 m fragments
    cm = ng(w, h, 6, 2, s + 5)
    base = ng(w, h, 8, 4, s + 6)
    gr = ng(w, h, 96, 1, s + 7)
    gr2 = ng(w, h, 40, 2, s + 8)
    veg = ng(w, h, 7, 3, s + 9)
    lic = ng(w, h, 12, 3, s + 10)
    crack = [0.0] * (w * h)
    for i in range(w * h):
        a = sstep(0.07, 0.0, ed[i]) * sstep(0.35, 0.6, cm[i])
        b = sstep(0.05, 0.0, ed2[i]) * 0.55 * sstep(0.5, 0.75, cm[i])
        crack[i] = max(a, b)
    sx, sy = sun_img_dir()
    cs = shift(crack, w, h, int(round(sx * 3)), int(round(sy * 3)))

    def fn(i, x, y):
        lum = 0.86 + 0.24 * fv[i] + 0.08 * (gv[i] - 0.5)          # facet planes
        lum *= 1.0 + 0.10 * (base[i] - 0.5) * 2.0
        lum *= 1.0 + 0.08 * (gr[i] - 0.5) * 2.0 + 0.05 * (gr2[i] - 0.5) * 2.0
        c = mixc((1.0, 1.0, 1.0), (1.05, 1.0, 0.95), fv[i] * 0.5)  # weathered faces a touch warmer
        lum *= 1.0 - 0.50 * crack[i] - 0.18 * max(0.0, cs[i] - crack[i])  # crack + its shadowed lip
        v = sstep(0.5, 0.75, veg[i]) * min(1.0, crack[i] * 1.6 + 0.25 * sstep(0.7, 0.85, veg[i]))
        c = mixc(c, (0.86, 1.10, 0.84), v * 0.85)
        lum *= 1.0 - 0.08 * v
        c = mixc(c, (1.06, 1.06, 0.98), 0.5 * sstep(0.78, 0.92, lic[i]) * (1.0 - crack[i]))
        lum *= 1.0 + 0.16 * sstep(0.78, 0.92, lic[i]) * (1.0 - crack[i])
        return (c[0] * lum, c[1] * lum, c[2] * lum)
    R, G, B = fields(w, h, fn)
    return neutral(w, h, R, G, B, TERRAIN_MEAN["t_rock"])


def t_ash():
    """Upper cone (> 700 m): tephra and ash, 64 m: soft mottling, reddish scoria patches, cinder grit, pale
    pumice, erosion rills (isotropic ridged noise), a few lava bombs with shadows."""
    w = h = 256
    s = _seed("t_ash")
    m1 = ng(w, h, 5, 3, s + 1)
    m2 = ng(w, h, 14, 3, s + 2)
    sco = ng(w, h, 7, 3, s + 3)
    cin = ng(w, h, 96, 1, s + 4)
    gr = ng(w, h, 128, 1, s + 5)
    pum = ng(w, h, 40, 2, s + 6)
    rill = ng(w, h, 6, 4, s + 7, kind="ridged")
    bf1, _be, bv = worley(w, h, 16, s + 8)
    bomb = [sstep(0.16, 0.10, bf1[i]) * (1.0 if bv[i] > 0.72 else 0.0) for i in range(w * h)]
    sx, sy = sun_img_dir()
    bs = shift(bomb, w, h, int(round(sx * 3)), int(round(sy * 3)))

    def fn(i, x, y):
        lum = 1.0 + 0.07 * (m1[i] - 0.5) * 2.0 + 0.06 * (m2[i] - 0.5) * 2.0 + 0.06 * (gr[i] - 0.5) * 2.0
        c = mixc((0.99, 1.0, 1.02), (1.08, 0.98, 0.92), sstep(0.5, 0.85, sco[i]))     # grey ash <-> red scoria
        lum *= 1.0 - 0.14 * sstep(0.70, 0.92, cin[i])
        lum *= 1.0 + 0.14 * sstep(0.84, 0.96, pum[i])
        r = sstep(0.82, 0.96, rill[i])
        lum *= 1.0 - 0.16 * r
        lum *= 1.0 - 0.30 * max(0.0, bs[i] - bomb[i])
        b = bomb[i]
        if b > 0.0:
            c = mixc(c, (0.95, 0.97, 1.0), b)
            lum = lum * (1.0 - b) + 0.62 * b
        return (c[0] * lum, c[1] * lum, c[2] * lum)
    R, G, B = fields(w, h, fn)
    return neutral(w, h, R, G, B, TERRAIN_MEAN["t_ash"])


def t_lava():
    """Lava channels and crater floor, 64 m. NOT neutral: dark basalt crust plates with orange-red glowing
    fissures and a few dull hot spots (use a light tint so the glow survives)."""
    w = h = 256
    s = _seed("t_lava")
    wx = ng(w, h, 5, 3, s + 1)
    wy = ng(w, h, 5, 3, s + 2)
    warp = ([(v - 0.5) * 0.9 for v in wx], [(v - 0.5) * 0.9 for v in wy])
    _f1, ed, fv = worley(w, h, 12, s + 3, warp=warp)
    _g1, ed2, _gv = worley(w, h, 30, s + 4)
    gm = ng(w, h, 6, 3, s + 5)
    gr = ng(w, h, 96, 1, s + 6)
    gr2 = ng(w, h, 32, 2, s + 7)
    hot = ng(w, h, 4, 3, s + 8)
    sheen = ng(w, h, 20, 2, s + 9)
    crust = (54, 50, 49)
    core, rim = (255, 196, 92), (214, 64, 18)

    def fn(i, x, y):
        lum = 0.85 + 0.25 * fv[i]
        lum *= 1.0 + 0.16 * (gr[i] - 0.5) * 2.0 + 0.10 * (gr2[i] - 0.5) * 2.0
        c = mixc(crust, (70, 72, 80), 0.5 * sstep(0.7, 0.9, sheen[i]))           # glassy bluish sheen
        c = (c[0] * lum, c[1] * lum, c[2] * lum)
        c = mixc(c, (22, 20, 20), sstep(0.05, 0.0, ed2[i]) * 0.5)                # small dark cracks
        g = sstep(0.38, 0.62, gm[i])                                             # which fissures glow
        e = ed[i]
        glow_rim = sstep(0.12, 0.02, e) * g
        glow_core = sstep(0.04, 0.0, e) * g
        c = mixc(c, rim, glow_rim * 0.85)
        c = mixc(c, core, glow_core * 0.9)
        hs = sstep(0.82, 0.95, hot[i]) * (1.0 - glow_rim)                        # dull red hot spots
        c = mixc(c, (120, 40, 20), hs * 0.5)
        return c
    return compose(w, h, fn)


def t_forest():
    """A11: forest canopy from above, 64 m (0.25 m texels): packed broadleaf / laurel crowns 4-11 m across, each a
    dome lit on the sun side (az 230) with a dark NE flank, deep shadow gaps between crowns, a few emergent crowns
    and darker conifer (cryptomeria) clumps. Near-neutral (mean 160): the forest tint gives the hue. Strong
    contrast on purpose: it is what reads as 'forest' over the 80 m terrain cells from 300 m to ~2 km."""
    w = h = 256
    s = _seed("t_forest")
    wx = ng(w, h, 6, 2, s + 1)
    wy = ng(w, h, 6, 2, s + 2)
    warp = ([(v - 0.5) * 0.5 for v in wx], [(v - 0.5) * 0.5 for v in wy])
    f1, ed, cv = worley(w, h, 11, s + 3, jitter=0.95, warp=warp)        # main crowns ~5.8 m cells
    g1, ged, gv = worley(w, h, 23, s + 4, jitter=0.9)                    # small crowns / understorey 2.8 m
    con = ng(w, h, 3, 3, s + 5)                                          # conifer clumps
    lum0 = ng(w, h, 5, 3, s + 6)
    leaf = ng(w, h, 128, 1, s + 7)
    leaf2 = ng(w, h, 48, 2, s + 8)
    sx, sy = sun_img_dir()
    # crown height field: dome per main cell (radius by cell value), small crowns fill the gaps
    hgt = [0.0] * (w * h)
    kind = [0.0] * (w * h)
    for i in range(w * h):
        r = 0.42 + 0.22 * cv[i]
        a = max(0.0, 1.0 - (f1[i] / r) ** 2)
        b = max(0.0, 1.0 - (g1[i] / (0.40 + 0.15 * gv[i])) ** 2) * 0.55
        if a >= b:
            hgt[i] = math.sqrt(a) * (0.75 + 0.5 * cv[i])
            kind[i] = 1.0
        else:
            hgt[i] = math.sqrt(b) * 0.6
    tw = shift(hgt, w, h, int(round(sx * 2)), int(round(sy * 2)))
    aw = shift(hgt, w, h, -int(round(sx * 2)), -int(round(sy * 2)))
    sh = shift(hgt, w, h, int(round(sx * 6)), int(round(sy * 6)))

    def fn(i, x, y):
        hv = hgt[i]
        slope = aw[i] - tw[i]                                    # > 0 on the sun-facing flank
        lum = 0.62 + 0.55 * hv + 0.9 * slope
        lum *= 1.0 - 0.45 * max(0.0, sh[i] - hv - 0.05)          # cast shadow from the crown sunward
        if hv < 0.05:
            lum *= 0.45                                          # gap: deep shade
        lum *= 1.0 + 0.10 * (lum0[i] - 0.5) * 2.0 + 0.10 * (leaf[i] - 0.5) * 2.0 + 0.06 * (leaf2[i] - 0.5) * 2.0
        c = mixc((0.95, 1.02, 0.96), (1.06, 1.04, 0.86), cv[i] * kind[i] * 0.6)   # crowns vary yellow-green
        k = sstep(0.62, 0.74, con[i])                                             # conifer clumps: dark blue-green
        c = mixc(c, (0.86, 0.98, 1.02), k)
        lum *= 1.0 - 0.22 * k
        return (c[0] * lum, c[1] * lum, c[2] * lum)
    R, G, B = fields(w, h, fn)
    return neutral(w, h, R, G, B, TERRAIN_MEAN["t_forest"])


def t_scrub():
    """A11: dry scrub / heath, 64 m: tan dry grass, dark low shrub mats (heather, myrtle) with shadows, bare
    ochre soil and pale stones, a few grey boulders. Leeward slopes, the upper grass belt, old lava fields and
    the stubble / fallow plots of the farmland patchwork. Near-neutral (mean 176)."""
    w = h = 256
    s = _seed("t_scrub")
    dry = ng(w, h, 5, 3, s + 1)
    gr = ng(w, h, 128, 1, s + 2)
    gr2 = ng(w, h, 40, 2, s + 3)
    soil = ng(w, h, 9, 3, s + 4)
    dens = ng(w, h, 7, 3, s + 5)
    bw = (ng(w, h, 8, 2, s + 6), ng(w, h, 8, 2, s + 7))
    warp = ([(v - 0.5) * 0.7 for v in bw[0]], [(v - 0.5) * 0.7 for v in bw[1]])
    bf1, _be, bv = worley(w, h, 30, s + 8, warp=warp)                    # shrub mats ~2.1 m cells
    rf1, _re, rv = worley(w, h, 14, s + 9, jitter=0.95)                  # boulders
    stone = ng(w, h, 96, 1, s + 10)
    shr = [0.0] * (w * h)
    rock = [0.0] * (w * h)
    for i in range(w * h):
        rr = 0.22 + 0.25 * bv[i]
        shr[i] = sstep(rr, rr * 0.5, bf1[i]) * sstep(0.40, 0.62, dens[i] * 0.85 + bv[i] * 0.3)
        rock[i] = sstep(0.17, 0.10, rf1[i]) * (1.0 if rv[i] > 0.86 else 0.0)
    sx, sy = sun_img_dir()
    occ = [max(shr[i], rock[i]) for i in range(w * h)]
    sh = shift(occ, w, h, int(round(sx * 4)), int(round(sy * 4)))
    tw = shift(rock, w, h, int(round(sx * 2)), int(round(sy * 2)))

    def fn(i, x, y):
        c = mixc((1.02, 1.01, 0.92), (1.12, 1.03, 0.80), sstep(0.3, 0.8, dry[i]))   # dry grass, straw
        lum = 1.0 + 0.09 * (gr[i] - 0.5) * 2.0 + 0.06 * (gr2[i] - 0.5) * 2.0
        sl = sstep(0.66, 0.86, soil[i]) * (1.0 - shr[i])
        c = mixc(c, (1.30, 1.02, 0.78), sl * 0.8)                                  # ochre soil
        lum *= 1.0 + 0.12 * sl + 0.10 * sstep(0.88, 0.97, stone[i]) * sl
        lum *= 1.0 - 0.32 * max(0.0, sh[i] - occ[i])
        b = shr[i]
        if b > 0.0:
            c = mixc(c, (0.86, 1.02, 0.86), b)
            lum = lum * (1.0 - b) + (0.52 + 0.18 * bv[i]) * b
        k = rock[i]
        if k > 0.0:
            c = mixc(c, (1.0, 1.0, 1.02), k)
            lum = lum * (1.0 - k) + (0.80 + 0.35 * max(0.0, tw[i] - rock[i] + 0.3)) * k
        return (c[0] * lum, c[1] * lum, c[2] * lum)
    R, G, B = fields(w, h, fn)
    return neutral(w, h, R, G, B, TERRAIN_MEAN["t_scrub"])


# ---------------------------------------------------------------------------------------------------- sea

def sea():
    """Open sea, 128 m (0.5 m texels): tileable wave trains toward WAVE_TO + ripple, facet light, sky
    reflection on tilted facets, sparse glints, scattered whitecaps."""
    N = 256
    s = _seed("sea")
    r = Rng(SEED, "sea", "waves")
    a = math.radians(WAVE_TO)
    dx, dy = math.sin(a), math.cos(a)          # image coords: x = east, y = north
    waves = []
    used = set()
    for c, amp, nk in [(3, 1.0, 2), (5, 0.62, 3), (8, 0.40, 4), (12, 0.22, 4), (18, 0.12, 5), (27, 0.06, 5)]:
        k = 0
        tries = 0
        while k < nk and tries < 50:
            tries += 1
            t = math.radians(r.range(-38.0, 38.0))
            ux = dx * math.cos(t) - dy * math.sin(t)
            uy = dx * math.sin(t) + dy * math.cos(t)
            m = c * r.range(0.85, 1.15)
            kx, ky = int(round(ux * m)), int(round(uy * m))
            if (kx, ky) == (0, 0) or (kx, ky) in used or (-kx, -ky) in used:
                continue
            used.add((kx, ky))
            waves.append((kx, ky, amp * r.range(0.7, 1.2) / max(1.0, math.hypot(kx, ky)) * c,
                          r.range(0.0, 2.0 * math.pi), c <= 8))
            k += 1
    w1 = ng(N, N, 4, 3, s + 1)
    w2 = ng(N, N, 4, 3, s + 2)
    rip = ng(N, N, 64, 2, s + 3)
    big = ng(N, N, 3, 3, s + 4)
    wcn = ng(N, N, 24, 2, s + 5)
    wcd = ng(N, N, 5, 2, s + 7)
    glt = ng(N, N, 128, 1, s + 6)
    two_pi_n = 2.0 * math.pi / N
    hgt = [0.0] * (N * N)
    swell = [0.0] * (N * N)                    # long waves only: where whitecaps form
    for y in range(N):
        for x in range(N):
            i = y * N + x
            px = x + (w1[i] - 0.5) * 18.0          # domain warp (tileable: integer wave vectors)
            py = y + (w2[i] - 0.5) * 18.0
            v = 0.0
            sw = 0.0
            for kx, ky, am, ph, long_wave in waves:
                e = am * math.sin(two_pi_n * (kx * px + ky * py) + ph)
                v += e
                if long_wave:
                    sw += e
            hgt[i] = v + 0.4 * (rip[i] - 0.5)
            swell[i] = sw
    lo, hi = min(hgt), max(hgt)
    hn = [(v - lo) / (hi - lo) for v in hgt]
    srt = sorted(swell)
    s_hi = srt[int(len(srt) * 0.93)]           # top 7 % of the swell crests may carry foam
    s_top = srt[-1]
    # slopes by central differences (wrap); scale so the RMS slope is ~0.14
    gx = [0.0] * (N * N)
    gy = [0.0] * (N * N)
    for y in range(N):
        for x in range(N):
            i = y * N + x
            gx[i] = hgt[y * N + (x + 1) % N] - hgt[y * N + (x - 1) % N]
            gy[i] = hgt[((y + 1) % N) * N + x] - hgt[((y - 1) % N) * N + x]
    rms = math.sqrt(sum(gx[i] * gx[i] + gy[i] * gy[i] for i in range(N * N)) / (N * N))
    k = 0.14 / rms
    az, el = math.radians(SUN_AZ), math.radians(SUN_EL)
    L = (math.sin(az) * math.cos(el), math.sin(el), math.cos(az) * math.cos(el))      # (east, up, north)
    va = az + math.pi                                  # typical viewer: opposite side, looking toward the sun
    Vw = (math.sin(va) * math.cos(math.radians(28)), math.sin(math.radians(28)), math.cos(va) * math.cos(math.radians(28)))
    H = M.vnorm(M.vadd(L, Vw))
    deep, mid = (14, 50, 88), (26, 74, 112)
    skyc = (150, 190, 222)
    foam = (214, 226, 232)

    def fn(i, x, y):
        nx, nz = -gx[i] * k, -gy[i] * k
        inv = 1.0 / math.sqrt(nx * nx + 1.0 + nz * nz)
        n = (nx * inv, inv, nz * inv)
        ndl = n[0] * L[0] + n[1] * L[1] + n[2] * L[2]
        c = mixc(deep, mid, 0.45 + 0.12 * big[i] + 0.3 * hn[i])
        lum = 1.0 + 1.5 * (ndl - L[1])
        c = (c[0] * lum, c[1] * lum, c[2] * lum)
        c = mixc(c, skyc, min(0.28, (1.0 - n[1]) * 5.0))                 # tilted facets reflect the bright sky
        nh = max(0.0, n[0] * H[0] + n[1] * H[1] + n[2] * H[2])
        g = (nh ** 600) * sstep(0.90, 0.98, glt[i]) * 1.5                # rare sun glints
        c = mixc(c, (255, 250, 236), min(0.9, g))
        wc = sstep(s_hi, s_top, swell[i]) * sstep(0.55, 0.85, wcn[i]) * sstep(0.45, 0.7, wcd[i])
        c = mixc(c, foam, min(0.8, wc * 2.2))
        return c
    return compose(N, N, fn)


def foam():
    """Premultiplied foam strip 256x64 (u along the coast 64 m, v across: 0 landward .. 1 seaward)."""
    W, H = 256, 64
    s = _seed("foam")
    wob = periodic1d(W, 3, s + 1)
    wob2 = periodic1d(W, 4, s + 2)
    wob3 = periodic1d(W, 5, s + 3)
    brk1 = periodic1d(W, 7, s + 4)
    brk2 = periodic1d(W, 9, s + 5)
    lace = aniso(W, H, 48, 10, 3, s + 6)
    lace2 = aniso(W, H, 96, 16, 2, s + 7)
    det = aniso(W, H, 64, 16, 2, s + 8)
    rgba = Image.new("RGBA", (W, H))
    px = rgba.load()
    for y in range(H):
        t = (y + 0.5) / H
        for x in range(W):
            i = y * W + x
            ts = 0.17 + (wob[x] - 0.5) * 0.05                         # shore break centre (waterline 0.15)
            shore = math.exp(-((t - ts) / 0.05) ** 2) * (0.42 + 0.58 * det[i])
            swash = sstep(0.04, 0.12, t) * sstep(ts + 0.02, ts - 0.04, t) * sstep(0.5, 0.75, lace[i]) * 0.55
            t1 = 0.45 + (wob2[x] - 0.5) * 0.08
            t2 = 0.70 + (wob3[x] - 0.5) * 0.10
            b1 = math.exp(-((t - t1) / 0.04) ** 2) * 0.75 * sstep(0.3, 0.55, brk1[x]) * (0.35 + 0.65 * lace2[i])
            b2 = math.exp(-((t - t2) / 0.035) ** 2) * 0.45 * sstep(0.4, 0.65, brk2[x]) * (0.5 + 0.5 * lace2[i])
            trail = sstep(ts + 0.02, ts + 0.08, t) * sstep(t1 + 0.04, t1 - 0.04, t) * sstep(0.62, 0.82, lace[i]) * 0.35
            a = max(shore, swash, b1, b2, trail)
            a *= sstep(0.0, 0.05, t) * sstep(1.0, 0.88, t)
            a = min(1.0, a)
            av = clamp8(a * 255.0)
            px[x, y] = (clamp8(240 * a), clamp8(245 * a), clamp8(246 * a), av)
    return rgba


# ---------------------------------------------------------------------------------------------------- airfield

A = 256      # airfield authoring resolution (A x A for one repeat, then box-filtered to 256 x 64)


def _asphalt(width_m, length_m, s, base0, base1, rubber=True, grain=1.0):
    """Authoring-resolution asphalt (x across, y along) -> (R, G, B, rub) flat lists. Same seed = same grain."""
    n = A * A
    mid = ng(A, A, 4, 3, s + 1)
    g1 = ng(A, A, 48, 2, s + 2)
    g2 = ng(A, A, 96, 1, s + 3)
    spk = ng(A, A, 80, 1, s + 4)
    streak = aniso(A, A, 56, 4, 2, s + 5)
    edgeN = ng(A, A, 24, 3, s + 6)
    lane = periodic1d(A, 3, s + 7)
    lane2 = periodic1d(A, 5, s + 8)
    mu = width_m / A
    half = width_m * 0.5
    R = [0.0] * n
    G = [0.0] * n
    B = [0.0] * n
    rub = [0.0] * n
    for y in range(A):
        for x in range(A):
            i = y * A + x
            um = (x + 0.5) * mu
            dc = abs(um - half)
            de = half - dc                                         # metres from the nearer edge
            c = mixc(base0, base1, 0.3 + 0.45 * mid[i])
            lum = 1.0 + grain * (0.08 * (g1[i] - 0.5) * 2.0 + 0.06 * (g2[i] - 0.5) * 2.0)
            lum *= 1.0 + 0.05 * (streak[i] - 0.5) * 2.0
            c = mixc(c, (118, 116, 110), sstep(0.84, 0.96, spk[i]) * 0.35)    # aggregate chips
            rb = 0.0
            if rubber:
                # rubber from the main gear: dark smears along the centre, strongest +-4 m off it
                rb = (math.exp(-((dc - 3.5) / 3.0) ** 2) * 0.8 + math.exp(-(dc / 2.0) ** 2) * 0.35)
                rb *= (0.72 + 0.28 * lane[y]) * (0.7 + 0.3 * streak[i]) * (0.82 + 0.18 * lane2[y])
                c = mixc(c, (34, 34, 36), min(0.6, rb * 0.55))
            if de < 1.0:                                           # rough, dusty edge
                e = sstep(1.0, 0.0, de) * (0.4 + 0.6 * edgeN[i])
                c = mixc(c, (104, 100, 92), e * 0.5)
            R[i], G[i], B[i] = c[0] * lum, c[1] * lum, c[2] * lum
            rub[i] = rb
    return R, G, B, rub


def _paint_lines(R, G, B, rub, width_m, length_m, lines, colour, s, wear=0.18):
    """lines: [(u0, u1, v0, v1)] metres (v ranges may wrap past length_m). Exact box coverage per texel."""
    mu = width_m / A
    mv = length_m / A
    pn = ng(A, A, 64, 2, s + 21)
    for y in range(A):
        v0, v1 = y * mv, (y + 1) * mv
        for x in range(A):
            u0, u1 = x * mu, (x + 1) * mu
            cv = 0.0
            for a0, a1, b0, b1 in lines:
                cu = cov(a0, a1, u0, u1)
                if cu > 0.0:
                    cv = max(cv, cu * cov_wrap(b0, b1, v0, v1, length_m))
            if cv > 0.0:
                i = y * A + x
                p = cv * (1.0 - wear * pn[i]) * (1.0 - 0.55 * min(1.0, rub[i]))
                R[i] += (colour[0] - R[i]) * p
                G[i] += (colour[1] - G[i]) * p
                B[i] += (colour[2] - B[i]) * p


def _final(R, G, B, size):
    return to_img(A, A, R, G, B).resize(size, Image.BOX)


THR_KEYS = (6.0, 34.8)      # piano keys from / to (m from the runway end) in rwy_thr


def thr_stripes(W_M):
    """12 stripes 1.8 m, 1.8 m apart, 2.7 m centre gap, 1.35 m margins -> [(u0, u1)] metres."""
    out = []
    for k in range(6):
        a = 1.35 + 3.6 * k
        out.append((a, a + 1.8))
        out.append((W_M - a - 1.8, W_M - a))
    return sorted(out)


def runway(kind="centre"):
    """kind 'centre' (runway: dashed centre line), 'end' (rwy_end: none), 'thr' (rwy_thr: piano keys)."""
    W_M, L_M = PERIOD["runway"]
    s = _seed("runway")
    R, G, B, rub = _asphalt(W_M, L_M, s, (70, 70, 72), (92, 91, 89))
    lines = [(1.0, 1.9, 0.0, L_M), (W_M - 1.9, W_M - 1.0, 0.0, L_M)]
    if kind == "centre":
        lines.append((W_M * 0.5 - 0.45, W_M * 0.5 + 0.45, 10.0, 40.0))
    elif kind == "thr":
        lines += [(a, b, THR_KEYS[0], THR_KEYS[1]) for a, b in thr_stripes(W_M)]
    _paint_lines(R, G, B, rub, W_M, L_M, lines, PAINT, s)
    return _final(R, G, B, SIZES["runway"])


def taxi():
    W_M, L_M = PERIOD["taxi"]
    s = _seed("taxi")
    R, G, B, rub = _asphalt(W_M, L_M, s, (84, 83, 80), (104, 101, 96), rubber=False)
    sh = ng(A, A, 16, 3, s + 31)
    mu = W_M / A
    for y in range(A):                                    # shoulders: lighter, greyer, weathered
        for x in range(A):
            i = y * A + x
            um = (x + 0.5) * mu
            de = min(um, W_M - um)
            t = cov(0.0, 1.5, x * mu, (x + 1) * mu) + cov(W_M - 1.5, W_M, x * mu, (x + 1) * mu)
            if de < 1.6:
                k = min(1.0, t) * 0.55
                c = mixc((R[i], G[i], B[i]), (126, 122, 114), k * (0.75 + 0.25 * sh[i]))
                R[i], G[i], B[i] = c
    lines = [(W_M * 0.5 - 0.15, W_M * 0.5 + 0.15, 0.0, L_M),
             (1.5, 1.75, 0.0, L_M), (2.0, 2.25, 0.0, L_M),
             (W_M - 2.25, W_M - 2.0, 0.0, L_M), (W_M - 1.75, W_M - 1.5, 0.0, L_M)]
    _paint_lines(R, G, B, rub, W_M, L_M, lines, YELLOW, s, wear=0.25)
    return _final(R, G, B, SIZES["taxi"])


def apron():
    """Concrete apron 128x128 = 32 m: 8 x 8 slabs of 4 m (16 px), dark joints, per-slab tone, stains."""
    w = h = 128
    s = _seed("apron")
    r = Rng(SEED, "apron", "slabs")
    tone = [r.range(-1.0, 1.0) for _ in range(64)]
    warm = [r.range(-1.0, 1.0) for _ in range(64)]
    g1 = ng(w, h, 64, 2, s + 1)
    g2 = ng(w, h, 32, 2, s + 2)
    mot = ng(w, h, 8, 3, s + 3)
    oil = ng(w, h, 12, 3, s + 4)
    tyre = aniso(w, h, 3, 24, 2, s + 5)
    base = (170, 167, 160)

    def fn(i, x, y):
        k = (y // 16) * 8 + (x // 16)
        c = (base[0] * (1.0 + 0.02 * warm[k]), base[1], base[2] * (1.0 - 0.02 * warm[k]))
        lum = 1.0 + 0.045 * tone[k] + 0.05 * (g1[i] - 0.5) * 2.0 + 0.04 * (g2[i] - 0.5) * 2.0
        lum *= 1.0 + 0.04 * (mot[i] - 0.5) * 2.0
        fx, fy = x % 16, y % 16
        if fx == 0 or fy == 0:
            lum *= 0.74                                            # sealed joint
        elif fx == 1 or fy == 1:
            lum *= 1.03                                            # chipped edge catches light
        c = mixc(c, (88, 80, 70), 0.55 * sstep(0.80, 0.93, oil[i]))
        c = mixc(c, (110, 108, 104), 0.3 * sstep(0.75, 0.9, tyre[i]))
        return (c[0] * lum, c[1] * lum, c[2] * lum)
    return compose(w, h, fn)


# ---------------------------------------------------------------------------------------------------- marks

DIG_BOX = (3.0, 9.0)        # glyph size in metres (width across the runway, length along it)
DIG_STROKE = 0.8
DIG_PPM = 5.0               # 0.2 m texels
DIG_PAD = 0.3               # transparent margin around a glyph (m) -> rect 3.6 x 9.6 m = 18 x 48 px


def _arc(cx, cy, rx, ry, a0, a1, n=14):
    out = []
    for k in range(n + 1):
        a = math.radians(a0 + (a1 - a0) * k / float(n))
        out.append((cx + rx * math.cos(a), cy + ry * math.sin(a)))
    return out


def _digit_strokes(d):
    """Polylines (metres, x right, y down, in the 3 x 9 box) of a runway designation digit."""
    xl, xr, yt, yb, cx = 0.4, 2.6, 0.4, 8.6, 1.5
    rx = 1.1
    if d == "0":
        p = _arc(cx, yt + 1.6, rx, 1.6, 180, 360) + _arc(cx, yb - 1.6, rx, 1.6, 0, 180)
        return [p + [p[0]]]
    if d == "1":
        return [[(0.7, 1.7), (1.8, yt), (1.8, yb)]]
    if d == "2":
        return [_arc(cx, yt + 1.7, rx, 1.7, 180, 380) + [(xl, yb), (xr + 0.05, yb)]]
    if d == "3":
        return [_arc(cx, yt + 1.95, rx, 1.95, 195, 450) + _arc(cx, yb - 2.15, rx, 2.15, 270, 525)]
    if d == "4":
        return [[(2.05, yb), (2.05, yt), (xl, 6.0), (xr + 0.05, 6.0)]]
    if d == "5":
        return [[(xr, yt), (xl + 0.05, yt), (xl, 4.2)] + _arc(cx, 6.15, rx, 2.45, 215, 505)]
    if d == "6":
        return [_arc(cx, yb - 2.3, rx, 2.3, 0, 360),
                [(xl, yb - 2.3), (xl, 2.2)] + _arc(cx, yt + 1.8, rx, 1.8, 180, 325)]
    if d == "7":
        return [[(xl - 0.05, yt), (xr, yt), (1.05, yb)]]
    if d == "8":
        a = _arc(cx, yt + 1.95, 1.0, 1.95, 0, 360)
        b = _arc(cx, yb - 2.15, rx, 2.15, 0, 360)
        return [a, b]
    if d == "9":
        return [[(3.0 - x, 9.0 - y) for x, y in pl] for pl in _digit_strokes("6")]
    raise ValueError(d)


def _paint_rgba(img, s, amount=0.08):
    """Subtle paint tone (luminance only) on an RGBA element; alpha untouched."""
    w, h = img.size
    n = ng(w, h, max(2, w // 4), 2, s) if w == h else aniso(w, h, max(2, w // 4), max(2, h // 4), 2, s)
    px = img.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            k = 1.0 - amount * n[y * w + x]
            px[x, y] = (clamp8(r * k), clamp8(g * k), clamp8(b * k), a)
    return img


def mark_digit(d):
    pw = int(round((DIG_BOX[0] + 2 * DIG_PAD) * DIG_PPM))
    ph = int(round((DIG_BOX[1] + 2 * DIG_PAD) * DIG_PPM))

    def draw(dr, im, f):
        sc = DIG_PPM * f
        wid = int(round(DIG_STROKE * sc))
        for pl in _digit_strokes(d):
            pts = [((x + DIG_PAD) * sc, (y + DIG_PAD) * sc) for x, y in pl]
            dr.line(pts, fill=PAINT + (255,), width=wid, joint="curve")
    img = I.supersample(pw, ph, draw)
    return _paint_rgba(img, _seed("digit", d))


def mark_bar():
    w, h = 16, 64
    img = Image.new("RGBA", (w, h), PAINT + (255,))
    return _paint_rgba(img, _seed("bar"), 0.06)


def mark_hold():
    """Runway holding position (yellow): across a 23 m taxiway (128 px), 3.0 m deep (16 px): 2 solid lines,
    then 2 dashed lines (1 m dash / 1 m gap) on the RUNWAY side = image top. Lines 0.4 m, gaps 0.3 m (wider than
    real 0.15 m lines so they survive the mips)."""
    w, h = 128, 16
    mu, mv = 23.0 / w, 3.0 / h
    rgba = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = rgba.load()
    rows = [(0.35, 0.75, False), (1.05, 1.45, False), (1.75, 2.15, True), (2.45, 2.85, True)]   # v from the taxiway side
    for y in range(h):
        for x in range(w):
            a = 0.0
            for v0, v1, dashed in rows:
                cvv = cov(3.0 - v1, 3.0 - v0, y * mv, (y + 1) * mv)   # dashed rows near the top (runway side)
                if cvv <= 0.0:
                    continue
                if not dashed:
                    cu = cov(0.5, 22.5, x * mu, (x + 1) * mu)
                else:
                    cu = 0.0
                    u = 0.5
                    while u < 22.5:
                        cu += cov(u, min(22.5, u + 1.0), x * mu, (x + 1) * mu)
                        u += 2.0
                a = max(a, cvv * cu)
            px[x, y] = YELLOW + (clamp8(a * 255),)
    return rgba


def rwy_marks():
    """-> (RGBA atlas, rect text)."""
    at = I.Atlas(SIZES["rwy_marks"][0], SIZES["rwy_marks"][1], gutter=2)
    elems = [("bar", mark_bar(), None, None, "solid paint: TDZ bars 1.8 x 22.5 m, aiming point 9 x 45 m (stretch)")]
    for d in "0123456789":
        elems.append(("d%s" % d, mark_digit(d), 3.6, 9.6, "designation digit (glyph 3 x 9 m + 0.3 m margin)"))
    elems.append(("hold", mark_hold(), 23.0, 3.0, "yellow holding position across the taxiway, dashed side = image top = runway"))
    for name, img, _w, _h, _c in elems:
        at.add(name, img)
    sheet = at.image()
    lines = [
        "# rwy_marks.bmp (A3 gen_ground.py): keyed atlas, exact magenta = transparent (ALPHATEST, cull none ok).",
        "# Each line: name x y w h world_w world_h  (pixels in the 256x128 atlas; metres of the quad showing the",
        "# whole rect, 0 = stretch to any size). Use UVs inset by half a texel (assetkit Atlas.uv convention).",
        "# Orientation: image UP = landing direction (toward the far end), image LEFT = pilot's left on approach.",
        "# Quads 0.05-0.1 m above the runway ribbon (+ zBias). Suggested placement from EACH runway end (ICAO-like,",
        "# 45 m runway, 2800 m long): piano keys are in rwy_thr.bmp (6..34.8 m, opaque, so they don't vanish in",
        "# the mips like keyed stripes); digits 46..55.6 m, rect centres 2.1 m either side of the centre",
        "# line (two digits, e.g. heading 233 deg -> '2' '3', reciprocal '0' '5'); aiming point = two 'bar' quads",
        "# 9 m wide x 45 m long at 400 m, inner edges 9 m from the centre line; touchdown zone = 'bar' quads",
        "# 1.8 x 22.5 m, inner edge 9 m from the centre line, 1.5 m apart: 3 per side at 150 m and 300 m, 2 at",
        "# 450 m and 600 m, 1 at 750 m and 900 m (bar starts at those distances). Ribbon textures from each end:",
        "# rwy_thr 0..50 m (v = 0 at the runway end), rwy_end 50..100 m, runway after that (all v = s / 50; dash",
        "# gaps sit on the v wrap, so every switch at whole v is seamless).",
        "# hold: on each taxiway link, 75 m from the runway centre line, dashed side toward the runway.",
    ]
    for name, _img, ww, hh, com in elems:
        x, y, w, h = at.rect(name)
        lines.append("%-5s %3d %3d %3d %3d %6.2f %6.2f   # %s" % (name, x, y, w, h, ww or 0.0, hh or 0.0, com))
    return sheet, "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------------------------------- all

def textures():
    """[(name, image)] in NAMES order (foam and rwy_marks RGBA) + the rect text."""
    out = [("t_sand", t_sand()), ("t_grass", t_grass()), ("t_rock", t_rock()), ("t_ash", t_ash()),
           ("t_lava", t_lava()), ("t_forest", t_forest()), ("t_scrub", t_scrub()), ("sea", sea()), ("foam", foam()),
           ("runway", runway("centre")),
           ("rwy_end", runway("end")), ("rwy_thr", runway("thr")), ("taxi", taxi()), ("apron", apron())]
    marks, txt = rwy_marks()
    out.append(("rwy_marks", marks))
    for n, img in out:
        if img.size != SIZES[n]:
            raise ValueError("%s is %dx%d, expected %dx%d" % ((n,) + img.size + SIZES[n]))
    return out, txt


# ---------------------------------------------------------------------------------------------------- previews

def _flat_light():
    """Preview light = A2's data/sky/light.txt values (sun 255 244 226, ambient 132 150 178, 230/40); flat ground
    normalised to 1.0 (the suggested tints assume that: a bake whose flat ground is L gives tint x L)."""
    az, el = math.radians(SUN_AZ), math.radians(SUN_EL)
    L = (math.sin(az) * math.cos(el), math.sin(el), math.cos(az) * math.cos(el))
    sun, amb = (1.0, 244 / 255.0, 226 / 255.0), (132 / 255.0, 150 / 255.0, 178 / 255.0)
    flat = tuple(amb[k] + sun[k] * L[1] for k in range(3))
    return L, sun, amb, flat


def _lit(n, tint):
    L, sun, amb, flat = _flat_light()
    d = max(0.0, n[0] * L[0] + n[1] * L[1] + n[2] * L[2])
    sky = 0.75 + 0.25 * n[1]
    return tuple(clamp8(tint[k] * (amb[k] * sky + sun[k] * d) / flat[k]) for k in range(3)) + (255,)


def tiled(img, reps=3):
    w, h = img.size
    big = Image.new(img.mode, (w * reps, h * reps))
    for yy in range(reps):
        for xx in range(reps):
            big.paste(img, (xx * w, yy * h))
    return big


def tiling_sheet(texs, cell=240):
    """Each opaque texture tiled 3x3 (world-proportional), above its RGB565-dithered version."""
    items = [(n, img) for n, img in texs if img.mode == "RGB"]
    cols = 4
    rows = (len(items) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell, rows * (2 * cell + 34)), (60, 60, 66))
    for k, (n, img) in enumerate(items):
        pu, pv = PERIOD[n]
        th = int(round(cell * pv / float(pu)))
        th = min(cell, th)
        big = tiled(img).resize((cell, th), Image.BOX)
        q = I.to16(big, "565", dither=True)
        x0 = (k % cols) * cell
        y0 = (k // cols) * (2 * cell + 34)
        sheet.paste(big, (x0, y0 + 16))
        sheet.paste(q, (x0, y0 + 32 + cell))
        V.label(sheet, "%s 3x3 = %gx%g m" % (n, 3 * pu, 3 * pv), (x0 + 3, y0 + 2), (235, 235, 235))
        V.label(sheet, "RGB565 dithered", (x0 + 3, y0 + cell + 18), (235, 235, 235))
    return sheet


def tinted_sheet(texs, cell=256):
    """Terrain textures tiled 3x3 x suggested tint (flat sunlit ground), 565: what the island will look like."""
    tints = suggested_tints()
    d = dict(texs)
    names = TERRAIN + ["shelf"]
    sheet = Image.new("RGB", (3 * cell, ((len(names) + 2) // 3) * (cell + 16)), (60, 60, 66))
    for k, n in enumerate(names):
        src = d["t_sand" if n == "shelf" else n]
        t = tints[n]
        big = tiled(src).resize((cell, cell), Image.BOX)
        r, g, b = big.split()
        big = Image.merge("RGB", (r.point(lambda v, t=t[0]: v * t // 255), g.point(lambda v, t=t[1]: v * t // 255),
                                  b.point(lambda v, t=t[2]: v * t // 255)))
        if n == "shelf":                                   # under the sea blend (alpha 0.65)
            big = Image.blend(big, d["sea"].resize((cell, cell), Image.BOX), 0.65)
        big = I.to16(big, "565", dither=True)
        x0 = (k % 3) * cell
        y0 = (k // 3) * (cell + 16)
        sheet.paste(big, (x0, y0 + 16))
        lbl = "%s tint %d,%d,%d" % (n, t[0], t[1], t[2]) + (" + sea 0.65" if n == "shelf" else "")
        V.label(sheet, lbl, (x0 + 3, y0 + 2), (235, 235, 235))
    return sheet


def _quad(g, p, uv, cols, mat):
    """Quad p0..p3 (bottom-left, top-left, top-right, bottom-right seen from above = clockwise) as 2 tris."""
    idx = [g.vert(p[k], n=(0, 1, 0), uv=uv[k], c=cols[k]) for k in range(4)]
    g.tri(idx[0], idx[1], idx[2], mat=mat)
    g.tri(idx[0], idx[2], idx[3], mat=mat)


def _ribbon(g, x0, x1, z0, z1, y, mat, uvfn, col):
    """Flat quad x0..x1 (u 0..1 across), z0..z1, seen from above; uvfn(u, z) -> uv."""
    p = [(x0, y, z0), (x0, y, z1), (x1, y, z1), (x1, y, z0)]
    uv = [uvfn(0.0, z0), uvfn(0.0, z1), uvfn(1.0, z1), uvfn(1.0, z0)]
    _quad(g, p, uv, [col] * 4, mat)


MATS = {"t_sand": ("t_sand", "t_sand"), "t_grass": ("t_grass", "t_grass"), "t_rock": ("t_rock", "t_rock"),
        "t_ash": ("t_ash", "t_ash"), "t_lava": ("t_lava", "t_lava"), "sea": ("sea", "sea"),
        "foam": ("foam", "foam", M.RB_ADD), "runway": ("runway", "runway"), "rwy_end": ("rwy_end", "rwy_end"),
        "rwy_thr": ("rwy_thr", "rwy_thr"),
        "taxi": ("taxi", "taxi"), "apron": ("apron", "apron"), "marks": ("marks", "rwy_marks", M.RB_ALPHATEST, True)}

FOG = (196, 212, 228)       # A2 light.txt fog
ZEN = (50, 98, 182)         # A2 light.txt top


def airfield_geo(tex_dirs, marks_rects):
    """Flat mock airfield: runway along +Z from z = 0 (threshold, at the coast), taxiway, apron, grass, beach,
    foam, sea to the south. Returns a Geo with material names."""
    tints = suggested_tints()
    g = M.Geo()
    up = (0.0, 1.0, 0.0)
    gc = _lit(up, tints["t_grass"])
    sc = _lit(up, tints["t_sand"])
    white = _lit(up, (255, 255, 255))
    # sea (big, darker far away)
    S = 128.0
    for zi in range(-12, 4):
        for xi in range(-10, 10):
            x0, x1, z0, z1 = xi * 1200.0, (xi + 1) * 1200.0, zi * 1200.0 - 330.0, (zi + 1) * 1200.0 - 330.0
            z1 = min(z1, -250.0)
            if z0 >= z1:
                continue
            cols = []
            for (x, z) in ((x0, z0), (x0, z1), (x1, z1), (x1, z0)):
                dd = math.hypot(x, z + 1200.0)
                k = 1.0 - 0.3 * min(1.0, dd / 14000.0)
                cols.append((clamp8(255 * k), clamp8(255 * k), clamp8(255 * k), 255))
            _quad(g, [(x0, 0.0, z0), (x0, 0.0, z1), (x1, 0.0, z1), (x1, 0.0, z0)],
                  [(x0 / S, z0 / S), (x0 / S, z1 / S), (x1 / S, z1 / S), (x1 / S, z0 / S)], cols, "sea")
    # beach (sloping into the sea) and grass
    for xi in range(-10, 10):
        x0, x1 = xi * 1200.0, (xi + 1) * 1200.0
        _quad(g, [(x0, -1.5, -320.0), (x0, 1.0, -260.0), (x1, 1.0, -260.0), (x1, -1.5, -320.0)],
              [(x0 / 32.0, -320 / 32.0), (x0 / 32.0, -260 / 32.0), (x1 / 32.0, -260 / 32.0), (x1 / 32.0, -320 / 32.0)],
              [sc] * 4, "t_sand")
        for zi in range(0, 6):
            z0, z1 = -260.0 + zi * 1000.0, -260.0 + (zi + 1) * 1000.0
            _quad(g, [(x0, 1.0, z0), (x0, 1.0, z1), (x1, 1.0, z1), (x1, 1.0, z0)],
                  [(x0 / 64.0, z0 / 64.0), (x0 / 64.0, z1 / 64.0), (x1 / 64.0, z1 / 64.0), (x1 / 64.0, z0 / 64.0)],
                  [gc] * 4, "t_grass")
    # foam strip along the waterline (z = -300 contour, from 6 m inland to 34 m out)
    for xi in range(-40, 40):
        x0, x1 = xi * 64.0, (xi + 1) * 64.0
        zl, zs = -300.0 + 6.0, -300.0 - 34.0
        _quad(g, [(x0, 0.3, zs), (x0, 0.3, zl), (x1, 0.3, zl), (x1, 0.3, zs)],
              [(x0 / 64.0, 1.0), (x0 / 64.0, 0.0), (x1 / 64.0, 0.0), (x1 / 64.0, 1.0)], [white] * 4, "foam")
    # runway, taxiway, apron (0.2 m above the ground)
    y = 1.2
    rw = 45.0
    for k in range(56):
        z0, z1 = k * 50.0, (k + 1) * 50.0
        m = "rwy_thr" if k in (0, 55) else ("rwy_end" if k in (1, 54) else "runway")
        sgn = -1.0 if k == 55 else 1.0            # far end: v runs from that end
        _ribbon(g, -rw / 2, rw / 2, z0, z1, y, m, lambda u, z, sgn=sgn: (u, sgn * z / 50.0), white)
    for k in range(112):
        z0, z1 = k * 25.0, (k + 1) * 25.0
        _ribbon(g, 180.0 - 11.5, 180.0 + 11.5, z0, z1, y, "taxi", lambda u, z: (u, -z / 25.0), white)
    for zz in (300.0, 2500.0):                          # links
        _ribbon(g, rw / 2, 180.0 - 11.5, zz - 11.5, zz + 11.5, y, "apron", lambda u, z: (u * 4.0, -z / 32.0), white)
    _quad(g, [(191.5, y, 1100.0), (191.5, y, 1400.0), (341.5, y, 1400.0), (341.5, y, 1100.0)],
          [(191.5 / 32, -1100 / 32.0), (191.5 / 32, -1400 / 32.0), (341.5 / 32, -1400 / 32.0), (341.5 / 32, -1100 / 32.0)],
          [white] * 4, "apron")
    # marks (landing toward +Z from the southern threshold at z = 0)
    ym = y + 0.08

    def mark(name, cxm, z0, wm, lm):
        x, yy, w, h = marks_rects[name]
        aw, ah = SIZES["rwy_marks"]
        u0, v0, u1, v1 = (x + 0.5) / aw, (yy + 0.5) / ah, (x + w - 0.5) / aw, (yy + h - 0.5) / ah
        p = [(cxm - wm / 2, ym, z0), (cxm - wm / 2, ym, z0 + lm), (cxm + wm / 2, ym, z0 + lm), (cxm + wm / 2, ym, z0)]
        _quad(g, p, [(u0, v1), (u0, v0), (u1, v0), (u1, v1)], [white] * 4, "marks")
    mark("d2", -2.1, 46.0, 3.6, 9.6)
    mark("d3", 2.1, 46.0, 3.6, 9.6)
    for side in (-1, 1):
        mark("bar", side * (9.0 + 4.5), 400.0, 9.0, 45.0)
        for dist, nb in ((150, 3), (300, 3), (450, 2), (600, 2), (750, 1), (900, 1)):
            for b in range(nb):
                mark("bar", side * (9.0 + 0.9 + b * 3.3), float(dist), 1.8, 22.5)
    return g


def island_geo():
    """Mock cone island (heights + classes like DESIGN 8.1, simplified) on a 128 m grid, for texture readability
    at altitude. Per-quad class, per-vertex lit tint."""
    tints = suggested_tints()
    cell = 128.0
    n = 100
    x0 = -n * cell / 2
    R0, H0 = 6200.0, 1750.0
    hn = ng(128, 128, 6, 4, _seed("mock", "h"))
    ch = ng(128, 128, 24, 3, _seed("mock", "ch"), kind="ridged")

    def samp(lst, x, z):
        u = int((x - x0) / (n * cell) * 128) % 128
        v = int((z - x0) / (n * cell) * 128) % 128
        return lst[v * 128 + u]

    def height(x, z):
        r = math.hypot(x, z)
        r2 = r * (1.0 + 0.12 * (samp(hn, x, z) - 0.5))
        t = max(0.0, 1.0 - r2 / R0)
        hh = H0 * t ** 1.6 + 60.0 * (samp(hn, x * 2.1, z * 2.1) - 0.5) * t + 260.0 * samp(ch, x * 0.5, z * 0.5) * t
        if r < 450.0:
            hh -= 220.0 * (1.0 - (r / 450.0) ** 2)
        return hh - 25.0
    Hs = [[height(x0 + i * cell, x0 + j * cell) for i in range(n + 1)] for j in range(n + 1)]

    def nrm(i, j):
        hl = Hs[j][max(0, i - 1)]
        hr = Hs[j][min(n, i + 1)]
        hd = Hs[max(0, j - 1)][i]
        hu = Hs[min(n, j + 1)][i]
        v = (-(hr - hl) / (2 * cell), 1.0, -(hu - hd) / (2 * cell))
        return M.vnorm(v)
    g = M.Geo()
    S = 128.0
    for j in range(n):
        for i in range(n):
            hs = [Hs[j][i], Hs[j + 1][i], Hs[j + 1][i + 1], Hs[j][i + 1]]
            if max(hs) < -1.0:
                continue
            xa, za = x0 + i * cell, x0 + j * cell
            hm = sum(hs) / 4.0
            nn = nrm(i, j)
            slope = math.degrees(math.acos(max(-1.0, min(1.0, nn[1]))))
            r = math.hypot(xa + cell / 2, za + cell / 2)
            chan = samp(ch, xa, za)
            if hm < 4.0:
                cls = "t_sand"
            elif r < 450.0 or (chan > 0.86 and 250.0 < hm):
                cls = "t_lava"
            elif slope > 30.0:
                cls = "t_rock"
            elif hm > 700.0 or (hm > 600.0 and samp(hn, xa * 3, za * 3) > 0.5):
                cls = "t_ash"
            elif slope > 25.0:
                cls = "t_rock"
            else:
                cls = "t_grass"
            per = PERIOD[cls][0]
            tint = tints[cls]
            pts = [(xa, hs[0], za), (xa, hs[1], za + cell), (xa + cell, hs[2], za + cell), (xa + cell, hs[3], za)]
            ns = [nrm(i, j), nrm(i, j + 1), nrm(i + 1, j + 1), nrm(i + 1, j)]
            cols = [_lit(nv, tint) for nv in ns]
            _quad(g, pts, [(p[0] / per, p[2] / per) for p in pts], cols, cls)
    # sea
    for zi in range(-12, 12):
        for xi in range(-12, 12):
            a0, a1, b0, b1 = xi * 2000.0, (xi + 1) * 2000.0, zi * 2000.0, (zi + 1) * 2000.0
            col = (235, 235, 235, 255)
            _quad(g, [(a0, 0.0, b0), (a0, 0.0, b1), (a1, 0.0, b1), (a1, 0.0, b0)],
                  [(a0 / S, b0 / S), (a0 / S, b1 / S), (a1 / S, b1 / S), (a1 / S, b0 / S)], [col] * 4, "sea")
    return g


def scene_views(kit, marks_rects):
    tex_dirs = kit.tex_dirs
    vfov = math.degrees(2.0 * math.atan(math.tan(math.radians(35.0)) * 0.75))   # 70 deg horizontal at 4:3
    sky = I.gradient(640, 480, ZEN, FOG)
    out = []
    ag = airfield_geo(tex_dirs, marks_rects)
    views = [("airfield_approach", (0.0, 40.0, -700.0), (0.0, 0.0, 700.0), "short final 700 m out, 40 m"),
             ("airfield_roll", (0.0, 3.5, 120.0), (0.0, 2.0, 1400.0), "takeoff roll, eye 3.5 m"),
             ("airfield_thr", (-120.0, 160.0, -140.0), (0.0, 0.0, 140.0), "threshold from 160 m"),
             ("airfield_600m", (-1000.0, 600.0, -900.0), (150.0, 0.0, 900.0), "airfield from 600 m")]
    for name, eye, tgt, label in views:
        img = V.render([ag], V.Camera(eye, tgt, fov=vfov, near=1.0), size=(640, 480), bg=sky,
                       fog=(3600.0, 18000.0, FOG), tex_dirs=tex_dirs, lighting="vertex", mats=MATS, mips=True,
                       q16=True)
        V.label(img, "mock %s: %s (kit renderer, point+mips, fog 3.6-18 km, 565)" % (name, label), (4, 3),
                (250, 250, 250))
        out.append((name, img))
    ig = island_geo()
    views = [("island_15km", (2000.0, 900.0, -15500.0), (0.0, 600.0, 0.0), "15 km out at sea, 900 m"),
             ("island_slopes", (-5200.0, 1900.0, -5200.0), (0.0, 700.0, 0.0), "slopes from 1.9 km altitude"),
             ("island_low", (-6500.0, 160.0, 1200.0), (0.0, 900.0, 0.0), "low over the coast, 160 m")]
    for name, eye, tgt, label in views:
        img = V.render([ig], V.Camera(eye, tgt, fov=vfov, near=1.0), size=(640, 480), bg=sky,
                       fog=(3600.0, 18000.0, FOG), tex_dirs=tex_dirs, lighting="vertex", mats=MATS, mips=True,
                       q16=True)
        V.label(img, "mock %s: %s (565)" % (name, label), (4, 3), (250, 250, 250))
        out.append((name, img))
    return out


def previews(kit, texs, marks_rects):
    kit.save_preview("ground_sheet.png", V.contact_sheet([(n, img) for n, img in texs], cols=4, cell=200, mips=True,
                                                        title="A3 ground/sea/airfield textures (mips below)"))
    kit.save_preview("ground_tiled.png", tiling_sheet(texs))
    kit.save_preview("ground_tinted.png", tinted_sheet(texs))
    for name, img in scene_views(kit, marks_rects):
        kit.save_preview("ground_%s.png" % name, img)


# ---------------------------------------------------------------------------------------------------- build

SEAM_AXES = {"foam": "u"}       # foam: v = across the strip (clamped)


def build(out_dir, kit):
    texs, txt = textures()
    bad = []
    rects = {}
    for line in txt.splitlines():
        if line and not line.startswith("#"):
            f = line.split()
            rects[f[0]] = (int(f[1]), int(f[2]), int(f[3]), int(f[4]))
    for name, img in texs:
        if name == "foam":
            kit.save_bmp("tex/foam.bmp", img, alpha=True, alpha_bleed=0)
        elif name == "rwy_marks":
            kit.save_bmp("tex/rwy_marks.bmp", img, keyed=True)
        else:
            kit.save_bmp("tex/%s.bmp" % name, img)
        if name != "rwy_marks":
            rgb = img.convert("RGB")
            sr = seam_ratio(rgb, SEAM_AXES.get(name, "uv"))
            if sr > SEAM_MAX:
                bad.append("%s seam ratio %.2f" % (name, sr))
            m = mean_rgb(rgb)
            kit.log("%-9s %dx%d seam %.2f  mean %3.0f %3.0f %3.0f  %3.0f KB" % (
                name, img.size[0], img.size[1], sr, m[0], m[1], m[2], I.tex_kb(img.size[0], img.size[1])))
    kit.save_text("tex/rwy_marks.txt", txt, crlf=True)
    if bad:
        raise ValueError("not seamless: " + ", ".join(bad))
    if kit.previews:
        previews(kit, texs, rects)


if __name__ == "__main__":
    from assetkit import build as B
    sys.exit(B.main_for(__file__))
