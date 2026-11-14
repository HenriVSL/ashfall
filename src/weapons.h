/* weapons.h - stores, firing, flares, live missiles, hits, terrain impacts (DESIGN 12.1, 12.5, 12.6). Portable.
   Owner: T12. Game mechanics with invented numbers ([weapons], missiles in [lrm]/[srm], missile.h).
   T21a: an aircraft with AC_PLAYER launches its LRMs with the [lrm_player] set (missile_kind; Missile.kind,
   MissileView.kind); enemies fly [lrm], everyone [srm].

   weapons_tick(g) is tick step 7 (after flight_step + world_check). Per alive aircraft it reads Aircraft.in (the
   PilotInput of this tick, player and AI alike): wpnNext = toggle LRM/SRM, fire = launch request (cooldown
   fireCooldownSec, rails alternate), flare = one burst of flareBurst flares (pilot.cpp pulses every 500 ms while F
   is held). AI may also call weapons_fire / weapons_flares directly (T14). Then every live missile flies one
   missile_step at SIM_DT with a TrackView built from the truth of its target (datalink = the shooter's lock on that
   target, from the sensor below), other aircraft are proximity-fuzed, flares move, and events are queued.

   Firing rules (DESIGN 12.6): LRM with a TD in TRACK -> guided (datalink while the lock holds); TD in MEMORY ->
   guided inertial launch from the memory estimate; no TD -> maddog. SRM with the seeker lock (weapons_state.srmLock,
   refreshed every srmLockSec: the TD inside the gimbal and the aspect lock range with LOS, or with no TD the nearest
   aircraft inside the acqConeDeg circle at the boresight) -> guided; else unguided search. Empty -> WPE_EMPTY.
   Stores (12.1): lrm/srm/flares per life; refilled when the aircraft comes back alive or the session restarts it
   (session_respawns changes) and by weapons_set_stores / weapons_refill. Enemy default stores: [weapons]
   enemyLrm/enemySrm/enemyFlares (T14/T17 set per difficulty with weapons_set_stores).

   Sensor (radar lock state per observer, DESIGN 11.3) comes through weapons_set_sensor(fn). Until T11/T17 wire
   it, a fallback sensor is used: TD = nearest alive aircraft of the other side within fbLockDeg of the nose and
   fbLockM, LOS clear (refreshed at 10 Hz, staggered per observer); lock = TRACK while it qualifies.

   Flares (12.5): each burst spawns flareBurst flares (velocity = aircraft velocity - flareBackMs along the nose +
   random spread from g->rng, gravity flareGravG, drag flareDragK, burn flareBurnSec). Then a decoy roll for every
   hostile missile in TERMINAL homing on the dispenser within decoyM whose gimbal contains the flare: p = pFlareSrm
   or pFlareLrm, + pTailBonus x tail aspect, x aiFlareFactor when the dispenser is not the player, cap pCap; one
   roll per missile per rollSec. Success: the missile homes on the flare (MF_DECOYED, never reacquires).
   Target destroyed (AC_ALIVE cleared): missiles homing on it self-destruct (14.2). */
#ifndef WEAPONS_H
#define WEAPONS_H

#include "game.h"
#include "missile.h"

void  weapons_params(void);                         /* [weapons] */
void  weapons_reset(Game *g);
void  weapons_tick(Game *g);                        /* tick step 7 */

/* ---- T12 additions ---- */
enum { WLOCK_NONE, WLOCK_TRACK, WLOCK_MEMORY };
typedef struct WpnSensor {
    int tdId;                                       /* designated target aircraft id, -1 = none */
    int lock;                                       /* WLOCK_*: TRACK = STT holds (datalink), MEMORY = TD dashed */
    Vec3 memPos, memVel;                            /* last known target state when lock == MEMORY */
} WpnSensor;
typedef void (*WpnSensorFn)(const Game *g, int observer, WpnSensor *out);
void  weapons_set_sensor(WpnSensorFn fn);           /* NULL = built-in fallback sensor */
void  weapons_sensor(const Game *g, int observer, WpnSensor *out);   /* whichever is active */

