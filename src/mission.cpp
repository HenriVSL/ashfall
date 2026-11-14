/* mission.cpp - the mission director: TRAINING drones, SORTIE waves, damage, deaths, respawns, lives, score,
   messages (DESIGN 14). Portable. Owner: T17 (then T20). Rules and API: mission.h.
   Everything here runs inside session_tick (step 9, after weapons + radar), reads the weapons / world event rings
   with its own read indices, and uses only g->rng, so replays and headless runs reproduce it exactly. */
#include "mission.h"
#include "params.h"
#include "e_log.h"
#include "e_terrain.h"
#include "world.h"
#include "takeoff.h"
#include "radar.h"
#include "weapons.h"
#include "missile.h"
#include "dlz.h"
#include "ai.h"
#include "e_file.h"
#include <stdio.h>
#include <time.h>

/* ---------------------------------------------------------------- [mission] */
static float s_missileDamage, s_damagedAt, s_invulnSec, s_deathRespawnSec, s_waveDelaySec, s_completeDelaySec;
static float s_spawnMinKm, s_spawnMaxKm, s_spawnAltMin, s_spawnAltMax, s_trainRespawnSec, s_bvrKm;
static float s_laterMinKm, s_laterMaxKm, s_timeRefSec;
static int   s_lives, s_sortieWaves, s_trainDrones, s_trainUnlimited;
static const Param s_tab[] = {
    { "mission", "missileDamage", P_FLOAT, &s_missileDamage, "34", 0.0f, 100.0f, "player hp lost per missile hit (hp 100: 34 = three hits; user 2026-11-13, revisit)" },
    { "mission", "damagedAt", P_FLOAT, &s_damagedAt, "50", 0.0f, 100.0f, "damage at which the jet smokes and DAMAGED shows (hp <= 100 - this)" },
    { "mission", "lives", P_INT, &s_lives, "3", 0.0f, 9.0f, "SORTIE lives (0 = unlimited); the death that uses the last one fails the mission" },
    { "mission", "invulnSec", P_FLOAT, &s_invulnSec, "3", 0.0f, 30.0f, "invulnerable for this long after the takeoff handover (and during the takeoff)" },
    { "mission", "deathRespawnSec", P_FLOAT, &s_deathRespawnSec, "2.5", 0.5f, 20.0f, "wreck camera after a missile death, then the runway (crashes: [world] wreckSec)" },
    { "mission", "sortieWaves", P_INT, &s_sortieWaves, "3", 1.0f, 3.0f, "SORTIE waves (DESIGN 14.3 full sortie: 3; the M2 play-test used 1)" },
    { "mission", "waveDelaySec", P_FLOAT, &s_waveDelaySec, "8", 0.0f, 120.0f, "next wave this long after a wave is cleared" },
    { "mission", "completeDelaySec", P_FLOAT, &s_completeDelaySec, "3", 0.0f, 60.0f, "MISSION COMPLETE this long after the last kill (SPLASH first)" },
    /* T21a: 22-28 -> 45-55 km: the player's AIM-120 now reaches ~38 km head-on, so a wave starts outside it (BVR approach) */
    { "mission", "spawnMinKm", P_FLOAT, &s_spawnMinKm, "45", 5.0f, 60.0f, "SORTIE enemies spawn at least this far from the player (km)" },
    { "mission", "spawnMaxKm", P_FLOAT, &s_spawnMaxKm, "55", 5.0f, 60.0f, "... and at most this far (km), around the player's heading" },
    { "mission", "spawnAltMinM", P_FLOAT, &s_spawnAltMin, "4000", 500.0f, 12000.0f, "SORTIE enemy spawn altitude, lowest (m)" },
    { "mission", "spawnAltMaxM", P_FLOAT, &s_spawnAltMax, "6000", 500.0f, 12000.0f, "SORTIE enemy spawn altitude, highest (m)" },
    { "mission", "trainDrones", P_INT, &s_trainDrones, "3", 0.0f, 5.0f, "TRAINING drones (non-shooting, on circuits ahead of the runway)" },
    { "mission", "trainRespawnSec", P_FLOAT, &s_trainRespawnSec, "10", 0.0f, 120.0f, "a TRAINING drone comes back this long after it died" },
    { "mission", "trainUnlimited", P_BOOL, &s_trainUnlimited, "1", 0.0f, 1.0f, "TRAINING: missiles and flares never run out" },
    { "mission", "spawnLaterMinKm", P_FLOAT, &s_laterMinKm, "45", 5.0f, 60.0f, "SORTIE waves 2+ spawn at least this far from the player (km; T20: 35-45 saved only ~5 percent of robot sortie time)" },
    { "mission", "spawnLaterMaxKm", P_FLOAT, &s_laterMaxKm, "55", 5.0f, 60.0f, "... and at most this far (km)" },
    { "mission", "timeBonusRefSec", P_FLOAT, &s_timeRefSec, "750", 60.0f, 3600.0f, "score time bonus = max(0, this - mission seconds) x 5 on COMPLETE (T20: 600 + ~150 s for the 45-55 km approaches)" },
    { "mission", "bvrKm", P_FLOAT, &s_bvrKm, "10", 0.0f, 100.0f, "a kill launched beyond this range scores the BVR bonus (km)" },
};
void mission_params(void) { params_add(s_tab, E_COUNT(s_tab)); }

