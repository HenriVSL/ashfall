/* tests_mission.cpp - -test mission (DESIGN 14, 16.1). Portable. Owner: T17 (T20 extends).
   Runs the real session (session_start / session_tick: pilot, takeoff, AI, flight, world, weapons, radar, mission)
   on the real island (data/island/jet.isl; flat sea without it) with the mission director on and the code defaults
   for every param. Parts (arg = one part name or none for all):
     training  3 drones on their circuits (state DRONE, altitude held), a drone killed by a player LRM comes back
               trainRespawnSec later on its circuit, the player's missiles are topped up (unlimited)
     sensor    weapons fire at the RADAR designation (T16 item 3): with a nearer aircraft dead ahead, an LRM launched
               with the TD on the other one goes to the TD
     damage    three enemy LRM hits: hp 66 -> 32 (+ DAMAGED) -> dead; wreck 2.5 s, then the runway with hp 100,
               invulnerable through the takeoff, lives 3 -> 2; hostile missiles at the dead player self-destruct;
               a hit while invulnerable changes nothing
     sortie    wave 1 = 2 bandits (Normal) spawnMinKm-spawnMaxKm (45-55 km, T21a) out at 4-6 km with the "WAVE 1" message after the handover; both
               shot down -> MISSION COMPLETE 3 s later, score, result line; sortieWaves 2 -> wave 2 8 s after the clear
     lives     lives 2: two crashes -> MISSION FAILED
     robot     headless robot sortie (ai_robot_hook, A held during the takeoff) for 300 s: no robot/enemy terrain crash,
               a result; the same run twice gives the same hash (determinism with the director)
   T20:
     score     DESIGN 14.4 scoring examples through mission_score (pure): kills, BVR, deaths, time bonus only on
               COMPLETE, accuracy, clamp at 0, the default timeBonusRefSec
     waves     the full 3-wave SORTIE per difficulty (sortieWaves default 3): wave sizes 2,3,3 / 2,3,4 / 3,4,5, max 5
               alive, spawn 45-55 km / 4-6 km, "WAVE n: k BANDITS BRG xxx", "WAVE n CLEARED", next wave 8 s after a
               clear, MISSION COMPLETE after wave 3, live score = mission_score(stats), mission_results fields
     records   records.ini (a test file): missing -> no rows, submit = new record + CRLF file, reload equal, second
               submit of the same mission = no change, lower score no record, higher score replaces it, corrupt
               file / bad rows ignored, reset */
#include "tests.h"
#include "e_log.h"
#include "e_file.h"
#include "e_terrain.h"
#include "params.h"
#include "mission.h"
#include "world.h"
#include "takeoff.h"
#include "radar.h"
#include "weapons.h"
#include "missile.h"
#include "dlz.h"
#include "ai.h"
#include <stdio.h>
#include <stdlib.h>

static Terrain s_ter;
static int s_terInit, s_terOk;
static Game s_mg;

static Terrain *ter(void)
{
    char path[E_PATH];
    if (!s_terInit) {
        data_path(path, (int)sizeof(path), "island/jet.isl");
        s_terOk = terrain_load(&s_ter, path);
        s_terInit = 1;
        log_printf(LOG_INFO, "test: mission terrain loaded=%d path=%s", s_terOk, path);
    }
    return s_terOk ? &s_ter : NULL;
}

static void ms_start(int mode, int diff, int start, u32 seed)
{
    mission_set_director(1);
    session_start(&s_mg, ter(), mode, diff, start, seed);
}

/* one session tick; holdA = hold A while a takeoff phase runs (READY needs a throttle-up) */
static void ms_tick(int holdA, int cmd)
{
    PilotKeys k;
    memset(&k, 0, sizeof(k));
    if (holdA && takeoff_active(session_takeoff())) k.fast = 1;
    session_tick(&s_mg, &k, cmd);
}

static void ms_run(float sec, int holdA)
{
    u32 n = (u32)(sec * (float)SIM_HZ + 0.5f), i;
    for (i = 0; i < n; i++) ms_tick(holdA, 0);
}

static void place(int id, Vec3 pos, float hdg, float V)
{
    Aircraft *a = &s_mg.ac[id];
    flight_init(&a->fs, pos, hdg, V);
    a->fs.onGround = 0;
    a->fs.gearPos = 0.0f;
    a->fs.thr = THR_NORMAL;
    a->prev = a->fs;
    a->in.thr = THR_NORMAL;
    world_reset_aircraft(id);
}

/* a point over the open sea west of the island at altitude y */
static Vec3 sea_point(float y)
{
    float cx = 1200.0f, cz = 1800.0f;
    if (ter()) world_centre(&s_mg, &cx, &cz);
    return v3(cx - 50000.0f, y, cz);
}

static int count_alive_enemies_t(void)
{
    int i, n = 0;
    for (i = 1; i < s_mg.numAircraft; i++) if (s_mg.ac[i].flags & AC_ALIVE) n++;
    return n;
}

static int count_msgs(const char *prefix, u32 from)
{
    u32 i;
    int n = 0;
    for (i = from; i < mission_msg_count(); i++) {
        const char *m = mission_msg(i, NULL);
        if (m && !strncmp(m, prefix, strlen(prefix))) n++;
    }
    return n;
}

static int find_event(u32 from, int type, int owner, int *victim)
{
    u32 i;
    for (i = from; i < weapons_event_count(); i++) {
        const WpnEvent *e = weapons_event(i);
        if (e && e->type == type && (owner < 0 || e->owner == owner)) { if (victim) *victim = e->victim; return 1; }
    }
    return 0;
}

