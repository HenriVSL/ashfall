/* hud_font.cpp - see hud_font.h. Windows/render side. Owner: T07. */
#include "hud_font.h"
#include "e_math.h"
#include "e_log.h"
#include "e_file.h"
#include "e_image.h"
#include "e_tex.h"
#include "e_ui.h"
#include "params.h"

enum { HG_MAXPAGES = 4, HG_MAXSPR = 64, HG_MAXQ = 1536, HG_MAXTEX = 8 };

typedef struct HGlyph { short x, y, w, h, ox, adv; u8 ok; } HGlyph;
typedef struct HFontM { int cap, cellH, oy, adv, gap, inkW, page, ok; HGlyph g[128]; } HFontM;
typedef struct HSpr { char name[24]; short x, y, w, h, ox, oy; int page; } HSpr;
typedef struct HQuad { RVert2D v[4]; RTex tex; } HQuad;

static int     s_atlasSetParam;                     /* [hud] atlasSet */
static int     s_lines, s_pages, s_stroke, s_shOff, s_ready, s_inited;
static RTex    s_tex[HG_MAXPAGES];
static HFontM  s_font[2];
static HSpr    s_spr[HG_MAXSPR];
static int     s_nspr;
static HSpr    s_white[HG_MAXPAGES], s_line[HG_MAXPAGES];
static int     s_haveWhite[HG_MAXPAGES], s_haveLine[HG_MAXPAGES];
static Font    s_gdi[2];
static int     s_gdiOk[2];
static float   s_k = 1.0f;
static int     s_sw = 640, s_sh = 480;

static HQuad   s_q[HG_MAXQ];
static int     s_nq, s_overflow;
static int     s_clip; static float s_cx0, s_cy0, s_cx1, s_cy1;
static int     s_nex; static float s_ex[6][4];         /* keep-out boxes */
static int     s_quadsFrame, s_drawsFrame, s_quadsLast, s_drawsLast;
static int     s_bHave; static float s_bx0, s_by0, s_bx1, s_by1;

static const Param s_tab[] = {
    { "hud", "atlasSet", P_INT, &s_atlasSetParam, "0", 0, 1050,
      "HUD atlas set: 0 = by screen height, else 480 / 600 / 768 / 1050 (restart)", P_RESTART },
};
void hfont_params(void) { params_add(s_tab, E_COUNT(s_tab)); }

/* ---------------------------------------------------------------- loading */
static void set_white_line(HSpr *dst, int *have, const HSpr *s)
{
    if (s->page < 0 || s->page >= HG_MAXPAGES) return;
    dst[s->page] = *s;
    have[s->page] = 1;
}

static int parse_txt(const char *path)
{
    char *txt, *line, *next;
    int size;
    txt = (char *)file_load(path, &size);
    if (!txt) return 0;
    for (line = txt; line && *line; line = next) {
        char name[32];
        int a[12], n;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (line[0] == '#' || line[0] == '\r' || !line[0]) continue;
        if (!strncmp(line, "set ", 4)) {
            memset(a, 0, sizeof(a));
            n = sscanf(line + 4, "%d %d %d %d %d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7],
                       &a[8], &a[9]);
            if (n >= 6) { s_lines = a[0]; s_pages = a[1]; s_stroke = a[5]; }
            if (n >= 9) s_shOff = a[8];
            continue;
        }
        if (!strncmp(line, "page ", 5)) continue;   /* handled by the caller (texture names) */
        if (!strncmp(line, "font ", 5)) {
            memset(a, 0, sizeof(a));
            n = sscanf(line + 5, "%d %d %d %d %d %d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6],
                       &a[7], &a[8], &a[9], &a[10]);
            if (n >= 7 && a[0] >= 0 && a[0] < 2) {
                HFontM *f = &s_font[a[0]];
                f->cap = a[1]; f->cellH = a[2]; f->oy = a[3]; f->adv = a[4]; f->gap = a[6];
                f->inkW = n >= 8 ? a[7] : a[4] - a[6];
                f->page = n >= 11 ? a[10] : 0;
                f->ok = 1;
            }
            continue;
        }
        if (line[0] == 'g' && line[1] == ' ') {
            if (sscanf(line + 2, "%d %d %d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7]) == 8 &&
                a[0] >= 0 && a[0] < 2 && a[1] > 0 && a[1] < 128) {
                HGlyph *g = &s_font[a[0]].g[a[1]];
                g->x = (short)a[2]; g->y = (short)a[3]; g->w = (short)a[4]; g->h = (short)a[5];
                g->ox = (short)a[6]; g->adv = (short)a[7]; g->ok = 1;
            }
            continue;
        }
        memset(a, 0, sizeof(a));
        n = sscanf(line, "%31s %d %d %d %d %d %d %d", name, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]);
        if (n >= 7) {
            HSpr s;
            memset(&s, 0, sizeof(s));
            e_strlcpy(s.name, name, (int)sizeof(s.name));
            s.x = (short)a[0]; s.y = (short)a[1]; s.w = (short)a[2]; s.h = (short)a[3]; s.ox = (short)a[4];
            s.oy = (short)a[5]; s.page = n >= 8 ? a[6] : 0;
            if (!strcmp(name, "white")) set_white_line(s_white, s_haveWhite, &s);
            else if (!strcmp(name, "line_sh")) set_white_line(s_line, s_haveLine, &s);
            else if (!strcmp(name, "line")) { if (!s_shOff) set_white_line(s_line, s_haveLine, &s); }
            else if (s_nspr < HG_MAXSPR) s_spr[s_nspr++] = s;
        }
    }
    free(txt);
    return 1;
}

