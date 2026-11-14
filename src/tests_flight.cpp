/* tests_flight.cpp - -test flight (DESIGN 4.7 feel card, 16.1) and `simh feel`. Portable. Owner: T05.

   -test flight runs with the CODE DEFAULTS of every param (it snapshots the loaded values, loads an empty ini,
   runs, restores), so a user's jet.ini edits never make the suite fail; `simh feel` / feel_card_print measures the
   card with the values that are loaded now (jet.ini or -ini FILE), so it shows what an ini change does.
   Every card line is MEASURED by running flight_step (and pilot_update for the key lines), never computed from
   the formulas. When the user accepts new feel numbers (T10), update the targets in s_card.

   Test lines: "test: flight <what> ok|FAIL ..." and "feel: ..." for the card. Timing line:
   "test: flight timing us_per_step=X steps=N" (acceptance: <= 15 us per aircraft on the VM). */
#include "tests.h"
#include "params.h"
#include "e_log.h"
#include "e_prof.h"
#include "e_file.h"
#include "e_terrain.h"
#include "replay.h"

/* ---------------------------------------------------------------- the feel card (DESIGN 4.7) */
enum {
    FC_TURN_NORMAL, FC_TURN_350, FC_TURN_150, FC_TURN_FAST, FC_ROLL90, FC_ROLL360, FC_PITCH_FULL, FC_TAP,
    FC_TO_FAST, FC_TO_350, FC_TO_150, FC_CLIMB30, FC_COUNT
};
typedef struct CardLine {
    const char *key;                                /* short name in the log line */
    const char *text;                               /* situation (as DESIGN 4.7) */
    const char *unit;
    float target, lo, hi;                           /* pass if lo <= value <= hi (lo == hi == 0: +-10 % of target) */
    int scalesWithG;                                /* 1 = a turn rate (turnBoost check) */
} CardLine;

static const CardLine s_card[FC_COUNT] = {
    { "turn_normal", "level turn, NORMAL 510 kt, 3000 m, 9 g", "deg/s", 19.1f, 0, 0, 1 },
    { "turn_350", "level turn, 350 kt (Z), 9 g", "deg/s", 27.9f, 0, 0, 1 },
    { "turn_150", "level turn, 150 kt floor, 4.5 g", "deg/s", 32.0f, 0, 0, 1 },
    { "turn_fast", "level turn, FAST M1.6 (~1020 kt), 7 g", "deg/s", 7.4f, 0, 0, 1 },
    { "roll_90", "roll 0 -> 90 deg, key held, 500 kt", "s", 0.57f, 0, 0, 0 },
    { "roll_360", "full 360 roll, key held, 500 kt", "s", 1.83f, 0, 0, 0 },
    { "pitch_full", "pitch key: full command after", "s", 0.48f, 0, 0, 0 },
    { "tap_01s", "0.1 s tap of Up at 500 kt: nose change", "deg", 0.45f, 0.30f, 0.60f, 0 },
    { "normal_to_fast", "NORMAL -> FAST, sea level, to within 5 %", "s", 13.9f, 0, 0, 0 },
    { "fast_to_350", "FAST -> 350 kt with Z, sea level", "s", 13.3f, 0, 0, 0 },
    { "z350_to_150", "350 -> 155 kt with Z held", "s", 40.8f, 0, 0, 0 },
    { "climb30_loss", "30 deg climb, NORMAL: speed loss, average of the first 10 s", "kt/s", 2.9f, 0, 0, 0 },
};

static int card_ok(int i, float v)
{
    const CardLine *c = &s_card[i];
    if (c->lo != 0.0f || c->hi != 0.0f) return v >= c->lo && v <= c->hi;
    return (float)fabs(v - c->target) <= 0.10f * (float)fabs(c->target);
}

/* ---------------------------------------------------------------- helpers */
static void set_state(FlightState *fs, float Vkt, float hM, float hdgDeg, float gamDeg, float bankDeg)
{
    flight_init(fs, v3(0.0f, hM, 0.0f), E_DEG2RAD(hdgDeg), Vkt * KT2MS);
    flight_set_path(fs, E_DEG2RAD(hdgDeg), E_DEG2RAD(gamDeg), E_DEG2RAD(bankDeg));
    fs->alpha = 0.0f;
}

static void input(PilotInput *in, float up, float ur, int thr)
{
    memset(in, 0, sizeof(*in));
    in->up = up;
    in->ur = ur;
    in->thr = thr;
}

/* level turn at speed Vkt pinned (instantaneous sustained rate at that speed), full pull; the bank is
   iterated until n * cos(bank) = 1 so the turn is level. Returns deg/s; *nOut = load factor, *dAlt = m lost. */
static float measure_turn(float Vkt, float hM, int thr, float *nOut, float *dAlt)
{
    FlightState fs;
    PilotInput in;
    float bank, n, V0 = Vkt * KT2MS, h0, sum = 0.0f;
    int it, i;
    n = flight_g_limit(V0);
    for (it = 0; it < 4; it++) {
        bank = (float)acos(1.0f / E_MAXF(n, 1.01f));
        set_state(&fs, Vkt, hM, 0.0f, 0.0f, E_RAD2DEG(bank));
        input(&in, 1.0f, 0.0f, thr);
        for (i = 0; i < 72; i++) { flight_step(&fs, &in, SIM_DT); fs.V = V0; }
        n = fs.n;
    }
    bank = (float)acos(1.0f / E_MAXF(n, 1.01f));
    set_state(&fs, Vkt, hM, 0.0f, 0.0f, E_RAD2DEG(bank));
    for (i = 0; i < 60; i++) { flight_step(&fs, &in, SIM_DT); fs.V = V0; }
    h0 = fs.pos.y;
    for (i = 0; i < 120; i++) {
        flight_step(&fs, &in, SIM_DT);
        fs.V = V0;
        sum += fs.turnRate;
    }
    if (nOut) *nOut = fs.n;
    if (dAlt) *dAlt = fs.pos.y - h0;
    return E_RAD2DEG(sum / 120.0f);
}

/* key held right from wings level at 500 kt: time to 90 deg and to 360 deg of roll (integrated roll rate) */
static void measure_roll(float *t90, float *t360)
{
    FlightState fs;
    PilotState ps;
    PilotKeys k;
    PilotInput in;
    float roll = 0.0f;
    int i;
    *t90 = *t360 = -1.0f;
    set_state(&fs, 500.0f, 3000.0f, 0.0f, 0.0f, 0.0f);
    pilot_reset(&ps);
    memset(&k, 0, sizeof(k));
    k.right = 1;
    for (i = 1; i <= 600; i++) {
        pilot_update(&ps, &k, &in);
        flight_step(&fs, &in, SIM_DT);
        roll += fs.p * SIM_DT;
        if (*t90 < 0.0f && roll >= E_DEG2RAD(90.0f)) *t90 = (float)i * SIM_DT;
        if (roll >= E_DEG2RAD(360.0f)) { *t360 = (float)i * SIM_DT; break; }
    }
}

static float measure_pitch_full(void)
{
    PilotState ps;
    PilotKeys k;
    PilotInput in;
    int i;
    pilot_reset(&ps);
    memset(&k, 0, sizeof(k));
    k.up = 1;
    for (i = 1; i <= 600; i++) {
        pilot_update(&ps, &k, &in);
        if (in.up >= 0.999f) return (float)i * SIM_DT;
    }
    return -1.0f;
}

/* 0.1 s (12 ticks) tap of Up in level flight at 500 kt: nose pitch change after things settle */
static float measure_tap(int ticks)
{
    FlightState fs;
    PilotState ps;
    PilotKeys k;
    PilotInput in;
    float p0;
    int i;
    set_state(&fs, 500.0f, 3000.0f, 0.0f, 0.0f, 0.0f);
    pilot_reset(&ps);
    memset(&k, 0, sizeof(k));
    for (i = 0; i < 120; i++) { pilot_update(&ps, &k, &in); flight_step(&fs, &in, SIM_DT); }
    p0 = fs.pitch;
    for (i = 0; i < 360; i++) {
        k.up = i < ticks;
        pilot_update(&ps, &k, &in);
        flight_step(&fs, &in, SIM_DT);
    }
    return E_RAD2DEG(fs.pitch - p0);
}

