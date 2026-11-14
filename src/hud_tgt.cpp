/* hud_tgt.cpp - target symbology + minimap contacts + -hudtest contacts. See hud_tgt.h. Windows/render side.
   Owner: T15. Layout values are virtual 640x480 units scaled by k = screen h / 480 (hud_font.h). */
#include <stdarg.h>

#include "hud_tgt.h"
#include "hud.h"
#include "hud_font.h"
#include "hud_map.h"
#include "radar.h"
#include "target.h"
#include "weapons.h"
#include "flight.h"
#include "params.h"
#include "e_log.h"
#include "e_plat.h"
#include "e_ui.h"
#include "e_file.h"

#define COL_GREEN   0xFF40FF60u
#define COL_YELLOW  0xFFFFE040u
#define COL_RED     0xFFFF3030u
#define COL_GREY    0xFFB4B4B4u

/* ================================================================ params ([hud] tgt*) */
static float s_hideM = 400.0f, s_arrowNm = 40.0f, s_arrowInset = 20.0f, s_locLen = 60.0f, s_splashSec = 1.5f;
static float s_fadeSec = 0.8f, s_overlapPx = 12.0f, s_lblHz = 5.0f, s_memAlpha = 0.5f, s_killDiaSec = 3.0f, s_killMapSec = 15.0f;
static int   s_arrowMax = 6, s_lblOthers = 2, s_mapNoRadar = 1, s_mapMissiles = 1, s_mapRwr = 1;

static const Param s_tab[] = {
    { "hud", "tgtHideM", P_FLOAT, &s_hideM, "400", 0, 3000, "target symbols hidden within this range (m), except the TD box" },
    { "hud", "tgtArrowNm", P_FLOAT, &s_arrowNm, "40", 0, 60, "off-screen arrows for contacts within this range (NM)" },
    { "hud", "tgtArrowMax", P_INT, &s_arrowMax, "6", 0, 16, "max off-screen arrows (nearest first)" },
    { "hud", "tgtArrowInset", P_FLOAT, &s_arrowInset, "20", 8, 60, "off-screen arrows this far inside the screen edge (virtual units)" },
    { "hud", "tgtLocatorLen", P_FLOAT, &s_locLen, "60", 20, 120, "TD locator line length from the waterline / aim cross (virtual units)" },
    { "hud", "tgtSplashSec", P_FLOAT, &s_splashSec, "1.5", 0, 5, "SPLASH display time after a kill (s; radar lists dead contacts for [radar] splashSec)" },
    { "hud", "tgtFadeSec", P_FLOAT, &s_fadeSec, "0.8", 0.05f, 5, "a killed contact's grey diamond fades out in this time after killDiamondSec (s)" },
    { "hud", "killDiamondSec", P_FLOAT, &s_killDiaSec, "3", 0, 20, "a killed enemy's diamond stays grey at the kill position this long, then fades (s)" },
    { "hud", "killMapSec", P_FLOAT, &s_killMapSec, "15", 0, 120, "a killed enemy stays crossed out (X) on the minimap this long (s)" },
    { "hud", "tgtOverlapPx", P_FLOAT, &s_overlapPx, "12", 4, 40, "symbol centres closer than this: the later label moves right (virtual units)" },
    { "hud", "tgtLabelHz", P_FLOAT, &s_lblHz, "5", 1, 10, "range/altitude/off-nose label update rate (Hz)" },
    { "hud", "tgtLabelOthers", P_INT, &s_lblOthers, "2", 0, 4, "labels for this many nearest contacts besides the designated and selected" },
    { "hud", "tgtMemAlpha", P_FLOAT, &s_memAlpha, "0.5", 0.1f, 1, "alpha of contacts in MEMORY (dashed)" },
    { "hud", "mapNoRadar", P_BOOL, &s_mapNoRadar, "1", 0, 1, "with mapShowAll: enemies the radar has not detected drawn dim on the minimap" },
    { "hud", "mapMissiles", P_BOOL, &s_mapMissiles, "1", 0, 1, "live missiles on the minimap (own green, hostile red blinking)" },
    { "hud", "mapRwr", P_BOOL, &s_mapRwr, "1", 0, 1, "RWR ticks on the minimap frame for enemies locking the player" },
};
void hudtgt_params(void) { params_add(s_tab, E_COUNT(s_tab)); }

/* ================================================================ sprites */
static int s_sprSet = -1;
static int s_sprDia = -1, s_sprDiaD = -1, s_sprBox = -1, s_sprBoxD = -1, s_sprTri = -1, s_sprOff = -1, s_sprLoc = -1;
static int s_diaHalf = 6, s_boxHalf = 12;
static float s_ms;
static const float *s_brightP;                      /* [hud] bright (hud.cpp) */
static const float *s_mapP[3];                      /* [hud] mapX mapY mapSize (hud_map.cpp) */

static void sprites_init(void)
{
    int oy;
    const Param *p;
    if (s_sprSet == hg_set_lines() && s_sprSet != -1) return;
    s_sprSet = hg_set_lines();
    s_sprDia = hg_sprite_id("tgt_diamond");
    s_sprDiaD = hg_sprite_id("tgt_diamond_dash");
    s_sprBox = hg_sprite_id("tgt_box");
    s_sprBoxD = hg_sprite_id("tgt_box_dash");
    s_sprTri = hg_sprite_id("sel_tri");
    s_sprOff = hg_sprite_id("offscreen_tri");
    s_sprLoc = hg_sprite_id("loc_arrow");
    s_diaHalf = hg_sprite_info(s_sprDia, NULL, NULL, NULL, &oy) ? oy : hg_px(6.0f);
    s_boxHalf = hg_sprite_info(s_sprBox, NULL, NULL, NULL, &oy) ? oy : hg_px(11.0f);
    p = params_find("hud", "bright");   s_brightP = p ? (const float *)p->ptr : NULL;
    p = params_find("hud", "mapX");     s_mapP[0] = p ? (const float *)p->ptr : NULL;
    p = params_find("hud", "mapY");     s_mapP[1] = p ? (const float *)p->ptr : NULL;
    p = params_find("hud", "mapSize");  s_mapP[2] = p ? (const float *)p->ptr : NULL;
    log_printf(LOG_INFO, "hud: tgt init set=%d dia=%d box=%d tri=%d off=%d loc=%d diaHalf=%d boxHalf=%d",
               s_sprSet, s_sprDia, s_sprBox, s_sprTri, s_sprOff, s_sprLoc, s_diaHalf, s_boxHalf);
}

/* ================================================================ fixed-rate numbers (10.7.3) */
typedef struct TNum { float next; int init, mode, shown, upd, lastDir, lastUpd, havePend, pendDir, changes; } TNum;

static int qround(float q) { return (int)floor(q + 0.5f); }

static int tnum_due(TNum *n, float t, float period)
{
    if (n->init && t + 1e-5f < n->next) return 0;
    if (!n->init) n->next = t;
    n->next += period;
    if (n->next <= t) n->next = t + period;
    n->upd++;
    return 1;
}

/* q = raw value in quanta of 'mode'. 1 = shown changed. Dead band 0.5 + 0.2 quanta; continuing the last change's
   direction at the next update applies at once, a first move or a reversal needs 2 updates in a row (= T07). */
static int tnum_apply(TNum *n, float q, int mode)
{
    int cand, dir;
    if (!n->init || mode != n->mode) {
        n->init = 1; n->mode = mode; n->shown = qround(q); n->lastDir = 0; n->havePend = 0; n->lastUpd = -10;
        n->changes++;
        return 1;
    }
    if ((float)fabs(q - (float)n->shown) <= 0.7f) { n->havePend = 0; return 0; }
    cand = qround(q);
    if (cand == n->shown) { n->havePend = 0; return 0; }
    dir = cand > n->shown ? 1 : -1;
    if ((dir == n->lastDir && n->lastUpd == n->upd - 1) || (n->havePend && n->pendDir == dir)) {
        n->shown = cand; n->lastDir = dir; n->lastUpd = n->upd; n->havePend = 0;
        n->changes++;
        return 1;
    }
    n->havePend = 1; n->pendDir = dir;
    return 0;
}

/* ================================================================ per-contact cache */
enum { TGT_IDS = 16 };
enum { K_NONE = 0, K_DIAMOND, K_ARROW };
typedef struct TCache {
    int used, frame;
    TNum rng, alt, off;
    char rngStr[8], altStr[8], offStr[8], lbl[20];
    int lblDesig;                                   /* label built with the altitude part */
    int onScreen, inFrame, isNear, inArrow, right;    /* hysteresis flags */
    float dirX, dirY;                               /* last screen direction (dead astern fallback) */
} TCache;
static TCache s_c[TGT_IDS];
static int    s_frame;
static int    s_hold[4] = { -1, -1, -1, -1 };       /* label holders besides designated/selected (declutter) */
static float  s_holdT[4];
typedef struct KillRec { int active, show3d; float t0; Vec3 pos; } KillRec;   /* per aircraft id (own timers) */
static KillRec s_kill[TGT_IDS];

void hudtgt_reset(void)
{
    int i;
    memset(s_c, 0, sizeof(s_c));
    for (i = 0; i < 4; i++) { s_hold[i] = -1; s_holdT[i] = 0.0f; }
    memset(s_kill, 0, sizeof(s_kill));
}

float hudtgt_ms_last(void) { return s_ms; }


/* ---- debug record for -hudtest contacts */
typedef struct TgtDbg {
    int id, kind, col, dashed, tri, td, label, right, splash;
    int cx, cy, boxX, boxY, triX, triY;
    float fx, fy;
    char lbl[20], rng[8];
} TgtDbg;
static TgtDbg s_dbg[TGT_MAX];
static int    s_ndbg, s_dbgLabels, s_dbgLoc, s_dbgArrows, s_dbgQuads, s_dbgMapKills;
static char   s_dbgLocStr[8];
static float  s_dbgLocX, s_dbgLocY;
static int    s_tdHave, s_tdX, s_tdY, s_tdClamped;    /* hudtgt_td (T16) */
static int    s_tdLblHave, s_tdLbl[4];              /* hudtgt_td_label (T18) */
int hudtgt_td(int *x, int *y, int *clamped, int *halfPx)
{
    if (x) *x = s_tdX;
    if (y) *y = s_tdY;
    if (clamped) *clamped = s_tdClamped;
    if (halfPx) *halfPx = s_boxHalf;
    return s_tdHave;
}

/* ================================================================ geometry */
typedef struct FrameSym {
    const TgtSym *s;
    TCache *c;
    int front;                                      /* in front of the camera (projection valid) */
    float sx, sy;                                   /* projected position (px) */
    float dx, dy;                                   /* unit screen direction from the screen centre */
    float range;                                    /* m from the own aircraft (render state) */
    int kind, cx, cy, label, td, dbg;
} FrameSym;

static int s_W = 640, s_H = 480;

static float ray_exit(float ox, float oy, float dx, float dy, float x0, float y0, float x1, float y1)
{
    float t = 1e9f;
    if (dx > 1e-6f) t = E_MINF(t, (x1 - ox) / dx); else if (dx < -1e-6f) t = E_MINF(t, (x0 - ox) / dx);
    if (dy > 1e-6f) t = E_MINF(t, (y1 - oy) / dy); else if (dy < -1e-6f) t = E_MINF(t, (y0 - oy) / dy);
    return t < 0.0f ? 0.0f : t;
}

