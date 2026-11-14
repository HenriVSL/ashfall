"""gen_jet.py - the player/enemy fighter (A1): body mesh with 3 LODs, moving parts, flame, liveries, fx textures.

Original single-engine delta-ish fighter in the F-16 spirit (DESIGN 9.5, TASKS A1), not a replica: own
proportions, own markings. Chin intake under the cockpit, bubble canopy that fairs into a high dorsal spine,
blended leading-edge root extensions (LERX) running into a cropped-delta wing (43 deg leading edge, straight
trailing edge), one fin with a dorsal fillet kink, all-moving stabilators, flaperons on the inner trailing edge,
two small canted ventral fins, wingtip launch rails and three pylons per wing, a convergent petal nozzle.

Body frame (DESIGN 3): metres, +Z forward (nose), +X right wing, +Y up, origin = CG. Length 15.27 m (nose tip
+8.12 to nozzle exit -7.15), span 9.33 m over the tip rails, height 5.1 m with the gear down (fin top +3.05,
wheels on the ground at y = -2.047 = GROUND_Y: the CG is 2.05 m above the runway).

Outputs (paths relative to data/):
    meshes/jet.msh          body, LOD0 < 300 m, LOD1 < 900 m, LOD2 beyond (the engine stops at acMeshDist).
                            LOD0 has NO stabilators, flaperons, nozzle or gear: those are parts drawn at the
                            hinge tags. LOD1/LOD2 are self-contained (fixed stabilators, nozzle, no flaperon
                            notch, no gear). Materials: "skin" (tex jet_grey: the game swaps in jet_aggr for
                            enemies) and "canopy" (tex jet_grey, MM_GLASS, dark tinted region of the atlas).
                            Vertex colours white (CPU-lit at run time, mesh_draw_lit).
    meshes/jet_stab_l.msh   stabilators: origin on the hinge axis, hinge axis = local X; texture jet_grey
    meshes/jet_stab_r.msh
    meshes/jet_flap_l.msh   flaperons: origin on the hinge line (= the wing trailing edge), axis local X;
    meshes/jet_flap_r.msh   texture jet_grey
    meshes/jet_nozzle.msh   12 petals (serrated exit), origin = centre of the hinge ring (tag hinge_nozzle),
                            petals run toward -Z; opening = scale x/y about the origin by 1 + 0.18 * open
                            (no single hinge axis); texture jet_parts
    meshes/jet_gear.msh     one gear leg (strut + oleo + wheel), origin = hinge at the top, hinge axis X,
                            drawn at gear_n, gear_l, gear_r; texture jet_parts
    meshes/jet_flame.msh    afterburner: 2 nested 8-sided cones (16 tris), origin = nozzle exit, extends
                            toward -Z (outer 4.5 m, core 1.7 m at full length); RB_ADD, cull none, tex flame
    tex/jet_grey.bmp        player livery 256x256: two-tone light/medium grey, low-vis markings
    tex/jet_aggr.bmp        enemy livery 256x256: dark two-tone aggressor camouflage, red accents
    tex/jet_parts.bmp       128x128: nozzle petals (outer/inner), gear strut/oleo/tyre/hub
    tex/flame.bmp + _a      128x128 additive flame (premultiplied: RGB fades to black), u around (tiles), v
                            along the flame (v = 0 at the nozzle)
    tex/glow_nozzle.bmp     128x128 additive radial glow (black = nothing), billboard at the nozzle tag

Tags (jet.msh): cam_cockpit cam_chase nozzle hinge_stab_l hinge_stab_r hinge_flap_l hinge_flap_r rail_0..rail_7
flare_l flare_r (DESIGN 8.8) + hinge_nozzle gear_n gear_l gear_r. Hinge tag direction = the hinge axis (+X).
Part angle convention (assetkit rotate_x = D3DXMatrixRotationX): a POSITIVE angle about +X moves the trailing
edge UP (a point at -Z goes to +Y), for both sides.

Liveries are painted by baking LOD0 + the parts (neutral) into the 256x256 UV atlas at 2x (assetkit.bake) and
evaluating features (panel lines, markings, camouflage, canopy glass and frame, intake, exhaust metal) at each
texel's 3D point. Same inputs give the same bytes.

    python3 tools/build_assets.py --only jet [--check]       # or: python3 tools/art/gen_jet.py
"""

import bisect
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "tools"))

from PIL import Image  # noqa: E402

from assetkit import bake as BK  # noqa: E402
from assetkit import mesh as M  # noqa: E402
from assetkit import palette as P  # noqa: E402
from assetkit import preview as V  # noqa: E402
from assetkit.rng import fbm, value_noise_1d  # noqa: E402

SEED = 1601
ORDER = 20

PART_CLASS = "jetpart"
FLAME_CLASS = "jetpart"     # 16 tris; origin at the nozzle exit (fx is a texture class)
ASSETS = [
    {"file": "meshes/jet.msh", "class": "jet"},
    {"file": "meshes/jet_stab_l.msh", "class": PART_CLASS},
    {"file": "meshes/jet_stab_r.msh", "class": PART_CLASS},
    {"file": "meshes/jet_flap_l.msh", "class": PART_CLASS},
    {"file": "meshes/jet_flap_r.msh", "class": PART_CLASS},
    {"file": "meshes/jet_nozzle.msh", "class": PART_CLASS},
    {"file": "meshes/jet_gear.msh", "class": PART_CLASS},
    {"file": "meshes/jet_flame.msh", "class": FLAME_CLASS},
    "tex/jet_grey.bmp", "tex/jet_aggr.bmp", "tex/jet_parts.bmp",
    "tex/flame.bmp", "tex/flame_a.bmp", "tex/glow_nozzle.bmp",
]

LOD_DIST = (300.0, 900.0)       # DESIGN 9.1
TEXN = 256                      # livery atlas size
PTEX = 128                      # parts atlas size

# ---------------------------------------------------------------------------------------------------- key points

HINGE_STAB = (0.62, -0.08, -5.75)       # right side; left = mirrored x
FLAP_X = (0.92, 3.28)                   # flaperon span (|x|)
FLAP_HINGE_Z = -1.42                    # = inner wing trailing edge
FLAP_TE_Z = -1.95
FLAP_YM = -0.02
HINGE_FLAP = (0.5 * (FLAP_X[0] + FLAP_X[1]), FLAP_YM, FLAP_HINGE_Z)
HINGE_NOZ = (0.0, 0.05, -6.20)
NOZ_LEN = 0.95
NOZZLE_EXIT = (0.0, 0.05, HINGE_NOZ[2] - NOZ_LEN)
GEAR_N = (0.0, -1.25, 2.55)
GEAR_M = (0.62, -1.25, -0.85)           # right main; left = mirrored
GEAR_LEN = 0.52                         # hinge -> axle
WHEEL_R = 0.30
GROUND_Y = GEAR_N[1] - GEAR_LEN - WHEEL_R * math.cos(math.pi / 8.0)   # -2.047: flat-bottomed 8-sided wheels
CAM_COCKPIT = (0.0, 0.98, 4.35)
CAM_CHASE = (0.0, 0.55, -0.60)          # chase-camera pivot: centre of the visual bounding box
FLARE = (0.50, -0.72, -4.90)            # right; dir down/out

# ---------------------------------------------------------------------------------------------------- curves

def pchip(keys):
    """Monotone cubic (Fritsch-Carlson) through (z, value) keys -> f(z); clamps outside. (gen_car pattern)"""
    ks = sorted(keys)
    zs = [k[0] for k in ks]
    vs = [k[1] for k in ks]
    n = len(zs)
    if n == 1:
        return lambda z: vs[0]
    h = [zs[i + 1] - zs[i] for i in range(n - 1)]
    d = [(vs[i + 1] - vs[i]) / h[i] for i in range(n - 1)]
    m = [0.0] * n
    m[0], m[-1] = d[0], d[-1]
    for i in range(1, n - 1):
        if d[i - 1] * d[i] <= 0.0:
            m[i] = 0.0
        else:
            w1 = 2.0 * h[i] + h[i - 1]
            w2 = h[i] + 2.0 * h[i - 1]
            m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i])

    def f(z):
        if z <= zs[0]:
            return vs[0]
        if z >= zs[-1]:
            return vs[-1]
        i = min(bisect.bisect_right(zs, z) - 1, n - 2)
        t = (z - zs[i]) / h[i]
        t2, t3 = t * t, t * t * t
        return ((2 * t3 - 3 * t2 + 1) * vs[i] + (t3 - 2 * t2 + t) * h[i] * m[i] +
                (-2 * t3 + 3 * t2) * vs[i + 1] + (t3 - t2) * h[i] * m[i + 1])
    return f


def lin(v, a, b):
    return min(1.0, max(0.0, (v - a) / (b - a)))


def smooth(e0, e1, x):
    t = lin(x, e0, e1)
    return t * t * (3.0 - 2.0 * t)


def plin(pts, x):
    """Piecewise linear through sorted (x, y) points; clamps."""
    if x <= pts[0][0]:
        return pts[0][1]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if x <= x1:
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0)
    return pts[-1][1]


