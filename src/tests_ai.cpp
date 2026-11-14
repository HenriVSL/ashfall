/* tests_ai.cpp - -test ai, simh aifight (DESIGN 13, 16.1, 16.3). Portable. Owner: T14.

   -test ai runs with the CODE DEFAULTS of every param (snapshot, empty ini, restore), like the other suites.
   Checks ("test: ai <what> ok|FAIL ..."):
     turn        CRUISE heading changes (90 / 180 deg): time, overshoot, altitude kept, wings level after
     ramps       a 1 v 1 fight (Hard vs the robot) over the sea: every AI axis step obeys the keyboard ramp rates
                 of [pilot], n <= G limit always and <= gUse x G limit outside AVOID
     intercept   Easy / Normal / Hard vs a straight target from 25 km: ATTACK_BVR, one guided LRM inside the truth
                 zone x lrmFireFrac, CRANK ~crankDeg off the line of sight (Normal), Easy fires further out than Hard
     defend      LRM and SRM at a CRUISE target: DEFEND reactSec after the missile is known, FAST, exactly one flare
                 burst inside flareRangeM (doctrine); defendProb rate over 40 missiles; every "react" missile found
                 the brain in DEFEND
     avoid       island: level into the volcano at 800 m, 60 deg sea dive, inverted dive, dive at the volcano: no
                 crash, AGL kept
     mask        Hard DEFEND near the volcano: masking entered, no crash
     bounds      AI beyond areaRadiusKm turns back
     fight       one robot sortie (Normal, 240 s): no crash outside masking, no stuck, no launch outside the zone,
                 DEFEND on every reacted missile; the same fight twice gives the same hash (determinism)
     timing      robot + 5 enemies over the island: think + steer per enemy per think tick (gate 200 us, VM)
     honesty     player shots inside 0.9 Rne at a Normal AI defending with the real DEFEND + flares (regression
                 sample; the full table is simh aifight shots)
   simh aifight N [seed] [diff]   N robot sorties (the robot player = the same AI with the [ai_robot] profile, vs the
                                  difficulty's waves, 3 lives, 1800 s cap): one line per fight + summary + checks.
                                  T20: flown through the mission director (waves, damage, respawns, score), so
                                  the numbers are those of a real 3-wave SORTIE.
   simh aifight shots N [seed]    player hit rates vs the AI's real DEFEND per difficulty and missile type:
                                  inside 0.9 Rne and Rne..Rmax (Rne for the difficulty's gUse, DESIGN 12.7). */
#include "tests.h"
#include "ai.h"
#include "radar.h"
#include "weapons.h"
#include "missile.h"
#include "dlz.h"
#include "world.h"
#include "takeoff.h"
#include "mission.h"
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
    log_printf(LOG_INFO, "test: ai terrain loaded=%d path=%s", s_terOk, path);
}

static float wrapf(float a)
{
    while (a > E_PI) a -= 2.0f * E_PI;
    while (a < -E_PI) a += 2.0f * E_PI;
    return a;
}
static float hd2(Vec3 a, float x, float z) { float dx = a.x - x, dz = a.z - z; return (float)sqrt(dx * dx + dz * dz); }

/* ================================================================ unit world (not g_game) */
static Game s_g;
static u32 s_aiMask;                                /* bit i: aircraft i is flown by ai_think / ai_steer */

static void test_sensor(const Game *g, int obs, WpnSensor *out)
{
    (void)g;
    out->tdId = obs == 0 ? 1 : -1;
    out->lock = obs == 0 ? WLOCK_TRACK : WLOCK_NONE;
    out->memPos = out->memVel = v3(0.0f, 0.0f, 0.0f);
}

static void w_start(const Terrain *ter, int n, int sensorTest, u32 seed)
{
    int i;
    memset(&s_g, 0, sizeof(s_g));
    rnd_seed(&s_g.rng, seed);
    s_g.seed = seed;
    s_g.terrain = (Terrain *)ter;
    s_g.numAircraft = n;
    s_g.tick = 1;
    for (i = 0; i < MAX_AIRCRAFT; i++) {
        s_g.ac[i].id = i;
        s_g.ac[i].side = i ? SIDE_RED : SIDE_BLUE;
        s_g.ac[i].flags = i < n ? AC_ALIVE : 0;
        s_g.ac[i].livery = i ? 1 : 0;
        s_g.ac[i].hp = 100.0f;
    }
    s_g.ac[0].flags |= AC_PLAYER;
    s_aiMask = 0;
    weapons_set_sensor(sensorTest ? test_sensor : NULL);
    world_reset(&s_g);
    radar_reset(&s_g);
    weapons_reset(&s_g);
    dlz_reset(&s_g);                                /* AI tables loaded as at a session start (not on the first lookup) */
    ai_reset(&s_g);
}

static void w_place(int i, Vec3 pos, float hdg, float gam, float bank, float V, int thr)
{
    Aircraft *a = &s_g.ac[i];
    flight_init(&a->fs, pos, hdg, V);
    flight_set_path(&a->fs, hdg, gam, bank);
    a->fs.onGround = 0;
    a->fs.gearPos = 0.0f;
    a->fs.alpha = 0.0f;
    a->fs.thr = thr;
    a->fs.ab = thr == THR_FAST ? 1.0f : 0.0f;
    a->prev = a->fs;
    memset(&a->in, 0, sizeof(a->in));
    a->in.thr = thr;
    world_reset_aircraft(i);
}

/* one tick in the DESIGN 2.6 order; plain aircraft keep their PilotInput (pulses cleared) */
static void w_tick(void)
{
    int i;
    for (i = 0; i < s_g.numAircraft; i++) {
        Aircraft *a = &s_g.ac[i];
        if (!(a->flags & AC_ALIVE)) continue;
        if (s_aiMask & (1u << i)) {
            if (((s_g.tick + (u32)i * 5u) % 12u) == 0) ai_think(&s_g, i);
            ai_steer(&s_g, i, &a->in);
        } else {
            a->in.fire = a->in.flare = a->in.wpnNext = 0;
        }
    }
    for (i = 0; i < s_g.numAircraft; i++) {
        Aircraft *a = &s_g.ac[i];
        a->prev = a->fs;
        if (a->flags & AC_ALIVE) flight_step(&a->fs, &a->in, SIM_DT);
    }
    world_check(&s_g);
    weapons_tick(&s_g);
    radar_tick(&s_g);
    s_g.tick++;
}

static void w_preroll(void)                         /* sensors + SRM lock with the aircraft frozen */
{
    int k;
    for (k = 0; k < 13; k++) { weapons_tick(&s_g); s_g.tick++; }
}

static int summit(Vec3 *out)
{
    float x, z, h, best = -1e9f, bx = 0.0f, bz = 0.0f;
    if (!s_terOk) return 0;
    for (z = s_ter.centreZ - s_ter.radius; z <= s_ter.centreZ + s_ter.radius; z += 400.0f)
        for (x = s_ter.centreX - s_ter.radius; x <= s_ter.centreX + s_ter.radius; x += 400.0f) {
            h = terrain_height(&s_ter, x, z);
            if (h > best) { best = h; bx = x; bz = z; }
        }
    *out = v3(bx, best, bz);
    return 1;
}