static int ray_enter(float ox, float oy, float dx, float dy, const float *b, float *tEnter)
{
    float tmin = -1e9f, tmax = 1e9f, t0, t1;
    if ((float)fabs(dx) < 1e-6f) { if (ox < b[0] || ox > b[2]) return 0; }
    else { t0 = (b[0] - ox) / dx; t1 = (b[2] - ox) / dx; if (t0 > t1) { float s = t0; t0 = t1; t1 = s; } tmin = E_MAXF(tmin, t0); tmax = E_MINF(tmax, t1); }
    if ((float)fabs(dy) < 1e-6f) { if (oy < b[1] || oy > b[3]) return 0; }
    else { t0 = (b[1] - oy) / dy; t1 = (b[3] - oy) / dy; if (t0 > t1) { float s = t0; t0 = t1; t1 = s; } tmin = E_MAXF(tmin, t0); tmax = E_MINF(tmax, t1); }
    if (tmax < tmin || tmax <= 0.0f) return 0;
    *tEnter = E_MAXF(tmin, 0.0f);
    return 1;
}

/* minimap square in px, expanded by e */
static void map_box(float e, float *b)
{
    float k = (float)s_H / 480.0f;
    float x = s_mapP[0] ? *s_mapP[0] : 6.0f, y = s_mapP[1] ? *s_mapP[1] : 338.0f, sz = s_mapP[2] ? *s_mapP[2] : 136.0f;
    b[0] = x * k - e; b[1] = y * k - e; b[2] = (x + sz) * k + e; b[3] = (y + sz) * k + e;
}

static int in_box(float x, float y, const float *b) { return x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3]; }

/* point on the screen edge (inset) along a direction from the screen centre, stopping at the minimap */
static void edge_point(float dx, float dy, float inset, float mapPad, float *px, float *py)
{
    float ox = (float)s_W * 0.5f, oy = (float)s_H * 0.5f, t, te, mb[4];
    t = ray_exit(ox, oy, dx, dy, inset, inset, (float)s_W - inset, (float)s_H - inset);
    map_box(mapPad, mb);
    if (ray_enter(ox, oy, dx, dy, mb, &te) && te < t) t = te;
    *px = ox + dx * t; *py = oy + dy * t;
}

static int flash_on(float t, float hz) { return (float)fmod((double)t * hz, 1.0) < 0.5f; }

/* ================================================================ labels */
static void rng_text(TCache *c, float t, float rangeM)
{
    float nm = rangeM / NM2M;
    int mode, ch;
    if (!tnum_due(&c->rng, t, 1.0f / s_lblHz)) return;
    if (!c->rng.init) mode = nm < 1.0f ? 0 : (nm < 10.0f ? 1 : 2);
    else {
        mode = c->rng.mode;
        if (mode == 0 && nm > 1.05f) mode = 1;
        else if (mode == 1 && nm < 0.95f) mode = 0;
        else if (mode == 1 && nm > 10.3f) mode = 2;
        else if (mode == 2 && nm < 9.7f) mode = 1;
    }
    ch = tnum_apply(&c->rng, mode == 0 ? rangeM * M2FT / 100.0f : (mode == 1 ? nm * 10.0f : nm), mode);
    if (!ch && c->rngStr[0]) return;
    if (c->rng.shown < 0) c->rng.shown = 0;
    if (mode == 1) e_snprintf(c->rngStr, (int)sizeof(c->rngStr), "%d.%d", c->rng.shown / 10, c->rng.shown % 10);
    else e_snprintf(c->rngStr, (int)sizeof(c->rngStr), "%d", c->rng.shown);
    c->lbl[0] = 0;
}

static void alt_text(TCache *c, float t, float altM)
{
    if (!tnum_due(&c->alt, t, 1.0f / s_lblHz)) return;
    if (!tnum_apply(&c->alt, altM * M2FT / 1000.0f, 0) && c->altStr[0]) return;
    e_snprintf(c->altStr, (int)sizeof(c->altStr), "%d", c->alt.shown < 0 ? 0 : c->alt.shown);
    c->lbl[0] = 0;
}

static void off_text(TCache *c, float t, float deg)
{
    if (!tnum_due(&c->off, t, 1.0f / s_lblHz)) return;
    if (!tnum_apply(&c->off, deg, 0) && c->offStr[0]) return;
    e_snprintf(c->offStr, (int)sizeof(c->offStr), "%d", c->off.shown < 0 ? 0 : c->off.shown);
}

static const char *label_of(TCache *c, int desig)
{
    if (!c->lbl[0] || c->lblDesig != desig) {
        if (desig) e_snprintf(c->lbl, (int)sizeof(c->lbl), "%s  %s", c->rngStr, c->altStr);
        else e_strlcpy(c->lbl, c->rngStr, (int)sizeof(c->lbl));
        c->lblDesig = desig;
    }
    return c->lbl;
}

/* ================================================================ the symbology */
static u32 sym_col(const TgtSym *s, float a)
{
    int hostile = s->state == CS_HOSTILE || (s->state != CS_DETECTED && (s->flags & CVF_IDENTIFIED));
    if (s->state == CS_DEAD) return hg_col(COL_GREY, a);
    if (s->state == CS_MEMORY) a *= s_memAlpha;
    return hg_col(hostile ? COL_RED : COL_YELLOW, a);
}

static int is_hostile(const TgtSym *s)
{
    return s->state == CS_HOSTILE || (s->state != CS_DETECTED && (s->flags & CVF_IDENTIFIED));
}

static void queue_map(const FrameSym *f, int n, float t)
{
    int pass, i;
    s_dbgMapKills = 0;
    for (i = 0; i < TGT_IDS; i++) {                 /* killed enemies: crossed out for killMapSec (under the contacts) */
        MapContact m;
        if (!s_kill[i].active || t - s_kill[i].t0 > s_killMapSec) continue;
        m.x = s_kill[i].pos.x; m.z = s_kill[i].pos.z; m.headingRad = 0.0f; m.kind = MAPC_KILL; m.flags = 0;
        map_draw_contact(&m);
        s_dbgMapKills++;
    }
    for (pass = 0; pass < 2; pass++)                /* designated last (drawn on top) */
        for (i = 0; i < n; i++) {
            const TgtSym *s = f[i].s;
            MapContact m;
            int des = (s->flags & CVF_DESIGNATED) ? 1 : 0;
            if (s->state == CS_DEAD || des != pass) continue;
            m.x = s->pos.x; m.z = s->pos.z; m.headingRad = s->headingRad;
            m.kind = is_hostile(s) ? MAPC_HOSTILE : MAPC_UNKNOWN;
            m.flags = (s->state == CS_MEMORY ? MAPF_MEMORY : 0) | (des ? MAPF_DESIGNATED : 0) |
                      ((s->flags & CVF_SELECTED) ? MAPF_SELECTED : 0);
            map_draw_contact(&m);
        }
}

