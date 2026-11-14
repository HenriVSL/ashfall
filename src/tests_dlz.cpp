/* tests_dlz.cpp - -test dlz, simh shots, simh dlztable (DESIGN 12.4, 12.7, 16.1). Portable. Owner: T13.

   -test dlz runs with the CODE DEFAULTS of every param (snapshot, empty ini, restore), like the other suites.
   Checks ("test: dlz <what> ok|FAIL ..."):
     solver vs T12's gameplay table (co-altitude 3000 m, both NORMAL), honesty with LIVE shots through weapons.cpp at
     dt 1/120 against targets flown by the real flight model (an escape pilot here: roll + pull to the level turn at
     escapeGUse x the G limit, afterburner), on 40 random geometries per missile type:
       0.97 Rmax straight hits >= 95 %, 1.05 Rmax straight misses >= 90 %, 0.95 Rne hits 100 % vs each escape of E,
       a random range inside 0.9 Rne hits >= 90 % vs a Normal-AI style defence (best escape after reactSec 0.7,
       flares with flareProb 0.5 inside 3 km), 1.1 Rmax vs the best escape misses >= 90 %;
     the live HUD solver (dlz_work) on a session-like Game: first live Rmax, full refresh at the Low budget, per-frame
     cost, lag behind a breaking target, determinism (Game untouched); AI tables (build time, node and off-node
     agreement with fly-outs); a designated target behind the volcano collapses Rmax.
   simh shots N [seed]: N random live shots (inside Rne / between Rne and Rmax / beyond Rmax, vs straight, every
   escape, the AI defence) -> hit-rate table. simh dlztable: prints the AI tables. */
#include "tests.h"
#include "dlz.h"
#include "missile.h"
#include "weapons.h"
#include "radar.h"
#include "params.h"
#include "e_log.h"
#include "e_prof.h"
#include "e_file.h"
#include "e_terrain.h"

static Terrain s_ter;
static int s_terInit, s_terOk;

static void terrain_once(void)
{
    char path[E_PATH];
    if (s_terInit) return;
    data_path(path, (int)sizeof(path), "island/jet.isl");
    s_terOk = terrain_load(&s_ter, path);
    s_terInit = 1;
    log_printf(LOG_INFO, "test: dlz terrain loaded=%d path=%s", s_terOk, path);
}

/* T21a: geometries carry a missile KIND (MSLK_*): "lrm" = the enemy LRM ([lrm], shooter without AC_PLAYER), "srm",
   "lrmp" = the player's LRM ([lrm_player], shooter with AC_PLAYER). weapons.cpp / dlz pick the set from AC_PLAYER. */
static const char *tname(int type) { return type == MSL_SRM ? "srm" : type == MSLK_LRM_PLAYER ? "lrmp" : "lrm"; }
enum { NK = MSLK_COUNT };

/* ================================================================ live world (weapons.cpp path) */
static Game s_g;
static void test_sensor(const Game *g, int obs, WpnSensor *out)
{
    (void)g;
    out->tdId = obs == 0 ? 1 : -1;
    out->lock = obs == 0 ? WLOCK_TRACK : WLOCK_NONE;
    out->memPos = out->memVel = v3(0.0f, 0.0f, 0.0f);
}

static u32 s_shotSeq;                               /* T17: per-shot RNG seed (was 4242 for every shot: the first flare
                                                       decoy roll was always the same draw, 0.803, so no decoy ever) */
static void g_start(const Terrain *ter)
{
    int i;
    memset(&s_g, 0, sizeof(s_g));
    rnd_seed(&s_g.rng, 4242u + 7919u * s_shotSeq++);
    s_g.terrain = (Terrain *)ter;
    s_g.numAircraft = 2;
    s_g.tick = 1;
    for (i = 0; i < MAX_AIRCRAFT; i++) {
        s_g.ac[i].id = i;
        s_g.ac[i].side = i ? SIDE_RED : SIDE_BLUE;
        s_g.ac[i].flags = i < 2 ? AC_ALIVE : 0;
        s_g.ac[i].hp = 100.0f;
    }
    s_g.ac[0].flags |= AC_PLAYER;
    weapons_set_sensor(test_sensor);
    weapons_reset(&s_g);
    radar_reset(&s_g);
}

static void g_place(int i, Vec3 pos, float hdg, float gamma, float V, int thr)
{
    Aircraft *a = &s_g.ac[i];
    flight_init(&a->fs, pos, hdg, V);
    flight_set_path(&a->fs, hdg, gamma, 0.0f);
    a->fs.onGround = 0;
    a->fs.gearPos = 0.0f;
    a->fs.alpha = 0.0f;
    a->fs.thr = thr;
    a->fs.ab = thr == THR_FAST ? 1.0f : 0.0f;
    a->prev = a->fs;
    memset(&a->in, 0, sizeof(a->in));
    a->in.thr = thr;
}

/* ---------------------------------------------------------------- escape pilot (real flight model) */
typedef struct EscPilot {
    int esc, active;
    float sign, turnTotal, turned, lastHdg, alt, gUse, reactT, t;
    int flares, flareTimer, flareRolled;
} EscPilot;

static float esc_wrap_pi(float a)
{
    while (a > E_PI) a -= 2.0f * E_PI;
    while (a < -E_PI) a += 2.0f * E_PI;
    return a;
}
static float esc_wrap_2pi(float a)
{
    while (a < 0.0f) a += 2.0f * E_PI;
    while (a >= 2.0f * E_PI) a -= 2.0f * E_PI;
    return a;
}

static void esc_init(EscPilot *p, const FlightState *t, Vec3 shooter, int esc, float reactSec)
{
    float ha, hd, a1, a2;
    memset(p, 0, sizeof(*p));
    p->esc = esc;
    p->alt = t->pos.y;
    p->gUse = dlz_escape_guse();
    p->reactT = reactSec;
    p->lastHdg = t->heading;
    if (esc == DLZ_STRAIGHT) return;
    p->sign = (esc == DLZ_BREAK_R || esc == DLZ_BEAM_R) ? 1.0f : -1.0f;
    ha = (float)atan2(t->pos.x - shooter.x, t->pos.z - shooter.z);
    hd = t->heading;
    if (esc == DLZ_BREAK_L || esc == DLZ_BREAK_R) p->turnTotal = esc_wrap_2pi(p->sign * (ha - hd));
    else {
        a1 = esc_wrap_2pi(p->sign * (ha + 0.5f * E_PI - hd));
        a2 = esc_wrap_2pi(p->sign * (ha - 0.5f * E_PI - hd));
        p->turnTotal = E_MINF(a1, a2);
    }
}

/* level turn at gUse x nLim until turned turnTotal, then wings level; altitude hold; FAST */
static void esc_input(EscPilot *p, const FlightState *fs, PilotInput *in)
{
    float bankDes = 0.0f, nDes, nLim = fs->nLim, nHold = fs->nHold, cg, cb, dh;
    memset(in, 0, sizeof(*in));
    p->t += SIM_DT;
    in->thr = p->esc == DLZ_STRAIGHT ? fs->thr : THR_FAST;
    if (p->esc == DLZ_STRAIGHT || p->t < p->reactT) { in->thr = fs->thr; return; }
    dh = esc_wrap_pi(fs->heading - p->lastHdg);
    p->lastHdg = fs->heading;
    if (!p->active) { p->active = 1; dh = 0.0f; }
    p->turned += p->sign * dh;
    if (p->turned < p->turnTotal - 0.01f - (float)fabs(fs->turnRate) * 0.30f) {   /* lead the roll-out: no overshoot */
        nDes = p->gUse * nLim;
        if (nDes < 1.05f) nDes = 1.05f;
        bankDes = p->sign * (float)acos(1.0f / nDes);
    }
    in->ur = E_CLAMP(4.0f * (bankDes - fs->bank), -1.0f, 1.0f);
    cg = (float)cos(fs->gamma);
    cb = (float)cos(fs->bank);
    if (cb < 0.08f) cb = 0.08f;
    nDes = cg / cb + 0.004f * (p->alt - fs->pos.y) - 0.04f * fs->vs;
    if (nDes > p->gUse * nLim) nDes = p->gUse * nLim;
    if (nDes >= nHold) in->up = nLim > nHold ? (nDes - nHold) / (nLim - nHold) : 0.0f;
    else in->up = -(nDes - nHold) / (flight_neg_g_limit() - nHold);
    in->up = E_CLAMP(in->up, -1.0f, 1.0f);
}

