/* flight.cpp - arcade flight model (DESIGN 4). Portable. Owner: T05. See flight.h for the model and conventions.

   Deviations from DESIGN 4 (also in docs/HANDOFF.md T05):
   - pitchRateCapDeg default 40 (DESIGN: 30): 30 clipped the card's own 150 kt / 4.5 g line (needs 31.2 deg/s) and
     kept turnBoost from scaling low-speed turns; with 40 the G limit table is what limits, the cap is a safety net.
   - decBrake default 28 m/s^2 (DESIGN table: 15): 15 gives FAST -> 350 kt in ~24 s, the feel card (4.7) asks ~13 s.
   - SLOW: the speed-brake deceleration fades from decBrake to decSlow over vManBandKt above vManKt (no hard step
     at 350 kt, and no exponential tail that would never reach 350 kt).
   - T10a (replaces T05's ceilingFade/ceilDrag, whose drag grew past g above 15 km and pinned the speed at the
     floor even in a vertical AB dive): positive throttle acceleration x flight_thrust_lapse(h) (ISA density^k,
     1 up to thrustFullM, 0 at ceilingM: climb rate fades to 0 at the service ceiling). Above ceilingM only:
     thinAirDecel (bounded, 0 at the floor speed, so every dive gains speed), zoomDrag (acts only while
     climbing: a zoom tops out smoothly, level flight and dives are untouched) and a gentle nose drop towards a
     5 deg descent (half of floorDropDeg) so the jet cannot float above the ceiling.
   - The floor nose drop is applied along the pilot's "down" (scaled by cos(bank), at least 0.5 in size), so it
     lowers the nose towards the horizon upright and inverted alike; DESIGN subtracts it from nCmd as-is.
   - The three per-tick rotations of DESIGN 4.3 are applied as one rotation vector (exact for a constant rate
     over the tick; the sequential product differs by O(dt^2)); its quaternion uses the series to angle^4.
   Speed (DESIGN acceptance: <= 15 us per aircraft on the VM): the step reuses the heading/gamma/bank derived at the
   end of the previous tick (flight_init / flight_set_path / flight_derive keep them current) and has no trig in
   the hot path except the derive at the end (atan2 x2, asin x2). */
#include "flight.h"
#include "game.h"
#include "params.h"
#include "e_log.h"

/* ---------------------------------------------------------------- [flight] (DESIGN 4.6) */
static PTable s_gLimTab, s_rollTab;
static float s_turnBoost, s_negG, s_qCapDeg, s_tauG, s_tauRoll, s_detentDeg, s_detentGain, s_latGrav;
static float s_machNormal, s_machFast, s_vManKt, s_vFloorKt, s_vManBandKt;
static float s_accMil, s_accAB, s_decOver, s_decBrake, s_decSlow, s_tauV, s_energyTrade, s_inducedPerG, s_vMaxMach;
static float s_floorBandKt, s_floorDropDeg, s_thrustFullM, s_ceilingM, s_lapseExp, s_thinAirDecel, s_zoomDrag;
static float s_alpha1gDeg, s_alphaRefKt, s_alphaMinDeg, s_alphaMaxDeg, s_tauAlpha;
static float s_stabMaxDeg, s_stabRollDeg, s_flapMaxDeg, s_surfTau, s_abOnSec, s_abOffSec, s_brakeSec;