enum { RAIL_COUNT = 8 };                            /* A1 tags rail_0..7: 0/1 wingtips (SRM), 2..7 pylons (LRM) */
typedef struct WpnState {                           /* per aircraft, read-only for HUD / AI / sound */
    int lrm, srm, flares;                           /* stores left */
    int sel;                                        /* MSL_LRM / MSL_SRM selected */
    int cooldown;                                   /* ticks until the next launch is allowed */
    int srmLock;                                    /* aircraft id the SRM seeker is locked on, -1 = none (tone) */
    int srmLockTd;                                  /* 1 = that lock is the TD (seeker circle slaved to the TD) */
    int srmNear;                                    /* 1 = a heat target is near the seeker circle (growl) */
    int lastRail[2];                                /* last rail used per type (alternation) */
    float railEmpty[RAIL_COUNT];                    /* seconds until rail i shows a missile again (0 = loaded) */
    int fired[2], hits[2];                          /* this life: launches / hits per type (score, results) */
} WpnState;
const WpnState *weapons_state(int id);
void  weapons_set_stores(int id, int lrm, int srm, int flares);   /* also the refill amounts of that aircraft */
void  weapons_refill(int id);
void  weapons_select(int id, int type);

/* launch / flares on request (AI); returns the missile slot or -1 (cooldown, empty: WPE_EMPTY queued) */
int   weapons_fire(Game *g, int id);
int   weapons_flares(Game *g, int id);              /* one burst; returns flares spawned */

/* live missiles: slots 0..MAX_MISSILES-1, active = phase != MPH_DEAD */
typedef struct MissileView {
    int slot, type, phase, flags, owner, target;    /* target: aircraft id (or flare index when MF_DECOYED) */
    Vec3 pos, prevPos, dir;                         /* render interpolates prevPos -> pos with alpha */
    float V, age;
    float act, ttg;                                 /* HUD: s until ACTIVE (LRM, else -1), s to impact (-1 unknown) */
    int homing;                                     /* aircraft id its seeker tracks (TERMINAL, not decoyed), -1 */
    int rail;
    u32 serial;                                     /* unique per launch (trail ring buffers) */
    float launchDist;                               /* m to the target at launch (radar_rwr_missile_warn), 0 none */
    /* ---- T21a ---- */
    int kind;                                       /* MSLK_* parameter set (the player's LRM: MSLK_LRM_PLAYER) */
} MissileView;
int   weapons_missiles(MissileView *out, int max);  /* active missiles, slot order; returns count */
const Missile *weapons_missile(int slot);           /* raw state (tests, dlz debugging); NULL if out of range */
int   weapons_live_count(void);
/* most relevant missile of 'owner' at 'target' for the HUD (ACT/TTG line): the most recent live one; -1 none */
int   weapons_latest_at(int owner, int target);
/* missiles homing on aircraft id (MISSILE warning, threat arrows): seeker or datalink guidance on it */
int   weapons_threats(int id, int *slots, int max);

typedef struct FlareView { int slot, owner; Vec3 pos, prevPos; float age, burn; } FlareView;
int   weapons_flares_view(FlareView *out, int max);

/* events (sound, fx, mission, HUD): ring of WPN_EVENTS, keep your own read index (like world.h) */
enum { WPE_NONE, WPE_LAUNCH, WPE_HIT, WPE_GROUND, WPE_EXPIRED, WPE_FLARE_HIT, WPE_SELFDESTRUCT, WPE_FLARES,
       WPE_DECOY, WPE_EMPTY, WPE_ACQUIRED, WPE_LOST, WPE_PASS, WPE_SELECT };
enum { WPN_EVENTS = 64 };
typedef struct WpnEvent {
    int type;                                       /* WPE_* */
    u32 tick;
    int slot, mtype;                                /* missile slot and MSL_* (-1 for flare/empty/select events) */
    int owner;                                      /* shooter / dispenser */
    int victim;                                     /* HIT: aircraft hit; LAUNCH/ACQUIRED/PASS: target; else -1 */
    Vec3 pos;                                       /* where (hit/impact point, launch point, dispenser) */
    float damage;                                   /* HIT: damageEnemy / damagePlayer of the missile type */
} WpnEvent;
u32   weapons_event_count(void);                    /* total events ever queued (read index runs up to this) */
const WpnEvent *weapons_event(u32 index);           /* NULL if overwritten or not yet queued */
const char *weapons_event_name(int type);

#endif
