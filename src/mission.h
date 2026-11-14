/* mission.h - modes, waves, damage, deaths, respawns, score, messages (DESIGN 14). Portable.
   Owner: T17 (M2: TRAINING + 1-wave SORTIE), then T20 (3 waves, records).

   The DIRECTOR (waves, drones, damage, lives, respawn after a missile death, score) runs only when the application
   turns it on with mission_set_director(1) (main.cpp for every interactive / headless / replayed flight, -test
   mission). Off (the default) mission_tick does nothing, so harnesses with their own director (T14's simh aifight /
   -test ai fight: own spawns + 34-damage rule) and the M1 tests keep working unchanged. FREE mode never spawns.

   Modes (DESIGN 14.3):
     FREE      runway start, no enemies, unlimited time (M1).
     TRAINING  trainDrones (3) non-shooting drones on circuits ahead of the runway at different ranges/altitudes
               (ai_spawn_drone); a drone comes back trainRespawnSec (10 s) after it died; the player's missiles and
               flares are topped up (unlimited); no lives, no end.
     SORTIE    runway start; after the takeoff handover "WAVE 1: 2 BANDITS BRG 040"; the wave spawns spawnMinKm..
               spawnMaxKm (45-55 km; waves 2+ spawnLaterMinKm..spawnLaterMaxKm, also 45-55) out around the player's
               heading at spawnAltMinM..spawnAltMaxM (4-6 km) with the attack order (ai_spawn); "WAVE n CLEARED", next
               wave waveDelaySec (8 s) after a wave is cleared; after sortieWaves waves (default 3, T20; M2: 1)
               "MISSION COMPLETE"; out of lives "MISSION FAILED". Wave sizes = DESIGN 13.3 (Easy 2,3,3 /
               Normal 2,3,4 / Hard 3,4,5). Max 5 enemies alive.
   Damage (14.1): weapons queues WPE_HIT (it never touches hp); here: enemy hit = destroyed (hp 0, AC_ALIVE cleared,
     radar shows DEAD/SPLASH next tick); player hit = hp - missileDamage (34: three hits), ignored while AC_INVULN;
     hp <= 100 - damagedAt (50) -> "DAMAGED" (smoke trail, fx); hp <= 0 -> dead. Crashes come from world_check.
   Death and respawn (14.2): wreck camera deathRespawnSec (2.5 s), then session_runway (crash: the session's own
     flow) with full hp/stores (weapons refills on the alive transition); AC_INVULN while the takeoff runs and
     invulnSec (3 s) after the handover. Hostile missiles at the old player self-destruct (weapons, automatic).
     Lives: [mission] lives (3; 0 = unlimited); the death that uses the last life fails the sortie. After the end
     the player still respawns (free flight); main shows the results and offers RESTART MISSION.
   Score (14.4, live; mission_score below): kill 1000, +300 if launched beyond 10 km, -1500 per death, time bonus
     max(0, timeBonusRefSec (750) - s) x 5 on COMPLETE, accuracy hits/fired x 2000, total >= 0. Records: below.
   Messages: a small ring (MIS_MSGS) of HUD texts with their display time; main forwards new ones to hud_message.
   Log: "mission: start/wave/kill/hit/death/respawn/drone/end ..." lines and one "mission: result ..." line. */
#ifndef MISSION_H
#define MISSION_H

#include "game.h"

void  mission_params(void);                         /* [mission] */
void  mission_start(Game *g);                       /* after the player is spawned; g->mode/diff set */
void  mission_tick(Game *g);                        /* tick step 9 */

/* ---- added by T17 ---- */
enum { MIS_RUNNING, MIS_COMPLETE, MIS_FAILED };
typedef struct MissionStats {
    int mode, diff, outcome;                        /* MODE_*, DIFF_*, MIS_* */
    int wave, waves;                                /* current wave (1-based, 0 = before the first), sortie waves
                                                       ([mission] sortieWaves, read live while the sortie runs) */
    int spawned, enemiesAlive;
    int lives, livesLeft;                           /* lives 0 = unlimited (livesLeft then -1) */
    int deaths, deathsMsl, deathsCrash;
    int kills, killsBvr, enemyCrashes, friendlyKills;
    int fired[2], hits[2];                          /* player's launches / hits per MSL_* (whole mission) */
    int playerHits;                                 /* damaging hits taken (sound/fx watch this counter) */
    int damaged;                                    /* 1 = hp <= 100 - damagedAt this life (smoke trail) */
    int invuln;                                     /* 1 = AC_INVULN set by the director */
    float timeSec;                                  /* mission clock (SORTIE: from the first wave spawn) */
    int score;                                      /* computed at the end (and kept live for the HUD) */
    u32 endTick;                                    /* Game.tick of COMPLETE / FAILED, 0 = running */
} MissionStats;

