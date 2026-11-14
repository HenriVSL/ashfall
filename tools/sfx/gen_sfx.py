#!/usr/bin/env python3
"""Jet sound pack v1 (flight), task A5: deterministic synthesis of the flight sounds (DESIGN 15, 8.8).

Writes mono 22050 Hz 16-bit PCM WAVs into data/sfx/:

  eng_idle.wav eng_mil.wav eng_ab.wav     turbine loops (3.0 s, seamless), crossfaded by spool + afterburner
  wind.wav brake.wav roll.wav             loops: air rush (4.0 s), speed brake buffet (2.0 s), runway roll (2.0 s)
  pullup.wav                              loop: PULL UP warning "whoop-whoop" (1.0 s)
  ab_light.wav gear_up.wav gear_clunk.wav one-shots: afterburner light-up, gear retraction motor, uplock clunk
  expl_near.wav expl_far.wav              one-shots: explosion close by / far away
  ui_move.wav ui_ok.wav ui_back.wav beep.wav   one-shots: menus, message beep
  README.md                               list, loop lengths, base pitch, mix law, suggested volumes

and, when previews are on (normal runs, not --check), tools/sfx/preview_flight.wav: an offline mix of a scripted
flight (ground idle, afterburner takeoff roll, gear up, mil cruise, afterburner run to Mach 1.4 with a pull, mil,
speed brake, mil) with exactly the mix law of the README (mix_engine(), mix_wind(), ... below).

Engine model (an original single-shaft-looking afterburning turbofan, heard from the cockpit/chase camera):
  * tonal part: fan tone (1150 Hz x N), compressor whine (3050 Hz x N, with 2nd harmonic), both amplitude
    modulated at the shaft rate (150 Hz x N: sidebands) with a slow periodic wobble; "buzz-saw" shaft harmonics
    (orders 4..14 with fixed random amplitudes) strong at mil, weak at idle;
  * noise part: core rumble (< 100 Hz), jet mixing roar (band-pass around 350 Hz at mil), mid rush, intake hiss;
  * afterburner loop: a deep broadband roar (band around 160 Hz), low rumble, a throb (6-12 Hz amplitude
    wobble) and crackle (sparse sharp pressure spikes with heavy-tailed amplitudes), almost no whine.
  N = spool fraction; eng_idle is synthesised at N = 0.65, eng_mil at N = 1.0, so with the README pitch law
  (pitch = N / N_base) both voices put their tones on the same frequencies at every moment of the crossfade.
Seamless loops: every tone is a whole number of cycles per loop (frequencies rounded to 1/T Hz, FM with zero
mean), every noise is a periodic excitation run through the (linear or periodically varying) filters for 3 periods
keeping the last one (steady()), so the seam is just another sample. The script prints a seam check per loop.

Python 3 standard library + assetkit (rng, validate via the build runner). Deterministic: same inputs = same bytes.
The assetkit has no WAV writer, so WAVs are written with the stdlib `wave` module (same as the racer's gen_sfx).

Usage:  python3 tools/sfx/gen_sfx.py [--check]      (= tools/build_assets.py --only sfx [--check])
Helpers (rng, filters, steady, seam_check, put_wav, tones, thumps, ...) are importable: A9's
tools/sfx/gen_sfx_combat.py reuses them with `from gen_sfx import ...` style imports (keep their signatures).
"""
import math
import os
import struct
import sys
import wave

SR = 22050
TWO_PI = 2.0 * math.pi
PEAK = 10.0 ** (-1.0 / 20.0)          # -1 dBFS
FADE = 0.005                          # 5 ms fades on one-shots
SEED = 5005                           # project-wide seed for this generator

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PREVIEW_WAV = os.path.join(HERE, "preview_flight.wav")

ENG_T = 3.0                           # engine loop length (s): 66150 samples
N_IDLE = 0.65                         # spool fraction at which eng_idle is synthesised
N_MIL = 1.0                           # ... eng_mil
F_FAN = 1150.0                        # fan tone at N = 1 (Hz)
F_WHINE = 3050.0                      # compressor whine at N = 1
F_SHAFT = 150.0                       # shaft rate at N = 1 (AM sidebands, buzz-saw orders)

ROLL_VREF = 40.0                      # roll.wav pitch 1.0 = 40 m/s (4 runway joints per second)

LOOP_NAMES = ["eng_idle", "eng_mil", "eng_ab", "wind", "brake", "roll", "pullup"]
SHOT_NAMES = ["ab_light", "gear_up", "gear_clunk", "expl_near", "expl_far", "ui_move", "ui_ok", "ui_back", "beep"]
ASSETS = ["sfx/%s.wav" % n for n in LOOP_NAMES + SHOT_NAMES] + ["sfx/README.md"]
ORDER = 50


# ---------------------------------------------------------------------------------------------
# Deterministic random numbers (assetkit.rng)
# ---------------------------------------------------------------------------------------------

def _kit_rng_class():
    kitdir = os.path.join(ROOT, "engine", "tools")
    if kitdir not in sys.path:
        sys.path.insert(0, kitdir)
    from assetkit.rng import Rng
    return Rng


_RNG = None


def rng(*names):
    """assetkit Rng(SEED, *names): a random.Random seeded by a stable hash (same on every run/machine)."""
    global _RNG
    if _RNG is None:
        _RNG = _kit_rng_class()
    return _RNG(SEED, *names)


# ---------------------------------------------------------------------------------------------
# DSP helpers (Python lists of floats)
# ---------------------------------------------------------------------------------------------

