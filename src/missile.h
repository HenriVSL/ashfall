/* missile.h - missile model + guidance: ONE step function shared by live missiles and DLZ fly-outs (DESIGN 12.2,
   12.3). Portable. Owner: T12. missile_step's signature is fixed by DESIGN 12.2 (orchestrator approval to change);
   the struct contents are T12's (append fields at the end).

   GAME MECHANICS with invented, tunable numbers ([lrm] / [srm], DESIGN 12.7): not real weapon data.

   How a caller flies a missile (weapons.cpp live at dt = 1/120; T13's fly-outs at dt = 1/30, same code):
     missile_launch(&m, type, owner, targetId, pos, dir, V, &aim, flags, losPhase);    once
     every step: fill a TrackView with the TRUTH of the missile's assigned target entity (aircraft or flare):
       valid (exists / alive), pos + vel (end of this step), ab (heat), isFlare, id, datalink (1 = the shooter's
       radar lock on this target holds this step: LRM midcourse gets the true position; 0 = inertial)
     missile_step(&m, mp, &tv, terrain, dt);   then read m.result (MRES_*) and m.flags (MF_MISSED etc.).
   The missile decides itself what it "sees": datalink vs inertial extrapolation in midcourse, own seeker (gimbal,
   seeker range, terrain LOS checked every losSec = 0.1 s, staggered by losPhase) in ACTIVE/TERMINAL, memory flight
   to the last known point for memSec after the seeker has been lost for lostSec, then unguided. So the fly-out of
   the launch-zone solver and a live shot behave identically as long as they get the same TrackView.

   Guidance law (one law for every phase that steers): pure PN on the missile's aim estimate
     omega = (r x vr) / (r.r), a = navN * (omega x vm) + gravity compensation (+ loft in LRM midcourse),
     |a| <= gMax g min(1, (V/gRefSpeed)^2).
   Deviation from DESIGN 12.2 (T12): + apnK x navN/2 x (target acceleration across the LOS), the acceleration estimated
   from consecutive observed velocities (datalink or seeker; 0 in memory/inertial). Pure PN at its G limit let the
   end-game miss distance grow past fuzeM against a breaking target, so Rne depended on a few metres of miss
   distance instead of energy; with the term the no-escape zone is decided by kinematics. apnK = 0 = pure PN.
   Phases (DESIGN 12.3): LRM  LAUNCH (straight, launchSec) -> MIDCOURSE (datalink / inertial; maddog: straight for
   maddogSec) -> ACTIVE (estimated distance < activeM: seeker searches acqConeDeg around the aim line, or around the
   nose when maddog) -> TERMINAL (seeker tracking, fire and forget) [-> LOST (memory) -> TERMINAL again or unguided].
   SRM  LAUNCH -> TERMINAL when launched with the seeker lock, else ACTIVE = search searchConeDeg for searchSec ->
   unguided. Acquisition range of the heat seeker by aspect: lockHeadM (target nose-on) .. lockTailM (from behind),
   x abLockMul in afterburner (missile_heat_range).
   Fuze: (not once the missile has given up: MF_UNGUIDED missiles only hit the ground or expire, so a lost missile
   never scores a lucky hit and the launch zone stays honest) after armSec, closest approach of this step's relative segment to the TrackView aircraft < fuzeM -> HIT.
   (Other aircraft near a live missile are fuzed by weapons.cpp; a fly-out has only the target.) A missile homing on
   a flare that passes within fuzeM of it detonates on the flare (MRES_FLARE).
   Ground: terrain_segment_hit(prev, pos) (mesh or sea; t == NULL = sea plane only) -> MRES_GROUND at that point.
   Dead: age > maxSec or V < minSpeed after the boost -> MRES_EXPIRED.
   Miss: MF_MISSED once the distance to the (aircraft) target has been opening for 0.5 s after arming (the missile
   flies on; fly-outs stop there). Deterministic: float only, no RNG, no statics. */
#ifndef MISSILE_H
#define MISSILE_H

#include "game.h"

struct Terrain;

