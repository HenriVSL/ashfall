/* pilot.cpp - keys -> shaped axes, throttle state, short/long press classifier (DESIGN 5). Portable. Owner: T05.
   See pilot.h for the axis model, the event rules and the Expert / Novice modes. Every tunable is a [pilot] key
   (live on F5). */
#include "pilot.h"
#include "params.h"
#include "game.h"                                   /* GRAV; flight.h (Novice reads the aircraft state) */
#include "e_log.h"

#define PILOT_DT (1.0f / 120.0f)                    /* pilot_update runs once per sim tick (SIM_DT, game.h) */

/* ---------------------------------------------------------------- [pilot] (DESIGN 5.2) */
static float s_pitchFine, s_pitchRateFine, s_pitchRate, s_pitchReturn, s_pitchCross;
static float s_rollRateIn, s_rollReturn, s_rollCross, s_rollFine, s_rollRateFine;
static int   s_invertPitch, s_longPressMs, s_flareRepeatMs;
static float s_joyExpo, s_joyDead;
static char  s_mode[PARAM_STR];
static float s_nvBankDeg, s_nvBankGain, s_nvRollRateDeg, s_nvGammaRateDeg, s_nvPitchLimitDeg, s_nvGammaGain;
static float s_nvLevelGain;
static int   s_nvAutoLevel;

static const Param s_tab[] = {
    { "pilot", "pitchFine", P_FLOAT, &s_pitchFine, "0.35", 0.0f, 1.0f,
      "pitch axis first stage (fine aiming): the axis moves at pitchRateFine up to this value, then at pitchRate (0 = one stage)" },
    { "pilot", "pitchRateFine", P_FLOAT, &s_pitchRateFine, "3.0", 0.1f, 50.0f,
      "pitch axis speed in the first stage (axis/s): 3.0 = 0 -> 0.35 in 0.12 s; lower = finer taps" },
    { "pilot", "pitchRate", P_FLOAT, &s_pitchRate, "1.8", 0.1f, 50.0f,
      "pitch axis speed after the first stage (axis/s): 1.8 = 0.35 -> 1 in 0.36 s (full pull after ~0.5 s hold)" },
    { "pilot", "pitchReturn", P_FLOAT, &s_pitchReturn, "6.0", 0.1f, 100.0f,
      "pitch axis speed back to 0 when the key is let go (axis/s): 6 = full -> 0 in 0.17 s" },
    { "pilot", "pitchCross", P_FLOAT, &s_pitchCross, "8.0", 0.1f, 100.0f,
      "pitch axis speed back to 0 when the opposite key is pressed (axis/s)" },
    { "pilot", "rollRateIn", P_FLOAT, &s_rollRateIn, "6.0", 0.1f, 100.0f,
      "roll axis speed towards the pressed side (axis/s): 6 = full roll command in 0.17 s" },
    { "pilot", "rollReturn", P_FLOAT, &s_rollReturn, "10.0", 0.1f, 100.0f,
      "roll axis speed back to 0 when the key is let go (axis/s): high = the bank stops where you leave it" },
    { "pilot", "rollCross", P_FLOAT, &s_rollCross, "12.0", 0.1f, 100.0f,
      "roll axis speed back to 0 when the opposite key is pressed (axis/s)" },
    { "pilot", "rollFine", P_FLOAT, &s_rollFine, "0", 0.0f, 1.0f,
      "optional roll first stage like pitchFine (0 = off, one stage). Try 0.3 if small bank corrections are too coarse" },
    { "pilot", "rollRateFine", P_FLOAT, &s_rollRateFine, "2.0", 0.1f, 50.0f,
      "roll axis speed in the optional first stage (axis/s, only when rollFine > 0)" },
    { "pilot", "invertPitch", P_BOOL, &s_invertPitch, "0", 0, 1,
      "0 = Up arrow nose UP (default), 1 = Up arrow nose DOWN (flight-sim style)" },
    { "pilot", "longPressMs", P_INT, &s_longPressMs, "300", 50, 2000,
      "S held this long (ms) = long press (designate); shorter = short press (next target)" },
    { "pilot", "flareRepeatMs", P_INT, &s_flareRepeatMs, "500", 100, 5000,
      "F held: one flare burst on the press, then one every this many ms" },
    { "pilot", "joyExpo", P_FLOAT, &s_joyExpo, "0.3", 0.0f, 1.0f,
      "joystick expo: 0 = linear, 1 = cubic (finer around the centre)" },
    { "pilot", "joyDead", P_FLOAT, &s_joyDead, "0.05", 0.0f, 0.5f,
      "joystick deadzone (fraction of full travel)" },
    { "pilot", "mode", P_STR, s_mode, "expert", 0, 0,
      "expert = true roll + pitch (Left/Right roll, Up/Down pull); novice = Left/Right turn, Up/Down climb/dive (F6 toggles)" },
    { "pilot", "noviceBankDeg", P_FLOAT, &s_nvBankDeg, "80", 10.0f, 85.0f,
      "novice: bank angle a held Left/Right turn goes to (deg); more = tighter level turn" },
    { "pilot", "noviceBankGain", P_FLOAT, &s_nvBankGain, "5", 0.5f, 30.0f,
      "novice: how firmly the bank is steered to its target (1/s); higher = snappier, lower = softer" },
    { "pilot", "noviceRollRateDeg", P_FLOAT, &s_nvRollRateDeg, "90", 10.0f, 360.0f,
      "novice: fastest automatic roll (deg/s), into a turn and back to wings level" },
    { "pilot", "noviceGammaRateDeg", P_FLOAT, &s_nvGammaRateDeg, "15", 1.0f, 60.0f,
      "novice: climb/dive angle change with Up/Down held (deg/s, less when the G limit is reached)" },
    { "pilot", "novicePitchLimitDeg", P_FLOAT, &s_nvPitchLimitDeg, "75", 10.0f, 89.0f,
      "novice: steepest climb or dive (deg): no loops, no inverted flight" },
    { "pilot", "noviceGammaGain", P_FLOAT, &s_nvGammaGain, "2.0", 0.0f, 20.0f,
      "novice: how firmly the climb angle is held (1/s)" },
    { "pilot", "noviceAutoLevelPitch", P_BOOL, &s_nvAutoLevel, "0", 0, 1,
      "novice: 0 = Up/Down released holds the climb angle, 1 = eases back to level flight" },
    { "pilot", "noviceLevelGain", P_FLOAT, &s_nvLevelGain, "0.5", 0.05f, 5.0f,
      "novice with noviceAutoLevelPitch=1: how fast the climb angle eases to level (1/s)" },
};

