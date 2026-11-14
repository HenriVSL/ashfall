/* hud_map.cpp - see hud_map.h. Windows/render side. Owner: T07. */
#include "hud_map.h"
#include "hud_font.h"
#include "params.h"
#include "e_log.h"
#include "e_tex.h"
#include "e_ui.h"
#include "e_plat.h"
#include "e_image.h"
#include "e_terrain.h"

enum { MAP_MAXC = 32, MAP_MAXRWR = 8 };

static int   s_rangeNm, s_showAll, s_auto = 1, s_rangeCur;
static float s_mapX, s_mapY, s_mapS, s_bgA, s_wedgeA, s_islandA;
static float s_imgX0, s_imgZ0, s_imgX1, s_imgZ1, s_areaX, s_areaZ, s_areaR;
static float s_rwyX, s_rwyZ, s_rwyHdg, s_rwyLen;
static RTex  s_island;
static MapContact s_q[MAP_MAXC];
static int   s_nq;
static float s_rwr[MAP_MAXRWR];
static int   s_nrwr;
static float s_ms;
static int   s_sprJet = -1, s_sprFrame = -1, s_sprArch = -1, s_sprArchD = -1, s_sprChev = -1, s_sprChevD = -1, s_sprDot = -1;

static const Param s_tab[] = {
    { "hud", "mapRange", P_INT, &s_rangeNm, "40", 10, 40, "minimap range at start (NM = half the square): 10, 20 or 40" },
    { "hud", "mapShowAll", P_BOOL, &s_showAll, "1", 0, 1, "minimap shows all enemies (1) or only radar contacts + memory (0)" },
    { "hud", "mapX", P_FLOAT, &s_mapX, "6", 0, 640, "minimap left edge (virtual 640x480 units)" },
    { "hud", "mapY", P_FLOAT, &s_mapY, "338", 0, 480, "minimap top edge (virtual units)" },
    { "hud", "mapSize", P_FLOAT, &s_mapS, "136", 60, 240, "minimap square size (virtual units)" },
    { "hud", "mapBgAlpha", P_FLOAT, &s_bgA, "0.38", 0, 1, "minimap background darkening (0x60 = 0.38)" },
    { "hud", "mapWedgeAlpha", P_FLOAT, &s_wedgeA, "0.15", 0, 1, "radar scan wedge alpha" },
    { "hud", "mapIslandAlpha", P_FLOAT, &s_islandA, "0.85", 0, 1, "island image alpha on the minimap" },
    { "hud", "mapImgX0", P_FLOAT, &s_imgX0, "-18512", 0, 0, "island image world rect (islandc MAPI chunk): x0" },
    { "hud", "mapImgZ0", P_FLOAT, &s_imgZ0, "-17912", 0, 0, "island image world rect: z0" },
    { "hud", "mapImgX1", P_FLOAT, &s_imgX1, "20912", 0, 0, "island image world rect: x1" },
    { "hud", "mapImgZ1", P_FLOAT, &s_imgZ1, "21512", 0, 0, "island image world rect: z1 (top of the image = north)" },
    { "hud", "mapAreaX", P_FLOAT, &s_areaX, "1200", 0, 0, "area boundary circle centre x (island centre, m)" },
    { "hud", "mapAreaZ", P_FLOAT, &s_areaZ, "1800", 0, 0, "area boundary circle centre z (m)" },
    { "hud", "mapAreaR", P_FLOAT, &s_areaR, "75000", 0, 100000, "area boundary radius (m) = [world] areaWarnKm (RETURN TO AREA)" },
};
void map_params(void) { params_add(s_tab, E_COUNT(s_tab)); }

int map_init(void)
{
    s_island = tex_get("map_island", IMG_ALPHA, RT_CLAMP | RT_NOMIP);
    s_sprJet = hg_sprite_id("map_jet");
    s_sprFrame = hg_sprite_id("map_frame");
    s_sprArch = hg_sprite_id("map_arch");
    s_sprArchD = hg_sprite_id("map_arch_dash");
    s_sprChev = hg_sprite_id("map_chev");
    s_sprChevD = hg_sprite_id("map_chev_dash");
    s_sprDot = hg_sprite_id("dot");
    s_rangeCur = s_rangeNm <= 10 ? 10 : (s_rangeNm <= 20 ? 20 : 40);
    s_rwyX = -2250.0f; s_rwyZ = -3980.0f; s_rwyHdg = 270.0f; s_rwyLen = 2800.0f;   /* T03 BASE defaults */
    log_printf(LOG_INFO, "hud: map init island=%d range=%d showAll=%d", s_island, s_rangeCur, s_showAll);
    return 1;
}