/* ================================================================ 1. turns */
static void t_turn(void)
{
    static const float goals[2] = { 90.0f, 270.0f };
    int k, n, reached;
    float tReach, over, altDev, bank, h, err, maxErrAfter;
    w_start(NULL, 2, 0, 11u);
    w_place(0, v3(0.0f, 5000.0f, -80000.0f), 0.0f, 0.0f, 0.0f, 260.0f, THR_NORMAL);   /* dummy far away */
    w_place(1, v3(0.0f, 5000.0f, 0.0f), 0.0f, 0.0f, 0.0f, flight_speed_target(THR_NORMAL, 5000.0f), THR_NORMAL);
    ai_spawn(&s_g, 1, DIFF_NORMAL, 0, v3(0.0f, 0.0f, 0.0f));
    ai_set_cruise(&s_g, 1);
    s_aiMask = 2;
    for (k = 0; k < 2; k++) {
        ai_set_cruise_goal(1, E_DEG2RAD(goals[k]), 5000.0f);
        reached = 0; tReach = -1.0f; over = 0.0f; altDev = 0.0f; maxErrAfter = 0.0f;
        for (n = 0; n < 30 * SIM_HZ; n++) {
            w_tick();
            h = s_g.ac[1].fs.heading;
            err = E_RAD2DEG(wrapf(h - E_DEG2RAD(goals[k])));
            altDev = E_MAXF(altDev, (float)fabs(s_g.ac[1].fs.pos.y - 5000.0f));
            if (!reached && (float)fabs(err) < 5.0f) { reached = 1; tReach = (float)n * SIM_DT; }
            if (reached) {
                over = E_MAXF(over, k == 0 ? err : -err);
                if ((float)n * SIM_DT > tReach + 5.0f) maxErrAfter = E_MAXF(maxErrAfter, (float)fabs(err));
            }
        }
        bank = E_RAD2DEG(s_g.ac[1].fs.bank);
        t_check(reached && tReach < (k == 0 ? 12.0f : 20.0f) && over < 10.0f && altDev < 400.0f && (float)fabs(bank) < 5.0f &&
                maxErrAfter < 3.0f, k == 0 ? "turn_90" : "turn_180",
                "t=%.1fs overshoot=%.1fdeg alt_dev=%.0fm settled_err=%.1fdeg bank_end=%.1fdeg", (double)tReach, (double)over,
                (double)altDev, (double)maxErrAfter, (double)bank);
    }
}

/* ================================================================ 2. keyboard ramps and G limits in a fight */
static const char *const s_rampNames[9] = { "pitchFine", "pitchRateFine", "pitchRate", "pitchReturn", "pitchCross",
                                            "rollRateIn", "rollReturn", "rollCross", "rollFine" };
static float pval(const char *sec, const char *key, float def)
{
    const Param *p = params_find(sec, key);
    return p && p->type == P_FLOAT ? *(const float *)p->ptr : def;
}

static int step_ok(float x0, float x1, float fine, float rFine, float rate, float ret, float cross)
{
    float lim;
    if (x0 * x1 < 0.0f) return 0;                   /* the keyboard axis stops at 0 for a tick */
    if ((float)fabs(x1) > (float)fabs(x0)) lim = ((float)fabs(x0) < fine ? rFine : rate) * SIM_DT;
    else lim = E_MAXF(ret, cross) * SIM_DT;
    return (float)fabs(x1 - x0) <= lim + 1e-4f;
}

static void t_ramps(void)
{
    float r[9], u0[2], r0[2], nOver = 0.0f;
    int k, n, i, bad = 0, steps = 0, ids[2] = { 0, 1 };
    for (k = 0; k < 9; k++) r[k] = pval("pilot", s_rampNames[k], 0.0f);
    w_start(NULL, 2, 0, 21u);
    w_place(0, v3(0.0f, 5000.0f, 0.0f), 0.0f, 0.0f, 0.0f, flight_speed_target(THR_NORMAL, 5000.0f), THR_NORMAL);
    w_place(1, v3(1500.0f, 5500.0f, 20000.0f), E_PI, 0.0f, 0.0f, flight_speed_target(THR_NORMAL, 5500.0f), THR_NORMAL);
    ai_spawn_robot(&s_g, 0);
    ai_spawn(&s_g, 1, DIFF_HARD, 1, s_g.ac[0].fs.pos);
    s_aiMask = 3;
    for (n = 0; n < 120 * SIM_HZ; n++) {
        for (k = 0; k < 2; k++) { u0[k] = s_g.ac[ids[k]].in.up; r0[k] = s_g.ac[ids[k]].in.ur; }
        w_tick();
        for (k = 0; k < 2; k++) {
            const Aircraft *a = &s_g.ac[ids[k]];
            if (!(a->flags & AC_ALIVE)) continue;
            steps++;
            if (!step_ok(u0[k], a->in.up, r[0], r[1], r[2], r[3], r[4]) || !step_ok(r0[k], a->in.ur, r[8], r[5], r[5], r[6], r[7])) {
                if (bad < 5) log_printf(LOG_INFO, "test: ai ramp step id=%d up %.4f->%.4f ur %.4f->%.4f", ids[k], (double)u0[k],
                                        (double)a->in.up, (double)r0[k], (double)a->in.ur);
                bad++;
            }
            if (a->fs.n > a->fs.nLim + 0.01f) nOver = E_MAXF(nOver, a->fs.n - a->fs.nLim);
        }
    }
    t_check(bad == 0 && steps > 10000, "ramps", "axis steps=%d outside the keyboard ramps=%d (pitch %.2f/%.1f/%.1f ret %.1f cross %.1f, roll %.1f/%.1f/%.1f)",
            steps, bad, (double)r[0], (double)r[1], (double)r[2], (double)r[3], (double)r[4], (double)r[5], (double)r[6], (double)r[7]);
    t_check(nOver <= 0.0f, "g_limit", "n above the G limit by %.3f g", (double)nOver);
    for (i = 0; i < 2; i++) {
        const AiStats *st = ai_stats(i);
        float gu = i == 0 ? ai_profile_robot()->gUse : ai_profile(DIFF_HARD)->gUse;
        t_check(st->maxNRatio <= gu + 0.03f && st->maxNRatioAvoid <= 1.001f, i == 0 ? "g_use_robot" : "g_use_hard",
                "max n/nLim %.3f outside AVOID (gUse %.2f), %.3f in AVOID; fired lrm=%d srm=%d", (double)st->maxNRatio, (double)gu,
                (double)st->maxNRatioAvoid, st->fired[0], st->fired[1]);
    }
}

/* ================================================================ 3. intercept + BVR shot */
typedef struct BvrRes { int fired, guided; float R, rmax, rmin, crankMax, tFire; int sawAttack; } BvrRes;
static void bvr_run(int diff, BvrRes *o)
{
    u32 ev;
    int n, sawFire = 0;
    float los, off;
    memset(o, 0, sizeof(*o));
    w_start(NULL, 2, 0, 31u);
    w_place(0, v3(0.0f, 4000.0f, 0.0f), E_DEG2RAD(90.0f), 0.0f, 0.0f, flight_speed_target(THR_NORMAL, 4000.0f), THR_NORMAL);
    w_place(1, v3(25000.0f, 5000.0f, 2000.0f), E_DEG2RAD(270.0f), 0.0f, 0.0f, flight_speed_target(THR_NORMAL, 5000.0f), THR_NORMAL);
    ai_spawn(&s_g, 1, diff, 1, s_g.ac[0].fs.pos);
    s_aiMask = 2;
    ev = weapons_event_count();
    for (n = 0; n < 90 * SIM_HZ && s_g.ac[0].flags & AC_ALIVE; n++) {
        w_tick();
        if (ai_state(1) == AIS_ATTACK_BVR) o->sawAttack = 1;
        while (ev < weapons_event_count()) {
            const WpnEvent *e = weapons_event(ev++);
            DlzZone z;
            if (!e || e->type != WPE_LAUNCH || e->owner != 1 || o->fired) continue;
            o->fired = 1;
            o->guided = weapons_missile(e->slot) && (weapons_missile(e->slot)->flags & MF_GUIDED);
            o->R = v3_len(v3_sub(s_g.ac[0].fs.pos, s_g.ac[1].fs.pos));
            if (dlz_ai_zone_ac(&s_g, 1, 0, MSL_LRM, &z)) { o->rmax = z.rmax; o->rmin = z.rmin; }
            o->tFire = (float)n * SIM_DT;
            sawFire = n;
        }
        if (sawFire && ai_state(1) == AIS_ATTACK_BVR && n > sawFire + 3 * SIM_HZ) {
            los = (float)atan2(s_g.ac[0].fs.pos.x - s_g.ac[1].fs.pos.x, s_g.ac[0].fs.pos.z - s_g.ac[1].fs.pos.z);
            off = (float)fabs(E_RAD2DEG(wrapf(s_g.ac[1].fs.heading - los)));
            o->crankMax = E_MAXF(o->crankMax, off);
        }
        if (sawFire && n > sawFire + 20 * SIM_HZ) break;
    }
}