# ---------------------------------------------------------------------------------------------------- fuselage
# Half section = 7 points on the +X side, bottom centre -> top centre:
#   0 bottom centre, 1 lower corner (belly / intake side), 2 chine (max width, wing/LERX line), 3 upper side,
#   4 shoulder = canopy rail, 5 canopy / spine side, 6 top centre.
# Forward keys (z >= 3.55): round radome growing into the cockpit section with the bubble canopy on top.
# Aft keys (z <= 3.45): the deep belly with the chin intake; the step between the two sets at z 3.55 -> 3.45
# is the intake mouth face. Each entry: z, P0y, then (x, y) for points 1..5, P6y.
FWD_KEYS = [
    (8.00, -0.05, (0.05, -0.03), (0.075, 0.06), (0.06, 0.14), (0.04, 0.17), (0.02, 0.185), 0.19),
    (7.40, -0.18, (0.19, -0.12), (0.27, 0.06), (0.21, 0.25), (0.13, 0.32), (0.07, 0.35), 0.36),
    (6.60, -0.38, (0.34, -0.27), (0.46, 0.04), (0.37, 0.36), (0.24, 0.46), (0.12, 0.50), 0.51),
    (5.85, -0.50, (0.45, -0.38), (0.60, 0.03), (0.49, 0.42), (0.33, 0.56), (0.17, 0.615), 0.625),
    (5.20, -0.55, (0.50, -0.43), (0.66, 0.02), (0.55, 0.44), (0.38, 0.62), (0.28, 0.95), 1.05),
    (4.40, -0.57, (0.53, -0.45), (0.69, 0.01), (0.58, 0.45), (0.40, 0.64), (0.32, 1.12), 1.27),
    (3.55, -0.58, (0.55, -0.46), (0.71, 0.00), (0.59, 0.45), (0.41, 0.65), (0.32, 1.09), 1.23),
]
AFT_KEYS = [
    (3.45, -1.32, (0.56, -1.22), (0.73, 0.00), (0.60, 0.45), (0.41, 0.65), (0.32, 1.08), 1.22),
    (2.60, -1.32, (0.60, -1.24), (0.76, -0.01), (0.62, 0.46), (0.42, 0.66), (0.30, 0.96), 1.07),
    (1.50, -1.32, (0.66, -1.24), (0.80, -0.02), (0.66, 0.48), (0.44, 0.68), (0.28, 0.86), 0.93),
    (0.00, -1.30, (0.70, -1.22), (0.82, -0.02), (0.68, 0.49), (0.46, 0.70), (0.27, 0.85), 0.90),
    (-1.50, -1.26, (0.72, -1.18), (0.83, -0.02), (0.69, 0.49), (0.46, 0.70), (0.27, 0.84), 0.89),
    (-3.00, -1.12, (0.72, -1.04), (0.82, -0.01), (0.68, 0.48), (0.45, 0.68), (0.26, 0.81), 0.86),
    (-4.20, -0.92, (0.68, -0.84), (0.78, 0.00), (0.64, 0.45), (0.42, 0.64), (0.24, 0.75), 0.79),
    (-5.20, -0.72, (0.60, -0.62), (0.71, 0.02), (0.58, 0.42), (0.38, 0.58), (0.21, 0.665), 0.69),
    (-6.30, -0.57, (0.44, -0.39), (0.62, 0.05), (0.44, 0.49), (0.26, 0.61), (0.11, 0.66), 0.67),
]
NKEY = 7
NPT = 8                         # section points after section() inserts the lower side point
P_CHINE, P_RAIL = 3, 5


def _point_curves(keys):
    out = []
    for k in range(NKEY):
        if k == 0:
            xs = [(e[0], 0.0) for e in keys]
            ys = [(e[0], e[1]) for e in keys]
        elif k == NKEY - 1:
            xs = [(e[0], 0.0) for e in keys]
            ys = [(e[0], e[-1]) for e in keys]
        else:
            xs = [(e[0], e[1 + k][0]) for e in keys]
            ys = [(e[0], e[1 + k][1]) for e in keys]
        out.append((pchip(xs), pchip(ys)))
    return out


FWD_C = _point_curves(FWD_KEYS)
AFT_C = _point_curves(AFT_KEYS)
Z_STEP = (3.55, 3.45)
STEP_DZ = {0: 0.30, 1: 0.25, 2: 0.12}   # intake lower lip raked forward

ST_FWD = [8.00, 7.00, 6.40, 5.85, 5.50, 5.10, 4.60, 4.05, 3.55]
ST_AFT = [3.45, 2.95, 2.45, 1.50, 0.40, -0.80, -2.00, -3.20, -4.30, -5.30, -6.30]
STATIONS = ST_FWD + ST_AFT
LOD1_ST = [8.00, 7.00, 5.85, 5.10, 4.05, 3.55, 3.45, 2.45, 0.40, -2.00, -4.30, -6.30]
LOD1_PT = [0, 1, 3, 5, 6, 7]
LOD2_ST = [8.00, 5.85, 3.55, 3.45, 0.40, -3.20, -6.30]
LOD2_PT = [0, 3, 6, 7]
CANOPY_Z = (2.45, 5.85)


SIDE_BULGE = 0.045


def section(z):
    """8 (x, y) points of the +X half section at station z (forward or aft key set): the 7 key points plus
    point 2 = the lower side, inserted between the lower corner and the chine and pushed out a little."""
    curves = FWD_C if z >= 3.5 else AFT_C
    k = [(fx(z), fy(z)) for fx, fy in curves]
    (x1, y1), (x2, y2) = k[1], k[2]
    dx, dy = x2 - x1, y2 - y1
    L = math.hypot(dx, dy) or 1.0
    b = SIDE_BULGE * L
    side = (0.5 * (x1 + x2) + b * dy / L, 0.5 * (y1 + y2) - b * dx / L)
    return k[:2] + [side] + k[2:]


def _row_params():
    segs = [0.0] * (NPT - 1)
    for z in STATIONS:
        s = section(z)
        for k in range(NPT - 1):
            segs[k] += math.hypot(s[k + 1][0] - s[k][0], s[k + 1][1] - s[k][1])
    rows = [0.0]
    for v in segs:
        rows.append(rows[-1] + v)
    return [r / rows[-1] for r in rows]


ROWS = _row_params()

# ---------------------------------------------------------------------------------------------------- atlas layout
# texel rectangles (x, y, w, h) in the 256x256 livery atlas
LAYOUT = {
    "fus": (0, 0, 256, 64),         # fuselage loft: column = z (nose left), row = point (top centre at the top);
                                    # both halves share it (the +X half is baked)
    "wing_r": (0, 64, 128, 80),     # plan projections (column = z, nose left; row = |x|, root at the top);
    "wing_l": (128, 64, 128, 80),   # top and bottom share texels (top baked)
    "fin_r": (0, 144, 96, 56),      # side projections (column = z, row = y, top at the top)
    "fin_l": (96, 144, 96, 56),
    "stab_r": (192, 144, 64, 56),   # plan projections
    "stab_l": (192, 200, 64, 56),
    "flap_r": (0, 200, 64, 24),     # plan (column = |x|, row = z)
    "flap_l": (64, 200, 64, 24),
    "mouth": (128, 200, 64, 32),    # front projection of the intake mouth face (x, y)
}
PATCH = {"tip": (0, 224, 16, 16), "engine": (16, 224, 16, 16), "rail": (32, 224, 16, 16),
         "pylon": (48, 224, 16, 16), "ventral": (64, 224, 16, 16), "nozzle": (80, 224, 16, 16),
         "black": (96, 224, 16, 16), "lower": (112, 224, 16, 16), "upper": (0, 240, 16, 16)}
FUS_Z = (8.02, -6.32)
WING_Z = (5.68, -2.00)
WING_X = (0.55, 4.70)
FIN_Z = (-1.80, -6.15)
FIN_Y = (3.12, 0.55)
STAB_Z = (-4.40, -7.05)
STAB_X = (0.55, 3.00)
FLAPU_X = (0.88, 3.32)
FLAPV_Z = (-1.36, -1.98)
MOUTH_X = (-0.85, 0.85)
MOUTH_Y = (-0.40, -1.36)


def texel(region, p, row=None):
    """Continuous texel coordinates (256 scale) of 3D point p in a region (fus: needs the point's row param)."""
    x0, y0, w, h = LAYOUT[region]
    x, y, z = p
    if region == "fus":
        return (x0 + 2 + lin(z, FUS_Z[0], FUS_Z[1]) * (w - 4), y0 + 2 + (1.0 - row) * (h - 4))
    if region in ("wing_r", "wing_l"):
        return (x0 + 2 + lin(z, WING_Z[0], WING_Z[1]) * (w - 4), y0 + 2 + lin(abs(x), WING_X[0], WING_X[1]) * (h - 4))
    if region in ("fin_r", "fin_l"):
        return (x0 + 2 + lin(z, FIN_Z[0], FIN_Z[1]) * (w - 4), y0 + 2 + lin(y, FIN_Y[0], FIN_Y[1]) * (h - 4))
    if region in ("stab_r", "stab_l"):
        return (x0 + 2 + lin(z, STAB_Z[0], STAB_Z[1]) * (w - 4), y0 + 2 + lin(abs(x), STAB_X[0], STAB_X[1]) * (h - 4))
    if region in ("flap_r", "flap_l"):
        return (x0 + 2 + lin(abs(x), FLAPU_X[0], FLAPU_X[1]) * (w - 4),
                y0 + 2 + lin(z, FLAPV_Z[0], FLAPV_Z[1]) * (h - 4))
    if region == "mouth":
        return (x0 + 2 + lin(x, MOUTH_X[0], MOUTH_X[1]) * (w - 4), y0 + 2 + lin(y, MOUTH_Y[0], MOUTH_Y[1]) * (h - 4))
    raise ValueError(region)


def patch_uv(name, size=TEXN, table=None):
    x, y, w, h = (table or PATCH)[name]
    return ((x + w * 0.5) / size, (y + h * 0.5) / size)


# ---------------------------------------------------------------------------------------------------- geometry helpers

def orient_outward(g, centre=None):
    """Flip all triangles of a piece if most of them face its centre (pieces are built with one consistent
    winding; this picks the outward one)."""
    if g.ntris == 0:
        return g
    if centre is None:
        lo, hi = g.bounds()
        centre = tuple((lo[k] + hi[k]) * 0.5 for k in range(3))
    score = 0.0
    for t in range(g.ntris):
        a, b, c = [g.P[v] for v in g.T[t]]
        n = M.vcross(M.vsub(b, a), M.vsub(c, a))
        cen = tuple((a[k] + b[k] + c[k]) / 3.0 for k in range(3))
        score += M.vdot(n, M.vsub(cen, centre))
    if score < 0.0:
        g.T = [(a, c, b) for a, b, c in g.T]
    return g


def grid(rings, closed=True):
    """Triangles between consecutive rings (same point count), one consistent winding (callers orient).
    Vertex UV = (ring index i, point index k); g.TS = index into the returned [(i, k)] quad list."""
    g = M.Geo()
    info = []
    ids = [[g.vert(p, uv=(i, k)) for k, p in enumerate(ring)] for i, ring in enumerate(rings)]
    npt = len(rings[0])
    nseg = npt if closed else npt - 1
    for i in range(len(rings) - 1):
        for k in range(nseg):
            k2 = (k + 1) % npt
            a, b, c, d = ids[i][k], ids[i + 1][k], ids[i + 1][k2], ids[i][k2]
            for tri in ((a, b, d), (b, c, d)):
                pa, pb, pc = g.P[tri[0]], g.P[tri[1]], g.P[tri[2]]
                if M.vlen(M.vcross(M.vsub(pb, pa), M.vsub(pc, pa))) < 1e-9:
                    continue
                g.tri(tri[0], tri[1], tri[2], None, len(info))
                info.append((i, k))
    return g, info


def mirror_pt(p):
    return (-p[0], p[1], p[2])


def tri_centroid(ps):
    return tuple((ps[0][k] + ps[1][k] + ps[2][k]) / 3.0 for k in range(3))


