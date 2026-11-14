"""isl.py - writer, reader and checker of data/island/jet.isl (DESIGN 8.6). The engine reader is e_terrain.cpp (T04).

FORMAT (all little-endian, every chunk padded to 4 bytes, version 1)
Container: the e_file chunk container (same as .trk, written by assetkit.export.trk_bytes) with the 4-byte tag
"RISL" instead of "RTRK": char tag[4] "RISL"; u32 version 1; u32 numChunks; u32 crc32 (zlib/IEEE) of every byte
after the 16-byte header. Chunks: char id[4]; u32 size (unpadded); data; zero pad to 4. Engine:
chunk_open(&cf, data, size, E_TAG('R','I','S','L')). Chunk order: INFO HGHT HMAX CLAS MATL CHNK BTCH VERT INDX BASE MAPI.

INFO  84 B
      0   u32  version (1)
      4   u32  gridN (513)                    samples per side (64 k + 1, <= 513: e_terrain superMax limit)
      8   f32  cell (T03 32, T03b 80)         metres between samples (island.txt `grid`)
      12  f32  x0, z0 (-(gridN-1)*cell/2)     world position of sample (0, 0) (south-west corner; T03b -20480)
      20  f32  hMin (-200), hScale (0.05)     height = hMin + u16 * hScale
      28  u32  chunkCells (16)                quads per chunk side at LOD0 (chunk = 16 cells: 512 m / T03b 1280 m)
      32  u32  nChunkX, nChunkZ (32, 32)      chunk grid (chunk (cx, cz) covers samples cx*16 .. cx*16+16)
      40  u32  nLods (5)
      44  f32  seaLevel (0)
      48  f32  lodEdge[5]                     grid edge of each LOD in metres: cell << k (T03b 80 160 320 640 1280)
      68  u32  nMaterials                     MATL record count
      72  f32  islandCentre x, z, radius      land bounding circle (minimap, area logic)
HGHT  u16 h[gridN * gridN]; index = z * gridN + x (row = z, south to north; column = x, west to east).
      Heights are the LOD0 render heights rounded to 0.05 m (the mesh may differ by <= 0.025 m on chunk borders).
      Coast rule: every sample below sea level is <= -coast_depth (4 m), every land sample >= beach_min (1 m),
      except a few chunk-border samples changed by the LOD0 snap (counted in report.txt). Floor = -60 m.
HMAX  u16 [nb * nb], nb = (gridN-1)/8 (64): max height code of each 8 x 8-cell block, index = bz * nb + bx; covers
      bx*8 .. bx*8+8 (inclusive, so the block max bounds the whole surface over those cells).
CLAS  u8 [(gridN-1)^2]: class of each LOD0 quad (cells), index = qz * (gridN-1) + qx. CLASS_* below.
MATL  40 B per material: char tex[32] (data\\tex\\<name>.bmp, no extension); u32 flags; u32 tint ARGB.
      flags: bits 0-7 = blend RB_* (0 opaque, 1 alphatest, 2 blend, 3 add: the e_rend/assetkit values);
      0x100 MF_CLAMPU, 0x200 MF_CLAMPV (clamp addressing on that axis), 0x400 MF_ZBIAS1, 0x800 MF_NOCULL.
      tint = the class tint (A3 convention) for minimap/debug ONLY: vertex colours already contain it, the
      renderer must NOT multiply it again.
CHNK  108 B per stored chunk, sorted by (cz, cx):
      0   u16  cx, cz
      4   f32  aabb[6]                        lo xyz, hi xyz over all LODs (terrain, skirts, foam)
      28  per LOD k = 0..4 (16 B each): u32 vFirst, vCount (into VERT), bFirst, bCount (into BTCH)
      LOD records may ALIAS: all-underwater chunks store only LOD2-4; their LOD0 and LOD1 records repeat the
      LOD2 record (same vFirst/bFirst). A reader must not assume disjoint ranges.
      Chunks not stored are deeper than -59.5 m everywhere (open sea: draw nothing, the opaque sea covers them).
BTCH  12 B per batch: u16 material, u16 skirtTris; u32 iFirst (into INDX), iCount (multiple of 3).
      Batches of one chunk-LOD are consecutive and sorted by material; the indices are RELATIVE to that
      chunk-LOD's vFirst (< vCount <= 65535). The LAST skirtTris triangles of a batch are skirt triangles
      (the DESIGN 8.6 'pad' field, 0 on LOD0 and foam): draw iCount - 3 * skirtTris indices when every
      neighbour chunk is at the same LOD, all iCount when a neighbour is at another LOD (saves 25-80 % of
      the LOD 1-4 triangles).
VERT  24 B per vertex = RVert1: f32 x, y, z; u32 colour ARGB (alpha 255; baked light x class colour, final);
      f32 u, v. Terrain UV = chunk-local planar: u = (x - chunkX0) / period, v = (z - chunkZ0) / period
      (chunkX0 = x0 + cx * chunk), period = the material's texture period (T03 32/64 m: u, v <= 16; T03b all
      64 m on 1280 m chunks: u, v <= 20, within T02's 16-32 rule) and
      tiling is seamless across chunks. Skirt vertices repeat the top vertex's colour and UV. Foam: u along the
      coast (1 per 64 m, kept < 9 by integer resets), v = 0 landward edge (6 m inland), 0.15 at the 0 m contour,
      1 seaward edge (34 m out).
INDX  u16 indices (clockwise = front; terrain and skirts are culled back faces; foam is MF_NOCULL).
BASE  4 + 104 + 24 * nBoxes B:
      0   f32  runway centre x, z, heading deg (takeoff direction), length, width, height (terrain under the runway)
      24  f32  threshold spawn x, y, z, heading deg   (y = runway surface = ribbon top)
      40  f32  air spawn[4] x, y, z, heading deg      (heading toward the island centre)
      104 u32  nBoxes (<= 32), then Aabb[nBoxes] (lo xyz, hi xyz): hangars, tower, tanks, radome (collision)
MAPI  48 B: char tex[32] minimap texture name; f32 x0, z0, x1, z1 world rect of the image
      (u = (x - x0) / (x1 - x0), v = (z1 - z) / (z1 - z0): image top = north).

Mesh topology: LOD k of a chunk is a regular (16 >> k)^2 quad grid of spacing cell << k m on the LOD0 samples
(point sampled, no filtering); each quad = triangles (SW, NW, NE) and (SW, NE, SE) (diagonal SW-NE), split into
per-class batches (class from the class pyramid, classes.py). LOD0 has no skirts (its border odd samples are
snapped to the LOD1 edge in the height grid itself); LOD 1-4 have outward-facing skirts on all 4 edges whose depth
covers the measured mismatch to every other LOD along that edge. Foam strips on LODs 0-2 follow that LOD's own
0 m contour (marching triangles over its triangles).
"""
import struct