static void t_intercept(void)
{
    BvrRes r[3];
    int d;
    static const char *const nm[3] = { "intercept_easy", "intercept_normal", "intercept_hard" };
    for (d = 0; d < 3; d++) {
        const AiProfile *p = ai_profile(d);
        bvr_run(d, &r[d]);
        t_check(r[d].sawAttack && r[d].fired && r[d].guided && r[d].R <= r[d].rmax * p->lrmFireFrac * 1.03f && r[d].R > r[d].rmin &&
                ai_stats(1)->envViolations == 0, nm[d],
                "LRM fired=%d guided=%d at t=%.1fs R=%.0f truth rmax=%.0f x lrmFireFrac %.2f = %.0f rmin=%.0f crank_max=%.0fdeg",
                r[d].fired, r[d].guided, (double)r[d].tFire, (double)r[d].R, (double)r[d].rmax, (double)p->lrmFireFrac,
                (double)(r[d].rmax * p->lrmFireFrac), (double)r[d].rmin, (double)r[d].crankMax);
    }
    t_check(r[1].crankMax > pval("ai", "crankDeg", 45.0f) - 12.0f && r[1].crankMax < 70.0f, "crank",
            "Normal crank max %.0f deg off the line of sight (crankDeg %.0f, lock gimbal 70)", (double)r[1].crankMax,
            (double)pval("ai", "crankDeg", 45.0f));
    t_check(r[0].R > r[2].R + 1000.0f, "easy_far_hard_close", "LRM launch range easy %.0f normal %.0f hard %.0f m",
            (double)r[0].R, (double)r[1].R, (double)r[2].R);
}

/* ================================================================ 4. DEFEND timing, flares, defendProb */
typedef struct DefRes { float delay, burstDist; int bursts, fast, entered, leftOk, hit; } DefRes;
static AiProfile s_testProf;

static void defend_run(int type, float R, const AiProfile *p, u32 seed, float maxSec, DefRes *o)
{
    u32 ev, L;
    int n, slot, wasDef = 0, leftAt = -1, goneAt = -1;
    const Missile *m;
    memset(o, 0, sizeof(*o));
    o->delay = -1.0f;
    w_start(NULL, 2, 1, seed);
    w_place(0, v3(0.0f, 5000.0f, R), E_PI, 0.0f, 0.0f, flight_speed_target(THR_NORMAL, 5000.0f), THR_NORMAL);
    w_place(1, v3(0.0f, 5000.0f, 0.0f), 0.0f, 0.0f, 0.0f, flight_speed_target(THR_NORMAL, 5000.0f), THR_NORMAL);
    ai_spawn_profile(&s_g, 1, p, 0, v3(0.0f, 0.0f, 0.0f));
    ai_set_cruise(&s_g, 1);
    w_preroll();
    weapons_select(0, type);
    slot = weapons_fire(&s_g, 0);
    if (slot < 0) return;
    L = s_g.tick;
    s_aiMask = 2;
    ev = weapons_event_count();
    for (n = 0; n < (int)(maxSec * SIM_HZ); n++) {
        w_tick();
        m = weapons_missile(slot);
        if (ai_state(1) == AIS_DEFEND && !wasDef) {
            wasDef = 1;
            o->entered = 1;
            o->delay = (float)(int)(s_g.tick - 1 - L) * SIM_DT;
        }
        if (wasDef && ai_state(1) == AIS_DEFEND && s_g.ac[1].in.thr == THR_FAST) o->fast = 1;
        while (ev < weapons_event_count()) {
            const WpnEvent *e = weapons_event(ev++);
            if (!e) continue;
            if (e->type == WPE_FLARES && e->owner == 1) {
                o->bursts++;
                if (m) o->burstDist = v3_len(v3_sub(m->pos, s_g.ac[1].fs.pos));
            }
            if (e->type == WPE_HIT && e->victim == 1) o->hit = 1;
        }
        if (goneAt < 0 && (!m || m->phase == MPH_DEAD || (m->flags & (MF_MISSED | MF_DECOYED | MF_UNGUIDED)))) goneAt = n;
        if (wasDef && leftAt < 0 && ai_state(1) != AIS_DEFEND) leftAt = n;
        if (goneAt >= 0 && n > goneAt + SIM_HZ) break;
        if (!(s_g.ac[1].flags & AC_ALIVE)) break;
    }
    o->leftOk = !wasDef || (leftAt >= 0 && (goneAt < 0 || leftAt <= goneAt + 30));
}

static void t_defend(void)
{
    DefRes d;
    const AiProfile *hp = ai_profile(DIFF_HARD), *np = ai_profile(DIFF_NORMAL);
    int i, threats = 0, yes = 0, due = 0, cov = 0;
    float rate;
    s_testProf = *hp;
    s_testProf.flareProb = 1.0f;
    defend_run(MSL_LRM, 6000.0f, &s_testProf, 41u, 40.0f, &d);
    t_check(d.entered && d.delay >= hp->reactSec - 0.01f && d.delay <= hp->reactSec + 0.12f && d.fast && d.bursts == 1 &&
            d.burstDist <= pval("ai", "flareRangeM", 3000.0f) && d.burstDist > 2300.0f && d.leftOk, "defend_lrm",
            "DEFEND %.2f s after launch (reactSec %.2f), FAST=%d, bursts=%d at %.0f m (flareRangeM %.0f), left after the missile=%d hit=%d",
            (double)d.delay, (double)hp->reactSec, d.fast, d.bursts, (double)d.burstDist, (double)pval("ai", "flareRangeM", 3000.0f),
            d.leftOk, d.hit);
    defend_run(MSL_SRM, 4000.0f, &s_testProf, 42u, 15.0f, &d);
    t_check(d.entered && d.delay >= hp->reactSec - 0.01f && d.delay <= hp->reactSec + 0.12f && d.fast && d.bursts <= 1, "defend_srm",
            "DEFEND %.2f s after an SRM launch at 4 km (known at launch inside 5 km), FAST=%d bursts=%d hit=%d", (double)d.delay,
            d.fast, d.bursts, d.hit);
    for (i = 0; i < 40; i++) {
        defend_run(MSL_LRM, 11000.0f, np, 1000u + (u32)i * 7919u, 3.0f, &d);
        threats += ai_stats(1)->threats;
        yes += ai_stats(1)->defendYes;
        due += ai_stats(1)->defendDue;
        cov += ai_stats(1)->defendCovered;
    }
    rate = threats ? (float)yes / (float)threats : 0.0f;
    t_check(threats == 40 && rate >= np->defendProb - 0.15f && rate <= E_MINF(1.0f, np->defendProb + 0.12f) && due == yes && cov == due,
            "defend_prob", "Normal: %d missiles known, %d react (%.2f, defendProb %.2f), %d reaction times passed, %d in DEFEND",
            threats, yes, (double)rate, (double)np->defendProb, due, cov);
}

/* ================================================================ 5. terrain: AVOID + floor */
static void avoid_run(const char *name, Vec3 pos, float hdgDeg, float gamDeg, float bankDeg, float V, float cruiseAlt, int needAvoid)
{
    int n, crashed = 0;
    float minAgl = 1e9f, agl;
    w_start(&s_ter, 2, 0, 51u);
    w_place(0, v3(pos.x, 6000.0f, pos.z - 80000.0f), 0.0f, 0.0f, 0.0f, 260.0f, THR_NORMAL);
    w_place(1, pos, E_DEG2RAD(hdgDeg), E_DEG2RAD(gamDeg), E_DEG2RAD(bankDeg), V, THR_NORMAL);
    ai_spawn(&s_g, 1, DIFF_NORMAL, 0, v3(0.0f, 0.0f, 0.0f));
    ai_set_cruise(&s_g, 1);
    ai_set_cruise_goal(1, E_DEG2RAD(hdgDeg), cruiseAlt);
    s_aiMask = 2;
    for (n = 0; n < 90 * SIM_HZ; n++) {
        w_tick();
        if (!(s_g.ac[1].flags & AC_ALIVE)) { crashed = 1; break; }
        agl = world_agl(&s_g, s_g.ac[1].fs.pos);
        if (agl < minAgl) minAgl = agl;
    }
    t_check(!crashed && minAgl > 100.0f && (!needAvoid || ai_stats(1)->avoidEntries > 0), name,
            "crashed=%d min_agl=%.0f m avoid_entries=%d end_alt=%.0f", crashed, (double)minAgl, ai_stats(1)->avoidEntries,
            (double)s_g.ac[1].fs.pos.y);
}