/* ---------------------------------------------------------------- state */
typedef struct MisMsg { char text[MIS_MSG_LEN]; float secs; } MisMsg;
static int   s_on;
static MissionStats s_st;
static u32   s_wev, s_pev;                          /* world / weapons event read indices */
static int   s_lastResp;
static int   s_deadPending; static u32 s_deadTick;  /* missile death: runway after deathRespawnSec */
static u32   s_handTick;                            /* first tick after the takeoff (invulnerability timer) */
static u32   s_clearTick;                           /* wave cleared at (0 = not cleared) */
static u32   s_clockTick;                           /* first wave spawn (mission clock) */
static float s_launchDist[MAX_MISSILES];            /* per missile slot: shooter -> target at launch (BVR bonus) */
static int   s_drone[MAX_AIRCRAFT];                 /* 1 = slot is a TRAINING drone */
static u32   s_droneDeadTick[MAX_AIRCRAFT];         /* 0 = alive */
static int   s_droneIdx[MAX_AIRCRAFT];
static MisMsg s_msg[MIS_MSGS];
static u32   s_msgN;
static u32   s_serial;                              /* mission_start count (records_submit once per mission) */

static const int s_waveSize[3][3] = { { 2, 3, 3 }, { 2, 3, 4 }, { 3, 4, 5 } };   /* DESIGN 13.3 */
enum { MAX_ENEMIES_ALIVE = 5, DRONES_MAX = 5 };
/* TRAINING circuits: distance ahead of the runway threshold along the takeoff heading, lateral offset (right +),
   radius, altitude: near/low, middle, far/high (LRM ranges), two spare */
static const float s_drAhead[DRONES_MAX] = { 12000.0f, 20000.0f, 30000.0f, 16000.0f, 26000.0f };
static const float s_drSide[DRONES_MAX]  = { 0.0f, 7000.0f, -7000.0f, -9000.0f, 9000.0f };
static const float s_drR[DRONES_MAX]     = { 3000.0f, 4000.0f, 5000.0f, 3500.0f, 4500.0f };
static const float s_drAlt[DRONES_MAX]   = { 1500.0f, 3500.0f, 6000.0f, 2500.0f, 8000.0f };

void mission_set_director(int on) { s_on = on ? 1 : 0; }
int  mission_director(void) { return s_on; }
const MissionStats *mission_stats(void) { return &s_st; }
const char *mission_outcome_name(int o) { return o == MIS_COMPLETE ? "complete" : (o == MIS_FAILED ? "failed" : "running"); }
u32  mission_msg_count(void) { return s_msgN; }

const char *mission_msg(u32 index, float *secs)
{
    if (index >= s_msgN || s_msgN - index > (u32)MIS_MSGS) return NULL;
    if (secs) *secs = s_msg[index % MIS_MSGS].secs;
    return s_msg[index % MIS_MSGS].text;
}

static void msg(const char *text, float secs)
{
    MisMsg *m = &s_msg[s_msgN % MIS_MSGS];
    e_strlcpy(m->text, text, MIS_MSG_LEN);
    m->secs = secs;
    s_msgN++;
}

static float tsec(u32 ticks) { return (float)ticks / (float)SIM_HZ; }
static u32 ticks_of(float s) { return s <= 0.0f ? 0u : (u32)(s * (float)SIM_HZ + 0.5f); }

float mission_clock(const Game *g)
{
    if (!s_on || g->mode != MODE_SORTIE || !s_clockTick) return -1.0f;
    return s_st.timeSec;
}

static int count_alive_enemies(const Game *g)
{
    int i, n = 0;
    for (i = 1; i < g->numAircraft; i++) if (g->ac[i].flags & AC_ALIVE) n++;
    return n;
}

/* DESIGN 14.4 (T20: time reference is a param, total clamped at 0) */
int mission_score(const ScoreIn *in, ScoreParts *out)
{
    ScoreParts p;
    p.kill = in->kills * 1000;
    p.bvr = in->killsBvr * 300;
    p.death = -in->deaths * 1500;
    p.time = (in->outcome == MIS_COMPLETE && in->timeSec < in->timeRefSec) ? (int)((in->timeRefSec - in->timeSec) * 5.0f) : 0;
    p.accuracy = in->fired > 0 ? (int)(2000.0f * (float)E_CLAMP(in->hits, 0, in->fired) / (float)in->fired) : 0;
    p.total = p.kill + p.bvr + p.death + p.time + p.accuracy;
    if (p.total < 0) p.total = 0;
    if (out) *out = p;
    return p.total;
}