static const Param s_tab[] = {
    { "flight", "gLimitTable", P_TABLE, &s_gLimTab, "150:4.5,200:6,250:7.5,300:8.5,330:9,700:9,1020:7,2000:6", 0.5f, 20.0f,
      "G limit (g) vs true airspeed (kt): full pull gives this G. More G = tighter turns at that speed" },
    { "flight", "turnBoost", P_FLOAT, &s_turnBoost, "1.0", 0.2f, 3.0f,
      "multiplies the whole G limit table (global 'how tight'): 1.2 = 20 % tighter turns everywhere" },
    { "flight", "negGLimit", P_FLOAT, &s_negG, "-3.0", -10.0f, 0.0f, "push limit (g) with the nose-down key held" },
    { "flight", "pitchRateCapDeg", P_FLOAT, &s_qCapDeg, "40", 1.0f, 180.0f,
      "max flight-path pitch rate (deg/s): a safety limit on how fast the nose comes round (the G table limits first)" },
    { "flight", "tauG", P_FLOAT, &s_tauG, "0.10", 0.005f, 2.0f, "G onset lag (s): higher = softer, slower pull" },
    { "flight", "rollRateTable", P_TABLE, &s_rollTab, "150:110,250:180,400:220,700:200", 0.0f, 720.0f,
      "max roll rate (deg/s) vs true airspeed (kt)" },
    { "flight", "tauRoll", P_FLOAT, &s_tauRoll, "0.06", 0.005f, 1.0f, "roll rate lag (s): higher = rolls start and stop softer" },
    { "flight", "detentDeg", P_FLOAT, &s_detentDeg, "4", 0.0f, 20.0f,
      "wings-level detent: with no roll key and less bank than this (deg), the wings level themselves (0 = off)" },
    { "flight", "detentGain", P_FLOAT, &s_detentGain, "1.5", 0.0f, 20.0f, "detent strength (1/s): how fast it levels" },
    { "flight", "latGrav", P_FLOAT, &s_latGrav, "1.0", 0.0f, 2.0f,
      "how much a bank turns the jet by itself (gravity's side pull); 0 = banking alone doesn't turn" },
    { "flight", "machNormal", P_FLOAT, &s_machNormal, "0.80", 0.3f, 2.0f, "cruise speed with no throttle key (Mach)" },
    { "flight", "machFast", P_FLOAT, &s_machFast, "1.60", 0.3f, 2.5f, "A held (afterburner) speed (Mach)" },
    { "flight", "vManKt", P_FLOAT, &s_vManKt, "350", 100.0f, 800.0f, "Z held: speed brake down to this (kt), then slow bleed" },
    { "flight", "vFloorKt", P_FLOAT, &s_vFloorKt, "150", 50.0f, 400.0f, "speed floor (kt): the jet never flies slower" },
    { "flight", "vManBandKt", P_FLOAT, &s_vManBandKt, "10", 0.5f, 100.0f,
      "Z: the speed brake fades out over this band (kt) above vManKt (no jolt at 350 kt)" },
    { "flight", "accMil", P_FLOAT, &s_accMil, "6", 0.1f, 50.0f, "max acceleration at normal power (m/s^2)" },
    { "flight", "accAB", P_FLOAT, &s_accAB, "18", 0.1f, 80.0f, "max acceleration in afterburner (m/s^2)" },
    { "flight", "decOver", P_FLOAT, &s_decOver, "15", 0.1f, 80.0f,
      "deceleration when faster than the throttle target, e.g. after letting go of A or after a dive (m/s^2)" },
    { "flight", "decBrake", P_FLOAT, &s_decBrake, "28", 0.1f, 100.0f, "Z: speed brake deceleration above vManKt (m/s^2)" },
    { "flight", "decSlow", P_FLOAT, &s_decSlow, "2.5", 0.1f, 50.0f, "Z: slow bleed from vManKt down to the floor (m/s^2)" },
    { "flight", "tauV", P_FLOAT, &s_tauV, "2.5", 0.1f, 30.0f, "how softly the speed settles on its target (s)" },
    { "flight", "energyTrade", P_FLOAT, &s_energyTrade, "1.0", 0.0f, 2.0f,
      "climbs cost speed, dives give speed (1 = gravity, 0 = none)" },
    { "flight", "inducedPerG", P_FLOAT, &s_inducedPerG, "0.9", 0.0f, 10.0f, "speed bleed per g above 1 g in hard turns (m/s^2 per g)" },
    { "flight", "vMaxMach", P_FLOAT, &s_vMaxMach, "2.0", 0.5f, 3.0f, "hard speed limit (Mach)" },
    { "flight", "floorBandKt", P_FLOAT, &s_floorBandKt, "20", 1.0f, 200.0f,
      "near the speed floor (this many kt above it) a climbing nose starts to sink" },
    { "flight", "floorDropDeg", P_FLOAT, &s_floorDropDeg, "8", 0.0f, 90.0f,
      "how fast the nose sinks towards the horizon at the speed floor or above the ceiling (deg/s)" },
    { "flight", "thrustFullM", P_FLOAT, &s_thrustFullM, "6000", 0.0f, 20000.0f,
      "full engine thrust up to this altitude (m); above it the thrust fades as the air thins" },
    { "flight", "ceilingM", P_FLOAT, &s_ceilingM, "15500", 2000.0f, 30000.0f,
      "service ceiling (m): no thrust left here, so climbing fades out by itself; a zoom coasts a bit higher" },
    { "flight", "thrustLapseExp", P_FLOAT, &s_lapseExp, "0.7", 0.1f, 4.0f,
      "thrust ~ air density^this between thrustFullM and ceilingM (shape of the fade; 0.7 = jet engine)" },
    { "flight", "thinAirDecel", P_FLOAT, &s_thinAirDecel, "0.5", 0.0f, 1.5f,
      "above ceilingM the jet slowly loses speed (m/s^2, full 1 km above and 200 kt over the floor, 0 at the floor speed): dives always gain" },
    { "flight", "zoomDrag", P_FLOAT, &s_zoomDrag, "3.0", 0.0f, 10.0f,
      "above ceilingM climbing costs extra: per 1000 m above it, this many extra times gravity's cost of the "
      "climb (ends zoom climbs smoothly; never acts in level flight or dives)" },
    { "flight", "alpha1gDeg", P_FLOAT, &s_alpha1gDeg, "2.0", 0.0f, 20.0f, "look only: nose angle above the flight path at 1 g and alphaRefKt (deg)" },
    { "flight", "alphaRefKt", P_FLOAT, &s_alphaRefKt, "300", 50.0f, 1000.0f, "look only: reference speed for alpha1gDeg (kt)" },
    { "flight", "alphaMinDeg", P_FLOAT, &s_alphaMinDeg, "-6", -30.0f, 0.0f, "look only: lowest angle of attack (deg)" },
    { "flight", "alphaMaxDeg", P_FLOAT, &s_alphaMaxDeg, "20", 0.0f, 45.0f, "look only: highest angle of attack (deg)" },
    { "flight", "tauAlpha", P_FLOAT, &s_tauAlpha, "0.15", 0.005f, 2.0f, "look only: angle of attack lag (s)" },
    { "flight", "stabMaxDeg", P_FLOAT, &s_stabMaxDeg, "20", 0.0f, 45.0f, "look only: stabilator travel for full pull/push (deg)" },
    { "flight", "stabRollDeg", P_FLOAT, &s_stabRollDeg, "8", 0.0f, 30.0f, "look only: differential stabilator for full roll (deg)" },
    { "flight", "flapMaxDeg", P_FLOAT, &s_flapMaxDeg, "20", 0.0f, 45.0f, "look only: flaperon travel for full roll (deg)" },
    { "flight", "surfTau", P_FLOAT, &s_surfTau, "0.08", 0.005f, 1.0f, "look only: control surface movement lag (s)" },
    { "flight", "abOnSec", P_FLOAT, &s_abOnSec, "0.3", 0.01f, 5.0f, "afterburner flame/sound level 0 -> 1 time (s)" },
    { "flight", "abOffSec", P_FLOAT, &s_abOffSec, "0.6", 0.01f, 5.0f, "afterburner flame/sound level 1 -> 0 time (s)" },
    { "flight", "brakeSec", P_FLOAT, &s_brakeSec, "0.5", 0.01f, 5.0f, "speed brake open/close time (s)" },
};

