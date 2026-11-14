"""height.py - island height field (DESIGN 8.1, ENGINE.md 3.4), airbase plateau, coast rule, quantisation.

H(x, z) is a pure function of position (assetkit.rng noise only), evaluated on the gridN x gridN grid (island.txt
`grid`: samples, cell; default 513 x 32 m; T03b island: 513 x 80 m = 41 km), centred on the origin:
  coast      ellipse (island_center/radii/rot) with a wobbly radius (periodic 1D fBm over the angle); dcoast =
             signed radial distance to the coast line (+ offshore)
  sea        -coast_depth at the coast down to -floor over the shelf width (noisy per angle); floor clamp
  land       coastal plain (plain_h reached plain_w inland, concave rise) + hills (fBm, positive) + weather-side cliffs
  volcano    cone h0 (1 - r/R)^exp, h0 chosen so the rim is at `summit`; crater bowl with a flat floor; rim noise;
             radial ridges/gullies (ridged noise in (angle x count, r / wavelength)); fBm erosion; lava channels
             (raised `lava_levee`, weight kept for the classes); satellite cones; all faded in from the coast
  airbase    plateau rectangle in the runway frame at `plateau` height, smoothstep blend
Post: floor clamp; coast rule (every sample < 0 is <= -coast_depth, every land sample >= beach_min); quantise to
u16 (hMin -200, 0.05 m); LOD0 chunk-border snap (odd samples on chunk lines = mean of their two neighbours
along the line, in float, so LOD0 meets LOD1 without T-cracks); HGHT stores the rounded snapped values.
T03b additions (all optional, defaults keep the T03 island): `feature_scale` (internal noise wavelengths and fade
distances x this), `coast_detail` (angular noise lattice of the coast), `bay` (bays / headlands), `beach` (patchy
flat beach zones), `islet` (small offshore islands), `edge_fade` (sea forced to the floor near the grid border so
the stored chunks end in deep water).
"""
import math

from assetkit import rng as R

N = 513               # defaults; build() takes island.txt `grid` (hf.N, hf.cell, hf.x0, hf.z0)
CELL = 32.0
X0 = Z0 = -8192.0
HMIN = -200.0
HSCALE = 0.05
CHUNK = 16            # cells per chunk
NC = 32               # chunks per side (= (N - 1) / CHUNK)
TWO_PI = 2.0 * math.pi


def smoothstep(e0, e1, x):
    if e1 == e0:
        return 1.0 if x >= e1 else 0.0
    t = (x - e0) / (e1 - e0)
    if t <= 0.0:
        return 0.0
    if t >= 1.0:
        return 1.0
    return t * t * (3.0 - 2.0 * t)


def ang_fbm(theta, k0, octaves, seed):
    """Periodic fBm over an angle (radians) in [0, 1]: k0 lattice cells around the circle at octave 0."""
    t = (theta / TWO_PI) % 1.0
    tot = norm = 0.0
    amp = 1.0
    k = k0
    for o in range(octaves):
        tot += amp * R.value_noise_1d(t * k, seed + o * 131, period=k)
        norm += amp
        amp *= 0.5
        k *= 2
    return tot / norm


def wrap_pi(a):
    while a > math.pi:
        a -= TWO_PI
    while a < -math.pi:
        a += TWO_PI
    return a


class Frame(object):
    """Runway frame: a = along the takeoff heading, b = right of it."""

    def __init__(self, cx, cz, heading):
        self.cx, self.cz, self.hd = cx, cz, heading
        h = math.radians(heading)
        self.d = (math.sin(h), math.cos(h))
        self.r = (math.cos(h), -math.sin(h))

    def to_local(self, x, z):
        dx, dz = x - self.cx, z - self.cz
        return dx * self.d[0] + dz * self.d[1], dx * self.r[0] + dz * self.r[1]

    def to_world(self, a, b):
        return (self.cx + a * self.d[0] + b * self.r[0], self.cz + a * self.d[1] + b * self.r[1])


