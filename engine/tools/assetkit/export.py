"""The ONLY writers (and readers) of engine binary formats. Little-endian, packed with struct.

    .msh  mesh file (DESIGN 8.3)            MshFile, write_msh(path, msh) / msh_bytes(msh), read_msh(path|bytes)
    .trk  chunk container (DESIGN 4.5)      trk_bytes(chunks), write_trk(path, chunks), read_trk(path|bytes)
    VPOL/BTCH/INDX vertex pools + batches   PoolEncoder (add_batch / add_geo -> encode()), decode_pools()
    helpers                                 pack_name(s, n), pack_names(list, n), latin1(s), crc32(b)

Binary layouts (all little-endian, every struct size a multiple of 4):

.msh  header (60 B): char tag[4] "RMSH"; u32 version 1; u32 numVerts, numIdx, numMats, numParts, numLods,
      numTags; Aabb box (lo xyz, hi xyz floats); float radius (max distance of any vertex from the mesh origin)
      MeshMatFile[numMats] 68 B: char tex[32], tex1[32] (names w/o path/extension, NUL padded, "" = none);
                                 u8 blend (RB_*), stage1 (RS1_*), cullNone, flags (MM_*)
      MeshPart[numParts]   12 B: s32 mat, firstIdx (absolute into idx[]), numIdx
      MeshLod[numLods]     20 B: float maxDist; s32 firstPart, numParts, firstVert, numVerts
      MeshTag[numTags]     40 B: char name[16]; Vec3 pos; Vec3 dir
      RVert1[numVerts]     24 B: float x, y, z; u32 color (0xAARRGGBB); float u, v
      Vec3 normals[numVerts]
      u16 idx[numIdx]      triangle lists, clockwise = front, RELATIVE to the owning LOD's firstVert
      (then zero padding to a multiple of 4)
      Parts of a LOD are sorted by material; LODs own disjoint vertex ranges. Last LOD maxDist = MSH_NO_LIMIT.

.trk  header (16 B): char tag[4] "RTRK"; u32 version; u32 numChunks; u32 crc32 (zlib/IEEE) of all bytes after
      the header. Chunks: char id[4]; u32 size (unpadded); u8 data[size]; zero pad to a multiple of 4.

VPOL  u32 numPools; per pool: u32 fmt (RFMT_V1 = 1 -> RVert1 24 B, RFMT_V2 = 2 -> RVert2 32 B), u32 count,
      then count vertices. RVert2 = float x, y, z; u32 color; float u, v, u2, v2.
BTCH  array of TrkBatch 24 B: s32 cell, lod, mat, pool; u32 firstIdx (into INDX), numIdx. count = size / 24.
INDX  u16 indices, relative to the batch's pool start. count = size / 2.
"""

import struct
import zlib

# ---------------------------------------------------------------------------------------------- engine enums
# (mirror DESIGN 2.5; keep in sync with e_rend.h / e_mesh.h / e_coll.h)
RB_OPAQUE, RB_ALPHATEST, RB_BLEND, RB_ADD, RB_SHADOW = 0, 1, 2, 3, 4
RS1_NONE, RS1_DETAIL, RS1_ENVADD = 0, 1, 2
MM_TINT, MM_ENV, MM_WHEEL, MM_GLASS, MM_LIGHT = 1, 2, 4, 8, 16
RFMT_V1, RFMT_V2 = 1, 2
(SURF_NONE, SURF_ASPHALT, SURF_PAINT, SURF_KERB, SURF_GRAVEL, SURF_GRASS, SURF_DIRT, SURF_WATER,
 SURF_COUNT) = range(9)

MSH_VERSION = 1
TRK_VERSION = 1
MSH_NO_LIMIT = 100000.0
"""maxDist written for the last LOD (= no distance limit)."""
MAX_LODS = 4        # Mesh.lod[4] in e_mesh.h
MAX_TAGS = 24       # Mesh.tags[24]
MAX_POOL_VERTS = 65535

_MAT = struct.Struct("<32s32s4B")       # 68 B
_PART = struct.Struct("<3i")            # 12 B
_LOD = struct.Struct("<f4i")            # 20 B
_TAG = struct.Struct("<16s3f3f")        # 40 B
_V1 = struct.Struct("<3fI2f")           # 24 B
_V2 = struct.Struct("<3fI4f")           # 32 B
_N = struct.Struct("<3f")               # 12 B
_BATCH = struct.Struct("<4i2I")         # 24 B
_CHUNK = struct.Struct("<4sI")
_TRKHDR = struct.Struct("<4sIII")


