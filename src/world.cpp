/* world.cpp - island query wrapper, area bounds, crash tests, spawn points, PULL UP predictor, world events
   (DESIGN 6.2-6.4). Portable. Owner: T06. See world.h. */
#include "world.h"
#include "takeoff.h"                                /* ap_steer (turn-back autopilot) */
#include "params.h"
#include "e_terrain.h"
#include "e_log.h"

/* ---------------------------------------------------------------- [world] */
static float s_crashClearM, s_boxRadiusM, s_acRadiusM, s_wreckSec;
static float s_areaWarnKm, s_areaTurnKm, s_turnDoneDeg, s_turnBankDeg;
static float s_pullUpSec, s_pullUpOffSec, s_pullUpRunwayFt, s_pullUpStepSec;

static const Param s_tab[] = {
    { "world", "crashClearM", P_FLOAT, &s_crashClearM, "1.5", 0.0f, 20.0f,
      "crash when the jet's centre comes this close to the ground or sea (m) while flying" },
    { "world", "boxRadiusM", P_FLOAT, &s_boxRadiusM, "5", 0.0f, 50.0f,
      "jet sphere radius against the hangar/tower/tank boxes (m)" },
    { "world", "acRadiusM", P_FLOAT, &s_acRadiusM, "6", 0.0f, 50.0f,
      "jet sphere radius for aircraft-aircraft collisions (m): two jets collide when their centres pass closer than 2x this" },
    { "world", "wreckSec", P_FLOAT, &s_wreckSec, "2.5", 0.0f, 30.0f,
      "after a crash: wreck camera time before the restart on the runway (s)" },
    { "world", "areaWarnKm", P_FLOAT, &s_areaWarnKm, "75", 1.0f, 100.0f,
      "RETURN TO AREA warning beyond this distance from the island centre (km)" },
    { "world", "areaTurnKm", P_FLOAT, &s_areaTurnKm, "90", 1.0f, 100.0f,
      "the autopilot turns the jet back beyond this distance from the island centre (km)" },
    { "world", "turnbackDoneDeg", P_FLOAT, &s_turnDoneDeg, "30", 1.0f, 180.0f,
      "automatic turn-back ends when the heading is within this many degrees of the island centre" },
    { "world", "turnbackBankDeg", P_FLOAT, &s_turnBankDeg, "75", 10.0f, 85.0f,
      "bank angle of the automatic turn-back (deg; less if the G limit can't hold a level turn at that bank)" },
    { "world", "pullUpSec", P_FLOAT, &s_pullUpSec, "4", 0.5f, 20.0f,
      "PULL UP when the ground or sea is predicted within this many seconds on the current flight path" },
    { "world", "pullUpOffSec", P_FLOAT, &s_pullUpOffSec, "0.5", 0.0f, 10.0f,
      "PULL UP goes off when the predicted impact is this much later than pullUpSec (s, avoids flicker)" },
    { "world", "pullUpRunwayFt", P_FLOAT, &s_pullUpRunwayFt, "200", 0.0f, 5000.0f,
      "no PULL UP below this height (ft above ground) over the runway (low passes)" },
    { "world", "pullUpStepSec", P_FLOAT, &s_pullUpStepSec, "0.5", 0.1f, 2.0f,
      "PULL UP predictor sample spacing along the flight path (s; samples out to pullUpSec + 1 s)" },
};

void world_params(void) { params_add(s_tab, E_COUNT(s_tab)); }
float world_crash_clear(void) { return s_crashClearM; }

/* ---------------------------------------------------------------- per-aircraft state + event ring */
static WorldStatus s_st[MAX_AIRCRAFT];
static WorldEvent  s_ev[WORLD_EVENTS_MAX];
static u32         s_evSeq;

static void status_clear(WorldStatus *w)
{
    memset(w, 0, sizeof(*w));
    w->pullUpT = 99.0f;
}