from assetkit import export as E

VERSION = 1
TAG = b"RISL"
CLASS_NAMES = ["water", "sand", "grass", "rock", "ash", "lava", "runway", "apron"]
MF_CLAMPU, MF_CLAMPV, MF_ZBIAS1, MF_NOCULL = 0x100, 0x200, 0x400, 0x800

_INFO = struct.Struct("<2I5f4If5fI3f")
_MATL = struct.Struct("<32sII")
_CHNK = struct.Struct("<2H6f20I")
_BTCH = struct.Struct("<2H2I")
_VERT = struct.Struct("<3fI2f")
_BASE = struct.Struct("<6f4f16fI")
_AABB = struct.Struct("<6f")
_MAPI = struct.Struct("<32s4f")
assert _INFO.size == 84 and _MATL.size == 40 and _CHNK.size == 108 and _BTCH.size == 12 and _VERT.size == 24
assert _BASE.size == 108 and _MAPI.size == 48
ORDER = ["INFO", "HGHT", "HMAX", "CLAS", "MATL", "CHNK", "BTCH", "VERT", "INDX", "BASE", "MAPI"]


class Isl(object):
    """Decoded jet.isl (also what encode() builds from): info dict, hght, hmax, clas, mats, chunks, batches,
    verts, idx, base dict, mapi dict, order (chunk ids in file order), problems."""
    pass


