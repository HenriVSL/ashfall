"""Asset validator: format checks, budgets per asset class, texture rules, .trk container checks.

CLI (exit code = number of errors, capped at 255; warnings don't count):
    PYTHONPATH=engine/tools python3 -m assetkit.validate data/meshes/v10gt.msh data/tex/car_*.bmp
    python3 engine/tools/assetkit/validate.py data/            # same, works from anywhere; dirs recurse
    options: --class CLASS (force the budget class of every mesh), --tex-dir DIR (extra texture search
             path), -q (errors only), --list-classes

Python API:
    probs = check_file(path, cls=None, tex_dirs=None)        # [Problem]
    probs = check_paths([...], classes={"meshes/v10gt.msh": "car"})
    errors(probs) -> only the errors;  class_for("tree_pine") -> "tree"
Problem fields: path, code, msg, level ("error" | "warn"). Codes: format, winding, normals, uv, degenerate,
bounds, tags, material, missing-tex, budget, lod, npot, size, aspect, key, fringe, alpha, wav, trk, pools.
"""

import os
import struct
import sys

if __name__ == "__main__" and not __package__:
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    sys.path = [p for p in sys.path if os.path.abspath(p or ".") != os.path.dirname(os.path.abspath(__file__))]
    import assetkit.validate as _v
    sys.exit(_v.main(sys.argv[1:]))

from . import export
from . import image
from . import palette

# ---------------------------------------------------------------------------------------------------- budgets
# Triangle budgets per LOD (min, max) per asset class (DESIGN 8.1). LODs beyond the listed ones must not
# have more triangles than the previous LOD. "lods_required": exact LOD count. "tex_max": max texture side.
# "hinge_origin": warn when the origin (hinge) lies outside the mesh box (jet moving parts).
BUDGETS = {
    "car":      {"lods": [(600, 800), (220, 300), (70, 110)], "lods_required": 3,
                 "tags_required": ["wheel_fl", "wheel_fr", "wheel_rl", "wheel_rr", "brake_l", "brake_r",
                                   "cam_bumper"]},
    "wheel":    {"lods": [(8, 64), (4, 24)]},
    "tree":     {"lods": [(4, 6)]},
    "bush":     {"lods": [(2, 4)]},
    "treeline": {"lods": [(2, 16)]},
    "building": {"lods": [(60, 300)]},
    "prop":     {"lods": [(8, 40)]},
    "free":     {"lods": []},          # samples, tests, debug meshes: format checks only
    # ---- jet classes (jet DESIGN 8.8; added by jet T01) ----
    "jet":      {"lods": [(600, 950), (220, 380), (70, 140)], "lods_required": 3,
                 "tags_required": ["cam_cockpit", "cam_chase", "nozzle", "hinge_stab_l", "hinge_stab_r",
                                   "hinge_flap_l", "hinge_flap_r"] +
                                  ["rail_%d" % _i for _i in range(8)] + ["flare_l", "flare_r"]},
    "jetpart":  {"lods": [(4, 60)], "hinge_origin": True},  # stabilators, flaperons, nozzle, gear; origin = hinge
    "missile":  {"lods": [(30, 120), (1, 20)]},
    "bldg":     {"lods": [(12, 300)]},  # base buildings (hangars, tower, tanks); racer "building" stays 60-300
}

# Mesh class by file-name prefix (first match wins). Declare others in a generator's ASSETS entry
# ({"file": "meshes/x.msh", "class": "prop"}) or pass --class on the command line.
CLASS_PREFIXES = [
    ("v10gt", "car"), ("car_", "car"),
    ("wheel", "wheel"),
    ("treeline", "treeline"), ("tree", "tree"), ("bush", "bush"),
    ("barn", "building"), ("farmhouse", "building"), ("house", "building"), ("church", "building"),
    ("sauna", "building"), ("shop", "building"), ("bldg_", "bldg"), ("building", "building"),
    ("shelter", "building"), ("jetty", "building"),
    ("post", "prop"), ("sign", "prop"), ("bale", "prop"), ("hay", "prop"), ("mailbox", "prop"),
    ("pole", "prop"), ("chevron", "prop"), ("kmpost", "prop"), ("boulder", "prop"), ("rock", "prop"),
    ("prop_", "prop"), ("fence", "prop"), ("rail", "prop"),
    ("sample", "free"), ("test", "free"), ("dbg_", "free"),
    # jet (DESIGN 8.8): "jetpart" before "jet"; base buildings use class bldg
    ("jetpart", "jetpart"), ("jet_stab", "jetpart"), ("jet_flap", "jetpart"), ("jet_nozzle", "jetpart"), ("jet_gear", "jetpart"), ("jet_flame", "jetpart"), ("jet", "jet"), ("missile", "missile"), ("msl_", "missile"), ("lrm", "missile"), ("srm", "missile"),
    ("hangar", "bldg"), ("tower", "bldg"), ("tank", "bldg"), ("radome", "bldg"), ("bunker", "bldg"),
]