/* shooter designates target, waits two ticks (weapons reads the sensor in its tick), fires one LRM */
static int fire_lrm(int shooter, int target)
{
    int i, slot;
    weapons_set_stores(shooter, 10, 10, 0);
    weapons_select(shooter, MSL_LRM);
    radar_designate(&s_mg, shooter, target);
    for (i = 0; i < 2; i++) ms_tick(0, 0);
    slot = weapons_fire(&s_mg, shooter);
    return slot;
}

/* ---------------------------------------------------------------- training */
static void t_training(void)
{
    int i, ok = 1, drones = 0, slot, victim = -1, back;
    float maxAltErr = 0.0f;
    u32 ev;
    Vec3 p;
    ms_start(MODE_TRAINING, DIFF_NORMAL, START_AIR, 17u);
    for (i = 1; i < s_mg.numAircraft; i++)
        if ((s_mg.ac[i].flags & AC_ALIVE) && s_mg.ac[i].side == SIDE_RED && ai_state(i) == AIS_DRONE) drones++;
    t_check(drones == 3 && s_mg.numAircraft == 4, "training_drones", "drones=%d numAircraft=%d", drones, s_mg.numAircraft);
    t_check(count_msgs("TRAINING", 0) == 1, "training_message", "%s", mission_msg(0, NULL) ? mission_msg(0, NULL) : "-");
    ms_run(30.0f, 0);
    for (i = 1; i <= 3; i++) {
        const Aircraft *a = &s_mg.ac[i];
        static const float alt[3] = { 1500.0f, 3500.0f, 6000.0f };
        float e = (float)fabs(a->fs.pos.y - alt[i - 1]);
        if (!(a->flags & AC_ALIVE)) ok = 0;
        if (e > maxAltErr) maxAltErr = e;
    }
    t_check(ok && maxAltErr < 400.0f, "training_circuits", "all alive=%d max altitude error after 30 s %.0f m", ok, (double)maxAltErr);
    /* kill drone 1 with a player LRM: player 6 km behind-ish, nose on it */
    p = s_mg.ac[1].fs.pos;
    {
        Vec3 d = q_rotate(s_mg.ac[1].fs.qPath, v3(0.0f, 0.0f, 1.0f)), sp = v3_madd(p, d, -5000.0f);
        sp.y = p.y;
        place(PLAYER_ID, sp, (float)atan2(p.x - sp.x, p.z - sp.z), 300.0f);
    }
    ev = weapons_event_count();
    slot = fire_lrm(PLAYER_ID, 1);
    for (i = 0; i < 20 * SIM_HZ && (s_mg.ac[1].flags & AC_ALIVE); i++) ms_tick(0, 0);
    t_check(slot >= 0 && !(s_mg.ac[1].flags & AC_ALIVE) && find_event(ev, WPE_HIT, PLAYER_ID, &victim) && victim == 1, "training_kill",
            "slot=%d drone1 alive=%d hit victim=%d kills=%d after %.1f s", slot, (s_mg.ac[1].flags & AC_ALIVE) ? 1 : 0, victim,
            mission_stats()->kills, (double)i / SIM_HZ);
    t_check(weapons_state(PLAYER_ID)->lrm == 16, "training_unlimited", "player lrm=%d after firing (topped up)", weapons_state(PLAYER_ID)->lrm);
    ms_run(9.0f, 0);
    back = (s_mg.ac[1].flags & AC_ALIVE) != 0;
    ms_run(1.5f, 0);
    t_check(!back && (s_mg.ac[1].flags & AC_ALIVE) && ai_state(1) == AIS_DRONE, "training_respawn",
            "alive 9 s after the kill=%d, 10.5 s after=%d state=%s", back, (s_mg.ac[1].flags & AC_ALIVE) ? 1 : 0, ai_state_name(ai_state(1)));
}

/* ---------------------------------------------------------------- sensor = radar TD */
static void t_sensor(void)
{
    int victim = -1, slot, i, tgt = -1;
    u32 ev;
    Vec3 p = sea_point(4000.0f);
    ms_start(MODE_SORTIE, DIFF_NORMAL, START_AIR, 23u);
    ms_tick(0, 0);                                  /* wave 1 spawns (air start: no takeoff) */
    for (i = 1; i < s_mg.numAircraft; i++) ai_off(i);
    place(PLAYER_ID, p, 0.0f, 260.0f);
    place(1, v3(p.x, p.y, p.z + 6000.0f), 0.0f, 260.0f);                                  /* dead ahead */
    place(2, v3(p.x + 9000.0f * (float)sin(0.26f), p.y, p.z + 9000.0f * (float)cos(0.26f)), 0.0f, 260.0f);   /* 15 deg right */
    ms_run(4.0f, 0);                                /* past the 3 s invulnerability, radar sees both */
    ev = weapons_event_count();
    slot = fire_lrm(PLAYER_ID, 2);
    find_event(ev, WPE_LAUNCH, PLAYER_ID, &victim);
    if (slot >= 0 && weapons_missile(slot)) tgt = weapons_missile(slot)->target;
    t_check(slot >= 0 && victim == 2 && tgt == 2 && radar_designated(PLAYER_ID) == 2 && radar_lock(PLAYER_ID) == LOCK_TRACK, "sensor_radar_td",
            "launch victim=%d missile target=%d TD=%d lock=%d (aircraft 1 dead ahead, 2 = TD 15 deg right)", victim, tgt,
            radar_designated(PLAYER_ID), radar_lock(PLAYER_ID));
}

/* ---------------------------------------------------------------- damage, death, respawn */
static int hit_player(float *hpOut)
{
    int i, slot;
    u32 ev = weapons_event_count();
    Vec3 pp = s_mg.ac[PLAYER_ID].fs.pos, f = q_rotate(s_mg.ac[PLAYER_ID].fs.qPath, v3(0.0f, 0.0f, 1.0f));
    Vec3 ep = v3_madd(pp, f, 5000.0f);
    place(1, ep, (float)atan2(pp.x - ep.x, pp.z - ep.z), 260.0f);
    slot = fire_lrm(1, PLAYER_ID);
    for (i = 0; i < 25 * SIM_HZ; i++) {
        ms_tick(0, 0);
        if (find_event(ev, WPE_HIT, 1, NULL)) break;
    }
    ms_tick(0, 0);
    *hpOut = s_mg.ac[PLAYER_ID].hp;
    return slot >= 0 && find_event(ev, WPE_HIT, 1, NULL);
}

