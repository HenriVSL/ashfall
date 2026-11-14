/* target.h - the player's target selection/designation from S short/long (DESIGN 11.4, 5.3). Portable.
   Owner: T11. Lock state and the designation itself live in radar.* (radar_designate / radar_designated /
   radar_lock), because the lock test shares the radar's scan and LOS; this module turns the player's S presses
   (PilotInput.tgtShort / tgtLong, classified by pilot_update) into selection and designation.

   S short: no selection, or the last S short was >= cycleSec (2 s) ago: build the candidate list (contacts in
            DETECTED / HOSTILE / MEMORY within selectRangeNm 40 NM, sorted by angle off the nose, ties by id) and
            select the first entry, or the entry after the current selection when it is in the list. Within
            cycleSec of the previous S short the list stays FROZEN and each tap walks to the next still-valid
            entry (wrapping), so repeated taps visit every contact once even while angles change.
   S long:  no selection -> select the contact nearest the nose; then designate the selection (radar_designate:
            lock tone event REV_LOCK). S long when the selection IS the designated contact = break lock
            (undesignate, the selection stays).
   Designated contact dead: the radar clears the designation; with autoReselect (1) the selection moves to the
            contact nearest the nose; autoRedesignate (default 0; 1 = re-lock it automatically, Ace Combat style).
   Selected contact DEAD (not designated): reselect the same way (autoReselect); selected contact gone (NONE):
            selection cleared. Player not alive: selection, cycle and designation cleared.
   Firing never requires HOSTILE (no ROE). Keys live in [radar] (selectRangeNm cycleSec autoReselect
   autoRedesignate). */
#ifndef TARGET_H
#define TARGET_H

#include "game.h"

void  target_params(void);                          /* [target] (or inside [radar], T11's choice) */
void  target_reset(Game *g);
void  target_update(Game *g, const PilotInput *in); /* tick step 2: S events of the player */

/* ---- added by T11 ---- */
int   target_selected(void);                        /* player's selected aircraft id, -1 = none */
int   target_designated(void);                      /* = radar_designated(PLAYER_ID) */
int   target_cycle(int *ids, int maxIds);           /* the frozen cycle list (tests, F10); returns its length */
int   target_nearest_nose(const Game *g, int obs, int exclude);   /* candidate nearest the nose (S rules), -1 */

#endif