class Model(object):
    def __init__(self, src):
        g = src.get
        self.seed = int(src.g1("seed"))
        s = self.seed
        self.S = dict((name, (R.stable_hash32(s, name) & 0x3FFFFFF)) for name in (
            "wob", "wobf", "shelf", "hills", "cliff", "warp", "ridge", "eros", "rim", "lava"))
        self.icx, self.icz = g("island_center")
        self.A, self.B = g("island_radii")
        rot = math.radians(src.g1("island_rot"))
        self.sa, self.ca = math.sin(rot), math.cos(rot)
        self.wob, self.wobf = g("coast_wobble")
        self.plain_h, self.plain_w = g("plain")
        self.hill_amp, self.hill_l, self.hill_fade = g("hills")
        caz, cspread, self.cliff_h, self.cliff_w = g("cliffs")
        self.cliff_az = math.radians(caz)
        self.cliff_cos = math.cos(math.radians(cspread))
        self.shelf_w, self.floor = g("shelf")
        self.coast_depth = src.g1("coast_depth")
        self.beach_min = src.g1("beach_min")
        self.vcx, self.vcz = g("cone_center")
        self.Rv = src.g1("cone_radius")
        self.exp = src.g1("cone_exp")
        self.summit = src.g1("summit")
        self.Rr, self.cdepth, self.cfloor = g("crater")
        self.h0 = self.summit / (1.0 - self.Rr / self.Rv) ** self.exp
        self.rim_noise = src.g1("rim_noise")
        self.ridge_k, self.ridge_amp, self.ridge_l = g("ridges")
        self.ridge_k = int(self.ridge_k)
        self.eros_amp, self.eros_l = g("erosion")
        self.levee = src.g1("lava_levee")
        self.lava = [tuple(v) for v in src.rep("lava")]
        self.sats = [tuple(v) for v in src.rep("satellite")]
        rc = g("runway_center")
        self.frame = Frame(rc[0], rc[1], src.g1("runway_heading"))
        self.plat_h, self.plat_blend = g("plateau")
        self.prect = g("plateau_rect")
        self.fs = src.g1("feature_scale")
        self.cd_lo, self.cd_hi = [int(v) for v in g("coast_detail")]
        self.bays = [tuple(v) for v in src.rep("bay")]
        self.beach_w, self.beach_cov = g("beach")
        self.islets = [tuple(v) for v in src.rep("islet")]
        self.S["beach"] = R.stable_hash32(s, "beach") & 0x3FFFFFF
        self.Rmean = 0.5 * (self.A + self.B)

    # ---- pieces
    def coast(self, x, z):
        """-> (dcoast signed metres, + = offshore, theta)"""
        dx, dz = x - self.icx, z - self.icz
        r = math.hypot(dx, dz)
        theta = math.atan2(dx, dz)
        along = dx * self.sa + dz * self.ca
        across = dx * self.ca - dz * self.sa
        rho = math.sqrt((along / self.A) ** 2 + (across / self.B) ** 2)
        wob = (1.0 + self.wob * (ang_fbm(theta, self.cd_lo, 4, self.S["wob"]) - 0.5) * 2.0 +
               self.wobf * (ang_fbm(theta, self.cd_hi, 2, self.S["wobf"]) - 0.5) * 2.0)
        for (baz, bhw, bdep) in self.bays:          # bay (depth > 0) / headland (< 0): cos^2 bump over the angle
            da = abs(wrap_pi(theta - math.radians(baz)))
            hw = math.radians(bhw)
            if da < hw:
                wob -= bdep / self.Rmean * math.cos(0.5 * math.pi * da / hw) ** 2
        s = rho / wob
        if s < 1e-6:
            return -1e9, theta
        return r * (1.0 - 1.0 / s), theta

    def beach_width(self, theta):
        """Flat beach zone width (m) at this coast angle: patchy (coverage = share of the coast), none on cliffs."""
        if self.beach_w <= 0.0:
            return 0.0
        n = ang_fbm(theta, max(4, self.cd_lo * 2), 3, self.S["beach"])
        f = smoothstep(1.0 - self.beach_cov - 0.08, 1.0 - self.beach_cov + 0.08, n)
        cw = smoothstep(self.cliff_cos, 1.0, math.cos(wrap_pi(theta - self.cliff_az)))
        return self.beach_w * f * (1.0 - smoothstep(0.0, 0.3, cw))

    def plateau_w(self, x, z):
        a, b = self.frame.to_local(x, z)
        a0, a1, b0, b1 = self.prect
        da = max(a0 - a, 0.0, a - a1)
        db = max(b0 - b, 0.0, b - b1)
        return 1.0 - smoothstep(0.0, self.plat_blend, math.hypot(da, db))

    def eval(self, x, z):
        """-> (h, dcoast, lava weight 0..1, crater floor 0/1)"""
        dcoast, theta = self.coast(x, z)
        lava_w = 0.0
        floor = 0
        if dcoast >= 0.0:
            sw = self.shelf_w * (0.55 + 0.9 * ang_fbm(theta, 7, 2, self.S["shelf"]))
            h = -self.coast_depth - (self.floor - self.coast_depth) * smoothstep(0.0, sw, dcoast)
        else:
            bw = self.beach_width(theta)
            dl = -dcoast - bw                       # inland distance past the beach zone
            if dl < 0.0:
                h = self.beach_min + 1.5 * (1.0 + dl / bw)  # flat beach: beach_min .. +1.5 m
            else:
                t = min(1.0, dl / self.plain_w)
                h = self.plain_h * (1.0 - (1.0 - t) ** 2)
                if bw > 0.0:
                    h += self.beach_min + 1.5
            dl = max(0.0, dl)
            lf = smoothstep(0.0, self.hill_fade, dl)
            if lf > 0.0:
                n = R.fbm(x / self.hill_l, z / self.hill_l, octaves=4, seed=self.S["hills"])
                h += self.hill_amp * smoothstep(0.38, 0.82, n) * lf
            cw = smoothstep(self.cliff_cos, 1.0, math.cos(wrap_pi(theta - self.cliff_az)))
            if cw > 0.0:
                ch = self.cliff_h * cw * (0.55 + 0.45 * ang_fbm(theta, 40, 2, self.S["cliff"]))
                h = max(h, ch * smoothstep(0.0, self.cliff_w, -dcoast))
            landfac = smoothstep(0.0, 900.0 * self.fs, dl)
            vx, vz = x - self.vcx, z - self.vcz
            rv = math.hypot(vx, vz)
            tv = math.atan2(vx, vz)
            cone = 0.0
            if rv < self.Rv:
                cone = self.h0 * (1.0 - rv / self.Rv) ** self.exp
                Rr = self.Rr
                env = smoothstep(Rr * 1.05, Rr * 2.4, rv) * (1.0 - smoothstep(self.Rv * 0.5, self.Rv * 0.97, rv))
                if env > 0.0:
                    wl = 2600.0 * self.fs
                    warp = (R.value_noise(x / wl, z / wl, self.S["warp"]) - 0.5) * 0.7
                    u = ((tv + warp) / TWO_PI) % 1.0 * self.ridge_k
                    rr = R.ridged(u, rv / self.ridge_l, octaves=3, seed=self.S["ridge"],
                                  period=(self.ridge_k, 1 << 20))
                    cone += self.ridge_amp * env * (rr - 0.5) * 2.0 * min(1.0, cone / (300.0 * self.fs))
                    e = R.fbm(x / self.eros_l, z / self.eros_l, octaves=3, seed=self.S["eros"])
                    cone += self.eros_amp * env * (e - 0.5) * 2.0 * min(1.0, cone / (200.0 * self.fs))
                rimw = smoothstep(Rr * 2.6, Rr, rv)
                if rv < Rr:
                    tt = rv / Rr
                    cone = self.summit - self.cdepth * (1.0 - smoothstep(self.cfloor, 1.0, tt))
                    if tt < self.cfloor * 1.05:
                        floor = 1
                if rimw > 0.0:
                    cone += self.rim_noise * (ang_fbm(tv, 5, 3, self.S["rim"]) - 0.5) * 2.0 * rimw
                # lava channels
                if rv < self.cfloor * Rr:
                    lava_w = 1.0
                for ci, (az, ln, wd, mea) in enumerate(self.lava):
                    if rv < Rr * 0.9 or rv > Rr + ln:
                        continue
                    q = (rv - Rr) / ln
                    tc = math.radians(az + mea * (R.value_noise_1d(rv / (700.0 * self.fs) + ci * 37.0,
                                                                    self.S["lava"] + ci) - 0.5) * 2.0)
                    across = abs(rv * wrap_pi(tv - tc))
                    w = wd * (0.6 + 0.9 * max(0.0, q))
                    wt = smoothstep(w, w * 0.45, across) * smoothstep(1.0, 0.85, q)
                    lava_w = max(lava_w, wt)
            for (sx, sz, sh, sr, scr) in self.sats:
                d = math.hypot(x - sx, z - sz)
                if d < sr:
                    hs = sh * (1.0 - d / sr) ** 1.5
                    if scr > 0.0 and d < scr:
                        hs -= sh * 0.22 * (1.0 - (d / scr) ** 2)
                    cone += hs
            cone += self.levee * lava_w
            h += cone * landfac
        for (ix, iz, ir, ih) in self.islets:      # offshore islets: dome above the local sea floor
            d = math.hypot(x - ix, z - iz)
            if d < ir:
                hi = ih - (ih + self.floor) * smoothstep(0.0, 1.0, d / ir) ** 0.8
                if hi > h:
                    h = hi
        w = self.plateau_w(x, z)
        if w > 0.0:
            h += (self.plat_h - h) * w
            lava_w *= (1.0 - w)
            if w > 0.5 and dcoast > -200.0:
                dcoast = min(dcoast, -200.0)
        return h, dcoast, lava_w, floor


