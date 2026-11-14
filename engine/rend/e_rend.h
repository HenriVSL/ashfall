/* e_rend.h - renderer interface (DESIGN 2.5, 3). Backends: r_d3d6.cpp (D3D6), r_null.cpp (headless/tests);
   later r_gl11/r_d3d9. e_rend.cpp dispatches through the RBackend table in r_backend.h. Types only: no windows.h,
   so portable headers (e_mesh.h, e_world.h) may include it. Owner: T05 (initial header by T01). */
#ifndef E_REND_H
#define E_REND_H

#include "e_math.h"

struct EImage;
typedef int RTex;  typedef int RVBuf;
enum { RFMT_V1 = 1, RFMT_V2 = 2 };                 /* vertex formats */
typedef struct RVert1 { float x, y, z; u32 color; float u, v; } RVert1;            /* 24 B: XYZ|DIFFUSE|TEX1 */
typedef struct RVert2 { float x, y, z; u32 color; float u, v, u2, v2; } RVert2;    /* 32 B: XYZ|DIFFUSE|TEX2 */
typedef struct RVert2D { float x, y; u32 color; float u, v; } RVert2D;             /* screen pixels */
enum { RB_OPAQUE, RB_ALPHATEST, RB_BLEND, RB_ADD, RB_SHADOW,                      /* 3.4 */
       RB_PREMUL };                                /* jet T02: premultiplied over (ONE, INVSRCALPHA) */
enum { RS1_NONE, RS1_DETAIL, RS1_ENVADD };         /* stage-1 op: MODULATE2X detail / ADD env map */
typedef struct RMaterial {
    RTex tex0, tex1;
    u8 blend, stage1, cullNone, zWrite, zTest, fog, clampUV, zBias;   /* zBias 0..2 (decals/shadows) */
    s8 lodBias;                                    /* T16: mip LOD bias of stage 0 in quarter mip levels (-2 = 0.5 sharper), 0 = none */
} RMaterial;
typedef struct RInit {
    int width, height, bpp, fullscreen;            /* bpp 16 or 32 */
    int fpsTarget;                                 /* 30, 60 (0 = refresh rate) -> vblank divisor (3.13) */
    int wantWBuffer, filter, maxTexSize;           /* filter 0 bilinear+point mip, 1 trilinear */
    int texBudgetKB;                               /* used when the driver reports no free VRAM */
    int noCap;                                     /* -nocap: present without pacing (measurements only) */
} RInit;
typedef struct RCaps {
    char device[64]; int hal;
    int maxTexW, maxTexH, maxAspect, texStages, addOp, mod2xOp;
    int fogTable, fogVertex, wbuffer, zBits, alphaTest, mipmap, trilinear, managedTex;
    int fmt565, fmt555, fmt1555, fmt4444, fmt8888;
    float refreshHz; int vblankDivisor;
    u32 vidTotalKB, vidFreeKB;                     /* 0 = unknown (86Box Voodoo3 may report ~0, td5 FINDINGS) */
} RCaps;
typedef struct RStats { int tris, verts, draws, texChanges, stateChanges, dynVerts; } RStats;
enum { RT_CLAMP = 1, RT_NOMIP = 2 };               /* r_tex_create flags */
int   r_init(void *hwnd, const RInit *ri);         /* D3D6; 0 = failed (error logged, message box by caller) */
int   r_init_null(const RInit *ri);                /* headless / tests: everything succeeds, draws nothing */
void  r_shutdown(void);
const RCaps *r_caps(void);
#define R_ZONLY 0x01000000                          /* r_begin_frame flag: clear Z only (sky covers the screen) */
int   r_begin_frame(u32 clearRGB);                 /* clears colour+Z (Z only with R_ZONLY); 0 = device lost */
void  r_end_frame(void);                           /* EndScene */
void  r_present(void);                             /* pacing (3.13), Flip or Blt */
void  r_set_viewport(int x, int y, int w, int h);
void  r_set_proj(const Mat4 *m); void r_set_view(const Mat4 *m); void r_set_world(const Mat4 *m); /* NULL = ident */
void  r_set_fog(int on, u32 rgb, float start, float end);
void  r_set_material(const RMaterial *m);          /* backend filters redundant state changes */
RTex  r_tex_create(const struct EImage *img, int flags);   /* RT_CLAMP, RT_NOMIP; format from image flags */
void  r_tex_free(RTex t);
int   r_tex_kb(void);                              /* our accounting of loaded texture memory */
RVBuf r_vb_create(int fmt, const void *verts, int count);  /* static geometry (D3D6 vertex buffer) */
void  r_vb_free(RVBuf vb);
void  r_draw_vb(RVBuf vb, const u16 *idx, int nIdx);       /* indexed triangle list, indices into vb */
void  r_draw_mem(int fmt, const void *verts, int nVerts, const u16 *idx, int nIdx);  /* dynamic */
void  r_draw_2d(const RVert2D *v, int nVerts, const u16 *idx, int nIdx, RTex tex, int blend); /* no Z/fog */
int   r_save_bmp(const char *path);                /* back buffer -> 24-bit BMP (call before r_present) */
void  r_stats(RStats *out, int reset);
void  r_device_lost_check(void);                   /* restore surfaces/textures after alt-tab */

/* ---- additions (T01) ---- */
const char *r_backend_name(void);                  /* "none", "null", "d3d6" (logs) */

/* ---- additions (T05) ---- */
/* Colours (RVert*.color, argb parameters) are D3DCOLOR 0xAARRGGBB. Winding: front faces are clockwise on screen
   (cull CCW, D3D default). r_draw_2d clamps vertices to the screen (the Voodoo3 has no guard band): callers clip
   (e_ui does). The D3D6 backend transforms and clips on the CPU (DESIGN 3 orchestrator decision): r_draw_vb only
   transforms the vertex range the index list uses, so keep each batch's vertices contiguous in its VB. */
void  r_screen_size(int *w, int *h);               /* render target size (RInit width/height) */
typedef struct RPace {
    int presents, misses;                          /* presents counted; misses = interval > divisor blanks */
    int hist[9];                                   /* hist[k] = presents k blanks after the previous one (8 = 8+) */
    int worstBlanks, divisor; float refreshHz;
    int vblankOk;                                  /* 0 = WaitForVerticalBlank unusable, paced by QPC sleep */
} RPace;
void  r_pace_stats(RPace *out, int reset);
int   r_mode_count(void);                          /* display modes for the options menu (3.1 step 7) */
int   r_mode_at(int i, int *w, int *h, int *bpp);
int   r_tex_budget_kb(void);                       /* effective budget: min(RInit.texBudgetKB, 85% of free VRAM) */
int   r_tex_count(void);                           /* live textures */

/* ---- additions (jet T01, DESIGN 2.3 / 9.1 Plan B) ---- */
void  r_clear_z(void);                             /* clear the whole Z buffer mid-frame (inside the scene) */

#endif
