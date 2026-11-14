/* dlz.h - launch zone: LIVE fly-outs of the real missile model (missile_step) + AI tables (DESIGN 12.4). Portable.
   Owner: T13.

   Honest by construction: every number the HUD shows comes from flying T12's missile_step (unchanged, dt 1/30,
   lock assumed held) from our current state against a target model, and terrain is checked every step (a volcano
   between us and the target collapses the zone). User request 2026-11-13: the values are computed LIVE, all the
   time, spread over frames; tables only seed the search, feed the AI's cheap checks and stand in (flagged) while
   the first live answer for a new target is computed.

   Definitions (DESIGN 12.4, HUD.md 2.1):
     Rmax  largest launch range that hits the target if it keeps flying straight at its current velocity.
     Rne   largest range that hits for EVERY escape of E = {break left, break right, beam left, beam right}: a level
           turn at escapeGUse x the flight model's G limit (speed-dependent, induced bleed as flight.cpp) while
           accelerating to FAST (afterburner), from the moment of launch. Break = turn (that way) until flying away
           from the shooter (along the launch-time line of sight), then run; beam = turn until the velocity is
           perpendicular to the line of sight, then hold. Rne <= Rmax (straight flight is an escape too).
     Rmin  smallest range that hits the straight target (arming time + turn ability).
   Fly-out geometry: launched from our position along our NOSE at our speed; the target is placed along the current
   line of sight at range R with its current velocity (keeps aspect and off-boresight). So the values do not depend
   on the current range, only on the geometry (aspect, angles, speeds, altitudes, terrain); the current range is
   then compared with them.

   Live answers per cycle (one geometry snapshot per cycle):
     NOW      one fly-out at the CURRENT range vs the straight target: "a shot now hits" + time of flight + the
              predicted intercept point (lead direction for the steering dot). Early-out once decided.
     NOWESC   one fly-out at the current range vs the escape that defines Rne: "can the target still escape by
              turning now" (only when NOW hits).
     RMAX / RNE / RMIN  bracketing range searches seeded from the previous answer (values change slowly): a fly-out
              that hits estimates how much further the missile could still reach (coast law of 12.2), one that misses
              how far it fell short; the next fly-out aims just past that estimate, so a converged value costs ~2
              fly-outs (one hit, one miss) per cycle. Rne: the worst escape is solved, the other three are checked
              with one fly-out each at that range (a hit there proves they are not worse).
   Misses are cut short once decided (missile can no longer close the gap even coasting: early-out).
   Time-sliced by missile steps: dlz_work(g, budget) per FRAME (DESIGN 2.6). Display only: it reads Game and other
   modules' state, never writes it, uses no RNG (determinism unaffected).

   Published values (DlzView): Rmax = middle of the final bracket (hit .. miss, width <= tolMax; the verified hit when
   the bracket stayed wide), Rne = the largest verified hit of the worst escape (conservative), Rmin = the smallest
   verified hit; a smaller new value is published at once, a larger one smoothed (smoothUp). Flags say per value
   whether it is live or a table estimate. Each cycle's snapshot is predicted ahead by leadFrac x the last cycle time
   (both aircraft keep their turn rates) so the values fit the moment they are shown.
   Guided SRM launch needs the seeker lock (weapons.cpp): a fly-out beyond the aspect lock range or outside the gimbal
   is a miss, so SRM Rmax is usually the lock range (the tone), not the missile's energy limit.
   Escape model detail: the target needs ~0.85 x bank/rollRate + tauG before the turn bites (roll-in, matched against
   flight_step); without it SRM beam escapes beat the zone (a later turn costs the missile more under PN).

   AI tables (DESIGN 12.4.2, extended): per missile kind (T21a: MSLK_LRM, MSLK_SRM, MSLK_LRM_PLAYER), aspect {0,45,90,135,180} x shooter speed {300,500,800 kt} x
   LOS elevation {-7,0,+7 deg} (the "altitude difference" axis: +-3000 m at ~25 km) x target speed {NORMAL, FAST} x
   shooter altitude {1500,4500,8000 m} = 270 nodes per kind, nose on the target, the target escaping at tableGUse (1.0: the
   player at full G), Rmax/Rne/Rmin/TOF from the SAME solver. Shipped precomputed in data/dlz.tab (simh dlztable -out
   data/dlz.tab; 16.2 KB with 3 kinds, loads in ~19 ms on the VM) with a portable key of the params text it was built from; a session
   start (dlz_reset) loads it, or dlz_cache.tab after a params change; if neither matches, the first lookup rebuilds
   synchronously (VM ~13 s, Mac ~0.1 s; only while tuning) and writes dlz_cache.tab. Never rebuilt mid-session
   (deterministic). Lookups interpolate the 32 surrounding nodes; SRM Rmax is capped by the seeker lock range of the
   actual geometry. */