static void t_damage(void)
{
    float hp1 = 0, hp2 = 0, hp3 = 0, hp0 = 0;
    int h1, h2, h3, h0, i, slot, selfd, resp0, inv;
    u32 m0, ev;
    const Param *pinv = params_find("mission", "invulnSec");
    Vec3 p = sea_point(4000.0f);
    ms_start(MODE_SORTIE, DIFF_NORMAL, START_AIR, 31u);
    ms_tick(0, 0);
    for (i = 1; i < s_mg.numAircraft; i++) ai_off(i);
    place(2, v3(p.x + 30000.0f, p.y, p.z), 0.0f, 260.0f);
    place(PLAYER_ID, p, 0.0f, 260.0f);
    /* invulnerable (start + 3 s): hit ignored. invulnSec 30 for this part so the hit lands inside it */
    if (pinv) *(float *)pinv->ptr = 30.0f;
    h0 = hit_player(&hp0);
    inv = (s_mg.ac[PLAYER_ID].flags & AC_INVULN) != 0;
    if (pinv) *(float *)pinv->ptr = 3.0f;
    ms_run(0.1f, 0);
    t_check(h0 && hp0 == 100.0f && inv, "damage_invulnerable", "hit=%d hp=%.0f invulnerable=%d", h0, (double)hp0, inv);
    m0 = mission_msg_count();
    h1 = hit_player(&hp1);
    t_check(h1 && (float)fabs(hp1 - 66.0f) < 0.01f && count_msgs("DAMAGED", m0) == 0, "damage_hit1", "hit=%d hp=%.0f (34 per hit)", h1, (double)hp1);
    h2 = hit_player(&hp2);
    t_check(h2 && (float)fabs(hp2 - 32.0f) < 0.01f && count_msgs("DAMAGED", m0) == 1 && mission_stats()->damaged, "damage_hit2",
            "hit=%d hp=%.0f DAMAGED msgs=%d damaged=%d", h2, (double)hp2, count_msgs("DAMAGED", m0), mission_stats()->damaged);
    /* third hit: dead. A second missile in the air at that moment must self-destruct */
    resp0 = session_respawns();
    {
        Vec3 pp = s_mg.ac[PLAYER_ID].fs.pos, f = q_rotate(s_mg.ac[PLAYER_ID].fs.qPath, v3(0.0f, 0.0f, 1.0f));
        place(2, v3_madd(pp, f, 14000.0f), (float)atan2(-f.x, -f.z), 260.0f);
        slot = fire_lrm(2, PLAYER_ID);
    }
    ev = weapons_event_count();
    h3 = hit_player(&hp3);
    selfd = 0;
    for (i = 0; i < 3 * SIM_HZ && !selfd; i++) { selfd = find_event(ev, WPE_SELFDESTRUCT, 2, NULL); if (!selfd) ms_tick(0, 0); }
    t_check(h3 && hp3 == 0.0f && !(s_mg.ac[PLAYER_ID].flags & AC_ALIVE) && mission_stats()->deaths == 1 &&
            mission_stats()->livesLeft == 2, "damage_dead", "hit=%d hp=%.0f alive=%d deaths=%d lives_left=%d", h3, (double)hp3,
            (s_mg.ac[PLAYER_ID].flags & AC_ALIVE) ? 1 : 0, mission_stats()->deaths, mission_stats()->livesLeft);
    t_check(slot >= 0 && selfd, "damage_selfdestruct", "second missile slot=%d self-destructed=%d", slot, selfd);
    /* wreck 2.5 s from the death tick, then the runway */
    for (i = 0; i < 4 * SIM_HZ && !(s_mg.ac[PLAYER_ID].flags & AC_ALIVE); i++) ms_tick(0, 0);
    t_check((s_mg.ac[PLAYER_ID].flags & AC_ALIVE) && session_respawns() == resp0 + 1 && session_takeoff()->phase == TO_READY &&
            s_mg.ac[PLAYER_ID].hp == 100.0f && (s_mg.ac[PLAYER_ID].flags & AC_INVULN) && weapons_state(PLAYER_ID)->lrm == 16,
            "damage_respawn", "alive=%d respawns=%d phase=%s hp=%.0f invuln=%d lrm=%d", (s_mg.ac[PLAYER_ID].flags & AC_ALIVE) ? 1 : 0,
            session_respawns() - resp0, takeoff_phase_name(session_takeoff()->phase), (double)s_mg.ac[PLAYER_ID].hp,
            (s_mg.ac[PLAYER_ID].flags & AC_INVULN) ? 1 : 0, weapons_state(PLAYER_ID)->lrm);
    /* invulnerable through the takeoff, vulnerable invulnSec after the handover */
    {
        int invTo = 1, handT = -1;
        for (i = 0; i < 60 * SIM_HZ; i++) {
            ms_tick(1, 0);
            if (takeoff_active(session_takeoff())) { if (!(s_mg.ac[PLAYER_ID].flags & AC_INVULN)) invTo = 0; }
            else if (handT < 0) handT = i;
            if (handT >= 0 && !(s_mg.ac[PLAYER_ID].flags & AC_INVULN)) break;
        }
        t_check(invTo && handT > 0 && i - handT >= 3 * SIM_HZ - 2 && i - handT <= 3 * SIM_HZ + 2, "damage_invuln_after_takeoff",
                "invulnerable all takeoff=%d, vulnerable %.2f s after the handover", invTo, (double)(i - handT) / SIM_HZ);
    }
}

