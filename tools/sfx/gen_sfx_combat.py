#!/usr/bin/env python3
"""Jet sound pack v2 (combat), task A9 (RWR tones redone to the user's spec in A9b): deterministic synthesis of
the combat sounds (DESIGN 15 v2 rows).

Writes mono 22050 Hz 16-bit PCM WAVs into data/sfx/ (next to A5's v1 pack, which this module never touches):

  loops      srm_growl   heat-seeker (AIM-9-style) search growl: low raspy tone, pitch/volume by seeker signal
             srm_lock    heat-seeker lock tone: clean steady high tone (1600 Hz)
             rwr_lock    RWR: an enemy fire-control radar locks the player (STT): 455/555 Hz alternating, 0.25 s
                         per tone, continuous (loop 1.0 s = 2 cycles)
             rwr_missile RWR: missile guidance uplink / active seeker: 455/555 Hz alternating, 0.10 s per tone,
                         continuous (loop 0.2 s = 1 cycle, starts on 455 Hz)
             msl_motor   rocket motor heard from outside (positional, doppler by game code)
  one-shots  msl_launch  own missile launch: rail release clunk, ignition crack, motor roar pulling away
             msl_pass    hostile missile passing close: doppler whoosh with a shock crack
             lock_beep   own radar lock on designate: three fast pips + a held higher note
             rwr_search  RWR "waterfall": a new enemy fighter (airborne-intercept) radar enters the threat table:
                         550 -> 520 -> 490 -> 470 Hz, ~0.084 s per step, 0.335 s, once
             rwr_launch  RWR missile onset: the rwr_missile alternation for exactly 0.5 s (555 first), ending in
                         phase so the rwr_missile loop started right after it (on 455 Hz) is one continuous tone
             flare       flare burst (2 cartridges): pop-pop + magnesium fizz
             hit         own aircraft hit: blast, shrapnel pings, airframe groan, rattle
             kill        SPLASH confirmation chime (two bell notes, rising fifth)
             jet_pass    enemy jet fly-by within 1.5 km: doppler whine + roar
             expl_air    warhead detonation / aircraft destroyed in the air (no ground rumble)
             no_msl      empty: dry double switch click (DESIGN 12.6 "NO MSL click")
  README_combat.md       list, loop lengths, use, suggested volumes, priorities (A5's build rewrites README.md)

and, when previews are on (normal runs, not --check), tools/sfx/preview_combat.wav: an offline mix of a scripted
fight over A5's engine/wind loops (from data/sfx) with exactly the suggested volumes and priority rules of the
README: RWR waterfalls, lock, LRM launch, growl -> lock tone -> SRM launch, RWR lock, launch + missile warning with
afterburner and flares, missile pass-by, jet pass, air kill + SPLASH, a hit, NO MSL.

Warning-tone design (they must be told apart instantly, over the engine, and not tire the ear in a long fight):
  * each tone has its own rhythm AND register: growl = low (400 Hz) continuous rough; srm_lock = high (1600 Hz)
    continuous clean; lock_beep = fast pips + held note. The RWR tones follow the user's spec (A9b, binding
    frequencies and timing, like real RWR audio): all three sit at 455-555 Hz, below the engine fan band, and are
    told apart by pattern: rwr_search = one falling 4-step waterfall, rwr_lock = slow 455/555 alternation (2
    tones/s), rwr_missile (+ rwr_launch onset) = fast 455/555 alternation (5 tones/s, continuous).
    A5's PULL UP (rising whoops 700 -> 1400 Hz) and message beep (1050 Hz bip-bip) stay unique too.
  * soft timbres (sine + weak low/odd harmonics, no raw squares), 3-10 ms attacks and raised-cosine releases (no
    clicks); A9's own pitches on a consonant set (lock_beep 1568/2093, kill 1047/1568; srm_lock 1600 on its own).
  * RWR tones are phase-continuous FSK with exact integer phase (fsk()): a frequency switch never jumps the
    waveform (each sample-to-sample step stays within the steady sine's own maximum step), and every loop holds a
    whole number of cycles in total, so sample N-1 -> sample 0 continues the sine. The build logs `click ...`
    lines (seam / switch / launch->missile join steps vs the steady-tone maximum).
Seamless loops: tones are whole cycles per loop (zero-mean FM / exact FSK phase), noise is periodic +
steady-state filtered (gen_sfx.steady()). The build prints A5's seam check per loop.

Python 3 standard library + assetkit (rng; WAV validation via the build runner). Deterministic: same bytes per run.
Usage:  python3 tools/sfx/gen_sfx_combat.py [--check]      (= tools/build_assets.py --only sfx_combat [--check])
"""
import math
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from gen_sfx import (SR, TWO_PI, PEAK, _kit_rng_class, add_pulse, bp, clamp, db,  # noqa: E402
                     fade_out, fades, hp, loudness_db, lp, mix, mix_engine, mix_wind, mul, norm_rms,
                     peak_of, periodic_smooth_noise, put_wav, rbj, rms, scale, silence, soft_clip, spool_step,
                     steady, thump, to_pcm, white, write_wav, zero_mean, noise_burst, grains_decaying)

SEED = 9009                           # this generator's seed (A5 uses 5005)
ROOT = os.path.dirname(os.path.dirname(HERE))
PREVIEW_WAV = os.path.join(HERE, "preview_combat.wav")

LOOP_NAMES = ["srm_growl", "srm_lock", "rwr_lock", "rwr_missile", "msl_motor"]
SHOT_NAMES = ["msl_launch", "msl_pass", "lock_beep", "rwr_search", "rwr_launch", "flare", "hit", "kill",
              "jet_pass", "expl_air", "no_msl"]
ASSETS = ["sfx/%s.wav" % n for n in LOOP_NAMES + SHOT_NAMES] + ["sfx/README_combat.md"]
ORDER = 51                            # after gen_sfx (50); independent of it

# designed pitches (Hz)
F_GROWL = 400.0
F_SRM_LOCK = 1600.0
RWR_LO = 455                          # RWR lock / missile tones (user spec A9b; integers: exact FSK phase)
RWR_HI = 555
RWR_WATERFALL = (550, 520, 490, 470)  # new fighter radar ("waterfall"), falling steps
RWR_HARM = ((1, 1.0), (3, 0.06), (5, 0.015))   # near-pure sine, a touch of odd harmonics for small speakers


_RNGC = None


def rng(*names):
    """assetkit Rng(SEED, *names) (stable hash seeding, same on every run/machine)."""
    global _RNGC
    if _RNGC is None:
        _RNGC = _kit_rng_class()
    return _RNGC(SEED, *names)


# ---------------------------------------------------------------------------------------------
# Small DSP additions (the rest comes from gen_sfx)
# ---------------------------------------------------------------------------------------------

def biquad_vary(x, kind, fc, q, every=16):
    """Biquad whose cutoff follows fc[i] (a list as long as x), coefficients refreshed every `every` samples."""
    out = [0.0] * len(x)
    x1 = x2 = y1 = y2 = 0.0
    b0 = b1 = b2 = a1 = a2 = 0.0
    for i, xi in enumerate(x):
        if i % every == 0:
            b0, b1, b2, a1, a2 = rbj(kind, min(fc[i], 0.45 * SR), q)
        y = b0 * xi + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2 = x1
        x1 = xi
        y2 = y1
        y1 = y
        out[i] = y
    return out


def osc(freqs, harm, ph0=0.0):
    """Oscillator following a per-sample frequency list; harm = [(harmonic, amp)]."""
    out = [0.0] * len(freqs)
    ph = ph0
    for i, f in enumerate(freqs):
        v = 0.0
        for h, a in harm:
            v += a * math.sin(TWO_PI * h * ph)
        out[i] = v
        ph += f / SR
    return out


def note(f, dur, harm, attack=0.006, release=0.012):
    """Constant-pitch note from phase 0: raised-cosine attack and release, no decay (warning tones)."""
    n = int(round(dur * SR))
    na = max(1, int(round(attack * SR)))
    nr = max(1, int(round(release * SR)))
    out = [0.0] * n
    for i in range(n):
        t = i / float(SR)
        v = 0.0
        for h, a in harm:
            v += a * math.sin(TWO_PI * f * h * t)
        g = 0.5 - 0.5 * math.cos(math.pi * i / float(na)) if i < na else 1.0
        k = n - 1 - i
        if k < nr:
            g *= 0.5 - 0.5 * math.cos(math.pi * k / float(nr))
        out[i] = v * g
    return out


def place(buf, seg, t0):
    """Add seg into buf starting at time t0 (s), clipped at the end."""
    s = int(round(t0 * SR))
    L = len(buf)
    for j, v in enumerate(seg):
        if s + j >= L:
            break
        buf[s + j] += v
    return buf