void map_free(void)
{
    if (s_island) tex_release(s_island);
    s_island = 0;
}

void  map_set_auto_contacts(int on) { s_auto = on ? 1 : 0; }
int   map_show_all(void) { return s_showAll; }
int   map_range_nm(void) { return s_rangeCur ? s_rangeCur : 20; }
void  map_set_range_nm(int nm) { s_rangeCur = nm <= 10 ? 10 : (nm <= 20 ? 20 : 40); }
int   map_cycle_range(void) { map_set_range_nm(s_rangeCur == 10 ? 20 : (s_rangeCur == 20 ? 40 : 10)); return s_rangeCur; }
float map_ms_last(void) { return s_ms; }

void map_draw_contact(const MapContact *c)
{
    if (c && s_nq < MAP_MAXC) s_q[s_nq++] = *c;
}

void map_draw_rwr(float bearingRad)
{
    if (s_nrwr < MAP_MAXRWR) s_rwr[s_nrwr++] = bearingRad;
}

/* ---------------------------------------------------------------- geometry */
typedef struct MapGeo { float x0, y0, x1, y1, cx, cy, half, scale, sh, ch; } MapGeo;   /* px; scale px per m */
static MapGeo s_g;

static void geo(const HudState *hs)
{
    float h = E_DEG2RAD(hs->headingDeg);
    s_g.x0 = (float)hg_px(s_mapX); s_g.y0 = (float)hg_px(s_mapY);
    s_g.x1 = (float)hg_px(s_mapX + s_mapS); s_g.y1 = (float)hg_px(s_mapY + s_mapS);
    s_g.cx = (float)e_ftoi((s_g.x0 + s_g.x1) * 0.5f);
    s_g.cy = (float)e_ftoi((s_g.y0 + s_g.y1) * 0.5f);
    s_g.half = (s_g.x1 - s_g.x0) * 0.5f;
    s_g.scale = s_g.half / ((float)map_range_nm() * NM2M);
    s_g.sh = (float)sin(h); s_g.ch = (float)cos(h);
}

/* world (x, z) -> map px, heading up */
static void w2m(const HudState *hs, float x, float z, float *sx, float *sy)
{
    float rx = x - hs->pos.x, rz = z - hs->pos.z;
    float right = rx * s_g.ch - rz * s_g.sh;        /* right = (cos h, -sin h) */
    float fwd = rx * s_g.sh + rz * s_g.ch;          /* forward = (sin h, cos h) */
    *sx = s_g.cx + right * s_g.scale;
    *sy = s_g.cy - fwd * s_g.scale;
}

int map_to_screen(const HudState *hs, float x, float z, float *sx, float *sy)
{
    geo(hs);
    w2m(hs, x, z, sx, sy);
    return *sx >= s_g.x0 && *sx < s_g.x1 && *sy >= s_g.y0 && *sy < s_g.y1;
}

/* Sutherland-Hodgman against one edge of the map rect (axis a: 0 = x, 1 = y; keep v >= lim if ge) */
typedef struct PV { float x, y, u, v; } PV;
static int clip_edge(const PV *in, int n, PV *out, int axis, float lim, int ge)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const PV *a = &in[i], *b = &in[(i + 1) % n];
        float va = axis ? a->y : a->x, vb = axis ? b->y : b->x;
        int ia = ge ? va >= lim : va <= lim, ib = ge ? vb >= lim : vb <= lim;
        if (ia) out[m++] = *a;
        if (ia != ib) {
            float t = (lim - va) / (vb - va);
            out[m].x = a->x + (b->x - a->x) * t; out[m].y = a->y + (b->y - a->y) * t;
            out[m].u = a->u + (b->u - a->u) * t; out[m].v = a->v + (b->v - a->v) * t;
            m++;
        }
    }
    return m;
}

