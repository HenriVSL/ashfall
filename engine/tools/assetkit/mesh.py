"""Meshes: Geo (indexed triangles with attributes), primitives, normals/UV tools, pre-lighting, LOD helpers,
MeshBuilder (materials, LODs, tags -> .msh).

Conventions (DESIGN 2.3): metres; left-handed +X right/east, +Y up, +Z forward/north; triangles CLOCKWISE seen
from the front = front face. With the plain cross product, the outward face normal of triangle (a, b, c) is
cross(b - a, c - a). Texture v = 0 is the TOP row of the image.

    from assetkit import mesh as M
    mb = M.MeshBuilder("barn_red")
    wall = mb.material("wall", tex="wall_red_board")
    mb.lod(120.0)
    mb.add(M.box(8, 5, 12, mat=wall, uv_scale=0.25))
    mb.save("data/meshes/barn_red.msh")
"""

import heapq
import math

from . import export
from . import palette
from .export import (RB_OPAQUE, RB_ALPHATEST, RB_BLEND, RB_ADD, RB_SHADOW, RS1_NONE, RS1_DETAIL, RS1_ENVADD,
                     MM_TINT, MM_ENV, MM_WHEEL, MM_GLASS, MM_LIGHT, RFMT_V1, RFMT_V2, SURF_NONE, SURF_ASPHALT,
                     SURF_PAINT, SURF_KERB, SURF_GRAVEL, SURF_GRASS, SURF_DIRT, SURF_WATER, MSH_NO_LIMIT, f32)

WHITE = (255, 255, 255, 255)

# ---------------------------------------------------------------------------------------------------- vectors


def vadd(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def vsub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def vscale(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def vdot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def vcross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def vlen(a):
    return math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])


def vnorm(a, fallback=(0.0, 1.0, 0.0)):
    l = vlen(a)
    if l < 1e-12:
        return fallback
    return (a[0] / l, a[1] / l, a[2] / l)


def vlerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def face_normal(a, b, c):
    """Unit outward normal of a clockwise-front triangle."""
    return vnorm(vcross(vsub(b, a), vsub(c, a)))


def _qk(v, q):
    return tuple(int(round(x * q)) for x in v)


def _smoothstep(e0, e1, x):
    if e1 <= e0:
        return 1.0 if x >= e1 else 0.0
    t = (x - e0) / (e1 - e0)
    t = 0.0 if t < 0 else (1.0 if t > 1 else t)
    return t * t * (3.0 - 2.0 * t)


# ---------------------------------------------------------------------------------------------------- material

class Material(object):
    """A mesh material (one MeshMatFile record). Create it with MeshBuilder.material(); Geo triangles refer to
    a Material object or to its name string."""

    def __init__(self, name, tex, tex1="", blend=RB_OPAQUE, stage1=RS1_NONE, cull_none=False, flags=0):
        if len(tex) > 31 or len(tex1) > 31:
            raise ValueError("texture names must be at most 31 characters: %r %r" % (tex, tex1))
        self.name = name
        self.tex = tex
        self.tex1 = tex1
        self.blend = blend
        self.stage1 = stage1
        self.cull_none = bool(cull_none)
        self.flags = flags
        self.index = -1

    def record(self):
        return {"tex": self.tex, "tex1": self.tex1, "blend": self.blend, "stage1": self.stage1,
                "cullNone": 1 if self.cull_none else 0, "flags": self.flags}

    def __repr__(self):
        return "Material(%r, tex=%r)" % (self.name, self.tex)


def mat_key(m):
    """Name used to identify a material reference (Material -> .name, str -> itself, None -> None)."""
    if isinstance(m, Material):
        return m.name
    return m


# ---------------------------------------------------------------------------------------------------- Geo

