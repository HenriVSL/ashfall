"""Self-test of the asset toolkit. Exit code = number of failed checks.

    python3 engine/tools/assetkit/selftest.py        [-v]

Prints one `selftest: <name> ok|FAIL ...` line per check and a summary line.
"""

import math
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path = [p for p in sys.path if os.path.abspath(p or ".") != os.path.dirname(os.path.abspath(__file__))]

from assetkit import KIT_DIR, project_root  # noqa: E402
from assetkit import bake, build, export, image, mesh as M, palette as P, preview, rng, validate  # noqa: E402
from PIL import Image  # noqa: E402

RESULTS = []
VERBOSE = "-v" in sys.argv
TMP = tempfile.mkdtemp(prefix="assetkit_selftest_")


def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond)))
    if cond:
        print("selftest: %-34s ok%s" % (name, ("  " + detail) if (VERBOSE and detail) else ""))
    else:
        print("selftest: %-34s FAIL %s" % (name, detail))
    return cond


def codes(probs, level="error"):
    return sorted(set(p.code for p in probs if p.level == level))


def closed_outward(g):
    e = g.edge_report()
    return e["closed"] and e["flipped"] == 0 and g.signed_volume() > 0, "%s vol=%.4f" % (e, g.signed_volume())


def normals_agree(g):
    """every triangle's clockwise face normal agrees with its vertex normals."""
    bad = 0
    for t, (a, b, c) in enumerate(g.T):
        fn = g.tri_normal(t)
        ns = M.vadd(M.vadd(g.N[a], g.N[b]), g.N[c])
        if M.vdot(fn, ns) <= 0:
            bad += 1
    return bad


def t_rng():
    a = [rng.Rng(11, "tree", 3).random() for _ in range(3)]
    b = [rng.Rng(11, "tree", 3).random() for _ in range(3)]
    c = rng.Rng(11, "tree", 4).random()
    check("rng: deterministic per name", a == b and a[0] != c)
    check("rng: stable hash value", rng.stable_hash(1, "a") == rng.stable_hash(1, "a") and
          rng.stable_hash(1, "a") != rng.stable_hash("1", "a ") and rng.stable_hash32("x") < 2 ** 32)
    r = rng.Rng(5, "c")
    ch = r.child("x")
    check("rng: child stream independent", ch.random() == rng.Rng(5, "c", "x").random())
    v0 = rng.value_noise(0.3, 0.7, 9, 8)
    v1 = rng.value_noise(8.3, 16.7, 9, 8)
    f0 = rng.fbm(1.25, 2.5, 4, 2, 0.5, 3, 4)
    f1 = rng.fbm(5.25, 6.5, 4, 2, 0.5, 3, 4)
    check("rng: periodic noise tiles", abs(v0 - v1) < 1e-12 and abs(f0 - f1) < 1e-12)
    g = rng.noise_grid(32, 16, 4, 3, seed=2)
    flat = [x for row in g for x in row]
    check("rng: noise_grid range", len(g) == 16 and len(g[0]) == 32 and min(flat) == 0.0 and max(flat) == 1.0)


def t_primitives():
    for name, g in [("box", M.box(2, 1, 3)),
                    ("box centred", M.box(1, 1, 1, pos=(2, 2, 2), base=False, uv_scale=0.5)),
                    ("cylinder y", M.cylinder(0.5, 2.0, 10)),
                    ("cylinder x (wheel)", M.cylinder(0.33, 0.25, 16, axis="x")),
                    ("cylinder z", M.cylinder(0.2, 1.0, 8, axis="z", r_top=0.1)),
                    ("cone lathe", M.lathe([(1.0, 0.0), (0.0, 2.0)], 12)),
                    ("tyre lathe", M.lathe([(0.2, -0.1), (0.33, -0.12), (0.33, 0.12), (0.2, 0.1)], 16)),
                    ("sphere", M.sphere(1.0, 12, 6)),
                    ("extrude closed", M.extrude([(0, 0), (1, 0), (1, 1), (0, 1)],
                                                 [(0, 0, 0), (0, 0, 3), (1, 1, 6)], closed=True)),
                    ("extrude ccw shape", M.extrude([(0, 0), (0, 1), (1, 0)], [(0, 0, 0), (3, 0, 0)])),
                    ]:
        ok, d = closed_outward(g)
        nb = normals_agree(g)
        check("prim: %s closed+outward" % name, ok and nb == 0, d + " normal-disagree=%d" % nb)
    # loft closed with caps (stations front -> back)
    st = []
    for i in range(5):
        z = 2.0 - i
        st.append([(math.sin(a) * 0.5, 0.5 + math.cos(a) * 0.4, z) for a in
                   [2 * math.pi * k / 10 for k in range(10)]])
    lf = M.loft(st, closed=True, cap_start=True, cap_end=True)
    ok, d = closed_outward(lf)
    lr = M.loft([list(reversed(s)) for s in st], closed=True, cap_start=True, cap_end=True)
    ok2, d2 = closed_outward(lr)
    check("prim: loft closed+caps (both orders)", ok and ok2 and normals_agree(lf) == 0 and normals_agree(lr) == 0,
          d + " / " + d2)
    tube = M.loft(st, closed=True)
    check("prim: uncapped loft follows the rule", tube.edge_report()["boundary"] == 20 and
          tube.tri_normal(0)[1] < 0, str(tube.tri_normal(0)))
    # mirrored half body (car style): half sections from bottom centre over +X side to top centre
    half = []
    for i in range(6):
        z = 2.0 - 0.8 * i
        half.append([(math.sin(a) * 0.9, 0.6 - math.cos(a) * 0.5, z) for a in
                     [math.pi * k / 8 for k in range(9)]])
    hb = M.loft(half, cap_start=True, cap_end=True)
    full = hb.copy().merge(hb.mirrored("x")).weld()
    ok, d = closed_outward(full)
    check("prim: half loft + mirror + weld closed", ok and normals_agree(full) == 0, d)
    q = M.quad((0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0))
    check("prim: quad faces -Z", q.tri_normal(0) == (0.0, 0.0, -1.0) and q.tri_normal(1) == (0.0, 0.0, -1.0))
    vq = M.vquad(1, 2, (0, 0, 5), 90.0)
    n = vq.tri_normal(0)
    check("prim: vquad yaw 90 faces -X", abs(n[0] + 1) < 1e-9 and abs(n[2]) < 1e-9, str(n))
    road = M.strip_along([(0, 0, 0), (0, 0, 10), (5, 0, 20)], [(-5, 0), (0, 0), (5, 0)],
                         surfs=[M.SURF_ASPHALT, M.SURF_PAINT])
    up = all(road.tri_normal(t)[1] > 0.99 for t in range(road.ntris))
    check("prim: strip road faces up", up and road.ntris == 8 and set(road.TS) == set([1, 2]))
    wall = M.strip_along([(0, 0, 0), (0, 0, 10)], [(6, 0), (6, 1.2)])
    check("prim: right wall faces the road", all(wall.tri_normal(t)[0] < -0.99 for t in range(wall.ntris)))
    loop = [(math.sin(a) * 50, 0, math.cos(a) * 50) for a in [2 * math.pi * k / 24 for k in range(24)]]
    ring = M.strip_along(loop, [(-4, 0), (4, 0)], closed=True)
    check("prim: closed strip", ring.ntris == 48 and ring.edge_report()["boundary"] == 48)
    cb2 = M.crossed_billboard(2, 4, 2)
    cb3 = M.crossed_billboard(2, 4, 3)
    check("prim: crossed billboard tris", cb2.ntris == 4 and cb3.ntris == 6)