/* ---------------------------------------------------------------- sortie: waves, kills, complete */
static void t_sortie(void)
{
    int i, n = 0, ok = 1, waveTick = -1, slot, kills;
    float rmin = 1e9f, rmax = 0.0f, amin = 1e9f, amax = 0.0f;
    u32 m0;
    const Param *pw = params_find("mission", "sortieWaves");
    const MissionStats *st = mission_stats();
    if (pw) *(int *)pw->ptr = 2;                   /* waves are read at mission_start */
    ms_start(MODE_SORTIE, DIFF_NORMAL, START_RUNWAY, 41u);
    for (i = 0; i < 60 * SIM_HZ && st->wave == 0; i++) {
        ms_tick(1, 0);
        if (st->wave && waveTick < 0) waveTick = i;
    }
    for (i = 1; i < s_mg.numAircraft; i++) {
        const Aircraft *a = &s_mg.ac[i];
        float r;
        if (!(a->flags & AC_ALIVE)) continue;
        n++;
        r = v3_len(v3_sub(a->fs.pos, s_mg.ac[PLAYER_ID].fs.pos));
        rmin = E_MINF(rmin, r); rmax = E_MAXF(rmax, r);
        amin = E_MINF(amin, a->fs.pos.y); amax = E_MAXF(amax, a->fs.pos.y);
        if (a->side != SIDE_RED || a->livery != 1 || ai_state(i) == AIS_OFF) ok = 0;
    }
    t_check(st->wave == 1 && n == 2 && ok && !takeoff_active(session_takeoff()) &&
            rmin >= *(float *)params_find("mission", "spawnMinKm")->ptr * 1000.0f - 100.0f &&
            rmax <= *(float *)params_find("mission", "spawnMaxKm")->ptr * 1000.0f + 100.0f &&
            amin >= 3999.0f && amax <= 6001.0f && count_msgs("WAVE 1: 2 BANDITS BRG", 0) == 1, "sortie_wave1",
            "wave=%d enemies=%d after takeoff=%d at %.1f s range %.0f-%.0f m alt %.0f-%.0f m msg=%s", st->wave, n,
            !takeoff_active(session_takeoff()), (double)waveTick / SIM_HZ, (double)rmin, (double)rmax, (double)amin, (double)amax,
            mission_msg(mission_msg_count() - 1, NULL) ? mission_msg(mission_msg_count() - 1, NULL) : "-");
    /* shoot both down: brains off, each placed 8 km ahead head-on in turn */
    ms_run(3.5f, 0);
    for (i = 1; i <= 2; i++) {
        Vec3 pp = s_mg.ac[PLAYER_ID].fs.pos, f = q_rotate(s_mg.ac[PLAYER_ID].fs.qPath, v3(0.0f, 0.0f, 1.0f)), ep;
        int k;
        ai_off(i);
        ep = v3_madd(pp, f, 8000.0f);
        place(i, ep, s_mg.ac[PLAYER_ID].fs.heading + E_PI, 220.0f);
        slot = fire_lrm(PLAYER_ID, i);
        for (k = 0; k < 25 * SIM_HZ && (s_mg.ac[i].flags & AC_ALIVE); k++) ms_tick(0, 0);
        if (slot < 0) ok = 0;
    }
    kills = st->kills;
    m0 = mission_msg_count();
    for (i = 0; i < 12 * SIM_HZ && st->wave < 2; i++) ms_tick(0, 0);
    t_check(ok && kills == 2 && st->waves == 2 && st->wave == 2 && i >= 8 * SIM_HZ - 3 && count_msgs("WAVE 2: 3 BANDITS", m0) == 1 &&
            st->outcome == MIS_RUNNING, "sortie_wave2", "kills=%d, wave 2 (3 bandits) spawned %.2f s after the clear (sortieWaves 2)",
            kills, (double)i / SIM_HZ);
    if (pw) *(int *)pw->ptr = 1;
    /* restart with 1 wave: kill both -> COMPLETE */
    ms_start(MODE_SORTIE, DIFF_NORMAL, START_AIR, 43u);
    ms_tick(0, 0);
    ms_run(1.0f, 0);
    for (i = 1; i <= 2; i++) {
        Vec3 pp = s_mg.ac[PLAYER_ID].fs.pos, f = q_rotate(s_mg.ac[PLAYER_ID].fs.qPath, v3(0.0f, 0.0f, 1.0f)), ep;
        int k;
        ai_off(i);
        ep = v3_madd(pp, f, 12000.0f);
        place(i, ep, s_mg.ac[PLAYER_ID].fs.heading + E_PI, 250.0f);     /* head-on, beyond 10 km: BVR bonus */
        slot = fire_lrm(PLAYER_ID, i);
        for (k = 0; k < 30 * SIM_HZ && (s_mg.ac[i].flags & AC_ALIVE); k++) ms_tick(0, 0);
    }
    m0 = mission_msg_count();
    for (i = 0; i < 5 * SIM_HZ && st->outcome == MIS_RUNNING; i++) ms_tick(0, 0);
    {
        ScoreIn si;
        MissionResults mr;
        int exact;
        si.outcome = st->outcome; si.kills = st->kills; si.killsBvr = st->killsBvr; si.deaths = st->deaths;
        si.fired = st->fired[0] + st->fired[1]; si.hits = st->hits[0] + st->hits[1]; si.timeSec = st->timeSec;
        si.timeRefSec = mission_time_ref();
        exact = mission_score(&si, NULL) == st->score;
        mission_results(&s_mg, &mr);
        t_check(exact && mr.valid && mr.score == st->score && mr.kills == 2 && mr.killsBvr == 2 && mr.outcome == MIS_COMPLETE &&
                mr.fired[MSL_LRM] == st->fired[MSL_LRM] && mr.hits[MSL_LRM] == 2 && mr.parts.kill == 2000 && mr.parts.bvr == 600 &&
                mr.parts.time > 0 && mr.parts.total == mr.score, "sortie_results",
                "score=%d (= mission_score %d) results valid=%d kills=%d bvr=%d parts kill %d bvr %d death %d time %d acc %d",
                st->score, exact, mr.valid, mr.kills, mr.killsBvr, mr.parts.kill, mr.parts.bvr, mr.parts.death, mr.parts.time,
                mr.parts.accuracy);
    }
    t_check(st->outcome == MIS_COMPLETE && st->kills == 2 && st->killsBvr == 2 && i >= 3 * SIM_HZ - 3 &&
            count_msgs("MISSION COMPLETE", m0) == 1 && st->score > 2000 && mission_clock(&s_mg) > 0.0f, "sortie_complete",
            "outcome=%s kills=%d bvr=%d after %.2f s, t=%.1f s, fired=%d/%d hits=%d/%d score=%d", mission_outcome_name(st->outcome),
            st->kills, st->killsBvr, (double)i / SIM_HZ, (double)st->timeSec, st->fired[0], st->fired[1], st->hits[0], st->hits[1],
            st->score);
}

