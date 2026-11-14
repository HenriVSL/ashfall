/* tests_world.cpp - -test world (DESIGN 6.2-6.4, 16.1): crash (sea, ground, boxes, aircraft), area bounds and the
   automatic turn-back, PULL UP, air starts, the event queue, timing. Portable. Owner: T06.

   Works on the T01 terrain stub (flat sea, default runway, no boxes) and on the real island: positions over the sea
   are taken at the air spawns (12 km out), the runway from terrain_base(), land points by scanning terrain_height.
   Cases that need land (ground crash, flying into the volcano) log "skipped" on the stub; the box case injects a
   synthetic box over the sea (and uses the first BASE box too when the island has boxes). Code defaults of every
   param (snapshot, empty ini, restore). Lines: "test: world <what> ok|FAIL ...". */
#include "tests.h"
#include "params.h"
#include "takeoff.h"
#include "world.h"
#include "e_terrain.h"
#include "e_file.h"
#include "e_log.h"
#include "e_prof.h"

static Terrain s_ter;
static int s_terInit, s_terOk;
static Game s_g;
static PilotState s_ps[MAX_AIRCRAFT];
static PilotKeys s_keys[MAX_AIRCRAFT];
static int s_bits;

static void terrain_once(void)
{
    char path[E_PATH];
    if (s_terInit) return;
    data_path(path, (int)sizeof(path), "island/jet.isl");
    s_terOk = terrain_load(&s_ter, path);
    s_terInit = 1;
    log_printf(LOG_INFO, "test: world terrain loaded=%d path=%s (0 = T01 stub: flat sea, default runway)", s_terOk, path);
}

static void w_start(int n)
{
    int i;
    terrain_once();
    memset(&s_g, 0, sizeof(s_g));
    s_g.terrain = &s_ter;
    s_g.numAircraft = n;
    s_g.tick = 1;
    for (i = 0; i < MAX_AIRCRAFT; i++) {
        s_g.ac[i].id = i;
        s_g.ac[i].side = i ? SIDE_RED : SIDE_BLUE;
        s_g.ac[i].flags = i < n ? AC_ALIVE : 0;
        s_g.ac[i].hp = 100.0f;
        pilot_reset(&s_ps[i]);
        memset(&s_keys[i], 0, sizeof(s_keys[i]));
    }
    s_g.ac[0].flags |= AC_PLAYER;
    s_bits = 0;
    world_reset(&s_g);
}

static void place(int i, Vec3 pos, float hdgDeg, float gamDeg, float Vkt)
{
    Aircraft *a = &s_g.ac[i];
    flight_init(&a->fs, pos, E_DEG2RAD(hdgDeg), Vkt * KT2MS);
    flight_set_path(&a->fs, E_DEG2RAD(hdgDeg), E_DEG2RAD(gamDeg), 0.0f);
    a->prev = a->fs;
}

/* one tick as main does it (DESIGN 2.6 steps 1, 4, 5, 6) */
static void w_tick(void)
{
    int i;
    PilotInput in;
    for (i = 0; i < s_g.numAircraft; i++) {
        Aircraft *a = &s_g.ac[i];
        if (!(a->flags & AC_ALIVE)) continue;
        pilot_update_fs(&s_ps[i], &s_keys[i], &a->fs, &in);
        world_override_input(&s_g, i, &in);
        a->in = in;
    }
    for (i = 0; i < s_g.numAircraft; i++) {
        Aircraft *a = &s_g.ac[i];
        a->prev = a->fs;
        if (a->flags & AC_ALIVE) flight_step(&a->fs, &a->in, SIM_DT);
    }
    s_bits |= world_check(&s_g);
    s_g.tick++;
}

static int run_until_crash(int i, float maxSec)
{
    u32 end = s_g.tick + (u32)(maxSec * SIM_HZ);
    while (s_g.tick < end && (s_g.ac[i].flags & AC_ALIVE)) w_tick();
    return !(s_g.ac[i].flags & AC_ALIVE);
}

static void run_sec(float sec)
{
    u32 end = s_g.tick + (u32)(sec * SIM_HZ);
    while (s_g.tick < end) w_tick();
}

static int count_events(u32 seq0, int type, int ac)
{
    u32 i;
    int n = 0;
    WorldEvent ev;
    for (i = seq0; i < world_event_seq(); i++)
        if (world_event_get(i, &ev) && ev.type == type && (ac < 0 || ev.ac == ac)) n++;
    return n;
}