class Geo(object):
    """Indexed triangle geometry. Per vertex: P position, N normal, UV, UV2 (2nd texture set for RVert2),
    C colour (r, g, b, a ints). Per triangle: T (i0, i1, i2) clockwise-front, TM material (Material or name),
    TS surface id (SURF_*, used by trackc for collision). All methods that change the geometry return self,
    so calls chain: box(...).translate(0, 1, 0).rotate_y(30)."""

    def __init__(self, mat=None):
        self.P = []
        self.N = []
        self.UV = []
        self.UV2 = []
        self.C = []
        self.T = []
        self.TM = []
        self.TS = []
        self.mat = mat

    # ---- building
    def vert(self, p, n=(0.0, 1.0, 0.0), uv=(0.0, 0.0), c=WHITE, uv2=(0.0, 0.0)):
        """Append a vertex, return its index."""
        self.P.append((float(p[0]), float(p[1]), float(p[2])))
        self.N.append((float(n[0]), float(n[1]), float(n[2])))
        self.UV.append((float(uv[0]), float(uv[1])))
        self.UV2.append((float(uv2[0]), float(uv2[1])))
        self.C.append(tuple(c) if len(c) == 4 else (c[0], c[1], c[2], 255))
        return len(self.P) - 1

    def tri(self, a, b, c, mat=None, surf=0):
        """Append triangle (a, b, c) = vertex indices in CLOCKWISE order seen from the front."""
        self.T.append((a, b, c))
        self.TM.append(mat if mat is not None else self.mat)
        self.TS.append(surf)
        return self

    def quad_idx(self, a, b, c, d, mat=None, surf=0):
        """Two triangles for vertex indices a, b, c, d in clockwise order seen from the front."""
        self.tri(a, b, c, mat, surf)
        self.tri(a, c, d, mat, surf)
        return self

    @property
    def nverts(self):
        return len(self.P)

    @property
    def ntris(self):
        return len(self.T)

    def copy(self):
        g = Geo(self.mat)
        g.P = list(self.P)
        g.N = list(self.N)
        g.UV = list(self.UV)
        g.UV2 = list(self.UV2)
        g.C = list(self.C)
        g.T = list(self.T)
        g.TM = list(self.TM)
        g.TS = list(self.TS)
        return g

    def merge(self, *others):
        """Append the geometry of other Geos (materials and surfaces kept)."""
        for o in others:
            base = len(self.P)
            self.P.extend(o.P)
            self.N.extend(o.N)
            self.UV.extend(o.UV)
            self.UV2.extend(o.UV2)
            self.C.extend(o.C)
            self.T.extend((a + base, b + base, c + base) for a, b, c in o.T)
            self.TM.extend(o.TM)
            self.TS.extend(o.TS)
        return self

    # ---- transforms
    def _xform(self, fp, fn, flip):
        self.P = [fp(p) for p in self.P]
        self.N = [vnorm(fn(n)) for n in self.N]
        if flip:
            self.T = [(a, c, b) for a, b, c in self.T]
        return self

    def translate(self, x=0.0, y=0.0, z=0.0):
        if isinstance(x, (tuple, list)):
            x, y, z = x
        self.P = [(p[0] + x, p[1] + y, p[2] + z) for p in self.P]
        return self

    def scale(self, sx, sy=None, sz=None):
        """Scale (uniform with one argument). A negative determinant flips the winding to stay outward."""
        if sy is None:
            sy = sx
        if sz is None:
            sz = sx
        return self._xform(lambda p: (p[0] * sx, p[1] * sy, p[2] * sz),
                           lambda n: (n[0] / sx, n[1] / sy, n[2] / sz), sx * sy * sz < 0)

    def rotate_y(self, deg):
        """Yaw: positive turns +Z toward +X (clockwise seen from above), like D3DXMatrixRotationY."""
        c = math.cos(math.radians(deg))
        s = math.sin(math.radians(deg))
        f = lambda p: (p[0] * c + p[2] * s, p[1], -p[0] * s + p[2] * c)
        return self._xform(f, f, False)

    def rotate_x(self, deg):
        """Positive turns +Y toward +Z (pitches +Z down), like D3DXMatrixRotationX."""
        c = math.cos(math.radians(deg))
        s = math.sin(math.radians(deg))
        f = lambda p: (p[0], p[1] * c - p[2] * s, p[1] * s + p[2] * c)
        return self._xform(f, f, False)

    def rotate_z(self, deg):
        """Positive turns +X toward +Y, like D3DXMatrixRotationZ."""
        c = math.cos(math.radians(deg))
        s = math.sin(math.radians(deg))
        f = lambda p: (p[0] * c - p[1] * s, p[0] * s + p[1] * c, p[2])
        return self._xform(f, f, False)

    def matrix(self, m):
        """Apply a row-vector 4x3 (or 4x4) matrix m (rows 0-2 basis, row 3 translation): p' = p * M."""
        r0, r1, r2, t = m[0], m[1], m[2], m[3]

        def fp(p):
            return (p[0] * r0[0] + p[1] * r1[0] + p[2] * r2[0] + t[0],
                    p[0] * r0[1] + p[1] * r1[1] + p[2] * r2[1] + t[1],
                    p[0] * r0[2] + p[1] * r1[2] + p[2] * r2[2] + t[2])
        a = [[r0[0], r0[1], r0[2]], [r1[0], r1[1], r1[2]], [r2[0], r2[1], r2[2]]]
        det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
        # cofactor matrix = det * inverse transpose; row-vector normals: n' = n * cof (scale doesn't matter)
        cof = [[a[1][1] * a[2][2] - a[1][2] * a[2][1], -(a[1][0] * a[2][2] - a[1][2] * a[2][0]), a[1][0] * a[2][1] - a[1][1] * a[2][0]],
               [-(a[0][1] * a[2][2] - a[0][2] * a[2][1]), a[0][0] * a[2][2] - a[0][2] * a[2][0], -(a[0][0] * a[2][1] - a[0][1] * a[2][0])],
               [a[0][1] * a[1][2] - a[0][2] * a[1][1], -(a[0][0] * a[1][2] - a[0][2] * a[1][0]), a[0][0] * a[1][1] - a[0][1] * a[1][0]]]
        sg = 1.0 if det >= 0 else -1.0

        def fn(n):
            return (sg * (n[0] * cof[0][0] + n[1] * cof[1][0] + n[2] * cof[2][0]),
                    sg * (n[0] * cof[0][1] + n[1] * cof[1][1] + n[2] * cof[2][1]),
                    sg * (n[0] * cof[0][2] + n[1] * cof[1][2] + n[2] * cof[2][2]))
        return self._xform(fp, fn, det < 0)

    def place(self, pos=(0.0, 0.0, 0.0), yaw=0.0, scale=1.0):
        """Scale, then yaw, then translate (instance-style placement)."""
        if scale != 1.0:
            self.scale(scale)
        if yaw:
            self.rotate_y(yaw)
        return self.translate(pos[0], pos[1], pos[2])

    def mirror(self, axis="x"):
        """Mirror in place across the plane axis = 0 (winding flipped so faces stay outward)."""
        k = "xyz".index(axis)

        def f(p):
            q = list(p)
            q[k] = -q[k]
            return tuple(q)
        return self._xform(f, f, True)

    def mirrored(self, axis="x"):
        """Mirrored copy (see mirror)."""
        return self.copy().mirror(axis)

    def flip(self):
        """Reverse the winding and the normals (turn faces inside out)."""
        self.T = [(a, c, b) for a, b, c in self.T]
        self.N = [(-n[0], -n[1], -n[2]) for n in self.N]
        return self

    # ---- topology
    def weld(self, eps=1e-5, attrs=True):
        """Merge vertices with the same position (within eps) and, with attrs=True, the same normal, UVs and
        colour. attrs=False merges by position only (keeps the first vertex's attributes). Degenerate
        triangles (repeated index) are dropped."""
        q = 1.0 / eps
        keys = {}
        remap = []
        P, N, UV, UV2, C = [], [], [], [], []
        for i in range(len(self.P)):
            if attrs:
                k = (_qk(self.P[i], q), _qk(self.N[i], 1e4), _qk(self.UV[i], 1e5), _qk(self.UV2[i], 1e5), self.C[i])
            else:
                k = _qk(self.P[i], q)
            j = keys.get(k)
            if j is None:
                j = len(P)
                keys[k] = j
                P.append(self.P[i])
                N.append(self.N[i])
                UV.append(self.UV[i])
                UV2.append(self.UV2[i])
                C.append(self.C[i])
            remap.append(j)
        T, TM, TS = [], [], []
        for t, (a, b, c) in enumerate(self.T):
            a, b, c = remap[a], remap[b], remap[c]
            if a == b or b == c or a == c:
                continue
            T.append((a, b, c))
            TM.append(self.TM[t])
            TS.append(self.TS[t])
        self.P, self.N, self.UV, self.UV2, self.C = P, N, UV, UV2, C
        self.T, self.TM, self.TS = T, TM, TS
        return self

    def remove_degenerate(self, min_area=1e-9):
        """Drop triangles with area < min_area (m^2)."""
        keep = [t for t in range(len(self.T)) if self.tri_area(t) >= min_area]
        self.T = [self.T[t] for t in keep]
        self.TM = [self.TM[t] for t in keep]
        self.TS = [self.TS[t] for t in keep]
        return self

    def compact(self):
        """Remove unused vertices."""
        used = sorted(set(i for t in self.T for i in t))
        remap = {}
        for n, i in enumerate(used):
            remap[i] = n
        self.P = [self.P[i] for i in used]
        self.N = [self.N[i] for i in used]
        self.UV = [self.UV[i] for i in used]
        self.UV2 = [self.UV2[i] for i in used]
        self.C = [self.C[i] for i in used]
        self.T = [(remap[a], remap[b], remap[c]) for a, b, c in self.T]
        return self

    def tri_area(self, t):
        a, b, c = self.T[t]
        return 0.5 * vlen(vcross(vsub(self.P[b], self.P[a]), vsub(self.P[c], self.P[a])))

    def tri_normal(self, t):
        a, b, c = self.T[t]
        return face_normal(self.P[a], self.P[b], self.P[c])

    def _rebuild(self, corner_n=None, corner_uv=None):
        """Re-index so each corner can have its own normal/uv; identical corners share a vertex."""
        keys = {}
        P, N, UV, UV2, C = [], [], [], [], []
        T = []
        for t, tri in enumerate(self.T):
            out = []
            for k in range(3):
                v = tri[k]
                n = corner_n[t * 3 + k] if corner_n is not None else self.N[v]
                uv = corner_uv[t * 3 + k] if corner_uv is not None else self.UV[v]
                key = (v, _qk(n, 1e5), _qk(uv, 1e6))
                j = keys.get(key)
                if j is None:
                    j = len(P)
                    keys[key] = j
                    P.append(self.P[v])
                    N.append(n)
                    UV.append(uv)
                    UV2.append(self.UV2[v])
                    C.append(self.C[v])
                out.append(j)
            T.append(tuple(out))
        self.P, self.N, self.UV, self.UV2, self.C, self.T = P, N, UV, UV2, C, T
        return self

    def smooth(self, crease=50.0, soft=None):
        """Smoothing-group normals (prefer this to normals() in new code; see smooth_normals()). Faces around a
        vertex share one normal when they are connected through edges whose dihedral angle is <= crease
        degrees (transitive), so smooth areas get exactly one vertex per position (per UV/colour) and hard
        edges split cleanly. soft(t) -> True: edges of triangle t never split. In place; returns self."""
        g = smooth_normals(self, crease, soft)
        self.P, self.N, self.UV, self.UV2, self.C = g.P, g.N, g.UV, g.UV2, g.C
        self.T, self.TM, self.TS = g.T, g.TM, g.TS
        return self

    def normals(self, crease=180.0):
        """Recompute vertex normals: area-weighted face normals, smoothed across faces that share a POSITION
        (also across UV seams) when their angle is <= crease degrees. crease=0 gives flat shading, 180 fully
        smooth. Vertices are split where needed.
        LEGACY: the test is per corner against the corner's own face, which isn't transitive (a vertex on a
        gently curved surface can get a slightly different normal per face, which splits it into several
        vertices). Kept unchanged because existing outputs depend on it (the primitives call it); use
        smooth() / smooth_normals() for new code."""
        fn = []
        fu = []
        for a, b, c in self.T:
            n = vcross(vsub(self.P[b], self.P[a]), vsub(self.P[c], self.P[a]))
            fn.append(n)
            fu.append(vnorm(n, (0.0, 0.0, 0.0)))
        groups = {}
        q = 1e5
        pk = [_qk(p, q) for p in self.P]
        for t, tri in enumerate(self.T):
            for v in tri:
                lst = groups.setdefault(pk[v], [])
                if not lst or lst[-1] != t:
                    lst.append(t)
        cc = math.cos(math.radians(min(max(crease, 0.0), 180.0))) - 1e-6
        cn = []
        for t, tri in enumerate(self.T):
            for v in tri:
                acc = (0.0, 0.0, 0.0)
                for g in groups[pk[v]]:
                    if g == t or vdot(fu[g], fu[t]) >= cc:
                        acc = vadd(acc, fn[g])
                n = vnorm(acc, (0.0, 0.0, 0.0))
                if n == (0.0, 0.0, 0.0):
                    n = vnorm(fn[t])
                cn.append(n)
        return self._rebuild(corner_n=cn)

    def flat(self):
        """Flat normals (= normals(0))."""
        return self.normals(0.0)

    def set_normals(self, fn):
        """Set every normal from a function of the position: g.set_normals(lambda p: (0, 1, 0))."""
        self.N = [vnorm(fn(p)) for p in self.P]
        return self

    # ---- UVs
    _FACE_UV = {
        "-z": lambda p: (p[0], -p[1]), "+z": lambda p: (-p[0], -p[1]),
        "+x": lambda p: (p[2], -p[1]), "-x": lambda p: (-p[2], -p[1]),
        "+y": lambda p: (p[0], -p[2]), "-y": lambda p: (-p[0], -p[2]),
    }

    def uv_planar(self, face="-z", scale=1.0, offset=(0.0, 0.0)):
        """Planar projection as seen by a viewer looking at the given face ('-z' = standing at -Z looking
        north: u = x, v = -y). scale = texture repeats per metre (number or (su, sv)); uv = proj*scale+offset."""
        su, sv = (scale, scale) if not isinstance(scale, (tuple, list)) else scale
        f = self._FACE_UV[face]
        self.UV = [(f(p)[0] * su + offset[0], f(p)[1] * sv + offset[1]) for p in self.P]
        return self

    def uv_box(self, scale=1.0, offset=(0.0, 0.0)):
        """Box projection per triangle by the dominant axis of its face normal, oriented so the texture is
        upright and unmirrored on every side. scale = repeats per metre."""
        su, sv = (scale, scale) if not isinstance(scale, (tuple, list)) else scale
        cuv = []
        for t, tri in enumerate(self.T):
            n = self.tri_normal(t)
            ax = max(range(3), key=lambda i: abs(n[i]))
            face = ("+" if n[ax] >= 0 else "-") + "xyz"[ax]
            f = self._FACE_UV[face]
            for v in tri:
                u, w = f(self.P[v])
                cuv.append((u * su + offset[0], w * sv + offset[1]))
        return self._rebuild(corner_uv=cuv)

    def uv_cylinder(self, axis="y", center=(0.0, 0.0, 0.0), u_repeat=1.0, v_scale=1.0, v_offset=0.0):
        """Cylindrical projection around an axis through center: u = angle/360 * u_repeat (angle from +Z toward
        +X for axis y), v = -height * v_scale + v_offset. Seams are split per triangle."""
        k = "xyz".index(axis)
        a1, a2 = {0: (2, 1), 1: (0, 2), 2: (0, 1)}[k]
        cuv = []
        for tri in self.T:
            us = []
            vs = []
            for v in tri:
                p = vsub(self.P[v], center)
                ang = math.atan2(p[a1], p[a2]) / (2.0 * math.pi)
                if ang < 0:
                    ang += 1.0
                us.append(ang)
                vs.append(-p[k] * v_scale + v_offset)
            if max(us) - min(us) > 0.5:
                us = [u + 1.0 if u < 0.5 else u for u in us]
            for i in range(3):
                cuv.append((us[i] * u_repeat, vs[i]))
        return self._rebuild(corner_uv=cuv)

    def uv_transform(self, su=1.0, sv=1.0, ou=0.0, ov=0.0):
        """uv = uv * (su, sv) + (ou, ov)."""
        self.UV = [(u * su + ou, v * sv + ov) for u, v in self.UV]
        return self

    def uv_remap(self, rect):
        """Map uv [0,1]^2 into an atlas rectangle (u0, v0, u1, v1) (normalised, e.g. from image.Atlas.uv())."""
        u0, v0, u1, v1 = rect
        self.UV = [(u0 + u * (u1 - u0), v0 + v * (v1 - v0)) for u, v in self.UV]
        return self

    def uv_fit(self, rect=(0.0, 0.0, 1.0, 1.0)):
        """Stretch the current UV bounds to fill rect."""
        if not self.UV:
            return self
        us = [u for u, _ in self.UV]
        vs = [v for _, v in self.UV]
        du = (max(us) - min(us)) or 1.0
        dv = (max(vs) - min(vs)) or 1.0
        mu, mv = min(us), min(vs)
        self.UV = [((u - mu) / du, (v - mv) / dv) for u, v in self.UV]
        return self.uv_remap(rect)

    def uv2_from_uv(self, scale=1.0):
        """Second UV set = first * scale (detail textures, RVert2)."""
        self.UV2 = [(u * scale, v * scale) for u, v in self.UV]
        return self

    # ---- attributes
    def set_mat(self, mat, where=None):
        """Set the material of all triangles (or of those whose current material key equals `where`)."""
        wk = mat_key(where)
        self.TM = [mat if (where is None or mat_key(m) == wk) else m for m in self.TM]
        if where is None:
            self.mat = mat
        return self

    def set_surface(self, surf):
        self.TS = [surf] * len(self.T)
        return self

    def set_color(self, c):
        """Set every vertex colour (r, g, b[, a])."""
        c = tuple(c) if len(c) == 4 else (c[0], c[1], c[2], 255)
        self.C = [c] * len(self.P)
        return self

    def tint(self, c):
        """Multiply vertex colours by c/255."""
        self.C = [(palette.clamp8(x[0] * c[0] / 255.0), palette.clamp8(x[1] * c[1] / 255.0),
                   palette.clamp8(x[2] * c[2] / 255.0), x[3]) for x in self.C]
        return self

    # ---- queries
    def bounds(self):
        """((minx, miny, minz), (maxx, maxy, maxz))."""
        if not self.P:
            return ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0))
        xs = [p[0] for p in self.P]
        ys = [p[1] for p in self.P]
        zs = [p[2] for p in self.P]
        return ((min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs)))

    def signed_volume(self):
        """Volume (m^3); positive when a closed surface faces outward."""
        s = 0.0
        for a, b, c in self.T:
            s += vdot(self.P[a], vcross(self.P[b], self.P[c]))
        return s / 6.0

    def edge_report(self):
        """Position-welded edge topology: dict(boundary=n, nonmanifold=n, flipped=n, closed=bool).
        flipped = edges used twice in the SAME direction (inconsistent winding)."""
        q = 1e4
        pk = [_qk(p, q) for p in self.P]
        ids = {}
        vid = []
        for k in pk:
            vid.append(ids.setdefault(k, len(ids)))
        und = {}
        for a, b, c in self.T:
            for u, v in ((a, b), (b, c), (c, a)):
                u, v = vid[u], vid[v]
                if u == v:
                    continue
                key = (u, v) if u < v else (v, u)
                und.setdefault(key, []).append((u, v))
        boundary = nonman = flipped = 0
        for key, lst in und.items():
            if len(lst) == 1:
                boundary += 1
            elif len(lst) == 2:
                if lst[0] == lst[1]:
                    flipped += 1
            else:
                nonman += 1
        return {"boundary": boundary, "nonmanifold": nonman, "flipped": flipped,
                "closed": boundary == 0 and nonman == 0 and len(self.T) > 0}

    def stats(self):
        lo, hi = self.bounds()
        return {"verts": len(self.P), "tris": len(self.T), "size": vsub(hi, lo)}

    def batches(self, fmt=RFMT_V1):
        """Split by material (first-use order) into engine vertex tuples for export.PoolEncoder:
        [(mat_key, verts, idx)], verts = (x, y, z, argb, u, v[, u2, v2])."""
        order = []
        groups = {}
        for t, m in enumerate(self.TM):
            k = mat_key(m)
            if k not in groups:
                groups[k] = []
                order.append(k)
            groups[k].append(t)
        out = []
        for k in order:
            vmap = {}
            verts = []
            idx = []
            for t in groups[k]:
                for v in self.T[t]:
                    j = vmap.get(v)
                    if j is None:
                        j = len(verts)
                        vmap[v] = j
                        verts.append(self.rvert(v, fmt))
                    idx.append(j)
            out.append((k, verts, idx))
        return out

    def rvert(self, v, fmt=RFMT_V1):
        """Engine vertex tuple of vertex v (float32-rounded)."""
        p = self.P[v]
        uv = self.UV[v]
        col = palette.argb(self.C[v])
        if fmt == RFMT_V1:
            return (f32(p[0]), f32(p[1]), f32(p[2]), col, f32(uv[0]), f32(uv[1]))
        uv2 = self.UV2[v]
        return (f32(p[0]), f32(p[1]), f32(p[2]), col, f32(uv[0]), f32(uv[1]), f32(uv2[0]), f32(uv2[1]))


