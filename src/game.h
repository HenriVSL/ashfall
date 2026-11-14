/* game.h - game constants and types shared by all game modules (DESIGN 2.4, 3). Portable.
   Owner: T01; append-only for later tasks (add fields at the END of a struct, new types at the end of this file).
   g_game and g_args are defined in game.cpp (portable, so tools/simh links them too).
   Notes:
   - DESIGN 2.4 calls the sim container "World"; it is named Game here because engine/world/e_world.h already
     defines the type World (the airbase .trk + sky loader).
   - Per-aircraft state of other modules (pilot shaping, takeoff phase, AI brain, radar contacts...) lives in that
     module's own arrays indexed by Aircraft.id (0 = the player), so this header never has to include them.
   - PilotInput (DESIGN 5.1) is defined in pilot.h, FlightState (4.2) in flight.h. */
#ifndef GAME_H
#define GAME_H

#include "e_base.h"
#include "e_math.h"
#include "e_cfg.h"
#include "pilot.h"
#include "flight.h"

struct Terrain;                                     /* e_terrain.h */
struct World;                                       /* e_world.h: airbase .trk + sky */

/* ---- time (DESIGN 2.6, 3) */
enum { SIM_HZ = 120 };
#define SIM_DT (1.0f / 120.0f)

/* ---- units (DESIGN 3): SI inside, these only at the ini/UI edge */
#define KT2MS   0.514444f                           /* knots -> m/s */
#define MS2KT   (1.0f / 0.514444f)
#define FT2M    0.3048f
#define M2FT    (1.0f / 0.3048f)
#define NM2M    1852.0f
#define GRAV    9.81f
float speed_of_sound(float h);                      /* 340.3 - 0.00405 h (h <= 11 km), 295.7 above (game.cpp) */

/* ---- limits */
enum { MAX_AIRCRAFT = 8,                            /* player + up to 5 enemies (+ spare) */
       MAX_MISSILES = 32, MAX_FLARES = 64 };
enum { PLAYER_ID = 0 };

/* ---- ids */
enum { APP_NONE, APP_TITLE, APP_MENU, APP_LOADING, APP_FLIGHT, APP_RESULTS, APP_IDLE, APP_QUIT };
enum { MODE_FREE, MODE_TRAINING, MODE_SORTIE };
enum { DIFF_EASY, DIFF_NORMAL, DIFF_HARD };
enum { START_RUNWAY, START_AIR };
enum { SIDE_BLUE, SIDE_RED };                       /* player side, enemies */
enum { VIEW_COCKPIT, VIEW_CHASE };
enum { BENCH_ROUTE_LOW, BENCH_ROUTE_VOLCANO, BENCH_ROUTE_HIGH, BENCH_ROUTE_CLOUDS, BENCH_ROUTE_COMBAT };
enum { AC_ALIVE = 1, AC_PLAYER = 2, AC_INVULN = 4, AC_ONGROUND = 8 };   /* Aircraft.flags */

typedef struct Aircraft {
    int id, side, flags;                            /* id = index in Game.ac */
    int livery;                                     /* 0 player grey, 1 enemy aggressor */
    FlightState fs;                                 /* state after the last tick */
    FlightState prev;                               /* state at the start of the last tick (render interpolation) */
    PilotInput in;                                  /* input used by the last tick */
    float hp;                                       /* damage model (DESIGN 14.1), 100 = intact */
} Aircraft;

typedef struct Game {
    int mode, diff, start;                          /* MODE_*, DIFF_*, START_* of this session */
    u32 seed; ERand rng;                            /* the only RNG in sim code */
    u32 tick;                                       /* sim ticks since the session started */
    struct Terrain *terrain;                        /* island (NULL until T04's loader works) */
    struct World *base;                             /* airbase .trk via e_world (render side, may be NULL) */
    int numAircraft;
    Aircraft ac[MAX_AIRCRAFT];                      /* ac[0] = the player */
} Game;
extern Game g_game;

/* ---- command line + ini for this run (DESIGN 2.7). args_parse: [video]/[dev] params first (game.cpp registers
   [video]; call args_parse AFTER params_load), then switches.
   -1 in an int field = not given (use the preset / default). Paths are as given (relative = exe dir). */