void hudtgt_draw_list(const HudState *hs, const TgtSym *inList, int nIn, int toMap)
{
    s64 t0 = plat_qpc();
    FrameSym f[TGT_MAX];
    TgtSym list[TGT_MAX];
    int n = 0;
    int order[TGT_MAX], arrows[TGT_MAX];
    float k, cxs, cys, fx0, fy0, fx1, fy1, t = hs->time, thrIn, thrOut, mbIn[4], mbOut[4];
    float inEnter, inLeave, bright;
    int i, j, nOrd = 0, nArr = 0, des = -1, cockpit = hs->view != VIEW_CHASE, capS, capL;
    u32 green;
    const HudLayout *lay = hud_layout();

    sprites_init();
    r_screen_size(&s_W, &s_H);
    if (s_W <= 0 || s_H <= 0) { s_W = 640; s_H = 480; }
    k = (float)s_H / 480.0f;
    cxs = (float)s_W * 0.5f; cys = (float)s_H * 0.5f;
    bright = s_brightP ? *s_brightP : 1.0f;
    green = hg_col(COL_GREEN, bright);
    capS = hg_cap(HF_SMALL); capL = hg_cap(HF_LARGE);
    fx0 = (float)hg_px(lay->frameX0); fy0 = (float)hg_px(lay->frameY0);
    fx1 = (float)hg_px(lay->frameX1); fy1 = (float)hg_px(lay->frameY1);
    thrIn = s_overlapPx * k; thrOut = (s_overlapPx + 3.0f) * k;
    inEnter = 14.0f * k; inLeave = 6.0f * k;
    map_box(6.0f * k, mbIn);                        /* not on screen while inside the minimap (+6 to enter) */
    map_box(-2.0f * k, mbOut);
    /* kill feedback: T11's DEAD (listed 2 s) starts a record with our own timers; DEAD entries are drawn from the
       records (grey diamond killDiamondSec + fade at the kill position; minimap X for killMapSec) */
    for (i = 0; i < nIn; i++) {
        const TgtSym *s = &inList[i];
        KillRec *kr = &s_kill[(unsigned)s->id % TGT_IDS];
        if (s->state == CS_DEAD) {
            float k0 = t - s->stateAge;
            if (!kr->active || (float)fabs(kr->t0 - k0) > 0.5f) { kr->active = 1; kr->show3d = 1; kr->t0 = k0; kr->pos = s->pos; }
        } else {
            if (kr->active) kr->show3d = 0;         /* the id is a live contact again (respawn): no grey diamond */
            if (n < TGT_MAX) list[n++] = *s;
        }
    }
    for (i = 0; i < TGT_IDS; i++) {
        KillRec *kr = &s_kill[i];
        float age = t - kr->t0;
        if (!kr->active) continue;
        if (age < -0.5f || (age > s_killMapSec && age > s_killDiaSec + s_fadeSec)) { kr->active = 0; continue; }
        if (kr->show3d && age < s_killDiaSec + s_fadeSec && n < TGT_MAX) {
            TgtSym *s = &list[n++];
            s->id = i; s->state = CS_DEAD; s->flags = CVF_IDENTIFIED; s->stateAge = age < 0.0f ? 0.0f : age;
            s->pos = kr->pos; s->headingRad = 0.0f;
        }
    }
    s_frame++;
    s_tdHave = 0;
    s_tdLblHave = 0;
    s_ndbg = 0; s_dbgLabels = 0; s_dbgLoc = 0; s_dbgArrows = 0; s_dbgLocStr[0] = 0;

    /* ---- per contact: projection, hysteresis flags, numbers */
    for (i = 0; i < n; i++) {
        FrameSym *q = &f[i];
        const TgtSym *s = &list[i];
        TCache *c = &s_c[(unsigned)s->id % TGT_IDS];
        Vec3 cv = m4_mul_point(&hs->viewM, s->pos), rel = v3_sub(s->pos, hs->pos);
        float m00 = hs->projM.m[0][0] * (float)s_W * 0.5f, m11 = hs->projM.m[1][1] * (float)s_H * 0.5f;
        float dl, nmA, offDeg;
        int fresh = !c->used || c->frame != s_frame - 1;
        if (fresh) { memset(c, 0, sizeof(*c)); c->used = 1; c->dirX = 0.0f; c->dirY = 1.0f; }
        c->frame = s_frame;
        memset(q, 0, sizeof(*q));
        q->s = s; q->c = c; q->dbg = -1;
        q->range = v3_len(rel);
        q->front = cv.z > 0.05f;
        if (q->front) { q->sx = cxs + cv.x / cv.z * m00; q->sy = cys - cv.y / cv.z * m11; }
        q->dx = cv.x * m00; q->dy = -cv.y * m11;
        dl = (float)sqrt(q->dx * q->dx + q->dy * q->dy);
        if (dl > 0.02f * v3_len(cv) * m00 && dl > 1e-6f) { q->dx /= dl; q->dy /= dl; c->dirX = q->dx; c->dirY = q->dy; }
        else { q->dx = c->dirX; q->dy = c->dirY; }   /* dead ahead / astern: keep the last direction */
        /* hysteresis flags */
        if (fresh) {
            c->onScreen = q->front && q->sx >= 10.0f * k && q->sx <= (float)s_W - 10.0f * k && q->sy >= 10.0f * k &&
                          q->sy <= (float)s_H - 10.0f * k && !in_box(q->sx, q->sy, mbOut);
            c->isNear = q->range < s_hideM + 25.0f;
            c->inArrow = q->range <= s_arrowNm * NM2M + 0.25f * NM2M;
        } else if (c->onScreen) {
            c->onScreen = q->front && q->sx >= inLeave && q->sx <= (float)s_W - inLeave && q->sy >= inLeave &&
                          q->sy <= (float)s_H - inLeave && !in_box(q->sx, q->sy, mbOut);
        } else {
            c->onScreen = q->front && q->sx >= inEnter && q->sx <= (float)s_W - inEnter && q->sy >= inEnter &&
                          q->sy <= (float)s_H - inEnter && !in_box(q->sx, q->sy, mbIn);
        }
        if (!fresh) {
            c->isNear = c->isNear ? q->range < s_hideM + 50.0f : q->range < s_hideM;
            nmA = s_arrowNm * NM2M;
            c->inArrow = c->inArrow ? q->range <= nmA + 0.5f * NM2M : q->range <= nmA;
        }
        if (cockpit) {
            float a = (float)s_boxHalf + (c->inFrame ? -2.0f * k : 2.0f * k);
            c->inFrame = q->front && q->sx >= fx0 + a && q->sx <= fx1 - a && q->sy >= fy0 + a && q->sy <= fy1 - a;
        } else c->inFrame = c->onScreen;
        /* numbers (every frame for every contact, so a label that appears is current) */
        rng_text(c, t, q->range);
        alt_text(c, t, s->pos.y);
        offDeg = q->range > 1.0f ? E_RAD2DEG((float)acos(E_CLAMP(v3_dot(rel, hs->noseDir) / q->range, -1.0f, 1.0f))) : 0.0f;
        off_text(c, t, offDeg);
        /* kind */
        if (s->state == CS_DEAD) q->kind = c->onScreen ? K_DIAMOND : K_NONE;
        else if (c->isNear) q->kind = K_NONE;
        else if (c->onScreen) q->kind = K_DIAMOND;
        else if (c->inArrow && !(s->flags & CVF_DESIGNATED)) q->kind = K_ARROW;
        else q->kind = K_NONE;
        if (q->kind == K_DIAMOND) { q->cx = e_ftoi(q->sx); q->cy = e_ftoi(q->sy); }
        if ((s->flags & CVF_DESIGNATED) && s->state != CS_DEAD) des = i;
    }

    /* ---- declutter: label holders (2 nearest others) with hysteresis */
    {
        int cand[TGT_MAX], nc = 0, h, slots = s_lblOthers < 0 ? 0 : (s_lblOthers > 4 ? 4 : s_lblOthers);
        for (i = 0; i < n; i++)
            if (f[i].kind == K_DIAMOND && f[i].s->state != CS_DEAD && !(f[i].s->flags & (CVF_SELECTED | CVF_DESIGNATED)))
                cand[nc++] = i;
        for (h = 0; h < 4; h++) {                   /* drop holders that are no longer candidates */
            int ok = 0;
            if (s_hold[h] < 0) continue;
            if (h < slots) for (j = 0; j < nc; j++) if (f[cand[j]].s->id == s_hold[h]) ok = 1;
            if (!ok) s_hold[h] = -1;
        }
        for (;;) {
            int best = -1, freeSlot = -1, farH = -1;
            float bestR = 1e30f, farR = -1.0f;
            for (j = 0; j < nc; j++) {              /* nearest non-holder candidate */
                int held = 0;
                for (h = 0; h < slots; h++) if (s_hold[h] == f[cand[j]].s->id) held = 1;
                if (!held && f[cand[j]].range < bestR) { bestR = f[cand[j]].range; best = cand[j]; }
            }
            if (best < 0) break;
            for (h = 0; h < slots; h++) {
                if (s_hold[h] < 0) { if (freeSlot < 0) freeSlot = h; continue; }
                for (j = 0; j < nc; j++)
                    if (f[cand[j]].s->id == s_hold[h] && f[cand[j]].range > farR) { farR = f[cand[j]].range; farH = h; }
            }
            if (freeSlot >= 0) { s_hold[freeSlot] = f[best].s->id; s_holdT[freeSlot] = t; continue; }
            if (farH >= 0 && bestR < 0.85f * farR && t - s_holdT[farH] >= 0.5f) { s_hold[farH] = f[best].s->id; s_holdT[farH] = t; continue; }
            break;
        }
        for (i = 0; i < n; i++) {
            const TgtSym *s = f[i].s;
            if (f[i].kind != K_DIAMOND || s->state == CS_DEAD) continue;
            if (s->flags & (CVF_SELECTED | CVF_DESIGNATED)) f[i].label = 1;
            else for (h = 0; h < slots; h++) if (s_hold[h] == s->id) f[i].label = 1;
        }
    }

    /* ---- draw order: others by id (stable), then selected, then designated (on top) */
    {
        int pass;
        for (pass = 0; pass < 3; pass++)
            for (i = 0; i < n; i++) {
                int fl = f[i].s->flags, p = (fl & CVF_DESIGNATED) ? 2 : ((fl & CVF_SELECTED) ? 1 : 0);
                if (p != pass) continue;
                for (j = nOrd; j > 0 && pass == 0 && f[order[j - 1]].s->id > f[i].s->id; j--) order[j] = order[j - 1];
                order[j] = i; nOrd++;
            }
    }
    /* overlap rule (label side) against earlier-drawn diamonds, with hysteresis */
    for (i = 0; i < nOrd; i++) {
        FrameSym *q = &f[order[i]];
        float best = 1e9f;
        if (q->kind != K_DIAMOND) continue;
        for (j = 0; j < i; j++) {
            const FrameSym *o = &f[order[j]];
            float ddx, ddy, d;
            if (o->kind != K_DIAMOND) continue;
            ddx = (float)(q->cx - o->cx); ddy = (float)(q->cy - o->cy);
            d = (float)sqrt(ddx * ddx + ddy * ddy);
            if (d < best) best = d;
        }
        q->c->right = q->c->right ? best < thrOut : best < thrIn;
    }
    /* off-screen arrows: nearest s_arrowMax */
    for (i = 0; i < n; i++) {
        if (f[i].kind != K_ARROW) continue;
        for (j = nArr; j > 0 && f[arrows[j - 1]].range > f[i].range; j--) arrows[j] = arrows[j - 1];
        arrows[j] = i; nArr++;
    }
    if (nArr > s_arrowMax) nArr = s_arrowMax < 0 ? 0 : s_arrowMax;

    hg_begin();
    hg_clip(0, 0, s_W, s_H);                        /* nothing outside the screen (edge cases: labels, SPLASH) */
    /* arrows */
    for (i = 0; i < nArr; i++) {
        const FrameSym *q = &f[arrows[i]];
        float ax, ay;
        edge_point(q->dx, q->dy, s_arrowInset * k, 10.0f * k, &ax, &ay);
        hg_sprite_rot(s_sprOff, ax, ay, (float)atan2(q->dy, q->dx), sym_col(q->s, 1.0f));
        s_dbgArrows++;
    }
    /* diamonds, labels, triangles */
    for (i = 0; i < nOrd; i++) {
        FrameSym *q = &f[order[i]];
        const TgtSym *s = q->s;
        float a = 1.0f;
        TgtDbg *d = s_ndbg < TGT_MAX ? &s_dbg[s_ndbg] : NULL;
        if (d) {
            memset(d, 0, sizeof(*d));
            d->id = s->id; d->kind = q->kind; d->cx = q->cx; d->cy = q->cy;
            d->fx = q->front ? q->sx : cxs + q->dx * 1e4f; d->fy = q->front ? q->sy : cys + q->dy * 1e4f;
            e_strlcpy(d->rng, q->c->rngStr, (int)sizeof(d->rng));
            d->col = s->state == CS_DEAD ? 2 : (is_hostile(s) ? 1 : 0); d->dashed = s->state == CS_MEMORY;
            q->dbg = s_ndbg++;
        }
        if (q->kind == K_ARROW && d) continue;
        if (q->kind != K_DIAMOND) continue;
        if (s->state == CS_DEAD) {
            a = s->stateAge < s_killDiaSec ? 1.0f : 1.0f - (s->stateAge - s_killDiaSec) / s_fadeSec;
            if (s->stateAge < s_splashSec) {
                hg_text(q->cx, q->cy - s_boxHalf - e_ftoi(4.0f * k) - capL, HF_LARGE, green, 1, "SPLASH");
                if (d) d->splash = 1;
            }
            if (a <= 0.0f) continue;
        }
        hg_sprite(s->state == CS_MEMORY ? s_sprDiaD : s_sprDia, (float)q->cx, (float)q->cy, sym_col(s, a));
        if (q->label) {
            const char *lb = label_of(q->c, (s->flags & CVF_DESIGNATED) ? 1 : 0);
            u32 lc = s->state == CS_MEMORY ? hg_col(COL_GREEN, bright * 0.6f) : green;
            int lw = hg_text_w(HF_SMALL, lb), lx, m = e_ftoi(2.0f * k);
            if (q->c->right) {                      /* right of the symbol; kept on screen near the edge */
                lx = q->cx + s_boxHalf + e_ftoi(3.0f * k);
                if (lx + lw > s_W - m) lx = q->cx - s_boxHalf - e_ftoi(3.0f * k) - lw;
                hg_text(lx, q->cy - capS / 2, HF_SMALL, lc, 0, lb);
            } else {
                lx = q->cx - lw / 2;
                if (lx < m) lx = m;
                if (lx + lw > s_W - m) lx = s_W - m - lw;
                hg_text(lx, q->cy + s_boxHalf + e_ftoi(2.0f * k), HF_SMALL, lc, 0, lb);
            }
            s_dbgLabels++;
            if (s->flags & CVF_DESIGNATED) {        /* T18: hud_wpn keeps LOCK / MISSILE off it */
                s_tdLblHave = 1;
                s_tdLbl[0] = lx; s_tdLbl[2] = lx + lw + hg_shadow_off();
                s_tdLbl[1] = q->c->right ? q->cy - capS / 2 : q->cy + s_boxHalf + e_ftoi(2.0f * k);
                s_tdLbl[3] = s_tdLbl[1] + capS + hg_shadow_off();
            }
            if (d) { d->label = 1; d->right = q->c->right; e_strlcpy(d->lbl, lb, (int)sizeof(d->lbl)); }
        }
        if ((s->flags & CVF_SELECTED) && s->state != CS_DEAD) {
            int ty = q->cy - (s_boxHalf + e_ftoi(3.0f * k));   /* constant: clears the TD box, never moves */
            hg_sprite(s_sprTri, (float)q->cx, (float)ty, green);
            if (d) { d->tri = 1; d->triX = q->cx; d->triY = ty; }
        }
    }
    /* TD box + locator */
    if (des >= 0) {
        FrameSym *q = &f[des];
        const TgtSym *s = q->s;
        int spr = (s->flags & CVF_LOCKMEM) ? s_sprBoxD : s_sprBox, bx, by;
        TgtDbg *d = q->dbg >= 0 ? &s_dbg[q->dbg] : NULL;
        if (q->c->inFrame && (q->kind == K_DIAMOND || q->c->isNear) && q->front) {
            bx = q->kind == K_DIAMOND ? q->cx : e_ftoi(q->sx);
            by = q->kind == K_DIAMOND ? q->cy : e_ftoi(q->sy);
            hg_sprite(spr, (float)bx, (float)by, green);
            if (d) { d->td = 1; d->boxX = bx; d->boxY = by; }
            s_tdHave = 1; s_tdX = bx; s_tdY = by; s_tdClamped = 0;
        } else {
            float ox, oy, px, py, ldx, ldy, len, tipx, tipy, L = s_locLen * k;
            /* clamped box: on the HUD frame (cockpit) / screen edge (chase) along the shortest-rotation direction */
            if (cockpit) {
                float wx = (float)hg_px(lay->wlX), wy = (float)hg_px(lay->wlY), b = (float)s_boxHalf + 1.0f;
                float tt = ray_exit(wx, wy, q->dx, q->dy, fx0 + b, fy0 + b, fx1 - b, fy1 - b);
                px = wx + q->dx * tt; py = wy + q->dy * tt;
                ox = wx; oy = wy;
            } else {
                edge_point(q->dx, q->dy, (float)s_boxHalf + 4.0f * k, (float)s_boxHalf + 2.0f * k, &px, &py);
                ox = cxs; oy = cys;
                if (!hud_project_point(hs, v3_madd(hs->pos, hs->noseDir, 1000.0f), &ox, &oy)) { ox = cxs; oy = cys; }
            }
            bx = e_ftoi(px); by = e_ftoi(py);
            if (flash_on(t, 3.0f)) hg_sprite(spr, (float)bx, (float)by, green);
            if (d) { d->td = 2; d->boxX = bx; d->boxY = by; }
            s_tdHave = 1; s_tdX = bx; s_tdY = by; s_tdClamped = 1;
            /* locator line */
            ldx = q->dx; ldy = q->dy;
            if (q->front) {
                float vx = q->sx - ox, vy = q->sy - oy;
                len = (float)sqrt(vx * vx + vy * vy);
                if (len > 1.0f) { ldx = vx / len; ldy = vy / len; }
            }
            tipx = ox + ldx * L; tipy = oy + ldy * L;
            hg_line(ox + ldx * 8.0f * k, oy + ldy * 8.0f * k, tipx - ldx * 2.0f * k, tipy - ldy * 2.0f * k, green);
            hg_sprite_rot(s_sprLoc, tipx, tipy, (float)atan2(ldy, ldx), green);
            {
                int w = hg_text_w(HF_SMALL, q->c->offStr);
                int tx = e_ftoi(tipx + ldx * ((float)w * 0.5f + 6.0f * k));
                int ty = e_ftoi(tipy + ldy * ((float)capS * 0.5f + 6.0f * k)) - capS / 2;
                hg_text(tx, ty, HF_SMALL, green, 1, q->c->offStr);
            }
            s_dbgLoc = 1; s_dbgLocX = tipx; s_dbgLocY = tipy;
            e_strlcpy(s_dbgLocStr, q->c->offStr, (int)sizeof(s_dbgLocStr));
        }
    }
    hg_end();
    s_dbgQuads = hg_quads_last();
    if (toMap) queue_map(f, n, t);
    s_ms = (float)((double)(plat_qpc() - t0) * 1000.0 / (double)plat_qpf());
}

