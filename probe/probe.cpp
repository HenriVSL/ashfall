/* probe.cpp - Jet M0 depth / fog / HUD-line probe (T02, DESIGN 9.2). Links ..\engine\engine.lib. Owner: T02.
   Usage (from M:\dev\jet\probe, or anywhere: paths are relative to the exe's parent folder M:\dev\jet):
     probe.exe -scene N|all [-wbuf 0|1] [-shot] [-fs] [-res 640|800|1024] [-tag NAME]
   -wbuf 1 = W-buffer (D3DZB_USEW, default), 0 = 16-bit Z. -shot keeps screenshots in probe\shots\ (BMP; the Mac side
   converts them to PNG). -fs = fullscreen (grabs go through r_save_bmp; benches skip the vsync-locked Flip), -res = mode
   size (default 640x480; windowed only fits 640x480 on the 800x600 desktop), default windowed 640x480 on the
   desktop (grabs = GDI copy of the client area after r_present). Every run exits by itself (windowed: no mode change).
   Results: one "probe: ..." line per measurement in probe\probe_<tag>.log (tag default "w" / "z").
   Scenes (DESIGN 9.2): 1 coastline, 2 parallel quads, 3 runway decal + zBias, 4 fog ramp + sea/sky join, 5 UV
   magnitudes, 6 aircraft visibility (A1 jet mesh, dots), 7 HUD lines/text/blend (A4 atlas) + blend check,
   8 costs (r_clear_z, ui_line batch, fog fill, blend fill). Method notes are in probe\RESULTS.md. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "e_base.h"
#include "e_math.h"
#include "e_log.h"
#include "e_cfg.h"
#include "e_file.h"
#include "e_image.h"
#include "e_prof.h"
#include "e_rend.h"
#include "e_plat.h"
#include "e_ui.h"
#include "e_tex.h"
#include "e_mesh.h"
#include "e_world.h"

enum { MAXPIX = 1024 * 768 };
static int SW = 640, SH = 480, NPIX = 640 * 480;   /* -res 800 = 800x600, -res 1024 = 1024x768 */

/* ---------------------------------------------------------------- state */
static int    s_wbuf = 1, s_fs, s_shot;
static char   s_tag[16];
static u32   *s_img[6];                             /* grab buffers: 0 test A, 1 ref A, 2 test B, 3 ref B, 4/5 spare */
static HDC    s_memDC;
static HBITMAP s_dib;
static u32   *s_dibBits;
static Font   s_font;
static int    s_fontOk;
static Mat4   s_view, s_proj;
static Vec3   s_eye;
static float  s_near = 1.0f, s_far = 18000.0f, s_fovH = 65.0f;
static World  s_world;
static int    s_skyOk;
static RTex   s_texSea, s_texGrass, s_texCloud, s_atlas;
static float  s_seaMean[3] = { 40.0f, 80.0f, 120.0f };   /* average texel of sea.bmp (fog fraction from colour) */
static Mesh   s_jet;
static int    s_jetOk;
static char   s_label[96];
static int    s_grabs;

static const u32 COL_SKY  = 0xFFC4D4E4;             /* A2 fog colour 196 212 228 (clear = sky bottom) */
static const u32 COL_SEA  = 0xFF2040C0;             /* flat colours exactly representable in RGB565 */
static const u32 COL_LAND = 0xFF20A020;
static const u32 COL_NEAR = 0xFF2040C0;
static const u32 COL_FAR  = 0xFFC02020;
static const u32 COL_RWY  = 0xFF606060;
static const u32 GRASS_TINT = 0xFF74A94B;           /* A3 gen_ground: t_grass is near-neutral (mean 184), tint = target (84,122,54) * 255 / 184 */

static const float PROJ_FAR[4]  = { 18000.0f, 18000.0f, 28000.0f, 28000.0f };
static const float PROJ_NEAR[4] = { 1.0f, 1.5f, 1.0f, 1.5f };

/* ---------------------------------------------------------------- small helpers */
static int iabs(int a) { return a < 0 ? -a : a; }
static int ch(u32 c, int k) { return (int)((c >> (16 - 8 * k)) & 0xFF); }   /* k 0 = r, 1 = g, 2 = b */
static int cdiff(u32 a, u32 b)                      /* max channel difference */
{
    int d = 0, k;
    for (k = 0; k < 3; k++) { int e = iabs(ch(a, k) - ch(b, k)); if (e > d) d = e; }
    return d;
}
static float luma(float r, float g, float b) { return 0.299f * r + 0.587f * g + 0.114f * b; }

static void shot_path(char *out, int n, const char *name)
{
    char rel[E_PATH];
    e_snprintf(rel, (int)sizeof(rel), "probe\\shots\\%s_%s.bmp", name, s_tag);
    base_path(out, n, rel);
}

static void save_img(const u32 *img, const char *name)
{
    char p[E_PATH];
    if (!s_shot) return;
    shot_path(p, (int)sizeof(p), name);
    img_save_bmp24(p, img, SW, SH);
    log_printf(LOG_INFO, "probe: shot file=%s", p);
}

/* 640x480 zoom of the (SW/k x SH/k) region centred at (cx, cy) */
static void save_zoom(const u32 *img, int cx, int cy, int k, const char *name)
{
    static u32 *z;
    if (!z) z = (u32 *)malloc(MAXPIX * 4);
    int x, y, w = SW / k, h = SH / k, x0 = cx - w / 2, y0 = cy - h / 2;
    if (!s_shot) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 + w > SW) x0 = SW - w;
    if (y0 + h > SH) y0 = SH - h;
    for (y = 0; y < SH; y++) for (x = 0; x < SW; x++) z[y * SW + x] = img[(y0 + y / k) * SW + x0 + x / k];
    save_img(z, name);
}

/* test frame dimmed, pixels that differ from the reference in red */
static void save_errvis(const u32 *test, const u32 *ref, const char *name, int zoomCx, int zoomCy)
{
    static u32 *v;
    if (!v) v = (u32 *)malloc(MAXPIX * 4);
    char zn[64];
    int i;
    if (!s_shot) return;
    for (i = 0; i < NPIX; i++) {
        u32 c = test[i];
        if ((c & 0xFFFFFF) != (ref[i] & 0xFFFFFF)) v[i] = 0xFFFF0000u;
        else v[i] = 0xFF000000u | (((c >> 1) & 0x7F7F7F) + 0x202020);
    }
    save_img(v, name);
    if (zoomCx >= 0) {
        e_snprintf(zn, (int)sizeof(zn), "%s_zoom", name);
        save_zoom(v, zoomCx, zoomCy, 4, zn);
    }
}

/* ---------------------------------------------------------------- camera, materials, geometry */
static void cam_apply(void)
{
    r_set_proj(&s_proj);
    r_set_view(&s_view);
    r_set_world(NULL);
}

static void cam_set(Vec3 eye, Vec3 at, float nearM, float farM)
{
    float aspect = (float)SW / (float)SH;
    float fovY = 2.0f * (float)atan(tan(E_DEG2RAD(s_fovH) * 0.5f) / aspect);
    s_eye = eye;
    s_near = nearM;
    s_far = farM;
    m4_persp_lh(&s_proj, fovY, aspect, nearM, farM);
    m4_look_lh(&s_view, eye, at, v3(0.0f, 1.0f, 0.0f));
    cam_apply();
}

/* world point -> screen pixel; 0 if behind the camera */
static int project(Vec3 p, float *sx, float *sy, float *w)
{
    Mat4 vp;
    float in[4], o[4];
    m4_mul(&vp, &s_view, &s_proj);
    in[0] = p.x; in[1] = p.y; in[2] = p.z; in[3] = 1.0f;
    m4_mul_vec4(&vp, in, o);
    if (o[3] <= 0.01f) return 0;
    *sx = (float)SW * 0.5f + o[0] / o[3] * (float)SW * 0.5f;
    *sy = (float)SH * 0.5f - o[1] / o[3] * (float)SH * 0.5f;
    if (w) *w = o[3];
    return 1;
}

static void mat(int blend, int zTest, int zWrite, int fog, int zBias, RTex tex)
{
    RMaterial m;
    memset(&m, 0, sizeof(m));
    m.tex0 = tex;
    m.blend = (u8)blend;
    m.zTest = (u8)zTest;
    m.zWrite = (u8)zWrite;
    m.fog = (u8)fog;
    m.zBias = (u8)zBias;
    m.cullNone = 1;
    r_set_material(&m);
}

static RVert1 s_gv[121 * 121];
static u16    s_gi[120 * 120 * 6];

/* bilinear patch p00 (u0,v0) .. p11 (u1,v1), nu x nv cells, one colour */
static void patch(Vec3 p00, Vec3 p10, Vec3 p01, Vec3 p11, int nu, int nv, u32 col, float u0, float v0, float u1, float v1)
{
    int i, j, n = 0, k = 0;
    if (nu < 1) nu = 1;
    if (nv < 1) nv = 1;
    if (nu > 120) nu = 120;
    if (nv > 120) nv = 120;
    for (j = 0; j <= nv; j++) {
        float b = (float)j / (float)nv;
        for (i = 0; i <= nu; i++) {
            float a = (float)i / (float)nu;
            RVert1 *v = &s_gv[n++];
            v->x = (p00.x * (1 - a) + p10.x * a) * (1 - b) + (p01.x * (1 - a) + p11.x * a) * b;
            v->y = (p00.y * (1 - a) + p10.y * a) * (1 - b) + (p01.y * (1 - a) + p11.y * a) * b;
            v->z = (p00.z * (1 - a) + p10.z * a) * (1 - b) + (p01.z * (1 - a) + p11.z * a) * b;
            v->color = col;
            v->u = u0 + (u1 - u0) * a;
            v->v = v0 + (v1 - v0) * b;
        }
    }
    for (j = 0; j < nv; j++) for (i = 0; i < nu; i++) {
        int q = j * (nu + 1) + i;
        s_gi[k++] = (u16)q; s_gi[k++] = (u16)(q + 1); s_gi[k++] = (u16)(q + nu + 2);
        s_gi[k++] = (u16)q; s_gi[k++] = (u16)(q + nu + 2); s_gi[k++] = (u16)(q + nu + 1);
    }
    r_draw_mem(RFMT_V1, s_gv, n, s_gi, k);
}

/* flat horizontal rectangle at height y */
static void flat(float x0, float z0, float x1, float z1, float y, int nu, int nv, u32 col, float uvPeriod, float uvOff)
{
    float s = uvPeriod > 0.0f ? 1.0f / uvPeriod : 0.0f;
    patch(v3(x0, y, z0), v3(x1, y, z0), v3(x0, y, z1), v3(x1, y, z1), nu, nv, col,
          uvOff + x0 * s, uvOff + z0 * s, uvOff + x1 * s, uvOff + z1 * s);
}

/* camera-facing square of half size h at p */
static void billboard(Vec3 p, float h, u32 col, float u0, float v0, float u1, float v1)
{
    Vec3 d = v3_norm(v3_sub(p, s_eye));
    Vec3 r = v3_norm(v3_cross(v3(0.0f, 1.0f, 0.0f), d));
    Vec3 up = v3_cross(d, r);
    Vec3 rr = v3_scale(r, h), uu = v3_scale(up, h);
    patch(v3_sub(v3_add(p, uu), rr), v3_add(v3_add(p, uu), rr), v3_sub(v3_sub(p, uu), rr), v3_add(v3_sub(p, uu), rr),
          1, 1, col, u0, v0, u1, v1);
}

static void draw_sky(void)
{
    WView wv;
    if (!s_skyOk) return;
    memset(&wv, 0, sizeof(wv));
    wv.eye = s_eye;
    wv.view = s_view;
    wv.proj = s_proj;
    wv.viewDist = s_far;
    world_draw_sky(&s_world, &wv);
    cam_apply();
}

/* Frame marker: 16 pixels at the top-left and bottom-left rows encode the frame number, so a GDI grab can tell that
   the windowed present (a Blt that runs asynchronously, top to bottom) has fully reached the screen. The grab
   blanks the marker pixels afterwards (test and reference frames then compare equal there). */
static unsigned s_frameNo;
enum { MARK_N = 16 };