def t_geo_ops():
    g = M.box(1, 1, 1)
    check("geo: box flat = 24 verts", g.nverts == 24 and g.ntris == 12)
    s = M.box(1, 1, 1)
    s.UV = [(0.0, 0.0)] * s.nverts
    s = s.normals(180.0).weld()
    check("geo: box smooth = 8 verts", s.nverts == 8, "verts=%d" % s.nverts)
    c = M.box(1, 1, 1).normals(30.0)
    check("geo: crease keeps flat box", c.nverts == 24)
    r = M.quad((0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0)).translate(0, 0, 1).rotate_y(90.0)
    p = r.P[0]
    check("geo: rotate_y(90) +Z -> +X", abs(p[0] - 1) < 1e-9 and abs(p[2]) < 1e-9, str(p))
    check("geo: rotate_x(90) +Z -> -Y", abs(M.Geo().merge(M.quad((0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)))
                                           .rotate_x(90.0).P[0][1] + 1) < 1e-9)
    m = M.box(1, 2, 3, pos=(2, 0, 0)).mirror("x")
    ok, d = closed_outward(m)
    check("geo: mirror stays outward", ok and normals_agree(m) == 0, d)
    sc = M.box(1, 1, 1).scale(-1, 2, 1)
    ok, d = closed_outward(sc)
    check("geo: negative scale stays outward", ok and normals_agree(sc) == 0, d)
    mt = M.box(1, 1, 1).matrix([[0, 0, -1], [0, 1, 0], [1, 0, 0], [5, 0, 0]])
    ok, d = closed_outward(mt)
    check("geo: matrix transform", ok and normals_agree(mt) == 0 and abs(mt.bounds()[0][0] - 4.5) < 1e-9, d)
    cyl = M.cylinder(1, 1, 8, caps=False).uv_cylinder("y", (0, 0, 0), 2.0, 1.0)
    us = [u for u, v in cyl.UV]
    check("geo: uv_cylinder seam split", max(us) <= 2.0 + 1e-9 and min(us) >= -1e-9)
    b = M.box(2, 2, 2).uv_box(0.5)
    check("geo: uv_box range", all(-0.5 - 1e-9 <= u <= 0.5 + 1e-9 and -1.0 - 1e-9 <= v <= 0.5 + 1e-9 for u, v in b.UV))
    rm = M.quad((0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0)).uv_remap((0.5, 0.25, 0.75, 0.5))
    check("geo: uv_remap", sorted(set(rm.UV)) == [(0.5, 0.25), (0.5, 0.5), (0.75, 0.25), (0.75, 0.5)])
    lit = M.prelight(M.box(1, 1, 1), mode="object", ao_height=0)
    top = [lit.C[i] for i in range(lit.nverts) if lit.N[i][1] > 0.9][0]
    bot = [lit.C[i] for i in range(lit.nverts) if lit.N[i][1] < -0.9][0]
    check("geo: prelight top brighter than bottom", P.luma(top) > P.luma(bot) and max(top) <= 255)
    w = M.prelight(M.box(1, 1, 1), mode="white")
    check("geo: prelight white", set(w.C) == set([(255, 255, 255, 255)]))
    sph = M.sphere(1.0, 16, 8)
    dec = M.decimate(sph, 100, crease=180.0)
    ok, d = closed_outward(dec)
    check("lod: decimate sphere %d -> %d" % (sph.ntris, dec.ntris), dec.ntris <= 110 and dec.ntris >= 60 and ok, d)
    st = [[(x, 0, z) for x in range(9)] for z in range(12)]
    rs = M.reduce_stations(st, 4)
    rp = M.reduce_points(st, 5, keep=(4,))
    check("lod: station/point reduction", len(rs) == 4 and rs[0] == st[0] and rs[-1] == st[-1] and
          len(rp[0]) == 5 and rp[0][2] == st[0][4])


def _mesh_with_box(name, flip=False, tris_extra=0, cls_tags=False):
    mb = M.MeshBuilder(name)
    mb.material("m", tex="sample_wall")
    g = M.box(1, 1, 1)
    if flip:
        g.T = [(a, c, b) for a, b, c in g.T]
    mb.lod()
    mb.add(g)
    if tris_extra:
        mb.add(M.sphere(0.5, tris_extra, 10, pos=(0, 3, 0)))
    return mb


def t_export():
    sample_out = os.path.join(KIT_DIR, "samples", "out")
    mb = _mesh_with_box("test_box")
    m = mb.to_msh()
    data = export.msh_bytes(m)
    m2 = export.read_msh(data)
    check("msh: round trip byte-identical", export.msh_bytes(m2) == data and len(data) % 4 == 0)
    hp = os.path.join(sample_out, "meshes", "sample_house.msh")
    if os.path.exists(hp):
        with open(hp, "rb") as f:
            hd = f.read()
        hm = export.read_msh(hd)
        check("msh: sample house round trip", export.msh_bytes(hm) == hd and hm.tri_counts()[0] >= 60,
              "tris %s" % hm.tri_counts())
    check("msh: header 60 B, counts", data[:4] == b"RMSH" and m2.tri_counts() == [12] and
          len(data) == 60 + 68 + 12 * 1 + 20 + 24 * 24 + 12 * 24 + 2 * 36)
    # pools
    enc = export.PoolEncoder(max_verts=40)
    road = M.strip_along([(0, 0, z) for z in range(0, 40, 4)], [(-5, 0), (0, 0), (5, 0)])
    road.TM = ["road"] * road.ntris
    enc.add_geo(0, 0, road, {"road": 2})
    enc.add_geo(0, 1, M.box(1, 1, 1).set_mat("wall"), {"wall": 1}, export.RFMT_V2)
    enc.add_batch(3, 0, 0, export.RFMT_V1, [(0, 0, 0, 0xFFFFFFFF, 0, 0)] * 3, [0, 1, 2])
    vp, bt, ix = enc.encode()
    pd = export.decode_pools(vp, bt, ix)
    check("pools: round trip byte-identical", export.encode_pools(pd) == (vp, bt, ix) and
          not export.check_pools(pd) and len(pd.pools) >= 3, "pools=%d batches=%d" % (len(pd.pools), len(pd.batches)))
    # trk container
    info = export.pack_name("testtrk", 32) + export.pack_name("Test", 48)
    info += b"\0" * (136 - len(info))          # INFO is 136 B (trkfile.py layout)
    chunks = [("INFO", info), ("TEXL", export.pack_names(["road_a", "gravel"])), ("VPOL", vp), ("BTCH", bt),
              ("INDX", ix)]
    trk = export.trk_bytes(chunks)
    t = export.read_trk(trk)
    check("trk: container round trip", not t.problems and [c[0] for c in t.chunks] == [c[0] for c in chunks] and
          export.trk_bytes(t.chunks) == trk and export.unpack_names(t.get("TEXL")) == ["road_a", "gravel"])
    p = os.path.join(TMP, "t.trk")
    with open(p, "wb") as f:
        f.write(trk)
    check("trk: validator passes", not validate.errors(validate.check_file(p)))
    bad = bytearray(trk)
    bad[-1] ^= 0x55
    with open(p, "wb") as f:
        f.write(bytes(bad))
    check("trk: validator catches crc/pool damage", "trk" in codes(validate.check_file(p)))