void pilot_params(void) { params_add(s_tab, E_COUNT(s_tab)); }

int pilot_long_press_ticks(void)
{
    int t = (s_longPressMs * 120 + 500) / 1000;
    return t < 1 ? 1 : t;
}

int pilot_invert_pitch(void) { return s_invertPitch; }

/* ---------------------------------------------------------------- press classifier (DESIGN 5.3) */
void press_reset(PressClass *pc) { pc->held = 0; pc->longFired = 0; }

int press_classify(PressClass *pc, int heldTicks, int releasedTicks, int longTicks)
{
    int ev;
    if (releasedTicks > 0) {
        /* release poll: short unless long already fired (a missed long on a gap in the counters still counts) */
        if (pc->longFired) ev = PRESS_NONE;
        else ev = releasedTicks >= longTicks ? PRESS_LONG : PRESS_SHORT;
        press_reset(pc);
        return ev;
    }
    if (heldTicks > 0) {
        pc->held = heldTicks;
        if (!pc->longFired && heldTicks >= longTicks) {
            pc->longFired = 1;
            return PRESS_LONG;
        }
        return PRESS_NONE;
    }
    press_reset(pc);                                /* idle, or focus loss: no event */
    return PRESS_NONE;
}

void press_counter_feed(PressCounter *c, int down, int *heldTicks, int *releasedTicks)
{
    if (down) {
        c->held++;
        *heldTicks = c->held;
        *releasedTicks = 0;
    } else {
        *heldTicks = 0;
        *releasedTicks = c->held;
        c->held = 0;
    }
}

/* ---------------------------------------------------------------- axis shaping (DESIGN 5.2) */
static float sgnf(float x) { return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f); }