/* ================================================================ live path */
int hudtgt_build(const Game *g, float alpha, TgtSym *out, int maxOut)
{
    ContactView cv[MAX_AIRCRAFT];
    int n, i, m = 0;
    n = radar_views(g, PLAYER_ID, cv, MAX_AIRCRAFT);
    for (i = 0; i < n && m < maxOut; i++) {
        TgtSym *s = &out[m];
        if (cv[i].state == CS_NONE) continue;
        s->id = cv[i].id;
        s->state = radar_view_pos(g, &cv[i], alpha, &s->pos);
        s->flags = cv[i].flags;
        s->stateAge = cv[i].stateAge;
        s->headingRad = cv[i].heading;
        m++;
    }
    return m;
}

void hudtgt_draw(const Game *g, const Camera *c, float alpha)
{
    HudState hs;
    TgtSym s[TGT_MAX];
    int n, i, j;
    map_set_auto_contacts(0);                       /* T15 owns the minimap contacts */
    hud_state_fill(&hs, g, c, alpha);
    n = hudtgt_build(g, alpha, s, TGT_MAX);
    /* minimap extras first (drawn under the radar contacts): undetected enemies, missiles, RWR ticks */
    if (s_mapNoRadar && map_show_all()) {
        for (i = 1; i < g->numAircraft; i++) {
            const Aircraft *a = &g->ac[i];
            MapContact m;
            int seen = 0;
            if (!(a->flags & AC_ALIVE) || a->side == g->ac[PLAYER_ID].side) continue;
            for (j = 0; j < n; j++) if (s[j].id == i && s[j].state != CS_DEAD) seen = 1;
            if (seen) continue;
            m.x = a->prev.pos.x + (a->fs.pos.x - a->prev.pos.x) * alpha;
            m.z = a->prev.pos.z + (a->fs.pos.z - a->prev.pos.z) * alpha;
            m.headingRad = a->fs.heading; m.kind = MAPC_UNKNOWN; m.flags = MAPF_NORADAR;
            map_draw_contact(&m);
        }
    }
    if (s_mapMissiles) {
        MissileView mv[MAX_MISSILES];
        int nm = weapons_missiles(mv, MAX_MISSILES);
        for (i = 0; i < nm && i < 20; i++) {
            MapContact m;
            m.x = mv[i].prevPos.x + (mv[i].pos.x - mv[i].prevPos.x) * alpha;
            m.z = mv[i].prevPos.z + (mv[i].pos.z - mv[i].prevPos.z) * alpha;
            m.headingRad = 0.0f; m.flags = 0;
            m.kind = mv[i].owner == PLAYER_ID ? MAPC_MSL_OWN : MAPC_MSL_HOSTILE;
            map_draw_contact(&m);
        }
    }
    if (s_mapRwr) {
        RwrView rv;
        radar_rwr(g, PLAYER_ID, &rv);
        for (i = 0; i < rv.nLocks; i++) map_draw_rwr(rv.lockBearing[i]);
    }
    hudtgt_draw_list(&hs, s, n, 1);
}

/* ================================================================ -hudtest contacts (DESIGN 10.8) */
static int s_fail;
static void TFAIL(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    e_vsnprintf(buf, (int)sizeof(buf), fmt, ap);
    va_end(ap);
    log_printf(LOG_INFO, "%s", buf);
    s_fail++;
}

static float s_fovH = 65.0f;

static void cam_mats(HudState *hs, int view, Vec3 pos, Quat qn, Vec3 *eyeOut, Vec3 *atOut, Vec3 *upOut)
{
    Vec3 nf = q_rotate(qn, v3(0.0f, 0.0f, 1.0f)), nu = q_rotate(qn, v3(0.0f, 1.0f, 0.0f)), eye, at;
    if (view == VIEW_CHASE) { eye = v3_add(v3_madd(pos, nf, -24.0f), v3_scale(nu, 5.5f)); at = v3_madd(pos, nf, 40.0f); }
    else { eye = pos; at = v3_madd(eye, nf, 1000.0f); }   /* not eye + unit: float precision at 5-60 km coordinates */
    hs->eye = eye;
    m4_look_lh(&hs->viewM, eye, at, nu);
    m4_persp_lh(&hs->projM, 2.0f * (float)atan(tan(E_DEG2RAD(s_fovH) * 0.5f) * 0.75f), 4.0f / 3.0f, 1.0f, 18000.0f);
    if (eyeOut) *eyeOut = eye;
    if (atOut) *atOut = at;
    if (upOut) *upOut = nu;
}

/* fixed flight state for the shot scenes (like hud.cpp's test_state, with a position) */
static void test_state(HudState *hs, int view, Vec3 pos, float hdgDeg, float pitchDeg, float bankDeg, float aoaDeg)
{
    Quat qn = flight_path_quat(E_DEG2RAD(hdgDeg), E_DEG2RAD(pitchDeg), E_DEG2RAD(bankDeg));
    Quat qp = q_mul(qn, q_axis_angle(v3(1.0f, 0.0f, 0.0f), E_DEG2RAD(aoaDeg)));
    Vec3 r;
    float t = hs->time;
    memset(hs, 0, sizeof(*hs));
    hs->time = t;
    hs->view = view;
    hs->pos = pos;
    hs->velDir = q_rotate(qp, v3(0.0f, 0.0f, 1.0f));
    hs->upDir = q_rotate(qp, v3(0.0f, 1.0f, 0.0f));
    hs->noseDir = q_rotate(qn, v3(0.0f, 0.0f, 1.0f));
    hs->aoaDeg = aoaDeg;
    hs->altFt = pos.y * M2FT;
    hs->headingDeg = E_RAD2DEG((float)atan2(hs->velDir.x, hs->velDir.z));
    if (hs->headingDeg < 0.0f) hs->headingDeg += 360.0f;
    r = q_rotate(qp, v3(1.0f, 0.0f, 0.0f));
    hs->bankDeg = E_RAD2DEG((float)atan2(-r.y, hs->upDir.y));
    hs->clockSec = -1.0f;
    hs->flares = 40;
    hs->speedKt = 452.0f; hs->g = 1.0f; hs->mach = 0.72f; hs->vsFpm = 0.0f;
    cam_mats(hs, view, pos, qn, NULL, NULL, NULL);
}

/* ---- static background like hud.cpp's: sky, haze band, sea, terrain below the true horizon, a cloud patch */
typedef struct BP { float x, y, g; } BP;
static int clip_lin(const BP *in, int n, BP *out, int keepBelow, float lim)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const BP *a = &in[i], *b = &in[(i + 1) % n];
        int ia = keepBelow ? a->g <= lim : a->g >= lim, ib = keepBelow ? b->g <= lim : b->g >= lim;
        if (ia) out[m++] = *a;
        if (ia != ib && m < 15) {
            float tt = (lim - a->g) / (b->g - a->g);
            out[m].x = a->x + (b->x - a->x) * tt; out[m].y = a->y + (b->y - a->y) * tt; out[m].g = lim;
            m++;
        }
    }
    return m;
}

static void bg_poly(const BP *p, int n, u32 col)
{
    int i, j;
    for (i = 1; i + 1 < n; i += 2) {
        RVert2D q[4];
        const BP *v[4];
        v[0] = &p[0]; v[1] = &p[i]; v[2] = &p[i + 1]; v[3] = i + 2 < n ? &p[i + 2] : &p[i + 1];
        for (j = 0; j < 4; j++) { q[j].x = v[j]->x; q[j].y = v[j]->y; q[j].u = 0.0f; q[j].v = 0.0f; q[j].color = col; }
        ui_quad4_px(0, q);
    }
}