static RTex load_page(const char *name)
{
    char p1[E_PATH], p2[E_PATH];
    EImage rgb, al;
    RTex t = 0;
    int i, ok = 0;
    e_snprintf(p1, (int)sizeof(p1), "tex/%s.bmp", name);
    e_snprintf(p2, (int)sizeof(p2), "tex/%s_a.bmp", name);
    /* NOT tex_get(.., IMG_ALPHA): img_bleed would fill the transparent texels with white (DESIGN 9.1b) */
    if (!img_load(&rgb, p1, 0)) return 0;
    if (img_load(&al, p2, 0)) {
        if (al.w == rgb.w && al.h == rgb.h) {
            for (i = 0; i < rgb.w * rgb.h; i++) rgb.pix[0][i] = (rgb.pix[0][i] & 0xFFFFFFu) | ((al.pix[0][i] >> 16 & 0xFF) << 24);
            ok = 1;
        }
        img_free(&al);
    }
    rgb.flags = IMG_ALPHA;
    if (ok) {
        char tn[48];
        e_snprintf(tn, (int)sizeof(tn), "%s_premul", name);
        t = tex_add(tn, &rgb, RT_CLAMP | RT_NOMIP);
    }
    img_free(&rgb);
    return t;
}

static int load_set(int lines)
{
    char rel[64], path[E_PATH], *txt, *line, *next;
    int size, i;
    s_lines = s_pages = s_stroke = s_shOff = 0;
    s_nspr = 0;
    memset(s_font, 0, sizeof(s_font));
    memset(s_haveWhite, 0, sizeof(s_haveWhite));
    memset(s_haveLine, 0, sizeof(s_haveLine));
    memset(s_tex, 0, sizeof(s_tex));
    e_snprintf(rel, (int)sizeof(rel), "tex\\hud_atlas_%d.txt", lines);
    data_path(path, (int)sizeof(path), rel);
    if (!parse_txt(path)) return 0;
    /* 'line' was read before 'line_sh' only when unshadowed; prefer line_sh when present (A4c) */
    txt = (char *)file_load(path, &size);
    if (!txt) return 0;
    for (line = txt; line && *line; line = next) {
        int pn;
        char name[48];
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (sscanf(line, "page %d %47s", &pn, name) == 2 && pn >= 0 && pn < HG_MAXPAGES) s_tex[pn] = load_page(name);
    }
    free(txt);
    if (s_pages < 1) s_pages = 1;
    if (s_pages > HG_MAXPAGES) s_pages = HG_MAXPAGES;
    for (i = 0; i < s_pages; i++) if (!s_tex[i] || !s_haveWhite[i] || !s_haveLine[i]) return 0;
    if (!s_font[0].ok || !s_font[1].ok || s_stroke <= 0) return 0;
    return 1;
}

