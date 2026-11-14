/* dlz.cpp - launch zone from LIVE fly-outs of missile_step + AI tables (DESIGN 12.4). Portable. Owner: T13.
   Model and API notes: dlz.h. Implementation map:
     target model (tgt_*)   straight, or a level escape turn at gUse x flight_g_limit(V) with the flight model's speed
                            law towards FAST (accAB/tauV/decOver/inducedPerG/thrust lapse read from [flight]).
     fly-out (run_*)        missile_launch + missile_step(dt 1/30) resumable in chunks; early-out for sure misses.
     root search (root_*)   bracketing search per value, next guess from the coast law (hit: reach; miss: shortfall).
     solver (solver_*)      one cycle: NOW, NOWESC, RMAX, RNE (worst escape + checks), RMIN.
     live HUD (dlz_work)    the player's designated target: cycles run back to back, time-sliced by steps.
     tables (tab_*)         AI tables built with the same solver. */
#include "dlz.h"
#include "missile.h"
#include "weapons.h"
#include "radar.h"
#include "params.h"
#include "e_log.h"
#include "e_prof.h"
#include "e_file.h"
#include "e_terrain.h"

/* ------------------------------------------------------------------ params */
static int   s_stepsLow, s_stepsHigh, s_maxUsFrame, s_earlyOut, s_tablesOn, s_trace;
static int   s_saveOnBuild;
static float s_tabGUse, s_leadFrac, s_gUseParam, s_tolMax, s_tolNe, s_tolMin, s_pad, s_smUp, s_smDown, s_dt, s_coastFarM;

static const Param s_tab[] = {
    { "dlz", "stepsLow", P_INT, &s_stepsLow, "200", 20, 20000,
      "missile_step calls per frame for the live launch zone on the Low preset (VM: ~6 us each at sea, ~9 over the island)" },
    { "dlz", "stepsHigh", P_INT, &s_stepsHigh, "1500", 20, 50000, "missile_step calls per frame on the High preset (T40)" },
    { "dlz", "maxUsFrame", P_INT, &s_maxUsFrame, "1400", 0, 100000,
      "time cap per frame (us) on top of the step budget (0 = steps only); display only, results unaffected" },
    { "dlz", "escapeGUse", P_FLOAT, &s_gUseParam, "0.80", 0.2f, 1.0f,
      "escape turns of the Rne set at this fraction of the flight model's G limit (AI gUse; T17 may set it per difficulty)" },
    { "dlz", "tableGUse", P_FLOAT, &s_tabGUse, "1.0", 0.2f, 1.0f,
      "AI tables (the AI shooting at the player): the player's escapes at this fraction of the G limit (full stick = 1)" },
    { "dlz", "dt", P_FLOAT, &s_dt, "0.0333333", 0.004f, 0.05f, "fly-out step (s); 1/30 agrees with live 1/120 (T12)" },
    { "dlz", "coastFarM", P_FLOAT, &s_coastFarM, "6000", 0.0f, 100000.0f,
      "T21d: live/sync fly-outs step 2 x dt after the motor burn while the target is farther than this (m; 0 = off; tables keep dt)" },
    { "dlz", "tolMax", P_FLOAT, &s_tolMax, "0.025", 0.005f, 0.2f, "Rmax search ends when the hit/miss bracket is this narrow (fraction)" },
    { "dlz", "tolNe", P_FLOAT, &s_tolNe, "0.03", 0.005f, 0.2f, "Rne search bracket (fraction); Rne publishes the verified hit side" },
    { "dlz", "tolMin", P_FLOAT, &s_tolMin, "0.05", 0.005f, 0.3f, "Rmin search bracket (fraction, at least 25 m)" },
    { "dlz", "padFrac", P_FLOAT, &s_pad, "0.006", 0.0f, 0.1f, "next guess aims this fraction past the estimate (closes the bracket)" },
    { "dlz", "smoothUp", P_FLOAT, &s_smUp, "0.5", 0.05f, 1.0f, "published value moves this fraction towards a LARGER new value" },
    { "dlz", "smoothDown", P_FLOAT, &s_smDown, "1.0", 0.05f, 1.0f, "... towards a SMALLER new value (1 = at once: honesty first)" },
    { "dlz", "leadFrac", P_FLOAT, &s_leadFrac, "0.5", 0.0f, 1.5f,
      "live snapshot predicted ahead by this fraction of the last cycle time (both aircraft keep turning): less lag" },
    { "dlz", "earlyOut", P_BOOL, &s_earlyOut, "1", 0, 1, "cut a fly-out short once it can no longer reach the target (sure miss)" },
    { "dlz", "saveTables", P_BOOL, &s_saveOnBuild, "1", 0, 1,
      "after a rebuild (params changed) write dlz_cache.tab next to jet.ini so the next start loads it (the shipped data/dlz.tab comes from simh dlztable -out)" },
    { "dlz", "trace", P_BOOL, &s_trace, "0", 0, 1, "log every fly-out of the solver (debug)" },
    { "dlz", "tables", P_BOOL, &s_tablesOn, "1", 0, 1, "build the AI tables at session start (dlz_ai_zone)" },
};
void dlz_params(void) { params_add(s_tab, E_COUNT(s_tab)); }

static float s_gUseSet = -1.0f;
static int s_tabMode;                               /* 1 while building tables: looser tolerances */                     /* dlz_set_escape_guse; < 0 = the param */
float dlz_escape_guse(void) { return s_gUseSet > 0.0f ? s_gUseSet : s_gUseParam; }
void  dlz_set_escape_guse(float gUse) { s_gUseSet = gUse; }

static const char *const s_escNames[DLZ_ESC_COUNT] = { "straight", "break_left", "break_right", "beam_left", "beam_right" };
const char *dlz_esc_name(int esc) { return esc >= 0 && esc < DLZ_ESC_COUNT ? s_escNames[esc] : "?"; }

static DlzStats s_stats;
const DlzStats *dlz_stats(void) { return &s_stats; }

/* ------------------------------------------------------------------ flight-model constants for the escape law */
static const float *s_pAccAB, *s_pTauV, *s_pDecOver, *s_pInduced;
static float fl_param(const float **cache, const char *key, float def)
{
    const Param *p;
    if (!*cache) {
        p = params_find("flight", key);
        if (p && p->type == P_FLOAT) *cache = (const float *)p->ptr;
    }
    return *cache ? **cache : def;
}

/* ------------------------------------------------------------------ target model */
typedef struct Tgt {
    Vec3 pos, vel;
    int esc;
    float hx, hz, V, Vgoal, turnLeft, turnSign, gUse, delay, n, w;
    int cacheN;
    float accAB, tauV, decOver, induced, vFloor, lapse;
} Tgt;

static float heading_of(float x, float z) { return (float)atan2(x, z); }
static float wrap2pi(float a)
{
    while (a < 0.0f) a += 2.0f * E_PI;
    while (a >= 2.0f * E_PI) a -= 2.0f * E_PI;
    return a;
}

static void tgt_init(Tgt *t, const DlzGeom *g, Vec3 pos, int esc)
{
    float h, len, ax, az, hd, ha, s, a1, a2;
    memset(t, 0, sizeof(*t));
    t->pos = pos;
    t->vel = g->tVel;
    t->esc = esc;
    if (esc == DLZ_STRAIGHT) return;
    t->gUse = g->gUse > 0.0f ? g->gUse : dlz_escape_guse();
    t->V = v3_len(g->tVel);
    t->vFloor = flight_speed_floor();
    if (t->V < t->vFloor) t->V = t->vFloor;
    len = (float)sqrt(g->tVel.x * g->tVel.x + g->tVel.z * g->tVel.z);
    ax = pos.x - g->sPos.x;
    az = pos.z - g->sPos.z;
    h = (float)sqrt(ax * ax + az * az);
    if (h < 1.0f) { ax = 0.0f; az = 1.0f; } else { ax /= h; az /= h; }    /* away from the shooter (horizontal) */
    if (len > 1.0f) { t->hx = g->tVel.x / len; t->hz = g->tVel.z / len; }
    else { t->hx = az; t->hz = -ax; }                                     /* climbing/diving vertically: beam */
    t->Vgoal = flight_speed_target(THR_FAST, pos.y);
    t->accAB = fl_param(&s_pAccAB, "accAB", 18.0f);
    t->tauV = fl_param(&s_pTauV, "tauV", 2.5f);
    t->decOver = fl_param(&s_pDecOver, "decOver", 15.0f);
    t->induced = fl_param(&s_pInduced, "inducedPerG", 0.9f);
    t->lapse = flight_thrust_lapse(pos.y);
    s = (esc == DLZ_BREAK_R || esc == DLZ_BEAM_R) ? 1.0f : -1.0f;         /* + = right (heading increases) */
    t->turnSign = s;
    hd = heading_of(t->hx, t->hz);
    ha = heading_of(ax, az);
    if (esc == DLZ_BREAK_L || esc == DLZ_BREAK_R) {
        t->turnLeft = wrap2pi(s * (ha - hd));
    } else {
        a1 = wrap2pi(s * (ha + 0.5f * E_PI - hd));
        a2 = wrap2pi(s * (ha - 0.5f * E_PI - hd));
        t->turnLeft = E_MINF(a1, a2);
    }
    t->vel = v3(t->hx * t->V, 0.0f, t->hz * t->V);                        /* escapes are flown level */
    /* roll-in: the flight model cannot turn at once; it rolls to the turn bank at its roll rate while the load builds
       (tauG). Measured against flight_step flown by a pilot (tests_dlz.cpp): ~0.85 x bank/rollRate + tauG of dead time.
       Not cosmetic: under PN a later turn costs the missile more energy, so an instant turn would NOT be the
       conservative case (T13: SRM beam escapes beat the instant-turn model by ~10 % in range). */
    a1 = t->gUse * flight_g_limit(t->V);
    if (a1 < 1.01f) a1 = 1.01f;
    t->delay = 0.85f * (float)acos(1.0f / a1) / E_MAXF(flight_roll_rate_max(t->V), 0.5f) + flight_tau_g();
}