def f32(x):
    """Round a Python float to the nearest float32 value (what the file will hold)."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def crc32(data):
    """CRC-32 (IEEE, same as zlib.crc32 and e_crc32)."""
    return zlib.crc32(data) & 0xFFFFFFFF


def latin1(s):
    """str -> Latin-1 bytes (the engine's text encoding); unknown characters become '?'."""
    return s.encode("latin-1", "replace")


def pack_name(s, n):
    """Fixed-size NUL-padded string field (at most n-1 characters + NUL). Raises if too long."""
    b = latin1(s) if isinstance(s, str) else bytes(s)
    if len(b) > n - 1:
        raise ValueError("name %r longer than %d characters" % (s, n - 1))
    return b + b"\0" * (n - len(b))


def unpack_name(b):
    """Fixed-size field -> str (up to the first NUL)."""
    i = b.find(b"\0")
    if i >= 0:
        b = b[:i]
    return b.decode("latin-1")


def pack_names(names, n=32):
    """List of names -> concatenated fixed-size fields (TEXL, MSHL)."""
    return b"".join(pack_name(s, n) for s in names)


def unpack_names(data, n=32):
    return [unpack_name(data[i:i + n]) for i in range(0, len(data) - n + 1, n)]


def pad4(b):
    return b + b"\0" * ((-len(b)) & 3)


def _load(src):
    if isinstance(src, (bytes, bytearray)):
        return bytes(src)
    with open(src, "rb") as f:
        return f.read()


def _save(path, data):
    if path is not None:
        with open(path, "wb") as f:
            f.write(data)
    return data


# ---------------------------------------------------------------------------------------------------- .msh

class MshFile(object):
    """In-memory image of a .msh file; field lists hold exactly what the file stores.

    mats    [dict(tex, tex1, blend, stage1, cullNone, flags)]
    parts   [(mat, firstIdx, numIdx)]
    lods    [(maxDist, firstPart, numParts, firstVert, numVerts)]
    tags    [(name, (px, py, pz), (dx, dy, dz))]
    verts   [(x, y, z, color_u32, u, v)]
    normals [(nx, ny, nz)]
    idx     [int]  (relative to the owning LOD's firstVert)
    box     ((lox, loy, loz), (hix, hiy, hiz)); radius float
    """

    def __init__(self):
        self.version = MSH_VERSION
        self.mats = []
        self.parts = []
        self.lods = []
        self.tags = []
        self.verts = []
        self.normals = []
        self.idx = []
        self.box = ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0))
        self.radius = 0.0
        self.name = ""

    def lod_tris(self, lod):
        """Triangle count of one LOD."""
        _, fp, np_, _, _ = self.lods[lod]
        return sum(self.parts[p][2] for p in range(fp, fp + np_)) // 3

    def tri_counts(self):
        return [self.lod_tris(i) for i in range(len(self.lods))]

    def lod_triangles(self, lod):
        """Yield (mat, i0, i1, i2) with ABSOLUTE vertex indices for one LOD."""
        _, fp, np_, fv, _ = self.lods[lod]
        for p in range(fp, fp + np_):
            mat, fi, ni = self.parts[p]
            for k in range(fi, fi + ni - 2, 3):
                yield (mat, fv + self.idx[k], fv + self.idx[k + 1], fv + self.idx[k + 2])

    def compute_bounds(self):
        """Set box and radius from the vertices (float32-rounded)."""
        if not self.verts:
            self.box = ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0))
            self.radius = 0.0
            return
        xs = [v[0] for v in self.verts]
        ys = [v[1] for v in self.verts]
        zs = [v[2] for v in self.verts]
        self.box = ((f32(min(xs)), f32(min(ys)), f32(min(zs))), (f32(max(xs)), f32(max(ys)), f32(max(zs))))
        r2 = 0.0
        for v in self.verts:
            d = v[0] * v[0] + v[1] * v[1] + v[2] * v[2]
            if d > r2:
                r2 = d
        r = f32(r2 ** 0.5)
        if r * r < r2:
            r = f32(r * (1.0 + 1e-6) + 1e-7)
        self.radius = r