class Piece(object):
    """Triangles with a per-triangle (region, material name, bake key name, row params) before UVs."""

    def __init__(self):
        self.tris = []      # (positions[3], region, mat, key, rows[3] or None, side)

    def add(self, ps, region, mat="skin", key=None, rows=None):
        self.tris.append((ps, region, mat, key, rows))

    def extend(self, other):
        self.tris.extend(other.tris)
        return self

    def mirrored(self, region_map):
        out = Piece()
        for ps, region, mat, key, rows in self.tris:
            q = [mirror_pt(p) for p in ps]
            q = [q[0], q[2], q[1]]
            r = None if rows is None else [rows[0], rows[2], rows[1]]
            out.add(q, region_map.get(region, region), mat, key, r)
        return out


def piece_from_geo(g, region, mat="skin", key=None):
    pc = Piece()
    for a, b, c in g.T:
        pc.add([g.P[a], g.P[b], g.P[c]], region, mat, key)
    return pc


def realise(piece, mats, crease, bake=None, patch_table=None, patch_size=TEXN):
    """Piece -> Geo with UVs (texel projection per region, or a patch), materials and smoothing-group
    normals. bake: list collecting (region, texels, positions, normals, key) for the livery bake."""
    g = M.Geo()
    tag = []
    for ps, region, mat, key, rows in piece.tris:
        if region.startswith("patch:"):
            uv = patch_uv(region[6:], patch_size, patch_table)
            uvs = [uv, uv, uv]
        else:
            tcs = [texel(region, p, None if rows is None else rows[j]) for j, p in enumerate(ps)]
            uvs = [(tc[0] / TEXN, tc[1] / TEXN) for tc in tcs]
        idx = [g.vert(p, (0.0, 1.0, 0.0), uv) for p, uv in zip(ps, uvs)]
        g.tri(idx[0], idx[1], idx[2], mats[mat], len(tag))
        tag.append((region, key))
    out = M.smooth_normals(g, crease)
    if bake is not None:
        for t in range(out.ntris):
            region, key = tag[out.TS[t]]
            if region.startswith("patch:"):
                continue
            tri = out.T[t]
            ps = [out.P[v] for v in tri]
            ns = [out.N[v] for v in tri]
            tcs = [(out.UV[v][0] * TEXN, out.UV[v][1] * TEXN) for v in tri]
            bake.append((region, tcs, ps, ns, key))
    out.TS = [0] * out.ntris
    M.prelight(out, mode="white")
    return out


# ---------------------------------------------------------------------------------------------------- fuselage pieces

def fuselage(stations, pts):
    """Both halves + nose cap + dished tail cap as a Piece (regions fus / mouth / patches)."""
    rings = []
    for z in stations:
        s = section(z)
        dz = STEP_DZ if z == Z_STEP[1] else {}
        rings.append([(s[k][0], s[k][1], z + dz.get(k, 0.0)) for k in pts])
    g, info = grid(rings, closed=False)
    half = Piece()
    score = 0.0
    for t, (a, b, c) in enumerate(g.T):
        i, k = info[g.TS[t]]
        ps = [g.P[a], g.P[b], g.P[c]]
        rr = [ROWS[pts[int(round(g.UV[v][1]))]] for v in (a, b, c)]
        z0, z1 = stations[i], stations[i + 1]
        ka, kb = pts[k], pts[k + 1]
        if z0 == Z_STEP[0] and z1 == Z_STEP[1] and kb <= P_CHINE:
            half.add(ps, "mouth", "skin", "z")
        elif ka >= P_RAIL and z0 <= CANOPY_Z[1] + 1e-6 and z1 >= CANOPY_Z[0] - 1e-6:
            half.add(ps, "fus", "canopy", "x", rr)
        else:
            half.add(ps, "fus", "skin", "x", rr)
        n = M.vcross(M.vsub(ps[1], ps[0]), M.vsub(ps[2], ps[0]))
        cen = tri_centroid(ps)
        sc = section(cen[2])
        yc = 0.5 * (sc[0][1] + sc[-1][1])
        score += n[0] * cen[0] + n[1] * (cen[1] - yc)
    if score < 0.0:
        half.tris = [([ps[0], ps[2], ps[1]], region, mat, key, None if rows is None else [rows[0], rows[2], rows[1]])
                     for ps, region, mat, key, rows in half.tris]
    full = Piece().extend(half).extend(half.mirrored({}))
    for z, d, name, bulge in ((stations[0], (0.0, 0.0, 1.0), "tip", 0.12),
                              (stations[-1], (0.0, 0.0, -1.0), "engine", -0.10)):
        s = section(z)
        ring = [(s[k][0], s[k][1], z) for k in pts] + [mirror_pt((s[k][0], s[k][1], z)) for k in reversed(pts[1:-1])]
        cg = M.cap(ring, d, center="bbox", bulge=bulge)
        full.extend(piece_from_geo(cg, "patch:" + name))
    return full


# ---------------------------------------------------------------------------------------------------- wing
WING_LE = [(0.62, 5.70), (0.98, 4.20), (1.24, 3.05), (1.46, 2.15), (4.55, -0.98)]
WING_TIP_X = 4.55
WING_TE_OUT = -1.95
WING_YM = -0.02
WING_T = [(0.62, 0.16), (0.98, 0.13), (1.46, 0.11), (2.40, 0.085), (3.30, 0.065), (4.55, 0.035)]
TE_THICK = 0.045                # blunt inner trailing edge = flaperon hinge face


def wing_le(x):
    return plin(WING_LE, x)


def wing_te(x, notch=True):
    if notch and x < 3.30 - 1e-6:
        return FLAP_HINGE_Z
    return WING_TE_OUT


def wing_ring(x, zte, tte, npts=7):
    zle = wing_le(x)
    c = zle - zte
    t = plin(WING_T, x)
    ym = WING_YM
    if npts == 4:
        return [(x, ym, zle), (x, ym + 0.5 * t, zle - 0.40 * c), (x, ym, zte), (x, ym - 0.5 * t, zle - 0.40 * c)]
    return [(x, ym, zle), (x, ym + 0.42 * t, zle - 0.18 * c), (x, ym + 0.50 * t, zle - 0.50 * c),
            (x, ym + 0.5 * tte, zte), (x, ym - 0.5 * tte, zte), (x, ym - 0.50 * t, zle - 0.50 * c),
            (x, ym - 0.42 * t, zle - 0.18 * c)]


def wing_lod0():
    """Right wing with the flaperon notch: inner panel (blunt TE) + outer panel + caps."""
    inner = [wing_ring(x, FLAP_HINGE_Z, TE_THICK) for x in (0.62, 0.98, 1.46, 2.40, 3.30)]
    outer = [wing_ring(x, WING_TE_OUT, 0.008) for x in (3.30, 3.95, WING_TIP_X)]
    g1, _ = grid(inner)
    g2, _ = grid(outer)
    g = M.merge(g1, g2)
    g = _fix_wing_orientation(g)
    g.merge(M.cap(outer[0], (-1.0, 0.0, 0.0)), M.cap(outer[-1], (1.0, 0.0, 0.0)))
    return g


def _fix_wing_orientation(g):
    """Make top-surface triangles face up (the thin wing defeats the centre heuristic)."""
    up = 0.0
    for t in range(g.ntris):
        a, b, c = [g.P[v] for v in g.T[t]]
        n = M.vcross(M.vsub(b, a), M.vsub(c, a))
        cy = (a[1] + b[1] + c[1]) / 3.0
        up += n[1] * (1.0 if cy > WING_YM else -1.0)
    if up < 0.0:
        g.T = [(a, c, b) for a, b, c in g.T]
    return g


def wing_lod1():
    rings = [wing_ring(x, WING_TE_OUT, 0.0, 4) for x in (0.62, 1.46, 3.30, WING_TIP_X)]
    g, _ = grid(rings)
    g = _fix_wing_orientation(g)
    g.merge(flat_poly(rings[-1], (1.0, 0.0, 0.0)))
    return g


def flat_poly(ring, d):
    """Convex planar polygon as a fan from its first corner (n - 2 triangles) facing direction d."""
    g = M.Geo()
    ids = [g.vert(p) for p in ring]
    for j in range(1, len(ids) - 1):
        a, b, c = ids[0], ids[j], ids[j + 1]
        n = M.vcross(M.vsub(g.P[b], g.P[a]), M.vsub(g.P[c], g.P[a]))
        if M.vdot(n, d) < 0.0:
            g.tri(a, c, b)
        else:
            g.tri(a, b, c)
    return g


def plate(outline, y, up):
    """Flat convex outline [(x, z)] at height y facing up (+1) or down (-1)."""
    return flat_poly([(x, y, z) for x, z in outline], (0.0, float(up), 0.0))


def wing_lod2():
    # fan from the (reflex) LERX kink so the concave outline triangulates correctly
    out = [(1.46, 2.15), (WING_TIP_X, -0.98), (WING_TIP_X, WING_TE_OUT), (0.62, WING_TE_OUT), (0.62, 5.70)]
    return M.merge(plate(out, WING_YM + 0.03, 1), plate(out, WING_YM - 0.03, -1))


def flaperon(lod_ring=4):
    """Right flaperon in BODY space (neutral): diamond section, nose 5 cm ahead of the hinge (hidden in the
    wing's blunt trailing edge), thickness = TE_THICK at the hinge, sharp trailing edge."""
    h = 0.5 * TE_THICK
    rings = []
    for x in FLAP_X:
        rings.append([(x, FLAP_YM, FLAP_HINGE_Z + 0.05), (x, FLAP_YM + h, FLAP_HINGE_Z - 0.02),
                      (x, FLAP_YM + 0.004, FLAP_TE_Z), (x, FLAP_YM - h, FLAP_HINGE_Z - 0.02)])
    g, _ = grid(rings)
    g = _fix_wing_orientation(g)
    g.merge(flat_poly(rings[0], (-1.0, 0.0, 0.0)), flat_poly(rings[1], (1.0, 0.0, 0.0)))
    return g


# ---------------------------------------------------------------------------------------------------- tail surfaces
STAB_ROOT_X, STAB_TIP_X = 0.58, 2.95
STAB_ANHEDRAL = 6.0


def stab_ring(x, npts=4):
    t = lin(x, STAB_ROOT_X, STAB_TIP_X)
    zle = -4.50 - 1.55 * t
    zte = -6.80 - 0.15 * t
    th = 0.10 - 0.065 * t
    ym = HINGE_STAB[1] - (x - STAB_ROOT_X) * math.tan(math.radians(STAB_ANHEDRAL))
    c = zle - zte
    return [(x, ym, zle), (x, ym + 0.5 * th, zle - 0.40 * c), (x, ym, zte), (x, ym - 0.5 * th, zle - 0.40 * c)]