static Vec3 sea_point(void)                         /* over open sea: air spawn 0's ground position */
{
    Vec3 p = terrain_base(&s_ter)->airSpawn[0];
    p.y = 0.0f;
    return p;
}

/* highest land point (256 m scan of the +-16 km world); returns its height */
static float land_peak(Vec3 *out)
{
    float x, z, h, best = -1e9f;
    for (z = -16000.0f; z <= 16000.0f; z += 256.0f)
        for (x = -16000.0f; x <= 16000.0f; x += 256.0f) {
            h = terrain_height(&s_ter, x, z);
            if (h > best) { best = h; *out = v3(x, h, z); }
        }
    return best;
}

/* ---------------------------------------------------------------- cases */
static void t_queue(void)
{
    int i;
    WorldEvent ev;
    w_start(1);
    for (i = 0; i < WORLD_EVENTS_MAX + 6; i++) world_event_push(&s_g, WEV_AREA_OK, i % 3, -1, v3((float)i, 0, 0));
    t_check(world_event_seq() == (u32)(WORLD_EVENTS_MAX + 6), "queue_seq", "seq=%u", world_event_seq());
    t_check(!world_event_get(0, &ev) && !world_event_get(5, &ev), "queue_overwritten_old", NULL);
    t_check(world_event_get(6, &ev) && ev.pos.x == 6.0f && world_event_get(world_event_seq() - 1, &ev) &&
            ev.pos.x == (float)(WORLD_EVENTS_MAX + 5), "queue_keeps_last", NULL);
    t_check(!world_event_get(world_event_seq(), &ev), "queue_future_empty", NULL);
    world_reset(&s_g);
    t_check(world_event_seq() == 0, "queue_reset", NULL);
}

static void t_air_start(void)
{
    Aircraft *a;
    float cx, cz, brg, err;
    w_start(1);
    a = &s_g.ac[0];
    a->flags = AC_PLAYER;                           /* dead: air start must revive */
    world_air_start(&s_g, a, v3(0.0f, 0.0f, 0.0f));
    world_centre(&s_g, &cx, &cz);
    brg = (float)atan2(cx - a->fs.pos.x, cz - a->fs.pos.z);
    err = (float)fabs(wrap_pi(brg - a->fs.heading));
    t_check((a->flags & AC_ALIVE) && !(a->flags & AC_ONGROUND) && !a->fs.onGround && a->fs.gearPos == 0.0f, "air_start_state",
            "flags=%d gear=%.2f", a->flags, (double)a->fs.gearPos);
    t_check(a->fs.pos.y >= 1500.0f && a->fs.pos.y <= 2500.0f, "air_start_altitude", "y=%.0f", (double)a->fs.pos.y);
    t_check((float)fabs(a->fs.V - flight_speed_target(THR_NORMAL, a->fs.pos.y)) < 0.5f, "air_start_normal_speed",
            "V=%.0fkt", (double)(a->fs.V * MS2KT));
    t_check(err < E_DEG2RAD(5.0f) && (float)fabs(a->fs.bank) < 1e-3f, "air_start_towards_centre", "err=%.1f deg",
            (double)E_RAD2DEG(err));
    log_printf(LOG_INFO, "test: world centre=%.0f,%.0f", (double)cx, (double)cz);
}

static void t_crash_sea(void)
{
    Vec3 p = sea_point();
    u32 seq0;
    Aircraft *a;
    int crashed;
    w_start(1);
    a = &s_g.ac[0];
    /* low level over the sea is allowed: 3 m clearance, level, 5 s */
    place(0, v3(p.x, 3.5f, p.z), 0.0f, 0.0f, 400.0f);
    run_sec(5.0f);
    t_check((a->flags & AC_ALIVE) != 0, "sea_low_level_ok", "y=%.2f", (double)a->fs.pos.y);
    w_start(1);
    seq0 = world_event_seq();
    place(0, v3(p.x, 300.0f, p.z), 90.0f, -45.0f, 400.0f);
    crashed = run_until_crash(0, 20.0f);
    t_check(crashed && world_status(0)->crashType == WEV_CRASH_SEA && (s_bits & WEV_CRASH_SEA), "crash_sea",
            "crashed=%d type=%s bits=%d", crashed, world_event_name(world_status(0)->crashType), s_bits);
    t_check(a->hp == 0.0f && count_events(seq0, WEV_CRASH_SEA, 0) == 1 && a->fs.pos.y >= 0.0f &&
            a->fs.pos.y < world_crash_clear(), "crash_sea_wreck", "hp=%.0f y=%.2f", (double)a->hp, (double)a->fs.pos.y);
    run_sec(1.0f);                                  /* a wreck stays put, no repeated events */
    t_check(count_events(seq0, WEV_CRASH_SEA, 0) == 1, "crash_once", NULL);
}

