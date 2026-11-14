"""gen_sky.py - A2: open-sea afternoon sky panorama, light set, cloud textures and sun glow (Jet, DESIGN 8.4).

Outputs (see ASSETS):
  data/sky/sky_hi_0..7.bmp   High panorama 2048x256 as 8 tiles of 256x256 (24-bit, no key)
  data/sky/sky_lo_0..3.bmp   Low panorama 1024x128 as 4 tiles of 256x128 (BOX downsample of High)
  data/sky/light.txt         light set + panorama geometry (zenith/top/horizon/fog colours, sun direction and
                             colour, ambient, sky fill; read by the engine, islandc's bake and the jet lighting)
  data/tex/cloud_puffs.bmp + _a   2x2 atlas of 128 px cumulus puffs (camera-facing billboards, RB_BLEND, 4444)
  data/tex/cloud_deck.bmp + _a    tileable 256 broken cloud deck (6 km layer, High only, RB_BLEND, 4444)
  data/tex/sun_glow.bmp      128x128 premultiplied (black = nothing) glow for the additive sun billboard

Panorama mapping (same as the racer's world_sky_load, documented again in light.txt):
  u (0..1 across all tiles, tile 0 first) = compass azimuth 0..360 deg: u = 0 north (+Z), clockwise toward east
  (+X), like palette.dir_from_angles. v = 0 top = ELEV_HI, v = V_HORIZON = elevation 0, v = 1 = ELEV_LO; rows are
  linear in elevation inside each band. Top row = exactly "top"; every row below the horizon = exactly "fog".

Picture: open sea, bright afternoon, sun at azimuth 230 (SW) elevation 40. Deep blue zenith, paler toward the
horizon, a bright aureole around the sun, faint cirrus streaks, fields of distant fair-weather cumulus sitting on
the horizon (smaller and hazier toward it) with a few distant towers, and a horizon haze band that reaches exactly
the fog colour at elevation 0. Below 0 deg there is only fog colour: the fogged sea (fog end = far plane) meets the
panorama seamlessly at any altitude (the geometric horizon dips 2-3 deg at 3-10 km).

fog = a designed colour; the haze ramp forces every pixel at elevation <= 0 to it and fades it out by +6 deg, so the
horizon band average equals the fog colour by construction (measured and logged).

Run: python3 tools/build_assets.py --only sky   (or python3 tools/art/gen_sky.py [--check]).
Deterministic: pure Python math + Pillow BOX resizes, rng from assetkit only.
"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "tools"))
from PIL import Image, ImageDraw  # noqa: E402

from assetkit import image as I, palette as P, preview as V  # noqa: E402
from assetkit.rng import Rng, value_noise, value_noise_1d, fbm  # noqa: E402

SEED = 23
ASSETS = (["sky/sky_hi_%d.bmp" % i for i in range(8)] +
          ["sky/sky_lo_%d.bmp" % i for i in range(4)] +
          ["sky/light.txt",
           "tex/cloud_puffs.bmp", "tex/cloud_puffs_a.bmp",
           "tex/cloud_deck.bmp", "tex/cloud_deck_a.bmp",
           "tex/sun_glow.bmp"])
ORDER = 20                        # before the island bake / anything that reads light.txt

W, H = 2048, 256                  # High panorama
V_HORIZON = 0.9375                # = row 240 of 256 (row 120 of the Low 128): the band below is uniform fog
ELEV_LO, ELEV_HI = -10.0, 55.0    # DESIGN 8.4 says -10..+40; +55 so the sun (40) and its aureole fit (see HANDOFF)
SUN_AZ, SUN_EL = 230.0, 40.0      # compass azimuth (SW), elevation in degrees
SUN_DISC = 1.35                   # painted disc radius (deg; stylised, ~12 px on a 640 px 70 deg view)
BAND = (-0.75, 0.25)              # horizon band for the fog check (degrees)
HAZE_TOP = 6.0                    # horizon haze fades out by this elevation

# light set
FOG = (196, 212, 228)             # table-fog colour = horizon haze (sea and island fade into the panorama)
ZENITH = (40, 86, 172)            # straight up (zenith cap: TOP at ELEV_HI -> ZENITH at 90 deg)
TOP = (50, 98, 182)               # uniform top row (elevation ELEV_HI)
SUN_COL = (255, 244, 226)         # direct sunlight, bright early afternoon
AMBIENT = (132, 150, 178)         # sky light for baked lighting (islandc) and the jet's ambient
SKYFILL = (116, 150, 204)         # jet lighting sky fill: average upper-sky colour, slightly dimmed
HEMI = 0.35

# clear-sky gradient stops (elevation deg, colour): toward the sun and away from it; mixed by azimuth
STOPS_SUN = [(0, (222, 228, 232)), (2, (214, 224, 234)), (5, (192, 210, 232)), (10, (160, 190, 228)),
             (18, (128, 166, 220)), (28, (104, 146, 212)), (40, (82, 128, 202)), (48, (64, 112, 192)),
             (55, TOP)]
STOPS_ANTI = [(0, (184, 204, 228)), (2, (174, 198, 228)), (5, (152, 184, 226)), (10, (122, 164, 220)),
              (18, (94, 142, 210)), (28, (74, 122, 200)), (40, (60, 108, 190)), (48, (54, 102, 185)),
              (55, TOP)]
GLOW_COL = (255, 252, 240)
DISC_COL = (255, 254, 246)

AZ_LABELS = [(0, "N"), (45, "NE"), (90, "E"), (135, "SE"), (180, "S"), (225, "SW"), (270, "W"), (315, "NW")]


# ------------------------------------------------------------------------------------------------ helpers

def clamp01(v):
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


def smoothstep(a, b, x):
    if a == b:
        return 1.0 if x >= b else 0.0
    t = clamp01((x - a) / (b - a))
    return t * t * (3 - 2 * t)


def mix(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def scale(c, f):
    return (c[0] * f, c[1] * f, c[2] * f)


def adiff(a, b):
    """Signed smallest azimuth difference a - b in (-180, 180]."""
    d = (a - b) % 360.0
    return d - 360.0 if d > 180.0 else d


def elev_of_y(y, h=H):
    """Elevation (deg) at continuous row coordinate y (0 = top edge, h = bottom edge)."""
    v = y / float(h)
    if v <= V_HORIZON:
        return ELEV_HI * (1.0 - v / V_HORIZON)
    return ELEV_LO * (v - V_HORIZON) / (1.0 - V_HORIZON)


def y_of_elev(e, h=H):
    """Continuous row coordinate of an elevation (inverse of elev_of_y)."""
    if e >= 0:
        v = V_HORIZON * (1.0 - e / ELEV_HI)
    else:
        v = V_HORIZON + (1.0 - V_HORIZON) * (e / ELEV_LO)
    return v * h


def az_of_x(x, w=W):
    return (x + 0.5) / w * 360.0


def catmull(stops, e):
    """Smooth (Catmull-Rom) interpolation of colour stops by elevation, monotone between stops."""
    n = len(stops)
    if e <= stops[0][0]:
        return stops[0][1]
    if e >= stops[-1][0]:
        return stops[-1][1]
    i = 0
    while stops[i + 1][0] < e:
        i += 1
    e0, e1 = stops[i][0], stops[i + 1][0]
    t = (e - e0) / (e1 - e0)
    p0 = stops[max(i - 1, 0)][1]
    p1 = stops[i][1]
    p2 = stops[i + 1][1]
    p3 = stops[min(i + 2, n - 1)][1]
    t2, t3 = t * t, t * t * t
    out = []
    for k in range(3):
        v = 0.5 * ((2 * p1[k]) + (-p0[k] + p2[k]) * t + (2 * p0[k] - 5 * p1[k] + 4 * p2[k] - p3[k]) * t2 +
                   (-p0[k] + 3 * p1[k] - 3 * p2[k] + p3[k]) * t3)
        lo, hi = min(p1[k], p2[k]), max(p1[k], p2[k])
        out.append(lo if v < lo else (hi if v > hi else v))
    return tuple(out)


def fbm1d(az, cells, octaves, seed, gain=0.5):
    """Periodic 1D fBm in [0, 1] over azimuth (cells per 360 deg at the first octave)."""
    x = az / 360.0 * cells
    tot, norm, amp, per = 0.0, 0.0, 1.0, cells
    for o in range(octaves):
        tot += amp * value_noise_1d(x, seed + 97 * o, per)
        norm += amp
        amp *= gain
        x *= 2.0
        per *= 2
    return tot / norm


def fl_of(az):
    """0 looking toward the sun's azimuth, 1 looking away."""
    return 0.5 - 0.5 * math.cos(math.radians(adiff(az, SUN_AZ)))