static void label(void)
{
    int i;
    ui_begin(SW, SH);
    if (s_fontOk && s_label[0]) {
        ui_rect((float)SW - 4.0f - (float)font_width(&s_font, s_label) - 4.0f, (float)SH - 19.0f, (float)font_width(&s_font, s_label) + 6.0f,
                17.0f, 0xC0000000u);
        ui_text(&s_font, (float)SW - 6.0f, (float)SH - 18.0f, 0xFFFFFFFFu, 2, s_label);
    }
    ui_set_blend(RB_BLEND);
    for (i = 0; i < MARK_N; i++) {
        u32 c = (s_frameNo >> i) & 1 ? 0xFFFFFFFFu : 0xFF000000u;
        ui_rect((float)i, 0.0f, 1.0f, 1.0f, c);
        ui_rect((float)i, (float)(SH - 1), 1.0f, 1.0f, c);
    }
    ui_end();
}

/* ---------------------------------------------------------------- frames and grabs */
static int frame_begin(u32 clear)
{
    plat_pump();
    s_frameNo++;
    if (!r_begin_frame(clear)) {
        r_device_lost_check();
        if (!r_begin_frame(clear)) return 0;
    }
    return 1;
}

static int s_grabRetries, s_grabFails;

static int marker_ok(const u32 *p, unsigned no)
{
    int i;
    for (i = 0; i < MARK_N; i++) {
        int want = (no >> i) & 1;
        if (((p[i] & 0xFF) > 128) != want) return 0;
        if (((p[(SH - 1) * SW + i] & 0xFF) > 128) != want) return 0;
    }
    return 1;
}

static void gdi_grab(u32 *dst)
{
    int i, tries;
    for (tries = 0; tries < 40; tries++) {
        HDC dc = GetDC((HWND)plat_hwnd());
        BitBlt(s_memDC, 0, 0, SW, SH, dc, 0, 0, SRCCOPY);
        GdiFlush();
        ReleaseDC((HWND)plat_hwnd(), dc);
        if (marker_ok(s_dibBits, s_frameNo)) break;
        s_grabRetries++;
        Sleep(10);
    }
    if (tries >= 40) { s_grabFails++; log_printf(LOG_INFO, "probe: grab marker mismatch frame=%u", s_frameNo); }
    for (i = 0; i < NPIX; i++) dst[i] = 0xFF000000u | (s_dibBits[i] & 0xFFFFFF);
    for (i = 0; i < MARK_N; i++) { dst[i] = 0xFF000000u; dst[(SH - 1) * SW + i] = 0xFF000000u; }
}

static int file_grab(u32 *dst)
{
    EImage im;
    const char *tmp = "C:\\WINDOWS\\TEMP\\probe_t.bmp";
    int ok;
    ok = r_save_bmp(tmp);
    r_present();
    if (!ok || !img_load(&im, tmp, 0)) { memset(dst, 0, NPIX * 4); return 0; }
    if (im.w == SW && im.h == SH) memcpy(dst, im.pix[0], NPIX * 4);
    img_free(&im);
    for (ok = 0; ok < MARK_N; ok++) { dst[ok] = 0xFF000000u; dst[(SH - 1) * SW + ok] = 0xFF000000u; }
    return 1;
}

/* label, end the frame, present, and copy the frame into dst (NULL = no grab) */
static void frame_end(u32 *dst)
{
    label();
    r_end_frame();
    if (!dst) { r_present(); return; }
    s_grabs++;
    if (s_fs) { file_grab(dst); return; }
    r_present();
    gdi_grab(dst);
}

static int count_diff(const u32 *a, const u32 *b)
{
    int i, n = 0;
    for (i = 0; i < NPIX; i++) if ((a[i] & 0xFFFFFF) != (b[i] & 0xFFFFFF)) n++;
    return n;
}

/* pixels whose error state (test != ref) differs between frame A and frame B */
static int count_flips(const u32 *ta, const u32 *ra, const u32 *tb, const u32 *rb)
{
    int i, n = 0;
    for (i = 0; i < NPIX; i++) {
        int ea = (ta[i] & 0xFFFFFF) != (ra[i] & 0xFFFFFF), eb = (tb[i] & 0xFFFFFF) != (rb[i] & 0xFFFFFF);
        if (ea != eb) n++;
    }
    return n;
}

static int count_col(const u32 *a, u32 col, int x0, int y0, int x1, int y1)
{
    int x, y, n = 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SW - 1) x1 = SW - 1;
    if (y1 > SH - 1) y1 = SH - 1;
    for (y = y0; y <= y1; y++) for (x = x0; x <= x1; x++) if (cdiff(a[y * SW + x], col) <= 16) n++;
    return n;
}

static int count_diff_box(const u32 *a, const u32 *b, int x0, int y0, int x1, int y1, int thr)
{
    int x, y, n = 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SW - 1) x1 = SW - 1;
    if (y1 > SH - 1) y1 = SH - 1;
    for (y = y0; y <= y1; y++) for (x = x0; x <= x1; x++) if (cdiff(a[y * SW + x], b[y * SW + x]) > thr) n++;
    return n;
}

/* average colour of a box */
static void avg_box(const u32 *a, int x0, int y0, int x1, int y1, float *r, float *g, float *b)
{
    int x, y, n = 0;
    float sr = 0, sg = 0, sb = 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SW - 1) x1 = SW - 1;
    if (y1 > SH - 1) y1 = SH - 1;
    for (y = y0; y <= y1; y++) for (x = x0; x <= x1; x++) {
        u32 c = a[y * SW + x];
        sr += (float)ch(c, 0); sg += (float)ch(c, 1); sb += (float)ch(c, 2); n++;
    }
    if (n < 1) n = 1;
    *r = sr / (float)n; *g = sg / (float)n; *b = sb / (float)n;
}

/* ================================================================ scene 1: coastline */
static void s1_draw(float D, int ref, float camZ, Vec3 dir)
{
    float lx = 0.35f * D, X = 1.3f * s_far;
    Vec3 eye = v3(0.0f, 300.0f, camZ);
    frame_begin(COL_SKY & 0xFFFFFF);
    cam_set(eye, v3_add(eye, dir), s_near, s_far);
    r_set_fog(0, 0, 1.0f, 2.0f);
    if (!ref) {
        mat(RB_OPAQUE, 1, 1, 0, 0, 0);
        patch(v3(-lx, -50.0f, D - 400.0f), v3(lx, -50.0f, D - 400.0f), v3(-lx, 0.0f, D), v3(lx, 0.0f, D), 16, 4, COL_LAND, 0, 0, 0, 0);
        patch(v3(-lx, 0.0f, D), v3(lx, 0.0f, D), v3(-lx, 300.0f, D + 2400.0f), v3(lx, 300.0f, D + 2400.0f), 16, 12, COL_LAND, 0, 0, 0, 0);
        flat(-X, camZ - 2000.0f, X, s_far + 2000.0f, 0.0f, 60, 60, COL_SEA, 0.0f, 0.0f);
    } else {
        mat(RB_OPAQUE, 0, 0, 0, 0, 0);
        flat(-X, camZ - 2000.0f, X, s_far + 2000.0f, 0.0f, 60, 60, COL_SEA, 0.0f, 0.0f);
        patch(v3(-lx, 0.0f, D), v3(lx, 0.0f, D), v3(-lx, 300.0f, D + 2400.0f), v3(lx, 300.0f, D + 2400.0f), 16, 12, COL_LAND, 0, 0, 0, 0);
    }
}

static void scene1(void)
{
    static const float dist[5] = { 1000.0f, 3000.0f, 6000.0f, 12000.0f, 17000.0f };
    int p, d, x, y;
    for (p = 0; p < 4; p++) for (d = 0; d < 5; d++) {
        float D = dist[d];
        Vec3 dir = v3_norm(v3(0.0f, -300.0f, D));
        int errA, errB, flips, coast = 0, seaOnLand = 0, landOnSea = 0, i;
        s_near = PROJ_NEAR[p]; s_far = PROJ_FAR[p];
        e_snprintf(s_label, (int)sizeof(s_label), "S1 coast %s far %.0f near %.1f D %.0f", s_tag, s_far, s_near, D);
        s1_draw(D, 0, 0.0f, dir);    frame_end(s_img[0]);
        s1_draw(D, 1, 0.0f, dir);    frame_end(s_img[1]);
        s1_draw(D, 0, 8.333f, dir);  frame_end(s_img[2]);   /* 250 m/s x 1/30 s later */
        s1_draw(D, 1, 8.333f, dir);  frame_end(s_img[3]);
        errA = count_diff(s_img[0], s_img[1]);
        errB = count_diff(s_img[2], s_img[3]);
        flips = count_flips(s_img[0], s_img[1], s_img[2], s_img[3]);
        for (y = 0; y + 1 < SH; y++) for (x = 0; x < SW; x++) {
            i = y * SW + x;
            if (cdiff(s_img[1][i], COL_LAND) <= 16 && cdiff(s_img[1][i + SW], COL_SEA) <= 16) coast++;
        }
        for (i = 0; i < NPIX; i++) {
            u32 t = s_img[0][i], r = s_img[1][i];
            if (cdiff(t, COL_SEA) <= 16 && cdiff(r, COL_LAND) <= 16) seaOnLand++;
            if (cdiff(t, COL_LAND) <= 16 && cdiff(r, COL_SEA) <= 16) landOnSea++;
        }
        log_printf(LOG_INFO, "probe: s1 mode=%s far=%.0f near=%.1f dist=%.0f err_a=%d err_b=%d flips=%d coast_px=%d "
                   "sea_on_land=%d land_through_sea=%d", s_tag, s_far, s_near, D, errA, errB, flips, coast, seaOnLand, landOnSea);
        if (p == 0 || (p == 2 && d == 4)) {
            char n[64];
            e_snprintf(n, (int)sizeof(n), "s1_coast_f%d_d%02d", (int)(s_far / 1000.0f), (int)(D / 1000.0f));
            save_errvis(s_img[0], s_img[1], n, SW / 2, SH / 2);
        }
    }
}

/* ================================================================ scene 2: parallel quads */
static void s2_pair(Vec3 c, float h, int oblique, float sep, int which, u32 col)
{
    float th = E_DEG2RAD(70.0f);
    Vec3 a, up = v3(0.0f, h, 0.0f), n;
    float k = which ? 1.25f : 1.0f;
    if (oblique) { a = v3(h, 0.0f, (float)tan(th) * h); n = v3_norm(v3(-(float)sin(th), 0.0f, (float)cos(th))); }   /* 70 deg off facing */
    else { a = v3(h, 0.0f, 0.0f); n = v3(0.0f, 0.0f, 1.0f); }
    if (which) c = v3_madd(c, n, sep);
    a = v3_scale(a, k); up = v3_scale(up, k);
    patch(v3_sub(v3_add(c, up), a), v3_add(v3_add(c, up), a), v3_sub(v3_sub(c, up), a), v3_add(v3_sub(c, up), a),
          2, 2, col, 0, 0, 0, 0);
}