def t_images():
    def disc(d, img, s):
        d.ellipse([4 * s, 4 * s, 60 * s, 60 * s], fill=(40, 120, 40, 255))
    img = image.supersample(64, 64, disc)
    p = os.path.join(TMP, "tex", "keyed.bmp")
    image.save_bmp(p, img, keyed=True)
    rgb, info = image.read_bmp(p)
    px = rgb.load()
    vals = set(px[x, y] for y in range(64) for x in range(64))
    nkey = sum(1 for y in range(64) for x in range(64) if px[x, y] == P.KEY)
    impure = [v for v in vals if v != P.KEY and (P.is_near_key(v, 20) or (v[0] > 200 and v[2] > 200))]
    check("bmp: key purity after threshold", info["bpp"] == 24 and info["h"] > 0 and nkey > 0 and not impure,
          "%d key px, %d colours" % (nkey, len(vals)))
    check("bmp: keyed validates", not validate.errors(validate.check_file(p)))
    with open(p, "rb") as f:
        raw = f.read()
    check("bmp: writer stable + reader round trip", image.bmp_bytes(rgb) == raw)
    a = image.new_rgba(32, 32, (200, 200, 200, 0))
    for y in range(32):
        for x in range(32):
            a.putpixel((x, y), (200, 180, 160, x * 8))
    pa = os.path.join(TMP, "tex", "smoke.bmp")
    w = image.save_bmp(pa, a, alpha=True)
    ag, ai = image.read_bmp(w[1])
    check("bmp: alpha pair 8-bit grey", len(w) == 2 and ai["bpp"] == 8 and ag.mode == "L" and
          ag.getpixel((31, 0)) == 248 and not validate.errors(validate.check_paths(w)))
    lt = image.load_texture(pa)
    check("bmp: load_texture alpha", lt.getpixel((0, 0))[3] == 0 and lt.getpixel((31, 5))[3] == 248)
    mips = image.mip_chain(rgb)
    tinted = sum(1 for lv in mips for q in lv.getdata() if q[3] > 0 and P.is_near_key(q, 20))
    check("img: mips key-safe", len(mips) == 7 and mips[-1].size == (1, 1) and tinted == 0)
    q = image.to16(image.gradient(64, 8, (0, 0, 0), (255, 255, 255)), "565")
    check("img: 565 preview quantises", len(set(q.getdata())) <= 64)
    t = image.noise_image(64, 64, (0, 0, 0), (255, 255, 255), cells=4, seed=3)
    check("img: tileable noise seams", image.seam_error(t) < 12.0, "seam %.2f" % image.seam_error(t))
    # atlas
    r = rng.Rng(1, "atlas")
    sizes = [(r.int(8, 60), r.int(8, 60)) for _ in range(20)]
    pos = image.pack_rects(sizes, 256, 256, 2)
    ok = True
    boxes = [(x - 2, y - 2, x + w + 2, y + h + 2) for (x, y), (w, h) in zip(pos, sizes)]
    for i, b in enumerate(boxes):
        if b[0] < 0 or b[1] < 0 or b[2] > 256 or b[3] > 256:
            ok = False
        for b2 in boxes[i + 1:]:
            if b[0] < b2[2] and b2[0] < b[2] and b[1] < b2[3] and b2[1] < b[3]:
                ok = False
    check("atlas: packing no overlap, gutters", ok and pos == image.pack_rects(sizes, 256, 256, 2))
    try:
        image.pack_rects([(200, 200), (100, 100)], 256, 256, 2)
        full = False
    except ValueError:
        full = True
    check("atlas: overflow raises", full)
    at = image.Atlas(64, 64, 2)
    at.add("a", image.new_rgba(16, 16, (255, 0, 0, 255)))
    at.add("b", image.new_rgba(8, 8, (0, 0, 255, 255)))
    sheet = at.image()
    x, y, w, h = at.rect("a")
    check("atlas: gutter bleed", sheet.getpixel((x - 1, y - 1)) == (255, 0, 0, 255) and
          at.uv("a")[0] == (x + 0.5) / 64.0)


def t_validator():
    d = os.path.join(TMP, "v")
    os.makedirs(os.path.join(d, "meshes"))
    os.makedirs(os.path.join(d, "tex"))
    image.save_bmp(os.path.join(d, "tex", "sample_wall.bmp"), image.new(16, 16, (100, 100, 100)))
    good = _mesh_with_box("test_good")
    good.save(os.path.join(d, "meshes", "test_good.msh"))
    check("validate: good mesh passes", not validate.errors(validate.check_file(os.path.join(d, "meshes",
                                                                                      "test_good.msh"))))
    bad = _mesh_with_box("test_flip", flip=True)
    bad.save(os.path.join(d, "meshes", "test_flip.msh"))
    check("validate: catches wrong winding", "winding" in codes(validate.check_file(os.path.join(d, "meshes",
                                                                                        "test_flip.msh"))))
    big = _mesh_with_box("prop_big", tris_extra=24)
    big.save(os.path.join(d, "meshes", "prop_big.msh"))
    pr = validate.check_file(os.path.join(d, "meshes", "prop_big.msh"))
    check("validate: catches over-budget mesh", "budget" in codes(pr), str(codes(pr)))
    car = _mesh_with_box("v10gt_test")
    car.save(os.path.join(d, "meshes", "v10gt_test.msh"))
    cc = codes(validate.check_file(os.path.join(d, "meshes", "v10gt_test.msh")))
    check("validate: car class needs LODs + tags", "budget" in cc and "tags" in cc)
    mt = M.MeshBuilder("test_missing")
    mt.material("m", tex="no_such_texture")
    mt.add(M.box(1, 1, 1))
    mt.save(os.path.join(d, "meshes", "test_missing.msh"))
    check("validate: catches missing texture", "missing-tex" in codes(validate.check_file(
        os.path.join(d, "meshes", "test_missing.msh"))))
    np_ = os.path.join(d, "tex", "npot.bmp")
    image.save_bmp(np_, image.new(100, 64), check=False)
    check("validate: catches NPOT texture", "npot" in codes(validate.check_file(np_)))
    big_t = os.path.join(d, "tex", "huge.bmp")
    image.save_bmp(big_t, image.new(512, 32), check=False)
    check("validate: catches size/aspect", set(["size", "aspect"]) <= set(codes(validate.check_file(big_t))))

    def disc(dr, img, s):
        dr.ellipse([4 * s, 4 * s, 60 * s, 60 * s], fill=(40, 120, 40))
    fr = image.supersample(64, 64, disc, mode="RGB", bg=P.KEY + (255,))    # AA against the key: fringe
    fp = os.path.join(d, "tex", "fringe.bmp")
    image.save_bmp(fp, fr)
    check("validate: catches magenta fringe", "fringe" in codes(validate.check_file(fp)))
    try:
        image.save_bmp(os.path.join(d, "tex", "x.bmp"), image.new(100, 64))
        raised = False
    except ValueError:
        raised = True
    check("image: save_bmp refuses NPOT", raised)
    rc = validate.main(["-q", os.path.join(d, "tex", "npot.bmp")])
    check("validate: CLI exit code = errors", rc == 1)


