/* hud.cpp - flight HUD + -hudtest scenes (DESIGN 10.1-10.3, 10.7, 10.8). See hud.h. Windows/render side. Owner: T07.
   Layout values are virtual 640x480 units (docs/HUD_LAYOUT.md); everything is placed at x k (hud_font.h). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>

#include "hud.h"
#include "hud_font.h"
#include "hud_map.h"
#include "hud_wpn.h"
#include "params.h"
#include "e_log.h"
#include "e_file.h"
#include "e_image.h"
#include "e_plat.h"
#include "e_ui.h"
#include "flight.h"

/* ================================================================ params + layout */
static float s_bright, s_aseR, s_aseOut, s_aseGap600, s_padX, s_padY, s_chaseAlpha, s_ladderWin;
static int   s_chaseHeading, s_chaseMach;

static const Param s_tab[] = {
    { "hud", "bright", P_FLOAT, &s_bright, "1.0", 0.2f, 1.0f, "HUD brightness (scales the green)" },
    { "hud", "aseRadius", P_FLOAT, &s_aseR, "60", 30, 90, "launch-zone (ASE) circle radius, virtual 640x480 units (T16 draws it)" },
    { "hud", "aseOuter", P_FLOAT, &s_aseOut, "8", 0, 20, "ASE circle range ticks/pointer reach beyond the radius (virtual units)" },
    { "hud", "aseGapMin600", P_FLOAT, &s_aseGap600, "6", 0, 30, "minimum gap ASE circle <-> speed/altitude boxes, px at 600 lines (scaled per set; -hudtest layout)" },
    { "hud", "boxPadX", P_FLOAT, &s_padX, "3", 1, 8, "speed/altitude box padding left/right of the digits (virtual units)" },
    { "hud", "boxPadY", P_FLOAT, &s_padY, "2.5", 1, 8, "speed/altitude box padding above/below the digits (virtual units)" },
    { "hud", "chaseAlpha", P_FLOAT, &s_chaseAlpha, "0.5", 0.1f, 1.0f, "chase view horizon/rung alpha" },
    { "hud", "ladderWindow", P_FLOAT, &s_ladderWin, "30", 10, 60, "pitch ladder rungs drawn within this many degrees of the VV" },
    { "hud", "chaseHeading", P_BOOL, &s_chaseHeading, "1", 0, 1, "chase view: small heading tape at the top" },
    { "hud", "chaseMach", P_BOOL, &s_chaseMach, "0", 0, 1, "chase view: Mach under the G readout" },
};

static HudLayout s_lay = {
    170.0f, 152.0f, 470.0f, 384.0f,                 /* frame (DESIGN 10.2; top moved 116 -> 152: below the heading caret) */
    320.0f, 240.0f,                                 /* waterline */
    320.0f, 240.0f, 60.0f, 8.0f,                    /* ASE circle (radius/outer from params) */
    244.0f, 228.0f, 396.0f, 228.0f,                 /* cockpit boxes: speed right edge, altitude left edge */
    198.0f, 442.0f, 228.0f,                         /* chase boxes (DESIGN 10.3) */
    320.0f, 200.0f, 216.0f                          /* T16b chase circle: x, y clamp (default camera: y 205 at FOV 70 .. 214
                                                       at 85; >= 200 keeps the ring clear of chase MISSILE / LOCK) */
};

void hud_params(void)
{
    params_add(s_tab, E_COUNT(s_tab));
    hfont_params();
    map_params();
}

const HudLayout *hud_layout(void)
{
    s_lay.aseR = s_aseR;
    s_lay.aseOut = s_aseOut;
    return &s_lay;
}

/* T16b: chase launch-zone circle centre = the steady-state aim cross. In the camera frame of a chase camera without lag
   (eye chaseDist behind / chaseUp above along the nose frame, looking at the nose point chaseLook ahead) the 1 km nose
   point lies atan(U/(D+L)) - atan(U/(D+1000)) above the view axis, straight up from the screen centre; its screen offset
   is m[1][1] x 240 x tan(that) virtual units. Only the FOV (smoothed by the camera) changes it. */
void hud_chase_ase(const HudState *hs, float *cxV, float *cyV)
{
    static const Param *pd, *pu, *pl;
    static int looked;
    float D = 24.0f, U = 6.0f, L = 40.0f, d, y;
    if (!looked) { pd = params_find("camera", "chaseDist"); pu = params_find("camera", "chaseUp"); pl = params_find("camera", "chaseLook"); looked = 1; }
    if (pd) D = *(const float *)pd->ptr;
    if (pu) U = *(const float *)pu->ptr;
    if (pl) L = *(const float *)pl->ptr;
    d = (float)atan2(U, D + L) - (float)atan2(U, D + 1000.0f);
    y = 240.0f - hs->projM.m[1][1] * 240.0f * (float)tan(d);
    *cxV = s_lay.chAseX;
    *cyV = E_CLAMP(y, s_lay.chAseY0, s_lay.chAseY1);
}

/* tape, bank scale, text positions (virtual) */
#define TAPE_Y      141.0f                          /* tick base (ticks hang up from here) */
#define TAPE_HALF   120.0f
#define TAPE_PXDEG  8.0f
#define BANK_R      112.0f
#define MSG_Y       168.0f
#define RETURN_Y    186.0f
#define PULLUP_Y    300.0f
#define WEAPON_Y    368.0f
#define CLOCK_X     178.0f
#define CLOCK_Y     372.0f
#define HUD_GREEN   0xFF40FF60u

/* ================================================================ numbers (10.7.3) */
typedef struct HNum {
    float period, quantum, next, hyst;
    int init, shown, upd, lastDir, lastUpd, havePend, pendDir;
    int changes;
    char str[16];
    int w;
} HNum;

static void hnum_setup(HNum *n, float rateHz, float quantum)
{
    memset(n, 0, sizeof(*n));
    n->period = 1.0f / rateHz;
    n->quantum = quantum;
    n->hyst = 0.2f;
}

static int qround(float q) { return (int)floor(q + 0.5f); }

/* 1 = shown changed. Fixed update rate; dead band of 0.5 + hyst quanta; a change continuing the last change's
   direction at the next update applies at once, any other change (first move, reversal) needs 2 updates in a row. */
