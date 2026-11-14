"""Software rasteriser to PNG, so asset agents check models without the VM.

Z-buffer, perspective-correct texturing (nearest texel), alpha test (magenta key / _a.bmp alpha < 128),
back-face culling by material, lambert + ambient for white (CPU-lit, e.g. car) meshes or the baked vertex
colours for pre-lit meshes, optional linear fog, optional 16-bit (565 + dither) quantise.

Inputs ("obj"): a mesh.MeshBuilder, an export.MshFile, a mesh.Geo, or a path to a .msh file.

    from assetkit import preview as V
    V.save(V.views(mb), "tools/art/preview/barn_views.png")            # front/back/side/top/3-4/wire
    V.save(V.turntable(mb, 8), "tools/art/preview/barn_turn.png")
    V.save(V.lods(mb), "tools/art/preview/car_lods.png")
    V.save(V.scene([(road, (0,0,0), 0), (barn, (12,0,30), -20)], eye=(0,1.2,-8), target=(0,1,20)), path)
    V.save(V.contact_sheet(["data/tex/a.bmp", "data/tex/b.bmp"]), "tools/art/preview/tex.png")
    img = V.render([(mb, (0,0,0), 0)], V.Camera((3,2,-5), (0,0.8,0)), size=(512, 512))
    img = V.render([road_geo], cam, mats=setting.MATERIALS)   # Geo materials as names + a material table
    V.label(img, "chase camera", color=(240, 240, 240))       # text label on any sheet
"""

import math
import os

from PIL import Image, ImageDraw, ImageFont

from . import export
from . import image as _img
from . import mesh as _mesh
from . import palette

BG = (178, 182, 188)


def _font(size=12):
    try:
        return ImageFont.load_default(size)
    except TypeError:
        return ImageFont.load_default()


# ---------------------------------------------------------------------------------------------------- camera

class Camera(object):
    """eye, target, up; fov = vertical field of view in degrees, or ortho = half height in metres (then
    an orthographic camera)."""

    def __init__(self, eye, target, up=(0.0, 1.0, 0.0), fov=45.0, ortho=None, near=0.05):
        self.eye = tuple(eye)
        self.target = tuple(target)
        f = _mesh.vnorm(_mesh.vsub(target, eye), (0.0, 0.0, 1.0))
        r = _mesh.vcross(up, f)
        if _mesh.vlen(r) < 1e-6:
            r = _mesh.vcross((0.0, 0.0, 1.0), f)
        r = _mesh.vnorm(r)
        u = _mesh.vcross(f, r)
        self.f, self.r, self.u = f, r, u
        self.fov = fov
        self.ortho = ortho
        self.near = near

    def to_view(self, p):
        d = (p[0] - self.eye[0], p[1] - self.eye[1], p[2] - self.eye[2])
        return (_mesh.vdot(d, self.r), _mesh.vdot(d, self.u), _mesh.vdot(d, self.f))


# ---------------------------------------------------------------------------------------------------- sources

class _Tex(object):
    def __init__(self, img):
        rgba = img.convert("RGBA")
        self.img = rgba
        self.w, self.h = rgba.size
        self.px = list(rgba.getdata())
        self._levels = None

    def levels(self):
        """Mip chain [(w, h, px)] like the engine's img_gen_mips (image.mip_chain, alpha-weighted)."""
        if self._levels is None:
            self._levels = [(l.size[0], l.size[1], list(l.getdata())) for l in _img.mip_chain(self.img, False)]
        return self._levels


_TEX_CACHE = {}


def _default_tex_dirs():
    from . import project_root
    return [os.path.join(project_root(), "data", "tex")]


def _get_tex(name, tex_dirs):
    if not name:
        return None
    path = None
    for d in tex_dirs:
        p = os.path.join(d, name + ".bmp")
        if os.path.exists(p):
            path = p
            break
    stamp = None
    if path is not None:
        st = os.stat(path)
        ap = _img.alpha_path(path)
        stamp = (path, st.st_mtime, st.st_size, os.path.exists(ap) and os.stat(ap).st_mtime)
    key = (name, stamp)
    if key in _TEX_CACHE:
        return _TEX_CACHE[key]
    t = None
    if path is not None:
        try:
            t = _Tex(_img.load_texture(path))
        except ValueError:
            t = None
    if t is None:
        t = _Tex(_img.checker(16, 16, 4, (200, 60, 200), (120, 120, 120)))
        t.missing = True
    _TEX_CACHE[key] = t
    return t