void world_reset(Game *g)
{
    int i;
    (void)g;
    for (i = 0; i < MAX_AIRCRAFT; i++) status_clear(&s_st[i]);
    memset(s_ev, 0, sizeof(s_ev));
    s_evSeq = 0;
}

void world_reset_aircraft(int id)
{
    if (id >= 0 && id < MAX_AIRCRAFT) status_clear(&s_st[id]);
}

const char *world_event_name(int type)
{
    switch (type) {
    case WEV_CRASH_GROUND: return "CRASH_GROUND";
    case WEV_CRASH_SEA: return "CRASH_SEA";
    case WEV_CRASH_BOX: return "CRASH_BOX";
    case WEV_CRASH_AIR: return "CRASH_AIR";
    case WEV_RETURN_AREA: return "RETURN_AREA";
    case WEV_TURNBACK: return "TURNBACK";
    case WEV_PULLUP: return "PULLUP";
    case WEV_AREA_OK: return "AREA_OK";
    case WEV_TURNBACK_END: return "TURNBACK_END";
    case WEV_PULLUP_OFF: return "PULLUP_OFF";
    case WEV_TO_READY: return "TO_READY";
    case WEV_TO_ROLL: return "TO_ROLL";
    case WEV_TO_LIFTOFF: return "TO_LIFTOFF";
    case WEV_GEAR_UP: return "GEAR_UP";
    case WEV_GEAR_LOCKED: return "GEAR_LOCKED";
    case WEV_HANDOVER: return "HANDOVER";
    }
    return "?";
}

void world_event_push(const Game *g, int type, int ac, int other, Vec3 pos)
{
    WorldEvent *e = &s_ev[s_evSeq % WORLD_EVENTS_MAX];
    e->type = type;
    e->ac = ac;
    e->other = other;
    e->tick = g ? g->tick : 0;
    e->pos = pos;
    s_evSeq++;
    log_printf(LOG_INFO, "world: event=%s ac=%d other=%d tick=%u pos=%.0f,%.0f,%.0f", world_event_name(type), ac, other,
               e->tick, (double)pos.x, (double)pos.y, (double)pos.z);
}

u32 world_event_seq(void) { return s_evSeq; }

int world_event_get(u32 seq, WorldEvent *ev)
{
    if (seq >= s_evSeq || s_evSeq - seq > WORLD_EVENTS_MAX) return 0;
    *ev = s_ev[seq % WORLD_EVENTS_MAX];
    return 1;
}

const WorldStatus *world_status(int id)
{
    if (id < 0 || id >= MAX_AIRCRAFT) id = 0;
    return &s_st[id];
}

void world_set_takeoff(int id, int active)
{
    if (id >= 0 && id < MAX_AIRCRAFT) {
        s_st[id].takeoff = active ? 1 : 0;
        if (active && s_st[id].pullUp) s_st[id].pullUp = 0;
    }
}

int world_respawn_due(const Game *g, int id)
{
    const WorldStatus *w = world_status(id);
    return w->crashed && g->tick - w->crashTick >= (u32)e_ftoi(s_wreckSec * (float)SIM_HZ);
}

/* ---------------------------------------------------------------- queries */
float world_surface(const Game *g, float x, float z)
{
    return g->terrain ? terrain_surface(g->terrain, x, z) : 0.0f;
}

float world_agl(const Game *g, Vec3 p) { return p.y - world_surface(g, p.x, p.z); }

void world_centre(const Game *g, float *x, float *z)
{
    int i;
    float sx = 0.0f, sz = 0.0f;
    if (!g->terrain) { *x = *z = 0.0f; return; }
    for (i = 0; i < 4; i++) {
        sx += terrain_base(g->terrain)->airSpawn[i].x;
        sz += terrain_base(g->terrain)->airSpawn[i].z;
    }
    *x = sx * 0.25f;
    *z = sz * 0.25f;
}