static void tgt_step(Tgt *t, float dt)
{
    float n = 1.0f, d, c, sn, x, a;
    if (t->esc == DLZ_STRAIGHT) { t->pos = v3_madd(t->pos, t->vel, dt); return; }
    if (t->delay > 0.0f) t->delay -= dt;
    else if (t->turnLeft > 0.0f) {
        if (t->cacheN-- <= 0) {                     /* G limit and turn rate change slowly with V: every 8 steps */
            t->n = t->gUse * flight_g_limit(t->V);
            if (t->n < 1.01f) t->n = 1.01f;
            t->w = GRAV * (float)sqrt(t->n * t->n - 1.0f);
            t->cacheN = 7;
        }
        n = t->n;
        d = t->w / t->V * dt;
        if (d > t->turnLeft) d = t->turnLeft;
        t->turnLeft -= d;
        d *= t->turnSign;
        c = 1.0f - 0.5f * d * d;                    /* |d| <= ~0.02 rad per step: series exact to float */
        sn = d - d * d * d * (1.0f / 6.0f);
        x = t->hx * c + t->hz * sn;
        t->hz = -t->hx * sn + t->hz * c;
        t->hx = x;
    }
    a = (t->Vgoal - t->V) / t->tauV;                                     /* flight.cpp thr_accel, THR_FAST */
    if (t->V < t->Vgoal) { if (a > t->accAB) a = t->accAB; a *= t->lapse; }
    else if (a < -t->decOver) a = -t->decOver;
    a -= t->induced * (n - 1.0f);
    t->V += a * dt;
    if (t->V < t->vFloor) t->V = t->vFloor;
    t->vel = v3(t->hx * t->V, 0.0f, t->hz * t->V);
    t->pos = v3_madd(t->pos, t->vel, dt);
}

/* ------------------------------------------------------------------ fly-out */
/* coast law (DESIGN 12.2 speed equation without thrust, guidance or gravity): range the missile can still close on
   a target receding at vAlong (m/s along the line of sight) before it is too slow or too old */
static float coast_reach(const MissileParams *mp, float V0, float age, float rho, float vAlong)
{
    float a = mp->dragK * rho, vEnd, t1, t2, t, s;
    if (a < 1.0e-7f) a = 1.0e-7f;
    vEnd = E_MAXF(mp->minSpeed, vAlong + 1.0f);
    if (V0 <= vEnd) return 0.0f;
    t1 = (1.0f / vEnd - 1.0f / V0) / a;
    t2 = mp->maxSec - age;
    if (t2 < 0.0f) t2 = 0.0f;
    t = E_MINF(t1, t2);
    s = (float)log(1.0f + a * V0 * t) / a;
    return s - vAlong * t;
}

typedef struct Run {
    Missile m;
    TrackView tv;
    Tgt tg;
    const MissileParams *mp;
    float R, t, tMin, lastMin, dt, shortfall, maxT, lockRange;
    int esc, steps, active, early, nolock, coarse;
} Run;

static void run_begin(Run *r, const DlzGeom *g, float R, int esc)
{
    Vec3 d = v3_sub(g->tPos, g->sPos), los = v3_norm(d);
    memset(r, 0, sizeof(*r));
    r->mp = missile_params_of(g->type);
    r->R = R;
    r->esc = esc;
    r->dt = s_dt > 0.0f ? s_dt : 1.0f / 30.0f;
    r->maxT = r->mp->maxSec + 1.0f;
    r->coarse = !s_tabMode && s_coastFarM > 0.0f && esc == DLZ_STRAIGHT;   /* T21d: coarse coast (straight targets only; not the AI tables) */
    tgt_init(&r->tg, g, v3_madd(g->sPos, los, R), esc);
    r->tv.valid = 1;
    r->tv.pos = r->tg.pos;
    r->tv.vel = r->tg.vel;
    r->tv.datalink = 1;                             /* lock assumed held (DESIGN 12.4) */
    r->tv.ab = esc != DLZ_STRAIGHT ? 1.0f : g->tAb;
    r->tv.id = 1;
    missile_launch(&r->m, g->type, 0, 1, g->sPos, g->sNose, g->sV, &r->tv, MF_GUIDED, 0.0f);
    r->lastMin = r->m.minDist;
    r->shortfall = -1.0f;
    r->active = 1;
    if (g->type == MSL_SRM) {                       /* guided SRM launch needs the seeker lock (weapons.cpp rule) */
        TrackView at = r->tv;
        float range, cg = (float)cos(E_DEG2RAD(r->mp->gimbalDeg));
        at.ab = g->tAb;                             /* at launch the target has not lit the afterburner yet */
        range = missile_heat_range(r->mp, &at, g->sPos);
        if (R > range || v3_dot(los, g->sNose) < cg || (g->ter && !terrain_los(g->ter, &g->sPos, &r->tg.pos))) {
            r->active = 0;
            r->early = 1;
            r->nolock = 1;
            r->shortfall = R > range ? R - range + 1.0f : -1.0f;
            r->lockRange = R > range ? range : 0.0f;
        }
    }
}

/* steps up to n; returns steps done; r->active = 0 when finished */
static int run_step(Run *r, const DlzGeom *g, int n)
{
    int k, s0 = r->steps;
    Missile *m = &r->m;
    const MissileParams *mp = r->mp;
    float boostEnd = missile_burn_end(mp), dist, vAl, dh, V0, reach;
    Vec3 d;
    for (k = 0; k < n && r->active; k++) {
        float h = r->dt;
        if (r->coarse && m->age > boostEnd) {      /* T21d: long coast far from the target: 2 x dt (one step's cost) */
            d = v3_sub(r->tg.pos, m->pos);
            if (v3_dot(d, d) > s_coastFarM * s_coastFarM) h = 2.0f * r->dt;
        }
        tgt_step(&r->tg, h);
        r->tv.pos = r->tg.pos;
        r->tv.vel = r->tg.vel;
        missile_step(m, mp, &r->tv, g->ter, h);
        r->t += h;
        r->steps++;
        if (m->minDist < r->lastMin) { r->lastMin = m->minDist; r->tMin = r->t; }
        if (m->phase == MPH_DEAD || (m->flags & MF_MISSED) || r->t > r->maxT) { r->active = 0; break; }
        if (s_earlyOut && (r->steps & 7) == 0 && m->age > boostEnd && m->age > mp->armSec) {
            d = v3_sub(r->tg.pos, m->pos);
            dist = v3_len(d);
            if (dist > 1.0f) {
                vAl = v3_dot(r->tg.vel, d) / dist;
                dh = m->pos.y - r->tg.pos.y;
                V0 = dh > 0.0f ? (float)sqrt(m->V * m->V + 2.0f * GRAV * dh) : m->V;   /* height is speed too */
                reach = coast_reach(mp, V0, m->age, mp->densityAlt ? m->rho : 1.0f, vAl);
                /* gave up (can no longer fuze, missile.h) or cannot close the gap even coasting: a sure miss */
                if ((m->flags & MF_UNGUIDED) || dist > 1.2f * reach + 400.0f) {
                    r->early = 1;
                    r->shortfall = dist - 0.65f * reach;    /* reach is an upper bound; ~0.6-0.8 is flown (T13 traces) */
                    if (r->shortfall < r->lastMin) r->shortfall = r->lastMin;
                    r->active = 0;
                    break;
                }
            }
        }
    }
    return r->steps - s0;
}

