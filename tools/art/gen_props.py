"""gen_props.py - Jet airbase props (task A11, A7 generator style): vehicles, apron light masts, windsock, fence.

    python3 tools/build_assets.py --only props [--check]       # or: python3 tools/art/gen_props.py [--check]

Placed by islandc from island.txt: `prop MESH along right yaw` (INST without a collision box, base.py; drawn to
PROP_DIST) and `fence a0 b0 a1 b1 ...` (runway-frame polyline -> a double-sided 2.4 m keyed chain-link ribbon in
the base cells, texture `fence`). Simple low-poly shapes, pre-lit with A2's light set like A7's buildings; colours
are vertex tints over one neutral texture (props.bmp). Origin = ground centre, front (cab, lamp face) toward -Z.

  mesh       size x * z * h       tris   notes
  truck      2.5 x 7.6 x 3.0      ~70    olive fuel bowser: cab + tank + wheels
  tug        2.0 x 3.6 x 1.8      ~40    yellow aircraft tug
  jeep       1.8 x 4.0 x 1.9      ~50    olive utility vehicle with windscreen
  lightmast  2.6 x 1.2 x 18.0     ~40    apron floodlight mast, lamp head facing -Z
  windsock   1.4 x 3.2 x 6.4      ~40    pole + orange/white sock pointing +Z (downwind)
Textures: props.bmp 64x64 neutral (light grain, grey 200), fence.bmp 128x32 keyed (posts every 32 px = 3 m,
diagonal mesh, top wire; 1 repeat per 12 m along, 2.4 m tall).
"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "tools"))

from PIL import Image, ImageDraw  # noqa: E402

from assetkit import mesh as M  # noqa: E402
from assetkit import preview as V  # noqa: E402
from assetkit.rng import Rng  # noqa: E402

SEED = 1212
ORDER = 46
MESHES = ["truck", "tug", "jeep", "lightmast", "windsock"]
ASSETS = (["tex/props.bmp", "tex/fence.bmp"] + [{"file": "meshes/%s.msh" % n, "class": "bldg"} for n in MESHES])

LIGHT_DEFAULT = {"sun": (255, 244, 226), "ambient": (132, 150, 178), "hemi": 0.35}
OLIVE = (98, 104, 72)
OLIVE_D = (70, 74, 54)
TYRE = (34, 34, 36)
GLASS = (54, 66, 80)
YELLOW = (214, 170, 40)
GREY = (150, 152, 150)
LAMP = (250, 246, 220)
ORANGE = (236, 104, 34)
WHITE = (236, 236, 232)


def read_light(kit):
    out = dict(LIGHT_DEFAULT)
    p = kit.find("sky/light.txt") if kit is not None else None
    if p:
        with open(p, "rb") as f:
            for line in f.read().decode("latin-1").splitlines():
                v = line.split("#", 1)[0].split()
                if len(v) >= 4 and v[0] in ("sun", "ambient"):
                    out[v[0]] = tuple(int(float(x)) for x in v[1:4])
                elif len(v) >= 2 and v[0] == "hemi":
                    out["hemi"] = float(v[1])
    return out


def tex_props():
    r = Rng(SEED, "props_tex")
    img = Image.new("RGB", (64, 64))
    px = img.load()
    for y in range(64):
        for x in range(64):
            v = 200 + int(r.range(-10, 10))
            px[x, y] = (v, v, v)
    return img


def tex_fence():
    w, h = 128, 32
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    wire = (168, 172, 170, 255)
    for k in range(-h, w + h, 4):                          # diagonal mesh (both directions)
        d.line([(k, 3), (k + h, h - 1)], fill=wire, width=1)
        d.line([(k + h, 3), (k, h - 1)], fill=wire, width=1)
    d.line([(0, 2), (w, 2)], fill=(140, 142, 140, 255), width=1)     # top wire
    d.line([(0, 3), (w, 3)], fill=(140, 142, 140, 255), width=1)
    for x in range(0, w, 32):                              # posts (3 m)
        d.rectangle([x, 0, x + 1, h - 1], fill=(96, 98, 96, 255))
    return img


def colbox(g, sx, sy, sz, pos, col, mat):
    g.merge(M.box(sx, sy, sz, pos=pos, mat=mat, color=col + (255,), skip=("-y",)))


def finish(g, light):
    g.weld()
    M.prelight(g, mode="object", sun=light["sun"], ambient=light["ambient"], hemi=light["hemi"], ao_height=1.0,
               ao_min=0.75)
    return g


def wheels(g, mat, xs, zs, r=0.5, w=0.35):
    for x in xs:
        for z in zs:
            colbox(g, w, 2 * r, 2 * r, (x, 0.0, z), TYRE, mat)


def truck(mat):
    g = M.Geo()
    colbox(g, 2.4, 1.0, 7.4, (0.0, 0.5, 0.0), OLIVE_D, mat)              # chassis
    colbox(g, 2.4, 1.6, 2.0, (0.0, 1.5, -2.6), OLIVE, mat)               # cab
    colbox(g, 2.2, 0.7, 0.05, (0.0, 2.2, -3.62), GLASS, mat)             # windscreen
    g.merge(M.lathe([(1.1, 0.0), (1.15, 0.2), (1.15, 4.8), (1.1, 5.0)], seg=8, mat=mat, caps=True,
                    color=OLIVE + (255,)).rotate_x(90.0).translate(0.0, 2.35, -1.2))  # tank along +Z
    wheels(g, mat, (-1.2, 1.2), (-2.6, 1.4, 2.6))
    return g


def tug(mat):
    g = M.Geo()
    colbox(g, 2.0, 0.9, 3.6, (0.0, 0.3, 0.0), YELLOW, mat)
    colbox(g, 1.0, 0.8, 0.9, (0.0, 1.2, 0.9), (60, 60, 62), mat)          # seat/cowl
    colbox(g, 0.1, 0.9, 0.1, (0.0, 1.2, -1.75), GREY, mat)                # tow bar post
    wheels(g, mat, (-0.95, 0.95), (-1.1, 1.1), r=0.35, w=0.3)
    return g


def jeep(mat):
    g = M.Geo()
    colbox(g, 1.8, 0.8, 4.0, (0.0, 0.45, 0.0), OLIVE, mat)
    colbox(g, 1.7, 0.6, 1.8, (0.0, 1.25, 0.6), OLIVE_D, mat)             # canvas top
    colbox(g, 1.6, 0.55, 0.05, (0.0, 1.3, -0.4), GLASS, mat)             # windscreen
    wheels(g, mat, (-0.9, 0.9), (-1.3, 1.3), r=0.4, w=0.3)
    return g


def lightmast(mat):
    g = M.Geo()
    colbox(g, 1.2, 0.6, 1.2, (0.0, 0.0, 0.0), GREY, mat)                  # footing
    g.merge(M.lathe([(0.25, 0.0), (0.14, 17.0)], seg=6, mat=mat, caps=False, color=GREY + (255,)))
    colbox(g, 2.6, 0.9, 0.5, (0.0, 17.1, 0.0), (90, 92, 92), mat)         # lamp bar
    colbox(g, 2.4, 0.7, 0.05, (0.0, 17.2, -0.27), LAMP, mat)              # lamp faces (-Z)
    return g


def windsock(mat):
    g = M.Geo()
    g.merge(M.lathe([(0.08, 0.0), (0.06, 6.4)], seg=5, mat=mat, caps=False, color=WHITE + (255,)))
    for k, (r0, r1, col) in enumerate(((0.65, 0.58, ORANGE), (0.58, 0.5, WHITE), (0.5, 0.42, ORANGE),
                                       (0.42, 0.35, WHITE), (0.35, 0.28, ORANGE))):
        z0, z1 = 0.1 + k * 0.62, 0.1 + (k + 1) * 0.62
        s = M.lathe([(r0, z0), (r1, z1)], seg=6, mat=mat, caps=False, color=col + (255,))
        s.rotate_x(90.0)
        s.translate(0.0, 5.9 - k * 0.12, 0.0)
        g.merge(s)
    return g


BUILDERS = {"truck": truck, "tug": tug, "jeep": jeep, "lightmast": lightmast, "windsock": windsock}


def build_mesh(name, light):
    mb = M.MeshBuilder(name)
    mat = mb.material("props", tex="props", cull_none=(name == "windsock"))   # the sock is an open tube
    mb.lod(None)
    g = BUILDERS[name](mat)
    mb.add(finish(g, light))
    return mb


def previews(kit, meshes):
    items = []
    for k, (name, mb) in enumerate(meshes):
        items.append((mb, (k * 8.0 - 16.0, 0.0, 0.0), 30.0))
    img = V.scene(items, (0.0, 9.0, -30.0), (0.0, 4.0, 0.0), size=(640, 360), fov=50.0, fog=(200.0, 600.0),
                  ground=((120, 124, 116), 200.0), tex_dirs=kit.tex_dirs, q16=True, lighting="vertex")
    V.label(img, "A11 base props: truck tug jeep lightmast windsock", color=(255, 255, 255))
    kit.save_preview("props_lineup.png", img)


def build(out_dir, kit):
    light = read_light(kit)
    kit.save_bmp("tex/props.bmp", tex_props())
    kit.save_bmp("tex/fence.bmp", tex_fence(), keyed=True)
    meshes = []
    for name in MESHES:
        mb = build_mesh(name, light)
        kit.save_msh("meshes/%s.msh" % name, mb)
        lo, hi = V.bounds_of(mb)
        kit.log("%-9s tris %s  box x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f" % (
            name, mb.tri_counts(), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
        meshes.append((name, mb))
    if kit.previews:
        previews(kit, meshes)


if __name__ == "__main__":
    from assetkit import build as B
    sys.exit(B.main_for(__file__))