def msh_bytes(m):
    """MshFile -> bytes (exact DESIGN 8.3 layout)."""
    out = bytearray()
    (lx, ly, lz), (hx, hy, hz) = m.box
    out += struct.pack("<4sI6I", b"RMSH", m.version, len(m.verts), len(m.idx), len(m.mats), len(m.parts),
                       len(m.lods), len(m.tags))
    out += struct.pack("<7f", lx, ly, lz, hx, hy, hz, m.radius)
    for mt in m.mats:
        out += _MAT.pack(pack_name(mt.get("tex", ""), 32), pack_name(mt.get("tex1", ""), 32),
                         int(mt.get("blend", 0)), int(mt.get("stage1", 0)), int(mt.get("cullNone", 0)),
                         int(mt.get("flags", 0)))
    for p in m.parts:
        out += _PART.pack(*p)
    for l in m.lods:
        out += _LOD.pack(*l)
    for name, pos, d in m.tags:
        out += _TAG.pack(pack_name(name, 16), pos[0], pos[1], pos[2], d[0], d[1], d[2])
    for v in m.verts:
        out += _V1.pack(v[0], v[1], v[2], v[3] & 0xFFFFFFFF, v[4], v[5])
    for n in m.normals:
        out += _N.pack(n[0], n[1], n[2])
    out += struct.pack("<%dH" % len(m.idx), *m.idx)
    return pad4(bytes(out))


def write_msh(path, m):
    """Write an MshFile to path (or return the bytes when path is None)."""
    return _save(path, msh_bytes(m))


MSH_HEADER_SIZE = 60


def read_msh(src):
    """Path or bytes -> MshFile. Raises ValueError on a malformed file (validate.py reports details)."""
    d = _load(src)
    if len(d) < MSH_HEADER_SIZE or d[:4] != b"RMSH":
        raise ValueError("not an RMSH file")
    m = MshFile()
    (tag, m.version, nv, ni, nm, np_, nl, nt) = struct.unpack_from("<4sI6I", d, 0)
    f = struct.unpack_from("<7f", d, 32)
    m.box = ((f[0], f[1], f[2]), (f[3], f[4], f[5]))
    m.radius = f[6]
    need = (MSH_HEADER_SIZE + nm * _MAT.size + np_ * _PART.size + nl * _LOD.size + nt * _TAG.size +
            nv * (_V1.size + _N.size) + ni * 2)
    if need > len(d):
        raise ValueError("file too short: counts need %d bytes, file has %d" % (need, len(d)))
    o = MSH_HEADER_SIZE
    for _ in range(nm):
        t0, t1, bl, s1, cn, fl = _MAT.unpack_from(d, o)
        m.mats.append({"tex": unpack_name(t0), "tex1": unpack_name(t1), "blend": bl, "stage1": s1,
                       "cullNone": cn, "flags": fl})
        o += _MAT.size
    for _ in range(np_):
        m.parts.append(_PART.unpack_from(d, o))
        o += _PART.size
    for _ in range(nl):
        m.lods.append(_LOD.unpack_from(d, o))
        o += _LOD.size
    for _ in range(nt):
        t = _TAG.unpack_from(d, o)
        m.tags.append((unpack_name(t[0]), (t[1], t[2], t[3]), (t[4], t[5], t[6])))
        o += _TAG.size
    for _ in range(nv):
        m.verts.append(_V1.unpack_from(d, o))
        o += _V1.size
    for _ in range(nv):
        m.normals.append(_N.unpack_from(d, o))
        o += _N.size
    m.idx = list(struct.unpack_from("<%dH" % ni, d, o))
    m.file_size = len(d)
    m.expected_size = need + ((-need) & 3)
    return m


# ---------------------------------------------------------------------------------------------------- .trk

def trk_bytes(chunks, version=TRK_VERSION):
    """chunks = [(id, data_bytes), ...] in file order (id: 4-char str, e.g. "SKY ") -> complete file bytes."""
    body = bytearray()
    for cid, data in chunks:
        bid = cid.encode("ascii") if isinstance(cid, str) else bytes(cid)
        if len(bid) != 4:
            raise ValueError("chunk id %r must be 4 characters" % (cid,))
        body += _CHUNK.pack(bid, len(data))
        body += data
        body += b"\0" * ((-len(data)) & 3)
    body = bytes(body)
    return _TRKHDR.pack(b"RTRK", version, len(chunks), crc32(body)) + body


def write_trk(path, chunks, version=TRK_VERSION):
    """Write a .trk container (see trk_bytes). Returns the bytes."""
    return _save(path, trk_bytes(chunks, version))