float pilot_axis_step(float x, int s, float fine, float rateFine, float rate, float ret, float cross, float dt)
{
    float a, t = dt, fs;
    if (s == 0) {                                   /* released (or both keys): back to 0 */
        a = (float)fabs(x) - ret * dt;
        return a <= 0.0f ? 0.0f : sgnf(x) * a;
    }
    fs = (float)s;
    if (x * fs < 0.0f) {                            /* against the axis: fast back to 0, then the stages */
        a = (float)fabs(x);
        if (a > cross * t) return sgnf(x) * (a - cross * t);
        t -= a / cross;
        x = 0.0f;
    }
    a = (float)fabs(x);
    if (fine > 0.0f && a < fine) {                  /* first stage */
        a += rateFine * t;
        if (a <= fine) return fs * a;
        t = (a - fine) / rateFine;                  /* time left after reaching the stage boundary */
        a = fine;
    }
    a += rate * t;
    if (a > 1.0f) a = 1.0f;
    return fs * a;
}

float pilot_joy_shape(float v)
{
    float a = (float)fabs(v), d = s_joyDead;
    if (a <= d) return 0.0f;
    a = (a - d) / (1.0f - d);
    if (a > 1.0f) a = 1.0f;
    a = (1.0f - s_joyExpo) * a + s_joyExpo * a * a * a;
    return sgnf(v) * a;
}

/* ---------------------------------------------------------------- per tick */
void pilot_reset(PilotState *ps) { memset(ps, 0, sizeof(*ps)); }

void pilot_seed_axes(PilotState *ps, float up, float ur)
{
    ps->up = E_CLAMP(up, -1.0f, 1.0f);
    ps->ur = E_CLAMP(ur, -1.0f, 1.0f);
}

/* ---------------------------------------------------------------- modes */
int pilot_mode(void) { return !e_stricmp(s_mode, "novice") ? PILOT_NOVICE : PILOT_EXPERT; }
const char *pilot_mode_name(int mode) { return mode == PILOT_NOVICE ? "NOVICE" : "EXPERT"; }

void pilot_set_mode(int mode)
{
    e_strlcpy(s_mode, mode == PILOT_NOVICE ? "novice" : "expert", PARAM_STR);
    log_printf(LOG_INFO, "pilot: mode=%s", s_mode);
}

int pilot_toggle_mode(void)
{
    int m = pilot_mode() == PILOT_NOVICE ? PILOT_EXPERT : PILOT_NOVICE;
    pilot_set_mode(m);
    return m;
}

/* Novice: key intents (ps->up/ur) -> model commands (out->up/ur) for the same flight model */
static void novice_commands(PilotState *ps, const FlightState *fs, PilotInput *out)
{
    float bankT, err, pDes, pMax, rMax, lim, gdot, g0, cphi, nC, nLim, nNeg, nHold, up, lead;
    int fresh = !ps->gRefValid;
    /* roll: steer the bank to the target (0 when released) */
    bankT = ps->ur * E_DEG2RAD(s_nvBankDeg);
    err = wrap_pi(bankT - fs->bank);
    pMax = E_DEG2RAD(s_nvRollRateDeg);
    pDes = E_CLAMP(err * s_nvBankGain, -pMax, pMax);
    rMax = flight_roll_rate_max(fs->V);
    out->ur = rMax > 1e-3f ? E_CLAMP(pDes / rMax, -1.0f, 1.0f) : 0.0f;
    /* pitch: integrate a flight-path angle reference, limited, held when released */
    lim = E_DEG2RAD(s_nvPitchLimitDeg);
    if (!ps->gRefValid) { ps->gRef = E_CLAMP(fs->gamma, -lim, lim); ps->gRefValid = 1; }
    gdot = ps->up * E_DEG2RAD(s_nvGammaRateDeg);
    if (ps->up == 0.0f && s_nvAutoLevel) gdot = -ps->gRef * s_nvLevelGain;
    /* fade the climb/dive rate over the last 10 deg before the limit (no overshoot at 75) */
    if (gdot > 0.0f) gdot *= E_CLAMP((lim - ps->gRef) / E_DEG2RAD(10.0f), 0.0f, 1.0f);
    if (gdot < 0.0f) gdot *= E_CLAMP((lim + ps->gRef) / E_DEG2RAD(10.0f), 0.0f, 1.0f);
    g0 = ps->gRef;
    ps->gRef += gdot * (1.0f / 120.0f);
    ps->gRef = E_CLAMP(ps->gRef, -lim, lim);
    ps->gRef = E_CLAMP(ps->gRef, fs->gamma - E_DEG2RAD(5.0f), fs->gamma + E_DEG2RAD(5.0f));   /* anti-windup */
    gdot = (ps->gRef - g0) * 120.0f + s_nvGammaGain * (ps->gRef - fs->gamma);
    /* lift for that path-angle rate in this bank: V*gammaDot = g*(n*cos(bank) - cos(gamma)) */
    cphi = (float)cos(fs->bank);
    if (cphi < 0.2f) cphi = 0.2f;
    nC = ((float)cos(fs->gamma) + fs->V * gdot / GRAV) / cphi;
    /* lead: cancel the model's G onset lag (tauG), so the nose stops where the key leaves it */
    if (fresh) ps->nDesPrev = nC;
    lead = flight_tau_g() * (nC - ps->nDesPrev) * 120.0f;
    ps->nDesPrev = nC;
    nC += E_CLAMP(lead, -3.0f, 3.0f);
    nLim = flight_g_limit(fs->V);
    nNeg = flight_neg_g_limit();
    nHold = fs->nHold;
    nC = E_CLAMP(nC, nNeg, nLim);
    if (nC >= nHold) up = (nC - nHold) / E_MAXF(nLim - nHold, 0.05f);   /* the model's up -> nCmd, inverted */
    else up = (nC - nHold) / E_MAXF(nHold - nNeg, 0.05f);
    out->up = E_CLAMP(up, -1.0f, 1.0f);
}