static void t_avoid(void)
{
    Vec3 s;
    float vn = flight_speed_target(THR_NORMAL, 1000.0f);
    if (!summit(&s)) { t_check(0, "avoid", "jet.isl not loaded"); return; }
    log_printf(LOG_INFO, "test: ai summit %.0f,%.0f h=%.0f", (double)s.x, (double)s.z, (double)s.y);
    avoid_run("avoid_volcano_level", v3(s.x - 12000.0f, 900.0f, s.z), 90.0f, 0.0f, 0.0f, vn, 900.0f, 0);
    avoid_run("avoid_sea_dive60", v3(s.x, 2100.0f, s.z - 45000.0f), 180.0f, -60.0f, 0.0f, 450.0f, 100.0f, 1);
    avoid_run("avoid_inverted_dive45", v3(s.x, 1500.0f, s.z - 45000.0f), 180.0f, -45.0f, 180.0f, 400.0f, 100.0f, 0);
    avoid_run("avoid_volcano_dive30", v3(s.x - 9000.0f, 4500.0f, s.z), 90.0f, -30.0f, 0.0f, 400.0f, 500.0f, 0);
}

/* ================================================================ 6. masking + bounds */
static void t_mask(void)
{
    Vec3 s;
    int n, slot, crashed = 0, losLost = 0, res = MRES_NONE;
    const Missile *m;
    if (!summit(&s)) { t_check(0, "mask", "jet.isl not loaded"); return; }
    w_start(&s_ter, 2, 1, 61u);
    w_place(0, v3(s.x - 16000.0f, 5000.0f, s.z), E_DEG2RAD(90.0f), 0.0f, 0.0f, flight_speed_target(THR_NORMAL, 5000.0f), THR_NORMAL);
    w_place(1, v3(s.x - 2500.0f, 3600.0f, s.z + 1500.0f), E_DEG2RAD(90.0f), 0.0f, 0.0f, flight_speed_target(THR_NORMAL, 3600.0f), THR_NORMAL);
    ai_spawn(&s_g, 1, DIFF_HARD, 0, v3(0.0f, 0.0f, 0.0f));
    ai_set_cruise(&s_g, 1);
    w_preroll();
    weapons_select(0, MSL_LRM);
    slot = weapons_fire(&s_g, 0);
    s_aiMask = 2;
    for (n = 0; n < 40 * SIM_HZ && slot >= 0; n++) {
        w_tick();
        if (!(s_g.ac[1].flags & AC_ALIVE)) { crashed = world_status(1)->crashed; break; }
        m = weapons_missile(slot);
        if (m && m->phase != MPH_DEAD && !terrain_los(&s_ter, &m->pos, &s_g.ac[1].fs.pos)) losLost = 1;
        if (m) res = m->result;
        if (!m || (m->phase == MPH_DEAD && n > 2 * SIM_HZ)) break;
    }
    t_check(slot >= 0 && !crashed && ai_stats(1)->maskEntries > 0, "mask",
            "Hard DEFEND near the summit: mask_entries=%d crashed=%d LOS blocked at some point=%d missile=%s alive=%d",
            ai_stats(1)->maskEntries, crashed, losLost, missile_result_name(res), (s_g.ac[1].flags & AC_ALIVE) ? 1 : 0);
}

static void t_bounds(void)
{
    float cx, cz, d0, dmax = 0.0f, d;
    int n;
    w_start(s_terOk ? &s_ter : NULL, 2, 0, 71u);
    world_centre(&s_g, &cx, &cz);
    w_place(0, v3(cx, 6000.0f, cz), 0.0f, 0.0f, 0.0f, 260.0f, THR_NORMAL);
    w_place(1, v3(cx + 58000.0f, 5000.0f, cz), E_DEG2RAD(80.0f), 0.0f, 0.0f, flight_speed_target(THR_NORMAL, 5000.0f), THR_NORMAL);
    ai_spawn(&s_g, 1, DIFF_NORMAL, 0, v3(0.0f, 0.0f, 0.0f));
    ai_set_cruise(&s_g, 1);
    s_aiMask = 2;
    d0 = hd2(s_g.ac[1].fs.pos, cx, cz);
    for (n = 0; n < 90 * SIM_HZ; n++) {
        w_tick();
        d = hd2(s_g.ac[1].fs.pos, cx, cz);
        if (d > dmax) dmax = d;
    }
    d = hd2(s_g.ac[1].fs.pos, cx, cz);
    t_check(dmax < 64000.0f && d < d0, "bounds", "start %.1f km, max %.1f km, after 90 s %.1f km (areaRadiusKm %.0f)",
            (double)(d0 * 0.001f), (double)(dmax * 0.001f), (double)(d * 0.001f), (double)pval("ai", "areaRadiusKm", 55.0f));
}

/* ================================================================ robot sortie (simh aifight, -test ai fight) */
typedef struct FightRes {
    int result;                                     /* 0 timeout, 1 complete, 2 failed (out of lives) */
    float t;
    int waves, spawned, kills, friendly;
    int deaths, deathMsl, deathCrash, deathCrashMask, deathCollide;
    int eCrash, eCrashMask, eCollide;
    int pFired[2], pHits[2], eFired[2], eHits[2];
    float eLrmRangeSum; int eLrmRangeN;
    int eThreats, eYes, eDue, eCov, eFlareBursts, pThreats, pYes, pDue, pCov;
    int stuck, envAi, envTruth;
    float maxState[AIS_COUNT], maxNoProg;
    float misSec; int score;                        /* T20: mission clock (first wave -> end) and score */
    u32 hash;
} FightRes;

/* T20: the fight runs on the MISSION DIRECTOR (mission.h): waves (DESIGN 13.3 sizes, [mission] spawn keys, attack
   order, next wave waveDelaySec after a clear), damage (missileDamage, invulnerability), missile death -> runway
   after deathRespawnSec, lives -> MISSION FAILED, MISSION COMPLETE completeDelaySec after the last kill. The harness
   only flies the robot (A held through every takeoff, a new robot brain per life) and collects statistics from the
   event rings: no own spawns or damage any more (T17 open issue 1: no double damage). sortieWaves and lives are
   forced to 3 for comparable numbers (with_params restores them). */
static Game s_fg;