def stabilator(lod=0):
    """Right stabilator in BODY space (neutral)."""
    xs = (STAB_ROOT_X, 1.75, STAB_TIP_X) if lod == 0 else (STAB_ROOT_X, STAB_TIP_X)
    rings = [stab_ring(x) for x in xs]
    if lod == 2:
        r0, r1 = rings
        out = [(r0[0][0], r0[0][2]), (r1[0][0], r1[0][2]), (r1[2][0], r1[2][2]), (r0[2][0], r0[2][2])]
        return M.merge(plate(out, r0[0][1] + 0.0, 1), plate(out, r0[0][1] - 0.02, -1))
    g, _ = grid(rings)
    g = _fix_wing_orientation_y(g, rings)
    g.merge(flat_poly(rings[-1], (1.0, 0.0, 0.0)))
    if lod == 0:
        g.merge(flat_poly(rings[0], (-1.0, 0.0, 0.0)))
    return g


def _fix_wing_orientation_y(g, rings):
    up = 0.0
    for t in range(g.ntris):
        a, b, c = [g.P[v] for v in g.T[t]]
        n = M.vcross(M.vsub(b, a), M.vsub(c, a))
        x = (a[0] + b[0] + c[0]) / 3.0
        ym = HINGE_STAB[1] - (x - STAB_ROOT_X) * math.tan(math.radians(STAB_ANHEDRAL))
        cy = (a[1] + b[1] + c[1]) / 3.0
        up += n[1] * (1.0 if cy > ym else -1.0)
    if up < 0.0:
        g.T = [(a, c, b) for a, b, c in g.T]
    return g


FIN_ST = [(0.62, -1.90, -6.05, 0.20), (1.40, -3.10, -5.95, 0.14), (3.05, -4.75, -5.80, 0.06)]


def fin_le(y):
    return plin([(s[0], s[1]) for s in FIN_ST], y)


def fin_te(y):
    return plin([(s[0], s[2]) for s in FIN_ST], y)


def fin(lod=0):
    sts = FIN_ST if lod == 0 else [FIN_ST[0], FIN_ST[-1]]
    if lod == 2:
        y0, le0, te0, _ = FIN_ST[0]
        y1, le1, te1, _ = FIN_ST[-1]
        g = M.Geo()
        for sx in (1.0, -1.0):
            ring = [(sx * 0.03, y0, le0), (sx * 0.03, y1, le1), (sx * 0.015, y1, te1), (sx * 0.03, y0, te0)]
            g.merge(flat_poly(ring, (sx, 0.0, 0.0)))
        return g
    rings = []
    for y, le, te, th in sts:
        c = le - te
        rings.append([(0.0, y, le), (0.5 * th, y, le - 0.40 * c), (0.0, y, te), (-0.5 * th, y, le - 0.40 * c)])
    g, _ = grid(rings)
    orient_side(g)
    g.merge(flat_poly(rings[-1], (0.0, 1.0, 0.0)))
    return g


def orient_side(g, x0=0.0):
    """Thin vertical surfaces: triangles on the +X side must face +X."""
    s = 0.0
    for t in range(g.ntris):
        a, b, c = [g.P[v] for v in g.T[t]]
        n = M.vcross(M.vsub(b, a), M.vsub(c, a))
        cx = (a[0] + b[0] + c[0]) / 3.0 - x0
        s += n[0] * (1.0 if cx > 0 else -1.0)
    if s < 0.0:
        g.T = [(a, c, b) for a, b, c in g.T]
    return g


def ventral():
    """Right ventral fin: canted 18 deg outward, root inside the lower rear fuselage."""
    ca, sa = math.cos(math.radians(18.0)), math.sin(math.radians(18.0))
    root = (0.42, -0.86)
    depth = 0.48
    tip = (root[0] + sa * depth, root[1] - ca * depth)
    nrm = (ca, sa)
    rings = []
    for (bx, by), le, te, th in ((root, -3.30, -4.55, 0.05), (tip, -3.95, -4.65, 0.02)):
        c = le - te
        mz = le - 0.4 * c
        rings.append([(bx, by, le), (bx + nrm[0] * th * 0.5, by + nrm[1] * th * 0.5, mz), (bx, by, te),
                      (bx - nrm[0] * th * 0.5, by - nrm[1] * th * 0.5, mz)])
    g, _ = grid(rings)
    # +nrm side must face +nrm
    s = 0.0
    for t in range(g.ntris):
        a, b, c = [g.P[v] for v in g.T[t]]
        n = M.vcross(M.vsub(b, a), M.vsub(c, a))
        cen = tri_centroid([a, b, c])
        rel = (cen[0] - root[0]) * nrm[0] + (cen[1] - root[1]) * nrm[1]
        s += (n[0] * nrm[0] + n[1] * nrm[1]) * (1.0 if rel > 0 else -1.0)
    if s < 0.0:
        g.T = [(a, c, b) for a, b, c in g.T]
    g.merge(flat_poly(rings[-1], (sa, -ca, 0.0)))
    return g


# ---------------------------------------------------------------------------------------------------- stores
PYLON_X = (3.62, 2.80, 1.95)            # outer, mid, inner
PYLON_H = 0.28
RAIL_X = 4.62


def pylon_box(x):
    zle = wing_le(x)
    zte = wing_te(x)
    zm = 0.5 * (zle + zte) + 0.10
    t = plin(WING_T, x)
    top = WING_YM - 0.30 * t
    g = M.box(0.07, PYLON_H + 0.02, 1.40, pos=(x, top - PYLON_H, zm), base=True, skip=("+y",))
    return g, (x, top - PYLON_H - 0.12, zm + 0.05)


def tip_rail():
    g = M.box(0.09, 0.13, 2.20, pos=(RAIL_X, WING_YM - 0.07, -0.80), base=True)
    return g, (RAIL_X, WING_YM - 0.07 - 0.09, -0.55)


# ---------------------------------------------------------------------------------------------------- body + LODs

def body_materials(mb):
    return {
        "skin": mb.material("skin", tex="jet_grey"),
        "canopy": mb.material("canopy", tex="jet_grey", flags=M.MM_GLASS),
    }


R2L = {"wing_r": "wing_l", "fin_r": "fin_l", "stab_r": "stab_l", "flap_r": "flap_l"}


def both(piece):
    return Piece().extend(piece).extend(piece.mirrored(R2L))


def fin_piece(g):
    pc = Piece()
    for a, b, c in g.T:
        ps = [g.P[a], g.P[b], g.P[c]]
        n = M.face_normal(*ps)
        pc.add(ps, "fin_r" if n[0] >= 0.0 else "fin_l", "skin", None)
    return pc


def body_piece(lod):
    if lod == 0:
        fus = fuselage(STATIONS, list(range(NPT)))
        wing = both(piece_from_geo(wing_lod0(), "wing_r", key="y"))
        f = fin_piece(fin(0))
        stores = Piece()
        ven = ventral()
        stores.extend(both(piece_from_geo(ven, "patch:ventral")))
        rail, _ = tip_rail()
        stores.extend(both(piece_from_geo(rail, "patch:rail")))
        for x in PYLON_X:
            pg, _ = pylon_box(x)
            stores.extend(both(piece_from_geo(pg, "patch:pylon")))
        return Piece().extend(fus).extend(wing).extend(f).extend(stores)
    if lod == 1:
        fus = fuselage(LOD1_ST, LOD1_PT)
        wing = both(piece_from_geo(wing_lod1(), "wing_r", key="y"))
        f = fin_piece(fin(1))
        stab = both(piece_from_geo(stabilator(1), "stab_r", key="y"))
        rail, _ = tip_rail()
        stores = both(piece_from_geo(rail, "patch:rail"))
        noz = piece_from_geo(nozzle_frustum(8), "patch:nozzle")
        return Piece().extend(fus).extend(wing).extend(f).extend(stab).extend(stores).extend(noz)
    fus = fuselage(LOD2_ST, LOD2_PT)
    wing = both(piece_from_geo(wing_lod2(), "wing_r", key="y"))
    f = fin_piece(fin(2))
    stab = both(piece_from_geo(stabilator(2), "stab_r", key="y"))
    noz = piece_from_geo(nozzle_frustum(6), "patch:nozzle")
    return Piece().extend(fus).extend(wing).extend(f).extend(stab).extend(noz)


def nozzle_frustum(seg):
    """Fixed nozzle for LOD1/LOD2 (outer surface only, open at the exit: the dark tail cap shows inside)."""
    r0, r1 = 0.54, 0.43
    cy = HINGE_NOZ[1]
    rings = []
    for z, r in ((HINGE_NOZ[2], r0), (NOZZLE_EXIT[2], r1)):
        rings.append([(r * math.sin(2 * math.pi * k / seg), cy + r * math.cos(2 * math.pi * k / seg), z)
                      for k in range(seg)])
    g, _ = grid(rings)
    return orient_outward(g, (0.0, cy, 0.5 * (HINGE_NOZ[2] + NOZZLE_EXIT[2])))


CREASE = 48.0


def build_body():
    """-> (MeshBuilder, LOD Geos, bake list)."""
    mb = M.MeshBuilder("jet")
    mats = body_materials(mb)
    bake = []
    geos = []
    for lod in range(3):
        g = realise(body_piece(lod), mats, CREASE, bake=bake if lod == 0 else None)
        geos.append(g)
    mb.lod(LOD_DIST[0])
    mb.add(geos[0])
    mb.lod(LOD_DIST[1])
    mb.add(geos[1])
    mb.lod()
    mb.add(geos[2])
    tags(mb)
    return mb, geos, bake


def tags(mb):
    mb.tag("cam_cockpit", CAM_COCKPIT, (0, 0, 1))
    mb.tag("cam_chase", CAM_CHASE, (0, 0, 1))
    mb.tag("nozzle", NOZZLE_EXIT, (0, 0, -1))
    mb.tag("hinge_stab_l", mirror_pt(HINGE_STAB), (1, 0, 0))
    mb.tag("hinge_stab_r", HINGE_STAB, (1, 0, 0))
    mb.tag("hinge_flap_l", mirror_pt(HINGE_FLAP), (1, 0, 0))
    mb.tag("hinge_flap_r", HINGE_FLAP, (1, 0, 0))
    _, tr = tip_rail()
    rails = [tr] + [pylon_box(x)[1] for x in PYLON_X]
    for i, p in enumerate(rails):
        mb.tag("rail_%d" % (2 * i), mirror_pt(p), (0, 0, 1))
        mb.tag("rail_%d" % (2 * i + 1), p, (0, 0, 1))
    d = M.vnorm((0.45, -0.85, -0.20))
    mb.tag("flare_l", mirror_pt(FLARE), (-d[0], d[1], d[2]))
    mb.tag("flare_r", FLARE, d)
    mb.tag("hinge_nozzle", HINGE_NOZ, (1, 0, 0))
    mb.tag("gear_n", GEAR_N, (0, 0, -1))         # dir = where the wheel goes when retracting (nose: aft)
    mb.tag("gear_l", mirror_pt(GEAR_M), (0, 0, 1))
    mb.tag("gear_r", GEAR_M, (0, 0, 1))


