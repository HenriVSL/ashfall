/* ai.h - enemy pilots (DESIGN 13) + the robot player for headless fights. Portable. Owner: T14.

   Every AI aircraft flies the SAME flight model through a PilotInput, with the keyboard ramp rates of [pilot]
   (pitchFine/pitchRateFine/pitchRate/pitchReturn/pitchCross, rollRateIn/rollReturn/rollCross; read live), load
   factor limited to gUse x the flight model's G limit (AVOID emergencies may use the full G limit, never more).
   Knowledge (no cheats): its own radar (radar_track / radar_designate / radar_lock), the wave-start attack order
   (the player's position when the wave spawned), the RWR/MAW (missiles at it, known as radar_rwr_missile_warn
   says: LRM at launch, SRM within 5 km), its own weapons state. The robot player profile may also read the
   minimap's knowledge (knowsAll: every enemy position, as the human sees with hud.mapShowAll = 1).

   Per tick (DESIGN 2.6 step 3, game.cpp session_tick):
       if (think tick) ai_think(g, id);   10 Hz: perception, threats, terrain look-ahead, state machine, fire/flare
                                          decisions, steering goal
       ai_steer(g, id, &g->ac[id].in);    every tick: goal -> desired direction -> bank/pull -> ramped axes,
                                          throttle, one-tick fire/flare pulses (weapons_tick reads Aircraft.in)
   States (DESIGN 13.1): PATROL, INTERCEPT, ATTACK_BVR (lock, fire one LRM, then CRANK), MERGE, EXTEND, DEFEND,
   + CRUISE (hold heading/altitude, only DEFEND interrupts it: tests), DRONE (TRAINING circuits, never fires or
   defends). AVOID is an override flag on top of any state (terrain within the dive-recovery distance, below
   avoidAglM, area radius, ceiling); see ai.cpp's header for the exact rules and the flare doctrine.

   Spawning (T17): put the aircraft into g->ac[id] (id, side SIDE_RED, flags AC_ALIVE, livery 1, hp, fs from
   flight_init + fs.onGround = 0, prev = fs), raise g->numAircraft, then ai_spawn(g, id, g->diff, 1, playerPos).
   ai_spawn sets the difficulty stores with weapons_set_stores (refilled by weapons when the aircraft comes back
   alive). Deterministic: the only randomness is g->rng (defend / flare rolls). */
#ifndef AI_H
#define AI_H

#include "game.h"

void  ai_params(void);                              /* [ai] [ai_easy] [ai_normal] [ai_hard] [ai_robot] (no dots: params snapshots split sec.key at the first dot) */
void  ai_reset(Game *g);                            /* session start: every brain off, summit search, stats */
void  ai_think(Game *g, int id);                    /* 10 Hz staggered (game.cpp) */
void  ai_steer(Game *g, int id, PilotInput *out);   /* every tick */

/* ---- added by T14 ---- */
enum { AIS_OFF = 0, AIS_PATROL, AIS_INTERCEPT, AIS_ATTACK_BVR, AIS_MERGE, AIS_EXTEND, AIS_DEFEND, AIS_CRUISE,
       AIS_DRONE, AIS_COUNT };

typedef struct AiProfile {                          /* DESIGN 13.3 (+ doctrine keys); params-backed per difficulty */
    float reactSec;                                 /* RWR/MAW warning -> DEFEND */
    float gUse;                                     /* fraction of the G limit used in manoeuvres */
    float lrmFireFrac;                              /* LRM fired inside table Rmax x this */
    float srmFireFrac;                              /* SRM fired inside table Rne x this (with seeker lock) */
    float fireCooldownSec;                          /* own launches at least this far apart */
    int   lrm, srm, flares;                         /* stores per life */
    float defendProb;                               /* chance to react (DEFEND) to each known inbound missile */
    float flareProb;                                /* chance per burst opportunity (missile < flareRangeM) */
    int   flareBursts;                              /* bursts per inbound missile at most (doctrine) */
    int   crank;                                    /* 1 = CRANK after an LRM launch */
    int   terrainMask;                              /* 1 = DEFEND hides behind the volcano when within maskRangeM */
    int   knowsAll;                                 /* robot player: minimap knowledge of every enemy */
    float lrmNeFrac;                                /* > 0: LRM also only inside table Rne x this (robot = a player
                                                       who waits for the flashing SHOOT); 0 = off (enemies) */
} AiProfile;

const AiProfile *ai_profile(int diff);              /* DIFF_EASY..DIFF_HARD (clamped) */
const AiProfile *ai_profile_robot(void);            /* [ai_robot]: the robot player of simh aifight / T17 */

/* spawn an AI brain on an aircraft already placed in g->ac[id]; hasOrder = attack order with the player's position
   (else PATROL until the radar finds someone) */
void  ai_spawn(Game *g, int id, int diff, int hasOrder, Vec3 orderPos);
void  ai_spawn_profile(Game *g, int id, const AiProfile *p, int hasOrder, Vec3 orderPos);
/* the robot player (id = PLAYER_ID normally): profile [ai_robot], targets the nearest enemy; no stores change */
void  ai_spawn_robot(Game *g, int id);
/* TRAINING drone: circuits of radius r around (cx, cz) at altitude alt, never fires, never defends */
void  ai_spawn_drone(Game *g, int id, float cx, float cz, float r, float alt);
/* tests: hold the current heading and altitude until a missile comes (DEFEND), then back to CRUISE */
void  ai_set_cruise(Game *g, int id);
void  ai_set_cruise_goal(int id, float hdgRad, float altM);   /* tests: new CRUISE heading/altitude */
void  ai_off(int id);                               /* brain off (dead slot reused for something else) */

/* session hook for the robot player: session_set_autopilot(ai_robot_hook). Thinks staggered at 10 Hz, steers every
   tick, replaces the player's input. Leaves the input alone while a takeoff phase owns the aircraft (the caller
   holds A in PilotKeys while READY so the takeoff roll starts). */
void  ai_robot_hook(const Game *g, int id, PilotInput *in);

int   ai_state(int id);                             /* AIS_* */
const char *ai_state_name(int st);
int   ai_target(int id);                            /* current target aircraft id or -1 */
int   ai_avoiding(int id);                          /* 1 = AVOID override active */
int   ai_masking(int id);                           /* 1 = deliberate terrain-masking manoeuvre in progress */
float ai_state_sec(const Game *g, int id);          /* seconds in the current state */

/* per-aircraft counters (simh aifight, -test ai); reset by ai_reset and ai_stats_clear */
typedef struct AiStats {
    int thinks;
    int threats;                                    /* known inbound missiles (RWR/MAW) */
    int defendYes;                                  /* defendProb rolls that said "react" */
    int defendEntered;                              /* DEFEND entries */
    int flareRolls, flareBursts;                    /* flare opportunities, bursts dispensed */
    int fired[2];                                   /* launch requests per MSL_* */
    int envViolations;                              /* launches outside the truth table zone (must stay 0) */
    int stuck;                                      /* watchdog: a state > stuckSec without progress */
    int avoidEntries, maskEntries;
    float maxStateSec[AIS_COUNT];                   /* longest stay per state (s) */
    float maxNoProgressSec;                         /* longest time without progress (s) */
    float maxNRatio;                                /* max n / nLim seen while not in AVOID */
    float maxNRatioAvoid;                           /* max n / nLim seen in AVOID */
    int defendDue;                                  /* "react" threats whose reaction time passed while alive */
    int defendCovered;                              /* ... of those, the AI was in DEFEND (must equal defendDue) */
} AiStats;
const AiStats *ai_stats(int id);
void  ai_stats_clear(void);

#endif
