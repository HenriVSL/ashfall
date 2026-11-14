/* radar.cpp - detection, identification, tracks, memory, designation + lock, RWR (DESIGN 11). Portable.
   Owner: T11. Rules and API: radar.h. All state lives in the static arrays below (per observer per aircraft,
   Aircraft.id indices); radar_reset clears them at session start. Deterministic: no RNG, integer ticks. */
#include "radar.h"
#include "params.h"
#include "e_log.h"
#include "e_terrain.h"

/* ---------------------------------------------------------------- [radar] */
static float s_detNm, s_aspGain, s_azDeg, s_elDeg, s_idNm, s_idSec, s_idInstNm, s_lostSec, s_memSec;
static float s_lockGimDeg, s_lockMul, s_lockLostSec, s_lockMemSec, s_latchSec, s_splashSec, s_rwrSrmM;
static float s_detPlNm, s_idPlNm;                   /* T21a: the player's radar (observer with AC_PLAYER) */

static const Param s_tab[] = {
    { "radar", "detRangeNm", P_FLOAT, &s_detNm, "20", 1.0f, 200.0f,
      "detection range (NM) for a head-on or tail-on target; beam aspect adds aspectGain" },
    { "radar", "aspectGain", P_FLOAT, &s_aspGain, "0.25", 0.0f, 2.0f,
      "detection range x (1 + aspectGain * sin^2(aspect)): beam targets are seen further" },
    { "radar", "scanAzDeg", P_FLOAT, &s_azDeg, "60", 5.0f, 89.0f, "scan volume half-width left/right of the nose (deg)" },
    { "radar", "scanElDeg", P_FLOAT, &s_elDeg, "45", 5.0f, 89.0f, "scan volume half-height above/below the nose (deg)" },
    { "radar", "idRangeNm", P_FLOAT, &s_idNm, "15", 0.0f, 200.0f, "identification range (NM): ID after idSec of track inside it" },
    { "radar", "idSec", P_FLOAT, &s_idSec, "1.5", 0.0f, 30.0f, "track time inside idRangeNm until unknown -> hostile (s)" },
    { "radar", "idInstantNm", P_FLOAT, &s_idInstNm, "5", 0.0f, 200.0f, "instant identification inside this range (NM)" },
    { "radar", "lostSec", P_FLOAT, &s_lostSec, "0.5", 0.1f, 10.0f, "not seen this long -> MEMORY (s)" },
    { "radar", "memorySec", P_FLOAT, &s_memSec, "6", 0.5f, 120.0f, "MEMORY (dashed, extrapolated) this long -> dropped (s)" },
    { "radar", "lockGimbalDeg", P_FLOAT, &s_lockGimDeg, "70", 5.0f, 180.0f, "lock (STT) gimbal: max angle off the nose (deg)" },
    { "radar", "lockRangeMul", P_FLOAT, &s_lockMul, "1.2", 0.1f, 5.0f, "lock range = lockRangeMul x the detection range" },
    /* T21a (user M2 play-test: the player's AIM-120 flies ~2x as far, ~38 km head-on): the PLAYER's radar sees and
       identifies further so a long shot has a target; enemy radars keep detRangeNm / idRangeNm (enemy sensors unchanged) */
    { "radar", "playerDetRangeNm", P_FLOAT, &s_detPlNm, "40", 1.0f, 200.0f,
      "detection range of the PLAYER's radar head-on/tail (NM; x (1 + aspectGain sin^2 aspect)); enemies use detRangeNm" },
    { "radar", "playerIdRangeNm", P_FLOAT, &s_idPlNm, "25", 0.0f, 200.0f,
      "identification range of the PLAYER's radar (NM); enemies use idRangeNm" },
    { "radar", "lockLostSec", P_FLOAT, &s_lockLostSec, "1.0", 0.1f, 10.0f, "lock test failing this long -> lock MEMORY (s)" },
    { "radar", "lockMemorySec", P_FLOAT, &s_lockMemSec, "3.0", 0.1f, 30.0f, "lock MEMORY this long -> designation dropped (s)" },
    { "radar", "latchSec", P_FLOAT, &s_latchSec, "0.3", 0.0f, 2.0f, "displayed contact states/flags hold at least this long (s)" },
    { "radar", "splashSec", P_FLOAT, &s_splashSec, "2.0", 0.0f, 10.0f, "dead contacts stay listed (SPLASH) this long (s)" },
    { "radar", "rwrSrmWarnM", P_FLOAT, &s_rwrSrmM, "5000", 0.0f, 50000.0f,
      "RWR: an SRM homing on the player gives MISSILE when launched inside or now within this range (m)" },
};