/* ---------------------------------------------------------------- geometry */
typedef struct Geo {
    int type;
    Vec3 sPos; float sHdg, sGam, sV;
    float az, el;                                   /* line of sight off the shooter's nose (rad) */
    float tHdg, tV; int tThr;
} Geo;

static Vec3 geo_los(const Geo *q)
{
    float h = q->sHdg + q->az, e = q->sGam + q->el;
    return v3((float)sin(h) * (float)cos(e), (float)sin(e), (float)cos(h) * (float)cos(e));
}

/* places shooter + target (at range R along the LOS) in s_g */
static void geo_place(const Geo *q, float R, const Terrain *ter)
{
    Vec3 tp = v3_madd(q->sPos, geo_los(q), R);
    g_start(ter);
    g_place(0, q->sPos, q->sHdg, q->sGam, q->sV, THR_NORMAL);
    g_place(1, tp, q->tHdg, 0.0f, q->tThr == THR_SLOW ? q->tV : flight_speed_target(q->tThr, tp.y), q->tThr);   /* steady speed */
    if (q->type == MSLK_LRM) s_g.ac[0].flags &= ~AC_PLAYER;   /* T21a: the enemy LRM = a non-player shooter */
    weapons_select(0, missile_kind_type(q->type));
    weapons_set_stores(0, 50, 50, 0);
    weapons_set_stores(1, 0, 0, 1000);
}

/* sensors + SRM lock refresh with the aircraft frozen (weapons_fire reads them; nothing moves) */
static void g_preroll(void)
{
    int k;
    for (k = 0; k < 13; k++) { weapons_tick(&s_g); s_g.tick++; }
}

/* random geometry over the sea (terrain off): both types, aspect 0-180 either side, off-boresight, altitudes 1.5-9 km */
static void geo_random(Geo *q, ERand *r, int type)
{
    float asp, side, los;
    memset(q, 0, sizeof(*q));
    q->type = type;
    q->sPos = v3(0.0f, rnd_range(r, 1500.0f, 9000.0f), 0.0f);
    q->sHdg = rnd_range(r, -E_PI, E_PI);
    q->sGam = E_DEG2RAD(rnd_range(r, -5.0f, 5.0f));
    q->sV = rnd_range(r, 180.0f, 420.0f);
    q->az = E_DEG2RAD(rnd_range(r, -1.0f, 1.0f) * (type == MSL_SRM ? 12.0f : 25.0f));
    q->el = E_DEG2RAD(rnd_range(r, -8.0f, 8.0f));
    if (q->sPos.y < 3000.0f && q->el < 0.0f) q->el = -q->el;
    asp = rnd_range(r, 0.0f, 180.0f);
    side = rnd_f01(r) < 0.5f ? -1.0f : 1.0f;
    los = q->sHdg + q->az;
    q->tHdg = los + E_PI - side * E_DEG2RAD(asp);   /* aspect 0 = flying at the shooter */
    /* the flight model holds a throttle state's speed: straight targets fly at their state's target speed (else they
       accelerate/decelerate and are not "straight at the current velocity") */
    q->tThr = rnd_f01(r) < 0.65f ? THR_NORMAL : THR_FAST;   /* SLOW is not steady (bleeds to the floor) */
    q->tV = flight_speed_target(q->tThr, q->sPos.y);
    q->sV = flight_speed_target(rnd_f01(r) < 0.7f ? THR_NORMAL : THR_FAST, q->sPos.y) * rnd_range(r, 0.9f, 1.0f);
}

static void geo_zone(const Geo *q, const Terrain *ter, DlzZone *z)
{
    DlzGeom g;
    DlzZone warm;
    geo_place(q, q->type == MSL_SRM ? 3000.0f : 9000.0f, ter);
    dlz_geom_from(&s_g, 0, 1, missile_kind_type(q->type), &g);       /* kind from AC_PLAYER (geo_place) */
    if (!dlz_ai_zone_ac(&s_g, 0, 1, missile_kind_type(q->type), &warm)) memset(&warm, 0, sizeof(warm));
    dlz_solve(&g, &warm, z);
}

/* ---------------------------------------------------------------- live shot */
enum { DEF_NONE, DEF_ESCAPE, DEF_AI };
static int s_flareRepeat, s_dbgTrack;                           /* 1: AI defence keeps dispensing every 0.5 s inside 3 km (stress) */
typedef struct ShotRes { int hit, res, decoyed, flares, guided; float t; } ShotRes;

static int live_shot(const Geo *q, float R, int esc, int defend, const Terrain *ter, ERand *rng, ShotRes *out)
{
    EscPilot ep;
    int slot, k, missedT = 0, maxTicks;
    const Missile *m;
    Aircraft *t;
    PilotInput in;
    float dist;
    memset(out, 0, sizeof(*out));
    geo_place(q, R, ter);
    t = &s_g.ac[1];
    esc_init(&ep, &t->fs, s_g.ac[0].fs.pos, esc, defend == DEF_AI ? 0.7f : 0.0f);
    g_preroll();
    slot = weapons_fire(&s_g, 0);
    out->guided = weapons_missile(slot) && (weapons_missile(slot)->flags & MF_GUIDED);
    if (slot < 0) return 0;
    maxTicks = (int)((missile_params_of(q->type)->maxSec + 2.0f) * SIM_HZ);
    for (k = 0; k < maxTicks; k++) {
        s_g.ac[0].prev = s_g.ac[0].fs;
        flight_step(&s_g.ac[0].fs, &s_g.ac[0].in, SIM_DT);
        t->prev = t->fs;
        esc_input(&ep, &t->fs, &in);
        t->in = in;
        flight_step(&t->fs, &in, SIM_DT);
        m = weapons_missile(slot);
        if (defend == DEF_AI && m && m->phase != MPH_DEAD) {
            dist = v3_len(v3_sub(m->pos, t->fs.pos));
            if (dist < 3000.0f && !ep.flareRolled) {
                ep.flareRolled = 1;
                ep.flares = rnd_f01(rng) < 0.5f;     /* Normal flareProb */
                if (ep.flares) { weapons_flares(&s_g, 1); out->flares++; ep.flareTimer = 60; }
            }
            if (s_flareRepeat && ep.flares && dist < 3000.0f && --ep.flareTimer <= 0) { weapons_flares(&s_g, 1); out->flares++; ep.flareTimer = 60; }
        }
        weapons_tick(&s_g);
        s_g.tick++;
        m = weapons_missile(slot);
        if (!m) break;
        if (m->flags & MF_DECOYED) out->decoyed = 1;
        if (m->phase == MPH_DEAD) break;
        if (m->flags & MF_UNGUIDED) break;
        if (m->flags & MF_MISSED) { if (++missedT > 240) break; }
        if (s_dbgTrack && (k % 60) == 0)
            log_printf(LOG_INFO, "trk: t=%.1f nLim=%.2f nCmd=%.2f up=%.2f nHold=%.2f hdg=%.1f V=%.0f bank=%.0f n=%.2f turned=%.1f/%.1f y=%.0f | msl V=%.0f d=%.0f", (double)(k / 120.0f), (double)t->fs.nLim, (double)t->fs.nCmd, (double)in.up, (double)t->fs.nHold,
                       (double)E_RAD2DEG(t->fs.heading), (double)t->fs.V, (double)E_RAD2DEG(t->fs.bank), (double)t->fs.n,
                       (double)E_RAD2DEG(ep.turned), (double)E_RAD2DEG(ep.turnTotal), (double)t->fs.pos.y, (double)m->V,
                       (double)v3_len(v3_sub(m->pos, t->fs.pos)));
    }
    m = weapons_missile(slot);
    out->res = m ? m->result : MRES_NONE;
    out->hit = m && m->result == MRES_HIT && m->target == 1;
    out->t = (float)k / (float)SIM_HZ;
    return 1;
}