enum { MSL_LRM, MSL_SRM };
/* T21a: missile PARAMETER SETS ("kinds"). The missile type (MSL_*) says how a missile behaves (radar LRM / heat SRM);
   the kind says which numbers it flies with. User 2026-11-14: the player's radar missile flies ~2x as far, enemy
   missiles unchanged -> the player's LRM has its own set [lrm_player]. MSLK_LRM / MSLK_SRM are the old sets ([lrm]
   [srm], equal to MSL_LRM / MSL_SRM), so every caller that passes a type still gets exactly the old numbers.
   Rule (weapons, dlz, ai): an aircraft with AC_PLAYER fires MSLK_LRM_PLAYER; everyone else MSLK_LRM; SRM is shared. */
enum { MSLK_LRM = MSL_LRM, MSLK_SRM = MSL_SRM, MSLK_LRM_PLAYER, MSLK_COUNT };
enum { MPH_LAUNCH, MPH_MIDCOURSE, MPH_ACTIVE, MPH_TERMINAL, MPH_LOST, MPH_DEAD };

typedef struct MissileParams {
    int type;
    float dropSec, boostSec, boostAcc, dragK, inducedK, gMax, gRefSpeed, navN;
    /* ---- T12 (DESIGN 12.7 table + the phase knobs of 12.2/12.3) ---- */
    float gimbalDeg;                                /* seeker gimbal (angle nose -> target) */
    float acqConeDeg;                               /* LRM: active search cone; SRM: lock circle at the boresight */
    float activeM;                                  /* LRM: goes ACTIVE when the estimated distance < this */
    float seekM;                                    /* LRM: seeker acquisition range */
    float lockTailM, lockHeadM;                     /* SRM: heat lock range from behind / nose-on */
    float abLockMul;                                /* SRM: lock range x this when the target is in afterburner */
    float trackMul;                                 /* tracking range = acquisition range x this */
    float armSec, fuzeM, maxSec, minSpeed;
    float damageEnemy, damagePlayer;                /* carried in hit events (mission decides, DESIGN 14.1) */
    float launchSec;                                /* LAUNCH phase (straight, no guidance) */
    float maddogSec;                                /* LRM without TD: straight this long, then ACTIVE */
    float searchSec, searchConeDeg;                 /* SRM without lock: search window + cone after launch */
    float lostSec, memSec;                          /* seeker lost after lostSec; memory flight memSec */
    float losSec;                                   /* terrain LOS check period of the seeker */
    float loftG;                                    /* LRM midcourse: extra up acceleration (g) while far */
    float loftRangeM;                               /* ... while the estimated distance > this */
    int   densityAlt;                               /* 1: rho = exp(-y / 8500) */
    float apnK;                                     /* augmented PN: + apnK x navN/2 x target accel (0 = pure PN) */
} MissileParams;

/* what the guidance is given each step: the truth of the assigned target entity (see header) */
typedef struct TrackView {
    int valid; Vec3 pos, vel; int isFlare;
    /* ---- T12 ---- */
    int datalink;                                   /* 1 = shooter's lock holds (LRM midcourse uses pos/vel) */
    float ab;                                       /* target afterburner 0..1 (heat seeker range) */
    int id;                                         /* aircraft id or flare index (bookkeeping only) */
} TrackView;

enum {                                              /* Missile.flags */
    MF_MADDOG   = 1,                                /* LRM launched without a TD */
    MF_GUIDED   = 2,                                /* launched with lock/TD (SRM: seeker lock) */
    MF_UNGUIDED = 4,                                /* gave up: flies straight until it dies */
    MF_DECOYED  = 8,                                /* homing on a flare (never reacquires) */
    MF_SEEN     = 16,                               /* seeker tracks the target this step */
    MF_INERTIAL = 32,                               /* midcourse without datalink this step */
    MF_MISSED   = 64,                               /* opening from the target for 0.5 s after arming */
    MF_LOSCHECK = 128,                              /* a seeker LOS check happened this step */
    MF_ACQUIRED = 256,                              /* acquired a target this step (event) */
    MF_ARMED    = 512
};
enum { MRES_NONE, MRES_HIT, MRES_GROUND, MRES_EXPIRED, MRES_FLARE, MRES_SELFDESTRUCT };

