/* tests_takeoff.cpp - -test takeoff (DESIGN 6.1, 16.1). Portable. Owner: T06.

   Runs the player's tick exactly as main does (DESIGN 2.6: pilot_update_fs -> takeoff_tick -> world_override_input
   -> flight_step -> world_check) on the island from data/island/jet.isl (the terrain stub until T04: flat sea and
   a default runway). Every runway fact (threshold, heading, centre line, runway end) comes from terrain_base(), so
   the same checks hold on the stub and on the real island. Code defaults of every param (snapshot, empty ini,
   restore), like -test flight.
   Cases: tap A (Expert, NORMAL climb), A held (FAST), Novice tap, override at 200 ft (key held from liftoff: no
   handover below 200 ft, handover as soon as 200 ft is reached), override above 200 ft (0.25 s), unbound Expert
   override, handover continuity, respawn after a crash, and the script tests/t06/takeoff.scr (DESIGN 16.2 format,
   also accepted as the argument: -test takeoff path\to\file.scr).
   Lines: "test: takeoff <what> ok|FAIL ..." and the module's own "takeoff: phase=..." timeline. */
#include "tests.h"
#include "params.h"
#include "takeoff.h"
#include "world.h"
#include "e_terrain.h"
#include "e_file.h"
#include "e_log.h"

/* ---------------------------------------------------------------- harness (shared shape with tests_world.cpp) */
static Terrain s_ter;
static int s_terInit, s_terOk;
static Game s_g;

typedef struct Sim {
    Game *g;
    PilotState ps;
    TakeoffState ts;
    PilotKeys keys;
    int bits;                                       /* world_check bits OR'ed */
    int bound;                                      /* takeoff_bind_pilot used */
    float maxDUp, maxDUr;                           /* largest per-tick change of the input axes (HANDOVER window) */
    PilotInput last;
} Sim;

static void terrain_once(void)
{
    char path[E_PATH];
    if (s_terInit) return;
    data_path(path, (int)sizeof(path), "island/jet.isl");
    s_terOk = terrain_load(&s_ter, path);
    s_terInit = 1;
    log_printf(LOG_INFO, "test: takeoff terrain loaded=%d path=%s (0 = T01 stub: flat sea, default runway)", s_terOk, path);
}

static void sim_start(Sim *s, int bind)
{
    Aircraft *pl;
    terrain_once();
    memset(&s_g, 0, sizeof(s_g));
    s_g.terrain = &s_ter;
    s_g.numAircraft = 1;
    s_g.tick = 1;                                   /* tick 0 is "never" in the takeoff stats */
    memset(s, 0, sizeof(*s));
    s->g = &s_g;
    pl = &s_g.ac[PLAYER_ID];
    pl->id = PLAYER_ID;
    pl->side = SIDE_BLUE;
    pl->flags = AC_ALIVE | AC_PLAYER;
    pl->fs.onGround = 1;
    world_reset(&s_g);
    pilot_reset(&s->ps);
    s->bound = bind;
    if (bind) takeoff_bind_pilot(&s->ts, &s->ps);
    takeoff_reset(&s->ts, pl, &s_g);
}

/* -log 2: one track line per second (distance along the runway heading from the threshold, height above it), for
   checking the climb-out against the island's terrain offline */
static void log_track(const Sim *s)
{
    const FlightState *fs = &s->g->ac[PLAYER_ID].fs;
    const TerrainBase *b = terrain_base(s->g->terrain);
    float h = E_DEG2RAD(b->thresholdHeadingDeg);
    if ((s->g->tick % SIM_HZ) != 0) return;
    log_printf(LOG_DEBUG, "test: takeoff track t=%.0f along=%.1f up=%.1f lat=%.2f agl=%.1f V=%.0fkt phase=%s",
               (double)((float)s->g->tick / (float)SIM_HZ), (double)((fs->pos.x - b->threshold.x) * (float)sin(h) + (fs->pos.z - b->threshold.z) * (float)cos(h)),
               (double)(fs->pos.y - b->threshold.y), (double)takeoff_centre_line_error(s->g, fs->pos),
               (double)world_agl(s->g, fs->pos), (double)(fs->V * MS2KT), takeoff_phase_name(s->ts.phase));
}