static void fight(u32 seed, int diff, float capSec, FightRes *res)
{
    Game *g = &s_fg;
    PilotKeys keys;
    u32 ev, wev;
    int resp, i, k, dir = mission_director();
    const MissionStats *ms = mission_stats();
    const Param *pw = params_find("mission", "sortieWaves"), *pl = params_find("mission", "lives");
    WorldEvent we;
    memset(res, 0, sizeof(*res));
    if (pw) *(int *)pw->ptr = 3;
    if (pl) *(int *)pl->ptr = 3;
    mission_set_director(1);
    session_start(g, s_terOk ? &s_ter : NULL, MODE_SORTIE, diff, START_AIR, seed);
    session_set_autopilot(ai_robot_hook);
    ai_spawn_robot(g, PLAYER_ID);
    resp = session_respawns();
    ev = weapons_event_count();
    wev = world_event_seq();
    while ((float)g->tick * SIM_DT < capSec) {
        memset(&keys, 0, sizeof(keys));
        if (takeoff_active(session_takeoff())) keys.fast = 1;
        session_tick(g, &keys, SES_CMD_NONE);
        if (session_respawns() != resp) { resp = session_respawns(); ai_spawn_robot(g, PLAYER_ID); }
        while (ev < weapons_event_count()) {
            const WpnEvent *e = weapons_event(ev++);
            DlzZone z;
            float R;
            if (!e) continue;
            if (e->type == WPE_LAUNCH && e->mtype >= 0 && e->mtype <= 1) {
                if (e->owner != PLAYER_ID) {
                    res->eFired[e->mtype]++;
                    if (e->mtype == MSL_LRM && e->victim >= 0) {
                        res->eLrmRangeSum += v3_len(v3_sub(g->ac[e->victim].fs.pos, g->ac[e->owner].fs.pos));
                        res->eLrmRangeN++;
                    }
                }
                if (e->victim >= 0 && (g->ac[e->victim].flags & AC_ALIVE) && dlz_ai_zone_ac(g, e->owner, e->victim, e->mtype, &z)) {
                    R = v3_len(v3_sub(g->ac[e->victim].fs.pos, g->ac[e->owner].fs.pos));
                    if (z.rmax <= 0.0f || R > z.rmax * 1.03f || R < z.rmin * 0.97f) {
                        res->envTruth++;
                        log_printf(LOG_INFO, "aifight: launch outside zone owner=%d type=%d R=%.0f rmin=%.0f rmax=%.0f", e->owner,
                                   e->mtype, (double)R, (double)z.rmin, (double)z.rmax);
                    }
                }
            } else if (e->type == WPE_HIT && e->victim == PLAYER_ID && e->owner != PLAYER_ID) {
                res->eHits[e->mtype & 1]++;         /* hits on the robot (the director applies the damage) */
            }
        }
        while (wev < world_event_seq()) {
            if (!world_event_get(wev++, &we)) continue;
            if (!(we.type & WEV_CRASH_ANY)) continue;
            if (we.ac == PLAYER_ID) {
                if (we.type == WEV_CRASH_AIR) res->deathCollide++;
                else if (ai_masking(PLAYER_ID)) res->deathCrashMask++;
                else res->deathCrash++;
                log_printf(LOG_INFO, "aifight: robot crash type=%s t=%.1f state=%s masking=%d pos=%.0f,%.0f,%.0f", world_event_name(we.type),
                           (double)(g->tick * SIM_DT), ai_state_name(ai_state(PLAYER_ID)), ai_masking(PLAYER_ID), (double)we.pos.x,
                           (double)we.pos.y, (double)we.pos.z);
            } else {
                if (we.type == WEV_CRASH_AIR) res->eCollide++;
                else if (ai_masking(we.ac)) res->eCrashMask++;
                else res->eCrash++;
                log_printf(LOG_INFO, "aifight: enemy crash id=%d type=%s t=%.1f state=%s masking=%d avoid=%d pos=%.0f,%.0f,%.0f", we.ac,
                           world_event_name(we.type), (double)(g->tick * SIM_DT), ai_state_name(ai_state(we.ac)), ai_masking(we.ac),
                           ai_avoiding(we.ac), (double)we.pos.x, (double)we.pos.y, (double)we.pos.z);
            }
        }
        if (ms->outcome != MIS_RUNNING) { res->result = ms->outcome == MIS_COMPLETE ? 1 : 2; break; }
    }
    /* the director's counters */
    res->waves = ms->wave;
    res->spawned = ms->spawned;
    res->kills = ms->kills;
    res->friendly = ms->friendlyKills;
    res->deaths = ms->deaths;
    res->deathMsl = ms->deathsMsl;
    for (k = 0; k < 2; k++) { res->pFired[k] = ms->fired[k]; res->pHits[k] = ms->hits[k]; }
    res->misSec = ms->timeSec;
    res->score = ms->score;
    mission_log_result(g, res->result == 0 ? "aifight_cap" : "aifight_end");
    mission_set_director(dir);
    if (res->result == 0)
        for (i = 1; i < g->numAircraft; i++) {
            const Aircraft *a = &g->ac[i];
            if (!(a->flags & AC_ALIVE)) continue;
            log_printf(LOG_INFO, "aifight: left id=%d state=%s R=%.0f alt=%.0f kt=%.0f lrm=%d srm=%d robot=%s robotR=%.0f robot_alive=%d", i,
                       ai_state_name(ai_state(i)), (double)v3_len(v3_sub(a->fs.pos, g->ac[0].fs.pos)), (double)a->fs.pos.y,
                       (double)(a->fs.V * MS2KT), weapons_state(i)->lrm, weapons_state(i)->srm, ai_state_name(ai_state(0)),
                       (double)g->ac[0].fs.pos.y, (g->ac[0].flags & AC_ALIVE) ? 1 : 0);
        }
    res->t = (float)g->tick * SIM_DT;
    for (i = 0; i < MAX_AIRCRAFT; i++) {
        const AiStats *st = ai_stats(i);
        res->stuck += st->stuck;
        res->envAi += st->envViolations;
        if (i == PLAYER_ID) { res->pThreats = st->threats; res->pYes = st->defendYes; res->pDue = st->defendDue; res->pCov = st->defendCovered; }
        else {
            res->eThreats += st->threats; res->eYes += st->defendYes; res->eDue += st->defendDue; res->eCov += st->defendCovered;
            res->eFlareBursts += st->flareBursts;
        }
        for (k = 0; k < AIS_COUNT; k++) if (st->maxStateSec[k] > res->maxState[k]) res->maxState[k] = st->maxStateSec[k];
        if (st->maxNoProgressSec > res->maxNoProg) res->maxNoProg = st->maxNoProgressSec;
    }
    res->hash = e_hash(&g->tick, (int)sizeof(g->tick), E_HASH_INIT);
    for (i = 0; i < g->numAircraft; i++) {
        res->hash = e_hash(&g->ac[i].fs.pos, (int)sizeof(Vec3), res->hash);
        res->hash = e_hash(&g->ac[i].fs.V, (int)sizeof(float), res->hash);
        res->hash = e_hash(&g->ac[i].flags, (int)sizeof(int), res->hash);
    }
    res->hash = e_hash(res, (int)((const char *)&res->hash - (const char *)res), res->hash);
    session_set_autopilot(NULL);
}

static const char *const s_resName[3] = { "timeout", "complete", "failed" };

static void fight_line(int i, u32 seed, int diff, const FightRes *f)
{
    log_printf(LOG_INFO, "aifight: fight=%d seed=%u diff=%d result=%s t=%.0f mission=%.0f score=%d waves=%d kills=%d/%d deaths=%d (msl %d crash %d mask %d "
               "collide %d) enemy_crash=%d (mask %d collide %d) friendly=%d pFired=%d/%d pHits=%d/%d eFired=%d/%d eHits=%d/%d "
               "defend=%d/%d/%d/%d eFlares=%d stuck=%d env=%d/%d hash=%08x", i, seed, diff, s_resName[f->result], (double)f->t,
               (double)f->misSec, f->score, f->waves, f->kills, f->spawned, f->deaths, f->deathMsl, f->deathCrash, f->deathCrashMask, f->deathCollide, f->eCrash,
               f->eCrashMask, f->eCollide, f->friendly, f->pFired[0], f->pFired[1], f->pHits[0], f->pHits[1], f->eFired[0], f->eFired[1],
               f->eHits[0], f->eHits[1], f->eThreats, f->eYes, f->eDue, f->eCov, f->eFlareBursts, f->stuck, f->envAi, f->envTruth,
               f->hash);
}

static void t_fight(void)
{
    FightRes a, b;
    fight(5u, DIFF_NORMAL, 240.0f, &a);
    fight_line(0, 5u, DIFF_NORMAL, &a);
    fight(5u, DIFF_NORMAL, 240.0f, &b);
    t_check(a.eCrash == 0 && a.deathCrash == 0 && a.stuck == 0 && a.envAi == 0 && a.envTruth == 0 && a.eDue == a.eCov &&
            a.pDue == a.pCov && a.eFired[0] + a.eFired[1] + a.pFired[0] + a.pFired[1] > 0, "fight",
            "Normal robot sortie 240 s: %s kills=%d deaths=%d, crashes outside masking enemy=%d robot=%d, stuck=%d, outside zone=%d/%d, "
            "defend due/covered enemy %d/%d robot %d/%d, launches robot %d enemy %d", s_resName[a.result], a.kills, a.deaths, a.eCrash,
            a.deathCrash, a.stuck, a.envAi, a.envTruth, a.eDue, a.eCov, a.pDue, a.pCov, a.pFired[0] + a.pFired[1], a.eFired[0] + a.eFired[1]);
    t_check(a.hash == b.hash && a.t == b.t, "determinism", "two runs of the same sortie: hash %08x / %08x, t %.2f / %.2f", a.hash, b.hash,
            (double)a.t, (double)b.t);
}