# ---------------------------------------------------------------------------------------------------- parts

def part_builder(name, geo_body, hinge, region, bake=None):
    """Part mesh: body-space Geo realised with livery UVs, then translated so the hinge is the origin."""
    mb = M.MeshBuilder(name)
    m = {"skin": mb.material("skin", tex="jet_grey")}
    pc = piece_from_geo(geo_body, region, key="y")
    g = realise(pc, m, CREASE, bake=bake)
    local = g.copy().translate(-hinge[0], -hinge[1], -hinge[2])
    mb.lod()
    mb.add(local)
    return mb, local


def build_surfaces(bake):
    out = {}
    st = stabilator(0)
    fl = flaperon()
    for side in ("r", "l"):
        sg, fg = st.copy(), fl.copy()
        hs, hf = HINGE_STAB, HINGE_FLAP
        rs, rf = "stab_r", "flap_r"
        if side == "l":
            sg.mirror("x")
            fg.mirror("x")
            hs, hf = mirror_pt(HINGE_STAB), mirror_pt(HINGE_FLAP)
            rs, rf = "stab_l", "flap_l"
        out["stab_" + side] = part_builder("jet_stab_" + side, sg, hs, rs, bake)
        out["flap_" + side] = part_builder("jet_flap_" + side, fg, hf, rf, bake)
    return out


# jet_parts.bmp layout (128x128)
PLAYOUT = {"noz_out": (0, 0, 128, 48), "noz_in": (0, 48, 128, 24), "wheel": (80, 80, 48, 48)}
PPATCH = {"strut": (0, 80, 16, 16), "oleo": (16, 80, 16, 16), "tyre": (32, 80, 16, 16), "hub": (48, 80, 16, 16),
          "door": (64, 80, 16, 16), "dark": (0, 96, 16, 16)}
NOZ_SEG = 12
NOZ_R = (0.54, 0.43)            # outer radius at the hinge / exit
NOZ_RI = (0.49, 0.405)          # inner


def _ptex(region, u, v):
    x0, y0, w, h = PLAYOUT[region]
    return ((x0 + 1 + u * (w - 2)) / PTEX, (y0 + 1 + v * (h - 2)) / PTEX)


def build_nozzle():
    """12-petal convergent nozzle around local +Z = forward; origin = hinge-ring centre; petals run from z = 0
    back to z = -NOZ_LEN with a serrated exit (alternate corners 6 cm shorter)."""
    mb = M.MeshBuilder("jet_nozzle")
    mat = mb.material("metal", tex="jet_parts")
    g = M.Geo()
    for surf, (r0, r1), region, outward in (("out", NOZ_R, "noz_out", 1.0), ("in", NOZ_RI, "noz_in", -1.0)):
        top, bot = [], []
        for k in range(NOZ_SEG + 1):
            a = 2.0 * math.pi * k / NOZ_SEG
            sa, ca = math.sin(a), math.cos(a)
            ze = -NOZ_LEN + (0.06 if k % 2 else 0.0)
            u = float(k) / NOZ_SEG
            top.append(g.vert((r0 * sa, r0 * ca, 0.0), (sa * outward, ca * outward, 0.0), _ptex(region, u, 0.0)))
            bot.append(g.vert((r1 * sa, r1 * ca, ze), (sa * outward, ca * outward, 0.0), _ptex(region, u, 1.0)))
        for k in range(NOZ_SEG):
            am = 2.0 * math.pi * (k + 0.5) / NOZ_SEG
            d = (math.sin(am) * outward, math.cos(am) * outward, 0.0)
            for a_, b_, c_ in ((top[k], top[k + 1], bot[k + 1]), (top[k], bot[k + 1], bot[k])):
                n = M.vcross(M.vsub(g.P[b_], g.P[a_]), M.vsub(g.P[c_], g.P[a_]))
                if M.vdot(n, d) < 0:
                    g.tri(a_, c_, b_, mat)
                else:
                    g.tri(a_, b_, c_, mat)
    g = M.smooth_normals(g, 40.0)
    M.prelight(g, mode="white")
    mb.lod()
    mb.add(g)
    return mb, g


def build_gear():
    """One leg: strut (into the belly) + chrome oleo + wheel on the axle at y = -GEAR_LEN. Axis X."""
    mb = M.MeshBuilder("jet_gear")
    mat = mb.material("metal", tex="jet_parts")
    pieces = []
    strut = M.box(0.12, 0.40, 0.12, pos=(0.0, -0.30, 0.0), base=True, skip=("+y",))
    pieces.append((strut, "strut"))
    oleo = M.box(0.08, 0.26, 0.08, pos=(0.0, -GEAR_LEN, 0.0), base=True, skip=("+y", "-y"))
    pieces.append((oleo, "oleo"))
    g = M.Geo()
    for pg, name in pieces:
        uv = patch_uv(name, PTEX, PPATCH)
        for a, b, c in pg.T:
            idx = [g.vert(pg.P[v], (0, 1, 0), uv) for v in (a, b, c)]
            g.tri(idx[0], idx[1], idx[2], mat)
    # wheel: 8 sides, both faces (hub disc)
    seg = 8
    wx = 0.10
    cy, cz = -GEAR_LEN, 0.0
    tyre = patch_uv("tyre", PTEX, PPATCH)
    wx0, wy0, ww, wh = PLAYOUT["wheel"]
    ring = [(math.cos(2 * math.pi * (k + 0.5) / seg), math.sin(2 * math.pi * (k + 0.5) / seg)) for k in range(seg)]
    for k in range(seg):
        (c0, s0), (c1, s1) = ring[k], ring[(k + 1) % seg]
        quad = [(-wx, cy + WHEEL_R * c0, cz + WHEEL_R * s0), (wx, cy + WHEEL_R * c0, cz + WHEEL_R * s0),
                (wx, cy + WHEEL_R * c1, cz + WHEEL_R * s1), (-wx, cy + WHEEL_R * c1, cz + WHEEL_R * s1)]
        mid = (0.0, (c0 + c1) * 0.5, (s0 + s1) * 0.5)
        for tri in ((0, 1, 2), (0, 2, 3)):
            idx = [g.vert(quad[j], (0, 1, 0), tyre) for j in tri]
            a, b, c = idx
            n = M.vcross(M.vsub(g.P[b], g.P[a]), M.vsub(g.P[c], g.P[a]))
            if M.vdot(n, mid) < 0:
                g.tri(a, c, b, mat)
            else:
                g.tri(a, b, c, mat)
    for sx in (1.0, -1.0):
        pts = [(sx * wx, cy + WHEEL_R * c, cz + WHEEL_R * s) for c, s in ring]
        cg = M.cap(pts, (sx, 0.0, 0.0))
        for a, b, c in cg.T:
            idx = []
            for v in (a, b, c):
                p = cg.P[v]
                u = 0.5 + 0.5 * (p[2] - cz) / WHEEL_R * 0.96
                vv = 0.5 - 0.5 * (p[1] - cy) / WHEEL_R * 0.96
                idx.append(g.vert(p, (sx, 0, 0), ((wx0 + u * ww) / PTEX, (wy0 + vv * wh) / PTEX)))
            g.tri(idx[0], idx[1], idx[2], mat)
    g = M.smooth_normals(g, 40.0)
    M.prelight(g, mode="white")
    mb.lod()
    mb.add(g)
    return mb, g


FLAME_LEN = 4.5
CORE_LEN = 1.7


def build_flame():
    mb = M.MeshBuilder("jet_flame")
    mat = mb.material("flame", tex="flame", blend=M.RB_ADD, cull_none=True)
    g = M.Geo()
    seg = 8
    for r0, length, col in ((0.40, FLAME_LEN, (180, 156, 128, 255)), (0.27, CORE_LEN, (230, 200, 160, 255))):
        base = []
        for k in range(seg + 1):
            a = 2.0 * math.pi * k / seg
            n = (math.sin(a), math.cos(a), 0.0)
            base.append(g.vert((r0 * n[0], r0 * n[1], 0.0), n, (float(k) / seg, 0.02), col))
        for k in range(seg):
            a = 2.0 * math.pi * (k + 0.5) / seg
            n = (math.sin(a), math.cos(a), 0.0)
            tip = g.vert((0.0, 0.0, -length), n, ((k + 0.5) / seg, 0.98), (0, 0, 0, 255))
            g.tri(base[k], base[k + 1], tip, mat)
    mb.lod()
    mb.add(g)
    return mb, g


# ---------------------------------------------------------------------------------------------------- texture features

PANEL_W = 0.026


def line_f(d, w=PANEL_W, depth=0.34):
    """Darkening factor of a panel line at distance d (m): soft profile (anti-aliased after the bake filter)."""
    t = max(0.0, 1.0 - d / w)
    return 1.0 - depth * t * t * (3.0 - 2.0 * t)


def seg_dist(px, py, a, b):
    vx, vy = b[0] - a[0], b[1] - a[1]
    wx, wy = px - a[0], py - a[1]
    L = vx * vx + vy * vy
    t = 0.0 if L <= 0 else max(0.0, min(1.0, (wx * vx + wy * vy) / L))
    dx, dy = wx - t * vx, wy - t * vy
    return math.sqrt(dx * dx + dy * dy)


def rect_line(u, v, u0, u1, v0, v1):
    """Distance to the outline of the axis rect (inf if far outside)."""
    if u < u0 - PANEL_W or u > u1 + PANEL_W or v < v0 - PANEL_W or v > v1 + PANEL_W:
        return 1e9
    return min(abs(u - u0), abs(u - u1), abs(v - v0), abs(v - v1)) if (u0 <= u <= u1 or v0 <= v <= v1) else 1e9


