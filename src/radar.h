/* radar.h - detection, identification, tracks, memory, designation + lock (STT), RWR (DESIGN 11.1-11.3, 11.5).
   Portable. Owner: T11.

   Every aircraft has the same radar; contacts are kept per OBSERVER per aircraft (s_c[obs][id], Aircraft.id
   indices; aircraft of the observer's own side are never contacts). All numbers are game numbers in [radar].

   Scan (10 Hz per pair, staggered: pair (obs, id) is scanned on ticks where (tick + obs*5 + id*7) % 12 == 0, so
   the player's 5 contacts and the 5 enemies' player contacts all fall on different ticks):
     body frame of the observer's NOSE (flight_nose: same frame as the HUD waterline)
     in scan volume  b.z > 0, |b.x| <= tan(scanAzDeg) b.z, |b.y| <= sin(scanElDeg) R
     in range        R <= Rdet = detRangeNm (T21a: playerDetRangeNm for an AC_PLAYER observer) * (1 + aspectGain * sin^2(aspect)), aspect = angle between the
                     target's velocity and the line of sight (head-on / tail = base range, beam = +25 %)
     lock candidate  only for the designated contact: angle off the nose <= lockGimbalDeg and R <= lockRangeMul*Rdet
     LOS             terrain_los(own, target), only if the cheap tests passed (no terrain = always clear)
     seen = (in volume && in range || lock candidate) && LOS    (a held lock keeps the track outside the volume)
   Contact states (DESIGN 11.2; "sim" state, changes only at scans except DEAD):
     NONE -> DETECTED on a seen scan (unknown, yellow)
     DETECTED -> HOSTILE after idSec of track within idRangeNm (T21a: playerIdRangeNm for the player) (timer from the first seen scan inside idRange;
                 broken by leaving idRange or by MEMORY), instantly on a seen scan within idInstantNm
     DETECTED/HOSTILE -> MEMORY when not seen for lostSec (0.5 s; the radar coasts the track until then, so the
                 MEMORY anchor is the true position/velocity at that moment: no symbol jump)
     MEMORY -> previous state (DETECTED or HOSTILE: ID is kept) when seen again; -> NONE after memorySec (6 s;
                 the ID is forgotten)
     any -> DEAD the tick after the aircraft loses AC_ALIVE (crash or kill; anchor = its position, velocity 0);
                 DEAD -> NONE after splashSec (SPLASH display); designations on it are cleared at once
     observer not alive: all its contacts NONE, designation cleared.
   Lock (DESIGN 11.3), per observer, for its designated contact, checked at that contact's scan:
     radar_designate(): immediate check: lock candidate && LOS -> LOCK_TRACK, else LOCK_MEMORY at once
     LOCK_TRACK while the lock test passes; not passed for lockLostSec (1 s since the last good scan) -> LOCK_MEMORY
     (TD dashed, missiles lose the datalink and go inertial); good again -> LOCK_TRACK; lockMemorySec (3 s) in
     LOCK_MEMORY -> designation dropped (REV_UNDESIGNATE arg 1).
   Display (DESIGN 10.7.4): ContactView.state/flags are LATCHED copies of the sim values: a change is applied only
   when the displayed value is >= latchSec (0.3 s) old. Exceptions: appearance (NONE ->) and DEAD apply at once;
   flag changes caused by the player's S presses apply at once (target.cpp calls radar_view_kick). AI and weapons
   read the sim values (radar_contact_state, radar_track, ContactView.simState).

   Time convention: positions are the FlightState of the last tick. Memory extrapolation: radar_track() to
   g->tick (correct inside a tick after flight_step: weapons, step 7; AI at step 3 gets 1 tick ahead, harmless);
   radar_view_pos() to the render time (g->tick - 2 + alpha after a session tick: exactly the time of
   lerp(prev.pos, fs.pos, alpha), so a symbol does not jump when its contact goes to MEMORY).

   Events: ring of RADAR_EVENTS_MAX with a running sequence (same pattern as world.h), several readers:
       static u32 s_seen; RadarEvent ev;
       while (s_seen < radar_event_seq()) { if (radar_event_get(s_seen, &ev)) handle(&ev); s_seen++; }
   Sound/HUD use REV_LOCK (lock tone), REV_DESIGNATE/UNDESIGNATE, REV_IDENT, REV_DEAD; RWR = events with
   id == PLAYER_ID (REV_LOCK / REV_LOCK_MEMORY / REV_UNDESIGNATE by an enemy, REV_LAUNCH). */
