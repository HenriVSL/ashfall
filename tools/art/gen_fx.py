"""gen_fx.py - weapons and effects art (A8): two missiles, missile texture, trail ribbon, fire/smoke atlases, flare,
debris, water splash and distance dot (DESIGN 9.5, 8.8 classes `missile` and fx, 12.1; TASKS A8).

Original, generic designs (no real markings): a long, slim off-white radar missile with long low mid-body strakes and
small swept tail fins (LRM, HUD "AIM-120"), and a shorter light-grey heat-seeker with a dark glass seeker dome, small
nose canards and cropped-delta tail fins (SRM, HUD "AIM-9"). Fins in an X (45 deg) so nothing touches a pylon/rail.

Outputs (relative to data/):
    meshes/lrm.msh    LRM 3.65 m, body radius 0.09 m. LOD0 < 120 m (96 tris), LOD1 beyond (18 tris; the game stops
                      drawing missiles at ~600 m). Origin = the RAIL POINT = on the centreline, at mid-length: put it
                      straight on A1's rail tags (rail_2..rail_7, dir +Z). Nose +Z.
    meshes/srm.msh    SRM 2.85 m, body radius 0.064 m, same rules (LOD0 96 tris, LOD1 18); rails 0/1 (wingtips).
                      Both: materials "body" (culled) + "fin" (LOD0, double-sided as two culled faces) / "fin2"
                      (LOD1, cull none); texture msl; vertex colours white (CPU-lit like the jet: mesh_draw_lit).
                      Tags: "nozzle" (tail exit, dir -Z: motor glow / trail start), "nose" (tip, dir +Z).
    tex/msl.bmp       128x128 opaque: LRM body strip, SRM body strip, 4 fin panels, nozzle disc.
    tex/trail.bmp+_a  64x256 straight alpha (RB_BLEND): soft smoke ribbon, u across (0 and 1 = alpha 0, clamp u),
                      v along (tiles: wrap v). Near-white, the vertex colour tints/lights it.
    tex/fx_fire.bmp+_a  256x256 2x2 atlas, PREMULTIPLIED (RGB already x coverage; RB_PREMUL "over", or RB_ADD as a pure
                      glow). Animation frames 0 flash, 1 fireball, 2 breaking up, 3 dying flames + soot.
                      Cell i at x = (i % 2) * 128, y = (i / 2) * 128 (cloud_puffs convention).
    tex/fx_smoke.bmp+_a 256x256 2x2 atlas, straight alpha (RB_BLEND, fogged): 4 puff variants (0 dense round,
                      1 lumpy cluster, 2 flat wisp, 3 thin torn), near-white with soft top-lit shading: vertex colour
                      sets black/grey/white smoke.
    tex/flare.bmp     64x64 RGB additive (RB_ADD; black = nothing): white-hot core, yellow halo, faint rays.
    tex/debris.bmp    64x64 keyed (RGB 255,0,255), 4x4 cells of 16 px: metal shards (RB_ALPHATEST, lit by vertex colour).
    tex/splash.bmp+_a 128x128 straight alpha (RB_BLEND): water spray column + crown + base skirt; bottom centre
                      = the water point; upright (axis-aligned) billboard.
    tex/dot.bmp       16x16 RGB additive: soft point for far missiles / flares / fires (2-4 px sprite).
Previews: tools/art/preview/fx_*.png (missile views + LODs, missiles on the jet's rails, texture sheet, textures
over sky/cloud/sea/terrain with their blend, explosion timeline, blend comparison, two in-game mocks).

    python3 tools/build_assets.py --only fx [--check]        # or: python3 tools/art/gen_fx.py
Deterministic: math + assetkit.rng + Pillow only.
"""

import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "tools"))

from PIL import Image, ImageChops, ImageDraw  # noqa: E402

from assetkit import export as E  # noqa: E402
from assetkit import image as I  # noqa: E402
from assetkit import mesh as M  # noqa: E402
from assetkit import palette as P  # noqa: E402
from assetkit import preview as V  # noqa: E402
from assetkit.rng import Rng, fbm, value_noise, value_noise_1d  # noqa: E402

SEED = 808
ORDER = 60                      # after gen_jet (20): previews read jet.msh + parts for the rail fit
ASSETS = [
    {"file": "meshes/lrm.msh", "class": "missile"},
    {"file": "meshes/srm.msh", "class": "missile"},
    "tex/msl.bmp",
    "tex/trail.bmp", "tex/trail_a.bmp",
    "tex/fx_fire.bmp", "tex/fx_fire_a.bmp",
    "tex/fx_smoke.bmp", "tex/fx_smoke_a.bmp",
    "tex/flare.bmp",
    "tex/debris.bmp",
    "tex/splash.bmp", "tex/splash_a.bmp",
    "tex/dot.bmp",
]

LIGHT_SUN = (255, 244, 226)     # A2 light set (light.txt)
LIGHT_AMB = (132, 150, 178)
FOG = (196, 212, 228)


# ---------------------------------------------------------------------------------------------------- helpers

def clamp(v, a=0.0, b=1.0):
    return a if v < a else (b if v > b else v)


def smooth(e0, e1, x):
    if e1 == e0:
        return 1.0 if x >= e1 else 0.0
    t = clamp((x - e0) / (e1 - e0))
    return t * t * (3.0 - 2.0 * t)


