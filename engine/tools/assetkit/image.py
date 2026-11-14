"""Textures: canvas helpers on Pillow images, BMP writer/reader, key threshold, alpha pairing, atlas packer,
mip and 16-bit previews, tileable noise.

Engine texture rules (DESIGN 3.3): power-of-two sides, at most 256 x 256, aspect <= 8:1. Files are 24-bit
uncompressed bottom-up BMPs (BI_RGB). Keyed textures use EXACT magenta (255, 0, 255) for transparent texels;
smooth alpha goes into a paired 8-bit greyscale <name>_a.bmp.

    from assetkit import image as I
    img = I.new_rgba(128, 256)                       # draw with Pillow (ImageDraw) or I.supersample(...)
    I.save_bmp("data/tex/tree_pine.bmp", img, keyed=True)   # threshold + bleed -> exact magenta key
    I.save_bmp("data/tex/smoke.bmp", img, alpha=True)       # smoke.bmp + smoke_a.bmp
    I.save_bmp("data/tex/props.bmp", img, bg=(128, 128, 128))   # opaque: RGBA flattened over grey, no key
    rgb, info = I.read_bmp("data/tex/tree_pine.bmp")
"""

import math
import os
import struct

from PIL import Image, ImageDraw

from . import palette
from . import rng as _rng

KEY = palette.KEY
MAX_SIZE = 256
MAX_ASPECT = 8


# ---------------------------------------------------------------------------------------------------- basics

def is_pow2(n):
    return n > 0 and (n & (n - 1)) == 0


def size_problems(w, h, max_size=MAX_SIZE, max_aspect=MAX_ASPECT):
    """List of rule violations for a texture size (empty = ok)."""
    p = []
    if not is_pow2(w) or not is_pow2(h):
        p.append("size %dx%d is not a power of two" % (w, h))
    if w > max_size or h > max_size:
        p.append("size %dx%d exceeds %d" % (w, h, max_size))
    if max(w, h) > max_aspect * min(w, h):
        p.append("aspect %dx%d exceeds %d:1" % (w, h, max_aspect))
    return p


def new(w, h, color=(0, 0, 0)):
    """New RGB image."""
    return Image.new("RGB", (w, h), tuple(color[:3]))


def new_rgba(w, h, color=(0, 0, 0, 0)):
    """New RGBA image (transparent by default) for keyed/alpha art."""
    c = tuple(color) if len(color) == 4 else (color[0], color[1], color[2], 255)
    return Image.new("RGBA", (w, h), c)


def supersample(w, h, draw_fn, factor=4, mode="RGBA", bg=(0, 0, 0, 0)):
    """Anti-aliased drawing: draw_fn(draw, img, s) paints on a (w*factor x h*factor) canvas where s = factor
    (multiply your coordinates by s); the result is box-filtered down to w x h (Pillow premultiplies alpha,
    so edge colours stay clean). Use for shapes whose edges are later thresholded by save_bmp(keyed=True)."""
    big = Image.new(mode, (w * factor, h * factor), bg if mode == "RGBA" else bg[:3])
    d = ImageDraw.Draw(big)
    draw_fn(d, big, factor)
    return big.resize((w, h), Image.BOX)


def gradient(w, h, top, bottom, mode="RGB"):
    """Vertical linear gradient image from colour top (row 0) to bottom."""
    img = Image.new(mode, (w, h))
    px = img.load()
    for y in range(h):
        t = y / float(max(1, h - 1))
        c = palette.lerp(top, bottom, t)
        if mode == "RGBA":
            c = c + (255,)
        for x in range(w):
            px[x, y] = c
    return img


def multiply(img, color):
    """Image multiplied by a colour (alpha kept)."""
    bands = img.split()
    out = [b.point(lambda v, f=color[i] / 255.0: palette.clamp8(v * f)) for i, b in enumerate(bands[:3])]
    if img.mode == "RGBA":
        out.append(bands[3])
    return Image.merge(img.mode, out)