void radar_params(void) { params_add(s_tab, E_COUNT(s_tab)); }

/* ---------------------------------------------------------------- state */
typedef struct Contact {
    int state;                                      /* CS_* (sim) */
    int hostile;                                    /* ID done */
    u32 lastSeen;                                   /* tick of the last seen scan (0 = never) */
    u32 idStart;                                    /* first seen scan inside idRange of this track (0 = none) */
    u32 memTick;                                    /* MEMORY / DEAD anchor tick */
    Vec3 memPos, memVel;
    /* display (latched) */
    int dState, dPrev, dFlags, dHostile;
    u32 dStateTick, dFlagTick;
} Contact;

typedef struct Observer {
    int des, lock, sel;                             /* designated id, LOCK_*, selected id (display flag) */
    u32 lockOkTick, lockMemTick;
    int kick;                                       /* apply flag changes at once on the next display update */
    int active;                                     /* alive at the last tick (dead -> clear once) */
} Observer;

static Contact s_c[MAX_AIRCRAFT][MAX_AIRCRAFT];
static Observer s_o[MAX_AIRCRAFT];
static RadarEvent s_ev[RADAR_EVENTS_MAX];
static u32 s_evSeq;
static RadarStats s_st;

static const char *const s_evNames[] = { "NONE", "DETECT", "IDENT", "MEMORY", "REACQUIRE", "DROP", "DEAD",
                                         "DESIGNATE", "UNDESIGNATE", "LOCK", "LOCK_MEMORY", "LAUNCH" };
static const char *const s_csNames[] = { "NONE", "DETECTED", "HOSTILE", "MEMORY", "DEAD" };

const char *radar_event_name(int type) { return (type >= 0 && type < E_COUNT(s_evNames)) ? s_evNames[type] : "?"; }
const char *radar_state_name(int cs) { return (cs >= 0 && cs < E_COUNT(s_csNames)) ? s_csNames[cs] : "?"; }
const RadarStats *radar_stats(void) { return &s_st; }

static int sec_ticks(float s) { return (int)(s * (float)SIM_HZ + 0.5f); }

static void ev_push(u32 tick, int type, int obs, int id, int arg)
{
    RadarEvent *e = &s_ev[s_evSeq % RADAR_EVENTS_MAX];
    int lvl = (type >= REV_DEAD) ? LOG_INFO : LOG_DEBUG;
    e->type = type;
    e->obs = obs;
    e->id = id;
    e->arg = arg;
    e->tick = tick;
    s_evSeq++;
    log_printf(lvl, "radar: %s obs=%d id=%d arg=%d tick=%u", radar_event_name(type), obs, id, arg, tick);
}

u32 radar_event_seq(void) { return s_evSeq; }

int radar_event_get(u32 seq, RadarEvent *ev)
{
    if (seq >= s_evSeq || s_evSeq - seq > RADAR_EVENTS_MAX) return 0;
    *ev = s_ev[seq % RADAR_EVENTS_MAX];
    return 1;
}

/* ---------------------------------------------------------------- helpers */
static int alive(const Game *g, int id) { return id >= 0 && id < g->numAircraft && (g->ac[id].flags & AC_ALIVE); }