int world_on_runway(const Game *g, float x, float z, float margin)
{
    const TerrainBase *b;
    float h, fx, fz, dx, dz, along, across;
    if (!g->terrain) return 0;
    b = terrain_base(g->terrain);
    h = E_DEG2RAD(b->rwyHeadingDeg);
    fx = (float)sin(h);
    fz = (float)cos(h);
    dx = x - b->rwyX;
    dz = z - b->rwyZ;
    along = dx * fx + dz * fz;
    across = dx * fz - dz * fx;                     /* + = right of the runway heading */
    return (float)fabs(along) <= b->rwyLength * 0.5f + margin && (float)fabs(across) <= b->rwyWidth * 0.5f + margin;
}

static float box_dist2(const Aabb *b, Vec3 p)
{
    float d = 0.0f, e;
    if (p.x < b->lo.x) { e = b->lo.x - p.x; d += e * e; } else if (p.x > b->hi.x) { e = p.x - b->hi.x; d += e * e; }
    if (p.y < b->lo.y) { e = b->lo.y - p.y; d += e * e; } else if (p.y > b->hi.y) { e = p.y - b->hi.y; d += e * e; }
    if (p.z < b->lo.z) { e = b->lo.z - p.z; d += e * e; } else if (p.z > b->hi.z) { e = p.z - b->hi.z; d += e * e; }
    return d;
}

/* union of the BASE boxes (quick reject: most of the time the jet is nowhere near the base) */
static const Terrain *s_boxT;
static int s_boxN;
static Aabb s_boxAll;

static const Aabb *boxes_union(const TerrainBase *b, const Terrain *t)
{
    int i;
    if (s_boxT != t || s_boxN != b->nBoxes) {
        s_boxT = t;
        s_boxN = b->nBoxes;
        s_boxAll.lo = v3(1e30f, 1e30f, 1e30f);
        s_boxAll.hi = v3(-1e30f, -1e30f, -1e30f);
        for (i = 0; i < b->nBoxes && i < TERRAIN_MAX_BOXES; i++) {
            s_boxAll.lo.x = E_MINF(s_boxAll.lo.x, b->box[i].lo.x); s_boxAll.hi.x = E_MAXF(s_boxAll.hi.x, b->box[i].hi.x);
            s_boxAll.lo.y = E_MINF(s_boxAll.lo.y, b->box[i].lo.y); s_boxAll.hi.y = E_MAXF(s_boxAll.hi.y, b->box[i].hi.y);
            s_boxAll.lo.z = E_MINF(s_boxAll.lo.z, b->box[i].lo.z); s_boxAll.hi.z = E_MAXF(s_boxAll.hi.z, b->box[i].hi.z);
        }
    }
    return &s_boxAll;
}

/* crash test of one point; *aglOut (may be NULL) = pos.y - surface */
static int crash_test_agl(const Game *g, Vec3 pos, float *aglOut)
{
    int i;
    float r2 = s_boxRadiusM * s_boxRadiusM, ht = -60.0f, sea = 0.0f, agl;
    if (g->terrain) {
        const TerrainBase *b = terrain_base(g->terrain);
        if (b->nBoxes > 0 && box_dist2(boxes_union(b, g->terrain), pos) < r2)
            for (i = 0; i < b->nBoxes && i < TERRAIN_MAX_BOXES; i++)
                if (box_dist2(&b->box[i], pos) < r2) {
                    if (aglOut) *aglOut = world_agl(g, pos);
                    return WEV_CRASH_BOX;
                }
        ht = terrain_height(g->terrain, pos.x, pos.z);
        sea = g->terrain->seaLevel;
    }
    agl = pos.y - (ht > sea ? ht : sea);
    if (aglOut) *aglOut = agl;
    if (agl < s_crashClearM) return ht >= sea ? WEV_CRASH_GROUND : WEV_CRASH_SEA;
    return 0;
}

int world_crash_test(const Game *g, Vec3 pos) { return crash_test_agl(g, pos, NULL); }