static int hnum_update(HNum *n, float t, float raw)
{
    float q;
    int cand, dir;
    if (n->init && t + 1e-5f < n->next) return 0;
    if (!n->init) n->next = t;
    n->next += n->period;
    if (n->next <= t) n->next = t + n->period;
    n->upd++;
    q = raw / n->quantum;
    if (!n->init) {
        n->init = 1; n->shown = qround(q); n->lastDir = 0; n->havePend = 0; n->lastUpd = -10;
        n->changes++;
        return 1;
    }
    if ((float)fabs(q - (float)n->shown) <= 0.5f + n->hyst) { n->havePend = 0; return 0; }
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

static float hnum_value(const HNum *n) { return (float)n->shown * n->quantum; }

enum { N_SPD, N_ALT, N_VVI, N_MACH, N_G, N_AOA, N_COUNT };
static HNum  s_num[N_COUNT];
static float s_peak; static float s_peakLow; static char s_peakStr[8]; static int s_peakW, s_peakShown;
static int   s_numInit;

static void numbers_reset(void)
{
    hnum_setup(&s_num[N_SPD], 10.0f, 1.0f);
    hnum_setup(&s_num[N_ALT], 10.0f, 10.0f);
    hnum_setup(&s_num[N_VVI], 5.0f, 10.0f);
    hnum_setup(&s_num[N_MACH], 5.0f, 0.01f);
    hnum_setup(&s_num[N_G], 5.0f, 0.1f);
    hnum_setup(&s_num[N_AOA], 5.0f, 0.1f);
    s_peak = 0.0f; s_peakLow = 0.0f; s_peakStr[0] = 0; s_peakW = 0; s_peakShown = 0;
    s_numInit = 1;
}

static void fmt_tenths(char *b, int n, const char *pre, int tenths)
{
    int a = tenths < 0 ? -tenths : tenths;
    e_snprintf(b, n, "%s%s%d.%d", pre, tenths < 0 ? "-" : "", a / 10, a % 10);
}

/* fixed-rate number updates; strings rebuilt only on change */
static void numbers_update(const HudState *hs)
{
    HNum *n;
    float dt;
    static float s_lastT = -1.0f;
    if (!s_numInit) numbers_reset();
    n = &s_num[N_SPD];
    if (hnum_update(n, hs->time, hs->speedKt)) { e_snprintf(n->str, 16, "%d", n->shown < 0 ? 0 : n->shown); n->w = hg_text_w(HF_LARGE, n->str); }
    n = &s_num[N_ALT];
    if (hnum_update(n, hs->time, hs->altFt)) { e_snprintf(n->str, 16, "%d", n->shown * 10); n->w = hg_text_w(HF_LARGE, n->str); }
    n = &s_num[N_VVI];
    if (hnum_update(n, hs->time, hs->vsFpm)) { e_snprintf(n->str, 16, "%d", n->shown * 10); n->w = hg_text_w(HF_SMALL, n->str); }
    n = &s_num[N_MACH];
    if (hnum_update(n, hs->time, hs->mach)) {
        int v = n->shown < 0 ? 0 : n->shown;
        e_snprintf(n->str, 16, "M %d.%02d", v / 100, v % 100);
        n->w = hg_text_w(HF_SMALL, n->str);
    }
    n = &s_num[N_G];
    if (hnum_update(n, hs->time, hs->g)) { fmt_tenths(n->str, 16, "G ", n->shown); n->w = hg_text_w(HF_SMALL, n->str); }
    n = &s_num[N_AOA];
    if (hnum_update(n, hs->time, hs->aoaDeg)) { fmt_tenths(n->str, 16, "@ ", n->shown); n->w = hg_text_w(HF_SMALL, n->str); }
    /* peak G: shown once above 4 g, cleared after 5 s below 2 g */
    dt = s_lastT >= 0.0f ? hs->time - s_lastT : 0.0f;
    if (dt < 0.0f || dt > 1.0f) dt = 0.0f;
    s_lastT = hs->time;
    if (hs->g > s_peak) s_peak = hs->g;
    if (hs->g < 2.0f) s_peakLow += dt; else s_peakLow = 0.0f;
    if (s_peakLow > 5.0f) s_peak = hs->g;
    {
        int p = s_peak > 4.0f ? qround(s_peak * 10.0f) : 0;
        if (p != s_peakShown) {
            s_peakShown = p;
            if (p) fmt_tenths(s_peakStr, (int)sizeof(s_peakStr), "", p); else s_peakStr[0] = 0;
            s_peakW = hg_text_w(HF_SMALL, s_peakStr);
        }
    }
}

/* ================================================================ messages */
typedef struct HMsg { char text[40]; float secs; } HMsg;
static HMsg  s_msg[4];
static int   s_nmsg;
static float s_msgStart = -1.0f;

void hud_message(const char *text, float secs)
{
    if (!text) return;
    if (s_nmsg >= 4) { memmove(&s_msg[1], &s_msg[2], 2 * sizeof(HMsg)); s_nmsg = 3; }
    e_strlcpy(s_msg[s_nmsg].text, text, (int)sizeof(s_msg[0].text));
    s_msg[s_nmsg].secs = secs > 0.1f ? secs : 0.1f;
    s_nmsg++;
    log_printf(LOG_INFO, "hud: message \"%s\" secs=%.1f queued=%d", text, (double)secs, s_nmsg);
}

static const char *message_current(float t)
{
    if (s_nmsg <= 0) return NULL;
    if (s_msgStart < 0.0f || t < s_msgStart) s_msgStart = t;
    if (t - s_msgStart >= s_msg[0].secs) {
        memmove(&s_msg[0], &s_msg[1], 3 * sizeof(HMsg));
        s_nmsg--;
        s_msgStart = t;
        if (s_nmsg <= 0) { s_msgStart = -1.0f; return NULL; }
    }
    return s_msg[0].text;
}

/* ================================================================ state, extras, projection */
static u32   s_xWarn; static float s_xClock = -1.0f; static char s_xWeapon[24]; static int s_xFlares = -1;
static float s_ms;
static int   s_hudQuads, s_hudDraws;

void hud_set_extras(u32 warn, float clockSec, const char *weapon, int flares)
{
    s_xWarn = warn; s_xClock = clockSec; s_xFlares = flares;
    e_strlcpy(s_xWeapon, weapon ? weapon : "", (int)sizeof(s_xWeapon));
}

void hud_reset(void)
{
    s_nmsg = 0; s_msgStart = -1.0f;
    numbers_reset();
}

float hud_ms_last(void) { return s_ms; }

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

void hud_state_fill(HudState *hs, const Game *g, const Camera *c, float alpha)
{
    const Aircraft *a = &g->ac[PLAYER_ID];
    const FlightState *p = &a->prev, *f = &a->fs;
    Quat qp, qn;
    Vec3 r;
    float aoa;
    memset(hs, 0, sizeof(*hs));
    hs->view = c ? c->mode : VIEW_COCKPIT;
    hs->time = ((float)g->tick + alpha) / (float)SIM_HZ;
    if (c) { hs->viewM = c->view; hs->projM = c->proj; hs->eye = c->eye; }
    else { m4_ident(&hs->viewM); m4_ident(&hs->projM); }
    hs->pos = v3_lerp(p->pos, f->pos, alpha);
    qp = q_slerp(p->qPath, f->qPath, alpha);
    aoa = lerpf(p->alpha, f->alpha, alpha);
    qn = q_mul(qp, q_axis_angle(v3(1.0f, 0.0f, 0.0f), -aoa));
    hs->velDir = q_rotate(qp, v3(0.0f, 0.0f, 1.0f));
    hs->upDir = q_rotate(qp, v3(0.0f, 1.0f, 0.0f));
    hs->noseDir = q_rotate(qn, v3(0.0f, 0.0f, 1.0f));
    r = q_rotate(qp, v3(1.0f, 0.0f, 0.0f));
    hs->speedKt = lerpf(p->V, f->V, alpha) * MS2KT;
    hs->altFt = hs->pos.y * M2FT;
    hs->vsFpm = lerpf(p->vs, f->vs, alpha) * M2FT * 60.0f;
    hs->mach = lerpf(p->mach, f->mach, alpha);
    hs->g = lerpf(p->n, f->n, alpha);
    hs->aoaDeg = E_RAD2DEG(aoa);
    if (hs->velDir.x * hs->velDir.x + hs->velDir.z * hs->velDir.z > 1e-6f)
        hs->headingDeg = E_RAD2DEG((float)atan2(hs->velDir.x, hs->velDir.z));
    else hs->headingDeg = E_RAD2DEG(f->heading);
    if (hs->headingDeg < 0.0f) hs->headingDeg += 360.0f;
    hs->bankDeg = E_RAD2DEG((float)atan2(-r.y, hs->upDir.y));
    hs->onGround = f->onGround;
    hs->warn = s_xWarn;
    hs->clockSec = s_xClock;
    e_strlcpy(hs->weapon, s_xWeapon, (int)sizeof(hs->weapon));
    hs->flares = s_xFlares;
}

static int s_W = 640, s_H = 480;

int hud_project_dir(const HudState *hs, Vec3 d, float *sx, float *sy)
{
    Vec3 c = m4_mul_dir(&hs->viewM, d);
    if (c.z <= 1e-4f) return 0;
    *sx = (float)s_W * 0.5f + c.x / c.z * hs->projM.m[0][0] * (float)s_W * 0.5f;
    *sy = (float)s_H * 0.5f - c.y / c.z * hs->projM.m[1][1] * (float)s_H * 0.5f;
    return 1;
}

int hud_project_point(const HudState *hs, Vec3 p, float *sx, float *sy)
{
    Vec3 c = m4_mul_point(&hs->viewM, p);
    if (c.z <= 0.05f) return 0;
    *sx = (float)s_W * 0.5f + c.x / c.z * hs->projM.m[0][0] * (float)s_W * 0.5f;
    *sy = (float)s_H * 0.5f - c.y / c.z * hs->projM.m[1][1] * (float)s_H * 0.5f;
    return 1;
}

/* ================================================================ debug record (tests) */
enum { DBG_MAXR = 48 };
typedef struct HudDbg {
    float vvx, vvy, vvTx, vvTy; int vvClamped, vvDrawn;
    int nr; float rTh[DBG_MAXR], rX[DBG_MAXR], rY[DBG_MAXR];
    int spdTextX, spdTextY, altTextX, altTextY;
    int spdBox[4], altBox[4];
    int conformalOnly;
} HudDbg;
static HudDbg s_dbg;

/* ================================================================ boxes */
typedef struct BoxR { int x0, y0, x1, y1; } BoxR;

static void box_geom_m(BoxR *b, float k, int cap, int adv, int gap, int stroke, int digits, float xv, int rightEdge,
                       float ycv)
{
    int padX = e_ftoi(s_padX * k), padY = e_ftoi(s_padY * k), w, h, yc;
    if (padX < 2) padX = 2;
    if (padY < 2) padY = 2;
    w = digits * adv - gap + 2 * padX + 2 * stroke;
    h = cap + 2 * padY + 2 * stroke;
    if (rightEdge) { b->x1 = e_ftoi(xv * k); b->x0 = b->x1 - w; }
    else { b->x0 = e_ftoi(xv * k); b->x1 = b->x0 + w; }
    yc = e_ftoi(ycv * k);
    b->y0 = yc - h / 2;
    b->y1 = b->y0 + h;
}

static void box_geom(BoxR *b, int digits, float xv, int rightEdge, float ycv)
{
    box_geom_m(b, hg_k(), hg_cap(HF_LARGE), hg_adv(HF_LARGE), hg_gap(HF_LARGE), hg_stroke(), digits, xv, rightEdge, ycv);
}

/* box outline + the number centred horizontally and vertically in the interior (A4b recipe) */
static void box_number(const BoxR *b, const HNum *n, u32 col, int *tx, int *ty)
{
    int s = hg_stroke(), ix0 = b->x0 + s, iy0 = b->y0 + s, iw = b->x1 - b->x0 - 2 * s, ih = b->y1 - b->y0 - 2 * s;
    int X = ix0 + (iw - n->w) / 2, Y = iy0 + (ih - hg_cap(HF_LARGE)) / 2;
    hg_box(b->x0, b->y0, b->x1, b->y1, col);
    if (n->str[0]) hg_text(X, Y, HF_LARGE, col, 0, n->str);
    if (tx) *tx = X;
    if (ty) *ty = Y;
}

/* ================================================================ elements */
static int flash_on(float t, float hz) { return (float)fmod((double)t * hz, 1.0) < 0.5f; }

static void draw_tape(const HudState *hs, float cyV, float halfV, u32 col)
{
    float k = hg_k(), cx = (float)hg_px(320.0f), base = (float)hg_px(cyV), half = halfV * k, ppd = TAPE_PXDEG * k;
    float hdg = hs->headingDeg, lo = hdg - halfV / TAPE_PXDEG, hi = hdg + halfV / TAPE_PXDEG;
    int s = hg_stroke(), d, cap = hg_cap(HF_SMALL);
    int caret = hg_sprite_id("caret");
    char buf[8];
    hg_clip(e_ftoi(cx - half), e_ftoi(base - 40.0f * k), e_ftoi(cx + half), e_ftoi(base + 8.0f * k));
    for (d = (int)floor(lo / 5.0f) * 5; d <= (int)ceil(hi / 5.0f) * 5; d += 5) {
        float xf = cx + ((float)d - hdg) * ppd, x;
        int big = (d % 10) == 0, len = e_ftoi((big ? 7.0f : 4.0f) * k);
        x = (float)(e_ftoi(xf - (float)s * 0.5f) + s / 2);      /* crisp vertical stroke */
        hg_line_ext(x, base - (float)len, x, base, 0.0f, col);
        if (big) {
            int dd = ((d % 360) + 360) % 360, w, tx;
            e_snprintf(buf, (int)sizeof(buf), "%03d", dd == 0 ? 360 : dd);
            w = hg_text_w(HF_SMALL, buf);
            tx = e_ftoi(xf) - w / 2;
            if (tx >= e_ftoi(cx - half) && tx + w <= e_ftoi(cx + half))
                hg_text(tx, e_ftoi(base) - len - e_ftoi(3.0f * k) - cap, HF_SMALL, col, 0, buf);
        }
    }
    hg_clip(0, 0, 0, 0);
    hg_sprite(caret, cx, base + (float)e_ftoi(3.0f * k), col);
}

/* reference heading for the ladder: the VV's heading (fallback at the vertical: from the lift vector) */
static float ladder_heading(const HudState *hs)
{
    float hx = hs->velDir.x, hz = hs->velDir.z;
    if (hx * hx + hz * hz < 1e-6f) {
        float sg = hs->velDir.y >= 0.0f ? 1.0f : -1.0f;
        hx = -hs->upDir.x * sg; hz = -hs->upDir.z * sg;
    }
    return (float)atan2(hx, hz);
}

/* draws the pitch ladder; chase = horizon + +-10 only */
static void draw_ladder(const HudState *hs, int chase, u32 col)
{
    float k = hg_k(), h = ladder_heading(hs), sh = (float)sin(h), ch = (float)cos(h);
    Vec3 fwd = v3(sh, 0.0f, ch), right = v3(ch, 0.0f, -sh);
    float gam = E_RAD2DEG((float)asin(E_CLAMP(hs->velDir.y, -1.0f, 1.0f)));
    int th, cap = hg_cap(HF_SMALL);
    int fx0 = hg_px(s_lay.frameX0), fy0 = hg_px(s_lay.frameY0), fx1 = hg_px(s_lay.frameX1), fy1 = hg_px(s_lay.frameY1);
    char buf[8];
    if (chase) { fx0 = e_ftoi(20.0f * k); fy0 = e_ftoi(20.0f * k); fx1 = s_W - fx0; fy1 = s_H - fy0; }
    hg_clip(fx0, fy0, fx1, fy1);
    s_dbg.nr = 0;
    for (th = -90; th <= 90; th += 5) {
        float tr = E_DEG2RAD((float)th), px, py, qx, qy, dx, dy, len, nx, ny;
        Vec3 d, d2;
        if (chase && th != 0 && th != 10 && th != -10) continue;
        if (!chase && (float)fabs((float)th - gam) > s_ladderWin) continue;
        if (th == 90 || th == -90) continue;
        d = v3(fwd.x * (float)cos(tr), (float)sin(tr), fwd.z * (float)cos(tr));
        d2 = v3_madd(d, right, 0.02f);
        if (!hud_project_dir(hs, d, &px, &py) || !hud_project_dir(hs, d2, &qx, &qy)) continue;
        dx = qx - px; dy = qy - py;
        len = (float)sqrt(dx * dx + dy * dy);
        if (len < 1e-4f) continue;
        dx /= len; dy /= len;
        nx = -dy; ny = dx;                          /* "down" in the ladder frame = towards lower pitch */
        if (s_dbg.nr < DBG_MAXR) { s_dbg.rTh[s_dbg.nr] = (float)th; s_dbg.rX[s_dbg.nr] = px; s_dbg.rY[s_dbg.nr] = py; s_dbg.nr++; }
#define LP(lx, ly) (px + dx * (lx) * k + nx * (ly) * k), (py + dy * (lx) * k + ny * (ly) * k)
        if (th == 0) {
            float o = chase ? 90.0f : 110.0f;
            hg_line(LP(-o, 0.0f), LP(-12.0f, 0.0f), col);
            hg_line(LP(12.0f, 0.0f), LP(o, 0.0f), col);
            continue;
        }
        if (th > 0) {
            hg_line(LP(-52.0f, 5.0f), LP(-52.0f, 0.0f), col);
            hg_line(LP(-52.0f, 0.0f), LP(-12.0f, 0.0f), col);
            hg_line(LP(12.0f, 0.0f), LP(52.0f, 0.0f), col);
            hg_line(LP(52.0f, 0.0f), LP(52.0f, 5.0f), col);
        } else {
            int sd, i;
            for (sd = -1; sd <= 1; sd += 2) {
                for (i = 0; i < 3; i++) {
                    float a0 = 12.0f + (float)i * 15.0f, a1 = a0 + 10.0f;
                    hg_line(LP((float)sd * a0, 0.0f), LP((float)sd * a1, 0.0f), col);
                }
                hg_line(LP((float)sd * 52.0f, 0.0f), LP((float)sd * 52.0f, -5.0f), col);
            }
        }
        /* numbers at both ends (unrotated, whole pixels), only when fully inside the clip box */
        {
            int sd;
            e_snprintf(buf, (int)sizeof(buf), "%d", th);
            for (sd = -1; sd <= 1; sd += 2) {
                float lx = (float)sd * 61.0f, cxp = px + dx * lx * k, cyp = py + dy * lx * k;
                int w = hg_text_w(HF_SMALL, buf), tx, ty;
                tx = cxp >= px ? e_ftoi(cxp) : e_ftoi(cxp) - w;   /* outward from the rung end */
                ty = e_ftoi(cyp) - cap / 2;
                if (tx < fx0 + 1 || ty < fy0 + 1 || tx + w > fx1 - 2 || ty + cap > fy1 - 2) continue;
                hg_text(tx, ty, HF_SMALL, col, 0, buf);
            }
        }
#undef LP
    }
    hg_clip(0, 0, 0, 0);
}

static void draw_vv(const HudState *hs, int chase, u32 col)
{
    float x, y, k = hg_k(), mx = 16.0f * k, my = 12.0f * k;
    float fx0 = (float)hg_px(s_lay.frameX0) + mx, fy0 = (float)hg_px(s_lay.frameY0) + my;
    float fx1 = (float)hg_px(s_lay.frameX1) - mx, fy1 = (float)hg_px(s_lay.frameY1) - my;
    int clamped = 0, vv = hg_sprite_id("vv");
    s_dbg.vvDrawn = 0;
    if (!hud_project_dir(hs, hs->velDir, &x, &y)) {
        if (chase) return;
        x = (float)hg_px(s_lay.wlX); y = fy1; clamped = 1;     /* behind: pin at the bottom */
    }
    s_dbg.vvTx = x; s_dbg.vvTy = y;
    if (chase) {
        fx0 = 20.0f * k; fy0 = 20.0f * k; fx1 = (float)s_W - fx0; fy1 = (float)s_H - fy0;
    }
    if (x < fx0) { x = fx0; clamped = 1; }
    if (x > fx1) { x = fx1; clamped = 1; }
    if (y < fy0) { y = fy0; clamped = 1; }
    if (y > fy1) { y = fy1; clamped = 1; }
    s_dbg.vvx = (float)e_ftoi(x); s_dbg.vvy = (float)e_ftoi(y); s_dbg.vvClamped = clamped;
    if (clamped && !flash_on(hs->time, 3.0f)) return;
    hg_sprite(vv, x, y, col);
    s_dbg.vvDrawn = 1;
}

static void draw_bank(const HudState *hs, u32 col)
{
    static const float ticks[4] = { 5.0f, 15.0f, 30.0f, 45.0f };
    float k = hg_k(), cx = (float)hg_px(s_lay.wlX), cy = (float)hg_px(s_lay.wlY), r = BANK_R * k, b;
    int i, sd, ptr = hg_sprite_id("ptr_tri");
    for (i = 0; i < 4; i++) for (sd = -1; sd <= 1; sd += 2) {
        float a = E_DEG2RAD(ticks[i]) * (float)sd, sx = (float)sin(a), sy = (float)cos(a);
        float len = (i >= 2 ? 9.0f : 5.0f) * k;
        hg_line(cx + sx * r, cy + sy * r, cx + sx * (r + len), cy + sy * (r + len), col);
    }
    b = E_CLAMP(hs->bankDeg, -47.0f, 47.0f);
    b = E_DEG2RAD(b);
    hg_sprite_rot(ptr, cx + (float)sin(b) * (r - 2.0f * k), cy + (float)cos(b) * (r - 2.0f * k), -b, col);
}

static void draw_common_text(const HudState *hs, u32 col)
{
    const char *m = message_current(hs->time);
    char buf[16];
    int cap = hg_cap(HF_LARGE);
    (void)cap;
    if (m) hg_text(hg_px(320.0f), hg_px(MSG_Y), HF_LARGE, col, 1, m);
    if ((hs->warn & HUDW_RETURN)) hg_text(hg_px(320.0f), hg_px(RETURN_Y), HF_LARGE, col, 1, "RETURN TO AREA");
    if ((hs->warn & HUDW_PULLUP) && flash_on(hs->time, 4.0f))
        hg_text(hg_px(320.0f), hg_px(PULLUP_Y), HF_LARGE, col, 1, "PULL UP");
    if (hs->weapon[0]) hg_text(hg_px(320.0f), hg_px(WEAPON_Y), HF_SMALL, col, 1, hs->weapon);
    if (hs->clockSec >= 0.0f) {
        int s = (int)hs->clockSec;
        e_snprintf(buf, (int)sizeof(buf), "%02d:%02d", s / 60 % 100, s % 60);
        hg_text(hg_px(CLOCK_X), hg_px(CLOCK_Y), HF_SMALL, col, 0, buf);
    }
}

/* keep the ladder out of the boxes and their text (readability over the F/A-18's overlap).
   T18: the box and its text stack are one keep-out each (hg_exclude holds 6), so the weapon HUD's fixed texts of the
   last frame (readout block, LOCK, MISSILE, weapon line + VISUAL / chase weapon block: hudwpn_keepout) fit too.
   The flight HUD's own boxes are kept for hud_keepout (hud_wpn pushes its threat arrows off them). */
static int   s_ko[2][4], s_nko, s_koView = -1;
static float s_koTime = -1.0f;

static void ko_box(int x0, int y0, int x1, int y1)
{
    if (s_nko < 2) { s_ko[s_nko][0] = x0; s_ko[s_nko][1] = y0; s_ko[s_nko][2] = x1; s_ko[s_nko][3] = y1; s_nko++; }
    hg_exclude(x0, y0, x1, y1);
}

static void exclude_boxes(const HudState *hs, const BoxR *bs, const BoxR *ba, int cockpit)
{
    float k = hg_k();
    int p = e_ftoi(2.0f * k), capS = hg_cap(HF_SMALL), advS = hg_adv(HF_SMALL), step = e_ftoi(14.0f * k);
    int wb[4][4], nw, i;
    s_nko = 0; s_koView = hs->view; s_koTime = hs->time;
    if (cockpit) {
        int y = bs->y1 + e_ftoi(10.0f * k);
        ko_box(bs->x0 - p, bs->y0 - p, E_MAXF(bs->x1 + p, bs->x0 + 7 * advS + p), y + 3 * step + capS + p);   /* speed + AOA/M/G/peak */
        ko_box(E_MINF(ba->x0 - p, ba->x1 - 7 * advS - p), ba->y0 - e_ftoi(4.0f * k) - capS - p, ba->x1 + p, ba->y1 + p);   /* VVI + alt */
    } else {
        ko_box(bs->x0 - p, bs->y0 - p, E_MAXF(bs->x1 + p, bs->x0 + 7 * advS + p), bs->y1 + e_ftoi(26.0f * k) + capS);   /* speed + G (Mach) */
        ko_box(ba->x0 - p, ba->y0 - p, ba->x1 + p, ba->y1 + p);
    }
    nw = hudwpn_keepout(wb, 4, hs->view);
    for (i = 0; i < nw; i++) hg_exclude(wb[i][0] - p, wb[i][1] - p, wb[i][2] + p, wb[i][3] + p);
}

int hud_keepout(int (*box)[4], int maxBoxes, int view, float time)
{
    int i, n = 0;
    if (view != s_koView || time != s_koTime) return 0;
    for (i = 0; i < s_nko && n < maxBoxes; i++, n++) { box[n][0] = s_ko[i][0]; box[n][1] = s_ko[i][1]; box[n][2] = s_ko[i][2]; box[n][3] = s_ko[i][3]; }
    return n;
}

static void draw_cockpit(const HudState *hs, u32 col)
{
    BoxR bs, ba;
    float k = hg_k();
    int x, y, step = e_ftoi(14.0f * k), capS = hg_cap(HF_SMALL);
    box_geom(&bs, 4, s_lay.spdX1, 1, s_lay.spdYc);
    box_geom(&ba, 5, s_lay.altX0, 0, s_lay.altYc);
    exclude_boxes(hs, &bs, &ba, 1);
    draw_ladder(hs, 0, col);
    hg_exclude_clear();
    draw_vv(hs, 0, col);
    if (s_dbg.conformalOnly) return;
    draw_tape(hs, TAPE_Y, TAPE_HALF, col);
    hg_sprite(hg_sprite_id("waterline"), (float)hg_px(s_lay.wlX), (float)hg_px(s_lay.wlY), col);
    box_number(&bs, &s_num[N_SPD], col, &s_dbg.spdTextX, &s_dbg.spdTextY);
    box_number(&ba, &s_num[N_ALT], col, &s_dbg.altTextX, &s_dbg.altTextY);
    s_dbg.spdBox[0] = bs.x0; s_dbg.spdBox[1] = bs.y0; s_dbg.spdBox[2] = bs.x1; s_dbg.spdBox[3] = bs.y1;
    s_dbg.altBox[0] = ba.x0; s_dbg.altBox[1] = ba.y0; s_dbg.altBox[2] = ba.x1; s_dbg.altBox[3] = ba.y1;
    /* VVI right-aligned above the altitude box */
    hg_text(ba.x1, ba.y0 - e_ftoi(4.0f * k) - capS, HF_SMALL, col, 2, s_num[N_VVI].str);
    /* AOA / Mach / G / peak G stack under the speed box */
    x = bs.x0;
    y = bs.y1 + e_ftoi(10.0f * k);
    hg_text(x, y, HF_SMALL, col, 0, s_num[N_AOA].str);
    hg_text(x, y + step, HF_SMALL, col, 0, s_num[N_MACH].str);
    hg_text(x, y + 2 * step, HF_SMALL, col, 0, s_num[N_G].str);
    if (s_peakStr[0]) hg_text(x + hg_adv(HF_SMALL) * 2, y + 3 * step, HF_SMALL, col, 0, s_peakStr);
    draw_bank(hs, col);
    draw_common_text(hs, col);
}

static void draw_chase(const HudState *hs, u32 col)
{
    BoxR bs, ba;
    float k = hg_k(), x, y;
    u32 half = hg_col(HUD_GREEN, s_bright * s_chaseAlpha);
    int capS = hg_cap(HF_SMALL);
    box_geom(&bs, 4, s_lay.chSpdX1, 1, s_lay.chYc);
    box_geom(&ba, 5, s_lay.chAltX0, 0, s_lay.chYc);
    exclude_boxes(hs, &bs, &ba, 0);
    draw_ladder(hs, 1, half);
    hg_exclude_clear();
    draw_vv(hs, 1, col);
    if (s_dbg.conformalOnly) return;
    if (hud_project_point(hs, v3_madd(hs->pos, hs->noseDir, 1000.0f), &x, &y) &&
        x > 0.0f && y > 0.0f && x < (float)s_W && y < (float)s_H)
        hg_sprite(hg_sprite_id("aim_cross"), x, y, col);
    box_number(&bs, &s_num[N_SPD], col, &s_dbg.spdTextX, &s_dbg.spdTextY);
    box_number(&ba, &s_num[N_ALT], col, &s_dbg.altTextX, &s_dbg.altTextY);
    s_dbg.spdBox[0] = bs.x0; s_dbg.spdBox[1] = bs.y0; s_dbg.spdBox[2] = bs.x1; s_dbg.spdBox[3] = bs.y1;
    s_dbg.altBox[0] = ba.x0; s_dbg.altBox[1] = ba.y0; s_dbg.altBox[2] = ba.x1; s_dbg.altBox[3] = ba.y1;
    hg_text(bs.x0, bs.y1 + e_ftoi(6.0f * k), HF_SMALL, col, 0, s_num[N_G].str);
    if (s_chaseMach) hg_text(bs.x0, bs.y1 + e_ftoi(6.0f * k) + e_ftoi(14.0f * k), HF_SMALL, col, 0, s_num[N_MACH].str);
    if (s_chaseHeading) draw_tape(hs, 34.0f, 60.0f, col);
    (void)capS;
    draw_common_text(hs, col);
}

void hud_draw_state(const HudState *hs)
{
    s64 t0 = plat_qpc();
    u32 col;
    r_screen_size(&s_W, &s_H);
    if (s_W <= 0 || s_H <= 0) { s_W = 640; s_H = 480; }
    numbers_update(hs);
    col = hg_col(HUD_GREEN, s_bright);
    hg_begin();
    if (hs->view == VIEW_CHASE) draw_chase(hs, col);
    else draw_cockpit(hs, col);
    hg_end();
    s_hudQuads = hg_quads_last();
    s_hudDraws = hg_draws_last();
    s_ms = (float)((double)(plat_qpc() - t0) * 1000.0 / (double)plat_qpf());
}

void hud_draw(const Game *g, const Camera *c, float alpha)
{
    HudState hs;
    hud_state_fill(&hs, g, c, alpha);
    hud_draw_state(&hs);
}

int hud_init(void)
{
    hud_reset();
    hud_layout();
    log_printf(LOG_INFO, "hud: init set=%d k=%.4f aseR=%.0f", hg_set_lines(), (double)hg_k(), (double)s_aseR);
    return 1;
}

void hud_free(void) {}

/* ================================================================ -hudtest (DESIGN 10.8) */
static int s_fail;

static void TFAIL(const char *fmt, ...)            /* logs one "hud: FAIL ..." line and counts it */
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    e_vsnprintf(buf, (int)sizeof(buf), fmt, ap);
    va_end(ap);
    log_printf(LOG_INFO, "%s", buf);
    s_fail++;
}

