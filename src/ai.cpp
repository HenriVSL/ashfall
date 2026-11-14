/* ai.cpp - enemy pilots (DESIGN 13) + the robot player. Portable. Owner: T14. API and knowledge rules: ai.h.

   Think (10 Hz, ai_think):
     1 perception: target = nearest alive aircraft of the other side that this brain knows (own radar track/memory;
       robot: + minimap truth); else the attack order (the player's position at wave start) until the radar finds
       him; a lost contact is dead-reckoned for memorySec, then forgotten (-> PATROL).
     2 threats (RWR/MAW): missiles with target == me, not decoyed/unguided, known by the 11.5 rule
       (radar_rwr_missile_warn). Each new one gets ONE defendProb roll (g->rng). A "yes" threat puts the brain into
       DEFEND reactSec after it became known. Flare doctrine (T14, see below) per "yes" threat.
     3 state machine (DESIGN 13.1):
       PATROL     waypoint ring (22 km) around the island at the brain's patrol altitude (3-6 km); a radar lock on
                  us (RWR bearing, no position) turns the brain towards the locker for 10 s so its radar finds him.
       INTERCEPT  collision-course lead, altitude matched within 1500 m, NORMAL (FAST while not closing and the nose
                  is within 30 deg). No progress (closing) for chaseSec: hopeless tail chase -> PATROL, which re-engages
                  when the target is within mergeExitM, closing, or after 20 s.
                  -> ATTACK_BVR: tracked, R < table Rmax x lrmFireFrac (and > 1.2 Rmin), LRM left, nose within
                  noseFireDeg, own cooldown done. -> MERGE: R < mergeRangeM.
       ATTACK_BVR designate (radar_designate), wait for LOCK_TRACK (<= lockWaitSec), fire one LRM, then CRANK
                  (crankDeg off the line of sight, inside the 70 deg lock gimbal) until the missile is ACTIVE/dead
                  or crankMaxSec; without crank (Easy) straight back to INTERCEPT (nose stays on, lock held).
       MERGE      lead pursuit (mergeLeadSec) at gUse, throttle for corner speed; SRM when the seeker locks the
                  target and R < table Rne x srmFireFrac (> 1.1 Rmin); LRM there when no SRM is left.
                  -> EXTEND: overshoot (target > 110 deg off the nose within 3 km and not pointing at us: we
                  passed it; a target on our tail is fought, not extended from) or mergeMaxSec;
                  -> INTERCEPT beyond mergeExitM.
       EXTEND     away from the target, FAST, extendSec -> INTERCEPT.
       DEFEND     escape class (break / beam) = the solver's best escape of E for this geometry (dlz_ai_zone
                  worstEsc), side = the shorter turn; heading fixed at entry relative to the missile; level turn at
                  gUse x G limit, FAST. terrainMask (Hard) with the summit within maskRangeM: fly to a point
                  maskBehindM behind the summit (seen from the missile) at maskAltFrac x summit height (floor
                  maskAglM: the deliberate masking manoeuvre). Exit: no defended threat left or defendMaxSec ->
                  EXTEND. A missile triggers DEFEND once; after a timeout again only within 6 km.
       CRUISE / DRONE: tests and TRAINING (ai.h).
     4 terrain look-ahead -> floorGamma (the steepest climb needed to pass floorAglM above the surface sampled
       1.5-10 s ahead along the velocity AND along the goal direction) and AVOID: a dive whose recovery (pull at
       0.9 x G limit after rolling upright) would end within recoverMarginM of the highest surface on the recovery
       path; AGL < avoidAglM (not masking); area radius (areaRadiusKm, heading out: goal = the centre); above
       ceilingM no climbing. AVOID pulls up (30 deg over the horizon along the velocity) with the full G limit,
       at least 1 s.
     4b mid-air collision avoidance: closest approach with any aircraft within 4 s closer than 200 m -> steer away
       from its position at that moment (full G limit) until the next think.
     5 watchdog: a state without progress (state change, launch, goal/target distance improved by 1 km, waypoint)
       for stuckSec -> counted as stuck (AiStats) and forced to EXTEND.
   Steer (every tick, ai_steer): goal -> desired direction d (floor and ceiling applied) -> acceleration demand =
     V x kTurn x angle(d, forward) towards d + gravity compensation -> lift direction (bank error) and load factor
     (<= gCap x nLim, pull scaled by cos(bank error), small nose-down corrections push instead of rolling inverted)
     -> up/ur through the inverse of the flight model's mapping -> keyboard ramps of [pilot] -> PilotInput.
   Flare doctrine (T14; T13 open issue 1 and DESIGN 12.7's ">= 90 % inside Rne vs Normal"): flareBursts bursts per
     defended missile at most (enemies 1), the first when it comes within flareRangeM (3 km), each with ONE
     flareProb roll, further bursts flareGapSec apart (= weapons rollSec, so each burst gets a decoy roll). With the
     AI decoy chance pFlare x aiFlareFactor (0.075-0.23 per burst) one burst costs the player <= ~10 % inside Rne;
     dispensing every 0.5 s (T13's stress case) would cost ~45 %. The robot player (AC_PLAYER: full decoy chance)
     uses 2 bursts.
   Deterministic: float math, integer ticks, randomness only from g->rng. No allocation per tick. */
#include "ai.h"
#include "params.h"
#include "radar.h"
#include "weapons.h"
#include "missile.h"
#include "dlz.h"
#include "world.h"
#include "takeoff.h"
#include "e_log.h"
#include "e_terrain.h"

/* ---------------------------------------------------------------- params */
static float s_floorAglM, s_avoidAglM, s_maskAglM, s_recoverMarginM, s_kTurn, s_kBank, s_mergeRangeM, s_mergeExitM;
static float s_extendSec, s_crankDeg, s_crankMaxSec, s_defendMaxSec, s_maskRangeM;
static float s_flareRangeM, s_noseFireDeg, s_mergeMaxSec, s_areaRadiusKm;
static float s_chaseSec;
static float s_stuckSec;
static int   s_logStates;
/* fixed tuning (not ini keys: the params registry is close to PARAMS_MAX 512) */
static const float s_pushMaxG = -1.0f;
static const float s_mergeLeadSec = 1.5f;
static const float s_leadMaxSec = 30.0f;
static const float s_lockWaitSec = 4.0f;
static const float s_overshootDeg = 110.0f;
static const float s_reDefendM = 6000.0f;
static const float s_maskBehindM = 2500.0f;
static const float s_maskAltFrac = 0.6f;
static const float s_flareGapSec = 1.5f;
static const float s_ceilingM = 12000.0f;
static const float s_memorySec = 30.0f;
static const float s_holdSec = 20.0f;
static const float s_collideSec = 4.0f;
static const float s_collideM = 200.0f;
static const float s_patrolRadiusKm = 22.0f;

static const Param s_tab[] = {
    { "ai", "floorAglM", P_FLOAT, &s_floorAglM, "450", 50.0f, 3000.0f, "m: AI keeps at least this far above the surface ahead (steering floor)" },
    { "ai", "avoidAglM", P_FLOAT, &s_avoidAglM, "300", 30.0f, 2000.0f, "m: below this AGL the AI pulls up (AVOID), unless terrain masking" },
    { "ai", "maskAglM", P_FLOAT, &s_maskAglM, "150", 30.0f, 1000.0f, "m: steering floor during a deliberate terrain-masking manoeuvre (Hard)" },
    { "ai", "recoverMarginM", P_FLOAT, &s_recoverMarginM, "250", 0.0f, 2000.0f, "m: dive recovery must end this far above the highest surface ahead" },
    { "ai", "kTurn", P_FLOAT, &s_kTurn, "1.2", 0.1f, 10.0f, "1/s: turn rate demanded per radian of angle between the nose and the goal" },
    { "ai", "kBank", P_FLOAT, &s_kBank, "4.0", 0.5f, 20.0f, "1/s: roll rate demanded per radian of bank error" },
    { "ai", "mergeRangeM", P_FLOAT, &s_mergeRangeM, "6000", 1000.0f, 20000.0f, "m: INTERCEPT -> MERGE inside this range" },
    { "ai", "mergeExitM", P_FLOAT, &s_mergeExitM, "9000", 2000.0f, 30000.0f, "m: MERGE -> INTERCEPT beyond this range" },
    { "ai", "mergeMaxSec", P_FLOAT, &s_mergeMaxSec, "40", 5.0f, 300.0f, "s: a merge without a shot ends in EXTEND after this" },
    { "ai", "extendSec", P_FLOAT, &s_extendSec, "5", 1.0f, 60.0f, "s: EXTEND duration (away from the target, FAST)" },
    { "ai", "crankDeg", P_FLOAT, &s_crankDeg, "45", 0.0f, 65.0f, "deg: CRANK offset from the line of sight after an LRM launch" },
    { "ai", "crankMaxSec", P_FLOAT, &s_crankMaxSec, "15", 1.0f, 60.0f, "s: CRANK ends when the missile goes active or after this" },
    { "ai", "noseFireDeg", P_FLOAT, &s_noseFireDeg, "20", 1.0f, 60.0f, "deg: LRM fired only with the target this close to the nose" },
    { "ai", "defendMaxSec", P_FLOAT, &s_defendMaxSec, "12", 2.0f, 60.0f, "s: DEFEND ends after this at the latest" },
    { "ai", "maskRangeM", P_FLOAT, &s_maskRangeM, "6000", 0.0f, 30000.0f, "m: terrainMask profiles hide behind the summit when it is this close" },
    { "ai", "flareRangeM", P_FLOAT, &s_flareRangeM, "3000", 100.0f, 20000.0f, "m: first flare burst when a defended missile comes this close" },
    { "ai", "areaRadiusKm", P_FLOAT, &s_areaRadiusKm, "55", 5.0f, 100.0f, "km: AI turns back towards the island centre beyond this" },
    { "ai", "chaseSec", P_FLOAT, &s_chaseSec, "40", 5.0f, 300.0f, "s: INTERCEPT without closing in this long breaks off to PATROL" },
    { "ai", "stuckSec", P_FLOAT, &s_stuckSec, "60", 5.0f, 600.0f, "s: watchdog: no progress this long = stuck (counted, forced EXTEND)" },
    { "ai", "logStates", P_INT, &s_logStates, "0", 0, 2, "1 = log every AI state change at info level (else debug), 2 = also a 1 Hz brain trace" },
};