def clear_cache():
    """Forget loaded textures (call after rewriting textures in the same process)."""
    _TEX_CACHE.clear()


def _as_msh(obj):
    if isinstance(obj, str):
        return export.read_msh(obj)
    if isinstance(obj, _mesh.MeshBuilder):
        return obj.to_msh()
    return obj


_NO_MAT = {"tex": "", "tex1": "", "blend": 0, "stage1": 0, "cullNone": 0, "flags": 0}


def material_table(mats):
    """Normalise a material table for render(mats=...): a dict {name: m} or a list of m, where m is a
    mesh.Material, a record dict ({"name", "tex", "blend", "cullNone", ...}; missing fields default) or a tuple
    (name, tex[, blend[, cull_none[, ...]]]) like trackc's setting.MATERIALS. -> {name: record dict}."""
    if mats is None:
        return {}
    items = list(mats.items()) if isinstance(mats, dict) else [(None, m) for m in mats]
    out = {}
    for name, m in items:
        if isinstance(m, _mesh.Material):
            rec = m.record()
            nm = m.name
        elif isinstance(m, dict):
            rec = dict(_NO_MAT)
            rec.update(m)
            if "cull_none" in m:
                rec["cullNone"] = 1 if m["cull_none"] else 0
            nm = m.get("name")
        else:
            m = tuple(m)
            rec = dict(_NO_MAT)
            rec["tex"] = m[1]
            if len(m) > 2:
                rec["blend"] = m[2]
            if len(m) > 3:
                rec["cullNone"] = 1 if m[3] else 0
            nm = m[0]
        key = name if name is not None else nm
        if key is None:
            raise ValueError("material_table: entry without a name: %r" % (m,))
        out[key] = rec
    return out


def _triangles(obj, lod=0, mats=None):
    """-> list of (p3, n3, uv3, col3 (r,g,b 0..255), mat dict). mats: material_table() result used for Geo
    triangles whose material is a name (or a Material, overriding it by name)."""
    out = []
    if isinstance(obj, _mesh.Geo):
        for t, (a, b, c) in enumerate(obj.T):
            m = obj.TM[t]
            k = _mesh.mat_key(m)
            if mats and k in mats:
                md = mats[k]
            elif isinstance(m, _mesh.Material):
                md = m.record()
            else:
                md = _NO_MAT
            out.append(((obj.P[a], obj.P[b], obj.P[c]), (obj.N[a], obj.N[b], obj.N[c]),
                        (obj.UV[a], obj.UV[b], obj.UV[c]), (obj.C[a][:3], obj.C[b][:3], obj.C[c][:3]), md))
        return out
    m = _as_msh(obj)
    lod = min(lod, len(m.lods) - 1)
    for mat, a, b, c in m.lod_triangles(lod):
        vs = [m.verts[a], m.verts[b], m.verts[c]]
        ns = [m.normals[a], m.normals[b], m.normals[c]]
        cols = [palette.from_argb(v[3])[:3] for v in vs]
        out.append((tuple(v[:3] for v in vs), tuple(ns), tuple((v[4], v[5]) for v in vs), tuple(cols),
                    m.mats[mat]))
    return out


def _place(tris, pos, yaw, scale):
    if pos == (0.0, 0.0, 0.0) and not yaw and scale == 1.0:
        return tris
    c = math.cos(math.radians(yaw))
    s = math.sin(math.radians(yaw))

    def fp(p):
        x, y, z = p[0] * scale, p[1] * scale, p[2] * scale
        return (x * c + z * s + pos[0], y + pos[1], -x * s + z * c + pos[2])

    def fn(n):
        return (n[0] * c + n[2] * s, n[1], -n[0] * s + n[2] * c)
    return [(tuple(fp(p) for p in t[0]), tuple(fn(n) for n in t[1]), t[2], t[3], t[4]) for t in tris]


def bounds_of(obj, lod=0):
    tris = _triangles(obj, lod)
    if not tris:
        return ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0))
    xs = [p[0] for t in tris for p in t[0]]
    ys = [p[1] for t in tris for p in t[0]]
    zs = [p[2] for t in tris for p in t[0]]
    return ((min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs)))


# ---------------------------------------------------------------------------------------------------- render