static void sim_tick(Sim *s)
{
    Game *g = s->g;
    Aircraft *pl = &g->ac[PLAYER_ID];
    PilotInput in, own;
    pilot_update_fs(&s->ps, &s->keys, &pl->fs, &in);
    if (takeoff_tick(&s->ts, pl, &in, &own, g)) in = own;
    world_override_input(g, PLAYER_ID, &in);
    if (s->ts.phase >= TO_CLIMB) {
        float du = (float)fabs(in.up - s->last.up), dr = (float)fabs(in.ur - s->last.ur);
        if (s->ts.phase == TO_HANDOVER || (s->ts.phase == TO_DONE && g->tick - s->ts.tickHand < 3 * SIM_HZ)) {
            if (du > s->maxDUp) s->maxDUp = du;
            if (dr > s->maxDUr) s->maxDUr = dr;
        }
    }
    s->last = in;
    pl->in = in;
    pl->prev = pl->fs;
    if (pl->flags & AC_ALIVE) flight_step(&pl->fs, &in, SIM_DT);
    s->bits |= world_check(g);
    g->tick++;
    log_track(s);
}

static float sim_t(const Sim *s) { return (float)s->g->tick / (float)SIM_HZ; }
static float since_a(const Sim *s, u32 tick) { return (float)(tick - s->ts.tickA) / (float)SIM_HZ; }

/* runs until the takeoff is DONE (or crash / maxSec); returns 1 if DONE */
static int sim_until_done(Sim *s, float maxSec)
{
    u32 end = s->g->tick + (u32)(maxSec * SIM_HZ);
    while (s->g->tick < end) {
        sim_tick(s);
        if (s->ts.phase == TO_DONE) return 1;
        if (!(s->g->ac[PLAYER_ID].flags & AC_ALIVE)) return 0;
    }
    return 0;
}

static void run_sec(Sim *s, float sec)
{
    u32 end = s->g->tick + (u32)(sec * SIM_HZ);
    while (s->g->tick < end) sim_tick(s);
}

/* events since seq0 of one type: count, and the order index of the first occurrence */
static int count_events(u32 seq0, int type)
{
    u32 i;
    int n = 0;
    WorldEvent ev;
    for (i = seq0; i < world_event_seq(); i++)
        if (world_event_get(i, &ev) && ev.type == type) n++;
    return n;
}

static int event_order_ok(u32 seq0, const int *types, int nTypes)
{
    u32 i;
    int k = 0;
    WorldEvent ev;
    for (i = seq0; i < world_event_seq() && k < nTypes; i++)
        if (world_event_get(i, &ev) && ev.type == types[k]) k++;
    return k == nTypes;
}

/* distance from the threshold to the far end of the runway along the takeoff heading */
static float runway_left(const Game *g)
{
    const TerrainBase *b = terrain_base(g->terrain);
    float h = E_DEG2RAD(b->rwyHeadingDeg), fx = (float)sin(h), fz = (float)cos(h);
    float ex = b->rwyX + fx * b->rwyLength * 0.5f, ez = b->rwyZ + fz * b->rwyLength * 0.5f;
    return (ex - b->threshold.x) * fx + (ez - b->threshold.z) * fz;
}

static float param_climb_deg(void)
{
    const Param *p = params_find("takeoff", "climbPitchDeg");
    return p ? *(const float *)p->ptr : 12.0f;
}