int hfont_init(void)
{
    static const int sets[4] = { 480, 600, 768, 1050 };
    int want, i, ok = 0;
    if (s_inited) hfont_free();
    r_screen_size(&s_sw, &s_sh);
    if (s_sw <= 0 || s_sh <= 0) { s_sw = 640; s_sh = 480; }
    s_k = (float)s_sh / 480.0f;
    want = s_atlasSetParam > 0 ? s_atlasSetParam : s_sh;
    for (i = 3; i >= 0; i--) {
        if (sets[i] > want && i > 0) continue;
        ok = load_set(sets[i]);
        if (ok) break;
    }
    s_ready = ok;
    s_inited = 1;
    if (ok) {
        log_printf(LOG_INFO, "hud: atlas set=%d pages=%d stroke=%d shadow=%d sprites=%d screen=%dx%d k=%.4f small=%d/%d "
                   "large=%d/%d", s_lines, s_pages, s_stroke, s_shOff, s_nspr, s_sw, s_sh, (double)s_k, s_font[0].cap,
                   s_font[0].adv, s_font[1].cap, s_font[1].adv);
    } else {
        int px;
        s_lines = 0; s_pages = 0; s_shOff = 0;
        s_stroke = e_ftoi(2.0f * s_k * 0.8f); if (s_stroke < 1) s_stroke = 1;
        px = e_ftoi(9.0f * s_k * 1.45f);
        s_gdiOk[0] = font_create(&s_gdi[0], "Courier New", px, 1, 0);
        px = e_ftoi(13.0f * s_k * 1.45f);
        s_gdiOk[1] = font_create(&s_gdi[1], "Courier New", px, 1, 0);
        log_printf(LOG_WARN, "hud: atlas missing, GDI fallback fonts=%d,%d", s_gdiOk[0], s_gdiOk[1]);
    }
    return ok;
}

void hfont_free(void)
{
    int i;
    for (i = 0; i < HG_MAXPAGES; i++) if (s_tex[i]) { tex_release(s_tex[i]); s_tex[i] = 0; }
    for (i = 0; i < 2; i++) if (s_gdiOk[i]) { font_free(&s_gdi[i]); s_gdiOk[i] = 0; }
    s_ready = 0;
    s_inited = 0;
}

/* ---------------------------------------------------------------- basics */
int   hg_ready(void) { return s_ready; }
int   hg_set_lines(void) { return s_lines; }
float hg_k(void) { return s_k; }
int   hg_px(float v) { return e_ftoi(v * s_k); }
float hg_pxf(float v) { return v * s_k; }
int   hg_stroke(void) { return s_stroke; }
int   hg_shadow_off(void) { return s_shOff; }
int   hg_screen_w(void) { return s_sw; }
int   hg_screen_h(void) { return s_sh; }

u32 hg_col(u32 argb, float a)
{
    float aa = (float)(argb >> 24) / 255.0f * E_CLAMP(a, 0.0f, 1.0f);
    int A = e_ftoi(aa * 255.0f), R, G, B;
    R = e_ftoi((float)((argb >> 16) & 0xFF) * aa);
    G = e_ftoi((float)((argb >> 8) & 0xFF) * aa);
    B = e_ftoi((float)(argb & 0xFF) * aa);
    return ((u32)A << 24) | ((u32)R << 16) | ((u32)G << 8) | (u32)B;
}

/* ---------------------------------------------------------------- record + emit */
static void bounds_add(const RVert2D *v)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (!s_bHave) { s_bx0 = s_bx1 = v[i].x; s_by0 = s_by1 = v[i].y; s_bHave = 1; continue; }
        if (v[i].x < s_bx0) s_bx0 = v[i].x;
        if (v[i].x > s_bx1) s_bx1 = v[i].x;
        if (v[i].y < s_by0) s_by0 = v[i].y;
        if (v[i].y > s_by1) s_by1 = v[i].y;
    }
}

static void rec(RTex t, const RVert2D *v)
{
    if (s_nq >= HG_MAXQ) { s_overflow++; return; }
    memcpy(s_q[s_nq].v, v, sizeof(s_q[s_nq].v));
    s_q[s_nq].tex = t;
    s_nq++;
    bounds_add(v);
}

void hg_flush(void)
{
    RTex order[HG_MAXTEX];
    int no = 0, i, j;
    if (s_nq <= 0) return;
    for (i = 0; i < s_nq; i++) {                    /* distinct textures in order of first use */
        for (j = 0; j < no; j++) if (order[j] == s_q[i].tex) break;
        if (j == no && no < HG_MAXTEX) order[no++] = s_q[i].tex;
    }
    for (j = 0; j < no; j++) {
        for (i = 0; i < s_nq; i++) if (s_q[i].tex == order[j]) ui_quad4_px(order[j], s_q[i].v);
        s_drawsFrame++;
    }
    s_quadsFrame += s_nq;
    s_nq = 0;
}

void hg_begin(void)
{
    r_screen_size(&s_sw, &s_sh);
    if (s_sw <= 0 || s_sh <= 0) { s_sw = 640; s_sh = 480; }
    s_k = (float)s_sh / 480.0f;
    ui_begin(640, 480);
    ui_set_blend(s_ready ? RB_PREMUL : RB_BLEND);
    s_nq = 0;
    s_clip = 0;
    s_nex = 0;
    s_quadsFrame = s_drawsFrame = 0;
}