/* closest approach of two points moving linearly over the tick: a0->a1, b0->b1 */
static float swept_min_dist2(Vec3 a0, Vec3 a1, Vec3 b0, Vec3 b1)
{
    Vec3 d0 = v3_sub(a0, b0), dv = v3_sub(v3_sub(a1, a0), v3_sub(b1, b0)), d;
    float vv = v3_dot(dv, dv), t = 0.0f;
    if (vv > 1e-9f) t = E_CLAMP(-v3_dot(d0, dv) / vv, 0.0f, 1.0f);
    d = v3_madd(d0, dv, t);
    return v3_dot(d, d);
}

/* predicted time to impact along the current straight flight path (DESIGN 6.4): clearance c(t) = height above the
   surface minus crashClearM at pos + v*t, sampled every pullUpStepSec out to pullUpSec + 1 s; the first sample at or
   below 0 is interpolated linearly against the previous one. t = 0 is the straight-down AGL. */
float world_pullup_predict(const Game *g, const FlightState *fs)
{
    Mat3 m;
    Vec3 v, p;
    float c0, c, t, tPrev = 0.0f, tEnd, step = s_pullUpStepSec;
    q_to_mat3(fs->qPath, &m);
    v = v3_scale(v3(m.m[2][0], m.m[2][1], m.m[2][2]), fs->V);
    c0 = world_agl(g, fs->pos) - s_crashClearM;
    if (c0 <= 0.0f) return 0.0f;
    tEnd = s_pullUpSec + 1.0f;
    for (t = step; t <= tEnd + 1e-3f; t += step) {
        p = v3_madd(fs->pos, v, t);
        c = world_agl(g, p) - s_crashClearM;
        if (c <= 0.0f) return tPrev + (t - tPrev) * c0 / (c0 - c);
        c0 = c;
        tPrev = t;
    }
    return 99.0f;
}

/* ---------------------------------------------------------------- spawns */
void world_spawn_runway(const Game *g, Vec3 *pos, float *headingRad)
{
    if (g->terrain) {
        const TerrainBase *b = terrain_base(g->terrain);
        *pos = b->threshold;
        *headingRad = E_DEG2RAD(b->thresholdHeadingDeg);
    } else {
        *pos = v3(0.0f, 0.0f, 0.0f);
        *headingRad = 0.0f;
    }
}

void world_spawn_air(const Game *g, Vec3 from, Vec3 *pos, float *headingRad)
{
    int i, best = 0;
    float bd = 1e30f, d;
    const TerrainBase *b;
    if (!g->terrain) {
        *pos = v3(from.x, 1500.0f, from.z);
        *headingRad = 0.0f;
        return;
    }
    b = terrain_base(g->terrain);
    for (i = 0; i < 4; i++) {
        d = v3_len(v3_sub(b->airSpawn[i], from));
        if (d < bd) { bd = d; best = i; }
    }
    *pos = b->airSpawn[best];
    *headingRad = E_DEG2RAD(b->airSpawnHeadingDeg[best]);
}

void world_air_start(Game *g, Aircraft *ac, Vec3 from)
{
    Vec3 pos;
    float hdg;
    world_spawn_air(g, from, &pos, &hdg);
    flight_init(&ac->fs, pos, hdg, flight_speed_target(THR_NORMAL, pos.y));
    ac->fs.onGround = 0;
    ac->fs.gearPos = 0.0f;
    ac->flags = (ac->flags | AC_ALIVE) & ~AC_ONGROUND;
    ac->prev = ac->fs;
    world_reset_aircraft(ac->id);
    log_printf(LOG_INFO, "world: air_start ac=%d pos=%.0f,%.0f,%.0f hdg=%.0f V=%.0fkt", ac->id, (double)pos.x,
               (double)pos.y, (double)pos.z, (double)E_RAD2DEG(hdg), (double)(ac->fs.V * MS2KT));
}

