/* e_terrain.h - island heightmap chunks (5 LODs), height/class/LOS/segment queries, airbase data, sea (DESIGN 8).
   Load + queries + LOD selection are portable (headless, simh: e_terrain.cpp); drawing lives in e_terrain_draw.cpp
   (Windows/render). Owner: T04. The API block "DESIGN 8.6" is fixed by DESIGN (changes need orchestrator approval);
   everything after "additions (T04)" is appended. API notes, rules and numbers: engine/world/e_terrain.md.
   File format: data/island/jet.isl (container tag RISL); the byte-level reference is the docstring of
   tools/islandc/isl.py (T03).
   Query rules (T04):
   - terrain_height = the LOD0 render mesh: each 32 m quad is split SW-NE into (SW, NW, NE) + (SW, NE, SE), linear
     inside each triangle (NOT bilinear; matches what the player sees within the 0.025 m HGHT rounding). Outside
     the 513 x 513 grid = TERRAIN_OUTSIDE_H (-60). terrain_surface = max(height, seaLevel).
   - terrain_los(a, b) = 1 when the segment stays strictly above the mesh AND both ends are above sea level;
     touching counts as blocked. HMAX 256 m blocks are skipped when the ray is above the block maximum; only the
     remaining blocks are walked cell by cell with an exact per-triangle test (no sampling, no step size).
   - terrain_segment_hit(a, b, hit) = first contact along a -> b with the mesh OR the sea plane (missiles hit the
     water too); *hit = that point (NULL allowed); 0 = no contact. */
#ifndef E_TERRAIN_H
#define E_TERRAIN_H

#include "e_base.h"
#include "e_math.h"
#include "e_rend.h"                                 /* RVert1 (types only) */

struct WView;                                       /* e_world.h */

enum { TERRAIN_MAX_BOXES = 32 };

/* BASE chunk (DESIGN 8.6) */
typedef struct TerrainBase {
    float rwyX, rwyZ, rwyHeadingDeg, rwyLength, rwyWidth, rwyHeight;
    Vec3 threshold; float thresholdHeadingDeg;      /* runway start spawn */
    Vec3 airSpawn[4]; float airSpawnHeadingDeg[4];
    int nBoxes; Aabb box[TERRAIN_MAX_BOXES];        /* hangars, tower, tanks */
} TerrainBase;

/* ---- T04 types (contents of Terrain) ---- */
enum { TERRAIN_LODS = 5, TERRAIN_MAX_MATS = 8, TERRAIN_NOLOD = 255 };
#define TERRAIN_OUTSIDE_H (-60.0f)
enum { TCLASS_WATER, TCLASS_SAND, TCLASS_GRASS, TCLASS_ROCK, TCLASS_ASH, TCLASS_LAVA, TCLASS_RUNWAY, TCLASS_APRON };
enum { TMF_BLEND_MASK = 0xFF, TMF_CLAMPU = 0x100, TMF_CLAMPV = 0x200, TMF_ZBIAS1 = 0x400, TMF_NOCULL = 0x800 };
typedef struct TerrainMat { char tex[32]; u32 flags, tint; } TerrainMat;      /* MATL (tint: minimap/debug only) */
typedef struct TerrainBatchRec { u16 mat, skirtTris; u32 iFirst, iCount; } TerrainBatchRec;   /* BTCH, 12 B */
typedef struct TerrainChunk {
    int cx, cz;
    Aabb box;                                       /* over all LODs: terrain, skirts, foam */
    Vec3 centre;                                    /* AABB centre (distMode 1) */
    int vFirst[TERRAIN_LODS], vCount[TERRAIN_LODS], bFirst[TERRAIN_LODS], bCount[TERRAIN_LODS];
    int tris[TERRAIN_LODS], trisNoSkirt[TERRAIN_LODS], foamTris[TERRAIN_LODS];   /* per LOD (aliases resolved) */
    u16 skirtSide[TERRAIN_LODS][4];                 /* skirt triangles per side S E N W (geometric) */
    short nb[4];                                    /* chunk index of the neighbour S E N W, -1 = none */
    u8 meshLod[TERRAIN_LODS];                       /* LOD whose mesh is stored for LOD k (underwater: 0,1 -> 2) */
    u8 water;                                       /* any sample of the chunk below sea level */
    u8 underwater;                                  /* every sample below sea level (LOD0/1 alias LOD2) */
    u8 pad;
} TerrainChunk;
struct TerrainGpu;                                  /* e_terrain_draw.cpp */