def insignia(du, dv, R, enemy):
    """Mark code of the national insignia at offset (du, dv) from its centre (both invented):
    player: light ring, dark disc, light 'island peak' triangle with a dark crater notch;
    enemy: accent (red) ring, dark disc, a red 4-point star."""
    r = math.hypot(du, dv)
    if r > R:
        return 0
    if r > 0.80 * R:
        return 3 if enemy else 2
    if not enemy:
        # upward triangle: apex (0, 0.55R), base y = -0.42R, half width 0.62R
        y = dv
        if -0.42 * R < y < 0.55 * R:
            hw = 0.62 * R * (0.55 * R - y) / (0.97 * R)
            if abs(du) < hw:
                if y > 0.30 * R and abs(du) < 0.10 * R:
                    return 1        # crater notch
                return 2
        return 1
    a = math.atan2(dv, du)
    star = 0.18 * R + 0.50 * R * abs(math.cos(2.0 * a)) ** 6
    return 3 if r < star else 1


DIGITS = {
    "0": ["111", "101", "101", "101", "111"], "1": ["010", "110", "010", "010", "111"],
    "2": ["111", "001", "111", "100", "111"], "3": ["111", "001", "011", "001", "111"],
    "4": ["101", "101", "111", "001", "001"], "5": ["111", "100", "111", "001", "111"],
    "6": ["111", "100", "111", "101", "111"], "7": ["111", "001", "010", "010", "010"],
    "8": ["111", "101", "111", "101", "111"], "9": ["111", "101", "111", "001", "111"],
}


def text_hit(s, r, text, cell):
    """Is (s, r) (reading coordinate from the text's left edge, rows from the top, metres) inside a glyph?"""
    if s < 0 or r < 0:
        return False
    col = int(s / cell)
    row = int(r / cell)
    if row >= 5:
        return False
    ch = col // 4
    if ch >= len(text) or col % 4 == 3:
        return False
    return DIGITS[text[ch]][row][col % 4] == "1"


def camo_value(a, b, seed):
    return fbm(a * 0.30 + 11.0, b * 0.30 + 5.0, octaves=3, seed=seed)


def feat(f, tone, mp=0, me=0, camo=0.5):
    return (0, f, tone, mp, me, camo)


def fixed(c):
    return (1, c[0], c[1], c[2])


GLASS_DARK = (20, 22, 28)
GLASS_HI = (64, 70, 84)
FRAME = (44, 46, 50)
BLACK = (10, 10, 12)
METAL = (128, 124, 116)
METAL_DARK = (62, 58, 54)


def fus_feature(p, n, canopy, cd):
    x, y, z = p
    ax = abs(x)
    side = abs(n[0]) > 0.55
    tone = smooth(-0.55, 0.30, n[1])
    camo = camo_value(z, y + 0.4 * ax, SEED)
    if canopy:
        if cd <= 2 or abs(z - 5.12) < 0.035:
            return fixed(FRAME)
        t = smooth(0.70, 1.20, y)
        base = P.lerp(GLASS_DARK, GLASS_HI, t * 0.8)
        s = (z * 0.80 + y * 2.3) % 1.7
        if 0.60 < s < 0.74:
            base = P.shade(base, 1.55)
        elif 0.80 < s < 0.84:
            base = P.shade(base, 1.3)
        return fixed(base)
    if z < -5.55:                           # bare metal exhaust section
        t = lin(z, -5.55, -6.30)
        c = P.lerp(METAL, METAL_DARK, t)
        if abs(z + 5.56) < 0.02:
            c = P.shade(c, 0.6)
        streak = value_noise_1d((math.atan2(y - 0.05, x) + 4.0) * 6.0, seed=SEED)
        return fixed(P.shade(c, 0.88 + 0.22 * streak))
    f = 1.0
    mp = me = 0
    if z > 5.95:                            # radome
        f *= 0.93
        if abs(z - 5.95) < 0.03:
            f *= 0.65
        if z > 7.85:
            return fixed((70, 72, 78))
        return feat(f, tone, 0, 0, camo)
    # anti-glare panel ahead of the windscreen
    if n[1] > 0.55 and 5.85 < z < 6.6 and ax < 0.28:
        return fixed((54, 57, 62))
    # rings (bulkhead lines)
    for zr in (3.40, 2.30, 1.10, -0.20, -1.60, -3.00, -4.40):
        if abs(z - zr) < PANEL_W:
            if not (zr == 2.30 and y > 0.60 and ax < 0.35):
                f *= line_f(abs(z - zr))
    if side:
        if -5.4 < z < 2.3 and abs(y - 0.28) < PANEL_W:
            f *= line_f(abs(y - 0.28))
        if -4.4 < z < 3.3 and abs(y + 0.62) < PANEL_W:
            f *= line_f(abs(y + 0.62))
        for zc0, zc1, yc0, yc1 in ((-0.55, 0.25, -0.98, -0.72), (-2.75, -2.10, 0.05, 0.22),
                                   (-4.25, -3.55, -0.50, -0.20), (1.30, 2.05, 0.06, 0.24)):
            d = rect_line(z, y, zc1 if zc1 < zc0 else zc0, max(zc0, zc1), yc0, yc1)
            if d < PANEL_W:
                f *= line_f(d)
        # formation light strips
        if (-3.95 < z < -3.30 and abs(y - 0.12) < 0.035) or (3.55 < z < 4.30 and abs(y - 0.30) < 0.035):
            return fixed((176, 196, 118))
        # flare dispenser
        if abs(z - FLARE[2]) < 0.16 and abs(y - FLARE[1]) < 0.09:
            if (int((z + 10.0) / 0.06) + int((y + 10.0) / 0.06)) % 2 == 0:
                return fixed((40, 40, 44))
            return fixed((70, 70, 74))
        # insignia on the intake side
        if 0.3 < z < 1.6 and -1.05 < y < -0.05:
            mp = insignia(z - 0.95, y + 0.55, 0.38, False)
            me = insignia(z - 0.95, y + 0.55, 0.38, True)
    elif n[1] > 0.55:                       # spine / top
        if -5.4 < z < 2.3 and abs(ax - 0.20) < PANEL_W:
            f *= line_f(abs(ax - 0.20))
        if 1.15 < z < 1.55 and ax < 0.12:   # refuelling receptacle door
            if ax > 0.09 or z < 1.18 or z > 1.52:
                return fixed((40, 42, 46))
            f *= 0.82
        if -1.1 < z < -0.9 and ax < 0.06:   # antenna base
            return fixed((52, 54, 58))
    elif n[1] < -0.55:                      # belly: gear doors
        for zc0, zc1, xc0, xc1 in ((2.15, 3.05, -1.0, 0.13), (-1.40, -0.30, 0.42, 0.76)):
            d = rect_line(z, ax, zc0, zc1, xc0, xc1)
            if d < PANEL_W:
                f *= line_f(d)
        if 2.15 < z < 3.05 and ax < PANEL_W:
            f *= line_f(ax)
    # exhaust soot ahead of the metal
    if z < -4.9:
        f *= 1.0 - 0.18 * lin(z, -4.9, -5.55)
    return feat(f, tone, mp, me, camo)


def wing_feature(p, n, side):
    x, y, z = p
    ax = abs(x)
    camo = camo_value(z, ax * 1.1 + 3.0, SEED + 1)
    f = 1.0
    mp = me = 0
    zle = wing_le(ax)
    if ax > 1.46:
        # leading-edge flap line ~0.38 m behind the leading edge (perpendicular, 43 deg sweep)
        d = (zle - z) * math.cos(math.radians(43.0))
        if abs(d - 0.38) < PANEL_W and ax < 4.35:
            f *= line_f(abs(d - 0.38))
        # spar line at 55 % of the outer chord
        zte = wing_te(ax)
        zs = zle - 0.55 * (zle - zte)
        if abs(z - zs) < PANEL_W and ax < 4.35:
            f *= line_f(abs(z - zs))
        for xc in (2.15, 3.30):
            if abs(ax - xc) < PANEL_W and z < zle - 0.40:
                f *= line_f(abs(ax - xc))
    else:
        # LERX / wing boundary
        dl = seg_dist(ax, z, (1.46, 2.15), (0.90, -1.40))
        if dl < PANEL_W:
            f *= line_f(dl)
    if ax < 3.32 and z < FLAP_HINGE_Z + 0.10:
        f *= 0.86                            # hinge cover strip
    if ax > 4.35:
        f *= 0.90
    # navigation lights near the tip leading edge: red left, green right
    if math.hypot(ax - 4.38, z - (wing_le(4.38) - 0.22)) < 0.055:
        return fixed((200, 30, 26) if side < 0 else (40, 190, 80))
    if side < 0:
        mp = insignia(x + 2.95, z + 0.40, 0.45, False)
        me = insignia(x + 2.95, z + 0.40, 0.45, True)
    return feat(f, 1.0, mp, me, camo)


def fin_feature(p, n, side):
    x, y, z = p
    camo = camo_value(z, y * 1.2 + 9.0, SEED + 2)
    f = 1.0
    mp = me = 0
    le, te = fin_le(y), fin_te(y)
    if y > 2.88:
        return fixed((58, 60, 64))           # tip antenna cap
    if abs(le - z - 0.14) < PANEL_W:
        f *= line_f(abs(le - z - 0.14))
    zr = te + 0.62                           # rudder hinge line (visual only: no yaw control)
    if 0.95 < y < 2.75 and abs(z - zr) < PANEL_W:
        f *= line_f(abs(z - zr))
    for yr in (0.95, 2.75):
        if abs(y - yr) < PANEL_W and z < zr:
            f *= line_f(abs(y - yr))
    if y < 1.0 and abs(y - 0.92) < PANEL_W:
        f *= line_f(abs(y - 0.92))
    # tail band (player: low-vis dark; enemy: red tip band)
    if 2.42 < y < 2.70:
        mp = 4
    if 2.50 < y < 2.88:
        me = 3
    # numbers: reading direction +Z on the right side, -Z on the left
    cell = 0.095
    width = 7 * cell
    zc, ytop = -4.62, 2.18
    s = ((z - zc) if side > 0 else (zc - z)) + 0.5 * width
    r = ytop - y
    if text_hit(s, r, "07", cell):
        mp = 1
    if text_hit(s, r, "21", cell):
        me = 2
    return feat(f, 1.0, mp, me, camo)


def stab_feature(p, n):
    x, y, z = p
    ax = abs(x)
    camo = camo_value(z, ax * 1.1 + 13.0, SEED + 3)
    t = lin(ax, STAB_ROOT_X, STAB_TIP_X)
    zle = -4.50 - 1.55 * t
    f = 1.0
    if abs(zle - z - 0.20) < PANEL_W:
        f *= line_f(abs(zle - z - 0.20))
    if ax > STAB_TIP_X - 0.12:
        f *= 0.88
    if abs(ax - 1.75) < PANEL_W:
        f *= line_f(abs(ax - 1.75))
    return feat(f, 1.0, 0, 0, camo)


