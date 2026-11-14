/* r_backend.h - internal backend table between e_rend.cpp (dispatch) and r_d3d6.cpp / r_null.cpp (DESIGN 3.15).
   Not for game code. Owner: T05 (initial version by T01). init fills *caps; every other entry may assume init
   succeeded. A backend may leave an entry NULL: e_rend.cpp then does nothing / returns 0. */
#ifndef R_BACKEND_H
#define R_BACKEND_H

#include "e_rend.h"

typedef struct RBackend {
    const char *name;
    int   (*init)(void *hwnd, const RInit *ri, RCaps *caps);
    void  (*shutdown)(void);
    int   (*begin_frame)(u32 clearRGB);
    void  (*end_frame)(void);
    void  (*present)(void);
    void  (*set_viewport)(int x, int y, int w, int h);
    void  (*set_proj)(const Mat4 *m);
    void  (*set_view)(const Mat4 *m);
    void  (*set_world)(const Mat4 *m);
    void  (*set_fog)(int on, u32 rgb, float start, float end);
    void  (*set_material)(const RMaterial *m);
    RTex  (*tex_create)(const struct EImage *img, int flags);
    void  (*tex_free)(RTex t);
    int   (*tex_kb)(void);
    RVBuf (*vb_create)(int fmt, const void *verts, int count);
    void  (*vb_free)(RVBuf vb);
    void  (*draw_vb)(RVBuf vb, const u16 *idx, int nIdx);
    void  (*draw_mem)(int fmt, const void *verts, int nVerts, const u16 *idx, int nIdx);
    void  (*draw_2d)(const RVert2D *v, int nVerts, const u16 *idx, int nIdx, RTex tex, int blend);
    int   (*save_bmp)(const char *path);
    void  (*stats)(RStats *out, int reset);
    void  (*device_lost_check)(void);
    /* additions (T05) */
    void  (*pace_stats)(RPace *out, int reset);
    int   (*mode_count)(void);
    int   (*mode_at)(int i, int *w, int *h, int *bpp);
    int   (*tex_budget_kb)(void);
    int   (*tex_count)(void);
    /* additions (jet T01) */
    void  (*clear_z)(void);
} RBackend;

const RBackend *r_backend_null(void);              /* r_null.cpp */
const RBackend *r_backend_d3d6(void);              /* r_d3d6.cpp */

#endif