def merge(*geos):
    """New Geo containing all given Geos."""
    g = Geo()
    g.merge(*geos)
    return g


def smooth_normals(g, crease=50.0, soft=None):
    """Normals by smoothing groups -> NEW welded Geo (g is not changed; Geo.smooth() is the in-place form).

    Two triangles are in the same group at a shared vertex position when they are connected (directly or
    through other triangles around that position) by manifold edges whose dihedral angle is <= crease
    degrees. Each group gets one area-weighted normal, so a smooth area has exactly one vertex per position
    (UV seams and colour changes still split, by the final weld) and hard edges (arch lips, cap rims, box
    edges) split cleanly. Edges are matched by position (1e-5 m), so UV seams don't break smoothing; edges
    used by 1 or > 2 triangles never join groups. soft(t) -> True: the edges of triangle t always join
    (e.g. black wheel wells whose shading is invisible: fewer vertices). Keeps UV, UV2, colours, materials
    and surfaces. Output order: vertices in first use by triangle corners, then Geo.weld() (identical
    attributes merge, degenerate triangles drop). (From A1's gen_car.py; the same results byte for byte.)"""
    pk = [_qk(p, 1e5) for p in g.P]
    fn = [vcross(vsub(g.P[b], g.P[a]), vsub(g.P[c], g.P[a])) for a, b, c in g.T]
    fu = [vnorm(n, (0.0, 0.0, 0.0)) for n in fn]
    edges = {}
    for t, (a, b, c) in enumerate(g.T):
        for u, v in ((a, b), (b, c), (c, a)):
            ku, kv = pk[u], pk[v]
            edges.setdefault((ku, kv) if ku < kv else (kv, ku), []).append(t)
    parent = {}

    def find(x):
        while parent.get(x, x) != x:
            parent[x] = parent.get(parent[x], parent[x])
            x = parent[x]
        return x

    cc = math.cos(math.radians(crease))
    for key, ts in edges.items():
        if len(ts) == 2 and (vdot(fu[ts[0]], fu[ts[1]]) >= cc or
                             (soft is not None and (soft(ts[0]) or soft(ts[1])))):
            for e in key:
                ra, rb = find((e, ts[0])), find((e, ts[1]))
                if ra != rb:
                    parent[max(ra, rb)] = min(ra, rb)
    acc = {}
    for t, tri in enumerate(g.T):
        for v in tri:
            r = find((pk[v], t))
            acc[r] = vadd(acc.get(r, (0.0, 0.0, 0.0)), fn[t])
    out = Geo()
    for t, tri in enumerate(g.T):
        idx = [out.vert(g.P[v], vnorm(acc[find((pk[v], t))], fu[t]), g.UV[v], g.C[v], g.UV2[v]) for v in tri]
        out.tri(idx[0], idx[1], idx[2], g.TM[t], g.TS[t])
    out.mat = g.mat
    return out.weld()


