/* tests_missile.cpp - -test missile (DESIGN 12, 16.1). Portable. Owner: T12.

   -test missile [table]: runs with the CODE DEFAULTS of every param (snapshot, empty ini, restore), like the other
   suites; "table" prints only the gameplay table with the params loaded now (jet.ini / simh -ini), for tuning.
   Checks (lines "test: missile <what> ok|FAIL ..."):
     straight chase (LRM + SRM), crossing target, G limit never exceeded (and reached), terrain impact with the
     target behind the volcano (real island), seeker LOS loss (gimbal blip, loss -> memory -> unguided, reacquire),
     flare decoy rates over 2000 trials (+-0.05 of the configured p, 5 cases), maddog LRM + SRM no-lock search
     acquisition through the live weapons path, firing rules / stores / rails / cooldown / refill, determinism,
     dt consistency (fly-out 1/30 vs live 1/120), the gameplay table of DESIGN 12.7 (printed, each value in its
     range), and the timing of 12 live missiles (weapons_tick <= 500 us per tick on the VM).
   The fly-out helper here is T12's own small tuning tool (kinematic target: straight, or the escapes of DESIGN 12.4
   flown at gUse x the flight model's G limit while accelerating to FAST); T13 builds the real solver on the same
   missile_step. */
#include "tests.h"
#include "missile.h"
#include "weapons.h"
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
    log_printf(LOG_INFO, "test: missile terrain loaded=%d path=%s", s_terOk, path);
}

/* ================================================================ fly-out helper (kinematic target) */
enum { ESC_NONE, ESC_BREAK_L, ESC_BREAK_R, ESC_BEAM_L, ESC_BEAM_R, ESC_COUNT };
static const char *const s_escName[ESC_COUNT] = { "straight", "break_left", "break_right", "beam_left", "beam_right" };

typedef struct Fly {
    int type;
    float alt;                                      /* both aircraft (m) */
    float shooterV, tgtV;                           /* m/s */
    float aspectDeg;                                /* 0 = target flies at the shooter (head-on), 180 = away (tail) */
    float offDeg;                                   /* line of sight off the shooter's nose (deg, + right) */
    int escape;                                     /* ESC_* from launch, at gUse, accelerating to FAST */
    float gUse, accel;                              /* escape turn load (x G limit), m/s^2 towards FAST */
    float dt;
    const Terrain *ter;
    int datalink;
} Fly;
typedef struct FlyRes { int res; float t, minDist, maxRatio; int lost, missed; } FlyRes;
typedef struct Tgt { Vec3 pos; float hdg, V, turnLeft, turnSign, Vgoal; } Tgt;

static Vec3 tgt_vel(const Tgt *t) { return v3((float)sin(t->hdg) * t->V, 0.0f, (float)cos(t->hdg) * t->V); }

static void tgt_step(Tgt *t, float dt, float gUse, float accel)
{
    float n, rate, dh;
    if (t->turnLeft > 0.0f) {
        n = gUse * flight_g_limit(t->V);
        if (n < 1.01f) n = 1.01f;
        rate = GRAV * (float)sqrt(n * n - 1.0f) / t->V;
        dh = rate * dt;
        if (dh > t->turnLeft) dh = t->turnLeft;
        t->hdg += t->turnSign * dh;
        t->turnLeft -= dh;
    }
    if (t->V < t->Vgoal) { t->V += accel * dt; if (t->V > t->Vgoal) t->V = t->Vgoal; }
    t->pos = v3_madd(t->pos, tgt_vel(t), dt);
}

static void fly_default(Fly *c, int type, float aspectDeg, int escape)
{
    memset(c, 0, sizeof(*c));
    c->type = type;
    c->alt = 3000.0f;
    c->shooterV = flight_speed_target(THR_NORMAL, c->alt);
    c->tgtV = c->shooterV;
    c->aspectDeg = aspectDeg;
    c->escape = escape;
    c->gUse = 0.80f;                                /* DESIGN 13.3 Normal */
    c->accel = 17.0f;                               /* ~ flight model NORMAL -> FAST (T05 card: 13.9 s) */
    c->dt = 1.0f / 30.0f;
    c->datalink = 1;
}

static int flyout(const Fly *c, float R, FlyRes *fr)
{
    Missile m;
    TrackView tv;
    Tgt tg;
    const MissileParams *mp = missile_params_of(c->type);
    float los = E_DEG2RAD(c->offDeg), t = 0.0f, ratio;
    Vec3 sp = v3(0.0f, c->alt, 0.0f);
    memset(&tg, 0, sizeof(tg));
    tg.pos = v3_madd(sp, v3((float)sin(los), 0.0f, (float)cos(los)), R);
    tg.hdg = los + E_PI - E_DEG2RAD(c->aspectDeg);
    tg.V = c->tgtV;
    tg.Vgoal = c->tgtV;
    if (c->escape != ESC_NONE) {
        tg.turnLeft = (c->escape == ESC_BREAK_L || c->escape == ESC_BREAK_R) ? E_PI : 0.5f * E_PI;
        tg.turnSign = (c->escape == ESC_BREAK_L || c->escape == ESC_BEAM_L) ? -1.0f : 1.0f;
        tg.Vgoal = flight_speed_target(THR_FAST, c->alt);
    }
    memset(&tv, 0, sizeof(tv));
    tv.valid = 1;
    tv.pos = tg.pos;
    tv.vel = tgt_vel(&tg);
    tv.datalink = c->datalink;
    tv.ab = c->escape != ESC_NONE ? 1.0f : 0.0f;
    tv.id = 1;
    missile_launch(&m, c->type, 0, 1, sp, v3(0.0f, 0.0f, 1.0f), c->shooterV, &tv, MF_GUIDED, 0.0f);
    memset(fr, 0, sizeof(*fr));
    while (m.phase != MPH_DEAD && t < 150.0f) {
        tgt_step(&tg, c->dt, c->gUse, c->accel);
        tv.pos = tg.pos;
        tv.vel = tgt_vel(&tg);
        missile_step(&m, mp, &tv, c->ter, c->dt);
        if (m.aLimit > 0.0f) { ratio = m.aLat / m.aLimit; if (ratio > fr->maxRatio) fr->maxRatio = ratio; }
        if (m.phase == MPH_LOST) fr->lost = 1;
        t += c->dt;
        if (m.flags & MF_MISSED) { fr->missed = 1; break; }
    }
    fr->res = m.result;
    fr->t = t;
    fr->minDist = m.minDist;
    return m.result == MRES_HIT;
}

static float s_worstRatio;                          /* over every fly-out of the run (G limit check) */
static int s_flyouts;

static int fly_hit(const Fly *c, float R)
{
    FlyRes fr;
    int h = flyout(c, R, &fr);
    if (fr.maxRatio > s_worstRatio) s_worstRatio = fr.maxRatio;
    s_flyouts++;
    return h;
}

