"""chunks.py - terrain chunk meshes (DESIGN 8.2, 8.3, 8.6): 16 x 16-cell chunks (512 m at 32 m cells, 1280 m at
80 m), 5 LODs, per-class batches with chunk-local UVs, skirts on LOD 1-4, coast foam strips on LOD 0-2.

Stored chunks: max height over the chunk's samples > -59.5 m (land or shelf). All-underwater chunks (max < 0)
keep only LOD 2-4 and alias LOD0/1 to LOD2 (the 0.65 sea blend hides the shelf detail; saves ~40 % of the file).
Vertex = (lattice point, material): colour = classes.group_mean(water group if the vertex is below sea level, else
land; quads within +-2^k cells) / texture mean x 255 x bake light (clamped); UV = (x - chunkX0, z - chunkZ0) /
period of the material's texture.
Skirts: every border segment of LOD k >= 1 gets an outward-facing vertical quad down by
max(4 m, 0.15 x edge, measured mismatch of that chunk edge to LOD k-1 and k+1 + 1 m); skirt triangles are the
last ones of each batch and counted in BTCH.skirtTris (renderer may skip them when no neighbour changes LOD).
Foam: marching triangles over the LOD's own triangles -> 0 m contour polylines (water on the right), then a strip
of 3 rows (6 m inland at terrain + 0.25 m, the contour at 0.25 m, 34 m offshore at 0.25 m); v 0 / 0.15 / 1.
"""
import math

import classes as CL
import bake as BK

MATERIALS = [   # (texture, flags, tint key)  -> MATL; index = material id
    ("t_sand", 0, "sand"), ("t_grass", 0, "grass"), ("t_rock", 0, "rock"), ("t_ash", 0, "ash"),
    ("t_lava", 0, "lava"), ("foam", 3 | 0x200 | 0x800, None),
    ("t_forest", 0, "grass_forest"), ("t_scrub", 0, "scrub")]       # A11: 8 = TERRAIN_MAX_MATS (e_terrain.h)
CLASS_MAT = [0, 0, 1, 2, 3, 4, 1, 1, 6, 7]     # by material class (classes.mcls: + 8 forest, 9 scrub)
FOAM = 5
FOAM_IN, FOAM_OUT, FOAM_Y, FOAM_UREP = 6.0, 34.0, 0.25, 64.0


class Chunk(object):
    pass


class Chunks(object):
    pass


def argb(c):
    return 0xFF000000 | (c[0] << 16) | (c[1] << 8) | c[2]