static void draw_island(const HudState *hs)
{
    PV a[12], b[12];
    int n = 4, i;
    u32 col;
    if (!s_island) return;
    w2m(hs, s_imgX0, s_imgZ1, &a[0].x, &a[0].y); a[0].u = 0.0f; a[0].v = 0.0f;
    w2m(hs, s_imgX1, s_imgZ1, &a[1].x, &a[1].y); a[1].u = 1.0f; a[1].v = 0.0f;
    w2m(hs, s_imgX1, s_imgZ0, &a[2].x, &a[2].y); a[2].u = 1.0f; a[2].v = 1.0f;
    w2m(hs, s_imgX0, s_imgZ0, &a[3].x, &a[3].y); a[3].u = 0.0f; a[3].v = 1.0f;
    n = clip_edge(a, n, b, 0, s_g.x0 + 1.0f, 1); if (n < 3) return;
    n = clip_edge(b, n, a, 0, s_g.x1 - 1.0f, 0); if (n < 3) return;
    n = clip_edge(a, n, b, 1, s_g.y0 + 1.0f, 1); if (n < 3) return;
    n = clip_edge(b, n, a, 1, s_g.y1 - 1.0f, 0); if (n < 3) return;
    col = ((u32)e_ftoi(s_islandA * 255.0f) << 24) | 0xFFFFFFu;
    hg_flush();
    ui_set_blend(RB_BLEND);                         /* the island image is straight alpha */
    for (i = 1; i + 1 < n; i += 2) {                /* convex fan as quads (last one degenerate if needed) */
        RVert2D q[4];
        const PV *p[4];
        int k;
        p[0] = &a[0]; p[1] = &a[i]; p[2] = &a[i + 1]; p[3] = i + 2 < n ? &a[i + 2] : &a[i + 1];
        for (k = 0; k < 4; k++) { q[k].x = p[k]->x; q[k].y = p[k]->y; q[k].u = p[k]->u; q[k].v = p[k]->v; q[k].color = col; }
        ui_quad4_px(s_island, q);
    }
    ui_set_blend(hg_ready() ? RB_PREMUL : RB_BLEND);
}

static void draw_contact(const HudState *hs, const MapContact *c, u32 green, float t)
{
    float sx, sy, rel, k = hg_k();
    int spr;
    u32 col;
    w2m(hs, c->x, c->z, &sx, &sy);
    if (sx < s_g.x0 + 3.0f || sx > s_g.x1 - 3.0f || sy < s_g.y0 + 3.0f || sy > s_g.y1 - 3.0f) return;
    if (c->kind == MAPC_KILL) {                     /* T15: killed enemy = grey-red X (two line_sh strokes) */
        float r = 3.5f * k;
        col = hg_col(0xFFE07070, 0.95f);       /* grey-red: readable over the island image */
        hg_line_w(sx - r, sy - r, sx + r, sy + r, 0.85f, col);
        hg_line_w(sx - r, sy + r, sx + r, sy - r, 0.85f, col);
        return;
    }
    if (c->kind == MAPC_MSL_OWN || c->kind == MAPC_MSL_HOSTILE) {
        if (c->kind == MAPC_MSL_HOSTILE && fmod(t * 4.0f, 1.0f) >= 0.5f) return;   /* 4 Hz blink */
        col = c->kind == MAPC_MSL_OWN ? green : hg_col(0xFFFF3030, 1.0f);
        hg_sprite(s_sprDot, sx, sy, col);
        return;
    }
    col = c->kind == MAPC_HOSTILE ? 0xFFFF3030 : 0xFFFFE040;
    col = hg_col(col, (c->flags & MAPF_MEMORY) ? 0.5f : ((c->flags & MAPF_NORADAR) ? 0.4f : 1.0f));   /* NORADAR: T15 */
    if (c->kind == MAPC_HOSTILE) spr = (c->flags & MAPF_MEMORY) ? s_sprChevD : s_sprChev;
    else spr = (c->flags & MAPF_MEMORY) ? s_sprArchD : s_sprArch;
    hg_sprite(spr, sx, sy, col);
    rel = c->headingRad - E_DEG2RAD(hs->headingDeg);
    hg_line_w(sx, sy, sx + (float)sin(rel) * 6.0f * k, sy - (float)cos(rel) * 6.0f * k, 0.75f, col);
    if (c->flags & MAPF_DESIGNATED) {
        int r = e_ftoi(6.0f * k), x = e_ftoi(sx), y = e_ftoi(sy) - e_ftoi(2.0f * k);
        hg_box(x - r, y - r, x + r, y + r, green);
    }
}

static void draw_rwr(float bearing, const HudState *hs)
{
    float rel = bearing - E_DEG2RAD(hs->headingDeg), dx = (float)sin(rel), dy = -(float)cos(rel), t, m, ex, ey;
    float k = hg_k();
    m = E_MAXF((float)fabs(dx), (float)fabs(dy));
    if (m < 1e-4f) return;
    t = (s_g.half - 1.0f) / m;
    ex = s_g.cx + dx * t; ey = s_g.cy + dy * t;
    hg_line(ex, ey, ex - dx * 7.0f * k / m * 0.7f, ey - dy * 7.0f * k / m * 0.7f, hg_col(0xFFFF3030, 1.0f));
}