/* largest hitting range (hits assumed between Rmin and Rmax); 0 = never hits */
static float find_rmax(const Fly *c)
{
    float R = 300.0f, lastHit = 0.0f, firstMiss = 0.0f, lo, hi, mid;
    int i;
    for (; R < 90000.0f; R *= 1.2f) {
        if (fly_hit(c, R)) lastHit = R;
        else if (lastHit > 0.0f) { firstMiss = R; break; }
    }
    if (lastHit <= 0.0f) return 0.0f;
    if (firstMiss <= 0.0f) return lastHit;
    lo = lastHit; hi = firstMiss;
    for (i = 0; i < 12 && hi - lo > 10.0f; i++) {
        mid = 0.5f * (lo + hi);
        if (fly_hit(c, mid)) lo = mid; else hi = mid;
    }
    return lo;
}

/* smallest hitting range */
static float find_rmin(const Fly *c)
{
    float R = 20.0f, prev = 0.0f, lo, hi, mid;
    int i;
    for (; R < 20000.0f; R *= 1.25f) {
        if (fly_hit(c, R)) break;
        prev = R;
    }
    if (R >= 20000.0f) return 0.0f;
    lo = prev; hi = R;
    for (i = 0; i < 12 && hi - lo > 5.0f; i++) {
        mid = 0.5f * (lo + hi);
        if (fly_hit(c, mid)) hi = mid; else lo = mid;
    }
    return hi;
}

static float find_rne(int type, float aspectDeg, int *worstEsc)
{
    Fly c;
    float r, best = 1.0e9f;
    int e;
    *worstEsc = ESC_NONE;
    for (e = ESC_BREAK_L; e < ESC_COUNT; e++) {
        fly_default(&c, type, aspectDeg, e);
        r = find_rmax(&c);
        if (r < best) { best = r; *worstEsc = e; }
    }
    return best;
}

/* ================================================================ gameplay table (DESIGN 12.7) */
typedef struct GRow { const char *name; int type, kind; float aspect, lo, hi; } GRow;   /* kind 0 Rmax 1 Rne 2 Rmin */
static const GRow s_rows[] = {
    { "lrm_head_rmax", MSL_LRM, 0, 0.0f, 16000.0f, 22000.0f },
    { "lrm_tail_rmax", MSL_LRM, 0, 180.0f, 5000.0f, 8000.0f },
    { "lrm_head_rne", MSL_LRM, 1, 0.0f, 6000.0f, 9000.0f },
    { "lrm_head_rmin", MSL_LRM, 2, 0.0f, 600.0f, 1200.0f },
    { "srm_head_rmax", MSL_SRM, 0, 0.0f, 5000.0f, 7000.0f },
    { "srm_tail_rmax", MSL_SRM, 0, 180.0f, 2500.0f, 3500.0f },
    { "srm_head_rne", MSL_SRM, 1, 0.0f, 3500.0f, 5500.0f },   /* T12 deviation from 1.8-3 km, see HANDOFF T12 */
    { "srm_head_rmin", MSL_SRM, 2, 0.0f, 300.0f, 500.0f },
    { "lrm_beam_rmax", MSL_LRM, 0, 90.0f, 8000.0f, 14000.0f },    /* T12 ranges (not in 12.7): crossing shots work */
    { "srm_beam_rmax", MSL_SRM, 0, 90.0f, 2000.0f, 4000.0f },
    /* T21a: the player's LRM ([lrm_player], user 2026-11-14: ~2x the distance of the enemy LRM above) */
    { "lrmp_head_rmax", MSLK_LRM_PLAYER, 0, 0.0f, 35000.0f, 42000.0f },
    { "lrmp_tail_rmax", MSLK_LRM_PLAYER, 0, 180.0f, 13000.0f, 20000.0f },
    { "lrmp_head_rne", MSLK_LRM_PLAYER, 1, 0.0f, 11000.0f, 17000.0f },
    { "lrmp_head_rmin", MSLK_LRM_PLAYER, 2, 0.0f, 600.0f, 1200.0f },
    { "lrmp_beam_rmax", MSLK_LRM_PLAYER, 0, 90.0f, 19000.0f, 28000.0f },
};

static int gameplay_table(int check)
{
    Fly c;
    FlyRes fr;
    int i, esc, ok, fails = 0;
    float v, tof;
    log_printf(LOG_INFO, "table: DESIGN 12.7 gameplay targets, co-altitude 3000 m, both NORMAL (%.0f m/s), fly-out dt 1/30, "
               "escapes at gUse 0.80 to FAST from launch", (double)flight_speed_target(THR_NORMAL, 3000.0f));
    for (i = 0; i < E_COUNT(s_rows); i++) {
        const GRow *r = &s_rows[i];
        esc = ESC_NONE;
        fly_default(&c, r->type, r->aspect, ESC_NONE);
        if (r->kind == 0) v = find_rmax(&c);
        else if (r->kind == 1) v = find_rne(r->type, r->aspect, &esc);
        else v = find_rmin(&c);
        ok = v >= r->lo && v <= r->hi;
        if (r->kind == 1) c.escape = esc;
        flyout(&c, v, &fr);
        tof = fr.t;
        flyout(&c, r->kind == 2 ? v * 0.97f : v * 1.03f, &fr);
        log_printf(LOG_INFO, "table: %-14s %7.0f m  target %5.0f-%5.0f  %s  tof=%.1f s  beyond=%s%s%s%s", r->name, (double)v,
                   (double)r->lo, (double)r->hi, ok ? "ok " : "OUT", (double)tof, missile_result_name(fr.res),
                   fr.missed ? "/missed" : "", r->kind == 1 ? "  worst=" : "", r->kind == 1 ? s_escName[esc] : "");
        if (check) { if (!t_check(ok, r->name, "m=%.0f range=%.0f-%.0f", (double)v, (double)r->lo, (double)r->hi)) fails++; }
    }
    /* extra lines for the record (not checked): other aspects and altitudes */
    fly_default(&c, MSL_LRM, 180.0f, ESC_NONE);
    log_printf(LOG_INFO, "table: lrm_tail_rne   %7.0f m  (info)", (double)find_rne(MSL_LRM, 180.0f, &esc));
    fly_default(&c, MSL_LRM, 0.0f, ESC_NONE);
    c.alt = 8000.0f; c.shooterV = c.tgtV = flight_speed_target(THR_NORMAL, c.alt);
    log_printf(LOG_INFO, "table: lrm_head_rmax_8km %7.0f m  (info)", (double)find_rmax(&c));
    c.alt = 1000.0f; c.shooterV = c.tgtV = flight_speed_target(THR_NORMAL, c.alt);
    log_printf(LOG_INFO, "table: lrm_head_rmax_1km %7.0f m  (info)", (double)find_rmax(&c));
    fly_default(&c, MSL_SRM, 180.0f, ESC_NONE);
    log_printf(LOG_INFO, "table: srm_tail_rmin  %7.0f m  (info)", (double)find_rmin(&c));
    fly_default(&c, MSL_LRM, 180.0f, ESC_NONE);
    log_printf(LOG_INFO, "table: lrm_tail_rmin  %7.0f m  (info)", (double)find_rmin(&c));
    log_printf(LOG_INFO, "table: lrmp_tail_rne  %7.0f m  (info)", (double)find_rne(MSLK_LRM_PLAYER, 180.0f, &esc));
    fly_default(&c, MSLK_LRM_PLAYER, 0.0f, ESC_NONE);
    c.alt = 8000.0f; c.shooterV = c.tgtV = flight_speed_target(THR_NORMAL, c.alt);
    log_printf(LOG_INFO, "table: lrmp_head_rmax_8km %7.0f m  (info)", (double)find_rmax(&c));
    c.alt = 1000.0f; c.shooterV = c.tgtV = flight_speed_target(THR_NORMAL, c.alt);
    log_printf(LOG_INFO, "table: lrmp_head_rmax_1km %7.0f m  (info)", (double)find_rmax(&c));
    fly_default(&c, MSLK_LRM_PLAYER, 180.0f, ESC_NONE);
    log_printf(LOG_INFO, "table: lrmp_tail_rmin %7.0f m  (info)", (double)find_rmin(&c));
    return fails;
}