void hg_end(void)
{
    hg_flush();
    ui_end();
    s_quadsLast = s_quadsFrame;
    s_drawsLast = s_drawsFrame;
    if (s_overflow) { log_printf(LOG_WARN, "warn: hud record overflow quads=%d", s_overflow); s_overflow = 0; }
}

int hg_quads_last(void) { return s_quadsLast; }
int hg_draws_last(void) { return s_drawsLast; }
void hg_bounds_reset(void) { s_bHave = 0; }
int hg_bounds(float *x0, float *y0, float *x1, float *y1)
{
    if (!s_bHave) return 0;
    *x0 = s_bx0; *y0 = s_by0; *x1 = s_bx1; *y1 = s_by1;
    return 1;
}

void hg_exclude(int x0, int y0, int x1, int y1)
{
    if (s_nex >= 6 || x1 <= x0 || y1 <= y0) return;
    s_ex[s_nex][0] = (float)x0; s_ex[s_nex][1] = (float)y0; s_ex[s_nex][2] = (float)x1; s_ex[s_nex][3] = (float)y1;
    s_nex++;
}

void hg_exclude_clear(void) { s_nex = 0; }

static int excluded_box(float x0, float y0, float x1, float y1)
{
    int i;
    for (i = 0; i < s_nex; i++)
        if (x1 > s_ex[i][0] && x0 < s_ex[i][2] && y1 > s_ex[i][1] && y0 < s_ex[i][3]) return 1;
    return 0;
}

void hg_clip(int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0) { s_clip = 0; return; }
    s_clip = 1;
    s_cx0 = (float)x0; s_cy0 = (float)y0; s_cx1 = (float)x1; s_cy1 = (float)y1;
}

/* axis-aligned quad with uv, clipped to the clip box */
static void aquad(RTex t, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 c)
{
    RVert2D v[4];
    if (s_clip) {
        if (x1 <= s_cx0 || y1 <= s_cy0 || x0 >= s_cx1 || y0 >= s_cy1) return;
        if (x0 < s_cx0) { u0 += (u1 - u0) * ((s_cx0 - x0) / (x1 - x0)); x0 = s_cx0; }
        if (y0 < s_cy0) { v0 += (v1 - v0) * ((s_cy0 - y0) / (y1 - y0)); y0 = s_cy0; }
        if (x1 > s_cx1) { u1 -= (u1 - u0) * ((x1 - s_cx1) / (x1 - x0)); x1 = s_cx1; }
        if (y1 > s_cy1) { v1 -= (v1 - v0) * ((y1 - s_cy1) / (y1 - y0)); y1 = s_cy1; }
    }
    v[0].x = x0; v[0].y = y0; v[0].u = u0; v[0].v = v0; v[0].color = c;
    v[1].x = x1; v[1].y = y0; v[1].u = u1; v[1].v = v0; v[1].color = c;
    v[2].x = x1; v[2].y = y1; v[2].u = u1; v[2].v = v1; v[2].color = c;
    v[3].x = x0; v[3].y = y1; v[3].u = u0; v[3].v = v1; v[3].color = c;
    rec(t, v);
}

static int ccode(float x, float y, float x0, float y0, float x1, float y1)
{
    return (x < x0 ? 1 : 0) | (x > x1 ? 2 : 0) | (y < y0 ? 4 : 0) | (y > y1 ? 8 : 0);
}

static int clip_seg(float *ax, float *ay, float *bx, float *by, float x0, float y0, float x1, float y1)
{
    int ca = ccode(*ax, *ay, x0, y0, x1, y1), cb = ccode(*bx, *by, x0, y0, x1, y1), c, it;
    float x = 0.0f, y = 0.0f;
    if (x1 <= x0 || y1 <= y0) return 0;
    for (it = 0; it < 8; it++) {
        if (!(ca | cb)) return 1;
        if (ca & cb) return 0;
        c = ca ? ca : cb;
        if (c & 8)      { x = *ax + (*bx - *ax) * (y1 - *ay) / (*by - *ay); y = y1; }
        else if (c & 4) { x = *ax + (*bx - *ax) * (y0 - *ay) / (*by - *ay); y = y0; }
        else if (c & 2) { y = *ay + (*by - *ay) * (x1 - *ax) / (*bx - *ax); x = x1; }
        else            { y = *ay + (*by - *ay) * (x0 - *ax) / (*bx - *ax); x = x0; }
        if (c == ca) { *ax = x; *ay = y; ca = ccode(x, y, x0, y0, x1, y1); }
        else         { *bx = x; *by = y; cb = ccode(x, y, x0, y0, x1, y1); }
    }
    return !(ca | cb);
}