float mission_time_ref(void) { return s_timeRefSec; }

static void score_in(ScoreIn *in)
{
    in->outcome = s_st.outcome;
    in->kills = s_st.kills;
    in->killsBvr = s_st.killsBvr;
    in->deaths = s_st.deaths;
    in->fired = s_st.fired[0] + s_st.fired[1];
    in->hits = s_st.hits[0] + s_st.hits[1];
    in->timeSec = s_st.timeSec;
    in->timeRefSec = s_timeRefSec;
}

static int compute_score(void)
{
    ScoreIn in;
    score_in(&in);
    return mission_score(&in, NULL);
}

void mission_log_result(const Game *g, const char *why)
{
    ScoreIn in;
    ScoreParts sp;
    score_in(&in);
    s_st.score = mission_score(&in, &sp);
    log_printf(LOG_INFO, "mission: score kill=%d bvr=%d death=%d time=%d (ref %.0f s) accuracy=%d total=%d", sp.kill, sp.bvr, sp.death,
               sp.time, (double)s_timeRefSec, sp.accuracy, sp.total);
    log_printf(LOG_INFO, "mission: result mode=%s diff=%d outcome=%s why=%s t=%.1f simtime=%.1f waves=%d/%d kills=%d/%d bvr=%d "
               "enemy_crashes=%d deaths=%d (msl %d crash %d) lives_left=%d fired=%d/%d hits=%d/%d hits_taken=%d score=%d",
               g->mode == MODE_SORTIE ? "sortie" : (g->mode == MODE_TRAINING ? "training" : "free"), g->diff,
               mission_outcome_name(s_st.outcome), why, (double)s_st.timeSec, (double)tsec(g->tick), s_st.wave, s_st.waves,
               s_st.kills, s_st.spawned, s_st.killsBvr, s_st.enemyCrashes, s_st.deaths, s_st.deathsMsl, s_st.deathsCrash,
               s_st.livesLeft, s_st.fired[0], s_st.fired[1], s_st.hits[0], s_st.hits[1], s_st.playerHits, s_st.score);
}

static void mission_end(Game *g, int outcome)
{
    if (s_st.outcome != MIS_RUNNING) return;
    s_st.outcome = outcome;
    s_st.endTick = g->tick;
    msg(outcome == MIS_COMPLETE ? "MISSION COMPLETE" : "MISSION FAILED", 6.0f);
    mission_log_result(g, "end");
}

/* ---------------------------------------------------------------- spawning */
static void place_enemy(Game *g, int id, Vec3 pos, float hdg)
{
    Aircraft *a = &g->ac[id];
    memset(a, 0, sizeof(*a));
    a->id = id;
    a->side = SIDE_RED;
    a->flags = AC_ALIVE;
    a->livery = 1;
    a->hp = 100.0f;
    flight_init(&a->fs, pos, hdg, flight_speed_target(THR_NORMAL, pos.y));
    a->fs.onGround = 0;
    a->fs.gearPos = 0.0f;
    a->fs.thr = THR_NORMAL;
    a->prev = a->fs;
    a->in.thr = THR_NORMAL;
    world_reset_aircraft(id);
    if (g->numAircraft < id + 1) g->numAircraft = id + 1;
}

static void drone_circuit(const Game *g, int k, float *cx, float *cz)
{
    const TerrainBase *b = g->terrain ? terrain_base(g->terrain) : NULL;
    Vec3 th = b ? b->threshold : v3(0.0f, 0.0f, 0.0f);
    float h = b ? E_DEG2RAD(b->rwyHeadingDeg) : 0.0f, fx = (float)sin(h), fz = (float)cos(h);
    *cx = th.x + fx * s_drAhead[k] + fz * s_drSide[k];      /* right of heading h = (cos h, -sin h) */
    *cz = th.z + fz * s_drAhead[k] - fx * s_drSide[k];
}

static void spawn_drone(Game *g, int id, int k)
{
    float cx, cz, a = rnd_range(&g->rng, 0.0f, 6.2831853f), r = s_drR[k];
    Vec3 pos;
    drone_circuit(g, k, &cx, &cz);
    pos = v3(cx + r * (float)sin(a), s_drAlt[k], cz + r * (float)cos(a));
    place_enemy(g, id, pos, a + 1.5707963f);        /* clockwise tangent */
    ai_spawn_drone(g, id, cx, cz, r, s_drAlt[k]);
    s_drone[id] = 1;
    s_droneIdx[id] = k;
    s_droneDeadTick[id] = 0;
    log_printf(LOG_INFO, "mission: drone id=%d circuit=%d centre=%.0f,%.0f r=%.0f alt=%.0f tick=%u", id, k, (double)cx, (double)cz,
               (double)r, (double)s_drAlt[k], g->tick);
}