# ---------------------------------------------------------------------------------------------------- primitives

def quad(p0, p1, p2, p3, mat=None, uv=(0.0, 0.0, 1.0, 1.0), surf=0, color=WHITE):
    """One quad (2 tris). Corners CLOCKWISE seen from the front: p0 bottom-left, p1 top-left, p2 top-right,
    p3 bottom-right. uv = (u0, v0, u1, v1): p1 gets (u0, v0) (image top-left), p3 gets (u1, v1)."""
    g = Geo(mat)
    n = face_normal(p0, p1, p2)
    u0, v0, u1, v1 = uv
    a = g.vert(p0, n, (u0, v1), color)
    b = g.vert(p1, n, (u0, v0), color)
    c = g.vert(p2, n, (u1, v0), color)
    d = g.vert(p3, n, (u1, v1), color)
    g.quad_idx(a, b, c, d, mat, surf)
    return g


def vquad(w, h, pos=(0.0, 0.0, 0.0), yaw=0.0, mat=None, uv=(0.0, 0.0, 1.0, 1.0)):
    """Vertical w x h quad standing on pos, centred in x, FACING -Z (seen by a viewer at -Z looking north),
    then turned by yaw degrees. Signs, boards, billboard planes."""
    hw = w * 0.5
    g = quad((-hw, 0.0, 0.0), (-hw, h, 0.0), (hw, h, 0.0), (hw, 0.0, 0.0), mat, uv)
    if yaw:
        g.rotate_y(yaw)
    return g.translate(pos[0], pos[1], pos[2])


def box(sx, sy, sz, pos=(0.0, 0.0, 0.0), base=True, mat=None, uv_scale=None, skip=(), color=WHITE):
    """Axis-aligned box sx x sy x sz, flat normals, outward. base=True: pos is the centre of the BOTTOM face
    (objects stand on y = 0); base=False: pos is the centre. skip = faces to leave out, e.g. ("-y",).
    uv_scale=None maps each face to the full [0,1] texture; a number = box projection, repeats per metre."""
    hx, hz = sx * 0.5, sz * 0.5
    x0, x1 = pos[0] - hx, pos[0] + hx
    z0, z1 = pos[2] - hz, pos[2] + hz
    y0 = pos[1] if base else pos[1] - sy * 0.5
    y1 = y0 + sy
    faces = {
        "-z": ((x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0)),
        "+z": ((x1, y0, z1), (x1, y1, z1), (x0, y1, z1), (x0, y0, z1)),
        "+x": ((x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)),
        "-x": ((x0, y0, z1), (x0, y1, z1), (x0, y1, z0), (x0, y0, z0)),
        "+y": ((x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0)),
        "-y": ((x1, y0, z0), (x1, y0, z1), (x0, y0, z1), (x0, y0, z0)),
    }
    g = Geo(mat)
    for name in ("-z", "+z", "+x", "-x", "+y", "-y"):
        if name in skip:
            continue
        g.merge(quad(*faces[name], mat=mat, color=color))
    if uv_scale is not None:
        g.uv_box(uv_scale)
    return g


def _grid(rows, uvf, mat=None, mats=None, surfs=None, color=WHITE):
    """rows[i][j] points; quads between rows i, i+1 and columns j, j+1. Front side = cross(row direction,
    column direction). uvf(i, j) -> (u, v). mats/surfs: per column segment j. Degenerate triangles skipped."""
    g = Geo(mat)
    nr = len(rows)
    nc = len(rows[0])
    idx = []
    for i in range(nr):
        r = []
        for j in range(nc):
            r.append(g.vert(rows[i][j], (0.0, 1.0, 0.0), uvf(i, j), color))
        idx.append(r)
    for i in range(nr - 1):
        for j in range(nc - 1):
            m = mats[j] if mats is not None else mat
            s = surfs[j] if surfs is not None else 0
            A, B, C, D = idx[i][j], idx[i][j + 1], idx[i + 1][j + 1], idx[i + 1][j]
            for t in ((A, D, C), (A, C, B)):
                pa, pb, pc = g.P[t[0]], g.P[t[1]], g.P[t[2]]
                if vlen(vcross(vsub(pb, pa), vsub(pc, pa))) > 1e-12:
                    g.tri(t[0], t[1], t[2], m, s)
    return g


def _cumlen(pts):
    out = [0.0]
    for i in range(1, len(pts)):
        out.append(out[-1] + vlen(vsub(pts[i], pts[i - 1])))
    return out


def _centroid(pts):
    n = float(len(pts))
    return (sum(p[0] for p in pts) / n, sum(p[1] for p in pts) / n, sum(p[2] for p in pts) / n)


def cap(ring, target_dir, mat=None, color=WHITE, center=None, bulge=0.0):
    """Fan polygon over a ring of points (closed implicitly from last to first), oriented so its front
    faces target_dir. UVs: planar over the polygon bounds. Used for loft/lathe/extrude ends.

    Default (center=None, bulge=0): flat fan from the ring centroid, the whole ring oriented at once.
    center = a point (x, y, z), 'centroid' or 'bbox' (bounding-box centre of the ring), and/or bulge = metres
    the fan centre is pushed along target_dir (a convex fascia / tail / dome; negative = dished): then EACH
    triangle is oriented toward target_dir on its own (robust for non-planar or slightly concave rings),
    triangles thinner than 1e-10 (|cross|) are dropped, and normals are smoothed over the fan (centre =
    average). A repeated closing point (last == first) is ignored in both modes."""
    pts = list(ring)
    if len(pts) > 2 and vlen(vsub(pts[0], pts[-1])) < 1e-9:
        pts = pts[:-1]
    if center is not None or bulge:
        return _cap_fan(pts, target_dir, mat, color, center, bulge)
    c = _centroid(pts)
    n = (0.0, 0.0, 0.0)
    for j in range(len(pts)):
        n = vadd(n, vcross(vsub(pts[j], c), vsub(pts[(j + 1) % len(pts)], c)))
    if vdot(n, target_dir) < 0:
        pts.reverse()
        n = vscale(n, -1.0)
    nn = vnorm(n, vnorm(target_dir))
    ax = vnorm(vsub(pts[0], c), (1.0, 0.0, 0.0))
    ay = vcross(nn, ax)
    pr = [(vdot(vsub(p, c), ax), vdot(vsub(p, c), ay)) for p in pts]
    mnx = min(p[0] for p in pr)
    mxx = max(p[0] for p in pr)
    mny = min(p[1] for p in pr)
    mxy = max(p[1] for p in pr)
    dx = (mxx - mnx) or 1.0
    dy = (mxy - mny) or 1.0
    g = Geo(mat)
    ci = g.vert(c, nn, ((0.0 - mnx) / dx, (0.0 - mny) / dy), color)
    vi = [g.vert(p, nn, ((q[0] - mnx) / dx, (q[1] - mny) / dy), color) for p, q in zip(pts, pr)]
    for j in range(len(pts)):
        a, b = vi[j], vi[(j + 1) % len(pts)]
        pa, pb = g.P[a], g.P[b]
        if vlen(vcross(vsub(pa, c), vsub(pb, c))) > 1e-12:
            g.tri(ci, a, b, mat)
    return g


