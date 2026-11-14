/* e_world.h - world: 128 m cells, batches, instances, culling, draw (DESIGN 2.5, 4). Loading is portable
   (world_load with WL_NO_RENDER), drawing goes through e_rend.h. Owner: T07 (initial header + stub by T01), T15 from M2 (sky, fog, instances: see the T15 notes at the end).
   Notes (T07):
   - world_load(w, path, flags): path = file path, data-relative ("tracks/testloop.trk") or a bare id ("testloop" ->
     data\tracks\testloop.trk). The whole file stays in w->fileData (CELL records are used in place; game chunks via
     world_chunk). Builds: materials (MATL+TEXL), cells + grid, batches (BTCH -> WBatch, idx points into INDX), one VB
     per VPOL pool (indices are pool-relative, each batch's vertices are contiguous), per-batch AABBs (from the
     vertices it uses), meshes (MSHL) + instances (INST), the CollWorld (COLT/COLG/WALL/BOXS). WL_NO_RENDER: no
     textures/VBs (vb = 0, rm textures 0). Returns 0 (err: logged) on a bad file.
   - Culling (DESIGN 4.7): cells in the grid rectangle around the eye within viewDist; per cell distance eye->AABB <=
     viewDist and AABB in the frustum; LOD0 if that distance < lodDist else LOD1; then per batch its AABB vs the frustum.
     Instances: per visible cell, sphere vs frustum and distance < drawDist * objScale; WI_THIN instances are drawn
     only if (flags & 0xFF) < treeDensity * 256. Batches are drawn sorted by material (counting sort).
   - Opaque pass = batches whose material blend is RB_OPAQUE (+ instances); alpha pass = every other blend.
   - WView additions: lodScale (mesh LOD distances x, 0 = 1) and treeDensity (0 = 1 = all).
   - world_draw_opaque/alpha set the view, projection and identity world matrix from the WView themselves.
   - world_draw_sky: placeholder gradient dome (zenith -> horizon -> fog colour below) until T15's panorama.
   - Drawing code is compiled out with -DE_NO_RENDER (physh); world_visible is the portable culling core. */
#ifndef E_WORLD_H
#define E_WORLD_H

#include "e_mesh.h"
#include "e_coll.h"

enum { WORLD_CELL = 128 };                          /* metres */
enum { WL_NO_RENDER = 1 };
typedef struct WBatch { int cell, lod, mat, fmt; RVBuf vb; const u16 *idx; int nIdx; } WBatch;
typedef struct WInst { int mesh; Mat4 xf; Vec3 c; float r, drawDist; u16 cell, flags; } WInst;
typedef struct WCell { s16 cx, cz; Aabb box; int batch0[2], nBatch[2]; int inst0, nInst; } WCell;
typedef struct WSky { char tile[8][32]; int nTiles; float vHorizon, elevLo, elevHi;
                      u32 zenith, horizon, fog; Vec3 sunDir; u32 sun, ambient; } WSky;
typedef struct World {
    char name[32]; int gx0, gz0, gw, gh; WCell **grid; int numCells; WCell *cells;
    int numMats; MeshMat *mats; int numBatches; WBatch *batches; int numMeshes; Mesh *meshes;
    int numInst; WInst *inst; CollWorld *coll; WSky sky; float fogStart, fogEnd;
    void *fileData; int fileSize;                   /* .trk kept in memory; game chunks read from it */
    /* additions (T07) */
    int numPools; RVBuf *poolVb; int *poolFmt; int *poolCount; const u8 **poolVerts;   /* VPOL pools */
    Aabb *batchBox;                                 /* per batch: AABB of the vertices it uses */
    int numTex; char (*texNames)[32];               /* TEXL */
    int totalTris[2];                               /* all batches per LOD (stats) */
    /* additions (T15) */
    u32 *instColor;                                 /* per instance ARGB multiplier from INST (0xFFFFFFFF = none) */
    int skyTiles;                                   /* panorama textures loaded (0 = gradient dome) */
    RTex skyTex[8];
    u32 skyTop[32], skyBot[32];                     /* panorama top/bottom row colour per sky segment */
    float skyElevLo, skyElevHi, skyVHor;            /* radians, radians, v of the horizon row */
    RTex glowTex; int sunGlow;                      /* additive sun glow billboard (world_set_sun_glow) */
    int detailOn;                                   /* RS1_DETAIL materials drawn with their detail pass */
    /* additions (jet T01) */
    char skyHiPrefix[32], skyLoPrefix[32];          /* tile_prefix / tile_prefix_low of the light set ("" = none) */
} World;
int   world_load(World *w, const char *trkPath, int flags);
void  world_free(World *w);
typedef struct WView { Vec3 eye; Mat4 view, proj; Frustum fr; float viewDist, lodDist, objScale;
                       float lodScale, treeDensity; /* additions (T07): 0 = 1 */ } WView;
