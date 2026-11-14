/* weapons.cpp - stores, firing, flares, live missiles, hits, terrain impacts (DESIGN 12.1, 12.5, 12.6). Portable.
   Owner: T12. Rules and API: weapons.h. Missile physics/guidance: missile.cpp (missile_step, shared with dlz).
   Default sensor = T11's radar (radar_designated / radar_lock / radar_track); tests install their own with
   weapons_set_sensor. All randomness (flare spread, decoy rolls) comes from g->rng. */
#include "weapons.h"
#include "params.h"
#include "radar.h"
#include "e_terrain.h"
#include "e_log.h"

/* ---------------------------------------------------------------- [weapons] */
static int   s_lrmCount, s_srmCount, s_flareCount, s_enemyLrm, s_enemySrm, s_enemyFlares;
static float s_cooldownSec, s_railReloadSec;
static int   s_flareBurst;
static float s_flareBackMs, s_flareSpreadMs, s_flareGravG, s_flareDragK, s_flareBurnSec;
static float s_decoyM, s_pFlareSrm, s_pFlareLrm, s_pTailBonus, s_pCap, s_aiFlareFactor, s_rollSec;
static float s_srmLockSec, s_passM;
static int   s_logEvents;

static const Param s_tab[] = {
    { "weapons", "lrmCount", P_INT, &s_lrmCount, "16", 0, 999, "player LRM (radar missile, HUD AIM-120) per life" },
    { "weapons", "srmCount", P_INT, &s_srmCount, "40", 0, 999, "player SRM (heat missile, HUD AIM-9) per life" },
    { "weapons", "flareCount", P_INT, &s_flareCount, "40", 0, 999, "player flares per life" },
    { "weapons", "enemyLrm", P_INT, &s_enemyLrm, "2", 0, 999, "default enemy LRM (T14/T17 set per difficulty)" },
    { "weapons", "enemySrm", P_INT, &s_enemySrm, "2", 0, 999, "default enemy SRM" },
    { "weapons", "enemyFlares", P_INT, &s_enemyFlares, "20", 0, 999, "default enemy flares" },
    { "weapons", "fireCooldownSec", P_FLOAT, &s_cooldownSec, "0.4", 0.0f, 10.0f, "s between launches (rails alternate)" },
    { "weapons", "railReloadSec", P_FLOAT, &s_railReloadSec, "3.0", 0.0f, 60.0f, "s until a rail shows a missile again (visual)" },
    { "weapons", "flareBurst", P_INT, &s_flareBurst, "2", 1, 8, "flares per burst (one burst per F pulse)" },
    { "weapons", "flareBackMs", P_FLOAT, &s_flareBackMs, "20", 0.0f, 200.0f, "m/s: flare ejected backwards relative to the jet" },
    { "weapons", "flareSpreadMs", P_FLOAT, &s_flareSpreadMs, "8", 0.0f, 100.0f, "m/s: random spread per axis" },
    { "weapons", "flareGravG", P_FLOAT, &s_flareGravG, "0.25", 0.0f, 2.0f, "g: flare sink acceleration" },
    { "weapons", "flareDragK", P_FLOAT, &s_flareDragK, "0.0015", 0.0f, 0.1f, "1/m: flare drag (decel = k V^2): it falls behind the jet" },
    { "weapons", "flareBurnSec", P_FLOAT, &s_flareBurnSec, "3.0", 0.1f, 20.0f, "s: flare burn time" },
    { "weapons", "decoyM", P_FLOAT, &s_decoyM, "3000", 0.0f, 50000.0f, "m: missiles within this of the dispenser roll vs a burst" },
    { "weapons", "pFlareSrm", P_FLOAT, &s_pFlareSrm, "0.55", 0.0f, 1.0f, "decoy chance per burst, SRM" },
    { "weapons", "pFlareLrm", P_FLOAT, &s_pFlareLrm, "0.30", 0.0f, 1.0f, "decoy chance per burst, LRM (arcade: no chaff)" },
    { "weapons", "pTailBonus", P_FLOAT, &s_pTailBonus, "0.15", 0.0f, 1.0f, "added x tail aspect (missile right behind = full)" },
    { "weapons", "pCap", P_FLOAT, &s_pCap, "0.9", 0.0f, 1.0f, "decoy chance cap" },
    { "weapons", "aiFlareFactor", P_FLOAT, &s_aiFlareFactor, "0.25", 0.0f, 1.0f, "decoy chance x this for non-player dispensers" },
    { "weapons", "rollSec", P_FLOAT, &s_rollSec, "1.5", 0.0f, 10.0f, "s: one decoy roll per missile per this" },
    { "weapons", "srmLockSec", P_FLOAT, &s_srmLockSec, "0.1", 0.0083f, 1.0f, "s: SRM seeker lock refresh (staggered per aircraft)" },
    { "weapons", "passM", P_FLOAT, &s_passM, "200", 0.0f, 5000.0f, "m: a miss closer than this queues WPE_PASS (pass-by sound)" },
    { "weapons", "logEvents", P_BOOL, &s_logEvents, "1", 0.0f, 1.0f, "log one 'wpn:' line per launch / result" },
};