def fsk(segs, harm=RWR_HARM, ph0=0):
    """Phase-continuous tone sequence (A9b RWR tones). segs = [(f in whole Hz, samples)]. The phase is an exact
    integer in units of 1/SR cycle (it advances by f per sample), so a frequency switch never jumps the waveform
    and a sequence whose phase total is a multiple of SR holds a whole number of cycles (seamless loop, sample
    N-1 -> 0). Sample i uses the phase reached after i samples. Returns (samples, end phase in 1/SR units)."""
    out = []
    ph = ph0
    for f, m in segs:
        for _ in range(m):
            v = 0.0
            for h, a in harm:
                v += a * math.sin(TWO_PI * ((h * ph) % SR) / float(SR))
            out.append(v)
            ph += f
    return out, ph


def ramp_ends(x, attack, release):
    """Raised-cosine attack over `attack` s and release over `release` s (0 = none), in place."""
    na = int(round(attack * SR))
    nr = int(round(release * SR))
    n = len(x)
    for i in range(min(na, n)):
        x[i] *= 0.5 - 0.5 * math.cos(math.pi * i / float(na))
    for k in range(min(nr, n)):
        x[n - 1 - k] *= 0.5 - 0.5 * math.cos(math.pi * k / float(nr))
    return x


def taper(n, t0, t1):
    """1 up to t0, raised-cosine down to 0 at t1 (ends grain clouds without an audible edge)."""
    out = [0.0] * n
    for i in range(n):
        t = i / float(SR)
        if t <= t0:
            out[i] = 1.0
        elif t < t1:
            out[i] = 0.5 + 0.5 * math.cos(math.pi * (t - t0) / (t1 - t0))
    return out


def lorentz_env(n, tc, w_in, w_out):
    """Pass-by envelope: 1 / (1 + ((t - tc) / w)^2), w = w_in before tc, w_out after."""
    out = [0.0] * n
    for i in range(n):
        u = i / float(SR) - tc
        w = w_in if u < 0 else w_out
        out[i] = 1.0 / (1.0 + (u / w) ** 2)
    return out


def spikes_periodic(r, n, rate, tau_lo, tau_hi, power=3.0):
    """Sparse sharp positive pulses (crackle), heavy-tailed amplitudes, wrapping around n."""
    out = [0.0] * n
    for _ in range(int(rate * n / float(SR))):
        add_pulse(out, r.uniform(0.0, n), r.random() ** power, 0.00004 * SR, r.uniform(tau_lo, tau_hi) * SR, n)
    return out


def loud_db(x, loop):
    """Rough perceived level (gen_sfx.loudness_db band 100 Hz - 6 kHz): whole loop for loops, loudest 300 ms
    window (100 ms hop) for one-shots."""
    if loop:
        while len(x) < SR:                    # short loops (rwr_missile 0.2 s): tile to >= 1 s first
            x = x + x
        return loudness_db(x)
    y = lp(hp(x, 100.0), 6000.0)
    w = int(0.3 * SR)
    best = 1e-9
    k = 0
    while True:
        seg = y[k:k + w]
        if not seg:
            break
        best = max(best, rms(seg))
        if k + w >= len(y):
            break
        k += int(0.1 * SR)
    return 20.0 * math.log10(best)


# ---------------------------------------------------------------------------------------------
# Loops
# ---------------------------------------------------------------------------------------------

def loop_srm_growl():
    """AIM-9-style search growl, 2.0 s: a 400 Hz buzzy tone (harmonics 1..10 + a 200 Hz sub for the rasp) with a
    ~6 Hz pitch waver and an irregular ~28 Hz amplitude flutter, a little band noise grit, soft saturation,
    low-passed at 2.8 kHz. Game: pitch 1.0 -> 1.25 and volume up as the seeker signal grows (README)."""
    T = 2.0
    n = int(T * SR)
    r = rng("srm_growl")
    wav = zero_mean(periodic_smooth_noise(r, n, int(6 * T)))
    fm = [0.012 * v for v in wav]
    flut = periodic_smooth_noise(r, n, int(28 * T), 0.2, 1.0)
    flut = [v * v for v in flut]
    slow = periodic_smooth_noise(r, n, int(3 * T), 0.75, 1.0)
    am = [0.25 + 0.75 * a * b for a, b in zip(flut, slow)]
    parts = [(1, 1.0), (2, 0.62), (3, 0.48), (4, 0.36), (5, 0.30), (6, 0.22), (7, 0.18), (8, 0.13), (9, 0.10),
             (10, 0.08)]
    rp = rng("srm_growl", "phases")
    tone = [0.0] * n
    for h, a in parts:
        f = h * F_GROWL
        ph = rp.random()
        inc = f / SR
        for i in range(n):
            tone[i] += a * math.sin(TWO_PI * ph)
            ph += inc * (1.0 + fm[i])
    sub = [0.0] * n
    ph = 0.25
    inc = 0.5 * F_GROWL / SR
    for i in range(n):
        sub[i] = math.sin(TWO_PI * ph)
        ph += inc * (1.0 + fm[i])
    tone = mul(mix((1.0, norm_rms(tone)), (0.45, norm_rms(sub))), am)
    grit, c1 = steady(white(r, n), lambda x: bp(x, 1200.0, 1.1))
    grit = mul(norm_rms(grit), flut)
    y = mix((1.0, norm_rms(tone)), (db(-16.0), grit))
    y = soft_clip(scale(y, 1.0 / peak_of(y)), 1.7)
    y, c2 = steady(y, lambda x: hp(lp(lp(x, 2800.0), 3200.0), 110.0))
    return y, max(c1, c2)


def loop_srm_lock():
    """Heat-seeker lock tone, 1.0 s: steady 1600 Hz, weak 2nd/3rd harmonics (clean, not shrill)."""
    n = SR
    y = [0.0] * n
    for h, a in ((1, 1.0), (2, 0.10), (3, 0.035)):
        f = h * F_SRM_LOCK
        for i in range(n):
            y[i] += a * math.sin(TWO_PI * f * i / SR)
    return y, 0.0


# A9b segment tables (samples at 22050 Hz). 0.25 s = 5512.5 samples, so rwr_lock alternates 5513/5512 per tone
# such that each frequency gets exactly 11025 samples per loop: phase total (455 + 555) x 11025 = 505 x SR (whole
# cycles). One 0.5 s cycle alone would hold 252.5 cycles (not seamless), hence the 1.0 s loop = 2 cycles.
RWR_LOCK_SEGS = [(RWR_LO, 5513), (RWR_HI, 5512), (RWR_LO, 5512), (RWR_HI, 5513)]
# 0.10 s = 2205 samples exactly; one 455 + 555 cycle = 101 whole cycles, so the 0.2 s loop is seamless as is.
RWR_MSL_SEGS = [(RWR_LO, 2205), (RWR_HI, 2205)]
# rwr_launch: 5 x 0.10 s = 0.5 s, 555 first so the rwr_missile loop (455 first) continues the alternation. Its phase
# total is 257.5 cycles, so it starts at phase 0.5 (SR/2) and ends on a whole cycle = the loop's phase 0.
RWR_LAUNCH_SEGS = [(RWR_HI, 2205), (RWR_LO, 2205), (RWR_HI, 2205), (RWR_LO, 2205), (RWR_HI, 2205)]
RWR_LAUNCH_PH0 = SR // 2
# waterfall: 0.335 s = 7386.75 samples -> 7387 (0.33501 s); steps 1847/1847/1847/1846 (0.0838 s each).
RWR_SEARCH_SEGS = [(RWR_WATERFALL[0], 1847), (RWR_WATERFALL[1], 1847), (RWR_WATERFALL[2], 1847),
                   (RWR_WATERFALL[3], 1846)]
RWR_JOIN_SHOTS = ("rwr_launch",)      # one-shots without the build's 5 ms end fade (they end on a whole cycle)


def loop_rwr_lock():
    """RWR lock (user spec): 455 / 555 Hz alternating, 0.25 s per tone, continuous; 1.0 s loop = 2 cycles, starts on
    455 Hz at phase 0, phase-continuous switches, whole cycles in total (seamless)."""
    y, ph = fsk(RWR_LOCK_SEGS)
    assert len(y) == SR and ph % SR == 0
    return y, 0.0


def loop_rwr_missile():
    """RWR missile (user spec): 455 / 555 Hz alternating, 0.10 s per tone, continuous (no bursts, no gaps); 0.2 s
    loop = 1 cycle, starts on 455 Hz at phase 0 (= where rwr_launch ends), whole cycles (seamless)."""
    y, ph = fsk(RWR_MSL_SEGS)
    assert len(y) == 4410 and ph % SR == 0
    return y, 0.0