def apply_noise(img, grid, amount=0.15, center=0.5):
    """Brightness modulation by a noise grid (rows of [0,1] floats, e.g. rng.noise_grid): each pixel
    *= 1 + (n - center) * 2 * amount. Grid is sampled with wrap if smaller than the image."""
    w, h = img.size
    gh = len(grid)
    gw = len(grid[0])
    px = img.load()
    for y in range(h):
        row = grid[y * gh // h]
        for x in range(w):
            f = 1.0 + (row[x * gw // w] - center) * 2.0 * amount
            p = px[x, y]
            q = (palette.clamp8(p[0] * f), palette.clamp8(p[1] * f), palette.clamp8(p[2] * f))
            px[x, y] = q + tuple(p[3:])
    return img


def noise_image(w, h, lo, hi, cells=8, octaves=4, seed=0, tile=True, kind="fbm"):
    """RGB image mixing colours lo..hi by tileable noise (rng.noise_grid)."""
    g = _rng.noise_grid(w, h, cells, octaves, 0.5, seed, tile, kind)
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        row = g[y]
        for x in range(w):
            px[x, y] = palette.lerp(lo, hi, row[x])
    return img


def wrap_offset(img, dx=None, dy=None):
    """Shift with wrap-around (default half size): seams of a tiling texture show up in the middle."""
    w, h = img.size
    dx = w // 2 if dx is None else dx % w
    dy = h // 2 if dy is None else dy % h
    out = img.copy()
    out.paste(img.crop((w - dx, 0, w, h)), (0, 0))
    out.paste(img.crop((0, 0, w - dx, h)), (dx, 0))
    tmp = out.copy()
    out.paste(tmp.crop((0, h - dy, w, h)), (0, 0))
    out.paste(tmp.crop((0, 0, w, h - dy)), (0, dy))
    return out


def make_tileable(img, border=None):
    """Make any image tile seamlessly: cross-fade it with its half-offset copy near the edges
    (border = blend width in pixels, default 1/4 of the size)."""
    w, h = img.size
    b = border or max(2, min(w, h) // 4)
    off = wrap_offset(img)
    out = img.copy()
    po = out.load()
    pa = img.load()
    pb = off.load()
    for y in range(h):
        ty = min(y, h - 1 - y)
        for x in range(w):
            tx = min(x, w - 1 - x)
            t = min(tx, ty) / float(b)
            if t >= 1.0:
                continue
            t = t * t * (3 - 2 * t)
            a = pa[x, y]
            c = pb[x, y]
            po[x, y] = tuple(palette.clamp8(c[i] + (a[i] - c[i]) * t) for i in range(len(a)))
    return out


def seam_error(img):
    """Mean absolute RGB difference between opposite edges (0 = perfectly tileable). Diagnostic only."""
    w, h = img.size
    px = img.convert("RGB").load()
    s = 0
    n = 0
    for y in range(h):
        a = px[0, y]
        b = px[w - 1, y]
        s += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
        n += 3
    for x in range(w):
        a = px[x, 0]
        b = px[x, h - 1]
        s += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
        n += 3
    return s / float(n)


# ---------------------------------------------------------------------------------------------------- key

def bleed(rgba, passes=4):
    """Fill the RGB of non-solid pixels (alpha < 255) from solid neighbours, ring by ring (alpha unchanged).
    Removes dark/magenta fringes that come from drawing over a background, and gives mip filters clean
    colours to blend into."""
    img = rgba.copy()
    w, h = img.size
    px = img.load()
    solid = [[px[x, y][3] == 255 for x in range(w)] for y in range(h)]
    if not any(any(r) for r in solid):
        solid = [[px[x, y][3] >= 128 for x in range(w)] for y in range(h)]
    for _ in range(passes):
        changes = []
        for y in range(h):
            for x in range(w):
                if solid[y][x]:
                    continue
                r = g = b = n = 0
                for dy in (-1, 0, 1):
                    yy = y + dy
                    if yy < 0 or yy >= h:
                        continue
                    for dx in (-1, 0, 1):
                        xx = x + dx
                        if (dx or dy) and 0 <= xx < w and solid[yy][xx]:
                            p = px[xx, yy]
                            r += p[0]
                            g += p[1]
                            b += p[2]
                            n += 1
                if n:
                    changes.append((x, y, (r // n, g // n, b // n)))
        if not changes:
            break
        for x, y, c in changes:
            px[x, y] = c + (px[x, y][3],)
            solid[y][x] = True
    return img


def key_threshold(rgba, bleed_passes=2, threshold=128):
    """RGBA -> RGB with the exact magenta key: alpha < threshold -> (255, 0, 255), else opaque. Edge colours
    are bled from the solid interior first (no magenta-tinted or dark fringes), and opaque pixels that are
    exactly the key colour are nudged to (254, 0, 254)."""
    src = bleed(rgba.convert("RGBA"), bleed_passes) if bleed_passes else rgba.convert("RGBA")
    w, h = src.size
    out = Image.new("RGB", (w, h))
    ps = src.load()
    po = out.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = ps[x, y]
            if a < threshold:
                po[x, y] = KEY
            elif (r, g, b) == KEY:
                po[x, y] = (254, 0, 254)
            else:
                po[x, y] = (r, g, b)
    return out


def keyed_to_rgba(rgb):
    """Magenta-keyed RGB -> RGBA (key = alpha 0)."""
    img = rgb.convert("RGBA")
    px = img.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            p = px[x, y]
            if p[0] == 255 and p[1] == 0 and p[2] == 255:
                px[x, y] = (255, 0, 255, 0)
    return img


# ---------------------------------------------------------------------------------------------------- BMP I/O

def bmp_bytes(img):
    """RGB image -> 24-bit bottom-up BI_RGB BMP bytes."""
    img = img.convert("RGB")
    w, h = img.size
    raw = img.tobytes()
    rowsize = (w * 3 + 3) & ~3
    pad = b"\0" * (rowsize - w * 3)
    out = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(raw[y * w * 3:(y + 1) * w * 3])
        row[0::3], row[2::3] = row[2::3], row[0::3]
        out += row + pad
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(out), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(out), 2835, 2835, 0, 0)
    return hdr + info + bytes(out)


def bmp8_bytes(grey):
    """'L' image -> 8-bit bottom-up BI_RGB BMP with a 256-entry greyscale palette (alpha files)."""
    grey = grey.convert("L")
    w, h = grey.size
    raw = grey.tobytes()
    rowsize = (w + 3) & ~3
    pad = b"\0" * (rowsize - w)
    out = bytearray()
    for y in range(h - 1, -1, -1):
        out += raw[y * w:(y + 1) * w] + pad
    pal = b"".join(struct.pack("<4B", i, i, i, 0) for i in range(256))
    off = 54 + len(pal)
    hdr = struct.pack("<2sIHHI", b"BM", off + len(out), 0, 0, off)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 8, 0, len(out), 2835, 2835, 256, 256)
    return hdr + info + pal + bytes(out)


def alpha_path(path):
    """data/tex/smoke.bmp -> data/tex/smoke_a.bmp"""
    root, ext = os.path.splitext(path)
    return root + "_a" + ext


def flatten(rgba, bg):
    """RGBA composited over an opaque colour -> RGB (opaque atlases: nothing keyed, gutters/empty areas = bg)."""
    base = Image.new("RGBA", rgba.size, (bg[0], bg[1], bg[2], 255))
    base.alpha_composite(rgba.convert("RGBA"))
    return base.convert("RGB")


def _check_size(path, img, check, max_size):
    if check:
        probs = size_problems(img.size[0], img.size[1], max_size)
        if probs:
            raise ValueError("%s: %s" % (path, "; ".join(probs)))
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d)


def save_bmp(path, img, keyed=None, alpha=False, check=True, bleed_passes=2, max_size=MAX_SIZE, alpha_bleed=8,
             bg=None):
    """Write a texture. RGB images are written as is. RGBA images: keyed=True (default for RGBA) thresholds
    to the exact magenta key (key_threshold); alpha=True writes the colour (bled under transparent texels,
    alpha_bleed passes; alpha_bleed=0 keeps the RGB exactly as drawn: premultiplied/additive textures such as
    a glow, whose colour must fade to black with the alpha) plus <name>_a.bmp (8-bit grey alpha).
    bg=(r, g, b): opaque texture, RGBA composited over bg (flatten), no key, no alpha file.
    check=True raises ValueError on size-rule violations (pass check=False for non-texture images, max_size
    for e.g. 2048-wide panorama sources). Returns the list of written paths."""
    _check_size(path, img, check, max_size)
    written = [path]
    if img.mode == "RGBA" and bg is not None:
        with open(path, "wb") as f:
            f.write(bmp_bytes(flatten(img, bg)))
        return written
    if img.mode == "RGBA" and alpha:
        rgb = (bleed(img, alpha_bleed) if alpha_bleed else img).convert("RGB")
        with open(path, "wb") as f:
            f.write(bmp_bytes(rgb))
        ap = alpha_path(path)
        with open(ap, "wb") as f:
            f.write(bmp8_bytes(img.split()[3]))
        written.append(ap)
        return written
    if img.mode == "RGBA" and keyed is not False:
        rgb = key_threshold(img, bleed_passes)
    else:
        rgb = img.convert("RGB")
    with open(path, "wb") as f:
        f.write(bmp_bytes(rgb))
    return written


def save_bmp8(path, img, check=True, max_size=MAX_SIZE):
    """Write an 8-bit greyscale BMP (bmp8_bytes), e.g. a hand-made <name>_a.bmp. img 'L' is written as is;
    'RGBA'/'LA' write their ALPHA band; anything else is converted to 'L'. Size rules as save_bmp.
    Returns the path."""
    _check_size(path, img, check, max_size)
    if img.mode in ("RGBA", "LA"):
        img = img.split()[-1]
    with open(path, "wb") as f:
        f.write(bmp8_bytes(img))
    return path


def bmp_info(data):
    """Parse a BMP header from bytes -> dict (sig, size, off, isz, w, h, planes, bpp, comp, clr, flen)."""
    if len(data) < 54:
        raise ValueError("file too short for a BMP header")
    sig, size, _, _, off = struct.unpack_from("<2sIHHI", data, 0)
    isz, w, h, planes, bpp, comp, isize, _, _, clr, _ = struct.unpack_from("<IiiHHIIiiII", data, 14)
    return {"sig": sig, "size": size, "off": off, "isz": isz, "w": w, "h": h, "planes": planes, "bpp": bpp,
            "comp": comp, "clr": clr, "flen": len(data)}


def read_bmp(src):
    """Path or bytes of a 24-bit or 8-bit uncompressed BMP -> (PIL image, info). 24-bit -> 'RGB',
    8-bit -> 'L' when the palette is grey, else 'RGB'. Own parser (no Pillow BMP quirks)."""
    if isinstance(src, (bytes, bytearray)):
        data = bytes(src)
    else:
        with open(src, "rb") as f:
            data = f.read()
    info = bmp_info(data)
    if info["sig"] != b"BM" or info["comp"] != 0 or info["bpp"] not in (8, 24):
        raise ValueError("unsupported BMP (sig %r, bpp %d, compression %d)" % (info["sig"], info["bpp"],
                                                                                 info["comp"]))
    w, h = info["w"], abs(info["h"])
    bottom_up = info["h"] > 0
    off = info["off"]
    if info["bpp"] == 24:
        rowsize = (w * 3 + 3) & ~3
        if off + rowsize * h > len(data):
            raise ValueError("BMP pixel data truncated")
        out = bytearray()
        for y in range(h):
            sy = h - 1 - y if bottom_up else y
            row = bytearray(data[off + sy * rowsize: off + sy * rowsize + w * 3])
            row[0::3], row[2::3] = row[2::3], row[0::3]
            out += row
        return Image.frombytes("RGB", (w, h), bytes(out)), info
    ncol = info["clr"] or 256
    pal_off = 14 + info["isz"]
    pal = [struct.unpack_from("<4B", data, pal_off + 4 * i) for i in range(ncol)]
    rowsize = (w + 3) & ~3
    if off + rowsize * h > len(data):
        raise ValueError("BMP pixel data truncated")
    rows = bytearray()
    for y in range(h):
        sy = h - 1 - y if bottom_up else y
        rows += data[off + sy * rowsize: off + sy * rowsize + w]
    grey = all(p[0] == p[1] == p[2] for p in pal)
    info["grey"] = grey
    if grey:
        lut = [pal[i][0] if i < ncol else 0 for i in range(256)]
        return Image.frombytes("L", (w, h), bytes(rows)).point(lut), info
    rgb = bytearray()
    for i in rows:
        b, g, r, _ = pal[i] if i < ncol else (0, 0, 0, 0)
        rgb += bytes((r, g, b))
    return Image.frombytes("RGB", (w, h), bytes(rgb)), info


def load_texture(path):
    """Texture as the engine sees it -> RGBA: <name>_a.bmp alpha if present, else magenta key = alpha 0."""
    rgb, _ = read_bmp(path)
    ap = alpha_path(path)
    if os.path.exists(ap):
        a, _ = read_bmp(ap)
        img = rgb.convert("RGBA")
        img.putalpha(a.convert("L"))
        return img
    return keyed_to_rgba(rgb)


# ---------------------------------------------------------------------------------------------------- atlas

def pack_rects(sizes, atlas_w, atlas_h, gutter=2):
    """Shelf packer. sizes = [(w, h), ...] -> [(x, y), ...] positions of the CONTENT (each rect gets
    `gutter` free pixels on every side, edges included). Deterministic: tallest first, ties by input order.
    Raises ValueError if it doesn't fit."""
    order = sorted(range(len(sizes)), key=lambda i: (-sizes[i][1], -sizes[i][0], i))
    pos = [None] * len(sizes)
    x = 0
    y = 0
    shelf_h = 0
    for i in order:
        w, h = sizes[i]
        bw, bh = w + 2 * gutter, h + 2 * gutter
        if bw > atlas_w or bh > atlas_h:
            raise ValueError("rect %d (%dx%d) larger than the atlas %dx%d" % (i, w, h, atlas_w, atlas_h))
        if x + bw > atlas_w:
            y += shelf_h
            x = 0
            shelf_h = 0
        if y + bh > atlas_h:
            raise ValueError("atlas %dx%d full (rect %d, %dx%d)" % (atlas_w, atlas_h, i, w, h))
        pos[i] = (x + gutter, y + gutter)
        x += bw
        if bh > shelf_h:
            shelf_h = bh
    return pos


class Atlas(object):
    """Pack named images into one texture with gutters; gutters are filled by repeating the edge texels so
    bilinear filtering and mips don't pull in neighbours (keyed images keep key gutters).

        at = Atlas(256, 256, gutter=2)
        at.add("sign_moose", img1); at.add("sign_curve", img2)
        sheet = at.image()                    # RGBA, packs on first use
        at.rect("sign_moose") -> (x, y, w, h);  at.uv("sign_moose") -> (u0, v0, u1, v1) for Geo.uv_remap
        I.save_bmp(path, sheet, keyed=True);  at.write_header("hud_rects.h", "HUD")
    """

    def __init__(self, w=256, h=256, gutter=2, bg=(0, 0, 0, 0)):
        self.w = w
        self.h = h
        self.gutter = gutter
        self.bg = bg
        self.names = []
        self.images = {}
        self.rects = None

    def add(self, name, img):
        if name in self.images:
            raise ValueError("duplicate atlas entry %r" % name)
        self.names.append(name)
        self.images[name] = img.convert("RGBA")
        self.rects = None
        return self

    def pack(self):
        sizes = [self.images[n].size for n in self.names]
        pos = pack_rects(sizes, self.w, self.h, self.gutter)
        self.rects = {}
        for n, (x, y), (w, h) in zip(self.names, pos, sizes):
            self.rects[n] = (x, y, w, h)
        return self.rects

    def rect(self, name):
        if self.rects is None:
            self.pack()
        return self.rects[name]

    def uv(self, name, inset=0.5):
        """Normalised (u0, v0, u1, v1) of an entry, inset by `inset` texels (bilinear-safe)."""
        x, y, w, h = self.rect(name)
        return ((x + inset) / float(self.w), (y + inset) / float(self.h),
                (x + w - inset) / float(self.w), (y + h - inset) / float(self.h))

    def image(self, bg=None):
        """Composite RGBA atlas with edge-extended gutters. bg=(r, g, b): opaque atlas instead, the RGBA
        composited over bg -> RGB (no key; empty areas and transparent texels show bg; save it as is)."""
        if bg is not None:
            return flatten(self.image(), bg)
        if self.rects is None:
            self.pack()
        out = Image.new("RGBA", (self.w, self.h), self.bg)
        g = self.gutter
        for n in self.names:
            x, y, w, h = self.rects[n]
            im = self.images[n]
            if g:
                ext = im.resize((w + 2 * g, h + 2 * g), Image.NEAREST)
                ext.paste(im, (g, g))
                pe = ext.load()
                pi = im.load()
                for yy in range(h + 2 * g):
                    sy = min(max(yy - g, 0), h - 1)
                    for xx in range(w + 2 * g):
                        if g <= xx < w + g and g <= yy < h + g:
                            continue
                        sx = min(max(xx - g, 0), w - 1)
                        pe[xx, yy] = pi[sx, sy]
                out.paste(ext, (x - g, y - g))
            else:
                out.paste(im, (x, y))
        return out

    def write_header(self, path, prefix):
        """C header with the rectangles (#define PREFIX_NAME_X/Y/W/H), CRLF line endings."""
        if self.rects is None:
            self.pack()
        lines = ["/* generated by assetkit.image.Atlas - do not edit */",
                 "#define %s_TEX_W %d" % (prefix, self.w), "#define %s_TEX_H %d" % (prefix, self.h)]
        for n in self.names:
            x, y, w, h = self.rects[n]
            k = (prefix + "_" + n).upper()
            lines.append("#define %s_X %d\r\n#define %s_Y %d\r\n#define %s_W %d\r\n#define %s_H %d" %
                         (k, x, k, y, k, w, k, h))
        with open(path, "wb") as f:
            f.write(("\r\n".join(lines) + "\r\n").encode("ascii"))
        return path


# ---------------------------------------------------------------------------------------------------- previews

def mip_chain(img, keyed=None, min_size=1):
    """Mip levels like the engine's img_gen_mips: alpha-weighted 2x2 box filter (keyed texels never bleed
    magenta). img RGB (magenta = key when keyed is not False) or RGBA. Returns a list of RGBA images."""
    if img.mode == "RGB" and keyed is not False:
        cur = keyed_to_rgba(img)
    else:
        cur = img.convert("RGBA")
    out = [cur]
    while min(cur.size) > min_size:
        w, h = cur.size
        nw, nh = max(1, w // 2), max(1, h // 2)
        nxt = Image.new("RGBA", (nw, nh))
        ps = cur.load()
        pn = nxt.load()
        for y in range(nh):
            for x in range(nw):
                r = g = b = a = 0
                for dy in (0, 1):
                    for dx in (0, 1):
                        p = ps[min(2 * x + dx, w - 1), min(2 * y + dy, h - 1)]
                        r += p[0] * p[3]
                        g += p[1] * p[3]
                        b += p[2] * p[3]
                        a += p[3]
                if a:
                    pn[x, y] = (r // a, g // a, b // a, (a + 2) // 4)
                else:
                    pn[x, y] = (0, 0, 0, 0)
        out.append(nxt)
        cur = nxt
    return out


def checker(w, h, cell=8, a=(150, 150, 150), b=(110, 110, 110)):
    """Checkerboard RGB image (transparency backdrop)."""
    img = Image.new("RGB", (w, h), a)
    d = ImageDraw.Draw(img)
    for y in range(0, h, cell):
        for x in range(0, w, cell):
            if ((x // cell) + (y // cell)) & 1:
                d.rectangle([x, y, x + cell - 1, y + cell - 1], fill=b)
    return img


def over_checker(rgba, cell=8):
    """RGBA composited over a checkerboard -> RGB (to look at transparency)."""
    bg = checker(rgba.size[0], rgba.size[1], cell)
    bg.paste(rgba, (0, 0), rgba)
    return bg


def mip_sheet(img, keyed=None):
    """All mip levels side by side over a checkerboard (RGB) - shows key/alpha behaviour when minified."""
    levels = mip_chain(img, keyed)
    w = sum(l.size[0] for l in levels) + 4 * len(levels)
    h = levels[0].size[1]
    out = Image.new("RGB", (w, h), (60, 60, 60))
    x = 0
    for l in levels:
        out.paste(over_checker(l, 4), (x, 0))
        x += l.size[0] + 4
    return out


_BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def to16(img, fmt="565", dither=False):
    """Preview of the texture after 16-bit conversion (565 opaque, 555/1555 keyed, 4444 alpha): banding
    shows here before it shows on the VM. dither=True adds 4x4 ordered dither (the device dithers too)."""
    bits = {"565": (5, 6, 5), "555": (5, 5, 5), "1555": (5, 5, 5), "4444": (4, 4, 4)}[fmt]
    rgba = img.mode == "RGBA"
    src = img if rgba else img.convert("RGB")
    out = src.copy()
    px = out.load()
    w, h = out.size
    for y in range(h):
        for x in range(w):
            p = px[x, y]
            q = []
            for i in range(3):
                bmax = (1 << bits[i]) - 1
                v = p[i] / 255.0 * bmax
                if dither:
                    v += (_BAYER4[y & 3][x & 3] + 0.5) / 16.0 - 0.5
                v = int(v + 0.5)
                v = 0 if v < 0 else (bmax if v > bmax else v)
                q.append((v * 255 + bmax // 2) // bmax)
            if rgba:
                a = p[3]
                if fmt == "1555":
                    a = 255 if a >= 128 else 0
                elif fmt == "4444":
                    a = ((a * 15 + 127) // 255) * 17
                q.append(a)
            px[x, y] = tuple(q)
    return out


def tex_kb(w, h, mips=True, bpp=16):
    """Video memory estimate of a texture in KB (16-bit, with mips = x4/3)."""
    b = w * h * bpp / 8.0
    if mips:
        b *= 4.0 / 3.0
    return b / 1024.0