typedef struct Terrain {
    int loaded;
    float seaLevel;
    TerrainBase base;
    /* T04: grid, HMAX, CLAS, chunks, materials (pointers into fileData unless noted) */
    u8 *fileData; int fileSize;                     /* the whole jet.isl, kept (VERT/INDX reordered by gpu_init) */
    int gridN, chunkCells, nChunkX, nChunkZ, nLods;
    float cell, invCell, x0, z0, hMin, hScale, lodEdge[TERRAIN_LODS];
    float centreX, centreZ, radius;                 /* land bounding circle */
    const u16 *hght;                                /* gridN * gridN */
    const u16 *hmax;                                /* 64 * 64 (8 x 8-cell blocks) */
    const u8 *clas;                                 /* (gridN-1)^2 */
    int nBlocksX;                                   /* (gridN - 1) / 8 */
    float *blockMax;                                /* malloc'd HMAX as metres (LOS march) */
    int nSuperX; float superMax[64];                /* max over 8 x 8 blocks (2048 m), nSuperX <= 8 */
    float hTop;                                     /* highest sample */
    int nMats; TerrainMat mats[TERRAIN_MAX_MATS];
    int foamMat;                                    /* material with blend ADD (-1 = none) */
    int nChunks; TerrainChunk *chunks;              /* malloc'd, sorted by (cz, cx) */
    float *cull;                                    /* malloc'd per chunk: AABB lo xyz, hi xyz, centre xyz (select;
                                                       water chunks: hi.y >= seaLevel + 1, so the sea fan counts) */
    int nGroupsX, nGroupsZ;                         /* 4 x 4-chunk groups (2 km): coarse cull before the chunks */
    int *grpStart;                                  /* malloc'd nGroups + 1: first index into grpList */
    int *grpList;                                   /* malloc'd nChunks: chunk indices by group */
    float *grpBox;                                  /* malloc'd nGroups * 6: AABB of the group's cull boxes */
    u8 *prevLod;                                    /* malloc'd nChunks: selLod of the previous select (hysteresis) */
    short *chunkAt;                                 /* malloc'd nChunkX * nChunkZ: chunk index or -1 */
    int nBatches; const TerrainBatchRec *batches;
    int nVerts; RVert1 *verts;
    int nIdx; u16 *idx;
    char mapTex[32]; float mapRect[4];              /* MAPI: minimap texture + world rect x0 z0 x1 z1 */
    /* LOD selection state (terrain_select; malloc'd nChunks each; pointees are mutable through a const Terrain) */
    u8 *selLod;                                     /* selected LOD per chunk, TERRAIN_NOLOD = beyond far */
    u8 *selFlags;                                   /* TSEL_* */
    int *visList; int nVis;                         /* visible chunk indices of the last selection */
    struct TerrainGpu *gpu;                         /* render data (NULL until terrain_gpu_init) */
    void (*gpuFree)(struct Terrain *t);             /* set by terrain_gpu_init; terrain_free calls it */
} Terrain;

/* ---- DESIGN 8.6 API ---- */
int   terrain_load(Terrain *t, const char *path);          /* no render data needed (headless, simh); 0 = failed */
void  terrain_free(Terrain *t);
float terrain_height(const Terrain *t, float x, float z);  /* LOD0 mesh rule (header notes); outside grid = -60 */
float terrain_surface(const Terrain *t, float x, float z); /* max(height, seaLevel) */
int   terrain_class(const Terrain *t, float x, float z);
int   terrain_los(const Terrain *t, const Vec3 *a, const Vec3 *b);   /* 1 = clear */
int   terrain_segment_hit(const Terrain *t, const Vec3 *a, const Vec3 *b, Vec3 *hit);   /* missiles; 1 = hit */
const TerrainBase *terrain_base(const Terrain *t);

/* render side (e_terrain_draw.cpp; not linked in simh) */
int   terrain_gpu_init(Terrain *t);
void  terrain_gpu_free(Terrain *t);
void  terrain_draw(const Terrain *t, const struct WView *view, float lodScale, float farDist);
void  sea_draw(const Terrain *t, const struct WView *view, float time);

/* ---- additions (T04) ---- */
/* Tunables of the terrain + sea renderer (one global set; the scene maps its params / preset onto it before
   drawing). Defaults = Low. The portable selection uses lodK, hyst, distMode, skirtMode. */
typedef struct TerrainDrawCfg {
    float lodK;                 /* LOD k ends at lodScale * lodK * lodEdge[k] (28.6: 16 px tris at 640 px, 70 deg) */
    float hyst;                 /* hysteresis fraction (0.10): coarser only beyond (1 + hyst) x the switch distance */
    int   distMode;             /* LOD distance: 0 = eye to nearest point of the chunk AABB (DESIGN), 1 = to the AABB
                                   centre (default: ~20 % fewer tris near the ground) */
    int   skirtMode;            /* 0 = skirts only on chunks with a neighbour at another LOD, 1 = always, 2 = never */
    int   mergeLod;             /* chunks with LOD >= mergeLod are copied into one draw per material (0 = all) */
    /* sea (DESIGN 8.3) */
    float seaPeriod;            /* m per sea texture repeat (128) */
    float seaScrollU, seaScrollV;   /* uv per second (A3: toward (-0.87, -0.50) at 0.012/s) */
    float seaShallowAlpha;      /* sea alpha over shallow water (0.65) */
    float seaDeepFrom, seaDeepTo;   /* depth (m) where the shallow sea alpha ramps to 1 (opaque over deep water) */
    float seaFarShade;          /* vertex colour factor at the far plane (1 at the eye) */
    int   seaSegs;              /* ring segments (16) */
    int   seaZBias;             /* zBias of the shallow sea over the opaque ring (coplanar) */
    int   foamZBias;            /* zBias of the foam (0.25 m above the sea) */
    int   shallowLod;           /* finest LOD of the shallow sea fans: 3 (256 m, 8 tris/chunk) or 4 (512 m, 2) */
    /* fog as the scene set it (r_set_fog): foam is additive and fades with a black fog; 0 end = fog off */
    u32   fogRGB; float fogStart, fogEnd;
} TerrainDrawCfg;
/* lodScale presets for terrain_draw (DESIGN 8.2 formula: switch distances x lodScale, so larger = finer; DESIGN 9.3's
   "0.5 (8 px)" for High would read 2.0 in this convention, ~16-18k tris). Low 0.85 with distMode 1 keeps the
   ENGINE 2.7 views <= ~5.6k terrain tris; High 1.5 <= ~11.3k (High budget 12k). T09/T23 tune. */