/* ---------------------------------------------------------------- lives */
static void t_lives(void)
{
    int i, d;
    const Param *pl = params_find("mission", "lives");
    const MissionStats *st = mission_stats();
    if (pl) *(int *)pl->ptr = 2;
    ms_start(MODE_SORTIE, DIFF_EASY, START_AIR, 47u);
    ms_tick(0, 0);
    for (i = 1; i < s_mg.numAircraft; i++) ai_off(i);
    for (d = 0; d < 2; d++) {
        Vec3 p = sea_point(150.0f);
        place(PLAYER_ID, p, 0.0f, 250.0f);
        s_mg.ac[PLAYER_ID].fs.qPath = flight_path_quat(0.0f, E_DEG2RAD(-40.0f), 0.0f);   /* dive into the sea */
        flight_derive(&s_mg.ac[PLAYER_ID].fs);
        s_mg.ac[PLAYER_ID].prev = s_mg.ac[PLAYER_ID].fs;
        for (i = 0; i < 10 * SIM_HZ && (s_mg.ac[PLAYER_ID].flags & AC_ALIVE); i++) ms_tick(0, 0);
        for (i = 0; i < 8 * SIM_HZ && !(s_mg.ac[PLAYER_ID].flags & AC_ALIVE); i++) ms_tick(0, 0);   /* session crash flow */
        if (d == 0) t_check(st->deaths == 1 && st->deathsCrash == 1 && st->outcome == MIS_RUNNING && st->livesLeft == 1, "lives_first",
                            "deaths=%d crash=%d outcome=%s lives_left=%d", st->deaths, st->deathsCrash, mission_outcome_name(st->outcome),
                            st->livesLeft);
    }
    t_check(st->deaths == 2 && st->outcome == MIS_FAILED && st->endTick > 0 && st->livesLeft == 0, "lives_failed",
            "deaths=%d outcome=%s lives_left=%d", st->deaths, mission_outcome_name(st->outcome), st->livesLeft);
    if (pl) *(int *)pl->ptr = 3;
}

/* ---------------------------------------------------------------- robot sortie */
static u32 robot_run(float sec, u32 seed, int *eCrash, int *rCrash)
{
    u32 n = (u32)(sec * (float)SIM_HZ), i, wev, h;
    int resp, k;
    WorldEvent we;
    *eCrash = *rCrash = 0;
    ms_start(MODE_SORTIE, DIFF_NORMAL, START_RUNWAY, seed);
    session_set_autopilot(ai_robot_hook);
    ai_spawn_robot(&s_mg, PLAYER_ID);
    resp = session_respawns();
    wev = world_event_seq();
    for (i = 0; i < n && mission_stats()->outcome == MIS_RUNNING; i++) {
        ms_tick(1, 0);
        if (session_respawns() != resp) { resp = session_respawns(); ai_spawn_robot(&s_mg, PLAYER_ID); }
        while (wev < world_event_seq()) {
            if (!world_event_get(wev++, &we) || !(we.type & WEV_CRASH_ANY) || we.type == WEV_CRASH_AIR) continue;
            if (we.ac == PLAYER_ID) { if (!ai_masking(PLAYER_ID)) (*rCrash)++; }
            else if (!ai_masking(we.ac)) (*eCrash)++;
        }
    }
    session_set_autopilot(NULL);
    mission_log_result(&s_mg, mission_stats()->outcome == MIS_RUNNING ? "test_cap" : "test_end");
    h = e_hash(&s_mg.tick, (int)sizeof(s_mg.tick), E_HASH_INIT);
    for (k = 0; k < s_mg.numAircraft; k++) {
        h = e_hash(&s_mg.ac[k].fs.pos, (int)sizeof(Vec3), h);
        h = e_hash(&s_mg.ac[k].flags, (int)sizeof(int), h);
        h = e_hash(&s_mg.ac[k].hp, (int)sizeof(float), h);
    }
    h = e_hash(mission_stats(), (int)sizeof(MissionStats), h);
    return h;
}

static void t_robot(void)
{
    int ec, rc, ec2, rc2;
    u32 h1 = robot_run(300.0f, 5u, &ec, &rc), h2;
    const MissionStats *st = mission_stats();
    int kills = st->kills, deaths = st->deaths, fired = st->fired[0] + st->fired[1], wave = st->wave;
    h2 = robot_run(300.0f, 5u, &ec2, &rc2);
    t_check(ec == 0 && rc == 0 && wave >= 1 && fired > 0, "robot_sortie", "300 s Normal: outcome=%s kills=%d deaths=%d fired=%d wave=%d "
            "terrain crashes enemy=%d robot=%d", mission_outcome_name(st->outcome), kills, deaths, fired, wave, ec, rc);
    t_check(h1 == h2, "robot_determinism", "hash %08x / %08x", h1, h2);
}