/* run level at thr until cond: V >= stop (up = 1) or V <= stop (up = 0); returns seconds or -1 */
static float measure_speed(FlightState *fs, int thr, float stopMs, int upwards, float maxSec)
{
    PilotInput in;
    int i, n = (int)(maxSec * SIM_HZ);
    input(&in, 0.0f, 0.0f, thr);
    for (i = 1; i <= n; i++) {
        flight_step(fs, &in, SIM_DT);
        if (upwards ? fs->V >= stopMs : fs->V <= stopMs) return (float)i * SIM_DT;
    }
    return -1.0f;
}

static void feel_card_measure(float *v)
{
    FlightState fs;
    PilotInput in;
    float a, b, V0;
    int i;
    v[FC_TURN_NORMAL] = measure_turn(0.8f * speed_of_sound(3000.0f) * MS2KT, 3000.0f, THR_NORMAL, NULL, NULL);
    v[FC_TURN_350] = measure_turn(350.0f, 3000.0f, THR_SLOW, NULL, NULL);
    v[FC_TURN_150] = measure_turn(150.0f, 3000.0f, THR_SLOW, NULL, NULL);
    v[FC_TURN_FAST] = measure_turn(1.6f * speed_of_sound(3000.0f) * MS2KT, 3000.0f, THR_FAST, NULL, NULL);
    measure_roll(&a, &b);
    v[FC_ROLL90] = a;
    v[FC_ROLL360] = b;
    v[FC_PITCH_FULL] = measure_pitch_full();
    v[FC_TAP] = measure_tap(12);
    set_state(&fs, 0.8f * speed_of_sound(0.0f) * MS2KT, 0.0f, 0.0f, 0.0f, 0.0f);
    v[FC_TO_FAST] = measure_speed(&fs, THR_FAST, 0.95f * flight_speed_target(THR_FAST, 0.0f), 1, 120.0f);
    set_state(&fs, 1.6f * speed_of_sound(0.0f) * MS2KT, 0.0f, 0.0f, 0.0f, 0.0f);
    v[FC_TO_350] = measure_speed(&fs, THR_SLOW, 350.0f * KT2MS, 0, 120.0f);
    v[FC_TO_150] = measure_speed(&fs, THR_SLOW, 155.0f * KT2MS, 0, 200.0f);
    set_state(&fs, 0.8f * speed_of_sound(3000.0f) * MS2KT, 3000.0f, 0.0f, 30.0f, 0.0f);
    V0 = fs.V;
    input(&in, 0.0f, 0.0f, THR_NORMAL);
    for (i = 0; i < 10 * SIM_HZ; i++) flight_step(&fs, &in, SIM_DT);
    v[FC_CLIMB30] = (V0 - fs.V) * MS2KT / 10.0f;
}

static void card_log(const float *v, int check, int *fails)
{
    int i, ok;
    for (i = 0; i < FC_COUNT; i++) {
        ok = card_ok(i, v[i]);
        if (check) {
            t_check(ok, s_card[i].key, "value=%.2f%s target=%.2f%s (%s)", (double)v[i], s_card[i].unit,
                    (double)s_card[i].target, s_card[i].unit, s_card[i].text);
        } else {
            log_printf(LOG_INFO, "feel: %-14s value=%7.2f %-5s target=%7.2f (%+6.1f %%) %s  %s", s_card[i].key,
                       (double)v[i], s_card[i].unit, (double)s_card[i].target,
                       (double)(100.0f * (v[i] / s_card[i].target - 1.0f)), ok ? "ok  " : "DIFF", s_card[i].text);
            if (!ok && fails) (*fails)++;
        }
    }
}

/* ---------------------------------------------------------------- Novice mode (pilot.h) */
typedef struct NoviceRes {
    float turnSec, bankMax, bankAvg, altDev, speedKt;   /* Left held from wings level, 500 kt: one full circle */
    float releaseSec;                               /* then released: to |bank| < 2 deg */
    float climbMax, climbEnd, diveMin;              /* Up held 15 s / Down held 15 s (deg) */
    float holdDrift;                                /* Up 1 s then released: gamma change over the next 5 s (deg) */
    float climbTurnRate, levelTurnRate, climbTurnGamma;  /* Up + Left vs Left only, after 4 s (deg/s, deg) */
} NoviceRes;

static void nv_run(FlightState *fs, PilotState *ps, PilotKeys *k, int ticks)
{
    PilotInput in;
    int i;
    for (i = 0; i < ticks; i++) {
        pilot_update_fs(ps, k, fs, &in);
        flight_step(fs, &in, SIM_DT);
    }
}

static void novice_measure(NoviceRes *r)
{
    FlightState fs;
    PilotState ps;
    PilotKeys k;
    PilotInput in;
    float turned = 0.0f, h0, g1, bsum = 0.0f, t;
    int i, nb = 0, oldMode = pilot_mode();
    memset(r, 0, sizeof(*r));
    pilot_set_mode(PILOT_NOVICE);
    /* level turn: Left held */
    set_state(&fs, 500.0f, 3000.0f, 0.0f, 0.0f, 0.0f);
    pilot_reset(&ps);
    memset(&k, 0, sizeof(k));
    k.left = 1;
    h0 = fs.pos.y;
    r->turnSec = -1.0f;
    for (i = 1; i <= 180 * SIM_HZ; i++) {
        pilot_update_fs(&ps, &k, &fs, &in);
        flight_step(&fs, &in, SIM_DT);
        turned += fs.turnRate * SIM_DT;
        if ((float)fabs(fs.pos.y - h0) > r->altDev) r->altDev = (float)fabs(fs.pos.y - h0);
        if ((float)fabs(E_RAD2DEG(fs.bank)) > r->bankMax) r->bankMax = (float)fabs(E_RAD2DEG(fs.bank));
        if (i > 3 * SIM_HZ) { bsum += (float)fabs(E_RAD2DEG(fs.bank)); nb++; }
        if (turned <= -2.0f * E_PI) { r->turnSec = (float)i * SIM_DT; break; }
    }
    r->bankAvg = nb ? bsum / (float)nb : 0.0f;
    r->speedKt = fs.V * MS2KT;
    /* release */
    k.left = 0;
    r->releaseSec = -1.0f;
    for (i = 1; i <= 10 * SIM_HZ; i++) {
        pilot_update_fs(&ps, &k, &fs, &in);
        flight_step(&fs, &in, SIM_DT);
        if ((float)fabs(E_RAD2DEG(fs.bank)) < 2.0f) { r->releaseSec = (float)i * SIM_DT; break; }
    }
    /* climb / dive limits */
    set_state(&fs, 500.0f, 3000.0f, 0.0f, 0.0f, 0.0f);
    pilot_reset(&ps);
    memset(&k, 0, sizeof(k));
    k.up = 1;
    r->climbMax = -90.0f;
    for (i = 0; i < 15 * SIM_HZ; i++) {
        nv_run(&fs, &ps, &k, 1);
        if (E_RAD2DEG(fs.gamma) > r->climbMax) r->climbMax = E_RAD2DEG(fs.gamma);
    }
    r->climbEnd = E_RAD2DEG(fs.gamma);
    set_state(&fs, 500.0f, 12000.0f, 0.0f, 0.0f, 0.0f);
    pilot_reset(&ps);
    k.up = 0;
    k.down = 1;
    r->diveMin = 90.0f;
    for (i = 0; i < 15 * SIM_HZ; i++) {
        nv_run(&fs, &ps, &k, 1);
        if (E_RAD2DEG(fs.gamma) < r->diveMin) r->diveMin = E_RAD2DEG(fs.gamma);
    }
    /* released: holds the climb angle */
    set_state(&fs, 500.0f, 3000.0f, 0.0f, 0.0f, 0.0f);
    pilot_reset(&ps);
    memset(&k, 0, sizeof(k));
    k.up = 1;
    nv_run(&fs, &ps, &k, SIM_HZ);
    k.up = 0;
    nv_run(&fs, &ps, &k, SIM_HZ / 2);
    g1 = fs.gamma;
    nv_run(&fs, &ps, &k, 5 * SIM_HZ);
    r->holdDrift = E_RAD2DEG(fs.gamma - g1);
    /* Up + Left = tighter climbing turn */
    for (t = 0.0f; t < 2.0f; t += 1.0f) {
        set_state(&fs, 500.0f, 3000.0f, 0.0f, 0.0f, 0.0f);
        pilot_reset(&ps);
        memset(&k, 0, sizeof(k));
        k.left = 1;
        k.up = t > 0.5f;
        nv_run(&fs, &ps, &k, 4 * SIM_HZ);
        if (k.up) { r->climbTurnRate = -E_RAD2DEG(fs.turnRate); r->climbTurnGamma = E_RAD2DEG(fs.gamma); }
        else r->levelTurnRate = -E_RAD2DEG(fs.turnRate);
    }
    pilot_set_mode(oldMode);
}