/* one textured line quad between two centre points (already extended and clipped) */
static void line_quad(float ax, float ay, float bx, float by, float dx, float dy, float w, float cen, float u, float v0,
                      float v1, RTex t, u32 c)
{
    float nx = -dy, ny = dx, a, b;
    RVert2D q[4];
    int k;
    if (nx + ny < 0.0f) { nx = -nx; ny = -ny; }    /* +v (the baked shadow) towards down/right */
    a = -cen; b = w - cen;
    q[0].x = ax + nx * a; q[0].y = ay + ny * a; q[0].v = v0;
    q[1].x = bx + nx * a; q[1].y = by + ny * a; q[1].v = v0;
    q[2].x = bx + nx * b; q[2].y = by + ny * b; q[2].v = v1;
    q[3].x = ax + nx * b; q[3].y = ay + ny * b; q[3].v = v1;
    for (k = 0; k < 4; k++) { q[k].u = u; q[k].color = c; }
    rec(t, q);
}

/* parameter interval [t0, t1] of the segment inside box e (Liang-Barsky); 0 = misses */
static int seg_in_box(float ax, float ay, float bx, float by, const float *e, float m, float *t0, float *t1)
{
    float p[4], q[4], lo = 0.0f, hi = 1.0f;
    int i;
    p[0] = -(bx - ax); q[0] = ax - (e[0] - m);
    p[1] = bx - ax;    q[1] = (e[2] + m) - ax;
    p[2] = -(by - ay); q[2] = ay - (e[1] - m);
    p[3] = by - ay;    q[3] = (e[3] + m) - ay;
    for (i = 0; i < 4; i++) {
        if (p[i] == 0.0f) { if (q[i] < 0.0f) return 0; continue; }
        {
            float r = q[i] / p[i];
            if (p[i] < 0.0f) { if (r > hi) return 0; if (r > lo) lo = r; }
            else { if (r < lo) return 0; if (r < hi) hi = r; }
        }
    }
    *t0 = lo; *t1 = hi;
    return hi > lo;
}

static void line_core(float ax, float ay, float bx, float by, float ext, float ws, u32 c)
{
    float dx = bx - ax, dy = by - ay, len = (float)sqrt(dx * dx + dy * dy), w, cen;
    float u = 0.0f, v0 = 0.0f, v1 = 0.0f, m;
    float seg[16][4];
    RTex t = 0;
    int ns, i;
    if (len < 0.01f) { dx = 1.0f; dy = 0.0f; len = 1.0f; if (ext <= 0.0f) return; }
    dx /= len; dy /= len;
    ax -= dx * ext; ay -= dy * ext; bx += dx * ext; by += dy * ext;
    if (s_ready) {
        const HSpr *L = &s_line[0];
        w = (float)L->h * ws;
        cen = (float)L->oy * ws;
        u = ((float)L->x + (float)L->w * 0.5f) / 256.0f;
        v0 = (float)L->y / 256.0f;
        v1 = (float)(L->y + L->h) / 256.0f;
        t = s_tex[0];
    } else {
        w = (float)s_stroke * ws;
        if (w < 1.0f) w = 1.0f;
        cen = w * 0.5f;
    }
    m = cen > w - cen ? cen : w - cen;
    if (s_clip && !clip_seg(&ax, &ay, &bx, &by, s_cx0 + m, s_cy0 + m, s_cx1 - m, s_cy1 - m)) return;
    if (!s_nex) { line_quad(ax, ay, bx, by, dx, dy, w, cen, u, v0, v1, t, c); return; }
    /* cut around the keep-out boxes */
    seg[0][0] = ax; seg[0][1] = ay; seg[0][2] = bx; seg[0][3] = by;
    ns = 1;
    for (i = 0; i < s_nex; i++) {
        float out[16][4];
        int no = 0, j;
        for (j = 0; j < ns; j++) {
            float t0, t1, *sg = seg[j];
            if (!seg_in_box(sg[0], sg[1], sg[2], sg[3], s_ex[i], m, &t0, &t1)) {
                if (no < 16) { memcpy(out[no], sg, sizeof(out[0])); no++; }
                continue;
            }
            if (t0 > 0.001f && no < 16) {
                out[no][0] = sg[0]; out[no][1] = sg[1];
                out[no][2] = sg[0] + (sg[2] - sg[0]) * t0; out[no][3] = sg[1] + (sg[3] - sg[1]) * t0; no++;
            }
            if (t1 < 0.999f && no < 16) {
                out[no][0] = sg[0] + (sg[2] - sg[0]) * t1; out[no][1] = sg[1] + (sg[3] - sg[1]) * t1;
                out[no][2] = sg[2]; out[no][3] = sg[3]; no++;
            }
        }
        memcpy(seg, out, (size_t)no * sizeof(seg[0]));
        ns = no;
    }
    for (i = 0; i < ns; i++) line_quad(seg[i][0], seg[i][1], seg[i][2], seg[i][3], dx, dy, w, cen, u, v0, v1, t, c);
}

