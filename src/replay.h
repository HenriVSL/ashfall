/* replay.h - input record/playback, param snapshot, periodic state hashes (DESIGN 2.5, 16). Portable.
   Owner: T01 (skeleton), T08 (complete: key records, session commands, main.cpp wiring, last.rpl, tests).

   What is recorded (T08): the RAW controls of every tick (PilotKeys + the session command, game.h), not the shaped
   PilotInput. The session re-runs pilot_update_fs on playback, so Novice, the takeoff override (which reads the
   shaped key axes) and every pause-menu restart replay exactly, and key records compress far better than ramping
   axes. Recording quantises the joystick axes (s16): the sim must fly with replay_quantise_keys'd keys.

   File (little-endian, streamed; flushed at every hash so a crash still leaves a playable prefix):
     header: "JRPL", u32 version (2), u32 exeCrc, s32 mode, diff, start, u32 seed, u32 hashEvery, u32 paramsHash,
             u32 snapBytes, then snapBytes of params_snapshot() text (all params at the start)
     records: 'K' + 12 bytes packed keys            controls of the next tick
              'R' + u16 n                            the previous controls repeat for n more ticks
              'H' + u32 tick + u32 hash              replay_state_hash BEFORE tick `tick`
              'P' + u32 tick + u32 n + n bytes       params snapshot after a live change (F5 reload, F6 mode toggle,
                                                     script param) at this tick; playback applies it with
                                                     params_restore before that tick
              'E' + u32 ticks                        end
   Key packing: u16 bits (1 up, 2 down, 4 left, 8 right, 16 fast, 32 slow, 64 fire, 128 flare, 256 wpnNext,
   512 view, 1024 look, 2048 joyValid; bits 12-15 = session command SES_CMD_*), u16 tgtHeld, u16 tgtReleased
   (saturated), s16 joyX, s16 joyY (x32767), u16 0.
   Per tick, BEFORE session_tick: recording: replay_quantise_keys(&k); replay_record_tick(g->tick, &k, cmd, g);
   playback: if (!replay_play_tick(g->tick, &k, &cmd, g)) -> replay over. exeCrc 0 = don't care (simh). */
#ifndef REPLAY_H
#define REPLAY_H

#include "game.h"

enum { REPLAY_HASH_EVERY = 120 };                   /* DESIGN 2.6 step 10: one hash per sim second */

typedef struct ReplaySetup { int mode, diff, start; u32 seed; } ReplaySetup;
typedef struct ReplayStats { u32 ticks, hashes, diffs, firstDiffTick, bytes, paramRecords, snapBytes; } ReplayStats;

void  replay_quantise_keys(PilotKeys *k);           /* in place; idempotent */
int   replay_record_open(const char *path, const ReplaySetup *rs, u32 exeCrc);
void  replay_record_tick(u32 tick, const PilotKeys *k, int cmd, const Game *g);
void  replay_record_params(u32 tick);               /* after a live param change during a recorded flight */
void  replay_record_close(const Game *g);           /* g may be NULL (no final hash) */
int   replay_play_open(const char *path, ReplaySetup *rs, u32 exeCrc);   /* applies the param snapshot; 0 = bad */
int   replay_play_tick(u32 tick, PilotKeys *k, int *cmd, const Game *g); /* 0 = end; logs hash: ok|DIFF */
void  replay_play_close(void);                      /* logs "replay: done ..." */
u32   replay_state_hash(const Game *g);
void  replay_stats(ReplayStats *st);
int   replay_active(void);                          /* 1 recording, 2 playing, 0 none */

int   test_replay(const char *arg);                 /* -test replay: format round trip + a real 60 s session */
int   test_determinism(const char *arg);            /* -test determinism: two sessions, same hashes */

#endif