def _cap_fan(pts, target_dir, mat, color, center, bulge):
    """cap() with an explicit/bulged centre: per-triangle orientation (A1's fan_cap, generalised)."""
    if center is None or center == "centroid":
        c = _centroid(pts)
    elif center == "bbox":
        c = tuple((min(p[k] for p in pts) + max(p[k] for p in pts)) * 0.5 for k in range(3))
    else:
        c = (float(center[0]), float(center[1]), float(center[2]))
    d = vnorm(target_dir, (0.0, 0.0, 1.0))
    if bulge:
        c = vadd(c, vscale(d, bulge))
    # plane basis for the UVs (ring centroid, ring normal toward target_dir)
    rc = _centroid(pts)
    n = (0.0, 0.0, 0.0)
    for j in range(len(pts)):
        n = vadd(n, vcross(vsub(pts[j], rc), vsub(pts[(j + 1) % len(pts)], rc)))
    if vdot(n, d) < 0:
        n = vscale(n, -1.0)
    nn = vnorm(n, d)
    ax = vnorm(vsub(pts[0], rc), (1.0, 0.0, 0.0))
    ay = vcross(nn, ax)
    pr = [(vdot(vsub(p, rc), ax), vdot(vsub(p, rc), ay)) for p in [c] + pts]
    mnx = min(p[0] for p in pr)
    mny = min(p[1] for p in pr)
    dx = (max(p[0] for p in pr) - mnx) or 1.0
    dy = (max(p[1] for p in pr) - mny) or 1.0
    uvs = [((q[0] - mnx) / dx, (q[1] - mny) / dy) for q in pr]
    g = Geo(mat)
    ci = g.vert(c, nn, uvs[0], color)
    vi = [g.vert(p, nn, uv, color) for p, uv in zip(pts, uvs[1:])]
    for j in range(len(vi)):
        a, b = vi[j], vi[(j + 1) % len(vi)]
        cr = vcross(vsub(g.P[a], g.P[ci]), vsub(g.P[b], g.P[ci]))
        if vlen(cr) > 1e-10:
            if vdot(cr, d) < 0.0:
                g.tri(ci, b, a, mat)
            else:
                g.tri(ci, a, b, mat)
    # smooth normals over the fan (one group: area-weighted, the centre gets the average)
    acc = [(0.0, 0.0, 0.0)] * len(g.P)
    for a, b, c3 in g.T:
        fnv = vcross(vsub(g.P[b], g.P[a]), vsub(g.P[c3], g.P[a]))
        for v in (a, b, c3):
            acc[v] = vadd(acc[v], fnv)
    g.N = [vnorm(x, nn) for x in acc]
    return g


def loft(stations, closed=False, cap_start=False, cap_end=False, mat=None, crease=50.0, u=None, v=None,
         color=WHITE):
    """Skin a list of cross-section stations (each a list of 3D points, all the same length).
    Front side = cross(station direction, point direction): stations running toward -Z (front to back) with
    points running bottom -> top on the +X side face outward (+X). closed=True: each station is a loop.
    cap_start/cap_end: flat fans at the first/last station facing away from the body; with any cap the
    side winding is made to agree with the caps, so capped lofts always come out outward.
    u: per-point list (closed: len+1 values, the seam repeats; default: length along station 0 normalised
    0..1; 'm' = metres), v: per-station list
    (default: centroid path length normalised; 'm' = metres). Normals: crease angle (degrees)."""
    st = [list(s) for s in stations]
    if closed:
        st = [s + [s[0]] for s in st]
    npts = len(st[0])
    for s in st:
        if len(s) != npts:
            raise ValueError("all loft stations need the same number of points")
    if u is None or u == "m":
        cl = _cumlen(st[0])
        tot = cl[-1] or 1.0
        us = cl if u == "m" else [x / tot for x in cl]
    else:
        us = list(u)
        if len(us) != npts:
            raise ValueError("loft u needs %d values (closed loops repeat the seam point)" % npts)
    if v is None or v == "m":
        cl = _cumlen([_centroid(s) for s in st])
        tot = cl[-1] or 1.0
        vs = cl if v == "m" else [x / tot for x in cl]
    else:
        vs = list(v)
    g = _grid(st, lambda i, j: (us[j], vs[i]), mat=mat, color=color)
    g.normals(crease)
    if cap_start or cap_end:
        c0 = _centroid(stations[0])
        c1 = _centroid(stations[-1])
        d = vsub(c1, c0)
        if vlen(d) < 1e-9:
            d = (0.0, 0.0, 1.0)
        caps = Geo(mat)
        if cap_start:
            caps.merge(cap(stations[0], vscale(d, -1.0), mat, color))
        if cap_end:
            caps.merge(cap(stations[-1], d, mat, color))
        # caps face away from the body; make the sides agree with them (consistent, outward winding)
        if merge(g, caps).edge_report()["flipped"] > 0:
            g.flip()
        g.merge(caps)
    return g


def lathe(profile, seg=12, mat=None, caps=True, crease=60.0, u_repeat=1.0, v=None, pos=(0.0, 0.0, 0.0),
          color=WHITE):
    """Surface of revolution about +Y. profile = [(radius, y), ...] from BOTTOM to TOP for outward faces.
    seg = segments around. caps: True/False/'top'/'bottom' adds flat discs where the end radius > 0.
    u = around (0..u_repeat), v = along the profile (default normalised length; or a per-point list)."""
    rings = []
    for k in range(seg + 1):
        a = 2.0 * math.pi * k / seg
        s, c = math.sin(a), math.cos(a)
        rings.append([(r * s, y, r * c) for r, y in profile])
    if v is None:
        cl = [0.0]
        for i in range(1, len(profile)):
            cl.append(cl[-1] + math.hypot(profile[i][0] - profile[i - 1][0], profile[i][1] - profile[i - 1][1]))
        tot = cl[-1] or 1.0
        vs = [1.0 - x / tot for x in cl]
    else:
        vs = list(v)
    g = _grid(rings, lambda i, j: (u_repeat * i / float(seg), vs[j]), mat=mat, color=color)
    g.normals(crease)
    if caps:
        r0, y0 = profile[0]
        r1, y1 = profile[-1]
        if (caps is True or caps == "bottom") and r0 > 1e-6:
            g.merge(cap([q[0] for q in rings[:-1]], (0.0, -1.0, 0.0), mat, color))
        if (caps is True or caps == "top") and r1 > 1e-6:
            g.merge(cap([q[-1] for q in rings[:-1]], (0.0, 1.0, 0.0), mat, color))
    return g.translate(pos[0], pos[1], pos[2])


def cylinder(r, h, seg=12, pos=(0.0, 0.0, 0.0), axis="y", caps=True, mat=None, r_top=None, u_repeat=1.0,
             crease=60.0, color=WHITE):
    """Cylinder (or cone frustum with r_top) of height h standing on pos (base centre), along +Y; axis='x'
    or 'z' lays it along +X / +Z (still starting at pos). caps like lathe."""
    rt = r if r_top is None else r_top
    g = lathe([(r, 0.0), (rt, h)], seg, mat, caps, crease, u_repeat, None, (0.0, 0.0, 0.0), color)
    if axis == "x":
        g.rotate_z(-90.0)
    elif axis == "z":
        g.rotate_x(90.0)
    return g.translate(pos[0], pos[1], pos[2])


def sphere(r, seg=12, rings=6, pos=(0.0, 0.0, 0.0), mat=None, color=WHITE):
    """UV sphere of radius r centred at pos (smooth)."""
    prof = []
    for i in range(rings + 1):
        a = math.pi * i / rings
        prof.append((r * math.sin(a), -r * math.cos(a)))
    return lathe(prof, seg, mat, False, 180.0, 1.0, None, pos, color)


def frames_along(path, closed=False, mode="horizontal"):
    """Per-point (right, up) frames along a polyline. mode 'horizontal': right = horizontal perpendicular of
    the tangent (+Z tangent -> +X right), up = world +Y (walls, fences stay vertical). mode 'rmf':
    rotation-minimising frames (tubes along arbitrary 3D paths)."""
    n = len(path)
    tans = []
    for i in range(n):
        if closed:
            a = path[(i - 1) % n]
            b = path[(i + 1) % n]
        else:
            a = path[max(i - 1, 0)]
            b = path[min(i + 1, n - 1)]
        tans.append(vnorm(vsub(b, a), (0.0, 0.0, 1.0)))
    frames = []
    if mode == "horizontal":
        for t in tans:
            r = vnorm((t[2], 0.0, -t[0]), (1.0, 0.0, 0.0))
            frames.append((r, (0.0, 1.0, 0.0)))
        return frames
    t0 = tans[0]
    r = (t0[2], 0.0, -t0[0])
    if vlen(r) < 1e-6:
        r = (1.0, 0.0, 0.0)
    r = vnorm(r)
    for i in range(n):
        t = tans[i]
        if i > 0:
            # double reflection (Wang et al.) approximated by projecting the previous right vector
            r = vsub(r, vscale(t, vdot(r, t)))
            r = vnorm(r, (1.0, 0.0, 0.0))
        up = vcross(t, r)
        frames.append((r, up))
    return frames


def strip_along(path, profile, closed=False, frames=None, mat=None, mats=None, surfs=None, u=None,
                v_scale=0.1, v0=0.0, crease=40.0, color=WHITE):
    """Sweep a 2D cross-section profile along a 3D polyline: roads, verges, walls, rails, fences.

    profile = [(lat, h), ...] or [(lat, h, u), ...]: lat along the frame's right vector (negative = left),
    h along its up vector. FRONT FACE RULE: rotate the profile's walking direction 90 degrees counter-clockwise
    in the (lat, h) plane: a profile walked left -> right faces up (road), one walked bottom -> top on the
    right side faces left (a right-hand wall facing the road).
    frames: per-point (right, up) (default frames_along(path, closed)); trackc passes banked frames.
    mats/surfs: per profile segment. u: per profile point (default: third tuple value, else profile length
    normalised 0..1). v = path length * v_scale + v0 (texture repeats per metre along the path).
    closed=True joins the last point to the first (circuits)."""
    pts = list(path)
    fr = list(frames) if frames is not None else frames_along(pts, closed)
    if closed:
        pts = pts + [pts[0]]
        fr = fr + [fr[0]]
    rows = []
    for p, (r, up) in zip(pts, fr):
        rows.append([vadd(vadd(p, vscale(r, q[0])), vscale(up, q[1])) for q in profile])
    if u is not None:
        us = list(u)
    elif len(profile[0]) > 2:
        us = [q[2] for q in profile]
    else:
        cl = [0.0]
        for j in range(1, len(profile)):
            cl.append(cl[-1] + math.hypot(profile[j][0] - profile[j - 1][0], profile[j][1] - profile[j - 1][1]))
        tot = cl[-1] or 1.0
        us = [x / tot for x in cl]
    cl = _cumlen(pts)
    vs = [x * v_scale + v0 for x in cl]
    g = _grid(rows, lambda i, j: (us[j], vs[i]), mat=mat, mats=mats, surfs=surfs, color=color)
    return g.normals(crease)