static void spawn_wave(Game *g)
{
    const Aircraft *pl = &g->ac[PLAYER_ID];
    Vec3 pp = pl->fs.pos, sum = v3(0.0f, 0.0f, 0.0f);
    float cx, cz, b0, b, dist, brg, minKm = s_st.wave > 0 ? s_laterMinKm : s_spawnMinKm;
    float maxKm = E_MAXF(s_st.wave > 0 ? s_laterMaxKm : s_spawnMaxKm, minKm);
    int n = s_waveSize[E_CLAMP(g->diff, 0, 2)][E_CLAMP(s_st.wave, 0, 2)], k, id = 1, placed = 0;
    char buf[MIS_MSG_LEN];
    if (n > MAX_ENEMIES_ALIVE) n = MAX_ENEMIES_ALIVE;
    world_centre(g, &cx, &cz);
    b0 = pl->fs.onGround ? (float)atan2(cx - pp.x, cz - pp.z) : pl->fs.heading;
    for (k = 0; k < n; k++) {
        Vec3 pos;
        while (id < MAX_AIRCRAFT && (g->ac[id].flags & AC_ALIVE)) id++;
        if (id >= MAX_AIRCRAFT) break;
        b = b0 + rnd_range(&g->rng, -0.9f, 0.9f);
        dist = rnd_range(&g->rng, minKm * 1000.0f, maxKm * 1000.0f);
        pos = v3(pp.x + (float)sin(b) * dist, 0.0f, pp.z + (float)cos(b) * dist);
        if ((pos.x - cx) * (pos.x - cx) + (pos.z - cz) * (pos.z - cz) > 60000.0f * 60000.0f) {   /* keep inside the area */
            b = (float)atan2(cx - pp.x, cz - pp.z) + rnd_range(&g->rng, -0.6f, 0.6f);
            pos = v3(pp.x + (float)sin(b) * dist, 0.0f, pp.z + (float)cos(b) * dist);
        }
        pos.y = rnd_range(&g->rng, s_spawnAltMin, E_MAXF(s_spawnAltMax, s_spawnAltMin));
        place_enemy(g, id, pos, (float)atan2(pp.x - pos.x, pp.z - pos.z));
        ai_spawn(g, id, g->diff, 1, pp);
        s_drone[id] = 0;
        sum = v3_add(sum, pos);
        log_printf(LOG_INFO, "mission: spawn wave=%d id=%d pos=%.0f,%.0f,%.0f range=%.0f tick=%u", s_st.wave + 1, id, (double)pos.x,
                   (double)pos.y, (double)pos.z, (double)dist, g->tick);
        placed++;
        id++;
    }
    s_st.wave++;
    s_st.spawned += placed;
    s_clearTick = 0;
    if (!s_clockTick) s_clockTick = g->tick;
    if (placed > 0) {
        sum = v3_scale(sum, 1.0f / (float)placed);
        brg = E_RAD2DEG((float)atan2(sum.x - pp.x, sum.z - pp.z));
        if (brg < 0.0f) brg += 360.0f;
        e_snprintf(buf, (int)sizeof(buf), "WAVE %d: %d BANDIT%s BRG %03d", s_st.wave, placed, placed == 1 ? "" : "S",
                   (int)(brg + 0.5f) % 360);
        msg(buf, 4.0f);
        log_printf(LOG_INFO, "mission: wave=%d of %d enemies=%d brg=%.0f tick=%u", s_st.wave, s_st.waves, placed, (double)brg, g->tick);
    }
}

/* ---------------------------------------------------------------- deaths and damage */
static void player_died(Game *g, int crash, Vec3 pos)
{
    s_st.deaths++;
    if (crash) s_st.deathsCrash++; else s_st.deathsMsl++;
    s_st.damaged = 0;
    if (!crash) { s_deadPending = 1; s_deadTick = g->tick; }
    if (g->mode == MODE_SORTIE && s_lives > 0) s_st.livesLeft = s_lives > s_st.deaths ? s_lives - s_st.deaths : 0;
    log_printf(LOG_INFO, "mission: death player cause=%s pos=%.0f,%.0f,%.0f deaths=%d lives_left=%d tick=%u", crash ? "crash" : "missile",
               (double)pos.x, (double)pos.y, (double)pos.z, s_st.deaths, s_st.livesLeft, g->tick);
    if (g->mode == MODE_SORTIE && s_lives > 0 && s_st.deaths >= s_lives) mission_end(g, MIS_FAILED);
}