static void scene2(void)
{
    static const float dist[5] = { 100.0f, 1000.0f, 5000.0f, 10000.0f, 17000.0f };
    static const float sep[3] = { 0.5f, 2.0f, 5.0f };
    int p, d, o, i, r;
    for (p = 0; p < 4; p++) for (d = 0; d < 5; d++) for (o = 0; o < 2; o++) {
        float L = dist[d], h = 0.05f * L;
        Vec3 eye = v3(0.0f, 300.0f, 0.0f), c[3];
        int bx0[3], by0[3], bx1[3], by1[3];
        s_near = PROJ_NEAR[p]; s_far = PROJ_FAR[p];
        for (i = 0; i < 3; i++) c[i] = v3((-0.32f + 0.32f * (float)i) * L, 300.0f, L);
        e_snprintf(s_label, (int)sizeof(s_label), "S2 quads %s far %.0f near %.1f L %.0f %s", s_tag, s_far, s_near, L, o ? "oblique70" : "facing");
        for (r = 0; r < 2; r++) {
            frame_begin(COL_SKY & 0xFFFFFF);
            cam_set(eye, v3(0.0f, 300.0f, L), s_near, s_far);
            r_set_fog(0, 0, 1.0f, 2.0f);
            if (!r) {                               /* test: near first, far second, Z decides */
                mat(RB_OPAQUE, 1, 1, 0, 0, 0);
                for (i = 0; i < 3; i++) s2_pair(c[i], h, o, sep[i], 0, COL_NEAR);
                for (i = 0; i < 3; i++) s2_pair(c[i], h, o, sep[i], 1, COL_FAR);
            } else {                                /* reference: painter's order, no Z */
                mat(RB_OPAQUE, 0, 0, 0, 0, 0);
                for (i = 0; i < 3; i++) s2_pair(c[i], h, o, sep[i], 1, COL_FAR);
                for (i = 0; i < 3; i++) s2_pair(c[i], h, o, sep[i], 0, COL_NEAR);
            }
            frame_end(s_img[r]);
        }
        for (i = 0; i < 3; i++) {                   /* screen box of the near quad */
            float sx, sy, xs[4], ys[4], hx = h, hz = o ? (float)tan(E_DEG2RAD(70.0f)) * h : 0.0f;
            int k;
            Vec3 cn[4];
            cn[0] = v3(c[i].x - hx, c[i].y - h, c[i].z - hz); cn[1] = v3(c[i].x + hx, c[i].y - h, c[i].z + hz);
            cn[2] = v3(c[i].x - hx, c[i].y + h, c[i].z - hz); cn[3] = v3(c[i].x + hx, c[i].y + h, c[i].z + hz);
            for (k = 0; k < 4; k++) { project(cn[k], &sx, &sy, NULL); xs[k] = sx; ys[k] = sy; }
            bx0[i] = (int)E_MINF(E_MINF(xs[0], xs[1]), E_MINF(xs[2], xs[3])) - 1;
            bx1[i] = (int)E_MAXF(E_MAXF(xs[0], xs[1]), E_MAXF(xs[2], xs[3])) + 1;
            by0[i] = (int)E_MINF(E_MINF(ys[0], ys[1]), E_MINF(ys[2], ys[3])) - 1;
            by1[i] = (int)E_MAXF(E_MAXF(ys[0], ys[1]), E_MAXF(ys[2], ys[3])) + 1;
        }
        for (i = 0; i < 3; i++) {
            int nearPx = count_col(s_img[1], COL_NEAR, bx0[i], by0[i], bx1[i], by1[i]);
            int err = count_diff_box(s_img[0], s_img[1], bx0[i], by0[i], bx1[i], by1[i], 0);
            log_printf(LOG_INFO, "probe: s2 mode=%s far=%.0f near=%.1f dist=%.0f orient=%s sep=%.1f near_px=%d err=%d leak_pct=%.2f",
                       s_tag, s_far, s_near, L, o ? "oblique70" : "facing", sep[i], nearPx, err,
                       nearPx ? 100.0 * (double)err / (double)nearPx : 0.0);
        }
        if (p == 0 && (d == 1 || d == 3 || d == 4)) {
            char n[64];
            e_snprintf(n, (int)sizeof(n), "s2_quads_d%05d_%s", (int)L, o ? "obl" : "face");
            save_errvis(s_img[0], s_img[1], n, -1, -1);
        }
    }
}

/* ================================================================ scene 3: runway decal over flat ground */
static void scene3(void)
{
    static const float dist[3] = { 500.0f, 2000.0f, 4000.0f };
    int p, d, zb, view, r;
    for (p = 0; p < 4; p++) for (d = 0; d < 3; d++) for (view = 0; view < 2; view++) for (zb = 0; zb < 3; zb++) {
        float D = dist[d], hgt = view ? 600.0f : D * (float)tan(E_DEG2RAD(3.0f)), X = 1.3f * PROJ_FAR[p];
        Vec3 eye = v3(0.0f, hgt, 0.0f);
        int err, rwy;
        s_near = PROJ_NEAR[p]; s_far = PROJ_FAR[p];
        e_snprintf(s_label, (int)sizeof(s_label), "S3 runway %s far %.0f near %.1f D %.0f %s zbias %d", s_tag, s_far, s_near, D,
                   view ? "h600" : "glide3", zb);
        for (r = 0; r < 2; r++) {
            frame_begin(COL_SKY & 0xFFFFFF);
            cam_set(eye, v3(0.0f, 0.0f, D), s_near, s_far);
            r_set_fog(0, 0, 1.0f, 2.0f);
            mat(RB_OPAQUE, r ? 0 : 1, r ? 0 : 1, 0, 0, 0);
            flat(-X, -500.0f, X, s_far + 1000.0f, 0.0f, 60, 60, COL_LAND, 0.0f, 0.0f);
            mat(RB_OPAQUE, r ? 0 : 1, r ? 0 : 1, 0, r ? 0 : zb, 0);
            flat(-22.5f, D - 300.0f, 22.5f, D + 2500.0f, 0.2f, 1, 56, COL_RWY, 0.0f, 0.0f);
            frame_end(s_img[r]);
        }
        err = count_diff(s_img[0], s_img[1]);
        rwy = count_col(s_img[1], COL_RWY, 0, 0, SW - 1, SH - 1);
        log_printf(LOG_INFO, "probe: s3 mode=%s far=%.0f near=%.1f dist=%.0f view=%s height=%.0f zbias=%d runway_px=%d err=%d err_pct=%.2f",
                   s_tag, s_far, s_near, D, view ? "h600" : "glide3", hgt, zb, rwy, err, rwy ? 100.0 * (double)err / (double)rwy : 0.0);
        if (p == 0 && view == 0 && (zb == 0 || zb == 1) && (d == 1 || d == 2)) {
            char n[64];
            float sx, sy;
            project(v3(0.0f, 0.0f, D + 600.0f), &sx, &sy, NULL);
            e_snprintf(n, (int)sizeof(n), "s3_rwy_d%04d_zb%d", (int)D, zb);
            save_errvis(s_img[0], s_img[1], n, (int)sx, (int)sy);
        }
    }
}

/* ================================================================ scene 4: fog */
static u32 fog_rgb(void) { return s_skyOk ? (s_world.sky.fog & 0xFFFFFF) : (COL_SKY & 0xFFFFFF); }

static void scene4(void)
{
    static const float pfar[4] = { 18000.0f, 28000.0f, 18000.0f, 28000.0f }, pst[4] = { 3600.0f, 5600.0f, 3600.0f, 5600.0f };
    static const float pen[4] = { 18000.0f, 28000.0f, 16200.0f, 25200.0f };   /* 2, 3: fog end trimmed to 0.9 x far */
    static const char *const pn[4] = { "Low", "High", "LowTrim", "HighTrim" };
    static const float alt[4] = { 100.0f, 1000.0f, 4000.0f, 4000.0f }, pitch[4] = { 0.0f, 0.0f, 0.0f, -8.0f };
    int ps, k, a;
    u32 fog = fog_rgb();
    float fr = (float)((fog >> 16) & 0xFF), fg = (float)((fog >> 8) & 0xFF), fb = (float)(fog & 0xFF);
    for (ps = 0; ps < 4; ps++) {
        float st = pst[ps], en = pen[ps], fa = pfar[ps], dl[8];   /* view depth W of each billboard */
        Vec3 eye = v3(0.0f, 500.0f, 0.0f);
        dl[0] = 0.5f * st; dl[1] = st; dl[2] = st + 0.2f * (en - st); dl[3] = st + 0.4f * (en - st);
        dl[4] = st + 0.6f * (en - st); dl[5] = st + 0.8f * (en - st); dl[6] = 0.97f * fa; dl[7] = 0.995f * fa;
        /* billboard k at view depth dl[k] exactly: eye + W * (tan az, tan el, 1) */
        /* (a) fog ramp: black billboards at known view depth, clear = fog colour */
        e_snprintf(s_label, (int)sizeof(s_label), "S4 fog ramp %s %s fog %.0f-%.0f far %.0f", s_tag, pn[ps], st, en, fa);
        frame_begin(fog);
        cam_set(eye, v3(0.0f, 500.0f, 1000.0f), 1.0f, fa);
        r_set_fog(1, fog, st, en);
        mat(RB_OPAQUE, 1, 1, 1, 0, 0);
        for (k = 0; k < 8; k++) {
            float az = E_DEG2RAD(-28.0f + 8.0f * (float)k), el = E_DEG2RAD(4.0f);
            Vec3 pnt = v3_add(eye, v3(dl[k] * (float)tan(az), dl[k] * (float)tan(el), dl[k]));
            billboard(pnt, 0.014f * dl[k], 0xFF000000u, 0, 0, 0, 0);
        }
        frame_end(s_img[0]);
        for (k = 0; k < 8; k++) {
            float az = E_DEG2RAD(-28.0f + 8.0f * (float)k), el = E_DEG2RAD(4.0f), sx, sy, w, r, g, b, expv, meas;
            Vec3 pnt = v3_add(eye, v3(dl[k] * (float)tan(az), dl[k] * (float)tan(el), dl[k]));
            project(pnt, &sx, &sy, &w);
            avg_box(s_img[0], (int)sx - 2, (int)sy - 2, (int)sx + 2, (int)sy + 2, &r, &g, &b);
            expv = E_CLAMP((w - st) / (en - st), 0.0f, 1.0f);
            meas = (r / fr + g / fg + b / fb) / 3.0f;
            log_printf(LOG_INFO, "probe: s4 fogramp mode=%s preset=%s w_set=%.0f w=%.0f expected=%.3f measured=%.3f rgb=%.0f,%.0f,%.0f",
                       s_tag, pn[ps], dl[k], w, expv, meas, r, g, b);
        }
        { char n[40]; e_snprintf(n, (int)sizeof(n), "s4_fogramp_%s", pn[ps]); save_img(s_img[0], n); }
        /* (b) panorama sky + textured, fogged sea to beyond the far plane: seam at the sea's far edge? */
        for (a = 0; a < 4; a++) {
            float pr = E_DEG2RAD(pitch[a]), hx, hy, ex, ey, maxStep = 0.0f, maxDev = 0.0f, prev[3], maxFogErr = 0.0f;
            int y, y0, y1, first = 1, edgeRow, horRow;
            Vec3 e2 = v3(0.0f, alt[a], 0.0f);
            e_snprintf(s_label, (int)sizeof(s_label), "S4 sea/sky %s %s alt %.0f pitch %.0f", s_tag, pn[ps], alt[a], pitch[a]);
            frame_begin(fog);
            cam_set(e2, v3_add(e2, v3(0.0f, (float)sin(pr), (float)cos(pr))), 1.0f, fa);
            draw_sky();
            r_set_fog(1, fog, st, en);
            mat(RB_OPAQUE, 1, 1, 1, 0, s_texSea);
            flat(-1.5f * fa, -3000.0f, 1.5f * fa, 1.6f * fa, 0.0f, 64, 64, 0xFFFFFFFFu, 256.0f, 0.0f);
            frame_end(s_img[0]);
            project(v3(0.0f, alt[a], 1.0e6f), &hx, &hy, NULL);
            project(v3(0.0f, 0.0f, fa), &ex, &ey, NULL);
            horRow = (int)hy; edgeRow = (int)ey;
            y0 = (horRow < edgeRow ? horRow : edgeRow) - 6;
            y1 = (horRow > edgeRow ? horRow : edgeRow) + 6;
            if (y0 < 0) y0 = 0;
            if (y1 > SH - 1) y1 = SH - 1;
            if (y1 < edgeRow + 10) y1 = edgeRow + 10 < SH - 1 ? edgeRow + 10 : SH - 1;
            for (y = y0; y <= y1; y++) {
                float r, g, b, st2, dv, fy = (float)SH * 0.5f / ((float)tan(E_DEG2RAD(s_fovH) * 0.5f) * (float)SH / (float)SW);
                float dy = (float)sin(pr) - (float)cos(pr) * ((float)y + 0.5f - (float)SH * 0.5f) / fy, W, fexp, fmeas, ls, lf;
                avg_box(s_img[0], 120, y, 520, y, &r, &g, &b);
                /* expected fog of the sea at this row's centre (W = view depth of the hit; no hit = sky = fog colour) */
                W = dy < -1e-6f ? -alt[a] / dy : 1.0e9f;
                fexp = E_CLAMP((W - st) / (en - st), 0.0f, 1.0f);
                ls = luma(s_seaMean[0], s_seaMean[1], s_seaMean[2]); lf = luma(fr, fg, fb);
                fmeas = (luma(r, g, b) - ls) / (lf - ls);
                if (W > 0.75f * fa && W < 1.0e8f && (float)fabs(fmeas - fexp) > maxFogErr) maxFogErr = (float)fabs(fmeas - fexp);
                log_printf(LOG_INFO, "probe: s4 row mode=%s preset=%s alt=%.0f pitch=%.0f y=%d w=%.0f fog_expected=%.3f fog_measured=%.3f "
                           "rgb=%.1f,%.1f,%.1f", s_tag, pn[ps], alt[a], pitch[a], y, W < 1.0e8f ? W : -1.0f, fexp, fmeas, r, g, b);
                dv = E_MAXF(E_MAXF((float)fabs(r - fr), (float)fabs(g - fg)), (float)fabs(b - fb));
                if (dv > maxDev) maxDev = dv;
                if (!first) {
                    st2 = E_MAXF(E_MAXF((float)fabs(r - prev[0]), (float)fabs(g - prev[1])), (float)fabs(b - prev[2]));
                    if (st2 > maxStep) maxStep = st2;
                }
                prev[0] = r; prev[1] = g; prev[2] = b; first = 0;
            }
            {
                float r, g, b, r2, g2, b2;
                avg_box(s_img[0], 120, edgeRow + 1, 520, edgeRow + 3, &r, &g, &b);   /* sea just inside the far edge */
                avg_box(s_img[0], 120, edgeRow - 3, 520, edgeRow - 1, &r2, &g2, &b2); /* beyond it (sky lower band or clipped) */
                log_printf(LOG_INFO, "probe: s4 seasky mode=%s preset=%s alt=%.0f pitch=%.0f horizon_row=%d edge_row=%d rows=%d..%d "
                           "max_row_step=%.1f max_dev_from_fog=%.1f max_fog_err_near_far=%.3f sea_edge_rgb=%.1f,%.1f,%.1f beyond_rgb=%.1f,%.1f,%.1f "
                           "fog=%.0f,%.0f,%.0f", s_tag, pn[ps], alt[a], pitch[a], horRow, edgeRow, y0, y1, maxStep, maxDev,
                           maxFogErr, r, g, b, r2, g2, b2, fr, fg, fb);
            }
            {
                char n[64];
                e_snprintf(n, (int)sizeof(n), "s4_seasky_%s_a%04d_p%d", pn[ps], (int)alt[a], (int)-pitch[a]);
                save_img(s_img[0], n);
                if (a == 2 || a == 0) {
                    e_snprintf(n, (int)sizeof(n), "s4_seasky_%s_a%04d_p%d_zoom", pn[ps], (int)alt[a], (int)-pitch[a]);
                    save_zoom(s_img[0], SW / 2, edgeRow, 4, n);
                }
            }
        }
    }
}