def strip_rows(rows, uv=None, mat=None, mats=None, surfs=None, group=None, skip=None, color=WHITE,
               v_scale=0.1, v0=0.0, crease=40.0, min_area=1e-9):
    """Strip whose cross-section varies per row (roads that widen, kerbs that come and go, verges that end):
    rows[i] = list of 3D points across the strip at station i (every row the same length; collapse a
    segment by repeating a point). Quad (i, k) joins rows i, i+1 between columns k, k+1.
    FRONT FACE RULE (= strip_along): rows running along the path with points walked left -> right face up.

    uv:     callable uv(i, j) -> (u, v) (uv(i, j, key) when group is given), or None: u = length across row
            i normalised 0..1, v = length of the row-centroid path * v_scale + v0.
    mats, surfs: per segment k a list, or a callable f(i, k) (varies per row); default mat / 0.
    group:  callable group(i, k) -> key: neighbouring quads share their corner vertices only when their keys
            are equal (one key per material role gives each role its own UVs/colours at the seams).
            Vertices are keyed (row, column, key) with the key of the quad that creates them.
    skip:   callable skip(i, k) -> True leaves out quad (i, k) (e.g. collision vs terrain parts in two calls).
    color:  vertex colour, or callable color(i, j[, key]).
    Quads whose two row edges are both shorter than 1e-6 m (per axis) are skipped before any vertex is made;
    other triangles with area < min_area m^2 are dropped. Vertices are created in quad order (i, then k,
    corners (i,k) (i,k+1) (i+1,k+1) (i+1,k)). crease: smoothing-group normals (Geo.smooth); None keeps
    placeholder (0, 1, 0) normals (callers that compute normals later). Returns a new Geo."""
    nr = len(rows)
    if nr < 2:
        return Geo(mat)
    nc = len(rows[0])
    for r in rows:
        if len(r) != nc:
            raise ValueError("strip_rows: all rows need the same number of points")
    if uv is None:
        ul = []
        for r in rows:
            cl = _cumlen(r)
            tot = cl[-1] or 1.0
            ul.append([x / tot for x in cl])
        vl = [x * v_scale + v0 for x in _cumlen([_centroid(r) for r in rows])]
        uvf = lambda i, j, key=None: (ul[i][j], vl[i])
    elif group is None:
        uvf = lambda i, j, key=None: uv(i, j)
    else:
        uvf = uv
    if callable(color):
        colf = (lambda i, j, key=None: color(i, j)) if group is None else color
    else:
        colf = lambda i, j, key=None: color
    lim = 4.0 * min_area * min_area
    g = Geo(mat)
    cache = {}

    def vid(i, j, key):
        k = (i, j, key)
        v = cache.get(k)
        if v is None:
            v = g.vert(rows[i][j], (0.0, 1.0, 0.0), uvf(i, j, key), colf(i, j, key))
            cache[k] = v
        return v

    def same(a, b):
        return abs(a[0] - b[0]) < 1e-6 and abs(a[1] - b[1]) < 1e-6 and abs(a[2] - b[2]) < 1e-6

    for i in range(nr - 1):
        ra, rb = rows[i], rows[i + 1]
        for k in range(nc - 1):
            if skip is not None and skip(i, k):
                continue
            if same(ra[k], ra[k + 1]) and same(rb[k + 1], rb[k]):
                continue
            key = group(i, k) if group is not None else None
            A = vid(i, k, key)
            B = vid(i, k + 1, key)
            C = vid(i + 1, k + 1, key)
            D = vid(i + 1, k, key)
            m = (mats(i, k) if callable(mats) else mats[k]) if mats is not None else mat
            s = (surfs(i, k) if callable(surfs) else surfs[k]) if surfs is not None else 0
            for t in ((A, D, C), (A, C, B)):
                cr = vcross(vsub(g.P[t[1]], g.P[t[0]]), vsub(g.P[t[2]], g.P[t[0]]))
                if vdot(cr, cr) > lim:
                    g.tri(t[0], t[1], t[2], m, s)
    if crease is not None and g.T:
        g.smooth(crease)
    return g


def extrude(shape, path, closed=True, caps=True, mat=None, crease=50.0, frames=None, u_scale=None,
            v_scale=None, color=WHITE):
    """Sweep a 2D shape [(x, y), ...] (x along the frame's right, y along up) along a 3D path.
    closed=True: the shape is a loop; its orientation is fixed automatically so faces point OUTWARD, and
    caps=True closes both ends. closed=False: open shape, front face by the strip_along rule.
    frames default: rotation-minimising (frames_along(path, mode='rmf')); pass 'horizontal' for vertical
    walls. u_scale/v_scale: repeats per metre (None = normalised 0..1)."""
    shp = list(shape)
    if closed:
        area = 0.0
        for i in range(len(shp)):
            x0, y0 = shp[i]
            x1, y1 = shp[(i + 1) % len(shp)]
            area += x0 * y1 - x1 * y0
        if area > 0:      # counter-clockwise in (right, up) would face inward
            shp.reverse()
    if frames is None or frames == "rmf":
        fr = frames_along(path, False, "rmf")
    elif frames == "horizontal":
        fr = frames_along(path, False, "horizontal")
    else:
        fr = frames
    stations = []
    for p, (r, up) in zip(path, fr):
        stations.append([vadd(vadd(p, vscale(r, x)), vscale(up, y)) for x, y in shp])
    if u_scale is None:
        u = None
    else:
        lp = shp + [shp[0]] if closed else shp
        cl = [0.0]
        for i in range(1, len(lp)):
            cl.append(cl[-1] + math.hypot(lp[i][0] - lp[i - 1][0], lp[i][1] - lp[i - 1][1]))
        u = [x * u_scale for x in cl]
    v = None if v_scale is None else [x * v_scale for x in _cumlen(list(path))]
    return loft(stations, closed, caps and closed, caps and closed, mat, crease, u, v, color)


def crossed_billboard(w, h, planes=2, pos=(0.0, 0.0, 0.0), mat=None, uv=(0.0, 0.0, 1.0, 1.0), yaw0=0.0,
                      normals="sphere"):
    """planes (2 or 3) vertical w x h quads crossing at the vertical axis, 180/planes degrees apart: one
    plane = 2 tris, so 2 planes = 4 tris, 3 planes = 6 tris. Needs a cull_none ALPHATEST material.
    normals: 'sphere' (round shading: from the trunk centre, biased up), 'up' (flat), 'face'."""
    g = Geo(mat)
    for k in range(planes):
        g.merge(vquad(w, h, (0.0, 0.0, 0.0), yaw0 + 180.0 * k / planes, mat, uv))
    if normals == "sphere":
        g.set_normals(lambda p: vadd(vnorm((p[0], p[1] - h * 0.45, p[2])), (0.0, 0.6, 0.0)))
    elif normals == "up":
        g.set_normals(lambda p: (0.0, 1.0, 0.0))
    return g.translate(pos[0], pos[1], pos[2])


# ---------------------------------------------------------------------------------------------------- walls (facade)

def _cross2(a, b, p):
    return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])


def clip_convex(poly, clip):
    """Sutherland-Hodgman in 2D: clip polygon `poly` by the CONVEX polygon `clip`; both clockwise in
    (x right, y up). Returns the clipped polygon (list of (x, y); may be empty)."""
    out = list(poly)
    n = len(clip)
    for i in range(n):
        a, b = clip[i], clip[(i + 1) % n]
        src = out
        out = []
        m = len(src)
        if m == 0:
            break
        for j in range(m):
            p, q = src[j], src[(j + 1) % m]
            dp, dq = _cross2(a, b, p), _cross2(a, b, q)
            pin, qin = dp <= 1e-9, dq <= 1e-9
            if pin:
                out.append(p)
            if pin != qin:
                t = dp / (dp - dq)
                out.append((p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t))
    res = []
    for p in out:
        if not res or abs(p[0] - res[-1][0]) > 1e-7 or abs(p[1] - res[-1][1]) > 1e-7:
            res.append(p)
    if len(res) > 1 and abs(res[0][0] - res[-1][0]) < 1e-7 and abs(res[0][1] - res[-1][1]) < 1e-7:
        res.pop()
    return res


def poly_area2(poly):
    """Twice the signed area of a 2D polygon (negative for clockwise in x right, y up)."""
    s = 0.0
    for i in range(len(poly)):
        a, b = poly[i], poly[(i + 1) % len(poly)]
        s += a[0] * b[1] - b[0] * a[1]
    return s


