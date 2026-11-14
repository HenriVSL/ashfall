/* hud_tgt.h - target symbology in the 3D view + minimap contacts (DESIGN 10.5, 10.7, user HUD decisions 2026-11-13).
   Windows/render side. Owner: T15.

   3D view (cockpit and chase, identical rules):
     contact      small hollow DIAMOND (tgt_diamond, corner down) centred on the contact's projected render
                  position: yellow = unknown (CS_DETECTED), red = hostile; MEMORY = tgt_diamond_dash at 50 % alpha
                  in the colour of CVF_IDENTIFIED, at the extrapolated position. HAFU symbols are minimap-only.
     selected     green sel_tri (points down) above the diamond (CVF_SELECTED, S tap).
     designated   green TD box (tgt_box, tgt_box_dash while the lock is in memory) around the diamond. Cockpit: the
                  TD box lives inside the HUD frame (hud_layout); outside it the box sits on the frame edge on the
                  line from the waterline, flashing 3 Hz, + LOCATOR LINE. Chase: the same at the screen edge (aim
                  cross for the locator). Locator: 60 px line from the waterline / aim cross towards the target
                  (shortest-rotation screen direction, continuous through "behind"), loc_arrow head, degrees off the
                  nose at the tip.
     labels       small green text under the symbol (fixed offset = TD box half size, so it never moves when a
                  designation comes or goes): range "8" (x100 ft, < 1 NM), "9.4" (< 10 NM), "17"; the designated
                  contact adds its altitude in thousands of ft ("12.4  17"). Declutter: designated + selected + the
                  2 nearest others (holders change with hysteresis), max 4 labels. Overlap (10.7.7): a symbol whose
                  centre is < 12 px (x k) from an earlier-drawn one gets its label on its right (hysteresis 15 px).
                  Draw order: others by id (stable), then selected, then designated last (on top).
     off-screen   offscreen_tri arrows 20 px inside the screen edge (stops at the minimap) for contacts within
                  20 NM that are not on screen (designated excluded: it has the locator), nearest 6, HAFU colour.
     near         within 400 m only the TD box is drawn (the jet itself is visible).
     kill         T11's CS_DEAD starts a kill record per aircraft id (own timers, outlive the 2 s DEAD listing): the
                  diamond turns GREY at the kill position for [hud] killDiamondSec (3 s), then fades (tgtFadeSec);
                  "SPLASH" (large, green) above it for 1.5 s; minimap: grey-red X (MAPC_KILL) for killMapSec (15 s).
   Stability (10.7): positions only from the render state (interpolated / extrapolated by radar_view_pos) and the
   3D frame's camera matrices; the symbol centre is rounded once and every part of a contact (diamond, box,
   triangle, label) is placed at integer offsets from it; numbers at 5 Hz with a dead band and the 2-update
   reversal rule; strings rebuilt only on change; every on/off decision (on screen, in the HUD frame, near, label
   side, label holder) has hysteresis; flashing phase from sim time. States/flags arrive latched from radar.cpp.

   Minimap: every contact goes to map_draw_contact (HAFU arch/chevron, dashed in memory, green square designated),
   designated last; with [hud] mapShowAll = 1 the enemies the radar has not detected are drawn too (MAPF_NORADAR:
   dim solid arch); live missiles as dots (own green, hostile red blinking); RWR lock ticks (map_draw_rwr). */
#ifndef HUD_TGT_H
#define HUD_TGT_H

#include "game.h"
#include "camera.h"

void  hudtgt_draw(const Game *g, const Camera *c, float alpha);

/* ---- added by T15 ---- */
struct HudState;

enum { TGT_MAX = 16 };                              /* symbols per frame */
typedef struct TgtSym {                             /* one contact for the symbology (built from ContactView) */
    int id;                                         /* stable id (aircraft id, < 16): per-contact caches */
    int state;                                      /* displayed CS_DETECTED / CS_HOSTILE / CS_MEMORY / CS_DEAD */
    int flags;                                      /* displayed CVF_* */
    float stateAge;                                 /* s since the displayed state changed (SPLASH, fade) */
    Vec3 pos;                                       /* render position (radar_view_pos) */
    float headingRad;                               /* contact's world heading (minimap tick) */
} TgtSym;

void  hudtgt_params(void);                          /* [hud] tgt* keys: call from main's register_params after
                                                       hud_params (the code defaults work without it) */
void  hudtgt_reset(void);                           /* forget per-contact caches (new flight / tests) */
void  hudtgt_draw_list(const struct HudState *hs, const TgtSym *s, int n, int toMap);   /* the core: draws the 3D
                                                       symbology (own hg_begin/hg_end batch) and, if toMap, queues
                                                       the minimap contacts */
int   hudtgt_build(const Game *g, float alpha, TgtSym *out, int maxOut);   /* player's radar_views -> TgtSym */
float hudtgt_ms_last(void);                         /* CPU ms of the last hudtgt_draw_list */
int   hudtgt_test(void);                            /* -hudtest contacts (DESIGN 10.8): exit code = failures */
int   hudtgt_td(int *x, int *y, int *clamped, int *halfPx);   /* TD box of the last hudtgt_draw_list (T16: SHOOT
                                                       above it, DLZ bar beside it): centre px, 1 = clamped (frame /
                                                       screen edge, flashing), half size px; returns 0 = no TD box */
int   hudtgt_td_label(int *r4);                 /* T18: the designated contact's label of the last draw (px: x0 y0
                                                       x1 y1, shadow included); 0 = no label drawn */

#endif