/* synthetic cockpit/chase camera like camera.cpp: 65 deg horizontal FOV at 4:3 */
static void test_state(HudState *hs, int view, float hdgDeg, float pitchDeg, float bankDeg, float aoaDeg, float altFt)
{
    Quat qn = flight_path_quat(E_DEG2RAD(hdgDeg), E_DEG2RAD(pitchDeg), E_DEG2RAD(bankDeg));
    Quat qp = q_mul(qn, q_axis_angle(v3(1.0f, 0.0f, 0.0f), E_DEG2RAD(aoaDeg)));
    Vec3 nf = q_rotate(qn, v3(0.0f, 0.0f, 1.0f)), nu = q_rotate(qn, v3(0.0f, 1.0f, 0.0f)), eye, at, r;
    float t = hs->time;
    memset(hs, 0, sizeof(*hs));
    hs->time = t;
    hs->view = view;
    hs->pos = v3(0.0f, altFt * FT2M, 0.0f);
    hs->velDir = q_rotate(qp, v3(0.0f, 0.0f, 1.0f));
    hs->upDir = q_rotate(qp, v3(0.0f, 1.0f, 0.0f));
    hs->noseDir = nf;
    hs->aoaDeg = aoaDeg;
    hs->altFt = altFt;
    hs->headingDeg = E_RAD2DEG((float)atan2(hs->velDir.x, hs->velDir.z));
    if (hs->headingDeg < 0.0f) hs->headingDeg += 360.0f;
    r = q_rotate(qp, v3(1.0f, 0.0f, 0.0f));
    hs->bankDeg = E_RAD2DEG((float)atan2(-r.y, hs->upDir.y));
    hs->clockSec = -1.0f;
    hs->flares = -1;
    if (view == VIEW_CHASE) {
        eye = v3_add(v3_madd(hs->pos, nf, -24.0f), v3_scale(nu, 5.5f));
        at = v3_madd(hs->pos, nf, 40.0f);
    } else {
        eye = hs->pos;
        at = v3_add(eye, nf);
    }
    hs->eye = eye;
    m4_look_lh(&hs->viewM, eye, at, nu);
    m4_persp_lh(&hs->projM, 2.0f * (float)atan(tan(E_DEG2RAD(65.0f) * 0.5f) * 0.75f), 4.0f / 3.0f, 1.0f, 18000.0f);
}