static void novice_log(const NoviceRes *r, int check)
{
    if (!check) {
        log_printf(LOG_INFO, "feel: novice turn_360=%.1fs bank_avg=%.1f bank_max=%.1f alt_dev=%.1fm speed=%.0fkt "
                   "release_to_level=%.2fs", (double)r->turnSec, (double)r->bankAvg, (double)r->bankMax,
                   (double)r->altDev, (double)r->speedKt, (double)r->releaseSec);
        log_printf(LOG_INFO, "feel: novice climb_max=%.1f climb_end=%.1f dive_min=%.1f hold_drift_5s=%.2fdeg "
                   "turn_rate left=%.1f up+left=%.1fdeg/s (gamma %.1f)", (double)r->climbMax, (double)r->climbEnd,
                   (double)r->diveMin, (double)r->holdDrift, (double)r->levelTurnRate, (double)r->climbTurnRate,
                   (double)r->climbTurnGamma);
        return;
    }
    t_check(r->turnSec > 0.0f && (float)fabs(r->bankAvg - 80.0f) < 3.0f && r->bankMax < 83.0f,
            "novice_left_held_80deg_bank", "bank_avg=%.1f bank_max=%.1f 360_in=%.1fs", (double)r->bankAvg,
            (double)r->bankMax, (double)r->turnSec);
    t_check(r->altDev <= 50.0f, "novice_turn_holds_altitude_50m", "alt_dev=%.1fm over the 360", (double)r->altDev);
    t_check(r->releaseSec > 0.0f && r->releaseSec < 1.5f, "novice_release_level_1_5s", "t=%.2fs (to |bank| < 2)",
            (double)r->releaseSec);
    t_check(r->climbMax <= 75.5f && r->climbEnd > 73.0f, "novice_climb_limit_75", "max=%.2f end=%.2f",
            (double)r->climbMax, (double)r->climbEnd);
    t_check(r->diveMin >= -75.5f, "novice_dive_limit_75", "min=%.2f", (double)r->diveMin);
    t_check((float)fabs(r->holdDrift) < 0.5f, "novice_release_holds_climb", "drift=%.2fdeg in 5s", (double)r->holdDrift);
    t_check(r->climbTurnRate > r->levelTurnRate * 1.1f && r->climbTurnGamma > 5.0f, "novice_up_left_tighter_climbing",
            "left=%.1f up+left=%.1fdeg/s gamma=%.1f", (double)r->levelTurnRate, (double)r->climbTurnRate,
            (double)r->climbTurnGamma);
}

static void t_novice(void)
{
    NoviceRes r;
    FlightState fs;
    PilotState ps;
    PilotKeys k;
    PilotInput a, b;
    const Param *p = params_find("pilot", "noviceAutoLevelPitch");
    int m0 = pilot_mode(), m1, m2;
    novice_measure(&r);
    novice_log(&r, 1);
    /* auto level option */
    if (p) {
        *(int *)p->ptr = 1;
        pilot_set_mode(PILOT_NOVICE);
        set_state(&fs, 500.0f, 3000.0f, 0.0f, 0.0f, 0.0f);
        pilot_reset(&ps);
        memset(&k, 0, sizeof(k));
        k.up = 1;
        nv_run(&fs, &ps, &k, 2 * SIM_HZ);
        k.up = 0;
        nv_run(&fs, &ps, &k, 10 * SIM_HZ);
        t_check((float)fabs(E_RAD2DEG(fs.gamma)) < 2.0f, "novice_auto_level_pitch", "gamma_after_10s=%.2f",
                (double)E_RAD2DEG(fs.gamma));
        *(int *)p->ptr = 0;
        pilot_set_mode(m0);
    }
    /* toggle; Expert ignores the aircraft state; pilot_update (no state) always flies Expert */
    m1 = pilot_toggle_mode();
    m2 = pilot_toggle_mode();
    t_check(m1 != m0 && m2 == m0, "mode_toggle", "m0=%d m1=%d m2=%d", m0, m1, m2);
    pilot_set_mode(PILOT_NOVICE);
    set_state(&fs, 500.0f, 3000.0f, 0.0f, 0.0f, 20.0f);
    pilot_reset(&ps);
    memset(&k, 0, sizeof(k));
    k.up = 1;
    pilot_update(&ps, &k, &a);
    pilot_set_mode(PILOT_EXPERT);
    pilot_reset(&ps);
    pilot_update_fs(&ps, &k, &fs, &b);
    t_check(a.up == b.up && a.ur == b.ur && (float)fabs(a.up - 3.0f / 120.0f) < 1e-6f, "expert_unchanged_by_state",
            "up=%.4f/%.4f",
            (double)a.up, (double)b.up);
    pilot_set_mode(m0);
}

static void climb_table_log(int check);             /* T10a, below */
static void zoom_log(void);

/* simh feel: the card with the values loaded now (jet.ini / -ini FILE). Exit code 0 (it is a report). */
int feel_card_print(void)
{
    float v[FC_COUNT], n, d;
    int diffs = 0;
    const Param *pb = params_find("flight", "turnBoost");
    char tb[32];
    tb[0] = 0;
    if (pb) params_value_str(pb, tb, (int)sizeof(tb));
    log_printf(LOG_INFO, "feel: card (DESIGN 4.7) params_hash=%08x turnBoost=%s; DIFF = more than 10 %% off the "
               "default target (expected after tuning)", params_hash(), tb);
    feel_card_measure(v);
    card_log(v, 0, &diffs);
    measure_turn(0.8f * speed_of_sound(3000.0f) * MS2KT, 3000.0f, THR_NORMAL, &n, &d);
    log_printf(LOG_INFO, "feel: extra turn_normal n=%.2f alt_change_1s=%.1fm 360_in=%.1fs", (double)n, (double)d,
               (double)(360.0f / E_MAXF(v[FC_TURN_NORMAL], 0.01f)));
    log_printf(LOG_INFO, "feel: extra glimit kt150=%.2f kt350=%.2f kt510=%.2f kt700=%.2f kt1020=%.2f",
               (double)flight_g_limit(150.0f * KT2MS), (double)flight_g_limit(350.0f * KT2MS),
               (double)flight_g_limit(510.0f * KT2MS), (double)flight_g_limit(700.0f * KT2MS),
               (double)flight_g_limit(1020.0f * KT2MS));
    {
        NoviceRes nr;
        novice_measure(&nr);
        novice_log(&nr, 0);
    }
    climb_table_log(0);
    zoom_log();
    log_printf(LOG_INFO, "feel: done lines=%d diff=%d", FC_COUNT, diffs);
    return 0;
}

