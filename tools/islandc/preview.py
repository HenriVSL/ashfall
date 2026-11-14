"""preview.py - minimap image (DESIGN 8.7) and the review previews in tools/islandc/out/:

  top_classes.png   CLAS top view (class colours), stored chunks + chunk grid, base layout, legend
  relief.png        baked colours top view (class colour x light, shallows under the 0.65 sea blend)
  minimap.png       the minimap image over a checker board
  view_runway.png   from the runway threshold (cockpit height) toward the volcano
  view_threshold.png the threshold end from 150 m (ribbons, markings, buildings)
  view_crater.png   over the crater from the sun side
  view_sea15km.png  15 km off the coast (az 230), 600 m high (Low fog 3.6-18 km); T03 island: 15 km from the centre
  view_sea40km.png, view_sea60km.png  40 / 60 km off the coast, rendered with a far plane beyond the game's (shape)
  view_approach.png landing approach from the east, 6 km out, 450 m
  view_high.png     high, south-west of the island (T03b: 6000 m with the High preset's 28 km far plane)
  view_slopes.png   250 m over the south-west slopes of the cone (LOD0 cell size check)
Perspective views use the assetkit renderer (perspective-correct, nearest texel, mips, A2 fog) with the engine's
chunk LOD rule (d_k = 0.85 x 28.6 x edge_k to the chunk AABB, far 18 km), A2's sky panorama behind, the sea as
a textured plane, the shallows blended (sea 0.65 over the terrain, emulated by compositing a second render
without the sea), foam additive, base ribbons/marks and the A7 meshes; 565-dithered like the device.
"""
import math
import os

from PIL import Image, ImageDraw

from assetkit import mesh as M
from assetkit import preview as V
from assetkit import image as I
from assetkit import export as E

import classes as CL
import chunks as CH

CLASS_RGB = [(40, 120, 150), (222, 206, 160), (90, 150, 60), (110, 104, 96), (170, 160, 150), (150, 40, 25),
             (250, 250, 250), (200, 200, 120)]
LOD_K = 28.6          # engine d_k = lodScale x lodK x edge_k (e_terrain cfg lodK)
LOD_SCALE = 0.85      # Low default (TERRAIN_LODSCALE_LOW)
FAR = 18000.0


def lod_d(cell, lod_scale=1.0):
    return [lod_scale * LOD_K * cell * (1 << k) for k in range(4)]


class MapInfo(object):
    pass


# --------------------------------------------------------------------------------------------- minimap

def minimap(hf, cl, bk, bs, S=128):
    import height as HT
    mp = MapInfo()
    mp.name = "map_island"
    mp.size = S
    cx, cz, rad = hf.centre
    half = math.ceil((rad + 400.0) / 256.0) * 256.0
    x0, z0, x1, z1 = cx - half, cz - half, cx + half, cz + half
    mp.rect = (x0, z0, x1, z1)
    NQ = hf.N - 1
    htop = max(1700.0, 0.94 * hf.model.summit)
    pix = (x1 - x0) / S
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    px = img.load()
    land = [[0.0] * S for _ in range(S)]
    cols = [[None] * S for _ in range(S)]
    n, lt = bk.light[0]
    for py in range(S):
        for pxx in range(S):
            acc = [0.0, 0.0, 0.0]
            nl = 0
            for sy in range(4):
                for sx in range(4):
                    x = x0 + (pxx + (sx + 0.5) / 4.0) * pix
                    z = z1 - (py + (sy + 0.5) / 4.0) * pix
                    h = HT.sample(hf, x, z)
                    if h <= 0.0:
                        continue
                    nl += 1
                    I_ = int(round((x - hf.x0) / hf.cell))
                    J_ = int(round((z - hf.z0) / hf.cell))
                    I_ = min(max(I_, 0), hf.N - 1)
                    J_ = min(max(J_, 0), hf.N - 1)
                    shade = sum(lt[J_ * n + I_]) / 3.0
                    t = min(1.0, h / htop)
                    q = min(NQ - 1, max(0, J_)) * NQ + min(NQ - 1, I_)
                    c = cl.cls[q]
                    if c == CL.LAVA:
                        base = (95, 60, 50)
                    elif t < 0.35:
                        base = CL.lerp3((84, 120, 70), (120, 132, 84), t / 0.35)
                    else:
                        base = CL.lerp3((120, 132, 84), (170, 160, 146), (t - 0.35) / 0.65)
                    sh = 0.55 + 0.45 * min(1.25, shade)
                    for k in range(3):
                        acc[k] += base[k] * sh
            land[py][pxx] = nl / 16.0
            if nl:
                cols[py][pxx] = tuple(int(min(255, acc[k] / nl)) for k in range(3))
    for py in range(S):
        for pxx in range(S):
            f = land[py][pxx]
            if f <= 0.0:
                continue
            coast = f < 0.999 or any(land[py + dy][pxx + dx] <= 0.0 for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))
                                      if 0 <= py + dy < S and 0 <= pxx + dx < S)
            if coast:
                px[pxx, py] = (210, 236, 222, int(255 * max(0.55, f)))
            else:
                c = cols[py][pxx]
                px[pxx, py] = (c[0], c[1], c[2], 255)
    # runway in white
    d = ImageDraw.Draw(img)
    fr = hf.frame
    L = bs.rwy[3]
    pa = fr.to_world(-L / 2, 0.0)
    pb = fr.to_world(L / 2, 0.0)

    def tp(p):
        return ((p[0] - x0) / pix, (z1 - p[1]) / pix)
    d.line([tp(pa), tp(pb)], fill=(255, 255, 255, 255), width=2 if S <= 128 else 3)
    mp.image = img
    return mp