/* ================================================================ 1. solver vs T12's gameplay table */
static void head_geo(Geo *q, int type, float aspDeg)
{
    memset(q, 0, sizeof(*q));
    q->type = type;
    q->sPos = v3(0.0f, 3000.0f, 0.0f);
    q->sV = flight_speed_target(THR_NORMAL, 3000.0f);
    q->tV = q->sV;
    q->tThr = THR_NORMAL;
    q->tHdg = E_PI - E_DEG2RAD(aspDeg);
}

static void t_gameplay(void)
{
    static const struct { int type; float asp; int kind; float lo, hi; const char *name; } rows[] = {
        { MSL_LRM, 0.0f, 0, 18800.0f, 20000.0f, "lrm_head_rmax(T12 19389)" },
        { MSL_LRM, 180.0f, 0, 6500.0f, 7100.0f, "lrm_tail_rmax(T12 6.8k)" },
        { MSL_LRM, 0.0f, 1, 7200.0f, 8000.0f, "lrm_head_rne(T12 7.2k instant turn; 7.6k with roll-in)" },
        { MSL_LRM, 0.0f, 2, 700.0f, 830.0f, "lrm_head_rmin(T12 760)" },
        { MSL_SRM, 0.0f, 0, 4900.0f, 5400.0f, "srm_head_rmax(lock range; T12 6.6k unguided tail)" },
        { MSL_SRM, 180.0f, 0, 2650.0f, 2850.0f, "srm_tail_rmax(T12 2758)" },
        { MSL_SRM, 0.0f, 1, 4300.0f, 5000.0f, "srm_head_rne(T12 4.6k)" },
        { MSL_SRM, 0.0f, 2, 320.0f, 400.0f, "srm_head_rmin(T12 350)" },
        /* T21b: the player's LRM ([lrm_player], realistic AMRAAM profile; tests_missile table: head ~40.4 km, Rne ~13.5 km) */
        { MSLK_LRM_PLAYER, 0.0f, 0, 39000.0f, 41800.0f, "lrmp_head_rmax(T21b 40.5k)" },
        { MSLK_LRM_PLAYER, 180.0f, 0, 15600.0f, 17200.0f, "lrmp_tail_rmax(T21b 16.4k)" },
        { MSLK_LRM_PLAYER, 0.0f, 1, 11900.0f, 13300.0f, "lrmp_head_rne(T21b 12.6k instant turn)" },
        { MSLK_LRM_PLAYER, 0.0f, 2, 870.0f, 1030.0f, "lrmp_head_rmin(T21b 0.95k)" },
    };
    Geo q;
    DlzZone z;
    int i;
    float v;
    for (i = 0; i < E_COUNT(rows); i++) {
        head_geo(&q, rows[i].type, rows[i].asp);
        geo_zone(&q, NULL, &z);
        v = rows[i].kind == 0 ? z.rmax : rows[i].kind == 1 ? z.rne : z.rmin;
        t_check(v >= rows[i].lo && v <= rows[i].hi, rows[i].name, "m=%.0f range=%.0f-%.0f flyouts=%d steps=%d worst=%s",
                (double)v, (double)rows[i].lo, (double)rows[i].hi, z.flyouts, z.steps, dlz_esc_name(z.worstEsc));
    }
}

/* ================================================================ 2. honesty with live shots */
typedef struct Tally { int n, hit; } Tally;
static void tally(Tally *t, int hit) { t->n++; t->hit += hit ? 1 : 0; }
static float rate(const Tally *t) { return t->n ? (float)t->hit / (float)t->n : 0.0f; }

