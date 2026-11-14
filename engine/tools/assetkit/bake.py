"""Bake 3D features into an atlas texture: rasterise mesh triangles in UV (texel) space and keep, per texel,
the interpolated 3D position, normal and a region name, so a generator can paint features (panel lines,
lights, louvres, window frames, moss on the north side, dirt near the ground...) by asking "where on the
model is this texel?". Generalised from A1's gen_car.py `rasterise` / `glass_distance`.

    from assetkit import bake as B
    bk = B.Baker(256, 256, ss=2)                         # 256x256 texture, baked at 2x (512x512 texels)
    bk.add_geo(body, clip=(0, 0, 256, 128), region="body", key=lambda p, n: p[0])   # UV * size = texel coords
    bk.add_tri([(10, 20), (60, 20), (10, 90)], [p0, p1, p2], [n0, n1, n2], keys=(z0, z1, z2), region="front")
    bk.dilate((0, 0, 256, 128), steps=6, region="body")  # grow into empty texels (filter / mip safety)
    img = bk.paint(lambda p, n, reg, ix, iy: (200, 30, 30) if p[1] > 0.8 else (90, 90, 90), bg=(0, 0, 0))
    # -> 256x256 RGB (box-filtered from the 2x bake); bk.pos / bk.nrm / bk.reg are flat lists (index iy*W+ix)

Coordinates: texel units of the FINAL texture (x right, y down; texture v = 0 is the top row, so texel
(u*w, v*h) for a Geo UV (u, v)). Rects are (x0, y0, w, h) in final texels. Internally everything is scaled by
`ss`: bk.W = w*ss, bk.H = h*ss, and sample (ix, iy) sits at hi-res texel centre (ix + 0.5, iy + 0.5).
Depth: where triangles overlap in texel space (mirrored halves sharing texels, front+back projections), the
larger interpolated key wins; keys=None = 0 for all (the first triangle drawn wins).
Deterministic (pure Python, no randomness).
"""

import math

from PIL import Image

from . import mesh as _mesh


