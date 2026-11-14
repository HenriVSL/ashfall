/* fx.h - afterburner flame + nozzle glow (T08), trails, explosions, flares, smoke (T17), polish (T22) (DESIGN 9.5).
   Windows/render side. Owner: T08 / T17 / T22 by section (fx.cpp has one section per owner). */
#ifndef FX_H
#define FX_H

#include "game.h"
#include "camera.h"

void  fx_params(void);                             /* [fx] (T08 flame/glow keys) */
int   fx_init(void);
void  fx_free(void);
void  fx_tick(const Game *g);                       /* once per sim tick (spawns from events) */
void  fx_draw(const Game *g, const Camera *c, float alpha);

/* ---- T10b: speed cues ---- */
void  fx_cue_params(void);                          /* [fx] speedCues + streak keys (fx_params calls it) */
int   fx_speed_cues(void);                          /* master switch: camera.cpp / snd.cpp read it */
int   fx_streaks_drawn(void);                       /* last frame */

/* ---- T17: combat effects (fx.cpp T17 section) ---- */
void  fx_reset(void);                               /* new session: clear effects, event read indices to "now" */
void  fx_combat_tick(const Game *g);                /* fx_tick calls it */
void  fx_combat_draw(const Game *g, const Camera *c, float alpha);   /* fx_draw calls it */
int   fx_combat_stats(int *trails, int *explosions, int *puffs);     /* last frame; returns quads */

/* ---- T22: polish (fx.cpp T22 section; [fx] vapour*, contrail*, expl*, column*, cloudHaze, flameDiamonds ...) ---- */
float fx_cam_shake(void);                           /* camera shake (deg) from near explosions / hits on the own jet, decaying */

#endif
