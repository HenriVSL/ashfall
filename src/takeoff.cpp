/* takeoff.cpp - ground roll, automated takeoff + climb-out, handover (DESIGN 6.1). Portable. Owner: T06.
   See takeoff.h for the phases. Deviations from DESIGN 6.1 (also in docs/HANDOFF.md T06):
   - rotatePitchDeg is the NOSE attitude at the end of the rotation; the path pitch is nose - alpha (the flight
     model's 1 g alpha at that speed), so the flight model takes over with a consistent alpha (no nose jump).
     Liftoff needs V >= vLiftKt AND the rotation complete (rotateSec).
   - The climb autopilot also tracks the extended runway centre line (trackGainDeg), so the climb-out stays in the
     T03 corridor.
   - HANDOVER blends the autopilot's axes into the player's over handoverBlendSec instead of seeding the player's
     shaped axes (pilot_seed_axes): it works the same in Expert and Novice and needs no extra call in main. */
#include "takeoff.h"
#include "world.h"
#include "params.h"
#include "e_terrain.h"
#include "e_log.h"

/* ---------------------------------------------------------------- [takeoff] */
static float s_rollAccMil, s_rollAccAB, s_vRotateKt, s_rotatePitchDeg, s_rotateSec, s_vLiftKt;
static float s_climbPitchDeg, s_climbGammaRateDeg, s_gearUpFt, s_gearSec, s_handoverFt, s_handoverKt;
static float s_handoverMsgSec, s_handoverBlendSec, s_overrideSec, s_overrideMinFt, s_gearHeightM;
static float s_trackGainDeg, s_trackMaxDeg, s_climbBankDeg, s_climbHdgGain;
static float s_apHdgGain, s_apBankGain, s_apGammaGain, s_apGammaRateDeg, s_apRollRateDeg;

