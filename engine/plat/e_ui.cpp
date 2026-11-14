/* e_ui.cpp - see e_ui.h. Windows (GDI for font atlases). Owner: T05. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "e_ui.h"
#include "e_image.h"
#include "e_log.h"
#include "e_plat.h"

/* ================================================================ fonts */
static const u8 s_extraLatin[] = { 0xC4, 0xC5, 0xD6, 0xDC, 0xC9, 0xE4, 0xE5, 0xF6, 0xFC, 0xE9 };   /* Ä Å Ö Ü É ä å ö ü é */

static int in_set(int c, int set)
{
    int i;
    if (set == 0) return (c >= 32 && c <= 126) || (c >= 160 && c <= 255);
    if (set == 1) {
        if (c >= 32 && c <= 126) return 1;
        for (i = 0; i < (int)sizeof(s_extraLatin); i++) if (s_extraLatin[i] == c) return 1;
        return 0;
    }
    return c >= 32 && c <= 95;
}

static int pow2_at_least(int v) { int p = 16; while (p < v) p <<= 1; return p; }

/* shelf packing of fixed-height cells: returns the needed height for atlas width W, or a huge value */
static int pack_height(const int *cw, int set, int cellH, int W)
{
    int c, x = 0, y = 0;
    for (c = 32; c < 256; c++) {
        if (!in_set(c, set) || cw[c] <= 0) continue;
        if (cw[c] > W) return 1 << 30;
        if (x + cw[c] > W) { x = 0; y += cellH; }
        x += cw[c];
    }
    return y + cellH;
}

