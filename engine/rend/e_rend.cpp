/* e_rend.cpp - renderer dispatch (DESIGN 3.15): r_init -> D3D6 backend, r_init_null -> null backend.
   Owner: T05 (initial version by T01). Calls before a successful init (or after r_shutdown) do nothing. */
#include "r_backend.h"
#include "e_log.h"

static const RBackend *s_be;
static RCaps s_caps;
static int s_w, s_h;

static int start(const RBackend *be, void *hwnd, const RInit *ri)
{
    r_shutdown();
    memset(&s_caps, 0, sizeof(s_caps));
    if (!be || !be->init || !be->init(hwnd, ri, &s_caps)) {
        log_printf(LOG_ERR, "err: rend init failed backend=%s", be ? be->name : "?");
        return 0;
    }
    s_be = be;
    s_w = ri ? ri->width : 0;
    s_h = ri ? ri->height : 0;
    log_printf(LOG_INFO, "rend: init backend=%s device=\"%s\" %dx%dx%d fullscreen=%d", be->name, s_caps.device,
               ri ? ri->width : 0, ri ? ri->height : 0, ri ? ri->bpp : 0, ri ? ri->fullscreen : 0);
    return 1;
}

int r_init(void *hwnd, const RInit *ri) { return start(r_backend_d3d6(), hwnd, ri); }
int r_init_null(const RInit *ri) { return start(r_backend_null(), NULL, ri); }

void r_shutdown(void)
{
    if (s_be && s_be->shutdown) s_be->shutdown();
    s_be = NULL;
}

const RCaps *r_caps(void) { return &s_caps; }
const char *r_backend_name(void) { return s_be ? s_be->name : "none"; }

int  r_begin_frame(u32 clearRGB) { return (s_be && s_be->begin_frame) ? s_be->begin_frame(clearRGB) : 0; }
void r_end_frame(void) { if (s_be && s_be->end_frame) s_be->end_frame(); }
void r_present(void) { if (s_be && s_be->present) s_be->present(); }
void r_set_viewport(int x, int y, int w, int h) { if (s_be && s_be->set_viewport) s_be->set_viewport(x, y, w, h); }
void r_set_proj(const Mat4 *m) { if (s_be && s_be->set_proj) s_be->set_proj(m); }
void r_set_view(const Mat4 *m) { if (s_be && s_be->set_view) s_be->set_view(m); }
void r_set_world(const Mat4 *m) { if (s_be && s_be->set_world) s_be->set_world(m); }
void r_set_fog(int on, u32 rgb, float start, float end) { if (s_be && s_be->set_fog) s_be->set_fog(on, rgb, start, end); }
void r_set_material(const RMaterial *m) { if (s_be && s_be->set_material) s_be->set_material(m); }
RTex r_tex_create(const struct EImage *img, int flags) { return (s_be && s_be->tex_create) ? s_be->tex_create(img, flags) : 0; }
void r_tex_free(RTex t) { if (s_be && s_be->tex_free) s_be->tex_free(t); }
int  r_tex_kb(void) { return (s_be && s_be->tex_kb) ? s_be->tex_kb() : 0; }
RVBuf r_vb_create(int fmt, const void *verts, int count) { return (s_be && s_be->vb_create) ? s_be->vb_create(fmt, verts, count) : 0; }
void r_vb_free(RVBuf vb) { if (s_be && s_be->vb_free) s_be->vb_free(vb); }
void r_draw_vb(RVBuf vb, const u16 *idx, int nIdx) { if (s_be && s_be->draw_vb) s_be->draw_vb(vb, idx, nIdx); }

void r_draw_mem(int fmt, const void *verts, int nVerts, const u16 *idx, int nIdx)
{
    if (s_be && s_be->draw_mem) s_be->draw_mem(fmt, verts, nVerts, idx, nIdx);
}

void r_draw_2d(const RVert2D *v, int nVerts, const u16 *idx, int nIdx, RTex tex, int blend)
{
    if (s_be && s_be->draw_2d) s_be->draw_2d(v, nVerts, idx, nIdx, tex, blend);
}

int  r_save_bmp(const char *path) { return (s_be && s_be->save_bmp) ? s_be->save_bmp(path) : 0; }

void r_stats(RStats *out, int reset)
{
    if (s_be && s_be->stats) s_be->stats(out, reset);
    else if (out) memset(out, 0, sizeof(*out));
}

void r_device_lost_check(void) { if (s_be && s_be->device_lost_check) s_be->device_lost_check(); }

/* ---- additions (T05) ---- */
void r_screen_size(int *w, int *h)
{
    if (w) *w = s_be ? s_w : 0;
    if (h) *h = s_be ? s_h : 0;
}

void r_pace_stats(RPace *out, int reset)
{
    if (s_be && s_be->pace_stats) s_be->pace_stats(out, reset);
    else if (out) memset(out, 0, sizeof(*out));
}

int r_mode_count(void) { return (s_be && s_be->mode_count) ? s_be->mode_count() : 0; }

int r_mode_at(int i, int *w, int *h, int *bpp)
{
    return (s_be && s_be->mode_at) ? s_be->mode_at(i, w, h, bpp) : 0;
}

int r_tex_budget_kb(void) { return (s_be && s_be->tex_budget_kb) ? s_be->tex_budget_kb() : 0; }
int r_tex_count(void) { return (s_be && s_be->tex_count) ? s_be->tex_count() : 0; }

/* ---- additions (jet T01) ---- */
void r_clear_z(void) { if (s_be && s_be->clear_z) s_be->clear_z(); }