static const Param s_tab[] = {
    { "takeoff", "rollAccMil", P_FLOAT, &s_rollAccMil, "7", 0.5f, 50.0f,
      "takeoff roll acceleration after a tap of A (m/s^2)" },
    { "takeoff", "rollAccAB", P_FLOAT, &s_rollAccAB, "11", 0.5f, 50.0f,
      "takeoff roll acceleration while A is held (afterburner, m/s^2)" },
    { "takeoff", "vRotateKt", P_FLOAT, &s_vRotateKt, "145", 50.0f, 300.0f, "the nose starts to come up at this speed (kt)" },
    { "takeoff", "rotatePitchDeg", P_FLOAT, &s_rotatePitchDeg, "10", 0.0f, 25.0f,
      "nose attitude at the end of the rotation (deg above the horizon)" },
    { "takeoff", "rotateSec", P_FLOAT, &s_rotateSec, "1.5", 0.1f, 10.0f, "rotation time (s)" },
    { "takeoff", "vLiftKt", P_FLOAT, &s_vLiftKt, "160", 50.0f, 300.0f,
      "liftoff speed (kt): from here the flight model flies (never below the speed floor)" },
    { "takeoff", "climbPitchDeg", P_FLOAT, &s_climbPitchDeg, "12", 1.0f, 45.0f,
      "automatic climb-out: flight path angle (deg)" },
    { "takeoff", "climbGammaRateDeg", P_FLOAT, &s_climbGammaRateDeg, "3", 0.2f, 30.0f,
      "automatic climb-out: how fast the climb angle builds up after liftoff (deg/s)" },
    { "takeoff", "climbBankDeg", P_FLOAT, &s_climbBankDeg, "15", 0.0f, 60.0f,
      "automatic climb-out: most bank used to stay on the runway centre line (deg)" },
    { "takeoff", "climbHdgGain", P_FLOAT, &s_climbHdgGain, "8", 0.1f, 20.0f,
      "automatic climb-out: bank per heading error (rad/rad) while holding the centre line" },
    { "takeoff", "trackGainDeg", P_FLOAT, &s_trackGainDeg, "0.15", 0.0f, 10.0f,
      "automatic climb-out: heading correction per metre off the extended centre line (deg/m)" },
    { "takeoff", "trackMaxDeg", P_FLOAT, &s_trackMaxDeg, "10", 0.0f, 45.0f,
      "automatic climb-out: largest heading correction towards the centre line (deg)" },
    { "takeoff", "gearUpFt", P_FLOAT, &s_gearUpFt, "50", 0.0f, 2000.0f, "gear starts to retract at this height above ground (ft)" },
    { "takeoff", "gearSec", P_FLOAT, &s_gearSec, "3", 0.1f, 20.0f, "gear retraction time (s)" },
    { "takeoff", "handoverFt", P_FLOAT, &s_handoverFt, "1500", 100.0f, 20000.0f,
      "the autopilot hands over at this height above ground (ft) ..." },
    { "takeoff", "handoverKt", P_FLOAT, &s_handoverKt, "300", 150.0f, 1000.0f, "... and at this speed (kt)" },
    { "takeoff", "handoverMsgSec", P_FLOAT, &s_handoverMsgSec, "2", 0.0f, 10.0f, "YOU HAVE CONTROL message time (s)" },
    { "takeoff", "handoverBlendSec", P_FLOAT, &s_handoverBlendSec, "0.5", 0.0f, 5.0f,
      "the autopilot's stick fades into yours over this time at the handover (s)" },
    { "takeoff", "overrideSec", P_FLOAT, &s_overrideSec, "0.25", 0.0f, 5.0f,
      "climb-out: a pitch/roll key held this long (s) takes control at once ..." },
    { "takeoff", "overrideMinFt", P_FLOAT, &s_overrideMinFt, "200", 0.0f, 5000.0f,
      "... once higher than this above the ground (ft)" },
    { "takeoff", "gearHeightM", P_FLOAT, &s_gearHeightM, "2.05", 0.0f, 10.0f,
      "jet centre above the runway with the wheels on it (m; A1 mesh: 2.05)" },
    { "takeoff", "apHdgGain", P_FLOAT, &s_apHdgGain, "2.0", 0.1f, 20.0f,
      "autopilot (climb-out, area turn-back): bank per heading error (rad/rad)" },
    { "takeoff", "apBankGain", P_FLOAT, &s_apBankGain, "3.0", 0.1f, 30.0f, "autopilot: roll rate per bank error (1/s)" },
    { "takeoff", "apRollRateDeg", P_FLOAT, &s_apRollRateDeg, "90", 5.0f, 360.0f, "autopilot: fastest roll (deg/s)" },
    { "takeoff", "apGammaGain", P_FLOAT, &s_apGammaGain, "1.5", 0.1f, 20.0f,
      "autopilot: climb angle rate per climb angle error (1/s)" },
    { "takeoff", "apGammaRateDeg", P_FLOAT, &s_apGammaRateDeg, "6", 0.5f, 60.0f, "autopilot: fastest climb angle change (deg/s)" },
};

void takeoff_params(void) { params_add(s_tab, E_COUNT(s_tab)); }
float takeoff_gear_height(void) { return s_gearHeightM; }