void weapons_params(void) { params_add(s_tab, E_COUNT(s_tab)); }

/* ---------------------------------------------------------------- state */
typedef struct Flare { int active, owner; Vec3 pos, prevPos, vel; float age; u32 serial; } Flare;

static Missile  s_m[MAX_MISSILES];
static u32      s_mSerial[MAX_MISSILES];
static u32      s_mFlareSerial[MAX_MISSILES];       /* serial of the flare a decoyed missile follows */
static float    s_mLaunchDist[MAX_MISSILES];
static int      s_mPassDone[MAX_MISSILES];
static u32      s_nextSerial;
static Flare    s_f[MAX_FLARES];
static int      s_fNext;
static u32      s_fSerial;
static WpnState s_w[MAX_AIRCRAFT];
static int      s_stock[MAX_AIRCRAFT][3];           /* refill amounts: lrm, srm, flares */
static int      s_stockSet[MAX_AIRCRAFT];
static int      s_wasAlive[MAX_AIRCRAFT];
static int      s_lastRespawns;
static WpnSensor s_sn[MAX_AIRCRAFT];                /* sensor of this tick */
static WpnEvent s_ev[WPN_EVENTS];
static u32      s_evN;
static WpnSensorFn s_sensorFn;
static const Game *s_curG;

/* A1 tags (HANDOFF A1): rail_0/1 wingtips (SRM), 2/3 outer, 4/5 middle, 6/7 inner pylons (LRM); even = left */
static const float s_railOff[RAIL_COUNT][3] = {
    { -4.67f, -0.18f, -0.55f }, { 4.67f, -0.18f, -0.55f },
    { -3.62f, -0.44f, -0.80f }, { 3.62f, -0.44f, -0.80f },
    { -2.80f, -0.44f, -0.80f }, { 2.80f, -0.44f, -0.80f },
    { -1.95f, -0.44f, -0.80f }, { 1.95f, -0.44f, -0.80f },
};
static const float s_flareOff[2][3] = { { -0.50f, -0.72f, -4.90f }, { 0.50f, -0.72f, -4.90f } };

static const char *const s_evNames[] = { "NONE", "LAUNCH", "HIT", "GROUND", "EXPIRED", "FLARE_HIT", "SELFDESTRUCT",
                                         "FLARES", "DECOY", "EMPTY", "ACQUIRED", "LOST", "PASS", "SELECT" };
const char *weapons_event_name(int type) { return type >= 0 && type < E_COUNT(s_evNames) ? s_evNames[type] : "?"; }

static void push_ev(const Game *g, int type, int slot, int mtype, int owner, int victim, Vec3 pos, float dmg)
{
    WpnEvent *e = &s_ev[s_evN % WPN_EVENTS];
    e->type = type;
    e->tick = g ? g->tick : 0;
    e->slot = slot;
    e->mtype = mtype;
    e->owner = owner;
    e->victim = victim;
    e->pos = pos;
    e->damage = dmg;
    s_evN++;
    if (s_logEvents && type != WPE_SELECT)
        log_printf(LOG_INFO, "wpn: %s tick=%u slot=%d type=%s owner=%d victim=%d pos=%.0f,%.0f,%.0f dmg=%.0f",
                   weapons_event_name(type), e->tick, slot, mtype == MSL_SRM ? "SRM" : (mtype == MSL_LRM ? "LRM" : "-"),
                   owner, victim, (double)pos.x, (double)pos.y, (double)pos.z, (double)dmg);
}

u32 weapons_event_count(void) { return s_evN; }
const WpnEvent *weapons_event(u32 index)
{
    if (index >= s_evN || s_evN - index > (u32)WPN_EVENTS) return NULL;
    return &s_ev[index % WPN_EVENTS];
}