def t_preview():
    hp = os.path.join(KIT_DIR, "samples", "out", "meshes", "sample_house.msh")
    td = [os.path.join(KIT_DIR, "samples", "out", "tex")]
    t0 = time.time()
    img = preview.render([hp], preview.Camera((7, 5, -9), (0, 2, 0), fov=40), (512, 512), tex_dirs=td)
    dt = time.time() - t0
    bgc = preview.BG
    cover = sum(1 for p in img.getdata() if p != bgc) / float(512 * 512)
    check("preview: 512 px view renders < 10 s", dt < 10.0 and cover > 0.1, "%.2f s, %.0f%% covered" % (dt, cover * 100))
    t0 = time.time()
    sh = preview.views(hp, 128, tex_dirs=td)
    tt = preview.turntable(M.box(1, 1, 1), 4, 64)
    ls = preview.lods(hp, 96, tex_dirs=td)
    cs = preview.contact_sheet([os.path.join(td[0], "sample_atlas.bmp")], 1, 96, mips=True)
    sc = preview.scene([(M.box(1, 1, 1), (0, 0, 5), 30)], (0, 1, -3), (0, 0.5, 5), (160, 120), ground=((90, 120, 50), 50))
    q = preview.render([M.box(1, 1, 1)], preview.Camera((2, 2, -3), (0, 0.5, 0)), (64, 64), q16=True)
    check("preview: sheets render", sh.size == (384, 256) and tt.size == (256, 64) and ls.size == (192, 192) and
          cs.size[0] == 96 and sc.size == (160, 120) and q.size == (64, 64), "%.2f s" % (time.time() - t0))
    # alpha test: keyed tree cut-outs show background between branches
    tp = os.path.join(KIT_DIR, "samples", "out", "meshes", "sample_tree.msh")
    ti = preview.render([tp], preview.Camera((0, 3, -8), (0, 3, 0), ortho=3.2), (64, 64), tex_dirs=td)
    check("preview: alpha test", ti.getpixel((2, 60)) == bgc and ti.getpixel((32, 30)) != bgc)


def t_sample_build():
    root = project_root()
    gens = build.discover(root, ["sample"])
    ok = check("build: sample discovered", len(gens) == 1)
    if not ok:
        return
    res = build.run_gen(gens[0], root, check=True, previews=False)
    check("build: sample --check identical + valid", res.ok and res.errors == 0 and res.status == "identical",
          res.status)


def _cone(seg=12, r=1.0, h=2.0):
    """Indexed cone fan (tip + ring, open base): every vertex shared."""
    g = M.Geo()
    tip = g.vert((0.0, h, 0.0))
    ring = [g.vert((r * math.sin(2 * math.pi * k / seg), 0.0, r * math.cos(2 * math.pi * k / seg)))
            for k in range(seg)]
    for k in range(seg):
        g.tri(tip, ring[k], ring[(k + 1) % seg])
    return g


def unit_normals(g):
    return all(abs(M.vlen(n) - 1.0) < 1e-6 for n in g.N)


def t_smooth_groups():
    cone = _cone()
    n0 = cone.nverts
    legacy = cone.copy().normals(50)
    sm = M.smooth_normals(cone, 50)
    tip = [i for i, p in enumerate(sm.P) if p[1] > 1.0]
    check("smooth: cone tip is one vertex", sm.nverts == 13 and len(tip) == 1 and legacy.nverts > 13 and
          cone.nverts == n0, "groups %d verts, legacy normals() %d" % (sm.nverts, legacy.nverts))
    check("smooth: tip normal points up, unit", abs(sm.N[tip[0]][1] - 1.0) < 1e-9 and unit_normals(sm) and
          normals_agree(sm) == 0)
    bx = M.box(1, 1, 1)
    bx.UV = [(0.0, 0.0)] * bx.nverts          # per-face UVs would keep the corners split (UV seams)
    check("smooth: box hard at 50, soft at 100", M.smooth_normals(bx, 50).nverts == 24 and
          M.smooth_normals(bx, 100).nverts == 8 and M.smooth_normals(bx, 50, soft=lambda t: True).nverts == 8)
    # transitivity: a 16-sided tube, 22.5 deg per edge: any crease >= 22.5 gives one vertex per position
    tube = M.lathe([(1.0, 0.0), (1.0, 1.0)], seg=16, caps=False, crease=0)
    st = tube.copy().weld(attrs=False).smooth(30)
    check("smooth: transitive around a tube", st.nverts == 32 and normals_agree(st) == 0 and
          all(abs(n[1]) < 1e-9 for n in st.N), "verts=%d (16 x 2)" % st.nverts)
    # in place keeps materials/surfaces, UV seams still split, crease 0 = flat
    g = M.box(1, 1, 1, mat="w").set_surface(M.SURF_GRAVEL)
    g.smooth(0)
    check("smooth: in place keeps TM/TS, crease 0 flat", g.nverts == 24 and set(g.TM) == set(["w"]) and
          set(g.TS) == set([M.SURF_GRAVEL]) and closed_outward(g)[0])