/* ---------------------------------------------------------------- param helpers for the tests */
static float *pf(const char *key)
{
    const Param *p = params_find("flight", key);
    return p ? (float *)p->ptr : NULL;
}

/* ---------------------------------------------------------------- the checks */
static void t_path_hold_bank(void)
{
    FlightState fs;
    PilotInput in;
    float g0, rateWant, rateGot, b0, h0, upHold;
    int i;
    /* 60 deg bank, no input: no roll drift, n stays at nHold, path pitch falls as gravity says (g sin^2 / V) */
    set_state(&fs, 500.0f, 3000.0f, 0.0f, 0.0f, 60.0f);
    input(&in, 0.0f, 0.0f, THR_NORMAL);
    b0 = fs.bank;
    g0 = fs.gamma;
    rateWant = GRAV * 0.75f / fs.V;                 /* sin^2(60) = 0.75 */
    for (i = 0; i < 60; i++) flight_step(&fs, &in, SIM_DT);
    rateGot = (g0 - fs.gamma) / 0.5f;
    t_check((float)fabs(rateGot - rateWant) <= 0.1f * rateWant, "bank60_noinput_nose_falls",
            "gamma_rate=%.2fdeg/s want=%.2f", (double)E_RAD2DEG(rateGot), (double)E_RAD2DEG(rateWant));
    for (i = 0; i < 540; i++) flight_step(&fs, &in, SIM_DT);
    t_check((float)fabs(E_RAD2DEG(fs.bank - b0)) < 2.0f && (float)fabs(fs.n - fs.nHold) < 0.02f && fs.p == 0.0f,
            "bank60_noinput_no_roll_drift", "bank=%.2f n=%.3f nHold=%.3f p=%.4f after 5s", (double)E_RAD2DEG(fs.bank),
            (double)fs.n, (double)fs.nHold, (double)fs.p);
    /* the same bank with the pull of a level turn (n = 2) holds altitude */
    set_state(&fs, 500.0f, 3000.0f, 0.0f, 0.0f, 60.0f);
    upHold = (2.0f - 0.5f) / (fs.nLim - 0.5f);
    input(&in, upHold, 0.0f, THR_NORMAL);
    h0 = fs.pos.y;
    for (i = 0; i < 600; i++) { flight_step(&fs, &in, SIM_DT); fs.V = 500.0f * KT2MS; }
    t_check((float)fabs(fs.pos.y - h0) < 15.0f && (float)fabs(E_RAD2DEG(fs.bank) - 60.0f) < 1.0f, "bank60_2g_level",
            "alt_change=%.1fm bank=%.2f n=%.3f up=%.3f", (double)(fs.pos.y - h0), (double)E_RAD2DEG(fs.bank),
            (double)fs.n, (double)upHold);
    /* wings level, no input: path held exactly (auto-trim) */
    set_state(&fs, 500.0f, 3000.0f, 45.0f, 10.0f, 0.0f);
    input(&in, 0.0f, 0.0f, THR_NORMAL);
    for (i = 0; i < 1200; i++) flight_step(&fs, &in, SIM_DT);
    t_check((float)fabs(E_RAD2DEG(fs.gamma) - 10.0f) < 0.05f && (float)fabs(E_RAD2DEG(fs.heading) - 45.0f) < 0.05f &&
            (float)fabs(fs.bank) < 1e-3f, "level_wings_path_hold", "gamma=%.3f hdg=%.3f after 10s",
            (double)E_RAD2DEG(fs.gamma), (double)E_RAD2DEG(fs.heading));
}

static void t_energy(void)
{
    FlightState fs;
    PilotInput in;
    float V0, a, *et = pf("energyTrade"), save;
    int i;
    set_state(&fs, 0.8f * speed_of_sound(3000.0f) * MS2KT, 3000.0f, 0.0f, -30.0f, 0.0f);
    input(&in, 0.0f, 0.0f, THR_NORMAL);
    V0 = fs.V;
    for (i = 0; i < 12; i++) flight_step(&fs, &in, SIM_DT);
    a = (fs.V - V0) / (12.0f * SIM_DT);
    t_check((float)fabs(a - GRAV * 0.5f) < 0.1f * GRAV * 0.5f, "energy_dive30_accel", "dVdt=%.2f want=%.2f",
            (double)a, (double)(GRAV * 0.5f));
    if (et) {
        save = *et;
        *et = 0.0f;
        set_state(&fs, 0.8f * speed_of_sound(3000.0f) * MS2KT, 3000.0f, 0.0f, -30.0f, 0.0f);
        V0 = fs.V;
        for (i = 0; i < 12; i++) flight_step(&fs, &in, SIM_DT);
        a = (fs.V - V0) / (12.0f * SIM_DT);
        *et = save;
        t_check((float)fabs(a) < 0.5f, "energy_trade_off", "dVdt=%.2f", (double)a);
    }
    /* induced drag: a 9 g pull at cruise bleeds speed */
    set_state(&fs, 510.0f, 3000.0f, 0.0f, 0.0f, 83.6f);
    input(&in, 1.0f, 0.0f, THR_NORMAL);
    V0 = fs.V;
    for (i = 0; i < 5 * SIM_HZ; i++) flight_step(&fs, &in, SIM_DT);
    t_check(fs.V < V0 - 15.0f, "induced_bleed_9g", "V0=%.0fkt V5s=%.0fkt", (double)(V0 * MS2KT),
            (double)(fs.V * MS2KT));
}

static void t_floor(void)
{
    FlightState fs, fs2;
    PilotInput in, push;
    float vmin = 1e9f, g2;
    int i;
    /* at the floor, 45 deg climb, no input: nose sinks ~floorDropDeg deg/s towards the horizon, V never < floor */
    set_state(&fs, 150.0f, 3000.0f, 0.0f, 45.0f, 0.0f);
    fs2 = fs;
    input(&in, 0.0f, 0.0f, THR_NORMAL);
    input(&push, -1.0f, 0.0f, THR_NORMAL);
    for (i = 0; i < 2 * SIM_HZ; i++) {
        flight_step(&fs, &in, SIM_DT);
        flight_step(&fs2, &push, SIM_DT);
        if (fs.V < vmin) vmin = fs.V;
    }
    g2 = E_RAD2DEG(fs.gamma);
    t_check(g2 > 26.0f && g2 < 33.0f && vmin >= flight_speed_floor() - 0.01f, "floor_nose_drop_2s",
            "gamma=%.1f (from 45) vmin=%.1fkt", (double)g2, (double)(vmin * MS2KT));
    t_check(fs2.gamma < fs.gamma - E_DEG2RAD(10.0f), "floor_push_still_works", "push_gamma=%.1f free_gamma=%.1f",
            (double)E_RAD2DEG(fs2.gamma), (double)g2);
    for (i = 0; i < 8 * SIM_HZ; i++) flight_step(&fs, &in, SIM_DT);
    t_check(E_RAD2DEG(fs.gamma) < 25.0f && fs.V > flight_speed_floor() + 10.0f * KT2MS, "floor_recovers_10s",
            "gamma=%.2f V=%.0fkt (out of the floor band, climbing on)", (double)E_RAD2DEG(fs.gamma),
            (double)(fs.V * MS2KT));
    /* vertical climb at the floor does not hang */
    set_state(&fs, 160.0f, 3000.0f, 0.0f, 89.5f, 0.0f);
    for (i = 0; i < 15 * SIM_HZ; i++) flight_step(&fs, &in, SIM_DT);
    t_check(E_RAD2DEG(fs.gamma) < 30.0f, "floor_vertical_no_hang", "gamma_after_15s=%.1f V=%.0fkt",
            (double)E_RAD2DEG(fs.gamma), (double)(fs.V * MS2KT));
    /* inverted climb at the floor: the nose still goes to the horizon (not up) */
    set_state(&fs, 150.0f, 3000.0f, 0.0f, 30.0f, 180.0f);
    for (i = 0; i < 3 * SIM_HZ; i++) flight_step(&fs, &in, SIM_DT);
    t_check(E_RAD2DEG(fs.gamma) < 15.0f, "floor_inverted_drop", "gamma_after_3s=%.1f", (double)E_RAD2DEG(fs.gamma));
}

