"""Deterministic randomness and noise.

Rules: never use hash(), time, os.urandom, set iteration or dict order of unordered sources for anything
that ends up in an asset. Everything here depends only on its arguments, so the same inputs give the same
bytes on every run and every machine with the same Python float behaviour.

    r = Rng(11, "tree", "pine", 3)     # a random.Random seeded from a stable hash of (11, "tree", "pine", 3)
    r.random(); r.uniform(1, 2); r.pick(["a", "b"]); r.child("bark")
    value_noise(x, y, seed=4, period=8)            # smooth noise in [0,1], lattice period 8 (tileable)
    fbm(x, y, octaves=4, seed=4, period=8)         # fractal sum in [0,1]
    ridged(x, y, octaves=4, seed=4)                # ridges in [0,1]
    noise_grid(256, 256, cells=8, octaves=4, seed=4)  # rows of floats in [0,1], tiles seamlessly
"""

import math
import random

_FNV64_OFFSET = 0xCBF29CE484222325
_FNV64_PRIME = 0x100000001B3
_M64 = 0xFFFFFFFFFFFFFFFF
_M32 = 0xFFFFFFFF


def _part_bytes(p):
    if isinstance(p, bytes):
        return p
    if isinstance(p, bool):
        return b"1" if p else b"0"
    if isinstance(p, int):
        return str(p).encode("ascii")
    if isinstance(p, float):
        return repr(p).encode("ascii")
    if isinstance(p, (tuple, list)):
        return b"(" + b",".join(_part_bytes(x) for x in p) + b")"
    return str(p).encode("utf-8")


def stable_hash(*parts):
    """64-bit FNV-1a of the parts (str/int/float/bytes/tuples), joined with a separator.
    stable_hash(11, "tree") == stable_hash(11, "tree") on every run; never uses hash()."""
    h = _FNV64_OFFSET
    first = True
    for p in parts:
        if not first:
            h ^= 0x1F
            h = (h * _FNV64_PRIME) & _M64
        first = False
        for b in _part_bytes(p):
            h ^= b
            h = (h * _FNV64_PRIME) & _M64
    return h


def stable_hash32(*parts):
    """32-bit fold of stable_hash."""
    h = stable_hash(*parts)
    return (h ^ (h >> 32)) & _M32


class Rng(random.Random):
    """random.Random seeded from stable_hash(seed, *names).

    Use one Rng per independent decision stream (e.g. Rng(seed, "scatter", zone_id)) so adding draws in one
    place doesn't shift the numbers elsewhere. Safe methods (stable across Python 3 versions): random,
    uniform, gauss, choice, shuffle, sample plus the helpers below. Avoid randrange/randint with floats."""

    def __init__(self, seed=0, *names):
        self.key = (seed,) + tuple(names)
        random.Random.__init__(self, stable_hash(seed, *names))

    def child(self, *names):
        """Independent sub-stream: Rng(seed, *names_of_parent, *names). Doesn't consume draws."""
        return Rng(*(self.key + tuple(names)))

    def range(self, a, b):
        """uniform float in [a, b)."""
        return a + (b - a) * self.random()

    def int(self, a, b):
        """integer in [a, b] inclusive."""
        return a + int(self.random() * (b - a + 1)) if b > a else a

    def chance(self, p):
        """True with probability p."""
        return self.random() < p

    def pick(self, seq):
        """one element of a sequence (list/tuple/str)."""
        return seq[int(self.random() * len(seq)) % len(seq)]

    def weighted(self, items):
        """items = [(value, weight), ...] -> value."""
        total = 0.0
        for _, w in items:
            total += w
        x = self.random() * total
        for v, w in items:
            x -= w
            if x < 0:
                return v
        return items[-1][0]

    def jitter(self, v, amount):
        """v + uniform(-amount, amount)."""
        return v + (self.random() * 2.0 - 1.0) * amount

    def sign(self):
        return 1 if self.random() < 0.5 else -1

    def unit2(self):
        """random unit vector in the XZ plane as (x, z)."""
        a = self.random() * 2.0 * math.pi
        return (math.sin(a), math.cos(a))


# ---------------------------------------------------------------------------------------------------- hashes

def hash2i(ix, iy, seed=0):
    """32-bit integer hash of an integer lattice point (deterministic, well mixed)."""
    h = (ix * 0x27D4EB2D + iy * 0x165667B1 + (seed & _M32) * 0x9E3779B9 + 0x7F4A7C15) & _M32
    h ^= h >> 15
    h = (h * 0x85EBCA6B) & _M32
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & _M32
    h ^= h >> 16
    return h