static void t_crash_ground(void)
{
    Vec3 pk;
    float h = land_peak(&pk);
    int crashed;
    if (h < 50.0f) {
        log_printf(LOG_INFO, "test: world crash_ground skipped (no land above 50 m: terrain stub) peak=%.0f", (double)h);
        return;
    }
    w_start(1);
    place(0, v3(pk.x, h + 300.0f, pk.z), 0.0f, -70.0f, 350.0f);
    crashed = run_until_crash(0, 20.0f);
    t_check(crashed && world_status(0)->crashType == WEV_CRASH_GROUND, "crash_ground", "type=%s peak=%.0f at %.0f,%.0f",
            world_event_name(world_status(0)->crashType), (double)h, (double)pk.x, (double)pk.z);
}

static void t_crash_box(void)
{
    TerrainBase *b = (TerrainBase *)terrain_base(&s_ter);   /* test-only: inject a box */
    Vec3 p = sea_point();
    int n0 = b->nBoxes, crashed;
    Aabb box;
    /* synthetic box over the sea: 40 x 14 x 30 m at 400 m */
    box.lo = v3(p.x - 20.0f, 400.0f, p.z - 15.0f);
    box.hi = v3(p.x + 20.0f, 414.0f, p.z + 15.0f);
    if (n0 < TERRAIN_MAX_BOXES) {
        b->box[n0] = box;
        b->nBoxes = n0 + 1;
        w_start(1);
        place(0, v3(p.x, 600.0f, p.z), 0.0f, -88.0f, 300.0f);
        crashed = run_until_crash(0, 20.0f);
        t_check(crashed && world_status(0)->crashType == WEV_CRASH_BOX && s_g.ac[0].fs.pos.y > 405.0f, "crash_box",
                "type=%s y=%.1f", world_event_name(world_status(0)->crashType), (double)s_g.ac[0].fs.pos.y);
        w_start(1);                                 /* fly level 8 m over its top: sphere 5 m clears */
        place(0, v3(p.x, 422.0f, p.z - 600.0f), 0.0f, 0.0f, 300.0f);
        run_sec(5.0f);
        t_check((s_g.ac[0].flags & AC_ALIVE) != 0, "box_near_miss", NULL);
        b->nBoxes = n0;
    }
    if (n0 > 0) {                                   /* a real BASE box (hangar/tower/tank) from above */
        Vec3 c = v3_scale(v3_add(b->box[0].lo, b->box[0].hi), 0.5f);
        w_start(1);
        place(0, v3(c.x, b->box[0].hi.y + 150.0f, c.z), 0.0f, -88.0f, 250.0f);
        crashed = run_until_crash(0, 20.0f);
        t_check(crashed && world_status(0)->crashType == WEV_CRASH_BOX, "crash_base_box", "type=%s boxes=%d",
                world_event_name(world_status(0)->crashType), n0);
    } else {
        log_printf(LOG_INFO, "test: world crash_base_box skipped (BASE has no boxes: terrain stub)");
    }
}

