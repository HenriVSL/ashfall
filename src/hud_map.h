/* hud_map.h - minimap: frame, island image, own jet, scan wedge, range, contacts (DESIGN 10.4). Windows/render side.
   Owner: T07.

   Bottom-left square (virtual 6,338 136x136), dark translucent background, heading-up, own jet at the centre, range
   10 / 20 / 40 NM = half the square ([hud] mapRange, map_cycle_range for Tab), island image (islandc MAPI rect)
   rotated with the heading, runway mark, area boundary circle, +-60 deg radar scan wedge, frame (map_frame 9-slice),
   range text top-left, flare count top-right.

   Contacts (for T15): map_draw_contact() QUEUES a contact for this frame (main draws the target symbology before
   the minimap); map_draw draws the queue on top of the map and clears it. HAFU mini symbols: unknown = yellow arch,
   hostile = red chevron, + a heading tick; MAPF_MEMORY = dashed, half alpha; MAPF_DESIGNATED = green square around;
   missiles = dots (own green, hostile red blinking 4 Hz). map_draw_rwr() queues a red tick on the frame at a bearing.
   Until T15 takes over, map_draw adds every live enemy of the Game as a hostile contact when [hud] mapShowAll = 1
   (user 2026-11-13: show all enemies for now); T15 calls map_set_auto_contacts(0) to switch that off. */
#ifndef HUD_MAP_H
#define HUD_MAP_H

#include "game.h"
#include "hud.h"

enum { MAPC_UNKNOWN, MAPC_HOSTILE, MAPC_MSL_OWN, MAPC_MSL_HOSTILE };
enum { MAPF_MEMORY = 1, MAPF_DESIGNATED = 2, MAPF_SELECTED = 4 };
typedef struct MapContact {
    float x, z;                                     /* world position (m) */
    float headingRad;                               /* world heading (0 = north, clockwise) */
    int kind;                                       /* MAPC_* */
    int flags;                                      /* MAPF_* */
} MapContact;

void  map_params(void);                             /* [hud] map* keys (called by hud_params) */
int   map_init(void);
void  map_free(void);
void  map_draw(const Game *g, float alpha);         /* fills a HudState for player 0 and calls map_draw_state */

/* ---- added by T07 ---- */
void  map_draw_state(const HudState *hs, const Game *g);   /* g may be NULL (no auto contacts, default runway) */
void  map_draw_contact(const MapContact *c);        /* queue for this frame (max 32) */
void  map_draw_rwr(float bearingRad);               /* queue a red RWR tick at a world bearing (max 8) */
void  map_set_auto_contacts(int on);                /* 1 (default) = map_draw adds the Game's enemies itself */
int   map_show_all(void);                           /* [hud] mapShowAll */
int   map_cycle_range(void);                        /* 10 -> 20 -> 40 -> 10 NM; returns the new range */
int   map_range_nm(void);
void  map_set_range_nm(int nm);
int   map_to_screen(const HudState *hs, float x, float z, float *sx, float *sy);   /* 1 = inside the map */
float map_ms_last(void);                            /* CPU ms of the last map_draw_state */

/* ---- added by T15 (contact drawing) ---- */
enum { MAPC_KILL = 4 };                             /* MapContact.kind: killed enemy, grey-red X (T15 keeps it killMapSec) */
enum { MAPF_NORADAR = 8 };                          /* MapContact.flags: enemy not detected by the radar (mapShowAll):
                                                       drawn solid at 40 % alpha */

#endif