class TrkFile(object):
    """read_trk result: version, chunks [(id_str, bytes)], crc_stored, crc_actual, problems [str]."""

    def __init__(self):
        self.version = 0
        self.chunks = []
        self.num_chunks = 0
        self.crc_stored = 0
        self.crc_actual = 0
        self.problems = []

    def get(self, cid):
        """First chunk with that id (bytes) or None."""
        for i, d in self.chunks:
            if i == cid:
                return d
        return None


def read_trk(src):
    """Path or bytes -> TrkFile. Structural problems are collected in .problems (not raised) unless the file
    isn't an RTRK file at all."""
    d = _load(src)
    if len(d) < 16 or d[:4] != b"RTRK":
        raise ValueError("not an RTRK file")
    t = TrkFile()
    _, t.version, t.num_chunks, t.crc_stored = _TRKHDR.unpack_from(d, 0)
    t.crc_actual = crc32(d[16:])
    if t.crc_actual != t.crc_stored:
        t.problems.append("crc32 mismatch (stored %08x, actual %08x)" % (t.crc_stored, t.crc_actual))
    o = 16
    while o < len(d):
        if o + 8 > len(d):
            t.problems.append("truncated chunk header at offset %d" % o)
            break
        cid, size = _CHUNK.unpack_from(d, o)
        o += 8
        if o + size > len(d):
            t.problems.append("chunk %r size %d runs past the end of the file" % (cid, size))
            break
        try:
            sid = cid.decode("ascii")
        except UnicodeDecodeError:
            sid = repr(cid)
            t.problems.append("chunk id %r is not ASCII" % (cid,))
        t.chunks.append((sid, d[o:o + size]))
        o += size
        padn = (-size) & 3
        if d[o:o + padn] != b"\0" * padn:
            t.problems.append("chunk %r padding is not zero" % sid)
        o += padn
    if len(t.chunks) != t.num_chunks:
        t.problems.append("header says %d chunks, file has %d" % (t.num_chunks, len(t.chunks)))
    return t


# ---------------------------------------------------------------------------------------------------- pools

def rvert_size(fmt):
    return 24 if fmt == RFMT_V1 else 32


def pack_rvert(fmt, v):
    """v = (x, y, z, color, u, v[, u2, v2]) -> bytes."""
    if fmt == RFMT_V1:
        return _V1.pack(v[0], v[1], v[2], v[3] & 0xFFFFFFFF, v[4], v[5])
    u2 = v[6] if len(v) > 6 else 0.0
    v2 = v[7] if len(v) > 7 else 0.0
    return _V2.pack(v[0], v[1], v[2], v[3] & 0xFFFFFFFF, v[4], v[5], u2, v2)


class PoolData(object):
    """Decoded VPOL/BTCH/INDX: pools [(fmt, [verts])], batches [(cell, lod, mat, pool, firstIdx, numIdx)],
    idx [u16]. encode_pools(PoolData) gives the same bytes back."""

    def __init__(self):
        self.pools = []
        self.batches = []
        self.idx = []


class PoolEncoder(object):
    """Builds the shared vertex-pool / batch / index encoding of .trk files (DESIGN 3.6, 4.5).

    One pool per (cell, fmt) holds the vertices of all batches of that cell and format (both LODs); a pool
    that would exceed 65535 vertices is closed and a new one started. Batches are stored in the order added
    (sort them before adding if the reader wants an order); indices are relative to the pool start.

        enc = PoolEncoder()
        enc.add_batch(cell=3, lod=0, mat=1, fmt=RFMT_V1, verts=[(x,y,z,col,u,v), ...], idx=[0,1,2, ...])
        enc.add_geo(cell=3, lod=0, geo=g, mat_index={"road": 1}, fmt=RFMT_V1)   # splits a Geo by material
        vpol, btch, indx = enc.encode()
    """

    def __init__(self, max_verts=MAX_POOL_VERTS):
        self.max_verts = max_verts
        self.data = PoolData()
        self._open = {}     # (cell, fmt) -> pool index

    def add_batch(self, cell, lod, mat, fmt, verts, idx):
        """Append one batch; verts are this batch's own vertices, idx index into them. Returns batch index."""
        if len(idx) % 3:
            raise ValueError("index count %d is not a multiple of 3" % len(idx))
        if len(verts) > self.max_verts:
            raise ValueError("batch has %d vertices (max %d per pool): split it" % (len(verts), self.max_verts))
        for i in idx:
            if i < 0 or i >= len(verts):
                raise ValueError("index %d out of range (%d vertices)" % (i, len(verts)))
        key = (cell, fmt)
        p = self._open.get(key)
        if p is None or len(self.data.pools[p][1]) + len(verts) > self.max_verts:
            self.data.pools.append((fmt, []))
            p = len(self.data.pools) - 1
            self._open[key] = p
        pool = self.data.pools[p][1]
        base = len(pool)
        pool.extend(verts)
        first = len(self.data.idx)
        self.data.idx.extend(base + i for i in idx)
        self.data.batches.append((cell, lod, mat, p, first, len(idx)))
        return len(self.data.batches) - 1

    def add_geo(self, cell, lod, geo, mat_index, fmt=RFMT_V1):
        """Add a mesh.Geo, one batch per material (in first-use order). mat_index maps the Geo's material
        keys (Material objects' .name or strings) to MATL indices: dict or function. Returns batch indices."""
        out = []
        for key, verts, idx in geo.batches(fmt):
            mi = mat_index(key) if callable(mat_index) else mat_index[key]
            out.append(self.add_batch(cell, lod, mi, fmt, verts, idx))
        return out

    def encode(self):
        """-> (vpol_bytes, btch_bytes, indx_bytes) chunk payloads."""
        return encode_pools(self.data)