static int pair_ok(const Game *g, int obs, int id)
{
    return obs >= 0 && id >= 0 && obs < g->numAircraft && id < g->numAircraft && obs != id &&
           g->ac[obs].side != g->ac[id].side;
}

static Vec3 ac_vel(const FlightState *fs) { return q_rotate(fs->qPath, v3(0.0f, 0.0f, fs->V)); }

static Quat q_conj(Quat q) { q.x = -q.x; q.y = -q.y; q.z = -q.z; return q; }

static float bearing_of(Vec3 d) { return (float)atan2(d.x, d.z); }

static float acos_c(float c) { return (float)acos(E_CLAMP(c, -1.0f, 1.0f)); }

/* trig of the volume/gimbal params, refreshed once per tick and per public call (params may change on F5) */
static float s_tanAz, s_sinEl, s_cosGim;
static void trig_cache(void)
{
    s_tanAz = (float)tan(E_DEG2RAD(s_azDeg));
    s_sinEl = (float)sin(E_DEG2RAD(s_elDeg));
    s_cosGim = (float)cos(E_DEG2RAD(s_lockGimDeg));
}

/* geometry of the pair: no trig on the decision path (angles only when 'angles' is set: radar_geom) */
static void geom_calc(const Game *g, const Aircraft *A, const Aircraft *B, int doLos, int angles, RadarGeom *o)
{
    Vec3 d = v3_sub(B->fs.pos, A->fs.pos), b, f;
    float R2 = v3_dot(d, d), R, inv, cosA, sin2, rdet, lockR;
    Quat qi = q_conj(flight_nose(&A->fs));
    R = (float)sqrt(R2);
    inv = R > 1e-3f ? 1.0f / R : 0.0f;
    b = q_rotate(qi, d);
    f = q_rotate(B->fs.qPath, v3(0.0f, 0.0f, 1.0f));
    cosA = v3_dot(f, d) * inv;
    sin2 = 1.0f - cosA * cosA;
    if (sin2 < 0.0f) sin2 = 0.0f;
    rdet = ((A->flags & AC_PLAYER) ? s_detPlNm : s_detNm) * NM2M * (1.0f + s_aspGain * sin2);
    lockR = s_lockMul * rdet;
    o->range = R;
    o->rdet = rdet;
    if (angles) {
        o->az = (float)atan2(b.x, b.z);
        o->el = (float)asin(E_CLAMP(b.y * inv, -1.0f, 1.0f));
        o->offNose = acos_c(b.z * inv);
    }
    o->inVolume = b.z > 0.0f && (float)fabs(b.x) <= s_tanAz * b.z && (float)fabs(b.y) <= s_sinEl * R;
    o->inRange = R2 <= rdet * rdet;
    o->inGimbal = b.z >= s_cosGim * R;
    o->inLockRange = R2 <= lockR * lockR;
    o->los = 0;
    if (doLos) {
        o->los = 1;
        if (g->terrain && g->terrain->loaded) {
            o->los = terrain_los(g->terrain, &A->fs.pos, &B->fs.pos);
            s_st.losCalls++;
        }
    }
}

void radar_geom(const Game *g, int obs, int id, int doLos, RadarGeom *out)
{
    memset(out, 0, sizeof(*out));
    if (obs < 0 || id < 0 || obs >= g->numAircraft || id >= g->numAircraft || obs == id) return;
    trig_cache();
    geom_calc(g, &g->ac[obs], &g->ac[id], doLos, 1, out);
}