/* ---------------------------------------------------------------- helpers */
static Vec3 ac_vel(const FlightState *fs) { return q_rotate(fs->qPath, v3(0.0f, 0.0f, fs->V)); }
static Vec3 ac_fwd(const FlightState *fs) { return q_rotate(fs->qPath, v3(0.0f, 0.0f, 1.0f)); }
static int  alive(const Game *g, int id) { return id >= 0 && id < g->numAircraft && (g->ac[id].flags & AC_ALIVE); }
static int  sec_ticks(float s) { int n = (int)(s * (float)SIM_HZ + 0.5f); return n < 0 ? 0 : n; }

static void sensor_radar(const Game *g, int obs, WpnSensor *out)
{
    int l;
    out->tdId = radar_designated(obs);
    out->lock = WLOCK_NONE;
    out->memPos = v3(0.0f, 0.0f, 0.0f);
    out->memVel = out->memPos;
    if (out->tdId < 0) return;
    l = radar_lock(obs);
    out->lock = l == LOCK_TRACK ? WLOCK_TRACK : WLOCK_MEMORY;
    if (out->lock == WLOCK_MEMORY && !radar_track(g, obs, out->tdId, &out->memPos, &out->memVel)) {
        out->tdId = -1;
        out->lock = WLOCK_NONE;
    }
}

void weapons_set_sensor(WpnSensorFn fn) { s_sensorFn = fn; }
void weapons_sensor(const Game *g, int observer, WpnSensor *out)
{
    if (s_sensorFn) s_sensorFn(g, observer, out);
    else sensor_radar(g, observer, out);
    if (out->tdId >= 0 && !alive(g, out->tdId)) { out->tdId = -1; out->lock = WLOCK_NONE; }
}

static void track_of_aircraft(const Game *g, int id, TrackView *tv)
{
    const Aircraft *a = &g->ac[id];
    tv->valid = (a->flags & AC_ALIVE) ? 1 : 0;
    tv->pos = a->fs.pos;
    tv->vel = ac_vel(&a->fs);
    tv->isFlare = 0;
    tv->datalink = 0;
    tv->ab = a->fs.ab;
    tv->id = id;
}

/* ---------------------------------------------------------------- stores */
static void stock_defaults(const Game *g, int id)
{
    if (s_stockSet[id]) return;
    if (id == PLAYER_ID || (g && (g->ac[id].flags & AC_PLAYER))) {
        s_stock[id][0] = s_lrmCount; s_stock[id][1] = s_srmCount; s_stock[id][2] = s_flareCount;
    } else {
        s_stock[id][0] = s_enemyLrm; s_stock[id][1] = s_enemySrm; s_stock[id][2] = s_enemyFlares;
    }
}

void weapons_refill(int id)
{
    WpnState *w;
    int i;
    if (id < 0 || id >= MAX_AIRCRAFT) return;
    w = &s_w[id];
    stock_defaults(s_curG, id);
    w->lrm = s_stock[id][0];
    w->srm = s_stock[id][1];
    w->flares = s_stock[id][2];
    w->cooldown = 0;
    for (i = 0; i < RAIL_COUNT; i++) w->railEmpty[i] = 0.0f;
    w->fired[0] = w->fired[1] = w->hits[0] = w->hits[1] = 0;
}

void weapons_set_stores(int id, int lrm, int srm, int flares)
{
    if (id < 0 || id >= MAX_AIRCRAFT) return;
    s_stock[id][0] = lrm; s_stock[id][1] = srm; s_stock[id][2] = flares;
    s_stockSet[id] = 1;
    s_w[id].lrm = lrm; s_w[id].srm = srm; s_w[id].flares = flares;
}

void weapons_select(int id, int type) { if (id >= 0 && id < MAX_AIRCRAFT) s_w[id].sel = type == MSL_SRM ? MSL_SRM : MSL_LRM; }
const WpnState *weapons_state(int id) { return &s_w[id >= 0 && id < MAX_AIRCRAFT ? id : 0]; }