int font_create(Font *f, const char *face, int px, int bold, int outlinePx)
{
    HDC dc = NULL;
    HFONT f1 = NULL, fss = NULL, old = NULL;
    HBITMAP bmp = NULL, oldBmp = NULL;
    BITMAPINFO bi;
    u32 *bits = NULL;
    TEXTMETRIC tm1, tmS;
    ABC abc[256];
    int cw[256], cx0[256];
    int SS, pad, cellH, c, set, W, H, maxTex, cellWss, cellHss, x, y, ok = 0, haveAbc;
    u8 *cov = NULL, *outl = NULL;
    EImage img;
    s64 t0 = plat_now_us();
    memset(f, 0, sizeof(*f));
    memset(&img, 0, sizeof(img));
    if (!face || px < 4) return 0;
    if (px > 160) px = 160;
    SS = 4;
    if (px * SS > 256) SS = 256 / px > 1 ? 256 / px : 1;
    pad = (outlinePx > 0 ? outlinePx : 0) + 1;
    dc = CreateCompatibleDC(NULL);
    if (!dc) goto done;
    f1 = CreateFont(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                    ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    fss = CreateFont(-px * SS, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_TT_PRECIS,
                     CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    if (!f1 || !fss) goto done;
    /* 1x metrics: line height, ascent, advances */
    old = (HFONT)SelectObject(dc, f1);
    GetTextMetrics(dc, &tm1);
    {
        INT wd[256];
        memset(wd, 0, sizeof(wd));
        if (!GetCharWidthA(dc, 0, 255, wd)) log_printf(LOG_WARN, "warn: ui GetCharWidth failed face=%s", face);
        for (c = 0; c < 256; c++) f->adv[c] = (u8)(wd[c] < 0 ? 0 : (wd[c] > 255 ? 255 : wd[c]));
    }
    /* supersampled glyph boxes */
    SelectObject(dc, fss);
    GetTextMetrics(dc, &tmS);
    haveAbc = GetCharABCWidths(dc, 0, 255, abc) ? 1 : 0;
    f->height = tm1.tmHeight;
    f->ascent = tm1.tmAscent;
    cellH = (tmS.tmHeight + SS - 1) / SS + 2 * pad;
    if (cellH < tm1.tmHeight + 2 * pad) cellH = tm1.tmHeight + 2 * pad;
    for (c = 0; c < 256; c++) {
        int a, b;
        if (haveAbc) { a = abc[c].abcA; b = (int)abc[c].abcB; }
        else { a = 0; b = f->adv[c] * SS; }
        /* glyph bitmap covers pen-relative pixels [x0, x0 + cw) */
        cx0[c] = (a >= 0 ? a / SS : -((-a + SS - 1) / SS)) - pad;
        cw[c] = (a + b + SS - 1) / SS + pad - cx0[c];
        if (cw[c] > 250) cw[c] = 250;
        if (cx0[c] < -120) cx0[c] = -120;
        if (cx0[c] > 120) cx0[c] = 120;
    }
    cw[32] = 0;                                     /* space: advance only */
    /* choose the glyph set and atlas size */
    maxTex = r_caps()->maxTexW > 0 ? r_caps()->maxTexW : 256;
    if (maxTex > 1024) maxTex = 1024;
    W = 256;
    for (set = 0; set < 3; set++) {
        for (W = 256; W <= maxTex; W <<= 1) if (pack_height(cw, set, cellH, W) <= W) break;
        if (W <= maxTex) break;
    }
    if (set >= 3) {
        log_printf(LOG_ERR, "err: ui font too large face=%s px=%d", face, px);
        goto done;
    }
    H = pow2_at_least(pack_height(cw, set, cellH, W));
    if (H > W) H = W;
    if (!img_alloc(&img, W, H, IMG_ALPHA)) goto done;
    cov = (u8 *)calloc((size_t)W * (size_t)H, 1);
    outl = (u8 *)calloc((size_t)W * (size_t)H, 1);
    if (!cov || !outl) goto done;
    /* render cell DIB (supersampled) */
    cellWss = 0;
    for (c = 32; c < 256; c++) if (in_set(c, set) && cw[c] > cellWss) cellWss = cw[c];
    cellWss *= SS;
    cellHss = cellH * SS;
    if (cellWss <= 0) cellWss = SS;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cellWss;
    bi.bmiHeader.biHeight = -cellHss;              /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!bmp || !bits) goto done;
    oldBmp = (HBITMAP)SelectObject(dc, bmp);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_TOP | TA_LEFT | TA_NOUPDATECP);
    x = y = 0;
    for (c = 32; c < 256; c++) {
        int gx, gy, sx, sy, w;
        char ch = (char)c;
        if (!in_set(c, set)) continue;
        f->ox[c] = (s8)cx0[c];
        w = cw[c];
        if (w <= 0) { f->gw[c] = 0; f->gh[c] = 0; f->glyphs++; continue; }
        if (x + w > W) { x = 0; y += cellH; }
        GdiFlush();
        memset(bits, 0, (size_t)cellWss * (size_t)cellHss * 4);
        TextOutA(dc, -cx0[c] * SS, pad * SS, &ch, 1);
        GdiFlush();
        for (gy = 0; gy < cellH; gy++) for (gx = 0; gx < w; gx++) {
            int sum = 0;
            for (sy = 0; sy < SS; sy++) {
                const u32 *row = bits + (size_t)(gy * SS + sy) * (size_t)cellWss + (size_t)(gx * SS);
                for (sx = 0; sx < SS; sx++) sum += (int)((row[sx] >> 8) & 0xFF);
            }
            cov[(y + gy) * W + x + gx] = (u8)(sum / (SS * SS));
        }
        f->gu[c] = (u16)x;
        f->gv[c] = (u16)y;
        f->gw[c] = (u8)w;
        f->gh[c] = (u8)cellH;
        f->glyphs++;
        x += w;
    }
    /* outline: dilate coverage (disc) per glyph cell; cells have pad >= outline so nothing crosses into neighbours */
    if (outlinePx > 0) {
        int r = outlinePx, dx, dy;
        for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
            int m = 0;
            for (dy = -r; dy <= r; dy++) {
                int yy = y + dy;
                if (yy < 0 || yy >= H) continue;
                for (dx = -r; dx <= r; dx++) {
                    int xx = x + dx;
                    if (xx < 0 || xx >= W || dx * dx + dy * dy > r * r + r) continue;
                    if (cov[yy * W + xx] > m) m = cov[yy * W + xx];
                }
            }
            outl[y * W + x] = (u8)m;
        }
    }
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
        int g = cov[y * W + x], o = outl[y * W + x], a = g > o ? g : o, col = a ? g * 255 / a : 255;
        img.pix[0][y * W + x] = ((u32)a << 24) | ((u32)col << 16) | ((u32)col << 8) | (u32)col;
    }
    f->tex = r_tex_create(&img, RT_CLAMP | RT_NOMIP);
    f->texW = W;
    f->texH = H;
    /* missing glyphs draw as '?' */
    for (c = 0; c < 256; c++) {
        if (in_set(c, set) || c < 32) continue;
        f->adv[c] = f->adv['?']; f->gw[c] = f->gw['?']; f->gh[c] = f->gh['?'];
        f->gu[c] = f->gu['?']; f->gv[c] = f->gv['?']; f->ox[c] = f->ox['?'];
    }
    ok = f->tex != 0;
    log_printf(LOG_INFO, "ui: font face=\"%s\" px=%d bold=%d outline=%d height=%d ascent=%d glyphs=%d set=%d atlas=%dx%d ss=%d "
               "abc=%d tex=%d ms=%d", face, px, bold, outlinePx, f->height, f->ascent, f->glyphs, set, W, H, SS, haveAbc,
               f->tex, (int)((plat_now_us() - t0) / 1000));