/* ================================================================ direct missile_step cases */
static void t_chase(int type, float R)
{
    Fly c;
    FlyRes fr;
    int h;
    fly_default(&c, type, 180.0f, ESC_NONE);
    c.dt = SIM_DT;
    h = flyout(&c, R, &fr);
    t_check(h, type == MSL_SRM ? "chase_srm" : "chase_lrm", "R=%.0f res=%s t=%.2f s miss=%.1f m", (double)R,
            missile_result_name(fr.res), (double)fr.t, (double)fr.minDist);
}

static void t_crossing(void)
{
    Fly c;
    FlyRes fr;
    int h;
    fly_default(&c, MSL_LRM, 90.0f, ESC_NONE);
    c.dt = SIM_DT;
    c.offDeg = 20.0f;
    h = flyout(&c, 7000.0f, &fr);
    t_check(h, "crossing_lrm", "R=7000 off=20deg beam at NORMAL: res=%s t=%.2f miss=%.1f", missile_result_name(fr.res),
            (double)fr.t, (double)fr.minDist);
    fly_default(&c, MSL_SRM, 90.0f, ESC_NONE);
    c.dt = SIM_DT;
    c.offDeg = -15.0f;
    h = flyout(&c, 1800.0f, &fr);
    t_check(h, "crossing_srm", "R=1800 off=-15deg beam: res=%s t=%.2f miss=%.1f", missile_result_name(fr.res),
            (double)fr.t, (double)fr.minDist);
}

/* hard turn: target 2 km at 60 deg off the nose, crossing: the missile must pull to its limit, never past it */
static void t_glimit(void)
{
    Fly c;
    FlyRes fr;
    s_worstRatio = 0.0f;
    fly_default(&c, MSL_SRM, 90.0f, ESC_BREAK_R);
    c.dt = SIM_DT;
    c.offDeg = 35.0f;
    flyout(&c, 1500.0f, &fr);
    t_check(fr.maxRatio > 0.999f && fr.maxRatio <= 1.0001f, "glimit_reached", "max |a|/limit=%.4f res=%s", (double)fr.maxRatio,
            missile_result_name(fr.res));
}

/* terrain: target hidden behind the volcano */
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

static void t_terrain(void)
{
    Missile m;
    TrackView tv;
    Vec3 summit, sp, tp;
    const MissileParams *mp;
    int los, steps, sawLost = 0;
    float t;
    terrain_once();
    if (!s_terOk) { t_check(0, "volcano_island", "jet.isl not loaded"); return; }
    find_summit(&summit);
    log_printf(LOG_INFO, "test: missile volcano summit=%.0f,%.0f,%.0f", (double)summit.x, (double)summit.y, (double)summit.z);
    /* A: LRM inertial launch at a target behind the cone (shooter's lock has no LOS: no datalink) */
    sp = v3(summit.x - 10000.0f, 1800.0f, summit.z);
    tp = v3(summit.x + 6000.0f, 1500.0f, summit.z);
    los = terrain_los(&s_ter, &sp, &tp);
    t_check(!los, "volcano_blocks_los", "shooter->target LOS=%d", los);
    memset(&tv, 0, sizeof(tv));
    tv.valid = 1; tv.pos = tp; tv.vel = v3(250.0f, 0.0f, 0.0f); tv.datalink = 0; tv.id = 1;   /* running away east */
    mp = missile_params_of(MSL_LRM);
    missile_launch(&m, MSL_LRM, 0, 1, sp, v3_norm(v3_sub(tp, sp)), 260.0f, &tv, MF_GUIDED, 0.0f);
    for (steps = 0, t = 0.0f; m.phase != MPH_DEAD && steps < 120 * 60; steps++, t += SIM_DT) {
        tv.pos = v3_madd(tv.pos, tv.vel, SIM_DT);
        missile_step(&m, mp, &tv, &s_ter, SIM_DT);
    }
    t_check(m.result == MRES_GROUND, "volcano_lrm_impact", "res=%s t=%.1f at %.0f,%.0f,%.0f (ground %.0f)",
            missile_result_name(m.result), (double)t, (double)m.endPos.x, (double)m.endPos.y, (double)m.endPos.z,
            (double)terrain_height(&s_ter, m.endPos.x, m.endPos.z));
    /* B: SRM tracking a target that is behind the cone from the missile: seeker LOS lost -> memory -> no hit */
    sp = v3(summit.x - 5500.0f, 1800.0f, summit.z + 300.0f);
    tp = v3(summit.x + 3500.0f, 1500.0f, summit.z);
    memset(&tv, 0, sizeof(tv));
    tv.valid = 1; tv.pos = tp; tv.vel = v3(0.0f, 0.0f, 260.0f); tv.id = 1;
    mp = missile_params_of(MSL_SRM);
    missile_launch(&m, MSL_SRM, 0, 1, sp, v3_norm(v3_sub(tp, sp)), 300.0f, &tv, MF_GUIDED, 0.0f);
    for (steps = 0; m.phase != MPH_DEAD && steps < 120 * 30; steps++) {
        tv.pos = v3_madd(tv.pos, tv.vel, SIM_DT);
        missile_step(&m, mp, &tv, &s_ter, SIM_DT);
        if (m.phase == MPH_LOST) sawLost = 1;
    }
    t_check(sawLost && m.result != MRES_HIT, "volcano_srm_seeker_lost", "lost=%d res=%s endY=%.0f ground=%.0f", sawLost,
            missile_result_name(m.result), (double)m.endPos.y, (double)terrain_height(&s_ter, m.endPos.x, m.endPos.z));
}

