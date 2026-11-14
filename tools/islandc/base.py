"""base.py - the airbase (DESIGN 8.1, 8.5): layout in the runway frame, data/island/base.trk for e_world (RTRK,
assetkit containers/pools, racer trackc chunk layouts) and the BASE chunk data of jet.isl.

Runway frame (height.Frame): a = along the takeoff heading (from the threshold end a = -L/2 to the departure end
a = +L/2), b = to the right of it. Everything sits on the flat plateau (terrain exactly `plateau` m high).
  runway  |a| <= L/2, |b| <= W/2; ribbon textures from EACH end (A3): rwy_thr 0-50 m, rwy_end 50-100 m, runway
          after that, v = s / 50 m per 50 m segment (v = 0 at the end), u = 0..1 from the pilot's left edge; the
          frame flips at the runway middle (both ends read correctly on approach).
  taxiway b = taxi offset, width tw (texture taxi: u across, v = s / 25 m), from inset to inset of the ends;
          3 links (both ends, middle) runway edge -> taxiway edge (no overlaps: nothing coplanar overlaps)
  apron   beyond the taxiway (texture apron, 32 m slabs)
  marks   rwy_marks quads 0.07 m above the ribbon (ALPHATEST, zBias 3): designation digits, touchdown-zone bars,
          aiming points; holding positions on the links 75 m from the centre line (LOD0 only)
Ribbons 0.2 m above the plateau, zBias 2 (T02 probe; marks 3). Vertex colour = white (flat sunlit light = 1.0; textures carry colour).
Buildings: INST records of the A7 meshes (hangar tower tank radome) with yaw = heading + 90 + building yaw (front
-Z faces the runway at yaw 0); the BASE collision boxes are the meshes' AABBs (read from data/meshes, rotated).
A missing mesh gets a placeholder box batch (texture apron) in the cells and still an INST record.
base.trk chunks: INFO SKY TEXL MATL MSHL CELL VPOL BTCH INDX INST (no collision chunks: e_world logs
"no COLT"; collision is BASE in jet.isl). 128 m cells, LOD0 = ribbons + marks (+ boxes), LOD1 = ribbons (+ boxes).
"""
import math
import struct

from assetkit import export as E
from assetkit import mesh as M

import height as HT

WCELL = 128.0
RIB_Y = 0.2
MARK_Y = 0.27
DRAW_DIST = 6000.0
FALLBACK_SIZE = {"hangar": (40.0, 30.0, 14.0), "tower": (8.0, 8.0, 25.0), "tank": (14.0, 14.0, 11.6),
                 "radome": (12.0, 12.0, 16.2)}
MARKS_FALLBACK = {"bar": (2, 2, 16, 64), "hold": (2, 70, 128, 16)}
MARKS_FALLBACK.update(dict(("d%d" % i, (22 + 22 * i, 2, 18, 48)) for i in range(10)))
TEX = ["runway", "rwy_thr", "rwy_end", "taxi", "apron", "rwy_marks", "fence"]
# material name -> (texture index, blend, zBias)
MATS = [("runway", 0, 0, 2), ("rwy_thr", 1, 0, 2), ("rwy_end", 2, 0, 2), ("taxi", 3, 0, 2), ("apron", 4, 0, 2),
        ("marks", 5, M.RB_ALPHATEST, 3), ("box", 4, 0, 0),
        ("fence", 6, M.RB_ALPHATEST, 0)]      # A11: chain-link fence ribbons (both windings: double-sided)
PROP_DIST = 2500.0      # A11: `prop` instances (vehicles, masts, windsock) drawn within this distance
FENCE_H = 2.4           # A11: fence height (m), texture repeat 12 m along  # zBias 2/3: T02 probe decision (orchestrator 2026-11-13)
MAT_INDEX = dict((m[0], i) for i, m in enumerate(MATS))
MAT_TEX = dict((m[0], TEX[m[1]]) for m in MATS)


def layout(src):
    """[(kind, a0, a1, b0, b1)] paved areas in the runway frame."""
    L, W = src.get("runway_size")
    tx, tw, inset = src.get("taxiway")
    ac, al, ad = src.get("apron")
    out = [("runway", -L / 2, L / 2, -W / 2, W / 2),
           ("taxi", -L / 2 + inset, L / 2 - inset, tx - tw / 2, tx + tw / 2)]
    for a in (-L / 2 + inset + tw / 2, 0.0, L / 2 - inset - tw / 2):
        out.append(("link", a - tw / 2, a + tw / 2, W / 2, tx - tw / 2))
    out.append(("apron", ac - al / 2, ac + al / 2, tx + tw / 2, tx + tw / 2 + ad))
    return out