static float ray_y(const HudState *hs, float x, float y)
{
    float sx = (x - (float)s_W * 0.5f) / (hs->projM.m[0][0] * (float)s_W * 0.5f);
    float sy = -(y - (float)s_H * 0.5f) / (hs->projM.m[1][1] * (float)s_H * 0.5f);
    return hs->viewM.m[1][0] * sx + hs->viewM.m[1][1] * sy + hs->viewM.m[1][2];
}

static void draw_bg(const HudState *hs)
{
    BP r[4], a[16], b[16];
    int nn, i;
    r[0].x = 0.0f; r[0].y = 0.0f; r[1].x = (float)s_W; r[1].y = 0.0f;
    r[2].x = (float)s_W; r[2].y = (float)s_H; r[3].x = 0.0f; r[3].y = (float)s_H;
    for (i = 0; i < 4; i++) r[i].g = ray_y(hs, r[i].x, r[i].y);
    ui_begin(640, 480);
    ui_set_blend(RB_BLEND);
    nn = clip_lin(r, 4, a, 1, 0.12f);
    if (nn >= 3) { nn = clip_lin(a, nn, b, 0, 0.0f); if (nn >= 3) bg_poly(b, nn, 0xFFB8CCE0); }
    nn = clip_lin(r, 4, a, 1, 0.0f);
    if (nn >= 3) bg_poly(a, nn, 0xFF2D5876);
    nn = clip_lin(r, 4, a, 1, -0.10f);
    if (nn >= 3) bg_poly(a, nn, 0xFF4E6B3A);
    ui_end();
    ui_begin(640, 480);                             /* cloud-white patch upper right (readability) */
    ui_rect(372.0f, 150.0f, 150.0f, 70.0f, 0xFFF4F6F8);
    ui_end();
}

static int frame_begin(void)
{
    if (!plat_pump()) return 0;
    if (!r_begin_frame(0x6A8FC0)) return 0;
    r_screen_size(&s_W, &s_H);
    return 1;
}

static void frame_end(const char *shot)
{
    r_end_frame();
    if (shot) {
        char path[E_PATH];
        int ok;
        base_path(path, (int)sizeof(path), shot);
        ok = r_save_bmp(path);
        log_printf(LOG_INFO, "hud: shot %s ok=%d", shot, ok);
    }
    r_present();
}

static Vec3 at_bearing(Vec3 from, float rangeNm, float bearingDeg, float elDeg)
{
    float R = rangeNm * NM2M, b = E_DEG2RAD(bearingDeg), e = E_DEG2RAD(elDeg);
    return v3(from.x + R * (float)sin(b) * (float)cos(e), from.y + R * (float)sin(e), from.z + R * (float)cos(b) * (float)cos(e));
}

static void sym(TgtSym *s, int id, int state, int flags, float age, Vec3 pos, float hdgDeg)
{
    s->id = id; s->state = state; s->flags = flags; s->stateAge = age; s->pos = pos; s->headingRad = E_DEG2RAD(hdgDeg);
}

static TgtDbg *dbg_of(int id)
{
    int i;
    for (i = 0; i < s_ndbg; i++) if (s_dbg[i].id == id) return &s_dbg[i];
    return NULL;
}

/* ---- part 1: overlap rule (10.7.7) with hysteresis, labels limited to 4 */
static void place_px(const HudState *hs, Vec3 *out, float px, float py, float R)
{
    /* world point R m away that projects to (px, py): inverse of the pinhole via the view matrix columns */
    float sx = (px - (float)s_W * 0.5f) / (hs->projM.m[0][0] * (float)s_W * 0.5f);
    float sy = -(py - (float)s_H * 0.5f) / (hs->projM.m[1][1] * (float)s_H * 0.5f);
    Vec3 r = v3(hs->viewM.m[0][0], hs->viewM.m[1][0], hs->viewM.m[2][0]);
    Vec3 u = v3(hs->viewM.m[0][1], hs->viewM.m[1][1], hs->viewM.m[2][1]);
    Vec3 fw = v3(hs->viewM.m[0][2], hs->viewM.m[1][2], hs->viewM.m[2][2]);
    Vec3 d = v3_norm(v3_add(fw, v3_add(v3_scale(r, sx), v3_scale(u, sy))));
    *out = v3_madd(hs->eye, d, R);
}

static int test_overlap(void)
{
    static const float offs[7] = { 8.0f, 11.0f, 14.0f, 16.0f, 14.0f, 11.0f, 20.0f };
    static const int expect[7] = { 1, 1, 1, 0, 0, 1, 0 };   /* enter < 12, leave >= 15 (x k) */
    HudState hs;
    TgtSym s[8];
    int i, fails = 0, f;
    float k = (float)s_H / 480.0f;
    hudtgt_reset();
    hs.time = 50.0f;
    test_state(&hs, VIEW_COCKPIT, v3(0.0f, 4000.0f, 0.0f), 0.0f, 0.0f, 0.0f, 0.0f);
    for (i = 0; i < 7; i++) {
        Vec3 pa, pb;
        TgtDbg *a, *b;
        place_px(&hs, &pa, (float)s_W * 0.5f + 0.25f, (float)s_H * 0.5f - 40.0f * k + 0.25f, 9.0f * NM2M);
        place_px(&hs, &pb, (float)s_W * 0.5f + 0.25f + offs[i] * k, (float)s_H * 0.5f - 40.0f * k + 0.25f, 9.3f * NM2M);
        sym(&s[0], 1, CS_HOSTILE, CVF_IDENTIFIED, 5.0f, pa, 0.0f);
        sym(&s[1], 2, CS_DETECTED, 0, 5.0f, pb, 0.0f);
        for (f = 0; f < 3; f++) { hs.time += 1.0f / 60.0f; hudtgt_draw_list(&hs, s, 2, 0); }
        a = dbg_of(1); b = dbg_of(2);
        log_printf(LOG_INFO, "hud: tgt overlap off=%.0f a=%d,%d b=%d,%d labelA=%d rightA=%d labelB=%d rightB=%d expect=%d",
                   (double)offs[i], a ? a->cx : -1, a ? a->cy : -1, b ? b->cx : -1, b ? b->cy : -1, a ? a->label : -1,
                   a ? a->right : -1, b ? b->label : -1, b ? b->right : -1, expect[i]);
        if (!a || !b || !a->label || !b->label || a->right || b->right != expect[i]) {
            TFAIL("hud: FAIL overlap off=%.0f rightB=%d expect=%d", (double)offs[i], b ? b->right : -1, expect[i]);
            fails++;
        }
    }
    /* 8 contacts on screen: labels = designated + selected + 2 nearest others = 4 */
    for (i = 0; i < 8; i++) {
        Vec3 p;
        place_px(&hs, &p, (float)s_W * (0.2f + 0.08f * (float)i), (float)s_H * (0.3f + 0.04f * (float)i), (2.0f + (float)i * 3.0f) * NM2M);
        sym(&s[i], i + 1, (i & 1) ? CS_HOSTILE : CS_DETECTED, (i & 1) ? CVF_IDENTIFIED : 0, 5.0f, p, 0.0f);
    }
    s[6].flags |= CVF_DESIGNATED | CVF_LOCKED;
    s[7].flags |= CVF_SELECTED;
    for (f = 0; f < 3; f++) { hs.time += 1.0f / 60.0f; hudtgt_draw_list(&hs, s, 8, 0); }
    {
        int labIds[8], nl = 0, ok;
        for (i = 0; i < s_ndbg; i++) if (s_dbg[i].label) labIds[nl++] = s_dbg[i].id;
        ok = nl == 4 && dbg_of(7) && dbg_of(7)->label && dbg_of(8) && dbg_of(8)->label && dbg_of(1)->label && dbg_of(2)->label;
        log_printf(LOG_INFO, "hud: tgt declutter labels=%d ids=%d,%d,%d,%d %s", nl, nl > 0 ? labIds[0] : -1, nl > 1 ? labIds[1] : -1,
                   nl > 2 ? labIds[2] : -1, nl > 3 ? labIds[3] : -1, ok ? "ok" : "FAIL");
        if (!ok) { TFAIL("hud: FAIL declutter labels=%d", nl); fails++; }
    }
    return fails;
}

/* ---- part 1b: boundary jitter: a symbol sitting on each hysteresis boundary with +-1.5 px (or range) noise must not
   toggle (at most the one change when the noise first crosses into the band) */
static int test_jitter(void)
{
    static const char *names[12] = { "screen-enter", "screen-leave", "minimap-edge", "hud-frame-td", "overlap-12", "overlap-15",
                                     "hide-400m", "arrow-20nm", "range-9.45nm", "range-10nm-unit", "range-1nm-unit", "range-17.5nm" };
    HudState hs;
    TgtSym s[2];
    int c, f, fails = 0;
    float k = (float)s_H / 480.0f;
    for (c = 0; c < 12; c++) {
        int changes = 0, n = 1, have = 0, pk = 0, ptd = 0, pr = 0;
        char prng[8];
        prng[0] = 0;
        hudtgt_reset();
        hs.time = 70.0f;
        test_state(&hs, VIEW_COCKPIT, v3(0.0f, 4000.0f, 0.0f), 0.0f, 0.0f, 0.0f, 0.0f);
        for (f = 0; f < 300; f++) {
            float j = 1.5f * (float)sin((float)f * 1.7f) + 0.4f * (float)sin((float)f * 5.3f);   /* px noise */
            float x = (float)s_W * 0.5f, y = (float)s_H * 0.5f - 60.0f * k, R = 9.0f * NM2M;
            float mb[4];
            TgtDbg *d;
            int flags = CVF_IDENTIFIED, st = CS_HOSTILE;
            map_box(0.0f, mb);
            hs.time += 1.0f / 60.0f;
            n = 1;
            switch (c) {
            case 0: x = 14.0f * k + j; break;                               /* on-screen enter margin */
            case 1: x = 6.0f * k + j; break;                                /* leave margin */
            case 2: x = mb[2] + 2.0f * k + j; y = (mb[1] + mb[3]) * 0.5f; break;   /* minimap right edge */
            case 3: x = (float)hg_px(hud_layout()->frameX0) + (float)s_boxHalf + j; flags |= CVF_DESIGNATED | CVF_LOCKED; break;
            case 4: case 5: break;
            case 6: R = 400.0f + j * 8.0f; break;
            case 7: R = 20.0f * NM2M + j * 40.0f; break;
            case 8: R = 9.45f * NM2M + j * 12.0f; break;
            case 9: R = 10.0f * NM2M + j * 12.0f; break;
            case 10: R = 1.0f * NM2M + j * 12.0f; break;
            case 11: R = 17.5f * NM2M + j * 40.0f; break;
            }
            if (c == 7) {                                                   /* behind: arrow on/off at 20 NM */
                Vec3 back = v3_madd(hs.eye, hs.noseDir, -R);
                back.x += R * 0.3f;
                sym(&s[0], 1, st, flags, 5.0f, back, 0.0f);
            } else {
                place_px(&hs, &s[0].pos, x, y, R);
                sym(&s[0], 1, st, flags, 5.0f, s[0].pos, 0.0f);
            }
            if (c == 4 || c == 5) {
                float dd = (c == 4 ? s_overlapPx : s_overlapPx + 3.0f) * k + j;
                place_px(&hs, &s[0].pos, x, y, R);
                place_px(&hs, &s[1].pos, x + dd, y, R + 500.0f);
                sym(&s[0], 1, st, flags, 5.0f, s[0].pos, 0.0f);
                sym(&s[1], 2, CS_DETECTED, CVF_SELECTED, 5.0f, s[1].pos, 0.0f);
                n = 2;
            }
            hudtgt_draw_list(&hs, s, n, 0);
            d = dbg_of(c == 4 || c == 5 ? 2 : 1);
            {
                int kk = d ? d->kind : -1, td = d ? d->td : -1, rr = d ? d->right : -1;
                const char *rs = d ? d->rng : "";
                if (have && f >= 2 && (kk != pk || td != ptd || rr != pr || (c >= 8 && strcmp(rs, prng)))) changes++;
                pk = kk; ptd = td; pr = rr; e_strlcpy(prng, rs, (int)sizeof(prng)); have = 1;
            }
        }
        log_printf(LOG_INFO, "hud: tgt jitter %-15s changes=%d last kind=%d td=%d right=%d rng=\"%s\" %s", names[c], changes, pk, ptd, pr, prng,
                   changes <= 1 ? "ok" : "FAIL");
        if (changes > 1) { TFAIL("hud: FAIL jitter %s changes=%d", names[c], changes); fails++; }
    }
    return fails;
}