/* ---------------------------------------------------------------- cases */
static void t_spawn(void)
{
    Sim s;
    const TerrainBase *b;
    const FlightState *fs;
    float dh;
    sim_start(&s, 1);
    b = terrain_base(s.g->terrain);
    fs = &s.g->ac[PLAYER_ID].fs;
    t_check(s.ts.phase == TO_READY, "spawn_ready", "phase=%s", takeoff_phase_name(s.ts.phase));
    t_check(fs->onGround && fs->gearPos == 1.0f && fs->V == 0.0f, "spawn_on_ground", "onGround=%d gear=%.2f V=%.1f",
            fs->onGround, (double)fs->gearPos, (double)fs->V);
    t_check((float)fabs(fs->pos.y - (b->threshold.y + takeoff_gear_height())) < 0.01f, "spawn_height",
            "y=%.2f threshold=%.2f gear=%.2f", (double)fs->pos.y, (double)b->threshold.y, (double)takeoff_gear_height());
    t_check((float)fabs(takeoff_centre_line_error(s.g, fs->pos)) < 2.0f, "spawn_on_centre_line", "lat=%.2f",
            (double)takeoff_centre_line_error(s.g, fs->pos));
    dh = (float)fabs(wrap_pi(fs->heading - E_DEG2RAD(b->rwyHeadingDeg)));
    t_check(dh < E_DEG2RAD(1.0f), "spawn_heading", "hdg=%.1f rwy=%.1f", (double)E_RAD2DEG(fs->heading),
            (double)b->rwyHeadingDeg);
    t_check(world_on_runway(s.g, fs->pos.x, fs->pos.z, 1.0f), "spawn_on_runway", NULL);
    t_check(takeoff_message(&s.ts, s.g) && !strcmp(takeoff_message(&s.ts, s.g), "A: TAKEOFF"), "ready_message", NULL);
    run_sec(&s, 5.0f);                              /* nothing pressed: stays parked */
    t_check(s.ts.phase == TO_READY && fs->V == 0.0f && world_status(PLAYER_ID)->pullUp == 0, "ready_waits",
            "phase=%s V=%.1f", takeoff_phase_name(s.ts.phase), (double)fs->V);
}

/* tap or hold A; mode 0 Expert, 1 Novice; returns A -> handover seconds */
static float t_takeoff_case(const char *name, int holdA, int novice)
{
    Sim s;
    u32 seq0;
    float tHand, tLift, left;
    int done, ok;
    char what[64];
    static const int order[] = { WEV_TO_ROLL, WEV_TO_LIFTOFF, WEV_GEAR_UP, WEV_HANDOVER };
    const FlightState *fs;
    pilot_set_mode(novice ? PILOT_NOVICE : PILOT_EXPERT);
    sim_start(&s, 1);
    seq0 = world_event_seq();
    fs = &s.g->ac[PLAYER_ID].fs;
    run_sec(&s, 1.0f);
    s.keys.fast = 1;
    if (!holdA) { run_sec(&s, 0.2f); s.keys.fast = 0; }
    done = sim_until_done(&s, 60.0f);
    tHand = since_a(&s, s.ts.tickHand);
    tLift = since_a(&s, s.ts.tickLift);
    left = runway_left(s.g);
    e_snprintf(what, (int)sizeof(what), "%s_done", name);
    t_check(done, what, "phase=%s alive=%d t=%.1f", takeoff_phase_name(s.ts.phase),
            (s.g->ac[PLAYER_ID].flags & AC_ALIVE) != 0, (double)sim_t(&s));
    e_snprintf(what, (int)sizeof(what), "%s_handover_20_35s", name);
    t_check(tHand >= 20.0f && tHand <= 35.0f, what, "a_to_handover=%.2f a_to_liftoff=%.2f", (double)tHand, (double)tLift);
    e_snprintf(what, (int)sizeof(what), "%s_liftoff_on_runway", name);
    t_check(s.ts.liftDist < left, what, "roll=%.0f m runway_left=%.0f m", (double)s.ts.liftDist, (double)left);
    e_snprintf(what, (int)sizeof(what), "%s_centre_line_lt_2m", name);
    t_check(s.ts.maxLat < 2.0f, what, "max_lat=%.3f", (double)s.ts.maxLat);
    e_snprintf(what, (int)sizeof(what), "%s_no_ground_contact", name);
    t_check(s.ts.minClear > world_crash_clear() && !(s.bits & WEV_CRASH_ANY), what, "min_clear=%.2f crash_clear=%.2f bits=%d",
            (double)s.ts.minClear, (double)world_crash_clear(), s.bits);
    e_snprintf(what, (int)sizeof(what), "%s_handover_state", name);
    t_check(!s.ts.overridden && fs->V >= 299.0f * KT2MS && world_status(PLAYER_ID)->agl >= 1490.0f * FT2M, what,
            "V=%.0fkt agl=%.0fft override=%d", (double)(fs->V * MS2KT), (double)(world_status(PLAYER_ID)->agl * M2FT),
            s.ts.overridden);
    e_snprintf(what, (int)sizeof(what), "%s_events", name);
    ok = event_order_ok(seq0, order, E_COUNT(order)) && count_events(seq0, WEV_PULLUP) == 0;
    t_check(ok, what, "pullups=%d", count_events(seq0, WEV_PULLUP));
    e_snprintf(what, (int)sizeof(what), "%s_handover_continuous", name);
    t_check(s.maxDUp < 0.1f && s.maxDUr < 0.1f, what, "max_dup=%.3f max_dur=%.3f", (double)s.maxDUp, (double)s.maxDUr);
    /* hands off for 20 s after the handover: still flying, gear up, no crash */
    run_sec(&s, 20.0f);
    e_snprintf(what, (int)sizeof(what), "%s_after_handover", name);
    t_check((s.g->ac[PLAYER_ID].flags & AC_ALIVE) && fs->gearPos == 0.0f && fs->pos.y > 300.0f && !s.ts.gearMoving, what,
            "alive=%d gear=%.2f alt=%.0fft gamma=%.1f", (s.g->ac[PLAYER_ID].flags & AC_ALIVE) != 0, (double)fs->gearPos,
            (double)(fs->pos.y * M2FT), (double)E_RAD2DEG(fs->gamma));
    if (novice) {                                   /* Novice holds the climb angle it was handed (bound) */
        e_snprintf(what, (int)sizeof(what), "%s_holds_climb_angle", name);
        t_check((float)fabs(fs->gamma - E_DEG2RAD(param_climb_deg())) < E_DEG2RAD(2.0f), what, "gamma=%.1f",
                (double)E_RAD2DEG(fs->gamma));
    }
    log_printf(LOG_INFO, "test: takeoff case=%s a_to_liftoff=%.2f a_to_handover=%.2f roll_m=%.0f max_lat=%.3f min_clear=%.2f",
               name, (double)tLift, (double)tHand, (double)s.ts.liftDist, (double)s.ts.maxLat, (double)s.ts.minClear);
    pilot_set_mode(PILOT_EXPERT);
    return tHand;
}