/* ---------------------------------------------------------------- contact state machine */
static void contact_seen(const Game *g, int obs, int id, float range)
{
    Contact *c = &s_c[obs][id];
    u32 tick = g->tick;
    int wasHostile = c->hostile;
    if (c->state == CS_DEAD) return;
    if (c->state == CS_NONE) {
        c->state = CS_DETECTED;
        c->hostile = 0;
        c->idStart = 0;
        ev_push(tick, REV_DETECT, obs, id, 0);
    } else if (c->state == CS_MEMORY) {
        c->state = c->hostile ? CS_HOSTILE : CS_DETECTED;
        ev_push(tick, REV_REACQUIRE, obs, id, 0);
    }
    c->lastSeen = tick;
    if (!c->hostile) {
        if (range <= s_idInstNm * NM2M) c->hostile = 1;
        else if (range <= ((g->ac[obs].flags & AC_PLAYER) ? s_idPlNm : s_idNm) * NM2M) {
            if (!c->idStart) c->idStart = tick;
            if ((int)(tick - c->idStart) >= sec_ticks(s_idSec)) c->hostile = 1;
        } else c->idStart = 0;
    }
    if (c->hostile && !wasHostile) {
        c->state = CS_HOSTILE;
        ev_push(tick, REV_IDENT, obs, id, 0);
    }
}

static void contact_unseen(const Game *g, int obs, int id)
{
    Contact *c = &s_c[obs][id];
    u32 tick = g->tick;
    if ((c->state == CS_DETECTED || c->state == CS_HOSTILE) && (int)(tick - c->lastSeen) >= sec_ticks(s_lostSec)) {
        c->state = CS_MEMORY;
        c->memPos = g->ac[id].fs.pos;               /* the radar coasted the track until now: no symbol jump */
        c->memVel = ac_vel(&g->ac[id].fs);
        c->memTick = tick;
        c->idStart = 0;
        ev_push(tick, REV_MEMORY, obs, id, 0);
    } else if (c->state == CS_MEMORY && (int)(tick - c->memTick) >= sec_ticks(s_memSec)) {
        c->state = CS_NONE;
        c->hostile = 0;
        ev_push(tick, REV_DROP, obs, id, 0);
    }
}

/* ---------------------------------------------------------------- designation + lock */
static void undesignate(u32 tick, int obs, int why)
{
    Observer *o = &s_o[obs];
    if (o->des < 0) return;
    ev_push(tick, REV_UNDESIGNATE, obs, o->des, why);
    o->des = -1;
    o->lock = LOCK_NONE;
}

static void lock_result(const Game *g, int obs, int ok)
{
    Observer *o = &s_o[obs];
    u32 tick = g->tick;
    if (o->des < 0) return;
    if (ok) {
        o->lockOkTick = tick;
        if (o->lock != LOCK_TRACK) {
            o->lock = LOCK_TRACK;
            ev_push(tick, REV_LOCK, obs, o->des, 0);
        }
    } else if (o->lock == LOCK_TRACK) {
        if ((int)(tick - o->lockOkTick) >= sec_ticks(s_lockLostSec)) {
            o->lock = LOCK_MEMORY;
            o->lockMemTick = tick;
            ev_push(tick, REV_LOCK_MEMORY, obs, o->des, 0);
        }
    } else if (o->lock == LOCK_MEMORY) {
        if ((int)(tick - o->lockMemTick) >= sec_ticks(s_lockMemSec)) undesignate(tick, obs, 1);
    }
}

int radar_designate(Game *g, int obs, int id)
{
    Observer *o;
    RadarGeom G;
    int ok;
    if (obs < 0 || obs >= MAX_AIRCRAFT) return LOCK_NONE;
    o = &s_o[obs];
    if (id >= 0 && id == o->des) return o->lock;
    if (o->des >= 0) undesignate(g->tick, obs, 0);
    if (id < 0 || !pair_ok(g, obs, id) || !alive(g, obs) || !alive(g, id) || s_c[obs][id].state == CS_DEAD)
        return LOCK_NONE;
    o->des = id;
    ev_push(g->tick, REV_DESIGNATE, obs, id, 0);
    trig_cache();
    geom_calc(g, &g->ac[obs], &g->ac[id], 0, 0, &G);
    ok = G.inGimbal && G.inLockRange;
    if (ok) {
        geom_calc(g, &g->ac[obs], &g->ac[id], 1, 0, &G);
        ok = G.los;
    }
    if (ok) {
        o->lock = LOCK_NONE;
        lock_result(g, obs, 1);
        contact_seen(g, obs, id, G.range);          /* STT is a track */
    } else {
        o->lock = LOCK_MEMORY;                      /* designated outside the lock limits: TD dashed, drops */
        o->lockMemTick = g->tick;
        ev_push(g->tick, REV_LOCK_MEMORY, obs, id, 0);
    }
    return o->lock;
}

