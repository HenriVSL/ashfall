/* flight.h - arcade flight model (DESIGN 4). Portable. Owner: T05.
   Must not include game.h (game.h includes this header).

   Model (DESIGN 4.1): a point mass with a speed V and a flight-path frame qPath (forward = velocity, up = lift,
   right = right wing). PilotInput.up commands load factor n between the hold value nHold (= lift that keeps the
   path straight, auto-trim) and the speed-dependent G limit; PilotInput.ur commands roll rate. Turn rate follows
   from n and V (fast = wide, slow = tight). No sideslip: gravity's side component in a bank turns the jet
   (auto-coordination, [flight] latGrav). Speed goes towards a target set by the throttle state, with energy trade,
   induced drag and a hard floor. Every tunable is a [flight] key (flight_params), live on F5.

   Conventions (DESIGN 3): body/path frame +X right wing, +Y up (lift), +Z forward; heading 0 = north (+Z),
   clockwise; pitch/gamma + = nose up; bank + = right wing down. Positive rotation about +X is nose DOWN in this
   left-handed frame, so a nose-up rate is applied as a negative angle about +X (see flight.cpp step 3).

   Ground: while fs->onGround is set (takeoff phases, T06), flight_step does not move the aircraft (T06 owns pos,
   V and qPath on the ground); it still updates the throttle lags (ab, brake), surfaces and derived values.

   One tick = flight_step(fs, in, SIM_DT). Deterministic: float only, no RNG, no statics that carry state across
   aircraft (the lag-coefficient cache only depends on (tau, dt)). */
#ifndef FLIGHT_H
#define FLIGHT_H

#include "e_base.h"
#include "e_math.h"
#include "pilot.h"

/* DESIGN 4.8: render-side animation values (computed in flight_step, interpolated by render). All angles in
   radians, + = trailing edge UP for every surface. Pull (nose up) = both stabilators trailing edge up; roll right
   (ur > 0) = right flaperon up / left down, right stabilator more up / left less (differential tail). */
typedef struct FlightVisual {
    float stabL, stabR;                             /* stabilator angles (rad, + = trailing edge up) */
    float flapL, flapR;                             /* flaperon angles (rad, + = trailing edge up) */
    float nozzle;                                   /* 0 normal .. 1 AB (follows ab) */
    float vapour;                                   /* 0..1: hard pull at low/medium speed (M3 polish) */
} FlightVisual;

/* DESIGN 4.2 */
typedef struct FlightState {
    Vec3 pos;                                       /* centre of mass (m) */
    Quat qPath;                                     /* flight-path frame (forward = velocity, up = lift) */
    float V;                                        /* true airspeed (m/s) */
    float n, nCmd;                                  /* load factor (g) and its command */
    float p;                                        /* roll rate (rad/s, lagged, + = rolling right) */
    float alpha;                                    /* cosmetic AOA (rad) */
    int thr;                                        /* THR_SLOW / NORMAL / FAST */
    float ab, brake;                                /* afterburner 0..1, speed brake 0..1 (lagged) */
    int onGround; float gearPos;                    /* takeoff phases (6) */
    float peakG;                                    /* max n since the last flight_reset_peak */
    float heading, pitch, bank, gamma, mach;        /* derived once per tick (rad, Mach). pitch = NOSE pitch */
    FlightVisual vis;
    /* ---- added by T05: derived once per tick, for HUD, F10 overlay, telemetry, AI, launch zones ---- */
    float nLim;                                     /* G limit at this speed (g, incl. turnBoost) */
    float nHold;                                    /* lift that holds the path straight (g) = up.y of qPath */
    float Vt;                                       /* speed target of the throttle state (m/s) */
    float vs;                                       /* vertical speed (m/s, + = up) */
    float turnRate;                                 /* heading change rate (rad/s, + = right), last tick */
    float pitchRate;                                /* path pitch rate about the path right axis (rad/s) */
    int   capped;                                   /* bit 0: pitch-rate cap limited nCmd, bit 1: floor/ceiling
                                                       nose drop active, bit 2: speed at the floor */
} FlightState;

void  flight_params(void);                          /* [flight] */
void  flight_init(FlightState *fs, Vec3 pos, float headingRad, float speedMs);   /* wings level, path level */
void  flight_step(FlightState *fs, const PilotInput *in, float dt);              /* DESIGN 4.3 */
Quat  flight_nose(const FlightState *fs);           /* qNose = qPath * rotX(-alpha) (render) */

/* ---- added by T05 ---- */
/* path from angles (rad): heading (0 = north, clockwise), gamma (+ = climb), bank (+ = right wing down) */
Quat  flight_path_quat(float headingRad, float gammaRad, float bankRad);
void  flight_set_path(FlightState *fs, float headingRad, float gammaRad, float bankRad);   /* + derive; n = nHold */
void  flight_derive(FlightState *fs);               /* recompute heading/pitch/bank/gamma/mach/nHold/nLim/vs */
float flight_g_limit(float V);                      /* G limit (g) at V (m/s): gLimitTable(kt) * turnBoost */
float flight_roll_rate_max(float V);                /* max roll rate (rad/s) at V (m/s) */
float flight_neg_g_limit(void);                     /* negGLimit (g) */
float flight_tau_g(void);                           /* tauG (s): G onset lag (Novice lead compensation) */
float flight_speed_floor(void);                     /* vFloorKt in m/s */
float flight_speed_target(int thr, float h);        /* NORMAL / FAST targets (m/s) at altitude h; SLOW = vMan */
void  flight_reset_peak(FlightState *fs);           /* peakG = n (HUD peak display timed out) */

/* -telemetry CSV for the player (DESIGN 2.7): one row per tick. main/T08 calls open at flight start, row after
   flight_step, close at exit. Columns are in the header line (flight.cpp). Returns 0 if the file can't be made. */
int   flight_telemetry_open(const char *path);
void  flight_telemetry_row(u32 tick, const PilotKeys *k, const PilotInput *in, const FlightState *fs);
void  flight_telemetry_close(void);
int   flight_telemetry_rows(void);                  /* rows written since open */

/* ---- added by T10a: thrust lapse with altitude ([flight] thrustFullM ceilingM thrustLapseExp) ---- */
float flight_thrust_lapse(float h);                 /* 1 up to thrustFullM .. 0 at ceilingM (ISA density^k) */
float flight_ceiling(void);                         /* ceilingM (m): service ceiling, climb rate fades to 0 */

#endif