static void run_result(const Run *r, DlzFly *f)
{
    const Missile *m = &r->m;
    const MissileParams *mp = r->mp;
    float vAl, V0, tb;
    memset(f, 0, sizeof(*f));
    f->hit = m->result == MRES_HIT;
    f->res = m->result;
    f->early = r->early;
    f->steps = r->steps;
    f->tof = r->t;
    f->minDist = r->lastMin;
    f->tMin = r->tMin;
    f->shortfall = -1.0f;
    f->hitPos = r->tg.pos;
    f->lockRange = r->lockRange;
    if (f->hit) {
        vAl = v3_dot(r->tg.vel, m->dir);
        tb = mp->dropSec + mp->boostSec - m->age;
        V0 = m->V + (tb > 0.0f ? 0.7f * mp->boostAcc * tb : 0.0f);
        tb = mp->dropSec + mp->boostSec + mp->sustainSec - m->age;
        V0 += (tb > 0.0f && mp->sustainSec > 0.0f ? 0.7f * mp->sustainAcc * tb : 0.0f);
        f->reach = coast_reach(mp, V0, m->age, mp->densityAlt ? m->rho : 1.0f, vAl);
    } else if (m->result == MRES_GROUND || m->result == MRES_FLARE) {
        f->shortfall = -1.0f;
    } else if (r->early && r->shortfall >= 0.0f) {
        f->shortfall = r->shortfall;
    } else if (r->early) {
        f->shortfall = -1.0f;                       /* unguided: no range metric */
    } else {
        f->shortfall = r->lastMin;                  /* expired / opening: closest approach ~ distance short */
    }
}

int dlz_flyout(const DlzGeom *g, float R, int esc, DlzFly *out)
{
    Run r;
    run_begin(&r, g, R, esc);
    while (r.active) {
        run_step(&r, g, s_trace ? 30 : 100000);
        if (s_trace)
            log_printf(LOG_INFO, "dlz: trk t=%.1f tgt V=%.0f hdg=%.1f turnLeft=%.1f | msl V=%.0f d=%.0f", (double)r.t, (double)r.tg.V,
                       (double)E_RAD2DEG((float)atan2(r.tg.vel.x, r.tg.vel.z)), (double)E_RAD2DEG(r.tg.turnLeft), (double)r.m.V,
                       (double)v3_len(v3_sub(r.m.pos, r.tg.pos)));
    }
    run_result(&r, out);
    s_stats.steps += (u32)r.steps;
    s_stats.flyouts++;
    return out->hit;
}

/* ------------------------------------------------------------------ root search */
enum { RK_OUTER, RK_INNER };                        /* outer: hits below the value (Rmax/Rne); inner: above (Rmin) */
typedef struct Root {
    int kind, esc, n, maxN, done, mid;
    float lo, hi, x, tol, floorR, ceilR, absTol;
    int haveLo, haveHi;
    float tofLo, tofHi;                             /* time of flight of the hit at lo (outer) / hi (inner) */
    float pmX, pmD;                                 /* inner: previous turn-limited miss (range, miss distance) */
    Vec3 hitPosLo;
} Root;

static void root_begin(Root *r, int kind, int esc, float x0, float tol, int maxN, int mid)
{
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    r->esc = esc;
    r->x = x0;
    r->tol = tol;
    r->maxN = maxN;
    r->mid = mid;
    r->floorR = 60.0f;
    r->ceilR = 90000.0f;
    r->absTol = kind == RK_INNER ? 25.0f : 40.0f;
}

static int root_narrow(const Root *r)
{
    return r->haveLo && r->haveHi && r->hi - r->lo <= E_MAXF(r->tol * r->hi, r->absTol);
}

/* put x inside the bracket (at least 0.4 % of it away from the known ends), or done */
static void root_fix_x(Root *r, float next)
{
    float w, m;
    if (root_narrow(r)) { r->done = 1; return; }
    if (next != next) next = r->x;
    if (r->haveLo && r->haveHi) {
        w = r->hi - r->lo;
        m = E_MINF(0.004f * r->hi, 0.25f * w);
        if (next <= r->lo || next >= r->hi) next = r->lo + 0.5f * w;           /* estimate left the bracket: bisect */
        next = E_CLAMP(next, r->lo + m, r->hi - m);
    } else if (r->haveLo && next <= r->lo) next = r->lo * 1.02f;
    else if (r->haveHi && next >= r->hi) next = r->hi * 0.98f;
    next = E_CLAMP(next, r->floorR, r->ceilR);
    r->x = next;
}

static void root_start(Root *r)
{
    float x = r->x;
    if (r->kind == RK_OUTER) {
        if (r->haveHi && x >= r->hi) x = 0.92f * r->hi;
        if (r->haveLo && x <= r->lo) x = 1.06f * r->lo;
    } else {
        if (r->haveHi && x >= r->hi) x = 0.7f * r->hi;
        if (r->haveLo && x <= r->lo) x = 1.1f * r->lo;
    }
    root_fix_x(r, x);
}

/* next guess from one fly-out: outer (Rmax/Rne) hit -> + coast reach, miss -> - shortfall; inner (Rmin) hit -> closer
   by the spare time after arming, miss -> further by the missing arm time, or (turn-limited) a secant on the miss
   distance; always aiming s_pad past the estimate so the next fly-out lands on the other side */
static void root_feed(Root *r, const DlzFly *f, float armSec, float fuzeM)
{
    float x = r->x, next, Vc;
    r->n++;
    if (r->kind == RK_OUTER) {
        if (f->hit) {
            r->lo = x; r->haveLo = 1; r->tofLo = f->tof; r->hitPosLo = f->hitPos;
            next = x + f->reach + s_pad * x;
            if (!r->haveHi && next < x * 1.02f) next = x * 1.02f;
        } else {
            r->hi = x; r->haveHi = 1;
            if (f->lockRange > 0.0f && f->lockRange < x) {   /* SRM: no lock beyond this range: an exact upper bound */
                r->hi = f->lockRange + 1.0f;
                if (r->haveLo && r->hi <= r->lo) r->hi = r->lo + 1.0f;
            }
            if (f->shortfall >= 0.0f) next = x - f->shortfall - s_pad * x;
            else next = r->haveLo ? 0.5f * (r->lo + r->hi) : x * 0.6f;
            if (r->pmX > x && r->pmD > f->minDist + 1.0f && f->minDist < 1000.0f) {   /* end-game misses: secant */
                Vc = x - (r->pmX - x) * (f->minDist - 0.5f * fuzeM) / (r->pmD - f->minDist);
                if (Vc < next) next = Vc;
            }
            r->pmX = x; r->pmD = f->minDist;
            if (!r->haveLo && next > x * 0.985f) next = x * 0.985f;
            if (!r->haveLo && x <= r->floorR * 1.01f) { r->done = 1; return; }   /* no hit anywhere: no zone */
        }
    } else {
        if (f->hit) {
            r->hi = x; r->haveHi = 1; r->tofHi = f->tof;
            Vc = x / E_MAXF(f->tof, 0.05f);
            next = x - Vc * (f->tof - armSec) - s_pad * x;
            if (!r->haveLo && next > x * 0.97f) next = x * 0.97f;
            if (next < x * 0.25f) next = x * 0.25f;
            if (x <= r->floorR * 1.01f) { r->done = 1; return; }
        } else {
            r->lo = x; r->haveLo = 1;
            if (f->tMin < armSec + 0.05f) {         /* passed before arming: closer in time by the missing arm time */
                Vc = x / E_MAXF(f->tMin, 0.05f);
                next = x + Vc * (armSec - f->tMin) + s_pad * x;
            } else if (r->pmX > 0.0f && r->pmX < x && r->pmD > f->minDist + 1.0f) {   /* turn-limited: secant on miss */
                next = x + (x - r->pmX) * (f->minDist - 0.5f * fuzeM) / (r->pmD - f->minDist);
            } else next = x * (f->minDist < 4.0f * fuzeM ? 1.04f : 1.25f);
            if (f->tMin >= armSec + 0.05f) { r->pmX = x; r->pmD = f->minDist; }
            if (!r->haveHi && next < x * 1.03f) next = x * 1.03f;
        }
    }
    if (r->n >= r->maxN) { r->done = 1; return; }
    root_fix_x(r, next);
}

static float root_value(const Root *r)
{
    if (r->kind == RK_OUTER) {
        if (!r->haveLo) return 0.0f;
        return (r->mid && r->haveHi && root_narrow(r)) ? 0.5f * (r->lo + r->hi) : r->lo;   /* wide: the verified hit */
    }
    return r->haveHi ? r->hi : 0.0f;
}

