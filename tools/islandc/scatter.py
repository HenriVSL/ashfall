"""scatter.py - A11 vegetation / rock / village scatter for the island -> data/island/veg.sct (src/scatter.cpp).

Sprites and their sizes come from gen_veg.py's table data/tex/veg_atlas.txt (name x y w h world_w world_h sink kind);
habitats from classes.py (material class, forest weight, conifer flag, moisture, field plots) and the height field.
island.txt keys:
  scatter NAME DENSITY   (*) per km2 of the sprite's habitat; hedge / wall: sprites per km of plot border;
                         houses: number of villages (5-9 house sprites each)
  scatter_range M        cell size of the file's grid (default 1280 m = one terrain chunk)
Habitats (per LOD0 quad, density x weight):
  grove   forest quads (not conifer) x1, forest-edge grass (forest weight 0.25-0.5) x0.35
  cedar   conifer forest quads (plantations, natural cedar stands) x1
  palms   coast: sand or grass within 260 m of the coast, below 30 m, slope < 14 deg x1; villages x0.6
  tree    open grass x1 (pasture, upland), field plots x0.5 + 30 % of plot borders, forest x0.15
  shrub   scrub / heath quads x1, open grass above 400 m x0.4, lowland outcrops x0.6
  rock    rock x1, ash x0.6, cooled lava x0.8, heath x0.25 (scale 0.7-2.6)
  hedge   lowland plot borders (field plots), wall: upland pasture borders + 30 % of lowland borders
Nothing within 350 m of the airbase layout (runway, taxiway, apron, building area) or under the climb-out.
Instance y = the LOD0 mesh height (SW-NE triangle rule = terrain_height), lowered on slopes by up to 0.6 x the
gradient over half the sprite width (no floating card edges). shade = baked terrain light of the quad x 200 (cast
shadow of the volcano included; 200 = flat sunlit ground).

veg.sct (e_file chunk container, tag RSCT, version 1, little-endian; chunk order INFO TYPE CELL INST):
  INFO  u32 version; u32 nTypes; u32 nCellX, nCellZ; f32 cell; f32 x0, z0 (south-west corner); u32 nInst;
        f32 maxSpriteH (largest world_h x max scale); u32 pad = 40 B
  TYPE  48 B per type: char name[16]; f32 u0 v0 u1 v1 (atlas UV, half-texel inset); f32 w h sink; u32 kind
        (0 = camera-facing card, 1 = fixed card along the instance yaw, double-sided)
  CELL  16 B per grid cell (row = z index south to north, column = x): u32 first, count; f32 yMin, yMax (instance
        base heights; yMax + maxSpriteH bounds the cell)
  INST  16 B per instance: f32 x, y, z; u8 type; u8 scale (x 1/100); u8 shade (x 1/200); u8 yaw (x 360/256 deg).
        Inside a cell the order is shuffled, so any prefix is an even thinning (the renderer's density LOD).
"""
import math
import struct

from assetkit import export as E
from assetkit import rng as R

import base as BASE
import classes as CL

VERSION = 1
TAG = b"RSCT"
_INFO = struct.Struct("<IIIIfffIfI")
_TYPE = struct.Struct("<16sfffffffI")
_CELL = struct.Struct("<IIff")
_INST = struct.Struct("<fffBBBB")
assert _INFO.size == 40 and _TYPE.size == 48 and _CELL.size == 16 and _INST.size == 16

DEFAULT_DENSITY = {"grove": 70.0, "cedar": 120.0, "palms": 60.0, "tree": 14.0, "shrub": 45.0, "rock": 30.0,
                   "hedge": 12.0, "wall": 10.0, "houses": 8.0}
SCALE = {"grove": (0.8, 1.25), "cedar": (0.85, 1.2), "palms": (0.75, 1.15), "tree": (0.7, 1.3), "shrub": (0.7, 1.4),
         "rock": (0.7, 2.6), "hedge": (0.9, 1.1), "wall": (1.0, 1.0), "houses": (0.85, 1.1)}