/* seeker gimbal: blip shorter than lostSec keeps the track; longer -> LOST, memory, UNGUIDED; reacquire in memory */
static void t_seeker(void)
{
    Missile m;
    TrackView tv, away;
    const MissileParams *mp = missile_params_of(MSL_SRM);
    int k, lostTick = -1, ungTick = -1, n;
    Vec3 sp = v3(0.0f, 3000.0f, 0.0f);
    memset(&tv, 0, sizeof(tv));
    tv.valid = 1; tv.pos = v3(0.0f, 3000.0f, 4000.0f); tv.vel = v3(0.0f, 0.0f, 200.0f); tv.id = 1;
    away = tv;
    away.pos = v3(0.0f, 3000.0f, -3000.0f);         /* behind the missile: outside any gimbal */
    /* blip 0.2 s */
    missile_launch(&m, MSL_SRM, 0, 1, sp, v3(0.0f, 0.0f, 1.0f), 260.0f, &tv, MF_GUIDED, 0.0f);
    for (k = 0; k < 60; k++) { tv.pos = v3_madd(tv.pos, tv.vel, SIM_DT); missile_step(&m, mp, &tv, NULL, SIM_DT); }
    for (k = 0; k < 24; k++) { tv.pos = v3_madd(tv.pos, tv.vel, SIM_DT); away.pos = v3_madd(away.pos, away.vel, SIM_DT);
                               missile_step(&m, mp, &away, NULL, SIM_DT); }
    for (k = 0; k < 12; k++) { tv.pos = v3_madd(tv.pos, tv.vel, SIM_DT); missile_step(&m, mp, &tv, NULL, SIM_DT); }
    t_check(m.phase == MPH_TERMINAL && (m.flags & MF_SEEN), "seeker_blip_kept", "0.2 s outside the gimbal: phase=%s",
            missile_phase_name(m.phase));
    /* loss */
    missile_launch(&m, MSL_SRM, 0, 1, sp, v3(0.0f, 0.0f, 1.0f), 260.0f, &tv, MF_GUIDED, 0.0f);
    for (k = 0; k < 60; k++) { tv.pos = v3_madd(tv.pos, tv.vel, SIM_DT); missile_step(&m, mp, &tv, NULL, SIM_DT); }
    for (k = 0; k < 400 && m.phase != MPH_DEAD; k++) {
        away.pos = v3_madd(away.pos, away.vel, SIM_DT);
        missile_step(&m, mp, &away, NULL, SIM_DT);
        if (lostTick < 0 && m.phase == MPH_LOST) lostTick = k + 1;
        if (ungTick < 0 && (m.flags & MF_UNGUIDED)) ungTick = k + 1;
    }
    n = (int)(mp->lostSec * SIM_HZ + 0.5f);
    t_check(lostTick >= n && lostTick <= n + 2, "seeker_lost_time", "lost after %d ticks (lostSec %.2f = %d)", lostTick,
            (double)mp->lostSec, n);
    n = (int)(mp->memSec * SIM_HZ + 0.5f);
    t_check(ungTick > lostTick && ungTick - lostTick >= n && ungTick - lostTick <= n + 2, "seeker_memory_then_unguided",
            "unguided %d ticks after lost (memSec %.2f = %d)", ungTick - lostTick, (double)mp->memSec, n);
    /* reacquire during memory */
    missile_launch(&m, MSL_SRM, 0, 1, sp, v3(0.0f, 0.0f, 1.0f), 260.0f, &tv, MF_GUIDED, 0.0f);
    tv.pos = v3(0.0f, 3000.0f, 4000.0f);
    for (k = 0; k < 60; k++) { tv.pos = v3_madd(tv.pos, tv.vel, SIM_DT); missile_step(&m, mp, &tv, NULL, SIM_DT); }
    away.pos = v3(0.0f, 3000.0f, -3000.0f);
    for (k = 0; k < 60; k++) { tv.pos = v3_madd(tv.pos, tv.vel, SIM_DT); away.pos = v3_madd(away.pos, away.vel, SIM_DT);
                               missile_step(&m, mp, &away, NULL, SIM_DT); }
    lostTick = m.phase == MPH_LOST;
    for (k = 0; k < 30; k++) { tv.pos = v3_madd(tv.pos, tv.vel, SIM_DT); missile_step(&m, mp, &tv, NULL, SIM_DT); }
    t_check(lostTick && m.phase == MPH_TERMINAL, "seeker_reacquire_in_memory", "was lost=%d now=%s", lostTick,
            missile_phase_name(m.phase));
}

/* HUD estimates: ACT at launch+1 s vs the real time to ACTIVE, TTG at ACTIVE vs the real time to impact (LRM head-on
   15 km, straight target: the estimate uses the current closing speed, so +-25 % is the honest claim) */
static void t_estimates(void)
{
    Missile m;
    TrackView tv;
    const MissileParams *mp = missile_params_of(MSL_LRM);
    float act1 = -1.0f, ttg = -1.0f, ttgA = -1.0f, tAct = -1.0f, t = 0.0f, a, tt;
    int k;
    memset(&tv, 0, sizeof(tv));
    tv.valid = 1; tv.pos = v3(0.0f, 3000.0f, 15000.0f); tv.vel = v3(0.0f, 0.0f, -262.0f); tv.datalink = 1; tv.id = 1;
    missile_launch(&m, MSL_LRM, 0, 1, v3(0.0f, 3000.0f, 0.0f), v3(0.0f, 0.0f, 1.0f), 262.0f, &tv, MF_GUIDED, 0.0f);
    for (k = 0; k < 120 * 60 && m.phase != MPH_DEAD; k++) {
        tv.pos = v3_madd(tv.pos, tv.vel, SIM_DT);
        missile_step(&m, mp, &tv, NULL, SIM_DT);
        t += SIM_DT;
        missile_estimates(&m, mp, &a, &tt);
        if (k == 119) { act1 = a; ttg = tt; }
        if (tAct < 0.0f && m.phase == MPH_ACTIVE) { tAct = t; ttgA = tt; }
    }
    t_check(m.result == MRES_HIT && act1 > 0.0f && tAct > 0.0f && (float)fabs(act1 - (tAct - 1.0f)) <= 0.25f * (tAct - 1.0f),
            "estimate_act", "ACT at 1 s=%.1f real=%.1f (ttg then %.1f, impact at %.1f)", (double)act1, (double)(tAct - 1.0f),
            (double)ttg, (double)t);
    t_check(ttgA > 0.0f && (float)fabs(ttgA - (t - tAct)) <= 0.25f * (t - tAct), "estimate_ttg", "TTG at ACTIVE=%.1f real=%.1f",
            (double)ttgA, (double)(t - tAct));
}

static void t_determinism(void)
{
    Fly c;
    FlyRes a, b;
    fly_default(&c, MSL_LRM, 30.0f, ESC_BREAK_L);
    c.dt = SIM_DT;
    flyout(&c, 9000.0f, &a);
    flyout(&c, 9000.0f, &b);
    t_check(a.res == b.res && a.t == b.t && a.minDist == b.minDist, "determinism", "res=%s t=%.4f miss=%.4f",
            missile_result_name(a.res), (double)a.t, (double)a.minDist);
}