done:
    if (!ok) log_printf(LOG_ERR, "err: ui font_create failed face=%s px=%d", face ? face : "?", px);
    if (oldBmp) SelectObject(dc, oldBmp);
    if (bmp) DeleteObject(bmp);
    if (old) SelectObject(dc, old);
    if (f1) DeleteObject(f1);
    if (fss) DeleteObject(fss);
    if (dc) DeleteDC(dc);
    free(cov);
    free(outl);
    img_free(&img);
    return ok;
}

void font_free(Font *f)
{
    if (f->tex) r_tex_free(f->tex);
    memset(f, 0, sizeof(*f));
}

int font_width(const Font *f, const char *s)
{
    int w = 0, best = 0;
    for (; *s; s++) {
        if (*s == '\n') { if (w > best) best = w; w = 0; continue; }
        w += f->adv[(unsigned char)*s];
    }
    return w > best ? w : best;
}

/* ================================================================ 2D batches */
enum { UI_MAXQ = 2048 };
static RVert2D s_v[UI_MAXQ * 4];
static u16 s_idx[UI_MAXQ * 6];
static int s_nq, s_idxReady;
static RTex s_tex;
static float s_scale = 1.0f, s_ox, s_oy;
static int s_sw = 640, s_sh = 480;
static int s_blend = RB_BLEND;                     /* jet T01: ui_set_blend */
static float s_cx0, s_cy0, s_cx1 = 640.0f, s_cy1 = 480.0f;   /* jet T01: clip rectangle in screen pixels */
static int s_quads;                                /* jet T07: ui_quad_count */

static void flush(void)
{
    if (s_nq > 0) r_draw_2d(s_v, s_nq * 4, s_idx, s_nq * 6, s_tex, s_blend);
    s_nq = 0;
}