def lerp3(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def ramp(stops, t):
    """Piecewise linear colour ramp: stops = [(t, (r, g, b)), ...] ascending."""
    if t <= stops[0][0]:
        return stops[0][1]
    for i in range(1, len(stops)):
        if t <= stops[i][0]:
            t0, c0 = stops[i - 1]
            t1, c1 = stops[i]
            return lerp3(c0, c1, (t - t0) / (t1 - t0))
    return stops[-1][1]


def edge_window(x, y, m=0.06):
    """1 inside, 0 on the cell border (bilinear / mip safety)."""
    return smooth(0.0, m, min(x, 1.0 - x, y, 1.0 - y))


def fill_clear(img):
    """Straight-alpha texture: give fully transparent texels (alpha 0) the average colour of the faint edge
    texels (alpha 1..64), so filtering never pulls in black. Semi-transparent texels keep their colour (the
    assetkit's save_bmp bleed would overwrite every texel below alpha 255 near an opaque one)."""
    px = img.load()
    w, h = img.size
    acc = [0, 0, 0, 0]
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if 0 < a <= 64:
                acc[0] += r
                acc[1] += g
                acc[2] += b
                acc[3] += 1
    if acc[3]:
        c = (acc[0] // acc[3], acc[1] // acc[3], acc[2] // acc[3], 0)
        for y in range(h):
            for x in range(w):
                if px[x, y][3] == 0:
                    px[x, y] = c
    return img


def blob_field(blobs, x, y, wrap_y=None, prof=1.6):
    """Sum of soft blob thicknesses at (x, y). blobs: (bx, by, r, weight). Profile (1 - d^2)^prof (soft edge, no
    hard sphere rim). Returns (thick, nx, ny, nz) with n the thickness-weighted sphere normal (x right, y down,
    z toward the viewer). wrap_y: period for tiling in y."""
    th = nx = ny = nz = 0.0
    for bx, by, r, w in blobs:
        dx = x - bx
        dy = y - by
        if wrap_y is not None:
            dy -= round(dy / wrap_y) * wrap_y
        dx /= r
        dy /= r
        d2 = dx * dx + dy * dy
        if d2 < 1.0:
            q = 1.0 - d2
            h = math.sqrt(q)
            t = (q ** prof) * r * w
            th += t
            nx += dx * t
            ny += dy * t
            nz += h * t
    ln = math.sqrt(nx * nx + ny * ny + nz * nz)
    if ln > 1e-9:
        return th, nx / ln, ny / ln, nz / ln
    return th, 0.0, 0.0, 1.0


def fbm_r(x, y, octaves=4, seed=0):
    """fBm on rotated coordinates with a non-integer lacunarity: value-noise octaves with lacunarity 2 put
    their zero-derivative lattice creases on the same lines, which shows as straight seams."""
    return fbm(0.8 * x + 0.6 * y + 0.31, -0.6 * x + 0.8 * y + 0.57, octaves=octaves, lacunarity=2.13, seed=seed)


def warp(x, y, amt, scale, seed, period=None):
    """Domain warp by two fbm fields (breaks circular outlines)."""
    if period is not None:
        wx = fbm(x * scale, y * scale, octaves=3, seed=seed, period=period)
        wy = fbm(x * scale + 17.0 * period[0], y * scale, octaves=3, seed=seed + 51, period=period)
    else:
        wx = fbm_r(x * scale, y * scale, octaves=3, seed=seed)
        wy = fbm_r(x * scale + 31.7, y * scale + 11.3, octaves=3, seed=seed + 51)
    return x + amt * (wx - 0.5), y + amt * (wy - 0.5)


def cluster(rng, n_big, n_small, cx, cy, rad, rbig, rsmall, sx=1.0, sy=1.0, up=0.0):
    """Billow cluster: big blobs inside `rad`, small blobs on their rims (upper side favoured by `up`)."""
    big = []
    for i in range(n_big):
        a = rng.uniform(0.0, 2.0 * math.pi)
        d = rad * math.sqrt(rng.random())
        big.append((cx + math.cos(a) * d * sx, cy + math.sin(a) * d * sy, rng.uniform(rbig[0], rbig[1]),
                    rng.uniform(0.8, 1.2)))
    out = list(big)
    for i in range(n_small):
        bx, by, br, bw = big[rng.int(0, len(big) - 1)]
        a = rng.uniform(0.0, 2.0 * math.pi)
        if up and rng.chance(up):
            a = rng.uniform(math.pi * 1.1, math.pi * 1.9)          # image y down: upper rim
        d = br * rng.uniform(0.55, 0.95)
        out.append((bx + math.cos(a) * d, by + math.sin(a) * d, rng.uniform(rsmall[0], rsmall[1]),
                    rng.uniform(0.7, 1.1)))
    return out


L_IMG = (lambda v: (v[0] / math.sqrt(sum(c * c for c in v)), v[1] / math.sqrt(sum(c * c for c in v)),
                    v[2] / math.sqrt(sum(c * c for c in v))))((-0.45, -0.70, 0.55))   # image space: upper left, front


# ---------------------------------------------------------------------------------------------------- missiles
LRM = {"name": "lrm", "L": 3.65, "r": 0.090, "strip": (0, 0, 128, 32),
       # nose profile (distance from the tip, radius fraction)
       "nose": [(0.0, 0.0), (0.24, 0.55), (0.50, 0.86), (0.74, 1.0)],
       "tail": [(0.20, 0.84)],              # boattail: (distance from the tail end, radius fraction)
       # fins: (root LE from nose tip, root TE, tip LE, tip TE, semi-span beyond the body), atlas panel
       "fins": [((1.27, 2.10, 1.80, 2.02, 0.115), (0, 64, 32, 32)),
                ((3.10, 3.60, 3.40, 3.60, 0.185), (32, 64, 32, 32))],
       "lod0": 120.0}
SRM = {"name": "srm", "L": 2.85, "r": 0.064, "strip": (0, 32, 128, 32),
       "nose": [(0.0, 0.0), (0.035, 0.62), (0.07, 0.80), (0.28, 1.0)],
       "tail": [(0.10, 0.88)],
       "fins": [((0.38, 0.66, 0.58, 0.68, 0.095), (64, 64, 32, 32)),
                ((2.32, 2.80, 2.66, 2.80, 0.120), (96, 64, 32, 32))],
       "lod0": 120.0}
NOZZLE_RECT = (0, 96, 32, 32)
TEXN = 128
SEG = 8


def uv_px(x, y):
    return ((x + 0.0) / TEXN, (y + 0.0) / TEXN)


def strip_uv(rect, t, a):
    """Body strip: t 0 nose tip -> 1 tail end along x, a 0..1 around (0 and 1 = top) along y; half-texel inset."""
    x, y, w, h = rect
    return uv_px(x + 0.5 + t * (w - 1.0), y + 0.5 + a * (h - 1.0))


def orient(g, t, outward):
    """Make triangle t face `outward` (clockwise from the front: assetkit normal = cross(b - a, c - a))."""
    a, b, c = g.T[t]
    n = M.face_normal(g.P[a], g.P[b], g.P[c])
    if M.vdot(n, outward) < 0.0:
        g.T[t] = (a, c, b)


def body_geo(spec, seg, mat, cap_mat):
    """Lathe-like body with atlas UVs. Rings from nose tip to tail end; the tail gets a flat nozzle cap."""
    L, R = spec["L"], spec["r"]
    z0 = 0.5 * L                                   # nose tip z (origin at mid-length)
    prof = [(d, f) for d, f in spec["nose"]]
    for d, f in reversed(spec["tail"]):
        prof.append((L - d, 1.0))
    prof.append((L, spec["tail"][0][1] if seg > 4 else 0.9))
    if seg <= 4:                                   # LOD1: tip, full radius, tail
        prof = [(0.0, 0.0), (spec["nose"][-1][0], 1.0), (L, 0.92)]
    g = M.Geo(mat=mat)
    rings = []
    for d, f in prof:
        z = z0 - d
        r = R * f
        t = d / L
        ring = []
        for k in range(seg + 1):
            th = 2.0 * math.pi * k / seg + (math.pi / 4.0 if seg == 4 else 0.0)
            p = (r * math.sin(th), r * math.cos(th), z)
            nrm = M.vnorm((math.sin(th), math.cos(th), 0.0))
            ring.append(g.vert(p, n=nrm, uv=strip_uv(spec["strip"], t, k / float(seg))))
        rings.append((z, r, ring))
    for i in range(len(rings) - 1):
        za, ra, A = rings[i]
        zb, rb, B = rings[i + 1]
        for k in range(seg):
            mid_th = 2.0 * math.pi * (k + 0.5) / seg + (math.pi / 4.0 if seg == 4 else 0.0)
            out = (math.sin(mid_th), math.cos(mid_th), 0.0)
            if ra < 1e-6:                          # nose cone fan (own tip vertex per segment for the UVs)
                tip = g.vert(g.P[A[k]], n=(0, 0, 1), uv=strip_uv(spec["strip"], 0.0, (k + 0.5) / seg))
                g.tri(tip, B[k + 1], B[k])
                orient(g, g.ntris - 1, M.vadd(out, (0, 0, 0.5)))
            else:
                g.tri(A[k], A[k + 1], B[k + 1])
                orient(g, g.ntris - 1, out)
                g.tri(A[k], B[k + 1], B[k])
                orient(g, g.ntris - 1, out)
    # tail cap (nozzle disc)
    zt, rt, T = rings[-1]
    x, y, w, h = NOZZLE_RECT
    cu, cv = uv_px(x + w * 0.5, y + h * 0.5)
    c = g.vert((0.0, 0.0, zt), n=(0, 0, -1), uv=(cu, cv))
    for k in range(seg):
        pa, pb = g.P[T[k]], g.P[T[k + 1]]
        ua = uv_px(x + w * 0.5 + (w * 0.5 - 1) * pa[0] / rt, y + h * 0.5 - (h * 0.5 - 1) * pa[1] / rt)
        ub = uv_px(x + w * 0.5 + (w * 0.5 - 1) * pb[0] / rt, y + h * 0.5 - (h * 0.5 - 1) * pb[1] / rt)
        ia = g.vert(pa, n=(0, 0, -1), uv=ua)
        ib = g.vert(pb, n=(0, 0, -1), uv=ub)
        g.tri(c, ia, ib, mat=cap_mat)
        orient(g, g.ntris - 1, (0, 0, -1))
    return g


def fin_geo(spec, fin, phi, mat, double=True):
    """One fin in the plane through the axis at roll angle phi (0 = up, clockwise seen from behind is +X first).
    Two culled faces 8 mm apart (double=True) or one cull-none face spanning both sides (LOD1 crossed planes)."""
    (le_r, te_r, le_t, te_t, span), rect = fin
    L, R = spec["L"], spec["r"]
    z0 = 0.5 * L
    d = (math.sin(phi), math.cos(phi), 0.0)                 # radial direction
    nrm = (math.cos(phi), -math.sin(phi), 0.0)              # fin plane normal
    r0 = R * 0.96
    r1 = R + span
    x, y, w, h = rect
    # planar UVs: u = radial (root 0 -> tip 1), v = chordwise along the full root chord
    zmin, zmax = z0 - te_r, z0 - le_r

    def puv(rr, z):
        u = (rr - r0) / (r1 - r0)
        v = (zmax - z) / (zmax - zmin)
        return uv_px(x + 0.5 + u * (w - 1.0), y + 0.5 + v * (h - 1.0))

    corners = [(r0, z0 - le_r), (r1, z0 - le_t), (r1, z0 - te_t), (r0, z0 - te_r)]
    g = M.Geo(mat=mat)
    if double:
        for side in (1.0, -1.0):
            off = M.vscale(nrm, 0.004 * side)
            idx = []
            for rr, z in corners:
                p = M.vadd((d[0] * rr, d[1] * rr, z), off)
                idx.append(g.vert(p, n=M.vscale(nrm, side), uv=puv(rr, z)))
            g.tri(idx[0], idx[1], idx[2])
            orient(g, g.ntris - 1, M.vscale(nrm, side))
            g.tri(idx[0], idx[2], idx[3])
            orient(g, g.ntris - 1, M.vscale(nrm, side))
    else:                                                    # crossed plane through the axis: both fins of a pair
        pts = [(-r1, z0 - le_t), (r1, z0 - le_t), (r1, z0 - te_t), (-r1, z0 - te_t)]
        idx = []
        for rr, z in pts:
            p = (d[0] * rr, d[1] * rr, z)
            idx.append(g.vert(p, n=nrm, uv=puv(abs(rr) * 0.5 + r0 * 0.5, z)))
        g.tri(idx[0], idx[1], idx[2])
        orient(g, g.ntris - 1, nrm)
        g.tri(idx[0], idx[2], idx[3])
        orient(g, g.ntris - 1, nrm)
    return g


def build_missile(spec):
    mb = M.MeshBuilder(spec["name"])
    body = mb.material("body", tex="msl")
    fin = mb.material("fin", tex="msl")
    fin2 = mb.material("fin2", tex="msl", cull_none=True)
    g = body_geo(spec, SEG, body, body)
    for f in spec["fins"]:
        for k in range(4):
            g.merge(fin_geo(spec, f, math.pi / 4.0 + k * math.pi / 2.0, fin))
    g.smooth(50.0)
    mb.lod(spec["lod0"])
    mb.add(g)
    g1 = body_geo(spec, 4, body, body)
    tail = spec["fins"][-1]
    for k in range(2):
        g1.merge(fin_geo(spec, tail, math.pi / 4.0 + k * math.pi / 2.0, fin2, double=False))
    g1.smooth(50.0)
    mb.lod()
    mb.add(g1)
    L = spec["L"]
    mb.tag("nozzle", (0.0, 0.0, -0.5 * L), (0.0, 0.0, -1.0))
    mb.tag("nose", (0.0, 0.0, 0.5 * L), (0.0, 0.0, 1.0))
    return mb


# ---------------------------------------------------------------------------------------------------- msl.bmp

def body_colour(kind, t, a, L):
    """Paint function of the body strips: t along (0 nose tip), a around (0/1 top, 0.5 bottom)."""
    s = t * L                                               # metres from the tip
    top = math.cos(2.0 * math.pi * a)                       # +1 top, -1 bottom
    if kind == "lrm":
        base = (232, 233, 228)
        if s < 0.74:                                        # radome: warmer off-white, slightly darker tip
            base = lerp3((206, 204, 196), (226, 224, 216), smooth(0.0, 0.5, s))
        col = base
        for c0, w0, cc in ((0.75, 0.025, (70, 72, 76)),      # radome joint ring
                           (1.20, 0.035, (64, 98, 112)),     # invented steel-teal band
                           (1.26, 0.035, (64, 98, 112)),
                           (2.12, 0.030, (96, 98, 100))):    # motor section band
            if abs(s - c0) < w0 * 0.5:
                col = cc
        for j in (0.74, 1.62, 2.12, 3.45):                  # section joints
            if abs(s - j) < 0.009:
                col = P.shade(col, 0.72)
        if s > 3.45:                                        # boattail: grey metal
            col = (120, 122, 124)
        if top > 0.92 and (abs(s - 1.40) < 0.06 or abs(s - 2.40) < 0.06):
            col = (88, 90, 94)                              # hanger lugs
        if top < -0.85 and 1.75 < s < 2.05:                 # stencil dashes underneath
            col = P.shade(col, 0.6) if int(s * 40) % 2 == 0 else col
    else:
        base = (200, 202, 200)
        col = base
        if s < 0.040:                                       # seeker dome: dark glass with a highlight
            col = (24, 30, 42)
            if top > 0.4:
                col = (120, 132, 150)
        elif s < 0.075:
            col = (54, 56, 60)                              # dome ring
        for c0, w0, cc in ((0.30, 0.03, (66, 68, 72)),
                           (0.95, 0.03, (150, 118, 58)),     # invented ochre band
                           (1.30, 0.025, (66, 68, 72))):
            if abs(s - c0) < w0 * 0.5:
                col = cc
        for j in (0.70, 0.95, 1.30, 2.75):
            if abs(s - j) < 0.008:
                col = P.shade(col, 0.72)
        if s > 2.75:
            col = (110, 112, 114)
        if top > 0.92 and (abs(s - 1.05) < 0.05 or abs(s - 1.75) < 0.05):
            col = (88, 90, 94)
    return col


def msl_texture():
    img = Image.new("RGB", (TEXN, TEXN), (120, 122, 124))
    px = img.load()
    for spec in (LRM, SRM):
        x0, y0, w, h = spec["strip"]
        for iy in range(h):
            a = (iy + 0.5 - 0.5) / (h - 1.0)
            for ix in range(w):
                t = clamp((ix + 0.5 - 0.5) / (w - 1.0))
                c = body_colour(spec["name"], t, a, spec["L"])
                n = value_noise(ix * 0.35, iy * 0.35 + y0, seed=SEED + 3)
                k = 0.97 + 0.05 * n
                px[x0 + ix, y0 + iy] = tuple(P.clamp8(c[i] * k) for i in range(3))
    # fin panels: base colour, darker hinge/root line, edge lines
    for spec, cols in ((LRM, ((226, 227, 222), (230, 231, 226))), (SRM, ((196, 198, 196), (192, 194, 192)))):
        for (fin, rect), col in zip(spec["fins"], cols):
            x0, y0, w, h = rect
            for iy in range(h):
                for ix in range(w):
                    c = col
                    if ix < 2:
                        c = P.shade(col, 0.70)              # root fillet
                    elif ix > w - 3 or iy < 2 or iy > h - 3:
                        c = P.shade(col, 0.85)              # edges
                    if spec is SRM and rect[0] == 96 and (ix - 26) ** 2 + (iy - 26) ** 2 < 16:
                        c = (70, 72, 76)                    # rolleron disc near the tip trailing edge
                    n = value_noise(ix * 0.4 + x0, iy * 0.4 + y0, seed=SEED + 4)
                    px[x0 + ix, y0 + iy] = tuple(P.clamp8(c[i] * (0.97 + 0.05 * n)) for i in range(3))
    # nozzle disc: dark metal with a black throat
    x0, y0, w, h = NOZZLE_RECT
    for iy in range(h):
        for ix in range(w):
            r = math.hypot((ix + 0.5) / w - 0.5, (iy + 0.5) / h - 0.5) * 2.0
            c = lerp3((18, 16, 15), (84, 82, 80), smooth(0.35, 0.8, r))
            px[x0 + ix, y0 + iy] = tuple(int(v) for v in c)
    return img


# ---------------------------------------------------------------------------------------------------- fire atlas

FIRE_RAMP = [(0.00, (0, 0, 0)), (0.16, (64, 12, 4)), (0.34, (160, 42, 8)), (0.50, (226, 88, 16)),
             (0.66, (255, 142, 36)), (0.82, (255, 196, 84)), (0.94, (255, 236, 168)), (1.0, (255, 250, 228))]
SOOT = (50, 44, 40)

FIRE_FRAMES = [
    # big/small blob counts, cluster radius, radii; soot cluster (n_big, n_small, rad, weight) or None;
    # temperature base / thickness gain / radial loss / turbulence; coverage K; opacity; halo (radius, amount);
    # warp; erosion
    dict(name="flash", big=6, small=16, rad=0.12, rbig=(0.14, 0.20), rsmall=(0.06, 0.10), soot=None,
         Tb=0.52, Tg=0.50, Tr=0.95, Tu=0.50, K=20.0, op=0.95, halo=(0.36, 0.60), warp=0.12, ero=0.35, rays=12),
    dict(name="fireball", big=8, small=24, rad=0.20, rbig=(0.12, 0.18), rsmall=(0.06, 0.10),
         soot=(5, 12, 0.32, 0.35), Tb=0.34, Tg=0.62, Tr=0.75, Tu=0.50, K=16.0, op=1.0, halo=(0.42, 0.22), warp=0.13,
         ero=0.40, rays=0),
    dict(name="breakup", big=9, small=26, rad=0.27, rbig=(0.10, 0.15), rsmall=(0.05, 0.09),
         soot=(8, 20, 0.33, 0.60), Tb=0.20, Tg=0.66, Tr=0.55, Tu=0.50, K=12.0, op=0.96, halo=(0.44, 0.10), warp=0.15,
         ero=0.60, rays=0),
    dict(name="dying", big=7, small=20, rad=0.28, rbig=(0.06, 0.10), rsmall=(0.035, 0.07),
         soot=(9, 24, 0.32, 0.90), Tb=0.08, Tg=0.64, Tr=0.32, Tu=0.45, K=9.0, op=0.85, halo=(0.42, 0.04), warp=0.17,
         ero=0.80, rays=0),
]


def fire_cell(fi, size=128):
    spec = FIRE_FRAMES[fi]
    rng = Rng(SEED, "fire", fi)
    blobs = cluster(rng, spec["big"], spec["small"], 0.5, 0.52, spec["rad"], spec["rbig"], spec["rsmall"])
    soot = []
    if spec["soot"]:
        nb, ns, srad, sw = spec["soot"]
        soot = [(bx, by, br, bw * sw) for bx, by, br, bw in
                cluster(rng.child("soot"), nb, ns, 0.5, 0.45, srad, (0.11, 0.17), (0.05, 0.10), up=0.6)]
    ray_ph = rng.uniform(0, 2 * math.pi)
    nr = max(1, spec["rays"])
    ray_w = [rng.uniform(0.4, 1.0) for _ in range(nr)]
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = img.load()
    seed = SEED + 100 + fi * 7
    for iy in range(size):
        y = (iy + 0.5) / size
        for ix in range(size):
            x = (ix + 0.5) / size
            win = edge_window(x, y, 0.10)
            if win <= 0.0:
                continue
            rc = math.hypot(x - 0.5, y - 0.5)
            wx, wy = warp(x, y, spec["warp"], 5.0, seed + 1)
            det = fbm_r(wx * 9.0, wy * 9.0, octaves=4, seed=seed)
            th, nx, ny, nz = blob_field(blobs, wx, wy)
            th = max(0.0, th * (1.0 + spec["ero"] * (det - 0.5) * 2.0) - spec["ero"] * 0.012)
            cov = 1.0 - math.exp(-spec["K"] * th)
            turb = fbm_r(wx * 16.0, wy * 16.0, octaves=3, seed=seed + 2)
            T = (spec["Tb"] + spec["Tg"] * min(1.0, th * 5.0) - spec["Tr"] * rc + spec["Tu"] * (turb - 0.5)
                 + 0.28 * (nz - 0.6))
            T = clamp(T)
            col = ramp(FIRE_RAMP, T)
            fa = cov * spec["op"]
            fr = [col[i] / 255.0 * fa for i in range(3)]
            hr, ha = spec["halo"]
            h = math.exp(-(rc / hr) ** 2) * ha
            if spec["rays"]:
                ang = math.atan2(y - 0.5, x - 0.5) + ray_ph
                k = int(((ang / (2 * math.pi)) % 1.0) * nr) % nr
                ray = abs(math.cos(ang * nr * 0.5)) ** 24 * ray_w[k]
                h += ray * math.exp(-(rc / 0.36) ** 2) * 0.55
            hc = ramp(FIRE_RAMP, clamp(0.55 + 0.35 * math.exp(-(rc / 0.2) ** 2)))
            for i in range(3):
                fr[i] = fr[i] + hc[i] / 255.0 * h * (1.0 - fa)
            if soot:                                         # premultiplied "fire over soot"
                sth, snx, sny, snz = blob_field(soot, wx, wy)
                dets = fbm_r(wx * 7.0, wy * 7.0, octaves=4, seed=seed + 3)
                sth = max(0.0, sth * (1.0 + 0.9 * (dets - 0.5) * 2.0) - 0.006)
                sa = (1.0 - math.exp(-14.0 * sth)) * 0.93
                lit = max(0.0, snx * L_IMG[0] + sny * L_IMG[1] + snz * L_IMG[2])
                shade = (0.55 + 0.6 * lit) * (0.8 + 0.4 * dets)
                glow = clamp(th * 5.0) * 0.45 * (1.0 - 0.6 * fi / 3.0)     # fire light on the soot
                sc = (SOOT[0] / 255.0 * shade + 0.32 * glow, SOOT[1] / 255.0 * shade + 0.10 * glow,
                      SOOT[2] / 255.0 * shade + 0.02 * glow)
                for i in range(3):
                    fr[i] = fr[i] + sc[i] * sa * (1.0 - fa)
                fa = fa + sa * (1.0 - fa)
            px[ix, iy] = (P.clamp8(fr[0] * 255 * win), P.clamp8(fr[1] * 255 * win), P.clamp8(fr[2] * 255 * win),
                          P.clamp8(fa * 255 * win))
    return img


def fire_atlas():
    img = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    for i in range(4):
        img.paste(fire_cell(i), ((i % 2) * 128, (i // 2) * 128))
    return img


# ---------------------------------------------------------------------------------------------------- smoke atlas

SMOKE_VARIANTS = [
    dict(name="dense", big=6, small=18, rad=0.15, rbig=(0.16, 0.22), rsmall=(0.06, 0.10), sx=1.0, sy=0.9, K=9.0,
         op=0.96, ero=0.40, warp=0.10, up=0.5),
    dict(name="lumpy", big=8, small=24, rad=0.22, rbig=(0.11, 0.17), rsmall=(0.05, 0.09), sx=1.05, sy=0.85, K=10.0,
         op=0.93, ero=0.50, warp=0.12, up=0.6),
    dict(name="wisp", big=8, small=18, rad=0.27, rbig=(0.12, 0.17), rsmall=(0.06, 0.10), sx=1.25, sy=0.45, K=9.0,
         op=0.88, ero=0.55, warp=0.16, up=0.4),
    dict(name="torn", big=8, small=20, rad=0.24, rbig=(0.11, 0.16), rsmall=(0.05, 0.09), sx=1.1, sy=0.9, K=8.0,
         op=0.84, ero=0.80, warp=0.20, up=0.4),
]
SMOKE_LIT = (252, 250, 246)
SMOKE_SHADE = (138, 146, 162)


def smoke_cell(vi, size=128):
    spec = SMOKE_VARIANTS[vi]
    rng = Rng(SEED, "smoke", vi)
    blobs = cluster(rng, spec["big"], spec["small"], 0.5, 0.52, spec["rad"], spec["rbig"], spec["rsmall"],
                    spec["sx"], spec["sy"], spec["up"])
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = img.load()
    seed = SEED + 200 + vi * 7
    for iy in range(size):
        y = (iy + 0.5) / size
        for ix in range(size):
            x = (ix + 0.5) / size
            win = edge_window(x, y, 0.10)
            if win <= 0.0:
                continue
            wx, wy = warp(x, y, spec["warp"], 5.0, seed + 1)
            th, nx, ny, nz = blob_field(blobs, wx, wy)
            if th <= 0.0:
                continue
            det = fbm_r(wx * 8.0, wy * 8.0, octaves=4, seed=seed)
            th = max(0.0, th * (1.0 + spec["ero"] * (det - 0.5) * 2.0) - spec["ero"] * 0.010)
            a = (1.0 - math.exp(-spec["K"] * th)) * spec["op"] * win
            lit = max(0.0, nx * L_IMG[0] + ny * L_IMG[1] + nz * L_IMG[2])
            lit = clamp((0.22 + 0.78 * lit) * (0.82 + 0.36 * det))
            c = lerp3(SMOKE_SHADE, SMOKE_LIT, lit)
            px[ix, iy] = (P.clamp8(c[0]), P.clamp8(c[1]), P.clamp8(c[2]), P.clamp8(a * 255))
    return img


def smoke_atlas():
    img = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    for i in range(4):
        img.paste(smoke_cell(i), ((i % 2) * 128, (i // 2) * 128))
    return img


# ---------------------------------------------------------------------------------------------------- trail

TRAIL_W, TRAIL_H = 64, 256
TRAIL_PERIOD = 4.0              # texture height in ribbon widths (square texels when v repeats every 4 widths)


def trail_texture():
    rng = Rng(SEED, "trail")
    blobs = []
    n = 22
    for i in range(n):
        y = (i + rng.uniform(-0.4, 0.4)) * TRAIL_PERIOD / n
        x = 0.5 + rng.uniform(-0.12, 0.12)
        blobs.append((x, y, rng.uniform(0.18, 0.32), rng.uniform(0.8, 1.2)))
    img = Image.new("RGBA", (TRAIL_W, TRAIL_H), (0, 0, 0, 0))
    px = img.load()
    per = (64, 16)                  # lattice period: x * 4 never wraps, y * 4 in [0, 16) tiles
    for iy in range(TRAIL_H):
        y = (iy + 0.5) / TRAIL_H * TRAIL_PERIOD
        for ix in range(TRAIL_W):
            x = (ix + 0.5) / TRAIL_W
            wxn = fbm(x * 4.0, y * 4.0, octaves=3, seed=SEED + 310, period=per)
            wyn = fbm(x * 4.0 + 7.0, y * 4.0, octaves=3, seed=SEED + 311, period=per)
            wx, wy = x + 0.10 * (wxn - 0.5), y + 0.14 * (wyn - 0.5)
            th, nx, ny, nz = blob_field(blobs, wx, wy, wrap_y=TRAIL_PERIOD)
            det = fbm(wx * 4.0, wy * 4.0, octaves=4, seed=SEED + 300, period=per)
            th = max(0.0, th * (0.70 + 0.6 * det) - 0.004)
            a = (1.0 - math.exp(-8.0 * th)) * 0.92
            a *= smooth(0.0, 0.18, x) * smooth(1.0, 0.82, x)
            lit = clamp((0.50 + 0.50 * nz - 0.10 * nx) * (0.85 + 0.3 * det))
            c = lerp3((188, 196, 208), (250, 250, 246), lit)
            px[ix, iy] = (P.clamp8(c[0]), P.clamp8(c[1]), P.clamp8(c[2]), P.clamp8(a * 255))
    return img


# ---------------------------------------------------------------------------------------------------- flare, dot

def flare_texture(N=64):
    S = 2
    big = Image.new("RGB", (N * S, N * S), (0, 0, 0))
    px = big.load()
    nr = 6
    for iy in range(N * S):
        for ix in range(N * S):
            dx = (ix + 0.5) / (N * S) - 0.5
            dy = (iy + 0.5) / (N * S) - 0.5
            r = math.hypot(dx, dy) * 2.0
            if r >= 1.0:
                continue
            core = math.exp(-(r / 0.10) ** 2)
            glow = math.exp(-(r / 0.30) ** 2)
            halo = (1.0 - r) ** 2.5
            ang = math.atan2(dy, dx)
            ray = abs(math.cos(ang * nr * 0.5)) ** 40 * (1.0 - r) ** 1.5
            inten = core * 1.0 + glow * 0.55 + halo * 0.22 + ray * 0.35
            c = lerp3((255, 132, 48), (255, 236, 176), clamp(glow * 1.2))
            c = lerp3(c, (255, 255, 255), clamp(core * 1.3))
            w = smooth(1.0, 0.85, r)
            px[ix, iy] = tuple(P.clamp8(c[i] * min(1.0, inten) * w) for i in range(3))
    return big.resize((N, N), Image.BOX)


def dot_texture(N=16):
    img = Image.new("RGB", (N, N), (0, 0, 0))
    px = img.load()
    for iy in range(N):
        for ix in range(N):
            r = math.hypot((ix + 0.5) / N - 0.5, (iy + 0.5) / N - 0.5) * 2.0
            v = math.exp(-(r / 0.55) ** 2) * smooth(1.0, 0.8, r)
            px[ix, iy] = (P.clamp8(255 * v),) * 3
    return img


# ---------------------------------------------------------------------------------------------------- debris

def debris_texture():
    rng = Rng(SEED, "debris")
    N, C = 64, 16
    tones = [(150, 154, 158), (128, 131, 136), (96, 98, 102), (70, 72, 76), (168, 170, 172), (58, 54, 50)]

    def draw(d, im, s):
        for cell in range(16):
            cx = ((cell % 4) + 0.5) * C * s
            cy = ((cell // 4) + 0.5) * C * s
            r = rng.child(cell)
            nv = r.int(5, 8)
            base = r.uniform(0, 2 * math.pi)
            pts = []
            for k in range(nv):
                a = base + 2 * math.pi * k / nv + r.uniform(-0.25, 0.25)
                rr = r.uniform(0.18, 0.44) * C * s
                if r.chance(0.3):
                    rr *= 0.55                               # jagged notch
                pts.append((cx + math.cos(a) * rr * (1.0 if cell % 3 else 0.6), cy + math.sin(a) * rr))
            tone = tones[cell % len(tones)]
            d.polygon(pts, fill=tone + (255,))
            # panel line / burnt streak
            if cell % 2 == 0:
                a = r.uniform(0, math.pi)
                dx, dy = math.cos(a) * C * 0.35 * s, math.sin(a) * C * 0.35 * s
                d.line((cx - dx, cy - dy, cx + dx, cy + dy), fill=P.shade(tone, 0.6) + (255,), width=max(1, s))
            if cell % 5 == 1:
                d.polygon([(px_ * 0.5 + cx * 0.5, py_ * 0.5 + cy * 0.5) for px_, py_ in pts[:3]] + [(cx, cy)],
                          fill=(40, 34, 30, 255))
        return d

    img = I.supersample(N, N, lambda d, im, s: draw(d, im, s))
    # keep a 1 px transparent gutter around every cell
    px = img.load()
    for y in range(N):
        for x in range(N):
            if x % C in (0, C - 1) or y % C in (0, C - 1):
                px[x, y] = (0, 0, 0, 0)
    return img


# ---------------------------------------------------------------------------------------------------- splash

def splash_texture(N=128):
    """Spray plume from the impact point: a column that widens upward (cone), feathery vertical spray streaks, a
    lumpy rounded top whose edges droop outward, a wide low base skirt. y = 0 top of the image, 1 = the water."""
    rng = Rng(SEED, "splash")
    lumps = []
    for i in range(14):                                    # rounded lumpy top, edges drooping
        u = (i + 0.5) / 14.0 * 2.0 - 1.0
        lumps.append((0.5 + u * 0.27, 0.22 + 0.14 * u * u + rng.uniform(-0.025, 0.025), rng.uniform(0.05, 0.085),
                      rng.uniform(0.45, 0.8)))
    skirt = []
    for i in range(14):
        x = 0.5 + rng.uniform(-0.30, 0.30)
        skirt.append((x, rng.uniform(0.86, 0.93), rng.uniform(0.07, 0.11) * (1.2 - abs(x - 0.5)), rng.uniform(0.8, 1.1)))
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    px = img.load()
    for iy in range(N):
        y = (iy + 0.5) / N
        for ix in range(N):
            x = (ix + 0.5) / N
            wx, wy = warp(x, y, 0.05, 6.0, SEED + 401)
            h = clamp((0.95 - wy) / 0.75)                  # 0 at the water, 1 at the crown
            half = 0.10 + 0.22 * h ** 0.8
            dx = (wx - 0.5) / half
            streak = (0.6 * value_noise_1d(wx * 24.0, seed=SEED + 402)          # vertical spray streaks
                      + 0.4 * fbm_r(wx * 9.0, wy * 9.0, octaves=3, seed=SEED + 403))
            body = math.exp(-dx * dx * 1.4) * (0.60 + 0.9 * streak) * smooth(0.97, 0.88, wy)
            top = 0.22 + 0.14 * ((wx - 0.5) / 0.27) ** 2   # crown line: body fades above it
            body *= smooth(top - 0.05, top + 0.10, wy)
            th, nx, ny, nz = blob_field(lumps + skirt, wx, wy)
            det = fbm_r(wx * 10.0, wy * 10.0, octaves=4, seed=SEED + 400)
            th = max(0.0, th * (0.5 + 1.0 * det) - 0.004)
            dens = body * 1.35 + 0.8 * (1.0 - math.exp(-12.0 * th))
            a = clamp(dens) * (0.70 + 0.30 * smooth(0.1, 0.7, y))
            a *= smooth(0.0, 0.06, x) * smooth(1.0, 0.94, x) * smooth(0.0, 0.08, y) * smooth(1.0, 0.95, y)
            # light from the upper left: blob normals where lumps dominate, else the column's side
            side = clamp(0.5 - 0.5 * dx)
            lit_b = max(0.0, nx * L_IMG[0] + ny * L_IMG[1] + nz * L_IMG[2])
            wb = clamp(th * 8.0)
            lit = (side * (1.0 - wb) + lit_b * wb)
            lit = clamp((0.35 + 0.70 * lit) * (0.85 + 0.3 * det))
            c = lerp3((138, 160, 178), (252, 252, 250), lit)
            if y > 0.80:                                   # sea-tinted base
                c = lerp3(c, (160, 190, 200), smooth(0.80, 1.0, y) * 0.5)
            px[ix, iy] = (P.clamp8(c[0]), P.clamp8(c[1]), P.clamp8(c[2]), P.clamp8(a * 255))
    return img


# ---------------------------------------------------------------------------------------------------- 2D compositor
# Mock-only: billboards and ribbons composited onto a backdrop with the engine's blend equations
# (RB_BLEND = src*a + dst*(1-a), RB_ADD = dst + src, RB_PREMUL = src + dst*(1-a), RB_ALPHATEST = a >= 128).

def tex_rgba(kit, name):
    """Texture as the engine sees it: colour from <name>.bmp, alpha from <name>_a.bmp (or the key)."""
    return I.load_texture(kit.out("tex/%s.bmp" % name))


def premul_of(img):
    """Straight RGBA -> (premultiplied RGB, alpha L)."""
    r, g, b, a = img.split()
    rgb = Image.merge("RGB", (r, g, b))
    return ImageChops.multiply(rgb, Image.merge("RGB", (a, a, a))), a


def blend_into(fb, src, pos, mode, colour=(255, 255, 255, 255)):
    """src RGBA patch (for 'premul': RGB already premultiplied) at integer pos with vertex colour (multiplies RGB
    and A like D3D MODULATE)."""
    x0, y0 = pos
    W, H = fb.size
    w, h = src.size
    cx0, cy0 = max(0, x0), max(0, y0)
    cx1, cy1 = min(W, x0 + w), min(H, y0 + h)
    if cx1 <= cx0 or cy1 <= cy0:
        return
    s = src.crop((cx0 - x0, cy0 - y0, cx1 - x0, cy1 - y0))
    r, g, b, a = s.split()
    rgb = Image.merge("RGB", (r, g, b))
    if colour[:3] != (255, 255, 255):
        rgb = ImageChops.multiply(rgb, Image.new("RGB", rgb.size, tuple(int(c) for c in colour[:3])))
    if colour[3] != 255:
        a = a.point(lambda v, k=colour[3]: v * k // 255)
    dst = fb.crop((cx0, cy0, cx1, cy1))
    if mode == "add":
        out = ImageChops.add(dst, rgb)
    elif mode == "premul":
        if colour[3] != 255:                                 # premultiplied fade: RGB x alpha too
            rgb = rgb.point(lambda v, k=colour[3]: v * k // 255)
        inv = ImageChops.invert(a)
        out = ImageChops.add(ImageChops.multiply(dst, Image.merge("RGB", (inv, inv, inv))), rgb)
    elif mode == "test":
        out = Image.composite(rgb, dst, a.point(lambda v: 255 if v >= 128 else 0))
    else:
        out = Image.composite(rgb, dst, a)
    fb.paste(out, (cx0, cy0))


def sprite(fb, tex, cell, centre, size, mode, colour=(255, 255, 255, 255), rot=0.0, mirror=False, aspect=1.0,
           anchor="centre"):
    """Billboard: atlas cell rect (x, y, w, h) of an RGBA texture -> screen. size = width in px."""
    if size < 0.6:
        return
    x, y, w, h = cell
    c = tex.crop((x, y, x + w, y + h))
    if mirror:
        c = c.transpose(Image.FLIP_LEFT_RIGHT)
    sw = max(1, int(round(size)))
    sh = max(1, int(round(size * aspect)))
    c = c.resize((sw, sh), Image.BILINEAR if sw >= w else Image.BOX)
    if rot:
        c = c.rotate(rot, resample=Image.BILINEAR, expand=True)
    if anchor == "bottom":
        pos = (int(round(centre[0] - c.size[0] * 0.5)), int(round(centre[1] - c.size[1])))
    else:
        pos = (int(round(centre[0] - c.size[0] * 0.5)), int(round(centre[1] - c.size[1] * 0.5)))
    blend_into(fb, c, pos, mode, colour)


def solve8(A, b):
    n = len(b)
    M_ = [list(A[i]) + [b[i]] for i in range(n)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(M_[r][c]))
        M_[c], M_[p] = M_[p], M_[c]
        piv = M_[c][c]
        if abs(piv) < 1e-12:
            return None
        for r in range(n):
            if r != c:
                f = M_[r][c] / piv
                if f:
                    for k in range(c, n + 1):
                        M_[r][k] -= f * M_[c][k]
    return [M_[i][n] / M_[i][i] for i in range(n)]


def quad_textured(fb, tex, src_quad, dst_quad, mode, colour):
    """Perspective-map the source quad (texture px) onto the destination quad (screen px)."""
    xs = [p[0] for p in dst_quad]
    ys = [p[1] for p in dst_quad]
    bx0, by0 = int(math.floor(min(xs))), int(math.floor(min(ys)))
    bx1, by1 = int(math.ceil(max(xs))) + 1, int(math.ceil(max(ys))) + 1
    W, H = fb.size
    if bx1 <= 0 or by1 <= 0 or bx0 >= W or by0 >= H or bx1 - bx0 < 1 or by1 - by0 < 1:
        return
    A, bb = [], []
    for (X, Y), (u, v) in zip(dst_quad, src_quad):
        X -= bx0
        Y -= by0
        A.append([X, Y, 1, 0, 0, 0, -u * X, -u * Y])
        bb.append(u)
        A.append([0, 0, 0, X, Y, 1, -v * X, -v * Y])
        bb.append(v)
    co = solve8(A, bb)
    if co is None:
        return
    size = (bx1 - bx0, by1 - by0)
    patch = tex.transform(size, Image.PERSPECTIVE, tuple(co), Image.BILINEAR)
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).polygon([(X - bx0, Y - by0) for X, Y in dst_quad], fill=255)
    r, g, b, a = patch.split()
    patch = Image.merge("RGBA", (r, g, b, ImageChops.multiply(a, mask)))
    blend_into(fb, patch, (bx0, by0), mode, colour)


class Cam2(object):
    """Pinhole camera for mocks: camera space x right, y up, z forward (metres); horizon row cy."""

    def __init__(self, W, H, hfov=70.0, cy=None):
        self.W, self.H = W, H
        self.f = (W * 0.5) / math.tan(math.radians(hfov) * 0.5)
        self.cx = W * 0.5
        self.cy = H * 0.5 if cy is None else cy

    def proj(self, p):
        z = max(0.5, p[2])
        return (self.cx + p[0] * self.f / z, self.cy - p[1] * self.f / z, z)

    def px(self, metres, z):
        return metres * self.f / max(0.5, z)


def ribbon(fb, cam, tex, pts, widths, alphas, colour, v_metres):
    """Camera-facing trail ribbon through 3D points (camera space) with per-point width (m) and alpha."""
    scr = [cam.proj(p) for p in pts]
    n = len(pts)
    levels = []                                              # mip chain (the engine mips the straight-alpha trail)
    lv = tex
    while True:
        w_, h_ = lv.size
        t3 = Image.new("RGBA", (w_, h_ * 3))
        for k in range(3):
            t3.paste(lv, (0, k * h_))
        levels.append(t3)
        if w_ <= 2:
            break
        lv = lv.resize((w_ // 2, h_ // 2), Image.BOX)
    TW0 = tex.size[0]
    # cumulative distance -> v
    s = [0.0]
    for i in range(1, n):
        s.append(s[-1] + M.vlen(M.vsub(pts[i], pts[i - 1])))
    edges = []
    for i in range(n):
        a = scr[max(0, i - 1)]
        b = scr[min(n - 1, i + 1)]
        dx, dy = b[0] - a[0], b[1] - a[1]
        ln = math.hypot(dx, dy) or 1.0
        nx, ny = -dy / ln, dx / ln
        hw = 0.5 * cam.px(widths[i], scr[i][2])
        hw = max(hw, 1.0)                                    # T17 rule: never thinner than 2 px
        edges.append(((scr[i][0] + nx * hw, scr[i][1] + ny * hw), (scr[i][0] - nx * hw, scr[i][1] - ny * hw)))
    for i in range(n - 1):
        v0 = (s[i] / v_metres) % 1.0
        v1 = v0 + (s[i + 1] - s[i]) / v_metres
        if v1 > 2.9:
            v1 = v0 + 1.0
        a = 0.5 * (alphas[i] + alphas[i + 1])
        if a <= 0.004:
            continue
        wpx = 0.5 * (math.hypot(edges[i][0][0] - edges[i][1][0], edges[i][0][1] - edges[i][1][1]) +
                     math.hypot(edges[i + 1][0][0] - edges[i + 1][1][0], edges[i + 1][0][1] - edges[i + 1][1][1]))
        lvl = 0
        while lvl < len(levels) - 1 and TW0 / float(1 << (lvl + 1)) >= wpx:
            lvl += 1
        tl = levels[lvl]
        TW_, TH_ = tl.size[0], tl.size[1] // 3
        src = [(0.5 * TW_ / TW0, (v0 + 1.0) * TH_), (TW_ - 0.5 * TW_ / TW0, (v0 + 1.0) * TH_),
               (TW_ - 0.5 * TW_ / TW0, (v1 + 1.0) * TH_), (0.5 * TW_ / TW0, (v1 + 1.0) * TH_)]
        dst = [edges[i][0], edges[i][1], edges[i + 1][1], edges[i + 1][0]]
        quad_textured(fb, tl, src, dst, "blend", (colour[0], colour[1], colour[2], int(255 * a)))


# ---------------------------------------------------------------------------------------------------- effect recipes
# The same recipes are written down for T17 in docs/HANDOFF.md (## A8).

CELL = [(0, 0, 128, 128), (128, 0, 128, 128), (0, 128, 128, 128), (128, 128, 128, 128)]


def lit_smoke(base, k_sun=0.55):
    """Smoke vertex colour: base x (ambient + sun) normalised (what T17 would compute once per frame)."""
    am, su = LIGHT_AMB, LIGHT_SUN
    return tuple(P.clamp8(base[i] * (am[i] / 255.0 * 0.75 + su[i] / 255.0 * k_sun) / 1.05) for i in range(3))


def explosion_draws(t, rng_seed, scale=1.0):
    """Billboards of one explosion `t` s after detonation. Returns [(kind, offset (x, y, z) m, size m, args)]
    kinds: smoke (variant, rgb, alpha, rot, mirror), fire (frame, weight, rot, mirror, bright), flash (k)."""
    r = Rng(SEED, "expl", rng_seed)
    out = []
    # smoke: 6 puffs, start 0.25-0.6 s, life 6 s, 8 -> 30 m, rising 3 m/s, dark -> grey
    for i in range(6):
        d = r.uniform(0.25, 0.6)
        life = r.uniform(5.0, 7.0)
        a = t - d
        o = (r.uniform(-5, 5), r.uniform(-3, 4), r.uniform(-4, 4))
        var = r.int(0, 2)
        rot = r.uniform(-25, 25)
        mir = r.chance(0.5)
        if 0.0 <= a < life:
            u = a / life
            size = (8.0 + 22.0 * (1.0 - (1.0 - u) ** 2)) * scale
            col = lerp3((52, 48, 44), (118, 116, 114), smooth(0.0, 0.6, u))
            alpha = 0.88 * smooth(0.0, 0.35, a) * (1.0 - u) ** 1.3
            off = (o[0] * (1 + u * 1.5) * scale, (o[1] * (1 + u) + 3.0 * a) * scale, o[2] * scale)
            out.append(("smoke", off, size, (var, col, alpha, rot, mir)))
    # fire: 7 billboards, delays 0-0.25 s, life 1.1-1.5 s, frames 0..3 with a cross-fade
    for i in range(7):
        d = 0.0 if i == 0 else r.uniform(0.02, 0.25)
        life = r.uniform(1.1, 1.5)
        o = (0.0, 0.0, 0.0) if i == 0 else (r.uniform(-1, 1), r.uniform(-0.7, 1), r.uniform(-1, 1))
        dist = 0.0 if i == 0 else r.uniform(3.0, 6.0)
        s0 = 9.0 if i == 0 else r.uniform(5.0, 8.0)
        rot = r.uniform(0, 360)
        mir = r.chance(0.5)
        a = t - d
        if 0.0 <= a < life:
            u = a / life
            f = u * 3.999
            k0 = int(f)
            frac = f - k0
            grow = 0.35 + 0.65 * (1.0 - (1.0 - u) ** 2)
            size = s0 * (1.0 + 1.4 * grow) * scale
            off = tuple(o[j] * dist * grow * scale for j in range(3))
            bright = 1.0 - smooth(0.72, 1.0, u)          # premultiplied fade: RGB and alpha together
            out.append(("fire", off, size, (k0, 1.0 - frac, rot, mir, bright)))
            if k0 < 3:
                out.append(("fire", off, size, (k0 + 1, frac, rot, mir, bright)))
    if t < 0.16:
        k = 1.0 - t / 0.16
        out.append(("flash", (0.0, 0.0, -1.0), (16.0 + 26.0 * k) * scale, (k,)))
    return out


def draw_explosion(fb, cam, texs, centre, t, seed, scale=1.0):
    draws = explosion_draws(t, seed, scale)
    # back to front: smoke (by camera z), then fire, then flash
    order = sorted([d for d in draws if d[0] == "smoke"], key=lambda d: -(centre[2] + d[1][2]))
    order += [d for d in draws if d[0] == "fire"] + [d for d in draws if d[0] == "flash"]
    for kind, off, size_m, args in order:
        p = M.vadd(centre, off)
        sx, sy, z = cam.proj(p)
        size = cam.px(size_m, z)
        if kind == "smoke":
            var, col, alpha, rot, mir = args
            c = lit_smoke(col)
            c = fog_col(c, z)
            sprite(fb, texs["smoke"], CELL[var], (sx, sy), size, "blend", c + (int(255 * alpha),), rot, mir)
        elif kind == "fire":
            fr, w, rot, mir, bright = args
            k = int(255 * w * bright * fade_add(z))
            if k > 0:
                sprite(fb, texs["fire"], CELL[fr], (sx, sy), size, "premul", (255, 255, 255, k), rot, mir)
        else:
            k = int(255 * args[0] * 0.9 * fade_add(z))
            sprite(fb, texs["flare"], (0, 0, 64, 64), (sx, sy), size, "add", (k, k, k, 255))
    # distance dot for small fires (T17 rule: fire smaller than ~5 px -> dot)
    if t < 1.3:
        sx, sy, z = cam.proj(centre)
        if cam.px(14.0 * scale, z) < 5.0:
            k = int(220 * (1.0 - t / 1.3))
            sprite(fb, texs["dot"], (0, 0, 16, 16), (sx, sy), 4.0, "add", (k, int(k * 0.7), int(k * 0.35), 255))


FOG_START, FOG_END = 7600.0, 34200.0         # Low preset at sea level (T10b)


def fog_col(c, z):
    f = clamp((z - FOG_START) / (FOG_END - FOG_START))
    return tuple(int(c[i] + (FOG[i] - c[i]) * f) for i in range(3))


def fade_add(z):
    """Manual distance fade for additive / premultiplied fx (fog off)."""
    return 1.0 - smooth(FOG_START, FOG_END, z)


def debris_draws(fb, cam, texs, centre, t, seed, n=8):
    r = Rng(SEED, "debris_fly", seed)
    for i in range(n):
        v = (r.uniform(-1, 1), r.uniform(-0.2, 1), r.uniform(-1, 1))
        v = M.vscale(M.vnorm(v), r.uniform(25.0, 55.0))
        size = r.uniform(0.6, 1.6)
        cell = r.int(0, 15)
        spin = r.uniform(-720, 720)
        p = (centre[0] + v[0] * t, centre[1] + v[1] * t - 4.9 * t * t, centre[2] + v[2] * t)
        sx, sy, z = cam.proj(p)
        s = cam.px(size, z)
        if s >= 1.0:
            sprite(fb, texs["debris"], ((cell % 4) * 16, (cell // 4) * 16, 16, 16), (sx, sy), s, "test",
                   (150, 150, 152, 255), spin * t)


# ---------------------------------------------------------------------------------------------------- previews

def load_texs(kit):
    return {"fire": tex_rgba(kit, "fx_fire"), "smoke": tex_rgba(kit, "fx_smoke"), "trail": tex_rgba(kit, "trail"),
            "flare": tex_rgba(kit, "flare"), "dot": tex_rgba(kit, "dot"), "debris": tex_rgba(kit, "debris"),
            "splash": tex_rgba(kit, "splash")}


def backdrop(kit, name, size):
    """In-game frame (T10b screenshot, read-only) if present, else a sky/sea gradient."""
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    p = os.path.join(root, "shots", "t10b", name)
    if os.path.exists(p):
        return Image.open(p).convert("RGB").resize(size, Image.BILINEAR), True
    W, H = size
    img = V.sky_image(W, H, (64, 110, 190), FOG)
    sea = I.gradient(W, H // 2, (150, 170, 190), (22, 60, 98))
    img.paste(sea, (0, H // 2))
    return img, False


def rgba_from_premul(rgb_img, a_img):
    return Image.merge("RGBA", tuple(rgb_img.split()) + (a_img,))


def preview_textures(kit, texs):
    """Each texture over sky, cloud white, sea and terrain with its intended blend, 16-bit quantised."""
    bgs = [("sky", (92, 140, 206)), ("cloud", (236, 238, 242)), ("sea", (30, 72, 110)), ("terrain", (98, 128, 70))]
    items = [
        ("fx_fire 0 flash (premul)", "fire", CELL[0], "premul"), ("fx_fire 1 fireball", "fire", CELL[1], "premul"),
        ("fx_fire 2 breakup", "fire", CELL[2], "premul"), ("fx_fire 3 dying", "fire", CELL[3], "premul"),
        ("fx_smoke 0 black (blend)", "smoke", CELL[0], "blend"), ("fx_smoke 1 grey", "smoke", CELL[1], "blend"),
        ("fx_smoke 2 white", "smoke", CELL[2], "blend"), ("fx_smoke 3 grey", "smoke", CELL[3], "blend"),
        ("trail (blend)", "trail", (0, 0, 64, 256), "blend"), ("splash (blend)", "splash", (0, 0, 128, 128), "blend"),
        ("flare (add)", "flare", (0, 0, 64, 64), "add"), ("debris (alpha test)", "debris", (0, 0, 64, 64), "test"),
    ]
    smoke_cols = {4: lit_smoke((60, 55, 50)), 5: lit_smoke((128, 128, 128)), 6: lit_smoke((255, 255, 255)),
                  7: lit_smoke((150, 150, 150))}
    cw, ch = 128, 128
    sheet = Image.new("RGB", (len(bgs) * cw + 150, len(items) * ch + 20), (60, 60, 66))
    for j, (bn, bc) in enumerate(bgs):
        V.label(sheet, bn, (150 + j * cw + 4, 3), (240, 240, 240))
    for i, (label, tn, cell, mode) in enumerate(items):
        V.label(sheet, label, (4, 20 + i * ch + 4), (240, 240, 240))
        for j, (bn, bc) in enumerate(bgs):
            tile = Image.new("RGB", (cw, ch), bc)
            if bn == "cloud":                                   # cloud-white with a soft grey-blue shadow
                tile = I.gradient(cw, ch, (244, 246, 250), (196, 204, 220))
            col = (255, 255, 255, 255)
            if tn == "smoke":
                col = smoke_cols[i] + (255,)
            elif tn in ("trail", "splash"):
                col = lit_smoke((255, 255, 255)) + (255,)
            elif tn == "debris":
                col = (170, 170, 172, 255)
            t = texs[tn]
            if tn == "trail":
                c = t.crop((0, 0, 64, 128)).resize((64, 128), Image.BILINEAR)
                blend_into(tile, c, (32, 0), mode, col)
            elif tn == "debris":
                c = t.resize((128, 128), Image.NEAREST)
                blend_into(tile, c, (0, 0), mode, col)
            else:
                sprite(tile, t, cell, (64, 64), 124 if tn != "flare" else 96, mode, col)
            tile = I.to16(tile, "565", dither=True)
            sheet.paste(tile, (150 + j * cw, 20 + i * ch))
    return sheet


def preview_blend_compare(kit, texs):
    """fx_fire frame 1 with RB_PREMUL (recommended) vs RB_ADD over the four backgrounds."""
    bgs = [("sky", (92, 140, 206)), ("cloud", (236, 238, 242)), ("sea", (30, 72, 110)), ("terrain", (98, 128, 70))]
    cw = 160
    sheet = Image.new("RGB", (len(bgs) * cw + 120, 2 * cw + 20), (60, 60, 66))
    for j, (bn, bc) in enumerate(bgs):
        V.label(sheet, bn, (120 + j * cw + 4, 3), (240, 240, 240))
        for i, mode in enumerate(("premul", "add")):
            tile = Image.new("RGB", (cw, cw), bc) if bn != "cloud" else I.gradient(cw, cw, (244, 246, 250),
                                                                                   (196, 204, 220))
            sprite(tile, texs["fire"], CELL[1], (cw // 2, cw // 2), 150, mode)
            sprite(tile, texs["fire"], CELL[2], (cw // 2 + 30, cw // 2 + 34), 70, mode)
            sheet.paste(I.to16(tile, "565", dither=True), (120 + j * cw, 20 + i * cw))
    V.label(sheet, "RB_PREMUL", (4, 20 + cw // 2), (240, 240, 240))
    V.label(sheet, "RB_ADD", (4, 20 + cw + cw // 2), (240, 240, 240))
    return sheet


def preview_explosion_seq(kit, texs):
    times = [0.04, 0.12, 0.25, 0.45, 0.7, 1.0, 1.4, 2.0, 3.0, 4.5]
    bg, real = backdrop(kit, "new_lvl0.png", (800, 600))
    tw, th = 200, 150
    cam = Cam2(tw, th, 60.0, cy=th * 0.62)
    sheet = Image.new("RGB", (5 * tw, 4 * th + 20), (60, 60, 66))
    V.label(sheet, "explosion timeline (t s after detonation), 90 m away, size x%.1f; rows 1-2 over sky, rows 3-4 "
            "over sea (565 dithered)" % EXPL_SCALE, (4, 3), (240, 240, 240))
    for k, (rowbg, centre) in enumerate((((400, 150), (0.0, 2.0, 90.0)), ((400, 450), (0.0, -6.0, 90.0)))):
        crop = bg.crop((rowbg[0] - tw, rowbg[1] - th, rowbg[0] + tw, rowbg[1] + th)).resize((tw, th), Image.BOX)
        for i, t in enumerate(times):
            tile = crop.copy()
            draw_explosion(tile, cam, texs, centre, t, 3, EXPL_SCALE)
            debris_draws(tile, cam, texs, centre, t, 3, 10)
            V.label(tile, "%.2f s" % t, (4, 3), (255, 255, 255))
            col = i % 5
            row = k * 2 + i // 5
            sheet.paste(I.to16(tile, "565", dither=True), (col * tw, 20 + row * th))
    return sheet


def trail_points(start, end, ctrl, n, t_head, life, w0=0.8, w1=3.5, a0=0.85, tau=1.2):
    """Points of a trail along a quadratic curve; the head (end) is youngest. Returns pts, widths, alphas."""
    pts, ws, al = [], [], []
    for i in range(n):
        u = i / float(n - 1)
        p = tuple((1 - u) ** 2 * start[j] + 2 * (1 - u) * u * ctrl[j] + u * u * end[j] for j in range(3))
        age = (1.0 - u) * t_head
        pts.append(p)
        ws.append(w0 + (w1 - w0) * (1.0 - math.exp(-age / tau)))
        al.append(a0 * max(0.0, 1.0 - age / life) ** 1.5 * smooth(0.0, 0.08, age + 0.02))
    return pts, ws, al


def flares_draw(fb, cam, texs, origin, t, seed, n=4):
    r = Rng(SEED, "flares", seed)
    for i in range(n):
        d = i * 0.25
        a = t - d
        if a < 0 or a > 3.0:
            continue
        v = ((1.0 if i % 2 else -1.0) * r.uniform(10, 20), r.uniform(-12, -3), r.uniform(-12, -6))
        pts = []
        for k in range(9):
            tt = max(0.0, a - k * 0.15)
            pts.append((origin[0] + v[0] * tt, origin[1] + v[1] * tt - 1.2 * tt * tt, origin[2] + v[2] * tt))
        ws = [0.6 + 2.2 * (k / 8.0) for k in range(9)]
        al = [0.80 * (1.0 - k / 8.0) * smooth(0.0, 0.1, a) for k in range(9)]
        if a > 0.15:
            ribbon(fb, cam, texs["trail"], pts, ws, al, lit_smoke((235, 235, 232)), 8.0)
        sx, sy, z = cam.proj(pts[0])
        s = max(3.0, cam.px(4.5, z))
        flick = 0.85 + 0.15 * math.sin(i * 7.3 + a * 40.0)
        k = int(255 * flick * (1.0 - smooth(2.6, 3.0, a)))
        sprite(fb, texs["flare"], (0, 0, 64, 64), (sx, sy), s, "add", (k, k, k, 255))


def motor_glow(fb, cam, texs, p, size_m=1.4):
    sx, sy, z = cam.proj(p)
    s = cam.px(size_m, z)
    if s >= 3.0:
        sprite(fb, texs["flare"], (0, 0, 64, 64), (sx, sy), s, "add", (255, 220, 170, 255))
    else:
        sprite(fb, texs["dot"], (0, 0, 16, 16), (sx, sy), 3.0, "add", (255, 230, 190, 255))


def splash_draw(fb, cam, texs, base, t, seed):
    r = Rng(SEED, "splash_i", seed)
    for k in range(3):
        off = (r.uniform(-4, 4), 0.0, r.uniform(-3, 3))
        h = r.uniform(26.0, 38.0) * (1.0 if k == 0 else 0.7)
        rise = 1.0 - (1.0 - min(1.0, t / 0.6)) ** 2
        alpha = 0.95 * (1.0 - smooth(1.0, 3.5, t))
        p = M.vadd(base, off)
        p = (p[0], p[1] - 0.06 * h, p[2])
        sx, sy, z = cam.proj(p)
        w = cam.px(h * 0.9, z)
        if w < 1:
            continue
        c = lit_smoke((255, 255, 255))
        sprite(fb, texs["splash"], (0, 0, 128, 128), (sx, sy), w, "blend", c + (int(255 * alpha),),
               mirror=(k % 2 == 1), aspect=(rise * 1.15), anchor="bottom")
    # vapour puff
    sx, sy, z = cam.proj(M.vadd(base, (0, 6.0 + 2.0 * t, 0)))
    s = cam.px(14.0 + 8.0 * t, z)
    sprite(fb, texs["smoke"], CELL[2], (sx, sy), s, "blend", lit_smoke((240, 240, 240)) + (int(150 * (1 - smooth(0.5, 4, t))),))


EXPL_SCALE = 1.5                # recommended arcade size factor (core fireball ~30 m at its peak)


def preview_mock_sea(kit, texs):
    W, H = 800, 600
    fb, real = backdrop(kit, "new_lvl0.png", (W, H))
    cam = Cam2(W, H, 70.0, cy=308.0)
    # old smoke from an earlier kill, high left
    draw_explosion(fb, cam, texs, (-300.0, 110.0, 950.0), 5.0, 11, EXPL_SCALE)
    # LRM: launched near the camera 3.5 s ago, curving up right to the target; hit 0.35 s ago
    tgt = (110.0, 55.0, 600.0)
    pts, ws, al = trail_points((5.0, -4.0, 22.0), tgt, (-40.0, 30.0, 260.0), 16, 3.5, 7.0)
    ribbon(fb, cam, texs["trail"], pts, ws, al, lit_smoke((240, 240, 236)), 12.0)
    draw_explosion(fb, cam, texs, tgt, 0.35, 1, EXPL_SCALE)
    debris_draws(fb, cam, texs, tgt, 0.35, 1, 10)
    # SRM: still flying left to right, motor burning
    pts2, ws2, al2 = trail_points((-90.0, -12.0, 120.0), (-25.0, 22.0, 340.0), (-80.0, 20.0, 260.0), 16, 1.8, 5.0,
                                  0.6, 2.6)
    ribbon(fb, cam, texs["trail"], pts2, ws2, al2, lit_smoke((232, 232, 230)), 12.0)
    motor_glow(fb, cam, texs, pts2[-1])
    # enemy dispensing flares (right)
    flares_draw(fb, cam, texs, (75.0, 12.0, 170.0), 1.2, 2)
    # splash on the sea
    splash_draw(fb, cam, texs, (-120.0, -60.0, 430.0), 1.0, 5)
    # near explosion low over the sea (0.6 s) with debris
    draw_explosion(fb, cam, texs, (45.0, -26.0, 240.0), 0.6, 7, EXPL_SCALE)
    debris_draws(fb, cam, texs, (45.0, -26.0, 240.0), 0.6, 7, 12)
    fb = I.to16(fb, "565", dither=True)
    V.label(fb, "A8 mock over sky + sea (%s): LRM trail -> hit 0.35 s at 600 m, SRM in flight, flares, splash "
            "430 m, explosion 0.6 s at 240 m, old smoke" % ("game frame" if real else "gradient"), (4, 586),
            (255, 255, 255))
    return fb


def preview_mock_land(kit, texs):
    W, H = 800, 600
    fb, real = backdrop(kit, "new_o03.png", (W, H))
    cam = Cam2(W, H, 70.0, cy=140.0)              # this frame looks down from altitude: horizon row ~140
    # a trail across the cloud band (high, far)
    pts2, ws2, al2 = trail_points((-420.0, 150.0, 800.0), (300.0, 190.0, 1000.0), (-40.0, 260.0, 850.0), 16, 6.0,
                                  8.0)
    ribbon(fb, cam, texs["trail"], pts2, ws2, al2, lit_smoke((240, 240, 236)), 12.0)
    # burning wreck smoke over the terrain, later stage
    draw_explosion(fb, cam, texs, (-95.0, -36.0, 330.0), 1.6, 22, EXPL_SCALE)
    # trail from the right ending in an explosion over the island
    tgt = (28.0, -30.0, 260.0)
    pts, ws, al = trail_points((150.0, 6.0, 60.0), tgt, (160.0, 10.0, 220.0), 16, 3.0, 7.0)
    ribbon(fb, cam, texs["trail"], pts, ws, al, lit_smoke((240, 240, 236)), 12.0)
    draw_explosion(fb, cam, texs, tgt, 0.5, 21, EXPL_SCALE)
    debris_draws(fb, cam, texs, tgt, 0.5, 21, 10)
    # damaged jet smoke puffs (dark grey) trailing to the left over the coast
    for i in range(12):
        a = i * 0.12
        p = (-40.0 - i * 6.0, -10.0 + i * 0.4, 110.0 + i * 3.0)
        sx, sy, z = cam.proj(p)
        s_ = cam.px(4.0 + 6.0 * a, z)
        sprite(fb, texs["smoke"], CELL[i % 4], (sx, sy), s_, "blend",
               lit_smoke((80, 78, 76)) + (int(220 * (1.0 - a / 1.6)),), rot=i * 37 % 50 - 25)
    flares_draw(fb, cam, texs, (-30.0, 10.0, 90.0), 1.4, 4, 6)
    fb = I.to16(fb, "565", dither=True)
    V.label(fb, "A8 mock over island, cloud and sea (%s): trail across clouds, explosion 0.5 s over terrain, "
            "wreck 1.6 s, damage smoke, flares" % ("game frame" if real else "gradient"), (4, 586), (255, 255, 255))
    return fb


def jet_items(kit):
    """A1's jet LOD0 + its surface parts at their hinge tags (no deflection)."""
    p = kit.find("meshes/jet.msh")
    if not p:
        return None, {}
    jet = E.read_msh(p)
    tags = dict((t[0], t[1]) for t in jet.tags)
    items = [jet]
    for part, tag in (("jet_stab_l", "hinge_stab_l"), ("jet_stab_r", "hinge_stab_r"), ("jet_flap_l", "hinge_flap_l"),
                      ("jet_flap_r", "hinge_flap_r"), ("jet_nozzle", "hinge_nozzle")):
        pp = kit.find("meshes/%s.msh" % part)
        if pp and tag in tags:
            items.append((E.read_msh(pp), tags[tag]))
    return items, tags


def preview_rails(kit, lrm, srm):
    items, tags = jet_items(kit)
    if not items:
        return None
    ml, ms = lrm.to_msh(), srm.to_msh()
    loaded = list(items)
    for i in range(8):
        tag = "rail_%d" % i
        if tag in tags:
            loaded.append((ms if i < 2 else ml, tags[tag]))
    td = kit.tex_dirs
    W, H = 512, 384
    cams = [
        ("left wingtip SRM, close: tail fins under the wing tip", V.Camera((-5.3, -0.75, -4.2), (-4.5, -0.1, -1.5),
                                                                           fov=24.0)),
        ("left wing from behind (ortho, close)", V.Camera((-3.3, -0.25, -30.0), (-3.3, -0.25, 0.0), ortho=0.9)),
        ("right side (ortho)", V.Camera((30.0, -0.3, -0.6), (0.0, -0.3, -0.6), ortho=2.4)),
        ("below (ortho)", V.Camera((0.0, -30.0, -0.5), (0.0, 0.0, -0.5), up=(0, 0, 1), ortho=5.4)),
        ("front 3/4 below", V.Camera((7.5, -3.2, 9.5), (0.5, -0.4, -0.5), fov=40.0)),
        ("chase view (24 m behind, 6 up)", V.Camera((0.0, 6.0, -24.0), (0.0, 0.0, 40.0), fov=55.0)),
    ]
    sheet = Image.new("RGB", (W * 2, H * 3), (90, 90, 96))
    for i, (name, cam) in enumerate(cams):
        img = V.render(loaded, cam, (W, H), bg=V.sky_image(W, H), tex_dirs=td, light=M.vnorm((0.4, 0.8, 0.3)))
        V.label(img, name, color=(250, 250, 250))
        sheet.paste(img, ((i % 2) * W, (i // 2) * H))
    V.label(sheet, "missiles on A1's rail tags: rail_0/1 SRM (wingtips), rail_2..7 LRM (pylons); origin = rail point",
            (6, H * 3 - 18), (250, 250, 250))
    return sheet


def make_previews(kit, lrm, srm):
    td = kit.tex_dirs
    V.clear_cache()
    for mb in (lrm, srm):
        kit.save_preview("fx_%s_views.png" % mb.name, V.views(mb, 384, tex_dirs=td, facing="+z",
                                                              title="%s (%d / %d tris)" % ((mb.name,) +
                                                                                           tuple(mb.tri_counts()))))
    both = Image.new("RGB", (640, 640), (90, 90, 96))
    both.paste(V.lods(lrm.to_msh(), 320, tex_dirs=td, facing="+z", title="lrm LODs"), (0, 0))
    both.paste(V.lods(srm.to_msh(), 320, tex_dirs=td, facing="+z", title="srm LODs"), (0, 320))
    kit.save_preview("fx_missile_lods.png", both)
    rails = preview_rails(kit, lrm, srm)
    if rails is not None:
        kit.save_preview("fx_rails.png", rails)
    kit.save_preview("fx_tex.png", V.contact_sheet(
        [kit.out("tex/msl.bmp"), kit.out("tex/fx_fire.bmp"), kit.out("tex/fx_fire_a.bmp"), kit.out("tex/fx_smoke.bmp"),
         kit.out("tex/fx_smoke_a.bmp"), kit.out("tex/trail.bmp"), kit.out("tex/trail_a.bmp"),
         kit.out("tex/splash.bmp"), kit.out("tex/splash_a.bmp"), kit.out("tex/flare.bmp"), kit.out("tex/debris.bmp"),
         kit.out("tex/dot.bmp")], cols=4, cell=192,
        title="msl, fx_fire (premul) + _a, fx_smoke + _a, trail + _a, splash + _a, flare, debris (keyed), dot"))
    texs = load_texs(kit)
    kit.save_preview("fx_over_bg.png", preview_textures(kit, texs))
    kit.save_preview("fx_blend_compare.png", preview_blend_compare(kit, texs))
    kit.save_preview("fx_explosion_seq.png", preview_explosion_seq(kit, texs))
    kit.save_preview("fx_mock_sea.png", preview_mock_sea(kit, texs))
    kit.save_preview("fx_mock_land.png", preview_mock_land(kit, texs))


# ---------------------------------------------------------------------------------------------------- build

def build(out_dir, kit):
    lrm = build_missile(LRM)
    srm = build_missile(SRM)
    kit.save_msh("meshes/lrm.msh", lrm)
    kit.save_msh("meshes/srm.msh", srm)
    kit.save_bmp("tex/msl.bmp", msl_texture())
    kit.save_bmp("tex/trail.bmp", fill_clear(trail_texture()), alpha=True, alpha_bleed=0)
    kit.save_bmp("tex/fx_fire.bmp", fire_atlas(), alpha=True, alpha_bleed=0)
    kit.save_bmp("tex/fx_smoke.bmp", fill_clear(smoke_atlas()), alpha=True, alpha_bleed=0)
    kit.save_bmp("tex/flare.bmp", flare_texture())
    kit.save_bmp("tex/debris.bmp", debris_texture(), keyed=True)
    kit.save_bmp("tex/splash.bmp", fill_clear(splash_texture()), alpha=True, alpha_bleed=0)
    kit.save_bmp("tex/dot.bmp", dot_texture())
    kit.log("tris lrm %s srm %s" % (lrm.tri_counts(), srm.tri_counts()))
    if kit.previews:
        make_previews(kit, lrm, srm)


if __name__ == "__main__":
    from assetkit import build as B
    sys.exit(B.main_for(__file__))
