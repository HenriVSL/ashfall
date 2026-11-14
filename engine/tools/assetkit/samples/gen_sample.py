"""Sample generator for the asset toolkit (and the reference template for real generators).

Builds a small red box house with a keyed warning sign on its front, and a crossed-billboard pine tree,
with their textures (one keyed atlas), plus preview PNGs. Used by selftest.py and documented in README.md.
Outputs go to engine/tools/assetkit/samples/out/ (the sample's data root), never into the game's data/.

    python3 tools/build_assets.py --only sample          # or: python3 engine/tools/assetkit/samples/gen_sample.py
"""

import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

from assetkit import image as I  # noqa: E402
from assetkit import mesh as M  # noqa: E402
from assetkit import palette as P  # noqa: E402
from assetkit import preview as V  # noqa: E402
from assetkit.rng import Rng, noise_grid  # noqa: E402

SEED = 7

ASSETS = [
    "tex/sample_wall.bmp",
    "tex/sample_roof.bmp",
    "tex/sample_atlas.bmp",
    {"file": "meshes/sample_house.msh", "class": "building"},
    {"file": "meshes/sample_tree.msh", "class": "tree"},
]


# ---------------------------------------------------------------------------------------------------- textures

def tex_wall():
    """128x128 vertical red boards (punamulta) with dark gaps; tiles in both directions."""
    w = h = 128
    r = Rng(SEED, "wall")
    img = I.new(w, h, P.C["punamulta"])
    px = img.load()
    for b in range(8):                       # 8 boards of 16 px
        f = 1.0 + r.jitter(0.0, 0.07)
        for y in range(h):
            for x in range(b * 16, b * 16 + 16):
                c = P.shade(P.C["punamulta"], f)
                if x == b * 16:
                    c = P.C["punamulta_dark"]
                px[x, y] = c
    I.apply_noise(img, noise_grid(w, h, cells=8, octaves=3, seed=SEED), amount=0.08)
    return img


def tex_roof():
    """64x64 grey tin with vertical ridges."""
    img = I.noise_image(64, 64, P.shade(P.C["roof_tin"], 0.9), P.shade(P.C["roof_tin"], 1.1), cells=4,
                        octaves=3, seed=SEED + 1)
    px = img.load()
    for y in range(64):
        for x in range(0, 64, 8):
            px[x, y] = P.shade(px[x, y], 0.7)
            px[x + 1, y] = P.shade(px[x + 1, y], 1.2)
    return img


def art_sign():
    """96x84 Finnish warning triangle (yellow, red border), keyed outside."""
    def draw(d, img, s):
        tri = [(48 * s, 2 * s), (94 * s, 82 * s), (2 * s, 82 * s)]
        d.polygon(tri, fill=P.C["sign_red"] + (255,))
        inner = [(48 * s, 16 * s), (82 * s, 75 * s), (14 * s, 75 * s)]
        d.polygon(inner, fill=P.C["sign_yellow"] + (255,))
        d.rectangle([44 * s, 34 * s, 52 * s, 60 * s], fill=P.C["sign_black"] + (255,))
        d.rectangle([44 * s, 64 * s, 52 * s, 70 * s], fill=P.C["sign_black"] + (255,))
    return I.supersample(96, 84, draw)


def art_tree():
    """96x192 pine silhouette: trunk + layered needle triangles, keyed outside."""
    r = Rng(SEED, "tree")

    def draw(d, img, s):
        d.rectangle([44 * s, 120 * s, 52 * s, 191 * s], fill=P.C["pine_bark"] + (255,))
        for k in range(7):
            y0 = 8 + k * 22
            hw = 12 + k * 6 + r.jitter(0, 2)
            c = P.shade(P.C["pine"], 0.75 + 0.06 * k)
            d.polygon([(48 * s, y0 * s), ((48 + hw) * s, (y0 + 38) * s), ((48 - hw) * s, (y0 + 38) * s)],
                      fill=c + (255,))
    return I.supersample(96, 192, draw)


def art_window():
    """48x48 opaque window: white frame, dark glass with a sky glint."""
    img = I.new(48, 48, P.C["trim_white"])
    px = img.load()
    for y in range(5, 43):
        for x in range(5, 43):
            if 22 <= x <= 25 or 22 <= y <= 25:
                continue
            t = (x + y) / 96.0
            px[x, y] = P.lerp(P.C["window"], P.shade(P.C["zenith"], 0.8), t * 0.6)
    return img.convert("RGBA")


def make_atlas():
    at = I.Atlas(256, 256, gutter=2)
    at.add("tree", art_tree())
    at.add("sign", art_sign())
    at.add("window", art_window())
    at.pack()
    return at


# ---------------------------------------------------------------------------------------------------- meshes