static void t_honesty(int geos)
{
    Tally in97[NK], out105[NK], ne95[NK][DLZ_ESC_COUNT], ai90[NK], beyond[NK], unguided[NK], aiRep[NK];
    ShotRes sr2;
    Geo q;
    DlzZone z;
    ShotRes sr;
    ERand r, rf;
    int type, i, e, noZone = 0, noNe = 0, fails, aiDecoy[NK] = { 0, 0, 0 };
    float R;
    char what[64];
    memset(in97, 0, sizeof(in97)); memset(out105, 0, sizeof(out105)); memset(ne95, 0, sizeof(ne95));
    memset(ai90, 0, sizeof(ai90)); memset(beyond, 0, sizeof(beyond)); memset(unguided, 0, sizeof(unguided)); memset(aiRep, 0, sizeof(aiRep));
    rnd_seed(&r, 1313u);
    rnd_seed(&rf, 777u);
    for (type = 0; type < NK; type++)               /* T21a: + the player's LRM (after the old two: same sequences) */
        for (i = 0; i < geos; i++) {
            geo_random(&q, &r, type);
            geo_zone(&q, NULL, &z);
            if (z.rmax <= 0.0f) { noZone++; continue; }
            live_shot(&q, 0.97f * z.rmax, DLZ_STRAIGHT, DEF_NONE, NULL, &rf, &sr); tally(&in97[type], sr.hit);
            if (!sr.hit) log_printf(LOG_INFO, "test: dlz miss97 %s geo=%d R=%.0f rmax=%.0f [%.0f..%.0f] res=%s", tname(type), i,
                                    (double)(0.97f * z.rmax), (double)z.rmax, (double)z.rmaxLo, (double)z.rmaxHi, missile_result_name(sr.res));
            /* beyond Rmax a guided SRM launch is often impossible (no seeker lock = no tone = no SHOOT): weapons.cpp then
               fires an unguided search missile, which may still find the target; counted apart, not in the gate */
            live_shot(&q, 1.05f * z.rmax, DLZ_STRAIGHT, DEF_NONE, NULL, &rf, &sr);
            if (sr.guided) tally(&out105[type], !sr.hit); else tally(&unguided[type], sr.hit);
            if (sr.guided && sr.hit) log_printf(LOG_INFO, "test: dlz hit105 %s geo=%d R=%.0f rmax=%.0f [%.0f..%.0f]", tname(type), i,
                                                (double)(1.05f * z.rmax), (double)z.rmax, (double)z.rmaxLo, (double)z.rmaxHi);
            live_shot(&q, 1.10f * z.rmax, z.worstEsc, DEF_ESCAPE, NULL, &rf, &sr);
            if (sr.guided) tally(&beyond[type], !sr.hit); else tally(&unguided[type], sr.hit);
            if (z.rne <= z.rmin * 1.15f) { noNe++; continue; }
            for (e = DLZ_BREAK_L; e <= DLZ_BEAM_R; e++) {
                live_shot(&q, 0.95f * z.rne, e, DEF_ESCAPE, NULL, &rf, &sr);
                tally(&ne95[type][e], sr.hit);
                if (!sr.hit) log_printf(LOG_INFO, "test: dlz miss95ne %s geo=%d esc=%s R=%.0f rne=%.0f rmin=%.0f worst=%s res=%s",
                                        tname(type), i, dlz_esc_name(e), (double)(0.95f * z.rne), (double)z.rne, (double)z.rmin,
                                        dlz_esc_name(z.worstEsc), missile_result_name(sr.res));
            }
            R = rnd_range(&rf, E_MAXF(1.15f * z.rmin, 0.3f * z.rne), 0.9f * z.rne);
            live_shot(&q, R, z.worstEsc, DEF_AI, NULL, &rf, &sr);
            tally(&ai90[type], sr.hit);
            s_flareRepeat = 1;                       /* stress variant (info): flares every 0.5 s inside 3 km */
            live_shot(&q, R, z.worstEsc, DEF_AI, NULL, &rf, &sr2);
            s_flareRepeat = 0;
            tally(&aiRep[type], sr2.hit);
            if (sr.decoyed) aiDecoy[type]++;
            if (!sr.hit) log_printf(LOG_INFO, "test: dlz missai %s geo=%d R=%.0f rne=%.0f esc=%s flares=%d decoyed=%d res=%s", tname(type), i,
                                    (double)R, (double)z.rne, dlz_esc_name(z.worstEsc), sr.flares, sr.decoyed, missile_result_name(sr.res));
        }
    log_printf(LOG_INFO, "test: dlz honesty geometries=%d per type, no zone=%d, Rne<=1.15 Rmin=%d", geos, noZone, noNe);
    for (type = 0; type < NK; type++) {
        e_snprintf(what, (int)sizeof(what), "honest_%s_097rmax_hit", tname(type));
        t_check(rate(&in97[type]) >= 0.95f, what, "%d/%d (gate 95 %%)", in97[type].hit, in97[type].n);
        e_snprintf(what, (int)sizeof(what), "honest_%s_105rmax_miss", tname(type));
        t_check(rate(&out105[type]) >= 0.90f, what, "%d/%d (gate 90 %%)", out105[type].hit, out105[type].n);
        e_snprintf(what, (int)sizeof(what), "honest_%s_110rmax_escape_miss", tname(type));
        t_check(rate(&beyond[type]) >= 0.90f, what, "%d/%d (gate 90 %%)", beyond[type].hit, beyond[type].n);
        for (e = DLZ_BREAK_L; e <= DLZ_BEAM_R; e++) {
            e_snprintf(what, (int)sizeof(what), "honest_%s_095rne_%s", tname(type), dlz_esc_name(e));
            t_check(ne95[type][e].n > 0 && ne95[type][e].hit == ne95[type][e].n, what, "%d/%d (gate 100 %%)", ne95[type][e].hit,
                    ne95[type][e].n);
        }
        log_printf(LOG_INFO, "test: dlz info %s beyond-Rmax shots launched without lock (unguided search): %d, of which hit %d; "
                   "AI defence: decoyed %d; stress (flares every 0.5 s inside 3 km): hits %d/%d", tname(type), unguided[type].n,
                   unguided[type].hit, aiDecoy[type], aiRep[type].hit, aiRep[type].n);
        e_snprintf(what, (int)sizeof(what), "honest_%s_inside_090rne_ai_defence", tname(type));
        t_check(rate(&ai90[type]) >= 0.90f, what, "%d/%d (gate 90 %%, best escape after 0.7 s + one flare burst with p 0.5 at 3 km)", ai90[type].hit,
                ai90[type].n);
    }
    fails = 0;
    (void)fails;
}

/* ================================================================ 3. live HUD solver */
static void live_setup(const Geo *q, float R)
{
    int k;
    geo_place(q, R, NULL);
    for (k = 0; k < 24; k++) { radar_tick(&s_g); s_g.tick++; }      /* contact detected */
    radar_designate(&s_g, 0, 1);
    dlz_reset(&s_g);
}

/* one "frame" = ticksPerFrame sim ticks (aircraft fly; target pilot optional) + dlz_work */
static void live_frame(int ticksPerFrame, EscPilot *ep, int budget, float *frameUs)
{
    int k;
    PilotInput in;
    s64 t0;
    for (k = 0; k < ticksPerFrame; k++) {
        s_g.ac[0].prev = s_g.ac[0].fs;
        flight_step(&s_g.ac[0].fs, &s_g.ac[0].in, SIM_DT);
        s_g.ac[1].prev = s_g.ac[1].fs;
        if (ep) { esc_input(ep, &s_g.ac[1].fs, &in); s_g.ac[1].in = in; }
        flight_step(&s_g.ac[1].fs, &s_g.ac[1].in, SIM_DT);
        radar_tick(&s_g);
        s_g.tick++;
    }
    t0 = prof_now_us();
    dlz_work(&s_g, budget);
    if (frameUs) *frameUs = t0 ? (float)(prof_now_us() - t0) : 0.0f;
}