def t_cap_bulge():
    ring = [(math.cos(2 * math.pi * k / 8), math.sin(2 * math.pi * k / 8) + 1.0, 0.0) for k in range(8)]
    flat = M.cap(ring, (0, 0, 1))
    dome = M.cap(ring, (0, 0, 1), bulge=0.2)
    dish = M.cap(list(reversed(ring)) + [ring[-1]], (0, 0, -1), center="bbox", bulge=-0.1)
    c = dome.P[0]
    check("cap: default flat fan unchanged", flat.ntris == 8 and flat.nverts == 9 and
          all(abs(p[2]) < 1e-12 for p in flat.P))
    check("cap: bulge pushes the centre", abs(c[2] - 0.2) < 1e-12 and abs(c[1] - 1.0) < 1e-9 and dome.ntris == 8 and
          all(dome.tri_normal(t)[2] > 0 for t in range(dome.ntris)), str(c))
    check("cap: dome normals smooth + unit", unit_normals(dome) and abs(dome.N[0][2] - 1.0) < 1e-9 and
          normals_agree(dome) == 0 and dome.N[1][2] < 0.99)
    check("cap: bbox centre, dished, closing point ignored", dish.nverts == 9 and dish.ntris == 8 and
          abs(dish.P[0][2] - 0.1) < 1e-12 and abs(dish.P[0][1] - 1.0) < 1e-9 and
          all(dish.tri_normal(t)[2] < 0 for t in range(dish.ntris)))
    pc = M.cap(ring, (0, 0, 1), center=(0.25, 1.0, 0.0))
    check("cap: explicit centre point", pc.P[0] == (0.25, 1.0, 0.0) and pc.ntris == 8 and
          all(0.0 <= u <= 1.0 and 0.0 <= v <= 1.0 for u, v in pc.UV))


def t_strip_rows():
    # a road along +Z that widens from 6 to 10 m, with a kerb column that collapses in the first rows
    rows = []
    for i in range(6):
        hw = 3.0 + 0.4 * i
        k = 0.0 if i < 2 else 0.5
        rows.append([(-hw - k, 0.0, i * 4.0), (-hw, 0.0, i * 4.0), (hw, 0.0, i * 4.0), (hw + k, 0.0, i * 4.0)])
    g = M.strip_rows(rows, mats=["kerb", "road", "kerb"], surfs=[M.SURF_KERB, M.SURF_ASPHALT, M.SURF_KERB],
                     crease=None)
    up = all(g.tri_normal(t)[1] > 0.99 for t in range(g.ntris))
    check("strip_rows: faces up, collapsed quads skipped", up and g.ntris == 5 * 2 + 2 * (3 * 2 + 1),
          "tris=%d" % g.ntris)
    check("strip_rows: per-segment mats/surfs, up normals", set(g.TM) == set(["kerb", "road"]) and
          g.TS.count(M.SURF_ASPHALT) == 10 and set(g.N) == set([(0.0, 1.0, 0.0)]))
    us = [u for u, v in g.UV]
    vs = sorted(set(round(v, 6) for u, v in g.UV))
    check("strip_rows: default uv (u across 0..1, v per metre)", min(us) == 0.0 and abs(max(us) - 1.0) < 1e-12 and
          vs[:3] == [0.0, 0.4, 0.8], str(vs[:4]))
    # group: per-role vertices; callables per row; skip
    g2 = M.strip_rows(rows, uv=lambda i, j, key: (j * 0.5, i * 0.25), group=lambda i, k: k == 1,
                      mats=lambda i, k: "road" if k == 1 else "kerb", skip=lambda i, k: i == 0 and k == 1,
                      color=lambda i, j, key: (255, 0, 0) if key else (0, 0, 255), crease=None)
    reds = sum(1 for c in g2.C if c[:3] == (255, 0, 0))
    check("strip_rows: group splits seams, skip, callables", g2.ntris == 8 + 14 and reds == 2 * 5 and
          g2.nverts == reds + 2 * 2 * 4 + 2 * 2, "tris=%d verts=%d reds=%d" % (g2.ntris, g2.nverts, reds))
    sm = M.strip_rows([[(-1, 0, z), (0, 0.3, z), (1, 0, z)] for z in (0, 2, 4)])
    mid = sm.P.index((0.0, 0.3, 2.0))
    check("strip_rows: crease default smooths (one vertex per point)", sm.nverts == 9 and normals_agree(sm) == 0 and
          abs(sm.N[mid][0]) < 1e-9 and sm.N[mid][1] > 0.99)
    raised = False
    try:
        M.strip_rows([[(0, 0, 0), (1, 0, 0)], [(0, 0, 1)]])
    except ValueError:
        raised = True
    check("strip_rows: row length mismatch raises", raised)


def t_bake():
    # a 2 x 1 m quad facing -Z, UV mapped onto the left half of a 64 x 32 texture
    q = M.quad((0, 0, 0), (0, 1, 0), (2, 1, 0), (2, 0, 0), uv=(0.0, 0.0, 0.5, 1.0))
    leak = bake.Baker(64, 32, ss=2)
    leak.add_geo(q)                         # default overlap 0.02 also takes texel centres just outside
    bk = bake.Baker(64, 32, ss=2, overlap=0.0)
    n = bk.add_geo(q, region="q")
    full = bk.covered((0, 0, 32, 32))
    check("bake: quad covers its rect", n == 64 * 64 and full == 64 * 64 and bk.covered((32, 0, 32, 32)) == 0 and
          leak.covered((32, 0, 32, 32)) == 64, "written %d covered %d" % (n, full))
    i = bk.index(3, 10)                     # hi-res texel centre (3.5, 10.5) -> final (1.75, 5.25)
    p = bk.pos[i]
    check("bake: position + normal interpolation", abs(p[0] - 1.75 / 32 * 2) < 1e-9 and abs(p[1] - (1 - 5.25 / 32)) < 1e-9
          and bk.nrm[i] == (0.0, 0.0, -1.0) and bk.reg[i] == "q" and bk.texel(3, 10) == (1.75, 5.25), str(p))
    # depth keys: a second triangle over the same texels with a larger key wins, a smaller one doesn't
    tex = [(0, 0), (32, 0), (0, 32)]
    lo = bk.add_tri(tex, [(9, 9, 9)] * 3, [(0, 1, 0)] * 3, keys=(-1, -1, -1), region="lo")
    hi = bk.add_tri(tex, [(5, 5, 5)] * 3, [(0, 1, 0)] * 3, keys=(1, 1, 1), region="hi")
    check("bake: depth key decides overlaps", lo == 0 and hi > 0 and bk.reg[bk.index(2, 2)] == "hi" and
          bk.reg[bk.index(60, 60)] == "q")
    # clip + dilation + edge distance + paint
    b2 = bake.Baker(16, 16)
    b2.add_tri([(4, 4), (12, 4), (4, 12)], [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 0, 1)] * 3, clip=(0, 0, 8, 16))
    before = b2.covered()
    filled = b2.dilate((0, 0, 8, 16), steps=2, region="d")
    check("bake: clip + dilate", before > 0 and b2.covered((8, 0, 8, 16)) == 0 and filled > 0 and
          b2.covered() == before + filled and b2.reg[b2.index(3, 5)] == "d" and b2.pos[b2.index(0, 0)] is None)
    mask = [p is not None for p in b2.pos]
    dist = b2.edge_distance(mask, cap=3, rects=[(0, 0, 8, 16)])
    check("bake: edge distance", dist[b2.index(2, 5)] == 1 and dist[b2.index(5, 7)] == 3 and
          dist[b2.index(7, 7)] == 1 and dist[b2.index(0, 0)] == 0, str([dist[b2.index(x, 7)] for x in range(8)]))
    img = bk.paint(lambda p, nn, reg, ix, iy: (255, 0, 0) if reg == "hi" else (0, 255, 0), bg=(0, 0, 40))
    raw = bk.paint(lambda p, nn, reg, ix, iy: (255, 255, 255), downsample=False)
    check("bake: paint + box downsample", img.size == (64, 32) and img.getpixel((2, 2)) == (255, 0, 0) and
          img.getpixel((30, 30)) == (0, 255, 0) and img.getpixel((60, 5)) == (0, 0, 40) and raw.size == (128, 64))
    b3 = bake.Baker(16, 16, ss=2)
    b3.add_geo(q, key=lambda p, nn: p[0])
    b4 = bake.Baker(16, 16, ss=2)
    b4.add_geo(q, key=lambda p, nn: p[0])
    check("bake: deterministic", b3.pos == b4.pos and b3.nrm == b4.nrm)