def sun_angle(az, e):
    """Great-circle angle (deg) between direction (az, e) and the sun."""
    a = math.radians(e)
    b = math.radians(SUN_EL)
    c = math.cos(math.radians(az - SUN_AZ))
    v = math.sin(a) * math.sin(b) + math.cos(a) * math.cos(b) * c
    v = 1.0 if v > 1 else (-1.0 if v < -1 else v)
    return math.degrees(math.acos(v))


def top_blend(e):
    return smoothstep(46.5, 54.2, e)


def haze_weight(e):
    """Horizon haze toward the fog colour: exactly 1 at e <= 0, 0 from HAZE_TOP up."""
    if e <= 0.0:
        return 1.0
    return (1.0 - smoothstep(0.0, HAZE_TOP, e)) ** 1.35


# ------------------------------------------------------------------------------------------------ sky

def sky_clear(az, e, fl=None):
    """Clear sky colour (gradient + sun aureole, no clouds, no disc, no horizon haze) at azimuth/elevation."""
    if fl is None:
        fl = fl_of(az)
    s = catmull(STOPS_SUN, e)
    a = catmull(STOPS_ANTI, e)
    c = mix(s, a, fl ** 0.9)
    g = sun_angle(az, e)
    glow = 0.55 * math.exp(-g / 2.0) + 0.30 * math.exp(-g / 7.5) + 0.12 * math.exp(-g / 24.0)
    if glow > 1.0:
        glow = 1.0
    return mix(c, GLOW_COL, glow)


def paint_sky(R, G, B):
    """Gradient + aureole + faint cirrus streaks into the float buffers (rows above the horizon)."""
    fls = [fl_of(az_of_x(x)) for x in range(W)]
    y_hor = int(round(V_HORIZON * H))
    cseed = SEED + 301
    for y in range(y_hor):
        e = elev_of_y(y + 0.5)
        cband = smoothstep(9.0, 15.0, e) * (1.0 - smoothstep(34.0, 42.0, e))
        o = y * W
        for x in range(W):
            az = az_of_x(x)
            c = sky_clear(az, e, fls[x])
            if cband > 0.0:
                # thin cirrus: long streaks (stretched in azimuth), periodic over 360 deg
                n = fbm(x / float(W) * 12.0, e * 0.55, octaves=4, gain=0.55, seed=cseed, period=(12, 4096))
                fib = value_noise(x / float(W) * 96.0, e * 3.2, seed=cseed + 7, period=(96, 4096))
                d = smoothstep(0.60, 0.82, n) * cband * (0.55 + 0.45 * fib)
                if d > 0.0:
                    c = mix(c, (250, 252, 255), 0.42 * d)
            R[o + x], G[o + x], B[o + x] = c


# ------------------------------------------------------------------------------------------------ cloud banks

def make_banks():
    """Distant fair-weather cumulus on the horizon: list of clouds (far first). Each cloud is
    (cx_az, base_e, height, blobs[(dx_deg, cy_deg, r_deg)]) with blob coords relative to (cx_az, base_e)."""
    r = Rng(SEED, "banks")
    clouds = []
    n_try = 520
    for i in range(n_try):
        az = r.range(0.0, 360.0)
        t = r.random()
        eb = 0.10 + 3.6 * t ** 2.4                       # base elevation: many far (low), few near (higher)
        dens = 0.18 + 0.82 * smoothstep(0.30, 0.72, fbm1d(az, 7, 3, SEED + 11))
        if r.random() > dens:
            continue
        hc = (0.18 + 0.55 * eb) * r.range(0.6, 1.25)     # apparent height grows with nearness
        wc = hc * r.range(2.0, 4.2)
        clouds.append((az, eb, hc, wc, r.child("c", i)))
    # a few distant towers (cumulus congestus) for scale and interest; none right under the sun
    for k, (az, eb, hc) in enumerate(((22.0, 0.35, 2.6), (118.0, 0.55, 3.4), (164.0, 0.28, 2.1), (305.0, 0.45, 2.9),
                                      (338.0, 0.30, 1.8))):
        clouds.append((az, eb, hc, hc * 1.25, Rng(SEED, "tower", k)))
    out = []
    for az, eb, hc, wc, cr in clouds:
        blobs = []
        tower = wc < hc * 1.5
        nb = max(2, int(wc / hc * 1.7))
        for j in range(nb):                              # base row of domes
            fx = (j + 0.5) / nb - 0.5
            rr = hc * cr.range(0.32, 0.52) * (1.0 - 0.55 * abs(fx) * 2.0 * (0.6 if tower else 1.0))
            dx = fx * (wc - rr * 1.2) + cr.range(-0.12, 0.12) * hc
            blobs.append((dx, rr * cr.range(0.25, 0.6), rr))
        nt = cr.int(1, 3) if not tower else 5
        for j in range(nt):                              # upper domes
            fy = (j + 1.0) / (nt + 0.6)
            rr = hc * (cr.range(0.26, 0.40) if not tower else 0.36 - 0.17 * fy)
            dx = cr.range(-0.28, 0.28) * wc * (1.0 - fy * 0.6)
            cy = hc * (0.35 + 0.45 * fy) if not tower else hc * (0.15 + 0.82 * fy) - rr * 0.4
            blobs.append((dx, cy, rr))
        # small cauliflower lobes on the upper outline
        big = list(blobs)
        for bx, by, br in big:
            for q in range(3):
                ang = math.radians(cr.range(25.0, 155.0))
                sr = br * cr.range(0.32, 0.48)
                blobs.append((bx + math.cos(ang) * br * 0.78, by + math.sin(ang) * br * 0.78, sr))
        out.append((az, eb, hc, wc, blobs))
    out.sort(key=lambda c: (c[1], c[0]))
    return out