def build_house(at):
    mb = M.MeshBuilder("sample_house")
    wall = mb.material("wall", tex="sample_wall")
    roof = mb.material("roof", tex="sample_roof")
    glass = mb.material("window", tex="sample_atlas")
    sign = mb.material("sign", tex="sample_atlas", blend=M.RB_ALPHATEST, cull_none=True)

    W, D, H, RIDGE = 6.0, 4.5, 3.0, 1.6
    pitch = math.degrees(math.atan2(RIDGE, D * 0.5))
    slope = math.hypot(RIDGE, D * 0.5) + 0.35

    def roof_slab(side, thick):
        # slab along X; tilt toward +-Z, then move so its top edge meets the ridge
        g = M.box(W + 0.6, thick, slope, base=False, mat=roof, uv_scale=0.5)
        g.translate(0.0, 0.0, side * slope * 0.5)
        g.rotate_x(side * pitch)      # +side: the +Z half tilts down (rotate_x positive turns +Z toward -Y)
        return g.translate(0.0, H + RIDGE + 0.08, 0.0)

    # LOD0: walls, attic (gable prism), two thick roof slabs, chimney, door, two windows, a keyed sign
    lod0 = M.Geo()
    lod0.merge(M.box(W, H, D, mat=wall, uv_scale=0.5, skip=("-y",)))
    attic = M.extrude([(-D * 0.5, 0.0), (D * 0.5, 0.0), (0.0, RIDGE)], [(-W * 0.5, H, 0.0), (W * 0.5, H, 0.0)],
                      closed=True, caps=True, mat=wall, frames="horizontal", crease=10.0)
    lod0.merge(attic.uv_box(0.5))
    lod0.merge(roof_slab(1, 0.12), roof_slab(-1, 0.12))
    lod0.merge(M.box(0.5, 1.6, 0.5, pos=(1.6, H + 0.6, 0.7), mat=roof, uv_scale=1.0, skip=("-y",)))
    lod0.merge(M.box(0.9, 1.9, 0.08, pos=(-1.5, 0.0, -D * 0.5 - 0.04), mat=roof, uv_scale=1.0,
                     skip=("+z", "-y")))
    for x in (0.4, 1.9):
        lod0.merge(M.vquad(0.9, 0.9, (x, 1.2, -D * 0.5 - 0.01), 0.0, glass, at.uv("window")))
    lod0.merge(M.vquad(0.9, 0.8, (-1.5, 2.05, -D * 0.5 - 0.06), 0.0, sign, at.uv("sign")))
    M.prelight(lod0, mode="object")

    # LOD1: walls + attic + thin roof planes (no chimney/door/sign)
    lod1 = M.Geo()
    lod1.merge(M.box(W, H, D, mat=wall, uv_scale=0.5, skip=("-y",)), attic.copy())
    for side in (1, -1):
        g = M.quad((-W * 0.5 - 0.3, 0.0, 0.0), (-W * 0.5 - 0.3, 0.0, side * slope),
                   (W * 0.5 + 0.3, 0.0, side * slope), (W * 0.5 + 0.3, 0.0, 0.0), mat=roof)
        if side < 0:
            g.flip()
        g.rotate_x(side * pitch).translate(0.0, H + RIDGE + 0.14, 0.0).uv_box(0.5)
        lod1.merge(g)
    M.prelight(lod1, mode="object")

    mb.lod(60.0)
    mb.add(lod0)
    mb.lod()
    mb.add(lod1)
    mb.tag("door", (-1.5, 0.0, -D * 0.5 - 0.08), (0.0, 0.0, -1.0))
    return mb


def build_tree(at):
    mb = M.MeshBuilder("sample_tree")
    leaf = mb.material("pine", tex="sample_atlas", blend=M.RB_ALPHATEST, cull_none=True)
    g = M.crossed_billboard(3.0, 6.0, planes=3, mat=leaf, uv=at.uv("tree"))
    M.prelight(g, mode="object", hemi=0.5)
    mb.lod()
    mb.add(g)
    return mb


# ---------------------------------------------------------------------------------------------------- build

def build(out_dir, kit):
    at = make_atlas()
    kit.save_bmp("tex/sample_wall.bmp", tex_wall())
    kit.save_bmp("tex/sample_roof.bmp", tex_roof())
    kit.save_bmp("tex/sample_atlas.bmp", at.image(), keyed=True)
    house = build_house(at)
    tree = build_tree(at)
    kit.save_msh("meshes/sample_house.msh", house)
    kit.save_msh("meshes/sample_tree.msh", tree)
    if kit.previews:
        td = kit.tex_dirs
        kit.save_preview("sample_house_views.png", V.views(house, 256, tex_dirs=td, title="sample_house"))
        kit.save_preview("sample_house_turn.png", V.turntable(house, 8, 192, tex_dirs=td))
        kit.save_preview("sample_house_lods.png", V.lods(house, 256, tex_dirs=td))
        kit.save_preview("sample_tree_views.png", V.views(tree, 192, tex_dirs=td, title="sample_tree"))
        road = M.strip_along([(0.0, 0.0, -20.0), (0.0, 0.0, 30.0), (12.0, 0.0, 80.0)],
                             [(-4.0, 0.02), (4.0, 0.02)], v_scale=0.1).set_color(P.shade(P.C["asphalt"], 3.0))
        road.TM = [None] * road.ntris
        items = [(road, (0, 0, 0), 0.0), (house, (10.0, 0.0, 22.0), -20.0)]
        r = Rng(SEED, "scene")
        for i in range(14):
            side = -1 if i % 2 else 1
            items.append((tree, (side * r.range(6.0, 22.0), 0.0, r.range(0.0, 70.0)), r.range(0, 360),
                          r.range(0.8, 1.3)))
        kit.save_preview("sample_scene.png", V.scene(items, eye=(0.0, 1.6, -12.0), target=(2.0, 1.5, 20.0),
                                                     size=(480, 320), ground=(P.C["grass"], 200.0), tex_dirs=td))
        kit.save_preview("sample_tex.png", V.contact_sheet([os.path.join(out_dir, "tex", n + ".bmp")
                                                            for n in ("sample_wall", "sample_roof",
                                                                      "sample_atlas")], cols=3, mips=True))


if __name__ == "__main__":
    from assetkit import build as B
    sys.exit(B.main_for(__file__))