def t_preview_followups():
    from PIL import Image as _I
    im = _I.new("RGB", (80, 20), (200, 200, 200))
    out = preview.label(im, "Hello", color=(0, 0, 0))
    check("preview: label() public", out is im and len(set(im.getdata())) > 1 and preview._label is preview.label)
    td = [os.path.join(KIT_DIR, "samples", "out", "tex")]
    named = M.box(1, 1, 1, mat="wall")
    obj = M.box(1, 1, 1, mat=M.Material("wall", "sample_wall"))
    cam = preview.Camera((2, 2, -3), (0, 0.5, 0))
    a = preview.render([obj], cam, (64, 64), tex_dirs=td)
    b = preview.render([named], cam, (64, 64), tex_dirs=td, mats={"wall": M.Material("wall", "sample_wall")})
    c = preview.render([named], cam, (64, 64), tex_dirs=td, mats=[("wall", "sample_wall", M.RB_OPAQUE, False)])
    d = preview.render([named], cam, (64, 64), tex_dirs=td, mats=[{"name": "wall", "tex": "sample_wall"}])
    e = preview.render([named], cam, (64, 64), tex_dirs=td)
    check("preview: render mats= table (str names)", a.tobytes() == b.tobytes() == c.tobytes() == d.tobytes() and
          a.tobytes() != e.tobytes())
    t = preview.material_table([("leaf", "tree_pine", M.RB_ALPHATEST, True)])
    check("preview: material_table tuple form", t["leaf"]["tex"] == "tree_pine" and t["leaf"]["cullNone"] == 1 and
          t["leaf"]["blend"] == M.RB_ALPHATEST)


def _trk_chunks():
    """Minimal but complete trackc-style chunk set (T03 layouts)."""
    import struct as S
    info = export.pack_name("t", 32) + export.pack_name("T", 48) + b"\0" * 56
    sky = b"\0" * 312
    texl = export.pack_names(["a"])
    matl = S.pack("<ii4B4Bf", 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0.0)
    cell = S.pack("<5i", 0, 0, 1, 1, 1) + S.pack("<2h6f6i", 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0)
    colt = S.pack("<13fBBH", 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0)
    grid1 = S.pack("<3f3i", 0, 0, 4, 1, 1, 1) + S.pack("<2I", 0, 1) + S.pack("<I", 0)
    wall = S.pack("<i", 1) + S.pack("<12f2iBBH", 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, -1, -1, 1, 0, 0) + grid1
    slce = b"\0" * 92 * 3
    chkp = S.pack("<3i", 1, 0, 0) + S.pack("<i", 0)
    gridc = b"\0" * 16 * 8
    return [("INFO", info), ("SKY ", sky), ("TEXL", texl), ("MATL", matl), ("CELL", cell), ("COLT", colt),
            ("COLG", grid1), ("WALL", wall), ("BOXS", S.pack("<i", 0)), ("SLCE", slce), ("CHKP", chkp),
            ("GRID", gridc), ("MAPI", b"\0" * 16)]


def t_trk_records():
    p = os.path.join(TMP, "rec.trk")

    def probs(chunks):
        with open(p, "wb") as f:
            f.write(export.trk_bytes(chunks))
        return validate.errors(validate.check_file(p))

    good = _trk_chunks()
    check("trk: T03 record layout passes", not probs(good), str([x.msg for x in probs(good)]))
    cases = [("SLCE", b"\0" * 91, "TrkSlice"), ("WALL", good[7][1][:-3], "WallSeg"), ("COLT", b"\0" * 55, "CollTri"),
             ("MATL", b"\0" * 21, "MATL"), ("CELL", good[4][1] + b"\0" * 4, "CELL"), ("INFO", b"\0" * 80, "INFO"),
             ("CHKP", good[10][1][:12] + b"\5\0\0\0", "CHKP index")]
    bad = []
    for cid, data, name in cases:
        ch = [(c, data if c == cid else d) for c, d in good]
        if not probs(ch):
            bad.append(name)
    check("trk: record size/range errors caught", not bad, "missed: %s" % bad)


# ---------------------------------------------------------------------------------------------------- 1.2 (T02c)

_GEN_OK = """ASSETS = ["tex/okgen.bmp"]
ORDER = 10
from assetkit import image as I
def build(out_dir, kit):
    kit.save_bmp("tex/okgen.bmp", I.new(8, 8, (90, 100, 110)))
"""
_GEN_BROKEN = """ASSETS = ["tex/broken.bmp"]
def build(out_dir, kit)        # half-saved file: syntax error
"""


def t_build_imports():
    import contextlib
    import io
    root = os.path.join(TMP, "proj")
    art = os.path.join(root, "tools", "art")
    os.makedirs(art)
    os.makedirs(os.path.join(root, "data"))
    with open(os.path.join(art, "gen_okgen.py"), "w") as f:
        f.write(_GEN_OK)
    with open(os.path.join(art, "gen_broken.py"), "w") as f:
        f.write(_GEN_BROKEN)
    for m in ("assetkit_gen_art_okgen", "assetkit_gen_art_broken"):
        sys.modules.pop(m, None)
    only = build.discover(root, ["okgen"])
    check("build: --only imports only the selection", [g.name for g in only] == ["okgen"] and only[0].module and
          "assetkit_gen_art_broken" not in sys.modules)
    lazy = build.discover(root, load=False)
    check("build: discover(load=False) imports nothing", all(g.module is None for g in lazy) and
          "assetkit_gen_art_broken" not in sys.modules)
    gens = {g.name: g for g in build.discover(root)}
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rb = build.run_gen(gens["broken"], root, check=False, previews=False)
        ro = build.run_gen(gens["okgen"], root, check=False, previews=False)
        rc_only = build.main(["--only", "okgen", "--check"], root=root)
        rc_both = build.main(["--only", "okgen,broken"], root=root)
    check("build: import error = that generator's failure", gens["broken"].load_error and not rb.ok and
          rb.status == "FAILED (import error)" and "SyntaxError" in out.getvalue() and
          "assetkit_gen_art_broken" not in sys.modules, rb.status)
    check("build: other generators still run", ro.ok and os.path.exists(os.path.join(root, "data", "tex", "okgen.bmp"))
          and rc_only == 0 and rc_both == 1, "%s rc %s %s" % (ro.status, rc_only, rc_both))