def flap_feature(p, n):
    x, y, z = p
    ax = abs(x)
    camo = camo_value(z, ax * 1.1 + 3.0, SEED + 1)
    f = 1.0
    if z > FLAP_HINGE_Z - 0.05:
        f *= 0.80
    for xc in (2.10,):
        if abs(ax - xc) < PANEL_W:
            f *= line_f(abs(ax - xc))
    return feat(f, 1.0, 0, 0, camo)


def mouth_feature(p, n):
    x, y, z = p
    e = (abs(x) / 0.48) ** 3 + ((y + 0.97) / 0.27) ** 2
    if e < 1.0:
        t = smooth(-1.24, -0.75, y)
        return fixed(P.lerp((6, 6, 8), (26, 26, 30), t))
    if e < 1.18:
        return feat(0.72, 0.5)
    return feat(0.95, 0.5)


# ---------------------------------------------------------------------------------------------------- baking

SS = 3


def bake_features(bake):
    bk = BK.Baker(TEXN, TEXN, ss=SS)
    for region, tcs, ps, ns, key in bake:
        if key == "x":
            keys = [p[0] for p in ps]
        elif key == "y":
            keys = [p[1] for p in ps]
        elif key == "z":
            keys = [p[2] for p in ps]
        else:
            keys = None
        bk.add_tri(tcs, ps, ns, keys, region, LAYOUT[region])
    for name in sorted(LAYOUT):
        bk.dilate(LAYOUT[name], steps=6)
    W = bk.W
    can = [False] * (W * bk.H)
    x0, y0, w, h = [v * SS for v in LAYOUT["fus"]]
    fx0, fy0, fw, fh = LAYOUT["fus"]
    c0 = fx0 + 2 + lin(CANOPY_Z[1], FUS_Z[0], FUS_Z[1]) * (fw - 4)
    c1 = fx0 + 2 + lin(CANOPY_Z[0], FUS_Z[0], FUS_Z[1]) * (fw - 4)
    r1 = fy0 + 2 + (1.0 - ROWS[P_RAIL]) * (fh - 4)
    for iy in range(y0, y0 + h):
        for ix in range(x0, x0 + w):
            tx, ty = bk.texel(ix, iy)
            can[iy * W + ix] = c0 <= tx <= c1 and ty <= r1
    cd = bk.edge_distance(can, cap=4, rects=[LAYOUT["fus"]])
    feats = [None] * (W * bk.H)
    for i in range(W * bk.H):
        p = bk.pos[i]
        if p is None:
            continue
        reg = bk.reg[i]
        nn = bk.nrm[i]
        if reg == "fus":
            feats[i] = fus_feature(p, nn, can[i], cd[i])
        elif reg in ("wing_r", "wing_l"):
            feats[i] = wing_feature(p, nn, 1 if reg == "wing_r" else -1)
        elif reg in ("fin_r", "fin_l"):
            feats[i] = fin_feature(p, nn, 1 if reg == "fin_r" else -1)
        elif reg in ("stab_r", "stab_l"):
            feats[i] = stab_feature(p, nn)
        elif reg in ("flap_r", "flap_l"):
            feats[i] = flap_feature(p, nn)
        elif reg == "mouth":
            feats[i] = mouth_feature(p, nn)
    return feats, W, bk.H


LIVERIES = {
    # name: upper, lower, camo dark (None = no camo), marks: 1 dark, 2 light, 3 accent, 4 band
    "grey": {"upper": (126, 133, 143), "lower": (168, 173, 180), "camo": None,
             "marks": {1: ("shade", 0.62), 2: ("mix", (196, 200, 206), 0.55), 3: ("shade", 0.62), 4: ("shade", 0.80)},
             "key": "mp"},
    "aggr": {"upper": (78, 88, 104), "lower": (90, 96, 108), "camo": (34, 38, 48),
             "marks": {1: ("fix", (22, 22, 26)), 2: ("fix", (176, 176, 170)), 3: ("fix", (150, 26, 24)),
                       4: ("shade", 0.75)},
             "key": "me"},
}


def paint_livery(feats, W, H, name):
    liv = LIVERIES[name]
    up, lo, dark = liv["upper"], liv["lower"], liv["camo"]
    mi = 3 if liv["key"] == "mp" else 4
    data = []
    for fe in feats:
        if fe is None:
            data.append(lo)
            continue
        if fe[0] == 1:
            data.append((fe[1], fe[2], fe[3]))
            continue
        _, f, tone, mp, me, camo = fe
        top = up
        if dark is not None:
            top = P.lerp(dark, up, smooth(0.47, 0.53, camo))
        c = P.lerp(lo, top, tone)
        m = fe[mi]
        if m:
            kind = liv["marks"][m]
            if kind[0] == "shade":
                c = P.shade(c, kind[1])
            elif kind[0] == "mix":
                c = P.lerp(c, kind[1], kind[2])
            else:
                c = kind[1]
        data.append((P.clamp8(c[0] * f), P.clamp8(c[1] * f), P.clamp8(c[2] * f)))
    hi = Image.new("RGB", (W, H))
    hi.putdata(data)
    img = hi.resize((TEXN, TEXN), Image.BOX)
    px = img.load()
    pcol = {"tip": (70, 72, 78), "engine": (26, 24, 24), "rail": P.shade(up, 0.92), "pylon": P.shade(lo, 0.95),
            "ventral": lo, "nozzle": (96, 94, 90), "black": BLACK, "lower": lo, "upper": up}
    for pname in sorted(PATCH):
        x0, y0, w, h = PATCH[pname]
        for yy in range(y0, y0 + h):
            for xx in range(x0, x0 + w):
                px[xx, yy] = pcol[pname]
    return img


def parts_texture():
    S = 2
    W = PTEX * S
    hi = Image.new("RGB", (W, W), (60, 60, 64))
    px = hi.load()
    for region in ("noz_out", "noz_in"):
        x0, y0, w, h = [v * S for v in PLAYOUT[region]]
        for iy in range(y0, y0 + h):
            v = (iy - y0 + 0.5) / h
            for ix in range(x0, x0 + w):
                u = (ix - x0 + 0.5) / w
                fu = (u * NOZ_SEG) % 1.0
                k = int(u * NOZ_SEG)
                grime = value_noise_1d(u * 48.0 + v * 3.0, seed=SEED + 7, period=48)
                if region == "noz_out":
                    c = P.lerp((86, 88, 92), (124, 110, 96), smooth(0.25, 0.85, v))
                    if 0.55 < v < 0.75:
                        c = P.lerp(c, (92, 98, 122), 0.35)
                    if k % 2:
                        c = P.shade(c, 0.93)
                    if fu < 0.07 or fu > 0.93:
                        c = P.shade(c, 0.55)
                    if v > 0.92:
                        c = P.shade(c, 0.6)
                    c = P.shade(c, 0.9 + 0.2 * grime)
                else:
                    c = P.lerp((40, 37, 35), (72, 60, 50), v)
                    if fu < 0.06 or fu > 0.94:
                        c = P.shade(c, 0.6)
                    c = P.shade(c, 0.85 + 0.3 * grime)
                px[ix, iy] = c
    # wheel face
    x0, y0, w, h = [v * S for v in PLAYOUT["wheel"]]
    for iy in range(y0, y0 + h):
        for ix in range(x0, x0 + w):
            dx = (ix - x0 + 0.5) / w - 0.5
            dy = (iy - y0 + 0.5) / h - 0.5
            r = math.hypot(dx, dy) * 2.0
            if r > 0.62:
                c = (26, 26, 28)
            elif r > 0.56:
                c = (110, 112, 116)
            elif r > 0.16:
                c = (168, 170, 174) if int(math.degrees(math.atan2(dy, dx)) + 360) % 60 > 8 else (120, 122, 126)
            else:
                c = (90, 92, 96)
            px[ix, iy] = c
    img = hi.resize((PTEX, PTEX), Image.BOX)
    p2 = img.load()
    pc = {"strut": (204, 206, 208), "oleo": (226, 230, 234), "tyre": (24, 24, 26), "hub": (150, 152, 156),
          "door": (150, 156, 164), "dark": (34, 34, 36)}
    for name in sorted(PPATCH):
        x0, y0, w, h = PPATCH[name]
        for yy in range(y0, y0 + h):
            for xx in range(x0, x0 + w):
                p2[xx, yy] = pc[name]
    return img


def flame_texture():
    """RGBA 128x128: v = 0 at the nozzle -> 1 at the tip, u around (tileable). Premultiplied (additive)."""
    S = 2
    N = 128 * S
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    px = img.load()
    diamonds = [(0.13, 0.50), (0.28, 0.38), (0.43, 0.26), (0.57, 0.15)]
    for iy in range(N):
        v = (iy + 0.5) / N
        body = (1.0 - v) ** 1.35 * smooth(0.0, 0.04, v)
        dia = sum(a * math.exp(-((v - c) / 0.028) ** 2) for c, a in diamonds)
        col = P.lerp((255, 248, 232), (255, 178, 92), smooth(0.0, 0.35, v))
        col = P.lerp(col, (220, 86, 40), smooth(0.35, 0.8, v))
        for ix in range(N):
            u = (ix + 0.5) / N
            streak = value_noise_1d(u * 10.0 + v * 1.5, seed=SEED + 11, period=10)
            streak2 = value_noise_1d(u * 23.0 - v * 2.0, seed=SEED + 12, period=23)
            inten = body * (0.72 + 0.28 * streak) * (0.85 + 0.15 * streak2) + dia * (0.8 + 0.2 * streak)
            inten = min(1.0, 0.82 * inten)
            c = P.lerp(col, (255, 252, 244), min(1.0, dia * 0.9))
            r, g, b = [P.clamp8(ch * inten) for ch in c]
            px[ix, iy] = (r, g, b, P.clamp8(255 * inten))
    return img.resize((128, 128), Image.BOX)


def glow_texture():
    N = 128
    S = 2
    img = Image.new("RGB", (N * S, N * S), (0, 0, 0))
    px = img.load()
    for iy in range(N * S):
        for ix in range(N * S):
            dx = (ix + 0.5) / (N * S) - 0.5
            dy = (iy + 0.5) / (N * S) - 0.5
            r = math.hypot(dx, dy) * 2.0
            if r >= 1.0:
                continue
            core = math.exp(-(r / 0.22) ** 2)
            halo = (1.0 - r) ** 2.2
            inten = min(1.0, core * 0.9 + halo * 0.55)
            c = P.lerp((255, 120, 50), (255, 244, 220), core)
            px[ix, iy] = (P.clamp8(c[0] * inten), P.clamp8(c[1] * inten), P.clamp8(c[2] * inten))
    return img.resize((N, N), Image.BOX)