void hg_line(float x0, float y0, float x1, float y1, u32 argb) { line_core(x0, y0, x1, y1, (float)s_stroke * 0.5f, 1.0f, argb); }
void hg_line_ext(float x0, float y0, float x1, float y1, float extPx, u32 argb) { line_core(x0, y0, x1, y1, extPx, 1.0f, argb); }
void hg_line_w(float x0, float y0, float x1, float y1, float ws, u32 argb)
{
    line_core(x0, y0, x1, y1, (float)s_stroke * 0.2f * ws, ws, argb);   /* short ends: chained chords barely overlap */
}

static void white_uv(float *u, float *v)
{
    if (s_ready) { *u = ((float)s_white[0].x + 1.0f) / 256.0f; *v = ((float)s_white[0].y + 1.0f) / 256.0f; }
    else { *u = 0.0f; *v = 0.0f; }
}

void hg_rect(float x0, float y0, float x1, float y1, u32 argb)
{
    float u, v;
    white_uv(&u, &v);
    if (x1 <= x0 || y1 <= y0) return;
    aquad(s_ready ? s_tex[0] : 0, x0, y0, x1, y1, u, v, u, v, argb);
}

void hg_tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 argb)
{
    RVert2D q[4];
    float u, v;
    int k;
    white_uv(&u, &v);
    q[0].x = x0; q[0].y = y0; q[1].x = x1; q[1].y = y1; q[2].x = x2; q[2].y = y2; q[3] = q[2];
    for (k = 0; k < 4; k++) { q[k].u = u; q[k].v = v; q[k].color = argb; }
    rec(s_ready ? s_tex[0] : 0, q);
}

/* crisp outline: the stroke covers whole pixel rows/columns inside the rect */
void hg_box(int x0, int y0, int x1, int y1, u32 argb)
{
    int s = s_stroke, h = s / 2;
    float fx0 = (float)x0, fy0 = (float)y0, fx1 = (float)x1, fy1 = (float)y1;
    if (s_ready) {
        /* the profile's core rows start (stroke/2) above the record's centre edge: centre = edge + stroke/2 */
        line_core(fx0, fy0 + (float)h, fx1, fy0 + (float)h, 0.0f, 1.0f, argb);
        line_core(fx0, fy1 - (float)(s - h), fx1, fy1 - (float)(s - h), 0.0f, 1.0f, argb);
        line_core(fx0 + (float)h, fy0 + (float)s, fx0 + (float)h, fy1 - (float)s, 0.0f, 1.0f, argb);
        line_core(fx1 - (float)(s - h), fy0 + (float)s, fx1 - (float)(s - h), fy1 - (float)s, 0.0f, 1.0f, argb);
    } else {
        hg_rect(fx0, fy0, fx1, fy0 + (float)s, argb);
        hg_rect(fx0, fy1 - (float)s, fx1, fy1, argb);
        hg_rect(fx0, fy0 + (float)s, fx0 + (float)s, fy1 - (float)s, argb);
        hg_rect(fx1 - (float)s, fy0 + (float)s, fx1, fy1 - (float)s, argb);
    }
}

/* ---------------------------------------------------------------- text */
static int map_char(int c)
{
    if (c >= 'a' && c <= 'z' && c != 'c') c -= 32;
    if (c <= 0 || c >= 128) c = '?';
    return c;
}

int hg_cap(int font)
{
    font = font ? 1 : 0;
    if (s_ready) return s_font[font].cap;
    return s_gdiOk[font] ? s_gdi[font].ascent * 3 / 4 : 8;
}
int hg_adv(int font)
{
    font = font ? 1 : 0;
    if (s_ready) return s_font[font].adv;
    return s_gdiOk[font] ? s_gdi[font].adv['0'] : 7;
}
int hg_gap(int font) { font = font ? 1 : 0; return s_ready ? s_font[font].gap : 0; }

int hg_text_w(int font, const char *s)
{
    int w = 0;
    font = font ? 1 : 0;
    if (!s || !*s) return 0;
    if (s_ready) {
        const HFontM *f = &s_font[font];
        for (; *s; s++) {
            int c = map_char((unsigned char)*s);
            const HGlyph *g = &f->g[c];
            if (!g->ok) g = &f->g['?'];
            w += g->ok ? g->adv : f->adv;
        }
        return w - f->gap;
    }
    return s_gdiOk[font] ? font_width(&s_gdi[font], s) : (int)strlen(s) * 7;
}