def read_types(kit):
    """veg_atlas.txt rows -> [dict]; None if gen_veg hasn't run."""
    p = kit.find("tex/veg_atlas.txt") if kit is not None else None
    if not p:
        return None
    out = []
    with open(p, "rb") as f:
        for line in f.read().decode("latin-1").splitlines():
            line = line.split("#", 1)[0].split()
            if len(line) != 9:
                continue
            x, y, w, h = [int(v) for v in line[1:5]]
            out.append({"name": line[0], "uv": ((x + 0.5) / 256.0, (y + 0.5) / 256.0, (x + w - 0.5) / 256.0,
                                               (y + h - 0.5) / 256.0),
                        "w": float(line[5]), "h": float(line[6]), "sink": float(line[7]), "kind": int(line[8])})
    return out


def mesh_height(hf, x, z):
    """LOD0 mesh height (SW-NE split), same rule as e_terrain terrain_height."""
    N, cell = hf.N, hf.cell
    fx = (x - hf.x0) / cell
    fz = (z - hf.z0) / cell
    i = max(0, min(N - 2, int(math.floor(fx))))
    j = max(0, min(N - 2, int(math.floor(fz))))
    u, v = fx - i, fz - j
    H = hf.H
    h00, h10, h01, h11 = H[j * N + i], H[j * N + i + 1], H[(j + 1) * N + i], H[(j + 1) * N + i + 1]
    if v >= u:
        return h00 + v * (h01 - h00) + u * (h11 - h01)
    return h00 + u * (h10 - h00) + v * (h11 - h10)


class Scatter(object):
    pass