static float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static float smooth01(float x) { x = clampf(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }

/* ---------------------------------------------------------------- autopilot */
static void ap_steer_k(const FlightState *fs, float hdgRad, float gammaRad, float maxBankRad, float hdgGain, PilotInput *out);

void ap_steer(const FlightState *fs, float hdgRad, float gammaRad, float maxBankRad, PilotInput *out)
{
    ap_steer_k(fs, hdgRad, gammaRad, maxBankRad, s_apHdgGain, out);
}

static void ap_steer_k(const FlightState *fs, float hdgRad, float gammaRad, float maxBankRad, float hdgGain, PilotInput *out)
{
    float err, bankT, pDes, pMax, rMax, gdot, gMax, cphi, nC, nLim, nNeg, nHold, up, bl;
    nLim = flight_g_limit(fs->V);
    bl = nLim > 1.02f ? (float)acos(1.0f / nLim) : 0.0f;   /* steepest level-turn bank the G limit allows */
    if (maxBankRad > bl) maxBankRad = bl;
    /* roll: bank towards the heading, steer the bank */
    err = wrap_pi(hdgRad - fs->heading);
    bankT = clampf(err * hdgGain, -maxBankRad, maxBankRad);
    pMax = E_DEG2RAD(s_apRollRateDeg);
    pDes = clampf(wrap_pi(bankT - fs->bank) * s_apBankGain, -pMax, pMax);
    rMax = flight_roll_rate_max(fs->V);
    out->ur = rMax > 1e-3f ? clampf(pDes / rMax, -1.0f, 1.0f) : 0.0f;
    /* pitch: flight-path angle rate -> load factor (V * gammaDot = g * (n cos(bank) - cos(gamma))) -> up */
    gMax = E_DEG2RAD(s_apGammaRateDeg);
    gdot = clampf((gammaRad - fs->gamma) * s_apGammaGain, -gMax, gMax);
    cphi = (float)cos(fs->bank);
    if (cphi < 0.2f) cphi = 0.2f;
    nC = ((float)cos(fs->gamma) + fs->V * gdot / GRAV) / cphi;
    nNeg = flight_neg_g_limit();
    nHold = fs->nHold;
    nC = clampf(nC, nNeg, nLim);
    if (nC >= nHold) up = (nC - nHold) / E_MAXF(nLim - nHold, 0.05f);   /* inverse of the model's up -> nCmd */
    else up = (nC - nHold) / E_MAXF(nHold - nNeg, 0.05f);
    out->up = clampf(up, -1.0f, 1.0f);
}

/* ---------------------------------------------------------------- helpers */
const char *takeoff_phase_name(int phase)
{
    switch (phase) {
    case TO_READY: return "READY";
    case TO_ROLL: return "ROLL";
    case TO_ROTATE: return "ROTATE";
    case TO_CLIMB: return "CLIMB";
    case TO_HANDOVER: return "HANDOVER";
    case TO_DONE: return "DONE";
    }
    return "?";
}

int takeoff_active(const TakeoffState *ts) { return ts->phase >= TO_READY && ts->phase < TO_DONE; }

float takeoff_centre_line_error(const Game *g, Vec3 pos)
{
    const TerrainBase *b;
    float h;
    if (!g->terrain) return 0.0f;
    b = terrain_base(g->terrain);
    h = E_DEG2RAD(b->rwyHeadingDeg);
    return (pos.x - b->rwyX) * (float)cos(h) - (pos.z - b->rwyZ) * (float)sin(h);   /* + = right of the heading */
}

static float runway_heading(const Game *g, const TakeoffState *ts)
{
    return g->terrain ? E_DEG2RAD(terrain_base(g->terrain)->rwyHeadingDeg) : ts->hdg;
}

static float param_f(const char *sec, const char *key, float def)
{
    const Param *p = params_find(sec, key);
    return (p && p->type == P_FLOAT) ? *(const float *)p->ptr : def;
}

/* the flight model's cosmetic 1 g alpha at speed V (DESIGN 4.3; read from [flight], so F5 changes follow) */
static float alpha_1g(float V)
{
    float ref = param_f("flight", "alphaRefKt", 300.0f) * KT2MS, a;
    if (V < 1.0f) V = 1.0f;
    a = E_DEG2RAD(param_f("flight", "alpha1gDeg", 2.0f)) * (ref / V) * (ref / V);
    return clampf(a, E_DEG2RAD(param_f("flight", "alphaMinDeg", -6.0f)), E_DEG2RAD(param_f("flight", "alphaMaxDeg", 20.0f)));
}

static void set_phase(TakeoffState *ts, int phase, const Aircraft *ac, const Game *g)
{
    const FlightState *fs = &ac->fs;
    ts->phase = phase;
    ts->phaseTick = g->tick;
    log_printf(LOG_INFO, "takeoff: phase=%s tick=%u t=%.2f V=%.1fkt agl=%.0fft pitch=%.1f gamma=%.1f gear=%.2f lat=%.2f",
               takeoff_phase_name(phase), g->tick, ts->tickA ? (double)(g->tick - ts->tickA) / SIM_HZ : 0.0,
               (double)(fs->V * MS2KT), (double)(world_agl(g, fs->pos) * M2FT), (double)E_RAD2DEG(fs->pitch),
               (double)E_RAD2DEG(fs->gamma), (double)fs->gearPos, (double)takeoff_centre_line_error(g, fs->pos));
}

/* 1 = the player is moving the pitch (axis 0) or roll (axis 1) control */
static int player_axis_active(const TakeoffState *ts, const PilotInput *player, int axis)
{
    float v;
    if (ts->ps) v = axis ? ts->ps->ur : ts->ps->up; /* shaped key/stick axes (both modes) */
    else if (pilot_mode() == PILOT_EXPERT) v = axis ? player->ur : player->up;
    else return 0;                                  /* Novice unbound: PilotInput holds commands, not keys */
    return (float)fabs(v) > 0.01f;
}

/* ---------------------------------------------------------------- park / reset */
void takeoff_bind_pilot(TakeoffState *ts, PilotState *ps) { ts->ps = ps; }

void takeoff_park(TakeoffState *ts, Aircraft *ac, const Game *g)
{
    PilotState *ps = ts->ps;
    Vec3 p;
    float hdg;
    memset(ts, 0, sizeof(*ts));
    ts->ps = ps;
    ts->acId = ac->id;
    world_spawn_runway(g, &p, &hdg);
    ts->origin = p;
    ts->hdg = hdg;
    ts->groundY = p.y + s_gearHeightM;
    ts->minClear = 1e9f;
    flight_init(&ac->fs, v3(p.x, ts->groundY, p.z), hdg, 0.0f);
    ac->fs.onGround = 1;
    ac->fs.gearPos = 1.0f;
    ac->fs.alpha = 0.0f;
    ac->flags |= AC_ALIVE | AC_ONGROUND;
    ac->hp = 100.0f;
    memset(&ac->in, 0, sizeof(ac->in));
    ac->in.thr = THR_NORMAL;
    ac->prev = ac->fs;
    world_reset_aircraft(ac->id);
    world_set_takeoff(ac->id, 1);
    set_phase(ts, TO_READY, ac, g);
    world_event_push(g, WEV_TO_READY, ac->id, -1, ac->fs.pos);
}

void takeoff_reset(TakeoffState *ts, Aircraft *ac, const Game *g)
{
    PilotState *ps = ts->ps;
    if (ac->fs.onGround) {
        takeoff_park(ts, ac, g);
        return;
    }
    memset(ts, 0, sizeof(*ts));                     /* air start: nothing to do */
    ts->ps = ps;
    ts->acId = ac->id;
    ts->phase = TO_DONE;
    ts->phaseTick = g->tick;
    world_set_takeoff(ac->id, 0);
}

const char *takeoff_message(const TakeoffState *ts, const Game *g)
{
    if (ts->phase == TO_READY) return "A: TAKEOFF";
    if (ts->phase == TO_HANDOVER && (float)(g->tick - ts->phaseTick) < s_handoverMsgSec * (float)SIM_HZ)
        return "YOU HAVE CONTROL";
    return NULL;
}

/* ---------------------------------------------------------------- per tick */
static void gear_tick(TakeoffState *ts, Aircraft *ac, const Game *g)
{
    if (!ts->gearMoving) return;
    ac->fs.gearPos -= SIM_DT / s_gearSec;
    if (ac->fs.gearPos <= 0.0f) {
        ac->fs.gearPos = 0.0f;
        ts->gearMoving = 0;
        world_event_push(g, WEV_GEAR_LOCKED, ac->id, -1, ac->fs.pos);
    }
}

static void gear_start(TakeoffState *ts, Aircraft *ac, const Game *g)
{
    if (ts->gearMoving || ac->fs.gearPos <= 0.0f) return;
    ts->gearMoving = 1;
    world_event_push(g, WEV_GEAR_UP, ac->id, -1, ac->fs.pos);
}

static void climb_autopilot(TakeoffState *ts, const Aircraft *ac, const Game *g, PilotInput *ap)
{
    float lat = takeoff_centre_line_error(g, ac->fs.pos), corr, step;
    step = E_DEG2RAD(s_climbGammaRateDeg) * SIM_DT;
    ts->gammaCmd += clampf(E_DEG2RAD(s_climbPitchDeg) - ts->gammaCmd, -step, step);
    corr = clampf(-s_trackGainDeg * lat, -s_trackMaxDeg, s_trackMaxDeg);
    ap_steer_k(&ac->fs, runway_heading(g, ts) + E_DEG2RAD(corr), ts->gammaCmd, E_DEG2RAD(s_climbBankDeg),
               s_climbHdgGain, ap);
    ts->apUp = ap->up;
    ts->apUr = ap->ur;
}

static void handover(TakeoffState *ts, Aircraft *ac, const Game *g, int overridden)
{
    const FlightState *fs = &ac->fs;
    ts->overridden = overridden;
    ts->tickHand = g->tick;
    gear_start(ts, ac, g);
    if (ts->ps) ts->ps->gRefValid = 0;             /* Novice: hold the climb angle we have now */
    world_set_takeoff(ac->id, 0);                   /* the player flies: PULL UP armed */
    set_phase(ts, TO_HANDOVER, ac, g);
    world_event_push(g, WEV_HANDOVER, ac->id, -1, fs->pos);
    log_printf(LOG_INFO, "takeoff: summary a_to_liftoff_s=%.2f a_to_handover_s=%.2f roll_m=%.0f max_lat_m=%.2f "
               "min_clear_m=%.2f V=%.0fkt agl=%.0fft override=%d mode=%s",
               (double)(ts->tickLift - ts->tickA) / SIM_HZ, (double)(ts->tickHand - ts->tickA) / SIM_HZ,
               (double)ts->liftDist, (double)ts->maxLat, (double)ts->minClear, (double)(fs->V * MS2KT),
               (double)(world_agl(g, fs->pos) * M2FT), overridden, pilot_mode_name(pilot_mode()));
}

int takeoff_tick(TakeoffState *ts, Aircraft *ac, const PilotInput *player, PilotInput *out, const Game *g)
{
    FlightState *fs = &ac->fs;
    PilotInput ap;
    float lat, agl, acc, f, s, alpha, nose, gam, w;
    int ax;
    if (!(ac->flags & AC_ALIVE)) return 0;
    gear_tick(ts, ac, g);
    if (ts->phase == TO_DONE) return 0;
    *out = *player;                                 /* buttons (view, ...) pass through; axes + throttle below */
    lat = takeoff_centre_line_error(g, fs->pos);
    if (ts->phase < TO_HANDOVER && (float)fabs(lat) > ts->maxLat) ts->maxLat = (float)fabs(lat);

    switch (ts->phase) {
    case TO_READY:
        out->up = out->ur = 0.0f;
        out->thr = THR_NORMAL;                      /* engine idle (snd: phase READY) */
        fs->V = 0.0f;
        if (player->thr == THR_FAST) {
            ts->tickA = g->tick;
            set_phase(ts, TO_ROLL, ac, g);
            world_event_push(g, WEV_TO_ROLL, ac->id, -1, fs->pos);
        }
        return 1;

    case TO_ROLL:
    case TO_ROTATE:
        out->up = out->ur = 0.0f;
        out->thr = player->thr == THR_FAST ? THR_FAST : THR_NORMAL;
        acc = player->thr == THR_FAST ? s_rollAccAB : s_rollAccMil;
        fs->V += acc * SIM_DT;
        fs->pos.x += (float)sin(ts->hdg) * fs->V * SIM_DT;
        fs->pos.z += (float)cos(ts->hdg) * fs->V * SIM_DT;
        fs->pos.y = ts->groundY;
        ts->liftDist += fs->V * SIM_DT;
        if (ts->phase == TO_ROLL) {
            if (fs->V >= s_vRotateKt * KT2MS) set_phase(ts, TO_ROTATE, ac, g);
            return 1;
        }
        f = (float)(g->tick - ts->phaseTick) * SIM_DT / s_rotateSec;
        s = smooth01(f);
        alpha = alpha_1g(fs->V) * s;
        nose = E_DEG2RAD(s_rotatePitchDeg) * s;
        gam = nose - alpha;
        if (gam < 0.0f) { gam = 0.0f; alpha = nose; }
        fs->alpha = alpha;
        fs->qPath = flight_path_quat(ts->hdg, gam, 0.0f);
        flight_derive(fs);
        if (f < 1.0f || fs->V < s_vLiftKt * KT2MS) return 1;
        /* liftoff: the flight model flies from this tick on */
        if (fs->V < flight_speed_floor()) fs->V = flight_speed_floor();
        fs->onGround = 0;
        ac->flags &= ~AC_ONGROUND;
        flight_set_path(fs, ts->hdg, gam, 0.0f);
        ts->gammaCmd = gam;
        ts->tickLift = g->tick;
        set_phase(ts, TO_CLIMB, ac, g);
        world_event_push(g, WEV_TO_LIFTOFF, ac->id, -1, fs->pos);
        /* FALLTHROUGH: this tick already flies */

    case TO_CLIMB:
        agl = world_agl(g, fs->pos);
        if (ts->tickLift != g->tick && agl < ts->minClear) ts->minClear = agl;
        climb_autopilot(ts, ac, g, &ap);
        out->up = ap.up;
        out->ur = ap.ur;
        out->thr = player->thr == THR_FAST ? THR_FAST : THR_NORMAL;
        if (agl >= s_gearUpFt * FT2M) gear_start(ts, ac, g);
        if (player_axis_active(ts, player, 0) || player_axis_active(ts, player, 1)) ts->holdTicks++;
        else ts->holdTicks = 0;
        if ((float)ts->holdTicks > s_overrideSec * (float)SIM_HZ && agl >= s_overrideMinFt * FT2M) {
            handover(ts, ac, g, 1);
            break;                                  /* this tick: blend weight 1 for idle axes */
        }
        if (agl >= s_handoverFt * FT2M && fs->V >= s_handoverKt * KT2MS) handover(ts, ac, g, 0);
        return 1;

    case TO_HANDOVER:
        climb_autopilot(ts, ac, g, &ap);
        break;
    }

    /* HANDOVER: blend autopilot -> player; an axis the player moves is his at once */
    w = s_handoverBlendSec > 0.0f ? 1.0f - (float)(g->tick - ts->phaseTick) * SIM_DT / s_handoverBlendSec : 0.0f;
    w = clampf(w, 0.0f, 1.0f);
    for (ax = 0; ax < 2; ax++) {
        float pv = ax ? player->ur : player->up, av = ax ? ts->apUr : ts->apUp, o;
        o = player_axis_active(ts, player, ax) ? pv : w * av + (1.0f - w) * pv;
        if (ax) out->ur = o; else out->up = o;
    }
    out->thr = player->thr;
    if ((float)(g->tick - ts->phaseTick) >= E_MAXF(s_handoverMsgSec, s_handoverBlendSec) * (float)SIM_HZ) {
        set_phase(ts, TO_DONE, ac, g);
    }
    return 1;
}