/* ---- part 2: radar pipeline run (real radar + target modules on a local Game): flicker, interpolation jitter,
   world-static deviation, label rates, label count, bounds */
static Game s_tg;
enum { ASP_KIND, ASP_COL, ASP_TRI, ASP_TD, ASP_LBL, ASP_SIDE, ASP_N };
typedef struct Track {
    int have, val[ASP_N]; float since[ASP_N], atX[ASP_N], atY[ASP_N];
    char lbl[20], lblPrev[20]; float lblT, lblPrevT; int lblChanges;
    int fcx, fcy, fHave; float ffx, ffy;            /* formation reference */
    float pcx, pcy, pex, pey; int pHave;            /* static contact previous frame */
} Track;

static void tg_place(int i, Vec3 pos, float hdgDeg, float V)
{
    Aircraft *a = &s_tg.ac[i];
    flight_init(&a->fs, pos, E_DEG2RAD(hdgDeg), V);
    a->fs.alpha = 0.0f;
    a->prev = a->fs;
}

static Vec3 tg_rel(float R, float azDeg, float dy)
{
    const FlightState *fs = &s_tg.ac[0].fs;
    float h = fs->heading + E_DEG2RAD(azDeg);
    return v3(fs->pos.x + R * (float)sin(h), fs->pos.y + dy, fs->pos.z + R * (float)cos(h));
}

static Vec3 s_formOff, s_staticPos;

static void tg_formation(void)
{
    Aircraft *p = &s_tg.ac[0], *a = &s_tg.ac[3];
    a->prev.pos = v3_add(p->prev.pos, q_rotate(flight_nose(&p->prev), s_formOff));
    a->fs.pos = v3_add(p->fs.pos, q_rotate(flight_nose(&p->fs), s_formOff));
    a->prev.qPath = p->prev.qPath; a->fs.qPath = p->fs.qPath;
    a->prev.V = p->prev.V; a->fs.V = p->fs.V;
    a->fs.heading = p->fs.heading;
}

static void tg_tick(PilotInput *in, float simT)
{
    int i;
    PilotInput pin;
    target_update(&s_tg, in);
    memset(in, 0, sizeof(*in));
    for (i = 0; i < s_tg.numAircraft; i++) {
        Aircraft *a = &s_tg.ac[i];
        a->prev = a->fs;
        if (!(a->flags & AC_ALIVE)) continue;
        if (i == 0) {
            memset(&pin, 0, sizeof(pin));
            pin.thr = THR_NORMAL;
            pin.up = 0.10f + 0.10f * (float)sin(simT * 0.9f);
            pin.ur = 0.45f * (float)sin(simT * 2.0f * E_PI / 7.0f);
            flight_step(&a->fs, &pin, SIM_DT);
        } else if (i == 5) {
            a->fs.pos = s_staticPos;
        } else if (i != 3) {
            a->fs.pos = v3_madd(a->fs.pos, q_rotate(a->fs.qPath, v3(0.0f, 0.0f, 1.0f)), a->fs.V * SIM_DT);
        }
    }
    tg_formation();
    radar_tick(&s_tg);
    s_tg.tick++;
}

static void tg_start(void)
{
    int i;
    memset(&s_tg, 0, sizeof(s_tg));
    s_tg.terrain = NULL;                            /* no island loaded in -hudtest: LOS always clear */
    s_tg.numAircraft = 8;
    s_tg.tick = 1;
    for (i = 0; i < MAX_AIRCRAFT; i++) {
        s_tg.ac[i].id = i;
        s_tg.ac[i].side = i ? SIDE_RED : SIDE_BLUE;
        s_tg.ac[i].flags = i < 8 ? AC_ALIVE : 0;
        s_tg.ac[i].hp = 100.0f;
    }
    s_tg.ac[0].flags |= AC_PLAYER;
    tg_place(0, v3(0.0f, 5000.0f, 0.0f), 0.0f, 250.0f);
    tg_place(1, tg_rel(16.5f * NM2M, 3.0f, 300.0f), 180.0f, 300.0f);     /* head-on: DETECTED -> HOSTILE (ID) */
    tg_place(2, tg_rel(8.0f * NM2M, 59.0f, -200.0f), 0.0f, 250.0f);      /* at the scan edge: in/out -> MEMORY */
    s_formOff = v3(300.0f, 60.0f, 2500.0f);                                /* formation: fixed in the player frame */
    tg_place(3, v3_add(s_tg.ac[0].fs.pos, s_formOff), 0.0f, 250.0f);
    tg_place(4, tg_rel(6.0f * NM2M, -15.0f, 0.0f), 165.0f, 280.0f);      /* closing; killed at 12 s (SPLASH) */
    s_staticPos = tg_rel(7.0f * NM2M, 4.0f, 250.0f);                       /* world-static point */
    tg_place(5, s_staticPos, 0.0f, 0.0f);
    tg_place(6, tg_rel(10.0f * NM2M, 40.0f, 500.0f), 90.0f, 420.0f);      /* crossing out of the volume -> MEMORY -> gone */
    tg_place(7, tg_rel(5.0f * NM2M, 180.0f, 0.0f), 0.0f, 250.0f);         /* behind: never detected (map only) */
    tg_formation();
    radar_reset(&s_tg);
    target_reset(&s_tg);
}

static void build_cam(Camera *cam, HudState *hsTmp, int view, float alpha, Vec3 *eye, Vec3 *at, Vec3 *up)
{
    const Aircraft *p = &s_tg.ac[0];
    Quat qp = q_slerp(p->prev.qPath, p->fs.qPath, alpha);
    float aoa = p->prev.alpha + (p->fs.alpha - p->prev.alpha) * alpha;
    Quat qn = q_mul(qp, q_axis_angle(v3(1.0f, 0.0f, 0.0f), -aoa));
    Vec3 pos = v3_lerp(p->prev.pos, p->fs.pos, alpha);
    memset(cam, 0, sizeof(*cam));
    cam_mats(hsTmp, view, pos, qn, eye, at, up);
    cam->mode = view; cam->eye = *eye; cam->q = qn; cam->view = hsTmp->viewM; cam->proj = hsTmp->projM;
    cam->fovDeg = s_fovH; cam->nearM = 1.0f; cam->farM = 18000.0f;
}

/* independent pinhole projection (camera basis from eye/at/up, focal length from the horizontal FOV) */
static int expect_px(Vec3 p, Vec3 eye, Vec3 at, Vec3 up, float *ex, float *ey)
{
    Vec3 fw = v3_norm(v3_sub(at, eye)), r = v3_norm(v3_cross(up, fw)), u = v3_cross(fw, r), v = v3_sub(p, eye);
    float z = v3_dot(v, fw), fpx = (float)s_W * 0.5f / (float)tan(E_DEG2RAD(s_fovH) * 0.5f);
    if (z <= 0.05f) return 0;
    *ex = (float)s_W * 0.5f + v3_dot(v, r) / z * fpx;
    *ey = (float)s_H * 0.5f - v3_dot(v, u) / z * fpx;
    return 1;
}