static void t_dt_consistency(void)
{
    Fly c;
    float r30, r120;
    fly_default(&c, MSL_LRM, 0.0f, ESC_NONE);
    r30 = find_rmax(&c);
    c.dt = SIM_DT;
    r120 = find_rmax(&c);
    t_check(r30 > 0.0f && (float)fabs(r30 - r120) <= 0.03f * r120, "dt_consistency_lrm", "Rmax head dt1/30=%.0f dt1/120=%.0f (%.1f %%)",
            (double)r30, (double)r120, (double)(r120 > 0.0f ? 100.0f * (r30 - r120) / r120 : 0.0f));
    fly_default(&c, MSL_SRM, 180.0f, ESC_NONE);
    r30 = find_rmax(&c);
    c.dt = SIM_DT;
    r120 = find_rmax(&c);
    t_check(r30 > 0.0f && (float)fabs(r30 - r120) <= 0.03f * r120, "dt_consistency_srm", "Rmax tail dt1/30=%.0f dt1/120=%.0f (%.1f %%)",
            (double)r30, (double)r120, (double)(r120 > 0.0f ? 100.0f * (r30 - r120) / r120 : 0.0f));
}

/* ================================================================ live path (weapons.cpp) */
static Game s_g;
static int s_testTd[MAX_AIRCRAFT], s_testLock[MAX_AIRCRAFT];

static void test_sensor(const Game *g, int obs, WpnSensor *out)
{
    out->tdId = s_testTd[obs];
    out->lock = s_testLock[obs];
    out->memPos = out->memVel = v3(0.0f, 0.0f, 0.0f);
    if (out->tdId >= 0 && out->lock == WLOCK_MEMORY) {
        out->memPos = g->ac[out->tdId].fs.pos;
        out->memVel = q_rotate(g->ac[out->tdId].fs.qPath, v3(0.0f, 0.0f, g->ac[out->tdId].fs.V));
    }
}

static void g_start(int n, int useTerrain)
{
    int i;
    memset(&s_g, 0, sizeof(s_g));
    rnd_seed(&s_g.rng, 12345u);
    s_g.terrain = useTerrain && s_terOk ? &s_ter : NULL;
    s_g.numAircraft = n;
    s_g.tick = 1;
    for (i = 0; i < MAX_AIRCRAFT; i++) {
        s_g.ac[i].id = i;
        s_g.ac[i].side = i ? SIDE_RED : SIDE_BLUE;
        s_g.ac[i].flags = i < n ? AC_ALIVE : 0;
        s_g.ac[i].hp = 100.0f;
        s_testTd[i] = -1;
        s_testLock[i] = WLOCK_NONE;
    }
    s_g.ac[0].flags |= AC_PLAYER;
    weapons_set_sensor(test_sensor);
    weapons_reset(&s_g);
}

static void g_place(int i, Vec3 pos, float hdgDeg, float V)
{
    Aircraft *a = &s_g.ac[i];
    flight_init(&a->fs, pos, E_DEG2RAD(hdgDeg), V);
    a->fs.onGround = 0;
    a->fs.gearPos = 0.0f;
    a->prev = a->fs;
    memset(&a->in, 0, sizeof(a->in));
    a->in.thr = THR_NORMAL;
}

/* one session-like tick: flight for everyone (straight), then weapons */
static void g_tick(void)
{
    int i;
    for (i = 0; i < s_g.numAircraft; i++) {
        Aircraft *a = &s_g.ac[i];
        a->prev = a->fs;
        if (a->flags & AC_ALIVE) flight_step(&a->fs, &a->in, SIM_DT);
    }
    weapons_tick(&s_g);
    for (i = 0; i < s_g.numAircraft; i++) { s_g.ac[i].in.fire = 0; s_g.ac[i].in.flare = 0; s_g.ac[i].in.wpnNext = 0; }
    s_g.tick++;
}

/* run until an event of 'type' (victim filter, -2 = any) shows up or maxTicks; returns ticks or -1 */
static int g_run_until(int type, int victim, int maxTicks, u32 *evFrom)
{
    int k;
    u32 i;
    for (k = 1; k <= maxTicks; k++) {
        g_tick();
        for (i = *evFrom; i < weapons_event_count(); i++) {
            const WpnEvent *e = weapons_event(i);
            if (e && e->type == type && (victim == -2 || e->victim == victim)) { *evFrom = i + 1; return k; }
        }
    }
    return -1;
}

static void t_maddog(void)
{
    u32 ev = 0;
    int acq, hit, k;
    const WpnState *w;
    g_start(2, 0);
    g_place(0, v3(0.0f, 3000.0f, 0.0f), 0.0f, 262.0f);
    g_place(1, v3(600.0f, 3000.0f, 9000.0f), 200.0f, 230.0f);      /* ~4 deg right of the nose, 9 km, inbound */
    weapons_select(0, MSL_LRM);
    s_g.ac[0].in.fire = 1;
    g_tick();
    w = weapons_state(0);
    t_check(w->lrm == 15 && weapons_live_count() == 1, "maddog_launch", "lrm=%d live=%d", w->lrm, weapons_live_count());
    acq = g_run_until(WPE_ACQUIRED, 1, 120 * 20, &ev);
    t_check(acq > 0 && acq >= 120 * 1.5f - 13, "maddog_acquires", "acquired after %.2f s (maddogSec %.1f)",
            (double)(acq / (float)SIM_HZ), (double)missile_params_of(MSL_LRM)->maddogSec);
    hit = g_run_until(WPE_HIT, 1, 120 * 40, &ev);
    t_check(hit > 0, "maddog_hits", "hit %.2f s after acquisition", (double)(hit / (float)SIM_HZ));
    /* SRM without lock: target outside the 3 deg circle, inside the search cone */
    g_start(2, 0);
    g_place(0, v3(0.0f, 3000.0f, 0.0f), 0.0f, 262.0f);
    g_place(1, v3(160.0f, 3000.0f, 1500.0f), 10.0f, 230.0f);       /* ~6 deg right, 1.5 km, flying away */
    weapons_select(0, MSL_SRM);
    for (k = 0; k < 13; k++) g_tick();                              /* SRM lock refresh has run */
    w = weapons_state(0);
    t_check(w->srmLock < 0 && w->srmNear, "srm_no_lock_outside_circle", "srmLock=%d near=%d", w->srmLock, w->srmNear);
    s_g.ac[0].in.fire = 1;
    ev = weapons_event_count();
    acq = g_run_until(WPE_ACQUIRED, 1, 120 * 3, &ev);
    t_check(acq > 0, "srm_search_acquires", "acquired after %.2f s", (double)(acq / (float)SIM_HZ));
    hit = g_run_until(WPE_HIT, 1, 120 * 15, &ev);
    t_check(hit > 0, "srm_search_hits", "hit %.2f s later", (double)(hit / (float)SIM_HZ));
}

