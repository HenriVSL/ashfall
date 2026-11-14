"""classes.py - material classes per quad (DESIGN 8.1), the class pyramid for the coarser LODs and the colour
targets the vertex colours are made from.

Class per LOD0 quad ((N-1) x (N-1), CLAS chunk) from the quad's mean height, slope (from its corners), the lava
weight of height.py, the airbase layout (base.layout) and two noise fields that make the borders irregular:
  water  mean height < 0                         runway / apron  inside the base layout (terrain under ribbons)
  lava   lava weight > 0.5 (channels, crater floor)
  rock   slope > rock_slope (+-3 deg noise), or between grass_slope and rock_slope by noise
  ash    height above a noisy line between grass_max and ash_min
  sand   height < beach_max (+-2 m noise) within 250 m of the coast
  grass  everything else
Colour target per quad = the average on-screen colour wanted on flat sunlit ground (tint x texture mean / 255,
island.txt `tint`), with macro variation: forest/dry/coastal grass, crater-tinted rock, red ash, shelf -> deep
water by depth, a little brightness jitter. Vertex colours (chunks.py) = mean target of the adjacent quads of the
same group (water / land) / texture mean x baked light, so colour is smooth across class borders while the texture
switches per quad.
LOD k quad (2^k cells): water if the mean of its 4 corner heights < 0, else the most common land class of its LOD0
quads (ties: lower class number; lava counts as rock at LOD >= 1 so thin flows never become 64-512 m lava blocks:
the dark vertex colour still draws them).

A11 (island detail, A6 colour pass): grass quads carry a MATERIAL sub-class (cl.mcls; CLAS keeps the 8 game
classes): FOREST (t_forest canopy texture) and SCRUB (t_scrub dry heath texture) next to plain GRASS, so the
lowlands switch texture per 80 m quad, not only colour. Inputs (island.txt, all optional):
  moisture az k        wind-from azimuth; windward (rain) side lush and forested, leeward dry (k = contrast)
  forest thr h0 h1     forest mask threshold (lower = more), lowest height, treeline (forest fades out above)
  fields hmax slope share   farmland patchwork: brick-like plots of 2-6 x 2-5 quads below hmax on gentle slopes,
                       plot types pasture / hay / crop (t_grass) and stubble / plough (t_scrub), in districts
  scrub_belt h0 h1     dry heath belt (SCRUB) between these heights, under the ash
  lava_cool h          glowing lava (t_lava) only above h and in the crater; below it the flows are cooled
                       basalt (rock / scrub with the basalt tint)
  outcrops share       rock outcrops on lowland hill tops and steeper knolls
  sand_black share     share of black-sand beaches (volcanic), more near lava flows
Per-quad extras for scatter.py: cl.forest (0..1), cl.moist (0..1), cl.plot (plot id or -1), cl.ptype (plot type).
"""
import math

from assetkit import rng as R

import base as BASE
from height import smoothstep

WATER, SAND, GRASS, ROCK, ASH, LAVA, RUNWAY, APRON = range(8)
NCLASS = 8
FOREST, SCRUB = 8, 9            # A11 material sub-classes of GRASS (cl.mcls / pyramid only; CLAS says GRASS)
NMCLASS = 10
MCLASS_NAMES = ["water", "sand", "grass", "rock", "ash", "lava", "runway", "apron", "forest", "scrub"]
PLOT_TYPES = ["pasture", "hay", "crop", "stubble", "plough"]    # field_<type> tints; stubble/plough on t_scrub


def ihash(*v):
    """Small integer hash (plots, outcrops): deterministic, fast."""
    h = 0x811C9DC5
    for a in v:
        h = ((h ^ (a & 0xFFFFFFFF)) * 0x01000193) & 0xFFFFFFFF
        h ^= h >> 13
        h = (h * 0x5BD1E995) & 0xFFFFFFFF
        h ^= h >> 15
    return h