void  mission_set_director(int on);                 /* 1 = waves/drones/damage/lives (main, -test mission) */
int   mission_director(void);
const MissionStats *mission_stats(void);
float mission_clock(const Game *g);                 /* HUD clock seconds: SORTIE only, < 0 = hidden */
void  mission_log_result(const Game *g, const char *why);   /* "mission: result ..." (main at exit if still running) */
const char *mission_outcome_name(int outcome);      /* "running" "complete" "failed" */

enum { MIS_MSGS = 8, MIS_MSG_LEN = 40 };
u32   mission_msg_count(void);                      /* messages queued since mission_start (read index runs up to it) */
const char *mission_msg(u32 index, float *secs);    /* NULL if overwritten / not yet queued */

/* ---- added by T20: score (14.4), results, records.ini ----------------------------------------------------------
   Score: kill 1000, +300 per kill launched beyond bvrKm (10 km), -1500 per death, accuracy hits/fired x 2000 (hits =
   player missiles that killed), time bonus max(0, timeBonusRefSec - s) x 5 only for MISSION COMPLETE (s = mission
   clock from the first wave spawn). timeBonusRefSec = [mission] key (T20: 750 for the 3-wave sortie at 45-55 km;
   DESIGN's 600 was set for 22-28 km waves; the robot needs ~820-1000 s, a good human ~450-600 s). The total never goes below 0. mission_score is pure (unit tests).
   Records: records.ini next to jet.exe (base_path, CRLF), one section per difficulty [easy] [normal] [hard] with the
   best SORTIE score (complete or failed; a quit mission is not recorded) and its date/time and stats. A missing,
   unreadable or corrupt file (or row) just means "no record"; it is rewritten whole on the next new record.
   Menus (T19): records_load() at startup or when the RECORDS screen opens; records_get(diff, &row) per line;
   on the results screen mission_results(&g_game, &res) then records_submit(&res) once (saves on a new record, sets
   res.newRecord; calling it again for the same mission changes nothing). Tests / headless runs never call submit. */
typedef struct ScoreIn {
    int outcome;                                    /* MIS_*: the time bonus only for MIS_COMPLETE */
    int kills, killsBvr, deaths, fired, hits;
    float timeSec, timeRefSec;                      /* mission clock; reference (timeBonusRefSec) */
} ScoreIn;
typedef struct ScoreParts { int kill, bvr, death, time, accuracy, total; } ScoreParts;
int   mission_score(const ScoreIn *in, ScoreParts *out);    /* returns the total (>= 0); out may be NULL */
float mission_time_ref(void);                       /* [mission] timeBonusRefSec */

typedef struct MissionResults {
    int valid;                                      /* 1 = SORTIE that ended (MIS_COMPLETE / MIS_FAILED) */
    int mode, diff, outcome;
    float timeSec;                                  /* mission clock (first wave spawn -> end) */
    int wave, waves;                                /* waves reached / of the sortie */
    int kills, spawned, killsBvr;
    int fired[2], hits[2];                          /* per MSL_LRM (AIM-120) / MSL_SRM (AIM-9) */
    int deaths, hitsTaken;
    ScoreParts parts;                               /* score breakdown for the results screen */
    int score;                                      /* = parts.total */
    int newRecord;                                  /* 1 = beats the stored best of this difficulty (or none stored) */
    int prevBest;                                   /* stored best before this mission, -1 = none */
} MissionResults;
int   mission_results(const Game *g, MissionResults *out);  /* fills out (also while running: valid 0); returns valid */

enum { REC_DATE_LEN = 20 };
typedef struct RecordRow {
    int valid;                                      /* 0 = no record for this difficulty */
    int score, outcome;                             /* outcome MIS_COMPLETE / MIS_FAILED */
    char date[REC_DATE_LEN];                        /* local "YYYY-MM-DD HH:MM" */
    float timeSec;
    int waves, kills, spawned, killsBvr, deaths;
    int fired[2], hits[2];
} RecordRow;
int   records_load(void);                           /* (re)read records.ini; returns valid rows 0..3; never fails */
int   records_get(int diff, RecordRow *out);        /* 1 = a record exists (loads the file on first use) */
int   records_submit(MissionResults *r);            /* 1 = new record (stored + saved); sets r->newRecord/prevBest */
int   records_save(void);                           /* write all rows (CRLF); 1 = ok */
void  records_reset(int diff);                      /* forget one difficulty (-1 = all); caller saves */
void  records_set_path(const char *path);           /* tests; NULL = base_path("records.ini") */
const char *records_diff_name(int diff);            /* "easy" "normal" "hard" (ini section names) */

#endif