enum { ARGS_MAX_SHOTS = 8 };
typedef struct Args {
    /* video */
    int fullscreen, width, height, bpp, fpsTarget, wbuffer;
    char preset[16];                                /* "low" | "high" */
    int nocap, fps, nosound, nojoy;
    /* run mode */
    int headless;
    float exitAfter;                                /* sim seconds, 0 = never */
    int logLevel;                                   /* -log N: 0 errors only, 1 info, 2 debug */
    char logFile[E_PATH];                           /* -logfile NAME overrides the automatic name */
    char test[32], testArg[64];                     /* -test NAME [arg] */
    char hudtest[32];                               /* -hudtest NAME */
    char dumpini[E_PATH];                           /* -dumpini FILE */
    int dev;                                        /* -dev or [dev] keys=1: F9/F10 */
    /* session */
    int mode, diff, start;                          /* MODE_*, DIFF_*, START_* */
    u32 seed; int haveSeed;
    /* tools */
    int numShots; float shotTime[ARGS_MAX_SHOTS]; char shotPath[ARGS_MAX_SHOTS][E_PATH];
    int haveCamat; float camat[5];                  /* -camat X Y Z HDG PITCH */
    char script[E_PATH], record[E_PATH], replay[E_PATH], telemetry[E_PATH];
    int bench, benchRoute;                          /* -bench N seconds, -benchroute */
    int aifight;                                    /* -aifight N fights */
    /* ---- added by T08 ---- */
    int benchView;                                  /* -benchview cockpit|chase (VIEW_*; default chase) */
} Args;
extern Args g_args;
void args_parse(Args *a);                           /* needs args_init and params_load first */
void game_params(void);                             /* registers [video] (restart-only keys) */
void args_log(const Args *a);                       /* "boot: args ..." lines + "warn: unknown switch" */
void args_log_not_yet(const Args *a);               /* "args: not yet -x" for every given switch not implemented */
extern const char *const g_argSwitches[];
extern const int g_numArgSwitches;

/* ---- added by T08: the portable session (DESIGN 2.6 tick order) -------------------------------------------------
   One flight session of the player (+ later enemies): main.cpp (live, -replay, -script, -bench), -test determinism /
   replay and simh all fly through these, so a replay of the same PilotKeys reproduces the run bit for bit.
   session_start: clears g, spawns the player (runway: takeoff_park READY; air: world_air_start), world_reset,
   takeoff_bind_pilot (Novice override + handover), resets every module. g->tick starts at 1 (0 = "never" in the
   takeoff stats).
   session_tick (one 120 Hz tick): cmd first (SES_CMD_RUNWAY = pause menu "restart on runway": pilot_reset +
   takeoff_park; SES_CMD_AIR = dev F9 air respawn), then 1 pilot_update_fs (player) 2 target_update 3 ai 4
   takeoff_tick (owns the input while active) + world_override_input (area turn-back) + the bench autopilot hook
   5 flight_step (all alive aircraft; prev = fs first) 6 world_check 7 weapons 8 radar 9 mission, then the crash
   flow: once world_respawn_due, pilot_reset + takeoff_park (user rule: always restart on the runway).
   The cmd is part of the recorded input (replay packs it with the keys), so restarts replay exactly. */
struct PilotState;
struct TakeoffState;
enum { SES_CMD_NONE = 0, SES_CMD_RUNWAY = 1, SES_CMD_AIR = 2 };
typedef void (*SessionApFn)(const Game *g, int id, PilotInput *in);   /* bench autopilot: replaces the input */
void  session_start(Game *g, struct Terrain *t, int mode, int diff, int start, u32 seed);
void  session_tick(Game *g, const PilotKeys *keys, int cmd);
void  session_place_air(Game *g, Vec3 pos, float headingRad, float speedMs);   /* player flying here (bench) */
void  session_set_autopilot(SessionApFn fn);        /* NULL = off */
struct PilotState *session_pilot(int id);
struct TakeoffState *session_takeoff(void);
const PilotInput *session_pilot_out(void);          /* player's pilot_update_fs output of the last tick (view /
                                                       look / fire edges, before the takeoff override) */
int   session_world_bits(void);                     /* world_check bits of the last tick (WEV_*) */
int   session_respawns(void);                       /* respawns + restarts so far (camera/HUD snap on change) */

/* -script FILE (DESIGN 16.2), portable: lines "T keydown|keyup KEY" (UP DOWN LEFT RIGHT A Z S SPACE D F C V),
   "T expect FIELD OP VALUE" (FIELD: phase kt agl_ft alt_ft gear lat_m onground alive pullup gamma pitch bank
   respawns mode; OP == != < <= > >=; phase values READY..DONE, mode EXPERT/NOVICE), "T param sec.key=value" (a live
   param change, recorded like F5), "T restart" (restart on runway), "T air" (air respawn), "T end" (marks the end).
   T in seconds; a line at T applies before tick round(T*120)+1, i.e. after T seconds of sim time. Comments '#'.
   script_step fills keys/cmd for this tick, logs "script: lN ... ok|FAIL" for expects; returns 1 when a param line
   was applied this tick (the caller records a replay PARAMS record). */
int   script_open(const char *path);                /* 0 = file missing / no valid line */
int   script_step(const Game *g, PilotKeys *keys, int *cmd);
int   script_fails(void);
int   script_checks(void);
int   script_done(const Game *g);                   /* every line applied */
void  script_close(void);

/* ---- added by T17 ---- */
void  session_runway(Game *g, const char *why);     /* restart the player on the runway (pilot_reset + takeoff_park,
                                                       respawn counter); mission_tick after a missile death */

#endif