def _uv_rect_of(uvs, key):
    if isinstance(key, tuple) and len(key) == 4 and not isinstance(key[0], str):
        return key
    if hasattr(uvs, "uv"):
        return uvs.uv(key)
    if callable(uvs):
        return uvs(key)
    return uvs[key]


def facade(outline, origin, right, elements=(), mat=None, elem_mat=None, uvs=None, tile=2.0, color=WHITE,
           uoff=0.0, voff=0.0):
    """Planar vertical wall as a grid of cells (A4's building technique): no decals, nothing coplanar, no
    T-junctions inside the wall.
    outline: CONVEX polygon, clockwise in wall-local (x right as seen from outside, y up), metres.
    origin: 3D point of local (0, 0); right: horizontal unit vector of local +x (the wall faces
    cross(up, right): right = (1, 0, 0) faces -Z).
    elements: [(cx, y0, w, h, key)] or (cx, y0, w, h, key, mat): rectangles (centre x, bottom y, size) each
    mapped EXACTLY onto one texture rectangle: key = a name looked up in `uvs` (an Atlas (.uv(key)), a dict
    or a function key -> (u0, v0, u1, v1)) or a (u0, v0, u1, v1) tuple itself; material elem_mat (or the
    element's own). Everything else = `mat` with world-scaled tiling UVs (u = x / tile + uoff, v = -y / tile
    + voff: one repeat per `tile` metres). Grid lines run through every element edge; cells are clipped to the
    outline. Elements must lie inside the outline (ValueError). No elements: one fan over the outline
    (not welded); else the welded grid. Normals = the wall normal; prelight afterwards.
        g = M.facade([(-4, 0), (-4, 3), (4, 3), (4, 0)], (0, 0, -3), (1, 0, 0),
                     [(0, 0, 1.0, 2.1, "door"), (-2.5, 1.0, 1.2, 1.2, "win")], wall, glass, atlas)"""
    n = vcross((0.0, 1.0, 0.0), right)
    g = Geo()

    def pt(x, y):
        return (origin[0] + right[0] * x, origin[1] + y, origin[2] + right[2] * x)

    def tuv(x, y):
        return (x / tile + uoff, (-y / tile + voff) if voff else -y / tile)

    if not elements:
        vids = [g.vert(pt(x, y), n, tuv(x, y), color) for x, y in outline]
        for k in range(1, len(vids) - 1):
            g.tri(vids[0], vids[k], vids[k + 1], mat)
        return g
    els = []
    for e in elements:
        cx, y0, w, h, key = e[:5]
        els.append((cx - w * 0.5, y0, cx + w * 0.5, y0 + h, key, e[5] if len(e) > 5 else elem_mat))
    for e in els:
        for corner in ((e[0], e[1]), (e[0], e[3]), (e[2], e[3]), (e[2], e[1])):
            for k in range(len(outline)):
                if _cross2(outline[k], outline[(k + 1) % len(outline)], corner) > 1e-6:
                    raise ValueError("element %s outside its wall" % (e[4],))
    bx = [min(p[0] for p in outline), max(p[0] for p in outline)]
    by = [min(p[1] for p in outline), max(p[1] for p in outline)]
    xs = sorted(set([round(v, 5) for v in bx] + [round(e[0], 5) for e in els] + [round(e[2], 5) for e in els]))
    ys = sorted(set([round(v, 5) for v in by] + [round(e[1], 5) for e in els] + [round(e[3], 5) for e in els]))
    for i in range(len(xs) - 1):
        for j in range(len(ys) - 1):
            x0, x1, y0, y1 = xs[i], xs[i + 1], ys[j], ys[j + 1]
            poly = clip_convex([(x0, y0), (x0, y1), (x1, y1), (x1, y0)], outline)
            if len(poly) < 3 or abs(poly_area2(poly)) < 1e-6:
                continue
            cx = sum(p[0] for p in poly) / len(poly)
            cy = sum(p[1] for p in poly) / len(poly)
            el = None
            for e in els:
                if e[0] < cx < e[2] and e[1] < cy < e[3]:
                    el = e
                    break
            vids = []
            for x, y in poly:
                if el is not None:
                    u0, v0, u1, v1 = _uv_rect_of(uvs, el[4])
                    uv = (u0 + (x - el[0]) / (el[2] - el[0]) * (u1 - u0), v0 + (el[3] - y) / (el[3] - el[1]) * (v1 - v0))
                else:
                    uv = tuv(x, y)
                vids.append(g.vert(pt(x, y), n, uv, color))
            for k in range(1, len(vids) - 1):
                g.tri(vids[0], vids[k], vids[k + 1], el[5] if el is not None else mat)
    return g.weld()


# ---------------------------------------------------------------------------------------------------- lighting

WRAP_OBJECT = 0.5
"""prelight(mode='object') default wrap (unchanged since 1.0)."""
WRAP_SOFT = 0.9
"""Softer object wrap (A4 buildings, T23 gantry): back walls of light colours keep their hue instead of going
muddy (yellow -> olive). prelight(g, mode="object", wrap=M.WRAP_SOFT). Not the default: it would change
every existing object mesh's vertex colours."""

def prelight(geo, mode="object", sun_dir=None, sun=None, ambient=None, hemi=0.35, ao_height=1.2, ao_min=0.7,
             ground_y=0.0, shadow=1.0, exposure=None, multiply=True, wrap=None):
    """Bake lighting into vertex colours (D3D never lights; DESIGN 3.4).

    mode 'white'  : colour = white (cars: lit on the CPU at run time; also sets ambient/sun aside).
    mode 'object' : static objects/meshes. Light from palette.KEY_DIR in MESH space (readable face contrast
                    whatever the instance yaw; trackc multiplies the per-instance sun/shadow on top).
    mode 'world'  : world-space geometry baked in place (trackc): light from palette.SUN_DIR.
    light = ambient * (1 - hemi + hemi * (n.y * 0.5 + 0.5)) + sun * diffuse * shadow, where diffuse =
    max(0, (n . L + wrap) / (1 + wrap)) (wrap default WRAP_OBJECT 0.5 for 'object' so the far side isn't flat
    ambient, 0 for 'world'; WRAP_SOFT 0.9 for light-coloured buildings), scaled by
    exposure (default: the fully lit value maps to 1.0), times a ground AO term rising from ao_min at
    ground_y to 1 at ground_y + ao_height (ao_height=0 disables), times the existing colour if multiply."""
    if mode == "white":
        geo.set_color(WHITE)
        return geo
    L = sun_dir if sun_dir is not None else (palette.KEY_DIR if mode == "object" else palette.SUN_DIR)
    L = vnorm(L)
    sc = sun if sun is not None else palette.C["sun"]
    am = ambient if ambient is not None else palette.C["ambient"]
    if exposure is None:
        top = max(am[0] + sc[0], am[1] + sc[1], am[2] + sc[2])
        exposure = 255.0 / top
    if wrap is None:
        wrap = WRAP_OBJECT if mode == "object" else 0.0
    out = []
    for i, n in enumerate(geo.N):
        sky = 1.0 - hemi + hemi * (n[1] * 0.5 + 0.5)
        d = max(0.0, (vdot(n, L) + wrap) / (1.0 + wrap)) * shadow
        ao = 1.0
        if ao_height > 0:
            ao = ao_min + (1.0 - ao_min) * _smoothstep(0.0, ao_height, geo.P[i][1] - ground_y)
        base = geo.C[i] if multiply else WHITE
        col = []
        for ch in range(3):
            l = (am[ch] * sky + sc[ch] * d) * exposure / 255.0 * ao
            col.append(palette.clamp8(base[ch] * l))
        out.append((col[0], col[1], col[2], base[3]))
    geo.C = out
    return geo


# ---------------------------------------------------------------------------------------------------- LOD helpers

def pick_even(seq, n, keep=()):
    """n items of seq spread evenly, always including the first, the last and the indices in keep."""
    L = len(seq)
    if n >= L:
        return list(seq)
    sel = set([0, L - 1])
    for k in keep:
        sel.add(k % L)
    if n > 1:
        for i in range(n):
            sel.add(int(round(i * (L - 1) / float(n - 1))))
    return [seq[i] for i in sorted(sel)]


def reduce_stations(stations, n, keep=()):
    """Keep n loft stations (first, last and keep indices always kept)."""
    return pick_even(stations, n, keep)


def reduce_points(stations, n, keep=()):
    """Keep n points per station (the same indices on every station; first/last/keep kept)."""
    idx = pick_even(list(range(len(stations[0]))), n, keep)
    return [[s[i] for i in idx] for s in stations]