def render(items, camera, size=(512, 512), bg=BG, light=None, ambient=None, sun=None, fog=None, lod=0,
           tex_dirs=None, wire=False, q16=False, lighting="auto", textured=True, mats=None, mips=False,
           mip_bias=0.0):
    """Render items -> RGB Image.
    items: list of obj or (obj, pos) / (obj, pos, yaw) / (obj, pos, yaw, scale).
    bg: colour or an Image (sky) of the same size. light: direction toward the light for lambert lighting
    (default palette.KEY_DIR flipped to the camera's side is NOT done: it is fixed, front-left-above).
    lighting: 'auto' (lambert if the mesh's vertex colours are all white, else vertex colours),
    'vertex', 'lambert'. fog: (start, end[, colour]) in metres of view depth. wire: overlay edges.
    mats: material table for Geos whose triangle materials are NAMES (trackc): {name: Material | record
    dict | (name, tex, blend, cull_none)} or a list of those (see material_table); names not in it render
    untextured. A Material object in a Geo is also looked up by its name first.
    mips=True: per-pixel mip selection (isotropic LOD from the larger UV derivative, nearest mip level, point
    sampled, levels = image.mip_chain like the engine): shows minification shimmer/thinning of distant
    textures and foliage cut-outs; mip_bias shifts the LOD (negative = sharper, the D3D LOD-bias sense).
    Default False = level 0 only (the 1.1 behaviour, pixel-identical)."""
    W, H = size
    mtab = material_table(mats)
    tex_dirs = list(tex_dirs) if tex_dirs else _default_tex_dirs()
    L = _mesh.vnorm(light if light is not None else palette.KEY_DIR)
    am = ambient if ambient is not None else palette.C["ambient"]
    sc = sun if sun is not None else palette.C["sun"]
    expo = 1.0 / max(am[0] + sc[0], am[1] + sc[1], am[2] + sc[2])
    if isinstance(bg, Image.Image):
        fb = bytearray(bg.convert("RGB").resize((W, H)).tobytes())
    else:
        fb = bytearray(bytes(bg[:3]) * (W * H))
    zb = [-1e30] * (W * H)
    cam = camera
    persp = cam.ortho is None
    if persp:
        fpx = (H * 0.5) / math.tan(math.radians(cam.fov) * 0.5)
    else:
        fpx = (H * 0.5) / cam.ortho
    cx, cy = W * 0.5, H * 0.5
    if fog is not None:
        fs, fe = fog[0], fog[1]
        fc = fog[2] if len(fog) > 2 else palette.C["fog"]
    edges = []

    norm_items = []
    for it in items:
        if not isinstance(it, tuple):
            it = (it,)
        obj = it[0]
        pos = tuple(it[1]) if len(it) > 1 else (0.0, 0.0, 0.0)
        yaw = it[2] if len(it) > 2 else 0.0
        scl = it[3] if len(it) > 3 else 1.0
        norm_items.append((obj, pos, yaw, scl))

    for obj, pos, yaw, scl in norm_items:
        tris = _place(_triangles(obj, lod, mtab), pos, yaw, scl)
        if lighting == "auto":
            lit = all(c == (255, 255, 255) for t in tris for c in t[3])
        else:
            lit = lighting == "lambert"
        for P3, N3, UV3, C3, md in tris:
            vp = [cam.to_view(p) for p in P3]
            fn = _mesh.vcross(_mesh.vsub(vp[1], vp[0]), _mesh.vsub(vp[2], vp[0]))
            front = (_mesh.vdot(fn, vp[0]) < 0) if persp else (fn[2] < 0)
            if not front and not md.get("cullNone"):
                continue
            cols = []
            for k in range(3):
                c = C3[k]
                if lit:
                    n = N3[k]
                    if not front:
                        n = (-n[0], -n[1], -n[2])
                    d = max(0.0, _mesh.vdot(n, L))
                    sky = 0.65 + 0.35 * (n[1] * 0.5 + 0.5)
                    cols.append(tuple(c[i] / 255.0 * (am[i] * sky + sc[i] * d) * expo for i in range(3)))
                else:
                    cols.append((c[0] / 255.0, c[1] / 255.0, c[2] / 255.0))
            tex = _get_tex(md.get("tex", ""), tex_dirs) if textured else None
            blend = md.get("blend", 0)
            atest = blend in (export.RB_ALPHATEST, export.RB_BLEND, export.RB_SHADOW)
            additive = blend == export.RB_ADD
            # near-plane clip (perspective)
            verts = [(vp[k], UV3[k], cols[k]) for k in range(3)]
            if persp:
                verts = _clip_near(verts, cam.near)
                if len(verts) < 3:
                    continue
            sv = []
            for (x, y, z), uv, col in verts:
                if persp:
                    w = 1.0 / z
                    sv.append((cx + x * fpx * w, cy - y * fpx * w, w, z, uv, col))
                else:
                    sv.append((cx + x * fpx, cy - y * fpx, 1.0, z, uv, col))
            for k in range(1, len(sv) - 1):
                _raster(fb, zb, W, H, sv[0], sv[k], sv[k + 1], tex, atest, additive, persp,
                        fog is not None and (fs, fe, fc) or None, mip_bias if (mips and tex is not None) else None)
            if wire:
                for k in range(len(sv)):
                    a, b = sv[k], sv[(k + 1) % len(sv)]
                    edges.append((a[0], a[1], b[0], b[1]))
    img = Image.frombytes("RGB", (W, H), bytes(fb))
    if wire and edges:
        d = ImageDraw.Draw(img)
        for e in edges:
            d.line(e, fill=(20, 20, 30), width=1)
    if q16:
        img = _img.to16(img, "565", dither=True)
    return img