/* ================================================================ 7. timing */
/* T18: the scenario is deterministic, so it runs twice and each think tick counts with the smaller of its two times:
   that removes the emulator's / OS's random stalls (a 6 ms think on an ordinary tick in one run, nothing in the
   other) and leaves the real per-think cost. Autoflush is off while timing: with it on, every radar event line
   (DESIGNATE / LOCK at INFO) was fflush'ed to the share inside the timed think (0.5-1 ms on the VM: the old
   "spikes" at designation thinks; the game loop never autoflushes). */
enum { TIMING_THINKS = 60 * SIM_HZ / 12 * 5 + 16 };
static float s_thinkUs[2][TIMING_THINKS];

static void timing_run(int pass, double *thinkUs, int *thinkCalls, double *steerUs, int *steerCalls, double *rawWorst)
{
    int n, i, k = 0, th;
    s64 t0, t1;
    double us;
    float cx, cz, a;
    Vec3 s;
    w_start(s_terOk ? &s_ter : NULL, 6, 0, 81u);
    if (!summit(&s)) s = v3(0.0f, 0.0f, 0.0f);
    world_centre(&s_g, &cx, &cz);
    w_place(0, v3(s.x - 6000.0f, 3500.0f, s.z), E_DEG2RAD(90.0f), 0.0f, 0.0f, flight_speed_target(THR_NORMAL, 3500.0f), THR_NORMAL);
    ai_spawn_robot(&s_g, 0);
    for (i = 1; i < 6; i++) {
        a = (float)i * 1.2f;
        w_place(i, v3(s.x + 14000.0f * (float)sin(a), 4000.0f + 400.0f * (float)i, s.z + 14000.0f * (float)cos(a)), a + E_PI, 0.0f,
                0.0f, flight_speed_target(THR_NORMAL, 4000.0f), THR_NORMAL);
        ai_spawn(&s_g, i, DIFF_NORMAL, 1, s_g.ac[0].fs.pos);
    }
    s_aiMask = 1;                                   /* robot by w_tick; enemies timed here */
    for (n = 0; n < 60 * SIM_HZ; n++) {
        for (i = 1; i < 6; i++) {
            Aircraft *ac = &s_g.ac[i];
            if (!(ac->flags & AC_ALIVE)) continue;
            th = ((s_g.tick + (u32)i * 5u) % 12u) == 0;
            t0 = prof_now_us();
            if (th) ai_think(&s_g, i);
            ai_steer(&s_g, i, &ac->in);
            t1 = prof_now_us();
            us = (double)(t1 - t0);
            if (th) {
                if (k < TIMING_THINKS) s_thinkUs[pass][k] = (float)us;
                k++;
                if (us > *rawWorst) *rawWorst = us;
                if (pass == 0) { *thinkUs += us; (*thinkCalls)++; }
            }
            else if (pass == 0) { *steerUs += us; (*steerCalls)++; }
        }
        w_tick();
    }
}

static void t_timing(void)
{
    int i, thinkCalls = 0, steerCalls = 0, over = 0, over300 = 0, worstK = 0, n;
    double thinkUs = 0.0, steerUs = 0.0, worst = 0.0, worst2 = 0.0, raw0 = 0.0, raw1 = 0.0, us;
    if (!prof_now_us()) { log_printf(LOG_INFO, "test: ai timing skipped=no_clock"); return; }
    log_set_autoflush(0);
    timing_run(0, &thinkUs, &thinkCalls, &steerUs, &steerCalls, &raw0);
    { double d0 = 0.0, d1 = 0.0; int c0 = 0, c1 = 0; timing_run(1, &d0, &c0, &d1, &c1, &raw1); }
    log_flush();
    log_set_autoflush(1);
    n = thinkCalls < TIMING_THINKS ? thinkCalls : TIMING_THINKS;
    for (i = 0; i < n; i++) {
        us = s_thinkUs[0][i] < s_thinkUs[1][i] ? s_thinkUs[0][i] : s_thinkUs[1][i];
        if (us > 200.0) { over++; log_printf(LOG_INFO, "test: ai timing slow think k=%d us=%.0f (runs %.0f / %.0f)", i, us, (double)s_thinkUs[0][i], (double)s_thinkUs[1][i]); }
        if (us > 300.0) over300++;
        if (us > worst) { worst2 = worst; worst = us; worstK = i; }
        else if (us > worst2) worst2 = us;
    }
    t_check(thinkCalls > 100 && thinkUs / thinkCalls <= 200.0 && worst <= 500.0, "timing",
            "per enemy: think tick (think + steer) avg %.1f us; worst %.0f us (think %d; 2nd %.0f; %d of %d above 200 us, %d above 300) "
            "as the smaller of two identical runs (single-run worst %.0f / %.0f: emulator/OS stalls); steer-only tick avg %.2f us; "
            "average per enemy per tick %.2f us (gate: avg 200 us, worst 500 us on the VM)",
            thinkUs / E_MAXF((float)thinkCalls, 1.0f), worst, worstK, worst2, over, n, over300, raw0, raw1,
            steerUs / E_MAXF((float)steerCalls, 1.0f), (thinkUs + steerUs) / E_MAXF((float)(thinkCalls + steerCalls), 1.0f));
}

/* ================================================================ 8. player shots vs the AI's real DEFEND */
typedef struct SGeo { int type; Vec3 sPos; float sHdg, sV, az, el, tHdg; int tThr; } SGeo;
typedef struct SRes { int hit, decoyed, bursts, defended, guided, res; } SRes;
typedef struct STally { int n, hit, decoyed, bursts, defended; } STally;
static AiProfile s_shotProf;

static void sgeo_random(SGeo *q, ERand *r, int type)
{
    float asp, side;
    memset(q, 0, sizeof(*q));
    q->type = type;
    q->sPos = v3(0.0f, rnd_range(r, 1500.0f, 9000.0f), 0.0f);
    q->sHdg = rnd_range(r, -E_PI, E_PI);
    q->az = E_DEG2RAD(rnd_range(r, -1.0f, 1.0f) * (type == MSL_SRM ? 12.0f : 25.0f));
    q->el = E_DEG2RAD(rnd_range(r, -8.0f, 8.0f));
    if (q->sPos.y < 3000.0f && q->el < 0.0f) q->el = -q->el;
    asp = rnd_range(r, 0.0f, 180.0f);
    side = rnd_f01(r) < 0.5f ? -1.0f : 1.0f;
    q->tHdg = q->sHdg + q->az + E_PI - side * E_DEG2RAD(asp);
    q->tThr = rnd_f01(r) < 0.65f ? THR_NORMAL : THR_FAST;
    q->sV = flight_speed_target(rnd_f01(r) < 0.7f ? THR_NORMAL : THR_FAST, q->sPos.y) * rnd_range(r, 0.9f, 1.0f);
}

static void sgeo_place(const SGeo *q, float R, u32 seed)
{
    float h = q->sHdg + q->az, e = q->el;
    Vec3 los = v3((float)sin(h) * (float)cos(e), (float)sin(e), (float)cos(h) * (float)cos(e)), tp = v3_madd(q->sPos, los, R);
    w_start(NULL, 2, 1, seed);
    w_place(0, q->sPos, q->sHdg, 0.0f, 0.0f, q->sV, THR_NORMAL);
    w_place(1, tp, q->tHdg, 0.0f, 0.0f, flight_speed_target(q->tThr, tp.y), q->tThr);
}

static int sgeo_zone(const SGeo *q, float gUse, DlzZone *z)
{
    DlzGeom gm;
    DlzZone warm;
    sgeo_place(q, q->type == MSL_SRM ? 3000.0f : 9000.0f, 1u);
    dlz_geom_from(&s_g, 0, 1, q->type, &gm);
    gm.gUse = gUse;
    gm.ter = NULL;
    if (!dlz_ai_zone_ac(&s_g, 0, 1, q->type, &warm)) memset(&warm, 0, sizeof(warm));
    return dlz_solve(&gm, &warm, z);
}