/* ================================================================ scene 5: UV magnitudes */
static void scene5(void)
{
    /* a 800 x 1200 m ground patch, 32 m texture period (uv span 25 x 37.5), seen from 150 m at -30 deg; every vertex
       uv shifted by an integer M (identical image with exact texture maths) and compared with M = 0 */
    static const float off[9] = { 0.0f, 8.0f, 32.0f, 64.0f, 128.0f, 256.0f, 512.0f, 1024.0f, 2048.0f };
    int k, i;
    for (k = 0; k < 9; k++) {
        Vec3 eye = v3(0.0f, 150.0f, 0.0f);
        int nd, nd24;
        float sum = 0.0f;
        e_snprintf(s_label, (int)sizeof(s_label), "S5 uv %s offset %.0f (uv %.0f..%.0f)", s_tag, off[k], off[k] - 12.5f, off[k] + 37.5f);
        frame_begin(COL_SKY & 0xFFFFFF);
        cam_set(eye, v3_add(eye, v3(0.0f, -(float)sin(E_DEG2RAD(30.0f)), (float)cos(E_DEG2RAD(30.0f)))), 1.0f, 18000.0f);
        r_set_fog(0, 0, 1.0f, 2.0f);
        mat(RB_OPAQUE, 1, 1, 0, 0, s_texGrass);
        flat(-400.0f, 0.0f, 400.0f, 1200.0f, 0.0f, 16, 24, GRASS_TINT, 32.0f, off[k]);
        frame_end(s_img[k == 0 ? 4 : 0]);
        if (k > 0) {
            nd = 0; nd24 = 0;
            for (i = 0; i < NPIX; i++) {
                int dd = cdiff(s_img[0][i], s_img[4][i]);
                if (dd > 8) nd++;
                if (dd > 24) nd24++;
                sum += (float)dd;
            }
            log_printf(LOG_INFO, "probe: s5 uv mode=%s offset=%.0f max_uv=%.1f diff_px_gt8=%d diff_px_gt24=%d mean_maxch_diff=%.2f",
                       s_tag, off[k], off[k] + 37.5f, nd, nd24, sum / (float)NPIX);
        }
        if (k == 0 || k == 3 || k == 4 || k == 6 || k == 8) {
            char n[48];
            e_snprintf(n, (int)sizeof(n), "s5_uv_off%04d", (int)off[k]);
            save_img(s_img[k == 0 ? 4 : 0], n);
            e_snprintf(n, (int)sizeof(n), "s5_uv_off%04d_zoom", (int)off[k]);
            save_zoom(s_img[k == 0 ? 4 : 0], SW / 2, 300, 4, n);
        }
    }
}

/* ================================================================ scene 6: aircraft visibility */
static void jet_draw(Vec3 pos, int aspect, float dist)
{
    Mat3 r;
    Mat4 w;
    Vec3 sunB, sunW = s_skyOk ? s_world.sky.sunDir : v3_norm(v3(-0.59f, 0.64f, -0.49f));
    u32 sun = s_skyOk ? s_world.sky.sun : 0xFFF4E2, amb = s_skyOk ? s_world.sky.ambient : 0x8496B2;
    int lod = dist < 300.0f ? 0 : (dist < 900.0f ? 1 : 2);
    if (lod >= s_jet.numLods) lod = s_jet.numLods - 1;
    m3_ident(&r);
    if (aspect == 0) {                              /* side: heading east, seen broadside from the south */
        r.m[0][0] = 0; r.m[0][1] = 0; r.m[0][2] = -1;   /* right = -z (toward camera) */
        r.m[1][0] = 0; r.m[1][1] = 1; r.m[1][2] = 0;
        r.m[2][0] = 1; r.m[2][1] = 0; r.m[2][2] = 0;
    } else if (aspect == 1) {                       /* tail: heading north, away from the camera */
        /* identity */
    } else {                                        /* plan: heading east, rolled 90 deg (wings vertical), top toward camera */
        r.m[0][0] = 0; r.m[0][1] = -1; r.m[0][2] = 0;   /* right wing down */
        r.m[1][0] = 0; r.m[1][1] = 0; r.m[1][2] = -1;   /* top toward the camera (-z) */
        r.m[2][0] = 1; r.m[2][1] = 0; r.m[2][2] = 0;
    }
    m4_from_m3_pos(&w, &r, pos);
    sunB = m3_tmul(&r, sunW);
    mesh_draw_lit(&s_jet, &w, lod, 0, sunB, sun, amb, 0, &s_view);
}

