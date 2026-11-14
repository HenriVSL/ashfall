/* r_null.cpp - null backend (headless runs, tests): every call succeeds and draws nothing, but handles are
   unique and RStats counts what would have been drawn. Portable (no Windows calls). Owner: T05 (initial by T01). */
#include "r_backend.h"
#include "e_image.h"

static RStats s_st;
static int s_nextTex, s_nextVb, s_texKB;

static int n_init(void *hwnd, const RInit *ri, RCaps *caps)
{
    (void)hwnd; (void)ri;
    memset(&s_st, 0, sizeof(s_st));
    s_nextTex = s_nextVb = 1;
    s_texKB = 0;
    e_strlcpy(caps->device, "null", (int)sizeof(caps->device));
    caps->maxTexW = caps->maxTexH = 256;
    caps->maxAspect = 8;
    caps->texStages = 2;
    caps->zBits = 16;
    caps->fmt565 = caps->fmt1555 = caps->fmt4444 = 1;
    caps->refreshHz = 60.0f;
    caps->vblankDivisor = 1;
    return 1;
}

static void n_shutdown(void) {}
static int  n_begin_frame(u32 clearRGB) { (void)clearRGB; return 1; }

static RTex n_tex_create(const struct EImage *img, int flags)
{
    (void)flags;
    if (img) s_texKB += (img->w * img->h * 2) / 1024;
    return s_nextTex++;
}

static int   n_tex_kb(void) { return s_texKB; }
static RVBuf n_vb_create(int fmt, const void *verts, int count) { (void)fmt; (void)verts; (void)count; return s_nextVb++; }

static void n_draw_vb(RVBuf vb, const u16 *idx, int nIdx)
{
    (void)vb; (void)idx;
    s_st.draws++;
    s_st.tris += nIdx / 3;
}

static void n_draw_mem(int fmt, const void *verts, int nVerts, const u16 *idx, int nIdx)
{
    (void)fmt; (void)verts; (void)idx;
    s_st.draws++;
    s_st.tris += nIdx / 3;
    s_st.verts += nVerts;
    s_st.dynVerts += nVerts;
}

static void n_draw_2d(const RVert2D *v, int nVerts, const u16 *idx, int nIdx, RTex tex, int blend)
{
    (void)v; (void)idx; (void)tex; (void)blend;
    s_st.draws++;
    s_st.tris += nIdx / 3;
    s_st.verts += nVerts;
}

static void n_stats(RStats *out, int reset)
{
    if (out) *out = s_st;
    if (reset) memset(&s_st, 0, sizeof(s_st));
}

static const RBackend s_null = {
    "null", n_init, n_shutdown, n_begin_frame, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    n_tex_create, NULL, n_tex_kb, n_vb_create, NULL, n_draw_vb, n_draw_mem, n_draw_2d, NULL, n_stats, NULL
};

const RBackend *r_backend_null(void) { return &s_null; }