def build(src, hf, cl, bk):
    N = hf.N
    NQ = N - 1
    H = hf.H
    cell = hf.cell
    periods = src.periods()
    ch = Chunks()
    ch.materials = []
    for tex, fl, tk in MATERIALS:
        t = cl.tints.get(tk, (255, 255, 255)) if tk else (255, 255, 255)
        ch.materials.append((tex, fl, argb(t), "%g" % periods[tex] if tex in periods else "foam 64 x 40 m"))
    means = cl.tex_means
    mat_mean = [means.get(t, (255.0, 255.0, 255.0)) for t, _, _ in MATERIALS]
    mat_per = [periods.get(t, 64.0) for t, _, _ in MATERIALS]
    ch.chunks = []
    ch.stats = [{"tris": 0, "terrain": 0, "skirt": 0, "foam": 0, "maxtris": 0, "verts": 0, "batches": 0}
                for _ in range(5)]
    ch.n_underwater = 0
    ch.max_uv = 0.0
    ch.max_uv_foam = 0.0
    ch.skirt_rng = [[1e9, 0.0] for _ in range(5)]
    ch.mismatch = [0.0] * 5
    ch.foam_segs = [0, 0, 0]
    colcache = {}

    def vcolour(k, I, J, mi):
        """Vertex colour of material mi at lattice (I, J) of LOD k; group (water/land) by the vertex height."""
        key = (k, I, J, mi)
        v = colcache.get(key)
        if v is None:
            s = 1 << k
            grp = 0 if H[J * N + I] < 0.0 else 1
            t = CL.group_mean(cl, grp, I, J, max(1, s >> 1))   # A11: half window (fields/forest edges survive LOD1-2)
            m = mat_mean[mi]
            lt = BK.light_at(bk, k, I, J)
            col = []
            for q in range(3):
                x = int(t[q] / m[q] * 255.0 * lt[q] + 0.5)
                col.append(0 if x < 0 else (255 if x > 255 else x))
            v = argb(col)
            colcache[key] = v
        return v

    def edge_profile(k, pts):
        """Heights of LOD k along a chunk edge given as 17 LOD0 lattice points."""
        s = 1 << k
        out = []
        for t in range(17):
            t0 = (t // s) * s
            t1 = min(16, t0 + s)
            a = H[pts[t0][1] * N + pts[t0][0]]
            b = H[pts[t1][1] * N + pts[t1][0]]
            f = 0.0 if t1 == t0 else (t - t0) / float(s)
            out.append(a + (b - a) * f)
        return out

    for cz in range(hf.NC):
        for cx in range(hf.NC):
            I0, J0 = cx * 16, cz * 16
            mx = max(H[(J0 + b) * N + I0 + a] for b in range(17) for a in range(17))
            if mx <= -59.5:
                continue
            c = Chunk()
            c.cx, c.cz = cx, cz
            c.underwater = mx < 0.0
            if c.underwater:
                ch.n_underwater += 1
            nland = sum(1 for b in range(16) for a in range(16) if cl.cls[(J0 + b) * NQ + I0 + a] != CL.WATER)
            c.land = nland / 256.0
            X0 = hf.x0 + I0 * cell
            Z0 = hf.z0 + J0 * cell
            # edges as LOD0 lattice point lists, walked with the outside on the right
            edges = {
                "S": [(I0 + t, J0) for t in range(17)],
                "E": [(I0 + 16, J0 + t) for t in range(17)],
                "N": [(I0 + 16 - t, J0 + 16) for t in range(17)],
                "W": [(I0, J0 + 16 - t) for t in range(17)]}
            prof = dict((e, [edge_profile(k, edges[e]) for k in range(5)]) for e in edges)
            mism = {}
            for e in edges:
                for k in range(5):
                    mm = 0.0
                    for j in range(max(0, k - 1), min(5, k + 2)):     # neighbours differ by <= 1 LOD
                        for t in range(17):
                            d = abs(prof[e][k][t] - prof[e][j][t])
                            if d > mm:
                                mm = d
                    mism[(e, k)] = mm
                    if mm > ch.mismatch[k]:
                        ch.mismatch[k] = mm
            c.lods = [None] * 5
            c.alias = [None] * 5
            c.tris = [0] * 5
            c.tris_ns = [0] * 5          # without skirts
            lo = [1e30, 1e30, 1e30]
            hi = [-1e30, -1e30, -1e30]
            for k in range(5):
                if c.underwater and k < 2:
                    c.alias[k] = 2
                    continue
                s = 1 << k
                n = 16 >> k
                nq = NQ >> k
                pyr = cl.pyr[k]
                verts = []
                vmap = {}
                tris = [[] for _ in MATERIALS]
                skirt_tris = [0] * len(MATERIALS)

                def vid(I, J, cc, ydrop=0.0, tag=0):
                    mi = CLASS_MAT[cc]
                    key = (I, J, mi, tag)
                    i = vmap.get(key)
                    if i is None:
                        i = len(verts)
                        vmap[key] = i
                        x = hf.x0 + I * cell
                        z = hf.z0 + J * cell
                        y = H[J * N + I] - ydrop
                        p = mat_per[mi]
                        u = (x - X0) / p
                        v = (z - Z0) / p
                        verts.append((x, y, z, vcolour(k, I, J, mi), u, v))
                    return i
                qcls = {}
                for b in range(n):
                    for a in range(n):
                        qi = (I0 >> k) + a
                        qj = (J0 >> k) + b
                        cc = pyr[qj * nq + qi]
                        qcls[(a, b)] = cc
                        Ia, Ja = I0 + a * s, J0 + b * s
                        A = vid(Ia, Ja, cc)
                        B = vid(Ia + s, Ja, cc)
                        C = vid(Ia + s, Ja + s, cc)
                        D = vid(Ia, Ja + s, cc)
                        tl = tris[CLASS_MAT[cc]]
                        tl.extend((A, D, C, A, C, B))
                nterr = n * n * 2
                nskirt = 0
                if k >= 1:
                    for e in ("S", "E", "N", "W"):
                        depth = max(4.0, 0.15 * cell * s, mism[(e, k)] + 1.0)
                        rg = ch.skirt_rng[k]
                        rg[0] = min(rg[0], depth)
                        rg[1] = max(rg[1], depth)
                        pts = edges[e][::s]
                        for t in range(n):
                            p, q = pts[t], pts[t + 1]
                            # adjacent quad inside the chunk
                            if e == "S":
                                qa, qb = t, 0
                            elif e == "E":
                                qa, qb = n - 1, t
                            elif e == "N":
                                qa, qb = n - 1 - t, n - 1
                            else:
                                qa, qb = 0, n - 1 - t
                            cc = qcls[(qa, qb)]
                            pt = vid(p[0], p[1], cc)
                            qt = vid(q[0], q[1], cc)
                            pb = vid(p[0], p[1], cc, depth, 1)
                            qb_ = vid(q[0], q[1], cc, depth, 1)
                            tris[CLASS_MAT[cc]].extend((pb, pt, qt, pb, qt, qb_))
                            skirt_tris[CLASS_MAT[cc]] += 2
                            nskirt += 2
                nfoam = 0
                if k <= 2 and mx > 0.0 and min(H[(J0 + b) * N + I0 + a] for b in range(17) for a in range(17)) < 0.0:
                    nfoam, nseg = foam_strip(hf, I0, J0, k, verts, tris[FOAM])
                    ch.foam_segs[k] += nseg
                batches = [(mi, tl, skirt_tris[mi]) for mi, tl in enumerate(tris) if tl]
                c.lods[k] = (verts, batches)
                c.tris[k] = nterr + nskirt + nfoam
                c.tris_ns[k] = nterr + nfoam
                st = ch.stats[k]
                st["terrain"] += nterr
                st["skirt"] += nskirt
                st["foam"] += nfoam
                st["tris"] += nterr + nskirt + nfoam
                st["maxtris"] = max(st["maxtris"], nterr + nskirt + nfoam)
                st["verts"] += len(verts)
                st["batches"] += len(batches)
                if len(verts) > 65535:
                    raise RuntimeError("chunk %d,%d LOD%d has %d vertices" % (cx, cz, k, len(verts)))
                foamset = set(tris[FOAM])
                for vi, v in enumerate(verts):
                    for q in range(3):
                        if v[q] < lo[q]:
                            lo[q] = v[q]
                        if v[q] > hi[q]:
                            hi[q] = v[q]
                    m = max(abs(v[4]), abs(v[5]))
                    if vi in foamset:
                        ch.max_uv_foam = max(ch.max_uv_foam, m)
                    else:
                        ch.max_uv = max(ch.max_uv, m)
            for k in range(5):
                if c.alias[k] is not None:
                    c.tris[k] = c.tris[c.alias[k]]
                    c.tris_ns[k] = c.tris_ns[c.alias[k]]
            c.aabb = tuple(lo) + tuple(hi)
            ch.chunks.append(c)
    ch.nverts = sum(len(c.lods[k][0]) for c in ch.chunks for k in range(5) if c.alias[k] is None)
    ch.nidx = sum(len(tl) for c in ch.chunks for k in range(5) if c.alias[k] is None for _, tl, _s in c.lods[k][1])
    ch.nbatches = sum(len(c.lods[k][1]) for c in ch.chunks for k in range(5) if c.alias[k] is None)
    for r in ch.skirt_rng:
        if r[0] > r[1]:
            r[0] = r[1] = 0.0
    return ch


def foam_strip(hf, I0, J0, k, verts, out_idx):
    """Coast foam for one chunk-LOD: appends vertices to verts and triangles to out_idx. -> (tris, segments)."""
    N = hf.N
    H = hf.H
    cell = hf.cell
    s = 1 << k
    n = 16 >> k
    X0 = hf.x0 + I0 * cell
    Z0 = hf.z0 + J0 * cell

    def P(I, J):
        return (hf.x0 + I * cell, H[J * N + I], hf.z0 + J * cell)
    segs = []           # (key_a, pa, key_b, pb, grad)
    for b in range(n):
        for a in range(n):
            Ia, Ja = I0 + a * s, J0 + b * s
            A = (Ia, Ja)
            B = (Ia + s, Ja)
            C = (Ia + s, Ja + s)
            D = (Ia, Ja + s)
            for tri in ((A, D, C), (A, C, B)):
                ps = [P(*t) for t in tri]
                land = [p[1] > 0.0 for p in ps]
                if all(land) or not any(land):
                    continue
                cross = []
                for e in range(3):
                    i, j = e, (e + 1) % 3
                    if land[i] != land[j]:
                        p, q = ps[i], ps[j]
                        f = p[1] / (p[1] - q[1])
                        key = tuple(sorted((tri[i], tri[j])))
                        cross.append((key, (p[0] + (q[0] - p[0]) * f, p[2] + (q[2] - p[2]) * f)))
                if len(cross) != 2:
                    continue
                # gradient of the triangle plane
                p0, p1, p2 = ps
                ux, uy, uz = p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]
                vx, vy, vz = p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]
                nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
                grad = math.hypot(nx, nz) / abs(ny) if abs(ny) > 1e-9 else 10.0
                (ka, pa), (kb, pb) = cross
                # water on the right of a -> b
                wi = land.index(False)
                w = ps[wi]
                dx, dz = pb[0] - pa[0], pb[1] - pa[1]
                if (w[0] - pa[0]) * dz + (w[2] - pa[1]) * (-dx) < 0.0:
                    ka, pa, kb, pb = kb, pb, ka, pa
                if abs(pa[0] - pb[0]) + abs(pa[1] - pb[1]) < 1e-6:
                    continue
                segs.append((ka, pa, kb, pb, grad))
    if not segs:
        return 0, 0
    start = {}
    ends = set()
    for i, sg in enumerate(segs):
        start.setdefault(sg[0], i)
        ends.add(sg[2])
    used = [False] * len(segs)
    lines = []
    order = [i for i, sg in enumerate(segs) if sg[0] not in ends] + list(range(len(segs)))
    for i0 in order:
        if used[i0]:
            continue
        line = []
        i = i0
        while i is not None and not used[i]:
            used[i] = True
            line.append(segs[i])
            i = start.get(segs[i][2])
        lines.append(line)
    ntri = 0
    for line in lines:
        pts = [line[0][1]] + [sg[3] for sg in line]
        grads = [line[0][4]] + [sg[4] for sg in line]
        closed = line[-1][2] == line[0][0]
        segn = []
        for sg in line:
            dx, dz = sg[3][0] - sg[1][0], sg[3][1] - sg[1][1]
            L = math.hypot(dx, dz)
            segn.append((dz / L, -dx / L, L))          # right normal (seaward), length
        cols = []
        u = 0.0
        for i, p in enumerate(pts):
            if i == 0:
                na = segn[-1] if closed else segn[0]
                nb = segn[0]
            elif i == len(pts) - 1:
                na = segn[-1]
                nb = segn[0] if closed else segn[-1]
            else:
                na, nb = segn[i - 1], segn[i]
            mxn, mzn = na[0] + nb[0], na[1] + nb[1]
            L = math.hypot(mxn, mzn)
            if L < 1e-6:
                mxn, mzn = nb[0], nb[1]
            else:
                mxn, mzn = mxn / L, mzn / L
            cosa = max(0.5, mxn * nb[0] + mzn * nb[1])
            g = grads[i] if i < len(grads) else grads[-1]
            if i > 0:
                u += segn[i - 1][2] / FOAM_UREP
            cols.append((p, mxn / cosa, mzn / cosa, min(6.0, g * FOAM_IN), u))
        shift = 0.0
        prev_base = -1
        for (p, nx, nz, rise, uu) in cols:
            x, z = p
            rows = ((-FOAM_IN, FOAM_Y + rise, 0.0), (0.0, FOAM_Y, 0.15), (FOAM_OUT, FOAM_Y, 1.0))
            base = len(verts)
            for off, y, vv in rows:
                verts.append((x + nx * off, y, z + nz * off, 0xFFFFFFFF, uu - shift, vv))
            if prev_base >= 0:
                for r in range(2):
                    a0, a1 = prev_base + r, prev_base + r + 1      # previous column: inland-side row, sea-side row
                    b0, b1 = base + r, base + r + 1
                    out_idx.extend((a0, b0, b1, a0, b1, a1))
                    ntri += 2
            prev_base = base
            if uu - shift >= 4.0:                                 # keep u small: restart at an integer offset
                shift = math.floor(uu)
                prev_base = len(verts)
                for off, y, vv in rows:
                    verts.append((x + nx * off, y, z + nz * off, 0xFFFFFFFF, uu - shift, vv))
    return ntri, len(segs)