static void t_crash_air(void)
{
    Vec3 p = sea_point();
    u32 seq0;
    int both;
    WorldEvent ev;
    u32 i;
    int otherOk = 1;
    /* head-on, 4 m apart sideways, 600 kt each */
    w_start(2);
    seq0 = world_event_seq();
    place(0, v3(p.x, 2000.0f, p.z - 1500.0f), 0.0f, 0.0f, 600.0f);
    place(1, v3(p.x + 4.0f, 2000.0f, p.z + 1500.0f), 180.0f, 0.0f, 600.0f);
    run_sec(10.0f);
    both = !(s_g.ac[0].flags & AC_ALIVE) && !(s_g.ac[1].flags & AC_ALIVE);
    for (i = seq0; i < world_event_seq(); i++)
        if (world_event_get(i, &ev) && ev.type == WEV_CRASH_AIR && ev.other != 1 - ev.ac) otherOk = 0;
    t_check(both && count_events(seq0, WEV_CRASH_AIR, -1) == 2 && otherOk && (s_bits & WEV_CRASH_AIR), "crash_air_both",
            "alive0=%d alive1=%d events=%d", (s_g.ac[0].flags & AC_ALIVE) != 0, (s_g.ac[1].flags & AC_ALIVE) != 0,
            count_events(seq0, WEV_CRASH_AIR, -1));
    /* near miss: 30 m apart */
    w_start(2);
    place(0, v3(p.x, 2000.0f, p.z - 1500.0f), 0.0f, 0.0f, 600.0f);
    place(1, v3(p.x + 30.0f, 2000.0f, p.z + 1500.0f), 180.0f, 0.0f, 600.0f);
    run_sec(10.0f);
    t_check((s_g.ac[0].flags & AC_ALIVE) && (s_g.ac[1].flags & AC_ALIVE), "air_near_miss", NULL);
    /* swept: the two tick segments cross although both end points are 50 m apart (no tunnelling) */
    w_start(2);
    place(0, v3(p.x, 2000.0f, p.z), 0.0f, 0.0f, 600.0f);
    place(1, v3(p.x + 3.0f, 2000.0f, p.z), 180.0f, 0.0f, 600.0f);
    s_g.ac[0].prev.pos = v3(p.x, 2000.0f, p.z - 25.0f);
    s_g.ac[0].fs.pos = v3(p.x, 2000.0f, p.z + 25.0f);
    s_g.ac[1].prev.pos = v3(p.x + 3.0f, 2000.0f, p.z + 25.0f);
    s_g.ac[1].fs.pos = v3(p.x + 3.0f, 2000.0f, p.z - 25.0f);
    world_check(&s_g);
    t_check(!(s_g.ac[0].flags & AC_ALIVE) && !(s_g.ac[1].flags & AC_ALIVE), "crash_air_swept", NULL);
}

/* fly out of the area: warning at areaWarnKm, turn-back at areaTurnKm (inputs overridden), back in; start 1 km
   inside the warning ring, overshoot past the turn-back ring < 4 km (8 km from a 950 kt run) */
static void t_bounds(int fast)
{
    const Param *pw = params_find("world", "areaWarnKm"), *pt = params_find("world", "areaTurnKm");
    float warnM = pw ? *(const float *)pw->ptr * 1000.0f : 26000.0f, turnM = pt ? *(const float *)pt->ptr * 1000.0f : 32000.0f;
    float cx, cz, maxD = 0.0f, d;
    u32 seq0, end;
    int tbTicks = 0, thrOk = 1, ended = 0, tbPrev = 0;
    const WorldStatus *w;
    char what[48];
    w_start(1);
    world_centre(&s_g, &cx, &cz);
    seq0 = world_event_seq();
    place(0, v3(cx + warnM - 1000.0f, 3000.0f, cz), 90.0f, 0.0f, fast ? 950.0f : 0.8f * speed_of_sound(3000.0f) * MS2KT);
    s_keys[0].fast = fast;
    w = world_status(0);
    end = s_g.tick + 400 * SIM_HZ;
    while (s_g.tick < end && (s_g.ac[0].flags & AC_ALIVE)) {
        s_keys[0].right = w->turnback && tbTicks < 5 * SIM_HZ;   /* the player fights the turn-back for 5 s: ignored */
        tbPrev = w->turnback;                       /* override active for this tick's input */
        w_tick();
        d = w->distCentre;
        if (d > maxD) maxD = d;
        if (tbPrev) { tbTicks++; if (s_g.ac[0].in.thr == THR_FAST) thrOk = 0; }
        if (count_events(seq0, WEV_TURNBACK_END, 0)) ended = 1;
        if (ended && count_events(seq0, WEV_AREA_OK, 0)) break;
    }
    e_snprintf(what, (int)sizeof(what), "bounds%s_events", fast ? "_fast" : "");
    t_check(count_events(seq0, WEV_RETURN_AREA, 0) == 1 && count_events(seq0, WEV_TURNBACK, 0) == 1 &&
            count_events(seq0, WEV_TURNBACK_END, 0) == 1 && count_events(seq0, WEV_AREA_OK, 0) == 1, what,
            "warn=%d turn=%d end=%d ok=%d", count_events(seq0, WEV_RETURN_AREA, 0), count_events(seq0, WEV_TURNBACK, 0),
            count_events(seq0, WEV_TURNBACK_END, 0), count_events(seq0, WEV_AREA_OK, 0));
    e_snprintf(what, (int)sizeof(what), "bounds%s_max_distance", fast ? "_fast" : "");
    t_check(maxD < turnM + (fast ? 8000.0f : 4000.0f) && (s_g.ac[0].flags & AC_ALIVE), what, "max=%.0f m turnback_s=%.1f",
            (double)maxD, (double)tbTicks / SIM_HZ);
    if (fast) t_check(thrOk, "bounds_fast_no_ab_in_turnback", NULL);
    log_printf(LOG_INFO, "test: world bounds fast=%d max_km=%.2f turnback_s=%.1f", fast, (double)(maxD / 1000.0f),
               (double)tbTicks / SIM_HZ);
}