def hash2(ix, iy, seed=0):
    """lattice hash as a float in [0, 1)."""
    return hash2i(ix, iy, seed) / 4294967296.0


def hash1(ix, seed=0):
    """1D lattice hash in [0, 1)."""
    return hash2i(ix, 0x5BD1E995, seed) / 4294967296.0


# ---------------------------------------------------------------------------------------------------- noise

def _fade(t):
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)


def value_noise(x, y, seed=0, period=None):
    """Smooth 2D value noise in [0, 1]. Lattice spacing 1. period=int or (px, py): wraps the lattice so the
    noise tiles with that period (x and x + period give the same value)."""
    ix = int(math.floor(x))
    iy = int(math.floor(y))
    fx = _fade(x - ix)
    fy = _fade(y - iy)
    x0, y0, x1, y1 = ix, iy, ix + 1, iy + 1
    if period is not None:
        if isinstance(period, (tuple, list)):
            px, py = int(period[0]), int(period[1])
        else:
            px = py = int(period)
        x0 %= px
        x1 %= px
        y0 %= py
        y1 %= py
    a = hash2(x0, y0, seed)
    b = hash2(x1, y0, seed)
    c = hash2(x0, y1, seed)
    d = hash2(x1, y1, seed)
    ab = a + (b - a) * fx
    cd = c + (d - c) * fx
    return ab + (cd - ab) * fy


def value_noise_1d(x, seed=0, period=None):
    """Smooth 1D value noise in [0, 1] (profiles, skylines). period wraps like value_noise."""
    ix = int(math.floor(x))
    f = _fade(x - ix)
    x0, x1 = ix, ix + 1
    if period is not None:
        x0 %= int(period)
        x1 %= int(period)
    a = hash1(x0, seed)
    b = hash1(x1, seed)
    return a + (b - a) * f


def fbm(x, y, octaves=4, lacunarity=2, gain=0.5, seed=0, period=None):
    """Fractal sum of value_noise, normalised to [0, 1]. For tileable fBm give an integer period and keep
    lacunarity an integer (the period is multiplied by it per octave)."""
    amp = 1.0
    total = 0.0
    norm = 0.0
    f = 1.0
    per = period
    for o in range(octaves):
        total += amp * value_noise(x * f, y * f, seed + o * 1013, per)
        norm += amp
        amp *= gain
        f *= lacunarity
        if per is not None:
            if isinstance(per, (tuple, list)):
                per = (int(per[0] * lacunarity), int(per[1] * lacunarity))
            else:
                per = int(per * lacunarity)
    return total / norm


def ridged(x, y, octaves=4, lacunarity=2, gain=0.5, seed=0, period=None):
    """Ridged multifractal-ish noise in [0, 1]: sharp crests (rock, mountain skylines, cracks)."""
    amp = 1.0
    total = 0.0
    norm = 0.0
    f = 1.0
    per = period
    for o in range(octaves):
        n = value_noise(x * f, y * f, seed + o * 7919, per)
        n = 1.0 - abs(n * 2.0 - 1.0)
        total += amp * n * n
        norm += amp
        amp *= gain
        f *= lacunarity
        if per is not None:
            if isinstance(per, (tuple, list)):
                per = (int(per[0] * lacunarity), int(per[1] * lacunarity))
            else:
                per = int(per * lacunarity)
    return total / norm


def noise_grid(w, h, cells=8, octaves=4, gain=0.5, seed=0, tile=True, kind="fbm"):
    """w x h grid (list of rows) of noise in [0, 1]; `cells` lattice cells across the width at octave 0.
    tile=True makes it wrap seamlessly (texture tiling). kind: "fbm" | "ridged" | "value".
    The result is stretched to use the full [0, 1] range."""
    cy = max(1, int(round(cells * h / float(w))))
    period = (cells, cy) if tile else None
    sx = cells / float(w)
    sy = cy / float(h)
    fn = {"fbm": fbm, "ridged": ridged}.get(kind)
    rows = []
    lo = 1e9
    hi = -1e9
    for y in range(h):
        row = []
        fy = (y + 0.5) * sy
        for x in range(w):
            fx = (x + 0.5) * sx
            if fn is None:
                v = value_noise(fx, fy, seed, period)
            else:
                v = fn(fx, fy, octaves, 2, gain, seed, period)
            row.append(v)
            if v < lo:
                lo = v
            if v > hi:
                hi = v
        rows.append(row)
    span = hi - lo if hi > lo else 1.0
    for row in rows:
        for i in range(w):
            row[i] = (row[i] - lo) / span
    return rows