/* hold Up (pitch) from liftoff: no handover below overrideMinFt; handover once there. bind 0 = unbound Expert */
static void t_override_200(const char *name, int novice, int bind)
{
    Sim s;
    float aglHand = 0.0f;
    int early = 0;
    char what[64];
    pilot_set_mode(novice ? PILOT_NOVICE : PILOT_EXPERT);
    sim_start(&s, bind);
    run_sec(&s, 0.5f);
    s.keys.fast = 1;
    run_sec(&s, 0.2f);
    s.keys.fast = 0;
    while (s.ts.phase < TO_CLIMB && sim_t(&s) < 30.0f) sim_tick(&s);
    s.keys.up = 1;                                  /* pull from liftoff on */
    while (s.ts.phase == TO_CLIMB && sim_t(&s) < 60.0f) {
        sim_tick(&s);
        if (s.ts.phase == TO_CLIMB && world_status(PLAYER_ID)->agl >= 200.0f * FT2M + 15.0f) early = 1;
    }
    aglHand = world_agl(s.g, s.g->ac[PLAYER_ID].fs.pos) * M2FT;
    e_snprintf(what, (int)sizeof(what), "%s_handover_at_200ft", name);
    t_check(s.ts.phase >= TO_HANDOVER && s.ts.overridden && aglHand >= 199.0f && aglHand < 260.0f && !early, what,
            "phase=%s override=%d agl=%.0fft late=%d", takeoff_phase_name(s.ts.phase), s.ts.overridden, (double)aglHand, early);
    /* the pull is the player's now: the path pitch goes up */
    {
        float g0 = s.g->ac[PLAYER_ID].fs.gamma;
        run_sec(&s, 1.0f);
        e_snprintf(what, (int)sizeof(what), "%s_player_has_control", name);
        t_check(s.g->ac[PLAYER_ID].fs.gamma > g0 + E_DEG2RAD(3.0f), what, "gamma %.1f -> %.1f",
                (double)E_RAD2DEG(g0), (double)E_RAD2DEG(s.g->ac[PLAYER_ID].fs.gamma));
    }
    s.keys.up = 0;
    run_sec(&s, 5.0f);
    e_snprintf(what, (int)sizeof(what), "%s_gear_up_after_override", name);
    t_check(s.g->ac[PLAYER_ID].fs.gearPos == 0.0f && (s.g->ac[PLAYER_ID].flags & AC_ALIVE), what, "gear=%.2f",
            (double)s.g->ac[PLAYER_ID].fs.gearPos);
    pilot_set_mode(PILOT_EXPERT);
}