def encode_pools(pd):
    """PoolData -> (vpol, btch, indx) bytes."""
    vp = bytearray(struct.pack("<I", len(pd.pools)))
    for fmt, verts in pd.pools:
        vp += struct.pack("<II", fmt, len(verts))
        for v in verts:
            vp += pack_rvert(fmt, v)
    bt = bytearray()
    for b in pd.batches:
        bt += _BATCH.pack(*b)
    ix = struct.pack("<%dH" % len(pd.idx), *pd.idx)
    return bytes(vp), bytes(bt), ix


def decode_pools(vpol, btch, indx):
    """Chunk payloads -> PoolData (raises ValueError on malformed data)."""
    pd = PoolData()
    if len(vpol) < 4:
        raise ValueError("VPOL too short")
    (n,) = struct.unpack_from("<I", vpol, 0)
    o = 4
    for _ in range(n):
        if o + 8 > len(vpol):
            raise ValueError("VPOL truncated")
        fmt, cnt = struct.unpack_from("<II", vpol, o)
        o += 8
        if fmt not in (RFMT_V1, RFMT_V2):
            raise ValueError("VPOL pool format %d unknown" % fmt)
        st = _V1 if fmt == RFMT_V1 else _V2
        if o + cnt * st.size > len(vpol):
            raise ValueError("VPOL pool runs past the chunk")
        verts = [st.unpack_from(vpol, o + i * st.size) for i in range(cnt)]
        o += cnt * st.size
        pd.pools.append((fmt, verts))
    if o != len(vpol):
        raise ValueError("VPOL has %d trailing bytes" % (len(vpol) - o))
    if len(btch) % _BATCH.size:
        raise ValueError("BTCH size %d is not a multiple of %d" % (len(btch), _BATCH.size))
    pd.batches = [_BATCH.unpack_from(btch, i) for i in range(0, len(btch), _BATCH.size)]
    if len(indx) % 2:
        raise ValueError("INDX size is odd")
    pd.idx = list(struct.unpack("<%dH" % (len(indx) // 2), indx))
    return pd


def check_pools(pd):
    """Consistency problems of decoded pools as a list of strings (validate.py uses it)."""
    probs = []
    for p, (fmt, verts) in enumerate(pd.pools):
        if len(verts) > MAX_POOL_VERTS:
            probs.append("pool %d has %d vertices (> 65535)" % (p, len(verts)))
    for b, (cell, lod, mat, pool, fi, ni) in enumerate(pd.batches):
        if pool < 0 or pool >= len(pd.pools):
            probs.append("batch %d: pool %d out of range" % (b, pool))
            continue
        if ni % 3:
            probs.append("batch %d: numIdx %d not a multiple of 3" % (b, ni))
        if fi + ni > len(pd.idx):
            probs.append("batch %d: indices %d..%d past INDX (%d)" % (b, fi, fi + ni, len(pd.idx)))
            continue
        nv = len(pd.pools[pool][1])
        bad = [i for i in pd.idx[fi:fi + ni] if i >= nv]
        if bad:
            probs.append("batch %d: %d indices >= pool size %d" % (b, len(bad), nv))
        if lod not in (0, 1):
            probs.append("batch %d: lod %d not 0/1" % (b, lod))
    return probs