class Baker(object):
    """Per-texel position / normal / region buffers for one atlas texture (see the module docstring).

    w, h: final texture size; ss: supersampling factor of the bake (paint() box-filters back down);
    overlap: triangles also cover texel centres up to this far outside them in barycentric units (0.02 closes
    hairline gaps between neighbouring triangles)."""

    def __init__(self, w, h, ss=1, overlap=0.02):
        self.w, self.h, self.ss = int(w), int(h), int(ss)
        self.W, self.H = self.w * self.ss, self.h * self.ss
        n = self.W * self.H
        self.pos = [None] * n          # (x, y, z) or None where no triangle covers the texel
        self.nrm = [None] * n          # unit normal or None
        self.reg = [None] * n          # region name given to add_tri/add_geo/dilate, or None
        self.dep = [-1e9] * n          # depth key of the stored sample
        self.overlap = overlap

    # ---- helpers
    def _rect(self, rect):
        if rect is None:
            return 0, 0, self.W, self.H
        s = self.ss
        return rect[0] * s, rect[1] * s, rect[2] * s, rect[3] * s

    def index(self, ix, iy):
        """Flat buffer index of hi-res sample (ix, iy)."""
        return iy * self.W + ix

    def texel(self, ix, iy):
        """Centre of hi-res sample (ix, iy) in FINAL texel units."""
        return ((ix + 0.5) / self.ss, (iy + 0.5) / self.ss)

    def covered(self, rect=None):
        """Number of hi-res samples with a position (in rect, default the whole texture)."""
        x0, y0, w, h = self._rect(rect)
        return sum(1 for iy in range(y0, y0 + h) for ix in range(x0, x0 + w) if self.pos[iy * self.W + ix] is not None)

    # ---- rasterising
    def add_tri(self, texels, ps, ns, keys=None, region=None, clip=None):
        """Rasterise one triangle. texels: 3 (x, y) in final texel units; ps: 3 positions; ns: 3 normals;
        keys: 3 depth keys (larger wins) or None; region: name stored per texel; clip: (x0, y0, w, h) final
        texels the triangle may write (default the whole texture). Returns the number of samples written."""
        SS = self.ss
        W = self.W
        x0, y0, w, h = self._rect(clip)
        pts = [(tc[0] * SS, tc[1] * SS) for tc in texels]
        if keys is None:
            keys = (0.0, 0.0, 0.0)
        (ax_, ay_), (bx, by), (cx, cy) = pts
        den = (by - cy) * (ax_ - cx) + (cx - bx) * (ay_ - cy)
        if abs(den) < 1e-12:
            return 0
        ix0 = max(x0, int(math.floor(min(ax_, bx, cx))))
        ix1 = min(x0 + w - 1, int(math.ceil(max(ax_, bx, cx))))
        iy0 = max(y0, int(math.floor(min(ay_, by, cy))))
        iy1 = min(y0 + h - 1, int(math.ceil(max(ay_, by, cy))))
        eps = -self.overlap
        pos, nrm, reg, dep = self.pos, self.nrm, self.reg, self.dep
        n = 0
        for iy in range(iy0, iy1 + 1):
            py = iy + 0.5
            for ix in range(ix0, ix1 + 1):
                px = ix + 0.5
                l0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / den
                l1 = ((cy - ay_) * (px - cx) + (ax_ - cx) * (py - cy)) / den
                l2 = 1.0 - l0 - l1
                if l0 < eps or l1 < eps or l2 < eps:
                    continue
                k = l0 * keys[0] + l1 * keys[1] + l2 * keys[2]
                i = iy * W + ix
                if k <= dep[i]:
                    continue
                dep[i] = k
                pos[i] = tuple(l0 * ps[0][j] + l1 * ps[1][j] + l2 * ps[2][j] for j in range(3))
                nrm[i] = _mesh.vnorm(tuple(l0 * ns[0][j] + l1 * ns[1][j] + l2 * ns[2][j] for j in range(3)))
                reg[i] = region
                n += 1
        return n

    def add_geo(self, geo, key=None, region=None, clip=None, tris=None):
        """Rasterise triangles of a Geo at their UVs (texel = (u * w, v * h)), with the Geo's positions and
        vertex normals. key: callable key(p, n) -> depth per vertex (None: first wins); region: name or
        callable region(t) per triangle; clip as in add_tri; tris: triangle indices (default all).
        Returns the number of samples written."""
        n = 0
        for t in (range(geo.ntris) if tris is None else tris):
            tri = geo.T[t]
            ps = [geo.P[v] for v in tri]
            ns = [geo.N[v] for v in tri]
            tcs = [(geo.UV[v][0] * self.w, geo.UV[v][1] * self.h) for v in tri]
            keys = None if key is None else [key(p, nn) for p, nn in zip(ps, ns)]
            rg = region(t) if callable(region) else region
            n += self.add_tri(tcs, ps, ns, keys, rg, clip)
        return n

    def dilate(self, rect=None, steps=6, region=None):
        """Grow the baked samples into empty texels inside rect (final texels; default whole texture), one
        4-neighbour ring per step (neighbour order +x, -x, +y, -y; the first one found is copied), so
        bilinear filtering and mips don't pull in background. region: name given to the filled texels
        (None: the neighbour's). Returns the number of texels filled."""
        x0, y0, w, h = self._rect(rect)
        W = self.W
        pos, nrm, reg = self.pos, self.nrm, self.reg
        total = 0
        for _ in range(steps):
            fill = []
            for iy in range(y0, y0 + h):
                for ix in range(x0, x0 + w):
                    i = iy * W + ix
                    if pos[i] is not None:
                        continue
                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        jx, jy = ix + dx, iy + dy
                        if x0 <= jx < x0 + w and y0 <= jy < y0 + h:
                            j = jy * W + jx
                            if pos[j] is not None:
                                fill.append((i, j))
                                break
            if not fill:
                break
            for i, j in fill:
                pos[i], nrm[i], reg[i] = pos[j], nrm[j], (region if region is not None else reg[j])
            total += len(fill)
        return total

    # ---- analysis + painting
    def edge_distance(self, mask, cap=4, rects=None):
        """Chebyshev distance (hi-res samples, capped at `cap`) from every masked sample to the nearest
        unmasked one or to the border of its rect: 0 outside the mask, 1 on the mask edge ... cap deep
        inside. mask: list of bools (len W*H) or callable(i) -> bool. rects: list of (x0, y0, w, h) final
        texels processed separately (default the whole texture). For trims and frames (window rubber,
        light bezels, panel edges)."""
        n = self.W * self.H
        gl = list(mask) if not callable(mask) else [bool(mask(i)) for i in range(n)]
        W = self.W
        dist = [0 if not g else cap for g in gl]
        for rect in (rects if rects is not None else [None]):
            x0, y0, w, h = self._rect(rect)
            for d in range(1, cap):
                for iy in range(y0, y0 + h):
                    for ix in range(x0, x0 + w):
                        i = iy * W + ix
                        if dist[i] <= d:
                            continue
                        near = False
                        for dy in (-1, 0, 1):
                            for dx in (-1, 0, 1):
                                jx, jy = ix + dx, iy + dy
                                if not (x0 <= jx < x0 + w and y0 <= jy < y0 + h) or dist[jy * W + jx] == d - 1:
                                    near = True
                        if near:
                            dist[i] = d
        return dist

    def paint(self, fn, bg=(0, 0, 0), rects=None, empty=False, downsample=True):
        """Paint the texture: fn(pos, nrm, region, ix, iy) -> (r, g, b) for every covered hi-res sample
        (empty=True: also uncovered ones, with pos/nrm None) inside rects (final texels; default all);
        other samples get bg. Returns an RGB Image of w x h (box-filtered from the bake) or, with
        downsample=False, of W x H."""
        W = self.W
        data = [tuple(bg[:3])] * (W * self.H)
        for rect in (rects if rects is not None else [None]):
            x0, y0, w, h = self._rect(rect)
            for iy in range(y0, y0 + h):
                for ix in range(x0, x0 + w):
                    i = iy * W + ix
                    p = self.pos[i]
                    if p is None and not empty:
                        continue
                    c = fn(p, self.nrm[i], self.reg[i], ix, iy)
                    data[i] = (int(c[0]), int(c[1]), int(c[2]))
        img = Image.new("RGB", (W, self.H))
        img.putdata(data)
        if downsample and self.ss > 1:
            img = img.resize((self.w, self.h), Image.BOX)
        return img