def biquad(x, coef):
    b0, b1, b2, a1, a2 = coef
    out = [0.0] * len(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, xi in enumerate(x):
        y = b0 * xi + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2 = x1
        x1 = xi
        y2 = y1
        y1 = y
        out[i] = y
    return out


def rbj(kind, f, q, gain_db=0.0):
    """RBJ audio-EQ-cookbook biquad coefficients (normalised, a0 = 1)."""
    w = TWO_PI * f / SR
    cw = math.cos(w)
    sw = math.sin(w)
    al = sw / (2.0 * q)
    if kind == "lp":
        b0, b1, b2 = (1 - cw) / 2, 1 - cw, (1 - cw) / 2
        a0, a1, a2 = 1 + al, -2 * cw, 1 - al
    elif kind == "hp":
        b0, b1, b2 = (1 + cw) / 2, -(1 + cw), (1 + cw) / 2
        a0, a1, a2 = 1 + al, -2 * cw, 1 - al
    elif kind == "bp":                       # constant 0 dB peak gain
        b0, b1, b2 = al, 0.0, -al
        a0, a1, a2 = 1 + al, -2 * cw, 1 - al
    elif kind == "peak":
        A = 10.0 ** (gain_db / 40.0)
        b0, b1, b2 = 1 + al * A, -2 * cw, 1 - al * A
        a0, a1, a2 = 1 + al / A, -2 * cw, 1 - al / A
    else:
        raise ValueError(kind)
    return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def lp(x, f, q=0.7071):
    return biquad(x, rbj("lp", f, q))


def hp(x, f, q=0.7071):
    return biquad(x, rbj("hp", f, q))


def bp(x, f, q):
    return biquad(x, rbj("bp", f, q))


def peak_eq(x, f, q, gain_db):
    return biquad(x, rbj("peak", f, q, gain_db))


def bp_vary(x, fc, q, period, every=32):
    """Band-pass whose centre follows fc[i % period] (coefficients refreshed every `every` samples).
    With a periodic fc and a periodic input the steady state is periodic (use inside steady())."""
    out = [0.0] * len(x)
    x1 = x2 = y1 = y2 = 0.0
    b0 = b1 = b2 = a1 = a2 = 0.0
    for i, xi in enumerate(x):
        if (i % period) % every == 0:
            b0, b1, b2, a1, a2 = rbj("bp", fc[i % period], q)
        y = b0 * xi + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2 = x1
        x1 = xi
        y2 = y1
        y1 = y
        out[i] = y
    return out


def mix(*pairs):
    """mix((gain, list), ...) -> list (all lists the same length)."""
    n = len(pairs[0][1])
    out = [0.0] * n
    for g, sig in pairs:
        if g == 0.0:
            continue
        for i in range(n):
            out[i] += g * sig[i]
    return out


def scale(x, g):
    return [v * g for v in x]


def mul(x, m):
    return [a * b for a, b in zip(x, m)]


def db(v):
    return 10.0 ** (v / 20.0)


def steady(x_period, process, periods=3):
    """Run a periodic excitation through process() for `periods` periods, keep the last one.
    Returns (last_period, convergence) where convergence = max |last - previous period|."""
    n = len(x_period)
    y = process(x_period * periods)
    last = y[(periods - 1) * n:]
    prev = y[(periods - 2) * n:(periods - 1) * n]
    conv = max(abs(a - b) for a, b in zip(last, prev))
    return last, conv


def peak_of(x):
    return max(abs(v) for v in x) if x else 0.0


def rms(x):
    return math.sqrt(sum(v * v for v in x) / max(1, len(x)))


def norm_rms(x):
    """Scale to RMS 1 (component levels are then set in dB)."""
    r = rms(x)
    return scale(x, 1.0 / r) if r > 0 else list(x)


def loudness_db(x):
    """Rough perceived level: band-limit 100 Hz - 6 kHz, then RMS in dB."""
    y = lp(hp(x, 100.0), 6000.0)
    return 20.0 * math.log10(max(1e-9, rms(y)))


def fades(x, sec=FADE):
    n = min(len(x) // 2, int(round(sec * SR)))
    y = list(x)
    for i in range(n):
        g = i / float(n)
        y[i] *= g
        y[-1 - i] *= g
    return y


def fade_out(x, sec):
    """Raised-cosine fade over the last `sec` seconds."""
    n = min(len(x), int(round(sec * SR)))
    y = list(x)
    L = len(y)
    for i in range(n):
        g = 0.5 + 0.5 * math.cos(math.pi * (i + 1) / float(n))
        y[L - n + i] *= g
    return y


def periodic_smooth_noise(r, n, points, lo=-1.0, hi=1.0):
    """Smooth periodic noise of length n: cosine-interpolated random control points (wraps)."""
    ctrl = [r.uniform(lo, hi) for _ in range(points)]
    out = [0.0] * n
    step = n / float(points)
    for i in range(n):
        p = i / step
        k = int(p)
        f = p - k
        a = ctrl[k % points]
        b = ctrl[(k + 1) % points]
        w = 0.5 - 0.5 * math.cos(math.pi * f)
        out[i] = a + (b - a) * w
    return out


def zero_mean(x):
    m = sum(x) / float(len(x))
    return [v - m for v in x]


def white(r, n):
    return [r.uniform(-1.0, 1.0) for _ in range(n)]


def whole(f, T):
    """Frequency rounded to a whole number of cycles in T seconds."""
    return round(f * T) / T


def add_pulse(buf, t0, amp, tau_r, tau_d, nbuf=None):
    """Add (1-exp(-u/tau_r))*exp(-u/tau_d) normalised to peak `amp` at fractional sample t0.
    nbuf: wrap around (periodic buffers); None: clip at the end."""
    up = tau_r * math.log(1.0 + tau_d / tau_r)
    pk = (1.0 - math.exp(-up / tau_r)) * math.exp(-up / tau_d)
    a = amp / pk
    length = int(tau_d * 7.0) + 2
    start = int(math.ceil(t0))
    er = math.exp(-1.0 / tau_r)
    ed = math.exp(-1.0 / tau_d)
    u0 = start - t0
    gr = math.exp(-u0 / tau_r)
    gd = math.exp(-u0 / tau_d)
    L = len(buf)
    for k in range(length):
        idx = start + k
        if nbuf:
            idx %= nbuf
        elif idx >= L:
            break
        buf[idx] += a * (1.0 - gr) * gd
        gr *= er
        gd *= ed


def tones(n, partials, fm=None, am=None):
    """Sum of sines: partials = [(freq, amp, phase0 in cycles)], optional per-sample relative FM (zero mean for
    seamless loops) and amplitude envelope."""
    out = [0.0] * n
    for f, a, ph in partials:
        inc = f / SR
        p = ph
        if fm is None:
            for i in range(n):
                out[i] += a * math.sin(TWO_PI * p)
                p += inc
        else:
            for i in range(n):
                out[i] += a * math.sin(TWO_PI * p)
                p += inc * (1.0 + fm[i])
    if am is not None:
        out = mul(out, am)
    return out


def env_exp(n, decay, attack=0.001, start=0):
    out = [0.0] * n
    for i in range(start, n):
        t = (i - start) / float(SR)
        out[i] = (min(1.0, t / attack) if attack > 0 else 1.0) * math.exp(-t / decay)
    return out


def thump(n, f_hi, f_lo, sweep, decay, start=0, attack=0.001):
    """Sine whose frequency falls exponentially f_hi -> f_lo over `sweep` s, exponential decay, from `start`."""
    out = [0.0] * n
    ph = 0.0
    k = math.log(f_lo / f_hi) / sweep
    for i in range(start, n):
        t = (i - start) / float(SR)
        f = f_hi * math.exp(k * min(t, sweep))
        out[i] = math.sin(TWO_PI * ph) * min(1.0, t / attack) * math.exp(-t / decay)
        ph += f / SR
    return out


def noise_burst(r, n, decay, attack=0.0005, start=0):
    e = env_exp(n, decay, attack, start)
    return [a * r.uniform(-1.0, 1.0) for a in e]


def grains_decaying(r, n, count, t_lo, t_hi, dur_lo, dur_hi, decay):
    """Noise grains between t_lo and t_hi s, amplitudes falling with exp(-t/decay)."""
    env = [0.0] * n
    for _ in range(count):
        t0 = r.uniform(t_lo, t_hi) * SR
        a = r.uniform(0.3, 1.0) * math.exp(-t0 / (decay * SR))
        add_pulse(env, t0, a, 0.0002 * SR, r.uniform(dur_lo, dur_hi) * SR)
    return [e * r.uniform(-1.0, 1.0) for e in env]


def soft_clip(x, drive):
    return [math.tanh(drive * v) / math.tanh(drive) for v in x]


SOFT_SQ = ((1, 1.0), (3, 0.22), (5, 0.08), (7, 0.03))


def tone_seg(f, dur, decay, harm=((1, 1.0),), attack=0.002, release=0.0):
    n = int(dur * SR)
    out = [0.0] * n
    for i in range(n):
        t = i / float(SR)
        env = min(1.0, t / attack) * math.exp(-t / decay)
        if release > 0:
            env *= min(1.0, max(0.0, (dur - t) / release))
        v = 0.0
        for h, a in harm:
            v += a * math.sin(TWO_PI * f * h * t)
        out[i] = v * env
    return out


def silence(sec):
    return [0.0] * int(round(sec * SR))


# ---------------------------------------------------------------------------------------------
# WAV I/O and checks
# ---------------------------------------------------------------------------------------------

def to_pcm(x, gain):
    out = []
    for v in x:
        s = int(math.floor(v * gain * 32767.0 + 0.5))
        if s > 32767:
            s = 32767
        elif s < -32768:
            s = -32768
        out.append(s)
    return out


def write_wav(path, pcm):
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(struct.pack("<%dh" % len(pcm), *pcm))
    w.close()


def seam_check(pcm, w=64):
    """Compare the jump across the loop seam with the jumps inside the last+first w samples."""
    n = len(pcm)
    win = pcm[n - w:] + pcm[:w]
    d1 = [abs(win[i + 1] - win[i]) for i in range(len(win) - 1)]
    d2 = [abs(win[i + 1] - 2 * win[i] + win[i - 1]) for i in range(1, len(win) - 1)]
    seam_d1 = d1[w - 1]
    seam_d2 = max(d2[w - 2], d2[w - 1])
    ref_d1 = max(d1[:w - 1] + d1[w:])
    ref_d2 = max(d2[:w - 2] + d2[w:])
    ok = seam_d1 <= max(4, 1.25 * ref_d1) and seam_d2 <= max(8, 1.25 * ref_d2)
    return ok, seam_d1, ref_d1, seam_d2, ref_d2


def rms_window_jump(pcm, win=2205):
    """Loudness continuity across the seam: RMS (dB) of the last vs the first `win` samples (100 ms), and the
    largest such step between adjacent windows inside the loop (the seam step should be no bigger).
    Returns (seam_step_db, max_inside_db), or None when a window is silent (seam in a gap)."""
    def wdb(seg):
        r = rms(seg)
        return None if r < 1.0 else 20.0 * math.log10(r)
    a = wdb(pcm[-win:])
    b = wdb(pcm[:win])
    if a is None or b is None:
        return None
    inside = 0.0
    prev = wdb(pcm[:win])
    k = win
    while k + win <= len(pcm):
        cur = wdb(pcm[k:k + win])
        if cur is not None and prev is not None:
            inside = max(inside, abs(cur - prev))
        prev = cur
        k += win
    return b - a, inside


def band_split(pcm):
    """Energy share (%) below 250 Hz, 250-2000 Hz, above 2000 Hz (for the report / README)."""
    x = [s / 32768.0 for s in pcm]
    lo = lp(lp(x, 250.0), 250.0)
    hi = hp(hp(x, 2000.0), 2000.0)
    e_all = sum(v * v for v in x) or 1e-12
    e_lo = sum(v * v for v in lo)
    e_hi = sum(v * v for v in hi)
    e_mid = max(0.0, e_all - e_lo - e_hi)
    t = e_lo + e_mid + e_hi or 1e-12
    return 100.0 * e_lo / t, 100.0 * e_mid / t, 100.0 * e_hi / t


# ---------------------------------------------------------------------------------------------
# Engine loops
# ---------------------------------------------------------------------------------------------

class EngineCharacter(object):
    """Fixed properties shared by every engine loop (the same engine at different spool)."""

    def __init__(self):
        r = rng("engine", "character")
        self.buzz_amp = {}
        self.buzz_ph = {}
        for k in range(4, 15):
            self.buzz_amp[k] = r.uniform(0.3, 1.0) / math.sqrt(k)
            self.buzz_ph[k] = r.random()
        self.ph_fan = r.random()
        self.ph_whine = r.random()
        self.ph_whine2 = r.random()


_ENGINE = None


def engine_character():
    global _ENGINE
    if _ENGINE is None:
        _ENGINE = EngineCharacter()
    return _ENGINE


def engine_tonal(name, nspool, n, buzz_gain):
    """Fan tone, compressor whine (+2nd harmonic) with shaft-rate AM and wobble; buzz-saw orders.
    Returns (whine, fan, buzz) each normalised to RMS 1."""
    eng = engine_character()
    T = n / float(SR)
    r = rng("engine", name, "tonal")
    f_fan = whole(F_FAN * nspool, T)
    f_wh = whole(F_WHINE * nspool, T)
    f_wh2 = whole(2.0 * F_WHINE * nspool, T)
    f_sh = whole(F_SHAFT * nspool, T)
    wob = zero_mean(periodic_smooth_noise(r, n, 7))            # slow wander, ~2 points/s
    fm = [0.0025 * v for v in wob]
    am_sh = [1.0 + 0.22 * math.sin(TWO_PI * (f_sh * i / SR + 0.13)) for i in range(n)]
    am_slow = periodic_smooth_noise(r, n, 11, 0.8, 1.0)
    am = mul(am_sh, am_slow)
    whine = tones(n, [(f_wh, 1.0, eng.ph_whine), (f_wh2, 0.30, eng.ph_whine2)], fm, am)
    fan = tones(n, [(f_fan, 1.0, eng.ph_fan)], fm, am_slow)
    buzz = [0.0] * n
    if buzz_gain > 0.0:
        parts = [(k * f_sh, eng.buzz_amp[k], eng.buzz_ph[k]) for k in range(4, 15)]
        buzz = tones(n, parts, fm, periodic_smooth_noise(r, n, 23, 0.55, 1.0))
        buzz = soft_clip(scale(buzz, 1.0 / max(1e-9, peak_of(buzz))), 1.8)  # rasp: a little intermodulation
    return norm_rms(whine), norm_rms(fan), (norm_rms(buzz) if buzz_gain > 0.0 else buzz)


def engine_noises(name, n, roar_hz, roar_q):
    """Periodic noise components (each RMS 1): rumble, roar, mid rush, intake hiss. Returns (dict, conv)."""
    r = rng("engine", name, "noise")
    nz = white(r, n)
    conv = 0.0
    rumble, c = steady(nz, lambda x: lp(lp(x, 95.0), 120.0))
    conv = max(conv, c)
    rumble = mul(rumble, periodic_smooth_noise(r, n, 40, 0.55, 1.0))
    roar_src = nz[n // 3:] + nz[:n // 3]
    roar, c = steady(roar_src, lambda x: lp(bp(x, roar_hz, roar_q), 1600.0))
    conv = max(conv, c)
    roar = mul(roar, periodic_smooth_noise(r, n, 75, 0.7, 1.0))      # turbulence texture (~25 Hz)
    mid, c = steady(nz[::-1], lambda x: bp(x, 1700.0, 0.8))
    conv = max(conv, c)
    hiss, c = steady(nz[2 * n // 3:] + nz[:2 * n // 3], lambda x: lp(hp(x, 4500.0), 9000.0))
    conv = max(conv, c)
    out = {"rumble": norm_rms(rumble), "roar": norm_rms(roar), "mid": norm_rms(mid), "hiss": norm_rms(hiss)}
    return out, conv


def engine_voicing(x):
    def proc(v):
        return hp(lp(v, 7500.0), 28.0)
    return steady(x, proc)


def loop_eng_idle():
    """Ground idle / low spool: whine forward, soft roar and rumble."""
    n = int(round(ENG_T * SR))
    whine, fan, buzz = engine_tonal("idle", N_IDLE, n, 1.0)
    nzs, conv = engine_noises("idle", n, 300.0, 0.7)
    y = mix((db(-6.0), whine), (db(-11.0), fan), (db(-27.0), buzz),
            (db(-5.0), nzs["rumble"]), (db(-9.0), nzs["roar"]), (db(-17.0), nzs["mid"]),
            (db(-21.0), nzs["hiss"]))
    y, c = engine_voicing(y)
    return y, max(conv, c)


def loop_eng_mil():
    """Military power: broadband jet roar, buzz-saw rasp, whine behind it."""
    n = int(round(ENG_T * SR))
    whine, fan, buzz = engine_tonal("mil", N_MIL, n, 1.0)
    nzs, conv = engine_noises("mil", n, 360.0, 0.55)
    y = mix((db(-15.0), whine), (db(-16.0), fan), (db(-13.0), buzz),
            (db(-2.5), nzs["rumble"]), (db(0.0), nzs["roar"]), (db(-9.0), nzs["mid"]),
            (db(-20.0), nzs["hiss"]))
    y, c = engine_voicing(y)
    return y, max(conv, c)


def loop_eng_ab():
    """Afterburner: deep roar, rumble, throb and crackle; layered over eng_mil by the mix law."""
    n = int(round(ENG_T * SR))
    T = n / float(SR)
    r = rng("engine", "ab", "noise")
    nz = white(r, n)
    conv = 0.0
    roar, c = steady(nz, lambda x: lp(bp(x, 160.0, 0.5), 900.0))
    conv = max(conv, c)
    roar2, c = steady(nz[n // 2:] + nz[:n // 2], lambda x: bp(x, 620.0, 0.7))
    conv = max(conv, c)
    rumble, c = steady(nz[::-1], lambda x: lp(lp(x, 70.0), 90.0))
    conv = max(conv, c)
    hiss, c = steady(nz[n // 5:] + nz[:n // 5], lambda x: lp(hp(x, 3500.0), 8000.0))
    conv = max(conv, c)
    throb = periodic_smooth_noise(r, n, int(round(9.0 * T)), 0.6, 1.0)     # ~9 Hz wobble of the flame
    gust = periodic_smooth_noise(r, n, int(round(1.5 * T)), 0.85, 1.0)
    roar = mul(mul(roar, throb), gust)
    rumble = mul(rumble, periodic_smooth_noise(r, n, int(round(14.0 * T)), 0.5, 1.0))
    # crackle: sparse sharp spikes, heavy-tailed amplitudes, positive pressure (asymmetric), wraps
    rc = rng("engine", "ab", "crackle")
    spikes = [0.0] * n
    count = int(110 * T)
    for _ in range(count):
        t0 = rc.uniform(0.0, n)
        a = rc.random() ** 3.0
        add_pulse(spikes, t0, a, 0.00004 * SR, rc.uniform(0.0003, 0.0012) * SR, n)
    crackle, c = steady(spikes, lambda x: lp(hp(x, 350.0), 7000.0))
    conv = max(conv, c)
    whine, fan, _ = engine_tonal("ab", N_MIL, n, 0.0)
    y = mix((db(0.0), norm_rms(roar)), (db(-1.0), norm_rms(rumble)), (db(-8.0), norm_rms(roar2)),
            (db(-5.0), norm_rms(crackle)), (db(-23.0), norm_rms(hiss)), (db(-26.0), whine))
    y, c = engine_voicing(y)
    # tame the crest factor (crackle spikes would otherwise set the shared engine gain): soft limit at 3x RMS;
    # memoryless, so the loop stays periodic
    lim = 3.0 * rms(y)
    y = [lim * math.tanh(v / lim) for v in y]
    return y, max(conv, c)


# Designed relative loudness (loudness_db re eng_ab). The three loops share one gain (the loudest peak at
# -1 dBFS), so snd.cpp plays them with the mix-law weights only, no per-loop gain table.
ENGINE_LEVEL_DB = {"eng_idle": -10.0, "eng_mil": -4.5, "eng_ab": 0.0}


def make_engine_set():
    raw = {"eng_idle": loop_eng_idle(), "eng_mil": loop_eng_mil(), "eng_ab": loop_eng_ab()}
    leveled = {}
    for key in ("eng_idle", "eng_mil", "eng_ab"):
        x, conv = raw[key]
        g = db(ENGINE_LEVEL_DB[key] - loudness_db(x + x))
        leveled[key] = (scale(x, g), conv * g)
    pk = max(peak_of(x) for x, _ in leveled.values())
    gain = PEAK / pk
    out = {}
    for key in ("eng_idle", "eng_mil", "eng_ab"):
        x, conv = leveled[key]
        out[key] = (to_pcm(x, gain), conv * gain * 32767.0)
    return out


# ---------------------------------------------------------------------------------------------
# Other loops
# ---------------------------------------------------------------------------------------------

def loop_wind():
    """Air rush over the canopy: noise through a slowly moving band-pass, canopy hiss, low buffet. 4.0 s."""
    n = int(4.0 * SR)
    r = rng("wind")
    nz = white(r, n)
    fc = [800.0 + 900.0 * v for v in periodic_smooth_noise(r, n, 7, 0.0, 1.0)]
    gust = periodic_smooth_noise(r, n, 9, 0.75, 1.0)
    band, c1 = steady(nz, lambda x: bp_vary(x, fc, 0.6, n))
    band = mul(band, gust)
    hiss, c2 = steady(nz[n // 3:] + nz[:n // 3], lambda x: bp(x, 3600.0, 0.7))
    hiss = mul(hiss, periodic_smooth_noise(r, n, 13, 0.8, 1.0))
    low, c3 = steady(nz[::-1], lambda x: lp(lp(x, 110.0), 140.0))
    low = mul(low, periodic_smooth_noise(r, n, 30, 0.45, 1.0))
    y = mix((db(0.0), norm_rms(band)), (db(-7.0), norm_rms(hiss)), (db(-4.0), norm_rms(low)))
    y, c4 = steady(y, lambda x: hp(lp(x, 8000.0), 35.0))
    return y, max(c1, c2, c3, c4)


def loop_brake():
    """Speed brake buffet: heavy low rumble shaken at 6-14 Hz, a flutter band and some rush. 2.0 s."""
    n = int(2.0 * SR)
    r = rng("brake")
    nz = white(r, n)
    shake = periodic_smooth_noise(r, n, 22, 0.25, 1.0)          # ~11 Hz irregular buffet
    shake = [v * v for v in shake]
    low, c1 = steady(nz, lambda x: lp(lp(x, 120.0), 150.0))
    low = mul(low, shake)
    flutter, c2 = steady(nz[n // 2:] + nz[:n // 2], lambda x: bp(x, 290.0, 1.1))
    flutter = mul(flutter, periodic_smooth_noise(r, n, 34, 0.3, 1.0))
    rush, c3 = steady(nz[::-1], lambda x: bp(x, 950.0, 0.7))
    rush = mul(rush, periodic_smooth_noise(r, n, 10, 0.7, 1.0))
    y = mix((db(0.0), norm_rms(low)), (db(-5.0), norm_rms(flutter)), (db(-12.0), norm_rms(rush)))
    y, c4 = steady(y, lambda x: hp(lp(x, 5000.0), 30.0))
    return y, max(c1, c2, c3, c4)


def loop_roll():
    """Runway roll at 40 m/s (pitch 1): tyre roar, gear rumble, a joint every 10 m = 4/s (nose wheel, then the
    main gear 0.1 s later), small strut rattles. 2.0 s; pitch = V / 40 m/s."""
    n = int(2.0 * SR)
    r = rng("roll")
    nz = white(r, n)
    roar, c1 = steady(nz, lambda x: lp(bp(x, 260.0, 0.8), 1500.0))
    roar = mul(roar, periodic_smooth_noise(r, n, 30, 0.75, 1.0))
    rumble, c2 = steady(nz[n // 2:] + nz[:n // 2], lambda x: lp(lp(x, 100.0), 130.0))
    joints = [0.0] * n
    clicks = [0.0] * n
    for k in range(8):
        t_nose = (k * n / 8.0 + r.uniform(-30.0, 30.0)) % n
        t_main = (t_nose + 0.1 * SR + r.uniform(-20.0, 20.0)) % n
        add_pulse(joints, t_nose, r.uniform(0.45, 0.6), 0.0006 * SR, 0.010 * SR, n)
        add_pulse(joints, t_main, r.uniform(0.85, 1.0), 0.0006 * SR, 0.014 * SR, n)
        add_pulse(clicks, t_main, r.uniform(0.6, 1.0), 0.0001 * SR, 0.002 * SR, n)
    body, c3 = steady(joints, lambda x: mix((1.0, bp(x, 75.0, 1.3)), (0.6, bp(x, 150.0, 1.8)),
                                            (0.25, bp(x, 330.0, 2.5))))
    knock, c4 = steady([a * b for a, b in zip(clicks, nz[::-1])], lambda x: bp(x, 900.0, 1.4))
    rattle_src = [0.0] * n
    for _ in range(40):
        add_pulse(rattle_src, r.uniform(0.0, n), r.uniform(0.2, 1.0), 0.0001 * SR, 0.003 * SR, n)
    rattle, c5 = steady([a * b for a, b in zip(rattle_src, nz)], lambda x: mix((1.0, bp(x, 2300.0, 6.0)),
                                                                              (0.6, bp(x, 3700.0, 8.0))))
    y = mix((db(-2.0), norm_rms(roar)), (db(-3.0), norm_rms(rumble)), (db(0.0), norm_rms(body)),
            (db(-14.0), norm_rms(knock)), (db(-22.0), norm_rms(rattle)))
    y, c6 = steady(y, lambda x: hp(lp(x, 6000.0), 30.0))
    return y, max(c1, c2, c3, c4, c5, c6)


def loop_pullup():
    """PULL UP: two rising 'whoops' (700 -> 1400 Hz in 0.36 s) per 1.0 s loop; the seam lies in silence."""
    n = SR
    out = [0.0] * n
    dur = 0.36
    for start in (0.0, 0.5):
        i0 = int(start * SR)
        m = int(dur * SR)
        ph = 0.0
        k = math.log(1400.0 / 700.0) / dur
        for j in range(m):
            t = j / float(SR)
            f = 700.0 * math.exp(k * t)
            env = min(1.0, t / 0.012) * min(1.0, (dur - t) / 0.03)
            v = 0.0
            for h, a in SOFT_SQ:
                v += a * math.sin(TWO_PI * h * ph)
            out[i0 + j] = v * env
            ph += f / SR
    return out, 0.0


# ---------------------------------------------------------------------------------------------
# One-shots
# ---------------------------------------------------------------------------------------------

def shot_ab_light():
    """Afterburner light-up: a deep 'whump' (thump + low pressure burst) blooming into roar, 1.3 s, fading out
    so the eng_ab loop (fading in over 0.3 s) takes over."""
    n = int(1.3 * SR)
    r = rng("ab_light")
    boom = thump(n, 85.0, 38.0, 0.25, 0.22, start=int(0.01 * SR), attack=0.004)
    burst = lp(lp(noise_burst(r, n, 0.18, attack=0.006), 400.0), 500.0)
    swell = [0.0] * n
    nz = white(r, n)
    for i in range(n):
        t = i / float(SR)
        swell[i] = nz[i] * min(1.0, t / 0.15) * math.exp(-max(0.0, t - 0.15) / 0.35)
    swell = lp(bp(swell, 220.0, 0.6), 1200.0)
    pop = hp(noise_burst(r, n, 0.008, attack=0.0008), 700.0)
    y = mix((1.0, norm_rms(boom)), (0.9, norm_rms(burst)), (0.7, norm_rms(swell)), (0.12, norm_rms(pop)))
    y = soft_clip(scale(y, 1.0 / peak_of(y)), 1.6)
    y = hp(y, 25.0)
    return fade_out(y, 0.5)


def shot_gear_up():
    """Gear retraction, 3.0 s: doors unlatch (small knock), hydraulic pump whine (220 Hz ripple + harmonics) spins
    up, labours (pitch sags 3 % mid-stroke), fluid hiss and airflow rumble, then winds down. gear_clunk follows."""
    n = int(3.0 * SR)
    r = rng("gear_up")
    ph = 0.0
    whine = [0.0] * n
    harm = ((1, 1.0), (2, 0.55), (3, 0.35), (4, 0.18), (6, 0.10), (9, 0.06))
    for i in range(n):
        t = i / float(SR)
        spin = 1.0 - math.exp(-t / 0.12)
        sag = 1.0 - 0.03 * math.sin(math.pi * min(1.0, max(0.0, (t - 0.4) / 2.2)))
        f = 220.0 * (0.55 + 0.45 * spin) * sag
        env = min(1.0, t / 0.15) * min(1.0, max(0.0, (3.0 - t) / 0.35))
        v = 0.0
        for h, a in harm:
            v += a * math.sin(TWO_PI * h * ph)
        whine[i] = v * env
        ph += f / SR
    ripple = [1.0 + 0.15 * math.sin(TWO_PI * 27.0 * i / SR) for i in range(n)]
    whine = mul(whine, ripple)
    env_n = [min(1.0, (i / float(SR)) / 0.2) * min(1.0, max(0.0, (3.0 - i / float(SR)) / 0.4)) for i in range(n)]
    hiss = mul(bp(white(r, n), 2600.0, 1.2), env_n)
    air = mul(lp(lp(white(r, n), 160.0), 200.0), env_n)
    knock = mix((1.0, bp(noise_burst(r, n, 0.012, start=int(0.04 * SR)), 700.0, 2.0)),
                (0.6, thump(n, 120.0, 90.0, 0.04, 0.04, start=int(0.04 * SR))))
    y = mix((1.0, norm_rms(whine)), (0.22, norm_rms(hiss)), (0.6, norm_rms(air)), (0.5, norm_rms(knock)))
    return hp(lp(y, 6000.0), 40.0)


def shot_gear_clunk():
    """Uplock clunk: heavy thunk + struck-steel partials, the doors close 0.12 s later (smaller thunk). 0.6 s."""
    n = int(0.6 * SR)
    r = rng("gear_clunk")
    y = [0.0] * n
    for t0, g in ((0.0, 1.0), (0.12, 0.55)):
        s = int(t0 * SR)
        body = thump(n, 115.0, 62.0, 0.06, 0.07, start=s)
        metal = [0.0] * n
        for f, a, d in ((183.0, 1.0, 0.06), (472.0, 0.6, 0.035), (1021.0, 0.3, 0.018), (1647.0, 0.14, 0.01)):
            p0 = r.random()
            for i in range(s, n):
                t = (i - s) / float(SR)
                metal[i] += a * math.sin(TWO_PI * (p0 + f * g ** 0.1 * t)) * min(1.0, t / 0.0008) * math.exp(-t / d)
        knock = bp(noise_burst(r, n, 0.007, start=s), 850.0, 1.5)
        y = mix((1.0, y), (g, body), (0.5 * g, metal), (2.0 * g, knock))
    return hp(lp(y, 6000.0), 40.0)


def shot_expl_near():
    """Explosion close by, 3.5 s: crack, deep falling boom, roaring fireball, debris crackle, secondary pops,
    long rumble tail; soft-saturated for weight."""
    n = int(3.5 * SR)
    r = rng("expl_near")
    crack = hp(noise_burst(r, n, 0.006), 1200.0)
    boom = thump(n, 95.0, 28.0, 0.35, 0.55, attack=0.002)
    fire_src = [0.0] * n
    nz = white(r, n)
    for i in range(n):
        t = i / float(SR)
        fire_src[i] = nz[i] * min(1.0, t / 0.005) * math.exp(-t / 0.55)
    fire = lp(lp(fire_src, 450.0), 600.0)
    rumble_src = [nz[n - 1 - i] * min(1.0, (i / float(SR)) / 0.05) * math.exp(-(i / float(SR)) / 1.3)
                  for i in range(n)]
    rumble = lp(lp(rumble_src, 110.0), 140.0)
    debris = grains_decaying(r, n, 420, 0.02, 1.8, 0.0008, 0.005, 0.5)
    debris = mix((1.0, bp(debris, 1400.0, 1.0)), (0.7, bp(debris, 3200.0, 1.4)))
    pops = [0.0] * n
    for t0, a in ((0.24, 0.55), (0.52, 0.4), (0.9, 0.3), (1.35, 0.18)):
        pops = mix((1.0, pops), (a, thump(n, 80.0, 40.0, 0.12, 0.12, start=int(t0 * SR))),
                   (0.5 * a, lp(noise_burst(r, n, 0.05, start=int(t0 * SR)), 500.0)))
    y = mix((0.35, norm_rms(crack)), (1.0, norm_rms(boom)), (0.9, norm_rms(fire)), (0.8, norm_rms(rumble)),
            (0.30, norm_rms(debris)), (0.45, norm_rms(pops)))
    y = soft_clip(scale(y, 1.0 / peak_of(y)), 2.2)
    y = hp(lp(y, 8000.0), 22.0)
    return fade_out(y, 0.8)


def shot_expl_far():
    """Explosion far away, 4.0 s: no crack, a soft dull boom (high frequencies absorbed), rolling rumble and
    three echoes off the sea/terrain (0.38, 0.95, 1.7 s). Game code delays it by distance / 340 m/s."""
    n = int(4.0 * SR)
    r = rng("expl_far")
    boom = thump(n, 62.0, 30.0, 0.4, 0.7, attack=0.02)
    burst = lp(lp(noise_burst(r, n, 0.35, attack=0.02), 260.0), 320.0)
    dry = mix((1.0, norm_rms(boom)), (0.9, norm_rms(burst)))
    y = list(dry)
    for t0, g, f in ((0.38, 0.5, 300.0), (0.95, 0.32, 240.0), (1.7, 0.18, 200.0)):
        d = int(t0 * SR)
        e = lp(dry, f)
        for i in range(d, n):
            y[i] += g * e[i - d]
    nz = white(r, n)
    tail_src = [nz[i] * min(1.0, (i / float(SR)) / 0.25) * math.exp(-(i / float(SR)) / 1.4) for i in range(n)]
    tail = lp(lp(tail_src, 120.0), 150.0)
    tail = mul(tail, [0.6 + 0.4 * v for v in periodic_smooth_noise(r, n, 28, 0.0, 1.0)])
    y = mix((1.0, y), (0.55 * rms(y) / max(1e-9, rms(tail)), tail))
    y = hp(lp(y, 900.0), 22.0)
    return fade_out(y, 1.0)


def shot_ui_move():
    return tone_seg(1760.0, 0.045, 0.012, ((1, 1.0), (2, 0.25)))


def shot_ui_ok():
    return tone_seg(990.0, 0.07, 0.05, SOFT_SQ) + tone_seg(1480.0, 0.15, 0.06, SOFT_SQ)


def shot_ui_back():
    return tone_seg(1320.0, 0.06, 0.05, SOFT_SQ) + tone_seg(880.0, 0.12, 0.05, SOFT_SQ)


def shot_beep():
    """Message beep: two short soft-square pips at 1050 Hz ('bip-bip'), 0.27 s."""
    pip = tone_seg(1050.0, 0.09, 10.0, SOFT_SQ, attack=0.004, release=0.02)
    return pip + silence(0.06) + pip + silence(0.03)


# ---------------------------------------------------------------------------------------------
# Mix law (the reference for snd.cpp; README documents it) and the preview flight
# ---------------------------------------------------------------------------------------------

SPOOL_TARGET = {"READY": 0.65, "SLOW": 0.74, "NORMAL": 0.90, "FAST": 1.0}
SPOOL_TAU_UP = 0.8                    # s, first-order lag towards a higher target
SPOOL_TAU_DOWN = 1.5                  # s, towards a lower target


def spool_step(n_cur, state, dt):
    tgt = SPOOL_TARGET[state]
    tau = SPOOL_TAU_UP if tgt > n_cur else SPOOL_TAU_DOWN
    return n_cur + (tgt - n_cur) * (1.0 - math.exp(-dt / tau))


def clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def mix_engine(nspool, ab, mach):
    """[(name, weight, pitch)] for the three engine voices.
    nspool: lagged spool 0.65..1.0; ab: afterburner level 0..1 (flight model, lagged); mach: true Mach number."""
    kv = 1.0 + 0.06 * (clamp(mach, 0.2, 1.8) - 0.8)            # "pitch by V": +-6 % ram/airflow term
    t = clamp(math.log(nspool / N_IDLE) / math.log(N_MIL / N_IDLE), 0.0, 1.0)
    w_idle = math.cos(t * math.pi / 2)
    w_mil = math.sin(t * math.pi / 2)
    w_ab = math.sin(clamp(ab, 0.0, 1.0) * math.pi / 2)
    w_mil *= 1.0 - 0.35 * clamp(ab, 0.0, 1.0)
    return [("eng_idle", w_idle, nspool / N_IDLE * kv),
            ("eng_mil", w_mil, nspool / N_MIL * kv),
            ("eng_ab", w_ab, nspool / N_MIL * kv)]                 # same pitch as mil: tones stay in step


def mix_wind(v_kt, g):
    u = clamp(v_kt / 800.0, 0.0, 1.25)
    vol = min(0.8, 0.55 * u * u * (1.0 + 0.12 * max(0.0, g - 1.5)))
    pitch = 0.7 + 0.5 * u + 0.02 * max(0.0, g - 1.0)
    return vol, pitch


def mix_brake(brake, v_kt):
    vol = 0.5 * clamp(brake, 0.0, 1.0) * clamp(v_kt / 450.0, 0.3, 1.2)
    pitch = 0.85 + 0.25 * clamp(v_kt / 600.0, 0.0, 1.2)
    return vol, pitch


def mix_roll(v_ms, on_ground):
    if not on_ground:
        return 0.0, 1.0
    vol = 0.8 * clamp(v_ms / ROLL_VREF, 0.0, 1.0) ** 0.6
    pitch = clamp(v_ms / ROLL_VREF, 0.25, 2.2)
    return vol, pitch


ONESHOT_VOL = {"ab_light": 0.8, "gear_up": 0.35, "gear_clunk": 0.5}

# preview script: (t_end s, state, V at t_end kt, G, on ground); V interpolates linearly inside a segment
PREVIEW_SEGS = [(3.0, "READY", 0.0, 1.0, True),
                (12.0, "FAST", 160.0, 1.0, True),          # afterburner takeoff roll (11 m/s^2 is ~ 9 s)
                (16.0, "FAST", 260.0, 1.2, False),         # climb-out, gear up at 13 s, clunk at 16 s
                (24.0, "NORMAL", 480.0, 1.0, False),
                (29.0, "FAST", 760.0, 1.0, False),
                (32.0, "FAST", 900.0, 5.0, False),         # pull 5 g at Mach 1.3-1.4
                (37.0, "NORMAL", 650.0, 1.0, False),
                (44.0, "SLOW", 330.0, 1.0, False),         # speed brake
                (48.0, "NORMAL", 360.0, 1.0, False)]
PREVIEW_SHOTS = [(3.0, "ab_light"), (13.0, "gear_up"), (16.0, "gear_clunk"), (24.0, "ab_light")]


def preview_state(t):
    t_prev = 0.0
    v_prev = PREVIEW_SEGS[0][2]
    for t_end, state, v_end, g, gnd in PREVIEW_SEGS:
        if t <= t_end:
            f = (t - t_prev) / max(1e-9, t_end - t_prev)
            return state, v_prev + (v_end - v_prev) * f, g, gnd
        t_prev = t_end
        v_prev = v_end
    s = PREVIEW_SEGS[-1]
    return s[1], s[2], s[3], s[4]


def render_preview(pcms):
    """Offline mix like snd.cpp: looping voices with own play positions (linear interpolation), parameters
    updated at 120 Hz (the sim tick) and linearly ramped per sample (DirectSound commits per frame)."""
    total = PREVIEW_SEGS[-1][0]
    n = int(total * SR)
    snd = dict((k, [s / 32768.0 for s in v]) for k, v in pcms.items())
    loops = ["eng_idle", "eng_mil", "eng_ab", "wind", "brake", "roll"]
    pos = dict((k, 0.0) for k in loops)
    cur = dict((k, (0.0, 1.0)) for k in loops)
    out = [0.0] * n
    tick = SR // 120 + 1
    nsp = SPOOL_TARGET["READY"]
    ab = 0.0
    brake = 0.0
    roll_fade = 1.0
    i = 0
    while i < n:
        t = i / float(SR)
        state, v_kt, g, gnd = preview_state(t)
        dt = tick / float(SR)
        nsp = spool_step(nsp, state, dt)
        ab += ((1.0 if state == "FAST" else 0.0) - ab) * (1.0 - math.exp(-dt / 0.1))     # ~0.3 s to full
        brake += ((1.0 if state == "SLOW" else 0.0) - brake) * (1.0 - math.exp(-dt / 0.25))
        roll_fade += ((1.0 if gnd else 0.0) - roll_fade) * (1.0 - math.exp(-dt / 0.1))
        mach = v_kt * 0.514444 / 340.3
        tgt = {}
        for name, w, p in mix_engine(nsp, ab, mach):
            tgt[name] = (w, p)
        tgt["wind"] = mix_wind(v_kt, g)
        tgt["brake"] = mix_brake(brake, v_kt)
        rv, rp = mix_roll(v_kt * 0.514444, True)
        tgt["roll"] = (rv * roll_fade, rp)
        m = min(tick, n - i)
        for k in loops:
            buf = snd[k]
            ln = len(buf)
            w0, p0 = cur[k]
            w1, p1 = tgt[k]
            p = pos[k]
            if w0 < 0.0005 and w1 < 0.0005:
                p = (p + m * p1) % ln
            else:
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
    for t0, name in PREVIEW_SHOTS:
        buf = snd[name]
        s = int(t0 * SR)
        g = ONESHOT_VOL[name]
        for j in range(len(buf)):
            if s + j < n:
                out[s + j] += g * buf[j]
    pk = peak_of(out)
    return to_pcm(fades(out, 0.05), PEAK / pk), pk


# ---------------------------------------------------------------------------------------------
# README
# ---------------------------------------------------------------------------------------------

USES = {
    "eng_idle": "turbine at low spool (N 0.65 = ground idle): whine forward, soft roar",
    "eng_mil": "turbine at military power (N 1.0): broadband roar, buzz-saw rasp, whine behind",
    "eng_ab": "afterburner layer: deep roar, throb, crackle (on top of eng_mil)",
    "wind": "air rush over the canopy; volume/pitch by V and G",
    "brake": "speed brake buffet (SLOW with the brake out)",
    "roll": "runway roll: tyre roar + a joint thump pair every 10 m; pitch 1 = 40 m/s",
    "pullup": "PULL UP warning, two rising whoops per second; loop while the warning is active",
    "ab_light": "afterburner light-up whump (FAST pressed / ab starts rising)",
    "gear_up": "gear retraction motor, 3.0 s (start of the 3 s gear-up of DESIGN 6.1)",
    "gear_clunk": "gear uplock + doors closing (end of the gear-up)",
    "expl_near": "crash / kill close by (< ~1 km)",
    "expl_far": "explosion far away (delay by distance / 340 m/s in game code)",
    "ui_move": "menu cursor move", "ui_ok": "confirm (rising two-tone)", "ui_back": "back (falling two-tone)",
    "beep": "message beep (HUD messages, F5 INI RELOADED)",
}
VOL = {"wind": "0..0.8 (mix law)", "brake": "0..0.6 (mix law)", "roll": "0..0.8 (mix law)", "pullup": "0.55",
       "ab_light": "0.8", "gear_up": "0.35", "gear_clunk": "0.5", "expl_near": "1.0 x distance law",
       "expl_far": "0.4-0.8 x distance law", "ui_move": "0.5", "ui_ok": "0.6", "ui_back": "0.6", "beep": "0.5"}


def readme_text(info):
    L = []
    a = L.append
    a("# data/sfx: jet sound pack v1, flight (generated by `tools/sfx/gen_sfx.py`, task A5; do not edit)")
    a("")
    a("All files: mono, 22050 Hz, 16-bit PCM. Regenerate with `python3 tools/sfx/gen_sfx.py` (deterministic;")
    a("`--check` compares md5 with data/). Loops are seamless (whole cycles + steady-state filtering): play them")
    a("with DSBPLAY_LOOPING from sample 0. Pitch = DirectSound frequency multiplier of 22050 Hz (e_audio `pitch`).")
    a("Combat sounds (pack v2) are added by A9 (`gen_sfx_combat.py`) in their own README section.")
    a("")
    a("## Loops")
    a("")
    a("| File | Samples | Seconds | Base pitch | Peak | Use | Suggested volume |")
    a("|---|---|---|---|---|---|---|")
    base = {"eng_idle": "1.0 = N 0.65", "eng_mil": "1.0 = N 1.0", "eng_ab": "1.0 = N 1.0",
            "wind": "1.0 = 480 kt, 1 g", "brake": "1.0 = 360 kt", "roll": "1.0 = 40 m/s", "pullup": "1.0"}
    for nm in LOOP_NAMES:
        s, pk = info[nm]
        a("| `%s.wav` | %d | %.3f | %s | %.1f dBFS | %s | %s |" % (nm, s, s / float(SR), base[nm], pk, USES[nm],
                                                               "mix law" if nm.startswith("eng") else VOL[nm]))
    a("")
    a("The three engine loops share one gain (baked loudness relative to `eng_ab`: idle %+.1f dB, mil %+.1f dB,"
      % (ENGINE_LEVEL_DB["eng_idle"], ENGINE_LEVEL_DB["eng_mil"]))
    a("ab 0 dB; the loudest peaks at -1 dBFS). Play them with the mix-law weights only; don't normalise per loop.")
    a("")
    a("## Engine mix law (snd.cpp; `mix_engine()` in the generator is the reference)")
    a("")
    a("- **Spool** `N` (0.65..1.0), a first-order lag towards a target by throttle state: READY / on the ground")
    a("  before A = %.2f, SLOW = %.2f, NORMAL = %.2f, FAST = %.2f; time constant %.1f s up, %.1f s down"
      % (SPOOL_TARGET["READY"], SPOOL_TARGET["SLOW"], SPOOL_TARGET["NORMAL"], SPOOL_TARGET["FAST"],
         SPOOL_TAU_UP, SPOOL_TAU_DOWN))
    a("  (`N += (target - N) * (1 - exp(-dt / tau))`). After takeoff the jet never returns to READY.")
    a("- **Idle/mil crossfade** in pitch-ratio space: `t = clamp(log(N / 0.65) / log(1 / 0.65), 0, 1)`,")
    a("  `w_idle = cos(t*pi/2)`, `w_mil = sin(t*pi/2)` (equal power; the loops are uncorrelated noise + tones).")
    a("- **Afterburner**: `ab` = the flight model's lagged afterburner level (DESIGN 4.2, 0 -> 1 in 0.3 s):")
    a("  `w_ab = sin(ab*pi/2)`, and `w_mil *= 1 - 0.35*ab` (the roar covers the mil loop; the whine stays).")
    a("- **Pitch**: `kv = 1 + 0.06 * (clamp(Mach, 0.2, 1.8) - 0.8)` (the small 'pitch by V' term);")
    a("  idle voice `N / 0.65 * kv`, mil voice `N / 1.0 * kv` (so both voices put the whine and fan tone on the")
    a("  same frequency during the crossfade), ab voice = the mil pitch `N * kv` (its faint whine stays in step).")
    a("  Played ranges: idle 0.96-1.6 (inaudible above ~1.4), mil and ab 0.62-1.06 (ab only audible near N 1).")
    a("- **Volume**: e_audio linear `vol` = weight x master; start all three loops at once at position 0 and keep")
    a("  them looping (a -74 dB floor instead of stopping, as the racer does), so nothing restarts audibly.")
    a("- **ab_light**: play once (vol 0.8, pitch 1) when `ab` starts rising from 0 (FAST pressed, also at the")
    a("  takeoff roll). It fades out by 1.3 s while `eng_ab` fades in.")
    a("")
    a("## Other mix laws (suggested; the preview uses exactly these)")
    a("")
    a("- **wind** (V in kt, G load factor): `u = clamp(V / 800, 0, 1.25)`;")
    a("  `vol = min(0.8, 0.55 * u^2 * (1 + 0.12 * max(0, G - 1.5)))`; `pitch = 0.7 + 0.5*u + 0.02*max(0, G - 1)`.")
    a("  Silent on the ground at rest, about 0.15 at 400 kt, 0.55 at 800 kt, more in a hard pull.")
    a("- **brake** (`brake` = the flight visual's speed brake 0..1): `vol = 0.5 * brake * clamp(V / 450, 0.3, 1.2)`;")
    a("  `pitch = 0.85 + 0.25 * clamp(V / 600, 0, 1.2)`.")
    a("- **roll** (V in m/s, on the ground only): `vol = 0.8 * clamp(V / 40, 0, 1)^0.6`;")
    a("  `pitch = clamp(V / 40, 0.25, 2.2)` (the joint thumps then follow the speed: 4/s at 40 m/s, ~8/s at")
    a("  liftoff). Fade it out over ~0.3 s when the wheels leave the runway.")
    a("- **gear_up** (vol 0.35) when the gear starts up (50 ft AGL), **gear_clunk** (vol 0.5) 3.0 s later.")
    a("- **pullup**: loop (vol 0.55) while PULL UP is shown; stop it when the warning clears. The loop starts with")
    a("  a whoop, so starting it at position 0 gives an immediate cue.")
    a("- **expl_near / expl_far**: near below ~1 km, far beyond; game code adds the distance attenuation and the")
    a("  sound delay (distance / 340 m/s). Own crash: expl_near at vol 1.0.")
    a("- Reference mix: `python3 tools/sfx/gen_sfx.py` writes `tools/sfx/preview_flight.wav` (48 s: ground idle,")
    a("  afterburner takeoff roll with ab_light, gear up + clunk, NORMAL cruise, afterburner run to Mach 1.36 with")
    a("  a 5 g pull, NORMAL, speed brake down to 330 kt, NORMAL) with exactly these laws.")
    a("- **Headroom**: full afterburner + wind at Mach 1.4 + a pull sums to ~1.4x full scale in the preview (it is")
    a("  normalised afterwards). In game keep the sound master around 0.7 (DirectSound clips the primary mix).")
    a("")
    a("## One-shots (peak -1 dBFS, 5 ms fades)")
    a("")
    a("| File | Samples | Seconds | Use | Suggested volume |")
    a("|---|---|---|---|---|")
    for nm in SHOT_NAMES:
        s, pk = info[nm]
        a("| `%s.wav` | %d | %.3f | %s | %s |" % (nm, s, s / float(SR), USES[nm], VOL[nm]))
    a("")
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------------------------
# Build (assetkit protocol)
# ---------------------------------------------------------------------------------------------

def put_wav(kit, name, pcm, info, conv=None, loop=False, log=print):
    """Write sfx/<name>.wav through kit.out(); log one line (+ seam check for loops). Returns seam ok."""
    path = kit.out("sfx/%s.wav" % name)
    write_wav(path, pcm)
    pk = max(abs(s) for s in pcm)
    pk_db = 20.0 * math.log10(max(1, pk) / 32767.0)
    rms_db = 20.0 * math.log10(max(1e-9, rms(pcm)) / 32767.0)
    info[name] = (len(pcm), pk_db)
    lo, mid, hi = band_split(pcm)
    line = "%-10s %6d smp %5.2f s  peak %5.1f  rms %5.1f dBFS  bands lo/mid/hi %2.0f/%2.0f/%2.0f %%" % (
        name, len(pcm), len(pcm) / float(SR), pk_db, rms_db, lo, mid, hi)
    ok = True
    if loop:
        ok, sd1, rd1, sd2, rd2 = seam_check(pcm)
        jump = rms_window_jump(pcm)
        jtxt = ("100 ms rms step %+.1f dB (inside max %.1f)" % jump) if jump else "seam in silence"
        line += "  seam %s (d1 %d/%d, d2 %d/%d, %s, conv %.3f LSB)" % (
            "ok" if ok else "FAIL", sd1, rd1, sd2, rd2, jtxt, conv or 0.0)
    log(line)
    return ok


def build(out_dir, kit):
    """Write every file in ASSETS under out_dir (the data root of this run); preview when kit.previews."""
    log = kit.log
    info = {}
    pcms = {}
    bad = 0
    eng = make_engine_set()
    for nm in ("eng_idle", "eng_mil", "eng_ab"):
        pcm, conv = eng[nm]
        pcms[nm] = pcm
        bad += 0 if put_wav(kit, nm, pcm, info, conv, True, log) else 1
    for nm, fn in (("wind", loop_wind), ("brake", loop_brake), ("roll", loop_roll), ("pullup", loop_pullup)):
        x, conv = fn()
        g = PEAK / peak_of(x)
        pcm = to_pcm(x, g)
        pcms[nm] = pcm
        bad += 0 if put_wav(kit, nm, pcm, info, conv * g * 32767.0, True, log) else 1
    for nm, fn in (("ab_light", shot_ab_light), ("gear_up", shot_gear_up), ("gear_clunk", shot_gear_clunk),
                   ("expl_near", shot_expl_near), ("expl_far", shot_expl_far), ("ui_move", shot_ui_move),
                   ("ui_ok", shot_ui_ok), ("ui_back", shot_ui_back), ("beep", shot_beep)):
        x = fades(fn())
        pcm = to_pcm(x, PEAK / peak_of(x))
        pcms[nm] = pcm
        put_wav(kit, nm, pcm, info, log=log)
    kit.save_text("sfx/README.md", readme_text(info))
    if bad:
        raise RuntimeError("gen_sfx: %d loop seam check(s) failed" % bad)
    if kit.previews:
        pcm, pk = render_preview(pcms)
        write_wav(PREVIEW_WAV, pcm)
        log("preview %s (%.1f s, mix peak %.2f before normalising)" % (os.path.relpath(PREVIEW_WAV, ROOT),
                                                                     len(pcm) / float(SR), pk))


if __name__ == "__main__":
    sys.path.insert(0, os.path.join(ROOT, "engine", "tools"))
    from assetkit import build as _B
    sys.exit(_B.main_for(__file__))