/* ------------------------------------------------------------------ solver (one cycle) */
enum { ST_NOW, ST_NOWESC, ST_RMAX, ST_RNE, ST_CHECK, ST_RMIN, ST_DONE };
enum { PUB_NOW = 1, PUB_NOWESC = 2, PUB_MAX = 4, PUB_NE = 8, PUB_MIN = 16 };

typedef struct Solver {
    DlzGeom g;
    float R0;
    int stage, doNow, rootActive, running;
    Root root;
    Run run;
    DlzZone warm, out;
    int checked[DLZ_ESC_COUNT];
    float checkShort;                               /* shortfall of the failed check fly-out (-1 unknown) */
    int skipEsc;                                    /* live: this escape's check rests this cycle (-1 none) */
    int lite;                                       /* live: Rmin kept from the last cycle */
    int nowValid, nowHit, nowEscValid, nowNoEsc, nowEscSkipped;
    DlzFly nowFly, nowEscFly;
    int pub;                                        /* PUB_* completed since the caller last cleared it */
    int flyouts, steps;
} Solver;

static void default_zone(int type, DlzZone *z)
{
    memset(z, 0, sizeof(*z));
    z->rmax = type == MSL_SRM ? 5000.0f : type == MSLK_LRM_PLAYER ? 30000.0f : 15000.0f;
    z->rne = type == MSL_SRM ? 3500.0f : type == MSLK_LRM_PLAYER ? 13000.0f : 6500.0f;
    z->rmin = type == MSL_SRM ? 400.0f : 800.0f;
    z->worstEsc = DLZ_BREAK_L;
}

static void solver_begin(Solver *S, const DlzGeom *g, const DlzZone *warm, int doNow)
{
    memset(S, 0, sizeof(*S));
    S->g = *g;
    S->R0 = v3_len(v3_sub(g->tPos, g->sPos));
    S->doNow = doNow;
    if (warm && warm->rmax > 0.0f) S->warm = *warm; else default_zone(g->type, &S->warm);
    if (S->warm.worstEsc < DLZ_BREAK_L || S->warm.worstEsc > DLZ_BEAM_R) S->warm.worstEsc = DLZ_BREAK_L;
    if (S->warm.rne <= 0.0f) S->warm.rne = 0.5f * S->warm.rmax;
    if (S->warm.rmin <= 0.0f) S->warm.rmin = 0.05f * S->warm.rmax;
    S->out.worstEsc = S->warm.worstEsc;
    S->stage = doNow ? ST_NOW : ST_RMAX;
    S->skipEsc = -1;
}

static int sure_upper_miss(const DlzFly *f, float armSec)
{
    /* a miss that says "too far" (energy/opening, after arming), not "too close" or terrain */
    return !f->hit && f->shortfall >= 0.0f && f->tMin > armSec + 0.5f;
}

/* set up the next fly-out (starts S->run) or advance the stage; returns 0 when the cycle is done */
static int solver_next(Solver *S)
{
    const MissileParams *mp = missile_params_of(S->g.type);
    float x0, rmaxLo;
    int e;
    for (;;) {
        switch (S->stage) {
        case ST_NOW:
            run_begin(&S->run, &S->g, S->R0, DLZ_STRAIGHT);
            return 1;
        case ST_NOWESC:
            /* only where it can say something new: a straight-target miss can't be a no-escape hit, and well beyond
               the last Rne (x1.3) the escape answer is "escapes" with near certainty (saves ~200-500 steps) */
            if (!S->nowHit || (S->warm.rne > 0.0f && S->R0 > 1.3f * S->warm.rne)) {
                S->nowEscValid = 1; S->nowNoEsc = 0; S->nowEscSkipped = 1;
                S->pub |= PUB_NOWESC;
                S->stage = ST_RMAX;
                break;
            }
            run_begin(&S->run, &S->g, S->R0, S->warm.worstEsc);
            return 1;
        case ST_RMAX:
            if (!S->rootActive) {
                /* T21a: tables of the player's LRM get 12 Rmax fly-outs (its ~2x longer, end-game-limited misses need
                   more steps to bracket); enemy LRM / SRM tables keep T13's 6 (bit-identical nodes) */
                root_begin(&S->root, RK_OUTER, DLZ_STRAIGHT, S->warm.rmax, s_tabMode ? 0.04f : s_tolMax,
                           s_tabMode ? (S->g.type == MSLK_LRM_PLAYER ? 12 : 6) : 8, 1);
                if (S->nowValid && S->nowHit) { S->root.lo = S->R0; S->root.haveLo = 1; S->root.tofLo = S->nowFly.tof;
                                                 S->root.hitPosLo = S->nowFly.hitPos; }
                else if (S->nowValid && sure_upper_miss(&S->nowFly, mp->armSec) && S->R0 > S->warm.rmin * 2.0f)
                    { S->root.hi = S->R0; S->root.haveHi = 1; }
                root_start(&S->root);
                S->rootActive = 1;
            }
            if (!S->root.done) { run_begin(&S->run, &S->g, S->root.x, DLZ_STRAIGHT); return 1; }
            S->rootActive = 0;
            S->out.rmax = root_value(&S->root);
            S->out.rmaxLo = S->root.haveLo ? S->root.lo : 0.0f;
            S->out.rmaxHi = S->root.haveHi ? S->root.hi : 0.0f;
            S->out.tof = S->root.tofLo;
            S->pub |= PUB_MAX;
            if (S->out.rmax <= 0.0f) {
                S->out.rne = S->out.rmin = 0.0f;
                S->pub |= PUB_NE | PUB_MIN;
                S->stage = ST_DONE;
                break;
            }
            S->stage = ST_RNE;
            break;
        case ST_RNE:
            rmaxLo = S->out.rmaxLo;
            if (!S->rootActive) {
                e = S->out.worstEsc;
                x0 = E_MINF(S->warm.rne, rmaxLo);
                root_begin(&S->root, RK_OUTER, e, x0, s_tabMode ? 0.05f : s_tolNe, 6, 0);
                S->root.ceilR = rmaxLo * 1.5f;
                if (S->nowEscValid && e == S->warm.worstEsc) {
                    if (S->nowNoEsc && S->R0 <= rmaxLo) { S->root.lo = S->R0; S->root.haveLo = 1; }
                    else if (!S->nowNoEsc && sure_upper_miss(&S->nowEscFly, mp->armSec) && S->R0 > S->warm.rmin * 2.0f)
                        { S->root.hi = S->R0; S->root.haveHi = 1; }
                }
                if (S->checked[e] == 2) {               /* failed the check at the current Rne */
                    S->root.hi = S->out.rne; S->root.haveHi = 1;
                    S->root.x = S->checkShort >= 0.0f ? S->out.rne - S->checkShort - s_pad * S->out.rne : 0.85f * S->out.rne;
                }
                root_start(&S->root);
                S->rootActive = 1;
            }
            if (!S->root.done) { run_begin(&S->run, &S->g, S->root.x, S->root.esc); return 1; }
            S->rootActive = 0;
            x0 = E_MINF(root_value(&S->root), rmaxLo);
            S->checked[S->root.esc] = 1;
            if (S->out.rne <= 0.0f || x0 < S->out.rne) S->out.rne = x0;
            S->out.worstEsc = S->root.esc;
            S->stage = S->out.rne > 0.0f ? ST_CHECK : ST_RMIN;
            if (S->out.rne <= 0.0f) S->pub |= PUB_NE;
            break;
        case ST_CHECK:
            if (S->skipEsc >= DLZ_BREAK_L && S->skipEsc != S->out.worstEsc && !S->checked[S->skipEsc])
                S->checked[S->skipEsc] = 3;         /* live: one non-worst check rests this cycle (rotating) */
            for (e = DLZ_BREAK_L; e <= DLZ_BEAM_R; e++) if (!S->checked[e]) break;
            if (e > DLZ_BEAM_R) { S->pub |= PUB_NE; S->stage = ST_RMIN; break; }
            run_begin(&S->run, &S->g, S->out.rne, e);
            return 1;
        case ST_RMIN:
            if (S->lite && S->warm.rmin > 0.0f && S->warm.rmin < S->out.rmaxLo) {   /* live: Rmin every 2nd cycle */
                S->out.rmin = S->warm.rmin;
                S->stage = ST_DONE;
                break;
            }
            if (!S->rootActive) {
                rmaxLo = S->out.rmaxLo;
                x0 = S->warm.rmin;
                root_begin(&S->root, RK_INNER, DLZ_STRAIGHT, x0, s_tabMode ? 0.08f : s_tolMin, 5, 0);
                S->root.hi = rmaxLo; S->root.haveHi = 1; S->root.tofHi = S->out.tof;
                if (S->nowValid && S->nowHit && S->R0 < S->root.hi) { S->root.hi = S->R0; S->root.tofHi = S->nowFly.tof; }
                S->root.x = x0 * 0.97f;                 /* expect a miss just below the previous Rmin */
                if (S->root.x >= 0.8f * S->root.hi) S->root.x = 0.5f * S->root.hi;
                root_start(&S->root);
                S->rootActive = 1;
            }
            if (!S->root.done) { run_begin(&S->run, &S->g, S->root.x, DLZ_STRAIGHT); return 1; }
            S->rootActive = 0;
            S->out.rmin = root_value(&S->root);
            if (S->out.rne > 0.0f && S->out.rne < S->out.rmin) S->out.rne = S->out.rmin;
            S->pub |= PUB_MIN;
            S->stage = ST_DONE;
            break;
        default:
            return 0;
        }
    }
}