class HeightField(object):
    pass


def quant(h):
    q = int(round((h - HMIN) / HSCALE))
    return 0 if q < 0 else (65535 if q > 65535 else q)


def build(src):
    m = Model(src)
    hf = HeightField()
    hf.model = m
    hf.frame = m.frame
    gn, cell = src.get("grid")
    N = int(gn)
    CELL = float(cell)
    if N < 65 or N > 513 or (N - 1) % 64:
        raise ValueError("grid: samples must be 64 k + 1 and <= 513 (e_terrain superMax limit), got %d" % N)
    X0 = Z0 = -0.5 * (N - 1) * CELL
    NC = (N - 1) // CHUNK
    hf.N, hf.cell, hf.x0, hf.z0, hf.hmin, hf.hscale = N, CELL, X0, Z0, HMIN, HSCALE
    hf.NC, hf.CHUNK = NC, CHUNK
    hf.notes = []
    H = [0.0] * (N * N)
    DC = [0.0] * (N * N)
    LV = [0.0] * (N * N)
    CF = [0] * (N * N)
    for j in range(N):
        z = Z0 + j * CELL
        for i in range(N):
            h, dc, lw, fl = m.eval(X0 + i * CELL, z)
            k = j * N + i
            H[k] = h
            DC[k] = dc
            LV[k] = lw
            CF[k] = fl
    # grid-border fade: the sea reaches the floor before the edge (stored chunks end in deep water)
    ef = src.g1("edge_fade")
    nedge = 0
    if ef > 0.0:
        for j in range(N):
            dj = min(j, N - 1 - j) * CELL
            for i in range(N):
                de = min(dj, min(i, N - 1 - i) * CELL)
                if de < ef:
                    k = j * N + i
                    t = smoothstep(0.0, ef, de)
                    if H[k] > 0.0:
                        nedge += 1
                    H[k] = -m.floor + (H[k] + m.floor) * t
    # floor clamp + coast rule
    nfix_w = nfix_l = 0
    for k in range(N * N):
        h = H[k]
        if h < -m.floor:
            h = -m.floor
        if h < 0.0:
            if h > -m.coast_depth:
                h = -m.coast_depth
                nfix_w += 1
        elif h < m.beach_min:
            h = m.beach_min
            nfix_l += 1
        H[k] = h
    # quantise
    Q = [quant(h) for h in H]
    Hf = [HMIN + q * HSCALE for q in Q]
    # LOD0 chunk-border snap (float), then HGHT = rounded
    nsnap = 0
    viol = 0
    for line in range(0, N, CHUNK):
        for t in range(1, N - 1, 2):
            # vertical line x = line: odd z
            k = t * N + line
            Hf[k] = 0.5 * (Hf[k - N] + Hf[k + N])
            # horizontal line z = line: odd x
            k2 = line * N + t
            Hf[k2] = 0.5 * (Hf[k2 - 1] + Hf[k2 + 1])
            nsnap += 2
    for line in range(0, N, CHUNK):
        for t in range(1, N - 1, 2):
            for k in (t * N + line, line * N + t):
                v = Hf[k]
                if -m.coast_depth < v < m.beach_min:
                    viol += 1
    Q = [quant(h) for h in Hf]
    hf.Q = Q
    hf.H = Hf
    hf.DC = DC
    hf.LV = LV
    hf.CF = CF
    hf.hlo = min(Hf)
    hf.hhi = max(Hf)
    kp = max(range(N * N), key=lambda k: (Hf[k], -k))
    hf.peak = (Hf[kp], X0 + (kp % N) * CELL, Z0 + (kp // N) * CELL)
    fl = [Hf[k] for k in range(N * N) if CF[k]]
    hf.crater_floor = min(fl) if fl else 0.0
    land = [k for k in range(N * N) if Hf[k] > 0.0]
    hf.land_samples = len(land)
    xs = [X0 + (k % N) * CELL for k in land]
    zs = [Z0 + (k // N) * CELL for k in land]
    cx = 0.5 * (min(xs) + max(xs))
    cz = 0.5 * (min(zs) + max(zs))
    rad = max(math.hypot(x - cx, z - cz) for x, z in zip(xs, zs)) + CELL
    hf.centre = (cx, cz, rad)
    hf.bbox = (min(xs), min(zs), max(xs), max(zs))
    hf.notes.append("coast rule: %d sea samples pushed to -%.1f m, %d land samples raised to %.1f m; after the "
                    "LOD0 border snap %d border samples lie in (-%.1f, %.1f) m" % (
                        nfix_w, m.coast_depth, nfix_l, m.beach_min, viol, m.coast_depth, m.beach_min))
    hf.notes.append("LOD0 border snap: %d samples on %.0f m chunk lines (HGHT = rounded, mesh = exact, <= 0.025 m)"
                    % (nsnap, CHUNK * CELL))
    if ef > 0.0:
        hf.notes.append("edge fade %.0f m: %d land samples lowered%s" % (ef, nedge, " (WARNING: land at the grid "
                                                                           "border)" if nedge else ""))
    hf.notes.append("island size (land bbox) %.1f x %.1f km" % ((max(xs) - min(xs)) / 1000.0,
                                                               (max(zs) - min(zs)) / 1000.0))
    # HMAX: (N-1)/8 squared blocks of 8 x 8 cells (inclusive border samples)
    HM = []
    nb = (N - 1) // 8
    hf.nblocks = nb
    for bz in range(nb):
        for bx in range(nb):
            mx = 0
            for j in range(bz * 8, bz * 8 + 9):
                row = j * N
                for i in range(bx * 8, bx * 8 + 9):
                    if Q[row + i] > mx:
                        mx = Q[row + i]
            HM.append(mx)
    hf.HMAX = HM
    return hf


def sample(hf, x, z):
    """Bilinear height of the float grid (preview / checks)."""
    N = hf.N
    fx = (x - hf.x0) / hf.cell
    fz = (z - hf.z0) / hf.cell
    if fx < 0 or fz < 0 or fx > N - 1 or fz > N - 1:
        return -60.0
    i = min(int(fx), N - 2)
    j = min(int(fz), N - 2)
    tx, tz = fx - i, fz - j
    H = hf.H
    a = H[j * N + i]
    b = H[j * N + i + 1]
    c = H[(j + 1) * N + i]
    d = H[(j + 1) * N + i + 1]
    return (a * (1 - tx) + b * tx) * (1 - tz) + (c * (1 - tx) + d * tx) * tz