typedef struct Missile {
    int type, phase, flags, owner, target; Vec3 pos, dir; float V, age;
    /* ---- T12 ---- */
    int result;                                     /* MRES_* (phase = MPH_DEAD once != NONE) */
    Vec3 prevPos;                                   /* position at the start of the last step */
    Vec3 aimPos, aimVel;                            /* the missile's estimate of its target (datalink/seeker/memory) */
    float lostT, memT;                              /* seeker out of gimbal/LOS time; memory flight time */
    float losTimer; int losOk;                      /* next seeker LOS check; result of the last one */
    float aLat;                                     /* |lateral acceleration| of the last step (m/s^2) */
    float aLimit;                                   /* the limit it was clamped to (m/s^2) */
    float openT, lastDist, minDist;                 /* miss detection vs the true target */
    Vec3 endPos;                                    /* hit / impact / expiry point */
    int rail;                                       /* launch rail (visual, weapons.cpp) */
    float rollAge;                                  /* age at the last flare decoy roll (-1 = never) */
    Vec3 aimAcc;                                    /* target acceleration estimate (APN), 0 when not seen */
    int aimFresh;                                   /* aimVel came from a real observation last step */
    float rhoY, rho;                                /* cached air density (refreshed every 20 m of altitude) */
    /* ---- T21a ---- */
    int kind;                                       /* MSLK_* parameter set (missile_launch: from its 'type' argument) */
} Missile;

void  missile_params(void);                         /* [lrm] [srm] [lrm_player] */
const MissileParams *missile_params_of(int type);   /* type MSL_* or kind MSLK_* (MSLK_LRM_PLAYER -> [lrm_player]) */
void  missile_step(Missile *m, const MissileParams *mp, const TrackView *tgt, const struct Terrain *t, float dt);

/* ---- T12 additions ---- */
/* launch: aim = the target known at launch (may be NULL or invalid for maddog / unguided search); flags = MF_MADDOG
   (LRM, no TD) or MF_GUIDED (LRM with TD - lock or memory; SRM with seeker lock) or 0 (SRM search). losPhase in
   [0,1): fraction of losSec before the first LOS check (stagger; fly-outs pass 0).
   T21a: 'type' may also be a kind (MSLK_*): m->type = its missile type, m->kind = the kind (the caller still passes
   missile_params_of(m->kind) to missile_step). */
void  missile_launch(Missile *m, int type, int owner, int target, Vec3 pos, Vec3 dir, float V, const TrackView *aim,
                     int flags, float losPhase);
/* heat seeker acquisition range (m) of tgt seen from 'from' (SRM aspect rule; LRM = seekM) */
float missile_heat_range(const MissileParams *mp, const TrackView *tgt, Vec3 from);
/* acquisition test: tgt within coneDeg of 'axis' and within the gimbal of m->dir, inside the acquisition range,
   terrain LOS clear. On success the missile switches to TERMINAL on that target (m->target = tgt->id). Used by
   missile_step for its own target and by weapons.cpp for other candidates (maddog, SRM search, ACTIVE search). */
int   missile_try_acquire(Missile *m, const MissileParams *mp, const TrackView *tgt, const struct Terrain *t);
int   missile_searching(const Missile *m);          /* 1 = seeker is looking for a target this step */
/* HUD estimates from the missile's own aim: act = seconds until ACTIVE (LRM before ACTIVE, else -1), ttg = seconds
   to impact (-1 = unknown / unguided) */
void  missile_estimates(const Missile *m, const MissileParams *mp, float *act, float *ttg);
const char *missile_phase_name(int phase);
const char *missile_result_name(int res);
/* segment vs terrain with a cheap reject (above every HMAX block the segment can touch); t == NULL = sea only */
int   missile_ground_hit(const struct Terrain *t, const Vec3 *a, const Vec3 *b, Vec3 *hit);

/* ---- T21a additions ---- */
int   missile_kind(int type, int player);           /* LRM + player -> MSLK_LRM_PLAYER, else the type itself */
int   missile_kind_type(int kind);                  /* MSLK_LRM_PLAYER -> MSL_LRM, else kind */
const char *missile_kind_name(int kind);            /* "lrm" / "srm" / "lrm_player" */

#endif