typedef struct WDrawStats { int cells, batches, inst, tris; } WDrawStats;
void  world_draw_opaque(const World *w, const WView *v, WDrawStats *st);  /* sorted by material (3.5) */
void  world_draw_alpha(const World *w, const WView *v, WDrawStats *st);   /* alpha-tested foliage etc. */
void  world_draw_sky(const World *w, const WView *v);

/* ---- additions (T07) ---- */
enum { WI_THIN = 0x100 };                           /* WInst/INST flag: subject to treeDensity thinning (T13) */
enum { WORLD_MAX_VISIBLE = 4096 };
const u8 *world_chunk(const World *w, const char id[4], int *size);  /* raw chunk of the kept .trk, NULL if absent */
/* view from eye/at: fovY radians, near 1 m, far viewDist + 50 m, frustum, lodScale/objScale/treeDensity 1 */
void  world_view_lookat(WView *v, Vec3 eye, Vec3 at, float fovY, float aspect, float viewDist, float lodDist);
/* portable culling core: visible batch indices sorted by material (pass 0 opaque, 1 alpha), returns the count;
   st (may be NULL) gets cells/batches/tris; instIdx/nInstOut (may be NULL) get the visible instances (pass 0 only) */
int   world_visible(const World *w, const WView *v, int pass, int *out, int maxOut, int *instIdx, int *nInstOut,
                    int maxInst, WDrawStats *st);
/* one whole world view: r_set_fog from the world (end = min(fogEnd, viewDist), start = min(fogStart, 0.9 end);
   no SKY values -> 0.4/1.0 x viewDist), sky, opaque, alpha. st = sum of both passes (may be NULL) */
void  world_draw_view(const World *w, const WView *v, WDrawStats *st);

/* ---- additions (T15) ---- */
/* Panorama sky (DESIGN 3.7). world_sky_load loads the SKY chunk's tiles (or `prefix`_0..7 when prefix is given: a dev
   override, path as given or data\sky\): tile names resolve to data\sky\<name>.bmp, then data\tex\<name>.bmp.
   maxTiles 4 (Low) uses "<base>_low_<i>" files when all 4 exist, else downsamples pairs of the 8 high tiles.
   Mapping: u over all tiles = compass azimuth 0..360 deg (u 0 = north +Z, growing clockwise toward east +X);
   v 0 = elevHi, v = vHorizon at 0 deg, v 1 = elevLo, linear in elevation per band (elevLo/Hi in degrees in the chunk;
   |x| <= 3.2 read as radians). Returns the tile count (0 = gradient dome stays). Logs the azimuth of the panorama's
   brightest column next to sunDir's azimuth ("sky: sun_check ..."). Freed by world_free.
   world_draw_sky: panorama cylinder (one draw per tile) + zenith cap (vertex colours: panorama top row -> zenith) +
   lower band (bottom row -> fog at -30 deg -> fog at the nadir), no Z, no fog, view translation removed: covers every
   pixel, so the frame may clear Z only (R_ZONLY). Sun glow (if world_set_sun_glow on): additive billboard at sunDir.
   Instances (INST): colour multiplier applied (CPU-tinted vertices); meshes of <= 64 vertices per LOD are pre-
   transformed and merged into one draw per (mesh, LOD, part) (trees); opaque parts draw in the opaque pass,
   alpha-tested/blended parts in the alpha pass. world_set_detail(0) drops RS1_DETAIL stage-1 passes (Low preset). */
/* jet T01 (A2's data/sky/): the light set file is <prefix>.txt (as a path), data\sky\<prefix>.txt, else
   data\sky\light.txt. Its word keys tile_prefix / tile_prefix_low name the tiles: High = <tile_prefix>_0..7 when
   <prefix>_0 does not exist (so world_sky_load(w, n, "light") or (w, n, "sky_hi") both load sky_hi_*), Low (maxTiles
   <= 4) = <tile_prefix_low>_0..3 first, then the racer's <base>_low_0..3, then halved High tiles. fogStart/fogEnd keys
   set World.fogStart/fogEnd. world_set_sun_glow: tex\sun_glow.bmp, else tex\glow.bmp, else a procedural glow. */
int   world_sky_load(World *w, int maxTiles, const char *prefix);
void  world_set_sun_glow(World *w, int on);
void  world_set_detail(World *w, int on);

/* ---- additions (T16) ----
   world_set_sky_late(1): world_draw_sky Z-tests (no Z write) and scales the dome to viewDist + 25 m, so the caller can
   draw it after the opaque pass and the sky only fills pixels no geometry covered (saves ~2/3 of a screen of fill on
   the VM). Default 0 = T15's behaviour (sky first, no Z). */
void  world_set_sky_late(int on);

#endif