static int test_pipeline(int view)
{
    static const float sShort[] = { 2.0f, 2.6f, 3.2f, 9.0f, 18.5f, 22.0f, 22.4f, 26.0f };
    static const float sLong[] = { 4.0f, 14.0f, 16.0f, 20.0f, 25.0f };
    Track tr[MAX_AIRCRAFT];
    PilotInput in;
    int frames = 1500, fr, ticks = 0, i, a, fails = 0, flick = 0, lblRate = 0, lblAba = 0, maxLabels = 0, killed = -1;
    int splashSeen = 0, memSeen = 0, hostSeen = 0, tdSeen = 0, locSeen = 0, arrowSeen = 0, bounds = 0, triSeen = 0;
    int quick = 0, formN = 0, statN = 0, formJump = 0, greyLate = 0, greyStay = 0, mapKillLate = 0, mapKillStay = 0;
    float killT = 1e9f, formDevT = 0.0f;
    int formLog = 0, setChanged = 0, prevSet = -1;
    float formDev = 0.0f, statDev = 0.0f, statMotion = 0.0f, statFloat = 0.0f, lastPress = -10.0f;
    unsigned si = 0, li = 0;
    double msSum = 0.0; int msN = 0; float msMax = 0.0f;
    const char *vn = view == VIEW_CHASE ? "chase" : "cockpit";
    memset(tr, 0, sizeof(tr));
    memset(&in, 0, sizeof(in));
    hudtgt_reset();
    tg_start();
    for (fr = 0; fr < frames; fr++) {
        float simT = (float)fr / 50.0f, alpha, t;     /* 50 fps render over the 120 Hz sim: alpha varies */
        Camera cam;
        HudState hs, hsTmp;
        TgtSym s[TGT_MAX];
        Vec3 eye, at, up;
        int n, nl = 0, render = (fr % 3) == 0;
        float bx0, by0, bx1, by1;
        while ((float)ticks * SIM_DT <= simT) {
            float tt = (float)ticks * SIM_DT;
            if (si < E_COUNT(sShort) && tt >= sShort[si]) { in.tgtShort = 1; si++; lastPress = tt; }
            if (li < E_COUNT(sLong) && tt >= sLong[li]) { in.tgtLong = 1; li++; lastPress = tt; }
            if (killed < 0 && tt >= 12.0f) {
                int d = target_designated(), c2;
                TgtDbg *dd = d > 0 ? dbg_of(d) : NULL;
                killed = (d > 0 && d != 3 && d != 5 && dd && dd->kind == K_DIAMOND) ? d : -1;
                for (c2 = 1; c2 < 8 && killed < 0; c2++)   /* else a visible contact (SPLASH on screen) */
                    if (c2 != 3 && c2 != 5 && dbg_of(c2) && dbg_of(c2)->kind == K_DIAMOND) killed = c2;
                if (killed < 0) killed = 4;
                s_tg.ac[killed].flags &= ~AC_ALIVE;
                killT = tt;
                log_printf(LOG_INFO, "hud: tgt pipeline %s kill id=%d t=%.2f", vn, killed, (double)tt);
            }
            tg_tick(&in, tt);
            ticks++;
        }
        alpha = simT / SIM_DT - (float)(ticks - 1);
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;
        build_cam(&cam, &hsTmp, view, alpha, &eye, &at, &up);
        hud_state_fill(&hs, &s_tg, &cam, alpha);
        t = hs.time;
        n = hudtgt_build(&s_tg, alpha, s, TGT_MAX);
        if (render) {
            if (!frame_begin()) return fails + 1;
            draw_bg(&hs);
            hud_draw_state(&hs);
        }
        hg_bounds_reset();
        hudtgt_draw_list(&hs, s, n, render);
        if (hg_bounds(&bx0, &by0, &bx1, &by1) && (bx0 < -0.5f || by0 < -0.5f || bx1 > (float)s_W + 0.5f || by1 > (float)s_H + 0.5f)) bounds++;
        if (render) {
            map_draw_state(&hs, NULL);
            frame_end(fr == 600 ? (view == VIEW_CHASE ? "shots\\tgt_pipe_chase.bmp" : "shots\\tgt_pipe_cockpit.bmp") : NULL);
            if (hudtgt_ms_last() > msMax) msMax = hudtgt_ms_last();
            msSum += hudtgt_ms_last(); msN++;
        }
        {
            int set = 0;
            for (i = 0; i < s_ndbg; i++) if (s_dbg[i].kind == K_DIAMOND) set |= 1 << s_dbg[i].id;
            setChanged = set != prevSet;
            prevSet = set;
        }
        if (s_dbgLoc) locSeen++;
        if (killed >= 0) {                          /* kill feedback outlives the radar's 2 s DEAD listing */
            TgtDbg *kd = dbg_of(killed);
            float ka = t - killT;
            if (kd && kd->col == 2 && ka > 2.3f && ka < s_killDiaSec) greyLate++;
            if (kd && ka > s_killDiaSec + s_fadeSec + 0.2f) greyStay++;
            if (render && s_dbgMapKills > 0 && ka > 10.0f && ka < s_killMapSec - 0.3f) mapKillLate++;
            if (render && s_dbgMapKills > 0 && ka > s_killMapSec + 0.3f) mapKillStay++;
        }
        if (s_dbgArrows) arrowSeen++;
        /* per contact checks */
        for (i = 0; i < MAX_AIRCRAFT; i++) {
            Track *k = &tr[i];
            TgtDbg *d = dbg_of(i);
            int v[ASP_N], present = d != NULL;
            const TgtSym *ss = NULL;
            for (a = 0; a < n; a++) if (s[a].id == i) ss = &s[a];
            if (!present) { k->have = 0; k->fHave = 0; k->pHave = 0; k->lbl[0] = 0; k->lblPrev[0] = 0; continue; }
            if (d->label) nl++;
            if (d->splash) splashSeen++;
            if (d->dashed) memSeen++;
            if (d->col == 1 && d->kind == K_DIAMOND) hostSeen++;
            if (d->td) tdSeen++;
            if (d->tri) triSeen++;
            v[ASP_KIND] = d->kind; v[ASP_COL] = d->col * 2 + d->dashed + (ss && ss->state == CS_DEAD ? 8 : 0);
            v[ASP_TRI] = d->tri; v[ASP_TD] = d->td; v[ASP_LBL] = d->label; v[ASP_SIDE] = d->label ? d->right : 0;
            /* state aspects (colour/dash, selection triangle): latched >= 0.3 s unless a key press / kill / the kind
               change explains it. Geometric aspects (kind, TD placement, label shown, label side): a reversal within
               0.3 s counts as flicker only if the symbol did not move (>= 3 px) since the last change: hysteresis
               bands are crossed by real motion only (the boundary-jitter test proves the bands) */
            if (k->have) {
                for (a = 0; a < ASP_N; a++) {
                    if (v[a] == k->val[a]) continue;
                    if (t - k->since[a] < 0.29f) {
                        int geo = a == ASP_KIND || a == ASP_TD || a == ASP_LBL || a == ASP_SIDE, excused;
                        float mv = (float)sqrt((d->fx - k->atX[a]) * (d->fx - k->atX[a]) + (d->fy - k->atY[a]) * (d->fy - k->atY[a]));
                        excused = (ss && ss->state == CS_DEAD) || d->col == 2 || t - lastPress < 0.12f;
                        if (a != ASP_KIND && v[ASP_KIND] != k->val[ASP_KIND]) excused = 1;   /* follows the kind */
                        if (a == ASP_SIDE && v[ASP_LBL] != k->val[ASP_LBL]) excused = 1;
                        if ((a == ASP_SIDE || a == ASP_LBL) && setChanged) excused = 1;   /* a neighbour appeared/left */
                        if (geo) { quick++; if (mv >= 3.0f) excused = 1; }
                        if (!excused) {
                            flick++;
                            if (flick <= 12)
                                log_printf(LOG_INFO, "hud: tgt flicker %s id=%d aspect=%d %d->%d after %.3f s moved %.1f px t=%.2f", vn, i, a,
                                           k->val[a], v[a], (double)(t - k->since[a]), (double)mv, (double)t);
                        }
                    }
                    k->since[a] = t; k->atX[a] = d->fx; k->atY[a] = d->fy;
                }
            } else for (a = 0; a < ASP_N; a++) { k->since[a] = t; k->atX[a] = d->fx; k->atY[a] = d->fy; }
            for (a = 0; a < ASP_N; a++) k->val[a] = v[a];
            k->have = 1;
            /* range text (recorded every frame, shown or not): rate and A-B-A */
            if (d->rng[0] && strcmp(d->rng, k->lbl)) {
                if (k->lbl[0] && t - k->lblT < 0.19f) {
                    lblRate++;
                    if (lblRate <= 6) log_printf(LOG_INFO, "hud: tgt label rate %s id=%d \"%s\"->\"%s\" dt=%.3f", vn, i, k->lbl, d->rng, (double)(t - k->lblT));
                }
                if (k->lblPrev[0] && !strcmp(d->rng, k->lblPrev) && t - k->lblPrevT < 0.6f) {
                    lblAba++;
                    if (lblAba <= 6) log_printf(LOG_INFO, "hud: tgt label flicker %s id=%d \"%s\"<->\"%s\"", vn, i, k->lbl, d->rng);
                }
                e_strlcpy(k->lblPrev, k->lbl, (int)sizeof(k->lblPrev)); k->lblPrevT = k->lblT;
                e_strlcpy(k->lbl, d->rng, (int)sizeof(k->lbl)); k->lblT = t; k->lblChanges++;
            }
            /* formation contact: screen position must not move at all (interpolation consistency) */
            if (i == 3 && d->kind == K_DIAMOND) {
                if (k->fHave) {
                    float dv = (float)sqrt((d->fx - k->ffx) * (d->fx - k->ffx) + (d->fy - k->ffy) * (d->fy - k->ffy));
                    if (dv > formDev) { formDev = dv; formDevT = t; }
                    if (dv > 0.05f && formLog < 6) {
                        formLog++;
                        float ex = 0.0f, ey = 0.0f;
                        Vec3 rel = ss ? v3_sub(ss->pos, eye) : v3(0.0f, 0.0f, 0.0f);
                        Mat3 m;
                        if (ss) expect_px(ss->pos, eye, at, up, &ex, &ey);
                        q_to_mat3(cam.q, &m);
                        rel = m3_tmul(&m, rel);
                        log_printf(LOG_INFO, "hud: tgt formation %s t=%.3f alpha=%.3f fx=%.3f fy=%.3f ref=%.3f,%.3f exp=%.3f,%.3f body=%.3f,%.3f,%.3f state=%d tick=%u",
                                   vn, (double)t, (double)alpha, (double)d->fx, (double)d->fy, (double)k->ffx, (double)k->ffy, (double)ex, (double)ey,
                                   (double)rel.x, (double)rel.y, (double)rel.z, ss ? ss->state : -1, s_tg.tick);
                    }
                    if (d->cx != k->fcx || d->cy != k->fcy) formJump++;
                    formN++;
                } else { k->ffx = d->fx; k->ffy = d->fy; k->fcx = d->cx; k->fcy = d->cy; k->fHave = 1; }
            }
            /* world-static contact vs an independent projection */
            if (i == 5 && d->kind == K_DIAMOND && ss) {
                float ex, ey;
                if (expect_px(ss->pos, eye, at, up, &ex, &ey)) {
                    float dx = (float)d->cx - ex, dy = (float)d->cy - ey, df;
                    float dev = E_MAXF((float)fabs(dx), (float)fabs(dy));
                    if (dev > statDev) statDev = dev;
                    df = E_MAXF((float)fabs(d->fx - ex), (float)fabs(d->fy - ey));
                    if (df > statFloat) statFloat = df;
                    if (k->pHave) {
                        float mdev = E_MAXF((float)fabs(((float)d->cx - k->pcx) - (ex - k->pex)), (float)fabs(((float)d->cy - k->pcy) - (ey - k->pey)));
                        if (mdev > statMotion) statMotion = mdev;
                    }
                    k->pcx = (float)d->cx; k->pcy = (float)d->cy; k->pex = ex; k->pey = ey; k->pHave = 1;
                    statN++;
                }
            } else if (i == 5) k->pHave = 0;
        }
        if (nl > maxLabels) maxLabels = nl;
        if ((fr % 250) == 0 || fr == frames - 1) {
            char buf[200];
            int p = 0;
            buf[0] = 0;
            for (i = 0; i < s_ndbg && p < 180; i++)
                p += e_snprintf(buf + p, (int)sizeof(buf) - p, " %d:%c%s%s%s%s(%d,%d)\"%s\"", s_dbg[i].id,
                                s_dbg[i].kind == K_DIAMOND ? 'D' : (s_dbg[i].kind == K_ARROW ? 'A' : '-'),
                                s_dbg[i].col == 2 ? "g" : (s_dbg[i].col ? "r" : "y"), s_dbg[i].dashed ? "m" : "", s_dbg[i].tri ? "s" : "",
                                s_dbg[i].td == 1 ? "T" : (s_dbg[i].td == 2 ? "C" : ""), s_dbg[i].cx, s_dbg[i].cy, s_dbg[i].lbl);
            log_printf(LOG_INFO, "hud: tgt pipe %s t=%.2f n=%d sel=%d des=%d lock=%d loc=%d \"%s\" arrows=%d%s", vn, (double)t, n,
                       target_selected(), target_designated(), radar_lock(PLAYER_ID), s_dbgLoc, s_dbgLocStr, s_dbgArrows, buf);
        }
    }
    log_printf(LOG_INFO, "hud: tgt pipeline %s geometric transitions within 0.3 s (moving symbols, not flicker)=%d", vn, quick);
    log_printf(LOG_INFO, "hud: tgt pipeline %s frames=%d flicker=%d labelRate=%d labelAba=%d maxLabels=%d bounds=%d formation n=%d dev=%.4fpx (t=%.2f) jumps=%d static n=%d dev=%.3fpx float=%.4fpx motion=%.3fpx",
               vn, frames, flick, lblRate, lblAba, maxLabels, bounds, formN, (double)formDev, (double)formDevT, formJump, statN, (double)statDev,
               (double)statFloat, (double)statMotion);
    log_printf(LOG_INFO, "hud: tgt pipeline %s seen hostile=%d memory=%d splash=%d tri=%d td=%d locator=%d arrows=%d ms avg=%.3f max=%.3f",
               vn, hostSeen, memSeen, splashSeen, triSeen, tdSeen, locSeen, arrowSeen, msN ? msSum / msN : 0.0, (double)msMax);
    if (flick) { TFAIL("hud: FAIL %s state flicker=%d", vn, flick); fails++; }
    if (lblRate || lblAba) { TFAIL("hud: FAIL %s label rate=%d aba=%d", vn, lblRate, lblAba); fails++; }
    if (maxLabels > 4) { TFAIL("hud: FAIL %s labels=%d > 4", vn, maxLabels); fails++; }
    if (bounds) { TFAIL("hud: FAIL %s drawn outside the screen %d frames", vn, bounds); fails++; }
    if (formN < 100 || formDev > 0.1f || formJump) { TFAIL("hud: FAIL %s formation n=%d dev=%.4f jumps=%d", vn, formN, (double)formDev, formJump); fails++; }
    if (statN < 100 || statDev > 0.51f || statFloat > 0.02f || statMotion >= 1.0f) {
        TFAIL("hud: FAIL %s static n=%d dev=%.3f float=%.4f motion=%.3f", vn, statN, (double)statDev, (double)statFloat, (double)statMotion);
        fails++;
    }
    log_printf(LOG_INFO, "hud: tgt pipeline %s kill grey after radar drop=%d frames, stays too long=%d, map X 10-15 s=%d frames, stays too long=%d",
               vn, greyLate, greyStay, mapKillLate, mapKillStay);
    if (!greyLate || greyStay || !mapKillLate || mapKillStay) { TFAIL("hud: FAIL %s kill feedback grey=%d/%d mapX=%d/%d", vn, greyLate, greyStay, mapKillLate, mapKillStay); fails++; }
    if (!hostSeen || !memSeen || !splashSeen || !tdSeen || !triSeen) {
        TFAIL("hud: FAIL %s scenario coverage hostile=%d memory=%d splash=%d td=%d tri=%d", vn, hostSeen, memSeen, splashSeen, tdSeen, triSeen);
        fails++;
    }
    return fails;
}