# ---------------------------------------------------------------------------------------------------- previews

def retex(geo, frm, to):
    g = geo.copy()
    cache = {}
    tm = []
    for m in g.TM:
        if isinstance(m, M.Material) and m.tex == frm:
            if m.name not in cache:
                cache[m.name] = M.Material(m.name, to, m.tex1, m.blend, m.stage1, m.cull_none, m.flags)
            tm.append(cache[m.name])
        else:
            tm.append(m)
    g.TM = tm
    return g


def assembled(lod0, parts, tex="jet_grey", stab=0.0, flap_r=0.0, flap_l=0.0, noz=0.0, gear=None, flame=None):
    """LOD0 + parts at their tags as one Geo (preview). Angles in degrees (positive = trailing edge up).
    noz: nozzle opening 0..1 (x/y scale 1 + 0.18 * noz). gear: None = up (not drawn), else gearPos 0..1.
    flame: None or ab level 0..1 (length scale 0.3 + 0.7 * ab)."""
    g = retex(lod0, "jet_grey", tex)
    for key, hinge, ang in (("stab_r", HINGE_STAB, stab), ("stab_l", mirror_pt(HINGE_STAB), stab),
                            ("flap_r", HINGE_FLAP, flap_r), ("flap_l", mirror_pt(HINGE_FLAP), flap_l)):
        c = retex(parts[key][1], "jet_grey", tex)
        g.merge(c.rotate_x(ang).translate(*hinge))
    s = 1.0 + 0.18 * noz
    g.merge(parts["nozzle"][1].copy().scale(s, s, 1.0).translate(*HINGE_NOZ))
    if gear is not None:
        for pos, sign in ((GEAR_N, 1.0), (GEAR_M, -1.0), (mirror_pt(GEAR_M), -1.0)):
            g.merge(parts["gear"][1].copy().rotate_x(sign * (1.0 - gear) * 90.0).translate(*pos))
    if flame is not None:
        L = 0.3 + 0.7 * flame
        g.merge(parts["flame"][1].copy().scale(s, s, L).translate(*NOZZLE_EXIT))
    return g


def make_previews(kit, mb, geos, parts):
    td = kit.tex_dirs
    lod0 = geos[0]
    V.clear_cache()
    for tex, tag in (("jet_grey", "grey"), ("jet_aggr", "aggr")):
        jet = assembled(lod0, parts, tex)
        kit.save_preview("jet_views_%s.png" % tag,
                         V.views(jet, 384, tex_dirs=td, facing="+z", title="jet %s (LOD0 + parts, gear up)" % tag))
        kit.save_preview("jet_turn_%s.png" % tag,
                         V.turntable(jet, 8, 256, elev=18.0, tex_dirs=td, facing="+z", title="jet %s" % tag))
    kit.save_preview("jet_lods.png", V.lods(mb.to_msh(), 320, tex_dirs=td, facing="+z",
                                            title="jet.msh LODs (LOD0 without parts)"))
    # surfaces deflected: stab +20 (TE up), right flaperon +20 (TE up), left -20 (TE down), nozzle open, AB
    jd = assembled(lod0, parts, "jet_grey", stab=20.0, flap_r=20.0, flap_l=-20.0, noz=1.0, gear=1.0, flame=1.0)
    W, H = 512, 384
    sheet = Image.new("RGB", (W * 2, H * 2), (90, 90, 96))
    cams = [
        ("tail, right side (ortho): stabilator +20 = TE up", V.Camera((30.0, 0.2, -5.6), (0.0, 0.2, -5.6), ortho=2.0)),
        ("from behind (ortho): flap R +20 up, L -20 down", V.Camera((0.0, 0.3, -30.0), (0.0, 0.3, 0.0), ortho=3.6)),
        ("rear 3/4 above", V.Camera((-8.0, 5.5, -15.0), (0.0, 0.0, -2.5), fov=42.0)),
        ("right side (ortho): gear down, nozzle open, AB", V.Camera((30.0, 0.0, -1.0), (0.0, 0.0, -1.0), ortho=5.2)),
    ]
    for i, (name, cam) in enumerate(cams):
        img = V.render([jd], cam, (W, H), bg=V.sky_image(W, H), tex_dirs=td, light=M.vnorm((0.4, 0.8, 0.3)))
        V.label(img, name, color=(250, 250, 250))
        sheet.paste(img, ((i % 2) * W, (i // 2) * H))
    V.label(sheet, "surfaces deflected: stab +20, flaperons +-20, nozzle open (s 1.18), AB flame, gear down",
            (6, H * 2 - 18), (250, 250, 250))
    kit.save_preview("jet_deflect.png", sheet)
    # chase camera (DESIGN 7: 24 m behind, 6 m up, look 40 m ahead, 75 deg in AB) + an enemy ahead
    W, H = 640, 480
    me = assembled(lod0, parts, "jet_grey", stab=4.0, flap_r=8.0, flap_l=-8.0, noz=1.0, flame=1.0)
    en = assembled(lod0, parts, "jet_aggr", stab=-3.0, noz=0.0)
    sea = M.quad((-3000.0, -400.0, -3000.0), (-3000.0, -400.0, 3000.0), (3000.0, -400.0, 3000.0),
                 (3000.0, -400.0, -3000.0))
    sea.set_color((52, 84, 110))
    sea.TM = [None] * sea.ntris
    vfov = math.degrees(2.0 * math.atan(math.tan(math.radians(75.0) * 0.5) * H / W))
    cam = V.Camera((0.0, 6.0, -24.0), (0.0, 0.0, 40.0), fov=vfov)
    img = V.render([(sea, (0, 0, 0), 0.0), (me, (0.0, 0.0, 0.0), 0.0), (en, (-18.0, 6.0, 120.0), 25.0)], cam, (W, H),
                   bg=V.sky_image(W, H), tex_dirs=td, fog=(300.0, 4000.0), light=M.vnorm((0.3, 0.8, 0.5)))
    V.label(img, "chase view mock: player (grey, AB) + enemy (aggressor) 120 m ahead", color=(250, 250, 250))
    kit.save_preview("jet_chase.png", img)
    # on the ground, gear down
    W, H = 900, 360
    gj = assembled(lod0, parts, "jet_grey", gear=1.0)
    ground = M.quad((-40.0, GROUND_Y, -40.0), (-40.0, GROUND_Y, 40.0), (40.0, GROUND_Y, 40.0), (40.0, GROUND_Y, -40.0))
    ground.set_color((92, 94, 96))
    ground.TM = [None] * ground.ntris
    img = V.render([(ground, (0, 0, 0), 0.0), gj], V.Camera((30.0, 0.5, 0.4), (0.0, 0.5, 0.4), ortho=3.4), (W, H),
                   bg=V.sky_image(W, H), tex_dirs=td, light=M.vnorm((0.5, 0.8, 0.2)))
    V.label(img, "gear down, wheels on y = %.2f (CG height %.2f m); length 15.27 m" % (GROUND_Y, -GROUND_Y))
    kit.save_preview("jet_ground.png", img)
    # parts
    psheet = Image.new("RGB", (256 * 4, 256 * 2), (90, 90, 96))
    for i, key in enumerate(("stab_r", "stab_l", "flap_r", "flap_l", "nozzle", "gear", "flame")):
        mbp = parts[key][0]
        lo, hi = V.bounds_of(mbp.to_msh())
        c = tuple((lo[k] + hi[k]) * 0.5 for k in range(3))
        r = max(hi[k] - lo[k] for k in range(3))
        eye = (c[0] + r * 1.4, c[1] + r * 1.1, c[2] - r * 1.6)
        img = V.render([mbp.to_msh()], V.Camera(eye, c, fov=40.0), (256, 256), tex_dirs=td)
        V.label(img, "%s  %d tris (origin = hinge)" % (mbp.name, mbp.tri_counts()[0]))
        psheet.paste(img, ((i % 4) * 256, (i // 4) * 256))
    legend = Image.new("RGB", (256, 256), (70, 70, 76))
    for j, line in enumerate(["parts: origin = hinge,", "hinge axis = local X.", "angle > 0 (rot about +X)",
                              "= trailing edge UP.", "nozzle: scale x,y 1+0.18*open", "gear: rot X, swing to tag dir",
                              "flame: scale z by length"]):
        V.label(legend, line, (8, 10 + 18 * j), (235, 235, 235))
    psheet.paste(legend, (3 * 256, 256))
    kit.save_preview("jet_parts.png", psheet)
    kit.save_preview("jet_tex.png", V.contact_sheet(
        [kit.out("tex/jet_grey.bmp"), kit.out("tex/jet_aggr.bmp"), kit.out("tex/jet_parts.bmp"),
         kit.out("tex/flame.bmp"), kit.out("tex/flame_a.bmp"), kit.out("tex/glow_nozzle.bmp")],
        cols=3, cell=256, title="jet_grey, jet_aggr, jet_parts, flame, flame_a, glow_nozzle"))


# ---------------------------------------------------------------------------------------------------- build

def build(out_dir, kit):
    mb, geos, bake = build_body()
    parts = build_surfaces(bake)
    parts["nozzle"] = build_nozzle()
    parts["gear"] = build_gear()
    parts["flame"] = build_flame()
    kit.save_msh("meshes/jet.msh", mb)
    for key in ("stab_l", "stab_r", "flap_l", "flap_r", "nozzle", "gear", "flame"):
        kit.save_msh("meshes/jet_%s.msh" % key, parts[key][0])
    feats, W, H = bake_features(bake)
    kit.save_bmp("tex/jet_grey.bmp", paint_livery(feats, W, H, "grey"))
    kit.save_bmp("tex/jet_aggr.bmp", paint_livery(feats, W, H, "aggr"))
    kit.save_bmp("tex/jet_parts.bmp", parts_texture())
    kit.save_bmp("tex/flame.bmp", flame_texture(), alpha=True, alpha_bleed=0)
    kit.save_bmp("tex/glow_nozzle.bmp", glow_texture())
    kit.log("jet tris per LOD %s; parts %s" % (mb.tri_counts(), dict((k, parts[k][0].tri_counts()[0])
                                                                       for k in sorted(parts))))
    if kit.previews:
        make_previews(kit, mb, geos, parts)


if __name__ == "__main__":
    from assetkit import build as B
    sys.exit(B.main_for(__file__))
