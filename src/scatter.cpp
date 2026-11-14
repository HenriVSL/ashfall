/* scatter.cpp - island vegetation / rock / village sprites (A11). See scatter.h. Owner: A11. */
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "scatter.h"
#include "scene.h"
#include "params.h"
#include "e_log.h"
#include "e_prof.h"
#include "e_plat.h"
#include "e_rend.h"
#include "e_tex.h"
#include "e_image.h"
#include "e_file.h"

enum { SC_MAX_TYPES = 16, SC_MAX_SPRITES = 4096, SC_BATCH = 1024, SC_MAX_CELLS = 128 };

typedef struct ScType { float u0, v0, u1, v1, w, h, sink; int kind; } ScType;
typedef struct ScCell { u32 first, count; float yMin, yMax; } ScCell;
typedef struct ScInst { float x, y, z; u8 type, scale, shade, yaw; } ScInst;

/* ---------------------------------------------------------------- params (defaults also work unregistered) */
static int   s_on = 1;
static float s_range = 0.0f, s_nearFrac = 0.35f, s_farDens = 0.0f, s_fadeFrac = 0.15f, s_tiltK = 0.5f;
static int   s_max = 0;
static float s_bright = 1.0f;
static const Param s_tab[] = {
    { "scatter", "on", P_BOOL, &s_on, "1", 0, 1, "A11 island vegetation / rock / village sprites (data/island/veg.sct)" },
    { "scatter", "rangeM", P_FLOAT, &s_range, "0", 0, 8000, "sprites drawn within this distance (m); 0 = preset (Low 1600, High 3500)" },
    { "scatter", "maxSprites", P_INT, &s_max, "0", 0, SC_MAX_SPRITES, "cap per frame, nearest kept; 0 = preset (Low 600, High 2400)" },
    { "scatter", "nearFrac", P_FLOAT, &s_nearFrac, "0.35", 0, 1, "full density up to this fraction of the range" },
    { "scatter", "farDensity", P_FLOAT, &s_farDens, "0", 0, 1, "density at the range (thinned in between); 0 = preset (Low 0.35, High 0.6)" },
    { "scatter", "fadeFrac", P_FLOAT, &s_fadeFrac, "0.15", 0.01f, 1, "sprites grow in from zero over this outer fraction of the range" },
    { "scatter", "tiltK", P_FLOAT, &s_tiltK, "0.5", 0, 1, "camera-facing cards lean back by this x the view elevation (max 35 deg)" },
    { "scatter", "bright", P_FLOAT, &s_bright, "1.0", 0.3f, 2, "sprite brightness x the baked terrain light" },
};
void scatter_params(void) { params_add(s_tab, E_COUNT(s_tab)); }

/* ---------------------------------------------------------------- data */
static int s_tried, s_ok;
static u8 *s_file;
static int s_nTypes, s_ncx, s_ncz, s_nInst;
static float s_cell, s_x0, s_z0, s_maxH;
static ScType s_type[SC_MAX_TYPES];
static const ScCell *s_cells;
static const ScInst *s_inst;
static RTex s_tex;
static RMaterial s_mat;
static int s_sec = -1;
static ScatterStats s_st;
static RVert1 s_v[SC_BATCH * 4];
static u16 s_idx[SC_BATCH * 6];
static float s_cosY[256], s_sinY[256];