# Texture rules: (prefix, exact size(s) or None, max side). exact = one (w, h) or a list of allowed sizes.
# Name rules (prefix of the file stem): the first match applies. Folder rules (prefix ending in "/"): apply to
# every texture whose parent folder has that name (e.g. data/sky/), in addition to a name rule.
TEX_RULES = [
    ("car_", (256, 256), 256),
    ("wheel", None, 128),
    ("env_", None, 128),
    ("sky/", [(256, 256), (256, 128)], 256),     # sky panorama tiles: High 256x256, Low 256x128 (A5, T15)
    ("fx_", [(128, 128), (256, 256)], 256),      # jet effect atlases (DESIGN 8.8)
]

CAR_TAGS = BUDGETS["car"]["tags_required"]


class Problem(object):
    def __init__(self, path, code, msg, level="error"):
        self.path = path
        self.code = code
        self.msg = msg
        self.level = level

    def __repr__(self):
        return "%s: %s [%s] %s" % (self.path, self.level, self.code, self.msg)


def errors(probs):
    return [p for p in probs if p.level == "error"]


def class_for(name):
    """Budget class of a mesh name (file stem) by prefix, or None."""
    n = os.path.basename(name).lower()
    for pre, cls in CLASS_PREFIXES:
        if n.startswith(pre):
            return cls
    return None


def _stem(path):
    return os.path.splitext(os.path.basename(path))[0]


def _name_ok(s):
    return 0 < len(s) <= 31 and all(c in "abcdefghijklmnopqrstuvwxyz0123456789_" for c in s)


# ---------------------------------------------------------------------------------------------------- .msh

def _vsub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def find_texture(name, tex_dirs):
    for d in tex_dirs or []:
        p = os.path.join(d, name + ".bmp")
        if os.path.exists(p):
            return p
    return None