int radar_designated(int obs) { return (obs >= 0 && obs < MAX_AIRCRAFT) ? s_o[obs].des : -1; }
int radar_lock(int obs) { return (obs >= 0 && obs < MAX_AIRCRAFT) ? s_o[obs].lock : LOCK_NONE; }
void radar_set_selected(int obs, int id) { if (obs >= 0 && obs < MAX_AIRCRAFT) s_o[obs].sel = id; }
void radar_view_kick(int obs) { if (obs >= 0 && obs < MAX_AIRCRAFT) s_o[obs].kick = 1; }

u32 radar_locked_by(int id)
{
    u32 m = 0;
    int o;
    for (o = 0; o < MAX_AIRCRAFT; o++)
        if (s_o[o].des == id && s_o[o].lock == LOCK_TRACK) m |= 1u << o;
    return m;
}

/* ---------------------------------------------------------------- tick */
static void scan_pair(Game *g, int obs, int id)
{
    RadarGeom G;
    int isDes = s_o[obs].des == id, scanOk, lockCand, seen;
    geom_calc(g, &g->ac[obs], &g->ac[id], 0, 0, &G);
    scanOk = G.inVolume && G.inRange;
    lockCand = isDes && G.inGimbal && G.inLockRange;
    if (scanOk || lockCand) {
        G.los = 1;
        if (g->terrain && g->terrain->loaded) {
            G.los = terrain_los(g->terrain, &g->ac[obs].fs.pos, &g->ac[id].fs.pos);
            s_st.losCalls++;
        }
    }
    seen = (scanOk || lockCand) && G.los;
    s_st.scans++;
    if (seen) contact_seen(g, obs, id, G.range);
    else contact_unseen(g, obs, id);
    if (isDes) lock_result(g, obs, lockCand && G.los);
}

static int want_flags(int obs, int id)
{
    const Observer *o = &s_o[obs];
    int f = 0;
    if (o->sel == id) f |= CVF_SELECTED;
    if (o->des == id) {
        f |= CVF_DESIGNATED;
        if (o->lock == LOCK_TRACK) f |= CVF_LOCKED;
        else if (o->lock == LOCK_MEMORY) f |= CVF_LOCKMEM;
    }
    return f;
}

static void display_update(const Game *g, int obs)
{
    int id, latch = sec_ticks(s_latchSec), wf;
    u32 tick = g->tick;
    for (id = 0; id < MAX_AIRCRAFT; id++) {
        Contact *c = &s_c[obs][id];
        if (c->state != c->dState) {
            int now = c->dState == CS_NONE || c->state == CS_DEAD || c->dState == CS_DEAD ||
                      (int)(tick - c->dStateTick) >= latch;
            if (now) {
                c->dPrev = c->dState;
                c->dState = c->state;
                c->dStateTick = tick;
                c->dHostile = c->state == CS_NONE ? 0 : c->hostile;
            }
        }
        wf = c->dState == CS_NONE ? 0 : want_flags(obs, id);
        if (wf != c->dFlags && (s_o[obs].kick || (int)(tick - c->dFlagTick) >= latch)) {
            c->dFlags = wf;
            c->dFlagTick = tick;
        }
    }
    s_o[obs].kick = 0;
}

static void observer_clear(u32 tick, int obs)
{
    int id;
    undesignate(tick, obs, 3);
    for (id = 0; id < MAX_AIRCRAFT; id++) {
        Contact *c = &s_c[obs][id];
        memset(c, 0, sizeof(*c));
    }
    s_o[obs].sel = -1;
    s_o[obs].active = 0;
}