/* difficulty profiles (DESIGN 13.3) */
static AiProfile s_prof[4];                         /* easy, normal, hard (ini), robot (fixed below) */
#define PROF_KEYS(sec, P, react, guse, lfrac, sfrac, cool, nl, ns, nf, dp, fp, fb, ck, tm) \
    { sec, "reactSec", P_FLOAT, &P.reactSec, react, 0.0f, 10.0f, "s: missile warning -> DEFEND" }, \
    { sec, "gUse", P_FLOAT, &P.gUse, guse, 0.2f, 1.0f, "fraction of the G limit used in manoeuvres" }, \
    { sec, "lrmFireFrac", P_FLOAT, &P.lrmFireFrac, lfrac, 0.1f, 1.0f, "LRM fired inside table Rmax x this" }, \
    { sec, "srmFireFrac", P_FLOAT, &P.srmFireFrac, sfrac, 0.1f, 1.0f, "SRM fired inside table Rne x this (seeker lock needed)" }, \
    { sec, "fireCooldownSec", P_FLOAT, &P.fireCooldownSec, cool, 0.5f, 120.0f, "s: own launches at least this far apart" }, \
    { sec, "lrm", P_INT, &P.lrm, nl, 0, 99, "LRM per life" }, \
    { sec, "srm", P_INT, &P.srm, ns, 0, 99, "SRM per life" }, \
    { sec, "flares", P_INT, &P.flares, nf, 0, 999, "flares per life" }, \
    { sec, "defendProb", P_FLOAT, &P.defendProb, dp, 0.0f, 1.0f, "chance to react (DEFEND) to each known inbound missile" }, \
    { sec, "flareProb", P_FLOAT, &P.flareProb, fp, 0.0f, 1.0f, "chance per flare burst opportunity (defended missile within flareRangeM)" }, \
    { sec, "flareBursts", P_INT, &P.flareBursts, fb, 0, 20, "flare bursts per inbound missile at most (doctrine: keeps shots inside Rne honest)" }, \
    { sec, "crank", P_BOOL, &P.crank, ck, 0, 1, "1 = CRANK after an LRM launch" }, \
    { sec, "terrainMask", P_BOOL, &P.terrainMask, tm, 0, 1, "1 = hide behind the volcano in DEFEND when it is within maskRangeM" }
static const Param s_profTab[] = {
    PROF_KEYS("ai_easy",   s_prof[0], "1.2",  "0.65", "0.95", "1.0",  "14", "1",  "2",  "20", "0.5",  "0.2", "1", "0", "0"),
    PROF_KEYS("ai_normal", s_prof[1], "0.7",  "0.80", "0.75", "0.85", "9",  "2",  "2",  "20", "0.85", "0.5", "1", "1", "0"),
    PROF_KEYS("ai_hard",   s_prof[2], "0.35", "0.95", "0.60", "0.70", "6",  "2",  "4",  "20", "1.0",  "0.8", "1", "1", "1"),
};

/* the robot player of simh aifight / headless sorties: a player who waits for no-escape LRM shots and dispenses two
   bursts per missile (fixed, not ini keys: the params registry is close to PARAMS_MAX) */
static const AiProfile s_robot = { 0.5f, 0.90f, 0.95f, 1.0f, 3.0f, 16, 40, 40, 1.0f, 1.0f, 2, 1, 0, 1, 1.2f };

void ai_params(void)
{
    params_add(s_tab, E_COUNT(s_tab));
    params_add(s_profTab, E_COUNT(s_profTab));
    s_prof[3] = s_robot;
}

const AiProfile *ai_profile(int diff) { return &s_prof[diff < 0 ? 0 : (diff > 2 ? 2 : diff)]; }
const AiProfile *ai_profile_robot(void) { return &s_prof[3]; }

/* keyboard ramp rates of [pilot] (DESIGN 5.2), looked up once, read live (F5) */
static const float *s_kp[9];
static const char *const s_kpNames[9] = { "pitchFine", "pitchRateFine", "pitchRate", "pitchReturn", "pitchCross",
                                          "rollRateIn", "rollReturn", "rollCross", "rollFine" };
static const float s_kpDef[9] = { 0.35f, 3.0f, 1.8f, 6.0f, 8.0f, 6.0f, 10.0f, 12.0f, 0.0f };
static float kp(int i)
{
    if (!s_kp[i]) {
        const Param *p = params_find("pilot", s_kpNames[i]);
        if (p && p->type == P_FLOAT) s_kp[i] = (const float *)p->ptr;
        else return s_kpDef[i];
    }
    return *s_kp[i];
}

/* ---------------------------------------------------------------- brains */
enum { KIND_ENEMY, KIND_ROBOT, KIND_DRONE };
enum { SM_POINT, SM_HDG };
enum { AI_MAX_THREATS = 8 };
enum { ESC_BREAK, ESC_BEAM };

typedef struct Threat {
    int slot, type, owner;
    int kind;                                       /* T21a: MSLK_* parameter set of the missile (DEFEND's table) */
    float age;                                      /* missile age at the last look (slot reuse = age went down) */
    u32 firstTick;                                  /* when it became known */
    int defend;                                     /* defendProb roll said yes */
    int bursts, rolls;                              /* flare bursts dispensed / rolls made at this missile */
    u32 lastBurstTick;
    float dist;
    Vec3 pos;
    int seen;
    int due;                                        /* reaction time passed (counted once) */
    int defends;                                    /* DEFEND entries for this missile (a 2nd one only when close) */
} Threat;

typedef struct Brain {
    int on, kind;
    const AiProfile *p;
    int state, prevState;
    u32 stateTick, thinkTick;
    /* perception */
    int target;
    int know;                                       /* 0 none, 1 attack order only, 2 dead-reckoned memory, 3 radar */
    int tracked;                                    /* radar_track == 1 this think */
    Vec3 kPos, kVel; u32 kTick;
    int hasOrder; Vec3 orderPos;
    /* steering goal */
    int sm, useAlt, turnSign, thr;
    Vec3 aim, aimVel;
    float altDes, hdgDes, gCap;
    /* terrain / overrides */
    float floorGamma;
    int avoid, masking, bounds, collide, rwrValid;
    float rwrBearing; u32 rwrTick;
    Vec3 colDir;
    u32 avoidTick;
    /* axes (keyboard ramps) */
    float up, ur;
    /* weapons */
    int fireReq, flareReq, fired, firedType;
    u32 lastFireTick;
    int mySlot; float mySlotAge;
    /* defend */
    Threat th[AI_MAX_THREATS];
    int nTh, defSlot, escType;
    float defAlt;
    /* patrol / drone / cruise */
    int wp;
    float patrolAlt, droneCx, droneCz, droneR, cruiseHdg, cruiseAlt;
    /* watchdog */
    u32 progTick, holdTick;                         /* holdTick: PATROL re-engages from this tick (after a chase) */
    float bestDist;
} Brain;

static Brain   s_b[MAX_AIRCRAFT];
static AiStats s_st[MAX_AIRCRAFT];
static MissileView s_views[MAX_MISSILES];
static int     s_nViews;
static u32     s_viewsTick = 0xffffffffu;
static int     s_haveSummit;
static Vec3    s_summit;

static const char *const s_stateNames[AIS_COUNT] = { "OFF", "PATROL", "INTERCEPT", "ATTACK_BVR", "MERGE", "EXTEND",
                                                      "DEFEND", "CRUISE", "DRONE" };