def check_msh(path, data=None, cls=None, tex_dirs=None):
    """Checks of one .msh (bytes or path). tex_dirs: folders searched for <tex>.bmp (None = skip)."""
    P = []

    def err(code, msg):
        P.append(Problem(path, code, msg, "error"))

    def warn(code, msg):
        P.append(Problem(path, code, msg, "warn"))

    if data is None:
        with open(path, "rb") as f:
            data = f.read()
    try:
        m = export.read_msh(data)
    except ValueError as e:
        err("format", str(e))
        return P
    if m.version != export.MSH_VERSION:
        err("format", "version %d (expected %d)" % (m.version, export.MSH_VERSION))
    if m.file_size != m.expected_size:
        err("format", "file is %d bytes, header counts give %d" % (m.file_size, m.expected_size))
    nv = len(m.verts)
    if not m.lods:
        err("lod", "no LODs")
    if len(m.lods) > export.MAX_LODS:
        err("lod", "%d LODs (max %d)" % (len(m.lods), export.MAX_LODS))
    if len(m.tags) > export.MAX_TAGS:
        err("tags", "%d tags (max %d)" % (len(m.tags), export.MAX_TAGS))
    # materials
    for i, mt in enumerate(m.mats):
        if not _name_ok(mt["tex"]):
            err("material", "material %d texture name %r (need 1-31 chars of a-z 0-9 _)" % (i, mt["tex"]))
        if mt["tex1"] and not _name_ok(mt["tex1"]):
            err("material", "material %d tex1 name %r invalid" % (i, mt["tex1"]))
        if mt["blend"] > export.RB_SHADOW:
            err("material", "material %d blend %d unknown" % (i, mt["blend"]))
        if mt["stage1"] > export.RS1_ENVADD:
            err("material", "material %d stage1 %d unknown" % (i, mt["stage1"]))
        if mt["cullNone"] > 1:
            err("material", "material %d cullNone %d (0/1)" % (i, mt["cullNone"]))
        if mt["flags"] > 31:
            err("material", "material %d flags 0x%x has unknown bits" % (i, mt["flags"]))
        if tex_dirs is not None:
            for t in (mt["tex"], mt["tex1"]):
                if t and _name_ok(t) and find_texture(t, tex_dirs) is None:
                    err("missing-tex", "material %d texture %s.bmp not found in %s" % (i, t, ", ".join(tex_dirs)))
    # LOD / parts ranges
    lod_ok = True
    prev_max = -1.0
    expect_vert = 0
    expect_part = 0
    for li, (maxd, fp, npp, fv, nvv) in enumerate(m.lods):
        if fp != expect_part or fp + npp > len(m.parts):
            err("lod", "LOD %d parts %d..%d not contiguous/in range (%d parts)" % (li, fp, fp + npp, len(m.parts)))
            lod_ok = False
        if fv != expect_vert or fv + nvv > nv:
            err("lod", "LOD %d vertices %d..%d not contiguous/in range (%d verts)" % (li, fv, fv + nvv, nv))
            lod_ok = False
        if nvv > 65535:
            err("lod", "LOD %d has %d vertices (max 65535)" % (li, nvv))
        if not maxd > prev_max:
            err("lod", "LOD %d maxDist %.1f not greater than the previous LOD" % (li, maxd))
        prev_max = maxd
        expect_part = fp + npp
        expect_vert = fv + nvv
        if lod_ok:
            for p in range(fp, fp + npp):
                mat, fi, ni = m.parts[p]
                if mat < 0 or mat >= len(m.mats):
                    err("material", "part %d material %d out of range" % (p, mat))
                if ni % 3 or fi < 0 or fi + ni > len(m.idx):
                    err("format", "part %d indices %d+%d invalid" % (p, fi, ni))
                    lod_ok = False
                    continue
                bad = sum(1 for k in range(fi, fi + ni) if m.idx[k] >= nvv)
                if bad:
                    err("format", "part %d: %d indices >= LOD %d vertex count %d" % (p, bad, li, nvv))
                    lod_ok = False
    if expect_vert != nv and lod_ok:
        warn("lod", "%d vertices belong to no LOD" % (nv - expect_vert))
    # vertices
    nbad = 0
    for n in m.normals:
        l = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) ** 0.5
        if not abs(l - 1.0) <= 0.02:
            nbad += 1
    if nbad:
        err("normals", "%d normals are not unit length" % nbad)
    ubad = 0
    ufar = 0
    for v in m.verts:
        for x in (v[0], v[1], v[2], v[4], v[5]):
            if x != x or x in (float("inf"), float("-inf")):
                ubad += 1
        if abs(v[4]) > 256 or abs(v[5]) > 256:
            ufar += 1
    if ubad:
        err("uv", "%d NaN/inf values in positions/UVs" % ubad)
    if ufar:
        warn("uv", "%d vertices with |uv| > 256 (precision loss)" % ufar)
    # bounds
    if nv:
        (lx, ly, lz), (hx, hy, hz) = m.box
        out = sum(1 for v in m.verts if v[0] < lx - 1e-4 or v[1] < ly - 1e-4 or v[2] < lz - 1e-4 or
                  v[0] > hx + 1e-4 or v[1] > hy + 1e-4 or v[2] > hz + 1e-4)
        if out:
            err("bounds", "%d vertices outside the stored box" % out)
        r = max((v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) ** 0.5 for v in m.verts)
        if m.radius + 1e-3 < r:
            err("bounds", "radius %.3f smaller than the farthest vertex %.3f" % (m.radius, r))
    # tags
    names = []
    for name, pos, d in m.tags:
        if not name or any(ord(c) < 33 or ord(c) > 126 for c in name):
            err("tags", "tag name %r invalid" % name)
        names.append(name)
        l = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) ** 0.5
        if l > 1e-6 and abs(l - 1.0) > 0.02:
            warn("tags", "tag %s direction not unit length" % name)
    if len(set(names)) != len(names):
        err("tags", "duplicate tag names")
    # winding: per LOD
    if lod_ok:
        for li in range(len(m.lods)):
            _check_winding(m, li, err, warn)
    # budgets
    cls = cls or class_for(_stem(path))
    counts = m.tri_counts() if lod_ok else []
    if cls is None:
        warn("budget", "no budget class for %s (name prefix unknown; declare it in ASSETS)" % _stem(path))
    elif cls not in BUDGETS:
        err("budget", "unknown budget class %r" % cls)
    elif lod_ok:
        b = BUDGETS[cls]
        req = b.get("lods_required")
        if req and len(m.lods) != req:
            err("budget", "class %s needs %d LODs, mesh has %d" % (cls, req, len(m.lods)))
        for li, c in enumerate(counts):
            if li < len(b["lods"]):
                lo, hi = b["lods"][li]
                if c < lo or c > hi:
                    err("budget", "LOD %d has %d tris; class %s allows %d-%d" % (li, c, cls, lo, hi))
            elif li > 0 and c > counts[li - 1]:
                err("budget", "LOD %d has more tris (%d) than LOD %d (%d)" % (li, c, li - 1, counts[li - 1]))
        for t in b.get("tags_required", []):
            if t not in names:
                err("tags", "class %s needs tag %s" % (cls, t))
        if b.get("hinge_origin") and nv:
            # moving part: the origin is the hinge, so it must lie on/inside the box (small margin)
            (lx, ly, lz), (hx, hy, hz) = m.box
            mg = 0.05
            if not (lx - mg <= 0.0 <= hx + mg and ly - mg <= 0.0 <= hy + mg and lz - mg <= 0.0 <= hz + mg):
                warn("bounds", "class %s: origin (hinge) is outside the mesh box; hinge axis must be local X "
                               "through the origin" % cls)
    return P