static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static float rdf(const u8 *p) { u32 u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

static int load(void)
{
    char path[260];
    int size = 0, sz, i;
    ChunkFile cf;
    const u8 *p;
    data_path(path, sizeof(path), "island/veg.sct");
    if (!file_exists(path)) { log_printf(LOG_WARN, "scatter: %s missing: off", path); return 0; }
    s_file = (u8 *)file_load(path, &size);
    if (!s_file || !chunk_open(&cf, s_file, size, E_TAG('R', 'S', 'C', 'T'))) {
        log_printf(LOG_WARN, "scatter: %s unreadable: off", path); return 0;
    }
    p = chunk_find(&cf, "INFO", &sz);
    if (!p || sz < 40 || rd32(p) != 1) { log_printf(LOG_WARN, "scatter: bad INFO: off"); return 0; }
    s_nTypes = (int)rd32(p + 4); s_ncx = (int)rd32(p + 8); s_ncz = (int)rd32(p + 12);
    s_cell = rdf(p + 16); s_x0 = rdf(p + 20); s_z0 = rdf(p + 24); s_nInst = (int)rd32(p + 28); s_maxH = rdf(p + 32);
    if (s_nTypes <= 0 || s_nTypes > SC_MAX_TYPES || s_ncx <= 0 || s_ncz <= 0 || s_ncx > SC_MAX_CELLS ||
        s_ncz > SC_MAX_CELLS || s_cell < 100.0f) { log_printf(LOG_WARN, "scatter: bad INFO values: off"); return 0; }
    p = chunk_find(&cf, "TYPE", &sz);
    if (!p || sz != s_nTypes * 48) { log_printf(LOG_WARN, "scatter: bad TYPE: off"); return 0; }
    for (i = 0; i < s_nTypes; i++) {
        const u8 *t = p + i * 48 + 16;
        ScType *ty = &s_type[i];
        ty->u0 = rdf(t); ty->v0 = rdf(t + 4); ty->u1 = rdf(t + 8); ty->v1 = rdf(t + 12);
        ty->w = rdf(t + 16); ty->h = rdf(t + 20); ty->sink = rdf(t + 24); ty->kind = (int)rd32(t + 28);
    }
    p = chunk_find(&cf, "CELL", &sz);
    if (!p || sz != s_ncx * s_ncz * 16) { log_printf(LOG_WARN, "scatter: bad CELL: off"); return 0; }
    s_cells = (const ScCell *)p;                    /* 16 B records, 4-byte aligned in the chunk file */
    p = chunk_find(&cf, "INST", &sz);
    if (!p || sz != s_nInst * 16) { log_printf(LOG_WARN, "scatter: bad INST: off"); return 0; }
    s_inst = (const ScInst *)p;
    for (i = 0; i < s_ncx * s_ncz; i++)
        if (s_cells[i].first + s_cells[i].count > (u32)s_nInst) { log_printf(LOG_WARN, "scatter: bad cell: off"); return 0; }
    for (i = 0; i < s_nInst; i++)
        if (s_inst[i].type >= s_nTypes) { log_printf(LOG_WARN, "scatter: bad type: off"); return 0; }
    s_tex = tex_get("veg_atlas", IMG_KEYED, 0);
    if (!s_tex) { log_printf(LOG_WARN, "scatter: veg_atlas missing: off"); return 0; }
    memset(&s_mat, 0, sizeof(s_mat));
    s_mat.tex0 = s_tex; s_mat.blend = RB_ALPHATEST; s_mat.zWrite = 1; s_mat.zTest = 1; s_mat.fog = 1;
    s_mat.cullNone = 1; s_mat.lodBias = -2;
    for (i = 0; i < SC_BATCH; i++) {
        u16 b = (u16)(i * 4);
        s_idx[i * 6 + 0] = b; s_idx[i * 6 + 1] = (u16)(b + 1); s_idx[i * 6 + 2] = (u16)(b + 2);
        s_idx[i * 6 + 3] = b; s_idx[i * 6 + 4] = (u16)(b + 2); s_idx[i * 6 + 5] = (u16)(b + 3);
    }
    for (i = 0; i < 256; i++) {
        double a = (double)i * 6.283185307 / 256.0;
        s_cosY[i] = (float)cos(a); s_sinY[i] = (float)sin(a);
    }
    s_sec = prof_section("scatter");
    log_printf(LOG_INFO, "scatter: loaded %s types=%d cells=%dx%d (%.0f m) inst=%d bytes=%d", path, s_nTypes, s_ncx,
               s_ncz, (double)s_cell, s_nInst, size);
    return 1;
}

int scatter_loaded(void) { return s_ok; }
const ScatterStats *scatter_stats(void) { return &s_st; }

void scatter_free(void)
{
    if (s_tex) tex_release(s_tex);
    s_tex = 0;
    free(s_file);
    s_file = NULL;
    s_ok = 0; s_tried = 0;
}

/* ---------------------------------------------------------------- draw */
static int s_nq;

static void flush(void)
{
    if (!s_nq) return;
    r_draw_mem(RFMT_V1, s_v, s_nq * 4, s_idx, s_nq * 6);
    s_st.draws++;
    s_nq = 0;
}

void scatter_draw(const Camera *c)
{
    const ScenePreset *pr;
    int high, maxS, ci, n = 0, k;
    float range, farD, nearD, fadeD, range2, invFade, eyeX, eyeY, eyeZ;
    int cx0, cx1, cz0, cz1, cx, cz;
    int order[SC_MAX_CELLS * 4];
    float cdist[SC_MAX_CELLS * 4];
    Frustum fr;
    s64 t0 = plat_qpc();
    memset(&s_st, 0, sizeof(s_st));
    if (!s_on || !c) return;
    if (!s_tried) { s_tried = 1; s_ok = load(); }
    if (!s_ok) return;
    pr = scene_preset();
    high = (pr && pr->name[0] == 'h');
    range = s_range > 0.0f ? s_range : (high ? 3500.0f : 1600.0f);
    maxS = s_max > 0 ? s_max : (high ? 2400 : 600);
    if (maxS > SC_MAX_SPRITES) maxS = SC_MAX_SPRITES;
    farD = s_farDens > 0.0f ? s_farDens : (high ? 0.6f : 0.35f);
    nearD = range * s_nearFrac;
    fadeD = range * s_fadeFrac;
    invFade = 1.0f / (fadeD > 1.0f ? fadeD : 1.0f);
    range2 = range * range;
    eyeX = c->eye.x; eyeY = c->eye.y; eyeZ = c->eye.z;
    prof_begin(s_sec);
    cam_frustum(c, &fr);
    /* candidate cells: overlap the range circle, AABB in the frustum, not far below the eye */
    cx0 = (int)floor((eyeX - range - s_x0) / s_cell); cx1 = (int)floor((eyeX + range - s_x0) / s_cell);
    cz0 = (int)floor((eyeZ - range - s_z0) / s_cell); cz1 = (int)floor((eyeZ + range - s_z0) / s_cell);
    if (cx0 < 0) cx0 = 0;
    if (cz0 < 0) cz0 = 0;
    if (cx1 >= s_ncx) cx1 = s_ncx - 1;
    if (cz1 >= s_ncz) cz1 = s_ncz - 1;
    for (cz = cz0; cz <= cz1; cz++) {
        for (cx = cx0; cx <= cx1; cx++) {
            const ScCell *ce = &s_cells[cz * s_ncx + cx];
            float bx0 = s_x0 + cx * s_cell, bz0 = s_z0 + cz * s_cell, dx, dz, dy, d2;
            Aabb bb;
            if (!ce->count || n >= E_COUNT(order)) continue;
            dx = eyeX < bx0 ? bx0 - eyeX : (eyeX > bx0 + s_cell ? eyeX - bx0 - s_cell : 0.0f);
            dz = eyeZ < bz0 ? bz0 - eyeZ : (eyeZ > bz0 + s_cell ? eyeZ - bz0 - s_cell : 0.0f);
            dy = eyeY > ce->yMax + s_maxH ? eyeY - ce->yMax - s_maxH : (eyeY < ce->yMin - 10.0f ? ce->yMin - 10.0f - eyeY : 0.0f);
            d2 = dx * dx + dz * dz + dy * dy;
            if (d2 > range2) continue;
            bb.lo = v3(bx0, ce->yMin - 10.0f, bz0);
            bb.hi = v3(bx0 + s_cell, ce->yMax + s_maxH, bz0 + s_cell);
            if (!frustum_aabb(&fr, &bb)) continue;
            for (k = n; k > 0 && cdist[k - 1] > d2; k--) { cdist[k] = cdist[k - 1]; order[k] = order[k - 1]; }
            cdist[k] = d2; order[k] = cz * s_ncx + cx;
            n++;
        }
    }
    s_st.cellsVisited = n;
    if (!n) { prof_end(s_sec); s_st.ms = (float)((double)(plat_qpc() - t0) * 1000.0 / (double)plat_qpf()); return; }
    r_set_world(NULL);
    r_set_material(&s_mat);
    s_nq = 0;
    for (ci = 0; ci < n && s_st.sprites < maxS; ci++) {
        const ScCell *ce = &s_cells[order[ci]];
        u32 first = ce->first, e = ce->first + ce->count, gi;
        for (gi = first; gi < e && s_st.sprites < maxS; gi++) {
            const ScInst *in = &s_inst[gi];
            const ScType *ty = &s_type[in->type];
            float dx = in->x - eyeX, dy = in->y - eyeY, dz = in->z - eyeZ, d2 = dx * dx + dy * dy + dz * dz, d, dens, sz;
            float hs, w2, h, sink, rx, rz, ux, uy, uz, bx, by, bz;
            u32 col, sh;
            RVert1 *v;
            s_st.instTested++;
            if (d2 > range2) continue;
            hs = ty->h * (float)in->scale * 0.01f;
            if (!frustum_sphere(&fr, v3(in->x, in->y + 0.5f * hs, in->z), 0.6f * (hs > ty->w ? hs : ty->w * (float)in->scale * 0.01f))) continue;
            /* density LOD: hash of the instance against the density at this distance; shrink near the threshold */
            sz = 1.0f;
            if (d2 > nearD * nearD) {
                float hfrac = (float)(((gi * 2654435761u) >> 16) & 0xFFFF) * (1.0f / 65536.0f);
                d = (float)sqrt(d2);
                dens = 1.0f - (1.0f - farD) * (d - nearD) / (range - nearD);
                if (hfrac > dens) continue;
                if (dens - hfrac < 0.06f) sz = (dens - hfrac) * (1.0f / 0.06f);
                if (d > range - fadeD) sz *= (range - d) * invFade;
            }
            if (sz <= 0.02f) continue;
            sz *= (float)in->scale * 0.01f;
            w2 = 0.5f * ty->w * sz; h = ty->h * sz; sink = ty->sink * sz;
            if (ty->kind == 1) {                    /* fixed card along the yaw, upright */
                rx = s_cosY[in->yaw] * w2; rz = -s_sinY[in->yaw] * w2;
                ux = 0.0f; uy = 1.0f; uz = 0.0f;
            } else {                                /* camera-facing about the vertical, leaning back */
                float hx = -dx, hz = -dz, hl = (float)sqrt(hx * hx + hz * hz), el, th, ct, st;
                if (hl < 0.01f) { hx = 0.0f; hz = -1.0f; hl = 1.0f; }
                hx /= hl; hz /= hl;
                rx = -hz * w2; rz = hx * w2;
                el = -dy / hl;                      /* tan(elevation); atan, sin, cos by cheap polynomials */
                if (el < 0.0f) el = 0.0f;
                el = el / (1.0f + 0.28f * el * el); /* atan(x), good to ~0.005 rad */
                th = el * s_tiltK;
                if (th > 0.61f) th = 0.61f;
                st = th - th * th * th * (1.0f / 6.0f);
                ct = 1.0f - 0.5f * th * th + th * th * th * th * (1.0f / 24.0f);
                ux = -hx * st; uy = ct; uz = -hz * st;
            }
            bx = in->x - ux * sink; by = in->y - uy * sink; bz = in->z - uz * sink;
            sh = (u32)e_ftoi((float)in->shade * (255.0f / 200.0f) * s_bright);
            if (sh > 255) sh = 255;
            col = 0xFF000000u | (sh << 16) | (sh << 8) | sh;
            v = &s_v[s_nq * 4];
            v[0].x = bx - rx; v[0].y = by; v[0].z = bz - rz; v[0].u = ty->u0; v[0].v = ty->v1;
            v[1].x = bx - rx + ux * h; v[1].y = by + uy * h; v[1].z = bz - rz + uz * h; v[1].u = ty->u0; v[1].v = ty->v0;
            v[2].x = bx + rx + ux * h; v[2].y = by + uy * h; v[2].z = bz + rz + uz * h; v[2].u = ty->u1; v[2].v = ty->v0;
            v[3].x = bx + rx; v[3].y = by; v[3].z = bz + rz; v[3].u = ty->u1; v[3].v = ty->v1;
            v[0].color = v[1].color = v[2].color = v[3].color = col;
            s_st.sprites++;
            if (++s_nq == SC_BATCH) flush();
        }
    }
    flush();
    prof_count(s_sec, s_st.sprites);
    prof_end(s_sec);
    s_st.ms = (float)((double)(plat_qpc() - t0) * 1000.0 / (double)plat_qpf());
    if (c->fixed) {                                 /* -camat (screenshots / cost probes): log the average */
        static int s_n, s_spr;
        static float s_msSum;
        s_n++; s_spr += s_st.sprites; s_msSum += s_st.ms;
        if (s_n == 150) {
            log_printf(LOG_INFO, "scatter: camat frames=150 avg_ms=%.3f sprites=%d cells=%d draws=%d",
                       (double)(s_msSum / 150.0f), s_spr / 150, s_st.cellsVisited, s_st.draws);
            s_n = s_spr = 0; s_msSum = 0.0f;
        }
    }
}
