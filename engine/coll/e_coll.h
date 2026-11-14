/* e_coll.h - collision world: triangles (4 m bucket grid), one-sided walls (16 m grid), boxes, ray queries,
   2D OBB SAT (DESIGN 2.5, 5.3, 5.4). Portable. Owner: T06.
   Conventions (T06):
   - Triangles: the plane is recomputed at load from a, b, c: n = unit normal facing up (n.y >= 0; vertex order is
     fixed internally if needed), d = -dot(n, a), so dot(n, p) + d = 0 on the plane. Rays hit front faces only
     (dot(n, dir) < 0): a ray cast downwards finds the drivable surface; undersides are ignored. Shared edges are
     closed with a 0.1 mm tolerance (no cracks).
   - coll_create(): every n* argument is the BYTE size of that .trk chunk (what chunk_find returns).
       COLT = CollTri[size / sizeof(CollTri)] (56 bytes each; n/d may be zero, they are recomputed);
       COLG = ignored in 1.0 (the 4 m bucket grid is always built at load from COLT; may be NULL);
       WALL = u32 numWalls, then WallSeg[numWalls] (60 bytes each); anything after is ignored (the 16 m grid is
              built at load); may be NULL;
       BOXS = u32 numBoxes, then CollBox[numBoxes]; may be NULL.
     The data is copied: the chunks may be freed afterwards. coll_create_tris takes element counts instead.
   - WallSeg: (nx, nz) = unit normal pointing to the drivable side; if both are 0 it is set to the right-hand side
     of the direction (x0,z0) -> (x1,z1). n0/n1 = smoothed vertex normals; if zero they are computed from the
     prev/next links (-1 = none). Collision ignores height (yLo/yHi are visual).
   - Obb2: (ax, az) = unit forward axis, hz = half length along it, hx = half width along the right axis
     (az, -ax). Contact2.n pushes A out of B (unit, XZ), depth >= 0, (px, pz) = contact point. */
#ifndef E_COLL_H
#define E_COLL_H

#include "e_math.h"

enum { SURF_NONE, SURF_ASPHALT, SURF_PAINT, SURF_KERB, SURF_GRAVEL, SURF_GRASS, SURF_DIRT, SURF_WATER,
       SURF_COUNT };                                /* grip etc. per PHYSICS.md 3.7 (table lives in phys) */
typedef struct CollTri { Vec3 a, b, c; Vec3 n; float d; u8 surface, flags; u16 pad; } CollTri;
typedef struct CollHit { float t; Vec3 pos, n; int surface, tri; } CollHit;
typedef struct WallSeg {                           /* one-sided: n points to the drivable side */
    float x0, z0, x1, z1;                          /* XZ plane */
    float nx, nz;                                  /* segment normal */
    float n0x, n0z, n1x, n1z;                      /* smoothed vertex normals (anti-snag) */
    float yLo, yHi;                                /* visual extent; collision ignores height in 1.0 */
    s32 prev, next; u8 type, side; u16 pad;
} WallSeg;
typedef struct CollBox { Vec3 c; float yaw, hx, hy, hz; u8 surface; } CollBox; /* buildings (open world, unused in 1.0) */
typedef struct CollWorld CollWorld;                /* opaque: tris + 4 m bucket grid, walls + 16 m grid, boxes */
CollWorld *coll_create(const void *colt, int ncolt, const void *colg, int ncolg,
                       const void *wall, int nwall, const void *boxs, int nboxs); /* from .trk chunks */
CollWorld *coll_create_plane(float y, int surface, float halfSize);  /* tests: infinite flat ground */
CollWorld *coll_create_tris(const CollTri *t, int n, const WallSeg *w, int nw); /* physh synthetic roads */
void  coll_free(CollWorld *w);
int   coll_ray(const CollWorld *w, Vec3 orig, Vec3 dir, float maxT, CollHit *hit); /* nearest hit */
typedef struct CollCache { int bx, bz; int n; int tri[48]; } CollCache;  /* per wheel: tris of last bucket(s) */
int   coll_ray_cached(const CollWorld *w, CollCache *cc, Vec3 orig, Vec3 dir, float maxT, CollHit *hit);
int   coll_walls_near(const CollWorld *w, float x, float z, float r, int *out, int max);
const WallSeg *coll_wall(const CollWorld *w, int i);
int   coll_boxes_near(const CollWorld *w, float x, float z, float r, int *out, int max);
typedef struct Obb2 { float cx, cz, ax, az, hx, hz; } Obb2;  /* centre, unit forward axis (ax,az), half sizes */
typedef struct Contact2 { float px, pz, nx, nz, depth; } Contact2;   /* n pushes A out of B */
int   coll_obb2_obb2(const Obb2 *a, const Obb2 *b, Contact2 *c);
int   coll_obb2_wall(const Obb2 *a, const WallSeg *w, Contact2 *c);   /* uses smoothed normals */

/* ---- additions (T06) ---- */
int   coll_num_tris(const CollWorld *w);
const CollTri *coll_tri(const CollWorld *w, int i);        /* plane fields recomputed (see above) */
int   coll_num_walls(const CollWorld *w);
/* signed distance of a point to a one-sided wall along its smoothed normal. Returns 1 when the point is within
   the segment's extent (+ 0.25 m at each end) and -maxDepth < dist < 0 (behind the wall face); fills depth = -dist
   and the smoothed push-out normal. */
int   coll_wall_point(const WallSeg *w, float x, float z, float maxDepth, float *depth, float *nx, float *nz);
/* signed distance (no extent/depth checks): > 0 on the drivable side; *t = projection parameter 0..1 (unclamped) */
float coll_wall_dist(const WallSeg *w, float x, float z, float *t);
/* ground height below (x, yTop, z) by a downward ray (yTop - maxDrop .. yTop); 0 = no ground */
void  coll_cache_reset(CollCache *cc);              /* must be called before first use (a zeroed cache is NOT
                                                      empty-valid) and whenever the world changes; veh_place does it */
int   coll_ground(const CollWorld *w, float x, float yTop, float z, float maxDrop, CollHit *hit);

#endif