static void t_live(void)
{
    Geo q;
    const DlzView *v;
    DlzGeom g;
    DlzZone truth;
    EscPilot ep;
    int f, firstReady = -1, budget = dlz_budget(0), c0, f0 = 0, fC = 0, cycles, lagF;
    float us, worstUs = 0.0f, sumUs = 0.0f, err, lagNe = -1.0f, lagMax = -1.0f, sumSteady = 0.0f;
    int over = 0, nSteady = 0;
    static Game before;
    u32 s0;
    float hT[20], hTruth[20], hPub[20], hTruthNe[20], hPubNe[20];
    int nh;
    /* (a) new target -> first live Rmax; steady refresh at the Low budget (head-on LRM at 15 km, 30 fps = 4 ticks) */
    head_geo(&q, MSLK_LRM_PLAYER, 20.0f);              /* T21a: the HUD zone flies the player's own LRM */
    q.az = E_DEG2RAD(5.0f);
    live_setup(&q, 15000.0f);
    v = dlz_view();
    t_check(radar_designated(0) == 1, "live_designated", "td=%d", radar_designated(0));
    s0 = dlz_stats()->steps;
    for (f = 0; f < 300; f++) {
        live_frame(4, NULL, budget, &us);
        if (us > worstUs) worstUs = us;
        sumUs += us;
        if (v->ready && f > 10) { sumSteady += us; nSteady++; if (us > 2000.0f) over++; }
        if (firstReady < 0 && v->ready) firstReady = f + 1;
        if (f == 60) { f0 = f; c0 = (int)v->cycles; }
        if (f == 299) fC = f;
    }
    cycles = (int)v->cycles - c0;
    t_check(v->valid && v->ready && v->srcMax == DLZV_LIVE && v->srcNe == DLZV_LIVE && v->srcMin == DLZV_LIVE, "live_values",
            "rmax=%.0f rne=%.0f rmin=%.0f range=%.0f tof=%.1f now=%d noesc=%d worst=%s", (double)v->rmax, (double)v->rne,
            (double)v->rmin, (double)v->range, (double)v->tof, v->nowHit, v->nowNoEscape, dlz_esc_name(v->worstEsc));
    t_check(firstReady > 0 && firstReady * 4 <= 30 + 4, "live_first_rmax", "first live Rmax after %d frames = %.2f s at 30 fps "
            "(budget %d steps/frame; 10.6: <= 0.25 s)", firstReady, (double)(firstReady * 4) / 120.0, budget);
    /* T21a: the player's LRM flies ~2x as far (fly-outs up to ~40 s = 1200 steps instead of ~24 s): same budget and time
       cap, so a full cycle takes longer; gate 0.7 -> 1.2 s at 30 fps (T13 measured 0.66 s on the VM for the old LRM).
       T21b/T21d: the realistic lofted fly-outs (~42 s) took the VM to 1.33 s; T21d's coarse coast step (dlz.coastFarM,
       straight targets) brought it back to 1.14 s on the VM (the 1400 us time cap holds it to ~160 steps/frame there).
       Gate 1.2 -> 1.4 s at 30 fps for margin: the user accepts 15 fps on the VM (2.8 s) and the T40 (High budget 1500
       steps, ~8-10x faster) refreshes in ~0.2 s */
    t_check(cycles > 0 && (float)(fC - f0) / (float)cycles * 4.0f / 120.0f <= 1.4f, "live_refresh",
            "%d cycles in %d frames: full refresh %.2f s at 30 fps, %.2f s at 15 fps (Low budget %d; gate 1.4 s at 30 fps), "
            "avg steps/frame %.0f", cycles, fC - f0, (double)((float)(fC - f0) / (float)E_MAXF((float)cycles, 1.0f) * 4.0f / 120.0f),
            (double)((float)(fC - f0) / (float)E_MAXF((float)cycles, 1.0f) * 8.0f / 120.0f), budget,
            (double)((float)(dlz_stats()->steps - s0) / 300.0f));
    log_printf(LOG_INFO, "test: dlz live_cycle flyouts=%d steps=%d", v->cycleFlyouts, v->cycleSteps);
    log_printf(LOG_INFO, "test: dlz live_frame_cost avg_us=%.0f worst_us=%.0f steady_avg_us=%.0f frames_over_2ms=%d/%d budget=%d steps "
               "cap=%s us", (double)(sumUs / 300.0f), (double)worstUs, (double)(sumSteady / E_MAXF((float)nSteady, 1.0f)), over, nSteady,
               budget, params_find("dlz", "maxUsFrame") ? "on" : "?");
    if (prof_now_us()) t_check(sumSteady / E_MAXF((float)nSteady, 1.0f) <= 1500.0f, "live_frame_cost", "steady avg %.0f us per frame "
                               "(gate 1500 on the VM), worst %.0f us, %d frames over 2 ms", (double)(sumSteady / E_MAXF((float)nSteady, 1.0f)),
                               (double)worstUs, over);
    /* (b) determinism: dlz_work never touches the Game */
    memcpy(&before, &s_g, sizeof(s_g));
    for (f = 0; f < 20; f++) dlz_work(&s_g, budget);
    t_check(memcmp(&before, &s_g, sizeof(s_g)) == 0, "live_game_untouched", "20 dlz_work calls, Game bytes equal");
    /* (c) lag behind a breaking target: at 8 km inside a head-on LRM zone the target breaks; compare the published Rne/Rmax
       every 0.5 s with a synchronous solve of the same instant's geometry */
    head_geo(&q, MSLK_LRM_PLAYER, 10.0f);
    live_setup(&q, 12000.0f);
    for (f = 0; f < 60; f++) live_frame(4, NULL, budget, NULL);
    esc_init(&ep, &s_g.ac[1].fs, s_g.ac[0].fs.pos, DLZ_BREAK_R, 0.0f);
    lagF = -1;
    nh = 0;
    for (f = 0; f < 240; f++) {
        live_frame(4, &ep, budget, NULL);
        if ((f % 15) == 14) {
            dlz_geom_from(&s_g, 0, 1, MSL_LRM, &g);
            dlz_solve(&g, NULL, &truth);
            hT[nh] = (float)((f + 1) * 4) / 120.0f; hTruth[nh] = truth.rmax; hPub[nh] = v->rmax; hTruthNe[nh] = truth.rne; hPubNe[nh] = v->rne;
            nh++;
            err = truth.rmax > 0.0f ? (v->rmax - truth.rmax) / truth.rmax : 0.0f;
            log_printf(LOG_INFO, "test: dlz lag t=%.1f s pub rmax=%.0f rne=%.0f | truth rmax=%.0f rne=%.0f | err %.1f %% src=%d",
                       (double)((f + 1) * 4) / 120.0, (double)v->rmax, (double)v->rne, (double)truth.rmax, (double)truth.rne,
                       (double)(100.0f * err), v->srcMax);
            if (lagMax < 0.0f && f > 60 && (float)fabs(err) < 0.05f && (float)fabs(v->rne - truth.rne) < 0.06f * E_MAXF(truth.rne, 500.0f))
                lagMax = (float)((f + 1) * 4) / 120.0f;
        }
    }
    (void)lagNe; (void)lagF; (void)lagMax;
    /* lag = how long ago the truth had the value published now (while the zone shrinks: the target turns cold) */
    {
        float sum = 0.0f, worst = 0.0f, sumNe = 0.0f, worstNe = 0.0f, lg;
        int i, j, n = 0, nNe = 0;
        for (i = 1; i < nh; i++) {
            for (j = i; j > 0 && hTruth[j] < hPub[i]; j--) {}
            if (j < i && hTruth[j] >= hPub[i] && hTruth[j + 1] < hPub[i] && hTruth[j] > hTruth[j + 1]) {
                lg = hT[i] - (hT[j] + (hT[j + 1] - hT[j]) * (hTruth[j] - hPub[i]) / (hTruth[j] - hTruth[j + 1]));
                sum += lg; n++; if (lg > worst) worst = lg;
            }
            for (j = i; j > 0 && hTruthNe[j] < hPubNe[i]; j--) {}
            if (j < i && hTruthNe[j] >= hPubNe[i] && hTruthNe[j + 1] < hPubNe[i] && hTruthNe[j] > hTruthNe[j + 1]) {
                lg = hT[i] - (hT[j] + (hT[j + 1] - hT[j]) * (hTruthNe[j] - hPubNe[i]) / (hTruthNe[j] - hTruthNe[j + 1]));
                sumNe += lg; nNe++; if (lg > worstNe) worstNe = lg;
            }
        }
        log_printf(LOG_INFO, "test: dlz lag_break rmax avg=%.2f s worst=%.2f s (n=%d), rne avg=%.2f s worst=%.2f s (n=%d); cycle %.2f s at 30 fps",
                   (double)(n ? sum / (float)n : 0.0f), (double)worst, n, (double)(nNe ? sumNe / (float)nNe : 0.0f), (double)worstNe, nNe,
                   (double)v->cycleSec);
        t_check(1, "live_lag_info", "rmax lag avg %.2f s, rne lag avg %.2f s behind a breaking target (Low budget, 30 fps)",
                (double)(n ? sum / (float)n : 0.0f), (double)(nNe ? sumNe / (float)nNe : 0.0f));
    }
}