/* roll key pressed at 600 ft: handover after overrideSec; a short tap (0.1 s) does not hand over */
static void t_override_high(void)
{
    Sim s;
    u32 t0;
    float dt;
    sim_start(&s, 1);
    run_sec(&s, 0.5f);
    s.keys.fast = 1;
    run_sec(&s, 0.2f);
    s.keys.fast = 0;
    while (s.ts.phase <= TO_CLIMB && world_status(PLAYER_ID)->agl < 600.0f * FT2M && sim_t(&s) < 60.0f) sim_tick(&s);
    s.keys.left = 1;                                /* tap: 0.1 s */
    run_sec(&s, 0.1f);
    s.keys.left = 0;
    run_sec(&s, 0.5f);
    t_check(s.ts.phase == TO_CLIMB, "override_tap_ignored", "phase=%s", takeoff_phase_name(s.ts.phase));
    t0 = s.g->tick;
    s.keys.left = 1;
    while (s.ts.phase == TO_CLIMB && s.g->tick - t0 < 2 * SIM_HZ) sim_tick(&s);
    dt = (float)(s.g->tick - t0) / SIM_HZ;
    t_check(s.ts.phase == TO_HANDOVER && s.ts.overridden && dt >= 0.24f && dt <= 0.30f, "override_hold_025s",
            "phase=%s dt=%.3f", takeoff_phase_name(s.ts.phase), (double)dt);
    run_sec(&s, 0.5f);
    t_check(s.g->ac[PLAYER_ID].fs.bank < -E_DEG2RAD(20.0f), "override_roll_is_players", "bank=%.1f",
            (double)E_RAD2DEG(s.g->ac[PLAYER_ID].fs.bank));
}

/* climb-out centre-line tracking: displaced 20 m right just after liftoff; back within 2 m in under 15 s with
   less than 4 m overshoot (the climb-out stays in the T03 corridor, +-600 m) */
static void t_track(void)
{
    Sim s;
    FlightState *fs;
    const TerrainBase *b;
    float h, lat, over = 0.0f, settle = 0.0f;
    u32 t0;
    sim_start(&s, 1);
    fs = &s.g->ac[PLAYER_ID].fs;
    b = terrain_base(s.g->terrain);
    run_sec(&s, 0.5f);
    s.keys.fast = 1;
    run_sec(&s, 0.2f);
    s.keys.fast = 0;
    while (s.ts.phase < TO_CLIMB && sim_t(&s) < 30.0f) sim_tick(&s);
    run_sec(&s, 2.0f);
    h = E_DEG2RAD(b->rwyHeadingDeg);
    fs->pos.x += 20.0f * (float)cos(h);             /* 20 m to the right of the heading */
    fs->pos.z -= 20.0f * (float)sin(h);
    t0 = s.g->tick;
    while (s.ts.phase == TO_CLIMB && sim_t(&s) < 80.0f) {
        sim_tick(&s);
        lat = takeoff_centre_line_error(s.g, fs->pos);
        if (-lat > over) over = -lat;
        if ((float)fabs(lat) > 2.0f) settle = (float)(s.g->tick - t0) / SIM_HZ;
    }
    t_check(settle < 15.0f && over < 4.0f && (s.g->ac[PLAYER_ID].flags & AC_ALIVE), "track_recovers_20m",
            "settle_2m_s=%.1f overshoot_m=%.2f lat_at_handover=%.2f", (double)settle, (double)over,
            (double)takeoff_centre_line_error(s.g, fs->pos));
}