/* ---------------------------------------------------------------- turn-back autopilot (DESIGN 6.3) */
int world_override_input(const Game *g, int id, PilotInput *in)
{
    const Aircraft *a;
    float nLim, bankMax;
    if (id < 0 || id >= g->numAircraft || !s_st[id].turnback) return 0;
    a = &g->ac[id];
    if (!(a->flags & AC_ALIVE) || a->fs.onGround) return 0;
    nLim = flight_g_limit(a->fs.V);
    bankMax = E_DEG2RAD(s_turnBankDeg);
    if (nLim > 1.05f && (float)acos(1.0f / nLim) < bankMax) bankMax = (float)acos(1.0f / nLim);
    if ((float)fabs(wrap_pi(s_st[id].brgCentre - a->fs.heading)) < E_DEG2RAD(s_turnDoneDeg))
        ap_steer(&a->fs, a->fs.heading, 0.0f, bankMax, in);   /* heading good: roll the wings level, then hand back */
    else
        ap_steer(&a->fs, s_st[id].brgCentre, 0.0f, bankMax, in);
    if (in->thr == THR_FAST) in->thr = THR_NORMAL;  /* no afterburner while being turned back (smaller circle) */
    return 1;
}

/* ---------------------------------------------------------------- per tick (DESIGN 2.6 step 6) */
static void crash(Game *g, int i, int type, int other)
{
    Aircraft *a = &g->ac[i];
    WorldStatus *w = &s_st[i];
    float s;
    if (w->crashed || !(a->flags & AC_ALIVE)) return;
    s = world_surface(g, a->fs.pos.x, a->fs.pos.z);
    if (type != WEV_CRASH_AIR && a->fs.pos.y < s) a->fs.pos.y = s;
    a->flags &= ~AC_ALIVE;
    a->hp = 0.0f;
    w->crashed = 1;
    w->crashType = type;
    w->crashTick = g->tick;
    w->pullUp = w->turnback = w->areaWarn = 0;
    world_event_push(g, type, i, other, a->fs.pos);
    log_printf(LOG_INFO, "world: crash ac=%d type=%s V=%.0fkt gamma=%.1f bank=%.1f", i, world_event_name(type),
               (double)(a->fs.V * MS2KT), (double)E_RAD2DEG(a->fs.gamma), (double)E_RAD2DEG(a->fs.bank));
}

static void area_update(Game *g, int i)
{
    Aircraft *a = &g->ac[i];
    WorldStatus *w = &s_st[i];
    float cx, cz, dx, dz, warn = s_areaWarnKm * 1000.0f, turn = s_areaTurnKm * 1000.0f;
    world_centre(g, &cx, &cz);
    dx = cx - a->fs.pos.x;
    dz = cz - a->fs.pos.z;
    w->distCentre = (float)sqrt(dx * dx + dz * dz);
    if (w->distCentre >= warn - 2000.0f || w->turnback || w->areaWarn) w->brgCentre = (float)atan2(dx, dz);
    if (!w->areaWarn && w->distCentre >= warn) {
        w->areaWarn = 1;
        world_event_push(g, WEV_RETURN_AREA, i, -1, a->fs.pos);
    } else if (w->areaWarn && w->distCentre < warn - 500.0f) {
        w->areaWarn = 0;
        world_event_push(g, WEV_AREA_OK, i, -1, a->fs.pos);
    }
    if (!w->turnback && w->distCentre >= turn &&
        (float)fabs(wrap_pi(w->brgCentre - a->fs.heading)) >= E_DEG2RAD(s_turnDoneDeg)) {   /* not already heading in */
        w->turnback = 1;
        world_event_push(g, WEV_TURNBACK, i, -1, a->fs.pos);
    } else if (w->turnback && (float)fabs(wrap_pi(w->brgCentre - a->fs.heading)) < E_DEG2RAD(s_turnDoneDeg) &&
               (float)fabs(a->fs.bank) < E_DEG2RAD(10.0f)) {   /* hand back with the wings (nearly) level */
        w->turnback = 0;
        world_event_push(g, WEV_TURNBACK_END, i, -1, a->fs.pos);
    }
}

