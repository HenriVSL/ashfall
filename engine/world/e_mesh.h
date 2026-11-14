/* e_mesh.h - .msh load (portable part), LOD pick, draw (Windows part) (DESIGN 2.5, 8.3). Owner: T07 (initial header +
   stub by T01), T15 from M2 (mesh_draw_ex / mesh_append_part at the end).
   Notes (T07):
   - mesh_load(m, path, noRender): path = a file path, a data-relative path ("meshes/v10gt.msh") or a bare name
     ("v10gt" -> data\meshes\v10gt.msh). Copies everything out of the file (the file buffer is freed). noRender = 1
     makes no textures/VBs (physh, headless tests). idx[] stays as in the file: RELATIVE to the owning LOD's firstVert;
     parts[].firstIdx is absolute into idx[]. Without noRender, each LOD gets its own VB (lodVb[l], vb = lodVb[0]) so
     the LOD-relative indices index it directly, and every material gets rm (tex0/tex1 via tex_get: RB_ALPHATEST ->
     IMG_KEYED, RB_BLEND -> IMG_ALPHA; env/sphere textures RT_CLAMP).
   - mesh_lod(m, dist, lodScale): first LOD with dist < maxDist * lodScale (last LOD maxDist = 100000 = no limit).
   - Livery / texture override (mesh_draw tintTexOverride, mesh_draw_lit texOverride; 0 = none): replaces tex0 of every
     material whose tex0 name equals that of material 0 (the mesh's base atlas, e.g. car_0 -> car_<n>).
   - mesh_draw_lit (cars, DESIGN 3.9): CPU lighting per vertex into a scratch array, diffuse =
     vertexColour * (amb * (0.75 + 0.25 * n.y) + sun * max(0, n . sunDirBody)), sunDirBody = unit vector toward the sun
     in MESH space. envMap = 1: materials with RS1_ENVADD get uv2 from view-space normals (u = nx*.5+.5, v = -ny*.5+.5,
     view = the camera view matrix); envMap = 0 draws those materials without their env stage. Max 4096 vertices/LOD.
   - Drawing sets r_set_world(world) and resets it to identity afterwards. Callers scale rear wheels with the world
     matrix (A1: X 1.218, Y/Z 1.0215 about the hub) and skip wheel meshes for car LOD2 (wheels painted on).
   - Drawing code is compiled out with -DE_NO_RENDER (physh): draw calls are then no-ops. */
#ifndef E_MESH_H
#define E_MESH_H

#include "e_rend.h"

enum { MM_TINT = 1, MM_ENV = 2, MM_WHEEL = 4, MM_GLASS = 8, MM_LIGHT = 16 }; /* material flags */
typedef struct MeshMat { char tex[32]; char tex1[32]; u8 blend, stage1, cullNone, flags; RMaterial rm; } MeshMat;
typedef struct MeshPart { int mat, firstIdx, numIdx; } MeshPart;
typedef struct MeshLod { float maxDist; int firstPart, numParts; int firstVert, numVerts; } MeshLod;
typedef struct MeshTag { char name[16]; Vec3 pos; Vec3 dir; } MeshTag;   /* wheel_fl.., brake_l, exhaust_r.. */
typedef struct Mesh {
    char name[32]; int numVerts; RVert1 *verts; Vec3 *normals;  /* normals kept for CPU lighting/env */
    int numIdx; u16 *idx; int numMats; MeshMat *mats; int numParts; MeshPart *parts;
    int numLods; MeshLod lod[4]; int numTags; MeshTag tags[24];
    Aabb box; float radius; RVBuf vb;
    RVBuf lodVb[4]; void *mem;                      /* additions (T07): per-LOD VBs, allocation block */
} Mesh;
int   mesh_load(Mesh *m, const char *path, int noRender);  /* data\meshes\x.msh (8.3) */
void  mesh_free(Mesh *m);
int   mesh_lod(const Mesh *m, float dist, float lodScale);
const MeshTag *mesh_tag(const Mesh *m, const char *name);
void  mesh_draw(const Mesh *m, const Mat4 *world, int lod, RTex tintTexOverride);     /* static (pre-lit) */
void  mesh_draw_lit(const Mesh *m, const Mat4 *world, int lod, RTex texOverride,
                    Vec3 sunDirBody, u32 sunRGB, u32 ambRGB, int envMap, const Mat4 *view); /* cars (3.9) */

/* ---- additions (T07) ---- */
/* The Mesh struct gains (at its end, see below) lodVb[4] and mem (one block holding verts/normals/idx/mats/parts). */
int   mesh_lod_tris(const Mesh *m, int lod);       /* triangles drawn for one LOD (stats) */
int   mesh_tex_override_ok(const Mesh *m, int mat); /* 1 if material mat uses the base atlas (override applies) */

/* ---- additions (T15) ---- */
enum { MESH_PASS_OPAQUE = 1, MESH_PASS_ALPHA = 2, MESH_PASS_ALL = 3 };   /* parts by material blend */
/* static draw with an ARGB colour multiplier (0 or 0xFFFFFFFF = none: VB path; else CPU-tinted copy per part) and a
   pass mask (opaque parts = RB_OPAQUE, alpha parts = every other blend) */
void  mesh_draw_ex(const Mesh *m, const Mat4 *world, int lod, RTex texOverride, u32 tint, int passMask);
/* append one part of one LOD transformed by xf (world space) and tinted to dst arrays (indices offset by baseVert);
   returns the vertex count written (0 if it doesn't fit in maxVerts/maxIdx) and the index count in *nIdx */
int   mesh_append_part(const Mesh *m, int lod, int part, const Mat4 *xf, u32 tint, RVert1 *dst, int maxVerts,
                       u16 *dstIdx, int maxIdx, int baseVert, int *nIdx);
u32   mesh_tint_rgb(u32 c, u32 tint);              /* per-channel c * tint / 255 (alpha too) */

#endif