#define TERRAIN_LODSCALE_LOW  0.85f
#define TERRAIN_LODSCALE_HIGH 1.5f
TerrainDrawCfg *terrain_draw_cfg(void);            /* the global set (defaults on first call) */
void  terrain_draw_cfg_defaults(TerrainDrawCfg *c);

typedef struct TerrainStats {
    int tris;                   /* terrain triangles drawn (with the skirts that were drawn, without foam) */
    int skirtTris, foamTris;    /* skirt part of tris; foam triangles (sea_draw) */
    int chunksByLod[TERRAIN_LODS];  /* visible chunks per selected LOD */
    int skirtChunks;            /* visible chunks drawn with skirts */
    int draws;                  /* r_draw_* calls of terrain_draw */
    int verts;                  /* vertices terrain_draw handed to the renderer (each one is transformed) */
    int seaTris, shallowTris, seaDraws;   /* sea_draw: opaque ring, shallow fans, calls (incl. foam) */
    int inRange;                /* chunks that got a LOD (in the frustum and within farDist) */
} TerrainStats;

enum { TSEL_VISIBLE = 1, TSEL_SKIRT = 2, TSEL_SKIRT_S = 4 };   /* side s needs skirts: TSEL_SKIRT_S << s (S E N W) */
enum { TSIDE_S, TSIDE_E, TSIDE_N, TSIDE_W };
/* Portable LOD selection + culling (terrain_draw calls it): selLod for every chunk within farDist (TERRAIN_NOLOD
   beyond), TSEL_VISIBLE for chunks whose AABB is in the frustum, TSEL_SKIRT + TSEL_SKIRT_S << side for every side
   whose 4-neighbour (stored, within farDist) has another mesh LOD (skirtMode 0; only those sides are drawn). hysteresis = 1 uses the previous selLod (frame to frame),
   0 = fresh (counts, tests). st (may be NULL) gets tris (as drawn: skirts per TSEL_SKIRT), chunksByLod,
   skirtChunks, inRange, foamTris (foam of LODs 0-2). Returns the visible chunk count (t->visList). */
int   terrain_select(const Terrain *t, const struct WView *view, float lodScale, float farDist, int hysteresis,
                     TerrainStats *st);
/* (T04 note) only chunks in the frustum get a LOD (others TERRAIN_NOLOD): an edge shared with an invisible chunk is
   itself outside the frustum, so skirts / restriction / fan stitching toward it are never visible. */
const TerrainStats *terrain_stats(void);           /* counters of the last terrain_draw + sea_draw */
TerrainStats *terrain_stats_rw(void);              /* internal: e_terrain_draw.cpp fills the draw counters */

/* Reference (slow, exact) segment test for -test terrain: every cell the segment's XZ path can touch, no HMAX,
   no DDA. Same contact rule as terrain_segment_hit, but terrain only (no sea). 1 = contact, *tHit = first t. */
int   terrain_segment_ref(const Terrain *t, const Vec3 *a, const Vec3 *b, float *tHit);

/* internal: side (TSIDE_*) of a skirt triangle of chunk ch (all three vertices on that chunk edge), -1 = none */
int   terrain_tri_side(const Terrain *t, const TerrainChunk *ch, const RVert1 *a, const RVert1 *b, const RVert1 *c);

/* Fog colour for r_set_fog and World.sky.fog on the 16-bit Voodoo3 (T04 measured): texels are truncated to 565 but
   fog / vertex colours come out one 565 level darker, so the fogged far sea and the sky's vertex-coloured lower band
   show (189,210,222) against the panorama's fog rows (197,214,230). Adding one level (8,4,8) makes all three match. */
u32   terrain_fog_565fix(u32 rgb);

/* more portable queries */
Vec3  terrain_normal(const Terrain *t, float x, float z);     /* unit normal of the LOD0 triangle under (x, z) */
float terrain_block_max(const Terrain *t, float x, float z);  /* HMAX of the 256 m block (outside grid: -60) */
float terrain_area_max(const Terrain *t, float x0, float z0, float x1, float z1);  /* max HMAX over a rect */

#endif