static void t_fire_rules(void)
{
    const WpnState *w;
    const Missile *m;
    int s1, s2, s3, k, rails[4], slot;
    u32 ev;
    g_start(2, 0);
    g_place(0, v3(0.0f, 3000.0f, 0.0f), 0.0f, 262.0f);
    g_place(1, v3(0.0f, 3000.0f, 25000.0f), 180.0f, 262.0f);   /* beyond the player LRM's active range: MIDCOURSE holds */
    w = weapons_state(0);
    t_check(w->lrm == 16 && w->srm == 40 && w->flares == 40, "stores_player", "lrm=%d srm=%d flares=%d", w->lrm, w->srm, w->flares);
    t_check(weapons_state(1)->lrm == 2 && weapons_state(1)->srm == 2, "stores_enemy_default", "lrm=%d srm=%d",
            weapons_state(1)->lrm, weapons_state(1)->srm);
    /* guided LRM with TD in TRACK: datalink */
    s_testTd[0] = 1; s_testLock[0] = WLOCK_TRACK;
    g_tick();
    s1 = weapons_fire(&s_g, 0);
    s2 = weapons_fire(&s_g, 0);                                     /* cooldown */
    t_check(s1 >= 0 && s2 < 0, "cooldown", "first=%d second=%d", s1, s2);
    m = weapons_missile(s1);
    t_check(m && m->target == 1 && (m->flags & MF_GUIDED) && !(m->flags & MF_MADDOG), "lrm_guided_on_td", "target=%d flags=%x",
            m ? m->target : -9, m ? m->flags : 0);
    for (k = 0; k < 60; k++) g_tick();
    t_check(m->phase == MPH_MIDCOURSE && !(m->flags & MF_INERTIAL), "lrm_datalink", "phase=%s inertial=%d",
            missile_phase_name(m->phase), (m->flags & MF_INERTIAL) ? 1 : 0);
    s_testLock[0] = WLOCK_MEMORY;
    g_tick();
    t_check((m->flags & MF_INERTIAL) != 0, "lrm_inertial_on_memory", "inertial=%d", (m->flags & MF_INERTIAL) ? 1 : 0);
    /* rails alternate, cooldown in ticks */
    rails[0] = m->rail;
    for (k = 1; k < 4; k++) {
        int t = 0;
        while ((slot = weapons_fire(&s_g, 0)) < 0 && t < 200) { g_tick(); t++; }
        rails[k] = slot >= 0 ? weapons_missile(slot)->rail : -1;
        if (k == 2) t_check(t == (int)(0.4f * SIM_HZ + 0.5f), "cooldown_ticks", "waited %d ticks between launches", t);
    }
    t_check(rails[0] == 2 && rails[1] == 3 && rails[2] == 4 && rails[3] == 5, "lrm_rails_alternate", "rails %d %d %d %d",
            rails[0], rails[1], rails[2], rails[3]);
    weapons_select(0, MSL_SRM);
    for (k = 0; k < 60; k++) g_tick();
    s1 = weapons_fire(&s_g, 0);
    for (k = 0; k < 60; k++) g_tick();
    s2 = weapons_fire(&s_g, 0);
    for (k = 0; k < 60; k++) g_tick();
    s3 = weapons_fire(&s_g, 0);
    t_check(s1 >= 0 && s2 >= 0 && s3 >= 0 && weapons_missile(s1)->rail == 0 && weapons_missile(s2)->rail == 1 &&
            weapons_missile(s3)->rail == 0, "srm_rails_alternate", "slots %d %d %d", s1, s2, s3);
    /* wpnNext through the input toggles the selection */
    s_g.ac[0].in.wpnNext = 1;
    g_tick();
    t_check(weapons_state(0)->sel == MSL_LRM, "wpn_next_toggles", "sel=%d", weapons_state(0)->sel);
    /* empty */
    weapons_set_stores(0, 0, 0, 0);
    for (k = 0; k < 60; k++) g_tick();
    ev = weapons_event_count();
    s1 = weapons_fire(&s_g, 0);
    t_check(s1 < 0 && weapons_event(ev) && weapons_event(ev)->type == WPE_EMPTY, "empty_click", "slot=%d ev=%s", s1,
            weapons_event(ev) ? weapons_event_name(weapons_event(ev)->type) : "none");
    /* refill when the aircraft comes back alive */
    s_g.ac[0].flags &= ~AC_ALIVE;
    g_tick();
    s_g.ac[0].flags |= AC_ALIVE;
    g_tick();
    w = weapons_state(0);
    t_check(w->lrm == 0 && w->srm == 0, "refill_uses_set_stores", "lrm=%d srm=%d (set_stores 0/0/0 is the refill amount)",
            w->lrm, w->srm);
    weapons_set_stores(0, 16, 40, 40);
    s_g.ac[0].flags &= ~AC_ALIVE; g_tick(); s_g.ac[0].flags |= AC_ALIVE; g_tick();
    t_check(weapons_state(0)->lrm == 16 && weapons_state(0)->flares == 40, "refill_on_respawn", "lrm=%d flares=%d",
            weapons_state(0)->lrm, weapons_state(0)->flares);
    /* target dies -> missiles homing on it self-destruct */
    s_testTd[0] = 1; s_testLock[0] = WLOCK_TRACK;
    weapons_select(0, MSL_LRM);
    for (k = 0; k < 60; k++) g_tick();
    s1 = weapons_fire(&s_g, 0);
    g_tick();
    s_g.ac[1].flags &= ~AC_ALIVE;
    ev = weapons_event_count();
    g_tick();
    t_check(s1 >= 0 && weapons_missile(s1)->phase == MPH_DEAD && weapons_event(ev) &&
            weapons_event(ev)->type == WPE_SELFDESTRUCT, "selfdestruct_on_target_death", "ev=%s",
            weapons_event(ev) ? weapons_event_name(weapons_event(ev)->type) : "none");
}