def _clip_near(verts, near):
    out = []
    n = len(verts)
    for i in range(n):
        a = verts[i]
        b = verts[(i + 1) % n]
        ina = a[0][2] >= near
        inb = b[0][2] >= near
        if ina:
            out.append(a)
        if ina != inb:
            t = (near - a[0][2]) / (b[0][2] - a[0][2])
            p = _mesh.vlerp(a[0], b[0], t)
            uv = (a[1][0] + (b[1][0] - a[1][0]) * t, a[1][1] + (b[1][1] - a[1][1]) * t)
            col = tuple(a[2][i2] + (b[2][i2] - a[2][i2]) * t for i2 in range(3))
            out.append((p, uv, col))
    return out


def _raster(fb, zb, W, H, v0, v1, v2, tex, atest, additive, persp, fog, mip=None):
    x0, y0, w0, z0, uv0, c0 = v0
    x1, y1, w1, z1, uv1, c1 = v1
    x2, y2, w2, z2, uv2, c2 = v2
    area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0)
    if abs(area) < 1e-9:
        return
    ia = 1.0 / area
    # barycentric planes: b_i(x, y) = A_i x + B_i y + C_i
    A0 = (y1 - y2) * ia
    B0 = (x2 - x1) * ia
    C0 = (x1 * y2 - x2 * y1) * ia
    A1 = (y2 - y0) * ia
    B1 = (x0 - x2) * ia
    C1 = (x2 * y0 - x0 * y2) * ia
    A2 = (y0 - y1) * ia
    B2 = (x1 - x0) * ia
    C2 = (x0 * y1 - x1 * y0) * ia
    ymin = max(0, int(math.floor(min(y0, y1, y2))))
    ymax = min(H - 1, int(math.ceil(max(y0, y1, y2))))
    xmin_t = max(0, int(math.floor(min(x0, x1, x2))))
    xmax_t = min(W - 1, int(math.ceil(max(x0, x1, x2))))
    if ymin > ymax or xmin_t > xmax_t:
        return
    # perspective-correct attributes: interpolate attr * w
    u0w, v0w = uv0[0] * w0, uv0[1] * w0
    u1w, v1w = uv1[0] * w1, uv1[1] * w1
    u2w, v2w = uv2[0] * w2, uv2[1] * w2
    if tex is not None:
        tw, th, tpx = tex.w, tex.h, tex.px
        offu = tw * 4096
        offv = th * 4096
    if mip is not None:
        levs = tex.levels()
        nlev = len(levs)
        # screen derivatives of u*w, v*w and w (constant per triangle): d(u)/dx = (Au - u * Aw) / w
        Au = A0 * u0w + A1 * u1w + A2 * u2w
        Bu = B0 * u0w + B1 * u1w + B2 * u2w
        Av = A0 * v0w + A1 * v1w + A2 * v2w
        Bv = B0 * v0w + B1 * v1w + B2 * v2w
        Aw = A0 * w0 + A1 * w1 + A2 * w2
        Bw = B0 * w0 + B1 * w1 + B2 * w2
        l2 = 0.5 / math.log(2.0)
    planes = ((A0, B0, C0), (A1, B1, C1), (A2, B2, C2))
    for py in range(ymin, ymax + 1):
        fy = py + 0.5
        xl = xmin_t + 0.5
        xr = xmax_t + 0.5
        ok = True
        for A, B, C in planes:
            k = B * fy + C
            if A > 1e-12:
                xl = max(xl, -k / A)
            elif A < -1e-12:
                xr = min(xr, -k / A)
            elif k < -1e-9:
                ok = False
                break
        if not ok or xl > xr:
            continue
        pxs = int(math.ceil(xl - 0.5))
        pxe = int(math.floor(xr - 0.5))
        if pxs < xmin_t:
            pxs = xmin_t
        if pxe > xmax_t:
            pxe = xmax_t
        row = py * W
        fx = pxs + 0.5
        b0 = A0 * fx + B0 * fy + C0
        b1 = A1 * fx + B1 * fy + C1
        b2 = A2 * fx + B2 * fy + C2
        for px in range(pxs, pxe + 1):
            iw = b0 * w0 + b1 * w1 + b2 * w2
            if persp:
                depth = iw
            else:
                depth = -(b0 * z0 + b1 * z1 + b2 * z2)
            i = row + px
            if depth > zb[i] and iw > 0:
                zz = 1.0 / iw
                if tex is not None:
                    u = (b0 * u0w + b1 * u1w + b2 * u2w) * zz
                    v = (b0 * v0w + b1 * v1w + b2 * v2w) * zz
                    if mip is None:
                        t = tpx[(int(v * th + offv) % th) * tw + (int(u * tw + offu) % tw)]
                    else:
                        dux = (Au - u * Aw) * zz * tw
                        dvx = (Av - v * Aw) * zz * th
                        duy = (Bu - u * Bw) * zz * tw
                        dvy = (Bv - v * Bw) * zz * th
                        rho2 = max(dux * dux + dvx * dvx, duy * duy + dvy * dvy)
                        lv = 0
                        if rho2 > 1e-12:
                            lv = int(math.log(rho2) * l2 + mip + 0.5)
                            lv = 0 if lv < 0 else (nlev - 1 if lv >= nlev else lv)
                        mw, mh, mpx = levs[lv]
                        t = mpx[(int(v * mh + mh * 4096) % mh) * mw + (int(u * mw + mw * 4096) % mw)]
                    if atest and t[3] < 128:
                        b0 += A0
                        b1 += A1
                        b2 += A2
                        continue
                    tr, tg, tb = t[0], t[1], t[2]
                else:
                    tr = tg = tb = 200
                cr = (b0 * c0[0] * w0 + b1 * c1[0] * w1 + b2 * c2[0] * w2) * zz
                cg = (b0 * c0[1] * w0 + b1 * c1[1] * w1 + b2 * c2[1] * w2) * zz
                cb = (b0 * c0[2] * w0 + b1 * c1[2] * w1 + b2 * c2[2] * w2) * zz
                r = tr * cr
                g = tg * cg
                b = tb * cb
                if fog is not None:
                    vz = zz if persp else (b0 * z0 + b1 * z1 + b2 * z2)
                    f = (vz - fog[0]) / (fog[1] - fog[0])
                    if f > 0:
                        if f > 1:
                            f = 1.0
                        fc = fog[2]
                        r += (fc[0] - r) * f
                        g += (fc[1] - g) * f
                        b += (fc[2] - b) * f
                j = i * 3
                if additive:
                    r += fb[j]
                    g += fb[j + 1]
                    b += fb[j + 2]
                else:
                    zb[i] = depth
                fb[j] = 255 if r > 255 else (0 if r < 0 else int(r))
                fb[j + 1] = 255 if g > 255 else (0 if g < 0 else int(g))
                fb[j + 2] = 255 if b > 255 else (0 if b < 0 else int(b))
            b0 += A0
            b1 += A1
            b2 += A2


