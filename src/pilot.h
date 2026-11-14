/* pilot.h - keys -> shaped axes, throttle state, short/long press classifier (DESIGN 5). Portable. Owner: T05.
   PilotInput is fixed by DESIGN 5.1 (changes need orchestrator approval). Must not include game.h (game.h
   includes this header).

   pilot_update runs once per 120 Hz tick for the player. Axis model (DESIGN 5.2, racer KEYBOARD.md lesson: a key
   moves an axis at a rate, never jumps to full):
     key held towards s (same side or from 0): up moves to s at pitchRateFine while |up| < pitchFine, then at
                                               pitchRate (two stages: taps aim, holds pull)
     key held against the current axis:       up moves back to 0 at pitchCross, then on at the stage rates
     no key (or both keys):                   up moves to 0 at pitchReturn
     roll: the same with rollRateIn / rollCross / rollReturn; an optional fine stage (rollFine, rollRateFine) is off
     by default (rollFine = 0 = one stage, as DESIGN 5.2).
   invertPitch swaps the arrow meaning (default 0: Up arrow = nose UP, user decision).
   Joystick (keys->joyValid): X/Y map straight to ur/up with deadzone joyDead and expo joyExpo, no ramps (stick back
   = joyY > 0 = nose up). When the stick is inside the deadzone the keyboard axis is used, and the keyboard axis
   continues from the stick value (no jump).
   Throttle: fast && !slow = FAST, slow && !fast = SLOW, else NORMAL.
   Events in PilotInput (all one-tick pulses except look and the axes):
     fire, wpnNext, view = press edges; flare = 1 on the press tick and every flareRepeatMs while held (one flare
     burst per pulse); look = held; tgtShort / tgtLong from the press classifier below.

   Press classifier (DESIGN 5.3; generic, T11 uses it for S): counted in sim ticks. Feed it every tick with the
   e_input counters in_action_held_ticks (heldTicks) and in_action_released (releasedTicks). LONG fires on the tick
   heldTicks reaches longTicks (longPressMs * 120 / 1000 = 36 ticks at 300 ms); on release SHORT fires if LONG
   never did. A tap shorter than one poll (released = 1, held never seen) is a SHORT. heldTicks dropping to 0
   without a release (focus loss) resets without an event. Scripts/tests without e_input turn a raw "is down" bool
   into the same two counters with press_counter_feed. */
#ifndef PILOT_H
#define PILOT_H

#include "e_base.h"

enum { THR_SLOW, THR_NORMAL, THR_FAST };            /* DESIGN 4.4 */

/* DESIGN 5.1: produced by pilot_update (player), ai_steer (enemies) or the takeoff autopilot */
typedef struct PilotInput { float up, ur; int thr; int fire, flare, wpnNext, tgtShort, tgtLong, view, look; } PilotInput;

/* raw controls for one tick (main.cpp fills it from e_input, a -script or a replay feeds it headless) */
typedef struct PilotKeys {
    int up, down, left, right;                      /* arrow keys held (before invertPitch) */
    int fast, slow;                                 /* A, Z held */
    int tgtHeld, tgtReleased;                       /* S: in_action_held_ticks / in_action_released (DESIGN 5.3) */
    int fire, flare, wpnNext, view, look;           /* held states (edges are found by pilot_update) */
    int joyValid; float joyX, joyY;                 /* analogue stick -1..1 when present */
} PilotKeys;

/* ---- generic short/long press classifier (DESIGN 5.3) ---- */
enum { PRESS_NONE = 0, PRESS_SHORT = 1, PRESS_LONG = 2 };
typedef struct PressClass { int held; int longFired; } PressClass;
void  press_reset(PressClass *pc);
int   press_classify(PressClass *pc, int heldTicks, int releasedTicks, int longTicks);   /* PRESS_* this tick */
typedef struct PressCounter { int held; } PressCounter;        /* raw bool -> e_input-style counters */
void  press_counter_feed(PressCounter *c, int down, int *heldTicks, int *releasedTicks);
int   pilot_long_press_ticks(void);                 /* [pilot] longPressMs in 120 Hz ticks (300 ms = 36) */