void weapons_reset(Game *g)
{
    int i;
    s_curG = g;
    memset(s_m, 0, sizeof(s_m));
    for (i = 0; i < MAX_MISSILES; i++) { s_m[i].phase = MPH_DEAD; s_m[i].target = -1; s_mSerial[i] = 0; s_mPassDone[i] = 0; }
    memset(s_f, 0, sizeof(s_f));
    s_fNext = 0;
    s_fSerial = 0;
    s_nextSerial = 0;
    memset(s_w, 0, sizeof(s_w));
    memset(s_stockSet, 0, sizeof(s_stockSet));
    memset(s_ev, 0, sizeof(s_ev));
    s_evN = 0;
    for (i = 0; i < MAX_AIRCRAFT; i++) {
        s_w[i].sel = MSL_LRM;
        s_w[i].srmLock = -1;
        s_w[i].lastRail[0] = 7;                     /* first LRM goes to rail 2 */
        s_w[i].lastRail[1] = 1;                     /* first SRM goes to rail 0 */
        s_sn[i].tdId = -1;
        s_sn[i].lock = WLOCK_NONE;
        stock_defaults(g, i);
        weapons_refill(i);
        s_wasAlive[i] = g && i < g->numAircraft && (g->ac[i].flags & AC_ALIVE);
    }
    s_lastRespawns = session_respawns();
}

/* ---------------------------------------------------------------- SRM seeker lock (12.3, 10.6) */
static void srm_lock_update(const Game *g, int id)
{
    const MissileParams *mp = missile_params_of(MSL_SRM);
    const Aircraft *a = &g->ac[id];
    WpnState *w = &s_w[id];
    Vec3 nose, r;
    TrackView tv;
    float cg = (float)cos(E_DEG2RAD(mp->gimbalDeg)), cc = (float)cos(E_DEG2RAD(mp->acqConeDeg));
    float cs = (float)cos(E_DEG2RAD(mp->searchConeDeg)), dist, best = 1.0e9f, range;
    int j, td = s_sn[id].tdId;
    w->srmLock = -1;
    w->srmLockTd = 0;
    w->srmNear = 0;
    nose = q_rotate(flight_nose(&a->fs), v3(0.0f, 0.0f, 1.0f));
    if (td >= 0 && alive(g, td)) {
        track_of_aircraft(g, td, &tv);
        r = v3_sub(tv.pos, a->fs.pos);
        dist = v3_len(r);
        range = missile_heat_range(mp, &tv, a->fs.pos);
        if (dist > 1.0f && dist <= range && v3_dot(r, nose) >= dist * cg && terrain_los(g->terrain, &a->fs.pos, &tv.pos)) {
            w->srmLock = td;
            w->srmLockTd = 1;
            w->srmNear = 1;
            return;
        }
    }
    for (j = 0; j < g->numAircraft; j++) {
        if (j == id || !alive(g, j) || g->ac[j].side == a->side) continue;
        track_of_aircraft(g, j, &tv);
        r = v3_sub(tv.pos, a->fs.pos);
        dist = v3_len(r);
        if (dist < 1.0f) continue;
        range = missile_heat_range(mp, &tv, a->fs.pos);
        if (dist <= range * 1.5f && v3_dot(r, nose) >= dist * cs) w->srmNear = 1;
        if (dist > range || dist >= best || v3_dot(r, nose) < dist * cc) continue;
        if (!terrain_los(g->terrain, &a->fs.pos, &tv.pos)) continue;
        best = dist;
        w->srmLock = j;
    }
}

/* ---------------------------------------------------------------- launch */
static int free_slot(void)
{
    int i;
    for (i = 0; i < MAX_MISSILES; i++) if (s_m[i].phase == MPH_DEAD) return i;
    return -1;
}

static int next_rail(WpnState *w, int type)
{
    int r = w->lastRail[type], k;
    if (type == MSL_SRM) r = r == 0 ? 1 : 0;
    else {
        for (k = 0; k < 6; k++) {                   /* LRM rails 2..7: next one that shows a missile */
            r = r >= 7 || r < 2 ? 2 : r + 1;
            if (w->railEmpty[r] <= 0.0f) break;
        }
    }
    w->lastRail[type] = r;
    return r;
}