def plot_lattice(NQ, seed):
    """Brick-like field plots on the quad grid: strips of 2-5 quad rows, each cut into plots 2-6 quads wide.
    Returns (row_strip[qj], strip_cols[strip][qi] -> plot column)."""
    row_strip = [0] * NQ
    qj = 0
    st = 0
    while qj < NQ:
        hq = 2 + ihash(seed, 1, st) % 4
        for j in range(qj, min(NQ, qj + hq)):
            row_strip[j] = st
        qj += hq
        st += 1
    cols = []
    for k in range(st):
        cc = [0] * NQ
        qi = (ihash(seed, 2, k) % 5)            # stagger the first cut per strip
        col = 0
        for i in range(0, qi):
            cc[i] = col
        while qi < NQ:
            wq = 2 + ihash(seed, 3, k, col) % 5
            col += 1
            for i in range(qi, min(NQ, qi + wq)):
                cc[i] = col
            qi += wq
        cols.append(cc)
    return row_strip, cols
CLASS_TEX = ["t_sand", "t_sand", "t_grass", "t_rock", "t_ash", "t_lava", "t_grass", "t_grass"]
CLASS_TINT = ["shelf", "sand", "grass", "rock", "ash", "lava", "plateau", "plateau"]
FALLBACK_MEAN = {"t_sand": (216.0, 216.0, 216.0), "t_grass": (184.0, 184.0, 184.0), "t_rock": (168.0, 168.0, 168.0),
                 "t_ash": (184.0, 184.0, 184.0), "t_lava": (62.0, 51.0, 47.0), "t_forest": (160.0, 160.0, 160.0),
                 "t_scrub": (176.0, 176.0, 176.0)}


def texture_means(kit):
    """Per-channel mean of each terrain texture (data/tex), so tints follow A3's textures exactly."""
    out = {}
    for name in sorted(FALLBACK_MEAN):
        p = kit.find("tex/%s.bmp" % name) if kit is not None else None
        if p:
            img, _ = kit.image.read_bmp(p)
            img = img.convert("RGB")
            data = img.tobytes()
            n = len(data) // 3
            out[name] = tuple(sum(data[c::3]) / float(n) for c in range(3))
        else:
            out[name] = FALLBACK_MEAN[name]
    return out