int hg_text(int x, int y, int font, u32 argb, int align, const char *s)
{
    int w, pen;
    font = font ? 1 : 0;
    if (!s || !*s) return 0;
    w = hg_text_w(font, s);
    pen = align == 1 ? x - w / 2 : (align == 2 ? x - w : x);
    if (s_nex && excluded_box((float)pen - 1.0f, (float)y - 1.0f, (float)(pen + w + 1), (float)(y + hg_cap(font) + 1))) return w;
    if (s_ready) {
        const HFontM *f = &s_font[font];
        RTex t = s_tex[f->page];
        for (; *s; s++) {
            int c = map_char((unsigned char)*s);
            const HGlyph *g = &f->g[c];
            if (!g->ok) g = &f->g['?'];
            if (!g->ok) { pen += f->adv; continue; }
            if (g->w > 0) {
                float gx = (float)(pen + g->ox), gy = (float)(y + f->oy);
                aquad(t, gx, gy, gx + (float)g->w, gy + (float)g->h, (float)g->x / 256.0f, (float)g->y / 256.0f,
                      (float)(g->x + g->w) / 256.0f, (float)(g->y + g->h) / 256.0f, argb);
            }
            pen += g->adv;
        }
    } else if (s_gdiOk[font]) {
        const Font *f = &s_gdi[font];
        float iw = 1.0f / (float)f->texW, ih = 1.0f / (float)f->texH;
        int top = y - (f->ascent - hg_cap(font));
        for (; *s; s++) {
            int c = (unsigned char)*s;
            if (f->gw[c]) {
                float x0 = (float)(pen + f->ox[c]), y0 = (float)(top - (f->gh[c] - f->height) / 2);
                aquad(f->tex, x0, y0, x0 + (float)f->gw[c], y0 + (float)f->gh[c], (float)f->gu[c] * iw, (float)f->gv[c] * ih,
                      (float)(f->gu[c] + f->gw[c]) * iw, (float)(f->gv[c] + f->gh[c]) * ih, argb);
            }
            pen += f->adv[c];
        }
    }
    return w;
}

void hfont_text(float x, float y, int size, u32 argb, int align, const char *s)
{
    hg_text(hg_px(x), hg_px(y), size, argb, align, s);
}

float hfont_width(int size, const char *s) { return (float)hg_text_w(size, s) / s_k; }

/* ---------------------------------------------------------------- sprites */
int hg_sprite_id(const char *name)
{
    int i;
    for (i = 0; i < s_nspr; i++) if (!strcmp(s_spr[i].name, name)) return i;
    return -1;
}

int hg_sprite_info(int id, int *w, int *h, int *ox, int *oy)
{
    const HSpr *s;
    if (id < 0 || id >= s_nspr) return 0;
    s = &s_spr[id];
    if (w) *w = s->w - s_shOff;
    if (h) *h = s->h - s_shOff;
    if (ox) *ox = s->ox;
    if (oy) *oy = s->oy;
    return 1;
}

static void fallback_marker(float ax, float ay, u32 argb)
{
    float r = 4.0f * s_k;
    hg_line(ax - r, ay - r, ax + r, ay - r, argb);
    hg_line(ax + r, ay - r, ax + r, ay + r, argb);
    hg_line(ax + r, ay + r, ax - r, ay + r, argb);
    hg_line(ax - r, ay + r, ax - r, ay - r, argb);
}

void hg_sprite(int id, float ax, float ay, u32 argb)
{
    const HSpr *s;
    float x0, y0;
    if (id < 0 || id >= s_nspr) { if (!s_ready) fallback_marker(ax, ay, argb); return; }
    s = &s_spr[id];
    x0 = (float)(e_ftoi(ax) - s->ox);
    y0 = (float)(e_ftoi(ay) - s->oy);
    if (s_nex && excluded_box(x0, y0, x0 + (float)s->w, y0 + (float)s->h)) return;
    aquad(s_tex[s->page], x0, y0, x0 + (float)s->w, y0 + (float)s->h, (float)s->x / 256.0f, (float)s->y / 256.0f,
          (float)(s->x + s->w) / 256.0f, (float)(s->y + s->h) / 256.0f, argb);
}