def _check_winding(m, li, err, warn):
    tris = list(m.lod_triangles(li))
    if not tris:
        return
    V = m.verts
    N = m.normals
    disagree = 0
    degenerate = 0
    q = 1e4
    pid = {}
    vid = {}
    edges = {}
    culled = True
    vol = 0.0
    for mat, a, b, c in tris:
        pa, pb, pc = V[a][:3], V[b][:3], V[c][:3]
        fn = _cross(_vsub(pb, pa), _vsub(pc, pa))
        area2 = _dot(fn, fn)
        if area2 < 1e-18:
            degenerate += 1
            continue
        cull_none = m.mats[mat]["cullNone"] if 0 <= mat < len(m.mats) else 0
        if not cull_none:
            ns = (N[a][0] + N[b][0] + N[c][0], N[a][1] + N[b][1] + N[c][1], N[a][2] + N[b][2] + N[c][2])
            if _dot(fn, ns) < 0:
                disagree += 1
        else:
            culled = False
        vol += _dot(pa, _cross(pb, pc))
        ids = []
        for v in (a, b, c):
            k = vid.get(v)
            if k is None:
                pk = (int(round(V[v][0] * q)), int(round(V[v][1] * q)), int(round(V[v][2] * q)))
                k = pid.setdefault(pk, len(pid))
                vid[v] = k
            ids.append(k)
        for u, w in ((ids[0], ids[1]), (ids[1], ids[2]), (ids[2], ids[0])):
            if u != w:
                edges.setdefault((min(u, w), max(u, w)), []).append((u, w))
    if degenerate:
        warn("degenerate", "LOD %d: %d zero-area triangles" % (li, degenerate))
    if disagree:
        err("winding", "LOD %d: %d triangles whose clockwise face normal points against their vertex "
                       "normals (wrong winding or flipped normals)" % (li, disagree))
    flipped = sum(1 for l in edges.values() if len(l) == 2 and l[0] == l[1])
    if flipped:
        err("winding", "LOD %d: %d edges shared by two triangles with the same direction (inconsistent "
                       "winding)" % (li, flipped))
    closed = all(len(l) == 2 and l[0] != l[1] for l in edges.values())
    if closed and culled and vol < 0:
        err("winding", "LOD %d: closed mesh is inside out (signed volume %.3f m^3)" % (li, vol / 6.0))


# ---------------------------------------------------------------------------------------------------- .bmp