const char *ai_state_name(int st) { return st >= 0 && st < AIS_COUNT ? s_stateNames[st] : "?"; }

/* ---------------------------------------------------------------- helpers */
static int   alive(const Game *g, int id) { return id >= 0 && id < g->numAircraft && (g->ac[id].flags & AC_ALIVE); }
static Vec3  ac_vel(const FlightState *fs) { return q_rotate(fs->qPath, v3(0.0f, 0.0f, fs->V)); }
static Vec3  ac_fwd(const FlightState *fs) { return q_rotate(fs->qPath, v3(0.0f, 0.0f, 1.0f)); }
static float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static float bearing_to(Vec3 from, Vec3 to) { return (float)atan2(to.x - from.x, to.z - from.z); }
static float hdist(Vec3 a, Vec3 b) { float dx = b.x - a.x, dz = b.z - a.z; return (float)sqrt(dx * dx + dz * dz); }
static Vec3  hdg_vec(float h) { return v3((float)sin(h), 0.0f, (float)cos(h)); }
static int   sec_ticks(float s) { int n = (int)(s * (float)SIM_HZ + 0.5f); return n < 1 ? 1 : n; }
static float tick_sec(u32 a, u32 b) { return (float)(int)(a - b) * SIM_DT; }
static float wrap_2pi(float a)
{
    while (a < 0.0f) a += 2.0f * E_PI;
    while (a >= 2.0f * E_PI) a -= 2.0f * E_PI;
    return a;
}

static float surface(const Game *g, float x, float z) { return world_surface(g, x, z); }

/* angle between the nose and the direction to p (rad) */
static float off_nose(const FlightState *fs, Vec3 p)
{
    Vec3 r = v3_sub(p, fs->pos);
    float l = v3_len(r);
    if (l < 1.0f) return 0.0f;
    return (float)acos(clampf(v3_dot(r, q_rotate(flight_nose(fs), v3(0.0f, 0.0f, 1.0f))) / l, -1.0f, 1.0f));
}

/* collision-course lead point: |p + v t - me| = myV t, t scaled by frac and capped */
static Vec3 lead_point(Vec3 me, float myV, Vec3 p, Vec3 v, float frac, float maxT)
{
    Vec3 r = v3_sub(p, me);
    float a = v3_dot(v, v) - myV * myV, b = 2.0f * v3_dot(r, v), c = v3_dot(r, r), t, disc, t1, t2;
    if ((float)fabs(a) < 1e-3f) t = b < 0.0f ? -c / b : (float)sqrt(c) / E_MAXF(myV, 1.0f);
    else {
        disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) t = (float)sqrt(c) / E_MAXF(myV, 1.0f);
        else {
            disc = (float)sqrt(disc);
            t1 = (-b - disc) / (2.0f * a);
            t2 = (-b + disc) / (2.0f * a);
            if (t1 > t2) { t = t1; t1 = t2; t2 = t; }
            t = t1 > 0.0f ? t1 : (t2 > 0.0f ? t2 : (float)sqrt(c) / E_MAXF(myV, 1.0f));
        }
    }
    t = clampf(t * frac, 0.0f, maxT);
    return v3_madd(p, v, t);
}

static void views_update(const Game *g)
{
    if (s_viewsTick == g->tick) return;
    s_nViews = weapons_missiles(s_views, MAX_MISSILES);
    s_viewsTick = g->tick;
}

/* summit of the island (masking goal): coarse grid, then refine */
static void find_summit(const Game *g)
{
    const Terrain *t = g->terrain;
    float x, z, h, best = -1e9f, bx = 0.0f, bz = 0.0f, cx, cz, r, step;
    int pass;
    s_haveSummit = 0;
    if (!t || !t->loaded) return;
    cx = t->centreX; cz = t->centreZ; r = t->radius > 1000.0f ? t->radius : 20000.0f;
    for (pass = 0; pass < 2; pass++) {
        float x0 = pass ? bx - 400.0f : cx - r, x1 = pass ? bx + 400.0f : cx + r;
        float z0 = pass ? bz - 400.0f : cz - r, z1 = pass ? bz + 400.0f : cz + r;
        step = pass ? 50.0f : 400.0f;
        for (z = z0; z <= z1; z += step)
            for (x = x0; x <= x1; x += step) {
                h = terrain_height(t, x, z);
                if (h > best) { best = h; bx = x; bz = z; }
            }
    }
    if (best > 200.0f) { s_haveSummit = 1; s_summit = v3(bx, best, bz); }
    log_printf(LOG_INFO, "ai: summit found=%d pos=%.0f,%.0f h=%.0f", s_haveSummit, (double)bx, (double)bz, (double)best);
}

/* ---------------------------------------------------------------- reset / spawn */
void ai_stats_clear(void) { memset(s_st, 0, sizeof(s_st)); }
const AiStats *ai_stats(int id) { return &s_st[id >= 0 && id < MAX_AIRCRAFT ? id : 0]; }

void ai_reset(Game *g)
{
    memset(s_b, 0, sizeof(s_b));
    ai_stats_clear();
    s_viewsTick = 0xffffffffu;
    s_nViews = 0;
    find_summit(g);
}

static void set_state(const Game *g, int id, Brain *b, int st, const char *why);

static void brain_init(Game *g, int id, int kind, const AiProfile *p, int hasOrder, Vec3 orderPos)
{
    Brain *b = &s_b[id];
    const FlightState *fs = &g->ac[id].fs;
    memset(b, 0, sizeof(*b));
    b->on = 1;
    b->kind = kind;
    b->p = p;
    b->target = -1;
    b->hasOrder = hasOrder;
    b->orderPos = orderPos;
    b->thr = THR_NORMAL;
    b->gCap = p->gUse;
    b->defSlot = -1;
    b->mySlot = -1;
    b->lastFireTick = g->tick - (u32)sec_ticks(p->fireCooldownSec);   /* may fire at once */
    b->sm = SM_HDG;
    b->hdgDes = fs->heading;
    b->altDes = fs->pos.y;
    b->patrolAlt = 3000.0f + 3000.0f * (float)((id * 7) % 5) / 4.0f;
    b->cruiseHdg = fs->heading;
    b->cruiseAlt = fs->pos.y;
    b->up = g->ac[id].in.up;
    b->ur = g->ac[id].in.ur;
    b->state = AIS_OFF;
    set_state(g, id, b, hasOrder ? AIS_INTERCEPT : AIS_PATROL, "spawn");
}

void ai_spawn_profile(Game *g, int id, const AiProfile *p, int hasOrder, Vec3 orderPos)
{
    if (id < 0 || id >= MAX_AIRCRAFT) return;
    brain_init(g, id, KIND_ENEMY, p, hasOrder, orderPos);
    weapons_set_stores(id, p->lrm, p->srm, p->flares);
}

void ai_spawn(Game *g, int id, int diff, int hasOrder, Vec3 orderPos)
{
    ai_spawn_profile(g, id, ai_profile(diff), hasOrder, orderPos);
}

void ai_spawn_robot(Game *g, int id)
{
    if (id < 0 || id >= MAX_AIRCRAFT) return;
    brain_init(g, id, KIND_ROBOT, ai_profile_robot(), 0, v3(0.0f, 0.0f, 0.0f));
}

void ai_spawn_drone(Game *g, int id, float cx, float cz, float r, float alt)
{
    Brain *b;
    if (id < 0 || id >= MAX_AIRCRAFT) return;
    brain_init(g, id, KIND_DRONE, ai_profile(DIFF_EASY), 0, v3(0.0f, 0.0f, 0.0f));
    b = &s_b[id];
    b->droneCx = cx; b->droneCz = cz; b->droneR = r; b->patrolAlt = alt;
    set_state(g, id, b, AIS_DRONE, "drone");
}

void ai_set_cruise(Game *g, int id)
{
    Brain *b;
    if (id < 0 || id >= MAX_AIRCRAFT || !s_b[id].on) return;
    b = &s_b[id];
    b->cruiseHdg = g->ac[id].fs.heading;
    b->cruiseAlt = g->ac[id].fs.pos.y;
    b->thr = g->ac[id].fs.thr;
    set_state(g, id, b, AIS_CRUISE, "cruise");
}

void ai_set_cruise_goal(int id, float hdgRad, float altM)
{
    if (id < 0 || id >= MAX_AIRCRAFT) return;
    s_b[id].cruiseHdg = hdgRad;
    s_b[id].cruiseAlt = altM;
}