/* ---------------------------------------------------------------- T20: score examples (DESIGN 14.4) */
typedef struct ScoreEx {
    const char *name;
    int outcome, kills, bvr, deaths, fired, hits;
    float t, ref;
    int kill, bonusBvr, death, time, acc, total;
} ScoreEx;

static void t_score(void)
{
    /* kill 1000, +300 BVR, -1500 per death, time max(0, ref - s) x 5 only on COMPLETE, accuracy hits/fired x 2000, >= 0 */
    static const ScoreEx ex[] = {
        { "score_clean",    MIS_COMPLETE, 9, 3, 0, 12, 9, 400.0f, 600.0f,  9000, 900,     0, 1000, 1500, 12400 },
        { "score_slow",     MIS_COMPLETE, 9, 0, 1, 18, 9, 700.0f, 600.0f,  9000,   0, -1500,    0, 1000,  8500 },
        { "score_failed",   MIS_FAILED,   4, 1, 3, 10, 4, 300.0f, 600.0f,  4000, 300, -4500,    0,  800,   600 },
        { "score_clamp",    MIS_FAILED,   0, 0, 3,  0, 0, 200.0f, 600.0f,     0,   0, -4500,    0,    0,     0 },
        { "score_fraction", MIS_COMPLETE, 2, 2, 0,  3, 2, 599.5f, 600.0f,  2000, 600,     0,    2, 1333,  3935 },
        { "score_at_ref",   MIS_COMPLETE, 8, 0, 0,  8, 8, 600.0f, 600.0f,  8000,   0,     0,    0, 2000, 10000 },
        { "score_ref750",   MIS_COMPLETE, 9, 2, 1, 16, 9, 450.0f, 750.0f,  9000, 600, -1500, 1500, 1125, 10725 },
        { "score_running",  MIS_RUNNING,  3, 0, 0,  4, 3, 100.0f, 750.0f,  3000,   0,     0,    0, 1500,  4500 },
    };
    int i, total;
    ScoreIn in;
    ScoreParts p;
    for (i = 0; i < E_COUNT(ex); i++) {
        const ScoreEx *e = &ex[i];
        in.outcome = e->outcome; in.kills = e->kills; in.killsBvr = e->bvr; in.deaths = e->deaths; in.fired = e->fired;
        in.hits = e->hits; in.timeSec = e->t; in.timeRefSec = e->ref;
        total = mission_score(&in, &p);
        t_check(total == e->total && p.total == e->total && p.kill == e->kill && p.bvr == e->bonusBvr && p.death == e->death &&
                p.time == e->time && p.accuracy == e->acc, e->name,
                "%s kills %d bvr %d deaths %d %d/%d hits t %.1f/%.0f s -> kill %d bvr %d death %d time %d acc %d = %d (want %d)",
                mission_outcome_name(e->outcome), e->kills, e->bvr, e->deaths, e->hits, e->fired, (double)e->t, (double)e->ref, p.kill,
                p.bvr, p.death, p.time, p.accuracy, total, e->total);
    }
    t_check(mission_time_ref() == 750.0f, "score_default_ref", "timeBonusRefSec default %.0f (want 750)", (double)mission_time_ref());
}