def check_bmp(path, data=None, max_size=image.MAX_SIZE):
    P = []

    def err(code, msg):
        P.append(Problem(path, code, msg, "error"))

    def warn(code, msg):
        P.append(Problem(path, code, msg, "warn"))

    if data is None:
        with open(path, "rb") as f:
            data = f.read()
    try:
        info = image.bmp_info(data)
    except ValueError as e:
        err("format", str(e))
        return P
    if info["sig"] != b"BM":
        err("format", "not a BMP (signature %r)" % info["sig"])
        return P
    is_alpha = _stem(path).endswith("_a")
    if info["isz"] != 40:
        err("format", "info header size %d (need BITMAPINFOHEADER 40)" % info["isz"])
    if info["comp"] != 0:
        err("format", "compressed BMP (BI_RGB required)")
    if info["planes"] != 1:
        err("format", "planes %d" % info["planes"])
    if info["h"] < 0:
        err("format", "top-down BMP (negative height); write bottom-up")
    want_bpp = 8 if is_alpha else 24
    if info["bpp"] != want_bpp and not (is_alpha and info["bpp"] == 24):
        err("format", "%d bpp (need %d)" % (info["bpp"], want_bpp))
    w, h = info["w"], abs(info["h"])
    if info["size"] != len(data):
        warn("format", "header file size %d != actual %d" % (info["size"], len(data)))
    for msg in image.size_problems(w, h, max_size):
        code = "npot" if "power of two" in msg else ("aspect" if "aspect" in msg else "size")
        err(code, msg)
    stem = _stem(path).lower()
    folder = os.path.basename(os.path.dirname(os.path.abspath(path))).lower()
    named = False
    for pre, exact, mx in TEX_RULES:
        if pre.endswith("/"):
            if folder != pre[:-1]:
                continue
            what = "%s textures" % pre
        else:
            if named or not stem.startswith(pre):
                continue
            named = True
            what = "%s* textures" % pre
        sizes = [exact] if exact and not isinstance(exact[0], (tuple, list)) else (exact or [])
        if sizes and (w, h) not in [tuple(z) for z in sizes]:
            err("size", "%s must be %s (is %dx%d)" % (what, " or ".join("%dx%d" % tuple(z) for z in sizes), w, h))
        if max(w, h) > mx:
            err("size", "%s at most %d px (is %dx%d)" % (what, mx, w, h))
    if P and any(p.code == "format" and p.level == "error" for p in P):
        return P
    try:
        img, info2 = image.read_bmp(data)
    except ValueError as e:
        err("format", str(e))
        return P
    if is_alpha:
        if img.mode != "L":
            px = img.load()
            if any(px[x, y][0] != px[x, y][1] or px[x, y][1] != px[x, y][2] for y in range(h) for x in range(w)):
                err("alpha", "alpha file is not greyscale")
        base = path[:-6] + path[-4:]
        if not os.path.exists(base):
            err("alpha", "alpha file without a base texture %s" % os.path.basename(base))
        else:
            with open(base, "rb") as f:
                bi = image.bmp_info(f.read(54))
            if (bi["w"], abs(bi["h"])) != (w, h):
                err("alpha", "alpha %dx%d does not match base %dx%d" % (w, h, bi["w"], abs(bi["h"])))
        return P
    # key purity + fringes
    px = img.load()
    near = 0
    fringe = 0
    nkey = 0
    for y in range(h):
        for x in range(w):
            p = px[x, y]
            if p == palette.KEY:
                nkey += 1
                continue
            if p[0] >= 200 and p[2] >= 200 and p[1] <= 60:
                near += 1
                continue
            if min(p[0], p[2]) - p[1] > 40:
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    xx, yy = x + dx, y + dy
                    if 0 <= xx < w and 0 <= yy < h and px[xx, yy] == palette.KEY:
                        fringe += 1
                        break
    if near:
        err("key", "%d near-magenta pixels that are not the exact key (255,0,255)" % near)
    if fringe:
        err("fringe", "%d magenta-tinted pixels next to keyed texels (threshold keyed art with "
                      "image.save_bmp(keyed=True) / key_threshold)" % fringe)
    if nkey and os.path.exists(image.alpha_path(path)):
        warn("alpha", "texture has both magenta key texels and an _a.bmp alpha file")
    return P


# ---------------------------------------------------------------------------------------------------- .wav

def check_wav(path, data=None):
    P = []
    if data is None:
        with open(path, "rb") as f:
            data = f.read()
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return [Problem(path, "wav", "not a RIFF/WAVE file")]
    o = 12
    fmt = None
    dlen = None
    while o + 8 <= len(data):
        cid, size = struct.unpack_from("<4sI", data, o)
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", data, o + 8)
        elif cid == b"data":
            dlen = size
        o += 8 + size + (size & 1)
    if fmt is None or dlen is None:
        return [Problem(path, "wav", "missing fmt or data chunk")]
    tag, ch, rate, _, _, bits = fmt
    if tag != 1:
        P.append(Problem(path, "wav", "not PCM (format tag %d)" % tag))
    if ch != 1:
        P.append(Problem(path, "wav", "%d channels (engine sounds are mono)" % ch))
    if bits not in (8, 16):
        P.append(Problem(path, "wav", "%d bits per sample" % bits))
    if rate != 22050:
        P.append(Problem(path, "wav", "sample rate %d (pack standard 22050)" % rate, "warn"))
    return P


