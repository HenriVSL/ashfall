/* scatter.h - island vegetation / rock / village sprites (A11). Windows/render side. Owner: A11.

   Data: data/island/veg.sct (islandc tools/islandc/scatter.py; format in its docstring), atlas data/tex/veg_atlas.bmp
   (keyed, gen_veg.py). Instances are stored per 1280 m grid cell in shuffled order with their base height (= LOD0
   terrain mesh), baked terrain light and, for fixed cards (hedges, walls), a yaw.
   Drawing (scatter_draw, one call per frame after the terrain, inside the scene): cells within `range` of the eye and
   inside the frustum, nearest first; per instance a distance LOD: full density up to nearFrac x range, then thinned
   (a hash per instance against a falling density, so thinned sprites shrink out smoothly instead of popping) down
   to farDensity at the range, and every sprite grows in from zero over the outer fadeFrac of the range. Camera-
   facing cards turn about the vertical and lean back by tiltK x the view elevation (max 35 deg) so they don't turn
   into slivers from above; fixed cards (kind 1) stand along their yaw. One alpha-tested, fogged, double-sided
   material, one texture, r_draw_mem batches of <= 1024 sprites. The total is capped by maxSprites (nearest kept).
   Nothing is drawn when the camera is higher above the cells than the range (high-altitude routes cost ~0).
   Presets (scene_preset()->name): Low range 1600 m / 600 sprites / far density 0.35, High 3500 m / 2400 / 0.6;
   every value overridable in [scatter] (0 = preset). Lazy init on the first draw (file + texture); missing data
   = scatter silently off (logged once).
   Wiring: scene.cpp calls scatter_draw(c) after the terrain (one line, T22's file); main.cpp may call
   scatter_params() next to fx_params() so the [scatter] keys land in jet.ini (T19); without it the defaults below
   apply and F5 can't tune them. */
#ifndef SCATTER_H
#define SCATTER_H

#include "camera.h"

typedef struct ScatterStats {                       /* last frame */
    int sprites, cellsVisited, instTested, draws;
    float ms;
} ScatterStats;

void  scatter_params(void);                         /* [scatter] keys (optional, see above) */
void  scatter_draw(const Camera *c);                /* inside the scene, after terrain_draw; sets world = identity */
void  scatter_free(void);
const ScatterStats *scatter_stats(void);
int   scatter_loaded(void);                         /* 1 after a successful lazy init */

#endif