static void ai_shot(const SGeo *q, float R, const AiProfile *p, u32 seed, SRes *o)
{
    int slot, k, missedT = 0, maxT;
    const Missile *m;
    u32 ev;
    memset(o, 0, sizeof(*o));
    sgeo_place(q, R, seed);
    ai_spawn_profile(&s_g, 1, p, 0, v3(0.0f, 0.0f, 0.0f));
    ai_set_cruise(&s_g, 1);
    w_preroll();
    weapons_select(0, q->type);
    slot = weapons_fire(&s_g, 0);
    if (slot < 0) return;
    o->guided = (weapons_missile(slot)->flags & MF_GUIDED) ? 1 : 0;
    s_aiMask = 2;
    ev = weapons_event_count();
    maxT = (int)((missile_params_of(weapons_missile(slot)->kind)->maxSec + 2.0f) * SIM_HZ);   /* T21a: the fired kind */
    for (k = 0; k < maxT; k++) {
        w_tick();
        if (ai_state(1) == AIS_DEFEND) o->defended = 1;
        while (ev < weapons_event_count()) {
            const WpnEvent *e = weapons_event(ev++);
            if (e && e->type == WPE_FLARES && e->owner == 1) o->bursts++;
        }
        m = weapons_missile(slot);
        if (!m) break;
        if (m->flags & MF_DECOYED) o->decoyed = 1;
        if (m->phase == MPH_DEAD || (m->flags & MF_UNGUIDED)) break;
        if ((m->flags & MF_MISSED) && ++missedT > 240) break;
        if (!(s_g.ac[1].flags & AC_ALIVE)) break;
    }
    m = weapons_missile(slot);
    o->res = m ? m->result : MRES_NONE;
    o->hit = m && m->result == MRES_HIT && m->target == 1 && !(m->flags & MF_DECOYED);
}

static void stally(STally *t, const SRes *r)
{
    t->n++; t->hit += r->hit; t->decoyed += r->decoyed; t->bursts += r->bursts; t->defended += r->defended;
}

/* n geometries per (difficulty, type); band 0 inside 0.9 Rne, band 1 Rne..Rmax. Returns the Normal inside-Rne rate. */
static float run_ai_shots(int n, u32 seed, int diffLo, int diffHi, STally out[3][2][2])
{
    ERand r;
    SGeo q;
    DlzZone z;
    SRes sr;
    int d, type, i, band;
    float lo, hi, R, nr = 0.0f;
    memset(out, 0, sizeof(STally) * 12);
    rnd_seed(&r, seed);
    for (d = diffLo; d <= diffHi; d++) {
        s_shotProf = *ai_profile(d);
        for (type = 0; type < 2; type++)
            for (i = 0; i < n; i++) {
                sgeo_random(&q, &r, type);
                if (!sgeo_zone(&q, s_shotProf.gUse, &z) || z.rmax <= 0.0f || z.rne <= 0.0f) { i--; continue; }
                for (band = 0; band < 2; band++) {
                    if (band == 0) { lo = E_MAXF(1.15f * z.rmin, 0.25f * z.rne); hi = 0.9f * z.rne; }
                    else { lo = 1.02f * z.rne; hi = 0.98f * z.rmax; }
                    if (hi <= lo) continue;
                    R = rnd_range(&r, lo, hi);
                    ai_shot(&q, R, &s_shotProf, rnd_u32(&r) | 1u, &sr);
                    if (!sr.guided) continue;
                    stally(&out[d][type][band], &sr);
                    if (band == 0 && !sr.hit)
                        log_printf(LOG_INFO, "aishots: miss inside Rne diff=%d %s R=%.0f rne=%.0f rmax=%.0f decoyed=%d bursts=%d defended=%d res=%s",
                                   d, type ? "srm" : "lrm", (double)R, (double)z.rne, (double)z.rmax, sr.decoyed, sr.bursts, sr.defended,
                                   missile_result_name(sr.res));
                }
            }
    }
    if (diffLo <= DIFF_NORMAL && diffHi >= DIFF_NORMAL) {
        int nn = out[1][0][0].n + out[1][1][0].n;
        nr = nn ? (float)(out[1][0][0].hit + out[1][1][0].hit) / (float)nn : 0.0f;
    }
    return nr;
}

static void shots_table(STally t[3][2][2], int diffLo, int diffHi)
{
    static const char *const dn[3] = { "easy", "normal", "hard" };
    int d, type, band;
    for (d = diffLo; d <= diffHi; d++)
        for (type = 0; type < 2; type++)
            for (band = 0; band < 2; band++) {
                const STally *x = &t[d][type][band];
                log_printf(LOG_INFO, "aishots: %-6s %s %-12s hits %3d/%3d = %5.1f %%  decoyed %3d  flare bursts %3d  defended %3d",
                           dn[d], type ? "SRM" : "LRM", band ? "Rne..Rmax" : "inside0.9Rne", x->hit, x->n,
                           x->n ? 100.0 * x->hit / x->n : 0.0, x->decoyed, x->bursts, x->defended);
            }
}

static void t_honesty(void)
{
    STally t[3][2][2];
    float nr = run_ai_shots(10, 2026u, DIFF_NORMAL, DIFF_NORMAL, t);
    shots_table(t, DIFF_NORMAL, DIFF_NORMAL);
    t_check(nr >= 0.85f && t[1][0][0].n + t[1][1][0].n >= 16, "honesty_sample",
            "Normal AI with DEFEND + flares, inside 0.9 Rne: %.0f %% of %d (regression gate 85 %%; full table: simh aifight shots)",
            (double)(nr * 100.0f), t[1][0][0].n + t[1][1][0].n);
}

/* ================================================================ entry points */
static int with_params(void (*fn)(void *), void *ctx, int defaults)
{
    static char snap[65536];
    int n, logEv = 0;
    Cfg *empty;
    const Param *tp;
    n = params_snapshot(snap, (int)sizeof(snap));
    if (defaults) {
        empty = cfg_parse("", 0);
        params_load(empty);
        cfg_free(empty);
    }
    tp = params_find("weapons", "logEvents");
    if (tp) { logEv = *(int *)tp->ptr; *(int *)tp->ptr = 0; }   /* thousands of launches: no wpn: lines */
    fn(ctx);
    if (tp) *(int *)tp->ptr = logEv;
    weapons_set_sensor(NULL);
    session_set_autopilot(NULL);
    if (n > 0) params_restore(snap, n);
    return 0;
}

static void run_all(void *ctx)
{
    const char *arg = (const char *)ctx;
    terrain_once();
    if (!arg || !strcmp(arg, "turn")) t_turn();
    if (!arg || !strcmp(arg, "ramps")) t_ramps();
    if (!arg || !strcmp(arg, "intercept")) t_intercept();
    if (!arg || !strcmp(arg, "defend")) t_defend();
    if (!arg || !strcmp(arg, "avoid")) t_avoid();
    if (!arg || !strcmp(arg, "mask")) t_mask();
    if (!arg || !strcmp(arg, "bounds")) t_bounds();
    if (!arg || !strcmp(arg, "fight")) t_fight();
    if (!arg || !strcmp(arg, "timing")) t_timing();
    if (!arg || !strcmp(arg, "honesty")) t_honesty();
}

int test_ai(const char *arg)
{
    t_begin("ai");
    log_printf(LOG_INFO, "test: ai params=defaults");
    with_params(run_all, (void *)arg, 1);
    return t_end();
}

/* simh aifight N [seed] [diff] | simh aifight shots N [seed] */
typedef struct FightCtx { int n, diff, fails; u32 seed; } FightCtx;
enum { AIFIGHT_CAP_SEC = 1800 };                    /* T20: 900 -> 1800 s (3 waves at 45-55 km, takeoffs after deaths) */