/* ---- static test background: sky, haze band, sea/terrain below the true horizon, a cloud-white patch */
typedef struct BP { float x, y, g; } BP;
static int clip_lin(const BP *in, int n, BP *out, int keepBelow, float lim)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const BP *a = &in[i], *b = &in[(i + 1) % n];
        int ia = keepBelow ? a->g <= lim : a->g >= lim, ib = keepBelow ? b->g <= lim : b->g >= lim;
        if (ia) out[m++] = *a;
        if (ia != ib && m < 15) {
            float t = (lim - a->g) / (b->g - a->g);
            out[m].x = a->x + (b->x - a->x) * t; out[m].y = a->y + (b->y - a->y) * t; out[m].g = lim;
            m++;
        }
    }
    return m;
}

static void bg_poly(const BP *p, int n, u32 col)
{
    int i;
    for (i = 1; i + 1 < n; i += 2) {
        RVert2D q[4];
        const BP *v[4];
        int j;
        v[0] = &p[0]; v[1] = &p[i]; v[2] = &p[i + 1]; v[3] = i + 2 < n ? &p[i + 2] : &p[i + 1];
        for (j = 0; j < 4; j++) { q[j].x = v[j]->x; q[j].y = v[j]->y; q[j].u = 0.0f; q[j].v = 0.0f; q[j].color = col; }
        ui_quad4_px(0, q);
    }
}