#ifndef RADAR_H
#define RADAR_H

#include "game.h"

void  radar_params(void);                           /* [radar] */
void  radar_reset(Game *g);
void  radar_tick(Game *g);                          /* tick step 8 (10 Hz staggered inside) */

/* ---- added by T11 ---- */
enum { CS_NONE = 0, CS_DETECTED, CS_HOSTILE, CS_MEMORY, CS_DEAD };          /* contact states (11.2) */
enum { LOCK_NONE = 0, LOCK_TRACK, LOCK_MEMORY };                           /* lock of the designation (11.3) */
enum { CVF_SELECTED = 1, CVF_DESIGNATED = 2, CVF_LOCKED = 4, CVF_LOCKMEM = 8, CVF_IDENTIFIED = 16 };

/* read-only contact list entry for HUD (T15), minimap, AI (T14), weapons (T12). Built on demand. */
typedef struct ContactView {
    int id;                                         /* aircraft id (Game.ac index) */
    int state;                                      /* displayed (latched) CS_DETECTED / HOSTILE / MEMORY / DEAD */
    int prevState;                                  /* displayed state before the last change (CS_NONE at first) */
    int flags;                                      /* displayed (latched) CVF_*: SELECTED (player's S tap),
                                                       DESIGNATED, LOCKED (LOCK_TRACK) or LOCKMEM (LOCK_MEMORY: TD
                                                       dashed), IDENTIFIED (= hostile, also kept in MEMORY) */
    int simState;                                   /* CS_* right now (not latched): AI / logic */
    float stateAge;                                 /* s since the displayed state changed (SPLASH timer) */
    float range;                                    /* m from the observer (tracked: true position, MEMORY:
                                                       extrapolated, DEAD: anchor), at the last tick */
    float offNose;                                  /* rad, angle between the observer's nose and the LOS */
    float bearing;                                  /* rad, world bearing observer -> contact (0 = north, cw) */
    float closure;                                  /* m/s, + = closing (MEMORY: from the extrapolated track) */
    float altitude;                                 /* m MSL of the contact (same source as range) */
    float heading;                                  /* rad, contact's world heading (MEMORY: of memVel) */
    Vec3 memPos, memVel; u32 memTick;               /* MEMORY / DEAD anchor (fs time of tick memTick) */
} ContactView;

int   radar_views(const Game *g, int obs, ContactView *out, int maxOut);   /* every contact != NONE (displayed),
                                                       by aircraft id; returns the count */
int   radar_view_get(const Game *g, int obs, int id, ContactView *out);   /* 0 = displayed state is NONE */
int   radar_view_pos(const Game *g, const ContactView *v, float alpha, Vec3 *pos);   /* render position: tracked =
                                                       lerp(prev.pos, fs.pos, alpha), MEMORY = extrapolated, DEAD =
                                                       anchor; returns the displayed state */

/* sim queries (not latched) */
int   radar_contact_state(int obs, int id);         /* CS_* */
int   radar_identified(int obs, int id);            /* 1 = hostile (ID done; kept in MEMORY) */
int   radar_track(const Game *g, int obs, int id, Vec3 *pos, Vec3 *vel);   /* 0 = no contact / DEAD, 1 = tracked
                                                       (true pos/vel), 2 = MEMORY (extrapolated to g->tick) */