/* ================================================================ 4. AI tables */
static void t_tables(void)
{
    DlzZone n, z, lk;
    DlzGeom g;
    Geo q;
    ERand r;
    int type, ia, is, id, iv, ih, i, ok90 = 0, okNe = 0, okNe2 = 0, total = 0, kN[NK] = { 0, 0, 0 }, kOk[NK] = { 0, 0, 0 },
        kOkNe[NK] = { 0, 0, 0 };
    float worstNode = 0.0f, e, eNe, errs[200], sumAbs = 0.0f, sumAbsNe = 0.0f;
    {
        float buildMs;
        dlz_tables_build();
        buildMs = dlz_tables_ms();
        t_check(dlz_tables_ok(), "tables_built", "build ms=%.1f (synchronous rebuild, only when params changed: info)", (double)buildMs);
        dlz_tables_reload();                         /* what a session start does: data/dlz.tab */
        t_check(dlz_tables_ok() && dlz_tables_loaded(), "tables_load", "data/dlz.tab loaded=%d ms=%.1f (VM gate < 1000 ms; key must match "
                "the code defaults: regenerate with simh dlztable -out data/dlz.tab after a params change)", dlz_tables_loaded(),
                (double)dlz_tables_ms());
        if (dlz_tables_ms() >= 0.0f) t_check(dlz_tables_ms() < 1000.0f, "tables_time", "load ms=%.1f", (double)dlz_tables_ms());
    }
    /* nodes: lookup at the node geometry = the stored solve; a fresh fly-out at 0.97/1.03 x stored Rmax hits/misses */
    for (type = 0; type < NK; type++)
      for (ih = 0; ih < DLZT_NH; ih++)
        for (iv = 0; iv < DLZT_NV; iv++)
          for (ia = 0; ia < DLZT_NA; ia++)
            for (is = 0; is < DLZT_NS; is++)
                for (id = 0; id < DLZT_ND; id++) {
                    DlzFly f1, f2;
                    dlz_table_node(type, ia, is, id, iv, ih, &n, &g);
                    if (n.rmax <= 0.0f) continue;
                    dlz_ai_zone(type, g.sPos, v3_scale(g.sNose, g.sV), g.tPos, g.tVel, &lk);
                    e = (float)fabs(lk.rmax - n.rmax) / n.rmax;
                    if (e > worstNode) worstNode = e;
                    dlz_flyout(&g, 0.97f * n.rmax, DLZ_STRAIGHT, &f1);
                    dlz_flyout(&g, 1.03f * n.rmax, DLZ_STRAIGHT, &f2);
                    if (!f1.hit || f2.hit)
                        log_printf(LOG_INFO, "test: dlz node %s a%d s%d e%d v%d h%d rmax=%.0f 0.97:%d 1.03:%d", tname(type), ia, is, id,
                                   iv, ih, (double)n.rmax, f1.hit, f2.hit);
                    total++;
                    ok90 += f1.hit && !f2.hit;
                }
    t_check(worstNode < 0.01f, "tables_node_lookup", "worst |lookup-node| = %.2f %%", (double)(100.0f * worstNode));
    t_check(ok90 >= total * 97 / 100, "tables_node_flyouts", "%d/%d nodes: 0.97 Rmax hits and 1.03 Rmax misses (gate 97 %%)", ok90, total);
    ok90 = 0;
    /* off-node random geometries: lookup vs a full solve */
    rnd_seed(&r, 99u);
    total = 0;
    for (i = 0; i < 150; i++) {
        geo_random(&q, &r, i < 100 ? (i & 1) : MSLK_LRM_PLAYER);   /* T13's 100 (enemy LRM / SRM), then 50 player LRM (T21a) */
        q.az = 0.0f;                                 /* AI use: it fires with the nose on the target */
        geo_place(&q, q.type == MSL_SRM ? 3000.0f : 8000.0f, NULL);
        dlz_geom_from(&s_g, 0, 1, missile_kind_type(q.type), &g);
        g.gUse = *(float *)params_find("dlz", "tableGUse")->ptr;   /* tables: the player escaping at full G */
        dlz_solve(&g, NULL, &z);
        dlz_ai_zone_ac(&s_g, 0, 1, missile_kind_type(q.type), &lk);
        if (z.rmax <= 0.0f) continue;
        e = (lk.rmax - z.rmax) / z.rmax;
        eNe = z.rne > 0.0f ? (lk.rne - z.rne) / z.rne : 0.0f;
        if (i < 100) {
            errs[total++] = e;
            sumAbs += (float)fabs(e);
            sumAbsNe += (float)fabs(eNe);
            if ((float)fabs(e) <= 0.15f) okNe++;
            if ((float)fabs(eNe) <= 0.20f) okNe2++;
        }
        kN[q.type]++;
        if ((float)fabs(e) <= 0.15f) kOk[q.type]++;
        if ((float)fabs(eNe) <= 0.20f) kOkNe[q.type]++;
        if ((float)fabs(e) > 0.15f || (float)fabs(eNe) > 0.25f)
            log_printf(LOG_INFO, "test: dlz offnode %s alt=%.0f sv=%.0f tv=%.0f az=%.0f el=%.0f rmax solve=%.0f table=%.0f (%+.0f %%) "
                       "rne solve=%.0f table=%.0f (%+.0f %%)", tname(q.type), (double)q.sPos.y, (double)q.sV, (double)q.tV,
                       (double)E_RAD2DEG(q.az), (double)E_RAD2DEG(q.el), (double)z.rmax, (double)lk.rmax, (double)(100.0f * e),
                       (double)z.rne, (double)lk.rne, (double)(100.0f * eNe));
    }
    t_check(total > 0 && (float)okNe / (float)total >= 0.85f, "tables_offnode_rmax", "%d/%d within 15 %% of a full fly-out solve, "
            "mean |err| %.1f %% (gate 85 %%; random altitude/speeds/aspect/elevation, nose on target)", okNe, total,
            (double)(100.0f * sumAbs / (float)E_MAXF((float)total, 1.0f)));
    log_printf(LOG_INFO, "test: dlz offnode per kind (rmax within 15 %% / rne within 20 %%): lrm %d/%d %d/%d, srm %d/%d %d/%d, lrmp %d/%d %d/%d",
               kOk[0], kN[0], kOkNe[0], kN[0], kOk[1], kN[1], kOkNe[1], kN[1], kOk[2], kN[2], kOkNe[2], kN[2]);
    t_check(total > 0 && (float)okNe2 / (float)total >= 0.80f, "tables_offnode_rne", "%d/%d within 20 %%, mean |err| %.1f %% (gate 80 %%)",
            okNe2, total, (double)(100.0f * sumAbsNe / (float)E_MAXF((float)total, 1.0f)));
    /* T21b: the player LRM's table is display-only (HUD first frames, replaced by the live solve within ~0.25 s; the AI
       fires [lrm]/[srm], not [lrm_player]). Its realistic early-active seeker (activeM 18 km) makes the Rne surface more
       nonlinear than linear interpolation captures, so the Rne gate is one notch looser than the AI tables' 80 %. */
    t_check(kN[2] > 0 && kOk[2] * 100 >= 85 * kN[2] && kOkNe[2] * 100 >= 75 * kN[2], "tables_offnode_lrmp",   /* integers: x87 */
            "player LRM: rmax %d/%d within 15 %% (gate 85 %%), rne %d/%d within 20 %% (gate 75 %%)", kOk[2], kN[2], kOkNe[2], kN[2]);
    (void)errs;
}

/* ================================================================ 5. terrain: target behind the volcano */
static void find_summit(Vec3 *s)
{
    int i, j;
    float best = -1000.0f, h, x, z;
    *s = v3(0.0f, 0.0f, 0.0f);
    for (i = -60; i <= 60; i++)
        for (j = -60; j <= 60; j++) {
            x = s_ter.centreX + (float)i * 160.0f;
            z = s_ter.centreZ + (float)j * 160.0f;
            h = terrain_height(&s_ter, x, z);
            if (h > best) { best = h; *s = v3(x, h, z); }
        }
}