/* ---------------------------------------------------------------- T10a: altitude, thrust lapse, speed truth */
/* excess acceleration (m/s^2) the throttle gives at h, V in level flight: one measured tick */
static float measure_excess(float hM, float Vms, int thr)
{
    FlightState fs;
    PilotInput in;
    flight_init(&fs, v3(0.0f, hM, 0.0f), 0.0f, Vms);
    input(&in, 0.0f, 0.0f, thr);
    flight_step(&fs, &in, SIM_DT);
    return (fs.V - Vms) / SIM_DT;
}

/* sustained climb rate (ft/min) at h: specific excess power V*a/g at the best speed below the throttle target
   (the speed-target controller gives full thrust only below Vt - acc*tauV; sampled 150 kt .. Vt) */
static float climb_rate_fpm(float hM, int thr, float *bestKt)
{
    float Vt = flight_speed_target(thr, hM), V, a, best = 0.0f, bk = 0.0f, ps;
    for (V = flight_speed_floor(); V <= Vt; V += 2.0f) {
        a = measure_excess(hM, V, thr);
        ps = V * a / GRAV;
        if (ps > best) { best = ps; bk = V * MS2KT; }
    }
    if (bestKt) *bestKt = bk;
    return best * M2FT * 60.0f;
}

static const float s_climbAlt[] = { 0.0f, 3000.0f, 6000.0f, 8000.0f, 10000.0f, 11000.0f, 12000.0f, 13000.0f,
                                    14000.0f, 15000.0f, 15500.0f, 16500.0f };

static void climb_table_log(int check)
{
    float ab, mil, abKt, milKt, prev = 1e30f, full = 0.0f;
    int i, mono = 1;
    for (i = 0; i < (int)E_COUNT(s_climbAlt); i++) {
        ab = climb_rate_fpm(s_climbAlt[i], THR_FAST, &abKt);
        mil = climb_rate_fpm(s_climbAlt[i], THR_NORMAL, &milKt);
        log_printf(LOG_INFO, "%s climb alt=%5.0fm (%5.0fft) lapse=%.3f AB=%6.0f fpm (at %4.0f kt)  NORMAL=%6.0f fpm "
                   "(at %4.0f kt)  AB_target=%4.0fkt M%.2f", check ? "test: flight" : "feel:",
                   (double)s_climbAlt[i], (double)(s_climbAlt[i] * M2FT), (double)flight_thrust_lapse(s_climbAlt[i]),
                   (double)ab, (double)abKt, (double)mil, (double)milKt,
                   (double)(flight_speed_target(THR_FAST, s_climbAlt[i]) * MS2KT),
                   (double)(flight_speed_target(THR_FAST, s_climbAlt[i]) / speed_of_sound(s_climbAlt[i])));
        if (ab > prev + 1.0f) mono = 0;
        prev = ab;
        if (i == 0) full = ab;
    }
    if (!check) return;
    {   /* below thrustFullM the throttle acceleration is exactly the M1 one (accAB / accMil caps) */
        float *acc = pf("accAB"), *mil0 = pf("accMil"), e0, e1, e2, m0, m1;
        e0 = measure_excess(0.0f, 300.0f * KT2MS, THR_FAST);
        e1 = measure_excess(3000.0f, 300.0f * KT2MS, THR_FAST);
        e2 = measure_excess(5900.0f, 300.0f * KT2MS, THR_FAST);
        m0 = measure_excess(0.0f, 300.0f * KT2MS, THR_NORMAL);
        m1 = measure_excess(5900.0f, 300.0f * KT2MS, THR_NORMAL);
        t_check(acc && mil0 && (float)fabs(e0 - *acc) < 0.05f && (float)fabs(e1 - *acc) < 0.05f &&
                (float)fabs(e2 - *acc) < 0.05f && (float)fabs(m0 - *mil0) < 0.05f && (float)fabs(m1 - *mil0) < 0.05f,
                "thrust_unchanged_below_6km", "AB 0/3/5.9km=%.2f/%.2f/%.2f NORMAL 0/5.9km=%.2f/%.2f m/s^2",
                (double)e0, (double)e1, (double)e2, (double)m0, (double)m1);
    }
    t_check(mono, "climb_rate_falls_with_altitude", NULL);
    t_check(flight_thrust_lapse(0.0f) == 1.0f && flight_thrust_lapse(5999.0f) == 1.0f &&
            flight_thrust_lapse(flight_ceiling()) == 0.0f && flight_thrust_lapse(flight_ceiling() + 3000.0f) == 0.0f,
            "lapse_1_low_0_at_ceiling", "lapse 0/6k/ceil=%.3f/%.3f/%.3f", (double)flight_thrust_lapse(0.0f),
            (double)flight_thrust_lapse(5999.0f), (double)flight_thrust_lapse(flight_ceiling()));
    ab = climb_rate_fpm(15000.0f, THR_FAST, NULL);
    mil = climb_rate_fpm(flight_ceiling(), THR_FAST, NULL);
    t_check(ab > 1000.0f && ab < 0.15f * full && mil < 1.0f, "climb_fades_to_0_at_ceiling",
            "AB 15km=%.0f ceiling=%.0f sea=%.0f fpm", (double)ab, (double)mil, (double)full);
}

/* the user's M1 case (tests/user/m1_ceiling.rpl, t=258-302 s): after a zoom to 17.4 km the jet sat at 150 kt for
   44 s in a full-AB dive of 35-90 deg (T05's ceilDrag 8-14 m/s^2 > g). Every dive must gain speed now. */
static void t_dive_always_gains(void)
{
    static const float alts[] = { 3000.0f, 9000.0f, 12000.0f, 15000.0f, 16000.0f, 17200.0f, 18500.0f, 20000.0f };
    static const float dives[] = { -10.0f, -20.0f, -35.0f, -60.0f, -90.0f };
    static const int thrs[] = { THR_FAST, THR_NORMAL };
    FlightState fs;
    PilotInput in;
    float V0 = flight_speed_floor(), worst = 1e9f, a, dv3, dv5, h0;
    int ia, id, it, i, bad = 0, wA = 0, wD = 0, wT = 0;
    for (ia = 0; ia < (int)E_COUNT(alts); ia++)
        for (id = 0; id < (int)E_COUNT(dives); id++)
            for (it = 0; it < 2; it++) {
                set_state(&fs, V0 * MS2KT, alts[ia], 0.0f, dives[id], 0.0f);
                input(&in, 0.0f, 0.0f, thrs[it]);
                flight_step(&fs, &in, SIM_DT);
                a = (fs.V - V0) / SIM_DT;
                if (a < worst) { worst = a; wA = ia; wD = id; wT = it; }
                if (a <= 0.0f) bad++;
            }
    t_check(bad == 0, "dive_always_gains_speed", "cases=%d bad=%d worst_dVdt=%.2f at alt=%.0f dive=%.0f thr=%d "
            "(from 150 kt, AB and NORMAL, 3-20 km, 10-90 deg)", (int)(E_COUNT(alts) * E_COUNT(dives) * 2), bad,
            (double)worst, (double)alts[wA], (double)dives[wD], thrs[wT]);
    /* the user's numbers: 17.2 km, 150 kt, 35 deg dive, full AB, path held */
    set_state(&fs, 150.0f, 17200.0f, 0.0f, -35.0f, 0.0f);
    input(&in, 0.0f, 0.0f, THR_FAST);
    h0 = fs.pos.y;
    for (i = 0; i < 3 * SIM_HZ; i++) flight_step(&fs, &in, SIM_DT);
    dv3 = fs.V * MS2KT;
    for (i = 0; i < 2 * SIM_HZ; i++) flight_step(&fs, &in, SIM_DT);
    dv5 = fs.V * MS2KT;
    t_check(dv3 > 180.0f && dv5 > 200.0f, "user_case_ab_dive_17km", "V 0/3/5s=150/%.0f/%.0f kt alt_lost=%.0fm "
            "(gravity: g sin35 = 10.8 kt/s)", (double)dv3, (double)dv5, (double)(h0 - fs.pos.y));
    for (i = 0; i < 60 * SIM_HZ && fs.V * MS2KT < 500.0f; i++) flight_step(&fs, &in, SIM_DT);
    log_printf(LOG_INFO, "test: flight user_case 35deg AB dive from 17.2 km: 500 kt after %.1f s at %.0f m",
               5.0 + (double)i / SIM_HZ, (double)fs.pos.y);
    /* vertical AB dive from the same point: well above the floor in a few seconds (gravity 19 kt/s) */
    set_state(&fs, 150.0f, 17200.0f, 0.0f, -89.0f, 0.0f);
    for (i = 0; i < 3 * SIM_HZ; i++) flight_step(&fs, &in, SIM_DT);
    dv3 = fs.V * MS2KT;
    for (i = 0; i < 2 * SIM_HZ; i++) flight_step(&fs, &in, SIM_DT);
    dv5 = fs.V * MS2KT;
    t_check(dv3 > 200.0f && dv5 > 235.0f, "user_case_vertical_dive", "V 0/3/5s=150/%.0f/%.0f kt", (double)dv3,
            (double)dv5);
}