int weapons_fire(Game *g, int id)
{
    const Aircraft *a;
    WpnState *w;
    Missile *m;
    TrackView aim;
    Quat qn;
    Vec3 nose, pos;
    int type, *stock, slot, rail, target = -1, flags = 0;
    const WpnSensor *sn;
    if (!g || !alive(g, id)) return -1;
    s_curG = g;
    a = &g->ac[id];
    w = &s_w[id];
    if (w->cooldown > 0) return -1;
    type = w->sel;
    stock = type == MSL_SRM ? &w->srm : &w->lrm;
    w->cooldown = sec_ticks(s_cooldownSec);
    if (*stock <= 0) { push_ev(g, WPE_EMPTY, -1, type, id, -1, a->fs.pos, 0.0f); return -1; }
    slot = free_slot();
    if (slot < 0) return -1;
    m = &s_m[slot];
    sn = &s_sn[id];
    memset(&aim, 0, sizeof(aim));
    if (type == MSL_LRM) {
        if (sn->tdId >= 0 && alive(g, sn->tdId)) {
            target = sn->tdId;
            flags = MF_GUIDED;
            if (sn->lock == WLOCK_TRACK) track_of_aircraft(g, target, &aim);
            else { aim.valid = 1; aim.pos = sn->memPos; aim.vel = sn->memVel; aim.id = target; }
        } else {
            flags = MF_MADDOG;
        }
    } else {
        if (w->srmLock >= 0 && alive(g, w->srmLock)) {
            target = w->srmLock;
            flags = MF_GUIDED;
            track_of_aircraft(g, target, &aim);
        }
    }
    qn = flight_nose(&a->fs);
    nose = q_rotate(qn, v3(0.0f, 0.0f, 1.0f));
    rail = next_rail(w, type);
    pos = v3_add(a->fs.pos, q_rotate(qn, v3(s_railOff[rail][0], s_railOff[rail][1], s_railOff[rail][2])));
    /* T21a: the player's LRM flies its own parameter set ([lrm_player]); enemies and every SRM unchanged */
    missile_launch(m, missile_kind(type, (a->flags & AC_PLAYER) != 0), id, target, pos, nose, a->fs.V, &aim, flags,
                   (float)(slot % 12) / 12.0f);
    m->rail = rail;
    s_mSerial[slot] = ++s_nextSerial;
    s_mFlareSerial[slot] = 0;
    s_mPassDone[slot] = 0;
    s_mLaunchDist[slot] = target >= 0 ? v3_len(v3_sub(g->ac[target].fs.pos, a->fs.pos)) : 0.0f;
    (*stock)--;
    w->fired[type]++;
    w->railEmpty[rail] = s_railReloadSec;
    push_ev(g, WPE_LAUNCH, slot, type, id, target, pos, 0.0f);
    if (target >= 0) radar_rwr_launch(g, id, target, type);
    return slot;
}

/* ---------------------------------------------------------------- flares (12.5) */
int weapons_flares(Game *g, int id)
{
    const Aircraft *a;
    WpnState *w;
    Quat qn;
    Vec3 fwd, vel, afwd, d;
    int k, n, i, first, fi, ok;
    float cg, dist, tail, p, roll;
    if (!g || !alive(g, id)) return 0;
    a = &g->ac[id];
    w = &s_w[id];
    if (w->flares <= 0) return 0;
    n = s_flareBurst < w->flares ? s_flareBurst : w->flares;
    qn = flight_nose(&a->fs);
    fwd = ac_fwd(&a->fs);
    first = s_fNext;
    for (k = 0; k < n; k++) {
        Flare *f = &s_f[s_fNext];
        s_fNext = (s_fNext + 1) % MAX_FLARES;
        vel = v3_scale(fwd, a->fs.V - s_flareBackMs);
        vel.x += rnd_range(&g->rng, -s_flareSpreadMs, s_flareSpreadMs);
        vel.y += rnd_range(&g->rng, -s_flareSpreadMs, s_flareSpreadMs);
        vel.z += rnd_range(&g->rng, -s_flareSpreadMs, s_flareSpreadMs);
        f->active = 1;
        f->owner = id;
        f->pos = v3_add(a->fs.pos, q_rotate(qn, v3(s_flareOff[k & 1][0], s_flareOff[k & 1][1], s_flareOff[k & 1][2])));
        f->prevPos = f->pos;
        f->vel = vel;
        f->age = 0.0f;
        f->serial = ++s_fSerial;
    }
    w->flares -= n;
    push_ev(g, WPE_FLARES, -1, -1, id, -1, a->fs.pos, 0.0f);
    /* decoy rolls: hostile missiles in TERMINAL homing on the dispenser */
    afwd = fwd;
    for (i = 0; i < MAX_MISSILES; i++) {
        Missile *m = &s_m[i];
        const MissileParams *mp;
        if (m->phase != MPH_TERMINAL || (m->flags & MF_DECOYED) || m->target != id || m->owner == id) continue;
        d = v3_sub(m->pos, a->fs.pos);
        dist = v3_len(d);
        if (dist > s_decoyM) continue;
        if (m->age - m->rollAge < s_rollSec) continue;
        mp = missile_params_of(m->kind);
        cg = (float)cos(E_DEG2RAD(mp->gimbalDeg));
        fi = -1;
        for (k = 0; k < n && fi < 0; k++) {
            Vec3 r;
            float rl;
            int s = (first + k) % MAX_FLARES;
            r = v3_sub(s_f[s].pos, m->pos);
            rl = v3_len(r);
            ok = rl < 1.0f || v3_dot(r, m->dir) >= rl * cg;
            if (ok) fi = s;
        }
        if (fi < 0) continue;
        m->rollAge = m->age;
        tail = dist > 1.0f ? -v3_dot(afwd, d) / dist : 0.0f;
        tail = E_CLAMP(tail, 0.0f, 1.0f);
        p = (m->type == MSL_SRM ? s_pFlareSrm : s_pFlareLrm) + s_pTailBonus * tail;
        if (p > s_pCap) p = s_pCap;
        if (!(a->flags & AC_PLAYER)) p *= s_aiFlareFactor;
        roll = rnd_f01(&g->rng);
        if (roll < p) {
            m->target = fi;
            m->flags |= MF_DECOYED;
            m->aimPos = s_f[fi].pos;
            m->aimVel = s_f[fi].vel;
            m->aimFresh = 0;
            s_mFlareSerial[i] = s_f[fi].serial;
            push_ev(g, WPE_DECOY, i, m->type, m->owner, id, m->pos, p);
        }
    }
    return n;
}