static void t_volcano(void)
{
    Vec3 summit, sp, tp;
    DlzGeom g;
    DlzZone zt, zs;
    float R0;
    if (!s_terOk) { t_check(0, "volcano_island", "jet.isl not loaded"); return; }
    find_summit(&summit);
    sp = v3(summit.x - 7000.0f, 1500.0f, summit.z);
    tp = v3(summit.x + 4500.0f, 1500.0f, summit.z);
    memset(&g, 0, sizeof(g));
    g.type = MSL_LRM;
    g.sPos = sp;
    g.sNose = v3_norm(v3_sub(tp, sp));
    g.sV = 260.0f;
    g.tPos = tp;
    g.tVel = v3(-250.0f, 0.0f, 0.0f);                /* coming at us from behind the cone */
    R0 = v3_len(v3_sub(tp, sp));
    g.ter = NULL;
    dlz_solve(&g, NULL, &zs);
    g.ter = &s_ter;
    dlz_solve(&g, NULL, &zt);
    t_check(zs.rmax > R0 && zt.rmax < R0 && zt.rmax < 0.8f * zs.rmax, "volcano_collapses_rmax",
            "range %.0f: open sea rmax=%.0f (SHOOT), behind the volcano rmax=%.0f rne=%.0f (no SHOOT)", (double)R0, (double)zs.rmax,
            (double)zt.rmax, (double)zt.rne);
    g.type = MSLK_LRM_PLAYER;                        /* T21a: the player's longer-ranged LRM must collapse too */
    g.ter = NULL;
    dlz_solve(&g, NULL, &zs);
    g.ter = &s_ter;
    dlz_solve(&g, NULL, &zt);
    t_check(zs.rmax > R0 && zt.rmax < R0 && zt.rmax < 0.8f * zs.rmax, "volcano_collapses_rmax_lrmp",
            "range %.0f: open sea rmax=%.0f (SHOOT), behind the volcano rmax=%.0f rne=%.0f (no SHOOT)", (double)R0, (double)zs.rmax,
            (double)zt.rmax, (double)zt.rne);
}

/* ================================================================ entry points */
static int with_defaults(void (*fn)(void *), void *ctx)
{
    static char snap[65536];
    int n, trace = 0;
    float dtKeep = 1.0f / 30.0f;
    Cfg *empty;
    const Param *tp = params_find("dlz", "trace");
    if (tp) trace = *(int *)tp->ptr;               /* debug switch survives the defaults */
    { const Param *dp = params_find("dlz", "dt"); if (dp) dtKeep = *(float *)dp->ptr; }
    n = params_snapshot(snap, (int)sizeof(snap));
    empty = cfg_parse("", 0);
    params_load(empty);
    cfg_free(empty);
    if (tp) *(int *)tp->ptr = trace;
    if (trace) { const Param *dp = params_find("dlz", "dt"); if (dp) *(float *)dp->ptr = dtKeep; }   /* debug: ini dt too */
    tp = params_find("weapons", "logEvents");
    if (tp) *(int *)tp->ptr = 0;                    /* thousands of live shots: no wpn: lines */
    fn(ctx);
    weapons_set_sensor(NULL);
    if (n > 0) params_restore(snap, n);
    return 0;
}

static void run_cmp(int type, int gi, int esc)
{
    Geo q;
    DlzZone z;
    DlzGeom g;
    DlzFly f;
    ShotRes sr;
    ERand r, rf;
    int i;
    rnd_seed(&r, 1313u);
    rnd_seed(&rf, 777u);
    for (i = 0; i <= gi + 40 * type; i++) geo_random(&q, &r, i >= 80 ? 2 : i >= 40 ? 1 : 0);   /* honesty set order */
    geo_zone(&q, NULL, &z);
    if (esc) {
        int k;
        float R = 0.95f * z.rne;
        geo_place(&q, R, NULL);
        dlz_geom_from(&s_g, 0, 1, q.type, &g);
        for (k = 0; k < 8; k++) {
            float RR = R * (0.80f + 0.05f * (float)k);
            dlz_flyout(&g, RR, esc, &f);
            log_printf(LOG_INFO, "cmp: esc=%s R=%.0f (rne %.0f) flyout hit=%d res=%s tof=%.2f min=%.0f", dlz_esc_name(esc), (double)RR,
                       (double)z.rne, f.hit, missile_result_name(f.res), (double)f.tof, (double)f.minDist);
            s_dbgTrack = k == 5;
            live_shot(&q, RR, esc, DEF_ESCAPE, NULL, &rf, &sr);
            s_dbgTrack = 0;
            log_printf(LOG_INFO, "cmp:   live hit=%d res=%s t=%.2f tgt V=%.0f hdg=%.1f", sr.hit, missile_result_name(sr.res), (double)sr.t,
                       (double)s_g.ac[1].fs.V, (double)E_RAD2DEG(s_g.ac[1].fs.heading));
        }
        return;
    }
    geo_place(&q, 0.97f * z.rmax, NULL);
    dlz_geom_from(&s_g, 0, 1, q.type, &g);
    dlz_flyout(&g, 0.97f * z.rmax, DLZ_STRAIGHT, &f);
    log_printf(LOG_INFO, "cmp: zone rmax=%.0f sV=%.0f tV=%.0f alt=%.0f | flyout hit=%d res=%s tof=%.2f min=%.0f", (double)z.rmax,
               (double)q.sV, (double)q.tV, (double)q.sPos.y, f.hit, missile_result_name(f.res), (double)f.tof, (double)f.minDist);
    live_shot(&q, 0.97f * z.rmax, DLZ_STRAIGHT, DEF_NONE, NULL, &rf, &sr);
    {
        const Param *pd = params_find("dlz", "dt");
        float old = *(float *)pd->ptr;
        *(float *)pd->ptr = SIM_DT;
        dlz_flyout(&g, 0.97f * z.rmax, DLZ_STRAIGHT, &f);
        log_printf(LOG_INFO, "cmp: flyout dt1/120 hit=%d res=%s tof=%.2f min=%.0f", f.hit, missile_result_name(f.res), (double)f.tof,
                   (double)f.minDist);
        *(float *)pd->ptr = old;
    }
    log_printf(LOG_INFO, "cmp: live hit=%d res=%s t=%.2f tgtV now=%.1f", sr.hit, missile_result_name(sr.res), (double)sr.t,
               (double)s_g.ac[1].fs.V);
}

static void run_all(void *ctx)
{
    const char *arg = (const char *)ctx;
    int geos = 40;
    if (arg && !strncmp(arg, "cmp:", 4)) { int a = 0, b = 0, c = 0; sscanf(arg, "cmp:%d:%d:%d", &a, &b, &c); run_cmp(a, b, c); return; }
    if (arg && !strncmp(arg, "geos:", 5)) geos = atoi(arg + 5);
    terrain_once();
    t_tables();
    t_gameplay();
    t_volcano();
    t_live();
    t_honesty(geos);
}

int test_dlz(const char *arg)
{
    t_begin("dlz");
    s_shotSeq = 0;
    log_printf(LOG_INFO, "test: dlz params=defaults");
    with_defaults(run_all, (void *)arg);
    return t_end();
}

/* simh shots N [seed]: random live shots by category */
enum { SC_INNE_ESC, SC_INNE_AI, SC_INNE_STR, SC_MID_STR, SC_MID_ESC, SC_OUT_STR, SC_OUT_ESC, SC_COUNT };
static const char *const s_scName[SC_COUNT] = {
    "inside Rne     vs escape (each of E)", "inside 0.9Rne  vs AI defence+flares", "inside Rne     vs straight",
    "Rne..Rmax      vs straight", "Rne..Rmax      vs escape", "1.03-1.3 Rmax  vs straight", "1.03-1.3 Rmax  vs escape" };