def paint_banks(R, G, B, banks):
    """Composite the distant cumulus over the sky (pre-haze; the horizon haze is applied afterwards)."""
    sx = W / 360.0                                       # px per deg
    for az, eb, hc, wc, blobs in banks:
        span = wc * 0.5 + hc * 0.6
        x0 = int(math.floor((az - span) * sx)) - 1
        x1 = int(math.ceil((az + span) * sx)) + 1
        etop = eb + max(b[1] + b[2] for b in blobs) + 0.05
        y0 = max(0, int(math.floor(y_of_elev(etop))) - 1)
        y1 = min(H, int(math.ceil(y_of_elev(eb - 0.3))) + 1)
        fl = fl_of(az)
        hz = mix(sky_clear(az, eb + hc * 0.4, fl), FOG, 0.35)
        haze = 0.14 + 0.62 * (1.0 - smoothstep(0.08, 3.8, eb))
        lit_top = mix((236, 238, 242), (255, 255, 253), fl)         # front-lit tops whiter
        shade = mix((150, 162, 182), (176, 188, 206), fl)           # flat grey-blue bases
        pxh = (ELEV_HI / (V_HORIZON * H))                           # deg per row (above the horizon)
        pxw = 1.0 / sx
        for xi in range(x0, x1 + 1):
            x = xi % W
            a_px = (xi + 0.5) / sx
            dx0 = a_px - az
            if dx0 > 180.0:
                dx0 -= 360.0
            elif dx0 < -180.0:
                dx0 += 360.0
            dxs = [dx0 + o * pxw for o in (-0.375, -0.125, 0.125, 0.375)]
            for y in range(y0, y1):
                e = elev_of_y(y + 0.5)
                rel = e - eb
                cov = 0.0
                for dx in dxs:                          # 4 sub-columns: horizontal AA
                    hit = 0.0
                    for bx, by, br in blobs:
                        ddx = dx - bx
                        ddy = rel - by
                        edge = (br - math.sqrt(ddx * ddx + ddy * ddy)) / pxh + 0.5
                        if edge > hit:
                            hit = edge if edge < 1.0 else 1.0
                    cov += hit
                cov *= 0.25
                best = -1e9
                bn = None
                for bx, by, br in blobs:                # front-most blob surface at the column centre: normal
                    ddx = dx0 - bx
                    ddy = rel - by
                    d2 = ddx * ddx + ddy * ddy
                    if d2 < br * br:
                        s = math.sqrt(1.0 - d2 / (br * br))
                        zz = by * 0.3 + br * s
                        if zz > best:
                            best = zz
                            bn = (ddx / br, ddy / br, s)
                cov *= smoothstep(-0.5 * pxh, 0.5 * pxh, rel)            # flat base, 1 row soft
                if cov <= 0.002:
                    continue
                vpos = clamp01(rel / max(hc, 0.05))
                if bn is None:
                    bn = (0.0, 0.5, 0.7)
                diff = clamp01((bn[0] * 0.15 + bn[1] * 0.80 + bn[2] * 0.55 + 0.30) / 1.30)
                occl = 0.80 + 0.20 * smoothstep(0.0, 0.55, bn[2])
                b = clamp01((0.30 + 0.70 * diff) * occl * (0.70 + 0.30 * smoothstep(0.0, 0.7, vpos)))
                c = mix(shade, lit_top, b)
                c = mix(c, hz, haze)
                i = y * W + x
                R[i] += (c[0] - R[i]) * cov
                G[i] += (c[1] - G[i]) * cov
                B[i] += (c[2] - B[i]) * cov


def paint_disc(R, G, B):
    """Sun disc with a soft edge (the aureole is part of sky_clear)."""
    sx = W / 360.0
    span = (SUN_DISC + 0.5) / math.cos(math.radians(SUN_EL))
    x0 = int(math.floor((SUN_AZ - span) * sx))
    x1 = int(math.ceil((SUN_AZ + span) * sx))
    y0 = int(math.floor(y_of_elev(SUN_EL + SUN_DISC + 0.5)))
    y1 = int(math.ceil(y_of_elev(SUN_EL - SUN_DISC - 0.5)))
    for y in range(y0, y1 + 1):
        for xi in range(x0, x1 + 1):
            # 4x4 supersampled coverage of the disc
            cov = 0.0
            for sy in range(4):
                e = elev_of_y(y + (sy + 0.5) / 4.0)
                for sxx in range(4):
                    az = (xi + (sxx + 0.5) / 4.0) / sx
                    if sun_angle(az, e) < SUN_DISC:
                        cov += 1.0
            cov /= 16.0
            if cov <= 0.0:
                continue
            i = y * W + (xi % W)
            R[i] += (DISC_COL[0] - R[i]) * cov
            G[i] += (DISC_COL[1] - G[i]) * cov
            B[i] += (DISC_COL[2] - B[i]) * cov


def apply_haze_and_caps(R, G, B):
    """Horizon haze toward FOG (exact at e <= 0), uniform TOP at the top. Returns nothing (in place)."""
    for y in range(H):
        e = elev_of_y(y + 0.5)
        o = y * W
        if e <= 0.0:
            for x in range(W):
                R[o + x], G[o + x], B[o + x] = FOG
            continue
        m = haze_weight(e)
        tb = top_blend(e)
        if m <= 0.0 and tb <= 0.0:
            continue
        for x in range(W):
            i = o + x
            if m > 0.0:
                R[i] += (FOG[0] - R[i]) * m
                G[i] += (FOG[1] - G[i]) * m
                B[i] += (FOG[2] - B[i]) * m
            if tb > 0.0:
                R[i] += (TOP[0] - R[i]) * tb
                G[i] += (TOP[1] - G[i]) * tb
                B[i] += (TOP[2] - B[i]) * tb
    for x in range(W):
        R[x], G[x], B[x] = TOP


def make_panorama():
    R = [0.0] * (W * H)
    G = [0.0] * (W * H)
    B = [0.0] * (W * H)
    paint_sky(R, G, B)
    paint_banks(R, G, B, make_banks())
    paint_disc(R, G, B)
    apply_haze_and_caps(R, G, B)
    img = Image.new("RGB", (W, H))
    img.putdata([(P.clamp8(R[i]), P.clamp8(G[i]), P.clamp8(B[i])) for i in range(W * H)])
    return img