/* ray y-component at a screen pixel (linear in x, y): ground where < 0 */
static float ray_y(const HudState *hs, float x, float y)
{
    float sx = (x - (float)s_W * 0.5f) / (hs->projM.m[0][0] * (float)s_W * 0.5f);
    float sy = -(y - (float)s_H * 0.5f) / (hs->projM.m[1][1] * (float)s_H * 0.5f);
    /* view matrix columns = camera right, up, forward in world */
    return hs->viewM.m[1][0] * sx + hs->viewM.m[1][1] * sy + hs->viewM.m[1][2];
}

static void draw_bg(const HudState *hs, int black)
{
    BP r[4], a[16], b[16];
    int n, i;
    if (black) return;
    r[0].x = 0.0f; r[0].y = 0.0f; r[1].x = (float)s_W; r[1].y = 0.0f;
    r[2].x = (float)s_W; r[2].y = (float)s_H; r[3].x = 0.0f; r[3].y = (float)s_H;
    for (i = 0; i < 4; i++) r[i].g = ray_y(hs, r[i].x, r[i].y);
    ui_begin(640, 480);
    ui_set_blend(RB_BLEND);
    /* haze band just above the horizon */
    n = clip_lin(r, 4, a, 1, 0.12f);
    if (n >= 3) { n = clip_lin(a, n, b, 0, 0.0f); if (n >= 3) bg_poly(b, n, 0xFFB8CCE0); }
    /* sea */
    n = clip_lin(r, 4, a, 1, 0.0f);
    if (n >= 3) bg_poly(a, n, 0xFF2D5876);
    /* nearer ground (terrain green-brown) */
    n = clip_lin(r, 4, a, 1, -0.10f);
    if (n >= 3) bg_poly(a, n, 0xFF4E6B3A);
    ui_end();
    /* cloud-white patch behind the altitude box / VVI (screen fixed: readability check) */
    ui_begin(640, 480);
    ui_rect(372.0f, 150.0f, 150.0f, 70.0f, 0xFFF4F6F8);
    ui_rect(400.0f, 212.0f, 110.0f, 40.0f, 0xFFE8ECF0);
    ui_end();
}

/* dummy launch-zone circle (T16 draws the real one): radius aseR, Rmax/Rne/Rmin ticks, range pointer */
static void draw_dummy_ase(void)
{
    float k = hg_k(), cx = (float)hg_px(s_lay.aseX), cy = (float)hg_px(s_lay.aseY), R = s_aseR * k;
    float px = cx, py = cy - R;
    u32 col = hg_col(HUD_GREEN, s_bright);
    int i;
    hg_begin();
    for (i = 1; i <= 40; i++) {
        float a = (float)i * (2.0f * E_PI / 40.0f), nx = cx + (float)sin(a) * R, ny = cy - (float)cos(a) * R;
        hg_line(px, py, nx, ny, col);
        px = nx; py = ny;
    }
    {
        float rmax = E_DEG2RAD(250.0f), rne = E_DEG2RAD(130.0f), rmin = E_DEG2RAD(25.0f), rp = E_DEG2RAD(200.0f);
        float o = s_aseOut * k;
        hg_line(cx + (float)sin(rmax) * R, cy - (float)cos(rmax) * R, cx + (float)sin(rmax) * (R + o), cy - (float)cos(rmax) * (R + o), col);
        hg_line(cx + (float)sin(rne - 0.03f) * R, cy - (float)cos(rne - 0.03f) * R, cx + (float)sin(rne - 0.03f) * (R + o * 0.75f), cy - (float)cos(rne - 0.03f) * (R + o * 0.75f), col);
        hg_line(cx + (float)sin(rne + 0.03f) * R, cy - (float)cos(rne + 0.03f) * R, cx + (float)sin(rne + 0.03f) * (R + o * 0.75f), cy - (float)cos(rne + 0.03f) * (R + o * 0.75f), col);
        hg_line(cx + (float)sin(rmin) * R, cy - (float)cos(rmin) * R, cx + (float)sin(rmin) * (R - o * 0.75f), cy - (float)cos(rmin) * (R - o * 0.75f), col);
        hg_sprite_rot(hg_sprite_id("ptr_tri"), cx + (float)sin(rp) * (R + 1.0f), cy - (float)cos(rp) * (R + 1.0f), rp + E_PI, col);
    }
    hg_end();
}