/* AB zoom climbs: top out smoothly, come back below the ceiling, never sit at the floor while descending */
static void zoom_run(float Vkt, float hM, float gamDeg, int pullTicks, int secs, float *hmax, float *hEnd,
                     int *floorDiveTicks, float *maxJerk, float *vEnd)
{
    FlightState fs;
    PilotInput in;
    float aPrev = 0.0f, a, Vp;
    int i;
    set_state(&fs, Vkt, hM, 0.0f, gamDeg, 0.0f);
    *hmax = 0.0f;
    *floorDiveTicks = 0;
    *maxJerk = 0.0f;
    for (i = 0; i < secs * SIM_HZ; i++) {
        input(&in, i < pullTicks ? 1.0f : 0.0f, 0.0f, THR_FAST);
        Vp = fs.V;
        flight_step(&fs, &in, SIM_DT);
        a = (fs.V - Vp) / SIM_DT;
        if (i > pullTicks + 2 && (float)fabs(a - aPrev) > *maxJerk && !(fs.capped & 4)) *maxJerk = (float)fabs(a - aPrev);
        aPrev = a;
        if (fs.pos.y > *hmax) *hmax = fs.pos.y;
        if ((fs.capped & 4) && fs.gamma < E_DEG2RAD(-5.0f)) (*floorDiveTicks)++;
    }
    *hEnd = fs.pos.y;
    *vEnd = fs.V * MS2KT;
}

static void zoom_log(void)
{
    float hmax, hEnd, jerk, vEnd;
    int fdt;
    zoom_run(510.0f, 3000.0f, 30.0f, 0, 300, &hmax, &hEnd, &fdt, &jerk, &vEnd);
    log_printf(LOG_INFO, "feel: zoom AB 30deg climb from 3 km 510 kt (held): top %.0f m (%.0f ft)", (double)hmax,
               (double)(hmax * M2FT));
    zoom_run(920.0f, 15000.0f, 0.0f, 4 * SIM_HZ, 300, &hmax, &hEnd, &fdt, &jerk, &vEnd);
    log_printf(LOG_INFO, "feel: zoom AB M1.6 at 15 km, 4 s full pull: top %.0f m (%.0f ft)", (double)hmax,
               (double)(hmax * M2FT));
    zoom_run(1000.0f, 4000.0f, 87.0f, 0, 300, &hmax, &hEnd, &fdt, &jerk, &vEnd);
    log_printf(LOG_INFO, "feel: zoom AB vertical from 4 km 1000 kt: top %.0f m (%.0f ft)", (double)hmax,
               (double)(hmax * M2FT));
}

static void t_ceiling(void)
{
    float hmax, hEnd, jerk, vEnd, c = flight_ceiling();
    int fdt;
    /* T05's case: 30 deg AB climb from 3000 m, path held for 300 s (floor nose drop does the rest) */
    zoom_run(510.0f, 3000.0f, 30.0f, 0, 300, &hmax, &hEnd, &fdt, &jerk, &vEnd);
    t_check(hmax < c + 2500.0f && hmax > c - 2500.0f, "ceiling_ab_climb_tops_out", "hmax=%.0fm ceiling=%.0f",
            (double)hmax, (double)c);
    t_check(hEnd < c + 300.0f && fdt == 0, "ceiling_settles", "h_300s=%.0fm V=%.0fkt floor_dive_ticks=%d",
            (double)hEnd, (double)vEnd, fdt);
    t_check(jerk < 0.5f, "ceiling_climb_smooth", "max_accel_step=%.3f m/s^2 per tick", (double)jerk);
    /* best case zoom: M1.6 level at 15 km, 4 s full pull, then path held */
    zoom_run(920.0f, 15000.0f, 0.0f, 4 * SIM_HZ, 300, &hmax, &hEnd, &fdt, &jerk, &vEnd);
    t_check(hmax < c + 2500.0f, "zoom_m16_15km_limited", "hmax=%.0fm (%.0fft) ceiling=%.0f", (double)hmax,
            (double)(hmax * M2FT), (double)c);
    t_check(hEnd < c + 300.0f && fdt == 0, "zoom_m16_comes_back", "h_300s=%.0fm V=%.0fkt floor_dive_ticks=%d",
            (double)hEnd, (double)vEnd, fdt);
    /* the user's zoom: 1000 kt at 4 km, vertical, AB, held */
    zoom_run(1000.0f, 4000.0f, 87.0f, 0, 300, &hmax, &hEnd, &fdt, &jerk, &vEnd);
    t_check(hmax < c + 2500.0f && hEnd < c + 300.0f && fdt == 0, "zoom_vertical_1000kt_4km",
            "hmax=%.0fm (%.0fft) h_300s=%.0fm floor_dive_ticks=%d", (double)hmax, (double)(hmax * M2FT),
            (double)hEnd, fdt);
    climb_table_log(1);
}

/* speed truth (user: "1000 knots doesn't feel like 1800 kph"): metres flown per second = V; HUD knots = V*MS2KT
   (true airspeed); 1000 kt = 1852 km/h; speed of sound vs ISA; Mach targets in TAS knots at altitude */
static void t_speed_truth(void)
{
    static const float isaH[] = { 0.0f, 3000.0f, 6000.0f, 9000.0f, 11000.0f, 15000.0f };
    static const float isaA[] = { 340.29f, 328.58f, 316.45f, 303.79f, 295.07f, 295.07f };
    FlightState fs;
    PilotInput in;
    Vec3 p0;
    float d, err = 0.0f, e;
    int i, ok = 1;
    flight_init(&fs, v3(0.0f, 1000.0f, 0.0f), E_DEG2RAD(30.0f), 1000.0f * KT2MS);
    input(&in, 0.0f, 0.0f, THR_FAST);
    p0 = fs.pos;
    for (i = 0; i < SIM_HZ; i++) flight_step(&fs, &in, SIM_DT);
    d = v3_len(v3_sub(fs.pos, p0));
    t_check((float)fabs(d - 0.5f * (1000.0f * KT2MS + fs.V)) < 0.5f, "speed_truth_distance_1s",
            "flown=%.1fm in 1s at %.0f..%.0f kt (HUD) = %.1f m/s = %.0f km/h", (double)d, 1000.0,
            (double)(fs.V * MS2KT), (double)d, (double)(d * 3.6f));
    t_check((float)fabs(1000.0f * KT2MS * 3.6f - 1852.0f) < 0.5f, "speed_truth_kt_to_kmh",
            "1000kt=%.1fkm/h", (double)(1000.0f * KT2MS * 3.6f));
    for (i = 0; i < (int)E_COUNT(isaH); i++) {
        e = (float)fabs(speed_of_sound(isaH[i]) / isaA[i] - 1.0f);
        if (e > err) err = e;
        if (e > 0.005f) ok = 0;
    }
    t_check(ok, "speed_truth_sound_vs_isa", "worst=%.2f%%", (double)(100.0f * err));
    log_printf(LOG_INFO, "test: flight speed_truth M1.6=%.0fkt@0m %.0fkt@6km %.0fkt@11km+  M0.8=%.0fkt@0m %.0fkt@11km+ "
               "(TAS; HUD shows TAS)", (double)(flight_speed_target(THR_FAST, 0.0f) * MS2KT),
               (double)(flight_speed_target(THR_FAST, 6000.0f) * MS2KT),
               (double)(flight_speed_target(THR_FAST, 11000.0f) * MS2KT),
               (double)(flight_speed_target(THR_NORMAL, 0.0f) * MS2KT),
               (double)(flight_speed_target(THR_NORMAL, 11000.0f) * MS2KT));
}