def decimate(geo, target_tris, lock_seams=True, lock_boundary=True, max_angle=60.0, crease=None):
    """Edge-collapse decimation to about target_tris (half-edge collapses, cheapest first; cost = edge
    length x normal change). Never moves seam vertices (same position, different UV/normal) or open
    boundary vertices (with the lock flags), never flips a triangle more than max_angle degrees. Returns
    a NEW Geo; crease=<deg> recomputes normals afterwards (None keeps the old ones)."""
    g = geo.copy().weld()
    nv = len(g.P)
    tris = [list(t) for t in g.T]
    alive = [True] * len(tris)
    adj = [set() for _ in range(nv)]
    for t, tri in enumerate(tris):
        for v in tri:
            adj[v].add(t)
    poscount = {}
    pk = [_qk(p, 1e5) for p in g.P]
    for k in pk:
        poscount[k] = poscount.get(k, 0) + 1
    locked = [False] * nv
    if lock_seams:
        for v in range(nv):
            if poscount[pk[v]] > 1:
                locked[v] = True
    if lock_boundary:
        ec = {}
        for tri in tris:
            for a, b in ((tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0])):
                k = (a, b) if a < b else (b, a)
                ec[k] = ec.get(k, 0) + 1
        for (a, b), c in ec.items():
            if c != 2:
                locked[a] = locked[b] = True
    cos_max = math.cos(math.radians(max_angle))
    ver = [0] * nv

    def neigh(v):
        s = set()
        for t in adj[v]:
            s.update(tris[t])
        s.discard(v)
        return s

    def cost(u, v):
        if locked[u] or u == v:
            return None
        shared = [t for t in adj[u] if v in tris[t]]
        if not shared or len(shared) > 2:
            return None
        nu, nvv = neigh(u), neigh(v)
        common = nu & nvv
        opp = set()
        for t in shared:
            for x in tris[t]:
                if x != u and x != v:
                    opp.add(x)
        if common != opp:
            return None
        dev = 0.0
        pv = g.P[v]
        for t in adj[u]:
            if t in shared:
                continue
            a, b, c = tris[t]
            old = face_normal(g.P[a], g.P[b], g.P[c])
            pa = pv if a == u else g.P[a]
            pb = pv if b == u else g.P[b]
            pc = pv if c == u else g.P[c]
            cr = vcross(vsub(pb, pa), vsub(pc, pa))
            if vlen(cr) < 1e-12:
                return None
            d = vdot(vnorm(cr), old)
            if d < cos_max:
                return None
            dev += 1.0 - d
        return vlen(vsub(g.P[u], pv)) * (1.0 + 8.0 * dev)

    heap = []
    seq = 0
    for t, tri in enumerate(tris):
        for a, b in ((tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0])):
            for u, v in ((a, b), (b, a)):
                c = cost(u, v)
                if c is not None:
                    heapq.heappush(heap, (c, seq, u, v, ver[u], ver[v]))
                    seq += 1
    count = sum(1 for a in alive if a)
    while count > target_tris and heap:
        c, _, u, v, vu, vv = heapq.heappop(heap)
        if ver[u] != vu or ver[v] != vv or not adj[u]:
            if adj[u] and adj[v]:
                c2 = cost(u, v)
                if c2 is not None:
                    heapq.heappush(heap, (c2, seq, u, v, ver[u], ver[v]))
                    seq += 1
            continue
        c2 = cost(u, v)
        if c2 is None:
            continue
        for t in list(adj[u]):
            if v in tris[t]:
                alive[t] = False
                count -= 1
                for x in tris[t]:
                    adj[x].discard(t)
            else:
                tris[t] = [v if x == u else x for x in tris[t]]
                adj[v].add(t)
        adj[u] = set()
        ver[u] += 1
        ns = neigh(v)
        ver[v] += 1
        for x in ns:
            ver[x] += 1
        for x in ns | set([v]):
            for y in neigh(x):
                for a, b in ((x, y), (y, x)):
                    c3 = cost(a, b)
                    if c3 is not None:
                        heapq.heappush(heap, (c3, seq, a, b, ver[a], ver[b]))
                        seq += 1
    out = Geo(g.mat)
    out.P, out.N, out.UV, out.UV2, out.C = g.P, g.N, g.UV, g.UV2, g.C
    for t, tri in enumerate(tris):
        if alive[t]:
            out.T.append(tuple(tri))
            out.TM.append(g.TM[t])
            out.TS.append(g.TS[t])
    out.compact()
    if crease is not None:
        out.normals(crease)
    return out


# ---------------------------------------------------------------------------------------------------- builder

class MeshBuilder(object):
    """Collects materials, LODs (each a Geo) and tags, and writes a .msh (DESIGN 8.3).

        mb = MeshBuilder("sample_house")
        wall = mb.material("wall", tex="sample_wall")
        sign = mb.material("sign", tex="sample_atlas", blend=RB_ALPHATEST, cull_none=True)
        mb.lod(60.0);  mb.add(house_geo)      # LOD0 drawn up to 60 m (x lodScale)
        mb.lod();      mb.add(low_geo)        # last LOD: no limit
        mb.tag("door", (0, 0, -4), (0, 0, -1))
        mb.save(path)  /  mb.to_msh()  /  mb.tri_counts()
    """

    def __init__(self, name):
        self.name = name
        self.mats = []
        self.lods = []      # [max_dist or None, Geo]
        self.tags = []

    def material(self, name, tex, tex1="", blend=RB_OPAQUE, stage1=RS1_NONE, cull_none=False, flags=0):
        """Add (or return the existing) material `name`. tex/tex1: texture names without path/extension
        (data/tex/<tex>.bmp). blend RB_*, stage1 RS1_*, flags MM_* (MM_ENV paint, MM_GLASS, MM_WHEEL...)."""
        for m in self.mats:
            if m.name == name:
                return m
        m = Material(name, tex, tex1, blend, stage1, cull_none, flags)
        m.index = len(self.mats)
        self.mats.append(m)
        return m

    def lod(self, max_dist=None):
        """Start a new LOD; following add() calls go into it. max_dist = drawn while distance < max_dist
        (x the preset lodScale); None = no limit (use for the last LOD). Returns the LOD index."""
        self.lods.append([max_dist, Geo()])
        return len(self.lods) - 1

    def _resolve(self, m):
        if isinstance(m, Material):
            if 0 <= m.index < len(self.mats) and self.mats[m.index] is m:
                return m.index
            m = m.name
        if m is None:
            if len(self.mats) == 1:
                return 0
            raise ValueError("triangle without a material (builder has %d materials)" % len(self.mats))
        if isinstance(m, int):
            return m
        for x in self.mats:
            if x.name == m:
                return x.index
        raise ValueError("unknown material %r" % (m,))

    def add(self, geo, lod=None):
        """Add a Geo to the current (or given) LOD. Materials resolve by object or name."""
        if not self.lods:
            self.lod(None)
        li = len(self.lods) - 1 if lod is None else lod
        g = geo.copy()
        g.TM = [self._resolve(m) for m in g.TM]
        self.lods[li][1].merge(g)
        return self

    def tag(self, name, pos, direction=(0.0, 0.0, 1.0)):
        """Named point + direction (wheel_fl, brake_l, cam_bumper, ...); name <= 15 characters."""
        if len(name) > 15:
            raise ValueError("tag name %r longer than 15 characters" % name)
        self.tags.append((name, tuple(float(x) for x in pos), tuple(float(x) for x in direction)))
        return self

    def geo(self, lod=0):
        """The Geo of one LOD (live object)."""
        return self.lods[lod][1]

    def tri_counts(self):
        """Triangles per LOD, e.g. [702, 258, 92]."""
        return [len(g.T) for _, g in self.lods]

    def to_msh(self):
        """Build the export.MshFile: per LOD identical vertices shared, triangles sorted by material into
        parts, indices relative to the LOD's first vertex."""
        m = export.MshFile()
        m.name = self.name
        m.mats = [x.record() for x in self.mats]
        for li, (maxd, g) in enumerate(self.lods):
            order = sorted(range(len(g.T)), key=lambda t: (g.TM[t], t))
            first_vert = len(m.verts)
            first_part = len(m.parts)
            vmap = {}
            cur_mat = None
            part_start = len(m.idx)
            for t in order:
                mt = g.TM[t]
                if mt != cur_mat:
                    if cur_mat is not None and len(m.idx) > part_start:
                        m.parts.append((cur_mat, part_start, len(m.idx) - part_start))
                    cur_mat = mt
                    part_start = len(m.idx)
                for v in g.T[t]:
                    rv = g.rvert(v)
                    n = g.N[v]
                    rn = (f32(n[0]), f32(n[1]), f32(n[2]))
                    key = (rv, rn)
                    j = vmap.get(key)
                    if j is None:
                        j = len(m.verts) - first_vert
                        vmap[key] = j
                        m.verts.append(rv)
                        m.normals.append(rn)
                    m.idx.append(j)
            if cur_mat is not None and len(m.idx) > part_start:
                m.parts.append((cur_mat, part_start, len(m.idx) - part_start))
            nverts = len(m.verts) - first_vert
            if nverts > 65535:
                raise ValueError("LOD %d of %s has %d vertices (max 65535)" % (li, self.name, nverts))
            md = maxd if maxd is not None else MSH_NO_LIMIT
            m.lods.append((f32(md), first_part, len(m.parts) - first_part, first_vert, nverts))
        m.tags = [(n, (f32(p[0]), f32(p[1]), f32(p[2])), (f32(d[0]), f32(d[1]), f32(d[2]))) for n, p, d in self.tags]
        m.compute_bounds()
        return m

    def save(self, path):
        """Write the .msh; returns the MshFile written."""
        m = self.to_msh()
        export.write_msh(path, m)
        return m