static void on_hit(Game *g, const WpnEvent *e)
{
    Aircraft *v;
    if (e->victim < 0 || e->victim >= g->numAircraft) return;
    v = &g->ac[e->victim];
    if (!(v->flags & AC_ALIVE)) return;
    if (e->victim == PLAYER_ID) {
        if (v->flags & AC_INVULN) {
            log_printf(LOG_INFO, "mission: hit player ignored (invulnerable) by=%d tick=%u", e->owner, g->tick);
            return;
        }
        v->hp -= s_missileDamage;
        s_st.playerHits++;
        log_printf(LOG_INFO, "mission: hit player by=%d type=%s hp=%.0f tick=%u", e->owner, e->mtype == MSL_SRM ? "SRM" : "LRM",
                   (double)v->hp, g->tick);
        if (v->hp <= 0.0f) {
            v->hp = 0.0f;
            v->flags &= ~AC_ALIVE;
            player_died(g, 0, v->fs.pos);
        } else if (!s_st.damaged && v->hp <= 100.0f - s_damagedAt) {
            s_st.damaged = 1;
            msg("DAMAGED", 2.5f);
        }
        return;
    }
    /* an enemy: one missile hit destroys (14.1) */
    v->hp = 0.0f;
    v->flags &= ~AC_ALIVE;
    if (e->owner == PLAYER_ID) {
        float ld = (e->slot >= 0 && e->slot < MAX_MISSILES) ? s_launchDist[e->slot] : 0.0f;
        s_st.kills++;
        if (e->mtype == MSL_LRM || e->mtype == MSL_SRM) s_st.hits[e->mtype]++;
        if (ld > s_bvrKm * 1000.0f) s_st.killsBvr++;
        log_printf(LOG_INFO, "mission: kill id=%d type=%s launch=%.1fkm bvr=%d kills=%d tick=%u", e->victim,
                   e->mtype == MSL_SRM ? "SRM" : "LRM", (double)(ld * 0.001f), ld > s_bvrKm * 1000.0f, s_st.kills, g->tick);
    } else {
        s_st.friendlyKills++;
        log_printf(LOG_INFO, "mission: kill id=%d by=%d (not the player) tick=%u", e->victim, e->owner, g->tick);
    }
    if (s_drone[e->victim]) s_droneDeadTick[e->victim] = g->tick;
}

static void read_weapons(Game *g)
{
    while (s_pev < weapons_event_count()) {
        const WpnEvent *e = weapons_event(s_pev++);
        if (!e) continue;
        if (e->type == WPE_LAUNCH) {
            if (e->slot >= 0 && e->slot < MAX_MISSILES)
                s_launchDist[e->slot] = (e->victim >= 0 && e->victim < g->numAircraft && e->owner >= 0 && e->owner < g->numAircraft)
                                        ? v3_len(v3_sub(g->ac[e->victim].fs.pos, g->ac[e->owner].fs.pos)) : 0.0f;
            if (e->owner == PLAYER_ID && (e->mtype == MSL_LRM || e->mtype == MSL_SRM)) s_st.fired[e->mtype]++;
        } else if (e->type == WPE_HIT) {
            on_hit(g, e);
        }
    }
}

static void read_world(Game *g)
{
    WorldEvent we;
    while (s_wev < world_event_seq()) {
        if (!world_event_get(s_wev++, &we)) continue;
        if (!(we.type & WEV_CRASH_ANY)) continue;
        if (we.ac == PLAYER_ID) player_died(g, 1, we.pos);
        else if (we.ac > 0 && we.ac < MAX_AIRCRAFT) {
            s_st.enemyCrashes++;
            if (s_drone[we.ac]) s_droneDeadTick[we.ac] = g->tick;
            log_printf(LOG_INFO, "mission: enemy crash id=%d type=%s tick=%u", we.ac, world_event_name(we.type), g->tick);
        }
    }
}

/* ---------------------------------------------------------------- start / tick */
void mission_start(Game *g)
{
    int k;
    memset(&s_st, 0, sizeof(s_st));
    s_serial++;
    s_st.mode = g->mode;
    s_st.diff = g->diff;
    s_st.outcome = MIS_RUNNING;
    s_st.waves = g->mode == MODE_SORTIE ? s_sortieWaves : 0;
    s_st.lives = g->mode == MODE_SORTIE ? s_lives : 0;
    s_st.livesLeft = s_st.lives > 0 ? s_st.lives : -1;
    s_wev = world_event_seq();
    s_pev = weapons_event_count();
    s_lastResp = session_respawns();
    s_deadPending = 0;
    s_handTick = s_clearTick = s_clockTick = 0;
    memset(s_launchDist, 0, sizeof(s_launchDist));
    memset(s_drone, 0, sizeof(s_drone));
    memset(s_droneDeadTick, 0, sizeof(s_droneDeadTick));
    memset(s_droneIdx, 0, sizeof(s_droneIdx));
    s_msgN = 0;
    if (!s_on || g->mode == MODE_FREE) return;
    dlz_set_escape_guse(ai_profile(g->diff)->gUse);  /* the player's live zone vs this difficulty's escape turn (T13) */
    if (g->mode == MODE_TRAINING) {
        int n = E_CLAMP(s_trainDrones, 0, DRONES_MAX);
        for (k = 0; k < n; k++) spawn_drone(g, 1 + k, k);
        msg(n == 1 ? "TRAINING: 1 DRONE AHEAD" : "TRAINING: DRONES AHEAD", 4.0f);
    }
    log_printf(LOG_INFO, "mission: start mode=%s diff=%d waves=%d lives=%d drones=%d", g->mode == MODE_SORTIE ? "sortie" : "training",
               g->diff, s_st.waves, s_st.lives, g->mode == MODE_TRAINING ? E_CLAMP(s_trainDrones, 0, DRONES_MAX) : 0);
}