void ai_off(int id) { if (id >= 0 && id < MAX_AIRCRAFT) s_b[id].on = 0; }
int  ai_state(int id) { return id >= 0 && id < MAX_AIRCRAFT && s_b[id].on ? s_b[id].state : AIS_OFF; }
int  ai_target(int id) { return id >= 0 && id < MAX_AIRCRAFT && s_b[id].on ? s_b[id].target : -1; }
int  ai_avoiding(int id) { return id >= 0 && id < MAX_AIRCRAFT && s_b[id].on ? s_b[id].avoid : 0; }
int  ai_masking(int id) { return id >= 0 && id < MAX_AIRCRAFT && s_b[id].on ? s_b[id].masking : 0; }
float ai_state_sec(const Game *g, int id)
{
    return id >= 0 && id < MAX_AIRCRAFT && s_b[id].on ? tick_sec(g->tick, s_b[id].stateTick) : 0.0f;
}

static void set_state(const Game *g, int id, Brain *b, int st, const char *why)
{
    float sec;
    if (b->state == st) return;
    sec = tick_sec(g->tick, b->stateTick);
    if (b->state > 0 && b->state < AIS_COUNT && sec > s_st[id].maxStateSec[b->state]) s_st[id].maxStateSec[b->state] = sec;
    log_printf(s_logStates ? LOG_INFO : LOG_DEBUG, "ai: state id=%d %s->%s why=%s t=%.1f tgt=%d", id, ai_state_name(b->state),
               ai_state_name(st), why, (double)(g->tick * SIM_DT), b->target);
    b->prevState = b->state;
    b->state = st;
    b->stateTick = g->tick;
    b->progTick = g->tick;
    b->bestDist = 1e9f;
    b->masking = 0;
    if (st == AIS_ATTACK_BVR) { b->fired = 0; b->mySlot = -1; }
    if (st == AIS_DEFEND) s_st[id].defendEntered++;
}

/* ---------------------------------------------------------------- goals */
static void goal_point(Brain *b, Vec3 aim, Vec3 aimVel, int useAlt, float altDes, float gCap, int thr)
{
    b->sm = SM_POINT; b->aim = aim; b->aimVel = aimVel; b->useAlt = useAlt; b->altDes = altDes; b->gCap = gCap;
    b->thr = thr; b->turnSign = 0;
}

static void goal_heading(Brain *b, float hdg, int turnSign, float altDes, float gCap, int thr)
{
    b->sm = SM_HDG; b->hdgDes = hdg; b->turnSign = turnSign; b->altDes = altDes; b->gCap = gCap; b->thr = thr;
}

/* progress for the watchdog: distance to something got 1 km better */
static void progress_dist(const Game *g, Brain *b, float d)
{
    if (d < b->bestDist - 1000.0f) { b->bestDist = d; b->progTick = g->tick; }
    else if (d < b->bestDist && b->bestDist > 1e8f) b->bestDist = d;
}

/* ---------------------------------------------------------------- perception */
static int other_side(const Game *g, int id, int j) { return j != id && alive(g, j) && g->ac[j].side != g->ac[id].side; }

static void perceive(Game *g, int id, Brain *b)
{
    const FlightState *fs = &g->ac[id].fs;
    Vec3 p, v, bp = v3(0.0f, 0.0f, 0.0f), bv = bp;
    int j, st, best = -1, bestSt = 0, lockedAttack;
    float d, bestD = 1e30f, curD = 1e30f;
    lockedAttack = b->state == AIS_ATTACK_BVR || b->state == AIS_DEFEND;
    /* candidates the brain knows right now */
    for (j = 0; j < g->numAircraft; j++) {
        if (!other_side(g, id, j)) continue;
        st = radar_track(g, id, j, &p, &v);
        if (!st && b->p->knowsAll) { p = g->ac[j].fs.pos; v = ac_vel(&g->ac[j].fs); st = 3; }
        if (!st) continue;
        d = v3_len(v3_sub(p, fs->pos));
        if (j == b->target) curD = d;
        if (d < bestD) { bestD = d; best = j; bestSt = st; bp = p; bv = v; }
    }
    if (b->target >= 0 && !alive(g, b->target)) { b->target = -1; b->know = 0; }
    /* switch: no target, or a much nearer one (not in the middle of an attack) */
    if (best >= 0 && (b->target < 0 || (best != b->target && !lockedAttack && bestD < 0.7f * curD))) {
        b->target = best;
        b->know = 0;
    }
    if (b->target < 0 && b->hasOrder) {
        for (j = 0; j < g->numAircraft; j++) if (other_side(g, id, j) && (g->ac[j].flags & AC_PLAYER)) { b->target = j; break; }
    }
    b->tracked = 0;
    if (best < 0) {                                  /* RWR: a lock on us gives the locker's bearing (no position) */
        RwrView rv;
        radar_rwr(g, id, &rv);
        if (rv.nLocks > 0) { b->rwrValid = 1; b->rwrBearing = rv.lockBearing[0]; b->rwrTick = g->tick; }
    }
    if (b->target < 0) { b->know = 0; return; }
    if (b->target == best) { st = bestSt; p = bp; v = bv; }
    else {
        st = radar_track(g, id, b->target, &p, &v);
        if (!st && b->p->knowsAll && alive(g, b->target)) { p = g->ac[b->target].fs.pos; v = ac_vel(&g->ac[b->target].fs); st = 3; }
    }
    if (st) {
        b->kPos = p; b->kVel = v; b->kTick = g->tick;
        b->know = 3;
        b->tracked = st == 1;
        b->hasOrder = 0;                            /* the order is superseded by real contact */
    } else if (b->know >= 2) {
        if (tick_sec(g->tick, b->kTick) > s_memorySec) b->know = 0;
        else b->know = 2;
    } else if (b->hasOrder) {
        b->know = 1; b->kPos = b->orderPos; b->kVel = v3(0.0f, 0.0f, 0.0f); b->kTick = g->tick;
    } else b->know = 0;
}

/* known target position now (dead-reckoned) */
static Vec3 known_pos(const Game *g, const Brain *b) { return v3_madd(b->kPos, b->kVel, tick_sec(g->tick, b->kTick)); }

/* ---------------------------------------------------------------- threats + flares */
static void threats(Game *g, int id, Brain *b)
{
    const FlightState *fs = &g->ac[id].fs;
    int i, k, n;
    for (k = 0; k < b->nTh; k++) b->th[k].seen = 0;
    views_update(g);
    for (i = 0; i < s_nViews; i++) {
        const MissileView *v = &s_views[i];
        Threat *t = NULL;
        float dist;
        if (v->target != id || (v->flags & (MF_DECOYED | MF_UNGUIDED)) || v->owner == id) continue;
        if (v->owner >= 0 && v->owner < g->numAircraft && g->ac[v->owner].side == g->ac[id].side) continue;
        dist = v3_len(v3_sub(v->pos, fs->pos));
        for (k = 0; k < b->nTh; k++) if (b->th[k].slot == v->slot && v->age >= b->th[k].age) { t = &b->th[k]; break; }
        if (!t) {
            if (!radar_rwr_missile_warn(v->type, v->launchDist, dist)) continue;   /* not known yet (11.5) */
            if (b->nTh >= AI_MAX_THREATS) continue;
            t = &b->th[b->nTh++];
            memset(t, 0, sizeof(*t));
            t->slot = v->slot;
            t->type = v->type;
            t->kind = v->kind;
            t->owner = v->owner;
            t->firstTick = g->tick;
            t->defend = b->kind != KIND_DRONE && b->p->defendProb > 0.0f && rnd_f01(&g->rng) < b->p->defendProb;
            s_st[id].threats++;
            if (t->defend) s_st[id].defendYes++;
            log_printf(LOG_DEBUG, "ai: threat id=%d slot=%d type=%d dist=%.0f defend=%d", id, v->slot, v->type, (double)dist, t->defend);
        }
        t->age = v->age;
        t->dist = dist;
        t->pos = v->pos;
        t->seen = 1;
    }
    /* drop the gone ones (dead, decoyed, gave up, hit something else) */
    for (k = n = 0; k < b->nTh; k++) if (b->th[k].seen) b->th[n++] = b->th[k];
    b->nTh = n;
    /* flare doctrine */
    if (weapons_state(id)->flares > 0)
        for (k = 0; k < b->nTh; k++) {
            Threat *t = &b->th[k];
            if (!t->defend || tick_sec(g->tick, t->firstTick) < b->p->reactSec) continue;
            if (t->dist > s_flareRangeM || t->rolls >= b->p->flareBursts) continue;
            if (t->rolls > 0 && tick_sec(g->tick, t->lastBurstTick) < s_flareGapSec) continue;
            t->rolls++;
            t->lastBurstTick = g->tick;
            s_st[id].flareRolls++;
            if (rnd_f01(&g->rng) < b->p->flareProb) {
                t->bursts++;
                b->flareReq = 1;
                s_st[id].flareBursts++;
                log_printf(LOG_DEBUG, "ai: flares id=%d slot=%d dist=%.0f", id, t->slot, (double)t->dist);
            }
        }
}