/* actual time to impact from now on with no input (copy of the state, straight on) */
static float actual_tti(const FlightState *fs0)
{
    FlightState fs = *fs0;
    PilotInput in;
    int i;
    memset(&in, 0, sizeof(in));
    in.thr = fs.thr;
    for (i = 0; i < 30 * SIM_HZ; i++) {
        if (world_agl(&s_g, fs.pos) < world_crash_clear()) return (float)i / SIM_HZ;
        flight_step(&fs, &in, SIM_DT);
    }
    return 99.0f;
}

static void t_pullup(void)
{
    Vec3 p = sea_point(), pk;
    const TerrainBase *b = terrain_base(&s_ter);
    const WorldStatus *w = world_status(0);
    u32 seq0, end;
    float tti = -1.0f, h, rh;
    int crashed;
    /* a) 30 deg dive at the sea: warning ~4 s before impact; pulling clears it */
    w_start(1);
    seq0 = world_event_seq();
    place(0, v3(p.x, 1500.0f, p.z), 0.0f, -30.0f, 500.0f);
    end = s_g.tick + 30 * SIM_HZ;
    while (s_g.tick < end && (s_g.ac[0].flags & AC_ALIVE) && !w->pullUp) w_tick();
    if (w->pullUp) tti = actual_tti(&s_g.ac[0].fs);
    t_check(w->pullUp && tti >= 3.0f && tti <= 4.6f, "pullup_dive_warns_at_4s", "on=%d actual_tti=%.2f predicted=%.2f",
            w->pullUp, (double)tti, (double)w->pullUpT);
    s_keys[0].up = 1;
    run_sec(4.0f);
    s_keys[0].up = 0;
    t_check((s_g.ac[0].flags & AC_ALIVE) && !w->pullUp && count_events(seq0, WEV_PULLUP, 0) == 1 &&
            count_events(seq0, WEV_PULLUP_OFF, 0) == 1 && (s_bits & WEV_PULLUP), "pullup_cleared_by_pull",
            "alive=%d on=%d gamma=%.1f", (s_g.ac[0].flags & AC_ALIVE) != 0, w->pullUp, (double)E_RAD2DEG(s_g.ac[0].fs.gamma));
    /* b) level at 100 ft over the sea: no warning */
    w_start(1);
    seq0 = world_event_seq();
    place(0, v3(p.x, 100.0f * FT2M, p.z), 0.0f, 0.0f, 450.0f);
    run_sec(10.0f);
    t_check((s_g.ac[0].flags & AC_ALIVE) && count_events(seq0, WEV_PULLUP, 0) == 0, "pullup_level_low_quiet", NULL);
    /* c) shallow descent over the runway below 200 ft: suppressed; the same over the sea: warns */
    rh = b->threshold.y;
    w_start(1);
    seq0 = world_event_seq();
    place(0, v3(b->rwyX, rh + 45.0f, b->rwyZ), b->rwyHeadingDeg, -4.0f, 200.0f);
    run_sec(3.0f);
    t_check(count_events(seq0, WEV_PULLUP, 0) == 0 && w->pullUpT <= 4.0f, "pullup_runway_low_suppressed",
            "predicted=%.2f", (double)w->pullUpT);
    w_start(1);
    seq0 = world_event_seq();
    place(0, v3(p.x, 45.0f, p.z), 0.0f, -4.0f, 200.0f);
    run_sec(3.0f);
    t_check(count_events(seq0, WEV_PULLUP, 0) == 1, "pullup_same_over_sea_warns", NULL);
    /* d) takeoff phases suppress it */
    w_start(1);
    seq0 = world_event_seq();
    world_set_takeoff(0, 1);
    place(0, v3(p.x, 400.0f, p.z), 0.0f, -30.0f, 400.0f);
    run_sec(2.0f);
    t_check(count_events(seq0, WEV_PULLUP, 0) == 0, "pullup_takeoff_suppressed", "predicted=%.2f", (double)w->pullUpT);
    world_set_takeoff(0, 0);
    run_sec(0.6f);
    t_check(w->pullUp == 1, "pullup_after_takeoff_armed", NULL);
    /* e) level into the volcano (needs land) */
    h = land_peak(&pk);
    if (h >= 300.0f) {
        Vec3 st = v3(pk.x, h - 150.0f, pk.z - 5000.0f);
        u32 tw = 0;
        w_start(1);
        seq0 = world_event_seq();
        place(0, st, 0.0f, 0.0f, 450.0f);
        while ((s_g.ac[0].flags & AC_ALIVE) && s_g.tick < 120 * SIM_HZ) {
            w_tick();
            if (!tw && w->pullUp) tw = s_g.tick;
        }
        crashed = !(s_g.ac[0].flags & AC_ALIVE);
        t_check(crashed && tw && (float)(s_g.tick - tw) / SIM_HZ >= 2.5f, "pullup_terrain_warns_before_impact",
                "crashed=%d type=%s warn_s_before=%.2f", crashed, world_event_name(w->crashType),
                tw ? (double)(s_g.tick - tw) / SIM_HZ : -1.0);
    } else {
        log_printf(LOG_INFO, "test: world pullup_terrain skipped (no land above 300 m: terrain stub)");
    }
}