static void flares_step(const Game *g, float dt)
{
    int i;
    float sp;
    for (i = 0; i < MAX_FLARES; i++) {
        Flare *f = &s_f[i];
        if (!f->active) continue;
        f->prevPos = f->pos;
        f->vel.y -= s_flareGravG * GRAV * dt;
        sp = v3_len(f->vel);
        f->vel = v3_scale(f->vel, 1.0f / (1.0f + s_flareDragK * sp * dt));
        f->pos = v3_madd(f->pos, f->vel, dt);
        f->age += dt;
        if (f->age >= s_flareBurnSec || f->pos.y <= (g->terrain ? terrain_surface(g->terrain, f->pos.x, f->pos.z) : 0.0f))
            f->active = 0;
    }
}

/* ---------------------------------------------------------------- live missiles */
static void track_for(const Game *g, int slot, TrackView *tv)
{
    const Missile *m = &s_m[slot];
    memset(tv, 0, sizeof(*tv));
    tv->id = -1;
    if (m->flags & MF_DECOYED) {
        const Flare *f;
        if (m->target < 0 || m->target >= MAX_FLARES) return;
        f = &s_f[m->target];
        tv->valid = f->active && f->serial == s_mFlareSerial[slot];
        tv->pos = f->pos;
        tv->vel = f->vel;
        tv->isFlare = 1;
        tv->ab = 1.0f;
        tv->id = m->target;
        return;
    }
    if (m->target < 0 || m->target >= g->numAircraft) return;
    track_of_aircraft(g, m->target, tv);
    tv->datalink = alive(g, m->owner) && s_sn[m->owner].tdId == m->target && s_sn[m->owner].lock == WLOCK_TRACK;
}

/* closest approach of missile segment vs aircraft segment over the last tick */
static float seg_dist(const Missile *m, const Aircraft *a, float *tOut)
{
    Vec3 d0 = v3_sub(a->prev.pos, m->prevPos), d1 = v3_sub(a->fs.pos, m->pos), dd = v3_sub(d1, d0);
    float den = v3_dot(dd, dd), t = den > 1e-6f ? -v3_dot(d0, dd) / den : 1.0f;
    t = E_CLAMP(t, 0.0f, 1.0f);
    *tOut = t;
    return v3_len(v3_madd(d0, dd, t));
}

static void finish(Game *g, int slot)
{
    Missile *m = &s_m[slot];
    int victim = (m->flags & MF_DECOYED) ? -1 : m->target, type = -1;
    float dmg = 0.0f;
    const MissileParams *mp = missile_params_of(m->kind);
    switch (m->result) {
    case MRES_HIT:
        type = WPE_HIT;
        dmg = (victim >= 0 && (g->ac[victim].flags & AC_PLAYER)) ? mp->damagePlayer : mp->damageEnemy;
        if (m->owner >= 0 && m->owner < MAX_AIRCRAFT) s_w[m->owner].hits[m->type]++;
        break;
    case MRES_GROUND: type = WPE_GROUND; break;
    case MRES_EXPIRED: type = WPE_EXPIRED; break;
    case MRES_FLARE: type = WPE_FLARE_HIT; break;
    default: type = WPE_SELFDESTRUCT; break;
    }
    push_ev(g, type, slot, m->type, m->owner, victim, m->endPos, dmg);
    m->phase = MPH_DEAD;
}