/* a fly-out finished: feed it */
static void solver_feed(Solver *S, const DlzFly *f)
{
    const MissileParams *mp = missile_params_of(S->g.type);
    int e;
    S->flyouts++;
    s_stats.flyouts++;
    if (s_trace)
        log_printf(LOG_INFO, "dlz: fly stage=%d esc=%s R=%.0f hit=%d res=%s early=%d steps=%d tof=%.2f tMin=%.2f min=%.0f reach=%.0f short=%.0f",
                   S->stage, dlz_esc_name(S->run.esc), (double)S->run.R, f->hit, missile_result_name(f->res), f->early, f->steps,
                   (double)f->tof, (double)f->tMin, (double)f->minDist, (double)f->reach, (double)f->shortfall);
    switch (S->stage) {
    case ST_NOW:
        S->nowValid = 1; S->nowHit = f->hit; S->nowFly = *f;
        S->pub |= PUB_NOW;
        S->stage = ST_NOWESC;
        break;
    case ST_NOWESC:
        S->nowEscValid = 1; S->nowNoEsc = f->hit; S->nowEscFly = *f;
        S->pub |= PUB_NOWESC;
        S->stage = ST_RMAX;
        break;
    case ST_RMAX: case ST_RNE: case ST_RMIN:
        root_feed(&S->root, f, mp->armSec, mp->fuzeM);
        break;
    case ST_CHECK:
        e = S->run.esc;
        if (f->hit) S->checked[e] = 1;
        else { S->checked[e] = 2; S->checkShort = f->shortfall; S->out.worstEsc = e; S->stage = ST_RNE; }   /* worse: solve it */
        break;
    default: break;
    }
}

/* run up to budget steps (and the time cap when capUs > 0); returns steps used. S->stage == ST_DONE when done */
static int solver_run(Solver *S, int budget, s64 capUs)
{
    int used = 0, n, chunk;
    DlzFly f;
    s64 t0 = capUs > 0 ? prof_now_us() : 0;
    while (used < budget && S->stage != ST_DONE) {
        if (!S->running) {
            if (!solver_next(S)) break;
            S->running = 1;
        }
        chunk = budget - used;
        if (capUs > 0) {                            /* time cap: never start a chunk that would end past it */
            s64 el = prof_now_us() - t0;
            float per = used > 0 ? (float)el / (float)used : 8.0f;      /* us per step so far (VM ~6-9) */
            int room = (int)((float)(capUs - el) / E_MAXF(per, 0.5f));
            if (room < 4) break;
            if (chunk > room) chunk = room;
            if (chunk > 16) chunk = 16;
        }
        n = run_step(&S->run, &S->g, chunk);
        used += n;
        S->steps += n;
        s_stats.steps += (u32)n;
        if (!S->run.active) {
            S->running = 0;
            run_result(&S->run, &f);
            solver_feed(S, &f);
        }
        if (capUs > 0 && t0 && prof_now_us() - t0 >= capUs) break;
    }
    if (S->stage == ST_DONE) { S->out.flyouts = S->flyouts; S->out.steps = S->steps; }
    return used;
}

int dlz_solve(const DlzGeom *g, const DlzZone *warm, DlzZone *out)
{
    Solver *S = (Solver *)malloc(sizeof(Solver));    /* setup-time only (tables/tests), not per tick */
    int ok;
    if (!S) { memset(out, 0, sizeof(*out)); return 0; }
    solver_begin(S, g, warm, 0);
    while (S->stage != ST_DONE) solver_run(S, 1 << 30, 0);
    *out = S->out;
    ok = out->rmax > 0.0f;
    free(S);
    return ok;
}

void dlz_geom_from(const Game *g, int shooter, int target, int type, DlzGeom *out)
{
    const FlightState *s = &g->ac[shooter].fs, *t = &g->ac[target].fs;
    memset(out, 0, sizeof(*out));
    out->type = missile_kind(type, (g->ac[shooter].flags & AC_PLAYER) != 0);   /* T21a: the shooter's own missile */
    out->sPos = s->pos;
    out->sNose = q_rotate(flight_nose(s), v3(0.0f, 0.0f, 1.0f));
    out->sV = s->V;
    out->tPos = t->pos;
    out->tVel = q_rotate(t->qPath, v3(0.0f, 0.0f, t->V));
    out->tAb = t->ab;
    out->ter = g->terrain;
}

/* ------------------------------------------------------------------ AI tables */
static const float s_tAsp[DLZT_NA] = { 0.0f, 45.0f, 90.0f, 135.0f, 180.0f };
static const float s_tSpd[DLZT_NS] = { 300.0f, 500.0f, 800.0f };              /* shooter kt */
static const float s_tEl[DLZT_ND] = { -7.0f, 0.0f, 7.0f };                    /* LOS elevation (deg) */
static const int   s_tThr[DLZT_NV] = { THR_NORMAL, THR_FAST };                /* target speed state */
static const float s_tAlt[DLZT_NH] = { 1500.0f, 4500.0f, 8000.0f };           /* shooter altitude (m) */
#define TAB_DIST 10000.0f                                                     /* node target distance (any: the solver re-places it) */
#define TAB_ALT 4500.0f                                                       /* fingerprint reference only */
enum { TAB_NODES = MSLK_COUNT * DLZT_NA * DLZT_NS * DLZT_ND * DLZT_NV * DLZT_NH };
static DlzZone s_tz[TAB_NODES];
static u32 s_tabKey, s_tabFp;
static int s_tabOk, s_tabStaleLogged, s_tabFromFile, s_tabTried;
static float s_tabMs = -1.0f;

static int tidx(int type, int ia, int is, int id, int iv, int ih)
{
    return ((((type * DLZT_NH + ih) * DLZT_NV + iv) * DLZT_ND + id) * DLZT_NS + is) * DLZT_NA + ia;
}