#ifndef DLZ_H
#define DLZ_H

#include "game.h"

struct Terrain;

void  dlz_params(void);                             /* [dlz] */
void  dlz_reset(Game *g);                           /* session start: clears the live solver, builds tables if stale */
void  dlz_work(Game *g, int budgetSteps);           /* per FRAME (display only, deterministic in result) */

/* ---- T13 additions ---- */
enum { DLZ_STRAIGHT, DLZ_BREAK_L, DLZ_BREAK_R, DLZ_BEAM_L, DLZ_BEAM_R, DLZ_ESC_COUNT };
const char *dlz_esc_name(int esc);

/* one geometry snapshot */
typedef struct DlzGeom {
    int type;                                       /* MSL_LRM / MSL_SRM, or a kind MSLK_* (T21a: MSLK_LRM_PLAYER) */
    Vec3 sPos, sNose; float sV;                     /* shooter position, unit nose vector, speed */
    Vec3 tPos, tVel; float tAb;                     /* target position, velocity, afterburner 0..1 (heat range) */
    float gUse;                                     /* escape turn load (x G limit); <= 0 = dlz_escape_guse() */
    const struct Terrain *ter;                      /* NULL = sea plane only */
} DlzGeom;

/* one complete fly-out at range R (tests, tools): target placed along the line of sight at R */
typedef struct DlzFly {
    int hit;                                        /* 1 = MRES_HIT */
    int res;                                        /* MRES_* (MRES_NONE = stopped as a miss: MF_MISSED/early-out) */
    int early;                                      /* 1 = cut short as a sure miss */
    int steps;                                      /* missile_step calls */
    float tof;                                      /* s (hit time, or when it stopped) */
    float minDist, tMin;                            /* closest approach (m) and its time (s) */
    float reach;                                    /* hit: further range the missile could still have flown (m) */
    float shortfall;                                /* miss: how far it fell short (m), -1 = unknown (ground) */
    Vec3 hitPos;                                    /* target position at the hit (intercept point) */
    float lockRange;                                /* SRM launched beyond its seeker lock range: that range, else 0 */
} DlzFly;
int   dlz_flyout(const DlzGeom *g, float R, int esc, DlzFly *out);

/* zone of one geometry (synchronous solve, warm = previous answer or NULL) */
typedef struct DlzZone {
    float rmax, rne, rmin;                          /* m; rmax 0 = no zone (no hit at any range) */
    float rmaxLo, rmaxHi;                           /* final Rmax bracket (verified hit / miss), 0 = unknown */
    float tof;                                      /* s: time of flight at rmax (tables), or at the NOW range */
    int worstEsc;                                   /* escape that defines Rne */
    int flyouts, steps;                             /* work done */
} DlzZone;
int   dlz_solve(const DlzGeom *g, const DlzZone *warm, DlzZone *out);   /* returns 1 if a zone exists */
void  dlz_geom_from(const Game *g, int shooter, int target, int type, DlzGeom *out);   /* truth of both aircraft;
                                                       T21a: type LRM + shooter AC_PLAYER -> kind MSLK_LRM_PLAYER */

/* ---- HUD (T16): the player's designated target, refreshed by dlz_work (T21a: flown with the player's own missile
   kind, i.e. [lrm_player] for the AIM-120; DlzView.type stays the plain MSL_* type) ---- */