static void missiles_step(Game *g)
{
    int i, j, prevPhase, best, k;
    TrackView tv, cand;
    float dt = SIM_DT, t, dist, bestD;
    for (i = 0; i < MAX_MISSILES; i++) {
        Missile *m = &s_m[i];
        const MissileParams *mp;
        if (m->phase == MPH_DEAD) continue;
        mp = missile_params_of(m->kind);
        /* target destroyed: missiles homing on it self-destruct (14.2) */
        if (!(m->flags & MF_DECOYED) && m->target >= 0 && m->target < g->numAircraft && !alive(g, m->target)) {
            m->result = MRES_SELFDESTRUCT;
            m->endPos = m->pos;
            finish(g, i);
            continue;
        }
        track_for(g, i, &tv);
        prevPhase = m->phase;
        missile_step(m, mp, &tv, g->terrain, dt);
        if (m->result != MRES_NONE) {
            if (m->result == MRES_HIT && !tv.isFlare) m->target = tv.id;
            finish(g, i);
            continue;
        }
        /* searching seeker: other candidates (maddog, SRM search, LRM active without its own target), nearest first */
        if ((m->flags & MF_LOSCHECK) && missile_searching(m)) {
            int tried[MAX_AIRCRAFT];
            memset(tried, 0, sizeof(tried));
            for (k = 0; k < g->numAircraft && m->phase == MPH_ACTIVE; k++) {
                best = -1;
                bestD = 1.0e9f;
                for (j = 0; j < g->numAircraft; j++) {
                    if (tried[j] || j == m->owner || j == m->target || !alive(g, j)) continue;
                    dist = v3_len(v3_sub(g->ac[j].fs.pos, m->pos));
                    if (dist < bestD) { bestD = dist; best = j; }
                }
                if (best < 0) break;
                tried[best] = 1;
                track_of_aircraft(g, best, &cand);
                missile_try_acquire(m, mp, &cand, g->terrain);
            }
        }
        if (m->flags & MF_ACQUIRED) push_ev(g, WPE_ACQUIRED, i, m->type, m->owner, m->target, m->pos, 0.0f);
        if (prevPhase == MPH_TERMINAL && m->phase == MPH_LOST)
            push_ev(g, WPE_LOST, i, m->type, m->owner, m->target, m->pos, 0.0f);
        /* proximity fuze vs every other aircraft (the target itself is fuzed inside missile_step) */
        if ((m->flags & MF_ARMED) && !(m->flags & MF_UNGUIDED)) {
            for (j = 0; j < g->numAircraft; j++) {
                const Aircraft *a = &g->ac[j];
                if (j == m->owner || !(a->flags & AC_ALIVE)) continue;
                if (j == m->target && !(m->flags & MF_DECOYED)) continue;
                if ((float)fabs(a->fs.pos.x - m->pos.x) > 300.0f || (float)fabs(a->fs.pos.z - m->pos.z) > 300.0f) continue;
                if (seg_dist(m, a, &t) < mp->fuzeM) {
                    m->result = MRES_HIT;
                    m->pos = v3_lerp(m->prevPos, m->pos, t);
                    m->endPos = m->pos;
                    m->flags &= ~MF_DECOYED;
                    m->target = j;
                    m->phase = MPH_DEAD;
                    break;
                }
            }
            if (m->result != MRES_NONE) { finish(g, i); continue; }
        }
        /* pass-by */
        if ((m->flags & MF_MISSED) && !s_mPassDone[i] && !(m->flags & MF_DECOYED)) {
            s_mPassDone[i] = 1;
            if (m->minDist < s_passM) push_ev(g, WPE_PASS, i, m->type, m->owner, m->target, m->pos, m->minDist);
        }
    }
}