def _u16(vals):
    return struct.pack("<%dH" % len(vals), *vals)


def container(chunks):
    data = E.trk_bytes(chunks, VERSION)
    return TAG + data[4:]


def pack(m):
    """Isl -> bytes."""
    inf = m.info
    out = []
    out.append(("INFO", _INFO.pack(inf["version"], inf["gridN"], inf["cell"], inf["x0"], inf["z0"], inf["hMin"],
                                   inf["hScale"], inf["chunkCells"], inf["nChunkX"], inf["nChunkZ"], inf["nLods"],
                                   inf["seaLevel"], *(list(inf["lodEdge"]) + [inf["nMaterials"]] +
                                                      list(inf["centre"])))))
    out.append(("HGHT", _u16(m.hght)))
    out.append(("HMAX", _u16(m.hmax)))
    out.append(("CLAS", bytes(m.clas)))
    out.append(("MATL", b"".join(_MATL.pack(E.pack_name(t, 32), f, c) for t, f, c in m.mats)))
    cb = []
    for c in m.chunks:
        vals = [c["cx"], c["cz"]] + list(c["aabb"])
        for r in c["lods"]:
            vals.extend(r)
        cb.append(_CHNK.pack(*vals))
    out.append(("CHNK", b"".join(cb)))
    out.append(("BTCH", b"".join(_BTCH.pack(b[0], b[1], b[2], b[3]) for b in m.batches)))
    out.append(("VERT", b"".join(_VERT.pack(*v) for v in m.verts)))
    out.append(("INDX", _u16(m.idx)))
    b = m.base
    bb = [_BASE.pack(*(list(b["rwy"]) + list(b["thr"]) + [v for s in b["air"] for v in s] + [len(b["boxes"])]))]
    bb += [_AABB.pack(*bx) for bx in b["boxes"]]
    out.append(("BASE", b"".join(bb)))
    out.append(("MAPI", _MAPI.pack(E.pack_name(m.mapi["tex"], 32), *m.mapi["rect"])))
    return container(out)


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def encode(hf, cl, ch, bs, mp):
    """Build the Isl from the pipeline results and pack it."""
    m = Isl()
    m.info = {"version": VERSION, "gridN": hf.N, "cell": hf.cell, "x0": hf.x0, "z0": hf.z0, "hMin": hf.hmin,
              "hScale": hf.hscale, "chunkCells": hf.CHUNK, "nChunkX": hf.NC, "nChunkZ": hf.NC, "nLods": 5,
              "seaLevel": 0.0, "lodEdge": [hf.cell * (1 << k) for k in range(5)],
              "nMaterials": len(ch.materials), "centre": hf.centre}
    m.hght = hf.Q
    m.hmax = hf.HMAX
    m.clas = cl.cls
    m.mats = [(mt[0], mt[1], mt[2]) for mt in ch.materials]
    m.chunks, m.batches, m.verts, m.idx = [], [], [], []
    for c in ch.chunks:
        recs = [None] * 5
        for k in range(5):
            if c.alias[k] is not None:
                continue
            verts, batches = c.lods[k]
            vf = len(m.verts)
            bf = len(m.batches)
            m.verts.extend(verts)
            for mat, idx, skirt in batches:
                m.batches.append((mat, skirt, len(m.idx), len(idx)))
                m.idx.extend(idx)
            recs[k] = (vf, len(verts), bf, len(batches))
        for k in range(5):
            if recs[k] is None:
                recs[k] = recs[c.alias[k]]
        m.chunks.append({"cx": c.cx, "cz": c.cz, "aabb": c.aabb, "lods": recs})
    m.base = {"rwy": bs.rwy, "thr": bs.thr, "air": bs.air, "boxes": [b for _, b in bs.boxes]}
    m.mapi = {"tex": mp.name, "rect": mp.rect}
    return pack(m)