static void run_fights(void *ctx)
{
    FightCtx *c = (FightCtx *)ctx;
    FightRes f, sum;
    int i, k, res[3] = { 0, 0, 0 }, cN = 0, cS = 0;
    float maxState[AIS_COUNT], maxNoProg = 0.0f, cT = 0.0f, cMin = 1e9f, cMax = 0.0f;
    const AiProfile *p = ai_profile(c->diff);
    terrain_once();
    memset(&sum, 0, sizeof(sum));
    memset(maxState, 0, sizeof(maxState));
    for (i = 0; i < c->n; i++) {
        u32 seed = c->seed + (u32)i * 7919u;
        fight(seed, c->diff, AIFIGHT_CAP_SEC, &f);
        fight_line(i, seed, c->diff, &f);
        res[f.result]++;
        sum.t += f.t; sum.misSec += f.misSec; sum.score += f.score;
        if (f.result == 1) { cN++; cT += f.misSec; cS += f.score; if (f.misSec < cMin) cMin = f.misSec; if (f.misSec > cMax) cMax = f.misSec; }
        sum.kills += f.kills; sum.spawned += f.spawned; sum.friendly += f.friendly;
        sum.deaths += f.deaths; sum.deathMsl += f.deathMsl; sum.deathCrash += f.deathCrash; sum.deathCrashMask += f.deathCrashMask;
        sum.deathCollide += f.deathCollide; sum.eCrash += f.eCrash; sum.eCrashMask += f.eCrashMask; sum.eCollide += f.eCollide;
        for (k = 0; k < 2; k++) { sum.pFired[k] += f.pFired[k]; sum.pHits[k] += f.pHits[k]; sum.eFired[k] += f.eFired[k]; sum.eHits[k] += f.eHits[k]; }
        sum.eLrmRangeSum += f.eLrmRangeSum; sum.eLrmRangeN += f.eLrmRangeN;
        sum.eThreats += f.eThreats; sum.eYes += f.eYes; sum.eDue += f.eDue; sum.eCov += f.eCov; sum.eFlareBursts += f.eFlareBursts;
        sum.pThreats += f.pThreats; sum.pYes += f.pYes; sum.pDue += f.pDue; sum.pCov += f.pCov;
        sum.stuck += f.stuck; sum.envAi += f.envAi; sum.envTruth += f.envTruth;
        for (k = 0; k < AIS_COUNT; k++) if (f.maxState[k] > maxState[k]) maxState[k] = f.maxState[k];
        if (f.maxNoProg > maxNoProg) maxNoProg = f.maxNoProg;
    }
    log_printf(LOG_INFO, "aifight: summary diff=%d fights=%d complete=%d failed=%d timeout=%d avg_t=%.0f s avg_mission=%.0f s avg_score=%.0f "
               "cap=%d s", c->diff, c->n, res[1], res[2], res[0], (double)(sum.t / E_MAXF((float)c->n, 1.0f)),
               (double)(sum.misSec / E_MAXF((float)c->n, 1.0f)), (double)sum.score / E_MAXF((float)c->n, 1.0f), (int)AIFIGHT_CAP_SEC);
    log_printf(LOG_INFO, "aifight: summary complete n=%d mission avg=%.0f min=%.0f max=%.0f s, score avg=%.0f (timeBonusRefSec %.0f)", cN,
               (double)(cT / E_MAXF((float)cN, 1.0f)), (double)(cN ? cMin : 0.0f), (double)cMax, (double)cS / E_MAXF((float)cN, 1.0f),
               (double)mission_time_ref());
    log_printf(LOG_INFO, "aifight: summary robot kills=%d of %d spawned, deaths=%d (missile %d, crash %d, masking crash %d, collision %d), "
               "K/D=%.2f; enemy crashes=%d (masking %d, collisions %d), friendly fire=%d", sum.kills, sum.spawned, sum.deaths, sum.deathMsl,
               sum.deathCrash, sum.deathCrashMask, sum.deathCollide, (double)sum.kills / E_MAXF((float)sum.deaths, 1.0f), sum.eCrash,
               sum.eCrashMask, sum.eCollide, sum.friendly);
    log_printf(LOG_INFO, "aifight: summary robot fired LRM %d SRM %d hits %d/%d (%.0f %%); enemies fired LRM %d SRM %d hits %d/%d (%.0f %%), "
               "avg enemy LRM launch range %.1f km", sum.pFired[0], sum.pFired[1], sum.pHits[0], sum.pHits[1],
               100.0 * (sum.pHits[0] + sum.pHits[1]) / E_MAXF((float)(sum.pFired[0] + sum.pFired[1]), 1.0f), sum.eFired[0], sum.eFired[1],
               sum.eHits[0], sum.eHits[1], 100.0 * (sum.eHits[0] + sum.eHits[1]) / E_MAXF((float)(sum.eFired[0] + sum.eFired[1]), 1.0f),
               (double)(sum.eLrmRangeSum * 0.001f / E_MAXF((float)sum.eLrmRangeN, 1.0f)));
    log_printf(LOG_INFO, "aifight: summary max state s: PATROL %.0f INTERCEPT %.0f ATTACK_BVR %.0f MERGE %.0f EXTEND %.0f DEFEND %.0f; "
               "longest without progress %.0f s", (double)maxState[AIS_PATROL], (double)maxState[AIS_INTERCEPT],
               (double)maxState[AIS_ATTACK_BVR], (double)maxState[AIS_MERGE], (double)maxState[AIS_EXTEND], (double)maxState[AIS_DEFEND],
               (double)maxNoProg);
    c->fails = 0;
    {
        int ok;
        ok = sum.eCrash == 0 && sum.deathCrash == 0;
        log_printf(LOG_INFO, "aifight: check crashes %s outside masking: enemy %d robot %d (masking: enemy %d robot %d; collisions %d/%d)",
                   ok ? "ok" : "FAIL", sum.eCrash, sum.deathCrash, sum.eCrashMask, sum.deathCrashMask, sum.eCollide, sum.deathCollide);
        if (!ok) c->fails++;
        ok = sum.stuck == 0;
        log_printf(LOG_INFO, "aifight: check stuck %s %d (watchdog %.0f s)", ok ? "ok" : "FAIL", sum.stuck, (double)maxNoProg);
        if (!ok) c->fails++;
        ok = sum.envAi == 0 && sum.envTruth == 0;
        log_printf(LOG_INFO, "aifight: check envelope %s launches outside the truth zone: %d (ai) %d (harness)", ok ? "ok" : "FAIL",
                   sum.envAi, sum.envTruth);
        if (!ok) c->fails++;
        ok = sum.eDue == sum.eCov && sum.pDue == sum.pCov &&
             (sum.eThreats < 20 || (float)sum.eYes / (float)sum.eThreats >= p->defendProb - 0.1f);
        log_printf(LOG_INFO, "aifight: check defend %s enemies: %d known missiles, %d react (%.2f, defendProb %.2f), %d due, %d in DEFEND; "
                   "robot %d/%d/%d/%d; enemy flare bursts %d", ok ? "ok" : "FAIL", sum.eThreats, sum.eYes,
                   (double)sum.eYes / E_MAXF((float)sum.eThreats, 1.0f), (double)p->defendProb, sum.eDue, sum.eCov, sum.pThreats, sum.pYes,
                   sum.pDue, sum.pCov, sum.eFlareBursts);
        if (!ok) c->fails++;
    }
}

typedef struct ShotCtx { int n; u32 seed; } ShotCtx;
static void run_shots_ctx(void *ctx)
{
    ShotCtx *c = (ShotCtx *)ctx;
    STally t[3][2][2];
    run_ai_shots(c->n, c->seed, DIFF_EASY, DIFF_HARD, t);
    shots_table(t, DIFF_EASY, DIFF_HARD);
}

int simh_aifight(int argc, char **argv)
{
    if (argc > 0 && !strcmp(argv[0], "shots")) {
        ShotCtx s;
        s.n = argc > 1 ? atoi(argv[1]) : 40;
        s.seed = argc > 2 ? (u32)atoi(argv[2]) : 2026u;
        if (s.n <= 0) s.n = 40;
        with_params(run_shots_ctx, &s, 0);
        return 0;
    } else {
        FightCtx c;
        c.n = argc > 0 ? atoi(argv[0]) : 10;
        c.seed = argc > 1 ? (u32)atoi(argv[1]) : 1u;
        c.diff = argc > 2 ? atoi(argv[2]) : DIFF_NORMAL;
        if (c.n <= 0) c.n = 10;
        if (c.diff < 0 || c.diff > 2) c.diff = DIFF_NORMAL;
        c.fails = 0;
        with_params(run_fights, &c, 0);
        return c.fails;
    }
}