enum { DLZV_LIVE = 1, DLZV_TABLE = 2 };             /* per-value source flags */
typedef struct DlzView {
    int valid;                                      /* 1 = designated target with a published zone (live or table) */
    int target, type;                               /* aircraft id, MSL_* the values are for */
    float range;                                    /* current range (m) from the radar track, this frame */
    float closure;                                  /* m/s, + = closing (this frame) */
    float rmax, rne, rmin;                          /* published (smoothed) values, m */
    int srcMax, srcNe, srcMin;                      /* DLZV_LIVE or DLZV_TABLE (estimate until the first live) */
    float raero;                                    /* T21c: max aerodynamic range (zero-closure; LAR GDD Raero) */
    int srcAero;                                    /* DLZV_LIVE or DLZV_TABLE */
    int ready;                                      /* 1 = Rmax is live (SHOOT allowed: 10.6 "first result") */
    int inRmax, inRne, inRmin;                      /* rmin < range <= rmax / range <= rne / range < rmin */
    int nowValid, nowHit;                           /* NOW fly-out: a shot at the current range hits (straight) */
    int nowEscValid, nowNoEscape;                   /* NOWESC fly-out: still hits vs the worst escape */
    int nowEscFlown;                                /* 0 = answer implied (NOW missed, or range > 1.3 x Rne) */
    float tof;                                      /* s, NOW fly-out time of flight (-1 = out of range/unknown) */
    Vec3 leadDir;                                   /* unit world vector: shooter -> predicted intercept point */
    int leadValid;                                  /* 0 = no lead (no target) */
    int worstEsc;                                   /* DLZ_* escape defining Rne */
    u32 cycles;                                     /* completed live cycles for this target */
    float cycleSec;                                 /* sim seconds the last full cycle took (live update period) */
    float ageSec;                                   /* sim seconds since the snapshot of the last published value */
    int memory;                                     /* 1 = target from radar memory (estimate, LOCK_MEMORY) */
    int cycleFlyouts, cycleSteps;                   /* work of the last full cycle */
} DlzView;
const DlzView *dlz_view(void);
int   dlz_budget(int requested);                    /* steps per frame actually used: min(requested, [dlz] preset
                                                       budget: stepsLow / stepsHigh by g_args.preset) */

/* ---- AI (T14): cheap table lookups (deterministic) ---- */
float dlz_escape_guse(void);                        /* current escape load factor fraction */
void  dlz_set_escape_guse(float gUse);              /* T14/T17: the enemy difficulty's gUse (tables follow) */
/* T21a: 'type' = the missile kind MSLK_* (MSLK_LRM enemy LRM, MSLK_SRM, MSLK_LRM_PLAYER the player's LRM); _ac maps
   an LRM of an AC_PLAYER shooter to MSLK_LRM_PLAYER itself */
int   dlz_ai_zone(int type, Vec3 sPos, Vec3 sVel, Vec3 tPos, Vec3 tVel, DlzZone *out);   /* 0 = no table */
int   dlz_ai_zone_ac(const Game *g, int shooter, int target, int type, DlzZone *out);   /* from Game truth */
void  dlz_tables_build(void);                       /* force a rebuild (logs "dlz: table ...") */
int   dlz_tables_ok(void);                          /* 1 = tables built for the current params */
int   dlz_tables_save(const char *path);
void  dlz_tables_reload(void);                      /* drop and ensure again (file first), as at a session start */
int   dlz_tables_loaded(void);                      /* 1 = the current tables came from a file */            /* data/dlz.tab (simh dlztable -out; the game after a rebuild) */
float dlz_tables_ms(void);                          /* last build time (ms), -1 = no clock */
/* raw node (tests/simh dlztable): ia aspect {0,45,90,135,180}, is shooter speed {300,500,800 kt}, id LOS elevation
   {-7,0,+7 deg} (= altitude difference +-3000 m at ~25 km), iv target {NORMAL, FAST}, ih shooter altitude {1500,4500,8000 m} */
int   dlz_table_node(int type, int ia, int is, int id, int iv, int ih, DlzZone *out, DlzGeom *geom);
enum { DLZT_NA = 5, DLZT_NS = 3, DLZT_ND = 3, DLZT_NV = 2, DLZT_NH = 3 };

/* stats for tests/bench */
typedef struct DlzStats { u32 frames, steps, flyouts, cycles; int lastFrameSteps; } DlzStats;
const DlzStats *dlz_stats(void);

#endif