void flight_params(void) { params_add(s_tab, E_COUNT(s_tab)); }

/* ---------------------------------------------------------------- helpers */
/* 1 - exp(-dt/tau) with a one-entry cache per call site (pure function of (tau, dt): deterministic) */
typedef struct LagK { float tau, dt, k; } LagK;
static LagK s_lagG, s_lagRoll, s_lagAlpha, s_lagSurf;
static float lag_k(LagK *c, float tau, float dt)
{
    if (c->tau != tau || c->dt != dt) {
        c->tau = tau;
        c->dt = dt;
        c->k = 1.0f - (float)exp(-dt / tau);
    }
    return c->k;
}

static float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static float smooth01(float x) { x = clampf(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }

static float move_to(float x, float target, float step)
{
    if (x < target) { x += step; return x > target ? target : x; }
    x -= step;
    return x < target ? target : x;
}

float flight_g_limit(float V)
{
    float n = ptable_eval(&s_gLimTab, V * MS2KT) * s_turnBoost;
    return n < 1.0f ? 1.0f : n;
}

float flight_roll_rate_max(float V) { return E_DEG2RAD(ptable_eval(&s_rollTab, V * MS2KT)); }
float flight_speed_floor(void) { return s_vFloorKt * KT2MS; }
float flight_neg_g_limit(void) { return s_negG; }
float flight_tau_g(void) { return s_tauG; }

float flight_speed_target(int thr, float h)
{
    if (thr == THR_FAST) return s_machFast * speed_of_sound(h);
    if (thr == THR_SLOW) return s_vManKt * KT2MS;
    return s_machNormal * speed_of_sound(h);
}

/* DESIGN 4.4: throttle acceleration (before the ceiling fade) and the target it works towards */
static float thr_accel(int thr, float V, float h, float *VtOut)
{
    float Vt, a, vMan, vFloor, band, d;
    if (thr == THR_SLOW) {
        vMan = s_vManKt * KT2MS;
        vFloor = s_vFloorKt * KT2MS;
        if (V > vMan) {                             /* speed brake, fading out just above vMan */
            band = s_vManBandKt * KT2MS;
            d = s_decSlow + (s_decBrake - s_decSlow) * clampf((V - vMan) / band, 0.0f, 1.0f);
            *VtOut = vMan;
            return -d;
        }
        *VtOut = vFloor;                            /* slow bleed to the floor, easing in at the end */
        a = (vFloor - V) / s_tauV;
        return a < -s_decSlow ? -s_decSlow : a;
    }
    Vt = (thr == THR_FAST ? s_machFast : s_machNormal) * speed_of_sound(h);
    *VtOut = Vt;
    a = (Vt - V) / s_tauV;
    if (V < Vt) {
        d = thr == THR_FAST ? s_accAB : s_accMil;
        return a > d ? d : a;
    }
    return a < -s_decOver ? -s_decOver : a;
}

/* T10a: thrust lapse with altitude (replaces the T05 ceilingFade/ceilDrag pair, which pinned the speed at the floor
   in dives above 15 km). ISA density ratio sigma(h); thrust ~ sigma^k, normalised so the factor is 1 at
   thrustFullM and 0 at ceilingM:  lapse = clamp((sigma^k - sigmaC^k) / (sigmaF^k - sigmaC^k), 0, 1).
   It scales only positive throttle acceleration (braking works at any height). Pure function of h and three
   params; a 161-entry table (every 200 m, 0..32 km) is rebuilt when one of the params changes (F5), so the hot path
   has no pow/exp. Deterministic (same params -> same table). */
enum { LAPSE_N = 161 };
#define LAPSE_STEP 200.0f
static float s_lapseTab[LAPSE_N], s_lapseKey[3] = { -1.0f, -1.0f, -1.0f };

static double isa_sigma(double h)
{
    if (h <= 11000.0) return pow(1.0 - h / 44330.8, 4.2559);
    return 0.29707 * exp(-(h - 11000.0) / 6341.6);
}

static void lapse_build(void)
{
    double k = s_lapseExp, sf, sc, d, v;
    float hf = s_thrustFullM, hc = s_ceilingM;
    int i;
    if (hf > hc - 500.0f) hf = hc - 500.0f;         /* keep a fade band even with odd ini values */
    if (hf < 0.0f) hf = 0.0f;
    sf = pow(isa_sigma(hf), k);
    sc = pow(isa_sigma(hc), k);
    d = sf - sc;
    for (i = 0; i < LAPSE_N; i++) {
        v = d > 1e-9 ? (pow(isa_sigma((double)i * LAPSE_STEP), k) - sc) / d : 0.0;
        s_lapseTab[i] = (float)(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
    }
    s_lapseKey[0] = s_lapseExp;
    s_lapseKey[1] = s_thrustFullM;
    s_lapseKey[2] = s_ceilingM;
}

float flight_thrust_lapse(float h)
{
    float x;
    int i;
    if (s_lapseKey[0] != s_lapseExp || s_lapseKey[1] != s_thrustFullM || s_lapseKey[2] != s_ceilingM) lapse_build();
    if (h <= s_thrustFullM) return 1.0f;
    if (h >= s_ceilingM) return 0.0f;
    x = h * (1.0f / LAPSE_STEP);
    i = (int)x;
    if (i >= LAPSE_N - 1) return s_lapseTab[LAPSE_N - 1];
    x -= (float)i;
    return s_lapseTab[i] + (s_lapseTab[i + 1] - s_lapseTab[i]) * x;
}

float flight_ceiling(void) { return s_ceilingM; }

Quat flight_path_quat(float headingRad, float gammaRad, float bankRad)
{
    Quat q = q_from_yaw(headingRad);
    q = q_mul(q, q_axis_angle(v3(1.0f, 0.0f, 0.0f), -gammaRad));   /* + about X = nose down */
    q = q_mul(q, q_axis_angle(v3(0.0f, 0.0f, 1.0f), -bankRad));    /* + about Z = roll left */
    return q_norm(q);
}

static void derive_m(FlightState *fs, const Mat3 *mm)
{
    float sa, ca, ny, a = fs->alpha, a2 = a * a;
    const Mat3 &m = *mm;                            /* rows: right, up, forward */
    fs->heading = (float)atan2(m.m[2][0], m.m[2][2]);
    fs->gamma = (float)asin(clampf(m.m[2][1], -1.0f, 1.0f));
    sa = a * (1.0f - a2 * (1.0f / 6.0f) * (1.0f - a2 * (1.0f / 20.0f)));   /* |alpha| <= 45 deg: err < 2e-5 */
    ca = 1.0f - a2 * 0.5f * (1.0f - a2 * (1.0f / 12.0f) * (1.0f - a2 * (1.0f / 30.0f)));
    ny = m.m[2][1] * ca + m.m[1][1] * sa;           /* nose = forward rotated up by alpha */
    fs->pitch = (float)asin(clampf(ny, -1.0f, 1.0f));
    fs->bank = (float)atan2(-m.m[0][1], m.m[1][1]);
    fs->mach = fs->V / speed_of_sound(fs->pos.y);
    fs->nHold = m.m[1][1];
    fs->nLim = flight_g_limit(fs->V);
    fs->vs = fs->V * m.m[2][1];
}

void flight_derive(FlightState *fs)
{
    Mat3 m;
    q_to_mat3(fs->qPath, &m);
    derive_m(fs, &m);
}

void flight_init(FlightState *fs, Vec3 pos, float headingRad, float speedMs)
{
    memset(fs, 0, sizeof(*fs));
    fs->pos = pos;
    fs->qPath = q_from_yaw(headingRad);
    fs->V = speedMs;
    fs->n = fs->nCmd = 1.0f;
    fs->thr = THR_NORMAL;
    fs->peakG = 1.0f;
    fs->Vt = flight_speed_target(THR_NORMAL, pos.y);
    flight_derive(fs);
}

void flight_set_path(FlightState *fs, float headingRad, float gammaRad, float bankRad)
{
    fs->qPath = flight_path_quat(headingRad, gammaRad, bankRad);
    flight_derive(fs);
    fs->n = fs->nCmd = fs->nHold;
    fs->p = 0.0f;
}

void flight_reset_peak(FlightState *fs) { fs->peakG = fs->n; }

Quat flight_nose(const FlightState *fs)
{
    return q_mul(fs->qPath, q_axis_angle(v3(1.0f, 0.0f, 0.0f), -fs->alpha));
}

/* DESIGN 4.8 */
static void update_surfaces(FlightState *fs, float ur, float dt)
{
    float frac, base, k, nNeg = s_negG, vkt, sR, sL, fR, fL;
    if (fs->n >= fs->nHold) frac = (fs->n - fs->nHold) / E_MAXF(fs->nLim - fs->nHold, 0.1f);
    else frac = -(fs->nHold - fs->n) / E_MAXF(fs->nHold - nNeg, 0.1f);
    if (fs->onGround) frac = 0.0f;
    frac = clampf(frac, -1.0f, 1.0f);
    base = E_DEG2RAD(s_stabMaxDeg) * frac;          /* pull = trailing edges up */
    sR = base + E_DEG2RAD(s_stabRollDeg) * ur;      /* roll right: right tail TE up, left TE down */
    sL = base - E_DEG2RAD(s_stabRollDeg) * ur;
    fR = E_DEG2RAD(s_flapMaxDeg) * ur;              /* roll right: right flaperon up, left down */
    fL = -fR;
    k = lag_k(&s_lagSurf, s_surfTau, dt);
    fs->vis.stabL += (sL - fs->vis.stabL) * k;
    fs->vis.stabR += (sR - fs->vis.stabR) * k;
    fs->vis.flapL += (fL - fs->vis.flapL) * k;
    fs->vis.flapR += (fR - fs->vis.flapR) * k;
    fs->vis.nozzle = fs->ab;
    vkt = fs->V * MS2KT;
    fs->vis.vapour = clampf((fs->n - 6.0f) * 0.5f, 0.0f, 1.0f) * clampf((650.0f - vkt) / 250.0f, 0.0f, 1.0f);
}

/* ---------------------------------------------------------------- the step (DESIGN 4.3) */
void flight_step(FlightState *fs, const PilotInput *in, float dt)
{
    Mat3 m;
    Vec3 r, u, f, w;
    Quat dq;
    float up, ur, V, h, nHold, nLim, nNeg, nCmd, qCapG, gam, phi, s, sCeil, fac, pCmd, qRate, rRate, ang;
    float aThr, aGrav, aInd, aCeil, Vt, Vmax, vFloor, aTgt, hdg0, e;

    up = clampf(in->up, -1.0f, 1.0f);
    ur = clampf(in->ur, -1.0f, 1.0f);
    fs->thr = in->thr;
    fs->capped = 0;

    /* throttle visuals: afterburner and speed brake levels (linear lags) */
    fs->ab = move_to(fs->ab, in->thr == THR_FAST ? 1.0f : 0.0f,
                     dt / (in->thr == THR_FAST ? s_abOnSec : s_abOffSec));
    fs->brake = move_to(fs->brake, in->thr == THR_SLOW ? 1.0f : 0.0f, dt / s_brakeSec);

    if (fs->onGround) {                             /* T06 moves the jet on the ground */
        fs->n = fs->nCmd = 1.0f;
        fs->p = 0.0f;
        fs->turnRate = fs->pitchRate = 0.0f;
        fs->Vt = flight_speed_target(in->thr, fs->pos.y);
        flight_derive(fs);
        update_surfaces(fs, ur, dt);
        return;
    }

    vFloor = s_vFloorKt * KT2MS;
    if (fs->V < vFloor) fs->V = vFloor;
    V = fs->V;
    h = fs->pos.y;
    q_to_mat3(fs->qPath, &m);
    r = v3(m.m[0][0], m.m[0][1], m.m[0][2]);
    u = v3(m.m[1][0], m.m[1][1], m.m[1][2]);
    f = v3(m.m[2][0], m.m[2][1], m.m[2][2]);
    hdg0 = fs->heading;                             /* derived values of the last tick (flight_derive after any
                                                       manual change of qPath / V) */

    /* 1 load factor: auto-trim + command within the G limit */
    nHold = u.y;                                    /* = cos(gamma) * cos(bank) */
    nLim = flight_g_limit(V);
    nNeg = s_negG;
    if (up >= 0.0f) nCmd = nHold + up * (nLim - nHold);
    else nCmd = nHold + (-up) * (nNeg - nHold);
    qCapG = E_DEG2RAD(s_qCapDeg) * V / GRAV;        /* pitch-rate cap as a G band around nHold */
    if (nCmd > nHold + qCapG) { nCmd = nHold + qCapG; fs->capped |= 1; }
    if (nCmd < nHold - qCapG) { nCmd = nHold - qCapG; fs->capped |= 1; }
    nCmd = clampf(nCmd, nNeg, nLim);

    /* 2 nose drop at the speed floor (climbing) and above the hard ceiling (DESIGN 4.5) */
    gam = fs->gamma;
    phi = fs->bank;
    s = 0.0f;
    if (gam > 0.0f)
        s = smooth01((vFloor + s_floorBandKt * KT2MS - V) / (s_floorBandKt * KT2MS)) *
            clampf(gam / E_DEG2RAD(10.0f), 0.0f, 1.0f);
    if (h > s_ceilingM) {                           /* above the ceiling: gently towards a 5 deg descent */
        sCeil = 0.5f * smooth01((h - s_ceilingM - 1000.0f) / 1500.0f) *
                clampf((gam + E_DEG2RAD(5.0f)) / E_DEG2RAD(10.0f), 0.0f, 1.0f);
        if (sCeil > s) s = sCeil;
    }
    if (s > 0.0f) {
        fac = (float)cos(phi);                      /* towards the pilot's "down": upright and inverted */
        if (fac >= 0.0f && fac < 0.5f) fac = 0.5f;
        if (fac < 0.0f && fac > -0.5f) fac = -0.5f;
        nCmd -= s * E_DEG2RAD(s_floorDropDeg) * V / GRAV * fac;
        fs->capped |= 2;
    }
    fs->nCmd = nCmd;
    fs->n += (nCmd - fs->n) * lag_k(&s_lagG, s_tauG, dt);

    /* 3 roll rate with the wings-level detent */
    pCmd = ur * flight_roll_rate_max(V);
    if (ur == 0.0f && s_detentDeg > 0.0f && (float)fabs(phi) < E_DEG2RAD(s_detentDeg) && (float)fabs(fs->p) < 0.5f &&
        (float)fabs(gam) < E_DEG2RAD(60.0f))
        pCmd = -phi * s_detentGain;
    fs->p += (pCmd - fs->p) * lag_k(&s_lagRoll, s_tauRoll, dt);

    /* 4 rotate the path frame: pitch about right (nose up = -X), gravity's side pull about up (+Y = right),
       roll about forward (roll right = -Z); one rotation vector, renormalised every tick */
    qRate = GRAV * (fs->n - nHold) / V;
    rRate = s_latGrav * GRAV * (-r.y) / V;
    w = v3(-qRate * dt, rRate * dt, -fs->p * dt);
    ang = v3_dot(w, w);                             /* angle^2; < 0.01 at any rate the model can reach */
    if (ang > 1e-18f) {                             /* exp of the rotation vector, series to angle^4 */
        e = 0.5f * (1.0f - ang * (1.0f / 24.0f));
        dq.x = w.x * e; dq.y = w.y * e; dq.z = w.z * e;
        dq.w = 1.0f - ang * 0.125f * (1.0f - ang * (1.0f / 48.0f));
        fs->qPath = q_norm(q_mul(fs->qPath, dq));
    }
    fs->pitchRate = qRate;

    /* 5 speed (DESIGN 4.4, 4.5) */
    aThr = thr_accel(in->thr, V, h, &Vt);
    if (aThr > 0.0f) aThr *= flight_thrust_lapse(h);   /* T10a: thrust fades with altitude, 0 at the ceiling */
    aGrav = -GRAV * f.y * s_energyTrade;            /* f.y = sin(gamma) */
    aInd = -s_inducedPerG * E_MAXF(0.0f, (float)fabs(fs->n) - 1.0f);
    aCeil = 0.0f;
    if (h > s_ceilingM) {                           /* thin air above the ceiling: no speed trap (see below) */
        e = (h - s_ceilingM) * (1.0f / 1000.0f);
        aCeil = -s_thinAirDecel * clampf(e, 0.0f, 1.0f) *  /* <= 1.5 m/s^2, grows with speed (0 at the floor, */
                clampf((V - vFloor) / (200.0f * KT2MS), 0.0f, 1.0f);   /* full 200 kt above): dives always gain */
        if (f.y > 0.0f) aCeil -= s_zoomDrag * e * GRAV * f.y * s_energyTrade;   /* climbing only: zooms end */
    }
    V += (aThr + aGrav + aInd + aCeil) * dt;
    Vmax = s_vMaxMach * speed_of_sound(h);
    if (V > Vmax) V = Vmax;
    if (V <= vFloor) { V = vFloor; fs->capped |= 4; }
    fs->V = V;
    fs->Vt = Vt;

    /* 6 position along the new path */
    q_to_mat3(fs->qPath, &m);
    fs->pos = v3_madd(fs->pos, v3(m.m[2][0], m.m[2][1], m.m[2][2]), V * dt);

    /* 7 cosmetic alpha, peak G, derived values, surfaces */
    aTgt = E_DEG2RAD(s_alpha1gDeg) * fs->n * (s_alphaRefKt * KT2MS / V) * (s_alphaRefKt * KT2MS / V);
    aTgt = clampf(aTgt, E_DEG2RAD(s_alphaMinDeg), E_DEG2RAD(s_alphaMaxDeg));
    fs->alpha += (aTgt - fs->alpha) * lag_k(&s_lagAlpha, s_tauAlpha, dt);
    if (fs->n > fs->peakG) fs->peakG = fs->n;
    derive_m(fs, &m);
    fs->turnRate = wrap_pi(fs->heading - hdg0) / dt;
    update_surfaces(fs, ur, dt);
}

/* ---------------------------------------------------------------- -telemetry CSV (DESIGN 2.7) */
static FILE *s_tel;
static int s_telRows;

int flight_telemetry_open(const char *path)
{
    flight_telemetry_close();
    s_tel = fopen(path, "wb");
    if (!s_tel) {
        log_printf(LOG_WARN, "warn: telemetry open failed path=%s", path);
        return 0;
    }
    fprintf(s_tel, "tick,t,kUp,kDown,kLeft,kRight,kFast,kSlow,up,ur,thr,n,nCmd,nLim,nHold,Vkt,Vtkt,mach,alphaDeg,"
                   "bankDeg,pitchDeg,gammaDeg,hdgDeg,altFt,vsFpm,rollRateDeg,turnRateDeg,ab,brake,capped\r\n");
    s_telRows = 0;
    log_printf(LOG_INFO, "telemetry: open path=%s", path);
    return 1;
}

void flight_telemetry_row(u32 tick, const PilotKeys *k, const PilotInput *in, const FlightState *fs)
{
    float hdg;
    if (!s_tel) return;
    hdg = E_RAD2DEG(fs->heading);
    if (hdg < 0.0f) hdg += 360.0f;
    fprintf(s_tel, "%u,%.4f,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%d,%.3f,%.3f,%.2f,%.3f,%.1f,%.1f,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,"
                   "%.0f,%.0f,%.1f,%.2f,%.2f,%.2f,%d\r\n",
            tick, (double)tick / 120.0, k->up, k->down, k->left, k->right, k->fast, k->slow, (double)in->up,
            (double)in->ur, in->thr, (double)fs->n, (double)fs->nCmd, (double)fs->nLim, (double)fs->nHold,
            (double)(fs->V * MS2KT), (double)(fs->Vt * MS2KT), (double)fs->mach, (double)E_RAD2DEG(fs->alpha),
            (double)E_RAD2DEG(fs->bank), (double)E_RAD2DEG(fs->pitch), (double)E_RAD2DEG(fs->gamma), (double)hdg,
            (double)(fs->pos.y * M2FT), (double)(fs->vs * M2FT * 60.0f), (double)E_RAD2DEG(fs->p),
            (double)E_RAD2DEG(fs->turnRate), (double)fs->ab, (double)fs->brake, fs->capped);
    s_telRows++;
}

void flight_telemetry_close(void)
{
    if (!s_tel) return;
    fclose(s_tel);
    s_tel = NULL;
    log_printf(LOG_INFO, "telemetry: close rows=%d", s_telRows);
}

int flight_telemetry_rows(void) { return s_telRows; }