void mission_tick(Game *g)
{
    Aircraft *pl = &g->ac[PLAYER_ID];
    int alive, i, inTakeoff;
    if (!s_on || g->mode == MODE_FREE) return;
    read_weapons(g);
    read_world(g);
    /* new life (runway restart from a death, the pause menu or the dev air start) */
    if (session_respawns() != s_lastResp) {
        s_lastResp = session_respawns();
        s_deadPending = 0;
        s_st.damaged = 0;
        s_handTick = 0;
        if (g->mode == MODE_SORTIE && s_lives > 0 && s_st.outcome == MIS_RUNNING && s_st.deaths > 0) {
            char buf[MIS_MSG_LEN];
            e_snprintf(buf, (int)sizeof(buf), s_st.livesLeft == 1 ? "LAST LIFE" : "LIVES LEFT %d", s_st.livesLeft);
            msg(buf, 3.0f);
        }
        log_printf(LOG_INFO, "mission: respawn player respawns=%d tick=%u", s_lastResp, g->tick);
    }
    alive = (pl->flags & AC_ALIVE) != 0;
    /* missile death: wreck camera, then the runway (crashes restart through the session's own crash flow) */
    if (s_deadPending && !alive && g->tick - s_deadTick >= ticks_of(s_deathRespawnSec)) {
        s_deadPending = 0;
        session_runway(g, "killed");
        s_lastResp = session_respawns();
        s_st.damaged = 0;
        s_handTick = 0;
        alive = 1;
        if (g->mode == MODE_SORTIE && s_lives > 0 && s_st.outcome == MIS_RUNNING) {
            char buf[MIS_MSG_LEN];
            e_snprintf(buf, (int)sizeof(buf), s_st.livesLeft == 1 ? "LAST LIFE" : "LIVES LEFT %d", s_st.livesLeft);
            msg(buf, 3.0f);
        }
    }
    /* invulnerable during the takeoff and invulnSec after the handover */
    inTakeoff = takeoff_active(session_takeoff());
    if (alive) {
        if (inTakeoff) { pl->flags |= AC_INVULN; s_handTick = 0; }
        else {
            if (!s_handTick) s_handTick = g->tick;
            if (g->tick - s_handTick < ticks_of(s_invulnSec)) pl->flags |= AC_INVULN;
            else pl->flags &= ~AC_INVULN;
        }
    }
    s_st.invuln = (pl->flags & AC_INVULN) != 0;
    s_st.enemiesAlive = count_alive_enemies(g);
    if (g->mode == MODE_TRAINING) {
        if (s_trainUnlimited && alive) {             /* top up (not weapons_refill: that would reset the cooldown) */
            const WpnState *w = weapons_state(PLAYER_ID);
            const Param *pl0 = params_find("weapons", "lrmCount"), *ps0 = params_find("weapons", "srmCount"),
                        *pf0 = params_find("weapons", "flareCount");
            int nl = pl0 ? *(const int *)pl0->ptr : 16, ns = ps0 ? *(const int *)ps0->ptr : 40, nf = pf0 ? *(const int *)pf0->ptr : 40;
            if (w->lrm < nl || w->srm < ns || w->flares < nf) weapons_set_stores(PLAYER_ID, nl, ns, nf);
        }
        for (i = 1; i < g->numAircraft; i++)
            if (s_drone[i] && !(g->ac[i].flags & AC_ALIVE) && s_droneDeadTick[i] &&
                g->tick - s_droneDeadTick[i] >= ticks_of(s_trainRespawnSec))
                spawn_drone(g, i, s_droneIdx[i]);
        return;
    }
    /* SORTIE (sortieWaves is live: F5 / a script param line can lengthen a running sortie) */
    if (s_st.outcome != MIS_RUNNING) return;
    s_st.waves = s_sortieWaves;
    if (s_clockTick) s_st.timeSec = tsec(g->tick - s_clockTick);
    if (s_st.wave == 0) {
        if (alive && !inTakeoff) spawn_wave(g);     /* after the handover (air start: at once) */
    } else if (s_st.enemiesAlive == 0) {
        if (!s_clearTick) {
            s_clearTick = g->tick;
            log_printf(LOG_INFO, "mission: wave=%d cleared t=%.1f tick=%u", s_st.wave, (double)s_st.timeSec, g->tick);
            if (s_st.wave < s_st.waves) {           /* T20: tell the player the next wave is coming */
                char buf[MIS_MSG_LEN];
                e_snprintf(buf, (int)sizeof(buf), "WAVE %d CLEARED", s_st.wave);
                msg(buf, 3.0f);
            }
        }
        if (s_st.wave >= s_st.waves) {
            if (g->tick - s_clearTick >= ticks_of(s_completeDelaySec)) mission_end(g, MIS_COMPLETE);
        } else if (g->tick - s_clearTick >= ticks_of(s_waveDelaySec)) spawn_wave(g);
    }
    s_st.score = compute_score();
}