/* ---------------------------------------------------------------- T20: the full 3-wave sortie per difficulty */
static void t_waves(void)
{
    static const int want[3][3] = { { 2, 3, 3 }, { 2, 3, 4 }, { 3, 4, 5 } };
    static const char *const nm[3] = { "waves_easy", "waves_normal", "waves_hard" };
    const MissionStats *st = mission_stats();
    const Param *pw = params_find("mission", "sortieWaves");
    float smin = *(float *)params_find("mission", "spawnMinKm")->ptr * 1000.0f, smax = *(float *)params_find("mission", "spawnMaxKm")->ptr * 1000.0f;
    float lmin = *(float *)params_find("mission", "spawnLaterMinKm")->ptr * 1000.0f;
    float lmax = *(float *)params_find("mission", "spawnLaterMaxKm")->ptr * 1000.0f;
    int d, w, i, k;
    t_check(pw && !strcmp(pw->def, "3"), "waves_default", "sortieWaves default %s (want 3)", pw ? pw->def : "-");
    if (pw) *(int *)pw->ptr = 3;
    for (d = 0; d < 3; d++) {
        int ok = 1, sizes[3] = { 0, 0, 0 }, maxAlive = 0, gapOk = 1, clearedMsgs, waveMsgs = 0;
        float rmin = 1e9f, rmax = 0.0f, amin = 1e9f, amax = 0.0f, gap[2] = { 0.0f, 0.0f }, doneSec = -1.0f;
        char want0[MIS_MSG_LEN];
        MissionResults mr;
        ms_start(MODE_SORTIE, d, START_AIR, 101u + (u32)d);
        ms_tick(0, 0);                              /* wave 1 at once (air start) */
        for (w = 0; w < 3; w++) {
            int n = 0, alive;
            Vec3 pp = s_mg.ac[PLAYER_ID].fs.pos;
            if (st->wave != w + 1) { ok = 0; break; }
            for (i = 1; i < s_mg.numAircraft; i++) {
                const Aircraft *a = &s_mg.ac[i];
                float r;
                if (!(a->flags & AC_ALIVE)) continue;
                n++;
                r = v3_len(v3_sub(v3(a->fs.pos.x, 0.0f, a->fs.pos.z), v3(pp.x, 0.0f, pp.z)));
                if (r < (w ? lmin : smin) - 100.0f || r > (w ? lmax : smax) + 100.0f) ok = 0;
                rmin = E_MINF(rmin, r); rmax = E_MAXF(rmax, r);
                amin = E_MINF(amin, a->fs.pos.y); amax = E_MAXF(amax, a->fs.pos.y);
                if (a->side != SIDE_RED || ai_state(i) == AIS_OFF) ok = 0;
            }
            sizes[w] = n;
            if (n > maxAlive) maxAlive = n;
            e_snprintf(want0, (int)sizeof(want0), "WAVE %d: %d BANDITS BRG ", w + 1, want[d][w]);
            waveMsgs += count_msgs(want0, 0) == 1;
            /* let them fly a little, then shoot the wave down (brains off; kills by clearing AC_ALIVE as a hit would) */
            ms_run(2.0f, 0);
            for (i = 1; i < s_mg.numAircraft; i++) { ai_off(i); s_mg.ac[i].flags &= ~AC_ALIVE; }
            for (k = 0; k < 15 * SIM_HZ && st->wave == w + 1 && st->outcome == MIS_RUNNING; k++) {
                ms_tick(0, 0);
                alive = count_alive_enemies_t();
                if (alive > maxAlive) maxAlive = alive;
            }
            if (w < 2) {
                gap[w] = (float)k / SIM_HZ;
                if (gap[w] < 8.0f - 0.05f || gap[w] > 8.0f + 0.1f) gapOk = 0;
            } else doneSec = (float)k / SIM_HZ;
        }
        clearedMsgs = count_msgs("WAVE 1 CLEARED", 0) + count_msgs("WAVE 2 CLEARED", 0);
        mission_results(&s_mg, &mr);
        t_check(ok && sizes[0] == want[d][0] && sizes[1] == want[d][1] && sizes[2] == want[d][2] && maxAlive <= 5 && gapOk &&
                waveMsgs == 3 && clearedMsgs == 2 && count_msgs("WAVE 3 CLEARED", 0) == 0 && st->outcome == MIS_COMPLETE &&
                count_msgs("MISSION COMPLETE", 0) == 1 && doneSec >= 2.9f && doneSec <= 3.2f && amin >= 3999.0f && amax <= 6001.0f &&
                st->spawned == want[d][0] + want[d][1] + want[d][2] && mr.valid && mr.wave == 3 && mr.waves == 3 &&
                mr.spawned == st->spawned && mr.diff == d, nm[d],
                "waves %d,%d,%d (want %d,%d,%d) max alive %d, range %.1f-%.1f km alt %.0f-%.0f m, next wave %.2f / %.2f s after the "
                "clear, wave msgs %d cleared msgs %d, %s %.2f s after the last clear, spawned %d, results valid=%d score=%d",
                sizes[0], sizes[1], sizes[2], want[d][0], want[d][1], want[d][2], maxAlive, (double)(rmin * 0.001f), (double)(rmax * 0.001f),
                (double)amin, (double)amax, (double)gap[0], (double)gap[1], waveMsgs, clearedMsgs, mission_outcome_name(st->outcome),
                (double)doneSec, st->spawned, mr.valid, mr.score);
    }
}

/* ---------------------------------------------------------------- T20: records.ini */
static int file_has_crlf_only(const char *path)
{
    int n = 0, i, ok = 1;
    char *t = (char *)file_load(path, &n);
    if (!t || n <= 0) { if (t) free(t); return 0; }
    for (i = 0; i < n; i++) if (t[i] == '\n' && (i == 0 || t[i - 1] != '\r')) ok = 0;
    free(t);
    return ok;
}

static void fake_result(MissionResults *r, int diff, int score)
{
    memset(r, 0, sizeof(*r));
    r->valid = 1;
    r->mode = MODE_SORTIE;
    r->diff = diff;
    r->outcome = MIS_COMPLETE;
    r->timeSec = 512.3f;
    r->wave = r->waves = 3;
    r->kills = r->spawned = 9;
    r->killsBvr = 4;
    r->fired[0] = 10; r->hits[0] = 5; r->fired[1] = 6; r->hits[1] = 4;
    r->score = r->parts.total = score;
    r->prevBest = -1;
}