# ---------------------------------------------------------------------------------------------------- .trk

KNOWN_CHUNKS = ["INFO", "SKY ", "TEXL", "MATL", "MSHL", "CELL", "VPOL", "BTCH", "INDX", "INST", "COLT", "COLG",
                "WALL", "BOXS", "SLCE", "CHKP", "GRID", "MAPI", "PVS "]
# Record sizes of the trackc chunks (layout block in tools/trackc/trkfile.py, structs in e_coll.h / track.h).
COLLTRI_SIZE = 56          # CollTri: f32 a[3] b[3] c[3] n[3] d; u8 surface, flags; u16 pad
WALLSEG_SIZE = 60          # WallSeg: f32 x0 z0 x1 z1 nx nz n0x n0z n1x n1z yLo yHi; s32 prev, next; u8 type, side; u16
TRKSLICE_SIZE = 92         # TrkSlice: f32 pos fwd right up [12]; s wallL wallR roadL roadR bank grade aiLat aiSpeed aiCurv; u16 flags, zone
COLLBOX_SIZE = 32          # CollBox: f32 c[3], yaw, hx, hy, hz; u8 surface (+3 pad)
TRK_FIXED = {"INFO": 136, "SKY ": 312}                                         # exact chunk sizes
TRK_RECORD = {"TEXL": 32, "MSHL": 32, "MATL": 20, "INST": 40, "COLT": COLLTRI_SIZE, "SLCE": TRKSLICE_SIZE,
              "GRID": 16, "MAPI": 8}                                          # size = n * record
CELL_HEADER, CELL_RECORD, GRIDBLOCK_HEADER = 20, 52, 24
GRID_SLOTS = 8


def _gridblock(d, off, nitems, name, err):
    """Check a GridBlock at d[off:]; -> end offset or None. nitems: count of the referenced records."""
    if len(d) - off < GRIDBLOCK_HEADER:
        err("trk", "%s GridBlock truncated (%d bytes)" % (name, len(d) - off))
        return None
    x0, z0, cell, w, h, nrefs = struct.unpack_from("<3f3i", d, off)
    if w < 0 or h < 0 or nrefs < 0 or not cell > 0:
        err("trk", "%s GridBlock header bad (cell %g, %d x %d, %d refs)" % (name, cell, w, h, nrefs))
        return None
    end = off + GRIDBLOCK_HEADER + 8 * w * h + 4 * nrefs
    if end > len(d):
        err("trk", "%s GridBlock needs %d bytes, chunk has %d" % (name, end, len(d)))
        return None
    ranges = struct.unpack_from("<%dI" % (2 * w * h), d, off + GRIDBLOCK_HEADER)
    bad = sum(1 for k in range(0, len(ranges), 2) if ranges[k] + ranges[k + 1] > nrefs)
    if bad:
        err("trk", "%s GridBlock: %d bucket ranges past numRefs %d" % (name, bad, nrefs))
    refs = struct.unpack_from("<%dI" % nrefs, d, off + GRIDBLOCK_HEADER + 8 * w * h)
    if nitems is not None and refs and max(refs) >= nitems:
        err("trk", "%s GridBlock refers to record %d of %d" % (name, max(refs), nitems))
    return end