/* ---------------------------------------------------------------- T20: results + records.ini */
static RecordRow s_rec[3];
static int   s_recLoaded;
static char  s_recPath[E_PATH];                     /* "" = base_path("records.ini") */
static u32   s_subSerial;                           /* mission serial of the last records_submit (0 = none) */
static int   s_subNew, s_subPrev;
static const char *const s_diffName[3] = { "easy", "normal", "hard" };

const char *records_diff_name(int diff) { return s_diffName[E_CLAMP(diff, 0, 2)]; }

void records_set_path(const char *path)
{
    e_strlcpy(s_recPath, path ? path : "", (int)sizeof(s_recPath));
    s_recLoaded = 0;
}

static void rec_path(char *out, int n)
{
    if (s_recPath[0]) e_strlcpy(out, s_recPath, n);
    else base_path(out, n, "records.ini");
}

/* an int key that must be present and within lo..hi */
static int rec_int(const Cfg *c, const char *sec, const char *key, int lo, int hi, int *out)
{
    int v;
    if (!e_parse_int(cfg_str(c, sec, key, ""), &v) || v < lo || v > hi) return 0;
    *out = v;
    return 1;
}

int records_load(void)
{
    char path[E_PATH];
    Cfg *c;
    int d, n = 0, ok, tenths;
    const char *s;
    memset(s_rec, 0, sizeof(s_rec));
    s_recLoaded = 1;
    rec_path(path, (int)sizeof(path));
    if (!file_exists(path)) { log_printf(LOG_INFO, "records: none path=%s", path); return 0; }
    c = cfg_load(path);
    for (d = 0; d < 3; d++) {
        RecordRow *r = &s_rec[d];
        const char *sec = s_diffName[d];
        ok = rec_int(c, sec, "score", 0, 99999999, &r->score) &&
             rec_int(c, sec, "timeTenths", 0, 360000 * 10, &tenths) &&
             rec_int(c, sec, "waves", 0, 99, &r->waves) && rec_int(c, sec, "kills", 0, 9999, &r->kills) &&
             rec_int(c, sec, "spawned", 0, 9999, &r->spawned) && rec_int(c, sec, "bvr", 0, 9999, &r->killsBvr) &&
             rec_int(c, sec, "deaths", 0, 9999, &r->deaths) &&
             rec_int(c, sec, "lrmFired", 0, 99999, &r->fired[0]) && rec_int(c, sec, "lrmHits", 0, 99999, &r->hits[0]) &&
             rec_int(c, sec, "srmFired", 0, 99999, &r->fired[1]) && rec_int(c, sec, "srmHits", 0, 99999, &r->hits[1]);
        s = cfg_str(c, sec, "outcome", "");
        if (!strcmp(s, "complete")) r->outcome = MIS_COMPLETE;
        else if (!strcmp(s, "failed")) r->outcome = MIS_FAILED;
        else ok = 0;
        if (ok && (r->hits[0] > r->fired[0] || r->hits[1] > r->fired[1] || r->kills > r->spawned)) ok = 0;
        if (!ok) {
            if (cfg_has(c, sec, "score")) log_printf(LOG_WARN, "records: bad row [%s] ignored path=%s", sec, path);
            memset(r, 0, sizeof(*r));
            continue;
        }
        r->timeSec = (float)tenths * 0.1f;
        e_strlcpy(r->date, cfg_str(c, sec, "date", "?"), REC_DATE_LEN);
        r->valid = 1;
        n++;
    }
    cfg_free(c);
    log_printf(LOG_INFO, "records: loaded rows=%d path=%s easy=%d normal=%d hard=%d", n, path, s_rec[0].valid ? s_rec[0].score : -1,
               s_rec[1].valid ? s_rec[1].score : -1, s_rec[2].valid ? s_rec[2].score : -1);
    return n;
}

int records_get(int diff, RecordRow *out)
{
    if (!s_recLoaded) records_load();
    if (diff < 0 || diff > 2) { if (out) memset(out, 0, sizeof(*out)); return 0; }
    if (out) *out = s_rec[diff];
    return s_rec[diff].valid;
}