typedef struct ShotsCtx { int n; u32 seed; } ShotsCtx;
static void run_shots(void *ctx)
{
    ShotsCtx *c = (ShotsCtx *)ctx;
    Tally tl[NK][SC_COUNT];
    Geo q;
    DlzZone z;
    ShotRes sr;
    ERand r, rf;
    int i, type, cat, esc, def, terr = 0, nozone = 0;
    float R, lo, hi;
    memset(tl, 0, sizeof(tl));
    rnd_seed(&r, c->seed);
    rnd_seed(&rf, c->seed * 7u + 1u);
    for (i = 0; i < c->n; i++) {
        type = (int)(rnd_u32(&r) % (u32)NK);         /* T21a: enemy LRM, SRM, player LRM */
        cat = (int)(rnd_u32(&r) % (u32)SC_COUNT);
        geo_random(&q, &r, type);
        geo_zone(&q, NULL, &z);
        if (z.rmax <= 0.0f) { nozone++; continue; }
        esc = DLZ_BREAK_L + (int)(rnd_u32(&r) % 4u);
        def = DEF_ESCAPE;
        lo = E_MAXF(1.15f * z.rmin, 0.25f * z.rne);
        switch (cat) {
        case SC_INNE_ESC: hi = 0.97f * z.rne; break;
        case SC_INNE_AI: hi = 0.9f * z.rne; esc = z.worstEsc; def = DEF_AI; break;
        case SC_INNE_STR: hi = 0.97f * z.rne; esc = DLZ_STRAIGHT; def = DEF_NONE; break;
        case SC_MID_STR: lo = z.rne; hi = 0.98f * z.rmax; esc = DLZ_STRAIGHT; def = DEF_NONE; break;
        case SC_MID_ESC: lo = z.rne; hi = 0.98f * z.rmax; break;
        case SC_OUT_STR: lo = 1.03f * z.rmax; hi = 1.3f * z.rmax; esc = DLZ_STRAIGHT; def = DEF_NONE; break;
        default: lo = 1.03f * z.rmax; hi = 1.3f * z.rmax; break;
        }
        if (hi <= lo) { nozone++; continue; }
        R = rnd_range(&rf, lo, hi);
        live_shot(&q, R, esc, def, NULL, &rf, &sr);
        if (sr.res == MRES_GROUND) terr++;
        tally(&tl[type][cat], sr.hit);
    }
    log_printf(LOG_INFO, "shots: %d random live shots (weapons.cpp at 1/120 vs the flight model; zone from dlz_solve), seed %u; "
               "skipped (no zone / empty interval) %d, sea impacts %d", c->n, c->seed, nozone, terr);
    log_printf(LOG_INFO, "shots: %-38s %14s %14s %14s", "category", "LRM(enemy)", "SRM hits", "LRM(player)");
    for (cat = 0; cat < SC_COUNT; cat++)
        log_printf(LOG_INFO, "shots: %-38s %4d/%-4d %3.0f%% %4d/%-4d %3.0f%% %4d/%-4d %3.0f%%", s_scName[cat], tl[0][cat].hit,
                   tl[0][cat].n, (double)(100.0f * rate(&tl[0][cat])), tl[1][cat].hit, tl[1][cat].n, (double)(100.0f * rate(&tl[1][cat])),
                   tl[2][cat].hit, tl[2][cat].n, (double)(100.0f * rate(&tl[2][cat])));
}

int simh_shots(int argc, char **argv)
{
    ShotsCtx c;
    c.n = argc > 0 ? atoi(argv[0]) : 500;
    c.seed = argc > 1 ? (u32)atoi(argv[1]) : 2026u;
    if (c.n <= 0) c.n = 500;
    s_shotSeq = 0;
    terrain_once();
    with_defaults(run_shots, &c);
    return 0;
}

static void run_table(void *ctx)
{
    int type, ia, is, id, iv, ih;
    DlzZone n;
    (void)ctx;
    dlz_tables_build();
    for (type = 0; type < NK; type++)
      for (ih = 0; ih < DLZT_NH; ih++)
        for (iv = 0; iv < DLZT_NV; iv++) {
        log_printf(LOG_INFO, "dlztable: %s  shooter %s m, target %s; nose on target; Rmax/Rne/Rmin km, TOF s, worst escape, fly-outs",
                   type == MSL_SRM ? "SRM" : type == MSLK_LRM_PLAYER ? "LRMP" : "LRM", ih == 0 ? "1500" : ih == 1 ? "4500" : "8000", iv ? "FAST" : "NORMAL");
        for (id = 0; id < DLZT_ND; id++)
            for (is = 0; is < DLZT_NS; is++) {
                char line[400];
                int o = 0;
                o += e_snprintf(line + o, (int)sizeof(line) - o, "el %+3d sv %3d |", id * 7 - 7, is == 0 ? 300 : is == 1 ? 500 : 800);
                for (ia = 0; ia < DLZT_NA; ia++) {
                    dlz_table_node(type, ia, is, id, iv, ih, &n, NULL);
                    o += e_snprintf(line + o, (int)sizeof(line) - o, " a%3d %5.1f/%4.1f/%4.2f %4.1fs %s %2d|", ia * 45,
                                    (double)(n.rmax * 0.001f), (double)(n.rne * 0.001f), (double)(n.rmin * 0.001f), (double)n.tof,
                                    n.worstEsc == DLZ_BREAK_L ? "BL" : n.worstEsc == DLZ_BREAK_R ? "BR" : n.worstEsc == DLZ_BEAM_L ? "ML" : "MR", n.flyouts);
                }
                log_printf(LOG_INFO, "dlztable: %s", line);
            }
    }
}

int simh_dlztable(int argc, char **argv)
{
    int i;
    if (argc >= 7 && !strcmp(argv[0], "probe")) {    /* probe TYPE IA IS ID IV IH: fly-outs around the node's Rmax */
        DlzZone n; DlzGeom g; DlzFly f; float R;
        with_defaults(run_table, NULL);
        dlz_table_node(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), &n, &g);
        {
            const Param *tp = params_find("dlz", "trace");
            DlzZone z0, zw;
            int ty = atoi(argv[1]), a = atoi(argv[2]), sp = atoi(argv[3]), el = atoi(argv[4]), v = atoi(argv[5]), h = atoi(argv[6]);
            if (el > 0) dlz_table_node(ty, a, sp, el - 1, v, h, &zw, NULL);
            else if (sp > 0) dlz_table_node(ty, a, sp - 1, el, v, h, &zw, NULL);
            else memset(&zw, 0, sizeof(zw));
            *(int *)tp->ptr = 1;
            dlz_solve(&g, &zw, &z0);
            *(int *)tp->ptr = 0;
        }
        for (R = 0.5f * n.rmax; R < 1.3f * n.rmax; R += 0.02f * n.rmax) {
            dlz_flyout(&g, R, DLZ_STRAIGHT, &f);
            log_printf(LOG_INFO, "probe: R=%.0f (%.2f rmax) hit=%d res=%s early=%d tof=%.2f min=%.0f", (double)R, (double)(R / n.rmax), f.hit,
                       missile_result_name(f.res), f.early, (double)f.tof, (double)f.minDist);
        }
        return 0;
    }
    with_defaults(run_table, NULL);
    for (i = 0; i + 1 < argc; i++)
        if (!strcmp(argv[i], "-out")) return dlz_tables_save(argv[i + 1]) ? 0 : 1;   /* tables of the CODE defaults */
    return 0;
}