/* live guided SRM + LRM hits through weapons_tick with lock (end to end, hit event + damage) */
static void t_live_hits(void)
{
    u32 ev = 0;
    int hit, k;
    const WpnEvent *e;
    g_start(2, 0);
    g_place(0, v3(0.0f, 3000.0f, 0.0f), 0.0f, 262.0f);
    g_place(1, v3(0.0f, 3000.0f, 2000.0f), 0.0f, 240.0f);
    weapons_select(0, MSL_SRM);
    for (k = 0; k < 13; k++) g_tick();
    t_check(weapons_state(0)->srmLock == 1, "srm_lock_tone", "srmLock=%d", weapons_state(0)->srmLock);
    s_g.ac[0].in.fire = 1;
    ev = weapons_event_count();
    hit = g_run_until(WPE_HIT, 1, 120 * 15, &ev);
    e = weapons_event(ev - 1);
    t_check(hit > 0 && e && e->damage == missile_params_of(MSL_SRM)->damageEnemy, "srm_live_hit", "t=%.2f s dmg=%.0f",
            (double)(hit / (float)SIM_HZ), e ? (double)e->damage : -1.0);
    t_check(weapons_state(0)->hits[MSL_SRM] == 1 && weapons_state(0)->fired[MSL_SRM] == 1, "hit_counters", "fired=%d hits=%d",
            weapons_state(0)->fired[MSL_SRM], weapons_state(0)->hits[MSL_SRM]);
    /* enemy fires an LRM at the player: damage = damagePlayer */
    g_start(2, 0);
    g_place(0, v3(0.0f, 3000.0f, 0.0f), 0.0f, 262.0f);
    g_place(1, v3(0.0f, 3000.0f, 14000.0f), 180.0f, 262.0f);
    s_testTd[1] = 0; s_testLock[1] = WLOCK_TRACK;
    weapons_select(1, MSL_LRM);
    g_tick();
    s_g.ac[1].in.fire = 1;
    ev = weapons_event_count();
    hit = g_run_until(WPE_HIT, 0, 120 * 40, &ev);
    e = weapons_event(ev - 1);
    t_check(hit > 0 && e && e->damage == missile_params_of(MSL_LRM)->damagePlayer, "enemy_lrm_hits_player", "t=%.2f s dmg=%.0f",
            (double)(hit / (float)SIM_HZ), e ? (double)e->damage : -1.0);
}

/* flare decoy rates over 2000 trials per case (DESIGN 12.5) */
static void t_flares(void)
{
    static const struct { const char *name; int type; float tailDeg; int ai; } cs[] = {
        { "flare_srm_tail", MSL_SRM, 0.0f, 0 }, { "flare_srm_beam", MSL_SRM, 90.0f, 0 },
        { "flare_lrm_beam", MSL_LRM, 90.0f, 0 }, { "flare_lrm_tail", MSL_LRM, 0.0f, 0 },
        { "flare_srm_tail_ai", MSL_SRM, 0.0f, 1 },
    };
    int c, k, decoys, slot, trials = 2000;
    float p, rate, base;
    u32 i0;
    Missile *m;
    const Aircraft *a;
    Vec3 fwd, d;
    for (c = 0; c < E_COUNT(cs); c++) {
        g_start(2, 0);
        /* dispenser = aircraft 'disp', shooter = the other one */
        {
            int disp = cs[c].ai ? 1 : 0, sh = cs[c].ai ? 0 : 1;
            g_place(disp, v3(0.0f, 3000.0f, 0.0f), 0.0f, 262.0f);
            g_place(sh, v3(0.0f, 3000.0f, -20000.0f), 0.0f, 262.0f);
            weapons_set_stores(disp, 0, 0, 1000000);
            weapons_set_stores(sh, 10, 10, 0);
            s_testTd[sh] = disp; s_testLock[sh] = WLOCK_TRACK;
            weapons_select(sh, cs[c].type);
            g_tick();
            slot = weapons_fire(&s_g, sh);
            if (slot < 0) { t_check(0, cs[c].name, "no launch"); continue; }
            m = (Missile *)weapons_missile(slot);
            a = &s_g.ac[disp];
            fwd = q_rotate(a->fs.qPath, v3(0.0f, 0.0f, 1.0f));
            d = v3((float)sin(E_DEG2RAD(180.0f + cs[c].tailDeg)), 0.0f, (float)cos(E_DEG2RAD(180.0f + cs[c].tailDeg)));
            decoys = 0;
            for (k = 0; k < trials; k++) {
                m->phase = MPH_TERMINAL;
                m->flags &= ~(MF_DECOYED | MF_UNGUIDED);
                m->target = disp;
                m->age = 5.0f;
                m->rollAge = -100.0f;
                m->pos = v3_madd(a->fs.pos, d, 1500.0f);
                m->dir = v3_scale(d, -1.0f);
                i0 = weapons_event_count();
                weapons_flares(&s_g, disp);
                if (m->flags & MF_DECOYED) decoys++;
                (void)i0;
            }
            base = cs[c].type == MSL_SRM ? 0.55f : 0.30f;
            {
                const Param *pp = params_find("weapons", cs[c].type == MSL_SRM ? "pFlareSrm" : "pFlareLrm");
                const Param *pt = params_find("weapons", "pTailBonus");
                const Param *pc = params_find("weapons", "pCap");
                const Param *pa = params_find("weapons", "aiFlareFactor");
                float tail = -v3_dot(fwd, d);
                if (pp) base = *(float *)pp->ptr;
                tail = E_CLAMP(tail, 0.0f, 1.0f);
                p = base + (pt ? *(float *)pt->ptr : 0.15f) * tail;
                if (pc && p > *(float *)pc->ptr) p = *(float *)pc->ptr;
                if (cs[c].ai) p *= pa ? *(float *)pa->ptr : 0.25f;
            }
            rate = (float)decoys / (float)trials;
            t_check((float)fabs(rate - p) <= 0.05f, cs[c].name, "p=%.3f measured=%.3f (%+.1f %% rel) trials=%d", (double)p,
                    (double)rate, (double)(p > 0.0f ? 100.0f * (rate - p) / p : 0.0f), trials);
        }
    }
    /* one roll per rollSec; a flare outside the gimbal does not roll */
    g_start(2, 0);
    g_place(0, v3(0.0f, 3000.0f, 0.0f), 0.0f, 262.0f);
    g_place(1, v3(0.0f, 3000.0f, -20000.0f), 0.0f, 262.0f);
    s_testTd[1] = 0; s_testLock[1] = WLOCK_TRACK;
    weapons_select(1, MSL_SRM);
    g_tick();
    slot = weapons_fire(&s_g, 1);
    m = (Missile *)weapons_missile(slot);
    m->phase = MPH_TERMINAL; m->target = 0; m->age = 5.0f; m->rollAge = -100.0f;
    m->pos = v3(0.0f, 3000.0f, -1500.0f); m->dir = v3(0.0f, 0.0f, 1.0f);
    weapons_flares(&s_g, 0);
    k = (m->flags & MF_DECOYED) ? 1 : 0;
    m->flags &= ~MF_DECOYED; m->target = 0; m->age = 5.5f;
    i0 = weapons_event_count();
    weapons_flares(&s_g, 0);
    t_check(m->rollAge == 5.0f && !(m->flags & MF_DECOYED), "flare_one_roll_per_rollsec", "first=%d second roll skipped rollAge=%.1f",
            k, (double)m->rollAge);
    m->dir = v3(0.0f, 0.0f, -1.0f);                                 /* flying away: flares outside the gimbal */
    m->age = 9.0f;
    weapons_flares(&s_g, 0);
    t_check(m->rollAge == 5.0f, "flare_outside_gimbal_no_roll", "rollAge=%.1f", (double)m->rollAge);
    (void)i0;
}