/* screen-pixel quad, clipped to the clip rectangle (default: the screen) with matching uv */
static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 argb, RTex t)
{
    RVert2D *v;
    float cx0 = s_cx0, cy0 = s_cy0, sw = s_cx1, sh = s_cy1;
    if (x1 <= x0 || y1 <= y0 || x1 <= cx0 || y1 <= cy0 || x0 >= sw || y0 >= sh) return;
    if (x0 < cx0) { u0 += (u1 - u0) * ((cx0 - x0) / (x1 - x0)); x0 = cx0; }
    if (y0 < cy0) { v0 += (v1 - v0) * ((cy0 - y0) / (y1 - y0)); y0 = cy0; }
    if (x1 > sw) { u1 -= (u1 - u0) * ((x1 - sw) / (x1 - x0)); x1 = sw; }
    if (y1 > sh) { v1 -= (v1 - v0) * ((y1 - sh) / (y1 - y0)); y1 = sh; }
    if (t != s_tex || s_nq >= UI_MAXQ) { flush(); s_tex = t; }
    v = &s_v[s_nq * 4];
    v[0].x = x0; v[0].y = y0; v[0].u = u0; v[0].v = v0; v[0].color = argb;
    v[1].x = x1; v[1].y = y0; v[1].u = u1; v[1].v = v0; v[1].color = argb;
    v[2].x = x1; v[2].y = y1; v[2].u = u1; v[2].v = v1; v[2].color = argb;
    v[3].x = x0; v[3].y = y1; v[3].u = u0; v[3].v = v1; v[3].color = argb;
    s_nq++;
    s_quads++;
}

int ui_px(float designPx)
{
    int w, h, p;
    r_screen_size(&w, &h);
    if (h <= 0) h = 480;
    p = e_ftoi(designPx * (float)h / 480.0f);
    return p < 4 ? 4 : p;
}

float ui_scale(void) { return s_scale; }

void ui_begin(int virtW, int virtH)
{
    int i;
    if (!s_idxReady) {
        for (i = 0; i < UI_MAXQ; i++) {
            s_idx[i * 6 + 0] = (u16)(i * 4); s_idx[i * 6 + 1] = (u16)(i * 4 + 1); s_idx[i * 6 + 2] = (u16)(i * 4 + 2);
            s_idx[i * 6 + 3] = (u16)(i * 4); s_idx[i * 6 + 4] = (u16)(i * 4 + 2); s_idx[i * 6 + 5] = (u16)(i * 4 + 3);
        }
        s_idxReady = 1;
    }
    r_screen_size(&s_sw, &s_sh);
    if (s_sw <= 0 || s_sh <= 0) { s_sw = 640; s_sh = 480; }
    if (virtW <= 0) virtW = 640;
    if (virtH <= 0) virtH = 480;
    s_scale = (float)s_sh / (float)virtH;
    s_ox = ((float)s_sw - (float)virtW * s_scale) * 0.5f;
    s_oy = 0.0f;
    s_nq = 0;
    s_tex = 0;
    s_blend = RB_BLEND;
    s_cx0 = 0.0f; s_cy0 = 0.0f; s_cx1 = (float)s_sw; s_cy1 = (float)s_sh;
}

void ui_text_px(const Font *f, int px, int py, u32 argb, const char *s)
{
    float iw, ih;
    int x = px;
    if (!f || !f->tex || !s) return;
    iw = 1.0f / (float)f->texW;
    ih = 1.0f / (float)f->texH;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c == '\n') { x = px; py += f->height; continue; }
        if (f->gw[c]) {
            float x0 = (float)(x + f->ox[c]), y0 = (float)(py - (f->gh[c] - f->height) / 2);
            quad(x0, y0, x0 + (float)f->gw[c], y0 + (float)f->gh[c], (float)f->gu[c] * iw, (float)f->gv[c] * ih,
                 (float)(f->gu[c] + f->gw[c]) * iw, (float)(f->gv[c] + f->gh[c]) * ih, argb, f->tex);
        }
        x += f->adv[c];
    }
}

void ui_text(const Font *f, float x, float y, u32 argb, int align, const char *s)
{
    int px, py, w;
    if (!f || !s) return;
    px = e_ftoi(s_ox + x * s_scale);
    py = e_ftoi(s_oy + y * s_scale);
    if (align) {
        w = font_width(f, s);
        px -= align == 1 ? w / 2 : w;
    }
    ui_text_px(f, px, py, argb, s);
}