# --------------------------------------------------------------------------------------------- top views

def _quad_rgb(hf, cl, bk, sea=(29, 72, 109)):
    n, lt = bk.light[0]
    NQ = cl.NQ
    out = Image.new("RGB", (NQ, NQ))
    px = out.load()
    N = hf.N
    for qj in range(NQ):
        for qi in range(NQ):
            k = qj * NQ + qi
            t = cl.target[k]
            l0 = lt[qj * n + qi]
            l1 = lt[(qj + 1) * n + qi + 1]
            c = [t[q] * 0.5 * (l0[q] + l1[q]) for q in range(3)]
            if cl.cls[k] == CL.WATER:
                hm = hf.H[qj * N + qi]
                a = 0.65 if hm > -59.0 else 1.0
                c = [c[q] * (1 - a) + sea[q] * a for q in range(3)]
            px[qi, NQ - 1 - qj] = tuple(int(min(255, max(0, v))) for v in c)
    return out


def top_classes(hf, cl, ch, bs):
    NQ = cl.NQ
    S = 2
    img = Image.new("RGB", (NQ * S, NQ * S))
    px = img.load()
    stored = set((c.cx, c.cz) for c in ch.chunks)
    for qj in range(NQ):
        for qi in range(NQ):
            c = CLASS_RGB[cl.cls[qj * NQ + qi]]
            if (qi // 16, qj // 16) not in stored:
                c = (20, 40, 70)
            for dy in range(S):
                for dx in range(S):
                    px[qi * S + dx, (NQ - 1 - qj) * S + dy] = c
    d = ImageDraw.Draw(img)
    for k in range(0, NQ + 1, 16):
        d.line([(k * S, 0), (k * S, NQ * S)], fill=(0, 0, 0), width=1)
        d.line([(0, k * S), (NQ * S, k * S)], fill=(0, 0, 0), width=1)
    _draw_base(d, hf, bs, S)
    y = 6
    for k, name in enumerate(["water/shelf", "sand", "grass", "rock", "ash", "lava", "runway", "apron"]):
        d.rectangle([6, y, 20, y + 12], fill=CLASS_RGB[k], outline=(0, 0, 0))
        V.label(img, name, xy=(24, y), color=(255, 255, 255))
        y += 16
    V.label(img, "stored chunks %d (dark = open sea, not stored); grid = %.0f m chunks" % (len(ch.chunks),
                                                                                          16 * hf.cell),
            xy=(6, NQ * S - 18), color=(255, 255, 255))
    return img


def _draw_base(d, hf, bs, S):
    import base as BASE
    fr = hf.frame

    def tp(x, z):
        return ((x - hf.x0) / hf.cell * S, (hf.N - 1 - (z - hf.z0) / hf.cell) * S)
    for kind, a0, a1, b0, b1 in BASE.layout(bs.src):
        pts = [tp(*fr.to_world(a, b)) for a, b in ((a0, b0), (a1, b0), (a1, b1), (a0, b1))]
        d.polygon(pts, outline=(255, 0, 0) if kind == "runway" else (255, 255, 0))
    for nm, bx in bs.boxes:
        d.rectangle([tp(bx[0], bx[5]), tp(bx[3], bx[2])], outline=(255, 128, 0))


def relief(hf, cl, bk, bs):
    img = _quad_rgb(hf, cl, bk).resize((1024, 1024), Image.BILINEAR)
    d = ImageDraw.Draw(img)
    fr = hf.frame
    L = bs.rwy[3]
    S = 2

    def tp(p):
        return ((p[0] - hf.x0) / hf.cell * S, (hf.N - 1 - (p[1] - hf.z0) / hf.cell) * S)
    d.line([tp(fr.to_world(-L / 2, 0)), tp(fr.to_world(L / 2, 0))], fill=(255, 255, 255), width=1)
    V.label(img, "baked vertex colour x light (no textures); shallows under the 0.65 sea blend; sun az 230 el 40",
            xy=(6, 6), color=(255, 255, 255))
    return img


# --------------------------------------------------------------------------------------------- perspective

def _sky_strip(kit):
    tiles = []
    for i in range(8):
        p = kit.find("sky/sky_hi_%d.bmp" % i)
        if not p:
            return None
        img, _ = I.read_bmp(p)
        tiles.append(img.convert("RGB"))
    strip = Image.new("RGB", (256 * len(tiles), 256))
    for i, t in enumerate(tiles):
        strip.paste(t, (256 * i, 0))
    return strip


def _sky_bg(cam, W, H, strip, light):
    fog = tuple(int(v) for v in light["fog"][:3])
    top = tuple(int(v) for v in light.get("top", light["fog"])[:3])
    zen = tuple(int(v) for v in light.get("zenith", light["fog"])[:3])
    vh = light.get("vHorizon", [0.9375])[0]
    ehi = light.get("elevHi", [55.0])[0]
    img = Image.new("RGB", (W, H))
    out = img.load()
    sp = strip.load() if strip else None
    SW, SH = strip.size if strip else (1, 1)
    fpx = (H * 0.5) / math.tan(math.radians(cam.fov) * 0.5)
    f, r, u = cam.f, cam.r, cam.u
    for y in range(H):
        py = -(y + 0.5 - H * 0.5) / fpx
        for x in range(W):
            pxx = (x + 0.5 - W * 0.5) / fpx
            dx = f[0] + r[0] * pxx + u[0] * py
            dy = f[1] + r[1] * pxx + u[1] * py
            dz = f[2] + r[2] * pxx + u[2] * py
            el = math.degrees(math.atan2(dy, math.hypot(dx, dz)))
            if el < 0.0 or sp is None:
                out[x, y] = fog if el < 0.0 else top
                continue
            if el > ehi:
                t = min(1.0, (el - ehi) / (90.0 - ehi))
                out[x, y] = tuple(int(top[k] + (zen[k] - top[k]) * t) for k in range(3))
                continue
            az = math.degrees(math.atan2(dx, dz)) % 360.0
            v = vh * (ehi - el) / ehi
            sx = min(SW - 1, int(az / 360.0 * SW))
            sy = min(SH - 1, max(0, int(v * SH)))
            out[x, y] = sp[sx, sy]
    return img


def _chunk_geo(c, k, ch):
    k2 = c.alias[k] if c.alias[k] is not None else k
    verts, batches = c.lods[k2]
    g = M.Geo()
    g.P = [(v[0], v[1], v[2]) for v in verts]
    g.N = [(0.0, 1.0, 0.0)] * len(verts)
    g.UV = [(v[4], v[5]) for v in verts]
    g.UV2 = [(0.0, 0.0)] * len(verts)
    g.C = [((v[3] >> 16) & 255, (v[3] >> 8) & 255, v[3] & 255, 255) for v in verts]
    foam = M.Geo()
    for mi, idx, _sk in batches:
        name = CH.MATERIALS[mi][0]
        tgt = foam if mi == CH.FOAM else g
        for t in range(0, len(idx), 3):
            tgt.T.append((idx[t], idx[t + 1], idx[t + 2]))
            tgt.TM.append(name)
            tgt.TS.append(0)
    if foam.T:
        foam.P, foam.N, foam.UV, foam.UV2, foam.C = g.P, g.N, g.UV, g.UV2, g.C
    return g, foam


def _aabb_dist(eye, bb):
    d = 0.0
    for q in range(3):
        lo, hi = bb[q], bb[q + 3]
        if eye[q] < lo:
            d += (lo - eye[q]) ** 2
        elif eye[q] > hi:
            d += (eye[q] - hi) ** 2
    return math.sqrt(d)


def _visible(cam, bb, aspect=4.0 / 3.0):
    """Conservative frustum test of an AABB (all 8 corners outside one plane = culled)."""
    ty = math.tan(math.radians(cam.fov) * 0.5)
    tx = ty * aspect
    vs = [cam.to_view((x, y, z)) for x in (bb[0], bb[3]) for y in (bb[1], bb[4]) for z in (bb[2], bb[5])]
    tests = (lambda v: v[2] < 1.0, lambda v: v[0] > tx * v[2], lambda v: v[0] < -tx * v[2],
             lambda v: v[1] > ty * v[2], lambda v: v[1] < -ty * v[2])
    for t in tests:
        if all(t(v) for v in vs):
            return False
    return True


def _sea_geo(eye, far=FAR):
    g = M.Geo()
    step = 2048.0
    ox = math.floor(eye[0] / 128.0) * 128.0
    oz = math.floor(eye[2] / 128.0) * 128.0
    n = max(10, int(far / step) + 1)
    for j in range(-n, n):
        for i in range(-n, n):
            xa, za = ox + i * step, oz + j * step
            vs = []
            for x, z in ((xa, za), (xa, za + step), (xa + step, za + step), (xa + step, za)):
                vs.append(g.vert((x, 0.0, z), (0.0, 1.0, 0.0), ((x - ox) / 128.0, (z - oz) / 128.0),
                                 (235, 235, 235, 255)))
            g.tri(vs[0], vs[1], vs[2], "sea")
            g.tri(vs[0], vs[2], vs[3], "sea")
    return g


def render_view(eye, target, ch, bs, kit, light, strip, size=(640, 480), fov=55.0, lod_scale=LOD_SCALE, title="",
                cell=32.0, far=FAR, extra=None):
    W, H = size
    cam = V.Camera(eye, target, fov=fov, near=1.0)
    terr, foams = [], []
    stats = [0] * 5
    tris = 0
    LD = lod_d(cell, lod_scale)
    for c in ch.chunks:
        d = _aabb_dist(eye, c.aabb)
        if d > far or not _visible(cam, c.aabb):
            continue
        k = 4
        for q in range(4):
            if d < LD[q]:
                k = q
                break
        g, f = _chunk_geo(c, k, ch)
        stats[k] += 1
        tris += g.ntris + f.ntris
        terr.append(g)
        if f.T:
            foams.append(f)
    mats = {}
    for name, fl, _, _ in ch.materials:
        mats[name] = {"name": name, "tex": name, "blend": fl & 0xFF, "cullNone": 1 if fl & 0x800 else 0}
    mats["sea"] = {"name": "sea", "tex": "sea", "blend": 0}
    mats.update(bs.mats)
    base_items = []
    near_base = math.hypot(eye[0] - bs.rwy[0], eye[2] - bs.rwy[1]) < 8000.0
    if near_base:
        base_items = [bs.rib, bs.marks]
        if bs.boxgeo.ntris:
            base_items.append(bs.boxgeo)
        for ins in bs.insts:
            p = kit.find("meshes/%s.msh" % ins["mesh"])
            if p:
                base_items.append((p, (ins["x"], ins["y"], ins["z"]), math.degrees(ins["yaw"])))
    fogc = tuple(int(v) for v in light["fog"][:3])
    fog = (light["fogStart"][0], light["fogEnd"][0], fogc)
    if far != FAR:                                  # long-range look (beyond the game's far plane): fog scaled
        fog = (light["fogStart"][0] * far / FAR, far * 0.9, fogc)
    bg = _sky_bg(cam, W, H, strip, light)
    sea = _sea_geo(eye, far)
    ex = list(extra or [])
    if ex:
        mats["veg"] = {"name": "veg", "tex": "veg_atlas", "blend": 1, "cullNone": 1}
    imgA = V.render([sea] + terr + base_items + ex + foams, cam, size, bg=bg, fog=fog, tex_dirs=kit.tex_dirs,
                    lighting="vertex", mats=mats, mips=True)
    key = (255, 0, 255)
    imgB = V.render(terr + base_items + ex, cam, size, bg=key, fog=fog, tex_dirs=kit.tex_dirs, lighting="vertex",
                    mats=mats, mips=True)
    a = imgA.load()
    b = imgB.load()
    for y in range(H):
        for x in range(W):
            pb = b[x, y]
            if pb == key:
                continue
            pa = a[x, y]
            if pa != pb:
                a[x, y] = tuple(int(pa[k] * 0.65 + pb[k] * 0.35) for k in range(3))
    img = I.to16(imgA, "565", dither=True)
    V.label(img, "%s  eye %.0f %.0f %.0f  chunks LOD0-4 %s  terrain+foam tris %d" % (
        title, eye[0], eye[1], eye[2], "/".join(str(s) for s in stats), tris), xy=(4, 3), color=(255, 255, 255))
    return img, stats, tris


def _centre_dist(eye, bb):
    return math.sqrt(sum((0.5 * (bb[q] + bb[q + 3]) - eye[q]) ** 2 for q in range(3)))


def view_counts(ch, eye, yaw, pitch, lod_scale=1.0, far=FAR, hfov=70.0, cell=32.0):
    """Terrain triangles the engine rule would draw from eye (frustum 70 deg x 4:3, cull by the chunk AABB, LOD
    by the distance to the chunk centre = engine distMode 1) -> (tris with skirts, without skirts, chunks per LOD)."""
    LD = lod_d(cell, lod_scale)
    vf = math.degrees(2.0 * math.atan(math.tan(math.radians(hfov) * 0.5) * 0.75))
    cy, sy = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    tgt = (eye[0] + 1000.0 * cy * math.sin(math.radians(yaw)), eye[1] + 1000.0 * sy,
           eye[2] + 1000.0 * cy * math.cos(math.radians(yaw)))
    cam = V.Camera(eye, tgt, fov=vf, near=1.0)
    full = ns = 0
    per = [0] * 5
    for c in ch.chunks:
        d = _aabb_dist(eye, c.aabb)
        if d > far or not _visible(cam, c.aabb):
            continue
        d = _centre_dist(eye, c.aabb)
        k = 4
        for q in range(4):
            if d < LD[q]:
                k = q
                break
        per[k] += 1
        full += c.tris[k]
        ns += c.tris_ns[k]
    return full, ns, per


def _ground(hf, x, z):
    import height as HT
    return max(0.0, HT.sample(hf, x, z))


def viewpoint_lines(hf, ch, bs):
    """ENGINE.md 2.7 bench viewpoints: max/min over 8 headings."""
    m = hf.model
    cx, cz, _ = hf.centre
    pts = [("50 m over the runway", (bs.rwy[0], bs.rwy[5] + 50.0, bs.rwy[1]), 0.0),
           ("1500 m over the crater", (m.vcx, m.summit + 1500.0, m.vcz), -15.0),
           ("8000 m over the sea 10 km SW", (cx - 7071.0, 8000.0, cz - 7071.0), -15.0),
           ("on the threshold (3.5 m)", (bs.thr[0], bs.thr[1] + 3.3, bs.thr[2]), 0.0),
           ("300 m over the mid slopes", (0.5 * (m.vcx + cx) + 0.45 * m.Rv, 0.0, 0.5 * (m.vcz + cz)), 0.0)]
    gx, _gy, gz = pts[4][1]
    pts[4] = (pts[4][0], (gx, _ground(hf, gx, gz) + 300.0, gz), 0.0)
    out = []
    for ls in (1.0, LOD_SCALE):
        for name, eye, pitch in pts:
            res = [view_counts(ch, eye, yaw, pitch, lod_scale=ls, cell=hf.cell) for yaw in range(0, 360, 45)]
            mx = max(res, key=lambda r: r[0])
            mn = min(res, key=lambda r: r[0])
            out.append("view lodScale %.1f %-28s pitch %4.0f: tris max %5d (no skirts %5d, LOD0-4 chunks %s)  "
                       "min %5d (%5d)" % (ls, name, pitch, mx[0], mx[1], "/".join(str(v) for v in mx[2]), mn[0], mn[1]))
    return out


def all_previews(out_dir, src, hf, cl, bk, ch, bs, mp, light, kit, log=None, sc=None):
    bs.src = src
    V.save(top_classes(hf, cl, ch, bs), os.path.join(out_dir, "top_classes.png"))
    V.save(relief(hf, cl, bk, bs), os.path.join(out_dir, "relief.png"))
    big = mp.image.resize((384, 384) if mp.size <= 128 else (512, 512), Image.NEAREST)
    V.save(I.over_checker(big), os.path.join(out_dir, "minimap.png"))
    strip = _sky_strip(kit)
    fr = hf.frame
    L = bs.rwy[3]
    ph = bs.rwy[5]
    m = hf.model
    views = []
    # from the runway threshold (cockpit height) toward the volcano summit
    ex, ez = fr.to_world(-L / 2 + 40.0, 0.0)
    views.append(("view_runway.png", (ex, ph + 3.5, ez), (m.vcx, m.summit * 0.55, m.vcz),
                  "from the runway threshold toward the volcano"))
    # the threshold end from 150 m (markings check)
    tx, tz = fr.to_world(-L / 2 - 120.0, -60.0)
    gx, gz = fr.to_world(-L / 2 + 160.0, 0.0)
    views.append(("view_threshold.png", (tx, ph + 150.0, tz), (gx, ph, gz), "runway 27 threshold from 150 m"))
    # over the crater from the sun side (south-west)
    sx, sz = m.vcx - 3.3 * m.Rr, m.vcz - 4.0 * m.Rr
    views.append(("view_crater.png", (sx, m.summit + 2.45 * m.Rr, sz), (m.vcx, m.summit - 0.45 * m.Rr, m.vcz),
                  "over the crater"))
    # at sea from the sun side (az 230): D km beyond the coast (40/60 km: beyond the game's far plane, rendered with
    # a longer far plane + fog to show the shape)
    cx, cz, _ = hf.centre
    a = math.radians(230.0)
    rc = 0.0
    while rc < 40000.0 and _ground(hf, cx + rc * math.sin(a), cz + rc * math.cos(a)) > 0.0 or rc < 2000.0:
        rc += 100.0
    for dkm, alt, far in ((15, 600.0, FAR), (40, 2500.0, 75000.0), (60, 3000.0, 95000.0)):
        r = rc + dkm * 1000.0
        views.append(("view_sea%dkm.png" % dkm, (cx + r * math.sin(a), alt, cz + r * math.cos(a)),
                      (cx, 0.35 * m.summit if dkm > 15 else 500.0, cz),
                      "%d km off the coast, %.0f m%s" % (
                          dkm, alt, " (Low fog 3.6-18 km)" if far == FAR else
                          " (far %.0f km: beyond the game's far plane)" % (far / 1000.0)),
                      far))
    # approach from the far end of the runway (landing toward the threshold end)
    ax, az = fr.to_world(-L / 2 - 6000.0, 0.0)
    gx, gz = fr.to_world(-L / 2 + 300.0, 0.0)
    ay = max(450.0, _ground(hf, ax, az) + 250.0)    # T03b: hills east of the base
    views.append(("view_approach.png", (ax, ay, az), (gx, ph, gz), "approach from the east, 6 km, %.0f m" % ay))
    if hf.cell <= 32.0:
        views.append(("view_high.png", (cx - 9000.0, 5000.0, cz - 9000.0), (cx, 300.0, cz), "5000 m, south-west"))
    else:                                           # big island: the High preset's 28 km far plane
        views.append(("view_high.png", (cx - 13000.0, 6000.0, cz - 13000.0), (cx, 300.0, cz),
                      "6000 m, south-west (High far 28 km)", 28000.0))
    # low over the mid slopes (LOD0 cell size check), looking up toward the summit
    lx, lz = m.vcx + 0.55 * m.Rv * math.sin(a), m.vcz + 0.55 * m.Rv * math.cos(a)
    views.append(("view_slopes.png", (lx, _ground(hf, lx, lz) + 250.0, lz), (m.vcx, m.summit * 0.75, m.vcz),
                  "250 m over the south-west slopes"))
    if sc is not None:
        review_previews(out_dir, src, hf, cl, bk, ch, bs, light, kit, sc, log=log)
    views = [v if len(v) == 5 else v + (FAR,) for v in views]
    for name, eye, tgt, title, far in views:
        img, stats, tris = render_view(eye, tgt, ch, bs, kit, light, strip, title=title, cell=hf.cell, far=far)
        V.save(img, os.path.join(out_dir, name))
        if log:
            log("preview %s: chunks by LOD %s, %d tris" % (name, stats, tris))


# --------------------------------------------------------------------------------------------- A11 review sheet

def scatter_geo(sc, eye, rng_m=1600.0, far_dens=0.35, near_frac=0.35, tilt_k=0.5, cam=None):
    """The scatter sprites as src/scatter.cpp draws them on Low (range, density LOD, lean-back), one Geo."""
    g = M.Geo()
    if sc is None or sc.data is None:
        return g, 0
    types = sc.types
    n = 0
    near = rng_m * near_frac
    fade = rng_m * 0.15
    ex, ey, ez = eye
    for gi, (x, y, z, ti, scl, shade, yaw) in enumerate(sc.insts):
        dx, dy, dz = x - ex, y - ey, z - ez
        d2 = dx * dx + dy * dy + dz * dz
        if d2 > rng_m * rng_m:
            continue
        d = math.sqrt(d2)
        sz = 1.0
        if d > near:
            hf_ = (((gi * 2654435761) & 0xFFFFFFFF) >> 16 & 0xFFFF) / 65536.0
            dens = 1.0 - (1.0 - far_dens) * (d - near) / (rng_m - near)
            if hf_ > dens:
                continue
            if dens - hf_ < 0.06:
                sz = (dens - hf_) / 0.06
        if d > rng_m - fade:
            sz *= (rng_m - d) / fade
        if sz <= 0.02:
            continue
        t = types[ti]
        sz *= scl / 100.0
        if cam is not None and cam.to_view((x, y, z))[2] < -t["w"] * sz:
            continue
        w2, h, sink = 0.5 * t["w"] * sz, t["h"] * sz, t["sink"] * sz
        if t["kind"] == 1:
            a = yaw * 2.0 * math.pi / 256.0
            rx, rz = math.cos(a) * w2, -math.sin(a) * w2
            ux, uy, uz = 0.0, 1.0, 0.0
        else:
            hx, hz = -dx, -dz
            hl = math.hypot(hx, hz) or 1.0
            hx, hz = hx / hl, hz / hl
            rx, rz = -hz * w2, hx * w2
            th = min(0.61, max(0.0, math.atan2(-dy, hl) * tilt_k))
            ux, uy, uz = -hx * math.sin(th), math.cos(th), -hz * math.sin(th)
        bx, by, bz = x - ux * sink, y - uy * sink, z - uz * sink
        c = min(255, int(shade * 255.0 / 200.0))
        col = (c, c, c, 255)
        u0, v0, u1, v1 = t["uv"]
        a0 = g.vert((bx - rx, by, bz - rz), uv=(u0, v1), c=col)
        a1 = g.vert((bx - rx + ux * h, by + uy * h, bz - rz + uz * h), uv=(u0, v0), c=col)
        a2 = g.vert((bx + rx + ux * h, by + uy * h, bz + rz + uz * h), uv=(u1, v0), c=col)
        a3 = g.vert((bx + rx, by, bz + rz), uv=(u1, v1), c=col)
        g.tri(a0, a1, a2, "veg")
        g.tri(a0, a2, a3, "veg")
        n += 1
    return g, n


MC_RGB = [(30, 60, 100), (230, 215, 170), (110, 170, 70), (110, 110, 110), (180, 170, 160), (200, 40, 20),
          (255, 255, 255), (255, 255, 0), (30, 90, 40), (190, 170, 100)]


def review_previews(out_dir, src, hf, cl, bk, ch, bs, light, kit, sc, log=None):
    """A11/A6 review sheet: tools/islandc/out/review_*.png (top, classes, perspective views with scatter)."""
    import classes as CLm
    m = hf.model
    fr = hf.frame
    L = bs.rwy[3]
    ph = bs.rwy[5]
    top = relief(hf, cl, bk, bs)
    d = ImageDraw.Draw(top)
    S = 2

    def tp(x, z):
        return ((x - hf.x0) / hf.cell * S, (hf.N - 1 - (z - hf.z0) / hf.cell) * S)
    marks = [("AIRBASE", bs.rwy[0], bs.rwy[1]), ("CRATER 3077 m", m.vcx, m.vcz)]
    for i, (vx, vz) in enumerate(getattr(sc, "villages", [])):
        marks.append(("village", vx, vz))
    for nm, x, z in marks:
        px, py = tp(x, z)
        d.ellipse([px - 3, py - 3, px + 3, py + 3], outline=(255, 255, 255))
        V.label(top, nm, xy=(int(px) + 5, int(py) - 6), color=(255, 255, 255))
    V.label(top, "A11 review: top view (baked vertex colour x light, no texture); villages + landmarks marked",
            xy=(6, 22), color=(255, 255, 255))
    V.save(top, os.path.join(out_dir, "review_top.png"))
    NQ = cl.NQ
    mi = Image.new("RGB", (NQ, NQ))
    px = mi.load()
    for qj in range(NQ):
        for qi in range(NQ):
            px[qi, NQ - 1 - qj] = MC_RGB[cl.mcls[qj * NQ + qi]]
    mi = mi.resize((1024, 1024), Image.NEAREST)
    y = 6
    dd = ImageDraw.Draw(mi)
    for k, name in enumerate(CLm.MCLASS_NAMES):
        dd.rectangle([6, y, 20, y + 12], fill=MC_RGB[k], outline=(0, 0, 0))
        V.label(mi, name + (" (t_forest)" if k == 8 else (" (t_scrub)" if k == 9 else "")), xy=(24, y),
                color=(255, 255, 255))
        y += 16
    V.save(mi, os.path.join(out_dir, "review_classes.png"))
    relief_img = None
    strip = _sky_strip(kit)
    views = []

    def gy(x, z):
        return max(0.0, _ground(hf, x, z))
    # takeoff: from the threshold at cockpit height, down the runway to the west (heading 270, out to sea)
    ex, ez = fr.to_world(-L / 2 + 40.0, 0.0)
    tx, tz = fr.to_world(L / 2 + 3000.0, 0.0)
    views.append(("review_takeoff.png", (ex, ph + 3.5, ez), (tx, ph + 60.0, tz), "takeoff view: threshold 27, hdg 270"))
    bx, bz = fr.to_world(-L / 2 - 1500.0, -900.0)
    views.append(("review_base.png", (bx, ph + 300.0, bz), (bs.rwy[0], ph, bs.rwy[1]), "300 m over the airbase"))
    # low passes over the lowlands (fields W, forest E, pastures S), 120-200 m AGL
    def densest(pred, exclude=()):
        """Centre of the 2.4 km block (30 quads) with the most quads matching pred(k), away from exclude."""
        best, bk_ = -1, (0.0, 0.0)
        for bj in range(0, NQ - 30, 10):
            for bi in range(0, NQ - 30, 10):
                x = hf.x0 + (bi + 15) * hf.cell
                z = hf.z0 + (bj + 15) * hf.cell
                if any(math.hypot(x - a, z - b) < 6000.0 for a, b in exclude):
                    continue
                c = 0
                for qj in range(bj, bj + 30, 3):
                    for qi in range(bi, bi + 30, 3):
                        if pred(qj * NQ + qi):
                            c += 1
                if c > best:
                    best, bk_ = c, (x, z)
        return bk_
    pf = densest(lambda k: cl.plot[k] >= 0 and cl.ptype[k] >= 0 and _ground(hf, 0, 0) is not None and
                 cl.mcls[k] != CLm.FOREST and k % NQ < NQ and hf.H[(k // NQ) * hf.N + k % NQ] < 250.0)
    ff = densest(lambda k: cl.mcls[k] == CLm.FOREST, exclude=[pf])
    pp = densest(lambda k: cl.mcls[k] == CLm.GRASS and cl.plot[k] < 0, exclude=[pf, ff])

    def toward(p, q):
        dx_, dz_ = q[0] - p[0], q[1] - p[1]
        ln = math.hypot(dx_, dz_) or 1.0
        return (dx_ / ln, dz_ / ln)
    for name, (lx, lz), (dx_, dz_), agl, title in (
            ("review_low_fields.png", (pf[0] - 1500.0 * toward(pf, (m.vcx, m.vcz))[0],
                                       pf[1] - 1500.0 * toward(pf, (m.vcx, m.vcz))[1]),
             toward(pf, (m.vcx, m.vcz)), 120.0, "120 m AGL over farmland toward the volcano"),
            ("review_low_forest.png", (ff[0] - 1500.0 * toward(ff, (m.vcx, m.vcz))[0],
                                       ff[1] - 1500.0 * toward(ff, (m.vcx, m.vcz))[1]),
             toward(ff, (m.vcx, m.vcz)), 150.0, "150 m AGL over the forest"),
            ("review_low_pasture.png", (pp[0] - 1500.0 * toward(pp, (m.vcx, m.vcz))[0],
                                        pp[1] - 1500.0 * toward(pp, (m.vcx, m.vcz))[1]),
             toward(pp, (m.vcx, m.vcz)), 150.0, "150 m AGL over pastures"),
            ("review_low_coast.png", (6000.0, -12600.0), (-1.0, 0.15), 60.0, "60 m along the south coast")):
        e = (lx, gy(lx, lz) + agl, lz)
        t = (lx + dx_ * 3000.0, gy(lx, lz) + agl * 0.4, lz + dz_ * 3000.0)
        views.append((name, e, t, title))
    # 3 km and 8 km over the island
    cx, cz, _ = hf.centre
    views.append(("review_3km.png", (cx - 14000.0, 3000.0, cz - 12000.0), (cx, 400.0, cz), "3000 m, south-west"))
    views.append(("review_8km.png", (cx - 16000.0, 8000.0, cz - 16000.0), (cx, 0.0, cz + 2000.0), "8000 m"))
    # slopes of the volcano at 1500 m
    views.append(("review_slopes.png", (m.vcx + 6500.0, 1700.0, m.vcz - 5200.0), (m.vcx, 2200.0, m.vcz),
                  "1700 m on the south-east slopes toward the crater"))
    # hide behind the volcano: 1400 m on the east flank, the cone hides the west side
    hx, hz = m.vcx + 9000.0, m.vcz + 1200.0
    views.append(("review_hide.png", (hx, 1400.0, hz), (m.vcx - 6000.0, 1500.0, m.vcz),
                  "hide behind the volcano: 1400 m on the east flank looking west"))
    for name, eye, tgt, title in views:
        cam = V.Camera(eye, tgt, fov=55.0, near=1.0)
        geo, nspr = scatter_geo(sc, eye, cam=cam)
        img, stats, tris = render_view(eye, tgt, ch, bs, kit, light, strip, title=title + "  sprites %d" % nspr,
                                       cell=hf.cell, far=38000.0, extra=[geo] if nspr else None)
        V.save(img, os.path.join(out_dir, name))
        if log:
            log("review %s: chunks by LOD %s, %d tris, %d sprites" % (name, stats, tris, nspr))
    return relief_img