def lerp3(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


class Classes(object):
    def class_tint_str(self, k):
        t = self.tints[CLASS_TINT[k]]
        return "%d %d %d (%s on %s)" % (t[0], t[1], t[2], CLASS_TINT[k], CLASS_TEX[k])


def build(src, hf, tex_means):
    N = hf.N
    NQ = N - 1
    H = hf.H
    cl = Classes()
    cl.tex_means = tex_means
    tints = src.tints()
    cl.tints = tints

    def tgt(name, tex):
        t = tints[name]
        m = tex_means[tex]
        return (t[0] * m[0] / 255.0, t[1] * m[1] / 255.0, t[2] * m[2] / 255.0)

    T = {
        "grass": tgt("grass", "t_grass"), "forest": tgt("grass_forest", "t_grass"), "dry": tgt("grass_dry", "t_grass"),
        "coast": tgt("grass_coast", "t_grass"), "plateau": tgt("plateau", "t_grass"), "rock": tgt("rock", "t_rock"),
        "rockc": tgt("rock_crater", "t_rock"), "ash": tgt("ash", "t_ash"), "ashr": tgt("ash_red", "t_ash"),
        "lava": tgt("lava", "t_lava"), "sand": tgt("sand", "t_sand"), "shelf": tgt("shelf", "t_sand"),
        "deep": tgt("deep", "t_sand"),
        # A11 (materials t_forest / t_scrub: the target divides by that texture's mean in chunks.vcolour)
        "forest2": tgt("grass_forest", "t_forest"), "forestl": tgt("forest_light", "t_forest"), "conifer": tgt("forest_conifer", "t_forest"),
        "wet": tgt("grass_wet", "t_grass"), "scrub": tgt("scrub", "t_scrub"), "heath": tgt("scrub_heath", "t_scrub"),
        "basalt": tgt("basalt", "t_rock"), "basalt_s": tgt("basalt", "t_scrub"), "sandb": tgt("sand_black", "t_sand"),
        "sulphur": tgt("ash_sulphur", "t_ash"), "rockl": tgt("rock_lowland", "t_rock")}
    for pt in PLOT_TYPES:
        T["f_" + pt] = tgt("field_" + pt, "t_scrub" if pt in ("stubble", "plough") else "t_grass")
    cl.targets = T
    seed = int(src.g1("seed"))
    sN1 = R.stable_hash32(seed, "cls1") & 0x3FFFFFF
    sN2 = R.stable_hash32(seed, "cls2") & 0x3FFFFFF
    sF = R.stable_hash32(seed, "forest") & 0x3FFFFFF
    sJ = R.stable_hash32(seed, "jitter") & 0x3FFFFFF
    sA = R.stable_hash32(seed, "ashred") & 0x3FFFFFF
    beach_max = src.g1("beach_max")
    grass_max = src.g1("grass_max")
    ash_min = src.g1("ash_min")
    gs = src.g1("grass_slope")
    rs = src.g1("rock_slope")
    rects = BASE.layout(src)
    m = hf.model
    w_az, w_k = src.get("moisture")
    wdx, wdz = math.sin(math.radians(w_az)), math.cos(math.radians(w_az))
    f_thr, f_h0, f_h1 = src.get("forest")
    fld_h, fld_sl, fld_share = src.get("fields")
    sb_h0, sb_h1 = src.get("scrub_belt")
    lava_cool = src.g1("lava_cool")
    outc = src.g1("outcrops")
    sblack = src.g1("sand_black")
    sM = R.stable_hash32(seed, "moist") & 0x3FFFFFF
    sFm = R.stable_hash32(seed, "farm") & 0x3FFFFFF
    sCo = R.stable_hash32(seed, "conifer") & 0x3FFFFFF
    sSb = R.stable_hash32(seed, "sandblack") & 0x3FFFFFF
    sOc = R.stable_hash32(seed, "outcrop") & 0x3FFFFFF
    sD = R.stable_hash32(seed, "dryscrub") & 0x3FFFFFF
    sW = R.stable_hash32(seed, "woodlot") & 0x3FFFFFF
    sCl = R.stable_hash32(seed, "clearing") & 0x3FFFFFF
    sFv = R.stable_hash32(seed, "forestvar") & 0x3FFFFFF
    sGv = R.stable_hash32(seed, "grassvar") & 0x3FFFFFF
    sUp = R.stable_hash32(seed, "upland") & 0x3FFFFFF
    pseed = R.stable_hash32(seed, "plots") & 0x3FFFFFF
    row_strip, strip_cols = plot_lattice(NQ, pseed)
    ns = src.g1("noise_scale")                      # class noise wavelengths x this (T03b: coarser cells)
    hs = src.g1("summit") / 1750.0                   # height bands of the macro tints follow the summit
    sand_dc = -max(250.0, hf.model.beach_w + 120.0)
    fr = hf.frame
    a_lo = min(r[1] for r in rects) - 40.0
    a_hi = max(r[2] for r in rects) + 40.0
    b_lo = min(r[3] for r in rects) - 40.0
    b_hi = max(r[4] for r in rects) + 40.0
    cell = hf.cell
    cls = [0] * (NQ * NQ)
    mcls = [0] * (NQ * NQ)
    tg = [None] * (NQ * NQ)
    counts = [0] * NCLASS
    mcounts = [0] * NMCLASS
    forest_w = [0.0] * (NQ * NQ)
    moist_w = [0.0] * (NQ * NQ)
    plot_id = [-1] * (NQ * NQ)
    plot_ty = [-1] * (NQ * NQ)
    conif = [0] * (NQ * NQ)
    for qj in range(NQ):
        z = hf.z0 + (qj + 0.5) * cell
        r0 = qj * N
        r1 = r0 + N
        for qi in range(NQ):
            x = hf.x0 + (qi + 0.5) * cell
            h00 = H[r0 + qi]
            h10 = H[r0 + qi + 1]
            h01 = H[r1 + qi]
            h11 = H[r1 + qi + 1]
            hc = 0.25 * (h00 + h10 + h01 + h11)
            gx = ((h10 + h11) - (h00 + h01)) / (2.0 * cell)
            gz = ((h01 + h11) - (h00 + h10)) / (2.0 * cell)
            slope = math.degrees(math.atan(math.hypot(gx, gz)))
            k = qj * NQ + qi
            c = -1
            if hc < 0.0:
                c = WATER
                t = lerp3(T["shelf"], T["deep"], smoothstep(4.0, 40.0, -hc))
            else:
                a, b = fr.to_local(x, z)
                if a_lo <= a <= a_hi and b_lo <= b <= b_hi:
                    for kind, a0, a1, b0, b1 in rects:
                        if a0 <= a <= a1 and b0 <= b <= b1:
                            c = RUNWAY if kind == "runway" else APRON
                            break
                if c >= 0:
                    t = T["plateau"]
                else:
                    lw = 0.25 * (hf.LV[r0 + qi] + hf.LV[r0 + qi + 1] + hf.LV[r1 + qi] + hf.LV[r1 + qi + 1])
                    dc = 0.25 * (hf.DC[r0 + qi] + hf.DC[r0 + qi + 1] + hf.DC[r1 + qi] + hf.DC[r1 + qi + 1])
                    n1 = R.fbm(x / (340.0 * ns), z / (340.0 * ns), octaves=3, seed=sN1)
                    n2 = R.value_noise(x / (140.0 * ns), z / (140.0 * ns), sN2)
                    pw = hf.model.plateau_w(x, z) if (a_lo - 600.0 <= a <= a_hi + 600.0 and
                                                       b_lo - 600.0 <= b <= b_hi + 600.0) else 0.0
                    mc = -1
                    # ---- moisture: windward (wind from w_az) wet, leeward dry, cloud belt on the mid slopes
                    rx, rz = x - m.vcx, z - m.vcz
                    rr = math.hypot(rx, rz) or 1.0
                    wind = (rx * wdx + rz * wdz) / rr * min(1.0, rr / (0.35 * m.Rv))
                    belt = math.exp(-((hc - 900.0 * hs) / (420.0 * hs)) ** 2)
                    moist = 0.5 + 0.5 * w_k * wind + 0.18 * belt * w_k + 0.30 * (
                        R.fbm(x / (2600.0 * ns), z / (2600.0 * ns), octaves=2, seed=sM) - 0.5)
                    moist = 0.0 if moist < 0.0 else (1.0 if moist > 1.0 else moist)
                    moist_w[k] = moist
                    cooled = lw > 0.5 and lava_cool > 0.0 and hc < lava_cool and not hf.CF[r0 + qi]
                    if lw > 0.5 and not cooled:
                        c = LAVA
                    elif cooled:
                        c = ROCK                        # cooled basalt flow (rock class; tint below)
                    elif pw > 0.02 and hc > beach_max:
                        c = GRASS                       # plateau embankments stay grassy
                    elif slope > rs + (n1 - 0.5) * 6.0:
                        c = ROCK
                    elif hc > grass_max + (ash_min - grass_max) * n1:
                        c = ASH
                    elif slope > gs + (rs - gs) * n2:
                        c = ROCK
                    elif hc < beach_max + (n1 - 0.5) * 4.0 and dc > sand_dc:
                        c = SAND
                    else:
                        c = GRASS
                    lap = 0.0
                    if 0 < qi < NQ - 1 and 0 < qj < NQ - 1:
                        lap = (H[r0 + qi - 1] + H[r0 + qi + 1] + H[r0 - N + qi] + H[r1 + qi]) - 4.0 * h00
                    if c == GRASS and outc > 0.0 and pw <= 0.02 and 40.0 < hc < grass_max * 0.7 and dc < -300.0:
                        # rock outcrops: knolls (convex) and the odd boulder field
                        o = R.value_noise(x / (330.0 * ns), z / (330.0 * ns), sOc)
                        if o > 1.0 - outc * (0.6 + 2.5 * max(0.0, min(1.0, -lap / (60.0 * ns)))) and slope > 5.0:
                            c = ROCK
                            mc = -2                      # lowland outcrop tint
                    jit = 0.94 + 0.12 * R.value_noise(x / (190.0 * ns), z / (190.0 * ns), sJ)
                    if c == GRASS:
                        mc = GRASS
                        # forest mask: big fBm patches, more on the wet side and in gullies, none on the base
                        fn = R.fbm(x / (1100.0 * ns), z / (1100.0 * ns), octaves=3, seed=sF)
                        fv = fn + 0.32 * (moist - 0.5) + max(0.0, min(0.12, lap / (50.0 * ns)))
                        f = smoothstep(f_thr - 0.05, f_thr + 0.05, fv)
                        wl = R.fbm(x / (520.0 * ns), z / (520.0 * ns), octaves=2, seed=sW)
                        f = max(f, smoothstep(0.66, 0.72, wl))                     # woodlots, also on the dry side
                        if R.value_noise(x / (240.0 * ns), z / (240.0 * ns), sCl) > 0.80 and f < 0.98:
                            f = min(f, 0.45)                                        # clearings in the forest
                        f *= 1.0 - smoothstep(f_h1 - 160.0, f_h1 + 60.0, hc)
                        f *= smoothstep(f_h0, f_h0 + 40.0, hc)
                        f *= smoothstep(-160.0, -420.0, dc)               # not on the beach strip
                        if pw > 0.02:
                            f = 0.0
                        forest_w[k] = f
                        # farmland district
                        farm = 0.0
                        if fld_share > 0.0 and hc < fld_h and slope < fld_sl and dc < -150.0 and pw <= 0.02:
                            fm = R.fbm(x / (3200.0 * ns), z / (3200.0 * ns), octaves=2, seed=sFm)
                            farm = smoothstep(1.0 - fld_share - 0.04, 1.0 - fld_share + 0.04, fm)
                            farm *= 1.0 - smoothstep(fld_h * 0.75, fld_h, hc)
                        st = row_strip[qj]
                        pid = st * 4096 + strip_cols[st][qi]
                        ph = ihash(pseed, 7, pid)
                        if farm > 0.5 and (ph & 0xFF) < 236:          # ~8 % of plots stay woodlots/rough
                            pty = (ph >> 8) % 100
                            pty = 0 if pty < 40 else (1 if pty < 60 else (2 if pty < 76 else (3 if pty < 90 else 4)))
                            plot_id[k] = pid
                            plot_ty[k] = pty
                            t = T["f_" + PLOT_TYPES[pty]]
                            mc = SCRUB if pty >= 3 else GRASS
                            jit = 0.93 + 0.14 * ((ph >> 16) & 0xFF) / 255.0
                            forest_w[k] = 0.0
                        elif (f < 0.5 and fld_share > 0.0 and pw <= 0.02 and hc < fld_h * 3.2 and slope < 14.0 and
                              dc < -300.0 and (ph & 0xFF) < 200 and R.fbm(x / (2400.0 * ns), z / (2400.0 * ns),
                                                                        octaves=2, seed=sUp) > 0.42):
                            # upland pastures (stone-walled plots on the slopes): subtle pasture / hay patchwork
                            pty = 0 if ((ph >> 8) % 100) < 62 else 1
                            plot_id[k] = pid
                            plot_ty[k] = pty
                            t = lerp3(T["grass"], T["f_" + PLOT_TYPES[pty]], 0.55)
                            t = lerp3(t, T["dry"], smoothstep(0.50, 0.15, moist) * 0.4)
                            jit = 0.95 + 0.10 * ((ph >> 16) & 0xFF) / 255.0
                        elif f > 0.5:
                            mc = FOREST
                            co = smoothstep(0.55, 0.70, R.fbm(x / (700.0 * ns), z / (700.0 * ns), octaves=2,
                                                               seed=sCo)) * smoothstep(250.0, 500.0, hc)
                            if 180.0 < hc < 1000.0 and ((ph >> 8) % 100) < 24:
                                co = 1.0                                            # cedar plantation block
                            vari = smoothstep(0.40, 0.70, R.fbm(x / (650.0 * ns), z / (650.0 * ns), octaves=2,
                                                                 seed=sFv))
                            t = lerp3(T["forest2"], T["forestl"], vari * 0.75)
                            t = lerp3(t, T["conifer"], co)
                            conif[k] = 1 if co > 0.5 else 0
                        else:
                            gv = R.fbm(x / (1700.0 * ns), z / (1700.0 * ns), octaves=2, seed=sGv)
                            t = lerp3(T["grass"], T["wet"], smoothstep(0.55, 0.85, moist + 0.5 * (gv - 0.5)))
                            t = lerp3(t, T["dry"], smoothstep(0.50, 0.15, moist) * 0.6 + smoothstep(0.55, 0.75, gv) * 0.3)
                            t = lerp3(t, T["dry"], smoothstep(350.0 * hs, 800.0 * hs, hc) * 0.45)
                            t = lerp3(t, T["forest"], 0.35 * f)            # forest edge: darker grass
                            t = lerp3(t, T["coast"], smoothstep(-260.0, -40.0, dc) * 0.6)
                            heath = 0.0
                            if sb_h1 > sb_h0:
                                heath = smoothstep(sb_h0 - 120.0, sb_h0 + 120.0, hc + (n1 - 0.5) * 240.0)
                            dryk = smoothstep(0.30, 0.15, moist) * smoothstep(0.56, 0.66, R.fbm(
                                x / (900.0 * ns), z / (900.0 * ns), octaves=2, seed=sD))
                            if pw > 0.02:
                                t = T["plateau"]
                            elif heath > 0.5 or dryk > 0.5:
                                mc = SCRUB
                                t = lerp3(T["scrub"], T["heath"], smoothstep(sb_h0, sb_h1, hc))
                    elif c == ROCK:
                        if cooled:
                            t = T["basalt"]
                            if hc < lava_cool * 0.55:
                                mc = SCRUB              # old, vegetated flow: dark heath over basalt
                                t = lerp3(T["basalt_s"], T["heath"], smoothstep(lava_cool * 0.55, 0.0, hc) * 0.5)
                                c = GRASS
                        elif mc == -2:
                            t = T["rockl"]
                        else:
                            t = lerp3(T["rock"], T["rockc"], smoothstep(900.0 * hs, 1550.0 * hs, hc))
                    elif c == ASH:
                        ar = smoothstep(0.5, 0.72, R.fbm(x / (520.0 * ns), z / (520.0 * ns), octaves=3, seed=sA))
                        t = lerp3(T["ash"], T["ashr"], ar * (0.4 + 0.6 * smoothstep(1100.0 * hs, 1650.0 * hs, hc)))
                        dr = math.hypot(x - m.vcx, z - m.vcz)
                        su = smoothstep(m.Rr + 260.0, m.Rr - 40.0, dr) * smoothstep(0.62, 0.74, n2)
                        t = lerp3(t, T["sulphur"], su * 0.8)
                        t = lerp3(t, T["ash"], smoothstep(0.0, 1.0, max(0.0, min(1.0, lap / (90.0 * ns)))) * 0.25)
                    elif c == SAND:
                        sbk = 0.0
                        if sblack > 0.0:
                            sbn = R.fbm(x / (5200.0 * ns), z / (5200.0 * ns), octaves=2, seed=sSb)
                            sbk = smoothstep(1.0 - sblack - 0.05, 1.0 - sblack + 0.05, sbn + 0.6 * min(1.0, lw * 4.0))
                        t = lerp3(T["sand"], T["sandb"], sbk)
                        jit = 0.97 + 0.06 * (jit - 0.94) / 0.12
                    else:
                        t = T["lava"]
                        jit = 1.0
                    if mc < 0:
                        mc = c
                    t = (t[0] * jit, t[1] * jit, t[2] * jit)
            if c < 0:
                c = WATER
            cls[k] = c
            if c in (WATER, RUNWAY, APRON):
                mc = c
            mcls[k] = mc
            tg[k] = t
            counts[c] += 1
            mcounts[mc] += 1
    cl.cls = cls
    cl.mcls = mcls
    cl.mcounts = mcounts
    cl.forest = forest_w
    cl.moist = moist_w
    cl.plot = plot_id
    cl.ptype = plot_ty
    cl.conifer = conif
    cl.target = tg
    cl.counts = counts
    cl.NQ = NQ

    # ---- SATs per group (0 water, 1 land): sums of r, g, b, count over quads
    W = NQ + 1
    sat = []
    for grp in (0, 1):
        sr = [0.0] * (W * W)
        sg = [0.0] * (W * W)
        sb = [0.0] * (W * W)
        sn = [0] * (W * W)
        for qj in range(NQ):
            rr = gg = bb = 0.0
            nn = 0
            base_prev = qj * W
            base = (qj + 1) * W
            for qi in range(NQ):
                k = qj * NQ + qi
                if (cls[k] != WATER) == (grp == 1):
                    t = tg[k]
                    rr += t[0]
                    gg += t[1]
                    bb += t[2]
                    nn += 1
                o = base + qi + 1
                p = base_prev + qi + 1
                sr[o] = sr[p] + rr
                sg[o] = sg[p] + gg
                sb[o] = sb[p] + bb
                sn[o] = sn[p] + nn
        sat.append((sr, sg, sb, sn))
    cl.sat = sat
    cl.W = W

    # ---- pyramid: histograms of land classes, then per-level classes
    hist = [[0] * NMCLASS for _ in range(NQ * NQ)]
    for k in range(NQ * NQ):
        hist[k][mcls[k]] = 1
    pyr = [mcls]
    for lev in range(1, 5):
        nq = NQ >> lev
        pn = nq * 2
        nh = []
        out = []
        s = 1 << lev
        for qj in range(nq):
            for qi in range(nq):
                a = hist[(2 * qj) * pn + 2 * qi]
                b = hist[(2 * qj) * pn + 2 * qi + 1]
                c2 = hist[(2 * qj + 1) * pn + 2 * qi]
                d = hist[(2 * qj + 1) * pn + 2 * qi + 1]
                hsum = [a[t] + b[t] + c2[t] + d[t] for t in range(NMCLASS)]
                nh.append(hsum)
                hmode = list(hsum)
                hmode[ROCK] += hmode[LAVA]          # thin lava channels: rock texture at LOD >= 1 (the vertex
                hmode[LAVA] = 0                     # colour keeps the dark flow), never 2^k m lava blocks
                I, J = qi * s, qj * s
                hm = 0.25 * (H[J * N + I] + H[J * N + I + s] + H[(J + s) * N + I] + H[(J + s) * N + I + s])
                if hm < 0.0:
                    out.append(WATER)
                else:
                    best, bc = SAND, 0
                    for t in range(1, NMCLASS):
                        if hmode[t] > bc:
                            best, bc = t, hmode[t]
                    out.append(best)
        hist = nh
        pyr.append(out)
    cl.pyr = pyr
    return cl


def group_mean(cl, grp, I, J, s):
    """Mean colour target of the group's quads in [I-s, I+s) x [J-s, J+s) (grid lattice I, J); falls back to the
    other group when the window holds none of this one."""
    NQ = cl.NQ
    W = cl.W
    i0 = max(0, I - s)
    i1 = min(NQ, I + s)
    j0 = max(0, J - s)
    j1 = min(NQ, J + s)
    for g in (grp, 1 - grp):
        sr, sg, sb, sn = cl.sat[g]
        n = sn[j1 * W + i1] - sn[j0 * W + i1] - sn[j1 * W + i0] + sn[j0 * W + i0]
        if n > 0:
            def S(a):
                return a[j1 * W + i1] - a[j0 * W + i1] - a[j1 * W + i0] + a[j0 * W + i0]
            return (S(sr) / n, S(sg) / n, S(sb) / n)
    return (128.0, 128.0, 128.0)