void ui_rect(float x, float y, float w, float h, u32 argb)
{
    float x0 = (float)e_ftoi(s_ox + x * s_scale), y0 = (float)e_ftoi(s_oy + y * s_scale);
    float x1 = (float)e_ftoi(s_ox + (x + w) * s_scale), y1 = (float)e_ftoi(s_oy + (y + h) * s_scale);
    quad(x0, y0, x1, y1, 0.0f, 0.0f, 0.0f, 0.0f, argb, 0);
}

void ui_image(RTex t, float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 argb)
{
    float x0 = (float)e_ftoi(s_ox + x * s_scale), y0 = (float)e_ftoi(s_oy + y * s_scale);
    float x1 = (float)e_ftoi(s_ox + (x + w) * s_scale), y1 = (float)e_ftoi(s_oy + (y + h) * s_scale);
    quad(x0, y0, x1, y1, u0, v0, u1, v1, argb, t);
}

void ui_end(void) { flush(); }

/* ---- additions (jet T01) ---- */
void ui_set_blend(int blend)
{
    if (blend != RB_ADD && blend != RB_PREMUL) blend = RB_BLEND;   /* jet T02: + RB_PREMUL */
    if (blend == s_blend) return;
    flush();
    s_blend = blend;
}

void ui_clip(float x, float y, float w, float h)
{
    float x0, y0, x1, y1;
    if (w <= 0.0f || h <= 0.0f) {
        s_cx0 = 0.0f; s_cy0 = 0.0f; s_cx1 = (float)s_sw; s_cy1 = (float)s_sh;
        return;
    }
    x0 = (float)e_ftoi(s_ox + x * s_scale);       y0 = (float)e_ftoi(s_oy + y * s_scale);
    x1 = (float)e_ftoi(s_ox + (x + w) * s_scale); y1 = (float)e_ftoi(s_oy + (y + h) * s_scale);
    s_cx0 = E_CLAMP(x0, 0.0f, (float)s_sw); s_cy0 = E_CLAMP(y0, 0.0f, (float)s_sh);
    s_cx1 = E_CLAMP(x1, 0.0f, (float)s_sw); s_cy1 = E_CLAMP(y1, 0.0f, (float)s_sh);
}

static int clip_code(float x, float y, float x0, float y0, float x1, float y1)
{
    return (x < x0 ? 1 : 0) | (x > x1 ? 2 : 0) | (y < y0 ? 4 : 0) | (y > y1 ? 8 : 0);
}

/* Cohen-Sutherland: 0 = fully outside */
static int clip_seg(float *ax, float *ay, float *bx, float *by, float x0, float y0, float x1, float y1)
{
    int ca = clip_code(*ax, *ay, x0, y0, x1, y1), cb = clip_code(*bx, *by, x0, y0, x1, y1), c, it;
    float x = 0.0f, y = 0.0f;
    for (it = 0; it < 8; it++) {
        if (!(ca | cb)) return 1;
        if (ca & cb) return 0;
        c = ca ? ca : cb;
        if (c & 8)      { x = *ax + (*bx - *ax) * (y1 - *ay) / (*by - *ay); y = y1; }
        else if (c & 4) { x = *ax + (*bx - *ax) * (y0 - *ay) / (*by - *ay); y = y0; }
        else if (c & 2) { y = *ay + (*by - *ay) * (x1 - *ax) / (*bx - *ax); x = x1; }
        else            { y = *ay + (*by - *ay) * (x0 - *ax) / (*bx - *ax); x = x0; }
        if (c == ca) { *ax = x; *ay = y; ca = clip_code(x, y, x0, y0, x1, y1); }
        else         { *bx = x; *by = y; cb = clip_code(x, y, x0, y0, x1, y1); }
    }
    return !(ca | cb);
}

