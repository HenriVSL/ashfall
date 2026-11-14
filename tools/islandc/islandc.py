#!/usr/bin/env python3
"""islandc.py - Jet island compiler (T03, DESIGN 8, ENGINE.md 3.4). Python 3 + Pillow, deterministic, assetkit.

    python3 tools/islandc/islandc.py island/island.txt               # build into data/ (+ previews)
    python3 tools/islandc/islandc.py island/island.txt --check       # build twice into temp dirs: md5 A == B ==
                                                                     # data/, reader round trip + validation
    python3 tools/islandc/islandc.py island/island.txt --no-previews
    python3 tools/build_assets.py --only island [--check]            # same compiler through the asset runner

Outputs (relative to the data root): island/jet.isl (DESIGN 8.6, layout in isl.py), island/base.trk (RTRK, e_world),
tex/map_island.bmp + tex/map_island_a.bmp (minimap, 8.7; island.txt minimap_size, T03b 256x256),
island/report.txt. Previews: tools/islandc/out/*.png. Grid: island.txt `grid` (T03 513 x 32 m, T03b 513 x 80 m).

Pipeline: islandc (source) -> height (height field, airbase, coast rule, LOD0 border snap, quantise) -> classes
(per-quad classes, class pyramid, colour targets) -> bake (normals, shadow sweep, AO, vertex light) -> chunks
(5 LODs, per-class batches, skirts, foam) -> isl (RISL writer/reader) + base (base.trk) + preview.
"""
import hashlib
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
for _p in (HERE, os.path.join(ROOT, "engine", "tools")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

OUTPUTS = ["island/jet.isl", "island/base.trk", "tex/map_island.bmp", "tex/map_island_a.bmp", "island/report.txt",
           "island/veg.sct"]                                # A11: vegetation scatter (scatter.py, src/scatter.cpp)
PREVIEW_DIR = os.path.join(HERE, "out")

# ------------------------------------------------------------------------------------------------- source format
# key -> (default values, repeatable). Values are numbers unless the default holds a string.
SPEC = {
    "seed": ([7101], False),
    "grid": ([513.0, 32.0], False),
    "feature_scale": ([1.0], False),
    "coast_detail": ([5.0, 90.0], False),
    "bay": ([], True),
    "beach": ([0.0, 0.5], False),
    "islet": ([], True),
    "edge_fade": ([0.0], False),
    "noise_scale": ([1.0], False),
    "minimap_size": ([128.0], False),
    "island_center": ([0.0, 0.0], False),
    "island_radii": ([6000.0, 5000.0], False),
    "island_rot": ([0.0], False),
    "coast_wobble": ([0.1, 0.015], False),
    "plain": ([26.0, 950.0], False),
    "hills": ([60.0, 1700.0, 700.0], False),
    "cliffs": ([60.0, 55.0, 45.0, 70.0], False),
    "shelf": ([900.0, 60.0], False),
    "coast_depth": ([4.0], False),
    "beach_min": ([1.0], False),
    "cone_center": ([0.0, 0.0], False),
    "cone_radius": ([5800.0], False),
    "cone_exp": ([1.6], False),
    "summit": ([1750.0], False),
    "crater": ([450.0, 220.0, 0.42], False),
    "rim_noise": ([45.0], False),
    "ridges": ([27.0, 60.0, 1300.0], False),
    "erosion": ([45.0, 850.0], False),
    "lava_levee": ([3.0], False),
    "lava": ([], True),
    "satellite": ([], True),
    "runway_center": ([0.0, -4000.0], False),
    "runway_heading": ([270.0], False),
    "runway_size": ([2800.0, 45.0], False),
    "plateau": ([12.0, 300.0], False),
    "plateau_rect": ([-1650.0, 1650.0, -260.0, 560.0], False),
    "taxiway": ([200.0, 23.0, 60.0], False),
    "apron": ([0.0, 300.0, 150.0], False),
    "building": ([], True),
    "air_spawns": ([12000.0, 2000.0], False),
    "climbout": ([5000.0, 600.0, 1500.0, 150.0], False),
    "beach_max": ([4.0], False),
    "grass_max": ([600.0], False),
    "ash_min": ([720.0], False),
    "grass_slope": ([25.0], False),
    "rock_slope": ([31.0], False),
    "tint": ([], True),
    # A11 (island detail + A6 colour pass; classes.py docstring, scatter.py)
    "moisture": ([65.0, 0.0], False),
    "forest": ([0.58, 0.0, 1060.0], False),
    "fields": ([0.0, 7.0, 0.0], False),
    "scrub_belt": ([0.0, 0.0], False),
    "lava_cool": ([0.0], False),
    "outcrops": ([0.0], False),
    "sand_black": ([0.0], False),
    "scatter": ([], True),
    "scatter_range": ([1280.0], False),
    "prop": ([], True),
    "fence": ([], True),
    "period": ([], True),
    "light": (["sky/light.txt"], False),
    "sun_dir": ([230.0, 40.0], False),
    "shadow": ([0.88], False),
    "ao": ([0.10], False),
}

DEFAULT_TINTS = {   # A3 HANDOFF suggested tints (+ T03 variants)
    "sand": (253, 235, 189), "grass": (116, 169, 75), "grass_forest": (72, 118, 56), "grass_dry": (164, 172, 99),
    "grass_coast": (150, 176, 92), "plateau": (136, 178, 86), "rock": (134, 131, 124),
    "rock_crater": (128, 108, 98), "ash": (164, 155, 147), "ash_red": (182, 140, 120), "lava": (230, 222, 214),
    "shelf": (102, 198, 189), "deep": (42, 102, 124),
    # A11
    "forest_conifer": (52, 92, 64), "forest_light": (96, 142, 64), "grass_wet": (96, 160, 70), "scrub": (150, 150, 92), "scrub_heath": (128, 112, 84),
    "basalt": (70, 66, 64), "sand_black": (92, 88, 86), "ash_sulphur": (200, 186, 110), "rock_lowland": (140, 138, 128),
    "field_pasture": (110, 178, 72), "field_hay": (176, 182, 96), "field_crop": (78, 140, 58),
    "field_stubble": (196, 176, 112), "field_plough": (132, 100, 76)}
DEFAULT_PERIODS = {"t_sand": 32.0, "t_grass": 64.0, "t_rock": 64.0, "t_ash": 64.0, "t_lava": 64.0, "t_forest": 64.0,
                   "t_scrub": 64.0}


class Source(object):
    """Parsed island.txt: get(key) -> list of values (defaults filled), rep(key) -> list of lists."""

    def __init__(self, path):
        self.path = path
        self.vals = {}
        self.reps = {}
        with open(path, "rb") as f:
            text = f.read().decode("latin-1")
        for ln, line in enumerate(text.replace("\r\n", "\n").split("\n"), 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            key, args = parts[0], parts[1:]
            if key not in SPEC:
                raise ValueError("%s:%d: unknown key %r" % (path, ln, key))
            default, rep = SPEC[key]
            conv = []
            for k, a in enumerate(args):
                if key in ("tint", "period", "building", "scatter", "prop") and k == 0:
                    conv.append(a)
                elif default and isinstance(default[0], str):
                    conv.append(a)
                else:
                    try:
                        conv.append(float(a))
                    except ValueError:
                        raise ValueError("%s:%d: %s: %r is not a number" % (path, ln, key, a))
            if rep:
                self.reps.setdefault(key, []).append(conv)
            else:
                if key in self.vals:
                    raise ValueError("%s:%d: key %r given twice" % (path, ln, key))
                if len(conv) != len(default):
                    raise ValueError("%s:%d: %s needs %d values" % (path, ln, key, len(default)))
                self.vals[key] = conv

    def get(self, key):
        return list(self.vals.get(key, SPEC[key][0]))

    def g1(self, key):
        return self.get(key)[0]

    def rep(self, key):
        return [list(v) for v in self.reps.get(key, [])]

    def tints(self):
        t = dict(DEFAULT_TINTS)
        for v in self.rep("tint"):
            if len(v) != 4:
                raise ValueError("tint needs: name r g b")
            t[v[0]] = (int(v[1]), int(v[2]), int(v[3]))
        return t

    def chunk_m(self):
        return 16.0 * self.get("grid")[1]

    def periods(self):
        p = dict(DEFAULT_PERIODS)
        for v in self.rep("period"):
            if len(v) != 2:
                raise ValueError("period needs: texture metres")
            p[v[0]] = v[1]
        cm = self.chunk_m()
        for t, v in sorted(p.items()):
            if cm % v != 0.0:
                raise ValueError("period %s %g does not divide the %g m chunk" % (t, v, cm))
        return p


def read_light(kit, src):
    """A2's light set (data/sky/light.txt) as a dict of float lists; fallback from island.txt sun_dir."""
    import math
    rel = src.g1("light")
    path = kit.find(rel) if kit is not None else None
    L = {}
    if path:
        with open(path, "rb") as f:
            for line in f.read().decode("latin-1").splitlines():
                line = line.split("#", 1)[0].split()
                if len(line) < 2:
                    continue
                try:
                    L[line[0]] = [float(x) for x in line[1:]]
                except ValueError:
                    pass
        L["_source"] = rel
    if "sunDir" not in L:
        az, el = [math.radians(v) for v in src.get("sun_dir")]
        L["sunDir"] = [math.sin(az) * math.cos(el), math.sin(el), math.cos(az) * math.cos(el)]
        L["_source"] = "island.txt sun_dir (no %s)" % rel
    L.setdefault("sun", [255.0, 244.0, 226.0])
    L.setdefault("ambient", [132.0, 150.0, 178.0])
    L.setdefault("hemi", [0.35])
    L.setdefault("fog", [196.0, 212.0, 228.0])
    L.setdefault("fogStart", [3600.0])
    L.setdefault("fogEnd", [18000.0])
    return L


# ------------------------------------------------------------------------------------------------- build

def build_island(src_path, out_dir, kit):
    """assetkit track protocol (tools/build_assets.py): writes OUTPUTS under out_dir, returns them."""
    import height
    import classes
    import bake
    import chunks
    import isl
    import base
    import preview
    import scatter

    src = Source(src_path)
    light = read_light(kit, src)
    tex_means = classes.texture_means(kit)

    hf = height.build(src)                                  # height grid + aux fields + airbase frame
    cl = classes.build(src, hf, tex_means)                  # classes, pyramid, colour targets
    bk = bake.build(src, hf, light)                         # per-LOD vertex light
    ch = chunks.build(src, hf, cl, bk)                      # chunk meshes
    bs = base.build(src, hf, light, kit)                    # base.trk + BASE chunk data
    mp = preview.minimap(hf, cl, bk, bs, int(src.g1("minimap_size")))   # minimap image + rect
    sc = scatter.build(src, hf, cl, bk, kit)                # A11 vegetation scatter
    if sc.data is None:
        raise RuntimeError("; ".join(sc.notes))
    with open(kit.out("island/veg.sct"), "wb") as f:
        f.write(sc.data)

    data = isl.encode(hf, cl, ch, bs, mp)
    with open(kit.out("island/jet.isl"), "wb") as f:
        f.write(data)
    with open(kit.out("island/base.trk"), "wb") as f:
        f.write(bs.trk)
    kit.save_bmp("tex/map_island.bmp", mp.image, alpha=True)

    parsed = isl.decode(data)
    probs = isl.check(parsed, data)
    if probs:
        raise RuntimeError("jet.isl self-check failed:\n  " + "\n  ".join(probs[:20]))
    rep = report_text(src, hf, cl, bk, ch, bs, mp, data, parsed, light, sc)
    kit.save_text("island/report.txt", rep, crlf=True)

    if kit.previews:
        if not os.path.isdir(PREVIEW_DIR):
            os.makedirs(PREVIEW_DIR)
        preview.all_previews(PREVIEW_DIR, src, hf, cl, bk, ch, bs, mp, light, kit, log=kit.log, sc=sc)
    return list(OUTPUTS)


def report_text(src, hf, cl, bk, ch, bs, mp, data, parsed, light, sc=None):
    import isl
    out = []
    w = out.append
    w("# report.txt - jet.isl build report (islandc, T03/T03b). GENERATED: do not edit.")
    w("source %s" % os.path.basename(src.path))
    w("file jet.isl bytes=%d (%.2f MB, budget <= 16 MB (T03b; load ~1.1 MB/s over SMB): %s)" % (
        len(data), len(data) / 1048576.0, "ok" if len(data) <= 16 * 1048576 else "OVER"))
    w("file base.trk bytes=%d" % len(bs.trk))
    w("light %s sunDir %.6f %.6f %.6f" % (light.get("_source"), light["sunDir"][0], light["sunDir"][1],
                                          light["sunDir"][2]))
    w("bake: cast shadow (sh > 0.5) on %.2f %% of land samples; flat sunlit ground = light 1.0 per channel" %
      (100.0 * bk.shadow_frac))
    w("")
    w("[grid]")
    w("gridN %d cell %.0f x0 %.0f z0 %.0f  hMin %.1f hScale %.3f" % (hf.N, hf.cell, hf.x0, hf.z0, hf.hmin,
                                                                   hf.hscale))
    w("height min %.2f max %.2f (land samples %d, %.1f km2)" % (hf.hlo, hf.hhi, hf.land_samples,
                                                                  hf.land_samples * hf.cell * hf.cell / 1e6))
    w("summit max %.1f at x %.0f z %.0f; crater floor %.1f" % (hf.peak[0], hf.peak[1], hf.peak[2], hf.crater_floor))
    w("island centre %.0f %.0f radius %.0f" % hf.centre)
    for line in hf.notes:
        w(line)
    w("")
    w("[classes] share of land quads (water = below sea level)")
    tot = float(len(cl.cls))
    for k, name in enumerate(isl.CLASS_NAMES):
        n = cl.counts[k]
        w("class %d %-7s quads %6d  %5.1f %%  tint %s" % (k, name, n, 100.0 * n / tot, cl.class_tint_str(k)))
    w("")
    w("[material classes] A11: grass split by texture (CLAS keeps grass); share of land quads")
    import classes as CLm
    land = float(sum(cl.mcounts[1:]))
    w(" ".join("%s %.1f%%" % (CLm.MCLASS_NAMES[k], 100.0 * cl.mcounts[k] / land) for k in range(1, CLm.NMCLASS)))
    w("")
    w("[scatter] A11 (scatter.py -> veg.sct, drawn by src/scatter.cpp). Budgets: Low <= 600 sprites within 1.6 km "
      "(VM A measured ~11 us/sprite: 130-175 sprites = 1.5-1.9 ms at 120-150 m AGL over forest/fields/coast, 0.2 ms "
      "avg on the low bench route, ~0 above 1.6 km AGL), High <= 2400 within 3.5 km (VM: 750-910 sprites = 7.7-8.3 ms; "
      "T40 target). Textures: veg_atlas 171 KB, t_forest + t_scrub 2 x 171 KB, props/fence ~50 KB")
    if sc is not None:
        import scatter as SCm
        for line in SCm.report_lines(sc):
            w(line)
    w("")
    w("[materials]")
    for i, m in enumerate(ch.materials):
        w("mat %d %-8s flags 0x%04x tint %08x period %s" % (i, m[0], m[1], m[2], m[3]))
    w("")
    w("[chunks]")
    w("stored %d of %d (%d all-underwater: LOD0/1 alias LOD2)" % (len(ch.chunks), hf.NC * hf.NC, ch.n_underwater))
    w("lod edge_m  tris_total  terrain  skirts  foam   tris/chunk(max)  verts_total  batches")
    for k in range(5):
        s = ch.stats[k]
        w("%d   %4d    %8d  %8d  %6d  %5d   %6d          %8d    %6d" % (
            k, int(hf.cell) << k, s["tris"], s["terrain"], s["skirt"], s["foam"], s["maxtris"], s["verts"],
            s["batches"]))
    w("vertices %d (%d KB)  indices %d  batches %d" % (ch.nverts, ch.nverts * 24 // 1024, ch.nidx, ch.nbatches))
    w("max UV %.2f (terrain) %.2f (foam)" % (ch.max_uv, ch.max_uv_foam))
    w("skirt depth by LOD (m, max of DESIGN min and the measured border mismatch + 1): " +
      " ".join("%d:%.1f-%.1f" % (k, ch.skirt_rng[k][0], ch.skirt_rng[k][1]) for k in range(1, 5)))
    w("max border mismatch between LODs (m): " + " ".join("%d:%.1f" % (k, ch.mismatch[k]) for k in range(5)))
    w("coast foam segments LOD0 %d LOD1 %d LOD2 %d" % tuple(ch.foam_segs))
    w("")
    w("[views] engine LOD rule (d_k = lodScale x 28.6 x edge_k to the chunk centre, far 18 km, 70 deg), terrain + "
      "foam triangles, skirts counted on every side (upper bound; the engine draws them only at LOD changes: use "
      "'simh island 0.85' for the exact engine numbers); DESIGN 9.3 Low = 1.5-3k typ., 4k max")
    import preview
    for line in preview.viewpoint_lines(hf, ch, bs):
        w(line)
    w("")
    w("[base]")
    b = bs
    w("runway centre %.1f %.1f heading %.1f length %.0f width %.0f height %.2f" % (
        b.rwy[0], b.rwy[1], b.rwy[2], b.rwy[3], b.rwy[4], b.rwy[5]))
    w("threshold spawn %.1f %.2f %.1f heading %.1f" % (b.thr[0], b.thr[1], b.thr[2], b.thr[3]))
    for s in b.air:
        w("air spawn %.0f %.0f %.0f heading %.1f" % s)
    w("designation %s / %s" % b.designators)
    for line in b.notes:
        w(line)
    for nm, box in b.boxes:
        w("box %-7s %.1f %.1f %.1f  %.1f %.1f %.1f" % ((nm,) + tuple(box)))
    w("")
    w("[minimap]")
    w("map %s %dx%d rect x0 %.0f z0 %.0f x1 %.0f z1 %.0f (%.0f m/px)" % (
        mp.name, mp.size, mp.size, mp.rect[0], mp.rect[1], mp.rect[2], mp.rect[3],
        (mp.rect[2] - mp.rect[0]) / float(mp.size)))
    w("")
    w("[chunk list] cx cz  tris LOD0..4  land%")
    for c in ch.chunks:
        w("chunk %2d %2d  %4d %4d %4d %3d %3d  %3d" % ((c.cx, c.cz) + tuple(c.tris) + (int(c.land * 100),)))
    return "\n".join(out) + "\n"


# ------------------------------------------------------------------------------------------------- CLI

def _md5(p):
    with open(p, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def _run(src, tmp, previews):
    from assetkit import build as B
    data_dir = os.path.join(ROOT, "data")
    kit = B.Kit("island", ROOT, tmp, data_dir, PREVIEW_DIR, check=not previews, previews=previews)
    return build_island(src, tmp, kit)


def main(argv):
    import time
    args = [a for a in argv if not a.startswith("--")]
    flags = set(a for a in argv if a.startswith("--"))
    src = args[0] if args else os.path.join(ROOT, "island", "island.txt")
    data_dir = os.path.join(ROOT, "data")
    from assetkit import validate as VA
    t0 = time.time()
    if "--check" in flags:
        tA = tempfile.mkdtemp(prefix="islandc_a_")
        tB = tempfile.mkdtemp(prefix="islandc_b_")
        bad = 0
        try:
            outs = _run(src, tA, False)
            _run(src, tB, False)
            import isl
            for rel in outs:
                a, b = _md5(os.path.join(tA, rel)), _md5(os.path.join(tB, rel))
                d = os.path.join(data_dir, rel)
                dd = _md5(d) if os.path.exists(d) else "missing"
                ok = a == b == dd
                bad += 0 if ok else 1
                print("check %-22s %s run A %s run B %s data %s" % (rel, "identical" if ok else "DIFF", a[:12],
                                                                    b[:12], dd[:12]))
            with open(os.path.join(tA, "island/jet.isl"), "rb") as f:
                blob = f.read()
            parsed = isl.decode(blob)
            probs = isl.check(parsed, blob)
            rt = isl.reencode(parsed) == blob
            print("reader: %d chunks, %d stored island chunks, problems %d, round trip %s" % (
                len(parsed.order), len(parsed.chunks), len(probs), "identical" if rt else "DIFF"))
            for p in probs[:20]:
                print("  problem: %s" % p)
            bad += len(probs) + (0 if rt else 1)
            vp = VA.check_paths([os.path.join(tA, r) for r in outs], None, [os.path.join(data_dir, "tex")])
            ve = VA.errors(vp)
            for p in vp:
                print("  validate %s: %s [%s] %s" % (p.level, os.path.relpath(p.path, tA), p.code, p.msg))
            bad += len(ve)
        finally:
            shutil.rmtree(tA, ignore_errors=True)
            shutil.rmtree(tB, ignore_errors=True)
        print("islandc --check: %s (%.1f s)" % ("OK" if not bad else "FAILED %d" % bad, time.time() - t0))
        return 1 if bad else 0
    tmp = tempfile.mkdtemp(prefix="islandc_")
    try:
        outs = _run(src, tmp, "--no-previews" not in flags)
        vp = VA.check_paths([os.path.join(tmp, r) for r in outs], None, [os.path.join(data_dir, "tex")])
        for p in vp:
            print("  validate %s: %s [%s] %s" % (p.level, os.path.relpath(p.path, tmp), p.code, p.msg))
        if VA.errors(vp):
            print("islandc: validation errors, data/ not updated")
            return 1
        for rel in outs:
            s, d = os.path.join(tmp, rel), os.path.join(data_dir, rel)
            if not os.path.isdir(os.path.dirname(d)):
                os.makedirs(os.path.dirname(d))
            same = os.path.exists(d) and _md5(d) == _md5(s)
            if not same:
                shutil.copyfile(s, d)
            print("%-22s %s %s" % (rel, "unchanged" if same else "written", _md5(s)[:12]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("islandc: done (%.1f s)" % (time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