class Base(object):
    pass


def _read_marks(kit):
    p = kit.find("tex/rwy_marks.txt") if kit is not None else None
    out = dict(MARKS_FALLBACK)
    if p:
        with open(p, "rb") as f:
            for line in f.read().decode("latin-1").splitlines():
                line = line.split("#", 1)[0].split()
                if len(line) >= 5:
                    try:
                        out[line[0]] = tuple(int(v) for v in line[1:5])
                    except ValueError:
                        pass
    return out


def _mark_uv(r):
    x, y, w, h = r
    return ((x + 0.5) / 256.0, (y + 0.5) / 128.0, (x + w - 0.5) / 256.0, (y + h - 0.5) / 128.0)


def _quad(g, fr, pts, uvs, y, mat):
    """pts: 4 (a, b) corners in order around the quad, uvs: their UVs. Adds 2 triangles facing up."""
    vs = []
    for (a, b), uv in zip(pts, uvs):
        x, z = fr.to_world(a, b)
        vs.append(g.vert((x, y, z), (0.0, 1.0, 0.0), uv, (255, 255, 255, 255)))
    P = [g.P[v] for v in vs]
    nyv = (P[1][2] - P[0][2]) * (P[2][0] - P[0][0]) - (P[1][0] - P[0][0]) * (P[2][2] - P[0][2])
    if nyv < 0.0:
        vs = vs[::-1]
    g.tri(vs[0], vs[1], vs[2], mat)
    g.tri(vs[0], vs[2], vs[3], mat)


def designator(hdg):
    n = int(math.floor(hdg / 10.0 + 0.5)) % 36
    return "%02d" % (36 if n == 0 else n)


