/* scene.h - draw order, sky, terrain, sea, base, aircraft, clouds, presets (DESIGN 9). Windows/render side.
   Owner: T08 (T09 tunes the preset table, T17 adds the aircraft list / distance dots for combat, T22 polish).

   Frame (DESIGN 9.4; main.cpp calls these in order inside r_begin_frame(scene_clear_flags()) .. r_end_frame):
     scene_draw:      fog -> terrain (opaque, by class) -> base cells/instances (e_world, base.trk) -> aircraft
                      (mesh_draw_lit + moving parts, own jet last and only in chase view) -> sea (shallow, ring,
                      fog band, foam) -> late sky (panorama, Z-tested) -> cloud deck (High) -> cumulus puffs (sorted,
                      blended, fogged, faded near the camera)
     fx_draw (fx.cpp): afterburner flame + nozzle glow (additive, fog off)
     scene_draw_dots: 2x2 dark dots for aircraft 1.5-4 km away (under the mesh to 2.5 km)
     HUD, minimap, overlays (main)
   Presets (DESIGN 9.3): Low (VM, 800x600x16, 30 fps) / High (T40 1400x1050, 60 fps) in s_presets; every value can
   be overridden live in [scene] (0 / -1 = the preset's value).
   Props (M1 only, render-only, no simulation, no collision): two aggressor-livery jets parked on the apron and one
   circling the island at 1200 m ([scene] props = 0 hides them). They help judge the 3D look; combat aircraft come
   from the Game (T17). */
#ifndef SCENE_H
#define SCENE_H

#include "game.h"
#include "camera.h"
#include "e_mesh.h"

typedef struct ScenePreset {
    char name[8];
    float farM, fogStartM, fogEndFrac;              /* far plane, linear W fog start (m), fog end = frac x far */
    float lodScale;                                 /* terrain LOD scale (larger = finer, e_terrain.h) */
    int skyTiles, clouds, cloudDeck, sunGlow;       /* 4 / 8 tiles, puff count, 6 km deck, sun glow billboard */
    int texBudgetKB, fpsTarget;
    float acLodScale;                               /* jet mesh LOD distances x this */
} ScenePreset;

typedef struct SceneStats {                         /* last frame */
    int terrainTris, seaTris, baseTris, acTris, cloudQuads, aircraftDrawn, dots;
    float terrainMs, seaMs, baseMs, acMs, skyMs, cloudMs;
    float farM, fogStart, fogEnd;                   /* T10b: effective (altitude-aware) distances of the frame */
} SceneStats;

void  scene_params(void);                           /* [scene] */
void  scene_preset_init(const char *name);          /* before r_init: "low" | "high" (args) */
const ScenePreset *scene_preset(void);              /* effective values (preset + [scene] overrides) */
float scene_far_m(void);
int   scene_init(const Game *g);                    /* after r_init and terrain_load; 0 = nothing to draw */
void  scene_free(void);
u32   scene_clear_flags(void);                      /* r_begin_frame argument (fog colour | R_ZONLY) */
void  scene_draw(const Game *g, const Camera *c, float alpha);

/* ---- T08 ---- */
void  scene_draw_dots(const Game *g, const Camera *c, float alpha);
void  scene_draw_loading(const char *text);         /* one presented frame with a centred message (island load) */
const Mesh *scene_jet_mesh(void);                   /* jet.msh (tags: nozzle, cam_cockpit ...), NULL if missing */
const SceneStats *scene_stats(void);
Vec3  scene_sun_dir(void);                          /* unit vector toward the sun (light set) */
/* render-only prop aircraft (M1): count and pose at sim time t (seconds); fills an Aircraft-like pose */
int   scene_prop_count(void);
int   scene_prop_pose(int i, float t, Vec3 *pos, Quat *q, float *gearPos);

/* ---- T10b: altitude-aware view distances (the "ball" fix) ---- */
float scene_alt_factor(float eyeY);                 /* haze factor >= 1 ([scene] hazeScaleM) */
void  scene_view_dist(float eyeY, float *farM, float *fogStart, float *fogEnd);   /* preset x factor, capped farMaxM */
float scene_far_at(float eyeY);                     /* far plane for a camera at this altitude */
float scene_far_radial(const Camera *c);            /* eye distance of the far plane's corners (radial culls) */

/* ---- T17: combat ---- */
void  scene_light(u32 *sunRGB, u32 *ambRGB);        /* light set sun / ambient colours (fx smoke, trails) */
int   scene_draw_wreck(const Camera *c, Vec3 pos, Quat q);   /* an aggressor-livery jet without moving surfaces
                                                       (falling wreck, fx.cpp); returns triangles */
/* Also T17: in scene_draw the M1 props show only in MODE_FREE; LOD0 jets carry their rail missiles (stores of that
   aircraft, weapons.h railEmpty); missiles in flight are meshes within 600 m (lrm/srm, LOD0 < 120 m). */

/* ---- T22: polish ---- */
/* [scene] acFast: jets drawn with only their camera-facing triangles, lit on the CPU and batched per material (draw_jet).
   [scene] shadow*: jet shadows on the ground / runway / sea below shadowAglM, cast along the sun. */
int   scene_shadows_drawn(void);                    /* last frame */
float scene_cloud_near(void);                       /* last frame: 0 = clear air .. 1 = deep inside a cloud puff (fx haze) */

#endif