static void t_detent(void)
{
    FlightState fs;
    PilotInput in;
    int i;
    set_state(&fs, 450.0f, 3000.0f, 0.0f, 0.0f, 3.0f);
    input(&in, 0.0f, 0.0f, THR_NORMAL);
    for (i = 0; i < 2 * SIM_HZ; i++) flight_step(&fs, &in, SIM_DT);
    t_check((float)fabs(E_RAD2DEG(fs.bank)) < 0.3f, "detent_levels_3deg", "bank_after_2s=%.3f",
            (double)E_RAD2DEG(fs.bank));
    set_state(&fs, 450.0f, 3000.0f, 0.0f, 0.0f, -3.0f);
    for (i = 0; i < 2 * SIM_HZ; i++) flight_step(&fs, &in, SIM_DT);
    t_check((float)fabs(E_RAD2DEG(fs.bank)) < 0.3f, "detent_levels_minus3deg", "bank_after_2s=%.3f",
            (double)E_RAD2DEG(fs.bank));
    set_state(&fs, 450.0f, 3000.0f, 0.0f, 0.0f, 8.0f);
    for (i = 0; i < 2 * SIM_HZ; i++) flight_step(&fs, &in, SIM_DT);
    t_check((float)fabs(E_RAD2DEG(fs.bank) - 8.0f) < 1.0f, "detent_keeps_8deg", "bank_after_2s=%.3f",
            (double)E_RAD2DEG(fs.bank));
}

static void t_quat_and_determinism(void)
{
    FlightState a, b;
    PilotInput in;
    ERand r;
    float worst = 0.0f, len, vmin = 1e9f, vmax = 0.0f;
    int i;
    set_state(&a, 500.0f, 5000.0f, 0.0f, 0.0f, 0.0f);
    b = a;
    rnd_seed(&r, 1234u);
    memset(&in, 0, sizeof(in));
    for (i = 0; i < 120 * SIM_HZ; i++) {
        if ((i % 60) == 0) {
            in.up = rnd_range(&r, -1.0f, 1.0f);
            in.ur = rnd_range(&r, -1.0f, 1.0f);
            if (rnd_f01(&r) < 0.3f) in.ur = 0.0f;
            in.thr = (int)(rnd_u32(&r) % 3u);
        }
        flight_step(&a, &in, SIM_DT);
        flight_step(&b, &in, SIM_DT);
        len = (float)fabs(q_dot(a.qPath, a.qPath) - 1.0f);
        if (len > worst) worst = len;
        if (a.V < vmin) vmin = a.V;
        if (a.V > vmax) vmax = a.V;
    }
    t_check(worst < 1e-5f, "quat_norm_2min_random", "worst=%.2e", (double)worst);
    t_check(!memcmp(&a, &b, sizeof(a)), "deterministic_same_input", NULL);
    t_check(vmin >= flight_speed_floor() - 0.01f && vmax <= 2.0f * speed_of_sound(0.0f) + 0.01f, "speed_clamps",
            "vmin=%.0fkt vmax=%.0fkt", (double)(vmin * MS2KT), (double)(vmax * MS2KT));
    /* vertical dive in AB: Mach 2 clamp */
    set_state(&a, 900.0f, 12000.0f, 0.0f, -80.0f, 0.0f);
    input(&in, 0.0f, 0.0f, THR_FAST);
    for (i = 0; i < 20 * SIM_HZ && a.pos.y > 1000.0f; i++) flight_step(&a, &in, SIM_DT);
    t_check(a.mach <= 2.0f + 1e-3f, "mach2_clamp", "mach=%.3f", (double)a.mach);
}

static void t_visual_and_ground(void)
{
    FlightState fs;
    PilotInput in;
    Vec3 p0;
    int i;
    set_state(&fs, 450.0f, 3000.0f, 0.0f, 0.0f, 0.0f);
    input(&in, 1.0f, 1.0f, THR_FAST);
    for (i = 0; i < 60; i++) flight_step(&fs, &in, SIM_DT);
    t_check(fs.vis.stabL > 0.0f && fs.vis.stabR > fs.vis.stabL && fs.vis.flapR > 0.0f && fs.vis.flapL < 0.0f &&
            fs.ab > 0.99f && fs.vis.nozzle > 0.99f, "visual_signs_pull_roll_right",
            "stabL=%.1f stabR=%.1f flapL=%.1f flapR=%.1f ab=%.2f", (double)E_RAD2DEG(fs.vis.stabL),
            (double)E_RAD2DEG(fs.vis.stabR), (double)E_RAD2DEG(fs.vis.flapL), (double)E_RAD2DEG(fs.vis.flapR),
            (double)fs.ab);
    set_state(&fs, 450.0f, 3000.0f, 0.0f, 0.0f, 0.0f);
    input(&in, 0.0f, 0.0f, THR_FAST);
    for (i = 0; i < 18; i++) flight_step(&fs, &in, SIM_DT);
    t_check(fs.ab > 0.45f && fs.ab < 0.55f, "ab_lag_0_15s", "ab=%.2f", (double)fs.ab);
    /* on the ground the model leaves pos/V alone (T06 owns them) */
    flight_init(&fs, v3(10.0f, 5.0f, 20.0f), 1.0f, 30.0f);
    fs.onGround = 1;
    p0 = fs.pos;
    input(&in, 1.0f, 1.0f, THR_FAST);
    for (i = 0; i < 120; i++) flight_step(&fs, &in, SIM_DT);
    t_check(fs.pos.x == p0.x && fs.pos.y == p0.y && fs.pos.z == p0.z && fs.V == 30.0f && fs.ab > 0.99f,
            "on_ground_no_motion", NULL);
    /* flight_set_path round trip */
    set_state(&fs, 400.0f, 1000.0f, 123.0f, 12.0f, -35.0f);
    t_check((float)fabs(E_RAD2DEG(wrap_pi(fs.heading - E_DEG2RAD(123.0f)))) < 0.01f &&
            (float)fabs(E_RAD2DEG(fs.gamma) - 12.0f) < 0.01f && (float)fabs(E_RAD2DEG(fs.bank) + 35.0f) < 0.01f,
            "path_angles_roundtrip", "hdg=%.3f gamma=%.3f bank=%.3f", (double)E_RAD2DEG(fs.heading),
            (double)E_RAD2DEG(fs.gamma), (double)E_RAD2DEG(fs.bank));
}