def build(src, hf, light, kit):
    bs = Base()
    fr = hf.frame
    L, W = src.get("runway_size")
    tx, tw, inset = src.get("taxiway")
    ac, al, ad = src.get("apron")
    ph = src.get("plateau")[0]
    y = ph + RIB_Y
    hd = fr.hd
    bs.notes = []
    rib = M.Geo()           # both LODs
    marks = M.Geo()         # LOD0 only
    # ---- runway: per 50 m segment, frame flips at the middle
    nseg = int(round(L / 50.0))
    for i in range(nseg):
        a0 = -L / 2 + 50.0 * i
        a1 = a0 + 50.0
        if a1 <= 1e-6:
            s0 = a0 + L / 2                       # from the threshold end
            flip = False
        else:
            s0 = L / 2 - a1                       # from the departure end
            flip = True
        mat = "rwy_thr" if s0 < 49.0 else ("rwy_end" if s0 < 99.0 else "runway")
        # corners: (a, b) and uv with u from the pilot's left (approach direction), v = 0 at the near end
        if not flip:
            pts = [(a0, -W / 2), (a1, -W / 2), (a1, W / 2), (a0, W / 2)]
            uvs = [(0.0, 0.0), (0.0, 1.0), (1.0, 1.0), (1.0, 0.0)]
        else:
            pts = [(a1, W / 2), (a0, W / 2), (a0, -W / 2), (a1, -W / 2)]
            uvs = [(0.0, 0.0), (0.0, 1.0), (1.0, 1.0), (1.0, 0.0)]
        _quad(rib, fr, pts, uvs, y, mat)
    # ---- taxiway (50 m segments, v 0..2)
    ta0, ta1 = -L / 2 + inset, L / 2 - inset
    n = int(math.ceil((ta1 - ta0) / 50.0))
    for i in range(n):
        a0 = ta0 + (ta1 - ta0) * i / n
        a1 = ta0 + (ta1 - ta0) * (i + 1) / n
        vlen = (a1 - a0) / 25.0
        _quad(rib, fr, [(a0, tx - tw / 2), (a1, tx - tw / 2), (a1, tx + tw / 2), (a0, tx + tw / 2)],
              [(0.0, 0.0), (0.0, vlen), (1.0, vlen), (1.0, 0.0)], y, "taxi")
    # ---- links (taxi texture turned: u across = along a, v along b)
    links = [r for r in layout(src) if r[0] == "link"]
    for _, a0, a1, b0, b1 in links:
        n = max(1, int(math.ceil((b1 - b0) / 50.0)))
        for i in range(n):
            c0 = b0 + (b1 - b0) * i / n
            c1 = b0 + (b1 - b0) * (i + 1) / n
            vlen = (c1 - c0) / 25.0
            _quad(rib, fr, [(a1, c0), (a1, c1), (a0, c1), (a0, c0)],
                  [(0.0, 0.0), (0.0, vlen), (1.0, vlen), (1.0, 0.0)], y, "taxi")
        # holding position 75 m from the centre line: 23 x 3 m, image top (dashed) toward the runway
        r = _read_marks(kit)["hold"]
        u0, v0, u1, v1 = _mark_uv(r)
        _quad(marks, fr, [(a1, 73.5), (a0, 73.5), (a0, 76.5), (a1, 76.5)],
              [(u0, v0), (u1, v0), (u1, v1), (u0, v1)], ph + MARK_Y, "marks")
    # ---- apron (50 m grid, uv from its corner / 32 m)
    a0, a1 = ac - al / 2, ac + al / 2
    b0, b1 = tx + tw / 2, tx + tw / 2 + ad
    na = max(1, int(math.ceil((a1 - a0) / 50.0)))
    nb = max(1, int(math.ceil((b1 - b0) / 50.0)))
    for i in range(na):
        for j in range(nb):
            aa0 = a0 + (a1 - a0) * i / na
            aa1 = a0 + (a1 - a0) * (i + 1) / na
            bb0 = b0 + (b1 - b0) * j / nb
            bb1 = b0 + (b1 - b0) * (j + 1) / nb
            pts = [(aa0, bb0), (aa1, bb0), (aa1, bb1), (aa0, bb1)]
            uvs = [((pa - a0) / 32.0, (pb - b0) / 32.0) for pa, pb in pts]
            _quad(rib, fr, pts, uvs, y, "apron")
    # ---- runway marks from each end
    mk = _read_marks(kit)
    des = (designator(hd), designator((hd + 180.0) % 360.0))
    bs.designators = des
    for end in (0, 1):
        sgn = 1.0 if end == 0 else -1.0
        aend = -L / 2 if end == 0 else L / 2

        def put(s0, s1, l0, l1, rect):
            # (s along the landing direction from this end, l to the pilot's right) -> runway frame
            u0, v0, u1, v1 = _mark_uv(rect)
            P = [(aend + sgn * s1, sgn * l0), (aend + sgn * s1, sgn * l1), (aend + sgn * s0, sgn * l1),
                 (aend + sgn * s0, sgn * l0)]
            UV = [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]          # image top = far side, left = pilot's left
            _quad(marks, fr, P, UV, ph + MARK_Y, "marks")
        dg = des[end]
        for k, ch in enumerate(dg):
            c = -2.1 if k == 0 else 2.1
            put(46.0, 55.6, c - 1.8, c + 1.8, mk["d" + ch])
        for st, cnt in ((150, 3), (300, 3), (450, 2), (600, 2), (750, 1), (900, 1)):
            for q in range(cnt):
                l0 = 9.0 + q * 3.3
                for side in (-1.0, 1.0):
                    a, b = sorted((side * l0, side * (l0 + 1.8)))
                    put(st, st + 22.5, a, b, mk["bar"])
        for side in (-1.0, 1.0):
            a, b = sorted((side * 9.0, side * 18.0))
            put(400.0, 445.0, a, b, mk["bar"])
    # ---- buildings
    insts = []
    boxes = []
    boxgeo = M.Geo()
    meshes = []
    pr = src.get("plateau_rect")
    for v in src.rep("building"):
        if len(v) != 4:
            raise ValueError("building needs: mesh along right yaw")
        name, ba, bb, byaw = v[0], v[1], v[2], v[3]
        x, z = fr.to_world(ba, bb)
        yaw = (hd + 90.0 + byaw) % 360.0
        p = kit.find("meshes/%s.msh" % name) if kit is not None else None
        if p:
            m = E.read_msh(p)
            lo, hi = m.box
        else:
            sx, sz, sy = FALLBACK_SIZE.get(name, (10.0, 10.0, 10.0))
            lo, hi = (-sx / 2, 0.0, -sz / 2), (sx / 2, sy, sz / 2)
            g = M.box(hi[0] - lo[0], hi[1], hi[2] - lo[2], pos=(0.0, 0.0, 0.0), mat="box", uv_scale=0.125,
                      skip=("-y",))
            g.set_color((205, 205, 200))
            g.rotate_y(yaw)
            g.translate(x, ph, z)
            boxgeo.merge(g)
            bs.notes.append("placeholder box for missing mesh %s" % name)
        cy, sy_ = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
        xs, zs = [], []
        for cxl in (lo[0], hi[0]):
            for czl in (lo[2], hi[2]):
                xs.append(x + cxl * cy + czl * sy_)
                zs.append(z - cxl * sy_ + czl * cy)
        box = (min(xs), ph + lo[1], min(zs), max(xs), ph + hi[1], max(zs))
        boxes.append((name, box))
        if name not in meshes:
            meshes.append(name)
        insts.append({"mesh": name, "x": x, "y": ph, "z": z, "yaw": math.radians(yaw), "box": box})
        if not (pr[0] <= ba <= pr[1] and pr[2] <= bb <= pr[3]):
            bs.notes.append("WARNING building %s at %.0f %.0f is off the plateau" % (name, ba, bb))
    # ---- A11 props: INST records without a collision box (vehicles, light masts, windsock)
    for v in src.rep("prop"):
        if len(v) != 4:
            raise ValueError("prop needs: mesh along right yaw")
        name, ba, bb, byaw = v[0], v[1], v[2], v[3]
        x, z = fr.to_world(ba, bb)
        yaw = (hd + 90.0 + byaw) % 360.0
        p = kit.find("meshes/%s.msh" % name) if kit is not None else None
        if not p:
            bs.notes.append("prop %s skipped: no mesh" % name)
            continue
        lo, hi = E.read_msh(p).box
        if name not in meshes:
            meshes.append(name)
        box = (x - 3.0, ph + lo[1], z - 3.0, x + 3.0, ph + hi[1], z + 3.0)
        insts.append({"mesh": name, "x": x, "y": ph, "z": z, "yaw": math.radians(yaw), "box": box,
                      "dist": PROP_DIST})
    # ---- A11 fences: runway-frame polylines -> double-sided keyed ribbons (LOD0 cells)
    fence = M.Geo()
    nf = 0.0
    for v in src.rep("fence"):
        if len(v) < 4 or len(v) % 2:
            raise ValueError("fence needs: a0 b0 a1 b1 [...] (runway frame)")
        pts = [fr.to_world(v[i], v[i + 1]) for i in range(0, len(v), 2)]
        u = 0.0
        for (xa, za), (xb, zb) in zip(pts[:-1], pts[1:]):
            seg = math.hypot(xb - xa, zb - za)
            ns = max(1, int(math.ceil(seg / 48.0)))
            for k in range(ns):
                x0 = xa + (xb - xa) * k / ns
                z0 = za + (zb - za) * k / ns
                x1 = xa + (xb - xa) * (k + 1) / ns
                z1 = za + (zb - za) * (k + 1) / ns
                ln = seg / ns
                y0 = max(ph, HT.sample(hf, x0, z0)) - 0.1
                y1 = max(ph, HT.sample(hf, x1, z1)) - 0.1
                u1 = u + ln / 12.0
                col = (255, 255, 255, 255)
                a = fence.vert((x0, y0, z0), uv=(u, 1.0), c=col)
                b = fence.vert((x0, y0 + FENCE_H, z0), uv=(u, 0.0), c=col)
                c = fence.vert((x1, y1 + FENCE_H, z1), uv=(u1, 0.0), c=col)
                d = fence.vert((x1, y1, z1), uv=(u1, 1.0), c=col)
                fence.tri(a, b, c, "fence")
                fence.tri(a, c, d, "fence")
                fence.tri(a, c, b, "fence")
                fence.tri(a, d, c, "fence")
                u = u1 - math.floor(u1)
                nf += ln
    if nf:
        bs.notes.append("fence %.0f m, %d tris (double-sided)" % (nf, fence.ntris))
        marks = M.merge(marks, fence)
    if len(boxes) > 32:
        raise ValueError("more than 32 buildings (TERRAIN_MAX_BOXES)")
    bs.boxes = boxes
    bs.insts = insts
    bs.meshes = meshes
    bs.rib, bs.marks, bs.boxgeo = rib, marks, boxgeo
    bs.mats = dict((mm[0], {"name": mm[0], "tex": MAT_TEX[mm[0]], "blend": mm[2]}) for mm in MATS)

    # ---- BASE chunk data
    bs.rwy = (fr.cx, fr.cz, hd, L, W, ph)
    tx_, tz_ = fr.to_world(-L / 2 + 40.0, 0.0)
    bs.thr = (tx_, y, tz_, hd)
    D, alt = src.get("air_spawns")
    cx, cz, _ = hf.centre
    bs.air = [(cx, alt, cz + D, 180.0), (cx + D, alt, cz, 270.0), (cx, alt, cz - D, 0.0), (cx - D, alt, cz, 90.0)]

    # ---- checks: climb-out corridor, coast after the runway end
    clen, w0, w1, hmax = src.get("climbout")
    worst = (-1e9, 0.0, 0.0)
    N = hf.N
    for j in range(N):
        zz = hf.z0 + j * hf.cell
        for i in range(N):
            xx = hf.x0 + i * hf.cell
            a, b = fr.to_local(xx, zz)
            if a < -L / 2 or a > L / 2 + clen:
                continue
            t = max(0.0, (a - L / 2) / clen)
            if abs(b) > w0 + (w1 - w0) * t:
                continue
            h = hf.H[j * N + i]
            if h > worst[0]:
                worst = (h, a, b)
    bs.climb_max = worst[0]
    bs.notes.append("climb-out corridor (runway + %.0f m, half width %.0f -> %.0f m): max terrain %.1f m at a %.0f "
                    "b %.0f (limit %.0f): %s" % (clen, w0, w1, worst[0], worst[1], worst[2], hmax,
                                                 "ok" if worst[0] < hmax else "VIOLATION"))
    sea = None
    for step in range(0, 6001, 10):
        xx, zz = fr.to_world(L / 2 + step, 0.0)
        if HT.sample(hf, xx, zz) < 0.0:
            sea = step
            break
    bs.notes.append("coast crossed %s m past the departure end (heading %.0f)" % (
        "%d" % sea if sea is not None else "> 6000", hd))
    side = None
    for step in range(0, 3001, 10):
        xx, zz = fr.to_world(0.0, -float(step))
        if HT.sample(hf, xx, zz) < 0.0:
            side = step
            break
    bs.notes.append("sea %s m left of the runway centre (b < 0 side)" % ("%d" % side if side is not None else "> 3000"))

    bs.trk = _trk(src, hf, light, bs, rib, marks, boxgeo, insts, meshes)
    return bs