/* crash at sea after the handover, then the M1/M2 flow: respawn on the runway after wreckSec */
static void t_respawn(void)
{
    Sim s;
    Aircraft *pl;
    int guard = 0;
    sim_start(&s, 1);
    pl = &s.g->ac[PLAYER_ID];
    run_sec(&s, 0.5f);
    s.keys.fast = 1;
    run_sec(&s, 0.2f);
    s.keys.fast = 0;
    sim_until_done(&s, 60.0f);
    s.keys.down = 1;                                /* push into the sea (or the ground) */
    while ((pl->flags & AC_ALIVE) && guard++ < 120 * SIM_HZ) sim_tick(&s);
    s.keys.down = 0;
    t_check(!(pl->flags & AC_ALIVE) && world_status(PLAYER_ID)->crashed && pl->hp == 0.0f, "crash_detected",
            "type=%s", world_event_name(world_status(PLAYER_ID)->crashType));
    t_check(!world_respawn_due(s.g, PLAYER_ID), "wreck_time_not_yet", NULL);
    run_sec(&s, 2.6f);
    t_check(world_respawn_due(s.g, PLAYER_ID), "wreck_time_done", NULL);
    takeoff_park(&s.ts, pl, s.g);
    t_check(s.ts.phase == TO_READY && (pl->flags & AC_ALIVE) && (pl->flags & AC_ONGROUND) && pl->fs.onGround &&
            pl->hp == 100.0f && !world_status(PLAYER_ID)->crashed && world_on_runway(s.g, pl->fs.pos.x, pl->fs.pos.z, 1.0f),
            "respawn_on_runway", "phase=%s", takeoff_phase_name(s.ts.phase));
    /* and the automated takeoff runs again */
    run_sec(&s, 0.5f);
    s.keys.fast = 1;
    run_sec(&s, 0.2f);
    s.keys.fast = 0;
    t_check(sim_until_done(&s, 60.0f), "respawn_takeoff_again", "phase=%s", takeoff_phase_name(s.ts.phase));
}

/* ---------------------------------------------------------------- script (DESIGN 16.2 format) */
static int key_ptr(PilotKeys *k, const char *name, int **out)
{
    if (!e_stricmp(name, "UP")) *out = &k->up;
    else if (!e_stricmp(name, "DOWN")) *out = &k->down;
    else if (!e_stricmp(name, "LEFT")) *out = &k->left;
    else if (!e_stricmp(name, "RIGHT")) *out = &k->right;
    else if (!e_stricmp(name, "A")) *out = &k->fast;
    else if (!e_stricmp(name, "Z")) *out = &k->slow;
    else if (!e_stricmp(name, "SPACE")) *out = &k->fire;
    else if (!e_stricmp(name, "D")) *out = &k->wpnNext;
    else if (!e_stricmp(name, "F")) *out = &k->flare;
    else if (!e_stricmp(name, "C")) *out = &k->view;
    else if (!e_stricmp(name, "V")) *out = &k->look;
    else return 0;
    return 1;
}

static float field_value(const Sim *s, const char *f, int *ok)
{
    const FlightState *fs = &s->g->ac[PLAYER_ID].fs;
    *ok = 1;
    if (!e_stricmp(f, "kt")) return fs->V * MS2KT;
    if (!e_stricmp(f, "agl_ft")) return world_agl(s->g, fs->pos) * M2FT;
    if (!e_stricmp(f, "alt_ft")) return fs->pos.y * M2FT;
    if (!e_stricmp(f, "gear")) return fs->gearPos;
    if (!e_stricmp(f, "lat_m")) return (float)fabs(takeoff_centre_line_error(s->g, fs->pos));
    if (!e_stricmp(f, "onground")) return (float)fs->onGround;
    if (!e_stricmp(f, "alive")) return (s->g->ac[PLAYER_ID].flags & AC_ALIVE) ? 1.0f : 0.0f;
    if (!e_stricmp(f, "pullup")) return (float)world_status(PLAYER_ID)->pullUp;
    if (!e_stricmp(f, "gamma")) return E_RAD2DEG(fs->gamma);
    if (!e_stricmp(f, "pitch")) return E_RAD2DEG(fs->pitch);
    *ok = 0;
    return 0.0f;
}

static int compare(float a, const char *op, float b)
{
    if (!strcmp(op, "==")) return (float)fabs(a - b) < 1e-4f;
    if (!strcmp(op, "!=")) return (float)fabs(a - b) >= 1e-4f;
    if (!strcmp(op, "<")) return a < b;
    if (!strcmp(op, "<=")) return a <= b;
    if (!strcmp(op, ">")) return a > b;
    if (!strcmp(op, ">=")) return a >= b;
    return -1;
}