static void pullup_update(Game *g, int i)
{
    Aircraft *a = &g->ac[i];
    WorldStatus *w = &s_st[i];
    int suppress;
    if (((g->tick + (u32)i * 7u) % (u32)(SIM_HZ / 2)) != 0) return;   /* 2 Hz */
    suppress = w->takeoff ||
               (w->agl < s_pullUpRunwayFt * FT2M && world_on_runway(g, a->fs.pos.x, a->fs.pos.z, 150.0f));
    w->pullUpT = world_pullup_predict(g, &a->fs);
    if (suppress) {
        if (w->pullUp) { w->pullUp = 0; world_event_push(g, WEV_PULLUP_OFF, i, -1, a->fs.pos); }
        return;
    }
    if (!w->pullUp && w->pullUpT <= s_pullUpSec) {
        w->pullUp = 1;
        world_event_push(g, WEV_PULLUP, i, -1, a->fs.pos);
    } else if (w->pullUp && w->pullUpT > s_pullUpSec + s_pullUpOffSec) {
        w->pullUp = 0;
        world_event_push(g, WEV_PULLUP_OFF, i, -1, a->fs.pos);
    }
}

int world_check(Game *g)
{
    int i, j, t, bits = 0;
    float r2 = 4.0f * s_acRadiusM * s_acRadiusM;
    /* ground, sea, boxes */
    for (i = 0; i < g->numAircraft && i < MAX_AIRCRAFT; i++) {
        Aircraft *a = &g->ac[i];
        if (!(a->flags & AC_ALIVE)) continue;
        if (a->fs.onGround) { s_st[i].agl = world_agl(g, a->fs.pos); continue; }
        t = crash_test_agl(g, a->fs.pos, &s_st[i].agl);
        if (t) crash(g, i, t, -1);
    }
    /* aircraft vs aircraft (swept over the tick, both crash) */
    for (i = 0; i < g->numAircraft && i < MAX_AIRCRAFT; i++) {
        Aircraft *a = &g->ac[i];
        if (!(a->flags & AC_ALIVE) || a->fs.onGround) continue;
        for (j = i + 1; j < g->numAircraft && j < MAX_AIRCRAFT; j++) {
            Aircraft *b = &g->ac[j];
            if (!(b->flags & AC_ALIVE) || b->fs.onGround) continue;
            if ((float)fabs(a->fs.pos.x - b->fs.pos.x) > 100.0f || (float)fabs(a->fs.pos.z - b->fs.pos.z) > 100.0f ||
                (float)fabs(a->fs.pos.y - b->fs.pos.y) > 100.0f) continue;   /* < 12 m per tick each */
            if (swept_min_dist2(a->prev.pos, a->fs.pos, b->prev.pos, b->fs.pos) < r2) {
                crash(g, i, WEV_CRASH_AIR, j);
                crash(g, j, WEV_CRASH_AIR, i);
                break;
            }
        }
    }
    /* area + PULL UP */
    for (i = 0; i < g->numAircraft && i < MAX_AIRCRAFT; i++) {
        Aircraft *a = &g->ac[i];
        if (!(a->flags & AC_ALIVE) || a->fs.onGround) continue;
        area_update(g, i);
        if (a->flags & AC_PLAYER) pullup_update(g, i);
    }
    {
        const WorldStatus *w = &s_st[PLAYER_ID];
        if (w->crashed && w->crashTick == g->tick) bits |= w->crashType;
        if (w->areaWarn) bits |= WEV_RETURN_AREA;
        if (w->turnback) bits |= WEV_TURNBACK;
        if (w->pullUp) bits |= WEV_PULLUP;
    }
    return bits;
}