void ui_line(float x0, float y0, float x1, float y1, float w, u32 argb)
{
    float ax = s_ox + x0 * s_scale, ay = s_oy + y0 * s_scale, bx = s_ox + x1 * s_scale, by = s_oy + y1 * s_scale;
    float hw = w * s_scale * 0.5f, dx, dy, len, nx, ny;
    RVert2D *v;
    if (hw < 0.5f) hw = 0.5f;
    /* clip the centre line to the clip rectangle shrunk by the half width, so the quad never needs the screen
       clamp of r_draw_2d (which would bend it) */
    if (!clip_seg(&ax, &ay, &bx, &by, s_cx0 + hw, s_cy0 + hw, s_cx1 - hw, s_cy1 - hw)) return;
    dx = bx - ax; dy = by - ay;
    len = (float)sqrt(dx * dx + dy * dy);
    if (len < 0.001f) { dx = 1.0f; dy = 0.0f; len = 1.0f; }
    nx = -dy / len * hw; ny = dx / len * hw;
    if (s_tex != 0 || s_nq >= UI_MAXQ) { flush(); s_tex = 0; }
    v = &s_v[s_nq * 4];
    /* clockwise on screen: a (left), b, b', a' */
    v[0].x = ax + nx; v[0].y = ay + ny;
    v[1].x = ax - nx; v[1].y = ay - ny;
    v[2].x = bx - nx; v[2].y = by - ny;
    v[3].x = bx + nx; v[3].y = by + ny;
    {
        int k;
        for (k = 0; k < 4; k++) { v[k].u = 0.0f; v[k].v = 0.0f; v[k].color = argb; }
    }
    s_nq++;
    s_quads++;
}

/* ---- additions (jet T07) ---- */
void ui_quad4_px(RTex t, const RVert2D v[4])
{
    if (t != s_tex || s_nq >= UI_MAXQ) { flush(); s_tex = t; }
    memcpy(&s_v[s_nq * 4], v, 4 * sizeof(RVert2D));
    s_nq++;
    s_quads++;
}

void ui_line_tex(RTex t, float x0, float y0, float x1, float y1, float u, float v0, float v1, float widthPx,
                 float centrePx, u32 argb)
{
    float ax = s_ox + x0 * s_scale, ay = s_oy + y0 * s_scale, bx = s_ox + x1 * s_scale, by = s_oy + y1 * s_scale;
    float m = centrePx > widthPx - centrePx ? centrePx : widthPx - centrePx, dx, dy, len, nx, ny, a, b;
    RVert2D q[4];
    int k;
    if (!clip_seg(&ax, &ay, &bx, &by, s_cx0 + m, s_cy0 + m, s_cx1 - m, s_cy1 - m)) return;
    dx = bx - ax; dy = by - ay;
    len = (float)sqrt(dx * dx + dy * dy);
    if (len < 0.001f) { dx = 1.0f; dy = 0.0f; len = 1.0f; }
    nx = -dy / len; ny = dx / len;
    if (nx + ny < 0.0f) { nx = -nx; ny = -ny; }    /* +v towards the screen down/right side */
    a = -centrePx; b = widthPx - centrePx;
    q[0].x = ax + nx * a; q[0].y = ay + ny * a; q[0].v = v0;
    q[1].x = bx + nx * a; q[1].y = by + ny * a; q[1].v = v0;
    q[2].x = bx + nx * b; q[2].y = by + ny * b; q[2].v = v1;
    q[3].x = ax + nx * b; q[3].y = ay + ny * b; q[3].v = v1;
    for (k = 0; k < 4; k++) { q[k].u = u; q[k].color = argb; }
    ui_quad4_px(t, q);
}

int ui_quad_count(int reset)
{
    int n = s_quads;
    if (reset) s_quads = 0;
    return n;
}