def build(src, hf, cl, bk, kit):
    sc = Scatter()
    sc.notes = []
    types = read_types(kit)
    if not types:
        sc.data = None
        sc.notes.append("scatter: tex/veg_atlas.txt missing (run gen_veg): no veg.sct")
        return sc
    tix = dict((t["name"], i) for i, t in enumerate(types))
    dens = dict(DEFAULT_DENSITY)
    for v in src.rep("scatter"):
        if len(v) != 2 or v[0] not in tix:
            raise ValueError("scatter needs: sprite density (sprites: %s)" % " ".join(sorted(tix)))
        dens[v[0]] = v[1]
    seed = int(src.g1("seed"))
    rng = R.Rng(seed, "scatter")
    N, NQ, cell, H = hf.N, cl.NQ, hf.cell, hf.H
    qa = cell * cell / 1e6                                          # km2 per quad
    n_l, lt = bk.light[0]
    m = hf.model
    fr = hf.frame
    rects = BASE.layout(src)
    a_lo = min(r[1] for r in rects) - 350.0
    a_hi = max(r[2] for r in rects) + 350.0
    b_lo = min(r[3] for r in rects) - 350.0
    b_hi = max(r[4] for r in rects) + 350.0
    rwy_len = src.get("runway_size")[0]
    co_len, co_hw0, co_hw1, _ = src.get("climbout")

    def blocked(x, z):
        a, b = fr.to_local(x, z)
        if a_lo <= a <= a_hi and b_lo <= b <= b_hi:
            return True
        # climb-out corridor (takeoff toward +a beyond the runway end): keep it clear of any sprite
        e = rwy_len * 0.5
        if e < a < e + co_len:
            hw = co_hw0 + (co_hw1 - co_hw0) * (a - e) / co_len
            if abs(b) < hw:
                return True
        return False

    def shade_of(qi, qj):
        l0 = lt[qj * n_l + qi]
        l1 = lt[(qj + 1) * n_l + qi + 1]
        L = (l0[0] + l0[1] + l0[2] + l1[0] + l1[1] + l1[2]) / 6.0
        return max(0, min(255, int(L * 200.0 + 0.5)))

    insts = []

    def add(name, x, z, qi, qj, yaw=None, scale=None):
        if blocked(x, z):
            return
        t = types[tix[name]]
        if scale is None:
            lo, hi = SCALE[name]
            scale = rng.range(lo, hi)
        y = mesh_height(hf, x, z)
        if y < 0.6:
            return
        # sink on slopes: gradient over half the sprite width
        d = min(8.0, 0.5 * t["w"] * scale)
        g = max(abs(mesh_height(hf, x + d, z) - y), abs(mesh_height(hf, x - d, z) - y),
                abs(mesh_height(hf, x, z + d) - y), abs(mesh_height(hf, x, z - d) - y))
        y -= min(4.0, 0.6 * g)
        if yaw is None:
            yaw = rng.range(0.0, 360.0)
        insts.append((x, y, z, tix[name], max(1, min(255, int(scale * 100.0 + 0.5))), shade_of(qi, qj),
                      int(yaw * 256.0 / 360.0 + 0.5) & 255))

    def count(e):
        n = int(e)
        if rng.random() < e - n:
            n += 1
        return n

    mc, cls, fw, plot, ptyp, conif = cl.mcls, cl.cls, cl.forest, cl.plot, cl.ptype, cl.conifer
    hedge_e = dens["hedge"] * cell / 1000.0
    wall_e = dens["wall"] * cell / 1000.0
    for qj in range(1, NQ - 1):
        z0 = hf.z0 + qj * cell
        for qi in range(1, NQ - 1):
            k = qj * NQ + qi
            c = cls[k]
            if c == CL.WATER or c == CL.RUNWAY or c == CL.APRON:
                continue
            x0 = hf.x0 + qi * cell
            mk = mc[k]
            hc = 0.25 * (H[qj * N + qi] + H[qj * N + qi + 1] + H[(qj + 1) * N + qi] + H[(qj + 1) * N + qi + 1])
            dc = hf.DC[qj * N + qi]
            gx = (H[qj * N + qi + 1] - H[qj * N + qi]) / cell
            gz = (H[(qj + 1) * N + qi] - H[qj * N + qi]) / cell
            slope = math.degrees(math.atan(math.hypot(gx, gz)))
            w = {}
            if mk == CL.FOREST:
                w["cedar" if conif[k] else "grove"] = 1.0
                w["tree"] = 0.15
            elif mk == CL.SCRUB:
                if plot[k] < 0:
                    w["shrub"] = 1.0
                    w["rock"] = 0.25
                    w["tree"] = 0.1
            elif mk == CL.GRASS:
                if plot[k] < 0:
                    w["tree"] = 1.0
                    if 0.25 < fw[k] < 0.5:
                        w["grove"] = 0.35
                    if hc > 400.0:
                        w["shrub"] = 0.4
                else:
                    w["tree"] = 0.5                 # field trees
            elif mk == CL.ROCK:
                w["rock"] = 1.0
                if hc < 600.0:
                    w["shrub"] = 0.6
            elif mk == CL.ASH:
                w["rock"] = 0.6
            if dc > -260.0 and hc < 30.0 and slope < 14.0 and mk in (CL.SAND, CL.GRASS, CL.SCRUB):
                w["palms"] = 1.0
            for name in sorted(w):
                e = dens[name] * qa * w[name]
                for _ in range(count(e)):
                    add(name, x0 + rng.range(2.0, cell - 2.0), z0 + rng.range(2.0, cell - 2.0), qi, qj)
            # plot borders: hedge (lowland fields) or wall (upland pasture, some lowland): east and north edges
            p = plot[k]
            if p >= 0:
                upland = hc > src.get("fields")[0]
                for (ni, nj, yaw, ex, ez) in ((qi + 1, qj, 90.0, x0 + cell, None), (qi, qj + 1, 0.0, None, z0 + cell)):
                    if plot[nj * NQ + ni] == p:
                        continue
                    bh = CL.ihash(p, ni, nj)
                    kind = "wall" if (upland or (bh & 0xFF) < 77) else "hedge"
                    e = wall_e if kind == "wall" else hedge_e
                    n = count(e)
                    for s in range(n):
                        t = (s + 0.5 + rng.range(-0.2, 0.2)) / max(1, n)
                        if ex is not None:
                            add(kind, ex, z0 + t * cell, qi, qj, yaw=yaw + rng.range(-3, 3))
                        else:
                            add(kind, x0 + t * cell, ez, qi, qj, yaw=yaw + rng.range(-3, 3))
                    if (bh >> 8) % 100 < 30:
                        add("tree", (ex if ex is not None else x0 + rng.range(0, cell)),
                            (ez if ez is not None else z0 + rng.range(0, cell)), qi, qj)
    # villages: farm plots near the coast, spaced >= 3.2 km
    nvil = int(dens["houses"])
    cands = []
    for k in range(NQ * NQ):
        if plot[k] >= 0 and cls[k] != CL.WATER:
            qi, qj = k % NQ, k // NQ
            dc = hf.DC[qj * N + qi]
            if -1600.0 < dc < -350.0:
                cands.append((CL.ihash(seed, 99, k), qi, qj))
    cands.sort()
    vil = []
    for _, qi, qj in cands:
        x = hf.x0 + (qi + 0.5) * cell
        z = hf.z0 + (qj + 0.5) * cell
        if blocked(x, z) or any(math.hypot(x - vx, z - vz) < 3200.0 for vx, vz in vil):
            continue
        vil.append((x, z))
        if len(vil) >= nvil:
            break
    for vx, vz in vil:
        nh = 5 + int(rng.random() * 5)
        for h in range(nh):
            a = rng.range(0.0, math.pi * 2.0)
            rr = 40.0 + 200.0 * math.sqrt(rng.random())
            x, z = vx + math.cos(a) * rr, vz + math.sin(a) * rr
            qi, qj = int((x - hf.x0) / cell), int((z - hf.z0) / cell)
            if 0 <= qi < NQ and 0 <= qj < NQ and cls[qj * NQ + qi] not in (CL.WATER, CL.ROCK):
                add("houses", x, z, qi, qj)
        for h in range(4):
            a = rng.range(0.0, math.pi * 2.0)
            rr = 60.0 + 260.0 * math.sqrt(rng.random())
            x, z = vx + math.cos(a) * rr, vz + math.sin(a) * rr
            qi, qj = int((x - hf.x0) / cell), int((z - hf.z0) / cell)
            if 0 <= qi < NQ and 0 <= qj < NQ and cls[qj * NQ + qi] != CL.WATER:
                add("palms" if hf.DC[qj * N + qi] > -900.0 else "tree", x, z, qi, qj)
    sc.villages = vil

    # ---- grid cells, shuffled order per cell
    csz = src.g1("scatter_range")
    span = (N - 1) * cell
    ncx = int(math.ceil(span / csz))
    cells = [[] for _ in range(ncx * ncx)]
    for ins in insts:
        cx = max(0, min(ncx - 1, int((ins[0] - hf.x0) / csz)))
        cz = max(0, min(ncx - 1, int((ins[2] - hf.z0) / csz)))
        cells[cz * ncx + cx].append(ins)
    order = []
    crec = []
    for ci, lst in enumerate(cells):
        lst = sorted(lst)
        for i in range(len(lst) - 1, 0, -1):                          # Fisher-Yates, deterministic
            j = int(rng.random() * (i + 1))
            lst[i], lst[j] = lst[j], lst[i]
        first = len(order)
        order.extend(lst)
        ys = [v[1] for v in lst] or [0.0]
        crec.append((first, len(lst), min(ys), max(ys)))
    maxh = max(t["h"] * SCALE.get(t["name"], (1.0, 1.0))[1] for t in types)
    chunks = [
        ("INFO", _INFO.pack(VERSION, len(types), ncx, ncx, csz, hf.x0, hf.z0, len(order), maxh, 0)),
        ("TYPE", b"".join(_TYPE.pack(E.pack_name(t["name"], 16), t["uv"][0], t["uv"][1], t["uv"][2], t["uv"][3],
                                     t["w"], t["h"], t["sink"], t["kind"]) for t in types)),
        ("CELL", b"".join(_CELL.pack(*r) for r in crec)),
        ("INST", b"".join(_INST.pack(*v) for v in order)),
    ]
    data = E.trk_bytes(chunks, VERSION)
    sc.data = TAG + data[4:]
    sc.types = types
    sc.insts = order
    sc.cells = crec
    sc.ncx = ncx
    sc.csz = csz
    byt = [0] * len(types)
    for v in order:
        byt[v[3]] += 1
    sc.by_type = [(types[i]["name"], byt[i]) for i in range(len(types))]
    sc.max_cell = max(r[1] for r in crec)
    return sc


def report_lines(sc):
    if sc.data is None:
        return list(sc.notes)
    out = ["veg.sct bytes=%d instances=%d types=%d cells %dx%d of %.0f m (max %d per cell) villages %d" % (
        len(sc.data), len(sc.insts), len(sc.types), sc.ncx, sc.ncx, sc.csz, sc.max_cell, len(sc.villages))]
    out.append("by type: " + " ".join("%s %d" % t for t in sc.by_type))
    return out
