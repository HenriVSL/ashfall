/* takeoff.h - ground roll, automated takeoff + climb-out, handover (DESIGN 6.1). Portable. Owner: T06.

   Phases (DESIGN 6.1; every number is a [takeoff] key, live on F5):
     READY    parked at the runway threshold (BASE spawn, wheels on the ribbon: CG gearHeightM above it), engine
              idle, HUD message "A: TAKEOFF". Ends on any throttle-up (PilotInput.thr == THR_FAST, i.e. A).
     ROLL     on the ground along the threshold heading: V += rollAccMil (rollAccAB while A held). Ends at vRotateKt.
     ROTATE   the NOSE attitude eases to rotatePitchDeg over rotateSec (smoothstep); the cosmetic alpha eases to the
              flight model's 1 g alpha at this speed and the path pitch is nose - alpha, so the flight model takes
              over at liftoff without a visible nose jump. Ends when V >= vLiftKt and the rotation is complete:
              onGround cleared, V >= the speed floor, flight_set_path() with the current path, WEV_TO_LIFTOFF.
     CLIMB    the flight model flies; the autopilot (ap_steer) makes PilotInput: wings level on the extended centre
              line, path pitch climbPitchDeg, NORMAL (FAST while A held). Gear up from gearUpFt AGL in gearSec.
              Ends at handoverFt AGL and handoverKt, or at once on a player override: a pitch/roll input held longer
              than overrideSec above overrideMinFt AGL.
     HANDOVER "YOU HAVE CONTROL" for handoverMsgSec. The output blends from the autopilot to the player over
              handoverBlendSec (an axis the player is moving is his at once), so the hand-off is continuous.
     DONE     takeoff_tick returns 0: the player's own input flies.
   Per tick (main, DESIGN 2.6 step 4, after pilot_update_fs for the player):
       if (takeoff_tick(&ts, pl, &in, &own, &g_game)) in = own;
   takeoff_bind_pilot(&ts, &s_pilot[PLAYER_ID]) once lets the override see the shaped KEY axes (PilotState.up/ur):
   needed in Novice, where PilotInput.up/ur are computed commands (not key axes), and at the handover it makes
   Novice take the current climb angle as its hold reference. Unbound, the override reads PilotInput.up/ur
   (works in Expert only) and Novice keeps its own lagging reference (a few degrees flatter).
   Events (world.h queue, for HUD messages and sounds): WEV_TO_READY, WEV_TO_ROLL, WEV_TO_LIFTOFF, WEV_GEAR_UP,
   WEV_GEAR_LOCKED, WEV_HANDOVER. Log: one "takeoff: phase=NAME ..." line per phase change (the phase timeline) and
   a "takeoff: summary ..." line at the handover. While a takeoff phase is active the PULL UP predictor is
   suppressed (world_set_takeoff). */
#ifndef TAKEOFF_H
#define TAKEOFF_H

#include "game.h"

enum { TO_READY, TO_ROLL, TO_ROTATE, TO_CLIMB, TO_HANDOVER, TO_DONE };
typedef struct TakeoffState {
    int phase; u32 phaseTick;                       /* TO_*, Game.tick when the phase began */
    /* ---- T06 ---- */
    PilotState *ps;                                 /* takeoff_bind_pilot (may be NULL) */
    int acId;                                       /* the aircraft parked by takeoff_park */
    Vec3 origin; float hdg;                         /* roll line: threshold point and heading (rad) */
    float groundY;                                  /* CG height on the runway (ribbon + gearHeightM) */
    float rotNose0;                                 /* rotation start: alpha at ROTATE entry (rad) */
    float gammaCmd;                                 /* CLIMB: path pitch command, ramps to climbPitchDeg (rad) */
    int gearMoving;                                 /* 1 while retracting */
    int holdTicks;                                  /* override: ticks with a pitch/roll input */
    int overridden;                                 /* 1 = the handover came from a player override */
    float apUp, apUr;                               /* last autopilot axes (HANDOVER blend) */
    u32 tickA, tickLift, tickHand;                  /* Game.tick of A, liftoff, handover (0 = not yet) */
    float maxLat;                                   /* max |centre-line error| from READY to handover (m) */
    float minClear;                                 /* min clearance above the surface after liftoff (m) */
    float liftDist;                                 /* ground roll distance to liftoff (m) */
} TakeoffState;

void  takeoff_params(void);                         /* [takeoff] */
void  takeoff_reset(TakeoffState *ts, Aircraft *ac, const Game *g);   /* fs.onGround set: takeoff_park; else (air
                                                       start) phase DONE */
int   takeoff_tick(TakeoffState *ts, Aircraft *ac, const PilotInput *player, PilotInput *out, const Game *g);
                                                    /* 1 while the takeoff logic owns the aircraft (out = its input) */

/* ---- added by T06 ---- */
void  takeoff_park(TakeoffState *ts, Aircraft *ac, const Game *g);   /* (re)spawn on the runway threshold, READY:
                                                       every start and respawn (user: always from the airfield);
                                                       sets fs (V 0, gear down, onGround), flags AC_ALIVE|AC_ONGROUND,
                                                       hp 100, prev = fs; clears the aircraft's world state */
void  takeoff_bind_pilot(TakeoffState *ts, PilotState *ps);   /* override reads its key axes; the handover
                                                       resets its Novice climb-angle reference (gRefValid = 0) */
int   takeoff_active(const TakeoffState *ts);       /* phase READY..HANDOVER */
const char *takeoff_phase_name(int phase);          /* "READY" ... "DONE" */
const char *takeoff_message(const TakeoffState *ts, const Game *g);   /* HUD text or NULL: "A: TAKEOFF" (READY),
                                                       "YOU HAVE CONTROL" (HANDOVER, handoverMsgSec) */
float takeoff_centre_line_error(const Game *g, Vec3 pos);   /* signed metres right of the runway centre line */
float takeoff_gear_height(void);                    /* [takeoff] gearHeightM: CG above the wheels */

/* Generic autopilot (takeoff climb, area turn-back; bench routes / tests may use it): sets out->up/ur so the
   aircraft turns to heading hdgRad (bank = error * apHdgGain, at most maxBankRad and the G limit's level-turn bank)
   and flies path pitch gammaRad (rate at most apGammaRateDeg). Same mapping as the flight model (up -> nCmd), so
   physics are untouched. Leaves the other PilotInput fields alone. */
void  ap_steer(const FlightState *fs, float hdgRad, float gammaRad, float maxBankRad, PilotInput *out);

#endif