/* nearest defended threat whose reaction time has passed (-1 none) */
static int primary_threat(const Game *g, const Brain *b)
{
    int k, best = -1;
    float d = 1e30f;
    for (k = 0; k < b->nTh; k++) {
        const Threat *t = &b->th[k];
        if (!t->defend || tick_sec(g->tick, t->firstTick) < b->p->reactSec) continue;
        if (t->defends >= 2 || (t->defends == 1 && t->dist > s_reDefendM)) continue;   /* once, again when close */
        if (t->dist < d) { d = t->dist; best = k; }
    }
    return best;
}

/* DEFEND goal for threat t (fixed at entry) */
static void defend_setup(Game *g, int id, Brain *b, Threat *t)
{
    const FlightState *fs = &g->ac[id].fs;
    DlzZone z;
    Vec3 ov = v3(0.0f, 0.0f, 0.0f), me = ac_vel(fs);
    float ha, c1, c2, e1, e2;
    int sign;
    b->defSlot = t->slot;
    b->escType = ESC_BREAK;
    t->defends++;
    if (t->owner >= 0 && t->owner < g->numAircraft) ov = ac_vel(&g->ac[t->owner].fs);
    if (dlz_ai_zone(t->kind, t->pos, ov, fs->pos, me, &z) && (z.worstEsc == DLZ_BEAM_L || z.worstEsc == DLZ_BEAM_R))
        b->escType = ESC_BEAM;
    ha = bearing_to(t->pos, fs->pos);               /* away from the missile */
    if (b->escType == ESC_BREAK) {
        e1 = wrap_pi(ha - fs->heading);
        sign = e1 >= 0.0f ? 1 : -1;
        b->hdgDes = ha;
    } else {
        c1 = ha + 0.5f * E_PI; c2 = ha - 0.5f * E_PI;
        e1 = wrap_pi(c1 - fs->heading); e2 = wrap_pi(c2 - fs->heading);
        if ((float)fabs(e1) <= (float)fabs(e2)) { b->hdgDes = c1; sign = e1 >= 0.0f ? 1 : -1; }
        else { b->hdgDes = c2; sign = e2 >= 0.0f ? 1 : -1; }
    }
    b->turnSign = sign;
    b->defAlt = fs->pos.y;
    log_printf(LOG_DEBUG, "ai: defend id=%d slot=%d esc=%s hdg=%.0f sign=%d", id, t->slot, b->escType == ESC_BEAM ? "beam" : "break",
               (double)E_RAD2DEG(b->hdgDes), sign);
}

/* DEFEND trigger (every state but DRONE): the nearest defended threat whose reaction time passed */
static void defend_trigger(Game *g, int id, Brain *b)
{
    int pt;
    if (b->kind == KIND_DRONE || b->state == AIS_DEFEND) return;
    pt = primary_threat(g, b);
    if (pt < 0) return;
    set_state(g, id, b, AIS_DEFEND, "missile");
    defend_setup(g, id, b, &b->th[pt]);
}

/* ---------------------------------------------------------------- weapons */
/* T21a: the LRM this aircraft fires (the robot player = AC_PLAYER flies [lrm_player]; enemies [lrm]) */
static int lrm_kind(const Game *g, int id) { return missile_kind(MSL_LRM, (g->ac[id].flags & AC_PLAYER) != 0); }

static int cooldown_ok(const Game *g, int id, const Brain *b)
{
    return (int)(g->tick - b->lastFireTick) >= sec_ticks(b->p->fireCooldownSec) && weapons_state(id)->cooldown == 0;
}

static void request_fire(Game *g, int id, Brain *b, int type, float R)
{
    DlzZone z;
    float Rt;
    weapons_select(id, type);
    b->fireReq = 1;
    b->firedType = type;
    b->lastFireTick = g->tick;
    b->progTick = g->tick;
    s_st[id].fired[type]++;
    /* honesty check against the truth (tables from both true states) */
    if (alive(g, b->target) && dlz_ai_zone_ac(g, id, b->target, type, &z)) {
        Rt = v3_len(v3_sub(g->ac[b->target].fs.pos, g->ac[id].fs.pos));
        if (z.rmax <= 0.0f || Rt > z.rmax * 1.03f || Rt < z.rmin * 0.97f) {
            s_st[id].envViolations++;
            log_printf(LOG_WARN, "warn: ai fire outside zone id=%d type=%d R=%.0f (known %.0f) rmin=%.0f rmax=%.0f", id, type,
                       (double)Rt, (double)R, (double)z.rmin, (double)z.rmax);
        }
    }
    log_printf(LOG_DEBUG, "ai: fire id=%d tgt=%d type=%s R=%.0f", id, b->target, type == MSL_SRM ? "SRM" : "LRM", (double)R);
}

/* LRM launch window of a profile for a table zone */
static int lrm_window(const AiProfile *p, const DlzZone *z, float R)
{
    if (z->rmax <= 0.0f || R >= z->rmax * p->lrmFireFrac || R <= z->rmin * 1.2f) return 0;
    return p->lrmNeFrac <= 0.0f || R < z->rne * p->lrmNeFrac;
}

/* my latest live missile at the target (for CRANK): slot or -1 */
static int my_missile(const Game *g, int id, const Brain *b)
{
    int i;
    views_update(g);
    for (i = 0; i < s_nViews; i++)
        if (s_views[i].owner == id && s_views[i].target == b->target && s_views[i].age < 30.0f &&
            !(s_views[i].flags & (MF_DECOYED | MF_UNGUIDED)))
            return i;
    return -1;
}

/* ---------------------------------------------------------------- state machine */
static void leave_defend(const Game *g, int id, Brain *b, const char *why)
{
    set_state(g, id, b, b->prevState == AIS_CRUISE ? AIS_CRUISE : AIS_EXTEND, why);
    b->defSlot = -1;
}