void map_draw_state(const HudState *hs, const Game *g)
{
    s64 t0 = plat_qpc();
    float k = hg_k(), wedgeY;
    u32 green = hg_col(0xFF40FF60, 1.0f), dim = hg_col(0xFF40FF60, 0.5f), wedge = hg_col(0xFF40FF60, s_wedgeA);
    char buf[16];
    int i, i0 = e_ftoi(s_g.x0);
    (void)i0;
    geo(hs);
    if (g && g->terrain) {
        const TerrainBase *b = terrain_base(g->terrain);
        if (b && b->rwyLength > 100.0f) { s_rwyX = b->rwyX; s_rwyZ = b->rwyZ; s_rwyHdg = b->rwyHeadingDeg; s_rwyLen = b->rwyLength; }
    }
    if (g && s_auto && s_showAll) {
        for (i = 1; i < g->numAircraft; i++) {
            const Aircraft *a = &g->ac[i];
            MapContact c;
            if (!(a->flags & AC_ALIVE)) continue;
            c.x = a->fs.pos.x; c.z = a->fs.pos.z; c.headingRad = a->fs.heading; c.kind = MAPC_HOSTILE; c.flags = 0;
            map_draw_contact(&c);
        }
    }
    hg_begin();
    /* background + scan wedge (+-60 deg from the nose, clipped to the square) */
    hg_rect(s_g.x0, s_g.y0, s_g.x1, s_g.y1, hg_col(0xFF000000, s_bgA));
    wedgeY = s_g.cy - s_g.half * 0.57735f;
    hg_tri(s_g.cx, s_g.cy, s_g.x0, wedgeY, s_g.x0, s_g.y0, wedge);
    hg_tri(s_g.cx, s_g.cy, s_g.x0, s_g.y0, s_g.x1, s_g.y0, wedge);
    hg_tri(s_g.cx, s_g.cy, s_g.x1, s_g.y0, s_g.x1, wedgeY, wedge);
    draw_island(hs);
    hg_clip(e_ftoi(s_g.x0) + 1, e_ftoi(s_g.y0) + 1, e_ftoi(s_g.x1) - 1, e_ftoi(s_g.y1) - 1);
    /* runway mark */
    {
        float h = E_DEG2RAD(s_rwyHdg), dx = (float)sin(h) * s_rwyLen * 0.5f, dz = (float)cos(h) * s_rwyLen * 0.5f;
        float ax, ay, bx, by;
        w2m(hs, s_rwyX - dx, s_rwyZ - dz, &ax, &ay);
        w2m(hs, s_rwyX + dx, s_rwyZ + dz, &bx, &by);
        hg_line_w(ax, ay, bx, by, 0.75f, hg_col(0xFFFFFFFF, 0.9f));
    }
    /* area boundary circle when any part is in view */
    {
        float cx, cy, R = s_areaR * s_g.scale, d;
        w2m(hs, s_areaX, s_areaZ, &cx, &cy);
        d = (float)sqrt((cx - s_g.cx) * (cx - s_g.cx) + (cy - s_g.cy) * (cy - s_g.cy));
        if (d - R < s_g.half * 1.42f && d + s_g.half * 1.42f > R) {
            float px = cx + R, py = cy;
            for (i = 1; i <= 24; i++) {             /* 24 chords: < 1 px error at 40 NM range */
                float a = (float)i * (2.0f * E_PI / 24.0f), nx = cx + (float)cos(a) * R, ny = cy + (float)sin(a) * R;
                hg_line_w(px, py, nx, ny, 0.75f, dim);
                px = nx; py = ny;
            }
        }
    }
    for (i = 0; i < s_nq; i++) draw_contact(hs, &s_q[i], green, hs->time);
    s_nq = 0;
    hg_clip(0, 0, 0, 0);
    for (i = 0; i < s_nrwr; i++) draw_rwr(s_rwr[i], hs);
    s_nrwr = 0;
    hg_sprite(s_sprJet, s_g.cx, s_g.cy, green);
    hg_nine(s_sprFrame, e_ftoi(s_g.x0), e_ftoi(s_g.y0), e_ftoi(s_g.x1), e_ftoi(s_g.y1), green);
    e_snprintf(buf, (int)sizeof(buf), "%d", map_range_nm());
    hg_text(e_ftoi(s_g.x0 + 5.0f * k), e_ftoi(s_g.y0 + 4.0f * k), HF_SMALL, green, 0, buf);
    if (hs->flares >= 0) {
        e_snprintf(buf, (int)sizeof(buf), "F%d", hs->flares);
        hg_text(e_ftoi(s_g.x1 - 5.0f * k), e_ftoi(s_g.y0 + 4.0f * k), HF_SMALL, green, 2, buf);
    }
    hg_end();
    s_ms = (float)((double)(plat_qpc() - t0) * 1000.0 / (double)plat_qpf());
}

void map_draw(const Game *g, float alpha)
{
    HudState hs;
    hud_state_fill(&hs, g, NULL, alpha);
    map_draw_state(&hs, g);
}