static u32 fnv(u32 h, const void *p, int n)
{
    const unsigned char *b = (const unsigned char *)p;
    int i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* cheap fingerprint of everything the tables depend on (bit level, this machine) */
static u32 tab_fp(void)
{
    float v[16];
    u32 h = 2166136261u;
    int i;
    h = fnv(h, missile_params_of(MSL_LRM), (int)sizeof(MissileParams));
    h = fnv(h, missile_params_of(MSL_SRM), (int)sizeof(MissileParams));
    h = fnv(h, missile_params_of(MSLK_LRM_PLAYER), (int)sizeof(MissileParams));
    for (i = 0; i < 8; i++) v[i] = flight_g_limit(80.0f + 70.0f * (float)i);
    v[8] = flight_speed_target(THR_FAST, TAB_ALT);
    v[9] = flight_speed_target(THR_NORMAL, TAB_ALT);
    v[10] = fl_param(&s_pAccAB, "accAB", 18.0f);
    v[11] = fl_param(&s_pTauV, "tauV", 2.5f);
    v[12] = fl_param(&s_pDecOver, "decOver", 15.0f);
    v[13] = fl_param(&s_pInduced, "inducedPerG", 0.9f);
    v[14] = s_tabGUse;
    v[15] = s_dt + s_pad;
    h = fnv(h, v, (int)sizeof(v));
    h = fnv(h, &s_earlyOut, (int)sizeof(int));
    return h;
}

/* portable key (same on VM and Mac): the params TEXT the tables depend on ([lrm] [srm] [flight] + the dlz keys of the
   solver) and the escape gUse; stored in data/dlz.tab */
static u32 tab_key(void)
{
    static char snap[65536];
    char gu[48];
    int n, i, ls;
    u32 h = 2166136261u;
    n = params_snapshot(snap, (int)sizeof(snap));
    for (i = 0, ls = 0; i < n; i++) {
        if (snap[i] != '\n') continue;
        if (!strncmp(snap + ls, "lrm.", 4) || !strncmp(snap + ls, "srm.", 4) || !strncmp(snap + ls, "lrm_player.", 11) ||
            !strncmp(snap + ls, "flight.", 7) ||
            !strncmp(snap + ls, "dlz.dt=", 7) || !strncmp(snap + ls, "dlz.padFrac=", 12) || !strncmp(snap + ls, "dlz.earlyOut=", 13) ||
            !strncmp(snap + ls, "dlz.tableGUse=", 14)) {
            int k, z = 0;                           /* VC6 prints exponents as e-005, clang as e-05: skip exponent zeros */
            for (k = ls; k < i; k++) {
                char ch = snap[k];
                if (z && ch == '0' && k + 1 < i && snap[k + 1] >= '0' && snap[k + 1] <= '9') continue;
                z = (ch == '+' || ch == '-') && k > ls && (snap[k - 1] == 'e' || snap[k - 1] == 'E');
                h = fnv(h, &ch, 1);
            }
        }
        ls = i + 1;
    }
    e_snprintf(gu, (int)sizeof(gu), "solver v4 nodes=%d", (int)TAB_NODES);
    h = fnv(h, gu, (int)strlen(gu));
    return h;
}

static void node_geom(int type, int ia, int is, int id, int iv, int ih, DlzGeom *g)
{
    float el = E_DEG2RAD(s_tEl[id]), a = E_DEG2RAD(s_tAsp[ia]), Vt, h = s_tAlt[ih];
    Vec3 los = v3(0.0f, (float)sin(el), (float)cos(el));
    memset(g, 0, sizeof(*g));
    g->type = type;
    g->sPos = v3(0.0f, h, 0.0f);
    g->sNose = los;
    g->sV = s_tSpd[is] * KT2MS;
    g->tPos = v3_madd(g->sPos, los, TAB_DIST);
    Vt = flight_speed_target(s_tThr[iv], h);
    g->tVel = v3(Vt * (float)sin(a), 0.0f, -Vt * (float)cos(a));   /* aspect 0 = flying at the shooter */
    g->tAb = s_tThr[iv] == THR_FAST ? 1.0f : 0.0f;
    g->gUse = s_tabGUse;
    g->ter = NULL;
}

int dlz_table_node(int type, int ia, int is, int id, int iv, int ih, DlzZone *out, DlzGeom *geom)
{
    if (type < 0 || type >= MSLK_COUNT || ia < 0 || ia >= DLZT_NA || is < 0 || is >= DLZT_NS || id < 0 || id >= DLZT_ND || iv < 0 ||
        iv >= DLZT_NV || ih < 0 || ih >= DLZT_NH) return 0;
    if (out) *out = s_tz[tidx(type, ia, is, id, iv, ih)];
    if (geom) node_geom(type, ia, is, id, iv, ih, geom);
    return s_tabOk;
}

void dlz_tables_build(void)
{
    int type, ia, is, id, iv, ih, k;
    DlzGeom g;
    DlzZone warm, z;
    s64 t0 = prof_now_us();
    u32 st0 = s_stats.steps, fo0 = s_stats.flyouts;
    s_tabMode = 1;
    for (type = 0; type < MSLK_COUNT; type++)
        for (ih = 0; ih < DLZT_NH; ih++)
            for (iv = 0; iv < DLZT_NV; iv++)
                for (id = 0; id < DLZT_ND; id++)
                    for (is = 0; is < DLZT_NS; is++)
                        for (ia = 0; ia < DLZT_NA; ia++) {
                            /* warm start from the most similar node already solved */
                            if (id > 0) warm = s_tz[tidx(type, ia, is, id - 1, iv, ih)];
                            else if (is > 0) warm = s_tz[tidx(type, ia, is - 1, id, iv, ih)];
                            else if (iv > 0) warm = s_tz[tidx(type, ia, is, id, iv - 1, ih)];
                            else if (ih > 0) warm = s_tz[tidx(type, ia, is, id, iv, ih - 1)];
                            else if (ia > 0) warm = s_tz[tidx(type, ia - 1, is, id, iv, ih)];
                            else default_zone(type, &warm);
                            node_geom(type, ia, is, id, iv, ih, &g);
                            dlz_solve(&g, &warm, &z);
                            /* T21a: the player's LRM reaches ~2x further than its warm start often says (e.g. after
                               the sea-limited -7 deg node at 1500 m): while the Rmax bracket has no miss (the table
                               mode's 6 fly-outs ran out), solve again from 1.5x the verified hit. Only this kind, so
                               the enemy LRM / SRM nodes stay bit-identical to T13's tables (enemy AI unchanged). */
                            for (k = 0; type == MSLK_LRM_PLAYER && k < 4 && z.rmax > 0.0f && z.rmaxHi <= 0.0f; k++) {
                                warm = z;
                                warm.rmax = 1.5f * z.rmax;
                                dlz_solve(&g, &warm, &z);
                            }
                            s_tz[tidx(type, ia, is, id, iv, ih)] = z;
                        }
    s_tabMode = 0;
    s_tabFromFile = 0;
    s_tabKey = tab_key();
    s_tabFp = tab_fp();
    s_tabOk = 1;
    s_tabStaleLogged = 0;
    s_tabMs = t0 ? (float)(prof_now_us() - t0) * 0.001f : -1.0f;
    log_printf(LOG_INFO, "dlz: table built ms=%.1f nodes=%d flyouts=%u steps=%u tableGUse=%.2f key=%08x", (double)s_tabMs, (int)TAB_NODES,
               s_stats.flyouts - fo0, s_stats.steps - st0, (double)s_tabGUse, s_tabKey);
}

int dlz_tables_ok(void) { return s_tabOk && s_tabFp == tab_fp(); }
float dlz_tables_ms(void) { return s_tabMs; }

/* data/dlz.tab: "DLZT", u32 version, u32 key, u32 node count, then per node (tidx order) rmax rne rmin tof (float32)
   + u32 worstEsc; little-endian (VM and Mac) */
enum { TAB_VER = 2, TAB_NODE_BYTES = 20, TAB_HDR = 16 };
static int tab_bytes(void) { return TAB_HDR + TAB_NODES * TAB_NODE_BYTES; }

int dlz_tables_save(const char *path)
{
    int n = tab_bytes(), i, o = TAB_HDR, ok;
    u32 hdr[4];
    u8 *buf;
    if (!s_tabOk) return 0;
    buf = (u8 *)malloc((size_t)n);
    if (!buf) return 0;
    memcpy(hdr, "DLZT", 4);
    hdr[1] = TAB_VER; hdr[2] = s_tabKey; hdr[3] = TAB_NODES;
    memcpy(buf, hdr, TAB_HDR);
    for (i = 0; i < TAB_NODES; i++) {
        const DlzZone *z = &s_tz[i];
        float f[4];
        u32 w = (u32)z->worstEsc;
        f[0] = z->rmax; f[1] = z->rne; f[2] = z->rmin; f[3] = z->tof;
        memcpy(buf + o, f, 16);
        memcpy(buf + o + 16, &w, 4);
        o += TAB_NODE_BYTES;
    }
    ok = file_save(path, buf, n);
    free(buf);
    log_printf(LOG_INFO, "dlz: table saved path=%s ok=%d key=%08x bytes=%d", path, ok, s_tabKey, n);
    return ok;
}

/* returns 1 when the file matches the current params key */
static int tables_load(const char *path)
{
    int size = 0, i, o = TAB_HDR, ok = 0;
    u32 hdr[4], key = tab_key();
    s64 t0 = prof_now_us();
    u8 *buf = (u8 *)file_load(path, &size);
    if (buf && size == tab_bytes()) {
        memcpy(hdr, buf, TAB_HDR);
        if (!memcmp(buf, "DLZT", 4) && hdr[1] == TAB_VER && hdr[2] == key && hdr[3] == (u32)TAB_NODES) {
            for (i = 0; i < TAB_NODES; i++) {
                DlzZone *z = &s_tz[i];
                float f[4];
                u32 w;
                memcpy(f, buf + o, 16);
                memcpy(&w, buf + o + 16, 4);
                memset(z, 0, sizeof(*z));
                z->rmax = f[0]; z->rne = f[1]; z->rmin = f[2]; z->tof = f[3]; z->worstEsc = (int)w;
                o += TAB_NODE_BYTES;
            }
            s_tabKey = key;
            s_tabFp = tab_fp();
            s_tabOk = 1;
            s_tabStaleLogged = 0;
            s_tabFromFile = 1;
            ok = 1;
        }
    }
    if (buf) free(buf);
    s_tabMs = t0 ? (float)(prof_now_us() - t0) * 0.001f : -1.0f;
    log_printf(LOG_INFO, "dlz: table %s path=%s ms=%.1f", ok ? "loaded" : buf ? "stale (params changed)" : "missing", path,
               (double)s_tabMs);
    return ok;
}

/* session start (dlz_reset): load data/dlz.tab, or rebuild when params changed (and save it for the next start).
   Never mid-session: AI decisions stay deterministic and nothing stalls after an F5; a stale table is logged once. */
/* allowBuild 0: session start (dlz_reset): load data/dlz.tab or the cache only; 1: first use (AI lookup, new HUD target):
   build synchronously if nothing matches (only after a params change; ~13 s on the VM, < 0.1 s on the Mac; logged), then
   save the cache. Deterministic: same params -> same tables at the same point of the session. A params change in the
   middle of a session (F5) keeps the old tables until the next session start (logged once). */
static void tables_ensure(int allowBuild)
{
    char path[E_PATH], cache[E_PATH];
    if (!s_tablesOn) return;
    if (s_tabOk && s_tabFp == tab_fp()) return;
    if (s_tabOk && s_tabKey == tab_key()) { s_tabFp = tab_fp(); return; }   /* same text, other bits: keep */
    if (s_tabOk && allowBuild) {
        if (!s_tabStaleLogged) log_printf(LOG_INFO, "dlz: table stale (params changed); rebuilt at the next session start");
        s_tabStaleLogged = 1;
        return;
    }
    if (!s_tabOk && s_tabTried && !allowBuild) return;
    if (!s_tabTried || !allowBuild) {
        s_tabTried = 1;
        data_path(path, (int)sizeof(path), "dlz.tab");         /* shipped (simh dlztable -out data/dlz.tab) */
        if (tables_load(path)) return;
        base_path(cache, (int)sizeof(cache), "dlz_cache.tab"); /* tuned params: last rebuild (never the shipped file) */
        if (tables_load(cache)) return;
        s_tabOk = 0;
    }
    if (!allowBuild) return;
    dlz_tables_build();
    if (s_saveOnBuild) { base_path(cache, (int)sizeof(cache), "dlz_cache.tab"); dlz_tables_save(cache); }
}

void dlz_tables_reload(void) { s_tabOk = 0; s_tabTried = 0; tables_ensure(0); tables_ensure(1); }
int  dlz_tables_loaded(void) { return s_tabOk && s_tabFromFile; }

static void axis(const float *ax, int n, float v, int *i0, float *f)
{
    int i;
    if (v <= ax[0]) { *i0 = 0; *f = 0.0f; return; }
    if (v >= ax[n - 1]) { *i0 = n - 2; *f = 1.0f; return; }
    for (i = 0; i < n - 2 && v > ax[i + 1]; i++) {}
    *i0 = i;
    *f = (v - ax[i]) / (ax[i + 1] - ax[i]);
}

int dlz_ai_zone(int type, Vec3 sPos, Vec3 sVel, Vec3 tPos, Vec3 tVel, DlzZone *out)
{
    int ia, is, id, ih, c, k, worst = DLZ_BREAK_L;
    int i0[5];
    float f[5], w, wBest = -1.0f, rmax = 0.0f, rne = 0.0f, rmin = 0.0f, tof = 0.0f, asp, el, hx, hz, h, tv, vn, vf, fv, lock;
    Vec3 d;
    TrackView at;
    memset(out, 0, sizeof(*out));
    if (type < 0 || type >= MSLK_COUNT) return 0;
    tables_ensure(1);
    if (!s_tabOk) return 0;
    d = v3_sub(sPos, tPos);                          /* target -> shooter */
    hx = d.x; hz = d.z;
    h = (float)sqrt(hx * hx + hz * hz);
    tv = (float)sqrt(tVel.x * tVel.x + tVel.z * tVel.z);
    if (h > 1.0f && tv > 1.0f) asp = E_RAD2DEG((float)acos(E_CLAMP((hx * tVel.x + hz * tVel.z) / (h * tv), -1.0f, 1.0f)));
    else asp = 90.0f;
    el = E_RAD2DEG((float)atan2(tPos.y - sPos.y, E_MAXF(h, 1.0f)));
    vn = flight_speed_target(THR_NORMAL, sPos.y);
    vf = flight_speed_target(THR_FAST, sPos.y);
    fv = E_CLAMP((v3_len(tVel) - vn) / E_MAXF(vf - vn, 1.0f), 0.0f, 1.0f);
    axis(s_tAsp, DLZT_NA, asp, &i0[0], &f[0]);
    axis(s_tSpd, DLZT_NS, v3_len(sVel) * MS2KT, &i0[1], &f[1]);
    axis(s_tEl, DLZT_ND, el, &i0[2], &f[2]);
    i0[3] = 0; f[3] = fv;
    axis(s_tAlt, DLZT_NH, sPos.y, &i0[4], &f[4]);
    for (c = 0; c < 32; c++) {
        const DlzZone *z;
        w = 1.0f;
        for (k = 0; k < 5; k++) w *= (c >> k) & 1 ? f[k] : 1.0f - f[k];
        if (w <= 0.0f) continue;
        ia = i0[0] + (c & 1); is = i0[1] + ((c >> 1) & 1); id = i0[2] + ((c >> 2) & 1); ih = i0[4] + ((c >> 4) & 1);
        z = &s_tz[tidx(type, ia, is, id, i0[3] + ((c >> 3) & 1), ih)];
        rmax += w * z->rmax; rne += w * z->rne; rmin += w * z->rmin; tof += w * z->tof;
        if (w > wBest) { wBest = w; worst = z->worstEsc; }
    }
    if (type == MSL_SRM) {                           /* guided SRM: never beyond the seeker lock range of THIS geometry */
        memset(&at, 0, sizeof(at));
        at.valid = 1; at.pos = tPos; at.vel = tVel; at.ab = fv > 0.5f ? 1.0f : 0.0f;
        lock = missile_heat_range(missile_params_of(MSL_SRM), &at, sPos);
        if (rmax > lock) rmax = lock;
    }
    if (rmax < rmin) rmax = 0.0f;
    if (rne > rmax) rne = rmax;
    out->rmax = rmax; out->rne = rne; out->rmin = rmin; out->tof = tof; out->worstEsc = worst;
    return 1;
}

int dlz_ai_zone_ac(const Game *g, int shooter, int target, int type, DlzZone *out)
{
    const FlightState *s = &g->ac[shooter].fs, *t = &g->ac[target].fs;
    type = missile_kind(type, (g->ac[shooter].flags & AC_PLAYER) != 0);         /* T21a: the shooter's own missile */
    return dlz_ai_zone(type, s->pos, q_rotate(s->qPath, v3(0.0f, 0.0f, s->V)), t->pos, q_rotate(t->qPath, v3(0.0f, 0.0f, t->V)), out);
}

/* ------------------------------------------------------------------ live HUD solver */
static Solver s_S;
static DlzView s_view;
static DlzZone s_last;                              /* last raw cycle result (warm start of the next) */
static int s_cycleActive, s_target = -1, s_type = -1;
static u32 s_cycleTick, s_pubTick;
static float s_avgV;                                /* missile average speed to the intercept (lead) */

const DlzView *dlz_view(void) { return &s_view; }

int dlz_budget(int requested)
{
    int b = (g_args.preset[0] == 'h' || g_args.preset[0] == 'H') ? s_stepsHigh : s_stepsLow;
    if (requested > 0 && requested < b) b = requested;
    return b;
}

static void view_clear(void)
{
    memset(&s_view, 0, sizeof(s_view));
    s_view.target = -1;
    s_view.tof = -1.0f;
    s_target = -1;
    s_type = -1;
    s_cycleActive = 0;
}

void dlz_reset(Game *g)
{
    (void)g;
    view_clear();
    memset(&s_last, 0, sizeof(s_last));
    if (s_tabOk && s_tabFp != tab_fp() && s_tabKey != tab_key()) s_tabOk = 0;   /* params changed: reload / rebuild */
    s_tabTried = 0;
    tables_ensure(0);
}

static void publish(float *v, int *src, float nv)
{
    if (*src != DLZV_LIVE || nv <= 0.0f || *v <= 0.0f) *v = nv;
    else *v += (nv < *v ? s_smDown : s_smUp) * (nv - *v);
    *src = DLZV_LIVE;
}

/* rotate a vector about the vertical by a (rad, + = right / clockwise from above) */
static Vec3 rot_y(Vec3 v, float a)
{
    float c = (float)cos(a), s = (float)sin(a);
    return v3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
}

/* both aircraft flown ahead by tau at their current turn rates (heading rate, rad/s) */
static void geom_lead(DlzGeom *g, float tau, float sTurn, float tTurn)
{
    Vec3 sv = v3_scale(g->sNose, g->sV);
    g->sPos = v3_madd(g->sPos, rot_y(sv, 0.5f * sTurn * tau), tau);
    g->sNose = rot_y(g->sNose, sTurn * tau);
    g->tPos = v3_madd(g->tPos, rot_y(g->tVel, 0.5f * tTurn * tau), tau);
    g->tVel = rot_y(g->tVel, tTurn * tau);
}

/* intercept direction for a missile of average speed vm (constant-velocity target) */
static Vec3 lead_dir(Vec3 sPos, Vec3 tPos, Vec3 tVel, float vm)
{
    Vec3 r = v3_sub(tPos, sPos);
    float a = v3_dot(tVel, tVel) - vm * vm, b = 2.0f * v3_dot(r, tVel), c = v3_dot(r, r), disc, t = -1.0f, t1, t2;
    if ((float)fabs(a) < 1e-3f) t = b < 0.0f ? -c / b : -1.0f;
    else {
        disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f) {
            disc = (float)sqrt(disc);
            t1 = (-b - disc) / (2.0f * a);
            t2 = (-b + disc) / (2.0f * a);
            t = t1 > 0.0f && (t1 < t2 || t2 <= 0.0f) ? t1 : t2;
        }
    }
    if (t <= 0.0f || t > 120.0f) return v3_norm(r);
    return v3_norm(v3_madd(r, tVel, t));
}