static void think_states(Game *g, int id, Brain *b)
{
    const FlightState *fs = &g->ac[id].fs;
    const WpnState *w = weapons_state(id);
    const AiProfile *p = b->p;
    Vec3 tp, tv, myVel = ac_vel(fs), r, dh, mp;
    float R = 1e9f, nose = E_PI, closure = 0.0f, alt, s, cx, cz, a, los, vkt, sec;
    int pt, noFire = b->kind == KIND_DRONE, k, thr, crankSign;
    DlzZone z;

    defend_trigger(g, id, b);
    if (b->know) {
        tp = known_pos(g, b);
        tv = b->kVel;
        r = v3_sub(tp, fs->pos);
        R = v3_len(r);
        nose = off_nose(fs, tp);
        if (R > 1.0f) closure = v3_dot(v3_sub(myVel, tv), v3_scale(r, 1.0f / R));
    } else {
        tp = fs->pos;
        tv = v3(0.0f, 0.0f, 0.0f);
    }
    sec = ai_state_sec(g, id);

    switch (b->state) {
    case AIS_PATROL:
        if (b->know && ((int)(g->tick - b->holdTick) >= 0 || R < s_mergeExitM || closure > 100.0f)) {
            set_state(g, id, b, AIS_INTERCEPT, b->know == 1 ? "order" : "contact");
            break;
        }
        if (b->rwrValid && tick_sec(g->tick, b->rwrTick) < 10.0f) {   /* someone locked us: look that way */
            goal_heading(b, b->rwrBearing, 0, E_MAXF(fs->pos.y, 2000.0f), p->gUse, THR_NORMAL);
            b->progTick = g->tick;
            break;
        }
        world_centre(g, &cx, &cz);
        a = (float)b->wp * 0.5f * E_PI + (float)id * 0.7f;
        tp = v3(cx + s_patrolRadiusKm * 1000.0f * (float)sin(a), b->patrolAlt, cz + s_patrolRadiusKm * 1000.0f * (float)cos(a));
        s = hdist(fs->pos, tp);
        if (s < 3000.0f) { b->wp = (b->wp + 1) & 3; b->progTick = g->tick; b->bestDist = 1e9f; }
        else progress_dist(g, b, s);
        goal_point(b, tp, v3(0.0f, 0.0f, 0.0f), 1, b->patrolAlt, p->gUse * 0.7f, THR_NORMAL);
        break;

    case AIS_DRONE:
        a = bearing_to(v3(b->droneCx, 0.0f, b->droneCz), fs->pos) + 0.6f;   /* a point ahead on the circle (clockwise) */
        tp = v3(b->droneCx + b->droneR * (float)sin(a), b->patrolAlt, b->droneCz + b->droneR * (float)cos(a));
        goal_point(b, tp, v3(0.0f, 0.0f, 0.0f), 1, b->patrolAlt, 0.5f, THR_NORMAL);
        b->progTick = g->tick;
        break;

    case AIS_CRUISE:
        goal_heading(b, b->cruiseHdg, 0, b->cruiseAlt, p->gUse, b->thr);
        b->progTick = g->tick;
        break;

    case AIS_INTERCEPT:
        if (!b->know) { set_state(g, id, b, AIS_PATROL, "lost"); break; }
        if (b->know >= 2 && R < s_mergeRangeM) { set_state(g, id, b, AIS_MERGE, "range"); break; }
        if (!noFire && b->tracked && w->lrm > 0 && cooldown_ok(g, id, b) && nose < E_DEG2RAD(s_noseFireDeg) &&
            dlz_ai_zone(lrm_kind(g, id), fs->pos, myVel, tp, tv, &z) && lrm_window(p, &z, R)) {
            set_state(g, id, b, AIS_ATTACK_BVR, "zone");
            radar_designate(g, id, b->target);
            break;
        }
        progress_dist(g, b, R);
        if (b->know >= 2 && tick_sec(g->tick, b->progTick) > s_chaseSec) {   /* hopeless tail chase: break off */
            b->holdTick = g->tick + (u32)sec_ticks(s_holdSec);
            set_state(g, id, b, AIS_PATROL, "chase");
            break;
        }
        alt = clampf(fs->pos.y, tp.y - 1500.0f, tp.y + 1500.0f);
        if (b->know == 1) alt = E_MAXF(fs->pos.y, 2000.0f);
        goal_point(b, b->know >= 2 ? lead_point(fs->pos, fs->V, tp, tv, 1.0f, s_leadMaxSec) : tp, tv, 1, alt, p->gUse,
                   (b->know >= 2 && closure < 50.0f && nose < E_DEG2RAD(30.0f)) ? THR_FAST : THR_NORMAL);
        break;

    case AIS_ATTACK_BVR:
        if (!b->know) { set_state(g, id, b, AIS_PATROL, "lost"); break; }
        alt = clampf(fs->pos.y, tp.y - 1500.0f, tp.y + 1500.0f);
        if (!b->fired) {
            if (R < s_mergeRangeM) { set_state(g, id, b, AIS_MERGE, "range"); break; }
            if (b->tracked) radar_designate(g, id, b->target);
            goal_point(b, lead_point(fs->pos, fs->V, tp, tv, 1.0f, s_leadMaxSec), tv, 1, alt, p->gUse, THR_NORMAL);
            if (b->tracked && radar_lock(id) == LOCK_TRACK && radar_designated(id) == b->target && w->lrm > 0 &&
                cooldown_ok(g, id, b) && nose < E_DEG2RAD(s_noseFireDeg) && dlz_ai_zone(lrm_kind(g, id), fs->pos, myVel, tp, tv, &z) &&
                lrm_window(p, &z, R)) {
                request_fire(g, id, b, MSL_LRM, R);
                b->fired = 1;
                b->turnSign = 0;
            } else if (sec > s_lockWaitSec) {
                set_state(g, id, b, AIS_INTERCEPT, "nolock");
            }
            break;
        }
        if (!p->crank) { set_state(g, id, b, AIS_INTERCEPT, "fired"); break; }
        k = my_missile(g, id, b);
        if (tick_sec(g->tick, b->lastFireTick) > 0.5f && (k < 0 || s_views[k].phase >= MPH_ACTIVE)) {
            set_state(g, id, b, R < s_mergeRangeM ? AIS_MERGE : AIS_INTERCEPT, k < 0 ? "msl_gone" : "msl_active");
            break;
        }
        if (tick_sec(g->tick, b->lastFireTick) > s_crankMaxSec) { set_state(g, id, b, AIS_INTERCEPT, "crank_time"); break; }
        if (R < 3000.0f) { set_state(g, id, b, AIS_MERGE, "close"); break; }
        los = bearing_to(fs->pos, tp);
        crankSign = wrap_pi(fs->heading - los) >= 0.0f ? 1 : -1;   /* stay on the side we are on */
        if (b->tracked) radar_designate(g, id, b->target);
        goal_heading(b, los + (float)crankSign * E_DEG2RAD(s_crankDeg), 0, alt, p->gUse, THR_NORMAL);
        b->progTick = g->tick;                      /* time-capped */
        break;

    case AIS_MERGE:
        if (!b->know) { set_state(g, id, b, AIS_PATROL, "lost"); break; }
        if (R > s_mergeExitM) { set_state(g, id, b, AIS_INTERCEPT, "opened"); break; }
        if (sec > s_mergeMaxSec) { set_state(g, id, b, AIS_EXTEND, "merge_time"); break; }
        if (R < 3000.0f && nose > E_DEG2RAD(s_overshootDeg) && v3_dot(tv, r) > 0.0f) {   /* passed it, it is not on my tail */
            set_state(g, id, b, AIS_EXTEND, "overshoot");
            break;
        }
        if (b->tracked) radar_designate(g, id, b->target);   /* SRM seeker slaved to the TD */
        if (!noFire && cooldown_ok(g, id, b)) {
            if (w->srm > 0 && w->srmLock == b->target && w->srmLock >= 0 && dlz_ai_zone(MSL_SRM, fs->pos, myVel, tp, tv, &z) &&
                z.rne > 0.0f && R < z.rne * p->srmFireFrac && R > z.rmin * 1.1f)
                request_fire(g, id, b, MSL_SRM, R);
            else if (w->srm <= 0 && w->lrm > 0 && b->tracked && radar_lock(id) == LOCK_TRACK && radar_designated(id) == b->target &&
                     nose < E_DEG2RAD(s_noseFireDeg) && dlz_ai_zone(lrm_kind(g, id), fs->pos, myVel, tp, tv, &z) && lrm_window(p, &z, R))
                request_fire(g, id, b, MSL_LRM, R);
        }
        vkt = fs->V * MS2KT;
        thr = THR_NORMAL;
        if (vkt > 420.0f && nose > E_DEG2RAD(45.0f)) thr = THR_SLOW;
        else if (vkt < 330.0f) thr = THR_FAST;
        goal_point(b, lead_point(fs->pos, fs->V, tp, tv, 1.0f, s_mergeLeadSec), tv, 0, 0.0f, p->gUse, thr);
        b->progTick = g->tick;                      /* time-capped (mergeMaxSec) */
        break;

    case AIS_EXTEND:
        if (sec > s_extendSec) { set_state(g, id, b, b->know ? AIS_INTERCEPT : AIS_PATROL, "extended"); break; }
        goal_heading(b, b->know ? bearing_to(tp, fs->pos) : fs->heading, 0, E_MAXF(fs->pos.y, 2500.0f), p->gUse, THR_FAST);
        break;

    case AIS_DEFEND:
        for (k = 0; k < b->nTh; k++) if (b->th[k].slot == b->defSlot && b->th[k].defend) break;
        if (k >= b->nTh) {
            pt = primary_threat(g, b);
            if (pt < 0) { leave_defend(g, id, b, "clear"); break; }
            defend_setup(g, id, b, &b->th[pt]);
            k = pt;
        }
        if (sec > s_defendMaxSec) { leave_defend(g, id, b, "time"); break; }
        b->progTick = g->tick;                      /* time-capped (defendMaxSec) */
        if (p->terrainMask && s_haveSummit && hdist(fs->pos, s_summit) < s_maskRangeM) {
            dh = v3_sub(s_summit, b->th[k].pos);
            dh.y = 0.0f;
            dh = v3_norm(dh);
            mp = v3_madd(s_summit, dh, s_maskBehindM);
            alt = s_summit.y * s_maskAltFrac;
            if (!b->masking) { b->masking = 1; s_st[id].maskEntries++; log_printf(LOG_DEBUG, "ai: mask id=%d", id); }
            goal_point(b, v3(mp.x, alt, mp.z), v3(0.0f, 0.0f, 0.0f), 1, alt, p->gUse, THR_FAST);
        } else {
            b->masking = 0;
            goal_heading(b, b->hdgDes, b->turnSign, b->defAlt, p->gUse, THR_FAST);
        }
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------- terrain look-ahead, AVOID, bounds */
static void terrain_look(Game *g, int id, Brain *b)
{
    static const float ts[6] = { 1.5f, 3.0f, 4.5f, 6.0f, 8.0f, 10.0f };
    const FlightState *fs = &g->ac[id].fs;
    Vec3 vel = ac_vel(fs), fh, dh, c;
    float hs, V = E_MAXF(fs->V, 100.0f), floorM, sfc, agl, fg, gam, D, s1, s2, sh[6], hmax, R, sg, cg, tR, hLoss, xRec, nA, l;
    int i, emerg = 0;
    hs = (float)sqrt(vel.x * vel.x + vel.z * vel.z);
    fh = hs > 1.0f ? v3(vel.x / hs, 0.0f, vel.z / hs) : hdg_vec(fs->heading);
    if (b->sm == SM_POINT) {
        c = v3_sub(b->aim, fs->pos);
        l = (float)sqrt(c.x * c.x + c.z * c.z);
        dh = l > 1.0f ? v3(c.x / l, 0.0f, c.z / l) : fh;
    } else dh = hdg_vec(b->hdgDes);
    floorM = b->masking ? s_maskAglM : s_floorAglM;
    sfc = surface(g, fs->pos.x, fs->pos.z);
    agl = fs->pos.y - sfc;
    fg = -1.5f;
    for (i = 0; i < 6; i++) {
        D = V * ts[i];
        s1 = surface(g, fs->pos.x + fh.x * D, fs->pos.z + fh.z * D);
        s2 = surface(g, fs->pos.x + dh.x * D, fs->pos.z + dh.z * D);
        sh[i] = s1;
        fg = E_MAXF(fg, (float)atan2(E_MAXF(s1, s2) + floorM - fs->pos.y, D));
    }
    if (agl < floorM) fg = E_MAXF(fg, E_DEG2RAD(15.0f) * clampf((floorM - agl) / floorM, 0.0f, 1.0f));
    b->floorGamma = clampf(fg, -1.5f, E_DEG2RAD(35.0f));
    /* dive recovery: roll upright, pull at 0.9 x G limit; must end recoverMarginM above the highest surface */
    gam = fs->gamma;
    if (gam < -0.03f) {
        nA = 0.9f * fs->nLim;
        if (nA < 2.0f) nA = 2.0f;
        R = V * V / (GRAV * (nA - 1.0f));
        sg = (float)sin(-gam);
        cg = (float)cos(gam);
        tR = 0.4f + (float)fabs(fs->bank) / E_MAXF(flight_roll_rate_max(V), 0.5f);
        hLoss = R * (1.0f - cg) + V * sg * tR;
        xRec = R * sg + V * cg * tR;
        hmax = sfc;
        for (i = 0; i < 6; i++) if (V * ts[i] <= xRec + 500.0f) hmax = E_MAXF(hmax, sh[i]);
        if (fs->pos.y - hLoss - s_recoverMarginM < hmax) emerg = 1;
    }
    if (!b->masking && agl < s_avoidAglM && gam < 0.1f) emerg = 1;
    if (b->masking && agl < 0.5f * s_maskAglM && gam < 0.05f) emerg = 1;
    if (emerg) {
        if (!b->avoid) { s_st[id].avoidEntries++; log_printf(LOG_DEBUG, "ai: avoid id=%d agl=%.0f gam=%.1f", id, (double)agl, (double)E_RAD2DEG(gam)); }
        b->avoid = 1;
        b->avoidTick = g->tick;
    } else if (b->avoid && tick_sec(g->tick, b->avoidTick) > 1.0f && gam > 0.0f) b->avoid = 0;
}

/* "DEFEND triggers on every inbound missile with defendProb": every yes-threat whose reaction time passed must find
   the brain in DEFEND (entered for it, or already defending another missile) */
static void defend_account(const Game *g, int id, Brain *b)
{
    int k;
    for (k = 0; k < b->nTh; k++) {
        Threat *t = &b->th[k];
        if (!t->defend || t->due || tick_sec(g->tick, t->firstTick) < b->p->reactSec) continue;
        t->due = 1;
        s_st[id].defendDue++;
        if (b->state == AIS_DEFEND) s_st[id].defendCovered++;
        else log_printf(LOG_WARN, "warn: ai defend missed id=%d slot=%d state=%s", id, t->slot, ai_state_name(b->state));
    }
}

/* mid-air collision avoidance (any aircraft): closest approach within collideSec closer than collideM -> steer away
   from the other aircraft's position at that moment, full G limit, until the next think */
static void collide_check(const Game *g, int id, Brain *b)
{
    const FlightState *fs = &g->ac[id].fs;
    Vec3 me = ac_vel(fs), f = ac_fwd(fs), r, v, c, cp;
    float vv, t, miss, best = 1e9f, l;
    int j;
    b->collide = 0;
    for (j = 0; j < g->numAircraft; j++) {
        if (j == id || !alive(g, j)) continue;
        r = v3_sub(g->ac[j].fs.pos, fs->pos);
        if (v3_dot(r, r) > 2500.0f * 2500.0f) continue;
        v = v3_sub(ac_vel(&g->ac[j].fs), me);
        vv = v3_dot(v, v);
        t = vv > 1.0f ? -v3_dot(r, v) / vv : 0.0f;
        if (t < 0.0f || t > s_collideSec) continue;
        c = v3_madd(r, v, t);
        miss = v3_len(c);
        if (miss > s_collideM || t >= best) continue;
        best = t;
        cp = v3_madd(c, f, -v3_dot(c, f));         /* its CPA offset across my nose */
        l = v3_len(cp);
        if (l < 5.0f) cp = (id < j) ? v3(0.0f, 1.0f, 0.0f) : v3(0.0f, -1.0f, 0.0f);   /* dead on: one up, one down */
        else cp = v3_scale(cp, 1.0f / l);
        b->colDir = v3_norm(v3_madd(f, cp, -1.5f));
        b->collide = 1;
    }
}

static void bounds_check(Game *g, int id, Brain *b)
{
    const FlightState *fs = &g->ac[id].fs;
    float cx, cz;
    world_centre(g, &cx, &cz);
    b->bounds = 0;
    if (b->state == AIS_DEFEND || hdist(fs->pos, v3(cx, 0.0f, cz)) < s_areaRadiusKm * 1000.0f) return;
    b->bounds = 1;
    goal_heading(b, bearing_to(fs->pos, v3(cx, 0.0f, cz)), 0, E_MAXF(fs->pos.y, 2000.0f), b->p->gUse, b->thr);
}

/* ---------------------------------------------------------------- think */
void ai_think(Game *g, int id)
{
    Brain *b;
    float np;
    if (id < 0 || id >= MAX_AIRCRAFT) return;
    b = &s_b[id];
    if (!b->on || !alive(g, id) || g->ac[id].fs.onGround) return;
    b->thinkTick = g->tick;
    s_st[id].thinks++;
    perceive(g, id, b);
    threats(g, id, b);
    think_states(g, id, b);
    if (b->state != AIS_DEFEND) { defend_trigger(g, id, b); if (b->state == AIS_DEFEND) think_states(g, id, b); }
    defend_account(g, id, b);
    collide_check(g, id, b);
    bounds_check(g, id, b);
    terrain_look(g, id, b);
    if (s_logStates >= 2 && (g->tick % 120u) == (u32)(id * 5) % 120u) {
        const FlightState *fs = &g->ac[id].fs;
        Vec3 kp2 = known_pos(g, b);
        DlzZone z;
        float R = v3_len(v3_sub(kp2, fs->pos));
        int zt = b->know ? dlz_ai_zone(lrm_kind(g, id), fs->pos, ac_vel(fs), kp2, b->kVel, &z) : 0;
        log_printf(LOG_INFO, "ai: trace id=%d t=%.0f st=%s tgt=%d know=%d trk=%d lock=%d R=%.0f nose=%.0f rmax=%.0f alt=%.0f kt=%.0f thr=%d "
                   "avoid=%d fg=%.0f nTh=%d lrm=%d srm=%d srmLock=%d", id, (double)(g->tick * SIM_DT), ai_state_name(b->state), b->target,
                   b->know, b->tracked, radar_lock(id), (double)R, (double)E_RAD2DEG(off_nose(fs, kp2)), (double)(zt ? z.rmax : -1.0f),
                   (double)fs->pos.y, (double)(fs->V * MS2KT), b->thr, b->avoid, (double)E_RAD2DEG(b->floorGamma), b->nTh,
                   weapons_state(id)->lrm, weapons_state(id)->srm, weapons_state(id)->srmLock);
    }
    /* watchdog */
    np = tick_sec(g->tick, b->progTick);
    if (np > s_st[id].maxNoProgressSec) s_st[id].maxNoProgressSec = np;
    if (np > s_stuckSec && b->state != AIS_CRUISE && b->state != AIS_DRONE) {
        s_st[id].stuck++;
        log_printf(LOG_WARN, "warn: ai stuck id=%d state=%s sec=%.0f tgt=%d know=%d", id, ai_state_name(b->state), (double)np,
                   b->target, b->know);
        set_state(g, id, b, AIS_EXTEND, "stuck");
    }
}

/* ---------------------------------------------------------------- steer */
/* keyboard axis towards a target value at the [pilot] rates (DESIGN 5.2): away from 0 at rFine below 'fine' then
   at 'rate', back towards 0 at 'ret', across 0 at 'cross' (stops at 0 for that tick) */
static float ramp(float x, float t, float fine, float rFine, float rate, float ret, float cross, float dt)
{
    float nx, r;
    if (x == t) return t;
    if (t * x < 0.0f) {
        nx = x > 0.0f ? x - cross * dt : x + cross * dt;
        if (nx * x <= 0.0f) nx = 0.0f;
        return nx;
    }
    if ((float)fabs(t) > (float)fabs(x)) {
        r = (float)fabs(x) < fine ? rFine : rate;
        nx = t > 0.0f ? x + r * dt : x - r * dt;
        if ((t > 0.0f && nx > t) || (t < 0.0f && nx < t)) nx = t;
        return nx;
    }
    nx = x > 0.0f ? x - ret * dt : x + ret * dt;
    if ((x > 0.0f && nx < t) || (x < 0.0f && nx > t)) nx = t;
    return nx;
}

static Vec3 goal_dir(const Game *g, Brain *b, const FlightState *fs)
{
    Vec3 p, r;
    float rh, gam, h, rem, V = E_MAXF(fs->V, 100.0f);
    if (b->sm == SM_POINT) {
        p = v3_madd(b->aim, b->aimVel, tick_sec(g->tick, b->thinkTick));
        r = v3_sub(p, fs->pos);
        if (!b->useAlt) return v3_dot(r, r) < 1.0f ? ac_fwd(fs) : v3_norm(r);
        rh = (float)sqrt(r.x * r.x + r.z * r.z);
        gam = clampf((float)atan2(b->altDes - fs->pos.y, E_MAXF(rh, V * 6.0f)), -0.6f, 0.6f);
        if (rh < 1.0f) return v3(0.0f, (float)sin(gam), 0.0f);
        h = (float)atan2(r.x, r.z);
    } else h = b->hdgDes;
    if (b->sm == SM_HDG && b->turnSign) {
        rem = wrap_2pi((float)b->turnSign * (b->hdgDes - fs->heading));
        if (rem < E_DEG2RAD(15.0f) || rem > 2.0f * E_PI - 0.05f) b->turnSign = 0;
        else h = fs->heading + (float)b->turnSign * E_MINF(rem, 0.5f * E_PI);
    }
    rem = wrap_pi(h - fs->heading);                 /* level turns: never ask for more than 90 deg at once */
    if (rem > 0.5f * E_PI) h = fs->heading + 0.5f * E_PI;
    else if (rem < -0.5f * E_PI) h = fs->heading - 0.5f * E_PI;
    if (b->sm == SM_POINT) return v3((float)sin(h) * (float)cos(gam), (float)sin(gam), (float)cos(h) * (float)cos(gam));
    gam = clampf((float)atan2(b->altDes - fs->pos.y, V * 6.0f), -0.6f, 0.6f);
    return v3((float)sin(h) * (float)cos(gam), (float)sin(gam), (float)cos(h) * (float)cos(gam));
}

/* DESIGN 13.2: desired direction -> bank error + load factor -> (up, ur) through the flight model's mapping */
static void steer_dir(const FlightState *fs, Vec3 d, float gCap, float *upT, float *urT)
{
    Mat3 m;
    Vec3 r, u, f, dp, A, L;
    float V = E_MAXF(fs->V, 50.0f), cosT, theta, len, aT, aMax, an, bankErr, nDes, nMax, nC, c, nLim, nHold, nNeg, up, pmax;
    q_to_mat3(fs->qPath, &m);
    r = v3(m.m[0][0], m.m[0][1], m.m[0][2]);
    u = v3(m.m[1][0], m.m[1][1], m.m[1][2]);
    f = v3(m.m[2][0], m.m[2][1], m.m[2][2]);
    nLim = fs->nLim;
    nHold = fs->nHold;
    nNeg = flight_neg_g_limit();
    nMax = gCap * nLim;
    cosT = clampf(v3_dot(d, f), -1.0f, 1.0f);
    theta = (float)acos(cosT);
    dp = v3_madd(d, f, -cosT);
    len = v3_len(dp);
    if (len < 1e-3f) dp = cosT > 0.0f ? v3(0.0f, 0.0f, 0.0f) : r;   /* straight behind: turn right */
    else dp = v3_scale(dp, 1.0f / len);
    aT = theta > 1.5f ? 1e6f : V * s_kTurn * theta;
    aMax = nMax * GRAV * 1.2f;
    if (aT > aMax) aT = aMax;
    A = v3_madd(v3(0.0f, GRAV, 0.0f), dp, aT);      /* turn demand + gravity compensation */
    A = v3_madd(A, f, -v3_dot(A, f));
    an = v3_len(A);
    nDes = an / GRAV;
    if (an < 0.3f * GRAV) bankErr = -0.5f * fs->bank;   /* near zero lift: keep the wings level-ish */
    else {
        L = v3_scale(A, 1.0f / an);
        bankErr = (float)atan2(v3_dot(L, r), v3_dot(L, u));
        if ((float)fabs(bankErr) > E_DEG2RAD(120.0f) && theta < 0.5f) {   /* small nose-down correction: push */
            bankErr = wrap_pi(bankErr - E_PI);
            nDes = -nDes;
            if (nDes < s_pushMaxG) nDes = s_pushMaxG;
        }
    }
    nDes = clampf(nDes, nNeg, nMax);
    c = (float)cos(bankErr);
    if (c < 0.0f) c = 0.0f;
    nC = nHold + (nDes - nHold) * c;
    if (nC > nMax) nC = nMax;
    if (nC >= nHold) up = (nC - nHold) / E_MAXF(nLim - nHold, 0.05f);
    else up = (nC - nHold) / E_MAXF(nHold - nNeg, 0.05f);
    *upT = clampf(up, -1.0f, 1.0f);
    pmax = flight_roll_rate_max(V);
    *urT = pmax > 1e-3f ? clampf(s_kBank * bankErr / pmax, -1.0f, 1.0f) : 0.0f;
}

void ai_steer(Game *g, int id, PilotInput *out)
{
    Brain *b;
    const FlightState *fs;
    Vec3 d, fh, hd;
    float gCap, el, fg, upT, urT, hl, ratio;
    memset(out, 0, sizeof(*out));
    out->thr = THR_NORMAL;
    if (id < 0 || id >= MAX_AIRCRAFT) return;
    b = &s_b[id];
    if (!b->on || !alive(g, id)) return;
    fs = &g->ac[id].fs;
    if (fs->onGround) return;
    d = goal_dir(g, b, fs);
    gCap = b->gCap;
    fh = hdg_vec(fs->heading);
    if (b->avoid) {
        fg = E_MAXF(E_DEG2RAD(30.0f), b->floorGamma);
        d = v3(fh.x * (float)cos(fg), (float)sin(fg), fh.z * (float)cos(fg));
        gCap = 1.0f;
    } else if (b->collide) {
        d = b->colDir;
        gCap = 1.0f;
    } else {
        el = (float)asin(clampf(d.y, -1.0f, 1.0f));
        if (el < b->floorGamma) {
            hl = (float)sqrt(d.x * d.x + d.z * d.z);
            hd = hl > 1e-3f ? v3(d.x / hl, 0.0f, d.z / hl) : fh;
            fg = b->floorGamma;
            d = v3(hd.x * (float)cos(fg), (float)sin(fg), hd.z * (float)cos(fg));
        }
    }
    if (fs->pos.y > s_ceilingM && d.y > 0.0f) {
        hl = (float)sqrt(d.x * d.x + d.z * d.z);
        d = hl > 1e-3f ? v3(d.x / hl, 0.0f, d.z / hl) : fh;
    }
    steer_dir(fs, d, gCap, &upT, &urT);
    b->up = ramp(b->up, upT, kp(0), kp(1), kp(2), kp(3), kp(4), SIM_DT);
    b->ur = ramp(b->ur, urT, kp(8), kp(5), kp(5), kp(6), kp(7), SIM_DT);
    out->up = b->up;
    out->ur = b->ur;
    out->thr = b->thr;
    out->fire = b->fireReq;
    out->flare = b->flareReq;
    b->fireReq = 0;
    b->flareReq = 0;
    /* G-use bookkeeping (n of the last tick vs this speed's limit) */
    ratio = fs->nLim > 0.1f ? fs->n / fs->nLim : 0.0f;
    if (b->avoid || tick_sec(g->tick, b->avoidTick) < 1.0f) { if (ratio > s_st[id].maxNRatioAvoid) s_st[id].maxNRatioAvoid = ratio; }
    else if (ratio > s_st[id].maxNRatio) s_st[id].maxNRatio = ratio;
}

/* ---------------------------------------------------------------- robot player hook */
void ai_robot_hook(const Game *cg, int id, PilotInput *in)
{
    Game *g = (Game *)cg;                           /* session hooks get a const Game; the AI's own state (radar
                                                       designation, g->rng rolls) is written as for any enemy */
    Brain *b;
    const struct TakeoffState *ts = session_takeoff();
    if (id < 0 || id >= MAX_AIRCRAFT) return;
    b = &s_b[id];
    if (!b->on || !alive(g, id)) return;
    if ((ts && takeoff_active(ts)) || g->ac[id].fs.onGround) { b->up = in->up; b->ur = in->ur; return; }
    if (((g->tick + (u32)id * 5u) % 12u) == 0) ai_think(g, id);
    ai_steer(g, id, in);
}