def _check_trk_records(t, err, warn):
    """Record sizes and cross references of the trackc chunks (T03 layout)."""
    n = {}
    for cid, size in sorted(TRK_FIXED.items()):
        d = t.get(cid)
        if d is not None and len(d) != size:
            err("trk", "%s size %d (expected %d)" % (cid.strip(), len(d), size))
    for cid, rec in sorted(TRK_RECORD.items()):
        d = t.get(cid)
        if d is None:
            continue
        if len(d) % rec:
            err("trk", "%s size %d not a multiple of its %d-byte record" % (cid, len(d), rec))
        n[cid] = len(d) // rec
    if t.get("GRID") is not None and n.get("GRID") != GRID_SLOTS:
        warn("trk", "GRID has %d slots (expected %d)" % (n.get("GRID"), GRID_SLOTS))
    if n.get("MAPI", 0) > 256:
        warn("trk", "MAPI has %d points (max 256)" % n["MAPI"])
    d = t.get("MATL")
    if d is not None and "TEXL" in n and not len(d) % 20:
        for k in range(len(d) // 20):
            tex0, tex1 = struct.unpack_from("<ii", d, k * 20)
            if not 0 <= tex0 < n["TEXL"] or not -1 <= tex1 < n["TEXL"]:
                err("trk", "MATL %d texture index %d/%d out of range (TEXL has %d)" % (k, tex0, tex1, n["TEXL"]))
                break
    d = t.get("CELL")
    ncells = None
    if d is not None:
        if len(d) < CELL_HEADER:
            err("trk", "CELL size %d < header %d" % (len(d), CELL_HEADER))
        else:
            gx0, gz0, gw, gh, ncells = struct.unpack_from("<5i", d, 0)
            if len(d) != CELL_HEADER + CELL_RECORD * ncells:
                err("trk", "CELL size %d != %d + %d * numCells %d" % (len(d), CELL_HEADER, CELL_RECORD, ncells))
                ncells = None
            else:
                btch = t.get("BTCH")
                nb = len(btch) // 24 if btch is not None else 0
                for k in range(ncells):
                    r = struct.unpack_from("<2h6f6i", d, CELL_HEADER + CELL_RECORD * k)
                    cx, cz = r[0], r[1]
                    b0, b1, c0, c1, i0, ni = r[8:14]
                    if not (gx0 <= cx < gx0 + gw and gz0 <= cz < gz0 + gh):
                        err("trk", "CELL %d (%d, %d) outside the grid" % (k, cx, cz))
                        break
                    if b0 < 0 or b1 < 0 or c0 < 0 or c1 < 0 or b0 + c0 > nb or b1 + c1 > nb:
                        err("trk", "CELL %d batch range past BTCH (%d batches)" % (k, nb))
                        break
                    if i0 < 0 or ni < 0 or i0 + ni > n.get("INST", 0):
                        err("trk", "CELL %d instance range past INST" % k)
                        break
                if btch is not None and not len(btch) % 24:
                    cells = [struct.unpack_from("<i", btch, k * 24)[0] for k in range(nb)]
                    if cells and (min(cells) < 0 or max(cells) >= ncells):
                        err("trk", "BTCH cell index out of range (CELL has %d)" % ncells)
    d = t.get("COLG")
    if d is not None:
        end = _gridblock(d, 0, n.get("COLT"), "COLG", err)
        if end is not None and end != len(d):
            err("trk", "COLG size %d, GridBlock is %d bytes" % (len(d), end))
    d = t.get("WALL")
    if d is not None:
        if len(d) < 4:
            err("trk", "WALL size %d < 4" % len(d))
        else:
            nw = struct.unpack_from("<i", d, 0)[0]
            seg_end = 4 + WALLSEG_SIZE * nw
            if nw < 0 or seg_end > len(d):
                err("trk", "WALL: %d segments of %d B need %d bytes, chunk has %d" % (nw, WALLSEG_SIZE, seg_end, len(d)))
            else:
                for k in range(nw):
                    prv, nxt = struct.unpack_from("<2i", d, 4 + WALLSEG_SIZE * k + 48)
                    if not (-1 <= prv < nw and -1 <= nxt < nw):
                        err("trk", "WallSeg %d prev/next %d/%d out of range" % (k, prv, nxt))
                        break
                if seg_end < len(d):
                    end = _gridblock(d, seg_end, nw, "WALL", err)
                    if end is not None and end != len(d):
                        err("trk", "WALL size %d, segments + GridBlock are %d bytes" % (len(d), end))
    d = t.get("BOXS")
    if d is not None:
        if len(d) < 4:
            err("trk", "BOXS size %d < 4" % len(d))
        else:
            nbx = struct.unpack_from("<i", d, 0)[0]
            if len(d) != 4 + COLLBOX_SIZE * nbx:
                err("trk", "BOXS size %d != 4 + %d * numBoxes %d" % (len(d), COLLBOX_SIZE, nbx))
    d = t.get("CHKP")
    if d is not None:
        if len(d) < 12:
            err("trk", "CHKP size %d < 12" % len(d))
        else:
            ncp, ss, fs = struct.unpack_from("<3i", d, 0)
            if ncp < 0 or len(d) != 12 + 4 * ncp:
                err("trk", "CHKP size %d != 12 + 4 * numCp %d" % (len(d), ncp))
            elif "SLCE" in n:
                cps = struct.unpack_from("<%di" % ncp, d, 12)
                ns = n["SLCE"]
                if not (0 <= ss < ns and 0 <= fs < ns) or any(not 0 <= c < ns for c in cps):
                    err("trk", "CHKP slice index out of range (SLCE has %d slices)" % ns)
                elif cps and cps[-1] != fs:
                    err("trk", "CHKP last checkpoint %d is not the finish slice %d" % (cps[-1], fs))


def check_trk(path, data=None):
    P = []

    def err(code, msg):
        P.append(Problem(path, code, msg, "error"))

    def warn(code, msg):
        P.append(Problem(path, code, msg, "warn"))

    if data is None:
        with open(path, "rb") as f:
            data = f.read()
    try:
        t = export.read_trk(data)
    except ValueError as e:
        err("trk", str(e))
        return P
    for msg in t.problems:
        err("trk", msg)
    if t.version != export.TRK_VERSION:
        err("trk", "version %d (expected %d)" % (t.version, export.TRK_VERSION))
    ids = [c[0] for c in t.chunks]
    for cid in ids:
        if cid not in KNOWN_CHUNKS:
            warn("trk", "unknown chunk %r (readers skip it)" % cid)
    for cid in set(ids):
        if ids.count(cid) > 1:
            err("trk", "chunk %r appears %d times" % (cid, ids.count(cid)))
    if "INFO" not in ids:
        err("trk", "INFO chunk missing")
    _check_trk_records(t, err, warn)
    have = [cid in ids for cid in ("VPOL", "BTCH", "INDX")]
    if any(have) and not all(have):
        err("pools", "VPOL, BTCH and INDX must appear together")
    elif all(have):
        try:
            pd = export.decode_pools(t.get("VPOL"), t.get("BTCH"), t.get("INDX"))
            for msg in export.check_pools(pd):
                err("pools", msg)
        except ValueError as e:
            err("pools", str(e))
    return P


# ---------------------------------------------------------------------------------------------------- driver

CHECKED_EXT = (".msh", ".bmp", ".wav", ".trk")


def check_file(path, cls=None, tex_dirs=None, data=None):
    """Problems of one file by extension (.msh .bmp .wav .trk; others: none). For .msh, tex_dirs=None
    searches <file dir>/../tex."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".msh":
        if tex_dirs is None:
            tex_dirs = [os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(path))), "tex")]
        return check_msh(path, data, cls, tex_dirs)
    if ext == ".bmp":
        return check_bmp(path, data)
    if ext == ".wav":
        return check_wav(path, data)
    if ext == ".trk":
        return check_trk(path, data)
    return []


def expand(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                dirs.sort()
                for f in sorted(files):
                    if f.lower().endswith(CHECKED_EXT):
                        out.append(os.path.join(root, f))
        else:
            out.append(p)
    return out


def check_paths(paths, classes=None, tex_dirs=None, force_class=None):
    """Check files/dirs. classes: {path-suffix or stem: class} (from generator ASSETS declarations)."""
    probs = []
    classes = classes or {}
    for p in expand(paths):
        cls = force_class
        if cls is None:
            norm = p.replace(os.sep, "/")
            for k, v in classes.items():
                if norm.endswith(k) or _stem(p) == k:
                    cls = v
                    break
        extra = None
        if tex_dirs:
            extra = [os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(p))), "tex")] + list(tex_dirs)
        if not os.path.exists(p):
            probs.append(Problem(p, "format", "file not found"))
            continue
        probs.extend(check_file(p, cls, extra))
    return probs


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    force = None
    tex_dirs = []
    quiet = False
    files = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--class":
            force = argv[i + 1]
            i += 1
        elif a == "--tex-dir":
            tex_dirs.append(argv[i + 1])
            i += 1
        elif a in ("-q", "--quiet"):
            quiet = True
        elif a == "--list-classes":
            for k in sorted(BUDGETS):
                print("%-9s %s" % (k, BUDGETS[k]["lods"]))
            for pre, c in CLASS_PREFIXES:
                print("  prefix %-10s -> %s" % (pre, c))
            return 0
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            files.append(a)
        i += 1
    if not files:
        print("usage: python3 -m assetkit.validate [--class C] [--tex-dir D] [-q] files/dirs...")
        return 2
    paths = expand(files)
    probs = check_paths(paths, None, tex_dirs or None, force)
    for p in probs:
        if p.level == "error" or not quiet:
            print("%s: %s [%s] %s" % (p.path, p.level.upper(), p.code, p.msg))
    ne = len(errors(probs))
    print("validate: %d files, %d errors, %d warnings" % (len(paths), ne, len(probs) - ne))
    return min(ne, 255)


if __name__ == "__main__":
    sys.exit(main())