def loop_msl_motor():
    """Rocket motor from outside, 1.5 s: tearing hiss (band 2.5 kHz with fast flutter), mid roar, dense crackle.
    Brighter and rougher than the jet engine. Game: pitch = doppler, volume = distance law."""
    T = 1.5
    n = int(T * SR)
    r = rng("msl_motor")
    nz = white(r, n)
    hiss, c1 = steady(nz, lambda x: bp(x, 2100.0, 0.55))
    hiss = mul(hiss, periodic_smooth_noise(r, n, int(90 * T), 0.45, 1.0))
    roar, c2 = steady(nz[n // 3:] + nz[:n // 3], lambda x: lp(bp(x, 420.0, 0.6), 2000.0))
    roar = mul(roar, periodic_smooth_noise(r, n, int(30 * T), 0.6, 1.0))
    crk, c3 = steady(spikes_periodic(rng("msl_motor", "crackle"), n, 320, 0.0002, 0.0008),
                     lambda x: lp(hp(x, 800.0), 8000.0))
    y = mix((1.0, norm_rms(hiss)), (db(-3.0), norm_rms(roar)), (db(-5.0), norm_rms(crk)))
    y, c4 = steady(y, lambda x: hp(lp(lp(x, 6500.0), 8000.0), 80.0))
    lim = 3.0 * rms(y)
    y = [lim * math.tanh(v / lim) for v in y]
    return y, max(c1, c2, c3, c4)


# ---------------------------------------------------------------------------------------------
# One-shots
# ---------------------------------------------------------------------------------------------

def shot_msl_launch():
    """Own launch, 2.2 s: rail release clunk (0 s), ignition crack (0.06 s), motor roar at full from 0.1 s that
    pulls away: level decays from 0.35 s and the low-pass closes 9 kHz -> 1.4 kHz (distance), low body thump."""
    T = 2.2
    n = int(T * SR)
    r = rng("msl_launch")
    clunk = mix((1.0, thump(n, 140.0, 70.0, 0.03, 0.05)),
                (0.5, bp(noise_burst(r, n, 0.004), 2500.0, 1.5)))
    t_ig = 0.06
    s_ig = int(t_ig * SR)
    crack = hp(noise_burst(r, n, 0.003, start=s_ig), 1500.0)
    env = [0.0] * n
    fc = [0.0] * n
    for i in range(n):
        t = i / float(SR) - t_ig
        if t < 0:
            env[i] = 0.0
        else:
            env[i] = min(1.0, t / 0.03) * (1.0 if t < 0.3 else math.exp(-(t - 0.3) / 0.55))
        u = max(0.0, t - 0.15)
        fc[i] = 1400.0 + 7600.0 * math.exp(-u / 0.5)
    nz = white(r, n)
    flut = periodic_smooth_noise(r, n, int(90 * T), 0.45, 1.0)
    hiss = mul(biquad_vary(bp(nz, 2400.0, 0.45), "lp", fc, 0.7071), mul(env, flut))
    body_env = [e * math.exp(-max(0.0, i / float(SR) - t_ig) / 0.25) for i, e in enumerate(env)]
    body = mul(lp(bp(white(r, n), 180.0, 0.7), 900.0), body_env)
    crk = [0.0] * n
    rc = rng("msl_launch", "crackle")
    for _ in range(int(350 * T)):
        t0 = rc.uniform(t_ig, T) * SR
        add_pulse(crk, t0, rc.random() ** 3.0, 0.00004 * SR, rc.uniform(0.0002, 0.0008) * SR)
    crk = mul(biquad_vary(hp(crk, 800.0), "lp", fc, 0.7071), env)
    y = mix((0.55, norm_rms(clunk)), (0.25, norm_rms(crack)), (1.0, norm_rms(hiss)), (0.7, norm_rms(body)),
            (0.35, norm_rms(crk)))
    y = soft_clip(scale(y, 1.0 / peak_of(y)), 1.5)
    y = hp(y, 40.0)
    return fade_out(y, 0.6)


def shot_msl_pass():
    """Hostile missile pass-by, 1.6 s, closest at 0.45 s: motor hiss whose band falls 5 kHz -> 1.1 kHz through
    the pass (doppler), sharp rise/slow fall, a small shock crack at the pass, a soft pressure whump."""
    T = 1.6
    n = int(T * SR)
    tc = 0.45
    r = rng("msl_pass")
    env = lorentz_env(n, tc, 0.07, 0.18)
    fc = [1100.0 * (1.0 + 3.5 * (0.5 - 0.5 * math.tanh((i / float(SR) - tc) / 0.05))) for i in range(n)]
    nz = white(r, n)
    flut = periodic_smooth_noise(r, n, int(90 * T), 0.45, 1.0)
    whoosh = mul(biquad_vary(nz, "bp", fc, 0.9), mul(env, flut))
    roar = mul(lp(bp(white(r, n), 380.0, 0.7), 1500.0), lorentz_env(n, tc + 0.03, 0.08, 0.30))
    crack = [0.0] * n
    s = int(tc * SR)
    add_pulse(crack, s, 1.0, 0.00003 * SR, 0.0004 * SR)
    add_pulse(crack, s + int(0.0012 * SR), -0.8, 0.00003 * SR, 0.0005 * SR)
    crack = hp(crack, 700.0)
    whump = thump(n, 75.0, 45.0, 0.1, 0.12, start=int((tc - 0.01) * SR), attack=0.01)
    y = mix((1.0, norm_rms(whoosh)), (0.6, norm_rms(roar)), (0.12, norm_rms(crack)), (0.35, norm_rms(whump)))
    y = soft_clip(scale(y, 1.0 / peak_of(y)), 1.3)
    y = hp(y, 35.0)
    return fade_out(y, 0.5)


def shot_lock_beep():
    """Own radar lock, 0.36 s: three fast 1568 Hz pips (30 ms on, 25 ms gap) then a held 2093 Hz note (0.15 s)."""
    harm = ((1, 1.0), (2, 0.12), (3, 0.08))
    y = []
    for _ in range(3):
        y += note(1568.0, 0.030, harm, attack=0.003, release=0.008) + silence(0.025)
    y += note(2093.0, 0.15, harm, attack=0.004, release=0.04)
    return y + silence(0.02)


def shot_rwr_search():
    """RWR "waterfall" (user spec): a new airborne-intercept (fighter) radar in the threat table: 550 -> 520 -> 490
    -> 470 Hz, 4 steps of ~0.0838 s, 0.335 s (7387 samples), phase-continuous steps (no gaps), 5 ms raised-cosine
    attack and release (plus the build's 5 ms one-shot fades)."""
    y, _ = fsk(RWR_SEARCH_SEGS)
    return ramp_ends(y, 0.005, 0.005)


def shot_rwr_launch():
    """RWR missile onset: the rwr_missile alternation for exactly 0.5 s (11025 samples): 555/455/555/455/555 Hz,
    0.10 s each, 4 ms raised-cosine attack, NO end fade: it starts at phase 0.5 and ends on a whole cycle, so the
    rwr_missile loop started right after it (sample 0, 455 Hz, phase 0) continues the tone without a seam."""
    y, ph = fsk(RWR_LAUNCH_SEGS, ph0=RWR_LAUNCH_PH0)
    assert len(y) == 11025 and ph % SR == 0
    return ramp_ends(y, 0.004, 0.0)


def shot_flare():
    """Flare burst (2 cartridges, DESIGN 12.5), 0.55 s: two pops 0.09 s apart (pressure thump + bright crack),
    each followed by a sputtering magnesium fizz. One file per burst; hold-repeat every 0.5 s fits."""
    T = 0.55
    n = int(T * SR)
    r = rng("flare")
    y = [0.0] * n
    for t0, g in ((0.0, 1.0), (0.09, 0.85)):
        s = int(t0 * SR)
        pop = thump(n, 190.0, 85.0, 0.02, 0.03, start=s)
        crk = bp(noise_burst(r, n, 0.010, start=s), 1800.0, 0.8)
        fz = [0.0] * n
        nz = white(r, n)
        spt = periodic_smooth_noise(r, n, int(45 * T), 0.2, 1.0)
        for i in range(s, n):
            t = (i - s) / float(SR)
            fz[i] = nz[i] * min(1.0, t / 0.01) * math.exp(-t / 0.16) * spt[i]
        fz = lp(hp(fz, 2800.0), 9000.0)
        y = mix((1.0, y), (g, norm_rms(pop)), (1.0 * g, norm_rms(crk)), (0.6 * g, norm_rms(fz)))
    y = soft_clip(scale(y, 1.0 / peak_of(y)), 1.4)
    return fade_out(hp(y, 50.0), 0.15)


def shot_hit():
    """Own aircraft hit, 1.8 s (heard inside): crack, heavy boom, shrapnel pings on the skin (0-0.25 s),
    airframe groan (inharmonic low partials bending down), rattle/debris to 0.8 s. Not the crash explosion."""
    T = 1.8
    n = int(T * SR)
    r = rng("hit")
    crack = hp(noise_burst(r, n, 0.005), 1500.0)
    boom = thump(n, 110.0, 35.0, 0.25, 0.35, attack=0.002)
    blast = lp(lp(noise_burst(r, n, 0.12, attack=0.002), 600.0), 800.0)
    pings = [0.0] * n
    rp = rng("hit", "pings")
    for _ in range(60):
        t0 = rp.uniform(0.0, 0.25) ** 1.5 / 0.25 ** 0.5
        f = rp.uniform(1500.0, 6000.0)
        d = rp.uniform(0.010, 0.040)
        a = rp.uniform(0.3, 1.0) * math.exp(-t0 / 0.15)
        s = int(t0 * SR)
        ph = rp.random()
        for i in range(s, min(n, s + int(d * 7 * SR))):
            t = (i - s) / float(SR)
            pings[i] += a * math.sin(TWO_PI * (ph + f * t)) * min(1.0, t / 0.0003) * math.exp(-t / d)
    groan = [0.0] * n
    for f, a, d in ((97.0, 1.0, 0.55), (163.0, 0.7, 0.4), (251.0, 0.45, 0.3), (419.0, 0.25, 0.2)):
        ph = rng("hit", "groan", str(f)).random()
        s = int(0.02 * SR)
        for i in range(s, n):
            t = (i - s) / float(SR)
            groan[i] += a * math.sin(TWO_PI * ph) * min(1.0, t / 0.01) * math.exp(-t / d)
            ph += f * (1.0 - 0.06 * min(1.0, t / 0.6)) / SR
    rattle = grains_decaying(r, n, 260, 0.03, 0.8, 0.0006, 0.004, 0.3)
    rattle = mul(rattle, taper(n, 0.03, 0.8))
    rattle = lp(mix((1.0, bp(rattle, 2500.0, 1.2)), (0.6, bp(rattle, 900.0, 1.0))), 6000.0)
    y = mix((0.35, norm_rms(crack)), (1.0, norm_rms(boom)), (0.8, norm_rms(blast)), (0.30, norm_rms(pings)),
            (0.55, norm_rms(groan)), (0.30, norm_rms(rattle)))
    y = soft_clip(scale(y, 1.0 / peak_of(y)), 2.0)
    y = hp(lp(y, 8000.0), 25.0)
    return fade_out(y, 0.5)


def bell(f, dur, start_pad=0.0):
    """Struck bell-like note: inharmonic partials with their own decays."""
    n = int((dur + start_pad) * SR)
    s = int(start_pad * SR)
    out = [0.0] * n
    for ratio, a, d in ((1.0, 1.0, 0.45), (2.0, 0.30, 0.28), (2.76, 0.22, 0.16), (4.07, 0.08, 0.08),
                        (5.40, 0.04, 0.05)):
        fr = f * ratio
        for i in range(s, n):
            t = (i - s) / float(SR)
            out[i] += a * math.sin(TWO_PI * fr * t) * min(1.0, t / 0.002) * math.exp(-t / d)
    return out


def shot_kill():
    """SPLASH chime, 1.0 s: two bell notes, C6 (1047 Hz) then G6 (1568 Hz) 0.13 s later. Rewarding, soft."""
    n = int(1.0 * SR)
    y = [0.0] * n
    place(y, bell(1047.0, 1.0), 0.0)
    place(y, bell(1568.0, 1.0), 0.13)
    return fade_out(y, 0.2)


def shot_jet_pass():
    """Enemy jet fly-by, 3.0 s, closest at 0.6 s: intake whine 2600 Hz x doppler (1.22 -> 0.80) loud on the
    approach, broadband roar whose low-pass falls 6 kHz -> 1.2 kHz, exhaust rumble heavier after the pass."""
    T = 3.0
    n = int(T * SR)
    tc = 0.6
    r = rng("jet_pass")
    dop = [1.01 - 0.21 * math.tanh((i / float(SR) - tc) / 0.18) for i in range(n)]
    whine = osc([2600.0 * d for d in dop], ((1, 1.0), (2, 0.2)), r.random())
    fan = osc([1150.0 * d for d in dop], ((1, 1.0),), r.random())
    w_env = lorentz_env(n, tc - 0.05, 0.25, 0.25)
    after = [0.5 + 0.5 * math.tanh((i / float(SR) - tc) / 0.15) for i in range(n)]
    tone = mul(mix((1.0, whine), (0.6, fan)), [e * (1.0 - 0.75 * a) for e, a in zip(w_env, after)])
    fc = [1200.0 + 4800.0 * (1.0 - a) for a in after]
    roar = mul(biquad_vary(white(r, n), "lp", fc, 0.7071), lorentz_env(n, tc, 0.30, 0.65))
    rumble = mul(lp(bp(white(r, n), 160.0, 0.6), 700.0),
                 [e * (0.3 + 0.7 * a) for e, a in zip(lorentz_env(n, tc + 0.15, 0.35, 0.8), after)])
    y = mix((0.5, norm_rms(tone)), (1.0, norm_rms(roar)), (0.9, norm_rms(rumble)))
    y = hp(y, 30.0)
    y = scale(y, 1.0 / peak_of(y))
    ni = int(0.15 * SR)
    for i in range(ni):
        y[i] *= i / float(ni)
    return fade_out(y, 1.0)


def shot_expl_air():
    """Warhead detonation / aircraft destroyed in the air, 2.5 s: sharp crack, boom, short fireball roar,
    fragment whizz and debris crackle, no long ground rumble (A5's expl_near/expl_far are for crashes)."""
    T = 2.5
    n = int(T * SR)
    r = rng("expl_air")
    crack = hp(noise_burst(r, n, 0.004), 1500.0)
    boom = thump(n, 120.0, 35.0, 0.25, 0.40, attack=0.002)
    nz = white(r, n)
    fire = [nz[i] * min(1.0, (i / float(SR)) / 0.004) * math.exp(-(i / float(SR)) / 0.4) for i in range(n)]
    fire = lp(lp(fire, 700.0), 900.0)
    frag = grains_decaying(r, n, 300, 0.02, 0.6, 0.002, 0.012, 0.25)
    frag = mul(frag, taper(n, 0.02, 0.6))
    frag = lp(mix((1.0, bp(frag, 3000.0, 1.0)), (0.7, bp(frag, 1500.0, 1.2))), 7000.0)
    deb = grains_decaying(r, n, 160, 0.1, 1.4, 0.0006, 0.003, 0.6)
    deb = lp(bp(mul(deb, taper(n, 0.1, 1.4)), 2200.0, 1.0), 6000.0)
    y = mix((0.45, norm_rms(crack)), (1.0, norm_rms(boom)), (0.85, norm_rms(fire)), (0.30, norm_rms(frag)),
            (0.20, norm_rms(deb)))
    y = soft_clip(scale(y, 1.0 / peak_of(y)), 2.0)
    y = hp(lp(y, 9000.0), 25.0)
    return fade_out(y, 0.6)


def shot_no_msl():
    """NO MSL: two dry switch clicks 60 ms apart, 0.15 s."""
    n = int(0.15 * SR)
    r = rng("no_msl")
    y = [0.0] * n
    for t0, g in ((0.0, 1.0), (0.06, 0.7)):
        s = int(t0 * SR)
        clk = bp(noise_burst(r, n, 0.005, start=s), 2600.0, 1.6)
        knk = thump(n, 420.0, 300.0, 0.012, 0.018, start=s)
        y = mix((1.0, y), (g, norm_rms(clk)), (0.7 * g, norm_rms(knk)))
    return soft_clip(scale(y, 1.0 / peak_of(y)), 2.0)


SHOT_FUNCS = {"msl_launch": shot_msl_launch, "msl_pass": shot_msl_pass, "lock_beep": shot_lock_beep,
              "rwr_search": shot_rwr_search, "rwr_launch": shot_rwr_launch, "flare": shot_flare, "hit": shot_hit,
              "kill": shot_kill, "jet_pass": shot_jet_pass, "expl_air": shot_expl_air, "no_msl": shot_no_msl}
LOOP_FUNCS = {"srm_growl": loop_srm_growl, "srm_lock": loop_srm_lock, "rwr_lock": loop_rwr_lock,
              "rwr_missile": loop_rwr_missile, "msl_motor": loop_msl_motor}


# ---------------------------------------------------------------------------------------------
# Levels: suggested volumes from designed perceived levels
# ---------------------------------------------------------------------------------------------
# Every file peaks at -1 dBFS (best resolution). The README's suggested e_audio volume is computed so that the
# sound's perceived level (loud_db above) lands on a designed target, on the same scale as A5's pack:
#   cruise engine (eng_mil at weight ~0.93) about -18 dB, full afterburner mix about -11.5 dB,
#   wind at 800 kt about -19.5 dB, PULL UP at its 0.55 about -9 dB, message beep at 0.5 about -11 dB.
# Warnings sit 2-7 dB above the cruise engine (tones cut through broadband noise far better than their level
# suggests) and below PULL UP; continuous tones are quieter than pulsed ones (a steady tone tires the ear).
# Impulsive one-shots measure low on this RMS scale (A5's expl_near = -13.8 dB at vol 1), so their targets are
# set on A5's explosion level, not above it.
# A9b: the RWR tones are continuous 455-555 Hz tones (user spec), where the engine/wind background is denser than
# at A9's 784-1568 Hz, so their targets went up 1-4 dB to keep the A9 margins over full afterburner (missile ~+13 dB,
# lock ~+9 dB, waterfall ~+7 dB; build log `mask` lines); still below PULL UP (-9 dB).
TARGET_DB = {
    "rwr_missile": -10.5, "rwr_launch": -10.5, "rwr_lock": -14.0, "rwr_search": -15.0,
    "srm_lock": -16.0, "srm_growl": -14.0, "lock_beep": -12.5, "no_msl": -20.0,
    "msl_launch": -14.0, "msl_pass": -14.5, "msl_motor": -15.0, "jet_pass": -14.0,
    "flare": -19.5, "hit": -14.0, "kill": -13.0, "expl_air": -14.0,
}
GROWL_SEARCH_DB = -6.0                # growl volume with no target near = vol x db(-6) (the "searching" floor)


def _jumps(pcm, idx):
    """Largest |first difference| and |second difference| of the (cyclic) waveform around sample boundary idx
    (between idx-1 and idx), checked over idx-2 .. idx+1."""
    n = len(pcm)
    w = [pcm[(idx + k) % n] for k in range(-3, 3)]
    d1 = max(abs(w[k + 1] - w[k]) for k in range(1, 4))
    d2 = max(abs(w[k + 1] - 2 * w[k] + w[k - 1]) for k in range(1, 5))
    return d1, d2


def rwr_click_report(pcms, log):
    """A9b click check (log only): sample steps at the loop seams, at every frequency switch and at the
    rwr_launch -> rwr_missile join, against the steady tones' own maximum steps (d1 = first difference, d2 =
    second difference, in LSB). A click would show as a seam/switch step far above the steady maximum."""
    for nm, segs in (("rwr_lock", RWR_LOCK_SEGS), ("rwr_missile", RWR_MSL_SEGS)):
        pcm = pcms[nm]
        n = len(pcm)
        bounds = []
        k = 0
        for _, m in segs[:-1]:
            k += m
            bounds.append(k)
        near = set()
        for b in [0] + bounds:
            for o in range(-3, 4):
                near.add((b + o) % n)
        steady_i = [i for i in range(n) if i not in near]
        d1max = max(abs(pcm[(i + 1) % n] - pcm[i]) for i in steady_i)
        d2max = max(abs(pcm[(i + 1) % n] - 2 * pcm[i] + pcm[i - 1]) for i in steady_i)
        sd1, sd2 = _jumps(pcm, 0)
        sw = [_jumps(pcm, b) for b in bounds]
        log("click %-11s seam N-1 -> 0: d1 %d d2 %d (pcm[N-1] %d, pcm[0] %d); switches max d1 %d d2 %d; steady tone "
            "max d1 %d d2 %d LSB" % (nm, sd1, sd2, pcm[-1], pcm[0], max(a for a, _ in sw), max(b for _, b in sw),
                                     d1max, d2max))
    la = pcms["rwr_launch"]
    ms = pcms["rwr_missile"]
    j = la[-3:] + ms[:3]
    d1 = max(abs(j[k + 1] - j[k]) for k in range(5))
    d2 = max(abs(j[k + 1] - 2 * j[k] + j[k - 1]) for k in range(1, 5))
    inner = [_jumps(la, b) for b in (2205, 4410, 6615, 8820)]
    log("click rwr_launch  -> rwr_missile join: d1 %d d2 %d (launch[N-1] %d, missile[0] %d); launch switches max d1 "
        "%d d2 %d LSB" % (d1, d2, la[-1], ms[0], max(a for a, _ in inner), max(b for _, b in inner)))


def suggest_vol(name, level):
    v = db(TARGET_DB[name] - level)
    return clamp(round(v * 20.0) / 20.0, 0.05, 1.0)


# ---------------------------------------------------------------------------------------------
# Preview: a scripted fight (offline mix like snd.cpp, with the README volumes and priority rules)
# ---------------------------------------------------------------------------------------------

PREVIEW_T = 44.0
MASTER = 0.7


def _read_wav(path):
    w = wave.open(path, "rb")
    d = w.readframes(w.getnframes())
    w.close()
    return [v / 32768.0 for v in struct.unpack("<%dh" % (len(d) // 2), d)]


def _smooth(t, t0, t1):
    """0 before t0, 1 after t1, smoothstep between."""
    if t <= t0:
        return 0.0
    if t >= t1:
        return 1.0
    u = (t - t0) / (t1 - t0)
    return u * u * (3.0 - 2.0 * u)


def preview_flight(t):
    """(throttle state, V kt, G) of the preview: NORMAL cruise, afterburner + break turn while defending."""
    if 22.0 <= t < 30.0:
        return "FAST", 480.0 + 220.0 * _smooth(t, 22.0, 28.0), 1.0 + 5.0 * _smooth(t, 23.0, 24.5) * (
            1.0 - _smooth(t, 28.5, 30.0))
    if t >= 30.0:
        return "NORMAL", 700.0 - 180.0 * _smooth(t, 30.0, 36.0), 1.0
    return "NORMAL", 480.0, 1.0


def render_preview(pcms, vols, data_sfx):
    """pcms: this pack's PCM; vols: suggested volumes; data_sfx: dict name -> float list of A5 loops."""
    snd = dict((k, [s / 32768.0 for s in v]) for k, v in pcms.items())
    snd.update(data_sfx)
    n = int(PREVIEW_T * SR)
    out = [0.0] * n

    # --- the scripted fight (times in s) -------------------------------------------------------
    growl_on, lock_at, srm_fire = 9.0, 13.0, 14.6
    rwr_lock_on, launch_at, warn_off = 17.0, 21.0, 28.4
    pass_at = 28.0 - 0.45                 # msl_pass closest approach at 28.0
    motor_on, motor_off = 30.5, 34.5      # a stray missile crossing ahead: msl_motor with doppler
    jet_at = 35.0 - 0.6                   # jet_pass closest approach at 35.0
    kill_boom = 37.0
    hit_at = 40.0
    shots = [(1.0, "rwr_search", 1.0, 1.0), (2.2, "rwr_search", 1.0, 1.0), (3.0, "lock_beep", 1.0, 1.0),
             (5.0, "msl_launch", 1.0, 1.0),
             (srm_fire, "msl_launch", 1.0, 1.04), (launch_at, "rwr_launch", 1.0, 1.0),
             (22.0, "ab_light", 0.8, 1.0),
             (24.0, "flare", 1.0, 1.0), (24.5, "flare", 1.0, 1.0), (25.0, "flare", 1.0, 1.0),
             (pass_at, "msl_pass", 1.0, 1.0), (jet_at, "jet_pass", 0.8, 1.0),
             (kill_boom, "expl_air", 0.55, 1.0), (kill_boom + 0.35, "kill", 1.0, 1.0),
             (hit_at, "hit", 1.0, 1.0), (43.0, "no_msl", 1.0, 1.0)]

    # loop voices: name -> function(t) -> (vol, pitch); vol 0 = silent (pattern loops restart at position 0)
    def growl(t):
        if t < growl_on or t >= lock_at:
            return 0.0, 1.0
        s = _smooth(t, growl_on + 1.0, lock_at - 0.3)                     # seeker signal 0 -> 1
        g = vols["srm_growl"] * db(GROWL_SEARCH_DB * (1.0 - s))
        if launch_at <= t < warn_off:
            g *= db(-6.0)
        return g, 1.0 + 0.25 * s

    def srm_lock(t):
        return (vols["srm_lock"] if lock_at <= t < srm_fire else 0.0), 1.0

    def rwr_lock(t):
        on = rwr_lock_on <= t < launch_at                                 # missile warning replaces it
        return (vols["rwr_lock"] if on else 0.0), 1.0

    def rwr_missile(t):
        if not (launch_at + 0.5 <= t < warn_off):
            return 0.0, 1.0
        age = t - (launch_at + 0.5)
        # full for 2 s, then -4 dB while the threat persists, back to full in the last 2 s (close)
        g = 1.0 if age < 2.0 or t > warn_off - 2.0 else db(-4.0)
        return vols["rwr_missile"] * g, 1.0

    def motor(t):
        if not (motor_on <= t < motor_off):
            return 0.0, 1.0
        # source passes 300 m abeam at 400 m/s relative, closest at the middle
        tc = 0.5 * (motor_on + motor_off)
        x = (t - tc) * 400.0
        d = math.sqrt(x * x + 300.0 * 300.0)
        vr = 400.0 * x / d                                                # range rate (+ = opening)
        pitch = clamp(340.0 / (340.0 + vr), 0.6, 1.6)
        g = vols["msl_motor"] * min(1.0, 100.0 / d) * _smooth(t, motor_on, motor_on + 0.3) * (
            1.0 - _smooth(t, motor_off - 0.3, motor_off))
        return g, pitch

    loop_fn = {"srm_growl": growl, "srm_lock": srm_lock, "rwr_lock": rwr_lock, "rwr_missile": rwr_missile,
               "msl_motor": motor}
    eng_names = ["eng_idle", "eng_mil", "eng_ab", "wind"]
    have_eng = all(k in snd for k in eng_names)
    names = (eng_names if have_eng else []) + list(loop_fn.keys())
    pos = dict((k, 0.0) for k in names)
    cur = dict((k, (0.0, 1.0)) for k in names)
    tick = SR // 120 + 1
    # loop on/off times split the update ticks, so a loop starts on its exact sample (rwr_missile joins the end of
    # rwr_launch sample-exactly: 21.0 s + 11025 samples)
    events = sorted(set(int(round(t * SR)) for t in (growl_on, lock_at, srm_fire, rwr_lock_on, launch_at,
                                                      launch_at + 0.5, warn_off, motor_on, motor_off)))
    nsp = 0.90
    ab = 0.0
    i = 0
    while i < n:
        t = i / float(SR)
        dt = tick / float(SR)
        tgt = {}
        if have_eng:
            state, v_kt, g_load = preview_flight(t)
            nsp = spool_step(nsp, state, dt)
            ab += ((1.0 if state == "FAST" else 0.0) - ab) * (1.0 - math.exp(-dt / 0.1))
            for nm, w, p in mix_engine(nsp, ab, v_kt * 0.514444 / 340.3):
                tgt[nm] = (w, p)
            tgt["wind"] = mix_wind(v_kt, g_load)
        for nm, fn in loop_fn.items():
            tgt[nm] = fn(t)
        m = min(tick, n - i)
        for e in events:
            if i < e < i + m:
                m = e - i
                break
        for k in names:
            buf = snd[k]
            ln = len(buf)
            w0, p0 = cur[k]
            w1, p1 = tgt[k]
            p = pos[k]
            if w0 < 0.0005 and w1 < 0.0005:
                if k in loop_fn:
                    p = 0.0                                               # stopped: next start at sample 0
                else:
                    p = (p + m * p1) % ln
            else:
                if k in loop_fn and w0 < 0.0005:
                    w0 = w1                                               # au_play at full: no fade-in ramp
                for j in range(m):
                    f = j / float(m)
                    w = w0 + (w1 - w0) * f
                    q = int(p)
                    fr = p - q
                    a = buf[q]
                    b = buf[q + 1] if q + 1 < ln else buf[0]
                    out[i + j] += w * (a + (b - a) * fr)
                    p += p0 + (p1 - p0) * f
                    if p >= ln:
                        p -= ln
            pos[k] = p
            cur[k] = (w1, p1)
        i += m
    for t0, name, vmul, pitch in shots:
        if vmul <= 0.0 or name not in snd:
            continue
        buf = snd[name]
        g = (vols[name] if name in vols else 1.0) * vmul
        s = int(t0 * SR)
        j = 0
        while s + j < n:
            p = j * pitch
            q = int(p)
            if q >= len(buf):
                break
            nxt = buf[q + 1] if q + 1 < len(buf) else buf[q]       # keeps the last sample (rwr_launch join)
            out[s + j] += g * (buf[q] + (nxt - buf[q]) * (p - q))
            j += 1
    out = [v * MASTER for v in out]
    pk = peak_of(out)
    return to_pcm(fades(out, 0.05), PEAK / pk), pk


PREVIEW_SCRIPT = [
    "0-22 s NORMAL cruise (eng_mil + wind at 480 kt)",
    "1.0 rwr_search waterfall (an enemy fighter radar detects us), 2.2 rwr_search (a second fighter)",
    "3.0 lock_beep (S long: own radar lock), 5.0 msl_launch (LRM)",
    "9.0-13.0 srm_growl: searching floor, then the seeker signal rises (volume up, pitch 1.0 -> 1.25)",
    "13.0-14.6 srm_lock (growl replaced), 14.6 msl_launch (SRM; lock tone stops)",
    "17.0-21.0 rwr_lock (enemy locks us)",
    "21.0 rwr_launch, 21.5-28.4 rwr_missile loop joined sample-exactly: one continuous 455/555 Hz 0.10 s tone "
    "from 21.0 (replaces rwr_lock; -4 dB after 2 s, full again near the end)",
    "22.0 afterburner (ab_light + eng_ab), 5 g break turn; 24.0/24.5/25.0 flare bursts",
    "28.0 msl_pass (the missile misses), warning off",
    "30.5-34.5 msl_motor: a stray missile crossing 300 m ahead (distance law + doppler)",
    "35.0 jet_pass (closest approach), 37.0 expl_air (our missile's kill, vol x 0.55 for ~2 km; the distance "
    "delay is left out), 37.35 kill",
    "40.0 hit (we are hit), 43.0 no_msl",
]


# Tone-to-masker check (log only; reads A5's loops from data/sfx, so it never goes into README_combat.md):
# level of each warning tone in a 1/3-octave band around its main frequencies vs the same band of the engine +
# wind background at cruise (NORMAL 480 kt) and in full afterburner at 700 kt, at the suggested volumes.
MASK_BANDS = {"srm_growl": (400.0, 800.0, 1200.0), "srm_lock": (1600.0,), "rwr_lock": (455.0, 555.0),
              "rwr_missile": (455.0, 555.0), "rwr_launch": (455.0, 555.0), "lock_beep": (1568.0, 2093.0),
              "rwr_search": (550.0, 470.0), "kill": (1047.0, 1568.0), "pullup": (1000.0,)}
MASK_TONE_WIN = ("rwr_lock", "rwr_missile")   # alternating loops: level while one tone sounds (50 ms windows)


def _band_db(x, f, win=None):
    y = bp(bp(x, f, 4.32), f, 4.32)
    if win is None:
        return 20.0 * math.log10(max(1e-9, rms(y)))
    best = 1e-9
    for k in range(0, max(1, len(y) - win), win // 2):
        best = max(best, rms(y[k:k + win]))
    return 20.0 * math.log10(best)


def masking_report(pcms, vols, data, log):
    need = ("eng_idle", "eng_mil", "eng_ab", "wind")
    if not all(k in data for k in need):
        return
    N = SR

    def seg(k, g):
        b = data[k]
        return [g * b[i % len(b)] for i in range(N)]
    cruise = mix((0.38, seg("eng_idle", 1.0)), (0.93, seg("eng_mil", 1.0)), (0.20, seg("wind", 1.0)))
    burner = mix((1.0, seg("eng_ab", 1.0)), (0.65, seg("eng_mil", 1.0)), (0.42, seg("wind", 1.0)))
    for nm, bands in sorted(MASK_BANDS.items()):
        if nm == "pullup":
            if "pullup" not in data:
                continue
            x = [0.55 * v for v in data["pullup"]]
            loop = True
        else:
            x = [vols[nm] * v / 32768.0 for v in pcms[nm]]
            loop = nm in LOOP_NAMES
        win = None if loop else int(0.1 * SR)
        if nm in MASK_TONE_WIN:
            while len(x) < SR:
                x = x + x
            win = int(0.05 * SR)
        best = None
        for f in bands:
            t_db = _band_db(x, f, win)
            c = t_db - _band_db(cruise, f)
            a = t_db - _band_db(burner, f)
            if best is None or a > best[2]:
                best = (f, c, a)
        extra = ""
        if nm == "srm_growl":
            extra = " (searching floor %+.0f dB lower)" % GROWL_SEARCH_DB
        log("mask %-11s band %4.0f Hz: %+5.1f dB over cruise, %+5.1f dB over afterburner%s" % (
            nm, best[0], best[1], best[2], extra))


# ---------------------------------------------------------------------------------------------
# README_combat.md
# ---------------------------------------------------------------------------------------------

LOOP_INFO = {
    "srm_growl": ("2.000", "1.0 = 400 Hz growl (searching)", "SRM selected, seeker searching / tracking heat"),
    "srm_lock": ("1.000", "1.0 = 1600 Hz", "SRM seeker locked (DESIGN 10.6 high lock tone)"),
    "rwr_lock": ("1.000", "1.0 = 455/555 Hz, 0.25 s each (2 cycles)",
                 "an enemy fire-control radar locks the player (STT, 11.5)"),
    "rwr_missile": ("0.200", "1.0 = 455/555 Hz, 0.10 s each (1 cycle, 455 first)",
                    "a hostile missile is homing on the player (guidance uplink / active seeker, 11.5)"),
    "msl_motor": ("1.500", "1.0 = no doppler", "rocket motor of other missiles, positional"),
}
SHOT_INFO = {
    "msl_launch": "own launch (LRM or SRM); rail clunk, ignition, motor pulling away",
    "msl_pass": "hostile missile misses within 200 m; closest approach at 0.45 s into the file",
    "lock_beep": "own radar lock when S long designates (11.4)",
    "rwr_search": "\"waterfall\" 550/520/490/470 Hz: a new enemy fighter radar detects the player (at most 1 per s)",
    "rwr_launch": "missile warning onset = the rwr_missile tone for 0.5 s (555 first, no end fade); rwr_missile "
                  "loop joins at 0.5 s",
    "flare": "one flare burst of 2 (F; hold repeats every 0.5 s)",
    "hit": "the player is hit (missile damage, 14.1)",
    "kill": "SPLASH: our missile destroyed a target",
    "jet_pass": "enemy jet within 1.5 km; closest approach at 0.6 s into the file",
    "expl_air": "enemy destroyed / missile detonates in the air",
    "no_msl": "Space with no missile of the selected type (12.6)",
}


def readme_text(info, levels, vols):
    L = []
    a = L.append
    a("# data/sfx: jet sound pack v2, combat (generated by `tools/sfx/gen_sfx_combat.py`, tasks A9 + A9b; do not "
      "edit)")
    a("")
    a("All files: mono, 22050 Hz, 16-bit PCM, every file peaks at -1 dBFS. Regenerate with")
    a("`python3 tools/sfx/gen_sfx_combat.py` (deterministic; `--check` compares md5 with data/), or")
    a("`python3 tools/build_assets.py --only sfx` (both sound generators). The v1 flight pack and its engine mix law")
    a("are in `README.md` (A5); this file covers only the combat sounds. Loops are seamless: play them with")
    a("DSBPLAY_LOOPING from sample 0. Pitch = e_audio `pitch` (rate multiplier of 22050 Hz).")
    a("")
    a("Levels: the *suggested volume* is the e_audio `vol` (before `[sound]` group volumes and the master) that")
    a("puts the sound at its designed perceived level on A5's scale: cruise engine about -18 dB, full afterburner")
    a("about -11.5 dB, PULL UP (0.55) about -9 dB. `Level` = measured perceived level of the file at vol 1 (band")
    a("100 Hz - 6 kHz RMS; loops over the whole loop, one-shots over their loudest 300 ms). Master ~0.7 as in A5.")
    a("")
    a("## Loops")
    a("")
    a("| File | Samples | Seconds | Base pitch | Level | Suggested volume | Use |")
    a("|---|---|---|---|---|---|---|")
    for nm in LOOP_NAMES:
        s, pk = info[nm]
        sec, base, use = LOOP_INFO[nm]
        vtxt = "%.2f" % vols[nm]
        if nm == "srm_growl":
            vtxt = "%.2f x db(-6 x (1 - s))" % vols[nm]
        elif nm == "msl_motor":
            vtxt = "%.2f x min(1, 100 m / d)" % vols[nm]
        a("| `%s.wav` | %d | %s | %s | %.1f dB | %s | %s |" % (nm, s, sec, base, levels[nm], vtxt, use))
    a("")
    a("## One-shots (5 ms fades; rwr_launch: 4 ms attack, no end fade)")
    a("")
    a("| File | Samples | Seconds | Level | Suggested volume | Use |")
    a("|---|---|---|---|---|---|")
    for nm in SHOT_NAMES:
        s, pk = info[nm]
        vtxt = "%.2f" % vols[nm]
        if nm == "expl_air":
            vtxt = "%.2f x distance law" % vols[nm]
        elif nm == "jet_pass":
            vtxt = "%.2f x min(1, 150 m / miss)" % vols[nm]
        a("| `%s.wav` | %d | %.3f | %.1f dB | %s | %s |" % (nm, s, s / float(SR), levels[nm], vtxt, SHOT_INFO[nm]))
    a("")
    a("## The warning tones: how to tell them apart")
    a("")
    a("| Sound | Register | Rhythm | Character |")
    a("|---|---|---|---|")
    a("| `srm_growl` | low, 400 Hz (pitch up to 500 Hz with signal) | continuous | raspy buzz with waver and flutter |")
    a("| `srm_lock` | high, 1600 Hz | continuous | clean steady tone |")
    a("| `lock_beep` | high, 1568 -> 2093 Hz | 3 fast pips + held note, once | bright confirmation |")
    a("| `rwr_search` | low-mid, 550 -> 470 Hz | one falling 4-step \"waterfall\" (0.335 s), once | new fighter |")
    a("| `rwr_lock` | low-mid, 455/555 Hz | slow two-tone alternation, 2 tones/s (0.25 s each), continuous |"
      " calm, insistent |")
    a("| `rwr_launch` + `rwr_missile` | low-mid, 455/555 Hz | fast two-tone alternation, 5 tones/s (0.10 s each),"
      " continuous | urgent |")
    a("| A5 `pullup` | 700 -> 1400 Hz | 2 rising whoops/s | terrain warning |")
    a("| A5 `beep` | 1050 Hz | bip-bip, once | message |")
    a("")
    a("Engine bands for reference: fan tone 750-1220 Hz and whine 1980-3230 Hz (they glide with spool). The RWR")
    a("tones (user spec, A9b) sit at 455-555 Hz, below the fan band: a clean steady pitch under the engine's")
    a("broadband roar, so they stand out by register and by their regular switching (the engine glides, it never")
    a("steps). **rwr_lock and rwr_missile use the same two pitches and differ only by rhythm**: 2 tones/s vs 5")
    a("tones/s (calm alternation vs fast trill), and rwr_search is a single falling run of 4 pitches. srm_growl")
    a("(400-500 Hz) shares the register but is a raspy buzz with flutter, never a clean alternation. Margins in")
    a("the tone's 1/3-octave band at the suggested volumes (build log `mask` lines, A9b build), over cruise / over")
    a("full afterburner at 700 kt: rwr_missile +18.1 / +12.9 dB, rwr_lock +14.5 / +9.4 dB, rwr_search +12.3 /")
    a("+7.2 dB (A5 PULL UP +14.0 / +10.0 dB): the missile tone stays clearly audible in full burner, the lock")
    a("tone a bit calmer.")
    a("A9's tones are sines with weak low harmonics; the RWR tones are near-pure sines (3rd harmonic -24 dB, 5th")
    a("-36 dB) with phase-continuous switches; 3-10 ms raised-cosine attacks/releases (no clicks, no square edge).")
    a("")
    a("## How to play them (snd_combat.cpp, T17)")
    a("")
    a("- **srm_growl / srm_lock** (SRM selected): keep both loops running from `sndc_init` at the -74 dB floor")
    a("  (as snd.cpp does with the engine) so they never restart audibly. Seeker signal `s` 0..1 (target heat in")
    a("  the seeker cone, e.g. 1 - angle/gimbal x range factor): growl `vol = %.2f x db(-6 x (1 - s))`, `pitch = 1 +"
      % vols["srm_growl"])
    a("  0.25 x s`. On lock: growl to the floor, srm_lock to %.2f (pitch 1). Lock lost or missile fired: back to the"
      % vols["srm_lock"])
    a("  growl. No SRM selected: both at the floor. No `lock_beep` for the SRM lock (the tone is the cue).")
    a("- **lock_beep**: once when S long designates and the radar lock (STT) is achieved; not on re-locks within 2 s.")
    a("- **rwr_lock**: `au_play` at position 0 (loop, vol %.2f) when the first enemy fire-control radar locks the"
      % vols["rwr_lock"])
    a("  player (STT), `au_stop` when no enemy has a lock any more. One voice no matter how many enemies lock.")
    a("- **rwr_launch + rwr_missile** (one continuous 455/555 Hz tone, 0.10 s per tone): on the first hostile missile")
    a("  homing on the player (11.5 rules: LRM at launch, SRM within 5 km): rwr_launch once (vol %.2f, the same as"
      % vols["rwr_launch"])
    a("  the loop), then `au_play` rwr_missile (loop, position 0, vol %.2f) 0.5 s later. rwr_launch ends on 555 Hz"
      % vols["rwr_missile"])
    a("  at a whole cycle and the loop starts on 455 Hz at phase 0, so a sample-exact start is seamless. au has no")
    a("  sample-exact chaining: start the loop when `au_playing(launch voice)` turns 0 or 0.5 s after the launch,")
    a("  whichever comes first; both ends sit at a zero crossing, so a frame of gap is a tiny dropout, not a click.")
    a("  (Equivalent and simpler: skip rwr_launch and start rwr_missile at once; it sounds the same, only")
    a("  starting on 455 Hz.) Stop it when no hostile missile is homing on the player. A second missile while the")
    a("  warning runs: **do not** play rwr_launch again (two copies of the alternation out of step would beat);")
    a("  the loop keeps going and the anti-fatigue level resets to full. **Anti-fatigue rule**: full volume for")
    a("  the first 2 s of a warning, then -4 dB while it persists; back to full when the nearest homing missile is")
    a("  < 2 km (TTG short) or a new missile is launched.")
    a("- **rwr_search** (\"waterfall\", vol %.2f): once for every new enemy fighter (airborne-intercept) radar that"
      % vols["rwr_search"])
    a("  first detects the player (it is added to the threat table); rate-limit to one per 1 s (a second new radar")
    a("  within 1 s is not announced again) and never while rwr_lock or rwr_missile play.")
    a("- **Stopping continuous tones** (rwr_lock, rwr_missile, srm_lock): the waveform is never silent, so a hard")
    a("  `au_stop` mid-cycle can tick; prefer 2-3 frames of falling `au_set` volume (x0.5, x0.25) before `au_stop`.")
    a("- **msl_launch**: own launch at the listed volume, pitch 1.0 (LRM) / 1.04 (SRM, a little brighter). Enemy")
    a("  launches: positional (vol x min(1, 300 m / d), pan by bearing), usually inaudible beyond 2 km.")
    a("- **msl_motor**: one voice per audible missile, nearest 3 only (au pool is 32 voices): `vol = %.2f x"
      % vols["msl_motor"])
    a("  min(1, 100 m / d)`, `pitch = clamp(340 / (340 + range_rate), 0.6, 1.6)` (range_rate in m/s, + = opening),")
    a("  pan by bearing. Own missiles: fade out over the first 2 s after launch (msl_launch already carries it).")
    a("- **msl_pass**: hostile missile that misses within 200 m: trigger 0.45 s before the predicted closest approach")
    a("  (or at the miss detection; the 0.45 s lead is just cosmetic), `vol x min(1, 50 m / miss)` floor 0.4.")
    a("- **jet_pass**: enemy within 1.5 km closing: trigger when time to closest approach <= 0.6 s and the miss")
    a("  distance < 600 m; `vol = %.2f x min(1, 150 m / miss)`, pitch 0.9-1.1 by closing speed; once per enemy per"
      % vols["jet_pass"])
    a("  8 s. Distinct from the player's own engine (it glides down through the pass).")
    a("- **flare**: once per burst (F; hold repeats every 0.5 s, the file is 0.55 s).")
    a("- **hit**: own damage, vol %.2f. **expl_air**: kills and air detonations: %.2f within 1 km, then x (1 km / d)"
      % (vols["hit"], vols["expl_air"]))
    a("  with the sound delayed by d / 340 m/s (A5's `expl_far` for ground/sea impacts and > 4 km). **kill**: 0.35 s")
    a("  after the kill (with the SPLASH message), not delayed by distance.")
    a("- **no_msl**: on Space with the selected weapon empty.")
    a("")
    a("## Priorities and ducking")
    a("")
    a("1. **PULL UP** (A5) and **rwr_missile / rwr_launch**: never ducked. While PULL UP plays, rwr_missile and")
    a("   rwr_launch -3 dB (both, so the joined tone keeps one level).")
    a("2. **hit**, **msl_launch** (own), **lock_beep**, **srm_lock**: never ducked; growl and srm_lock never together.")
    a("3. **rwr_lock**: muted while rwr_missile plays (the missile warning replaces it).")
    a("4. **srm_growl**: -6 dB while rwr_missile plays (you can still hear the seeker find a target).")
    a("5. **rwr_search**: skipped while rwr_lock or rwr_missile play (never queued for later).")
    a("6. Positional sounds (msl_motor, msl_pass, jet_pass, expl_air, enemy launches): no ducking; on voice")
    a("   shortage drop the farthest first. Warning/tone voices are reserved (never stolen).")
    a("Group volumes (suggested `[sound]` keys for T17): `warn` (rwr_*, srm_*, lock_beep, no_msl) 1.0, `combat`")
    a("(launches, motors, passes, explosions, hit, flare, kill) 1.0.")
    a("")
    a("## Preview")
    a("")
    a("`python3 tools/sfx/gen_sfx_combat.py` also writes `tools/sfx/preview_combat.wav` (%.0f s, not under `--check`):"
      % PREVIEW_T)
    a("A5's engine/wind loops with A5's mix law plus these sounds at exactly the volumes and rules above, master %.1f,"
      % MASTER)
    a("normalised afterwards. Script:")
    a("")
    for s in PREVIEW_SCRIPT:
        a("- " + s)
    a("")
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------------------------
# Build (assetkit protocol)
# ---------------------------------------------------------------------------------------------

def build(out_dir, kit):
    log = kit.log
    info = {}
    pcms = {}
    levels = {}
    bad = 0
    gains = {}
    for nm in LOOP_NAMES:
        x, conv = LOOP_FUNCS[nm]()
        g = PEAK / peak_of(x)
        gains[nm] = g
        pcm = to_pcm(x, g)
        pcms[nm] = pcm
        levels[nm] = loud_db([s / 32768.0 for s in pcm], True)
        bad += 0 if put_wav(kit, nm, pcm, info, conv * g * 32767.0, True, log) else 1
    for nm in SHOT_NAMES:
        x = SHOT_FUNCS[nm]()
        if nm in RWR_JOIN_SHOTS:
            pcm = to_pcm(x, gains["rwr_missile"])     # same scale as the loop it joins (no own fades/normalising)
        else:
            x = fades(x)
            pcm = to_pcm(x, PEAK / peak_of(x))
        pcms[nm] = pcm
        levels[nm] = loud_db([s / 32768.0 for s in pcm], False)
        put_wav(kit, nm, pcm, info, log=log)
    vols = dict((nm, suggest_vol(nm, levels[nm])) for nm in LOOP_NAMES + SHOT_NAMES)
    vols["rwr_launch"] = vols["rwr_missile"]          # one continuous tone: the onset plays at the loop's volume
    rwr_click_report(pcms, log)
    for nm in LOOP_NAMES + SHOT_NAMES:
        log("level %-11s %6.1f dB  target %6.1f  suggested vol %.2f" % (nm, levels[nm], TARGET_DB[nm], vols[nm]))
    kit.save_text("sfx/README_combat.md", readme_text(info, levels, vols))
    if bad:
        raise RuntimeError("gen_sfx_combat: %d loop seam check(s) failed" % bad)
    if kit.previews:
        data = {}
        for nm in ("eng_idle", "eng_mil", "eng_ab", "wind", "ab_light", "pullup"):
            p = kit.find("sfx/%s.wav" % nm)
            if p:
                data[nm] = _read_wav(p)
        masking_report(pcms, vols, data, log)
        if len(data) < 6:
            log("preview: A5 files missing in data/sfx (run gen_sfx first); mixing without engine/wind")
        vols_p = dict(vols)
        vols_p["ab_light"] = 1.0
        pcm, pk = render_preview(pcms, vols_p, data)
        write_wav(PREVIEW_WAV, pcm)
        log("preview %s (%.1f s, mix peak %.2f before normalising)" % (os.path.relpath(PREVIEW_WAV, ROOT),
                                                                     len(pcm) / float(SR), pk))


if __name__ == "__main__":
    sys.path.insert(0, os.path.join(ROOT, "engine", "tools"))
    from assetkit import build as _B
    sys.exit(_B.main_for(__file__))