static void t_telemetry(void)
{
    FlightState fs;
    PilotKeys k;
    PilotInput in;
    char line[512];
    FILE *f;
    int i, lines = 0;
    const char *path = "jet_test_telemetry.csv";
    set_state(&fs, 450.0f, 3000.0f, 0.0f, 0.0f, 0.0f);
    memset(&k, 0, sizeof(k));
    input(&in, 0.2f, 0.0f, THR_NORMAL);
    if (!flight_telemetry_open(path)) { t_check(0, "telemetry_open", "path=%s", path); return; }
    for (i = 0; i < 10; i++) {
        flight_step(&fs, &in, SIM_DT);
        flight_telemetry_row((u32)i, &k, &in, &fs);
    }
    flight_telemetry_close();
    f = fopen(path, "rb");
    if (f) {
        while (fgets(line, (int)sizeof(line), f)) lines++;
        fclose(f);
    }
    remove(path);
    t_check(lines == 11, "telemetry_csv_rows", "lines=%d (header + 10)", lines);
}

static void t_timing(void)
{
    FlightState fs[4];
    PilotInput in;
    s64 t0, t1;
    int i, j, steps = 0;
    float us;
    if (!prof_now_us()) { log_printf(LOG_INFO, "test: flight timing skipped=no_clock"); return; }
    for (j = 0; j < 4; j++) set_state(&fs[j], 300.0f + 100.0f * (float)j, 3000.0f, 0.0f, 0.0f, 20.0f * (float)j);
    input(&in, 0.6f, 0.3f, THR_NORMAL);
    t0 = prof_now_us();
    for (i = 0; i < 3000; i++) {
        in.ur = (i & 256) ? 0.3f : -0.3f;
        for (j = 0; j < 4; j++) { flight_step(&fs[j], &in, SIM_DT); steps++; }
    }
    t1 = prof_now_us();
    us = (float)(t1 - t0) / (float)steps;
    log_printf(LOG_INFO, "test: flight timing us_per_step=%.2f steps=%d", (double)us, steps);
    t_check(us <= 15.0f, "timing_step_le_15us", "us=%.2f", (double)us);
}

/* ---------------------------------------------------------------- -test flight FILE.rpl (T10a)
   Replays a recorded flight headless through the session (the file's params snapshot, its keys) and writes the
   flight telemetry of the player to FILE.rpl.csv (same columns as -telemetry). Logs a summary line per 5 s of the
   flight and the replay hash result. A replay recorded by an older flight model diverges (hash DIFF lines) once
   the physics differ: then the CSV shows the NEW model flown with the old keys. Exit code = hash differences. */
static int flight_replay_telemetry(const char *rpl)
{
    static Game g;
    static Terrain ter;
    static int terOk = -1;
    static char snap[65536];
    char path[E_PATH], csv[E_PATH + 8];
    ReplaySetup rs;
    ReplayStats st;
    PilotKeys k;
    int cmd, n, samples = 0, resp, skip = 1;
    const FlightState *fs;
    Vec3 p0;
    float vSum = 0.0f, d = 0.0f, e, worst = 0.0f, worstKt = 0.0f, fastKt = 0.0f, fastM = 0.0f;
    u32 t0 = 0;
    if (strlen(rpl) >= E_PATH) return 1;
    n = params_snapshot(snap, (int)sizeof(snap));
    if (terOk < 0) {
        data_path(path, (int)sizeof(path), "island/jet.isl");
        terOk = terrain_load(&ter, path);
    }
    if (!replay_play_open(rpl, &rs, 0)) {
        log_printf(LOG_ERR, "test: flight replay cannot open path=%s", rpl);
        return 1;
    }
    strcpy(csv, rpl);
    strcat(csv, ".csv");
    flight_telemetry_open(csv);
    session_start(&g, terOk ? &ter : NULL, rs.mode, rs.diff, rs.start, rs.seed);
    resp = session_respawns();
    p0 = g.ac[PLAYER_ID].fs.pos;
    for (;;) {
        if (!replay_play_tick(g.tick, &k, &cmd, &g)) break;
        session_tick(&g, &k, cmd);
        fs = &g.ac[PLAYER_ID].fs;
        flight_telemetry_row(g.tick, &k, &g.ac[PLAYER_ID].in, fs);
        /* speed truth through the whole session: metres actually flown (path length) per second of sim vs the
           HUD's speed (HUD = V * MS2KT, true airspeed). 1 s windows while airborne and alive, no respawn inside */
        if (fs->onGround || !(g.ac[PLAYER_ID].flags & AC_ALIVE) || session_respawns() != resp || !t0) {
            resp = session_respawns();
            t0 = g.tick;
            p0 = fs->pos;
            vSum = d = 0.0f;
            skip = 1;                               /* the first second after liftoff/respawn is not counted */
        } else {
            vSum += fs->V;
            d += v3_len(v3_sub(fs->pos, p0));       /* path length (a chord would be short in turns) */
            p0 = fs->pos;
            if (g.tick - t0 == (u32)SIM_HZ && skip) {
                skip = 0;
                t0 = g.tick;
                vSum = d = 0.0f;
            } else if (g.tick - t0 == (u32)SIM_HZ) {
                e = (float)fabs(d / (vSum * SIM_DT) - 1.0f);   /* sum of V * dt over 1 s = metres the HUD says */
                if (e > worst) { worst = e; worstKt = vSum / SIM_HZ * MS2KT; }
                if (e > 0.005f)
                    log_printf(LOG_INFO, "test: flight replay speed_truth window tick=%u err=%.3f V=%.0fkt d=%.1f",
                               g.tick, (double)e, (double)(vSum / SIM_HZ * MS2KT), (double)d);
                if (vSum / SIM_HZ * MS2KT > fastKt) { fastKt = vSum / SIM_HZ * MS2KT; fastM = d; }
                samples++;
                t0 = g.tick;
                vSum = d = 0.0f;
            }
        }
        if ((g.tick % (5 * SIM_HZ)) == 0)
            log_printf(LOG_INFO, "test: flight replay t=%.0f alt=%.0fft V=%.0fkt Vt=%.0fkt mach=%.2f gamma=%.1f "
                       "thr=%d n=%.2f capped=%d", (double)g.tick / SIM_HZ, (double)(fs->pos.y * M2FT),
                       (double)(fs->V * MS2KT), (double)(fs->Vt * MS2KT), (double)fs->mach,
                       (double)E_RAD2DEG(fs->gamma), fs->thr, (double)fs->n, fs->capped);
    }
    replay_play_tick(g.tick, &k, &cmd, &g);
    replay_play_close();
    flight_telemetry_close();
    replay_stats(&st);
    log_printf(LOG_INFO, "test: flight replay speed_truth windows=%d worst_err=%.3f%% (at %.0f kt) fastest_second: "
               "HUD %.0f kt = %.0f km/h, flown %.0f m (%.0f km/h)", samples, (double)(100.0f * worst),
               (double)worstKt, (double)fastKt, (double)(fastKt * KT2MS * 3.6f), (double)fastM,
               (double)(fastM * 3.6f));
    log_printf(LOG_INFO, "test: flight replay done ticks=%u hashes=%u diffs=%u first_diff=%u csv=%s", st.ticks,
               st.hashes, st.diffs, st.firstDiffTick, csv);
    if (n > 0) params_restore(snap, n);
    return (int)st.diffs;
}

int test_flight(const char *arg)
{
    static char snap[65536];
    float v[FC_COUNT];
    int n, fails;
    Cfg *empty;
    if (arg && arg[0] && strstr(arg, ".rpl")) return flight_replay_telemetry(arg);
    t_begin("flight");
    /* run against the code defaults (the shipped feel), not the user's jet.ini */
    n = params_snapshot(snap, (int)sizeof(snap));
    empty = cfg_parse("", 0);
    params_load(empty);
    cfg_free(empty);
    log_printf(LOG_INFO, "test: flight params=defaults hash=%08x", params_hash());

    feel_card_measure(v);
    card_log(v, 1, NULL);
    t_path_hold_bank();
    t_energy();
    t_floor();
    t_ceiling();
    t_dive_always_gains();
    t_speed_truth();
    t_detent();
    t_quat_and_determinism();
    t_visual_and_ground();
    t_telemetry();
    t_novice();
    t_timing();

    if (n > 0) params_restore(snap, n);
    fails = t_end();
    return fails;
}