def t_kit_helpers():
    out = os.path.join(TMP, "kitout")
    kit = build.Kit("t", TMP, out, out, os.path.join(out, "_p"))
    p = kit.save_text("sky/x.txt", "a 1\nb 2\n")
    with open(p, "rb") as f:
        raw = f.read()
    p2 = kit.save_text("tex/y.txt", "a\r\nb\nc", encoding="latin-1", crlf=True)
    with open(p2, "rb") as f:
        raw2 = f.read()
    p3 = kit.save_text("tex/z.txt", u"Järvi\n", encoding="latin-1")
    with open(p3, "rb") as f:
        raw3 = f.read()
    check("kit: save_text bytes / crlf / encoding", raw == b"a 1\nb 2\n" and raw2 == b"a\r\nb\r\nc" and
          raw3 == b"J\xe4rvi\n")
    rgba = image.new_rgba(16, 16, (0, 0, 0, 0))
    for y in range(16):
        for x in range(16):
            v = max(0, 255 - 32 * int(math.hypot(x - 7.5, y - 7.5)))
            rgba.putpixel((x, y), (v, v // 2, 0, v))
    w = kit.save_bmp("tex/glow.bmp", rgba, alpha=True, alpha_bleed=0)
    rgb, _ = image.read_bmp(w[0])
    bled = image.save_bmp(os.path.join(out, "tex", "glowb.bmp"), rgba, alpha=True)
    rgb_b, _ = image.read_bmp(bled[0])
    check("img: save_bmp(alpha_bleed=0) keeps premultiplied RGB", rgb.tobytes() == rgba.convert("RGB").tobytes() and
          rgb_b.tobytes() != rgb.tobytes() and not validate.errors(validate.check_paths(w)))
    a8 = kit.save_bmp8("tex/glow_a.bmp", rgba)
    a8b = image.save_bmp8(os.path.join(out, "tex", "g2_a.bmp"), rgba.split()[3])
    with open(a8, "rb") as f, open(w[1], "rb") as g, open(a8b, "rb") as h:
        same = f.read() == g.read() == h.read()
    check("img: save_bmp8 (alpha band / L) = alpha file", same and image.read_bmp(a8)[1]["bpp"] == 8)
    try:
        image.save_bmp8(os.path.join(out, "tex", "bad_a.bmp"), image.new(100, 16, (0, 0, 0)).convert("L"))
        raised = False
    except ValueError:
        raised = True
    check("img: save_bmp8 size rules", raised)
    # opaque atlas without the key
    at = image.Atlas(64, 64, 2)
    at.add("a", image.new_rgba(16, 16, (200, 0, 0, 255)))
    half = image.new_rgba(8, 8, (0, 0, 200, 255))
    half.putpixel((0, 0), (0, 0, 0, 0))
    at.add("b", half)
    op = at.image(bg=(128, 128, 128))
    bx, by, _, _ = at.rect("b")
    ref = image.flatten(at.image(), (128, 128, 128))
    pb = os.path.join(out, "tex", "opaque.bmp")
    image.save_bmp(pb, at.image(), bg=(128, 128, 128))
    rgb_o, _ = image.read_bmp(pb)
    check("atlas: image(bg=) opaque, no key", op.mode == "RGB" and op.tobytes() == ref.tobytes() and
          op.getpixel((63, 63)) == (128, 128, 128) and op.getpixel((bx, by)) == (128, 128, 128) and
          P.KEY not in set(op.getdata()) and rgb_o.tobytes() == op.tobytes())


def t_sky_rule():
    d = os.path.join(TMP, "data", "sky")
    os.makedirs(d)
    res = {}
    for name, size in (("hi", (256, 256)), ("lo", (256, 128)), ("bad", (128, 128)), ("wide", (256, 64))):
        p = os.path.join(d, "s_%s.bmp" % name)
        image.save_bmp(p, image.new(size[0], size[1], (100, 120, 160)))
        res[name] = codes(validate.check_file(p))
    q = os.path.join(TMP, "data", "tex", "s_bad.bmp")
    image.save_bmp(q, image.new(128, 128, (100, 120, 160)))
    q2 = os.path.join(TMP, "data", "tex", "car_9.bmp")
    image.save_bmp(q2, image.new(128, 128, (100, 120, 160)))
    check("validate: sky rule 256x256 / 256x128 under sky/", res["hi"] == [] and res["lo"] == [] and
          res["bad"] == ["size"] and res["wide"] == ["size"] and codes(validate.check_file(q)) == [] and
          codes(validate.check_file(q2)) == ["size"], str(res))
    real = os.path.join(project_root(), "data", "sky")
    if os.path.isdir(real):
        bmps = [os.path.join(real, f) for f in sorted(os.listdir(real)) if f.endswith(".bmp")]
        check("validate: data/sky passes the sky rule", bmps and not validate.errors(validate.check_paths(bmps)))


_PAL11 = "735e6e8466ac8d39"           # stable_hash of the 46 assetkit 1.1 names + values


def _pal_hash(names):
    return "%016x" % rng.stable_hash(*[(n,) + tuple(P.C[n]) for n in names])


_PAL11_NAMES = ("sun ambient fog horizon zenith asphalt asphalt_worn line_white line_yellow gravel dirt sand_shore "
                "grass grass_dry field rye forest_floor moss rock_granite rock_dark lake lake_deep spruce pine "
                "pine_bark birch_bark birch_mark birch_leaf bush punamulta punamulta_dark trim_white wall_yellow "
                "roof_tin roof_tile window wood_grey wood_dark concrete bale_white sign_yellow sign_red sign_black "
                "sign_white sign_blue glass").split()


def t_palette_merge():
    check("palette: 1.1 values unchanged", _pal_hash(_PAL11_NAMES) == _PAL11, _pal_hash(_PAL11_NAMES))
    new = ["spruce_tip", "juniper", "kerb_red", "granite_pink", "granite_boulder", "lichen", "lichen_pale",
           "church_cream", "brass", "fi_evening_fog", "fi_evening_zenith", "fi_evening_horizon"]
    check("palette: 1.2 names", all(n in P.C for n in new) and P.C["fog"] == (205, 170, 160) and
          P.C["fi_evening_fog"] == (203, 174, 153) and P.UI["accent"] == (242, 138, 38) and
          not any(P.is_near_key(c) for c in list(P.C.values()) + list(P.UI.values())))
    ls = P.LIGHT_SETS["fi_evening"]
    txt = os.path.join(project_root(), "data", "sky", "fi_evening.txt")
    ok = ls["fog"] == P.C["fi_evening_fog"] and ls["sun"] == P.C["sun"] and ls["ambient"] == P.C["ambient"]
    if os.path.exists(txt):
        kv = {}
        for line in open(txt):
            f = line.split("#")[0].split()
            if len(f) >= 2:
                kv[f[0]] = f[1:]
        for k in ("zenith", "top", "horizon", "fog", "sun", "ambient", "skyfill"):
            ok = ok and tuple(int(x) for x in kv[k]) == ls[k]
        for k in ("hemi", "sun_azim", "sun_elev", "fogStart", "fogEnd", "vHorizon", "elevLo", "elevHi"):
            ok = ok and abs(float(kv[k][0]) - ls[k]) < 1e-6
    check("palette: LIGHT_SETS fi_evening = data/sky txt", ok)


def t_facade():
    wall = M.Material("wall", "w")
    win = M.Material("win", "atlas")
    rects = {"door": (0.5, 0.0, 0.75, 0.5), "win": (0.0, 0.5, 0.25, 0.75)}
    outline = [(-4.0, 0.0), (-4.0, 3.0), (0.0, 5.0), (4.0, 3.0), (4.0, 0.0)]       # gable end, clockwise
    els = [(0.0, 0.0, 1.0, 2.0, "door"), (-2.5, 1.0, 1.2, 1.2, "win"), (2.5, 1.0, 1.2, 1.2, (0.1, 0.1, 0.2, 0.2))]
    g = M.facade(outline, (0.0, 0.0, -3.0), (1.0, 0.0, 0.0), els, wall, win, rects)
    area = sum(g.tri_area(t) for t in range(g.ntris))
    want = abs(M.poly_area2(outline)) * 0.5
    elem_area = sum(g.tri_area(t) for t in range(g.ntris) if g.TM[t] is win)
    door_uv = set(g.UV[i] for i in range(g.nverts) if abs(g.P[i][0]) <= 0.5 + 1e-9 and g.P[i][1] <= 2.0 + 1e-9)
    e = g.edge_report()
    check("mesh: facade covers wall, elements exact", abs(area - want) < 1e-6 and abs(elem_area - (2.0 + 2.88)) < 1e-6
          and all(n == (0.0, 0.0, -1.0) for n in g.N) and normals_agree(g) == 0 and e["nonmanifold"] == 0 and
          (0.5, 0.0) in door_uv and (0.75, 0.5) in door_uv and all(abs(p[2] + 3.0) < 1e-9 for p in g.P),
          "area %.3f/%.3f elem %.3f %s" % (area, want, elem_area, e))
    side = M.facade([(0, 0), (0, 2), (6, 2), (6, 0)], (1.0, 0.0, 0.0), (0.0, 0.0, 1.0), mat=wall, tile=2.0)
    try:
        M.facade(outline, (0, 0, 0), (1, 0, 0), [(3.9, 2.0, 1.0, 1.0, "win")], wall, win, rects)
        raised = False
    except ValueError:
        raised = True
    check("mesh: facade plain fan / outside element raises", side.ntris == 2 and side.N[0] == (1.0, 0.0, 0.0) and
          side.UV[2] == (3.0, -1.0) and raised)
    sq = M.clip_convex([(0, 0), (0, 2), (2, 2), (2, 0)], [(1, -1), (1, 3), (3, 3), (3, -1)])
    check("mesh: clip_convex", abs(abs(M.poly_area2(sq)) * 0.5 - 2.0) < 1e-9 and len(sq) == 4)
    b0 = M.box(1, 1, 1)
    b1 = M.box(1, 1, 1)
    b2 = M.box(1, 1, 1)
    M.prelight(b0, mode="object")
    M.prelight(b1, mode="object", wrap=M.WRAP_OBJECT)
    M.prelight(b2, mode="object", wrap=M.WRAP_SOFT)
    back = [i for i in range(b0.nverts) if b0.N[i] == (0.0, 0.0, 1.0)]
    check("mesh: prelight WRAP_OBJECT default, WRAP_SOFT softer", b0.C == b1.C and
          all(P.luma(b2.C[i]) > P.luma(b0.C[i]) for i in back))


def t_preview_mips():
    d = os.path.join(TMP, "miptex")
    os.makedirs(d)
    image.save_bmp(os.path.join(d, "chk.bmp"), image.checker(64, 64, 1, (255, 255, 255), (0, 0, 0)))
    g = M.quad((-20, 0, 0), (-20, 0, 400), (20, 0, 400), (20, 0, 0), M.Material("g", "chk"), (0, 0, 20, 200))
    g.set_color((255, 255, 255))
    cam = preview.Camera((0, 1.5, -2), (0, 0, 30), fov=60)
    lighting = "vertex"
    a = preview.render([g], cam, (96, 64), tex_dirs=[d], lighting=lighting)
    a2 = preview.render([g], cam, (96, 64), tex_dirs=[d], lighting=lighting, mips=False)
    b = preview.render([g], cam, (96, 64), tex_dirs=[d], lighting=lighting, mips=True)
    c = preview.render([g], cam, (96, 64), tex_dirs=[d], lighting=lighting, mips=True, mip_bias=-2.0)

    def contrast(img, y0, y1):
        vals = [img.getpixel((x, y))[0] for y in range(y0, y1) for x in range(20, 76)]
        m = sum(vals) / float(len(vals))
        return sum((v - m) ** 2 for v in vals) / len(vals)
    far_a, far_b = contrast(a, 33, 37), contrast(b, 33, 37)
    near_b, near_c = contrast(b, 55, 63), contrast(c, 55, 63)
    check("preview: mips (minified texels averaged, bias sharper)", a.tobytes() == a2.tobytes() and
          far_b < far_a * 0.25 and near_c > near_b + 1000, "var far %.0f -> %.0f, near %.0f -> bias -2 %.0f" % (
              far_a, far_b, near_b, near_c))
    ls = preview.lods(os.path.join(KIT_DIR, "samples", "out", "meshes", "sample_house.msh"), 96)
    check("preview: lods sheet (2-line labels)", ls.size == (192, 192))


def main():
    t0 = time.time()
    for fn in (t_rng, t_primitives, t_geo_ops, t_export, t_images, t_validator, t_preview, t_sample_build,
               t_smooth_groups, t_cap_bulge, t_strip_rows, t_bake, t_preview_followups, t_trk_records,
               t_build_imports, t_kit_helpers, t_sky_rule, t_palette_merge, t_facade, t_preview_mips):
        try:
            fn()
        except Exception as e:     # a crash in one group shouldn't hide the others
            import traceback
            traceback.print_exc()
            check(fn.__name__ + " (exception)", False, repr(e))
    shutil.rmtree(TMP, ignore_errors=True)
    nfail = sum(1 for _, ok in RESULTS if not ok)
    print("selftest: %d checks, %d passed, %d failed (%.1f s)" % (len(RESULTS), len(RESULTS) - nfail, nfail,
                                                                 time.time() - t0))
    return nfail


if __name__ == "__main__":
    sys.exit(main())