static int frame_begin(const HudState *hs, int black)
{
    if (!plat_pump()) return 0;
    if (!r_begin_frame(black ? 0x000000 : 0x6A8FC0)) return 0;
    r_screen_size(&s_W, &s_H);
    draw_bg(hs, black);
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

static void shots_dir(void)
{
    char path[E_PATH];
    base_path(path, (int)sizeof(path), "shots");
    CreateDirectoryA(path, NULL);
}

static void demo_contacts(const HudState *hs)
{
    MapContact c;
    float h = E_DEG2RAD(hs->headingDeg), sh = (float)sin(h), ch = (float)cos(h);
    c.flags = 0;
    c.kind = MAPC_HOSTILE; c.x = hs->pos.x + sh * 22000.0f + ch * 6000.0f; c.z = hs->pos.z + ch * 22000.0f - sh * 6000.0f;
    c.headingRad = h + E_PI; c.flags = MAPF_DESIGNATED; map_draw_contact(&c);
    c.kind = MAPC_UNKNOWN; c.x = hs->pos.x + sh * 28000.0f - ch * 14000.0f; c.z = hs->pos.z + ch * 28000.0f + sh * 14000.0f;
    c.headingRad = h + 2.0f; c.flags = 0; map_draw_contact(&c);
    c.kind = MAPC_HOSTILE; c.x = hs->pos.x + sh * 12000.0f - ch * 25000.0f; c.z = hs->pos.z + ch * 12000.0f + sh * 25000.0f;
    c.headingRad = h + 1.0f; c.flags = MAPF_MEMORY; map_draw_contact(&c);
    c.kind = MAPC_MSL_OWN; c.x = hs->pos.x + sh * 9000.0f + ch * 2000.0f; c.z = hs->pos.z + ch * 9000.0f - sh * 2000.0f;
    c.flags = 0; map_draw_contact(&c);
}

/* settle the number caches at a fixed state (rates + hysteresis need a few updates) */
static void settle(HudState *hs)
{
    int i;
    for (i = 0; i < 40; i++) { hs->time += 1.0f / 60.0f; numbers_update(hs); }
}

/* ---- layout: ASE gap for every set, views screenshots */
static int box_gap_px(const BoxR *b, float k, int *gapOut)
{
    float cx = (float)e_ftoi(s_lay.aseX * k), cy = (float)e_ftoi(s_lay.aseY * k), ro = (s_aseR + s_aseOut) * k;
    float dx = E_MAXF(E_MAXF((float)b->x0 - cx, 0.0f), cx - (float)b->x1);
    float dy = E_MAXF(E_MAXF((float)b->y0 - cy, 0.0f), cy - (float)b->y1);
    float gap = (float)sqrt(dx * dx + dy * dy) - ro;
    *gapOut = (int)floor(gap);
    return *gapOut;
}

static int test_layout_gaps(void)
{
    static const int sets[4] = { 480, 600, 768, 1050 };
    int i, fails = 0;
    for (i = 0; i < 4; i++) {
        int cap, adv, gap, stroke, gs, ga, minGap;
        float k = (float)sets[i] / 480.0f;
        BoxR bs, ba;
        if (!hfont_set_metrics(sets[i], HF_LARGE, &cap, &adv, &gap, &stroke)) {
            log_printf(LOG_INFO, "hud: layout set=%d metrics missing", sets[i]);
            continue;
        }
        box_geom_m(&bs, k, cap, adv, gap, stroke, 4, s_lay.spdX1, 1, s_lay.spdYc);
        box_geom_m(&ba, k, cap, adv, gap, stroke, 5, s_lay.altX0, 0, s_lay.altYc);
        box_gap_px(&bs, k, &gs);
        box_gap_px(&ba, k, &ga);
        minGap = (int)ceil(s_aseGap600 * (float)sets[i] / 600.0f);
        log_printf(LOG_INFO, "hud: layout set=%d aseR=%.1fpx outer=%.1fpx spdBox=%d,%d-%d,%d altBox=%d,%d-%d,%d gapSpd=%d gapAlt=%d min=%d %s",
                   sets[i], (double)(s_aseR * k), (double)((s_aseR + s_aseOut) * k), bs.x0, bs.y0, bs.x1, bs.y1, ba.x0, ba.y0,
                   ba.x1, ba.y1, gs, ga, minGap, (gs >= minGap && ga >= minGap) ? "ok" : "FAIL");
        if (gs < minGap || ga < minGap) fails++;
        /* chase boxes stay on screen and clear of the centre jet area */
        box_geom_m(&bs, k, cap, adv, gap, stroke, 4, s_lay.chSpdX1, 1, s_lay.chYc);
        box_geom_m(&ba, k, cap, adv, gap, stroke, 5, s_lay.chAltX0, 0, s_lay.chYc);
        if (bs.x0 < 0 || ba.x1 > e_ftoi(640.0f * k)) {
            log_printf(LOG_INFO, "hud: layout set=%d chase boxes off screen FAIL", sets[i]);
            fails++;
        }
        {   /* T16b: the chase circle (centre anywhere in its clamp range chAseY0..chAseY1) keeps the gap too */
            int gmin = 1 << 30, j;
            for (j = 0; j <= 16; j++) {
                float yv = s_lay.chAseY0 + (s_lay.chAseY1 - s_lay.chAseY0) * (float)j / 16.0f;
                float cx = (float)e_ftoi(s_lay.chAseX * k), cy = (float)e_ftoi(yv * k), ro = (s_aseR + s_aseOut) * k, g1, g2;
                float dx = E_MAXF(E_MAXF((float)bs.x0 - cx, 0.0f), cx - (float)bs.x1), dy = E_MAXF(E_MAXF((float)bs.y0 - cy, 0.0f), cy - (float)bs.y1);
                g1 = (float)sqrt(dx * dx + dy * dy) - ro;
                dx = E_MAXF(E_MAXF((float)ba.x0 - cx, 0.0f), cx - (float)ba.x1); dy = E_MAXF(E_MAXF((float)ba.y0 - cy, 0.0f), cy - (float)ba.y1);
                g2 = (float)sqrt(dx * dx + dy * dy) - ro;
                if ((int)floor(g1) < gmin) gmin = (int)floor(g1);
                if ((int)floor(g2) < gmin) gmin = (int)floor(g2);
            }
            log_printf(LOG_INFO, "hud: layout set=%d chase circle x=%.0f y=%.0f..%.0f (virtual) outer=%.1fpx spdBox=%d,%d-%d,%d altBox=%d,%d-%d,%d gap min=%d min=%d %s",
                       sets[i], (double)s_lay.chAseX, (double)s_lay.chAseY0, (double)s_lay.chAseY1, (double)((s_aseR + s_aseOut) * k),
                       bs.x0, bs.y0, bs.x1, bs.y1, ba.x0, ba.y0, ba.x1, ba.y1, gmin, minGap, gmin >= minGap ? "ok" : "FAIL");
            if (gmin < minGap) fails++;
        }
    }
    return fails;
}

typedef struct ViewCase { const char *name; float pitch, bank, aoa, kt, g, altFt, vsFpm, mach; } ViewCase;
static const ViewCase s_cases[5] = {
    { "level",    2.6f,   0.0f,  2.6f,  420.0f, 1.0f, 12450.0f,    0.0f, 0.68f },
    { "climb60",  32.0f,  60.0f, 2.0f,  480.0f, 2.0f, 18230.0f, 25400.0f, 0.79f },
    { "inverted", -4.0f, 180.0f, 1.0f,  510.0f, -1.0f, 9870.0f, -1800.0f, 0.80f },
    { "g9_350",   8.0f,  80.0f, 12.0f,  350.0f, 9.0f,  6520.0f,  2100.0f, 0.55f },
    { "aoa15",   12.0f,  10.0f, 15.0f,  180.0f, 3.5f,  4210.0f,  -600.0f, 0.28f },
};

static int test_layout(void)
{
    int v, c, fails = 0;
    char shot[64];
    fails += test_layout_gaps();
    shots_dir();
    for (v = 0; v < 2; v++) for (c = 0; c < 5; c++) {
        HudState hs;
        const ViewCase *vc = &s_cases[c];
        int f;
        hud_reset();
        hs.time = 10.0f;
        for (f = 0; f < 8; f++) {
            test_state(&hs, v ? VIEW_CHASE : VIEW_COCKPIT, 40.0f, vc->pitch, vc->bank, vc->aoa, vc->altFt);
            hs.time = 10.0f + (float)f / 60.0f;
            hs.speedKt = vc->kt; hs.g = vc->g; hs.vsFpm = vc->vsFpm; hs.mach = vc->mach;
            hs.weapon[0] = 0;
            if (c == 0) { e_strlcpy(hs.weapon, "AIM-120 16", (int)sizeof(hs.weapon)); hs.clockSec = 207.0f; }
            hs.flares = 40;
            if (f == 0) {
                settle(&hs);
                if (c == 3) { HudState t = hs; t.g = 9.4f; numbers_update(&t); }   /* peak G shown */
                if (c == 1) hud_message("YOU HAVE CONTROL", 30.0f);
                if (c == 4) hs.warn = HUDW_PULLUP;
            }
            if (c == 4) hs.warn = HUDW_PULLUP;
            /* frame 0 = flash phase on (time 10.0 + settle); use a phase-on frame for the shot */
            if (!frame_begin(&hs, 0)) return fails + 1;
            hud_draw_state(&hs);
            if (v == 0 && c == 0) draw_dummy_ase();
            demo_contacts(&hs);
            map_draw_state(&hs, NULL);
            e_snprintf(shot, (int)sizeof(shot), "shots\\hud_%s_%s_%d.bmp", v ? "chase" : "cockpit", vc->name, s_H);
            frame_end(f == 1 ? shot : NULL);
        }
        log_printf(LOG_INFO, "hud: view=%s case=%s spd=\"%s\" alt=\"%s\" vvi=\"%s\" g=\"%s\" mach=\"%s\" aoa=\"%s\" peak=\"%s\" vv=%.0f,%.0f clamped=%d quads=%d draws=%d",
                   v ? "chase" : "cockpit", vc->name, s_num[N_SPD].str, s_num[N_ALT].str, s_num[N_VVI].str, s_num[N_G].str,
                   s_num[N_MACH].str, s_num[N_AOA].str, s_peakStr, (double)s_dbg.vvx, (double)s_dbg.vvy, s_dbg.vvClamped,
                   s_hudQuads, s_hudDraws);
        if (c == 4 && v == 0 && !s_dbg.vvClamped) TFAIL("hud: FAIL aoa15 VV not clamped");
        s_nmsg = 0;
    }
    /* also a shot with the ASE circle on the climbing case to show the gap while banked */
    return fails;
}

/* ---- ladder: conformal accuracy, smooth motion, frame clipping on real pixels, timing */
static void expected_rung(float pitchDeg, float bankDeg, float th, float *ex, float *ey, float f)
{
    float L = f * (float)tan(E_DEG2RAD(th - pitchDeg)), b = E_DEG2RAD(bankDeg);
    *ex = (float)s_W * 0.5f - L * (float)sin(b);
    *ey = (float)s_H * 0.5f - L * (float)cos(b);
}

static int find_rung(float th, float *x, float *y)
{
    int i;
    for (i = 0; i < s_dbg.nr; i++) if (s_dbg.rTh[i] == th) { *x = s_dbg.rX[i]; *y = s_dbg.rY[i]; return 1; }
    return 0;
}

static int readback_outside(const char *path, int fx0, int fy0, int fx1, int fy1, int *lit)
{
    EImage im;
    int x, y, out = 0, in = 0;
    char full[E_PATH];
    base_path(full, (int)sizeof(full), path);
    if (!img_load(&im, full, 0)) return -1;
    for (y = 0; y < im.h; y++) for (x = 0; x < im.w; x++) {
        u32 p = im.pix[0][y * im.w + x];
        int m = (int)E_MAXF(E_MAXF((float)((p >> 16) & 0xFF), (float)((p >> 8) & 0xFF)), (float)(p & 0xFF));
        if (m < 40) continue;
        if (x < fx0 || x >= fx1 || y < fy0 || y >= fy1) out++; else in++;
    }
    img_free(&im);
    *lit = in;
    return out;
}

static void perf_run(int view, float *hudMs, float *mapMs, float *maxMs, int *quads)
{
    HudState hs;
    int f, n = 0, q = 0;
    double sh = 0.0, sm = 0.0, mx = 0.0;
    hud_reset();
    hs.time = 0.0f;
    hud_message("YOU HAVE CONTROL", 100.0f);
    for (f = 0; f < 150; f++) {
        float t = (float)f / 30.0f;
        test_state(&hs, view, 40.0f + t * 3.0f, 6.0f + 4.0f * (float)sin(t), 25.0f * (float)sin(t * 0.7f), 3.0f, 15300.0f + t * 40.0f);
        hs.time = 20.0f + t;
        hs.speedKt = 455.0f + t; hs.g = 1.6f; hs.vsFpm = 1240.0f; hs.mach = 0.74f;
        e_strlcpy(hs.weapon, "AIM-120 16", (int)sizeof(hs.weapon));
        hs.clockSec = 300.0f + t;
        hs.flares = 40;
        if (!frame_begin(&hs, 0)) break;
        ui_quad_count(1);
        hud_draw_state(&hs);
        demo_contacts(&hs);
        map_draw_state(&hs, NULL);
        if (f >= 30) {
            double tot = (double)hud_ms_last() + (double)map_ms_last();
            sh += hud_ms_last(); sm += map_ms_last(); n++;
            if (tot > mx) mx = tot;
            q = ui_quad_count(0);
        }
        frame_end(NULL);
    }
    hud_reset();
    *hudMs = n ? (float)(sh / n) : 0.0f;
    *mapMs = n ? (float)(sm / n) : 0.0f;
    *maxMs = (float)mx;
    *quads = q;
}

static int test_ladder(void)
{
    HudState hs;
    float f = 0.0f, prevMx = 0.0f, prevMy = 0.0f, prevEx = 0.0f, prevEy = 0.0f;
    float maxErr = 0.0f, maxStep = 0.0f, maxVv = 0.0f;
    int fr, nChecks = 0, havePrev = 0, fails = 0, sweep;
    int fx0, fy0, fx1, fy1;
    hud_reset();
    hs.time = 0.0f;
    r_screen_size(&s_W, &s_H);
    /* sweeps: A pitch -88..88 wings level, B bank 0..360 at pitch 10, C combined (no rendering needed for the
       geometry checks; every 8th frame is drawn so the run is visible) */
    for (sweep = 0; sweep < 3; sweep++) {
        int nf = sweep == 0 ? 704 : (sweep == 1 ? 480 : 600);
        havePrev = 0;
        for (fr = 0; fr < nf; fr++) {
            float t = (float)fr / 60.0f, pitch, bank, ex, ey, mx, my, th;
            if (sweep == 0) { pitch = -88.0f + t * 15.0f; bank = 0.0f; }
            else if (sweep == 1) { pitch = 10.0f; bank = t * 45.0f; }
            else { pitch = 60.0f * (float)sin(t * 0.9f); bank = t * 37.0f; }
            test_state(&hs, VIEW_COCKPIT, 75.0f, pitch, bank, 0.0f, 10000.0f);
            hs.time = (float)(sweep * 100) + t;
            hs.speedKt = 400.0f; hs.g = 1.0f; hs.mach = 0.7f;
            f = hs.projM.m[1][1] * (float)s_H * 0.5f;
            if ((fr & 7) == 0) {
                if (!frame_begin(&hs, 0)) return fails + 1;
                hud_draw_state(&hs);
                frame_end(NULL);
            } else {
                hud_draw_state(&hs);               /* outside a frame: the backend skips the draw, geometry runs */
            }
            /* VV at alpha 0 = boresight */
            {
                float dv = (float)sqrt((s_dbg.vvx - (float)s_W * 0.5f) * (s_dbg.vvx - (float)s_W * 0.5f) +
                                       (s_dbg.vvy - (float)s_H * 0.5f) * (s_dbg.vvy - (float)s_H * 0.5f));
                if (dv > maxVv) maxVv = dv;
            }
            if ((float)fabs(pitch) > 80.0f) { havePrev = 0; continue; }
            /* the rung nearest the screen centre */
            th = (float)(5 * qround(pitch / 5.0f));
            if (th >= 90.0f || th <= -90.0f || !find_rung(th, &mx, &my)) { havePrev = 0; continue; }
            expected_rung(pitch, bank, th, &ex, &ey, f);
            {
                float e = (float)sqrt((mx - ex) * (mx - ex) + (my - ey) * (my - ey));
                if (e > maxErr) maxErr = e;
                nChecks++;
                if (havePrev) {
                    float ddx = (mx - prevMx) - (ex - prevEx), ddy = (my - prevMy) - (ey - prevEy);
                    float st = (float)sqrt(ddx * ddx + ddy * ddy);
                    if (st > maxStep) maxStep = st;
                }
                prevMx = mx; prevMy = my; prevEx = ex; prevEy = ey; havePrev = 1;
            }
            if (fr % 120 == 0)
                log_printf(LOG_INFO, "hud: ladder sweep=%d t=%.2f pitch=%.1f bank=%.1f rung=%.0f at=%.2f,%.2f exp=%.2f,%.2f rungs=%d",
                           sweep, (double)t, (double)pitch, (double)bank, (double)th, (double)mx, (double)my, (double)ex,
                           (double)ey, s_dbg.nr);
        }
    }
    log_printf(LOG_INFO, "hud: ladder checks=%d maxErrPx=%.3f maxStepDevPx=%.3f maxVvPx=%.3f", nChecks, (double)maxErr,
               (double)maxStep, (double)maxVv);
    if (nChecks < 500) TFAIL("hud: FAIL ladder too few checks %d", nChecks);
    if (maxErr >= 1.0f) TFAIL("hud: FAIL ladder conformal error %.3f px", (double)maxErr);
    if (maxStep >= 1.0f) TFAIL("hud: FAIL ladder motion deviation %.3f px", (double)maxStep);
    if (maxVv > 0.75f) TFAIL("hud: FAIL VV off boresight at alpha 0 by %.3f px", (double)maxVv);

    /* nothing outside the frame: conformal layer only, over black, read back from the back buffer */
    {
        static const float att[12][3] = {
            { 0, 0, 0 }, { 10, 30, 0 }, { -20, 120, 0 }, { 45, 200, 0 }, { 70, 300, 0 }, { -70, 45, 0 },
            { 5, 89, 0 }, { 0, 180, 0 }, { 30, 330, 25 }, { -10, 0, 30 }, { 85, 0, 0 }, { 12, 60, 18 },
        };
        int i, outTot = 0, okShots = 0;
        fx0 = hg_px(s_lay.frameX0); fy0 = hg_px(s_lay.frameY0); fx1 = hg_px(s_lay.frameX1); fy1 = hg_px(s_lay.frameY1);
        s_dbg.conformalOnly = 1;
        for (i = 0; i < 12; i++) {
            int out, lit = 0, k;
            for (k = 0; k < 2; k++) {               /* VV flash: draw a phase-on frame */
                test_state(&hs, VIEW_COCKPIT, 20.0f, att[i][0], att[i][1], att[i][2], 9000.0f);
                hs.time = 500.0f + (float)i + (float)k * 0.01f;
                if (!frame_begin(&hs, 1)) { s_dbg.conformalOnly = 0; return fails + 1; }
                hud_draw_state(&hs);
                frame_end(k == 1 ? "hudtest_rb.bmp" : NULL);
            }
            out = readback_outside("hudtest_rb.bmp", fx0, fy0, fx1, fy1, &lit);
            log_printf(LOG_INFO, "hud: frameclip pitch=%.0f bank=%.0f aoa=%.0f outside=%d inside=%d vvClamped=%d",
                       (double)att[i][0], (double)att[i][1], (double)att[i][2], out, lit, s_dbg.vvClamped);
            if (out < 0) continue;
            okShots++;
            outTot += out;
            if (lit == 0) TFAIL("hud: FAIL frameclip nothing drawn at pitch=%.0f bank=%.0f", (double)att[i][0], (double)att[i][1]);
        }
        s_dbg.conformalOnly = 0;
        log_printf(LOG_INFO, "hud: frameclip shots=%d outsidePixels=%d", okShots, outTot);
        if (okShots < 12) TFAIL("hud: FAIL frameclip readback failed (%d of 12)", okShots);
        if (outTot > 0) TFAIL("hud: FAIL frameclip %d pixels outside the frame", outTot);
    }
    /* timing (DESIGN 9.3: HUD + minimap <= 1.5 ms on the VM) */
    {
        float hm, mm, mx;
        int q, v;
        for (v = 0; v < 2; v++) {
            perf_run(v ? VIEW_CHASE : VIEW_COCKPIT, &hm, &mm, &mx, &q);
            log_printf(LOG_INFO, "hud: ms=%.3f view=%s hudMs=%.3f mapMs=%.3f maxMs=%.3f quads=%d set=%d screen=%dx%d",
                       (double)(hm + mm), v ? "chase" : "cockpit", (double)hm, (double)mm, (double)mx, q, hg_set_lines(),
                       s_W, s_H);
            if (hm + mm > 1.5f) TFAIL("hud: FAIL ms %.3f > 1.5 (%s)", (double)(hm + mm), v ? "chase" : "cockpit");
        }
    }
    return fails;
}

/* ---- numbers: change rates, no flicker, strings, box fit and centring */
typedef struct NumTrack { int last, lastDir; float lastT, tChg[64]; int nChg, maxWin, flicker, init; } NumTrack;

static void track(NumTrack *tr, const HNum *n, float t)
{
    int i, inWin;
    if (!tr->init) { tr->init = 1; tr->last = n->shown; tr->lastT = t; return; }
    if (n->shown == tr->last) return;
    {
        int dir = n->shown > tr->last ? 1 : -1;
        if (tr->lastDir && dir != tr->lastDir && t - tr->lastT < 2.0f * n->period - 1e-3f) tr->flicker++;
        tr->lastDir = dir;
    }
    tr->last = n->shown;
    tr->lastT = t;
    tr->tChg[tr->nChg % 64] = t;
    tr->nChg++;
    inWin = 0;
    for (i = 0; i < 64 && i < tr->nChg; i++) if (t - tr->tChg[i] < 1.0f - 1e-4f) inWin++;
    if (inWin > tr->maxWin) tr->maxWin = inWin;
}

static int test_numbers(void)
{
    static const char *const names[N_COUNT] = { "speed", "alt", "vvi", "mach", "g", "aoa" };
    static const float rates[N_COUNT] = { 10, 10, 5, 5, 5, 5 };
    HudState hs;
    NumTrack tr[N_COUNT];
    ERand rng;
    int fr, i, fails = 0, holdChanges = 0, holdStart = -1, sawG99 = 0, saw5 = 0, saw4 = 0, boxChanged = 0;
    int box0[8], maxErrIdx = -1;
    float maxLag[N_COUNT];
    char buf[64];
    memset(tr, 0, sizeof(tr));
    memset(maxLag, 0, sizeof(maxLag));
    memset(box0, 0, sizeof(box0));
    rnd_seed(&rng, 1234u);
    hud_reset();
    hs.time = 0.0f;
    for (fr = 0; fr < 60 * 48; fr++) {
        float t = (float)fr / 60.0f, kt, alt, g, vs, aoa, mach, noise = rnd_range(&rng, -1.0f, 1.0f);
        /* 0-16 s sweeps up, 16-20 s hold (jitter), 20-36 s sweeps down, 36-48 s G/alt specials */
        if (t < 16.0f) { kt = 150.0f + t * 62.5f; alt = t * 3300.0f; g = -3.0f + t * 0.8f; aoa = -2.0f + t; }
        else if (t < 20.0f) { kt = 350.0f + 0.45f * noise; alt = 25000.0f + 4.0f * noise; g = 1.0f + 0.04f * noise; aoa = 3.0f + 0.04f * noise; }
        else if (t < 36.0f) { float u = t - 20.0f; kt = 1150.0f - u * 62.5f; alt = 52800.0f - u * 3300.0f; g = 9.9f - u * 0.8f; aoa = 14.0f - u; }
        else { kt = 480.0f; alt = t < 42.0f ? 9990.0f + (t - 36.0f) * 3.0f : 10045.0f; g = 9.9f; aoa = 12.5f; }
        if (t >= 16.0f && t < 20.0f) mach = 0.53f + 0.002f * noise; else mach = kt / 661.5f;
        vs = (t < 16.0f) ? 198000.0f / 60.0f : (t < 20.0f ? 3.0f * noise : -3300.0f);
        test_state(&hs, VIEW_COCKPIT, 90.0f, 3.0f, 0.0f, 2.0f, alt);
        hs.time = t;
        hs.speedKt = kt + ((t >= 16.0f && t < 20.0f) ? 0.0f : 0.3f * noise); hs.altFt = alt; hs.g = g; hs.vsFpm = vs; hs.aoaDeg = aoa; hs.mach = mach;
        if ((fr % 20) == 0) {                       /* draw every 20th frame (screen + layout values) */
            if (!frame_begin(&hs, 0)) return fails + 1;
            hud_draw_state(&hs);
            frame_end(fr == 60 * 44 ? "shots\\hud_numbers.bmp" : NULL);
            if (box0[2] == 0) { memcpy(box0, s_dbg.spdBox, 4 * sizeof(int)); memcpy(box0 + 4, s_dbg.altBox, 4 * sizeof(int)); }
            else if (memcmp(box0, s_dbg.spdBox, 4 * sizeof(int)) || memcmp(box0 + 4, s_dbg.altBox, 4 * sizeof(int))) boxChanged++;
            /* centring: text ink centred in the box interior (+-0.5 px), ink inside the interior */
            {
                int s = hg_stroke(), ix0 = s_dbg.altBox[0] + s, iw = s_dbg.altBox[2] - s_dbg.altBox[0] - 2 * s;
                int w = s_num[N_ALT].w, dx = (2 * s_dbg.altTextX + w) - (2 * ix0 + iw);
                int iy0 = s_dbg.altBox[1] + s, ih = s_dbg.altBox[3] - s_dbg.altBox[1] - 2 * s;
                int dy = (2 * s_dbg.altTextY + hg_cap(HF_LARGE)) - (2 * iy0 + ih);
                if (dx < -1 || dx > 1 || dy < -1 || dy > 1 || w > iw - 2) {
                    TFAIL("hud: FAIL alt centring str=%s w=%d iw=%d dx2=%d dy2=%d", s_num[N_ALT].str, w, iw, dx, dy);
                }
                ix0 = s_dbg.spdBox[0] + s; iw = s_dbg.spdBox[2] - s_dbg.spdBox[0] - 2 * s; w = s_num[N_SPD].w;
                dx = (2 * s_dbg.spdTextX + w) - (2 * ix0 + iw);
                if (dx < -1 || dx > 1 || w > iw - 2) TFAIL("hud: FAIL speed centring str=%s w=%d iw=%d dx2=%d", s_num[N_SPD].str, w, iw, dx);
            }
        } else {
            numbers_update(&hs);
        }
        for (i = 0; i < N_COUNT; i++) track(&tr[i], &s_num[i], t);
        if (t >= 16.5f && t < 20.0f) {             /* hold phase after settling: nothing may change */
            if (holdStart < 0) holdStart = s_num[N_SPD].changes + s_num[N_ALT].changes + s_num[N_G].changes + s_num[N_AOA].changes;
            holdChanges = s_num[N_SPD].changes + s_num[N_ALT].changes + s_num[N_G].changes + s_num[N_AOA].changes - holdStart;
        }
        if (!strcmp(s_num[N_G].str, "G 9.9")) sawG99 = 1;
        if (strlen(s_num[N_ALT].str) == 5) saw5 = 1;
        if (strlen(s_num[N_ALT].str) == 4) saw4 = 1;
        /* lag: display within quantum + slope x 2 update periods of the input */
        if (t > 1.0f && (t < 15.5f || (t > 21.0f && t < 35.5f))) {
            float slope[N_COUNT] = { 62.5f, 3300.0f, 0.0f, 62.5f / 661.5f, 0.8f, 1.0f };
            float raw[N_COUNT];
            raw[N_SPD] = hs.speedKt; raw[N_ALT] = hs.altFt; raw[N_VVI] = hs.vsFpm; raw[N_MACH] = hs.mach; raw[N_G] = hs.g; raw[N_AOA] = hs.aoaDeg;
            for (i = 0; i < N_COUNT; i++) {
                float lim = s_num[i].quantum * 1.2f + slope[i] * 2.0f * s_num[i].period + (i == N_SPD ? 0.3f : 0.0f);
                float e = (float)fabs(hnum_value(&s_num[i]) - raw[i]) / lim;
                if (e > maxLag[i]) { maxLag[i] = e; maxErrIdx = i; }
            }
        }
        if (fr % 240 == 0)
            log_printf(LOG_INFO, "hud: numbers t=%.1f spd=\"%s\" alt=\"%s\" vvi=\"%s\" %s %s %s peak=\"%s\"", (double)t,
                       s_num[N_SPD].str, s_num[N_ALT].str, s_num[N_VVI].str, s_num[N_MACH].str, s_num[N_G].str, s_num[N_AOA].str,
                       s_peakStr);
    }
    (void)maxErrIdx;
    for (i = 0; i < N_COUNT; i++) {
        e_snprintf(buf, (int)sizeof(buf), "%s", names[i]);
        log_printf(LOG_INFO, "hud: rate %s changes=%d maxPerSec=%d limit=%.0f flicker=%d lagRatio=%.2f", buf, tr[i].nChg,
                   tr[i].maxWin, (double)rates[i], tr[i].flicker, (double)maxLag[i]);
        if ((float)tr[i].maxWin > rates[i] + 0.01f) TFAIL("hud: FAIL rate %s %d/s > %.0f", buf, tr[i].maxWin, (double)rates[i]);
        if (tr[i].flicker) TFAIL("hud: FAIL flicker %s %d", buf, tr[i].flicker);
        if (maxLag[i] > 1.0f) TFAIL("hud: FAIL lag %s ratio %.2f", buf, (double)maxLag[i]);
    }
    log_printf(LOG_INFO, "hud: numbers hold changes=%d g99=%d alt5=%d alt4=%d boxChanged=%d", holdChanges, sawG99, saw5, saw4, boxChanged);
    if (holdChanges) TFAIL("hud: FAIL jitter changed the display %d times while holding", holdChanges);
    if (!sawG99) TFAIL("hud: FAIL G 9.9 never shown");
    if (!saw5 || !saw4) TFAIL("hud: FAIL altitude 4/5 digit strings not seen");
    if (boxChanged) TFAIL("hud: FAIL box size changed %d times", boxChanged);
    hud_reset();
    return fails;
}

int hud_test(const char *name)
{
    int r = -1;
    s_fail = 0;
    r_screen_size(&s_W, &s_H);
    log_printf(LOG_INFO, "hud: test %s screen=%dx%d set=%d k=%.4f atlas=%d", name, s_W, s_H, hg_set_lines(), (double)hg_k(), hg_ready());
    shots_dir();
    if (!e_stricmp(name, "ladder")) r = test_ladder();
    else if (!e_stricmp(name, "numbers")) r = test_numbers();
    else if (!e_stricmp(name, "layout")) r = test_layout();
    else if (!e_stricmp(name, "contacts")) { extern int hudtgt_test(void); r = hudtgt_test(); }   /* T15 */
    else if (!e_stricmp(name, "zone")) { extern int hudwpn_test(void); r = hudwpn_test(); }       /* T16 */
    else if (!e_stricmp(name, "all")) r = test_layout() + test_numbers() + test_ladder();
    else { log_printf(LOG_INFO, "hud: test unknown name=%s (ladder numbers layout all)", name); return -1; }
    r += s_fail;
    log_printf(LOG_INFO, "test: hud %s failures=%d", name, r);
    return r;
}