def decode(data):
    """bytes -> Isl (fields as in encode; floats are the stored float32 values)."""
    if data[:4] != TAG:
        raise ValueError("not a RISL file")
    t = E.read_trk(b"RTRK" + data[4:])
    m = Isl()
    m.problems = list(t.problems)
    m.version = t.version
    m.order = [cid for cid, _ in t.chunks]
    g = dict((cid, d) for cid, d in t.chunks)
    v = _INFO.unpack(g["INFO"])
    m.info = {"version": v[0], "gridN": v[1], "cell": v[2], "x0": v[3], "z0": v[4], "hMin": v[5], "hScale": v[6],
              "chunkCells": v[7], "nChunkX": v[8], "nChunkZ": v[9], "nLods": v[10], "seaLevel": v[11],
              "lodEdge": list(v[12:17]), "nMaterials": v[17], "centre": tuple(v[18:21])}
    n = len(g["HGHT"]) // 2
    m.hght = list(struct.unpack("<%dH" % n, g["HGHT"]))
    m.hmax = list(struct.unpack("<%dH" % (len(g["HMAX"]) // 2), g["HMAX"]))
    m.clas = list(g["CLAS"])
    d = g["MATL"]
    m.mats = []
    for o in range(0, len(d), 40):
        tx, fl, ti = _MATL.unpack_from(d, o)
        m.mats.append((E.unpack_name(tx), fl, ti))
    d = g["CHNK"]
    m.chunks = []
    for o in range(0, len(d), 108):
        v = _CHNK.unpack_from(d, o)
        m.chunks.append({"cx": v[0], "cz": v[1], "aabb": tuple(v[2:8]),
                         "lods": [tuple(v[8 + 4 * k: 12 + 4 * k]) for k in range(5)]})
    d = g["BTCH"]
    m.batches = []
    for o in range(0, len(d), 12):
        mat, skirt, i0, ic = _BTCH.unpack_from(d, o)
        if 3 * skirt > ic:
            m.problems.append("BTCH skirtTris > tris at %d" % (o // 12))
        m.batches.append((mat, skirt, i0, ic))
    d = g["VERT"]
    m.verts = [_VERT.unpack_from(d, o) for o in range(0, len(d), 24)]
    d = g["INDX"]
    m.idx = list(struct.unpack("<%dH" % (len(d) // 2), d))
    d = g["BASE"]
    v = _BASE.unpack_from(d, 0)
    nb = v[26]
    m.base = {"rwy": tuple(v[0:6]), "thr": tuple(v[6:10]), "air": [tuple(v[10 + 4 * i: 14 + 4 * i]) for i in range(4)],
              "boxes": [_AABB.unpack_from(d, 108 + 24 * i) for i in range(nb)]}
    if len(d) != 108 + 24 * nb:
        m.problems.append("BASE size %d != %d" % (len(d), 108 + 24 * nb))
    tx, x0, z0, x1, z1 = _MAPI.unpack(g["MAPI"])
    m.mapi = {"tex": E.unpack_name(tx), "rect": (x0, z0, x1, z1)}
    return m


def reencode(m):
    """Decoded Isl -> bytes again (round-trip test: must equal the file)."""
    return pack(m)


def height_at(m, I, J):
    inf = m.info
    return inf["hMin"] + m.hght[J * inf["gridN"] + I] * inf["hScale"]


def check(m, data=None):
    """Structural validation of a decoded file -> [problem strings] (empty = OK)."""
    P = list(m.problems)
    inf = m.info
    N = inf["gridN"]
    if m.order != ORDER:
        P.append("chunk order %s" % m.order)
    if inf["version"] != VERSION:
        P.append("INFO version %d" % inf["version"])
    if len(m.hght) != N * N:
        P.append("HGHT count %d != %d" % (len(m.hght), N * N))
    nb = (N - 1) // 8
    if len(m.hmax) != nb * nb:
        P.append("HMAX count %d" % len(m.hmax))
    nq = (N - 1)
    if len(m.clas) != nq * nq:
        P.append("CLAS count %d" % len(m.clas))
    if any(c >= len(CLASS_NAMES) for c in m.clas):
        P.append("CLAS value out of range")
    if inf["nMaterials"] != len(m.mats):
        P.append("nMaterials %d != MATL %d" % (inf["nMaterials"], len(m.mats)))
    # HMAX bounds
    for bz in range(nb):
        for bx in range(nb):
            mx = m.hmax[bz * nb + bx]
            for j in range(bz * 8, bz * 8 + 9):
                for i in range(bx * 8, bx * 8 + 9):
                    if m.hght[j * N + i] > mx:
                        P.append("HMAX block %d,%d below a sample" % (bx, bz))
                        break
    nv, nb, ni = len(m.verts), len(m.batches), len(m.idx)
    umax = 16.0 * inf["cell"] / 64.0 + 1e-4        # chunk / smallest period (64 m; 32 m only with 32 m cells)
    if inf["cell"] <= 32.0:
        umax = 16.0001
    prev = None
    for c in m.chunks:
        key = (c["cz"], c["cx"])
        if prev is not None and key <= prev:
            P.append("CHNK not sorted at %d,%d" % (c["cx"], c["cz"]))
        prev = key
        if c["cx"] >= inf["nChunkX"] or c["cz"] >= inf["nChunkZ"]:
            P.append("chunk %d,%d outside the grid" % (c["cx"], c["cz"]))
        lo, hi = c["aabb"][:3], c["aabb"][3:]
        for k, (vf, vc, bf, bc) in enumerate(c["lods"]):
            if vf + vc > nv or bf + bc > nb or vc > 65535 or vc == 0 or bc == 0:
                P.append("chunk %d,%d LOD%d range bad (%d+%d/%d, %d+%d/%d)" % (c["cx"], c["cz"], k, vf, vc, nv,
                                                                             bf, bc, nb))
                continue
            lastmat = -1
            for b in m.batches[bf:bf + bc]:
                mat, skirt, i0, ic = b
                if mat >= len(m.mats) or mat < lastmat:
                    P.append("chunk %d,%d LOD%d batch material order/range" % (c["cx"], c["cz"], k))
                lastmat = mat
                if ic % 3 or i0 + ic > ni:
                    P.append("chunk %d,%d LOD%d batch index range" % (c["cx"], c["cz"], k))
                    continue
                for ix in m.idx[i0:i0 + ic]:
                    if ix >= vc:
                        P.append("chunk %d,%d LOD%d index %d >= vCount %d" % (c["cx"], c["cz"], k, ix, vc))
                        break
            for v in m.verts[vf:vf + vc]:
                x, y, z = v[0], v[1], v[2]
                if not (lo[0] - 1e-3 <= x <= hi[0] + 1e-3 and lo[1] - 1e-3 <= y <= hi[1] + 1e-3 and
                        lo[2] - 1e-3 <= z <= hi[2] + 1e-3):
                    P.append("chunk %d,%d LOD%d vertex outside the AABB" % (c["cx"], c["cz"], k))
                    break
                if (v[3] >> 24) != 255:
                    P.append("chunk %d,%d LOD%d vertex alpha != 255" % (c["cx"], c["cz"], k))
                    break
            # terrain vertices on the grid: height vs HGHT
            if k == 0 and c["lods"][0] != c["lods"][2]:      # not aliased (underwater chunks alias LOD2)
                bad = 0
                for b in m.batches[bf:bf + bc]:
                    if m.mats[b[0]][1] & 0xFF:          # foam (not opaque)
                        continue
                    if b[1]:
                        bad += 1
                    for ix in m.idx[b[2]:b[2] + b[3]]:
                        v = m.verts[vf + ix]
                        fi = (v[0] - inf["x0"]) / inf["cell"]
                        fj = (v[2] - inf["z0"]) / inf["cell"]
                        I, J = int(round(fi)), int(round(fj))
                        if abs(fi - I) > 1e-3 or abs(fj - J) > 1e-3:
                            bad += 1
                            continue
                        hy = height_at(m, I, J)
                        if abs(v[1] - hy) > 0.026:
                            bad += 1
                        u, w = v[4], v[5]
                        if u < -1e-4 or w < -1e-4 or u > umax or w > umax:
                            bad += 1
                if bad:
                    P.append("chunk %d,%d LOD0: %d terrain vertices off the grid/HGHT/UV range" % (c["cx"], c["cz"],
                                                                                                   bad))
    if data is not None and reencode(m) != data:
        P.append("round trip: re-encoded bytes differ")
    return P