# ---------------------------------------------------------------------------------------------------- sheets

def label(img, text, xy=(4, 3), color=(15, 15, 20), size=12):
    """Draw a text label on a PIL image (default: top-left, dark grey, the sheets' 12 px font). Returns img.
    Use a light colour (240, 240, 240) on dark backgrounds."""
    d = ImageDraw.Draw(img)
    d.text(xy, text, fill=color, font=_font(size))
    return img


_label = label


def _fit(lo, hi):
    c = ((lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5)
    r = max(0.05, 0.5 * _mesh.vlen(_mesh.vsub(hi, lo)))
    return c, r


def _tri_text(obj, lod):
    if isinstance(obj, _mesh.Geo):
        return "%d tris" % obj.ntris
    m = _as_msh(obj)
    counts = m.tri_counts()
    return "LOD%d %d tris (LODs %s)" % (min(lod, len(counts) - 1), counts[min(lod, len(counts) - 1)],
                                        "/".join(str(c) for c in counts))


def views(obj, size=512, lod=0, tex_dirs=None, title=None, q16=False, light=None, facing="-z", mats=None):
    """Sheet (3 x 2 panels of size px): orthographic from +Z, from -Z, from +X (right side), from above, plus
    a 3/4 perspective and a 3/4 wireframe from the side the object faces (facing='-z' for objects/signs whose
    front faces -Z, '+z' for cars), labelled with triangle counts."""
    lo, hi = bounds_of(obj, lod)
    c, r = _fit(lo, hi)
    oh = r * 1.1
    dist = r * 3.2
    cams = [
        ("from +Z (car nose)", Camera((c[0], c[1], c[2] + dist), c, ortho=oh)),
        ("from -Z (object front)", Camera((c[0], c[1], c[2] - dist), c, ortho=oh)),
        ("from +X (right side)", Camera((c[0] + dist, c[1], c[2]), c, ortho=oh)),
        ("top (+Y, north up)", Camera((c[0], c[1] + dist, c[2]), c, up=(0.0, 0.0, 1.0), ortho=oh)),
        ("3/4 front (persp)", None),
        ("3/4 wire", None),
    ]
    a = math.radians(35.0 if facing == "+z" else 145.0)
    eye = (c[0] + math.sin(a) * dist * 1.25, c[1] + dist * 0.55, c[2] + math.cos(a) * dist * 1.25)
    sheet = Image.new("RGB", (size * 3, size * 2), (90, 90, 96))
    for i, (name, cam) in enumerate(cams):
        if cam is None:
            cam = Camera(eye, c, fov=34.0)
        img = render([obj], cam, (size, size), lod=lod, tex_dirs=tex_dirs, wire=(i == 5), q16=q16, light=light,
                     textured=(i != 5), mats=mats)
        _label(img, name)
        sheet.paste(img, ((i % 3) * size, (i // 3) * size))
    head = (title + "  " if title else "") + _tri_text(obj, lod)
    _label(sheet, head, (6, size * 2 - 18), (240, 240, 240))
    return sheet


def turntable(obj, n=8, size=256, elev=20.0, lod=0, tex_dirs=None, title=None, q16=False, light=None,
              facing="-z", mats=None):
    """n perspective views around the object (camera yaw steps of 360/n, starting in front of it: facing
    '-z' for objects, '+z' for cars), up to 4 per row."""
    lo, hi = bounds_of(obj, lod)
    c, r = _fit(lo, hi)
    dist = r * 3.6
    cols = min(4, n)
    rows = (n + cols - 1) // cols
    sheet = Image.new("RGB", (cols * size, rows * size), (90, 90, 96))
    e = math.radians(elev)
    for i in range(n):
        a = 2.0 * math.pi * i / n + (0.0 if facing == "+z" else math.pi)
        eye = (c[0] + math.sin(a) * math.cos(e) * dist, c[1] + math.sin(e) * dist, c[2] + math.cos(a) * math.cos(e) * dist)
        img = render([obj], Camera(eye, c, fov=34.0), (size, size), lod=lod, tex_dirs=tex_dirs, q16=q16, light=light,
                     mats=mats)
        _label(img, "%d deg" % int(round(360.0 * i / n)))
        sheet.paste(img, ((i % cols) * size, (i // cols) * size))
    if title:
        _label(sheet, title, (6, rows * size - 18), (240, 240, 240))
    return sheet


def lods(obj, size=320, tex_dirs=None, title=None, light=None, facing="-z"):
    """Every LOD side by side (3/4 view, same camera) with triangle counts; second row wireframe."""
    m = _as_msh(obj)
    n = len(m.lods)
    lo, hi = bounds_of(m, 0)
    c, r = _fit(lo, hi)
    dist = r * 3.6
    a = math.radians(35.0 if facing == "+z" else 145.0)
    eye = (c[0] + math.sin(a) * dist, c[1] + dist * 0.45, c[2] + math.cos(a) * dist)
    sheet = Image.new("RGB", (n * size, 2 * size), (90, 90, 96))
    for i in range(n):
        for row in range(2):
            img = render([m], Camera(eye, c, fov=34.0), (size, size), lod=i, tex_dirs=tex_dirs, wire=(row == 1),
                         textured=(row == 0), light=light)
            md = m.lods[i][0]
            _label(img, "LOD%d  %d tris" % (i, m.lod_tris(i)))
            _label(img, "last (no limit)" if md >= export.MSH_NO_LIMIT * 0.5 else "to %.0f m" % md, (4, 17))
            sheet.paste(img, (i * size, row * size))
    if title:
        _label(sheet, title, (6, 2 * size - 18), (240, 240, 240))
    return sheet


def sky_image(w, h, top=None, bottom=None):
    """Vertical gradient background (zenith -> horizon colours)."""
    return _img.gradient(w, h, top or palette.C["zenith"], bottom or palette.C["horizon"])


def scene(items, eye, target, size=(640, 480), fov=60.0, fog=(60.0, 300.0), ground=None, sky=True, lod=0,
          tex_dirs=None, q16=False, light=None, lighting="auto", mats=None, mips=False, mip_bias=0.0):
    """Mock composition (e.g. a road strip with trees and a barn): items as in render(); ground = None or
    (colour, half_size) for a flat untextured plane at y=0; sky gradient background; fog (start, end);
    mips/mip_bias as in render()."""
    W, H = size
    objs = list(items)
    if ground is not None:
        col, hs = ground
        g = _mesh.quad((-hs, 0.0, -hs), (-hs, 0.0, hs), (hs, 0.0, hs), (hs, 0.0, -hs))
        g.set_color(col)
        g.TM = [None] * g.ntris
        objs.insert(0, (g, (0.0, 0.0, 0.0), 0.0))
    bg = sky_image(W, H) if sky else BG
    return render(objs, Camera(eye, target, fov=fov), size, bg=bg, fog=fog, lod=lod, tex_dirs=tex_dirs, q16=q16,
                  light=light if light is not None else palette.SUN_DIR, lighting=lighting, mats=mats, mips=mips,
                  mip_bias=mip_bias)


def contact_sheet(textures, cols=4, cell=160, title=None, mips=False):
    """Grid of textures over a checkerboard (key/alpha visible) with name and size labels.
    textures: paths to .bmp files or (label, PIL image) pairs. mips=True shows the mip chain below each."""
    entries = []
    for t in textures:
        if isinstance(t, str):
            entries.append((os.path.splitext(os.path.basename(t))[0], _img.load_texture(t)))
        else:
            entries.append((t[0], t[1].convert("RGBA")))
    n = max(1, len(entries))
    rows = (n + cols - 1) // cols
    ch = cell + 18 + (cell // 2 if mips else 0)
    sheet = Image.new("RGB", (cols * cell, rows * ch + (18 if title else 0)), (70, 70, 76))
    for i, (name, im) in enumerate(entries):
        w, h = im.size
        s = min(cell / float(w), (cell - 4) / float(h))
        tw, th = max(1, int(w * s)), max(1, int(h * s))
        tile = _img.over_checker(im.resize((tw, th), Image.NEAREST), 8)
        x = (i % cols) * cell
        y = (i // cols) * ch
        sheet.paste(tile, (x + (cell - tw) // 2, y + 16))
        _label(sheet, "%s %dx%d" % (name, w, h), (x + 3, y + 2), (235, 235, 235))
        if mips:
            ms = _img.mip_sheet(im, None)
            sc2 = min(1.0, (cell - 4) / float(ms.size[0]))
            ms = ms.resize((max(1, int(ms.size[0] * sc2)), max(1, int(ms.size[1] * sc2))), Image.NEAREST)
            sheet.paste(ms, (x + 2, y + 18 + cell))
    if title:
        _label(sheet, title, (6, rows * ch + 2), (240, 240, 240))
    return sheet


def save(img, path):
    """Save a PNG, creating folders. Returns path."""
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    img.save(path, "PNG")
    return path