/* info for T13's budget: missile_step cost per step in fly-outs at dt 1/30, sea (no terrain) and over the island */
static void t_step_cost(void)
{
    Missile m;
    TrackView tv;
    const MissileParams *mp;
    s64 t0, t1;
    int k, n, steps, pass;
    float us[2];
    if (!prof_now_us()) { log_printf(LOG_INFO, "test: missile step_cost skipped=no_clock"); return; }
    for (pass = 0; pass < 2; pass++) {
        const Terrain *ter = pass && s_terOk ? &s_ter : NULL;
        Vec3 c = pass && s_terOk ? v3(s_ter.centreX, 0.0f, s_ter.centreZ) : v3(0.0f, 0.0f, 0.0f);
        steps = 0;
        t0 = prof_now_us();
        for (n = 0; n < 40; n++) {
            mp = missile_params_of(n & 1);
            memset(&tv, 0, sizeof(tv));
            tv.valid = 1; tv.datalink = 1; tv.id = 1;
            tv.pos = v3(c.x + 6000.0f, 1200.0f + 50.0f * (float)n, c.z - 4000.0f + 200.0f * (float)n);
            tv.vel = v3(-200.0f, 0.0f, 100.0f);
            missile_launch(&m, n & 1, 0, 1, v3(c.x - 6000.0f, 2500.0f, c.z), v3(1.0f, 0.0f, 0.0f), 262.0f, &tv, MF_GUIDED, 0.0f);
            for (k = 0; k < 600 && m.phase != MPH_DEAD; k++) {
                tv.pos = v3_madd(tv.pos, tv.vel, 1.0f / 30.0f);
                missile_step(&m, mp, &tv, ter, 1.0f / 30.0f);
                steps++;
            }
        }
        t1 = prof_now_us();
        us[pass] = steps ? (float)(t1 - t0) / (float)steps : 0.0f;
        log_printf(LOG_INFO, "test: missile step_cost %s us_per_step=%.2f steps=%d (info for T13: dt 1/30 fly-outs)",
                   pass ? "island" : "sea", (double)us[pass], steps);
    }
}

/* 12 live missiles over the island, 7 aircraft: weapons_tick cost */
static void t_timing(void)
{
    int i, k, n, fired = 0;
    s64 t0, t1, sum = 0;
    float us;
    terrain_once();
    g_start(7, 1);
    for (i = 0; i < 7; i++) {
        float ang = (float)i * 0.9f;
        g_place(i, v3(s_ter.centreX + 9000.0f * (float)cos(ang), 1500.0f + 400.0f * (float)i,
                      s_ter.centreZ + 9000.0f * (float)sin(ang)), (float)i * 51.0f, 262.0f);
        weapons_set_stores(i, 10, 10, 10);
    }
    for (k = 0; k < 2; k++)
        for (i = 0; i < 6; i++) {
            s_testTd[i] = (i + 1 + k) % 7; s_testLock[i] = WLOCK_TRACK;
            weapons_select(i, (i + k) & 1);
            if (weapons_fire(&s_g, i) >= 0) fired++;
            g_tick();
            for (n = 0; n < 50; n++) g_tick();
        }
    /* re-fire until 12 are in the air (some may have finished) */
    for (n = 0; n < 40 && weapons_live_count() < 12; n++) {
        i = n % 6;
        s_testTd[i] = 6; s_testLock[i] = WLOCK_TRACK;
        weapons_select(i, MSL_LRM);
        weapons_fire(&s_g, i);
        for (k = 0; k < 50; k++) g_tick();
    }
    n = weapons_live_count();
    if (!prof_now_us()) { t_check(n >= 12, "timing_setup", "live=%d (no clock: timing skipped)", n); return; }
    for (k = 0; k < 240; k++) {
        for (i = 0; i < s_g.numAircraft; i++) { Aircraft *a = &s_g.ac[i]; a->prev = a->fs; flight_step(&a->fs, &a->in, SIM_DT); }
        t0 = prof_now_us();
        weapons_tick(&s_g);
        t1 = prof_now_us();
        sum += t1 - t0;
        s_g.tick++;
    }
    us = (float)sum / 240.0f;
    t_check(n >= 12, "timing_live_missiles", "live at start=%d at end=%d", n, weapons_live_count());
    t_check(us <= 500.0f, "timing", "weapons_tick avg_us=%.1f for %d live missiles, 7 aircraft (gate 500 on the VM)", (double)us, n);
    (void)fired;
}

int test_missile(const char *arg)
{
    static char snap[65536];
    int n, fails;
    Cfg *empty;
    if (arg && !strncmp(arg, "probe", 5)) {       /* probe:TYPE:ASPECT:ESC:DT  sweep R, print result + miss distance */
        Fly c;
        FlyRes fr;
        int ty = 1, es = 1, dti = 30;
        float asp = 0.0f, R;
        sscanf(arg, "probe:%d:%f:%d:%d", &ty, &asp, &es, &dti);
        fly_default(&c, ty, asp, es);
        c.dt = 1.0f / (float)dti;
        for (R = 300.0f; R < 40000.0f; R *= 1.1f) {
            flyout(&c, R, &fr);
            log_printf(LOG_INFO, "probe: R=%6.0f res=%-8s missed=%d t=%5.2f miss=%7.1f lost=%d maxg=%.2f", (double)R,
                       missile_result_name(fr.res), fr.missed, (double)fr.t, (double)fr.minDist, fr.lost, (double)fr.maxRatio);
        }
        return 0;
    }
    if (arg && !strcmp(arg, "table")) {
        t_begin("missile");
        gameplay_table(0);
        log_printf(LOG_INFO, "table: fly-outs=%d worst |a|/limit=%.4f", s_flyouts, (double)s_worstRatio);
        return t_end();
    }
    t_begin("missile");
    n = params_snapshot(snap, (int)sizeof(snap));
    empty = cfg_parse("", 0);
    params_load(empty);
    cfg_free(empty);
    log_printf(LOG_INFO, "test: missile params=defaults hash=%08x", params_hash());
    terrain_once();

    t_chase(MSL_LRM, 5000.0f);
    t_chase(MSL_SRM, 2000.0f);
    t_crossing();
    t_glimit();
    t_seeker();
    t_terrain();
    t_determinism();
    t_estimates();
    t_dt_consistency();
    t_fire_rules();
    t_live_hits();
    t_maddog();
    t_flares();
    s_worstRatio = 0.0f;
    s_flyouts = 0;
    gameplay_table(1);
    t_check(s_worstRatio <= 1.0001f, "glimit_never_exceeded", "worst |a|/limit=%.5f over %d fly-outs", (double)s_worstRatio, s_flyouts);
    t_timing();
    t_step_cost();

    weapons_set_sensor(NULL);
    if (n > 0) params_restore(snap, n);
    fails = t_end();
    return fails;
}