/* designation + lock (per observer). The player's designation is made by target.cpp from S long; the AI (T14)
   calls radar_designate itself. Weapons (T12): guided LRM launch needs radar_lock(shooter) == LOCK_TRACK on the
   missile's target; datalink while radar_designated(shooter) == target && radar_lock(shooter) == LOCK_TRACK,
   otherwise inertial (12.3, 12.6). */
int   radar_designate(Game *g, int obs, int id);    /* id = -1 clears; immediate lock check; returns LOCK_* */
int   radar_designated(int obs);                    /* aircraft id or -1 */
int   radar_lock(int obs);                          /* LOCK_* of the observer's designation */
u32   radar_locked_by(int id);                      /* bit obs set = obs has LOCK_TRACK on aircraft id (RWR) */
void  radar_set_selected(int obs, int id);          /* display flag CVF_SELECTED (target.cpp, player only) */
void  radar_view_kick(int obs);                     /* next display update applies flag changes at once (S keys) */

/* geometry of one pair right now (cheap tests + LOS); AI "can I see / lock him", tests */
typedef struct RadarGeom {
    float range, rdet;                              /* m: distance, detection range for this aspect */
    float az, el, offNose;                          /* rad: body-frame azimuth (+ right), elevation (+ up), angle
                                                       off the nose */
    int inVolume, inRange, inGimbal, inLockRange;   /* 0/1 */
    int los;                                        /* terrain_los (computed only when doLos) */
} RadarGeom;
void  radar_geom(const Game *g, int obs, int id, int doLos, RadarGeom *out);

/* events */
enum { REV_DETECT = 1, REV_IDENT, REV_MEMORY, REV_REACQUIRE, REV_DROP, REV_DEAD, REV_DESIGNATE, REV_UNDESIGNATE,
       REV_LOCK, REV_LOCK_MEMORY, REV_LAUNCH };
enum { RADAR_EVENTS_MAX = 64 };
typedef struct RadarEvent {
    int type;                                       /* REV_* */
    int obs, id;                                    /* observer (shooter for REV_LAUNCH), contact (target) */
    int arg;                                        /* REV_UNDESIGNATE: 0 user/AI, 1 lock memory timeout, 2 target
                                                       dead, 3 observer dead; REV_LAUNCH: missile type (0 LRM, 1 SRM) */
    u32 tick;
} RadarEvent;
u32   radar_event_seq(void);
int   radar_event_get(u32 seq, RadarEvent *ev);     /* 0 = not pushed yet or overwritten */
const char *radar_event_name(int type);
const char *radar_state_name(int cs);               /* "NONE" "DETECTED" "HOSTILE" "MEMORY" "DEAD" */

/* RWR (DESIGN 11.5), for any aircraft id (the HUD asks for PLAYER_ID) */
enum { RWR_MAX = 8 };
typedef struct RwrView {
    int nLocks;                                     /* enemies with LOCK_TRACK on this aircraft */
    int lockId[RWR_MAX];
    float lockBearing[RWR_MAX];                     /* world bearing aircraft -> locker (rad, 0 = north, cw) */
    float lockRange[RWR_MAX];                       /* m */
} RwrView;
void  radar_rwr(const Game *g, int id, RwrView *out);
void  radar_rwr_launch(Game *g, int shooter, int target, int mslType);   /* weapons (T12) at every launch at an
                                                       aircraft: pushes REV_LAUNCH (arg = type 0 LRM / 1 SRM) */
int   radar_rwr_missile_warn(int mslType, float launchDistM, float distM);   /* 11.5 rule for one missile homing on
                                                       the player: LRM always 1; SRM 1 when launched inside or now
                                                       within rwrSrmWarnM (5 km) */

/* stats for tests / F10 */
typedef struct RadarStats { u32 scans, losCalls, ticks; } RadarStats;
const RadarStats *radar_stats(void);

#endif