/* shaping state per aircraft */
typedef struct PilotState {
    float up, ur;                                   /* shaped axes (-1..1) after the last update */
    int longFired;                                  /* = tgt.longFired (kept for older readers) */
    PilotKeys prevKeys;
    /* ---- added by T05 ---- */
    PressClass tgt;                                 /* S classifier */
    int flareHeld;                                  /* ticks F has been held */
    int joyPitch, joyRoll;                          /* 1 = that axis came from the stick last tick */
    float gRef;                                     /* Novice: flight-path angle reference (rad) */
    int gRefValid;                                  /* 0 = take the current gamma on the next Novice tick */
    int lastMode;                                   /* PILOT_* used on the last update */
    float nDesPrev;                                 /* Novice: last desired load factor (lead term) */
} PilotState;

void  pilot_params(void);                           /* [pilot] */
void  pilot_reset(PilotState *ps);
void  pilot_update(PilotState *ps, const PilotKeys *keys, PilotInput *out);   /* once per 120 Hz tick */

/* ---- added by T05 ---- */
void  pilot_seed_axes(PilotState *ps, float up, float ur);   /* takeoff handover: shaped axes start here (6.1) */
/* one keyboard axis step (exposed for tests and the takeoff override): x = axis, s = key direction -1/0/+1 */
float pilot_axis_step(float x, int s, float fine, float rateFine, float rate, float ret, float cross, float dt);
float pilot_joy_shape(float v);                     /* deadzone + expo of [pilot] */
int   pilot_invert_pitch(void);

/* ---- control modes (user 2026-11-13, Ace Combat "Expert" / "Novice") ----
   Expert (default): pilot_update as above, PilotInput.up/ur = the shaped key axes (true roll + pitch).
   Novice: an autopilot-style mapping in front of the SAME flight model. The shaped key axes (PilotState.up/ur,
   same ramps and invertPitch) become intents:
     roll axis  -> bank target = ur * noviceBankDeg (Left held = 70 deg left bank); the roll command steers the
                   bank there (rate noviceBankGain * error, at most noviceRollRateDeg); released = wings level.
     pitch axis -> flight-path angle rate = up * noviceGammaRateDeg into a gamma reference, limited to
                   +-novicePitchLimitDeg (no loops, no inverted flight); released = hold the current climb angle
                   (noviceAutoLevelPitch = 1: ease back to level at noviceLevelGain).
     load factor = (cos(gamma) + V * gammaRate / g) / cos(bank), within the G limit: a held turn is level, and
                   Up + Left = a tighter climbing turn up to the G limit. It is converted to PilotInput.up exactly
                   as the flight model maps up -> nCmd, so the physics are untouched.
   Novice needs the aircraft state: call pilot_update_fs (pilot_update = pilot_update_fs with fs = NULL, which
   always flies Expert). [pilot] mode = expert | novice (F5 re-applies the ini value); pilot_set_mode /
   pilot_toggle_mode change the running mode (dev key F6) by writing that param, so a replay recording must store
   a PARAMS record after a toggle (as after F5). */
enum { PILOT_EXPERT = 0, PILOT_NOVICE = 1 };
struct FlightState;
void  pilot_update_fs(PilotState *ps, const PilotKeys *keys, const struct FlightState *fs, PilotInput *out);
int   pilot_mode(void);                             /* PILOT_EXPERT / PILOT_NOVICE from [pilot] mode */
void  pilot_set_mode(int mode);                     /* logs "pilot: mode=novice" */
int   pilot_toggle_mode(void);                      /* returns the new mode */
const char *pilot_mode_name(int mode);              /* "EXPERT" / "NOVICE" (HUD message text) */

#endif