static void sprite_xf(int id, float ax, float ay, float ca, float sa, float sc, u32 argb)
{
    const HSpr *s;
    float lx[4], ly[4];
    RVert2D q[4];
    int k;
    if (id < 0 || id >= s_nspr) { if (!s_ready) fallback_marker(ax, ay, argb); return; }
    s = &s_spr[id];
    lx[0] = (float)-s->ox;          ly[0] = (float)-s->oy;
    lx[1] = (float)(s->w - s->ox);  ly[1] = (float)-s->oy;
    lx[2] = (float)(s->w - s->ox);  ly[2] = (float)(s->h - s->oy);
    lx[3] = (float)-s->ox;          ly[3] = (float)(s->h - s->oy);
    for (k = 0; k < 4; k++) {
        float x = lx[k] * sc, y = ly[k] * sc;
        q[k].x = ax + x * ca - y * sa;
        q[k].y = ay + x * sa + y * ca;
        q[k].color = argb;
        if (s_clip && (q[k].x < s_cx0 || q[k].x > s_cx1 || q[k].y < s_cy0 || q[k].y > s_cy1)) return;
    }
    q[0].u = (float)s->x / 256.0f;          q[0].v = (float)s->y / 256.0f;
    q[1].u = (float)(s->x + s->w) / 256.0f; q[1].v = q[0].v;
    q[2].u = q[1].u;                        q[2].v = (float)(s->y + s->h) / 256.0f;
    q[3].u = q[0].u;                        q[3].v = q[2].v;
    rec(s_tex[s->page], q);
}

void hg_sprite_rot(int id, float ax, float ay, float ang, u32 argb)
{
    sprite_xf(id, ax, ay, (float)cos(ang), (float)sin(ang), 1.0f, argb);
}

void hg_sprite_scaled(int id, float ax, float ay, float scale, u32 argb)
{
    sprite_xf(id, ax, ay, 1.0f, 0.0f, scale, argb);
}

/* 9-slice frame: corner c = (w - o) / 3; right/bottom slices are c + o wide (they carry the baked shadow) */
void hg_nine(int id, int x0, int y0, int x1, int y1, u32 argb)
{
    const HSpr *s;
    float sx[4], sy[4], dx[4], dy[4], u[4], v[4];
    int c, o = s_shOff, i, j;
    RTex t;
    if (id < 0 || id >= s_nspr) {
        hg_box(x0, y0, x1, y1, argb);
        return;
    }
    s = &s_spr[id];
    t = s_tex[s->page];
    c = (s->w - o) / 3;
    sx[0] = (float)s->x; sx[1] = (float)(s->x + c); sx[2] = (float)(s->x + 2 * c); sx[3] = (float)(s->x + s->w);
    sy[0] = (float)s->y; sy[1] = (float)(s->y + c); sy[2] = (float)(s->y + 2 * c); sy[3] = (float)(s->y + s->h);
    dx[0] = (float)x0; dx[1] = (float)(x0 + c); dx[2] = (float)(x1 - c); dx[3] = (float)(x1 + o);
    dy[0] = (float)y0; dy[1] = (float)(y0 + c); dy[2] = (float)(y1 - c); dy[3] = (float)(y1 + o);
    for (i = 0; i < 4; i++) { u[i] = sx[i] / 256.0f; v[i] = sy[i] / 256.0f; }
    for (j = 0; j < 3; j++) for (i = 0; i < 3; i++) {
        if (i == 1 && j == 1) continue;
        aquad(t, dx[i], dy[j], dx[i + 1], dy[j + 1], u[i], v[j], u[i + 1], v[j + 1], argb);
    }
}

void hg_image_quad(RTex t, const RVert2D v[4], int blend)
{
    int back = s_ready ? RB_PREMUL : RB_BLEND;
    hg_flush();
    ui_set_blend(blend);
    ui_quad4_px(t, v);
    s_quadsFrame++;
    s_drawsFrame++;
    bounds_add(v);
    ui_set_blend(back);
}

int hfont_set_metrics(int lines, int font, int *cap, int *adv, int *gap, int *stroke)
{
    char rel[64], path[E_PATH], *txt, *line, *next;
    int size, a[12], found = 0;
    e_snprintf(rel, (int)sizeof(rel), "tex\\hud_atlas_%d.txt", lines);
    data_path(path, (int)sizeof(path), rel);
    txt = (char *)file_load(path, &size);
    if (!txt) return 0;
    for (line = txt; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        memset(a, 0, sizeof(a));
        if (!strncmp(line, "set ", 4) && sscanf(line + 4, "%d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
            if (stroke) *stroke = a[5];
            found |= 1;
        } else if (!strncmp(line, "font ", 5) &&
                   sscanf(line + 5, "%d %d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]) == 7 &&
                   a[0] == font) {
            if (cap) *cap = a[1];
            if (adv) *adv = a[4];
            if (gap) *gap = a[6];
            found |= 2;
        }
    }
    free(txt);
    return found == 3;
}