void radar_tick(Game *g)
{
    int obs, id;
    u32 tick = g->tick;
    s_st.ticks++;
    trig_cache();
    for (obs = 0; obs < MAX_AIRCRAFT; obs++) {
        if (!alive(g, obs)) {                       /* dead or unused slot: no contacts, no designation */
            if (s_o[obs].active) observer_clear(tick, obs);
            continue;
        }
        s_o[obs].active = 1;
        for (id = 0; id < g->numAircraft; id++) {
            Contact *c;
            if (!pair_ok(g, obs, id)) continue;
            c = &s_c[obs][id];
            if (c->state == CS_DEAD) {               /* SPLASH time, also when it respawned meanwhile */
                if ((int)(tick - c->memTick) >= sec_ticks(s_splashSec)) {
                    c->state = CS_NONE;
                    c->hostile = 0;
                    ev_push(tick, REV_DROP, obs, id, 1);
                }
                continue;
            }
            if (!alive(g, id)) {
                if (c->state != CS_NONE) {
                    c->state = CS_DEAD;
                    c->memPos = g->ac[id].fs.pos;
                    c->memVel = v3(0.0f, 0.0f, 0.0f);
                    c->memTick = tick;
                    ev_push(tick, REV_DEAD, obs, id, 0);
                }
                if (s_o[obs].des == id) undesignate(tick, obs, 2);
                continue;
            }
            if (((tick + (u32)obs * 5u + (u32)id * 7u) % 12u) == 0) scan_pair(g, obs, id);
        }
        display_update(g, obs);
    }
}

void radar_reset(Game *g)
{
    int i;
    (void)g;
    memset(s_c, 0, sizeof(s_c));
    memset(s_o, 0, sizeof(s_o));
    for (i = 0; i < MAX_AIRCRAFT; i++) { s_o[i].des = -1; s_o[i].sel = -1; }
    memset(s_ev, 0, sizeof(s_ev));
    s_evSeq = 0;
    memset(&s_st, 0, sizeof(s_st));
}

/* ---------------------------------------------------------------- queries */
int radar_contact_state(int obs, int id)
{
    if (obs < 0 || id < 0 || obs >= MAX_AIRCRAFT || id >= MAX_AIRCRAFT) return CS_NONE;
    return s_c[obs][id].state;
}

int radar_identified(int obs, int id)
{
    if (obs < 0 || id < 0 || obs >= MAX_AIRCRAFT || id >= MAX_AIRCRAFT) return 0;
    return s_c[obs][id].state != CS_NONE && s_c[obs][id].hostile;
}

int radar_track(const Game *g, int obs, int id, Vec3 *pos, Vec3 *vel)
{
    const Contact *c;
    float dt;
    if (obs < 0 || id < 0 || obs >= g->numAircraft || id >= g->numAircraft) return 0;
    c = &s_c[obs][id];
    if (c->state == CS_DETECTED || c->state == CS_HOSTILE) {
        if (pos) *pos = g->ac[id].fs.pos;
        if (vel) *vel = ac_vel(&g->ac[id].fs);
        return 1;
    }
    if (c->state != CS_MEMORY) return 0;
    dt = (float)(int)(g->tick - c->memTick) * SIM_DT;
    if (pos) *pos = v3_madd(c->memPos, c->memVel, dt);
    if (vel) *vel = c->memVel;
    return 2;
}