void records_reset(int diff)
{
    int d;
    if (!s_recLoaded) records_load();
    for (d = 0; d < 3; d++) if (diff < 0 || diff == d) memset(&s_rec[d], 0, sizeof(s_rec[d]));
}

int records_save(void)
{
    char path[E_PATH];
    static const char head[] = "; Ashfall: Island Intercept - best SORTIE score per difficulty. Written by jet.exe; delete to reset.\r\n";
    Cfg *c = cfg_parse(head, (int)sizeof(head) - 1);
    int d, ok;
    if (!s_recLoaded) records_load();
    for (d = 0; d < 3; d++) {
        const RecordRow *r = &s_rec[d];
        const char *sec = s_diffName[d];
        if (!r->valid) continue;
        cfg_set_int(c, sec, "score", r->score);
        cfg_set_str(c, sec, "date", r->date);
        cfg_set_str(c, sec, "outcome", mission_outcome_name(r->outcome));
        cfg_set_int(c, sec, "timeTenths", (int)(r->timeSec * 10.0f + 0.5f));
        cfg_set_int(c, sec, "waves", r->waves);
        cfg_set_int(c, sec, "kills", r->kills);
        cfg_set_int(c, sec, "spawned", r->spawned);
        cfg_set_int(c, sec, "bvr", r->killsBvr);
        cfg_set_int(c, sec, "deaths", r->deaths);
        cfg_set_int(c, sec, "lrmFired", r->fired[0]);
        cfg_set_int(c, sec, "lrmHits", r->hits[0]);
        cfg_set_int(c, sec, "srmFired", r->fired[1]);
        cfg_set_int(c, sec, "srmHits", r->hits[1]);
    }
    rec_path(path, (int)sizeof(path));
    ok = cfg_save(c, path);
    cfg_free(c);
    log_printf(ok ? LOG_INFO : LOG_WARN, "records: save path=%s ok=%d", path, ok);
    return ok;
}

int mission_results(const Game *g, MissionResults *out)
{
    ScoreIn in;
    int d = E_CLAMP(s_st.diff, 0, 2);
    memset(out, 0, sizeof(*out));
    out->mode = s_st.mode;
    out->diff = s_st.diff;
    out->outcome = s_st.outcome;
    out->timeSec = s_st.timeSec;
    out->wave = s_st.wave;
    out->waves = s_st.waves;
    out->kills = s_st.kills;
    out->spawned = s_st.spawned;
    out->killsBvr = s_st.killsBvr;
    out->fired[0] = s_st.fired[0]; out->fired[1] = s_st.fired[1];
    out->hits[0] = s_st.hits[0]; out->hits[1] = s_st.hits[1];
    out->deaths = s_st.deaths;
    out->hitsTaken = s_st.playerHits;
    score_in(&in);
    out->score = mission_score(&in, &out->parts);
    out->valid = s_on && g && g->mode == MODE_SORTIE && s_st.mode == MODE_SORTIE && s_st.outcome != MIS_RUNNING;
    out->prevBest = -1;
    if (!out->valid) return 0;
    if (s_subSerial == s_serial) {                  /* already submitted: report what the submit decided */
        out->newRecord = s_subNew;
        out->prevBest = s_subPrev;
    } else {
        if (!s_recLoaded) records_load();
        out->prevBest = s_rec[d].valid ? s_rec[d].score : -1;
        out->newRecord = out->score > out->prevBest;
    }
    return 1;
}

int records_submit(MissionResults *r)
{
    RecordRow *row;
    int d;
    if (!r || !r->valid) return 0;
    if (s_subSerial == s_serial) { r->newRecord = s_subNew; r->prevBest = s_subPrev; return s_subNew; }
    if (!s_recLoaded) records_load();
    d = E_CLAMP(r->diff, 0, 2);
    row = &s_rec[d];
    s_subSerial = s_serial;
    s_subPrev = row->valid ? row->score : -1;
    s_subNew = r->score > s_subPrev;
    r->prevBest = s_subPrev;
    r->newRecord = s_subNew;
    if (s_subNew) {
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);
        memset(row, 0, sizeof(*row));
        row->valid = 1;
        row->score = r->score;
        row->outcome = r->outcome;
        if (lt) strftime(row->date, REC_DATE_LEN, "%Y-%m-%d %H:%M", lt);
        else e_strlcpy(row->date, "?", REC_DATE_LEN);
        row->timeSec = r->timeSec;
        row->waves = r->wave;
        row->kills = r->kills;
        row->spawned = r->spawned;
        row->killsBvr = r->killsBvr;
        row->deaths = r->deaths;
        row->fired[0] = r->fired[0]; row->fired[1] = r->fired[1];
        row->hits[0] = r->hits[0]; row->hits[1] = r->hits[1];
        records_save();
    }
    log_printf(LOG_INFO, "records: submit diff=%s score=%d prev=%d new=%d", s_diffName[d], r->score, s_subPrev, s_subNew);
    return s_subNew;
}