void dlz_work(Game *g, int budgetSteps)
{
    const Aircraft *pl;
    const WpnState *ws;
    DlzGeom geom;
    DlzZone z;
    Vec3 tp, tv, sVel, d;
    int td, type, kind, trk, budget, used, loops = 0, frameSteps = 0;
    s64 t0, cap;
    if (!g) return;
    s_stats.frames++;
    pl = &g->ac[PLAYER_ID];
    td = radar_designated(PLAYER_ID);
    ws = weapons_state(PLAYER_ID);
    type = ws ? ws->sel : MSL_LRM;
    kind = missile_kind(type, (pl->flags & AC_PLAYER) != 0);   /* T21a: the player's LRM is [lrm_player] */
    if (!(pl->flags & AC_ALIVE) || td <= 0 || td >= g->numAircraft || !(g->ac[td].flags & AC_ALIVE)) { view_clear(); return; }
    trk = radar_track(g, PLAYER_ID, td, &tp, &tv);
    if (!trk) { view_clear(); return; }
    sVel = q_rotate(pl->fs.qPath, v3(0.0f, 0.0f, pl->fs.V));
    if (td != s_target || kind != s_type) {        /* new target / weapon: table seed, nothing live yet */
        view_clear();
        s_target = td;
        s_type = kind;
        if (!dlz_ai_zone(kind, pl->fs.pos, sVel, tp, tv, &z)) default_zone(kind, &z);
        s_last = z;
        s_view.rmax = z.rmax; s_view.rne = z.rne; s_view.rmin = z.rmin;
        s_view.srcMax = s_view.srcNe = s_view.srcMin = DLZV_TABLE;
        {                                               /* T21c: Raero = zero-closure aerodynamic range */
            Vec3 dd = v3_sub(tp, pl->fs.pos);
            float l = v3_len(dd), vAlong = l > 1.0f ? v3_dot(tv, dd) / l : 0.0f;
            s_view.raero = z.rmax + vAlong * z.tof;
            s_view.srcAero = DLZV_TABLE;
        }
        s_view.worstEsc = z.worstEsc;
        s_avgV = type == MSL_SRM ? 650.0f : 750.0f;
        s_pubTick = g->tick;
    }
    s_view.valid = 1;
    s_view.target = td;
    s_view.type = type;
    s_view.memory = trk == 2;
    d = v3_sub(tp, pl->fs.pos);
    s_view.range = v3_len(d);
    s_view.closure = s_view.range > 1.0f ? -v3_dot(v3_sub(tv, sVel), d) / s_view.range : 0.0f;
    /* live cycles, time-sliced */
    budget = dlz_budget(budgetSteps);
    cap = s_maxUsFrame > 0 ? (s64)s_maxUsFrame : 0;
    if (!s_view.ready) { budget *= 2; cap *= 2; }   /* new target: first live Rmax within ~0.25 s (DESIGN 10.6) */
    t0 = cap ? prof_now_us() : 0;
    while (budget > 0 && loops++ < 8) {
        if (!s_cycleActive) {
            dlz_geom_from(g, PLAYER_ID, td, type, &geom);
            geom.tPos = tp;                          /* the radar's knowledge (memory = estimate), not the truth */
            geom.tVel = tv;
            /* predict the snapshot to the middle of the coming cycle (values are published ~one cycle later) */
            if (s_view.cycleSec > 0.0f && s_leadFrac > 0.0f) {
                float tau = E_MINF(s_leadFrac * s_view.cycleSec, 1.0f);
                geom_lead(&geom, tau, pl->fs.turnRate, trk == 1 ? g->ac[td].fs.turnRate : 0.0f);
            }
            solver_begin(&s_S, &geom, &s_last, s_view.ready);   /* new target: Rmax first, NOW from cycle 2 */
            if (s_view.ready) {                      /* steady state: spread the slow-changing work over cycles */
                s_S.skipEsc = DLZ_BREAK_L + (int)(s_view.cycles % 4u);
                s_S.lite = (int)(s_view.cycles & 1u);
            }
            s_cycleActive = 1;
            s_cycleTick = g->tick;
        }
        if (cap && t0 && prof_now_us() - t0 >= cap) break;
        used = solver_run(&s_S, budget, cap && t0 ? cap - (prof_now_us() - t0) : 0);
        budget -= used;
        frameSteps += used;
        if (s_S.pub) {
            int p = s_S.pub;
            s_S.pub = 0;
            if (p & PUB_NOW) {
                s_view.nowValid = 1;
                s_view.nowHit = s_S.nowHit;
                s_view.tof = s_S.nowHit ? s_S.nowFly.tof : -1.0f;
                if (s_S.nowHit && s_S.nowFly.tof > 0.1f)
                    s_avgV += 0.5f * (v3_len(v3_sub(s_S.nowFly.hitPos, s_S.g.sPos)) / s_S.nowFly.tof - s_avgV);
                if (!s_S.nowHit) { s_view.nowEscValid = 1; s_view.nowNoEscape = 0; }
            }
            if (p & PUB_NOWESC) { s_view.nowEscValid = 1; s_view.nowNoEscape = s_S.nowNoEsc; s_view.nowEscFlown = !s_S.nowEscSkipped; }
            if (p & PUB_MAX) {
                publish(&s_view.rmax, &s_view.srcMax, s_S.out.rmax);
                s_view.ready = 1;
                if (s_S.out.tof > 0.0f) {           /* T21c: Raero = Rmax + (target LOS velocity) x TOF at Rmax */
                    float l = v3_len(d), vAlong = l > 1.0f ? v3_dot(tv, d) / l : 0.0f;
                    publish(&s_view.raero, &s_view.srcAero, s_S.out.rmax + vAlong * s_S.out.tof);
                }
            }
            if (p & PUB_NE) { publish(&s_view.rne, &s_view.srcNe, s_S.out.rne); s_view.worstEsc = s_S.out.worstEsc; }
            if (p & PUB_MIN) publish(&s_view.rmin, &s_view.srcMin, s_S.out.rmin);
            s_pubTick = s_cycleTick;
        }
        if (s_S.stage == ST_DONE) {
            s_last = s_S.out;
            s_cycleActive = 0;
            s_view.cycles++;
            s_view.cycleSec = (float)(g->tick - s_cycleTick) / (float)SIM_HZ;
            s_view.cycleFlyouts = s_S.flyouts;
            s_view.cycleSteps = s_S.steps;
            s_stats.cycles++;
        }
        if (used == 0 || (cap && t0 && prof_now_us() - t0 >= cap)) break;
    }
    s_stats.lastFrameSteps = frameSteps;
    if (s_view.rne > s_view.rmax) s_view.rne = s_view.rmax;
    s_view.inRmax = s_view.rmax > 0.0f && s_view.range <= s_view.rmax && s_view.range > s_view.rmin;
    s_view.inRne = s_view.rne > 0.0f && s_view.range <= s_view.rne && s_view.range > s_view.rmin;
    s_view.inRmin = s_view.rmax > 0.0f && s_view.range <= s_view.rmin;
    s_view.leadDir = lead_dir(pl->fs.pos, tp, tv, s_avgV);
    s_view.leadValid = 1;
    s_view.ageSec = (float)(g->tick - s_pubTick) / (float)SIM_HZ;
}