static float time_check(int n, Vec3 p, int ticks)
{
    int i;
    s64 t0;
    w_start(n);
    for (i = 0; i < n; i++) place(i, v3(p.x + 500.0f * i, 2000.0f, p.z), 0.0f, 0.0f, 500.0f);
    t0 = prof_now_us();
    for (i = 0; i < ticks; i++) { world_check(&s_g); s_g.tick++; }
    return (float)(prof_now_us() - t0) / (float)ticks;
}

static void t_timing(void)
{
    Vec3 p = sea_point(), land = v3(-2000.0f, 0.0f, -3000.0f);
    int i, n = 2000;
    s64 t0;
    float us1, us6, usLand, usCrash, usPred, usH, acc = 0.0f;
    us1 = time_check(1, p, 1200);
    us6 = time_check(6, p, 1200);
    usLand = time_check(6, land, 1200);             /* over the island (inside the height grid) */
    t0 = prof_now_us();
    for (i = 0; i < n; i++) acc += (float)world_crash_test(&s_g, v3(land.x + (float)i, 500.0f, land.z));
    usCrash = (float)(prof_now_us() - t0) / (float)n;
    t0 = prof_now_us();
    for (i = 0; i < n; i++) acc += terrain_height(&s_ter, land.x + (float)i, land.z);
    usH = (float)(prof_now_us() - t0) / (float)n;
    t0 = prof_now_us();
    for (i = 0; i < 200; i++) acc += world_pullup_predict(&s_g, &s_g.ac[0].fs);
    usPred = (float)(prof_now_us() - t0) / 200.0f;
    log_printf(LOG_INFO, "test: world timing us_per_tick=%.2f (1 aircraft) %.2f (6, sea) %.2f (6, island) crash_test=%.2f "
               "terrain_height=%.2f pullup_predict=%.2f (2 Hz, player only) acc=%.0f", (double)us1, (double)us6,
               (double)usLand, (double)usCrash, (double)usH, (double)usPred, (double)acc);
    t_check(usLand <= 60.0f, "timing_tick_6ac_le_60us", "us=%.2f", (double)usLand);
}

int test_world(const char *arg)
{
    static char snap[65536];
    int n, fails;
    Cfg *empty;
    (void)arg;
    t_begin("world");
    n = params_snapshot(snap, (int)sizeof(snap));
    empty = cfg_parse("", 0);
    params_load(empty);
    cfg_free(empty);
    log_printf(LOG_INFO, "test: world params=defaults hash=%08x", params_hash());
    pilot_set_mode(PILOT_EXPERT);

    t_queue();
    t_air_start();
    t_crash_sea();
    t_crash_ground();
    t_crash_box();
    t_crash_air();
    t_bounds(0);
    t_bounds(1);
    t_pullup();
    t_timing();

    if (n > 0) params_restore(snap, n);
    fails = t_end();
    return fails;
}
