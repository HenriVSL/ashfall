/* world.h - island query wrapper, area bounds, crash tests, spawn points, PULL UP predictor, world events
   (DESIGN 6.2-6.4, 8). Portable. Owner: T06. Not to be confused with engine/world/e_world.h (airbase .trk + sky).

   Everything comes from the terrain API (DESIGN 8.6): surface = max(terrain_height, seaLevel), runway/threshold/air
   spawns/boxes from terrain_base(). The area centre is the mean of the 4 air spawns (islandc puts them N/E/S/W of
   the island centre at equal range; the terrain API has no centre accessor yet). Every number is a [world] key.

   world_check (tick step 6, after flight_step), for every alive aircraft that is not on the ground:
     crash   pos.y - surface < crashClearM (WEV_CRASH_GROUND, or WEV_CRASH_SEA where the terrain is below sea level);
             sphere boxRadiusM vs the BASE boxes (WEV_CRASH_BOX); aircraft vs aircraft: swept spheres acRadiusM
             (closest approach of the two straight tick segments; both crash, WEV_CRASH_AIR). A crash clears
             AC_ALIVE, sets hp 0, puts the wreck on the surface and records the tick (world_respawn_due after
             wreckSec: the caller restarts on the runway with takeoff_park; DESIGN 6.3, 14.2).
     area    horizontal distance from the centre: >= areaWarnKm -> areaWarn on (event WEV_RETURN_AREA; off below
             areaWarnKm - 0.5 km, event WEV_AREA_OK); >= areaTurnKm and not heading in -> turnback on (WEV_TURNBACK) until the heading
             is within turnbackDoneDeg of the centre (then it rolls the wings level) and the bank is under 10 deg
             (WEV_TURNBACK_END). While turnback is on,
             world_override_input replaces the steering (call it in tick step 4, after takeoff_tick).
     PULL UP (the player only, 2 Hz): world_pullup_predict; on when the predicted impact is within pullUpSec
             (WEV_PULLUP), off when it is beyond pullUpSec + pullUpOffSec (WEV_PULLUP_OFF). Suppressed while a
             takeoff phase is active (world_set_takeoff) and below pullUpRunwayFt AGL over the runway.
   Return value: the player's bits this tick: crash bits on the crash tick, and the states areaWarn
   (WEV_RETURN_AREA), turnback (WEV_TURNBACK), pullUp (WEV_PULLUP) while they are on.

   Events: a ring of WORLD_EVENTS_MAX with a running sequence number, so several readers (HUD messages, sound,
   mission) each keep their own read position:
       static u32 s_seen; WorldEvent ev;
       while (s_seen < world_event_seq()) { if (world_event_get(s_seen, &ev)) handle(&ev); s_seen++; }
   Per-aircraft state lives here in arrays indexed by Aircraft.id (game.h rule); world_reset at session start. */
#ifndef WORLD_H
#define WORLD_H

#include "game.h"

enum { WEV_NONE = 0, WEV_CRASH_GROUND = 1, WEV_CRASH_SEA = 2, WEV_CRASH_BOX = 4, WEV_CRASH_AIR = 8,
       WEV_RETURN_AREA = 16, WEV_TURNBACK = 32, WEV_PULLUP = 64 };

void  world_params(void);                           /* [world] */
void  world_spawn_runway(const Game *g, Vec3 *pos, float *headingRad);   /* threshold point (ribbon top), heading */
void  world_spawn_air(const Game *g, Vec3 from, Vec3 *pos, float *headingRad);   /* nearest of the 4 air spawns */
int   world_check(Game *g);                         /* tick step 6; returns the player's WEV_* bits this tick */
float world_surface(const Game *g, float x, float z);   /* max(terrain, sea); flat sea without an island */

/* ---- added by T06 ---- */
enum { WEV_CRASH_ANY = WEV_CRASH_GROUND | WEV_CRASH_SEA | WEV_CRASH_BOX | WEV_CRASH_AIR };
/* event-only types (queue; never in world_check's return) */
enum { WEV_AREA_OK = 128, WEV_TURNBACK_END = 256, WEV_PULLUP_OFF = 512,
       WEV_TO_READY = 1024, WEV_TO_ROLL = 2048, WEV_TO_LIFTOFF = 4096, WEV_GEAR_UP = 8192, WEV_GEAR_LOCKED = 16384,
       WEV_HANDOVER = 32768 };
enum { WORLD_EVENTS_MAX = 64 };

typedef struct WorldEvent {
    int type;                                       /* one WEV_* value */
    int ac;                                         /* aircraft id */
    int other;                                      /* WEV_CRASH_AIR: the other aircraft, else -1 */
    u32 tick;                                       /* Game.tick */
    Vec3 pos;                                       /* where (crash: the wreck position) */
} WorldEvent;

typedef struct WorldStatus {                        /* per aircraft, updated by world_check */
    int crashed; int crashType; u32 crashTick;      /* WEV_CRASH_* of the crash */
    int areaWarn, turnback, pullUp;                 /* states (HUD: "RETURN TO AREA" + map arrow, PULL UP) */
    float pullUpT;                                  /* last predicted time to impact (s), 99 = none */
    float distCentre;                               /* horizontal distance from the area centre (m) */
    float brgCentre;                                /* heading towards the centre (rad, 0 = north, clockwise);
                                                       updated only within 2 km of areaWarnKm and beyond */
    float agl;                                      /* pos.y - surface (m) */
    int takeoff;                                    /* 1 = a takeoff phase owns the aircraft (PULL UP off) */
} WorldStatus;

void  world_reset(Game *g);                         /* session start: per-aircraft state + event queue */
void  world_reset_aircraft(int id);                 /* respawn: that aircraft's state (keeps the queue) */
void  world_event_push(const Game *g, int type, int ac, int other, Vec3 pos);
u32   world_event_seq(void);                        /* events pushed since world_reset */
int   world_event_get(u32 seq, WorldEvent *ev);     /* 0 = not pushed yet or already overwritten */
const char *world_event_name(int type);             /* "CRASH_GROUND" ... (logs) */
const WorldStatus *world_status(int id);
void  world_set_takeoff(int id, int active);        /* takeoff.cpp */
int   world_respawn_due(const Game *g, int id);     /* 1 = crashed at least wreckSec ago */
int   world_override_input(const Game *g, int id, PilotInput *in);   /* turn-back autopilot; 1 = in replaced */
void  world_centre(const Game *g, float *x, float *z);   /* area centre (mean of the air spawns) */
float world_agl(const Game *g, Vec3 p);             /* p.y - surface */
int   world_on_runway(const Game *g, float x, float z, float margin);   /* inside the runway rectangle + margin */
int   world_crash_test(const Game *g, Vec3 pos);    /* WEV_CRASH_GROUND/SEA/BOX for a point, or 0 (no aircraft) */
float world_pullup_predict(const Game *g, const FlightState *fs);   /* predicted time to impact (s), 99 = none */
void  world_air_start(Game *g, Aircraft *ac, Vec3 from);   /* DESIGN 6.2: nearest air spawn, NORMAL speed, wings
                                                       level, its heading (towards the island), gear up, alive */
float world_crash_clear(void);                      /* [world] crashClearM */

#endif