/* ---- part 3: review scenes (screenshots) + cost */
enum { SC_IN = 0, SC_BEHIND = 1 };
static int scene_syms(const HudState *hs, int sc, TgtSym *s)
{
    Vec3 p = hs->pos;
    float h = hs->headingDeg;
    int n = 0;
    int desIn = sc == SC_IN ? CVF_DESIGNATED | CVF_LOCKED | CVF_SELECTED : 0;
    sym(&s[n++], 1, CS_HOSTILE, CVF_IDENTIFIED | desIn | (sc == SC_BEHIND ? CVF_SELECTED : 0), 5.0f, at_bearing(p, 9.4f, h + 4.0f, 1.2f), h + 175.0f);
    sym(&s[n++], 2, CS_DETECTED, 0, 5.0f, at_bearing(p, 22.0f, h - 9.0f, -0.4f), h + 160.0f);
    sym(&s[n++], 3, CS_HOSTILE, CVF_IDENTIFIED, 5.0f, at_bearing(p, 14.0f, h + 12.0f, -2.0f), h + 200.0f);
    sym(&s[n++], 4, CS_DETECTED, 0, 5.0f, at_bearing(p, 14.3f, h + 12.5f, -2.3f), h + 210.0f);
    sym(&s[n++], 5, CS_MEMORY, CVF_IDENTIFIED, 2.0f, at_bearing(p, 6.0f, h - 17.0f, -7.0f), h + 120.0f);
    sym(&s[n++], 6, CS_HOSTILE, CVF_IDENTIFIED, 5.0f, at_bearing(p, 0.8f, h - 6.0f, -3.5f), h + 90.0f);
    sym(&s[n++], 7, CS_DETECTED, sc == SC_BEHIND ? CVF_DESIGNATED | CVF_LOCKMEM : 0, 5.0f, at_bearing(p, 12.0f, h + 150.0f, 10.0f), h);
    sym(&s[n++], 8, CS_DEAD, CVF_IDENTIFIED, sc == SC_IN ? 0.35f : 2.4f, at_bearing(p, 3.0f, h + 19.0f, 4.0f), h);   /* kill: grey (+SPLASH) */
    sym(&s[n++], 9, CS_DEAD, CVF_IDENTIFIED, 8.0f, at_bearing(p, 7.0f, h - 40.0f, 0.0f), h);   /* old kill: minimap X only */
    return n;
}

static void scene_state(HudState *hs, int view, int sc)
{
    if (sc == SC_IN) test_state(hs, view, v3(1500.0f, 12450.0f * FT2M, -2000.0f), 40.0f, 2.6f, 0.0f, 2.6f);
    else test_state(hs, view, v3(1500.0f, 9800.0f * FT2M, -2000.0f), 70.0f, 6.0f, 25.0f, 3.0f);
}

static int test_shots(void)
{
    int view, sc, f, fails = 0;
    char shot[64];
    for (view = 0; view < 2; view++) for (sc = 0; sc < 2; sc++) {
        HudState hs;
        TgtSym s[TGT_MAX];
        int n;
        hudtgt_reset();
        hud_reset();
        hs.time = 10.0f - 6.0f / 60.0f;
        for (f = 0; f < 7; f++) {
            float tt = hs.time + 1.0f / 60.0f;
            hs.time = tt;
            scene_state(&hs, view ? VIEW_CHASE : VIEW_COCKPIT, sc);
            hs.time = tt;
            e_strlcpy(hs.weapon, "AIM-120 16", (int)sizeof(hs.weapon));
            n = scene_syms(&hs, sc, s);
            if (!frame_begin()) return fails + 1;
            draw_bg(&hs);
            hud_draw_state(&hs);
            hudtgt_draw_list(&hs, s, n, 1);
            map_draw_state(&hs, NULL);
            e_snprintf(shot, (int)sizeof(shot), "shots\\tgt_%s_%s_%d.bmp", view ? "chase" : "cockpit", sc ? "behind" : "in", s_H);
            frame_end(f == 6 ? shot : NULL);
        }
        {
            char buf[240];
            int i, p = 0;
            buf[0] = 0;
            for (i = 0; i < s_ndbg && p < 220; i++)
                p += e_snprintf(buf + p, (int)sizeof(buf) - p, " %d:%c%s%s%s%s(%d,%d)\"%s\"%s", s_dbg[i].id,
                                s_dbg[i].kind == K_DIAMOND ? 'D' : (s_dbg[i].kind == K_ARROW ? 'A' : '-'),
                                s_dbg[i].col == 2 ? "g" : (s_dbg[i].col ? "r" : "y"), s_dbg[i].dashed ? "m" : "", s_dbg[i].tri ? "s" : "",
                                s_dbg[i].td == 1 ? "T" : (s_dbg[i].td == 2 ? "C" : ""), s_dbg[i].cx, s_dbg[i].cy, s_dbg[i].lbl,
                                s_dbg[i].splash ? "SPLASH" : "");
            log_printf(LOG_INFO, "hud: tgt scene %s %s quads=%d loc=%d \"%s\" at %.0f,%.0f arrows=%d mapX=%d%s", view ? "chase" : "cockpit",
                       sc ? "behind" : "in", s_dbgQuads, s_dbgLoc, s_dbgLocStr, (double)s_dbgLocX, (double)s_dbgLocY, s_dbgArrows, s_dbgMapKills, buf);
            if (s_dbgMapKills != 2 || !dbg_of(8) || dbg_of(8)->col != 2 || dbg_of(9)) { TFAIL("hud: FAIL scene kill marks mapX=%d", s_dbgMapKills); fails++; }
            if (sc == SC_BEHIND && !s_dbgLoc) { TFAIL("hud: FAIL scene behind: no locator"); fails++; }
            if (sc == SC_IN && (!dbg_of(1) || dbg_of(1)->td != 1)) { TFAIL("hud: FAIL scene in: TD box not on the designated diamond"); fails++; }
            if (dbg_of(1) && dbg_of(1)->td == 1 && (dbg_of(1)->boxX != dbg_of(1)->cx || dbg_of(1)->boxY != dbg_of(1)->cy)) {
                TFAIL("hud: FAIL TD box anchor %d,%d != diamond %d,%d", dbg_of(1)->boxX, dbg_of(1)->boxY, dbg_of(1)->cx, dbg_of(1)->cy);
                fails++;
            }
        }
    }
    return fails;
}

static int test_cost(int nSyms)
{
    int view, f;
    for (view = 0; view < 2; view++) {
        HudState hs;
        TgtSym s[TGT_MAX];
        double sum = 0.0, mapWith = 0.0, mapWithout = 0.0;
        float mx = 0.0f;
        int n = 0, q = 0, cnt = 0;
        hudtgt_reset();
        hud_reset();
        hs.time = 30.0f;
        for (f = 0; f < 240; f++) {
            float tt = 30.0f + (float)f / 60.0f;
            int withMap = f & 1;
            hs.time = tt;
            scene_state(&hs, view ? VIEW_CHASE : VIEW_COCKPIT, SC_IN);
            test_state(&hs, view ? VIEW_CHASE : VIEW_COCKPIT, hs.pos, 40.0f + 2.0f * (float)sin(tt), 2.6f + (float)sin(tt * 0.7f), 8.0f * (float)sin(tt * 0.5f), 2.6f);
            hs.time = tt;
            n = scene_syms(&hs, SC_IN, s);
            if (nSyms < n) n = nSyms;
            if (!frame_begin()) return 1;
            draw_bg(&hs);
            hud_draw_state(&hs);
            hudtgt_draw_list(&hs, s, n, withMap);
            map_draw_state(&hs, NULL);
            frame_end(NULL);
            if (f >= 40) {
                sum += hudtgt_ms_last(); cnt++;
                if (hudtgt_ms_last() > mx) mx = hudtgt_ms_last();
                if (withMap) mapWith += map_ms_last(); else mapWithout += map_ms_last();
                q = s_dbgQuads;
            }
        }
        log_printf(LOG_INFO, "hud: tgt cost %s contacts=%d symbology avg=%.3f ms max=%.3f quads=%d | map with contacts=%.3f without=%.3f -> +%.3f ms | total +%.3f ms",
                   view ? "chase" : "cockpit", n, cnt ? sum / cnt : 0.0, (double)mx, q, mapWith / (cnt / 2), mapWithout / (cnt / 2),
                   (mapWith - mapWithout) / (cnt / 2), cnt ? sum / cnt + (mapWith - mapWithout) / (cnt / 2) : 0.0);
    }
    return 0;
}

int hudtgt_test(void)
{
    int r = 0;
    s_fail = 0;
    r_screen_size(&s_W, &s_H);
    sprites_init();
    log_printf(LOG_INFO, "hud: tgt test screen=%dx%d set=%d diaHalf=%d boxHalf=%d", s_W, s_H, hg_set_lines(), s_diaHalf, s_boxHalf);
    test_overlap();
    test_jitter();
    test_pipeline(VIEW_COCKPIT);
    test_pipeline(VIEW_CHASE);
    test_shots();
    test_cost(6);
    test_cost(8);
    r = s_fail;
    hudtgt_reset();
    log_printf(LOG_INFO, "hud: tgt test failures=%d", r);
    return r;
}

/* T18: the designated contact's label rectangle (px, x0 y0 x1 y1, shadow included) of the last hudtgt_draw_list */
int hudtgt_td_label(int *r4)
{
    int i;
    if (r4) for (i = 0; i < 4; i++) r4[i] = s_tdLbl[i];
    return s_tdLblHave;
}
