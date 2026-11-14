"""bake.py - baked terrain light (DESIGN 8.1): sun + sky ambient + curvature AO + height-field cast shadows,
using A2's light set (data/sky/light.txt: sunDir, sun, ambient, hemi).

Per channel c, for a vertex with normal n (central differences at the LOD's own spacing):
    L_c = (ambient_c * (1 - hemi + hemi * (n.y * 0.5 + 0.5)) * ao + sun_c * max(0, n.sunDir) * (1 - shadow * sh))
          / (ambient_c + sun_c * sunDir.y)
so flat sunlit ground is exactly 1.0 in every channel (= the colour targets of classes.py; A3's tints assume
that), sun-facing slopes go above 1 (clamped later at 255) and shadows turn ambient-blue.
ao = clamp(1 - ao_strength * laplacian / spacing, 0.72, 1.06) (valleys darker, crests slightly lighter).
sh = cast shadow 0..1 from a sweep along the sun direction: S(p) = max over the upstream column of
max(H, S) interpolated, minus the drop of the sun ray over one step; sh = smoothstep(0, 12 m, S - H). One pass in
the order away from the sun (O(N^2)). Coarse LODs use the box mean of sh over their spacing (summed-area table).
"""
import math

from height import smoothstep


class Bake(object):
    pass


def shadow_sweep(hf, sun):
    N = hf.N
    H = hf.H
    cell = hf.cell
    sx, sy, sz = sun
    hl = math.hypot(sx, sz)
    if hl < 1e-6 or sy <= 0.0:
        return [0.0] * (N * N)
    tan = sy / hl
    swap = abs(sz) > abs(sx)
    # generic axes: p = primary (dominant) axis, q = secondary
    if not swap:
        dp, dq = sx, sz
    else:
        dp, dq = sz, sx
    stepq = dq / abs(dp)                 # secondary offset (cells) per primary cell toward the sun
    sgn = 1 if dp > 0 else -1            # toward the sun along the primary axis
    drop = cell * math.sqrt(1.0 + stepq * stepq) * tan
    NEG = -1e9
    S = [NEG] * (N * N)
    order = range(N - 1, -1, -1) if sgn > 0 else range(N)

    def idx(p, q):
        return q * N + p if not swap else p * N + q
    for p in order:
        up = p + sgn
        if 0 <= up < N:
            Mrow = [max(H[idx(up, q)], S[idx(up, q)]) for q in range(N)]
            for q in range(N):
                fq = q + stepq
                q0 = int(math.floor(fq))
                t = fq - q0
                if q0 < 0 or q0 + 1 > N - 1:
                    if 0 <= q0 <= N - 1 and t < 1e-9:
                        v = Mrow[q0]
                    else:
                        continue
                else:
                    v = Mrow[q0] * (1.0 - t) + Mrow[q0 + 1] * t
                S[idx(p, q)] = v - drop
    out = [0.0] * (N * N)
    for k in range(N * N):
        d = S[k] - H[k]
        if d > 0.0:
            out[k] = smoothstep(0.0, 12.0, d)
    return out


def build(src, hf, light):
    N = hf.N
    H = hf.H
    cell = hf.cell
    bk = Bake()
    sun = tuple(light["sunDir"][:3])
    L = math.sqrt(sun[0] ** 2 + sun[1] ** 2 + sun[2] ** 2)
    sun = (sun[0] / L, sun[1] / L, sun[2] / L)
    bk.sun = sun
    sc = [v / 255.0 for v in light["sun"][:3]]
    am = [v / 255.0 for v in light["ambient"][:3]]
    hemi = light["hemi"][0]
    flat = [am[c] + sc[c] * sun[1] for c in range(3)]
    strength = src.g1("shadow")
    aok = src.g1("ao")
    sh = shadow_sweep(hf, sun)
    bk.shadow = sh
    bk.shadow_frac = sum(1 for k in range(N * N) if sh[k] > 0.5 and H[k] > 0.0) / float(max(1, hf.land_samples))
    # SAT of the shadow over samples
    W = N + 1
    sat = [0.0] * (W * W)
    for j in range(N):
        acc = 0.0
        for i in range(N):
            acc += sh[j * N + i]
            sat[(j + 1) * W + i + 1] = sat[j * W + i + 1] + acc

    def sh_box(I, J, r):
        i0, i1 = max(0, I - r), min(N - 1, I + r)
        j0, j1 = max(0, J - r), min(N - 1, J + r)
        tot = sat[(j1 + 1) * W + i1 + 1] - sat[j0 * W + i1 + 1] - sat[(j1 + 1) * W + i0] + sat[j0 * W + i0]
        return tot / float((i1 - i0 + 1) * (j1 - j0 + 1))

    bk.light = []
    for k in range(5):
        s = 1 << k
        n = (N - 1) // s + 1
        lt = [None] * (n * n)
        sp = s * cell
        for b in range(n):
            J = b * s
            for a in range(n):
                I = a * s
                h = H[J * N + I]
                hxp = H[J * N + min(N - 1, I + s)]
                hxm = H[J * N + max(0, I - s)]
                hzp = H[min(N - 1, J + s) * N + I]
                hzm = H[max(0, J - s) * N + I]
                dxs = (min(N - 1, I + s) - max(0, I - s)) * cell
                dzs = (min(N - 1, J + s) - max(0, J - s)) * cell
                nx = -(hxp - hxm) / dxs
                nz = -(hzp - hzm) / dzs
                ln = math.sqrt(nx * nx + 1.0 + nz * nz)
                nx, ny, nz = nx / ln, 1.0 / ln, nz / ln
                lap = hxp + hxm + hzp + hzm - 4.0 * h
                ao = 1.0 - aok * lap / sp
                ao = 0.72 if ao < 0.72 else (1.06 if ao > 1.06 else ao)
                shv = sh[J * N + I] if s == 1 else sh_box(I, J, s // 2)
                d = nx * sun[0] + ny * sun[1] + nz * sun[2]
                if d < 0.0:
                    d = 0.0
                sky = 1.0 - hemi + hemi * (ny * 0.5 + 0.5)
                f = (1.0 - strength * shv)
                lt[b * n + a] = tuple((am[c] * sky * ao + sc[c] * d * f) / flat[c] for c in range(3))
        bk.light.append((n, lt))
    return bk


def light_at(bk, k, I, J):
    n, lt = bk.light[k]
    s = 1 << k
    return lt[(J // s) * n + (I // s)]