static void fill_view(const Game *g, int obs, int id, ContactView *v)
{
    const Contact *c = &s_c[obs][id];
    const Aircraft *A = &g->ac[obs], *B = &g->ac[id];
    Vec3 p, vel, d, rel, b;
    float R, dt;
    memset(v, 0, sizeof(*v));
    v->id = id;
    v->state = c->dState;
    v->prevState = c->dPrev;
    v->flags = c->dFlags | (c->dHostile ? CVF_IDENTIFIED : 0);
    v->simState = c->state;
    v->stateAge = (float)(int)(g->tick - c->dStateTick) * SIM_DT;
    v->memPos = c->memPos;
    v->memVel = c->memVel;
    v->memTick = c->memTick;
    if (c->dState == CS_DETECTED || c->dState == CS_HOSTILE) {
        p = B->fs.pos;
        vel = ac_vel(&B->fs);
    } else if (c->dState == CS_MEMORY) {
        dt = (float)((int)(g->tick - c->memTick) - 1) * SIM_DT;   /* fs time of the last tick = g->tick - 1 */
        p = v3_madd(c->memPos, c->memVel, dt);
        vel = c->memVel;
    } else {
        p = c->memPos;
        vel = v3(0.0f, 0.0f, 0.0f);
    }
    d = v3_sub(p, A->fs.pos);
    R = v3_len(d);
    rel = v3_sub(vel, ac_vel(&A->fs));
    b = q_rotate(q_conj(flight_nose(&A->fs)), d);
    v->range = R;
    v->offNose = R > 1e-3f ? acos_c(b.z / R) : 0.0f;
    v->bearing = bearing_of(d);
    v->closure = R > 1e-3f ? -v3_dot(rel, d) / R : 0.0f;
    v->altitude = p.y;
    v->heading = (vel.x != 0.0f || vel.z != 0.0f) ? bearing_of(vel) : B->fs.heading;
}

int radar_views(const Game *g, int obs, ContactView *out, int maxOut)
{
    int id, n = 0;
    if (obs < 0 || obs >= g->numAircraft) return 0;
    for (id = 0; id < g->numAircraft && n < maxOut; id++) {
        if (id == obs || s_c[obs][id].dState == CS_NONE) continue;
        fill_view(g, obs, id, &out[n++]);
    }
    return n;
}

int radar_view_get(const Game *g, int obs, int id, ContactView *out)
{
    if (obs < 0 || id < 0 || obs >= g->numAircraft || id >= g->numAircraft || id == obs) return 0;
    if (s_c[obs][id].dState == CS_NONE) return 0;
    fill_view(g, obs, id, out);
    return 1;
}

int radar_view_pos(const Game *g, const ContactView *v, float alpha, Vec3 *pos)
{
    const Aircraft *B;
    float dt;
    if (v->id < 0 || v->id >= g->numAircraft) return CS_NONE;
    B = &g->ac[v->id];
    if (v->state == CS_DETECTED || v->state == CS_HOSTILE) *pos = v3_lerp(B->prev.pos, B->fs.pos, alpha);
    else if (v->state == CS_MEMORY) {
        dt = ((float)(int)(g->tick - v->memTick) - 2.0f + alpha) * SIM_DT;   /* render time = g->tick - 2 + alpha */
        *pos = v3_madd(v->memPos, v->memVel, dt);
    } else *pos = v->memPos;
    return v->state;
}

/* ---------------------------------------------------------------- RWR */
void radar_rwr(const Game *g, int id, RwrView *out)
{
    u32 m = radar_locked_by(id);
    int o;
    memset(out, 0, sizeof(*out));
    if (id < 0 || id >= g->numAircraft) return;
    for (o = 0; o < g->numAircraft && out->nLocks < RWR_MAX; o++) {
        Vec3 d;
        if (!(m & (1u << o)) || !alive(g, o)) continue;
        d = v3_sub(g->ac[o].fs.pos, g->ac[id].fs.pos);
        out->lockId[out->nLocks] = o;
        out->lockBearing[out->nLocks] = bearing_of(d);
        out->lockRange[out->nLocks] = v3_len(d);
        out->nLocks++;
    }
}

void radar_rwr_launch(Game *g, int shooter, int target, int mslType)
{
    ev_push(g->tick, REV_LAUNCH, shooter, target, mslType);
}

int radar_rwr_missile_warn(int mslType, float launchDistM, float distM)
{
    if (mslType == 0) return 1;
    return launchDistM <= s_rwrSrmM || distM <= s_rwrSrmM;
}