/* ---------------------------------------------------------------- tick */
void weapons_tick(Game *g)
{
    int i, r, lockTicks;
    float dt = SIM_DT;
    if (!g) return;
    s_curG = g;
    /* refills: back alive, or the session restarted the player on the runway */
    r = session_respawns();
    for (i = 0; i < g->numAircraft && i < MAX_AIRCRAFT; i++) {
        int al = (g->ac[i].flags & AC_ALIVE) ? 1 : 0;
        if (al && (!s_wasAlive[i] || (i == PLAYER_ID && r != s_lastRespawns))) weapons_refill(i);
        s_wasAlive[i] = al;
    }
    s_lastRespawns = r;
    /* sensors of this tick */
    for (i = 0; i < g->numAircraft; i++) {
        if (alive(g, i)) weapons_sensor(g, i, &s_sn[i]);
        else { s_sn[i].tdId = -1; s_sn[i].lock = WLOCK_NONE; }
    }
    /* per aircraft: rails, select, SRM lock, fire, flares */
    lockTicks = sec_ticks(s_srmLockSec);
    if (lockTicks < 1) lockTicks = 1;
    for (i = 0; i < g->numAircraft; i++) {
        WpnState *w = &s_w[i];
        const PilotInput *in = &g->ac[i].in;
        int k;
        if (w->cooldown > 0) w->cooldown--;
        for (k = 0; k < RAIL_COUNT; k++) {
            int have = k < 2 ? w->srm > 0 : w->lrm > 0;
            if (w->railEmpty[k] > 0.0f && have) w->railEmpty[k] -= dt;
            if (w->railEmpty[k] < 0.0f) w->railEmpty[k] = 0.0f;
        }
        if (!alive(g, i)) { w->srmLock = -1; w->srmNear = 0; w->srmLockTd = 0; continue; }
        if (in->wpnNext) {
            w->sel = w->sel == MSL_LRM ? MSL_SRM : MSL_LRM;
            push_ev(g, WPE_SELECT, -1, w->sel, i, -1, g->ac[i].fs.pos, 0.0f);
        }
        if (((g->tick + (u32)i * 3u) % (u32)lockTicks) == 0) srm_lock_update(g, i);
        if (in->fire) weapons_fire(g, i);
        if (in->flare) weapons_flares(g, i);
    }
    missiles_step(g);
    flares_step(g, dt);
}

/* ---------------------------------------------------------------- views */
int weapons_missiles(MissileView *out, int maxOut)
{
    int i, n = 0;
    for (i = 0; i < MAX_MISSILES && n < maxOut; i++) {
        const Missile *m = &s_m[i];
        MissileView *v;
        if (m->phase == MPH_DEAD) continue;
        v = &out[n++];
        v->slot = i;
        v->type = m->type;
        v->phase = m->phase;
        v->flags = m->flags;
        v->owner = m->owner;
        v->target = m->target;
        v->pos = m->pos;
        v->prevPos = m->prevPos;
        v->dir = m->dir;
        v->V = m->V;
        v->age = m->age;
        missile_estimates(m, missile_params_of(m->kind), &v->act, &v->ttg);
        v->homing = (m->phase == MPH_TERMINAL && !(m->flags & MF_DECOYED)) ? m->target : -1;
        v->rail = m->rail;
        v->serial = s_mSerial[i];
        v->launchDist = s_mLaunchDist[i];
        v->kind = m->kind;
    }
    return n;
}

const Missile *weapons_missile(int slot) { return slot >= 0 && slot < MAX_MISSILES ? &s_m[slot] : NULL; }

int weapons_live_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_MISSILES; i++) if (s_m[i].phase != MPH_DEAD) n++;
    return n;
}

int weapons_latest_at(int owner, int target)
{
    int i, best = -1;
    u32 s = 0;
    for (i = 0; i < MAX_MISSILES; i++) {
        const Missile *m = &s_m[i];
        if (m->phase == MPH_DEAD || m->owner != owner || m->target != target || (m->flags & MF_DECOYED)) continue;
        if (s_mSerial[i] > s) { s = s_mSerial[i]; best = i; }
    }
    return best;
}

int weapons_threats(int id, int *slots, int maxOut)
{
    int i, n = 0;
    for (i = 0; i < MAX_MISSILES && n < maxOut; i++) {
        const Missile *m = &s_m[i];
        if (m->phase == MPH_DEAD || m->target != id || m->owner == id) continue;
        if (m->flags & (MF_DECOYED | MF_UNGUIDED)) continue;
        slots[n++] = i;
    }
    return n;
}

int weapons_flares_view(FlareView *out, int maxOut)
{
    int i, n = 0;
    for (i = 0; i < MAX_FLARES && n < maxOut; i++) {
        const Flare *f = &s_f[i];
        if (!f->active) continue;
        out[n].slot = i;
        out[n].owner = f->owner;
        out[n].pos = f->pos;
        out[n].prevPos = f->prevPos;
        out[n].age = f->age;
        out[n].burn = s_flareBurnSec;
        n++;
    }
    return n;
}