void pilot_update(PilotState *ps, const PilotKeys *k, PilotInput *out) { pilot_update_fs(ps, k, NULL, out); }

void pilot_update_fs(PilotState *ps, const PilotKeys *k, const FlightState *fs, PilotInput *out)
{
    int sp, sr, ev, rep;
    float jx = 0.0f, jy = 0.0f;
    memset(out, 0, sizeof(*out));

    sp = (k->up ? 1 : 0) - (k->down ? 1 : 0);
    if (s_invertPitch) sp = -sp;
    sr = (k->right ? 1 : 0) - (k->left ? 1 : 0);
    if (k->joyValid) {
        jy = pilot_joy_shape(k->joyY);              /* stick back (joyY > 0) = nose up */
        if (s_invertPitch) jy = -jy;
        jx = pilot_joy_shape(k->joyX);
    }

    /* pitch: stick (outside the deadzone, no pitch key) or the shaped keyboard axis */
    ps->joyPitch = (k->joyValid && jy != 0.0f && sp == 0);
    if (ps->joyPitch) ps->up = jy;
    else ps->up = pilot_axis_step(ps->up, sp, s_pitchFine, s_pitchRateFine, s_pitchRate, s_pitchReturn,
                                  s_pitchCross, PILOT_DT);
    /* roll */
    ps->joyRoll = (k->joyValid && jx != 0.0f && sr == 0);
    if (ps->joyRoll) ps->ur = jx;
    else ps->ur = pilot_axis_step(ps->ur, sr, s_rollFine, s_rollRateFine, s_rollRateIn, s_rollReturn,
                                  s_rollCross, PILOT_DT);
    out->up = ps->up;
    out->ur = ps->ur;
    {
        int mode = (fs && !fs->onGround && pilot_mode() == PILOT_NOVICE) ? PILOT_NOVICE : PILOT_EXPERT;
        if (mode != ps->lastMode) ps->gRefValid = 0;
        ps->lastMode = mode;
        if (mode == PILOT_NOVICE) novice_commands(ps, fs, out);
    }

    /* throttle state (DESIGN 4.4): A and Z together = NORMAL */
    out->thr = (k->fast && !k->slow) ? THR_FAST : (k->slow && !k->fast) ? THR_SLOW : THR_NORMAL;

    /* S short/long (DESIGN 5.3) */
    ev = press_classify(&ps->tgt, k->tgtHeld, k->tgtReleased, pilot_long_press_ticks());
    out->tgtShort = ev == PRESS_SHORT;
    out->tgtLong = ev == PRESS_LONG;
    ps->longFired = ps->tgt.longFired;

    /* edges and repeats */
    out->fire = k->fire && !ps->prevKeys.fire;
    out->wpnNext = k->wpnNext && !ps->prevKeys.wpnNext;
    out->view = k->view && !ps->prevKeys.view;
    out->look = k->look ? 1 : 0;
    if (k->flare) {
        rep = (s_flareRepeatMs * 120 + 500) / 1000;
        if (rep < 1) rep = 1;
        ps->flareHeld++;
        out->flare = ((ps->flareHeld - 1) % rep) == 0;
    } else {
        ps->flareHeld = 0;
    }
    ps->prevKeys = *k;
}