def make_low(img):
    low = img.resize((W // 2, H // 2), Image.BOX)
    px = low.load()
    lw, lh = low.size
    for x in range(lw):
        px[x, 0] = TOP
    for y in range(lh):                                  # rows below the horizon: exactly fog (no float residue)
        if elev_of_y(y + 0.5, lh) <= 0.0:
            for x in range(lw):
                px[x, y] = FOG
    return low


def measure(img):
    """Numbers for light.txt, the log and the report."""
    px = img.load()
    w, h = img.size
    band = [y for y in range(h) if BAND[0] <= elev_of_y(y + 0.5, h) <= BAND[1]]
    tot = [0.0, 0.0, 0.0]
    colavg = []
    for x in range(w):
        c = [0.0, 0.0, 0.0]
        for y in band:
            p = px[x, y]
            for k in range(3):
                c[k] += p[k]
        c = [v / len(band) for v in c]
        colavg.append(c)
        for k in range(3):
            tot[k] += c[k]
    avg = tuple(int(round(v / w)) for v in tot)
    dev = max(abs(P.luma(c) - P.luma(avg)) for c in colavg)
    hor = [0.0, 0.0, 0.0]
    for x in range(w):
        p = sky_clear(az_of_x(x, w), 2.0)
        for k in range(3):
            hor[k] += p[k]
    horizon = tuple(int(round(v / w)) for v in hor)
    # brightest column above the horizon row (what the engine's sun_check measures)
    hy = int(V_HORIZON * h)
    best, bx = -1.0, 0
    for x in range(w):
        s = 0.0
        for y in range(hy):
            p = px[x, y]
            s += p[0] * 3 + p[1] * 6 + p[2]
        if s > best:
            best, bx = s, x
    fog_rows = [y for y in range(h) if all(px[x, y] == FOG for x in range(w))]
    first_fog = min(fog_rows) if fog_rows else h
    bottom_ok = bool(fog_rows) and fog_rows == list(range(first_fog, h))
    top_ok = all(px[x, 0] == TOP for x in range(w))
    seam = sum(abs(px[0, y][k] - px[w - 1, y][k]) for y in range(h) for k in range(3)) / (3.0 * h)
    adj = sum(abs(px[x, y][k] - px[x + 1, y][k]) for x in range(0, w - 1, 7) for y in range(h)
              for k in range(3)) / (3.0 * h * len(range(0, w - 1, 7)))
    return {"band_avg": avg, "band_dev_luma": dev, "horizon": horizon, "bright_az": az_of_x(bx, w),
            "seam": seam, "adjacent": adj, "colavg": colavg, "fog_rows": len(fog_rows), "first_fog": first_fog,
            "bottom_ok": bottom_ok, "top_ok": top_ok}


def light_txt(m):
    sd = P.dir_from_angles(SUN_AZ, SUN_EL)
    c3 = lambda c: "%d %d %d" % tuple(c)  # noqa: E731
    lines = [
        "# light.txt - Jet light set + panorama geometry (open sea, bright afternoon, green island).",
        "# GENERATED by tools/art/gen_sky.py (A2): edit the generator, not this file. Key names are stable.",
        "#",
        "# Format: one 'key value...' per line, '#' starts a comment, blank lines ignored, CRLF. Colours are 'r g b'",
        "# (0..255; pack as 0x00RRGGBB). Angles in degrees. Unknown keys: ignore (forward compatible). Keys whose",
        "# value is a word (set, tile_prefix, tile_prefix_low) are skipped by a numeric sscanf parser.",
        "#",
        "# Panorama files: data/sky/<tile_prefix>_<i>.bmp, i = 0..tiles-1 (High, 256x256 each, together 2048x256)",
        "# and data/sky/<tile_prefix_low>_<i>.bmp, i = 0..tiles_low-1 (Low, 256x128 each, together 1024x128).",
        "# 24-bit BMP, no key. Mapping: u (0..1 across all tiles, tile 0 first) = compass azimuth 0..360: u = 0",
        "# north (+Z), increasing clockwise toward east (+X) (= palette.dir_from_angles). v = 0 top = elevHi,",
        "# v = vHorizon = elevation 0, v = 1 bottom = elevLo; rows are linear in elevation inside each band.",
        "# Draw with clamp addressing per tile, no mips. The top row is exactly 'top' everywhere (zenith cap: 'top' at",
        "# elevHi -> 'zenith' at 90 deg); every row below the horizon (elevation < 0) is exactly 'fog' (lower band:",
        "# fog to the nadir). The sun disc (radius sun_disc deg) and its aureole are painted at sun_azim/sun_elev;",
        "# the brightest column above the horizon row is the sun azimuth (engine sun_check).",
        "#",
        "# Keys -> WSky: nTiles = tiles, vHorizon, elevLo, elevHi, zenith, horizon, fog, sunDir, sun, ambient,",
        "# fogStart, fogEnd. Extra keys: top, skyfill, hemi, sun_azim, sun_elev, sun_disc, tiles_low, width/height,",
        "# tile_prefix(_low), cloud_* (texture names + suggested use, DESIGN 8.4).",
        "#   zenith   sky colour straight up (cap target)",
        "#   top      the panorama's uniform top-row colour (elevation elevHi)",
        "#   horizon  average clear-sky colour at +2 deg before the haze: placeholder dome, env/reflection tint",
        "#   fog      table-fog colour = the horizon haze: every panorama row below 0 deg and the average of the",
        "#            horizon band (-0.75..+0.25 deg); fogged sea and island at the far plane match the panorama",
        "#   sun      direct sunlight colour; sunDir = unit vector TOWARD the sun (x east, y up, z north)",
        "#   ambient  sky light for baked lighting (islandc) and the jet; skyfill = jet sky fill from above;",
        "#            hemi = hemisphere factor (light = ambient*(1-hemi+hemi*(n.y*0.5+0.5)) + sun*max(0,n.L))",
        "#   fogStart/fogEnd  Low preset defaults in metres (DESIGN 9.1/9.3: presets override)",
        "",
        "set             light",
        "tile_prefix     sky_hi",
        "tile_prefix_low sky_lo",
        "tiles           8",
        "tiles_low       4",
        "width           %d" % W,
        "height          %d" % H,
        "vHorizon        %.6f" % V_HORIZON,
        "elevLo          %.1f" % ELEV_LO,
        "elevHi          %.1f" % ELEV_HI,
        "zenith          %s" % c3(ZENITH),
        "top             %s" % c3(TOP),
        "horizon         %s" % c3(m["horizon"]),
        "fog             %s" % c3(FOG),
        "sun             %s" % c3(SUN_COL),
        "ambient         %s" % c3(AMBIENT),
        "skyfill         %s" % c3(SKYFILL),
        "hemi            %.2f" % HEMI,
        "sun_azim        %.1f" % SUN_AZ,
        "sun_elev        %.1f" % SUN_EL,
        "sun_disc        %.2f" % SUN_DISC,
        "sunDir          %.6f %.6f %.6f" % sd,
        "fogStart        3600.0",
        "fogEnd          18000.0",
        "",
        "# clouds (DESIGN 8.4): cloud_puffs = 2x2 atlas of 128 px cumulus (cell i at x = (i % 2) * 128,",
        "# y = (i / 2) * 128), camera-facing billboards, RB_BLEND, alpha from cloud_puffs_a; puff sizes in metres",
        "# (min max) and altitude band. cloud_deck = tileable 256 (RB_BLEND, _a), deck_tile_m metres per repeat.",
        "# puff_tint/deck_tint = suggested vertex colours (the textures are lit white; fog does the distance).",
        "cloud_puff_size 450 1100",
        "cloud_alt       1800 4000",
        "deck_alt        6000",
        "deck_tile_m     3000",
        "puff_tint       %s" % c3((255, 255, 255)),
        "deck_tint       %s" % c3((246, 248, 252)),
        "",
    ]
    return "\n".join(lines)


def tiles_of(img, n):
    w, h = img.size
    tw = w // n
    return [img.crop((i * tw, 0, (i + 1) * tw, h)) for i in range(n)]


# ------------------------------------------------------------------------------------------------ cloud puffs

PUFF_KINDS = ("wide", "tower", "flat", "round")


def puff_blobs(i):
    """Blob list (cx, cy, r, z) in cell units (u, v in [-1, 1], v up) and the flat base height."""
    r = Rng(SEED, "puff", i)
    kind = PUFF_KINDS[i]
    blobs = []
    if kind == "wide":
        base = -0.42
        n = 5
        for j in range(n):
            fx = (j + 0.5) / n - 0.5
            rr = r.range(0.24, 0.32) * (1.0 - 0.45 * abs(fx) * 2.0)
            blobs.append((fx * 1.30, base + rr * r.range(0.55, 0.85), rr, r.range(-0.1, 0.1)))
        for j in range(3):
            rr = r.range(0.22, 0.30)
            blobs.append((r.range(-0.38, 0.38), base + 0.30 + r.range(0.0, 0.22), rr, r.range(0.0, 0.15)))
    elif kind == "tower":
        base = -0.62
        for j in range(4):
            fx = (j + 0.5) / 4 - 0.5
            rr = r.range(0.20, 0.27)
            blobs.append((fx * 0.95, base + rr * 0.7, rr, r.range(-0.1, 0.1)))
        y = base + 0.30
        for j in range(5):
            rr = 0.31 - 0.035 * j + r.range(-0.03, 0.03)
            blobs.append((r.range(-0.16, 0.16), y + rr * 0.5, rr, r.range(0.0, 0.15)))
            y += rr * 0.62
    elif kind == "flat":
        base = -0.18
        n = 7
        for j in range(n):
            fx = (j + 0.5) / n - 0.5
            rr = r.range(0.14, 0.22) * (1.0 - 0.4 * abs(fx) * 2.0)
            blobs.append((fx * 1.55 + r.range(-0.04, 0.04), base + rr * r.range(0.45, 0.8), rr, r.range(-0.1, 0.1)))
        for j in range(3):
            blobs.append((r.range(-0.45, 0.45), base + r.range(0.18, 0.30), r.range(0.13, 0.19), 0.1))
    else:  # round
        base = -0.40
        blobs.append((0.0, base + 0.36, 0.36, 0.0))
        for j in range(6):
            ang = math.radians(15.0 + j * 30.0 + r.range(-8.0, 8.0))
            rr = r.range(0.20, 0.28)
            blobs.append((math.cos(ang) * 0.36, base + 0.30 + math.sin(ang) * 0.30, rr, r.range(-0.05, 0.12)))
    # cauliflower lobes on the upper outline of every big blob
    big = list(blobs)
    for bx, by, br, bz in big:
        for q in range(4):
            ang = math.radians(r.range(10.0, 170.0))
            sr = br * r.range(0.30, 0.45)
            rad = br * r.range(0.70, 0.86)
            blobs.append((bx + math.cos(ang) * rad, by + math.sin(ang) * rad, sr, bz + br * 0.3))
    return blobs, base


def puff_rgba(i):
    """One 128 x 128 cumulus puff: lit white tops, grey-blue flat base, soft billowy edge, alpha 0 at the border."""
    n = 128
    blobs, base = puff_blobs(i)
    L = (0.30, 0.80, 0.52)
    ll = math.sqrt(sum(c * c for c in L))
    L = (L[0] / ll, L[1] / ll, L[2] / ll)
    nseed = SEED * 13 + i
    lit = (255, 255, 254)
    mid = (222, 228, 238)
    shade = (150, 162, 184)
    pix = []
    for y in range(n):
        v = 1.0 - (y + 0.5) / (n * 0.5)
        for x in range(n):
            u = (x + 0.5) / (n * 0.5) - 1.0
            nz = fbm(u * 4.0 + 7.0, v * 4.0 + 7.0, octaves=3, seed=nseed)
            wob = 1.0 + 0.22 * (nz - 0.5)
            hit = 0.0
            cand = []
            for bx, by, br, bz in blobs:
                du = u - bx
                dv = v - by
                d = math.sqrt(du * du + dv * dv) * wob
                edge = (br - d) * 64.0 / 1.4 + 0.5          # ~1.4 px soft edge
                if edge > hit:
                    hit = edge if edge < 1.0 else 1.0
                if d < br:
                    s = math.sqrt(1.0 - (d * d) / (br * br))
                    cand.append((bz + br * s, du / br, dv / br, s))
            bn = None
            if cand:
                # soft-max blend of the lobes near the front: smooth creases instead of hard seams
                zmax = max(c[0] for c in cand)
                sw = sx = sy = sz = 0.0
                for zz, nx, ny, nzz in cand:
                    wgt = math.exp((zz - zmax) * 18.0)
                    sw += wgt
                    sx += nx * wgt
                    sy += ny * wgt
                    sz += nzz * wgt
                # small billow bumps from the noise gradient
                gx = fbm(u * 10.0 + 0.05, v * 10.0, octaves=2, seed=nseed + 5) - \
                    fbm(u * 10.0 - 0.05, v * 10.0, octaves=2, seed=nseed + 5)
                gy = fbm(u * 10.0, v * 10.0 + 0.05, octaves=2, seed=nseed + 5) - \
                    fbm(u * 10.0, v * 10.0 - 0.05, octaves=2, seed=nseed + 5)
                bx_, by_, bz_ = sx / sw - gx * 2.2, sy / sw - gy * 2.2, sz / sw
                ln = math.sqrt(bx_ * bx_ + by_ * by_ + bz_ * bz_) or 1.0
                bn = (bx_ / ln, by_ / ln, bz_ / ln)
            a = hit * smoothstep(base - 0.02, base + 0.06, v)        # flat base
            # zero at the cell border (bilinear / mips never bleed into the neighbour cell)
            a *= clamp01((0.955 - max(abs(u), abs(v))) / 0.05)
            if a <= 0.0 or bn is None:
                pix.append((int(shade[0]), int(shade[1]), int(shade[2]), 0))
                continue
            diff = clamp01((bn[0] * L[0] + bn[1] * L[1] + bn[2] * L[2] + 0.35) / 1.35)
            occl = 0.84 + 0.16 * smoothstep(0.0, 0.55, bn[2])        # soft creases between lobes
            hv = smoothstep(base, base + 0.85, v)                    # higher = brighter (self-shadowing)
            tex = 0.94 + 0.12 * fbm(u * 9.0, v * 9.0, octaves=2, seed=nseed + 3)
            b = clamp01((0.18 + 0.82 * diff) * occl * (0.62 + 0.38 * hv) * tex)
            c = mix(shade, mid, smoothstep(0.0, 0.6, b))
            c = mix(c, lit, smoothstep(0.55, 1.0, b))
            # translucent thin edge: lighter, and the alpha thins out with billow noise
            rim = 1.0 - smoothstep(0.0, 0.9, hit - 0.1) if hit < 1.0 else 0.0
            a *= 0.85 + 0.15 * (1.0 - rim)
            a = clamp01(a)
            pix.append((P.clamp8(c[0]), P.clamp8(c[1]), P.clamp8(c[2]), P.clamp8(a * 255.0)))
    img = Image.new("RGBA", (n, n))
    img.putdata(pix)
    return img


def puffs_atlas():
    out = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    cells = []
    for i in range(4):
        c = puff_rgba(i)
        cells.append(c)
        out.paste(c, ((i % 2) * 128, (i // 2) * 128))
    return out, cells


# ------------------------------------------------------------------------------------------------ cloud deck

def deck_rgba():
    """Tileable 256 broken stratocumulus/altocumulus deck (periodic noise only: tiles exactly)."""
    n = 256
    seed = SEED + 500
    dens = []
    for y in range(n):
        for x in range(n):
            u = x / float(n)
            v = y / float(n)
            wx = fbm(u * 4.0, v * 4.0, octaves=3, seed=seed + 1, period=4) - 0.5
            wy = fbm(u * 4.0 + 17.0, v * 4.0 + 5.0, octaves=3, seed=seed + 2, period=4) - 0.5
            big = fbm(u * 4.0 + wx * 0.5, v * 4.0 + wy * 0.5, octaves=3, gain=0.5, seed=seed, period=4)
            cel = fbm(u * 12.0 + wx * 1.5, v * 12.0 + wy * 1.5, octaves=3, gain=0.45, seed=seed + 3, period=12)
            dens.append(0.60 * big + 0.40 * cel)
    pix = []
    lit = (255, 255, 255)
    core = (200, 208, 222)
    for y in range(n):
        for x in range(n):
            d = dens[y * n + x]
            a = smoothstep(0.43, 0.60, d)
            # relief: lighter on the side toward the sun (texture up-left), darker on the far side
            d2 = dens[((y + 2) % n) * n + (x + 2) % n]
            rel = clamp01(0.5 + (d - d2) * 6.0)
            t = smoothstep(0.52, 0.80, d)
            c = mix(lit, core, 0.65 * t)
            c = scale(c, 0.90 + 0.12 * rel)
            pix.append((P.clamp8(c[0]), P.clamp8(c[1]), P.clamp8(c[2]), P.clamp8(a * 0.92 * 255.0)))
    img = Image.new("RGBA", (n, n))
    img.putdata(pix)
    return img


# ------------------------------------------------------------------------------------------------ sun glow

def glow_rgb():
    """128 x 128 premultiplied glow for RB_ADD: bright core, soft halo, faint 6-ray star, black border."""
    n = 128
    pix = []
    for y in range(n):
        for x in range(n):
            dx = ((x + 0.5) / n) * 2.0 - 1.0
            dy = ((y + 0.5) / n) * 2.0 - 1.0
            d = math.hypot(dx, dy)
            if d >= 0.97:
                pix.append((0, 0, 0))
                continue
            e = clamp01(1.0 - d / 0.97)
            i = 0.70 * math.exp(-(d / 0.09) ** 2) + 0.42 * e ** 2.6 + 0.08 * e
            ang = math.atan2(dy, dx) + 0.26
            i += 0.08 * (0.5 + 0.5 * math.cos(ang * 6.0)) ** 10 * e ** 2.5
            i = clamp01(i) * 255.0
            pix.append((P.clamp8(i), P.clamp8(i * 0.975), P.clamp8(i * 0.93)))
    img = Image.new("RGB", (n, n))
    img.putdata(pix)
    return img


# ------------------------------------------------------------------------------------------------ previews

def swatch_row(entries, w, h=40):
    """Labelled colour swatches in a row."""
    out = Image.new("RGB", (w, h + 28), (24, 24, 28))
    sw = w // max(1, len(entries))
    for k, (name, c) in enumerate(entries):
        out.paste(Image.new("RGB", (sw - 4, h), tuple(int(round(v)) for v in c)), (k * sw + 2, 27))
        V.label(out, name, xy=(k * sw + 3, 1), color=(230, 230, 230), size=11)
        V.label(out, "%d,%d,%d" % tuple(int(round(v)) for v in c), xy=(k * sw + 3, 13), color=(230, 230, 230),
                size=11)
    return out


def preview_strip(img, low, m):
    w, h = img.size
    q = I.to16(img, "565", dither=True)
    lowq = I.to16(low, "565", dither=True).resize((w, h), Image.NEAREST)
    pad = 18
    sw = swatch_row([("zenith", ZENITH), ("top", TOP), ("horizon", m["horizon"]), ("fog", FOG),
                     ("band avg", m["band_avg"]), ("sun", SUN_COL), ("ambient", AMBIENT), ("skyfill", SKYFILL)], w)
    out = Image.new("RGB", (w, pad + (h + pad) * 3 + 60 + pad + sw.size[1] + 4), (24, 24, 28))
    out.paste(img, (0, pad))
    out.paste(q, (0, pad + (h + pad)))
    out.paste(lowq, (0, pad + 2 * (h + pad)))
    y0 = pad + 3 * (h + pad)
    band = Image.new("RGB", (w, 60), FOG)
    bp = band.load()
    for x in range(w):
        c = m["colavg"][x]
        for y in range(0, 30):
            bp[x, y] = (int(round(c[0])), int(round(c[1])), int(round(c[2])))
    out.paste(band, (0, y0))
    out.paste(sw, (0, y0 + 60 + pad))
    dr = ImageDraw.Draw(out)
    for yy, title in ((0, "High 2048x256 (24-bit source), elev %+.0f..%+.0f, horizon row %d" %
                       (ELEV_LO, ELEV_HI, int(V_HORIZON * H))),
                      (h + pad, "High after 565 + engine Bayer dither"),
                      (2 * (h + pad), "Low 1024x128 (565 dithered, shown 2x nearest)")):
        V.label(out, title, xy=(4, yy + 2), color=(230, 230, 230), size=12)
    V.label(out, "horizon band (-0.75..+0.25 deg) average per column (top half) vs fog %s (bottom half); max luma "
            "dev %.2f; rows == fog exactly: %d (from row %d, contiguous to bottom: %s); top row == top: %s" %
            (str(FOG), m["band_dev_luma"], m["fog_rows"], m["first_fog"], m["bottom_ok"], m["top_ok"]),
            xy=(4, y0 - 16), color=(230, 230, 230), size=12)
    for k in range(3):
        yy = pad + k * (h + pad)
        hy = yy + int(round(V_HORIZON * h))
        for x in range(0, w, 8):
            dr.line([(x, hy), (x + 3, hy)], fill=(255, 255, 0))
        for az, name in AZ_LABELS:
            x = int(az / 360.0 * w)
            dr.line([(x, yy + h - 10), (x, yy + h)], fill=(255, 255, 255))
            V.label(out, name, xy=(x + 3, yy + h - 14), color=(255, 255, 255), size=11)
        for t in range(1, 8):
            x = t * w // 8
            dr.line([(x, yy), (x, yy + 6)], fill=(0, 255, 255))
    return out


def sample_pano(pix, w, h, az, e):
    """Bilinear sample of a panorama (list of rows of RGB tuples) at azimuth/elevation; wraps in u."""
    u = (az % 360.0) / 360.0 * w - 0.5
    v = y_of_elev(e, h) - 0.5
    x0 = int(math.floor(u))
    y0 = int(math.floor(v))
    fu, fv = u - x0, v - y0
    y0c = 0 if y0 < 0 else (h - 1 if y0 > h - 1 else y0)
    y1c = 0 if y0 + 1 < 0 else (h - 1 if y0 + 1 > h - 1 else y0 + 1)
    x0w, x1w = x0 % w, (x0 + 1) % w
    a, b, c, d = pix[y0c][x0w], pix[y0c][x1w], pix[y1c][x0w], pix[y1c][x1w]
    return tuple(a[k] * (1 - fu) * (1 - fv) + b[k] * fu * (1 - fv) + c[k] * (1 - fu) * fv + d[k] * fu * fv
                 for k in range(3))


def mock_view(pano_img, yaw, pitch, alt, fog=(3600.0, 18000.0), size=(640, 480), fov=70.0, puffs=None,
              glow=None):
    """70 deg FOV view: sea plane (fogged by eye-space depth, cut at the far plane) + panorama + zenith cap +
    optional cloud billboards; 565 + dither like the VM. Panorama converted per tile like the engine upload."""
    w, h = pano_img.size
    up = Image.new("RGB", (w, h))
    tw = 256
    for i in range(w // tw):
        up.paste(I.to16(pano_img.crop((i * tw, 0, (i + 1) * tw, h)), "565", dither=True), (i * tw, 0))
    pp = up.load()
    pix = [[pp[x, y] for x in range(w)] for y in range(h)]
    W2, H2 = size
    out = Image.new("RGB", size)
    op = out.load()
    th = math.tan(math.radians(fov * 0.5))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    fs, fe = fog
    sea_deep = (22, 66, 112)
    sea_sky = (120, 158, 196)
    for py in range(H2):
        ny = (1.0 - 2.0 * (py + 0.5) / H2) * th * H2 / W2
        for px in range(W2):
            nx = (2.0 * (px + 0.5) / W2 - 1.0) * th
            dy = ny * cp + sp                       # camera z = 1 -> world (pitch about x, then yaw)
            dz = -ny * sp + cp
            dx = nx
            wx = dx * cy + dz * sy
            wz = -dx * sy + dz * cy
            hl = math.sqrt(wx * wx + wz * wz)
            c = None
            if dy < 0.0:
                depth = alt / -dy                   # eye-space depth of the sea hit (ray has camera z = 1)
                if depth <= fe:
                    ln = math.sqrt(dx * dx + dy * dy + dz * dz)
                    fr = (1.0 - (-dy / ln)) ** 5
                    base = mix(sea_deep, sea_sky, 0.25 + 0.6 * fr)
                    f = 0.0 if depth <= fs else (1.0 if depth >= fe else (depth - fs) / (fe - fs))
                    c = mix(base, FOG, f)
            if c is None:
                e = math.degrees(math.atan2(dy, hl))
                az = math.degrees(math.atan2(wx, wz)) % 360.0
                if e > ELEV_HI:
                    c = mix(TOP, ZENITH, (e - ELEV_HI) / (90.0 - ELEV_HI))
                elif e < ELEV_LO:
                    c = FOG
                else:
                    c = sample_pano(pix, w, h, az, e)
            op[px, py] = (P.clamp8(c[0]), P.clamp8(c[1]), P.clamp8(c[2]))
    if glow is not None:                            # additive sun billboard (engine: sun * 56/255, 11 deg half size)
        sd = P.dir_from_angles(SUN_AZ, SUN_EL)
        p = project(sd[0] * 1e5, sd[1] * 1e5, sd[2] * 1e5, 0.0, yaw, pitch, th, W2, H2)
        if p:
            half = math.tan(math.radians(11.0)) / th * W2 * 0.5
            gs = max(2, int(half * 2))
            g = glow.resize((gs, gs), Image.BILINEAR).load()
            k = [SUN_COL[c] * 56 / 255.0 / 255.0 for c in range(3)]
            x0, y0 = int(p[0] - gs / 2), int(p[1] - gs / 2)
            for yy in range(gs):
                for xx in range(gs):
                    X, Y = x0 + xx, y0 + yy
                    if 0 <= X < W2 and 0 <= Y < H2:
                        o = op[X, Y]
                        s = g[xx, yy]
                        op[X, Y] = tuple(min(255, int(o[c] + s[c] * k[c])) for c in range(3))
    if puffs:
        cells, items = puffs
        prj = []
        for (x, y, z, size, cell) in items:
            p = project(x, y, z, alt, yaw, pitch, th, W2, H2)
            if p and p[2] > 50.0:
                prj.append((p[2], p, size, cell))
        prj.sort(key=lambda t: -t[0])                # far to near
        for depth, p, size, cell in prj:
            if depth > fe:
                continue
            f = 0.0 if depth <= fs else (depth - fs) / (fe - fs)
            spx = size / depth / th * W2 * 0.5
            n = int(spx * 2)
            if n < 2:
                continue
            sprite = cells[cell].resize((n, n), Image.BILINEAR)
            fogged = Image.new("RGBA", (n, n), FOG + (255,))
            rgb = Image.blend(sprite.convert("RGB"), fogged.convert("RGB"), f)
            rgb.putalpha(sprite.split()[3])
            out.paste(rgb, (int(p[0] - n / 2), int(p[1] - n / 2)), rgb)
    return I.to16(out, "565", dither=True)


def project(x, y, z, alt, yaw, pitch, th, W2, H2):
    """World point (camera at (0, alt, 0)) -> (sx, sy, depth) or None."""
    y = y - alt
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    dx = x * cy - z * sy                         # inverse yaw
    dz = x * sy + z * cy
    cz = -y * sp + dz * cp                       # inverse pitch: camera space y up, z fwd
    cyy = y * cp + dz * sp
    if cz <= 1.0:
        return None
    sx = (dx / cz / th * 0.5 + 0.5) * W2
    syy = (0.5 - cyy / cz / th * 0.5 * W2 / H2) * H2
    return (sx, syy, cz)


def view_with_swatches(view, title, pano):
    """A 70 deg view + a swatch strip: fog, the panorama's bottom rows, its row at 0 deg, and the view's
    bottom-of-sky pixels next to it."""
    w, h = view.size
    px = pano.load()
    pw, ph = pano.size
    bottom = px[0, ph - 1]
    yrow = int(V_HORIZON * ph)                    # first row under the horizon
    under = px[pw // 2, yrow]
    above = px[pw // 2, yrow - 1]
    sw = swatch_row([("fog", FOG), ("bottom row", bottom), ("row under 0", under),
                     ("row over 0", above), ("top", TOP), ("zenith", ZENITH)], w)
    out = Image.new("RGB", (w, h + sw.size[1]), (24, 24, 28))
    out.paste(view, (0, 0))
    out.paste(sw, (0, h))
    V.label(out, title, xy=(4, 3), color=(10, 10, 20), size=11)
    return out


def puff_scene_items():
    """Deterministic cloud billboards for the mock view (what T08 will place: 1.8-4 km high)."""
    r = Rng(SEED, "scene_puffs")
    items = []
    for k in range(40):
        ang = math.radians(r.range(0.0, 360.0))
        dist = r.range(2500.0, 17000.0)
        items.append((math.sin(ang) * dist, r.range(1800.0, 4000.0), math.cos(ang) * dist,
                      r.range(450.0, 1100.0), r.int(0, 3)))
    return items


def puffs_preview(atlas, cells):
    cell = 256
    bgs = [("over checker", None), ("over sky (zenith side)", (88, 140, 210)), ("over fog", FOG),
           ("4444 + dither over sky", (88, 140, 210))]
    out = Image.new("RGB", (cell * len(bgs), cell + 22), (40, 40, 44))
    for k, (name, bg) in enumerate(bgs):
        a = atlas
        if k == 3:
            a = I.to16(atlas, "4444", dither=True)
        if bg is None:
            im = I.over_checker(a)
        else:
            im = Image.new("RGB", a.size, bg)
            im.paste(a.convert("RGB"), (0, 0), a)
        out.paste(im.convert("RGB"), (k * cell, 22))
        V.label(out, name, xy=(k * cell + 4, 4), color=(230, 230, 230), size=12)
    big = Image.new("RGB", (512, 512), (88, 140, 210))
    b2 = I.to16(cells[1], "4444", dither=True).resize((512, 512), Image.BILINEAR)
    big.paste(b2.convert("RGB"), (0, 0), b2)
    sheet = Image.new("RGB", (out.size[0], out.size[1] + 530), (40, 40, 44))
    sheet.paste(out, (0, 0))
    sheet.paste(big, (0, out.size[1] + 14))
    V.label(sheet, "tower puff x4, 4444 + dither, bilinear (close fly-by magnification)",
            xy=(520, out.size[1] + 16), color=(230, 230, 230), size=12)
    side = Image.new("RGB", (512, 512), (88, 140, 210))
    s2 = cells[0].resize((512, 512), Image.BILINEAR)
    side.paste(s2.convert("RGB"), (0, 0), s2)
    sheet.paste(side, (520, out.size[1] + 34))
    V.label(sheet, "wide puff x4, 24-bit source", xy=(524, out.size[1] + 36 + 512 - 16), color=(20, 20, 30),
            size=12)
    return sheet


def deck_preview(deck, seam, adj):
    tiled = Image.new("RGBA", (512, 512))
    for k in range(4):
        tiled.paste(deck, ((k % 2) * 256, (k // 2) * 256))
    out = Image.new("RGB", (512 * 3 + 16, 512 + 24), (40, 40, 44))
    bg = Image.new("RGB", (512, 512), (88, 140, 210))
    bg.paste(tiled.convert("RGB"), (0, 0), tiled)
    out.paste(bg, (0, 24))
    off = I.wrap_offset(deck)                       # seams move to the middle
    t2 = Image.new("RGBA", (512, 512))
    for k in range(4):
        t2.paste(off, ((k % 2) * 256, (k // 2) * 256))
    bg2 = Image.new("RGB", (512, 512), (40, 70, 110))
    bg2.paste(t2.convert("RGB"), (0, 0), t2)
    out.paste(bg2, (520, 24))
    q = I.to16(tiled, "4444", dither=True)
    bg3 = Image.new("RGB", (512, 512), FOG)
    bg3.paste(q.convert("RGB"), (0, 0), q)
    out.paste(bg3, (1040, 24))
    V.label(out, "2x2 tiled over sky (tile edges at 256)", xy=(4, 4), color=(230, 230, 230), size=12)
    V.label(out, "half-offset copy 2x2: seam error %.2f vs adjacent %.2f" % (seam, adj), xy=(524, 4),
            color=(230, 230, 230), size=12)
    V.label(out, "4444 + dither over fog colour", xy=(1044, 4), color=(230, 230, 230), size=12)
    dr = ImageDraw.Draw(out)
    for x in (256,):
        dr.line([(x, 24), (x, 30)], fill=(255, 0, 0))
        dr.line([(0, 24 + x), (6, 24 + x)], fill=(255, 0, 0))
    return out


def glow_preview(glow):
    out = Image.new("RGB", (128 * 3 + 16, 128 + 24), (40, 40, 44))
    out.paste(glow, (0, 24))
    for k, bg in enumerate(((88, 140, 210), FOG)):
        im = Image.new("RGB", (128, 128), bg)
        gp = glow.load()
        ip = im.load()
        for y in range(128):
            for x in range(128):
                s = gp[x, y]
                o = ip[x, y]
                ip[x, y] = tuple(min(255, int(o[c] + s[c] * SUN_COL[c] / 255.0 * 0.6)) for c in range(3))
        out.paste(im, (136 * (k + 1), 24))
    V.label(out, "sun_glow (RGB, additive)", xy=(4, 4), color=(230, 230, 230), size=12)
    V.label(out, "added x0.6 over sky / fog", xy=(140, 4), color=(230, 230, 230), size=12)
    return out


def tile_deck_stats(img):
    """Seam error (opposite edges) vs the mean adjacent-pixel difference inside the image."""
    rgb = img.convert("RGB")
    seam = I.seam_error(rgb)
    px = rgb.load()
    w, h = rgb.size
    s = 0
    n = 0
    for y in range(0, h, 3):
        for x in range(w - 1):
            a, b = px[x, y], px[x + 1, y]
            s += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
            n += 3
    adj = s / float(n)
    al = img.split()[3]
    ap = al.load()
    aseam = (sum(abs(ap[0, y] - ap[w - 1, y]) for y in range(h)) +
             sum(abs(ap[x, 0] - ap[x, h - 1]) for x in range(w))) / float(w + h)
    aadj = (sum(abs(ap[x, y] - ap[x + 1, y]) for y in range(h) for x in range(w - 1)) +
            sum(abs(ap[x, y] - ap[x, y + 1]) for y in range(h - 1) for x in range(w))) / float(2 * w * (h - 1))
    return seam, adj, aseam, aadj


# ------------------------------------------------------------------------------------------------ main

def build(out_dir, kit):
    img = make_panorama()
    low = make_low(img)
    m = measure(img)
    ml = measure(low)
    for i, t in enumerate(tiles_of(img, 8)):
        kit.save_bmp("sky/sky_hi_%d.bmp" % i, t)
    for i, t in enumerate(tiles_of(low, 4)):
        kit.save_bmp("sky/sky_lo_%d.bmp" % i, t)
    kit.save_text("sky/light.txt", light_txt(m), crlf=True)
    atlas, cells = puffs_atlas()
    kit.save_bmp("tex/cloud_puffs.bmp", atlas, alpha=True)
    deck = deck_rgba()
    kit.save_bmp("tex/cloud_deck.bmp", deck, alpha=True)
    glow = glow_rgb()
    kit.save_bmp("tex/sun_glow.bmp", glow)
    seam, adj, aseam, aadj = tile_deck_stats(deck)
    cov = sum(1 for a in deck.split()[3].getdata() if a >= 128) / float(256 * 256)
    kit.log("fog %s band avg High %s (max column luma dev %.2f) Low %s (dev %.2f); horizon %s; brightest column az "
            "%.1f (sun %.1f); fog rows High %d from row %d ok=%s, Low %d ok=%s; top row ok High=%s Low=%s; wrap seam "
            "%.2f vs adjacent %.2f" % (FOG, m["band_avg"], m["band_dev_luma"], ml["band_avg"], ml["band_dev_luma"],
                                       m["horizon"], m["bright_az"], SUN_AZ, m["fog_rows"], m["first_fog"],
                                       m["bottom_ok"], ml["fog_rows"], ml["bottom_ok"], m["top_ok"], ml["top_ok"],
                                       m["seam"], m["adjacent"]))
    kit.log("cloud_deck seam %.2f vs adjacent %.2f (rgb), alpha seam %.2f vs adjacent %.2f, coverage %.0f %%" %
            (seam, adj, aseam, aadj, cov * 100.0))
    if kit.previews:
        kit.save_preview("sky_strip.png", preview_strip(img, low, m))
        views = [("sun", img, 230.0, 22.0, 600.0, None, True, "toward the sun, pitch +22, 600 m"),
                 ("anti", img, 50.0, 0.0, 300.0, None, False, "away from the sun, level, 300 m"),
                 ("high", img, 140.0, -8.0, 4000.0, None, False, "SE, pitch -8, 4000 m (sea edge at far plane)"),
                 ("puffs", img, 100.0, 2.0, 1500.0, (cells, puff_scene_items()), False,
                  "E, 1500 m, 40 puffs 1.8-4 km, fogged 3.6-18 km"),
                 ("low", low, 320.0, 1.0, 300.0, None, False, "LOW tiles 1024x128, NW, level, 300 m")]
        for name, pano, yaw, pitch, alt, puffs, g, title in views:
            v = mock_view(pano, yaw, pitch, alt, puffs=puffs, glow=glow if g else None)
            kit.save_preview("sky_view_%s.png" % name, view_with_swatches(
                v, "yaw %.0f: %s; 70 deg FOV, 565+dither, sea fogged to the far plane" % (yaw, title), pano))
        kit.save_preview("sky_tiles.png", V.contact_sheet(
            [("sky_hi_%d" % i, t) for i, t in enumerate(tiles_of(img, 8))] +
            [("sky_lo_%d" % i, t) for i, t in enumerate(tiles_of(low, 4))], cols=4, cell=200))
        kit.save_preview("cloud_puffs.png", puffs_preview(atlas, cells))
        kit.save_preview("cloud_deck.png", deck_preview(deck, seam, adj))
        kit.save_preview("sun_glow.png", glow_preview(glow))


if __name__ == "__main__":
    from assetkit import build as BB
    sys.exit(BB.main_for(__file__))
