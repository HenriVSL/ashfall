/* snd_combat.h - combat sounds: lock/growl tones, RWR, missile warning, launches, motors, passes, explosions, hit,
   kill chime (DESIGN 15, data/sfx/README_combat.md = the playback, ducking and loop rules; A9 + A9b).
   Windows side. Owner: T17. Reads game state only (weapons / radar / world events with own read indices, the
   mission counters, hudwpn_tones for the displayed seeker/RWR states).
   Tones (from the HUD's displayed states, so sound and symbols agree): srm_growl (SRM searching with a heat target
   near the seeker; volume/pitch by the angle of the nearest target), srm_lock (SRM seeker lock), rwr_lock (an enemy
   STT lock: "LOCK"), rwr_launch + rwr_missile ("MISSILE": launch onset once, then the loop; a second missile does not
   replay the onset; -4 dB after 2 s, full again when a missile is < 2 km or new; -3 dB under PULL UP), rwr_search
   (the "waterfall": once per enemy radar that newly detects the player, max 1 per s, never during lock/missile),
   lock_beep (own STT lock after S long; not for a re-lock of the same target within 2 s).
   One-shots: msl_launch (own; enemy positional), flare (own bursts), no_msl (Space with the weapon empty), hit (own
   damage), expl_near (own missile death), expl_air (air kills, delayed d / 340 m/s; expl_far beyond 4 km and for
   ground/sea impacts and enemy crashes), kill (0.35 s after the player's kill), msl_pass (hostile miss within
   200 m), jet_pass (enemy closest approach < 600 m within 0.6 s, once per enemy per 8 s).
   Loops: msl_motor x3 (nearest three missiles, doppler, pan). Warning/tone voices are reserved; continuous tones
   stop with 3 frames of falling volume. Group volumes [sound] warn / combat; master and pause follow snd.cpp. */
#ifndef SND_COMBAT_H
#define SND_COMBAT_H

#include "game.h"
#include "camera.h"

int   sndc_init(void);
void  sndc_free(void);
void  sndc_frame(const Game *g, const Camera *c);

/* ---- added by T17 ---- */
void  sndc_params(void);                            /* [sound] warn, combat (call after snd_params) */
void  sndc_reset(void);                             /* new session: event read indices to "now", tones off */
void  sndc_set_paused(int on);                      /* pause menu / not in flight: every combat voice silent */

#endif