static void scene6(void)
{
    static const float dist[4] = { 1000.0f, 2500.0f, 4000.0f, 6000.0f }, az[4] = { -21.0f, -7.0f, 7.0f, 21.0f };
    static const char *const an[3] = { "side", "tail", "plan" };
    u32 fog = fog_rgb();
    int asp, k, bg;
    Vec3 eye = v3(0.0f, 1500.0f, 0.0f);
    if (!s_jetOk) { log_printf(LOG_INFO, "probe: s6 skipped (jet mesh not loaded)"); return; }
    log_printf(LOG_INFO, "probe: s6 jet box=%.2f,%.2f,%.2f..%.2f,%.2f,%.2f lods=%d lod_tris=%d,%d,%d", s_jet.box.lo.x, s_jet.box.lo.y,
               s_jet.box.lo.z, s_jet.box.hi.x, s_jet.box.hi.y, s_jet.box.hi.z, s_jet.numLods, mesh_lod_tris(&s_jet, 0),
               s_jet.numLods > 1 ? mesh_lod_tris(&s_jet, 1) : 0, s_jet.numLods > 2 ? mesh_lod_tris(&s_jet, 2) : 0);
    for (asp = 0; asp < 3; asp++) {
        for (bg = 1; bg >= 0; bg--) {               /* background-only frame first, then with the jets */
            e_snprintf(s_label, (int)sizeof(s_label), "S6 jets %s %s 1 / 2.5 / 4 / 6 km, fog 3.6-18 km", s_tag, an[asp]);
            frame_begin(fog);
            cam_set(eye, v3(0.0f, 1500.0f, 1000.0f), 1.0f, 18000.0f);
            draw_sky();
            r_set_fog(1, fog, 3600.0f, 18000.0f);
            mat(RB_OPAQUE, 1, 1, 1, 0, s_texSea);
            flat(-27000.0f, -3000.0f, 27000.0f, 29000.0f, 0.0f, 64, 64, 0xFFFFFFFFu, 256.0f, 0.0f);
            if (!bg) for (k = 0; k < 4; k++) {
                float a = E_DEG2RAD(az[k]);
                jet_draw(v3(dist[k] * (float)sin(a), 1500.0f, dist[k] * (float)cos(a)), asp, dist[k]);
            }
            frame_end(s_img[bg ? 1 : 0]);
        }
        for (k = 0; k < 4; k++) {
            float a = E_DEG2RAD(az[k]), sx, sy, lenPx;
            int n10, n40;
            project(v3(dist[k] * (float)sin(a), 1500.0f, dist[k] * (float)cos(a)), &sx, &sy, NULL);
            n10 = count_diff_box(s_img[0], s_img[1], (int)sx - 40, (int)sy - 40, (int)sx + 40, (int)sy + 40, 10);
            n40 = count_diff_box(s_img[0], s_img[1], (int)sx - 40, (int)sy - 40, (int)sx + 40, (int)sy + 40, 40);
            lenPx = 15.27f * ((float)SW * 0.5f / (float)tan(E_DEG2RAD(s_fovH) * 0.5f)) / dist[k];
            log_printf(LOG_INFO, "probe: s6 jet mode=%s aspect=%s dist=%.0f lod=%d px_diff_gt10=%d px_diff_gt40=%d len_px_expected=%.1f",
                       s_tag, an[asp], dist[k], dist[k] < 300.0f ? 0 : (dist[k] < 900.0f ? 1 : 2), n10, n40, lenPx);
        }
        {
            char n[48];
            float a = E_DEG2RAD(az[1]), sx, sy;
            e_snprintf(n, (int)sizeof(n), "s6_jets_%s", an[asp]);
            save_img(s_img[0], n);
            project(v3(dist[1] * (float)sin(a), 1500.0f, dist[1] * (float)cos(a)), &sx, &sy, NULL);
            e_snprintf(n, (int)sizeof(n), "s6_jets_%s_zoom", an[asp]);
            save_zoom(s_img[0], SW / 2, (int)sy, 4, n);
        }
    }
    /* dots: 2x2 px dark sprite (pre-fogged, alpha fading 2.5 -> 4 km) next to LOD2 meshes at the same distances */
    {
        static const float dd[4] = { 2500.0f, 3000.0f, 3500.0f, 4000.0f };
        float fr = (float)((fog >> 16) & 0xFF), fg = (float)((fog >> 8) & 0xFF), fb = (float)(fog & 0xFF);
        e_snprintf(s_label, (int)sizeof(s_label), "S6 dots %s: mesh (top) vs 2x2 dot (below) at 2.5/3/3.5/4 km", s_tag);
        frame_begin(fog);
        cam_set(eye, v3(0.0f, 1500.0f, 1000.0f), 1.0f, 18000.0f);
        draw_sky();
        r_set_fog(1, fog, 3600.0f, 18000.0f);
        mat(RB_OPAQUE, 1, 1, 1, 0, s_texSea);
        flat(-27000.0f, -3000.0f, 27000.0f, 29000.0f, 0.0f, 64, 64, 0xFFFFFFFFu, 256.0f, 0.0f);
        for (k = 0; k < 4; k++) {
            float a = E_DEG2RAD(az[k]);
            jet_draw(v3(dd[k] * (float)sin(a), 1500.0f + dd[k] * 0.02f, dd[k] * (float)cos(a)), 0, dd[k]);
        }
        ui_begin(SW, SH);
        ui_set_blend(RB_BLEND);
        for (k = 0; k < 4; k++) {
            float a = E_DEG2RAD(az[k]), sx, sy, f = E_CLAMP((dd[k] - 3600.0f) / (18000.0f - 3600.0f), 0.0f, 1.0f);
            float al = E_CLAMP(1.0f - (dd[k] - 2500.0f) / 1500.0f, 0.0f, 1.0f);
            int r = (int)(0x40 + (fr - 0x40) * f), g = (int)(0x44 + (fg - 0x44) * f), b = (int)(0x48 + (fb - 0x48) * f);
            if (al < 0.15f) al = 0.15f;             /* the 4 km one would be invisible: show the last visible step */
            project(v3(dd[k] * (float)sin(a), 1500.0f - dd[k] * 0.02f, dd[k] * (float)cos(a)), &sx, &sy, NULL);
            ui_rect((float)(int)sx, (float)(int)sy, 2.0f, 2.0f, ((u32)(al * 255.0f) << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b);
        }
        ui_end();
        frame_end(s_img[0]);
        save_img(s_img[0], "s6_dots");
        save_zoom(s_img[0], SW / 2, SH / 2, 4, "s6_dots_zoom");
    }
}

/* ================================================================ HUD atlas (A4) */
typedef struct AGlyph { short x, y, w, h, ox, adv, ok; } AGlyph;
typedef struct AFont { int cap, cellH, oy, adv, spaceAdv, gap; AGlyph g[128]; } AFont;
typedef struct ASpr { char name[24]; int x, y, w, h, ox, oy; } ASpr;
static AFont s_af[2];
static ASpr  s_spr[96];
static int   s_nspr;

static int atlas_load(void)
{
    char p[E_PATH], *txt, *line, *next;
    int size;
    data_path(p, (int)sizeof(p), "tex\\hud_atlas.txt");
    txt = (char *)file_load(p, &size);
    if (!txt) { log_printf(LOG_ERR, "probe: atlas txt missing %s", p); return 0; }
    for (line = txt; line && *line; line = next) {
        char name[32];
        int a[9], n;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (line[0] == '#' || line[0] == '\r' || !line[0]) continue;
        if (sscanf(line, "font %d %d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]) == 7) {
            if (a[0] >= 0 && a[0] < 2) {
                AFont *f = &s_af[a[0]];
                f->cap = a[1]; f->cellH = a[2]; f->oy = a[3]; f->adv = a[4]; f->spaceAdv = a[5]; f->gap = a[6];
            }
            continue;
        }
        if (sscanf(line, "g %d %d %d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7]) == 8) {
            if (a[0] >= 0 && a[0] < 2 && a[1] > 0 && a[1] < 128) {
                AGlyph *g = &s_af[a[0]].g[a[1]];
                g->x = (short)a[2]; g->y = (short)a[3]; g->w = (short)a[4]; g->h = (short)a[5]; g->ox = (short)a[6];
                g->adv = (short)a[7]; g->ok = 1;
            }
            continue;
        }
        n = sscanf(line, "%31s %d %d %d %d %d %d", name, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
        if (n == 7 && s_nspr < E_COUNT(s_spr)) {
            ASpr *s = &s_spr[s_nspr++];
            e_strlcpy(s->name, name, (int)sizeof(s->name));
            s->x = a[0]; s->y = a[1]; s->w = a[2]; s->h = a[3]; s->ox = a[4]; s->oy = a[5];
        }
    }
    free(txt);
    /* NOT tex_get(.., IMG_ALPHA, ..): img_load's IMG_ALPHA path runs img_bleed, which fills alpha-0 texels with the
       colour of their opaque neighbours (white here) and so breaks the premultiplied atlas (glyph cells come out as
       solid blocks in ADD/PREMUL). Load RGB and alpha without flags and compose. */
    {
        EImage rgb, al;
        int i, ok = 0;
        if (img_load(&rgb, "tex/hud_atlas.bmp", 0)) {
            if (img_load(&al, "tex/hud_atlas_a.bmp", 0)) {
                if (al.w == rgb.w && al.h == rgb.h) {
                    for (i = 0; i < rgb.w * rgb.h; i++) rgb.pix[0][i] = (rgb.pix[0][i] & 0xFFFFFFu) | ((al.pix[0][i] >> 16 & 0xFF) << 24);
                    ok = 1;
                }
                img_free(&al);
            }
            rgb.flags = IMG_ALPHA;
            if (ok) s_atlas = tex_add("hud_atlas_premul", &rgb, RT_CLAMP | RT_NOMIP);
            img_free(&rgb);
        }
    }
    log_printf(LOG_INFO, "probe: atlas sprites=%d font0 cap=%d adv=%d font1 cap=%d adv=%d tex=%d", s_nspr, s_af[0].cap, s_af[0].adv,
               s_af[1].cap, s_af[1].adv, s_atlas);
    return s_atlas != 0;
}

static float s_hox, s_hoy;                          /* shadow offset */
static int   s_shadowPass;
static u32   s_shadowCol = 0x80000000u;
static u32   hc(u32 c) { return s_shadowPass ? s_shadowCol : c; }

static float atext(int f, float X, float Y, u32 argb, const char *s)
{
    const AFont *af = &s_af[f];
    for (; *s; s++) {
        int c = (unsigned char)*s;
        const AGlyph *g;
        if (c >= 'a' && c <= 'z' && c != 'c') c -= 32;
        if (c >= 128 || !af->g[c].ok) c = '?';
        g = &af->g[c];
        if (g->w > 0)
            ui_image(s_atlas, X + (float)g->ox + s_hox, Y + (float)af->oy + s_hoy, (float)g->w, (float)g->h,
                     (float)g->x / 256.0f, (float)g->y / 256.0f, (float)(g->x + g->w) / 256.0f, (float)(g->y + g->h) / 256.0f, hc(argb));
        X += (float)g->adv;
    }
    return X;
}

static float atext_w(int f, const char *s)
{
    float w = 0.0f;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c >= 'a' && c <= 'z' && c != 'c') c -= 32;
        if (c >= 128 || !s_af[f].g[c].ok) c = '?';
        w += (float)s_af[f].g[c].adv;
    }
    return w > 0.0f ? w - (float)s_af[f].gap : 0.0f;
}

static void aspr(const char *name, float ax, float ay, u32 argb)
{
    int i;
    for (i = 0; i < s_nspr; i++) {
        const ASpr *s = &s_spr[i];
        if (strcmp(s->name, name)) continue;
        ui_image(s_atlas, ax - (float)s->ox + s_hox, ay - (float)s->oy + s_hoy, (float)s->w, (float)s->h,
                 (float)s->x / 256.0f, (float)s->y / 256.0f, (float)(s->x + s->w) / 256.0f, (float)(s->y + s->h) / 256.0f, hc(argb));
        return;
    }
}

static void hl(float x0, float y0, float x1, float y1, float w, u32 c)
{
    ui_line(x0 + s_hox, y0 + s_hoy, x1 + s_hox, y1 + s_hoy, w, hc(c));
}

static const u32 HUD_G = 0xFF40FF60u, HUD_Y = 0xFFFFE040u, HUD_R = 0xFFFF3030u;

/* draws content twice: shadow pass (RB_BLEND, black, +1,+1) when shadowA > 0, then the HUD pass in `blend` */
/* s_hudNative 0: the 640x480 virtual space scaled to the screen (ui_begin(640,480): x1.25 at 800x600, bilinear);
   1: atlas 1:1 in screen pixels, the 640x480 layout centred (HUD 20 % smaller on screen at 800x600) */
static int   s_hudNative;
static float s_hbx, s_hby;                          /* base offset of the layout (native mode) */

static void hud_begin(void)
{
    if (s_hudNative) { ui_begin(SW, SH); s_hbx = (float)((SW - 640) / 2); s_hby = (float)((SH - 480) / 2); }
    else { ui_begin(640, 480); s_hbx = 0.0f; s_hby = 0.0f; }
}

static void hud_passes(void (*content)(void), int blend, u32 shadowA)
{
    hud_begin();
    if (shadowA) {
        ui_set_blend(RB_BLEND);
        s_shadowPass = 1; s_shadowCol = shadowA << 24; s_hox = s_hbx + 1.0f; s_hoy = s_hby + 1.0f;
        content();
    }
    ui_set_blend(blend);
    s_shadowPass = 0; s_hox = s_hbx; s_hoy = s_hby;
    content();
    ui_end();
}

/* layout (virtual 640x480) -> screen pixel */
static int hud_sx(float x) { return s_hudNative ? (int)(x + (float)((SW - 640) / 2)) : (int)(x * (float)SH / 480.0f + ((float)SW - 640.0f * (float)SH / 480.0f) * 0.5f); }
static int hud_sy(float y) { return s_hudNative ? (int)(y + (float)((SH - 480) / 2)) : (int)(y * (float)SH / 480.0f); }

/* 3D contact marker (coordinator 2026-11-13): small hollow diamond, corner down, yellow unknown / red hostile */
static void diamond(float x, float y, float r, u32 c)
{
    hl(x, y - r, x + r, y, 2.0f, c); hl(x + r, y, x, y + r, 2.0f, c);
    hl(x, y + r, x - r, y, 2.0f, c); hl(x - r, y, x, y - r, 2.0f, c);
}

/* -------- 7a: line/text/symbol bands */
static const u32 BAND_FLAT[4] = { 0xFFC2D3E7u, 0xFFF4F6FAu, 0xFF1E466Eu, 0xFF466432u };   /* haze sky, cloud, sea, grass */
static const char *const BAND_NAME[4] = { "sky", "cloud", "sea", "terrain" };

static void bands_bg(void)
{
    int b, x;
    ui_begin(640, 480);                             /* 7a runs in the scaled HUD space only */
    for (b = 0; b < 4; b++) {
        float y0 = (float)(b * 120);
        ui_rect(0.0f, y0, 300.0f, 120.0f, BAND_FLAT[b]);
        if (b <= 1) {                               /* panorama tile near the sun (bright low sky) */
            if (s_skyOk) {
                ui_image(s_world.skyTex[2 % (s_world.skyTiles ? s_world.skyTiles : 1)], 300.0f, y0, 170.0f, 120.0f, 0.0f, 0.55f, 1.0f, 0.93f, 0xFFFFFFFFu);
                ui_image(s_world.skyTex[3 % (s_world.skyTiles ? s_world.skyTiles : 1)], 470.0f, y0, 170.0f, 120.0f, 0.0f, 0.55f, 1.0f, 0.93f, 0xFFFFFFFFu);
            } else ui_rect(300.0f, y0, 340.0f, 120.0f, BAND_FLAT[0]);
        }
        if (b == 1 && s_texCloud) {                 /* cumulus cells stretched: mostly white with soft edges */
            ui_image(s_texCloud, 290.0f, y0 - 40.0f, 200.0f, 200.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0xFFFFFFFFu);
            ui_image(s_texCloud, 440.0f, y0 - 50.0f, 220.0f, 220.0f, 0.5f, 0.5f, 1.0f, 1.0f, 0xFFFFFFFFu);
        }
        if (b >= 2) {
            RTex t = b == 2 ? s_texSea : s_texGrass;
            for (x = 300; x < SW; x += 256) ui_image(t, (float)x, y0, 256.0f, 120.0f, 0.0f, 0.0f, 1.0f, 120.0f / 256.0f, b == 2 ? 0xFFFFFFFFu : GRASS_TINT);
        }
    }
    ui_end();
}

static void bands_content(void)
{
    int b;
    for (b = 0; b < 4; b++) {
        float y0 = (float)(b * 120);
        hl(8.0f, y0 + 14.0f, 200.0f, y0 + 14.0f, 1.0f, HUD_G);
        hl(8.0f, y0 + 26.0f, 200.0f, y0 + 26.0f, 2.0f, HUD_G);
        hl(8.0f, y0 + 40.0f, 200.0f, y0 + 40.0f, 3.0f, HUD_G);
        hl(8.0f, y0 + 54.0f, 200.0f, y0 + 54.0f, 2.0f, HUD_Y);
        hl(8.0f, y0 + 66.0f, 200.0f, y0 + 66.0f, 2.0f, HUD_R);
        hl(210.0f, y0 + 60.0f, 260.0f, y0 + 10.0f, 1.0f, HUD_G);
        hl(230.0f, y0 + 60.0f, 280.0f, y0 + 10.0f, 2.0f, HUD_G);
        hl(250.0f, y0 + 60.0f, 300.0f, y0 + 10.0f, 3.0f, HUD_G);
        atext(0, 8.0f, y0 + 78.0f, HUD_G, "350 1500 M 0.84 G 7.5 @ 2.6");
        atext(1, 8.0f, y0 + 94.0f, HUD_G, "12450 SHOOT");
        atext(0, 310.0f, y0 + 8.0f, HUD_G, "350 000 010 020  12450  -1250  M 0.84");
        atext(1, 310.0f, y0 + 24.0f, HUD_G, "AIM-120 16  SHOOT");
        atext(0, 310.0f, y0 + 46.0f, HUD_Y, "12.4  17");
        atext(0, 400.0f, y0 + 46.0f, HUD_R, "MISSILE 8");
        atext(1, 500.0f, y0 + 42.0f, HUD_R, "PULL UP");
        aspr("vv", 330.0f, y0 + 90.0f, HUD_G);
        aspr("waterline", 380.0f, y0 + 88.0f, HUD_G);
        aspr("td_box", 430.0f, y0 + 90.0f, HUD_G);
        diamond(470.0f, y0 + 90.0f, 6.0f, HUD_Y);
        diamond(510.0f, y0 + 90.0f, 6.0f, HUD_R);
        aspr("sel_tri", 510.0f, y0 + 76.0f, HUD_G);
        aspr("td_box", 550.0f, y0 + 90.0f, HUD_G); diamond(550.0f, y0 + 90.0f, 6.0f, HUD_R);
        hl(575.0f, y0 + 90.0f, 615.0f, y0 + 90.0f, 2.0f, HUD_G); hl(615.0f, y0 + 90.0f, 615.0f, y0 + 95.0f, 2.0f, HUD_G);
    }
}

/* peak row of a horizontal line near y (virtual = screen at 640x480), vs background rows above */
static void line_measure(const u32 *img, int vy, float *lr, float *lg, float *lb, float *br, float *bg, float *bb)
{
    int yy, y = hud_sy((float)vy), x0 = hud_sx(40.0f), x1 = hud_sx(160.0f);
    avg_box(img, x0, y - 8, x1, y - 6, br, bg, bb);
    *lr = *br; *lg = *bg; *lb = *bb;
    for (yy = y - 4; yy <= y + 4; yy++) {        /* first row that differs: the line (its shadow is 1 px lower) */
        float r, g, b, d;
        avg_box(img, x0, yy, x1, yy, &r, &g, &b);
        d = (float)fabs(r - *br) + (float)fabs(g - *bg) + (float)fabs(b - *bb);
        if (d > 30.0f) { *lr = r; *lg = g; *lb = b; return; }
    }
}

static void scene7a(void)
{
    static const int modes[6] = { RB_ADD, RB_PREMUL, RB_PREMUL, RB_BLEND, RB_ADD, RB_PREMUL };
    static const u32 shad[6] = { 0x80, 0x80, 0xB0, 0x80, 0, 0 };
    static const char *const mn[6] = { "add_sh50", "premul_sh50", "premul_sh70", "blend_sh50", "add_nosh", "premul_nosh" };
    static const int ly[5] = { 14, 26, 40, 54, 66 };
    static const char *const lname[5] = { "g1px", "g2px", "g3px", "y2px", "r2px" };
    int m, b, l;
    for (m = 0; m < 6; m++) {
        e_snprintf(s_label, (int)sizeof(s_label), "S7 HUD %s %s", s_tag, mn[m]);
        frame_begin(0);
        bands_bg();
        hud_passes(bands_content, modes[m], shad[m]);
        frame_end(s_img[0]);
        for (b = 0; b < 4; b++) for (l = 0; l < 5; l++) {
            float lr, lg, lb, br, bgc, bb;
            line_measure(s_img[0], b * 120 + ly[l], &lr, &lg, &lb, &br, &bgc, &bb);
            log_printf(LOG_INFO, "probe: s7 line mode=%s blend=%s band=%s line=%s rgb=%.0f,%.0f,%.0f bg=%.0f,%.0f,%.0f "
                       "luma_contrast=%.0f chroma=%.0f", s_tag, mn[m], BAND_NAME[b], lname[l], lr, lg, lb, br, bgc, bb,
                       (float)fabs(luma(lr, lg, lb) - luma(br, bgc, bb)),
                       E_MAXF(E_MAXF(lr, lg), lb) - E_MINF(E_MINF(lr, lg), lb));
        }
        {
            char n[48];
            e_snprintf(n, (int)sizeof(n), "s7_lines_%s", mn[m]);
            save_img(s_img[0], n);
            e_snprintf(n, (int)sizeof(n), "s7_lines_%s_zoom", mn[m]);
            save_zoom(s_img[0], hud_sx(480.0f), hud_sy(150.0f), 2, n);
        }
    }
}

/* -------- 7b: HUD mock over a 3D view (sky, cloud puffs, sea, island slope) */
static void mock_content(void)
{
    int i;
    char buf[16];
    float hy = 252.0f;                              /* horizon 12 px below the waterline: nose 1.5 deg up */
    /* pitch ladder: horizon line + rungs every 5 deg (8 px/deg at 65 deg h-fov ~ 9.8 px/deg) */
    hl(210.0f, hy, 308.0f, hy, 2.0f, HUD_G); hl(332.0f, hy, 430.0f, hy, 2.0f, HUD_G);
    for (i = -2; i <= 2; i++) {
        float y = hy - (float)i * 49.0f;
        if (!i) continue;
        if (i > 0) {
            hl(268.0f, y, 308.0f, y, 2.0f, HUD_G); hl(332.0f, y, 372.0f, y, 2.0f, HUD_G);
            hl(268.0f, y, 268.0f, y + 5.0f, 2.0f, HUD_G); hl(372.0f, y, 372.0f, y + 5.0f, 2.0f, HUD_G);
        } else {
            float x;
            for (x = 268.0f; x < 308.0f; x += 10.0f) { hl(x, y, x + 6.0f, y, 2.0f, HUD_G); hl(x + 64.0f, y, x + 70.0f, y, 2.0f, HUD_G); }
            hl(268.0f, y, 268.0f, y - 5.0f, 2.0f, HUD_G); hl(372.0f, y, 372.0f, y - 5.0f, 2.0f, HUD_G);
        }
        e_snprintf(buf, (int)sizeof(buf), "%d", i * 5 < 0 ? -i * 5 : i * 5);
        atext(0, 250.0f - atext_w(0, buf), y - 4.0f, HUD_G, buf);
        atext(0, 378.0f, y - 4.0f, HUD_G, buf);
    }
    aspr("waterline", 320.0f, 240.0f, HUD_G);
    aspr("vv", 322.0f, 250.0f, HUD_G);
    /* heading tape */
    for (i = -15; i <= 15; i++) {
        float x = 320.0f + (float)i * 8.0f - 2.0f;
        hl(x, 124.0f, x, (i % 2) ? 128.0f : 132.0f, 1.0f, HUD_G);
    }
    atext(0, 296.0f - 2.0f - 10.0f, 110.0f, HUD_G, "350"); atext(0, 310.0f, 110.0f, HUD_G, "000"); atext(0, 376.0f - 2.0f, 110.0f, HUD_G, "010");
    aspr("caret", 320.0f, 136.0f, HUD_G);
    /* airspeed / altitude boxes */
    hl(206.0f, 220.0f, 254.0f, 220.0f, 1.0f, HUD_G); hl(206.0f, 236.0f, 254.0f, 236.0f, 1.0f, HUD_G);
    hl(206.0f, 220.0f, 206.0f, 236.0f, 1.0f, HUD_G); hl(254.0f, 220.0f, 254.0f, 236.0f, 1.0f, HUD_G);
    atext(1, 250.0f - atext_w(1, "352"), 222.0f, HUD_G, "352");
    hl(386.0f, 220.0f, 444.0f, 220.0f, 1.0f, HUD_G); hl(386.0f, 236.0f, 444.0f, 236.0f, 1.0f, HUD_G);
    hl(386.0f, 220.0f, 386.0f, 236.0f, 1.0f, HUD_G); hl(444.0f, 220.0f, 444.0f, 236.0f, 1.0f, HUD_G);
    atext(1, 440.0f - atext_w(1, "4010"), 222.0f, HUD_G, "4010");
    atext(0, 444.0f - atext_w(0, "-1250"), 202.0f, HUD_G, "-1250");
    atext(0, 196.0f, 262.0f, HUD_G, "@ 2.6"); atext(0, 196.0f, 276.0f, HUD_G, "M 0.84"); atext(0, 196.0f, 290.0f, HUD_G, "G 4.5");
    atext(0, 320.0f - atext_w(0, "AIM-120 16") * 0.5f, 364.0f, HUD_G, "AIM-120 16");
    /* contacts: unknown over cloud, hostile + TD box over cloud edge, unknown over sea, hostile over terrain */
    diamond(236.0f, 170.0f, 6.0f, HUD_Y); atext(0, 236.0f - atext_w(0, "17") * 0.5f, 180.0f, HUD_Y, "17");
    aspr("td_box", 402.0f, 166.0f, HUD_G); diamond(402.0f, 166.0f, 6.0f, HUD_R);
    atext(0, 402.0f - atext_w(0, "12.4  17") * 0.5f, 180.0f, HUD_R, "12.4  17");
    aspr("sel_tri", 455.0f, 186.0f, HUD_G); diamond(455.0f, 200.0f, 6.0f, HUD_Y);
    diamond(290.0f, 330.0f, 6.0f, HUD_R);
    diamond(370.0f, 300.0f, 6.0f, HUD_Y);
    atext(1, 320.0f - atext_w(1, "SHOOT") * 0.5f, 314.0f, HUD_G, "SHOOT");
}

static void mock_bg(void)
{
    static const float cx[6] = { -300.0f, 400.0f, 300.0f, 700.0f, 1300.0f, -900.0f };     /* first two behind the HUD centre */
    static const float cz[6] = { 3000.0f, 3800.0f, 5000.0f, 3900.0f, 6500.0f, 5200.0f };
    static const float cy[6] = { 1450.0f, 1500.0f, 1800.0f, 1450.0f, 2100.0f, 1700.0f };
    static const float cs[6] = { 700.0f, 600.0f, 800.0f, 500.0f, 900.0f, 700.0f };
    u32 fog = fog_rgb();
    int i;
    Vec3 eye = v3(0.0f, 1200.0f, 0.0f);
    frame_begin(fog);
    cam_set(eye, v3_add(eye, v3(0.0f, (float)sin(E_DEG2RAD(1.5f)), (float)cos(E_DEG2RAD(1.5f)))), 1.0f, 18000.0f);
    draw_sky();
    r_set_fog(1, fog, 3600.0f, 18000.0f);
    mat(RB_OPAQUE, 1, 1, 1, 0, s_texGrass);         /* island slope (1:3 rising to 1500 m) lower right */
    patch(v3(-200.0f, -20.0f, 3000.0f), v3(5000.0f, -20.0f, 3000.0f), v3(-200.0f, 1500.0f, 7500.0f), v3(5000.0f, 1500.0f, 7500.0f),
          24, 24, GRASS_TINT, 0.0f, 0.0f, 160.0f, 140.0f);
    mat(RB_OPAQUE, 1, 1, 1, 0, s_texSea);
    flat(-27000.0f, -3000.0f, 27000.0f, 29000.0f, 0.0f, 64, 64, 0xFFFFFFFFu, 256.0f, 0.0f);
    if (s_texCloud) {
        mat(RB_BLEND, 1, 0, 1, 0, s_texCloud);
        for (i = 5; i >= 0; i--) {                  /* far to near (index order is roughly by distance) */
            float u0 = (float)(i % 2) * 0.5f, v0 = (float)((i / 2) % 2) * 0.5f;
            billboard(v3(cx[i], cy[i], cz[i]), cs[i], 0xFFFFFFFFu, u0, v0, u0 + 0.5f, v0 + 0.5f);
        }
    }
}

static void scene7b(void)
{
    static const int modes[5] = { RB_ADD, RB_PREMUL, RB_PREMUL, RB_BLEND, RB_PREMUL };
    static const u32 shad[5] = { 0x80, 0x80, 0xB0, 0x80, 0x80 };
    static const char *const mn[5] = { "add_sh50", "premul_sh50", "premul_sh70", "blend_sh50", "premul_sh50_native1to1" };
    int m;
    for (m = 0; m < 5; m++) {
        if (m == 4 && SW == 640) break;             /* native = scaled at 640x480 */
        s_hudNative = m == 4;
        e_snprintf(s_label, (int)sizeof(s_label), "S7 HUD mock %s %s", s_tag, mn[m]);
        mock_bg();
        hud_passes(mock_content, modes[m], shad[m]);
        frame_end(s_img[0]);
        {
            char n[48];
            e_snprintf(n, (int)sizeof(n), "s7_mock_%s", mn[m]);
            save_img(s_img[0], n);
            e_snprintf(n, (int)sizeof(n), "s7_mock_%s_zoom", mn[m]);
            save_zoom(s_img[0], hud_sx(330.0f), hud_sy(200.0f), 2, n);
        }
    }
    s_hudNative = 0;
}

/* -------- blend check: known colours over a known background, measured */
static void blend_check(void)
{
    static const int bl[5] = { RB_BLEND, RB_ADD, RB_PREMUL, RB_PREMUL, RB_PREMUL };
    static const u32 col[5] = { 0x80FF0000u, 0xFF404000u, 0x80800000u, 0x00404040u, 0xFF20C040u };
    static const char *const bn[5] = { "blend_a50_red", "add_404000", "premul_a50_800000", "premul_a0_404040", "premul_a100_20c040" };
    int i;
    e_snprintf(s_label, (int)sizeof(s_label), "S7 blend check %s", s_tag);
    frame_begin(0x808080);
    ui_begin(SW, SH);
    for (i = 0; i < 5; i++) {
        ui_set_blend(bl[i]);
        ui_rect(40.0f + (float)i * 110.0f, 100.0f, 80.0f, 80.0f, col[i]);
    }
    ui_end();
    frame_end(s_img[0]);
    for (i = 0; i < 5; i++) {
        float r, g, b, a = (float)(col[i] >> 24) / 255.0f, er, eg, eb, sr = (float)ch(col[i], 0), sg = (float)ch(col[i], 1), sb = (float)ch(col[i], 2);
        avg_box(s_img[0], 50 + i * 110, 110, 110 + i * 110, 170, &r, &g, &b);
        if (bl[i] == RB_BLEND) { er = sr * a + 128.0f * (1 - a); eg = sg * a + 128.0f * (1 - a); eb = sb * a + 128.0f * (1 - a); }
        else if (bl[i] == RB_ADD) { er = E_MINF(sr + 128.0f, 255.0f); eg = E_MINF(sg + 128.0f, 255.0f); eb = E_MINF(sb + 128.0f, 255.0f); }
        else { er = E_MINF(sr + 128.0f * (1 - a), 255.0f); eg = E_MINF(sg + 128.0f * (1 - a), 255.0f); eb = E_MINF(sb + 128.0f * (1 - a), 255.0f); }
        log_printf(LOG_INFO, "probe: s7 blendcheck mode=%s case=%s expected=%.0f,%.0f,%.0f measured=%.1f,%.1f,%.1f ok=%d", s_tag, bn[i],
                   er, eg, eb, r, g, b, (float)fabs(r - er) <= 9.0f && (float)fabs(g - eg) <= 9.0f && (float)fabs(b - eb) <= 9.0f);
    }
    save_img(s_img[0], "s7_blendcheck");
}

static void scene7(void)
{
    blend_check();
    scene7a();
    scene7b();
}

/* ================================================================ scene 8: costs */
typedef void (*BenchFn)(void);
static int s_benchArg;
static float s_benchCpu;                            /* ms of the timed part inside the frame, summed */

static float s_syncCost;                            /* ms of gpu_sync on an idle pipeline */

/* wait until the card has finished everything queued: r_save_bmp locks the back buffer with DDLOCK_WAIT (then
   converts + writes a BMP, a roughly constant cost measured once as s_syncCost). The windowed present is an
   asynchronous Blt and nothing else in the frame blocks, so CPU-side frame times alone do not show fill cost. */
static void gpu_sync(void) { r_save_bmp("C:\\WINDOWS\\TEMP\\probe_s.bmp"); }

static void sync_calibrate(void)
{
    s64 qpf = plat_qpf(), t;
    int i;
    float best = 1e9f, ms;
    gpu_sync();
    for (i = 0; i < 4; i++) {
        t = plat_qpc();
        gpu_sync();
        ms = (float)((double)(plat_qpc() - t) * 1000.0 / (double)qpf);
        if (ms < best) best = ms;
    }
    s_syncCost = best;
    log_printf(LOG_INFO, "probe: s8 sync_cost_ms=%.2f", s_syncCost);
}

/* N frames between two syncs: GPU-inclusive ms per frame */
static float bench_sync(const char *name, BenchFn fn, int frames)
{
    s64 qpf = plat_qpf(), t0, t1, t2;
    int i;
    float gpu, cpu;
    for (i = 0; i < 5; i++) fn();
    gpu_sync();
    s_benchCpu = 0.0f;
    t0 = plat_qpc();
    for (i = 0; i < frames; i++) fn();
    t1 = plat_qpc();
    gpu_sync();
    t2 = plat_qpc();
    cpu = (float)((double)(t1 - t0) * 1000.0 / (double)qpf) / (float)frames;
    gpu = ((float)((double)(t2 - t0) * 1000.0 / (double)qpf) - s_syncCost) / (float)frames;
    log_printf(LOG_INFO, "probe: s8 sync mode=%s case=%s frames=%d frame_ms=%.3f cpu_submit_ms=%.3f cpu_part_ms=%.3f", s_tag, name, frames,
               gpu, cpu, s_benchCpu / (float)frames);
    return gpu;
}

static float bench(const char *name, BenchFn fn, int frames)
{
    s64 qpf = plat_qpf(), t0, t1;
    static float ft[400];
    int i, n = 0;
    float sum = 0.0f, med, tmp;
    int a, b;
    for (i = 0; i < 20; i++) { fn(); }
    s_benchCpu = 0.0f;
    t0 = plat_qpc();
    for (i = 0; i < frames && i < 400; i++) {
        s64 f0 = plat_qpc();
        fn();
        ft[n++] = (float)((double)(plat_qpc() - f0) * 1000.0 / (double)qpf);
    }
    t1 = plat_qpc();
    for (i = 0; i < n; i++) sum += ft[i];
    for (a = 0; a < n; a++) for (b = a + 1; b < n; b++) if (ft[b] < ft[a]) { tmp = ft[a]; ft[a] = ft[b]; ft[b] = tmp; }
    med = ft[n / 2];
    log_printf(LOG_INFO, "probe: s8 bench mode=%s case=%s frames=%d mean_ms=%.3f median_ms=%.3f p90_ms=%.3f cpu_part_ms=%.3f total_s=%.2f",
               s_tag, name, n, sum / (float)n, med, ft[n * 9 / 10], s_benchCpu / (float)n,
               (double)(t1 - t0) / (double)qpf);
    return med;
}

/* fullscreen: Flip is locked to the 120 Hz retrace, so benches render into the back buffer without presenting and
   time the card with gpu_sync; windowed: the Blt present is asynchronous and cheap, keep it */
static void bench_present(void) { if (s_fs) plat_pump(); else r_present(); }

static void bf_empty(void)
{
    frame_begin(0x203040);
    cam_set(v3(0.0f, 500.0f, 0.0f), v3(0.0f, 500.0f, 1000.0f), 1.0f, 18000.0f);
    r_end_frame();
    bench_present();
}

static void bf_clearz(void)
{
    s64 q;
    int i;
    frame_begin(0x203040);
    q = plat_qpc();
    for (i = 0; i < s_benchArg; i++) r_clear_z();
    s_benchCpu += (float)((double)(plat_qpc() - q) * 1000.0 / (double)plat_qpf());
    r_end_frame();
    bench_present();
}

static void bf_layers3d(void)                       /* 4 full-screen textured 3D quads at 5 km, fog on/off */
{
    int i;
    frame_begin(0x203040);
    cam_set(v3(0.0f, 500.0f, 0.0f), v3(0.0f, 500.0f, 1000.0f), 1.0f, 18000.0f);
    r_set_fog(s_benchArg, fog_rgb(), 3600.0f, 18000.0f);
    mat(RB_OPAQUE, 1, 1, s_benchArg, 0, s_texGrass);
    for (i = 0; i < 4; i++) patch(v3(-4000.0f, 4000.0f, 5000.0f), v3(4000.0f, 4000.0f, 5000.0f), v3(-4000.0f, -3000.0f, 5000.0f),
                                  v3(4000.0f, -3000.0f, 5000.0f), 4, 4, 0xFFFFFFFFu, 0.0f, 0.0f, 8.0f, 7.0f);
    r_end_frame();
    bench_present();
}

static void bf_layers2d(void)                       /* 4 full-screen textured 2D quads in blend mode s_benchArg */
{
    int i;
    RVert2D v[4];
    static const u16 idx[6] = { 0, 1, 2, 0, 2, 3 };
    frame_begin(0x203040);
    for (i = 0; i < 4; i++) {
        v[i].x = (i == 1 || i == 2) ? (float)SW : 0.0f;
        v[i].y = (i >= 2) ? (float)SH : 0.0f;
        v[i].u = v[i].x / 256.0f; v[i].v = v[i].y / 256.0f;
        v[i].color = 0x80808080u;
    }
    for (i = 0; i < 4; i++) r_draw_2d(v, 4, idx, 6, s_texGrass, s_benchArg);
    r_end_frame();
    bench_present();
}

static void bf_lines(void)                          /* 500 ui_line quads (2 px, 40 px long) */
{
    s64 q;
    int i;
    ERand rr;
    frame_begin(0x203040);
    rnd_seed(&rr, 1234);
    q = plat_qpc();
    ui_begin(SW, SH);
    ui_set_blend(s_benchArg);
    for (i = 0; i < 500; i++) {
        float x = rnd_range(&rr, 40.0f, 600.0f), y = rnd_range(&rr, 40.0f, 440.0f), a = rnd_range(&rr, 0.0f, 6.28f);
        ui_line(x, y, x + 40.0f * (float)cos(a), y + 40.0f * (float)sin(a), 2.0f, HUD_G);
    }
    ui_end();
    s_benchCpu += (float)((double)(plat_qpc() - q) * 1000.0 / (double)plat_qpf());
    r_end_frame();
    bench_present();
}

static void bf_glyphs(void)                         /* 500 atlas glyph quads (small font) */
{
    s64 q;
    int i;
    frame_begin(0x203040);
    q = plat_qpc();
    ui_begin(SW, SH);
    ui_set_blend(s_benchArg);
    for (i = 0; i < 25; i++) atext(0, 20.0f, 20.0f + (float)i * 17.0f, HUD_G, "350 1500 M 0.84 G 7.5 12450");   /* 20 x 25 = 500 */
    ui_end();
    s_benchCpu += (float)((double)(plat_qpc() - q) * 1000.0 / (double)plat_qpf());
    r_end_frame();
    bench_present();
}

static void bf_hudmock(void)                        /* the 7b HUD (shadow + main pass) over an empty frame */
{
    s64 q;
    frame_begin(0x203040);
    q = plat_qpc();
    hud_passes(mock_content, s_benchArg, 0x80);
    s_benchCpu += (float)((double)(plat_qpc() - q) * 1000.0 / (double)plat_qpf());
    r_end_frame();
    bench_present();
}

/* a frame with the planned Low content (DESIGN 9.3): sky, 3200-tri terrain patch (static VB), 160-tri sea, 16 cloud
   puffs, 2 jets LOD0 near, the mock HUD (shadow + PREMUL) */
static RVBuf s_terrVb;
static int   s_lowShot;
static u16   s_terrIdx[40 * 40 * 6];
static void lowframe_init(void)
{
    static RVert1 v[41 * 41];
    int i, j, k = 0;
    for (j = 0; j <= 40; j++) for (i = 0; i <= 40; i++) {
        RVert1 *o = &v[j * 41 + i];
        float x = -4000.0f + (float)i * 200.0f, z = 2500.0f + (float)j * 200.0f;
        float r = (float)sqrt((x - 0.0f) * (x - 0.0f) + (z - 7000.0f) * (z - 7000.0f));
        o->x = x; o->z = z;
        o->y = r < 4500.0f ? 1750.0f * (float)pow(1.0f - r / 4500.0f, 1.6f) - 30.0f + 25.0f * (float)sin(x * 0.01f) * (float)cos(z * 0.013f) : -60.0f;
        o->color = GRASS_TINT; o->u = x / 64.0f; o->v = z / 64.0f;
    }
    for (j = 0; j < 40; j++) for (i = 0; i < 40; i++) {
        int q = j * 41 + i;
        s_terrIdx[k++] = (u16)q; s_terrIdx[k++] = (u16)(q + 1); s_terrIdx[k++] = (u16)(q + 42);
        s_terrIdx[k++] = (u16)q; s_terrIdx[k++] = (u16)(q + 42); s_terrIdx[k++] = (u16)(q + 41);
    }
    s_terrVb = r_vb_create(RFMT_V1, v, 41 * 41);
}

static void bf_lowframe(void)
{
    u32 fog = fog_rgb();
    int i;
    Vec3 eye = v3(0.0f, 900.0f, 0.0f);
    frame_begin(fog | R_ZONLY);
    cam_set(eye, v3_add(eye, v3(0.0f, -(float)sin(E_DEG2RAD(3.0f)), (float)cos(E_DEG2RAD(3.0f)))), 1.0f, 18000.0f);
    draw_sky();
    r_set_fog(1, fog, 3600.0f, 16200.0f);
    mat(RB_OPAQUE, 1, 1, 1, 0, s_texGrass);
    r_draw_vb(s_terrVb, s_terrIdx, 40 * 40 * 6);
    mat(RB_OPAQUE, 1, 1, 1, 0, s_texSea);
    flat(-27000.0f, -3000.0f, 27000.0f, 19000.0f, 0.0f, 10, 8, 0xFFFFFFFFu, 256.0f, 0.0f);
    if (s_jetOk) {
        jet_draw(v3(-40.0f, 905.0f, 220.0f), 1, 220.0f);
        jet_draw(v3(60.0f, 890.0f, 280.0f), 0, 280.0f);
    }
    mat(RB_BLEND, 1, 0, 1, 0, s_texCloud);
    for (i = 15; i >= 0; i--) {
        float u0 = (float)(i % 2) * 0.5f, v0 = (float)((i / 2) % 2) * 0.5f;
        billboard(v3(-6000.0f + (float)i * 800.0f, 1800.0f + (float)(i % 3) * 400.0f, 3000.0f + (float)((i * 7) % 16) * 600.0f),
                  500.0f + (float)(i % 4) * 150.0f, 0xFFFFFFFFu, u0, v0, u0 + 0.5f, v0 + 0.5f);
    }
    hud_passes(mock_content, RB_PREMUL, 0x80);
    if (s_lowShot) { frame_end(s_img[0]); return; }
    r_end_frame();
    bench_present();
}

static void scene8(void)
{
    RStats st;
    s_label[0] = 0;
    sync_calibrate();
    bench_sync("empty", bf_empty, 100);
    s_benchArg = 1; bench_sync("clear_z_x1", bf_clearz, 100);
    s_benchArg = 4; bench_sync("clear_z_x4", bf_clearz, 100);
    s_benchArg = 0; bench_sync("layers3d_x4_fog_off", bf_layers3d, 200);
    s_benchArg = 1; bench_sync("layers3d_x4_fog_on", bf_layers3d, 200);
    s_benchArg = RB_OPAQUE; bench_sync("layers2d_x4_opaque", bf_layers2d, 200);
    s_benchArg = RB_BLEND;  bench_sync("layers2d_x4_blend", bf_layers2d, 200);
    s_benchArg = RB_ADD;    bench_sync("layers2d_x4_add", bf_layers2d, 200);
    s_benchArg = RB_PREMUL; bench_sync("layers2d_x4_premul", bf_layers2d, 200);
    s_benchArg = RB_PREMUL; bench_sync("hudmock_premul_sh_gpu", bf_hudmock, 100);
    s_benchArg = 0;         bench_sync("empty_again", bf_empty, 100);
    lowframe_init();
    r_stats(NULL, 1);
    bench_sync("lowframe", bf_lowframe, 120);
    r_stats(&st, 1);
    log_printf(LOG_INFO, "probe: s8 lowframe_stats res=%dx%d draws_per_frame=%.1f tris_per_frame=%.1f", SW, SH, (double)st.draws / 125.0,
               (double)st.tris / 125.0);
    e_snprintf(s_label, (int)sizeof(s_label), "S8 Low frame %s %dx%d", s_tag, SW, SH);
    s_lowShot = 1;
    bf_lowframe();                                  /* one labelled frame for the record */
    s_lowShot = 0;
    {
        char n[40];
        e_snprintf(n, (int)sizeof(n), "s8_lowframe_%dx%d", SW, SH);
        save_img(s_img[0], n);
    }
    s_label[0] = 0;
    s_benchArg = RB_BLEND;  bench("ui_line_500_blend", bf_lines, 300);
    s_benchArg = RB_ADD;    bench("ui_line_500_add", bf_lines, 300);
    s_benchArg = RB_PREMUL; bench("ui_line_500_premul", bf_lines, 300);
    s_benchArg = RB_ADD;    bench("glyphs_500_add", bf_glyphs, 300);
    s_benchArg = RB_PREMUL; bench("glyphs_500_premul", bf_glyphs, 300);
    r_stats(NULL, 1);
    s_benchArg = RB_ADD;    bench("hudmock_add_sh", bf_hudmock, 300);
    r_stats(&st, 1);
    log_printf(LOG_INFO, "probe: s8 hudmock_stats draws_per_frame=%.1f tris_per_frame=%.1f", (double)st.draws / 320.0, (double)st.tris / 320.0);
    s_benchArg = RB_PREMUL; bench("hudmock_premul_sh", bf_hudmock, 300);
}

/* ================================================================ main */
static int grab_check(void)
{
    const char *tmp = "C:\\WINDOWS\\TEMP\\probe_c.bmp";
    EImage im;
    int i, n = 0, ok;
    if (s_fs) return 1;
    e_snprintf(s_label, (int)sizeof(s_label), "grab check %s", s_tag);
    s1_draw(3000.0f, 0, 0.0f, v3_norm(v3(0.0f, -300.0f, 3000.0f)));
    label();
    r_end_frame();
    ok = r_save_bmp(tmp);
    r_present();
    gdi_grab(s_img[0]);
    if (!ok || !img_load(&im, tmp, 0)) { log_printf(LOG_INFO, "probe: grab_check result=nofile"); return 0; }
    for (i = 0; i < NPIX; i++) if (cdiff(im.pix[0][i], s_img[0][i]) > 8) n++;
    img_free(&im);
    log_printf(LOG_INFO, "probe: grab_check gdi_vs_backbuffer_diff_px=%d (of %d)", n, NPIX);
    return n < 100;
}

static int app(HINSTANCE inst)
{
    char base[E_PATH], path[E_PATH], *slash;
    const char *sc;
    PlatInit pi;
    RInit ri;
    BITMAPINFO bi;
    int i, all;
    s64 t0;
    e_strlcpy(base, plat_exe_dir(), (int)sizeof(base));
    slash = strrchr(base, '\\');
    if (slash && !e_stricmp(slash + 1, "probe")) *slash = 0;   /* M:\dev\jet\probe -> M:\dev\jet */
    file_set_base_dir(base);
    s_wbuf = arg_int("-wbuf", 0, 1) ? 1 : 0;
    s_fs = arg_has("-fs");
    s_shot = arg_has("-shot");
    if (arg_int("-res", 0, 640) == 800) { SW = 800; SH = 600; }
    else if (arg_int("-res", 0, 640) == 1024) { SW = 1024; SH = 768; }
    NPIX = SW * SH;
    e_strlcpy(s_tag, arg_str("-tag", 0, s_wbuf ? "w" : "z"), (int)sizeof(s_tag));
    e_snprintf(path, (int)sizeof(path), "probe\\probe_%s.log", s_tag);
    {
        char lp[E_PATH];
        base_path(lp, (int)sizeof(lp), path);
        log_open(lp, arg_has("-debug") ? LOG_DEBUG : LOG_INFO);
    }
    log_set_autoflush(1);
    log_printf(LOG_INFO, "probe: boot build=\"%s %s\" base=%s wbuf=%d fs=%d shot=%d", __DATE__, __TIME__, base, s_wbuf, s_fs, s_shot);
    plat_set_no_dialogs(1);
    plat_fpu_setup();
    prof_init(plat_now_us);
    memset(&pi, 0, sizeof(pi));
    pi.title = "Jet probe";
    pi.wndClass = "JetProbeWnd";
    pi.width = SW; pi.height = SH; pi.fullscreen = s_fs;
    if (!plat_init(inst, &pi)) { log_printf(LOG_ERR, "probe: window failed"); log_close(); return 1; }
    ShowWindow((HWND)plat_hwnd(), SW_SHOWNORMAL);
    ShowWindow((HWND)plat_hwnd(), SW_SHOWNORMAL);
    SetForegroundWindow((HWND)plat_hwnd());
    memset(&ri, 0, sizeof(ri));
    ri.width = SW; ri.height = SH; ri.bpp = 16; ri.fullscreen = s_fs; ri.fpsTarget = 60;
    ri.wantWBuffer = s_wbuf; ri.filter = 0; ri.maxTexSize = 256; ri.texBudgetKB = 8 * 1024; ri.noCap = 1;
    if (!r_init(plat_hwnd(), &ri)) { log_printf(LOG_ERR, "probe: r_init failed"); plat_shutdown(); log_close(); return 1; }
    plat_fpu_setup();
    log_printf(LOG_INFO, "probe: caps device=\"%s\" wbuffer_cap=%d zbits=%d fogtable=%d fmt4444=%d", r_caps()->device, r_caps()->wbuffer,
               r_caps()->zBits, r_caps()->fogTable, r_caps()->fmt4444);
    for (i = 0; i < 6; i++) s_img[i] = (u32 *)malloc(MAXPIX * 4);
    log_printf(LOG_INFO, "probe: vidmem res=%dx%d fs=%d total_kb=%u free_kb=%u tex_budget_kb=%d", SW, SH, s_fs, (unsigned)r_caps()->vidTotalKB,
               (unsigned)r_caps()->vidFreeKB, r_tex_budget_kb());
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = SW; bi.bmiHeader.biHeight = -SH; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    s_memDC = CreateCompatibleDC(NULL);
    s_dib = CreateDIBSection(s_memDC, &bi, DIB_RGB_COLORS, (void **)&s_dibBits, NULL, 0);
    SelectObject(s_memDC, s_dib);
    s_fontOk = font_create(&s_font, "Arial", 13, 1, 0);
    memset(&s_world, 0, sizeof(s_world));
    s_skyOk = world_sky_load(&s_world, 4, "light") > 0;
    s_texSea = tex_get("sea", 0, 0);
    {
        EImage im;
        if (img_load(&im, "tex/sea.bmp", 0)) {
            double sr = 0, sg = 0, sb = 0;
            int n = im.w * im.h;
            for (i = 0; i < n; i++) { sr += (im.pix[0][i] >> 16) & 0xFF; sg += (im.pix[0][i] >> 8) & 0xFF; sb += im.pix[0][i] & 0xFF; }
            s_seaMean[0] = (float)(sr / n); s_seaMean[1] = (float)(sg / n); s_seaMean[2] = (float)(sb / n);
            img_free(&im);
        }
        log_printf(LOG_INFO, "probe: sea_mean rgb=%.1f,%.1f,%.1f", s_seaMean[0], s_seaMean[1], s_seaMean[2]);
    }
    s_texGrass = tex_get("t_grass", 0, 0);
    s_texCloud = tex_get("cloud_puffs", IMG_ALPHA, RT_CLAMP);
    s_jetOk = mesh_load(&s_jet, "jet", 0);
    atlas_load();
    log_printf(LOG_INFO, "probe: assets sky=%d tiles=%d fog=%06X sea=%d grass=%d cloud=%d jet=%d tex_kb=%d", s_skyOk, s_world.skyTiles,
               (unsigned)fog_rgb(), s_texSea, s_texGrass, s_texCloud, s_jetOk, r_tex_kb());
    grab_check();
    sc = arg_str("-scene", 0, "all");
    all = !e_stricmp(sc, "all");
    t0 = plat_qpc();
    for (i = 1; i <= 8; i++) {
        char nb[8];
        s64 ts = plat_qpc();
        e_snprintf(nb, (int)sizeof(nb), "%d", i);
        if (!all && strcmp(sc, nb)) continue;
        s_grabs = 0;
        switch (i) {
        case 1: scene1(); break;
        case 2: scene2(); break;
        case 3: scene3(); break;
        case 4: scene4(); break;
        case 5: scene5(); break;
        case 6: scene6(); break;
        case 7: scene7(); break;
        case 8: scene8(); break;
        }
        log_printf(LOG_INFO, "probe: scene %d done grabs=%d seconds=%.1f", i, s_grabs, (double)(plat_qpc() - ts) / (double)plat_qpf());
        if (!plat_pump()) break;
    }
    log_printf(LOG_INFO, "probe: done seconds=%.1f", (double)(plat_qpc() - t0) / (double)plat_qpf());
    if (s_jetOk) mesh_free(&s_jet);
    world_free(&s_world);
    if (s_fontOk) font_free(&s_font);
    tex_release_all();
    r_shutdown();
    plat_shutdown();
    DeleteObject(s_dib);
    DeleteDC(s_memDC);
    for (i = 0; i < 6; i++) free(s_img[i]);
    log_close();
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdLine, int show)
{
    static char buf[1024];
    char *argv[64];
    int argc;
    (void)prev; (void)show;
    argv[0] = (char *)"probe.exe";
    e_strlcpy(buf, cmdLine ? cmdLine : "", (int)sizeof(buf));
    argc = 1 + args_split(buf, argv + 1, E_COUNT(argv) - 1);
    args_init(argc, argv);
    return app(inst);
}