static void t_records(void)
{
    char path[E_PATH];
    MissionResults r, r2;
    RecordRow row;
    int n, got, nw, again, i;
    FILE *f;
    base_path(path, (int)sizeof(path), "records_test.ini");
    remove(path);
    records_set_path(path);
    n = records_load();
    got = records_get(DIFF_NORMAL, &row);
    t_check(n == 0 && !got && !row.valid, "records_missing", "rows=%d normal=%d (no file)", n, got);
    /* a real ended mission (the waves part's result if it ran; else a fresh 1-kill sortie) */
    ms_start(MODE_SORTIE, DIFF_NORMAL, START_AIR, 211u);
    ms_tick(0, 0);
    for (i = 1; i < s_mg.numAircraft; i++) { ai_off(i); s_mg.ac[i].flags &= ~AC_ALIVE; }
    {
        const Param *pw = params_find("mission", "sortieWaves");
        int old = pw ? *(int *)pw->ptr : 3;
        if (pw) *(int *)pw->ptr = 1;
        for (i = 0; i < 6 * SIM_HZ && mission_stats()->outcome == MIS_RUNNING; i++) ms_tick(0, 0);
        if (pw) *(int *)pw->ptr = old;
    }
    mission_results(&s_mg, &r);
    nw = r.newRecord;
    got = records_submit(&r);
    again = records_submit(&r);
    t_check(r.valid && nw && got && again && r.newRecord && r.prevBest == -1 && file_exists(path) && file_has_crlf_only(path),
            "records_first", "valid=%d outcome=%s score=%d newRecord before=%d submit=%d again=%d prev=%d file=%d crlf=%d", r.valid,
            mission_outcome_name(r.outcome), r.score, nw, got, again, r.prevBest, file_exists(path), file_has_crlf_only(path));
    mission_results(&s_mg, &r2);
    n = records_load();
    got = records_get(DIFF_NORMAL, &row);
    t_check(n == 1 && got && row.score == r.score && row.outcome == r.outcome && row.waves == r.wave && row.kills == r.kills &&
            (float)fabs(row.timeSec - r.timeSec) < 0.051f && strlen(row.date) == 16 && row.date[4] == '-' && row.date[13] == ':' &&
            r2.newRecord == 1 && !records_get(DIFF_EASY, NULL), "records_reload",
            "rows=%d score=%d outcome=%s t=%.1f date=%s results after submit newRecord=%d", n, row.score, mission_outcome_name(row.outcome),
            (double)row.timeSec, row.date, r2.newRecord);
    /* new missions: lower score no record, higher replaces */
    ms_start(MODE_SORTIE, DIFF_NORMAL, START_AIR, 212u);
    fake_result(&r, DIFF_NORMAL, row.score > 0 ? row.score - 1 : 0);
    got = records_submit(&r);
    t_check(!got && !r.newRecord && r.prevBest == row.score && records_get(DIFF_NORMAL, &row) && row.score == r.prevBest,
            "records_lower", "score %d vs best %d: new=%d", r.score, r.prevBest, got);
    ms_start(MODE_SORTIE, DIFF_NORMAL, START_AIR, 213u);
    fake_result(&r, DIFF_NORMAL, 54321);
    got = records_submit(&r);
    ms_start(MODE_SORTIE, DIFF_HARD, START_AIR, 214u);
    fake_result(&r2, DIFF_HARD, 777);
    records_submit(&r2);
    n = records_load();
    t_check(got && r.newRecord && n == 2 && records_get(DIFF_NORMAL, &row) && row.score == 54321 && row.hits[1] == 4 &&
            row.fired[0] == 10 && row.killsBvr == 4 && records_get(DIFF_HARD, &row) && row.score == 777, "records_higher",
            "new=%d rows=%d normal=%d hard=%d", got, n, records_get(DIFF_NORMAL, NULL), records_get(DIFF_HARD, NULL));
    /* corrupt: garbage bytes, a bad row (score=abc), a row with hits > fired, one good row */
    f = fopen(path, "wb");
    if (f) {
        static const unsigned char junk[] = { 0x00, 0xFF, 0x13, 0x88, '[', '[', '\r', '\n', 0x7F, 0x00, '\n' };
        fwrite(junk, 1, sizeof(junk), f);
        fputs("[normal]\r\nscore=abc\r\noutcome=complete\r\n[hard]\r\nscore=500\r\ntimeTenths=10\r\nwaves=3\r\nkills=1\r\nspawned=12\r\n"
              "bvr=0\r\ndeaths=3\r\nlrmFired=1\r\nlrmHits=5\r\nsrmFired=0\r\nsrmHits=0\r\noutcome=failed\r\n"
              "[easy]\r\nscore=1234\r\ndate=2026-11-14 12:00\r\noutcome=complete\r\ntimeTenths=5000\r\nwaves=3\r\nkills=8\r\n"
              "spawned=8\r\nbvr=2\r\ndeaths=0\r\nlrmFired=4\r\nlrmHits=3\r\nsrmFired=6\r\nsrmHits=5\r\n", f);
        fclose(f);
    }
    n = records_load();
    got = records_get(DIFF_EASY, &row) ? row.score : -1;
    t_check(f && n == 1 && got == 1234 && (float)fabs(row.timeSec - 500.0f) < 0.01f &&
            !records_get(DIFF_NORMAL, NULL) && !records_get(DIFF_HARD, NULL), "records_corrupt",
            "rows=%d easy=%d normal=%d hard=%d (garbage, score=abc, hits>fired ignored)", n, got,
            records_get(DIFF_NORMAL, NULL), records_get(DIFF_HARD, NULL));
    records_reset(-1);
    records_save();
    n = records_load();
    t_check(n == 0, "records_reset", "rows after reset + save + load: %d", n);
    remove(path);
    records_set_path(NULL);
}

/* ---------------------------------------------------------------- entry */
static void run_parts(const char *arg)
{
    if (!arg || !strcmp(arg, "training")) t_training();
    if (!arg || !strcmp(arg, "sensor")) t_sensor();
    if (!arg || !strcmp(arg, "damage")) t_damage();
    if (!arg || !strcmp(arg, "sortie")) t_sortie();
    if (!arg || !strcmp(arg, "lives")) t_lives();
    if (!arg || !strcmp(arg, "robot")) t_robot();
    if (!arg || !strcmp(arg, "score")) t_score();
    if (!arg || !strcmp(arg, "waves")) t_waves();
    if (!arg || !strcmp(arg, "records")) t_records();
}

int test_mission(const char *arg)
{
    static char snap[65536];
    int n, logEv = 0, dir = mission_director();
    Cfg *empty;
    const Param *tp;
    t_begin("mission");
    log_printf(LOG_INFO, "test: mission params=defaults terrain=%d", ter() != NULL);
    n = params_snapshot(snap, (int)sizeof(snap));
    empty = cfg_parse("", 0);
    params_load(empty);
    cfg_free(empty);
    tp = params_find("weapons", "logEvents");
    if (tp) { logEv = *(int *)tp->ptr; *(int *)tp->ptr = 0; }
    weapons_set_sensor(NULL);                       /* the game's sensor: the radar designation */
    run_parts(arg && arg[0] ? arg : NULL);
    if (tp) *(int *)tp->ptr = logEv;
    session_set_autopilot(NULL);
    mission_set_director(dir);
    dlz_set_escape_guse(-1.0f);
    if (n > 0) params_restore(snap, n);
    return t_end();
}