/* one line: "T keydown NAME" | "T keyup NAME" | "T expect FIELD OP VALUE"; returns 0 on a parse error */
static int script_line(Sim *s, const char *line, int lineNo)
{
    char cmd[16], a[32], op[4], b[32], what[64];
    float t;
    int n, *kp, ok, r, ph;
    float v;
    n = sscanf(line, "%f %15s %31s %3s %31s", &t, cmd, a, op, b);
    if (n < 3) return 0;
    while ((float)s->g->tick < t * (float)SIM_HZ + 1.0f) sim_tick(s);   /* tick 1 = time 0 */
    if (!e_stricmp(cmd, "keydown") || !e_stricmp(cmd, "keyup")) {
        if (!key_ptr(&s->keys, a, &kp)) return 0;
        *kp = !e_stricmp(cmd, "keydown");
        return 1;
    }
    if (e_stricmp(cmd, "expect") || n < 5) return 0;
    e_snprintf(what, (int)sizeof(what), "script_l%d_%s", lineNo, a);
    if (!e_stricmp(a, "phase")) {
        for (ph = TO_READY; ph <= TO_DONE; ph++)
            if (!e_stricmp(b, takeoff_phase_name(ph))) break;
        if (ph > TO_DONE) return 0;
        r = compare((float)s->ts.phase, op, (float)ph);
        if (r < 0) return 0;
        t_check(r, what, "t=%.2f phase=%s %s %s", (double)t, takeoff_phase_name(s->ts.phase), op, b);
        return 1;
    }
    v = field_value(s, a, &ok);
    if (!ok) return 0;
    r = compare(v, op, (float)atof(b));
    if (r < 0) return 0;
    t_check(r, what, "t=%.2f %s=%.2f %s %s", (double)t, a, (double)v, op, b);
    return 1;
}

static void t_script(const char *rel)
{
    char path[E_PATH], line[160], *buf, *p;
    int size, i, lineNo = 0, bad = 0;
    Sim s;
    if (rel[0] == '/' || rel[0] == '\\' || (rel[0] && rel[1] == ':')) e_strlcpy(path, rel, (int)sizeof(path));
    else base_path(path, (int)sizeof(path), rel);
    buf = (char *)file_load(path, &size);
    if (!t_check(buf != NULL, "script_load", "path=%s", path)) return;
    pilot_set_mode(PILOT_EXPERT);
    sim_start(&s, 1);
    log_printf(LOG_INFO, "test: takeoff script=%s", path);
    p = buf;
    while (*p) {
        for (i = 0; *p && *p != '\n' && i < (int)sizeof(line) - 1; p++) if (*p != '\r') line[i++] = *p;
        line[i] = 0;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        lineNo++;
        for (i = 0; line[i] == ' ' || line[i] == '\t'; i++) {}
        if (!line[i] || line[i] == '#') continue;
        if (!script_line(&s, line + i, lineNo)) {
            log_printf(LOG_INFO, "test: takeoff script line %d not understood: %s", lineNo, line + i);
            bad++;
        }
    }
    free(buf);
    t_check(bad == 0, "script_parse", "bad_lines=%d", bad);
}

/* ---------------------------------------------------------------- entry */
int test_takeoff(const char *arg)
{
    static char snap[65536];
    int n, fails;
    Cfg *empty;
    float tTap, tHold;
    t_begin("takeoff");
    n = params_snapshot(snap, (int)sizeof(snap));
    empty = cfg_parse("", 0);
    params_load(empty);
    cfg_free(empty);
    log_printf(LOG_INFO, "test: takeoff params=defaults hash=%08x", params_hash());

    t_spawn();
    tTap = t_takeoff_case("tap", 0, 0);
    tHold = t_takeoff_case("hold_ab", 1, 0);
    t_takeoff_case("novice_tap", 0, 1);
    t_check(tHold < tTap, "afterburner_is_quicker", "tap=%.2f hold=%.2f", (double)tTap, (double)tHold);
    t_override_200("override", 0, 1);
    t_override_200("override_novice", 1, 1);
    t_override_200("override_unbound", 0, 0);
    t_override_high();
    t_track();
    t_respawn();
    t_script(arg && arg[0] ? arg : "tests/t06/takeoff.scr");

    if (n > 0) params_restore(snap, n);
    fails = t_end();
    return fails;
}