def _cell(x, z):
    return (int(math.floor(x / WCELL)), int(math.floor(z / WCELL)))


def _split(g):
    out = {}
    for t, (a, b, c) in enumerate(g.T):
        pa, pb, pc = g.P[a], g.P[b], g.P[c]
        out.setdefault(_cell((pa[0] + pb[0] + pc[0]) / 3.0, (pa[2] + pb[2] + pc[2]) / 3.0), []).append(t)
    return out


def _sub(g, tris):
    sg = M.Geo()
    vmap = {}
    for t in tris:
        o = []
        for v in g.T[t]:
            j = vmap.get(v)
            if j is None:
                j = len(sg.P)
                vmap[v] = j
                sg.P.append(g.P[v])
                sg.N.append(g.N[v])
                sg.UV.append(g.UV[v])
                sg.UV2.append(g.UV2[v])
                sg.C.append(g.C[v])
            o.append(j)
        sg.T.append(tuple(o))
        sg.TM.append(g.TM[t])
        sg.TS.append(g.TS[t])
    return sg


def _name(s, n):
    return E.pack_name(s, n)


def _trk(src, hf, light, bs, rib, marks, boxgeo, insts, meshes):
    lod0 = M.merge(rib, marks, boxgeo) if boxgeo.ntris else M.merge(rib, marks)
    lod1 = M.merge(rib, boxgeo) if boxgeo.ntris else rib.copy()
    per = [_split(lod0), _split(lod1)]
    ikeys = {}
    for k, ins in enumerate(insts):
        ikeys.setdefault(_cell(ins["x"], ins["z"]), []).append(k)
    keys = sorted(set(list(per[0]) + list(per[1]) + list(ikeys)), key=lambda k: (k[1], k[0]))
    enc = E.PoolEncoder()
    cells = []
    inst_out = []
    blo = [1e30] * 3
    bhi = [-1e30] * 3
    for ci, key in enumerate(keys):
        lo = [1e30] * 3
        hi = [-1e30] * 3
        b0 = [0, 0]
        nb = [0, 0]
        for lod, g in ((0, lod0), (1, lod1)):
            tl = sorted(per[lod].get(key, []), key=lambda t: (MAT_INDEX[g.TM[t]], t))
            b0[lod] = len(enc.data.batches)
            if tl:
                sg = _sub(g, tl)
                for p in sg.P:
                    for q in range(3):
                        lo[q] = min(lo[q], p[q])
                        hi[q] = max(hi[q], p[q])
                bl = sg.batches(E.RFMT_V1)
                bl.sort(key=lambda b: MAT_INDEX[b[0]])
                for mk, vs, idx in bl:
                    enc.add_batch(ci, lod, MAT_INDEX[mk], E.RFMT_V1, vs, idx)
            nb[lod] = len(enc.data.batches) - b0[lod]
        il = ikeys.get(key, [])
        i0 = len(inst_out)
        for k in il:
            ins = insts[k]
            inst_out.append((ins, ci))
            bx = ins["box"]
            for q in range(3):
                lo[q] = min(lo[q], bx[q])
                hi[q] = max(hi[q], bx[q + 3])
        for q in range(3):
            blo[q] = min(blo[q], lo[q])
            bhi[q] = max(bhi[q], hi[q])
        cells.append((key[0], key[1], lo, hi, b0, nb, i0 if il else 0, len(il)))
    vpol, btch, indx = enc.encode()
    gx0 = min(k[0] for k in keys)
    gz0 = min(k[1] for k in keys)
    gw = max(k[0] for k in keys) - gx0 + 1
    gh = max(k[1] for k in keys) - gz0 + 1
    info = struct.pack("<32s48siif6f16si", _name("jet_base", 32), _name("Jet airbase", 48), 1, 0,
                       src.get("runway_size")[0], *(blo + bhi + [_name("light", 16), 0]))
    chunks = [("INFO", info)]
    if "zenith" in light and "vHorizon" in light:
        def rgb(k):
            v = light[k]
            return (int(v[0]) << 16) | (int(v[1]) << 8) | int(v[2])
        tiles = ["sky_hi_%d" % i for i in range(int(light.get("tiles", [8])[0]))][:8]
        t = b"".join(_name(x, 32) for x in tiles) + b"\0" * (32 * (8 - len(tiles)))
        sd = light["sunDir"]
        chunks.append(("SKY ", struct.pack("<256si3f3I3f2I2f", t, len(tiles), light["vHorizon"][0],
                                           light["elevLo"][0], light["elevHi"][0], rgb("zenith"), rgb("horizon"),
                                           rgb("fog"), sd[0], sd[1], sd[2], rgb("sun"), rgb("ambient"),
                                           light["fogStart"][0], light["fogEnd"][0])))
    chunks.append(("TEXL", E.pack_names(TEX, 32)))
    chunks.append(("MATL", b"".join(struct.pack("<ii4B4Bf", m[1], -1, m[2], 0, 0, 0, m[3], 0, 0, 0, 0.0)
                                    for m in MATS)))
    chunks.append(("MSHL", E.pack_names(meshes, 32)))
    cb = [struct.pack("<5i", gx0, gz0, gw, gh, len(cells))]
    for c in cells:
        cb.append(struct.pack("<2h6f6i", c[0], c[1], c[2][0], c[2][1], c[2][2], c[3][0], c[3][1], c[3][2],
                              c[4][0], c[4][1], c[5][0], c[5][1], c[6], c[7]))
    chunks.append(("CELL", b"".join(cb)))
    chunks += [("VPOL", vpol), ("BTCH", btch), ("INDX", indx)]
    midx = dict((n, i) for i, n in enumerate(meshes))
    chunks.append(("INST", b"".join(struct.pack("<i5fIifI", midx[ins["mesh"]], ins["x"], ins["y"], ins["z"],
                                                ins["yaw"], 1.0, 0xFFFFFFFF, ci, ins.get("dist", DRAW_DIST), 0)
                                    for ins, ci in inst_out)))
    bs.trk_stats = (len(cells), len(enc.data.batches), lod0.ntris, lod1.ntris, len(inst_out))
    bs.notes.append("base.trk: %d cells (128 m), %d batches, LOD0 %d tris, LOD1 %d tris, %d instances (%s)" % (
        bs.trk_stats + (" ".join(meshes),)))
    return E.trk_bytes(chunks)
