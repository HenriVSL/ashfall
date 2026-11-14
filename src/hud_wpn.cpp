/* hud_wpn.cpp - weapon HUD + -hudtest zone. See hud_wpn.h. Windows/render side. Owner: T16.
   Layout values are virtual 640x480 units scaled by k = screen h / 480 (hud_font.h); docs/HUD_LAYOUT.md "Weapon HUD". */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "hud_wpn.h"
#include "hud.h"
#include "hud_font.h"
#include "hud_tgt.h"
#include "hud_map.h"
#include "radar.h"
#include "weapons.h"
#include "missile.h"
#include "flight.h"
#include "params.h"
#include "e_log.h"
#include "e_plat.h"
#include "e_ui.h"
#include "e_file.h"
#include "e_image.h"
#include "e_tex.h"
#include "e_rend.h"

#define COL_GREEN   0xFF40FF60u
#define COL_RED     0xFFFF3030u

/* layout (virtual 640x480 units) */
#define RD_X        396.0f                          /* readouts: left column = altitude box left edge */
#define RD_Y        290.0f                          /* first readout cap top; rows RD_DY apart */
#define RD_DY       14.0f
#define WPN_Y       368.0f                          /* cockpit weapon line (centred at x 320) */
#define VIS_Y       381.0f                          /* "VISUAL" under it */
#define MSL_Y_CP    155.0f                          /* MISSILE cap top, cockpit: between the heading caret (144) and
                                                       the circle's outer reach (172) */
#define MSL_Y_CH    98.0f                           /*                  chase (T16b: 120 -> 98, above the circle's reach
                                                       at its highest centre, 200 - 68 = 132) */
#define LOCK_Y_CP   318.0f                          /* LOCK cap top, cockpit: under the circle (308), over the bank
                                                       pointer */
#define LOCK_Y_CH   116.0f                          /* chase (T16b: 138 -> 116, bottom 125 + shadow < 132) */
#define CH_RD_OFF   44.0f                           /* T16b chase readouts: first cap top this far below the circle centre
                                                       (centre >= 200: top >= 244, clear of the chase altitude box ~239) */
#define BLK_X       560.0f                          /* chase weapon block: centre x, first row cap top */
#define BLK_Y       410.0f
#define BAR_HALF    25.0f                           /* chase DLZ bar: 50 tall */
#define BAR_TICK    6.0f                            /*                ticks 6 wide */
#define SHOOT_GAP   16.0f                           /* SHOOT cap bottom this far above the TD box top (T15's selection
                                                       triangle sits in between) */
#define SHOOT_LIM_CP 148.0f                         /* SHOOT top limit, cockpit: below the heading caret */
#define SHOOT_LIM_CH 48.0f                          /*                  chase: below the small heading tape */

enum { TL_NONE = 0, TL_TOF, TL_ACT, TL_TTG };       /* third readout line */

static const float s_scaleNm[9] = { 2.0f, 3.0f, 5.0f, 8.0f, 10.0f, 15.0f, 20.0f, 30.0f, 40.0f };

/* ================================================================ params ([hud] wpn*) */
static float s_band = 0.03f, s_onSec = 0.2f, s_latchSec = 0.3f, s_aseDegL = 20.0f, s_aseDegS = 15.0f;
static float s_ringR = 160.0f, s_threatAlpha = 0.6f;
static int   s_aspect = 1, s_maxThreats = 3, s_ringTexOn = 1, s_chaseAse = 1;
static char  s_nameL[PARAM_STR] = "AIM-120", s_nameS[PARAM_STR] = "AIM-9";

static const Param s_tab[] = {
    { "hud", "wpnShootBand", P_FLOAT, &s_band, "0.03", 0, 0.2f, "SHOOT / flashing / breakaway hysteresis: they enter only this fraction inside Rmax / Rne / Rmin and leave at the exact value" },
    { "hud", "wpnShootOnSec", P_FLOAT, &s_onSec, "0.2", 0, 2, "SHOOT (steady, flashing) appears after its zone condition held this long (s); it leaves at once" },
    { "hud", "wpnLatchSec", P_FLOAT, &s_latchSec, "0.3", 0, 2, "minimum display time of the seeker lock, growl, LOCK, breakaway X, TOF (s)" },
    { "hud", "aseDegLrm", P_FLOAT, &s_aseDegL, "20", 2, 60, "steering dot: the circle edge = this angle between the nose and the lead direction (LRM, deg)" },
    { "hud", "aseDegSrm", P_FLOAT, &s_aseDegS, "15", 2, 60, "steering dot: the circle edge = this angle between the nose and the lead direction (SRM, deg)" },
    { "hud", "aspectPointer", P_BOOL, &s_aspect, "1", 0, 1, "launch-zone circle: target aspect pointer (target heading relative to ours, 12 o'clock = same)" },
    { "hud", "wpnThreatRing", P_FLOAT, &s_ringR, "160", 80, 230, "threat arrows: tip radius around the screen centre (virtual units; 160 clears the boxes, bank scale, weapon line and minimap)" },
    { "hud", "wpnThreatMax", P_INT, &s_maxThreats, "3", 0, 8, "max threat arrows (nearest first)" },
    { "hud", "wpnThreatAlpha", P_FLOAT, &s_threatAlpha, "0.6", 0.1f, 1, "alpha of the threat arrows other than the nearest" },
    { "hud", "wpnRingTex", P_BOOL, &s_ringTexOn, "1", 0, 1, "launch-zone circle as one generated texture quad (0 = polyline: ~28 more quads)" },
    { "hud", "wpnNameLrm", P_STR, s_nameL, "AIM-120", 0, 0, "weapon line name of the LRM" },
    { "hud", "wpnNameSrm", P_STR, s_nameS, "AIM-9", 0, 0, "weapon line name of the SRM" },
    { "hud", "chaseAse", P_BOOL, &s_chaseAse, "1", 0, 1, "chase view: full weapon HUD (launch-zone circle on the steady aim cross + Vc/RNG/TOF readouts, VISUAL circle); 0 = T16's compact DLZ bar beside the TD box" },
};
void hudwpn_params(void) { params_add(s_tab, E_COUNT(s_tab)); }

/* ================================================================ sprites */
static int s_sprSet = -1, s_sprPtr = -1, s_sprDot = -1, s_sprX = -1, s_sprSeek = -1, s_sprThreat = -1;
static int s_seekRpx = 26, s_ptrOy = 7, s_ptrHw = 6;
static const float *s_brightP;
static int s_W = 640, s_H = 480;
static float s_ms;

static void sprites_init(void)
{
    int w, h, ox, oy;
    const Param *p;
    if (s_sprSet == hg_set_lines() && s_sprSet != -1) return;
    s_sprSet = hg_set_lines();
    s_sprPtr = hg_sprite_id("ptr_tri");
    s_sprDot = hg_sprite_id("dot4");
    s_sprX = hg_sprite_id("breakaway_x");
    s_sprSeek = hg_sprite_id("seeker");
    s_sprThreat = hg_sprite_id("threat_arrow");
    s_seekRpx = hg_sprite_info(s_sprSeek, &w, &h, &ox, &oy) ? ox : hg_px(26.0f);
    if (hg_sprite_info(s_sprPtr, &w, &h, &ox, &oy)) { s_ptrOy = oy; s_ptrHw = (w + 1) / 2; }
    else { s_ptrOy = hg_px(7.0f); s_ptrHw = hg_px(4.0f); }
    p = params_find("hud", "bright");
    s_brightP = p ? (const float *)p->ptr : NULL;
    log_printf(LOG_INFO, "hud: wpn init set=%d ptr=%d dot=%d x=%d seek=%d threat=%d seekR=%d ptrOy=%d ptrHw=%d",
               s_sprSet, s_sprPtr, s_sprDot, s_sprX, s_sprSeek, s_sprThreat, s_seekRpx, s_ptrOy, s_ptrHw);
}

/* ================================================================ launch-zone ring: one generated texture quad
   Premultiplied white like the atlas (RGB = coverage, A = coverage + 0.6 x shadow x (1 - coverage)), stroke profile
   of the atlas `line` (1 x stroke, 1/4 fringe), baked shadow offset (+o, +o) as A4c. 4x4 supersampled near the ring. */
static RTex  s_ringTex;
static int   s_ringSet = -1, s_ringN, s_ringC;
static float s_ringRk = -1.0f;

static int ring_ready(float Rpx)
{
    int set = hg_set_lines(), s, o, C, N, need, x, y, a, b;
    float half, *cov;
    EImage img;
    char name[48];
    s64 t0;
    if (set == s_ringSet && (float)fabs(Rpx - s_ringRk) < 0.01f) return s_ringTex != 0;
    s_ringSet = set;
    s_ringRk = Rpx;
    s_ringTex = 0;
    if (!s_ringTexOn || !hg_ready()) return 0;
    t0 = plat_qpc();
    s = hg_stroke();
    o = hg_shadow_off();
    half = (float)s * 0.5f;
    C = (int)ceil(Rpx + half + 2.0f);
    need = 2 * C + o + 1;
    for (N = 16; N < need; N *= 2) {}
    if (N > 256) {
        log_printf(LOG_INFO, "hud: wpn ring r=%.1f needs %d px > 256: polyline", (double)Rpx, need);
        return 0;
    }
    cov = (float *)malloc((size_t)(N * N) * sizeof(float));
    if (!cov) return 0;
    if (!img_alloc(&img, N, N, IMG_ALPHA)) { free(cov); return 0; }
    for (y = 0; y < N; y++) for (x = 0; x < N; x++) {
        float dx = (float)x + 0.5f - (float)C, dy = (float)y + 0.5f - (float)C, c = 0.0f;
        float e = (float)fabs(sqrt(dx * dx + dy * dy) - Rpx);
        if (e <= half + 2.0f) {
            for (b = 0; b < 4; b++) for (a = 0; a < 4; a++) {
                float sx = (float)x + ((float)a + 0.5f) * 0.25f - (float)C, sy = (float)y + ((float)b + 0.5f) * 0.25f - (float)C;
                float ee = (float)fabs(sqrt(sx * sx + sy * sy) - Rpx);
                c += ee <= half ? 1.0f : (ee <= half + 1.0f ? 0.25f : 0.0f);
            }
            c *= 1.0f / 16.0f;
        }
        cov[y * N + x] = c;
    }
    for (y = 0; y < N; y++) for (x = 0; x < N; x++) {
        float c = cov[y * N + x], sh = (o > 0 && x >= o && y >= o) ? cov[(y - o) * N + (x - o)] : 0.0f;
        float al = c + 0.6f * sh * (1.0f - c);
        u32 c8 = (u32)e_ftoi(c * 255.0f), a8 = (u32)e_ftoi(al * 255.0f);
        img.pix[0][y * N + x] = (a8 << 24) | (c8 << 16) | (c8 << 8) | c8;
    }
    free(cov);
    e_snprintf(name, (int)sizeof(name), "hudwpn_ring_%d_%d", set, e_ftoi(Rpx * 10.0f));
    s_ringTex = tex_add(name, &img, RT_CLAMP | RT_NOMIP);
    img_free(&img);
    s_ringN = N;
    s_ringC = C;
    log_printf(LOG_INFO, "hud: wpn ring texture set=%d r=%.2f stroke=%d shadow=%d N=%d tex=%d ms=%.1f", set, (double)Rpx, s, o, N, s_ringTex,
               (double)((double)(plat_qpc() - t0) * 1000.0 / (double)plat_qpf()));
    return s_ringTex != 0;
}

/* ================================================================ fixed-rate numbers (10.7.3, = T07/T15 rule) */
typedef struct SNum { float next; int init, mode, shown, upd, lastDir, lastUpd, havePend, pendDir, changes; } SNum;

static int qround(float q) { return (int)floor(q + 0.5f); }

static int snum_due(SNum *n, float t, float period)
{
    if (n->init && t + 1e-5f < n->next) return 0;
    if (!n->init) n->next = t;
    n->next += period;
    if (n->next <= t) n->next = t + period;
    n->upd++;
    return 1;
}

/* q in quanta of 'mode': 1 = shown changed. Dead band 0.7 quanta; continuing the last change's direction at the
   next update applies at once, a first move or a reversal needs 2 updates in a row. */
static int snum_apply(SNum *n, float q, int mode)
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

/* countdown in whole seconds (ACT / TTG): follows ceil(estimate) DOWN only (never back up on estimate noise or a
   coasting missile), at most one change per 0.5 s (2 Hz, 10.7.3); up only on a gross re-estimate (> 2 s) */
typedef struct CDown { int kind, valid, shown, changes, ups; u32 serial; float lastT; char str[8]; } CDown;

static void cdown_update(CDown *c, float t, int kind, u32 serial, float value)
{
    int target;
    if (kind != c->kind || serial != c->serial) {
        c->kind = kind; c->serial = serial; c->valid = 0; c->str[0] = 0; c->shown = -1;
    }
    if (value < 0.0f) return;                       /* unknown: keep what is shown */
    target = (int)ceil(value - 0.05f);
    if (target < 0) target = 0;
    if (!c->valid) {
        c->valid = 1; c->shown = target; c->lastT = t; c->changes++;
        e_snprintf(c->str, (int)sizeof(c->str), "%d", target);
        return;
    }
    if (t - c->lastT < 0.5f - 1e-4f) return;
    if (target < c->shown || target > c->shown + 2) {
        if (target > c->shown) c->ups++;
        c->shown = target; c->lastT = t; c->changes++;
        e_snprintf(c->str, (int)sizeof(c->str), "%d", target);
    }
}

/* latch: shown follows raw once raw differed for onSec (towards a nonzero value) / offSec (towards 0) and the shown
   value has been up for holdSec */
typedef struct Latch { int init, shown, pend, changes; float since, pendT; } Latch;

static int latch_update(Latch *l, float t, int raw, float onSec, float offSec, float holdSec)
{
    if (!l->init) { l->init = 1; l->shown = raw; l->since = t; l->pend = 0; return raw; }
    if (raw == l->shown) { l->pend = 0; return l->shown; }
    if (!l->pend) { l->pend = 1; l->pendT = t; }
    if (t - l->pendT >= (raw ? onSec : offSec) - 1e-4f && t - l->since >= holdSec - 1e-4f) {
        l->shown = raw; l->since = t; l->pend = 0; l->changes++;
    }
    return l->shown;
}

/* sticky whole-pixel position: moves only when the float position is > 0.75 px away (no 1 px rounding jitter) */
typedef struct Snap { int init, x, y; } Snap;
static void snap_xy(Snap *s, float x, float y, int *ox, int *oy)
{
    if (!s->init || (float)fabs(x - (float)s->x) > 0.75f || (float)fabs(y - (float)s->y) > 0.75f) {
        s->x = e_ftoi(x); s->y = e_ftoi(y); s->init = 1;
    }
    *ox = s->x; *oy = s->y;
}

static int flash_on(float t, float hz) { return (float)fmod((double)t * hz, 1.0) < 0.5f; }

/* ================================================================ display state */
typedef struct WpnSt {
    int lastTd, lastSel;
    /* zone */
    int shoot;                                      /* 0 off, 1 steady, 2 flashing */
    int onPend, nePend; float onPendT, nePendT; u32 onPendC, nePendC;
    int xOn, xPend; float xPendT;
    int scaleInit, scaleIdx, scaleChanges; float scaleT;
    int pinned;
    int shootBelow, barLeft, sideInit;
    /* readouts */
    int rngFeet;
    SNum vc, rng, tof;
    char vcStr[12], rngStr[12], tofStr[12];
    CDown cd;
    Latch tofL;
    int third;
    /* seeker */
    Latch seekL, growlL;
    int seekLock, seekOut;
    Vec3 seekPos;
    Snap seekSnap, dotSnap;
    /* warnings */
    Latch mslL, lockL;
    int mslOn, lockOn;
    int nearSerial;
    int mslSlot, lockSlot;                          /* T18: 0 = home slot, 1 = left, 2 = right (TD cluster in the way) */
    int pushSerial[WPN_THREATS_MAX], pushOn[WPN_THREATS_MAX];   /* T18: threat arrows pushed off text (hysteresis) */
    /* weapon line */
    int wlSel, wlCount; char wl[32];
    /* T16b chase circle */
    Snap aseSnap;                                   /* sticky whole-pixel centre y */
    int chRdY;                                      /* readout cap top (px) of the last chase frame with a zone */
    int blkCount; char blkCountStr[8]; int blkFlr; char blkFlrStr[12];
    HudWpnTones tones;
} WpnSt;
static WpnSt s_st;

/* debug record (tests) */
typedef struct WpnDbg {
    int zone, circle, ticks, raw, rawNe;
    float scaleNm, rmaxAng, rneAng, rminAng, ptrAng; int ptrPinned, ptrDrawn;
    float aspectAng; int aspect;
    int dot, dotX, dotY; float dotFx, dotFy;
    int shoot, shootDrawn, shootX, shootY, shootBelow;
    int brk, third; char vc[12], rng[12], thirdStr[16];
    int seek, seekX, seekY, seekLock, seekFlash, seekDrawn;
    int visual, msl, mslDrawn, lock, arrows; float arrowX[8], arrowY[8];
    int mslSlot, lockSlot, mslRect[4], lockRect[4]; int arrowPushed;   /* T18 */
    int bar, barX, barY0, barY1, barLeft; float caretY;
    char wl[32];
    float aseReach;                                 /* px: farthest ASE element from the circle centre */
    int ringQuad;                                   /* 1 = textured ring, else chords */
    int rdX0, rdY0, rdX1, rdY1;                     /* readout block bounds (px) */
    int aseCx, aseCy;                               /* T16b: circle centre drawn this frame (px; also VISUAL) */
} WpnDbg;
static WpnDbg s_dbg;
static int s_quads, s_draws;

static int s_wko[4][4], s_nwko, s_wkoView = -1;     /* T18: fixed texts of the last frame (hudwpn_keepout) */

void hudwpn_reset(void)
{
    memset(&s_st, 0, sizeof(s_st));
    s_nwko = 0; s_wkoView = -1;
    s_st.lastTd = -2;
    s_st.lastSel = -1;
    s_st.seekLock = -1;
    s_st.nearSerial = -1;
    s_st.wlSel = -1;
    s_st.blkCount = -1;
    s_st.blkFlr = -1;
    s_st.cd.serial = 0xFFFFFFFFu;
}

float hudwpn_ms_last(void) { return s_ms; }

int hudwpn_init(void)
{
    sprites_init();
    return ring_ready(hud_layout()->aseR * hg_k());
}
void hudwpn_tones(HudWpnTones *out) { if (out) *out = s_st.tones; }

static void zone_reset(void)
{
    s_st.shoot = 0; s_st.onPend = s_st.nePend = 0;
    s_st.xOn = s_st.xPend = 0;
    s_st.scaleInit = 0; s_st.pinned = 0;
    s_st.sideInit = 0;
    s_st.rngFeet = 0;
    memset(&s_st.vc, 0, sizeof(SNum)); memset(&s_st.rng, 0, sizeof(SNum)); memset(&s_st.tof, 0, sizeof(SNum));
    s_st.vcStr[0] = s_st.rngStr[0] = s_st.tofStr[0] = 0;
    memset(&s_st.cd, 0, sizeof(CDown)); s_st.cd.serial = 0xFFFFFFFFu;
    memset(&s_st.tofL, 0, sizeof(Latch));
    s_st.third = TL_NONE;
    memset(&s_st.dotSnap, 0, sizeof(Snap));
}

static int scale_pick(float needNm)
{
    int i;
    for (i = 0; i < 9; i++) if (s_scaleNm[i] >= needNm) return i;
    return 8;
}

/* ================================================================ decisions */
static void decide_zone(float t, const WpnIn *in, int zone)
{
    const DlzView *d = &in->dlz;
    float R = d->range, b = s_band;
    int raw = 0, rawIn = 0, ne = 0, neIn = 0;
    /* breakaway X: conservative (on early inside Rmin x (1 + band), off late beyond 1 + 2 band for the latch time) */
    if (zone && d->ready && d->rmin > 0.0f && d->rmax > 0.0f) {
        if (!s_st.xOn) { if (R < d->rmin * (1.0f + b)) { s_st.xOn = 1; s_st.xPend = 0; } }
        else if (R > d->rmin * (1.0f + 2.0f * b)) {
            if (!s_st.xPend) { s_st.xPend = 1; s_st.xPendT = t; }
            if (t - s_st.xPendT >= s_latchSec - 1e-4f) { s_st.xOn = 0; s_st.xPend = 0; }
        } else s_st.xPend = 0;
    } else { s_st.xOn = 0; s_st.xPend = 0; }
    /* SHOOT: only what the published zone says (DlzView). Entering needs the banded condition for wpnShootOnSec AND
       a completed live cycle since it started (every value refreshed while it held), so the next publication cannot
       cut a fresh SHOOT short; leaving is at once (honest). */
    if (zone && d->ready && d->rmax > 0.0f && !s_st.xOn) {
        raw = R <= d->rmax && R > d->rmin && !(d->nowValid && !d->nowHit);
        if (in->sel == MSL_SRM) raw = raw && in->srmLock >= 0 && in->srmLock == in->td;
        rawIn = raw && R <= d->rmax * (1.0f - b) && R >= d->rmin * (1.0f + 2.0f * b);   /* Rmin side: X releases there */
        ne = raw && d->rne > 0.0f && R <= d->rne && !(d->nowEscValid && !d->nowNoEscape) && !d->memory && !in->tdMemory;
        neIn = ne && R <= d->rne * (1.0f - b);
    }
    s_dbg.raw = raw;
    s_dbg.rawNe = ne;
    if (!raw) { s_st.shoot = 0; s_st.onPend = 0; s_st.nePend = 0; }
    else {
        if (s_st.shoot == 0) {
            if (!rawIn) s_st.onPend = 0;
            else {
                if (!s_st.onPend) { s_st.onPend = 1; s_st.onPendT = t; s_st.onPendC = d->cycles; }
                if (t - s_st.onPendT >= s_onSec - 1e-4f && d->cycles != s_st.onPendC) { s_st.shoot = 1; s_st.onPend = 0; }
            }
        }
        if (s_st.shoot >= 1) {
            if (!ne) { s_st.shoot = 1; s_st.nePend = 0; }
            else if (s_st.shoot == 1) {
                if (!neIn) s_st.nePend = 0;
                else {
                    if (!s_st.nePend) { s_st.nePend = 1; s_st.nePendT = t; s_st.nePendC = d->cycles; }
                    if (t - s_st.nePendT >= s_onSec - 1e-4f && d->cycles != s_st.nePendC) { s_st.shoot = 2; s_st.nePend = 0; }
                }
            }
        }
    }
    if (!zone) return;
    /* range scale: up when 1.2 Rmax passes 1.08 x the scale (at once beyond 1.15), down below 0.92 x the next lower
       one; 1 s between changes */
    {
        float need = d->rmax > 0.0f ? 1.2f * d->rmax / NM2M : 0.0f, sc;
        int i = s_st.scaleIdx;
        if (!s_st.scaleInit) {
            s_st.scaleIdx = scale_pick(need > 0.0f ? need : R / NM2M);
            s_st.scaleInit = 1;
            s_st.scaleT = t;
        } else if (need > 0.0f) {
            if (i < 8 && (need > s_scaleNm[i] * 1.15f || (need > s_scaleNm[i] * 1.08f && t - s_st.scaleT >= 1.0f))) {
                s_st.scaleIdx = scale_pick(need); s_st.scaleT = t; s_st.scaleChanges++;
            } else if (i > 0 && need < s_scaleNm[i - 1] * 0.92f && t - s_st.scaleT >= 1.0f) {
                s_st.scaleIdx = i - 1; s_st.scaleT = t; s_st.scaleChanges++;
            }
        }
        sc = s_scaleNm[s_st.scaleIdx] * NM2M;
        if (!s_st.pinned && R > sc) s_st.pinned = 1;
        else if (s_st.pinned && R < 0.98f * sc) s_st.pinned = 0;
    }
}

static void decide_readouts(float t, const WpnIn *in, int zone)
{
    const DlzView *d = &in->dlz;
    int kind = TL_NONE, tofVis;
    float v = -1.0f;
    if (!zone) return;
    if (snum_due(&s_st.vc, t, 0.2f) && snum_apply(&s_st.vc, d->closure * MS2KT / 10.0f, 0))      /* Vc 5 Hz, 10 kt */
        e_snprintf(s_st.vcStr, (int)sizeof(s_st.vcStr), "%d", s_st.vc.shown * 10);
    {                                                                   /* RNG 5 Hz: 0.1 NM, < 1 NM: 100 ft */
        float nm = d->range / NM2M;
        if (!s_st.rngFeet && nm < 0.95f) s_st.rngFeet = 1;
        else if (s_st.rngFeet && nm > 1.05f) s_st.rngFeet = 0;
        if (snum_due(&s_st.rng, t, 0.2f)) {
            int mode = s_st.rngFeet;
            float q = mode ? d->range * M2FT / 100.0f : nm * 10.0f;
            if (snum_apply(&s_st.rng, q, mode)) {
                int n = s_st.rng.shown;
                if (mode) e_snprintf(s_st.rngStr, (int)sizeof(s_st.rngStr), "%d", n);
                else e_snprintf(s_st.rngStr, (int)sizeof(s_st.rngStr), "%d.%d", n / 10, n % 10);
            }
        }
    }
    /* third line: own missile at the TD -> ACT (LRM until active) / TTG; else TOF of the NOW fly-out */
    tofVis = latch_update(&s_st.tofL, t, d->tof >= 0.0f, s_latchSec, s_latchSec, 0.0f);
    if (in->msl) {
        kind = (in->mslType == MSL_LRM && in->mslAct >= 0.0f) ? TL_ACT : TL_TTG;
        v = kind == TL_ACT ? in->mslAct : in->mslTtg;
    } else if (tofVis) kind = TL_TOF;
    if (kind == TL_ACT || kind == TL_TTG) cdown_update(&s_st.cd, t, kind, in->mslSerial, v);
    else { s_st.cd.kind = TL_NONE; s_st.cd.serial = 0xFFFFFFFFu; }
    if (kind == TL_TOF) {
        if (d->tof >= 0.0f && snum_due(&s_st.tof, t, 0.5f) && snum_apply(&s_st.tof, d->tof, 0))   /* 2 Hz, 1 s */
            e_snprintf(s_st.tofStr, (int)sizeof(s_st.tofStr), "%d", s_st.tof.shown);
    } else { memset(&s_st.tof, 0, sizeof(SNum)); s_st.tofStr[0] = 0; }
    s_st.third = kind;
}

static void decide_seeker(float t, const WpnIn *in)
{
    int srm = in->sel == MSL_SRM;
    int shown = latch_update(&s_st.seekL, t, srm && in->srmLock >= 0 ? in->srmLock + 1 : 0, 0.12f, 0.12f, s_latchSec);
    int growl = latch_update(&s_st.growlL, t, srm && in->srmLock < 0 && in->srmNear, 0.12f, 0.12f, s_latchSec);
    s_st.seekLock = shown - 1;
    if (in->srmLock >= 0) s_st.seekPos = in->srmLockPos;
    s_st.tones.srmLock = srm && s_st.seekLock >= 0;
    s_st.tones.growl = srm && growl && !s_st.tones.srmLock;
}

static void decide_warn(float t, const WpnIn *in)
{
    s_st.mslOn = latch_update(&s_st.mslL, t, in->nThreats > 0, 0.0f, 0.5f, 0.0f);
    s_st.lockOn = latch_update(&s_st.lockL, t, in->rwrLock ? 1 : 0, 0.1f, 0.5f, s_latchSec);
    s_st.tones.missile = s_st.mslOn;
    s_st.tones.rwrLock = s_st.lockOn;
}

/* ================================================================ drawing helpers */
static void reach_add(float r) { if (r > s_dbg.aseReach) s_dbg.aseReach = r; }

/* radial stroke from r0 to r1 at angle a (rad, clockwise from 12 o'clock), exact ends */
static void radial(float cx, float cy, float a, float r0, float r1, float ws, u32 col)
{
    float sa = (float)sin(a), ca = (float)cos(a), hw = (float)hg_stroke() * 0.5f * ws + 1.0f;
    float ro = r0 > r1 ? r0 : r1;
    if (ws >= 0.99f) hg_line_ext(cx + sa * r0, cy - ca * r0, cx + sa * r1, cy - ca * r1, 0.0f, col);
    else hg_line_w(cx + sa * r0, cy - ca * r0, cx + sa * r1, cy - ca * r1, ws, col);
    reach_add((float)sqrt(ro * ro + hw * hw) + (float)hg_shadow_off());
}

static void draw_ring(float cx, float cy, float Rpx, u32 col)
{
    if (ring_ready(Rpx)) {
        RVert2D v[4];
        float x0 = cx - (float)s_ringC, y0 = cy - (float)s_ringC, x1 = x0 + (float)s_ringN, y1 = y0 + (float)s_ringN;
        int i;
        v[0].x = x0; v[0].y = y0; v[0].u = 0.0f; v[0].v = 0.0f;
        v[1].x = x1; v[1].y = y0; v[1].u = 1.0f; v[1].v = 0.0f;
        v[2].x = x1; v[2].y = y1; v[2].u = 1.0f; v[2].v = 1.0f;
        v[3].x = x0; v[3].y = y1; v[3].u = 0.0f; v[3].v = 1.0f;
        for (i = 0; i < 4; i++) v[i].color = col;
        hg_image_quad(s_ringTex, v, RB_PREMUL);
        s_dbg.ringQuad = 1;
    } else {
        int n = e_ftoi(Rpx / 2.6f), i;
        float px = cx, py = cy - Rpx;
        if (n < 24) n = 24;
        if (n > 64) n = 64;
        for (i = 1; i <= n; i++) {
            float a = (float)i * (2.0f * E_PI / (float)n), nx = cx + (float)sin(a) * Rpx, ny = cy - (float)cos(a) * Rpx;
            hg_line_w(px, py, nx, ny, 1.0f, col);
            px = nx; py = ny;
        }
    }
    reach_add(Rpx + (float)hg_stroke() * 0.5f + 1.0f + (float)hg_shadow_off());
}

static void draw_dashed_circle(float cx, float cy, float Rpx, u32 col)
{
    int i, n = 16;
    for (i = 0; i < n; i++) {
        float a0 = ((float)i + 0.25f) * (2.0f * E_PI / (float)n), a1 = ((float)i + 0.75f) * (2.0f * E_PI / (float)n);
        hg_line_w(cx + (float)sin(a0) * Rpx, cy - (float)cos(a0) * Rpx, cx + (float)sin(a1) * Rpx, cy - (float)cos(a1) * Rpx, 1.0f, col);
    }
}

static float ang_of(float rM, float scM)
{
    float a = 2.0f * E_PI * rM / scM, lim = E_DEG2RAD(355.0f);
    return a > lim ? lim : (a < 0.0f ? 0.0f : a);
}

/* camera basis from the 3D frame's view matrix (columns = right, up, forward in world) */
static void cam_basis(const HudState *hs, Vec3 *r, Vec3 *u, Vec3 *f)
{
    *r = v3(hs->viewM.m[0][0], hs->viewM.m[1][0], hs->viewM.m[2][0]);
    *u = v3(hs->viewM.m[0][1], hs->viewM.m[1][1], hs->viewM.m[2][1]);
    *f = v3(hs->viewM.m[0][2], hs->viewM.m[1][2], hs->viewM.m[2][2]);
}

static int draw_number_label(int x1, int y, const char *num, const char *label, int space, u32 col)
{
    int w = 0;
    if (!num[0]) return 0;
    w = hg_text(x1, y, HF_SMALL, col, 2, num);
    hg_text(x1 + hg_gap(HF_SMALL) + (space ? hg_adv(HF_SMALL) : 0), y, HF_SMALL, col, 0, label);
    if (x1 - w < s_dbg.rdX0 || !s_dbg.rdX1) s_dbg.rdX0 = x1 - w;
    {
        int xe = x1 + hg_gap(HF_SMALL) + (space ? hg_adv(HF_SMALL) : 0) + hg_text_w(HF_SMALL, label);
        if (xe > s_dbg.rdX1) s_dbg.rdX1 = xe;
    }
    if (!s_dbg.rdY0 || y < s_dbg.rdY0) s_dbg.rdY0 = y;
    if (y + hg_cap(HF_SMALL) > s_dbg.rdY1) s_dbg.rdY1 = y + hg_cap(HF_SMALL);
    return 1;
}

static const char *third_label(int kind) { return kind == TL_ACT ? "ACT" : (kind == TL_TTG ? "TTG" : "TOF"); }
static const char *third_num(void) { return s_st.third == TL_TOF ? s_st.tofStr : (s_st.third == TL_NONE ? "" : s_st.cd.str); }

static void draw_shoot(const HudState *hs, const WpnIn *in, int zone, u32 col)
{
    int capL = hg_cap(HF_LARGE), tx, ty, top, lim;
    int cockpit = hs->view != VIEW_CHASE;
    if (in->tdBox) {
        top = in->tdY - in->tdHalf - hg_px(SHOOT_GAP) - capL;
        lim = hg_px(cockpit ? SHOOT_LIM_CP : SHOOT_LIM_CH);
        if (!s_st.shootBelow && top < lim) s_st.shootBelow = 1;
        else if (s_st.shootBelow && top > lim + hg_px(6.0f)) s_st.shootBelow = 0;
        tx = in->tdX;
        ty = s_st.shootBelow ? in->tdY + in->tdHalf + hg_px(14.0f) : top;
        if (s_st.shootBelow && !cockpit && s_dbg.bar) {          /* T18: below the chase bar's third line, not on it */
            int yb = s_dbg.barY1 + hg_px(4.0f) + hg_cap(HF_SMALL) + hg_px(4.0f);
            if (ty < yb) ty = yb;
        }
    } else {
        tx = hg_px(320.0f);
        ty = hg_px(cockpit ? 196.0f : 200.0f);
    }
    s_dbg.shoot = zone ? s_st.shoot : 0;
    s_dbg.shootX = tx; s_dbg.shootY = ty; s_dbg.shootBelow = s_st.shootBelow;
    if (!zone || !s_st.shoot) return;
    if (s_st.shoot == 2 && !flash_on(hs->time, 3.0f)) return;
    hg_text(tx, ty, HF_LARGE, col, 1, "SHOOT");
    s_dbg.shootDrawn = 1;
}

/* SRM seeker circle: boresight (waterline / aim cross) while searching, on the locked target when locked */
static void draw_seeker(const HudState *hs, const WpnIn *in, u32 col)
{
    const HudLayout *lay = hud_layout();
    float x, y, k = hg_k(), bx, by;
    int cockpit = hs->view != VIEW_CHASE, sx, sy, flash = 0, onTgt = 0;
    if (in->sel != MSL_SRM) return;
    if (cockpit) { bx = (float)hg_px(lay->wlX); by = (float)hg_px(lay->wlY); }
    else if (!hud_project_point(hs, v3_madd(hs->pos, hs->noseDir, 1000.0f), &bx, &by)) { bx = (float)s_W * 0.5f; by = (float)s_H * 0.5f; }
    x = bx; y = by;
    if (s_st.seekLock >= 0) onTgt = hud_project_point(hs, s_st.seekPos, &x, &y);
    if (!onTgt) { x = bx; y = by; }
    {
        float fx0, fy0, fx1, fy1, m = 2.0f * k;
        if (cockpit) { fx0 = (float)hg_px(lay->frameX0); fy0 = (float)hg_px(lay->frameY0); fx1 = (float)hg_px(lay->frameX1); fy1 = (float)hg_px(lay->frameY1); }
        else { fx0 = (float)s_seekRpx; fy0 = (float)s_seekRpx; fx1 = (float)(s_W - s_seekRpx); fy1 = (float)(s_H - s_seekRpx); }
        if (!s_st.seekOut && (x < fx0 - m || x > fx1 + m || y < fy0 - m || y > fy1 + m)) s_st.seekOut = 1;
        else if (s_st.seekOut && x > fx0 + m && x < fx1 - m && y > fy0 + m && y < fy1 - m) s_st.seekOut = 0;
        x = E_CLAMP(x, fx0, fx1);
        y = E_CLAMP(y, fy0, fy1);
        flash = s_st.seekOut;
    }
    snap_xy(&s_st.seekSnap, x, y, &sx, &sy);
    s_dbg.seek = 1; s_dbg.seekX = sx; s_dbg.seekY = sy; s_dbg.seekLock = s_st.seekLock; s_dbg.seekFlash = flash;
    if (flash && !flash_on(hs->time, 3.0f)) return;
    if (cockpit) hg_sprite(s_sprSeek, (float)sx, (float)sy, col);
    else {                                                       /* chase: 3 deg at the chase FOV */
        float want = (float)tan(E_DEG2RAD(3.0f)) * hs->projM.m[0][0] * (float)s_W * 0.5f;
        hg_sprite_scaled(s_sprSeek, (float)sx, (float)sy, s_seekRpx > 0 ? want / (float)s_seekRpx : 1.0f, col);
    }
    s_dbg.seekDrawn = 1;
}

static void weapon_strings(const WpnIn *in)
{
    int cnt = in->sel == MSL_SRM ? in->srm : in->lrm;
    if (cnt != s_st.wlCount || in->sel != s_st.wlSel) {
        s_st.wlSel = in->sel; s_st.wlCount = cnt;
        e_snprintf(s_st.wl, (int)sizeof(s_st.wl), "%s %d", in->sel == MSL_SRM ? s_nameS : s_nameL, cnt);
        e_snprintf(s_st.blkCountStr, (int)sizeof(s_st.blkCountStr), "%d", cnt);
    }
    if (in->flares != s_st.blkFlr) {
        s_st.blkFlr = in->flares;
        e_snprintf(s_st.blkFlrStr, (int)sizeof(s_st.blkFlrStr), "FLR %d", in->flares);
    }
    e_strlcpy(s_dbg.wl, s_st.wl, (int)sizeof(s_dbg.wl));
}

/* ================================================================ views */
/* the launch-zone circle with everything attached (both views, T16b): ring, Rmax / Rne / Rmin ticks, range pointer,
   aspect pointer, steering dot, breakaway X at the centre, readouts (4-digit column starting at x 396, first cap top
   rdY px). Cockpit: centre = the waterline, the dot from the camera (= nose) frame. Chase: centre = the steady aim
   cross (hud_chase_ase), the dot from the angle nose -> lead direction laid onto the camera's screen axes. */
static void draw_ase(const HudState *hs, const WpnIn *in, float cx, float cy, int rdY, u32 col)
{
    const HudLayout *lay = hud_layout();
    const DlzView *d = &in->dlz;
    float k = hg_k(), Rpx = lay->aseR * k;
    float Rlim = (lay->aseR + lay->aseOut) * k, st = (float)hg_stroke(), o = (float)hg_shadow_off();
    {
        float sc = s_scaleNm[s_st.scaleIdx] * NM2M, a;
        s_dbg.aseCx = e_ftoi(cx); s_dbg.aseCy = e_ftoi(cy);
        s_dbg.circle = 1;
        s_dbg.scaleNm = s_scaleNm[s_st.scaleIdx];
        draw_ring(cx, cy, Rpx, col);                             /* first: its own quad before the atlas batch */
        if (d->ready && d->rmax > 0.0f) {                        /* ticks once the first live Rmax is out */
            float tickOut = Rlim - o - 1.0f, neOut = Rpx + 0.75f * lay->aseOut * k, da = 1.5f * st / Rpx;
            s_dbg.ticks = 1;
            a = ang_of(d->rmax, sc); s_dbg.rmaxAng = E_RAD2DEG(a);
            radial(cx, cy, a, Rpx, tickOut, 1.0f, col);
            if (d->rne > 0.0f) {
                a = ang_of(d->rne, sc); s_dbg.rneAng = E_RAD2DEG(a);
                radial(cx, cy, a - da, Rpx, neOut, 1.0f, col);
                radial(cx, cy, a + da, Rpx, neOut, 1.0f, col);
            }
            if (d->rmin > 0.0f) {
                a = ang_of(d->rmin, sc); s_dbg.rminAng = E_RAD2DEG(a);
                radial(cx, cy, a, Rpx, Rpx - 6.0f * k, 1.0f, col);
            }
        }
        {                                                        /* range pointer: outside the ring, pointing in */
            float hw = (float)s_ptrHw + o, body = (float)s_ptrOy + o, lim2 = Rlim * Rlim - hw * hw;
            float tipR = Rpx + st * 0.5f, tipMax = (lim2 > 0.0f ? (float)sqrt(lim2) : Rlim) - body;
            if (tipR > tipMax) tipR = tipMax;
            a = s_st.pinned ? E_DEG2RAD(355.0f) : ang_of(d->range, sc);
            s_dbg.ptrAng = E_RAD2DEG(a); s_dbg.ptrPinned = s_st.pinned;
            if (!s_st.pinned || flash_on(hs->time, 3.0f)) {
                hg_sprite_rot(s_sprPtr, cx + (float)sin(a) * tipR, cy - (float)cos(a) * tipR, a, col);
                s_dbg.ptrDrawn = 1;
            }
            reach_add((float)sqrt((tipR + body) * (tipR + body) + hw * hw));
        }
        if (s_aspect && in->td >= 0) {                           /* aspect: target heading relative to ours */
            float rel = in->tdHeading - E_DEG2RAD(hs->headingDeg);
            while (rel < 0.0f) rel += 2.0f * E_PI;
            while (rel >= 2.0f * E_PI) rel -= 2.0f * E_PI;
            s_dbg.aspect = 1; s_dbg.aspectAng = E_RAD2DEG(rel);
            radial(cx, cy, rel, Rpx, Rpx + 0.55f * lay->aseOut * k, 0.6f, col);
        }
        if (d->leadValid) {                                      /* steering dot */
            Vec3 r, u, f;
            float bx, by, bz, n, ang, frac, fx, fy;
            int dx, dy;
            cam_basis(hs, &r, &u, &f);
            if (hs->view == VIEW_CHASE) {                        /* T16b: off the NOSE (the camera lags it) */
                float c = v3_dot(d->leadDir, hs->noseDir);
                Vec3 w = v3_sub(d->leadDir, v3_scale(hs->noseDir, c));
                bx = v3_dot(w, r); by = v3_dot(w, u);
                n = (float)sqrt(bx * bx + by * by);
                ang = E_RAD2DEG((float)atan2(v3_len(w), c));
            } else {
                bx = v3_dot(d->leadDir, r); by = v3_dot(d->leadDir, u); bz = v3_dot(d->leadDir, f);
                n = (float)sqrt(bx * bx + by * by);
                ang = E_RAD2DEG((float)atan2(n, bz));
            }
            frac = ang / (in->sel == MSL_SRM ? s_aseDegS : s_aseDegL);
            if (frac > 1.0f) frac = 1.0f;
            fx = cx; fy = cy;
            if (n > 1e-6f) { fx = cx + bx / n * frac * Rpx; fy = cy - by / n * frac * Rpx; }
            snap_xy(&s_st.dotSnap, fx, fy, &dx, &dy);
            hg_sprite(s_sprDot, (float)dx, (float)dy, col);
            s_dbg.dot = 1; s_dbg.dotX = dx; s_dbg.dotY = dy; s_dbg.dotFx = fx; s_dbg.dotFy = fy;
        }
        if (s_st.xOn) { hg_sprite(s_sprX, cx, cy, col); s_dbg.brk = 1; }
        {                                                        /* readouts right of the circle */
            int x1 = hg_px(RD_X) + hg_text_w(HF_SMALL, "0000"), y = rdY, dy = hg_px(RD_DY);   /* 4-digit numbers start at x 396 */
            draw_number_label(x1, y, s_st.vcStr, "Vc", 1, col);
            draw_number_label(x1, y + dy, s_st.rngStr, "RNG", 1, col);
            draw_number_label(x1, y + 2 * dy, third_num(), third_label(s_st.third), s_st.third == TL_TOF, col);
        }
    }
}

static void draw_cockpit(const HudState *hs, const WpnIn *in, int zone, u32 col)
{
    const HudLayout *lay = hud_layout();
    float k = hg_k(), cx = (float)hg_px(lay->aseX), cy = (float)hg_px(lay->aseY), Rpx = lay->aseR * k;
    if (zone) draw_ase(hs, in, cx, cy, hg_px(RD_Y), col);
    else if (in->sel == MSL_LRM && in->td < 0) {                 /* maddog: VISUAL */
        draw_dashed_circle(cx, cy, Rpx, col);
        hg_text(hg_px(320.0f), hg_px(VIS_Y), HF_SMALL, col, 1, "VISUAL");
        s_dbg.visual = 1;
    }
    draw_seeker(hs, in, col);
    draw_shoot(hs, in, zone, col);
    weapon_strings(in);
    hg_text(hg_px(320.0f), hg_px(WPN_Y), HF_SMALL, col, 1, s_st.wl);
}

/* T16b: chase circle centre (px): the steady aim cross from hud_chase_ase, y on a sticky whole pixel */
static void chase_centre(const HudState *hs, int *cx, int *cy)
{
    float xv, yv;
    int sx;
    hud_chase_ase(hs, &xv, &yv);
    snap_xy(&s_st.aseSnap, (float)hg_px(xv), yv * hg_k(), &sx, cy);
    *cx = hg_px(xv);
}

static void draw_chase(const HudState *hs, const WpnIn *in, int zone, u32 col)
{
    const DlzView *d = &in->dlz;
    float k = hg_k();
    if (s_chaseAse) {                                            /* T16b: the cockpit's circle on the steady aim cross */
        int cx, cy;
        chase_centre(hs, &cx, &cy);
        if (zone) {
            s_st.chRdY = cy + hg_px(CH_RD_OFF);
            draw_ase(hs, in, (float)cx, (float)cy, s_st.chRdY, col);
        } else if (in->sel == MSL_LRM && in->td < 0) {           /* maddog: dashed circle ("VISUAL" in the block) */
            draw_dashed_circle((float)cx, (float)cy, hud_layout()->aseR * k, col);
            s_dbg.aseCx = cx; s_dbg.aseCy = cy;
        }
    } else if (zone && in->tdBox) {                              /* T16: compact DLZ bar beside the TD box */
        float sc = s_scaleNm[s_st.scaleIdx] * NM2M, y0, y1, bx, dir, tick = BAR_TICK * k, yr;
        int off = hg_text_w(HF_LARGE, "SHOOT") / 2, need;
        if (in->tdHalf > off) off = in->tdHalf;
        off += hg_px(4.0f);
        need = in->tdX + off + e_ftoi(tick) + hg_px(7.0f) + s_ptrOy + hg_px(4.0f);
        if (!s_st.sideInit) { s_st.barLeft = need > s_W; s_st.sideInit = 1; }
        else if (!s_st.barLeft && need > s_W) s_st.barLeft = 1;
        else if (s_st.barLeft && need < s_W - hg_px(20.0f)) s_st.barLeft = 0;
        dir = s_st.barLeft ? -1.0f : 1.0f;
        bx = (float)(in->tdX + (s_st.barLeft ? -off : off));
        y0 = (float)in->tdY - BAR_HALF * k;
        y1 = (float)in->tdY + BAR_HALF * k;
        hg_line_w(bx, y0, bx, y1, 0.6f, col);
        if (d->ready && d->rmax > 0.0f) {
            float ymax = y1 - E_CLAMP(d->rmax / sc, 0.0f, 1.0f) * (y1 - y0), ymin = y1 - E_CLAMP(d->rmin / sc, 0.0f, 1.0f) * (y1 - y0);
            hg_line_ext(bx + dir * 3.0f * k, ymin, bx + dir * 3.0f * k, ymax, 0.0f, col);        /* the zone */
            hg_line_ext(bx, ymax, bx + dir * tick, ymax, 0.0f, col);                              /* Rmax */
            if (d->rne > 0.0f) {
                float yne = y1 - E_CLAMP(d->rne / sc, 0.0f, 1.0f) * (y1 - y0);
                hg_line_ext(bx, yne - 1.5f * k, bx + dir * tick, yne - 1.5f * k, 0.0f, col);      /* Rne double */
                hg_line_ext(bx, yne + 1.5f * k, bx + dir * tick, yne + 1.5f * k, 0.0f, col);
            }
            hg_line_ext(bx, ymin, bx + dir * tick, ymin, 0.0f, col);                              /* Rmin */
            s_dbg.ticks = 1;
        }
        yr = s_st.pinned ? y0 : y1 - E_CLAMP(d->range / sc, 0.0f, 1.0f) * (y1 - y0);
        if (!s_st.pinned || flash_on(hs->time, 3.0f))                                              /* range caret */
            hg_sprite_rot(s_sprPtr, bx + dir * (tick + 1.0f * k), yr, s_st.barLeft ? -0.5f * E_PI : 0.5f * E_PI, col);
        if (third_num()[0]) {
            char b[16];
            e_snprintf(b, (int)sizeof(b), s_st.third == TL_TOF ? "%s TOF" : "%s%s", third_num(), third_label(s_st.third));
            hg_text(e_ftoi(bx + dir * 3.0f * k), e_ftoi(y1 + 4.0f * k), HF_SMALL, col, 1, b);
        }
        s_dbg.bar = 1; s_dbg.barX = e_ftoi(bx); s_dbg.barY0 = e_ftoi(y0); s_dbg.barY1 = e_ftoi(y1);
        s_dbg.barLeft = s_st.barLeft; s_dbg.caretY = yr; s_dbg.circle = 0;
        s_dbg.scaleNm = s_scaleNm[s_st.scaleIdx];
    }
    if (zone && in->td >= 0 && !in->tdBox) s_dbg.bar = 0;
    draw_seeker(hs, in, col);
    draw_shoot(hs, in, zone, col);
    weapon_strings(in);
    {                                                            /* weapon block, bottom right */
        int x = hg_px(BLK_X), y = hg_px(BLK_Y);
        hg_text(x, y, HF_SMALL, col, 1, in->sel == MSL_SRM ? s_nameS : s_nameL);
        hg_text(x, y + hg_px(12.0f), HF_LARGE, col, 1, s_st.blkCountStr);
        if (in->flares >= 0) hg_text(x, y + hg_px(32.0f), HF_SMALL, col, 1, s_st.blkFlrStr);
        if (in->sel == MSL_SRM && s_st.seekLock >= 0) hg_text(x, y + hg_px(45.0f), HF_SMALL, col, 1, "TONE");
        else if (in->sel == MSL_LRM && in->td < 0) { hg_text(x, y + hg_px(45.0f), HF_SMALL, col, 1, "VISUAL"); s_dbg.visual = 1; }
    }
}

/* ================================================================ T18: text slots and keep-outs */
/* MISSILE / LOCK slots (virtual: text centre x, cap top y): home, left, right. The side slots sit at the home height
   (cockpit LOCK a little lower, clear of the bank scale ticks and the readout block); three slots ~110-150 apart so
   the TD cluster can never cover all of them. */
static const float s_slotMslCp[3][2]  = { { 320.0f, MSL_Y_CP }, { 212.0f, MSL_Y_CP }, { 428.0f, MSL_Y_CP } };
static const float s_slotLockCp[3][2] = { { 320.0f, LOCK_Y_CP }, { 236.0f, 334.0f }, { 404.0f, 334.0f } };
static const float s_slotMslCh[3][2]  = { { 320.0f, MSL_Y_CH }, { 170.0f, MSL_Y_CH }, { 470.0f, MSL_Y_CH } };
static const float s_slotLockCh[3][2] = { { 320.0f, LOCK_Y_CH }, { 170.0f, LOCK_Y_CH }, { 470.0f, LOCK_Y_CH } };

static void text_rect(int x, int y, int font, int align, const char *str, int *r)
{
    int w = hg_text_w(font, str), o = hg_shadow_off();
    r[0] = align == 1 ? x - w / 2 : (align == 2 ? x - w : x);
    r[1] = y; r[2] = r[0] + w + o; r[3] = y + hg_cap(font) + o;
}

static void slot_rect(const float (*sl)[2], int i, int font, const char *str, int *r) { text_rect(hg_px(sl[i][0]), hg_px(sl[i][1]), font, 1, str, r); }

static int rect_hit(const int *a, const int *b, int m)
{
    return a[2] + m > b[0] && a[0] - m < b[2] && a[3] + m > b[1] && a[1] - m < b[3];
}

/* the designated target's moving texts and symbols (px rects): TD box, SHOOT (with the gap to the box), the TD label,
   the chase DLZ bar with its third line */
static int td_cluster(const WpnIn *in, int zone, int (*cl)[4])
{
    int n = 0, o = hg_shadow_off(), r[4];
    if (in->tdBox) {
        cl[n][0] = in->tdX - in->tdHalf; cl[n][1] = in->tdY - in->tdHalf; cl[n][2] = in->tdX + in->tdHalf + o; cl[n][3] = in->tdY + in->tdHalf + o; n++;
        if (zone) {
            text_rect(s_dbg.shootX, s_dbg.shootY, HF_LARGE, 1, "SHOOT", cl[n]);
            if (s_dbg.shootBelow) cl[n][1] = in->tdY + in->tdHalf; else cl[n][3] = in->tdY - in->tdHalf;
            n++;
        }
    }
    {                                                            /* the label of this frame's TD box (hud_tgt) */
        int lx, ly;
        if (in->tdBox && hudtgt_td(&lx, &ly, NULL, NULL) && lx == in->tdX && ly == in->tdY && hudtgt_td_label(r)) { memcpy(cl[n], r, sizeof(r)); n++; }
    }
    if (s_dbg.bar) {
        int hw = E_MAXF(hg_px(14.0f), 3 * hg_adv(HF_SMALL));
        cl[n][0] = s_dbg.barX - hw; cl[n][2] = s_dbg.barX + hw + o;
        cl[n][1] = s_dbg.barY0 - hg_px(4.0f); cl[n][3] = s_dbg.barY1 + hg_px(4.0f) + hg_cap(HF_SMALL) + o; n++;
    }
    return n;
}

/* stay while the slot is clear (2 px); back home once home is clear by 8 px; else the clear side away from the TD */
static int pick_slot(int cur, const float (*sl)[2], int font, const char *str, const WpnIn *in, const int (*cl)[4], int ncl)
{
    int r[4], i, k, order[3], mNear = hg_px(2.0f), mFar = hg_px(8.0f), blocked;
    if (cur < 0 || cur > 2) cur = 0;
    if (cur != 0) {
        slot_rect(sl, 0, font, str, r);
        for (blocked = 0, i = 0; i < ncl; i++) if (rect_hit(r, cl[i], mFar)) blocked = 1;
        if (!blocked) return 0;
    }
    slot_rect(sl, cur, font, str, r);
    for (blocked = 0, i = 0; i < ncl; i++) if (rect_hit(r, cl[i], mNear)) blocked = 1;
    if (!blocked) return cur;
    order[0] = 0;
    order[1] = (in->tdBox && in->tdX < hg_px(320.0f)) ? 2 : 1;
    order[2] = 3 - order[1];
    for (k = 0; k < 3; k++) {
        if (order[k] == cur) continue;
        slot_rect(sl, order[k], font, str, r);
        for (blocked = 0, i = 0; i < ncl; i++) if (rect_hit(r, cl[i], mFar)) blocked = 1;
        if (!blocked) return order[k];
    }
    return cur;
}

/* the weapon HUD's fixed texts (px rects, worst-case widths so they never change with the numbers): cockpit readout
   block (zone) and weapon line (+ VISUAL); chase weapon block */
static int fixed_text_rects(const HudState *hs, const WpnIn *in, int zone, int (*r)[4])
{
    int n = 0, o = hg_shadow_off(), adv = hg_adv(HF_SMALL), capS = hg_cap(HF_SMALL), w, ws;
    char b[48];
    w = hg_text_w(HF_SMALL, s_nameL);
    ws = hg_text_w(HF_SMALL, s_nameS);
    if (ws > w) w = ws;
    if (hs->view != VIEW_CHASE) {
        if (zone) {
            int x1 = hg_px(RD_X) + hg_text_w(HF_SMALL, "0000");
            r[n][0] = hg_px(RD_X) - adv; r[n][1] = hg_px(RD_Y);
            r[n][2] = x1 + hg_gap(HF_SMALL) + adv + hg_text_w(HF_SMALL, "TTG") + o; r[n][3] = hg_px(RD_Y) + 2 * hg_px(RD_DY) + capS + o; n++;
        }
        e_snprintf(b, (int)sizeof(b), "%s 00", hg_text_w(HF_SMALL, s_nameL) >= hg_text_w(HF_SMALL, s_nameS) ? s_nameL : s_nameS);
        text_rect(hg_px(320.0f), hg_px(WPN_Y), HF_SMALL, 1, b, r[n]);
        if (!zone && in->sel == MSL_LRM && in->td < 0) {
            int v[4];
            text_rect(hg_px(320.0f), hg_px(VIS_Y), HF_SMALL, 1, "VISUAL", v);
            if (v[0] < r[n][0]) r[n][0] = v[0];
            if (v[2] > r[n][2]) r[n][2] = v[2];
            r[n][3] = v[3];
        }
        n++;
    } else {
        int x = hg_px(BLK_X), y = hg_px(BLK_Y);
        if (zone && s_chaseAse) {                                /* T16b: chase readout block (same widths as the cockpit) */
            int x1 = hg_px(RD_X) + hg_text_w(HF_SMALL, "0000");
            r[n][0] = hg_px(RD_X) - adv; r[n][1] = s_st.chRdY;
            r[n][2] = x1 + hg_gap(HF_SMALL) + adv + hg_text_w(HF_SMALL, "TTG") + o; r[n][3] = s_st.chRdY + 2 * hg_px(RD_DY) + capS + o; n++;
        }
        ws = hg_text_w(HF_SMALL, "VISUAL"); if (ws > w) w = ws;
        ws = hg_text_w(HF_SMALL, "FLR 00"); if (ws > w) w = ws;
        ws = hg_text_w(HF_LARGE, "00"); if (ws > w) w = ws;
        r[n][0] = x - w / 2; r[n][1] = y; r[n][2] = x - w / 2 + w + o; r[n][3] = y + hg_px(45.0f) + capS + o; n++;
    }
    return n;
}

/* forbidden interval of r for |a + b r| <= e (returns 0 = none; b ~ 0: all r or none) */
static int lin_iv(float a, float b, float e, float *lo, float *hi)
{
    if (e < 0.0f) return 0;
    if (fabs(b) < 1e-6f) { if (fabs(a) > e) return 0; *lo = -1e9f; *hi = 1e9f; return 1; }
    *lo = (-e - a) / b; *hi = (e - a) / b;
    if (*lo > *hi) { float q = *lo; *lo = *hi; *hi = q; }
    return 1;
}

/* smallest tip radius >= r0 at which the threat arrow clears every keep-out rect by marginV virtual units. The dart
   (tip on the ray at angle a, pointing out) is taken as its bounding rectangle: L long behind the tip, +-W wide
   (shadow included); separating axes x, y, along and across the ray give each rect a forbidden interval of r. */
static float arrow_radius(float cx, float cy, float a, float r0, const int (*ko)[4], int nko, float marginV)
{
    int w, h, ox, oy, i, it, moved, nI = 0;
    float dx = (float)sin(a), dy = -(float)cos(a), px = -dy, py = dx, L2, W, m = marginV * hg_k(), lo[12], hi[12], r = r0;
    float rMax = r0 + 60.0f * hg_k();
    if (!hg_sprite_info(s_sprThreat, &w, &h, &ox, &oy)) { ox = hg_px(19.0f); oy = hg_px(9.0f); }
    L2 = 0.5f * (float)ox + m;
    W = (float)(oy + hg_shadow_off()) + m;
    for (i = 0; i < nko && nI < 12; i++) {
        /* rect centre / half extents; arrow centre = C + d (r - L/2) */
        float bx = 0.5f * (float)(ko[i][0] + ko[i][2]), by = 0.5f * (float)(ko[i][1] + ko[i][3]);
        float hx = 0.5f * (float)(ko[i][2] - ko[i][0]), hy = 0.5f * (float)(ko[i][3] - ko[i][1]);
        float ax0 = cx - dx * (0.5f * (float)ox) - bx, ay0 = cy - dy * (0.5f * (float)ox) - by;   /* centre - B at r = 0 */
        float l = -1e9f, u = 1e9f, l1, u1;
        if (!lin_iv(ax0, dx, L2 * (float)fabs(dx) + W * (float)fabs(px) + hx, &l1, &u1)) continue;
        if (l1 > l) l = l1; if (u1 < u) u = u1;
        if (!lin_iv(ay0, dy, L2 * (float)fabs(dy) + W * (float)fabs(py) + hy, &l1, &u1)) continue;
        if (l1 > l) l = l1; if (u1 < u) u = u1;
        if (!lin_iv(ax0 * dx + ay0 * dy, 1.0f, L2 + hx * (float)fabs(dx) + hy * (float)fabs(dy), &l1, &u1)) continue;
        if (l1 > l) l = l1; if (u1 < u) u = u1;
        if (!lin_iv(ax0 * px + ay0 * py, 0.0f, W + hx * (float)fabs(px) + hy * (float)fabs(py), &l1, &u1)) continue;
        if (l > u) continue;
        lo[nI] = l; hi[nI] = u; nI++;
    }
    for (it = 0; it < 13; it++) {
        moved = 0;
        for (i = 0; i < nI; i++) if (r > lo[i] && r < hi[i]) { r = hi[i] + 0.5f; moved = 1; }
        if (!moved) break;
    }
    return r > rMax ? rMax : r;
}

/* T18: the weapon HUD's fixed texts of the last draw in this view (px), for the flight HUD's ladder keep-out */
int hudwpn_keepout(int (*box)[4], int maxBoxes, int view)
{
    int i;
    if (view != s_wkoView) return 0;
    for (i = 0; i < s_nwko && i < maxBoxes; i++) memcpy(box[i], s_wko[i], sizeof(s_wko[0]));
    return i;
}

static void draw_warn(const HudState *hs, const WpnIn *in, int zone, u32 red)
{
    int cockpit = hs->view != VIEW_CHASE, i, j, nearest = -1, used[WPN_THREATS_MAX], n;
    float cx = (float)hg_px(320.0f), cy = (float)hg_px(240.0f), r0 = s_ringR * hg_k(), r, dist[WPN_THREATS_MAX], hdg;
    int pSer[WPN_THREATS_MAX], pOn[WPN_THREATS_MAX], np = 0;
    int cl[6][4], ncl, ko[12][4], nko = 0, mr[4], lr[4];
    s_dbg.msl = s_st.mslOn;
    s_dbg.lock = s_st.lockOn;
    /* T18: MISSILE / LOCK move to a side slot while the TD cluster (box, SHOOT, label, chase bar) covers their home */
    ncl = td_cluster(in, zone, cl);
    s_st.mslSlot = pick_slot(s_st.mslSlot, cockpit ? s_slotMslCp : s_slotMslCh, HF_LARGE, "MISSILE", in, cl, ncl);
    s_st.lockSlot = pick_slot(s_st.lockSlot, cockpit ? s_slotLockCp : s_slotLockCh, HF_SMALL, "LOCK", in, cl, ncl);
    slot_rect(cockpit ? s_slotMslCp : s_slotMslCh, s_st.mslSlot, HF_LARGE, "MISSILE", mr);
    slot_rect(cockpit ? s_slotLockCp : s_slotLockCh, s_st.lockSlot, HF_SMALL, "LOCK", lr);
    s_dbg.mslSlot = s_st.mslSlot; s_dbg.lockSlot = s_st.lockSlot;
    memcpy(s_dbg.mslRect, mr, sizeof(mr)); memcpy(s_dbg.lockRect, lr, sizeof(lr));
    if (s_st.mslOn && flash_on(hs->time, 4.0f)) {
        const float *sl = (cockpit ? s_slotMslCp : s_slotMslCh)[s_st.mslSlot];
        hg_text(hg_px(sl[0]), hg_px(sl[1]), HF_LARGE, red, 1, "MISSILE");
        s_dbg.mslDrawn = 1;
    }
    if (s_st.lockOn) {
        const float *sl = (cockpit ? s_slotLockCp : s_slotLockCh)[s_st.lockSlot];
        hg_text(hg_px(sl[0]), hg_px(sl[1]), HF_SMALL, red, 1, "LOCK");
    }
    /* keep-out list: weapon texts (also for the flight HUD's ladder next frame) + the flight HUD's boxes */
    nko = fixed_text_rects(hs, in, zone, ko);
    if (s_st.mslOn && nko < 12) { memcpy(ko[nko], mr, sizeof(mr)); nko++; }
    if (s_st.lockOn && nko < 12) { memcpy(ko[nko], lr, sizeof(lr)); nko++; }
    for (s_nwko = 0; s_nwko < nko && s_nwko < 4; s_nwko++) memcpy(s_wko[s_nwko], ko[s_nwko], sizeof(ko[0]));
    s_wkoView = hs->view;
    nko += hud_keepout(ko + nko, 12 - nko, hs->view, hs->time);
    n = in->nThreats < WPN_THREATS_MAX ? in->nThreats : WPN_THREATS_MAX;
    if (n <= 0 || s_maxThreats <= 0) { s_st.nearSerial = -1; memset(s_st.pushOn, 0, sizeof(s_st.pushOn)); return; }
    for (i = 0; i < n; i++) { dist[i] = v3_len(v3_sub(in->threatPos[i], hs->pos)); used[i] = 0; }
    for (i = 0; i < n; i++) if (nearest < 0 || dist[i] < dist[nearest]) nearest = i;
    for (i = 0; i < n; i++)                                      /* keep the previous nearest unless 10 % beaten */
        if (in->threatSerial[i] == s_st.nearSerial && dist[i] < 1.1f * dist[nearest]) nearest = i;
    s_st.nearSerial = in->threatSerial[nearest];
    hdg = E_DEG2RAD(hs->headingDeg);
    for (j = 0; j < s_maxThreats && j < n; j++) {
        int pick = -1;
        float brg, a;
        if (j == 0) pick = nearest;
        else for (i = 0; i < n; i++) if (!used[i] && (pick < 0 || dist[i] < dist[pick])) pick = i;
        if (pick < 0) break;
        used[pick] = 1;
        {
            Vec3 to = v3_sub(in->threatPos[pick], hs->pos);
            brg = (float)atan2(to.x, to.z) - hdg;                /* RWR style: plan view, 12 o'clock = ahead */
        }
        a = brg;
        {                                                        /* T18: off the texts, along the bearing, hysteresis */
            int ser = in->threatSerial[pick], on = 0, q;
            float rOff = r0;
            for (q = 0; q < WPN_THREATS_MAX; q++) if (s_st.pushOn[q] && s_st.pushSerial[q] == ser) on = 1;
            if (!on && arrow_radius(cx, cy, a, r0, ko, nko, 0.0f) > r0) on = 1;   /* on: touching at the ring */
            if (on) {                                            /* placed 4 clear; off once the ring is 4 clear */
                rOff = arrow_radius(cx, cy, a, r0, ko, nko, 4.0f);
                if (rOff <= r0) on = 0;
            }
            r = on ? rOff : r0;
            if (on) s_dbg.arrowPushed++;
            if (np < WPN_THREATS_MAX) { pSer[np] = ser; pOn[np] = on; np++; }
        }
        hg_sprite_rot(s_sprThreat, cx + (float)sin(a) * r, cy - (float)cos(a) * r, a - 0.5f * E_PI,
                      j == 0 ? red : hg_col(COL_RED, s_threatAlpha));
        if (s_dbg.arrows < 8) { s_dbg.arrowX[s_dbg.arrows] = cx + (float)sin(a) * r; s_dbg.arrowY[s_dbg.arrows] = cy - (float)cos(a) * r; }
        s_dbg.arrows++;
    }
    memset(s_st.pushOn, 0, sizeof(s_st.pushOn));
    for (i = 0; i < np; i++) { s_st.pushSerial[i] = pSer[i]; s_st.pushOn[i] = pOn[i]; }
}

/* ================================================================ core + live path */
void hudwpn_draw_in(const HudState *hs, const WpnIn *in)
{
    s64 t0 = plat_qpc();
    const DlzView *d = &in->dlz;
    float t = hs->time, br;
    int zone;
    u32 col, red;
    r_screen_size(&s_W, &s_H);
    if (s_W <= 0 || s_H <= 0) { s_W = 640; s_H = 480; }
    sprites_init();
    br = s_brightP ? *s_brightP : 1.0f;
    col = hg_col(COL_GREEN, br);
    red = hg_col(COL_RED, 1.0f);
    memset(&s_dbg, 0, sizeof(s_dbg));
    if (in->td != s_st.lastTd || in->sel != s_st.lastSel) zone_reset();
    s_st.lastTd = in->td;
    s_st.lastSel = in->sel;
    zone = in->td >= 0 && d->valid && d->target == in->td && d->type == in->sel;
    s_dbg.zone = zone;
    decide_zone(t, in, zone);
    decide_readouts(t, in, zone);
    decide_seeker(t, in);
    decide_warn(t, in);
    hg_begin();
    if (hs->view == VIEW_CHASE) draw_chase(hs, in, zone, col);
    else draw_cockpit(hs, in, zone, col);
    draw_warn(hs, in, zone, red);
    hg_end();
    s_quads = hg_quads_last();
    s_draws = hg_draws_last();
    s_dbg.third = zone ? s_st.third : TL_NONE;
    e_strlcpy(s_dbg.vc, s_st.vcStr, (int)sizeof(s_dbg.vc));
    e_strlcpy(s_dbg.rng, s_st.rngStr, (int)sizeof(s_dbg.rng));
    e_snprintf(s_dbg.thirdStr, (int)sizeof(s_dbg.thirdStr), "%s%s", zone ? third_num() : "", zone && s_st.third ? third_label(s_st.third) : "");
    s_ms = (float)((double)(plat_qpc() - t0) * 1000.0 / (double)plat_qpf());
}

void hudwpn_build(const Game *g, float alpha, const HudState *hs, WpnIn *in)
{
    const WpnState *ws = weapons_state(PLAYER_ID);
    MissileView mv[MAX_MISSILES];
    int nm, i, j, slots[WPN_THREATS_MAX], nt;
    (void)hs;
    memset(in, 0, sizeof(*in));
    in->td = -1;
    in->srmLock = -1;
    in->mslAct = in->mslTtg = -1.0f;
    in->flares = -1;
    if (ws) {
        in->sel = ws->sel; in->lrm = ws->lrm; in->srm = ws->srm; in->flares = ws->flares;
        in->srmLock = ws->srmLock; in->srmLockTd = ws->srmLockTd; in->srmNear = ws->srmNear;
    }
    in->td = radar_designated(PLAYER_ID);
    if (in->td >= 0) {
        ContactView cv;
        if (radar_view_get(g, PLAYER_ID, in->td, &cv)) {
            radar_view_pos(g, &cv, alpha, &in->tdPos);
            in->tdHeading = cv.heading;
            in->tdMemory = (cv.flags & CVF_LOCKMEM) != 0 || cv.state == CS_MEMORY;
        } else in->tdMemory = 1;
    }
    in->tdBox = hudtgt_td(&in->tdX, &in->tdY, &in->tdClamped, &in->tdHalf);
    in->dlz = *dlz_view();
    nm = weapons_missiles(mv, MAX_MISSILES);
    if (in->td >= 0) {
        int slot = weapons_latest_at(PLAYER_ID, in->td);
        for (i = 0; i < nm; i++) if (slot >= 0 && mv[i].slot == slot) {
            in->msl = 1; in->mslType = mv[i].type; in->mslSerial = mv[i].serial;
            in->mslAct = mv[i].act; in->mslTtg = mv[i].ttg;
        }
    }
    if (in->srmLock >= 0 && in->srmLock < g->numAircraft) {
        const Aircraft *a = &g->ac[in->srmLock];
        in->srmLockPos = v3_lerp(a->prev.pos, a->fs.pos, alpha);
    } else in->srmLock = -1;
    in->rwrLock = radar_locked_by(PLAYER_ID) != 0;
    nt = weapons_threats(PLAYER_ID, slots, WPN_THREATS_MAX);
    for (j = 0; j < nt; j++) for (i = 0; i < nm; i++) if (mv[i].slot == slots[j]) {
        float dist = v3_len(v3_sub(mv[i].pos, g->ac[PLAYER_ID].fs.pos));
        if (in->nThreats < WPN_THREATS_MAX && radar_rwr_missile_warn(mv[i].type, mv[i].launchDist, dist)) {
            in->threatPos[in->nThreats] = v3_lerp(mv[i].prevPos, mv[i].pos, alpha);
            in->threatSerial[in->nThreats] = (int)mv[i].serial;
            in->nThreats++;
        }
    }
}

void hudwpn_draw(const Game *g, const Camera *c, float alpha)
{
    HudState hs;
    WpnIn in;
    hud_state_fill(&hs, g, c, alpha);
    hudwpn_build(g, alpha, &hs, &in);
    hudwpn_draw_in(&hs, &in);
}

/* ================================================================ -hudtest zone (DESIGN 10.8) */
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

/* synthetic cockpit/chase camera (camera.cpp numbers; eye + forward x 1000: no float quantisation) */
static void ts_cam(HudState *hs, int view, Vec3 pos, Quat qn)
{
    Vec3 nf = q_rotate(qn, v3(0.0f, 0.0f, 1.0f)), nu = q_rotate(qn, v3(0.0f, 1.0f, 0.0f)), eye, at;
    float fov = view == VIEW_CHASE ? 70.0f : 65.0f;
    if (view == VIEW_CHASE) { eye = v3_add(v3_madd(pos, nf, -24.0f), v3_scale(nu, 6.0f)); at = v3_madd(pos, nf, 40.0f); }
    else { eye = pos; at = v3_madd(eye, nf, 1000.0f); }
    hs->eye = eye;
    m4_look_lh(&hs->viewM, eye, at, nu);
    m4_persp_lh(&hs->projM, 2.0f * (float)atan(tan(E_DEG2RAD(fov) * 0.5f) * 0.75f), 4.0f / 3.0f, 1.0f, 18000.0f);
}

static void ts_state(HudState *hs, int view, Vec3 pos, float hdgDeg, float pitchDeg, float bankDeg, float aoaDeg)
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
    hs->flares = -1;
    hs->speedKt = 482.0f; hs->g = 1.0f; hs->mach = 0.76f; hs->vsFpm = 0.0f;
    ts_cam(hs, view, pos, qn);
}

/* direction az/el degrees off the camera forward (camera frame) */
static Vec3 cam_dir(const HudState *hs, float azDeg, float elDeg)
{
    Vec3 r, u, f;
    cam_basis(hs, &r, &u, &f);
    return v3_norm(v3_add(f, v3_add(v3_scale(r, (float)tan(E_DEG2RAD(azDeg))), v3_scale(u, (float)tan(E_DEG2RAD(elDeg))))));
}

/* ---- synthetic launch zone: published like dlz.cpp (cycles, smoothing up 50 %, NOW/NOWESC one cycle late) */
typedef struct SynZone {
    float rmaxT, rneT, rminT, cycle, nextPub, snapR, noise;
    int cycles; u32 rng;
    DlzView v;
} SynZone;

static float syn_rand(u32 *s) { *s = *s * 1664525u + 1013904223u; return (float)((*s >> 8) & 0xFFFF) / 32767.5f - 1.0f; }

static void syn_start(SynZone *z, int type, float rmax, float rne, float rmin, float t, u32 seed, float noise)
{
    memset(z, 0, sizeof(*z));
    z->rmaxT = rmax; z->rneT = rne; z->rminT = rmin;
    z->cycle = 0.66f; z->nextPub = t + 0.15f; z->rng = seed; z->noise = noise; z->snapR = -1.0f;
    z->v.valid = 1; z->v.target = 1; z->v.type = type;
    z->v.rmax = rmax * 1.08f; z->v.rne = rne * 0.9f; z->v.rmin = rmin * 1.1f;      /* table seed */
    z->v.srcMax = z->v.srcNe = z->v.srcMin = DLZV_TABLE;
    z->v.tof = -1.0f;
}

static void syn_pub(float *v, int *src, float nv)
{
    if (*src != DLZV_LIVE || nv < *v) *v = nv;
    else *v += 0.5f * (nv - *v);
    *src = DLZV_LIVE;
}

static void syn_step(SynZone *z, float t, float R, float closure, Vec3 lead)
{
    DlzView *v = &z->v;
    v->range = R;
    v->closure = closure;
    if (t >= z->nextPub) {
        syn_pub(&v->rmax, &v->srcMax, z->rmaxT * (1.0f + z->noise * syn_rand(&z->rng)));
        syn_pub(&v->rne, &v->srcNe, z->rneT * (1.0f + z->noise * syn_rand(&z->rng)));
        syn_pub(&v->rmin, &v->srcMin, z->rminT * (1.0f + z->noise * syn_rand(&z->rng)));
        v->ready = 1;
        if (z->snapR > 0.0f) {
            v->nowValid = 1;
            v->nowHit = z->snapR <= z->rmaxT && z->snapR > z->rminT;
            v->tof = v->nowHit ? z->snapR / 900.0f + 1.0f : -1.0f;
            v->nowEscValid = 1;
            v->nowNoEscape = v->nowHit && z->snapR <= z->rneT;
            v->nowEscFlown = 1;
        }
        z->snapR = R;
        z->cycles++;
        z->nextPub = t + z->cycle;
        v->cycles = (u32)z->cycles;
        v->cycleSec = z->cycle;
    }
    if (v->rne > v->rmax) v->rne = v->rmax;
    v->inRmax = v->rmax > 0.0f && R <= v->rmax && R > v->rmin;
    v->inRne = v->rne > 0.0f && R <= v->rne && R > v->rmin;
    v->inRmin = v->rmax > 0.0f && R <= v->rmin;
    v->leadDir = lead;
    v->leadValid = 1;
}

/* independent oracle of the honesty rule (hud_wpn.h) */
static int oracle_shoot(const DlzView *v, const WpnIn *in)
{
    return v->valid && v->ready && v->rmax > 0.0f && v->range <= v->rmax && v->range > v->rmin && !(v->nowValid && !v->nowHit) &&
           (in->sel != MSL_SRM || (in->srmLock >= 0 && in->srmLock == in->td));
}
static int oracle_ne(const DlzView *v, const WpnIn *in)
{
    return oracle_shoot(v, in) && v->rne > 0.0f && v->range <= v->rne && !(v->nowEscValid && !v->nowNoEscape) && !v->memory && !in->tdMemory;
}

/* displayed-state segments: changes + shortest hold */
typedef struct SegT { int init, val, changes; float since, minHold; } SegT;
static void seg_track(SegT *s, float t, int val)
{
    if (!s->init) { s->init = 1; s->val = val; s->since = t; s->minHold = 1e9f; return; }
    if (val == s->val) return;
    if (t - s->since < s->minHold) s->minHold = t - s->since;
    s->val = val; s->since = t; s->changes++;
}

static int s_boxHalf = 13;
static void syn_in(WpnIn *in, const HudState *hs, int sel, float R, float azDeg, float elDeg, const DlzView *v)
{
    float x, y;
    memset(in, 0, sizeof(*in));
    in->sel = sel; in->lrm = 16; in->srm = 40; in->flares = 40;
    in->td = v ? 1 : -1;
    in->srmLock = -1;
    in->mslAct = in->mslTtg = -1.0f;
    in->tdPos = v3_madd(hs->pos, cam_dir(hs, azDeg, elDeg), R);
    in->tdHeading = E_DEG2RAD(hs->headingDeg) + E_PI;
    if (v && hud_project_point(hs, in->tdPos, &x, &y)) { in->tdBox = 1; in->tdX = e_ftoi(x); in->tdY = e_ftoi(y); in->tdHalf = s_boxHalf; }
    if (v) in->dlz = *v;
    else { in->dlz.target = -1; in->dlz.tof = -1.0f; }
}

/* ---- part 1: closing through Rmax -> Rne -> Rmin: honesty, transitions, latency */
static int test_approach(int sel, int view)
{
    HudState hs;
    WpnIn in;
    SynZone z;
    SegT seg;
    float t = 100.0f, R, Vc, rmax, rne, rmin, tRmax = -1.0f, tShoot = -1.0f, tRne = -1.0f, tFlash = -1.0f, tX = -1.0f, tRmin = -1.0f;
    int viol = 0, violNe = 0, frames = 0, fails = 0, prevTicks = 0;
    u32 jr = 12345u + (u32)sel;
    memset(&seg, 0, sizeof(seg));
    hudwpn_reset();
    hs.time = t;
    ts_state(&hs, view, v3(2000.0f, 5000.0f, -3000.0f), 30.0f, 1.0f, 0.0f, 1.0f);
    if (sel == MSL_SRM) { R = 4.5f * NM2M; Vc = 250.0f; rmax = 5000.0f; rne = 2700.0f; rmin = 350.0f; }
    else { R = 30.0f * NM2M; Vc = 600.0f; rmax = 19400.0f; rne = 7600.0f; rmin = 800.0f; }
    syn_start(&z, sel, rmax, rne, rmin, t, 777u + (u32)sel, 0.02f);
    while (R > 0.4f * rmin) {
        float dt = (1.0f / 60.0f) * (1.0f + 0.2f * syn_rand(&jr));
        int sh;
        t += dt; R -= Vc * dt;
        hs.time = t;
        syn_step(&z, t, R, Vc, cam_dir(&hs, 2.0f, -1.0f));
        syn_in(&in, &hs, sel, R, 2.0f, -1.0f, &z.v);
        if (sel == MSL_SRM && R < 5200.0f) { in.srmLock = 1; in.srmLockTd = 1; in.srmLockPos = in.tdPos; }
        hudwpn_draw_in(&hs, &in);
        sh = s_dbg.shoot;
        if (sh >= 1 && !oracle_shoot(&in.dlz, &in)) viol++;
        if (sh == 2 && !oracle_ne(&in.dlz, &in)) violNe++;
        if (sh >= 1 && s_dbg.brk) viol++;
        if (s_dbg.ticks && !prevTicks && !in.dlz.ready) viol++;
        prevTicks = s_dbg.ticks;
        seg_track(&seg, t, sh);
        if (tRmax < 0.0f && R <= rmax) tRmax = t;
        if (tShoot < 0.0f && sh >= 1) tShoot = t;
        if (tRne < 0.0f && R <= rne) tRne = t;
        if (tFlash < 0.0f && sh == 2) tFlash = t;
        if (tX < 0.0f && s_dbg.brk) tX = t;
        if (tRmin < 0.0f && R <= rmin) tRmin = t;
        frames++;
    }
    log_printf(LOG_INFO, "hud: zone approach %s %s frames=%d violations=%d/%d changes=%d minHold=%.2f s | SHOOT %.2f s after Rmax, flashing %.2f s after Rne, X %.2f s before Rmin (scale %.0f NM, changes %d)",
               view == VIEW_CHASE ? "chase" : "cockpit", sel == MSL_SRM ? "srm" : "lrm", frames, viol, violNe, seg.changes, (double)seg.minHold, (double)(tShoot - tRmax),
               (double)(tFlash - tRne), (double)(tRmin - tX), (double)s_scaleNm[s_st.scaleIdx], s_st.scaleChanges);
    if (viol || violNe) { TFAIL("hud: FAIL zone approach: SHOOT shown when the published zone said no (%d/%d)", viol, violNe); fails++; }
    if (tShoot < 0.0f || tFlash < 0.0f || tX < 0.0f) { TFAIL("hud: FAIL zone approach: missing state shoot=%.1f flash=%.1f x=%.1f", (double)tShoot, (double)tFlash, (double)tX); fails++; }
    if (tShoot - tRmax > 3.0f || tFlash - tRne > 3.0f || tShoot < tRmax || tFlash < tRne) { TFAIL("hud: FAIL zone approach: latency"); fails++; }
    if (seg.changes > 4 || seg.minHold < 0.3f) { TFAIL("hud: FAIL zone approach: flicker changes=%d minHold=%.2f", seg.changes, (double)seg.minHold); fails++; }
    return fails;
}

/* ---- part 2: hovering on a boundary with published-value noise: still honest, no flicker */
static int test_hover(int which, int view)
{
    static const char *const names[3] = { "rmax", "rne", "rmin" };
    HudState hs;
    WpnIn in;
    SynZone z;
    SegT seg, segX;
    float t = 200.0f, t0, B;
    int viol = 0, frames = 0, fails = 0, shortOn = 0, lastOnStart = -1;
    float onStart = 0.0f;
    u32 jr = 999u + (u32)which;
    memset(&seg, 0, sizeof(seg));
    memset(&segX, 0, sizeof(segX));
    hudwpn_reset();
    hs.time = t;
    ts_state(&hs, view, v3(-8000.0f, 3000.0f, 4000.0f), 300.0f, 0.0f, 0.0f, 2.0f);
    syn_start(&z, MSL_LRM, 19400.0f, 7600.0f, 800.0f, t, 4242u + (u32)which, 0.02f);
    B = which == 0 ? z.rmaxT : (which == 1 ? z.rneT : z.rminT);
    t0 = t;
    while (t < t0 + 40.0f) {
        float dt = (1.0f / 60.0f) * (1.0f + 0.2f * syn_rand(&jr)), R;
        t += dt;
        hs.time = t;
        R = B * (1.0f + 0.05f * (float)sin(2.0f * E_PI * (t - t0) / 8.0f));
        syn_step(&z, t, R, 20.0f, hs.noseDir);
        syn_in(&in, &hs, MSL_LRM, R, 0.0f, 0.0f, &z.v);
        hudwpn_draw_in(&hs, &in);
        if (s_dbg.shoot >= 1 && !oracle_shoot(&in.dlz, &in)) viol++;
        if (s_dbg.shoot == 2 && !oracle_ne(&in.dlz, &in)) viol++;
        if (t - t0 > 1.0f) {
            int on = s_dbg.shoot >= 1;
            if (on && lastOnStart != 1) { onStart = t; lastOnStart = 1; }
            if (!on && lastOnStart == 1) { if (t - onStart < 0.25f) shortOn++; lastOnStart = 0; }
            seg_track(&seg, t, s_dbg.shoot);
            seg_track(&segX, t, s_dbg.brk);
        }
        frames++;
    }
    log_printf(LOG_INFO, "hud: zone hover %s %s frames=%d violations=%d shoot changes=%d minHold=%.2f s shortOn=%d | X changes=%d minHold=%.2f s",
               view == VIEW_CHASE ? "chase" : "cockpit", names[which], frames, viol, seg.changes, (double)(seg.changes ? seg.minHold : 0.0f), shortOn, segX.changes,
               (double)(segX.changes ? segX.minHold : 0.0f));
    if (viol) { TFAIL("hud: FAIL zone hover %s: %d frames SHOOT without the zone", names[which], viol); fails++; }
    if (shortOn || (seg.changes && seg.minHold < 0.2f) || seg.changes > 30) { TFAIL("hud: FAIL zone hover %s: flicker", names[which]); fails++; }
    if (segX.changes && segX.minHold < 0.3f) { TFAIL("hud: FAIL zone hover %s: X flicker", names[which]); fails++; }
    return fails;
}

/* ---- part 3: ACT counts down whole seconds and switches to TTG at activation; TOF rate */
static int test_countdown(int view)
{
    HudState hs;
    WpnIn in;
    SynZone z;
    float t = 300.0f, tL, actEst = 0.0f, ttgEst = 0.0f, nextEst = 0.0f, lastChange = -1.0f, minIv = 1e9f, maxIv = 0.0f;
    float tofLast = -1.0f, tofMinIv = 1e9f;
    int fails = 0, frames = 0, ups = 0, bigDown = 0, switchFrame = -1, actFrame = -1, prevVal = -1, prevKind = -1, off = 0, badKind = 0;
    int tofChanges = 0, actChanges = 0, notDrawn = 0;
    char prevTof[16];
    u32 jr = 31337u, nr = 4711u;
    prevTof[0] = 0;
    hudwpn_reset();
    hs.time = t;
    ts_state(&hs, view, v3(0.0f, 6000.0f, 0.0f), 90.0f, 0.0f, 0.0f, 2.0f);
    syn_start(&z, MSL_LRM, 19400.0f, 7600.0f, 800.0f, t, 99u, 0.01f);
    tL = t + 6.0f;                                  /* TOF before, launch at tL: act 17.4 s, then ttg 11.2 s */
    while (t < tL + 17.4f + 11.2f + 1.0f) {
        float dt = (1.0f / 60.0f) * (1.0f + 0.25f * syn_rand(&jr)), R = E_MAXF(9.0f * NM2M - 600.0f * (t - 300.0f), 300.0f), actT, ttgT;
        int kind, val;
        t += dt;
        hs.time = t;
        syn_step(&z, t, R, 400.0f, hs.noseDir);
        syn_in(&in, &hs, MSL_LRM, R, 1.0f, 0.0f, &z.v);
        actT = 17.4f - (t - tL);
        ttgT = 11.2f - (t - tL - 17.4f);
        if (t >= nextEst) { actEst = actT + 0.3f * syn_rand(&nr); ttgEst = ttgT + 0.4f * syn_rand(&nr); nextEst = t + 0.1f; }
        if (t >= tL && ttgT > 0.0f) {
            in.msl = 1; in.mslType = MSL_LRM; in.mslSerial = 77u;
            in.mslAct = actT > 0.0f ? E_MAXF(actEst, 0.0f) : -1.0f;
            in.mslTtg = ttgT > 0.0f ? E_MAXF(ttgEst, 0.0f) : -1.0f;
        }
        hudwpn_draw_in(&hs, &in);
        kind = s_dbg.third;
        val = atoi(s_dbg.thirdStr);
        if (kind != TL_NONE && s_dbg.rdY1 < s_dbg.rdY0 + 2 * hg_px(RD_DY)) notDrawn++;   /* third row really drawn (T16b) */
        if (kind == TL_TOF) {
            if (strcmp(prevTof, s_dbg.thirdStr)) {
                if (prevTof[0] && tofLast > 0.0f && t - tofLast < tofMinIv) tofMinIv = t - tofLast;
                tofLast = t; tofChanges++;
                e_strlcpy(prevTof, s_dbg.thirdStr, (int)sizeof(prevTof));
            }
        }
        if (in.msl) {
            int want = in.mslAct >= 0.0f ? TL_ACT : TL_TTG;
            if (kind != want) badKind++;
            if (want == TL_ACT && actFrame < 0) actFrame = frames;
            if (want == TL_TTG && switchFrame < 0) {
                switchFrame = frames;
                if (kind != TL_TTG) { TFAIL("hud: FAIL zone countdown: no TTG on the activation frame"); fails++; }
            }
            if (kind == prevKind && (kind == TL_ACT || kind == TL_TTG) && val != prevVal) {
                float iv = t - lastChange;
                if (val > prevVal) ups++;
                if (val < prevVal - 1) bigDown++;
                if (lastChange > 0.0f) { if (iv < minIv) minIv = iv; if (iv > maxIv) maxIv = iv; }
                lastChange = t;
                if (kind == TL_ACT) actChanges++;
            }
            if (kind != prevKind) lastChange = -1.0f;
            if (kind == TL_ACT && (val > (int)ceil(actT) + 1 || val < (int)ceil(actT) - 1)) off++;
            if (kind == TL_TTG && in.mslTtg >= 0.0f && (val > (int)ceil(ttgT) + 1 || val < (int)ceil(ttgT) - 1)) off++;
        }
        if (frames < 3 || (in.msl && kind != prevKind) || (kind == TL_ACT && val != prevVal && (val % 5) == 0))
            log_printf(LOG_DEBUG, "hud: zone countdown t=%.2f third=\"%s\" act=%.2f ttg=%.2f", (double)(t - tL), s_dbg.thirdStr, (double)in.mslAct, (double)in.mslTtg);
        prevKind = kind; prevVal = val;
        frames++;
    }
    log_printf(LOG_INFO, "hud: zone countdown %s frames=%d act changes=%d ups=%d bigDown=%d interval %.2f..%.2f s off=%d wrongKind=%d switch at frame %d (activation) | TOF changes=%d min interval %.2f s",
               view == VIEW_CHASE ? "chase" : "cockpit", frames, actChanges, ups, bigDown, (double)minIv, (double)maxIv, off, badKind, switchFrame, tofChanges, (double)tofMinIv);
    if (notDrawn) log_printf(LOG_INFO, "hud: zone countdown third line not drawn in %d frames", notDrawn);
    if (ups || badKind || off || minIv < 0.45f || maxIv > 1.75f || actChanges < 14 || notDrawn) { TFAIL("hud: FAIL zone countdown"); fails++; }
    if (tofChanges < 2 || tofMinIv < 0.45f) { TFAIL("hud: FAIL zone TOF rate (changes %d, min %.2f s)", tofChanges, (double)tofMinIv); fails++; }
    return fails;
}

/* ---- part 4: SRM seeker latch + slaving, warnings */
static int test_seeker_warn(void)
{
    HudState hs;
    WpnIn in;
    SegT segL, segLock;
    float t = 400.0f, t0, maxSlave = 0.0f;
    int fails = 0, toneBad = 0, flashOut = 0, clampOk = 1, mslBad = 0, arrowBad = 0, mslDrawnOn = 0, mslFrames = 0;
    memset(&segL, 0, sizeof(segL));
    memset(&segLock, 0, sizeof(segLock));
    hudwpn_reset();
    hs.time = t;
    ts_state(&hs, VIEW_COCKPIT, v3(0.0f, 4000.0f, 0.0f), 0.0f, 0.0f, 0.0f, 0.0f);
    t0 = t;
    while (t < t0 + 4.0f) {
        float tt;
        t += 1.0f / 60.0f;
        tt = t - t0;
        hs.time = t;
        syn_in(&in, &hs, MSL_SRM, 3000.0f, 0.0f, 0.0f, NULL);
        in.srmNear = 1;
        if (tt < 1.0f) in.srmLock = (((int)(tt * 10.0f)) & 1) ? 2 : -1;       /* raw lock flickering at 10 Hz */
        else if (tt < 2.5f) in.srmLock = 2;
        else if (tt < 3.5f) in.srmLock = 3;
        in.srmLockPos = v3_madd(hs.pos, cam_dir(&hs, in.srmLock == 3 ? 30.0f : 2.0f, 1.0f), 2500.0f);
        hudwpn_draw_in(&hs, &in);
        if (tt > 0.05f) seg_track(&segL, t, s_dbg.seekLock);
        if (s_st.tones.srmLock != (s_dbg.seekLock >= 0) || (s_st.tones.growl && s_dbg.seekLock >= 0)) toneBad++;
        if (tt > 1.6f && tt < 2.5f) {
            float ex, ey;
            if (hud_project_point(&hs, in.srmLockPos, &ex, &ey)) {
                float dd = (float)sqrt(((float)s_dbg.seekX - ex) * ((float)s_dbg.seekX - ex) + ((float)s_dbg.seekY - ey) * ((float)s_dbg.seekY - ey));
                if (dd > maxSlave) maxSlave = dd;
            }
        }
        if (tt > 3.0f && tt < 3.5f) {
            const HudLayout *lay = hud_layout();
            if (s_dbg.seekFlash) flashOut++;
            if (s_dbg.seekX > hg_px(lay->frameX1) + 1) clampOk = 0;
        }
    }
    log_printf(LOG_INFO, "hud: zone seeker lock changes=%d minHold=%.2f s toneMismatch=%d slaved max %.2f px, outside frame flashing %d frames clamped=%d",
               segL.changes, (double)segL.minHold, toneBad, (double)maxSlave, flashOut, clampOk);
    if (segL.minHold < 0.3f - 1e-3f || segL.changes > 6 || toneBad || maxSlave > 1.3f || flashOut < 20 || !clampOk) { TFAIL("hud: FAIL zone seeker"); fails++; }
    /* warnings */
    hudwpn_reset();
    t0 = t;
    while (t < t0 + 6.0f) {
        float tt;
        t += 1.0f / 60.0f;
        tt = t - t0;
        hs.time = t;
        syn_in(&in, &hs, MSL_LRM, 3000.0f, 0.0f, 0.0f, NULL);
        if (tt >= 1.0f && tt < 3.0f) {
            in.nThreats = 2;
            in.threatPos[0] = v3_add(hs.pos, v3(4000.0f, 0.0f, 0.0f));   /* heading 0: 90 deg right */
            in.threatPos[1] = v3_add(hs.pos, v3(-3000.0f, 500.0f, -6000.0f));
            in.threatSerial[0] = 11; in.threatSerial[1] = 12;
        }
        in.rwrLock = tt >= 1.0f && tt < 3.0f && ((int)(tt * 5.0f) & 1);        /* lock flickering at 5 Hz */
        hudwpn_draw_in(&hs, &in);
        seg_track(&segLock, t, s_dbg.lock);
        if ((tt >= 1.0f && tt < 3.0f && !s_dbg.msl) || (tt > 3.55f && s_dbg.msl) || (tt < 1.0f && s_dbg.msl)) mslBad++;
        if (s_dbg.msl) { mslFrames++; if (s_dbg.mslDrawn) mslDrawnOn++; }
        if (tt >= 1.0f && tt < 3.0f) {
            float ex = (float)hg_px(320.0f) + s_ringR * hg_k(), ey = (float)hg_px(240.0f);
            if (s_dbg.arrows != 2 || (float)fabs(s_dbg.arrowX[0] - ex) > 1.0f || (float)fabs(s_dbg.arrowY[0] - ey) > 1.0f) arrowBad++;
        }
    }
    log_printf(LOG_INFO, "hud: zone warn MISSILE wrong=%d drawn %d/%d frames (4 Hz) arrows wrong=%d | LOCK changes=%d minHold=%.2f s",
               mslBad, mslDrawnOn, mslFrames, arrowBad, segLock.changes, (double)segLock.minHold);
    if (mslBad || arrowBad || mslDrawnOn < mslFrames / 3 || mslDrawnOn > (2 * mslFrames) / 3 + 2 || segLock.minHold < 0.3f - 1e-3f || segLock.changes > 6) {
        TFAIL("hud: FAIL zone warnings");
        fails++;
    }
    return fails;
}

/* ---- part 5: geometry: ASE reach vs the boxes (user rule), readouts, steering dot, side switches */
static void box_px(int digits, float xv, int rightEdge, float ycv, int *b)   /* = T07's box_geom */
{
    const Param *ppx = params_find("hud", "boxPadX"), *ppy = params_find("hud", "boxPadY");
    float k = hg_k();
    int padX = e_ftoi((ppx ? *(const float *)ppx->ptr : 3.0f) * k), padY = e_ftoi((ppy ? *(const float *)ppy->ptr : 2.5f) * k), w, h, yc;
    if (padX < 2) padX = 2;
    if (padY < 2) padY = 2;
    w = digits * hg_adv(HF_LARGE) - hg_gap(HF_LARGE) + 2 * padX + 2 * hg_stroke();
    h = hg_cap(HF_LARGE) + 2 * padY + 2 * hg_stroke();
    if (rightEdge) { b[2] = e_ftoi(xv * k); b[0] = b[2] - w; }
    else { b[0] = e_ftoi(xv * k); b[2] = b[0] + w; }
    yc = e_ftoi(ycv * k);
    b[1] = yc - h / 2;
    b[3] = b[1] + h;
}

static float rect_dist(const int *b, float cx, float cy)
{
    float dx = E_MAXF(E_MAXF((float)b[0] - cx, 0.0f), cx - (float)b[2]);
    float dy = E_MAXF(E_MAXF((float)b[1] - cy, 0.0f), cy - (float)b[3]);
    return (float)sqrt(dx * dx + dy * dy);
}

static int test_geometry(void)
{
    HudState hs;
    WpnIn in;
    SynZone z;
    const HudLayout *lay = hud_layout();
    const Param *pg = params_find("hud", "aseGapMin600");
    float k = hg_k(), cx = (float)hg_px(lay->aseX), cy = (float)hg_px(lay->aseY), Rpx = lay->aseR * k, Rlim = (lay->aseR + lay->aseOut) * k;
    float t = 500.0f, maxReach = 0.0f, maxDot = 0.0f, dotErr = 0.0f;
    int f, fails = 0, bs[4], ba[4], gs, ga, minGap, rd[4], pinnedSeen = 0, i, ch;
    static const float dcase[5][4] = {      /* az, el -> expected offset as a fraction of the radius (x, y) */
        { 10.0f, 0.0f, 0.5f, 0.0f }, { 0.0f, 10.0f, 0.0f, -0.5f }, { 40.0f, 0.0f, 1.0f, 0.0f }, { -7.5f, 0.0f, -0.375f, 0.0f }, { 0.0f, -16.0f, 0.0f, 0.8f } };
    memset(rd, 0, sizeof(rd));
    hudwpn_reset();
    hs.time = t;
    ts_state(&hs, VIEW_COCKPIT, v3(0.0f, 5000.0f, 0.0f), 0.0f, 0.0f, 0.0f, 2.0f);
    syn_start(&z, MSL_LRM, 19400.0f, 7600.0f, 800.0f, t, 5u, 0.0f);
    for (f = 0; f < 600; f++) {
        float R = f < 150 ? 3.6f * NM2M : (0.2f + (float)(f - 150) / 450.0f * 16.0f) * NM2M;
        t += 1.0f / 60.0f;
        hs.time = t;
        syn_step(&z, t, R, 500.0f, cam_dir(&hs, 30.0f * (float)sin((float)f * 0.05f), 30.0f * (float)cos((float)f * 0.031f)));
        syn_in(&in, &hs, MSL_LRM, R, 2.0f, -1.0f, &z.v);
        in.tdHeading = (float)f * 0.05f;
        hudwpn_draw_in(&hs, &in);
        if (s_dbg.aseReach > maxReach) maxReach = s_dbg.aseReach;
        if (s_dbg.dot) {
            float dd = (float)sqrt(((float)s_dbg.dotX - cx) * ((float)s_dbg.dotX - cx) + ((float)s_dbg.dotY - cy) * ((float)s_dbg.dotY - cy));
            if (dd > maxDot) maxDot = dd;
        }
        if (s_dbg.ptrPinned) pinnedSeen = 1;
        if (f == 149) { rd[0] = s_dbg.rdX0; rd[1] = s_dbg.rdY0; rd[2] = s_dbg.rdX1; rd[3] = s_dbg.rdY1; }
    }
    box_px(4, lay->spdX1, 1, lay->spdYc, bs);
    box_px(5, lay->altX0, 0, lay->altYc, ba);
    gs = (int)floor(rect_dist(bs, cx, cy) - maxReach);
    ga = (int)floor(rect_dist(ba, cx, cy) - maxReach);
    minGap = (int)ceil((pg ? *(const float *)pg->ptr : 6.0f) * (float)hg_set_lines() / 600.0f);
    log_printf(LOG_INFO, "hud: zone geometry set=%d ring=%s r=%.1f px reach max %.2f px (limit r+outer %.2f) spdBox=%d,%d-%d,%d altBox=%d,%d-%d,%d gapSpd=%d gapAlt=%d min=%d pinned=%d dot max %.2f px from the centre",
               hg_set_lines(), s_dbg.ringQuad ? "texture" : "chords", (double)Rpx, (double)maxReach, (double)Rlim, bs[0], bs[1], bs[2], bs[3],
               ba[0], ba[1], ba[2], ba[3], gs, ga, minGap, pinnedSeen, (double)maxDot);
    if (maxReach > Rlim + 0.5f || gs < minGap || ga < minGap) { TFAIL("hud: FAIL zone geometry: ASE circle too close to the boxes"); fails++; }
    if (!pinnedSeen || maxDot > Rpx + 1.3f) { TFAIL("hud: FAIL zone geometry: pointer pin / dot clamp"); fails++; }
    {   /* readouts: below the altitude box, clear of the circle */
        float dy = (float)rd[1] - cy, xr = dy < maxReach ? cx + (float)sqrt(maxReach * maxReach - dy * dy) : cx;
        log_printf(LOG_INFO, "hud: zone readouts box %d,%d-%d,%d (alt box bottom %d, circle right edge at that row %.1f)", rd[0], rd[1], rd[2], rd[3], ba[3], (double)xr);
        if (rd[1] < ba[3] + 2 || (float)rd[0] < xr + 2.0f || rd[2] > s_W) { TFAIL("hud: FAIL zone readouts overlap"); fails++; }
    }
    for (i = 0; i < 5; i++) {   /* steering dot scale: circle edge = aseDegLrm (20) */
        float ex = cx + dcase[i][2] * Rpx, ey = cy + dcase[i][3] * Rpx, e;
        z.v.leadDir = cam_dir(&hs, dcase[i][0], dcase[i][1]);
        syn_in(&in, &hs, MSL_LRM, 3.6f * NM2M, 2.0f, -1.0f, &z.v);
        memset(&s_st.dotSnap, 0, sizeof(Snap));
        hs.time = (t += 1.0f / 60.0f);
        hudwpn_draw_in(&hs, &in);
        e = (float)sqrt(((float)s_dbg.dotX - ex) * ((float)s_dbg.dotX - ex) + ((float)s_dbg.dotY - ey) * ((float)s_dbg.dotY - ey));
        if (e > dotErr) dotErr = e;
    }
    log_printf(LOG_INFO, "hud: zone steering dot max error %.2f px (5 cases)", (double)dotErr);
    if (dotErr > 1.0f) { TFAIL("hud: FAIL zone steering dot"); fails++; }
    /* SHOOT above/below the TD box near the top limit: +-1.9 px jitter -> at most one switch */
    {
        int yb = hg_px(SHOOT_LIM_CP) + s_boxHalf + hg_px(SHOOT_GAP) + hg_cap(HF_LARGE), prev = -1;
        ch = 0;
        for (f = 0; f < 300; f++) {
            hs.time = (t += 1.0f / 60.0f);
            syn_in(&in, &hs, MSL_LRM, 3.6f * NM2M, 2.0f, -1.0f, &z.v);
            in.tdY = yb + e_ftoi(1.9f * (float)sin((float)f * 0.7f));
            hudwpn_draw_in(&hs, &in);
            if (prev >= 0 && s_dbg.shootBelow != prev) ch++;
            prev = s_dbg.shootBelow;
        }
        log_printf(LOG_INFO, "hud: zone SHOOT side jitter changes=%d", ch);
        if (ch > 1) { TFAIL("hud: FAIL zone SHOOT side flicker"); fails++; }
    }
    /* chase: DLZ bar on screen; side switch at the right edge with jitter ([hud] chaseAse 0 = T16's bar) */
    {
        int prev = -1, xb, off, keep = s_chaseAse;
        s_chaseAse = 0;
        hudwpn_reset();
        ts_state(&hs, VIEW_CHASE, v3(0.0f, 5000.0f, 0.0f), 0.0f, 0.0f, 0.0f, 2.0f);
        hs.time = t;
        off = hg_text_w(HF_LARGE, "SHOOT") / 2;
        if (s_boxHalf > off) off = s_boxHalf;
        off += hg_px(4.0f);
        xb = s_W - (off + e_ftoi(BAR_TICK * k) + hg_px(7.0f) + s_ptrOy + hg_px(4.0f));
        ch = 0;
        for (f = 0; f < 400; f++) {
            hs.time = (t += 1.0f / 60.0f);
            syn_step(&z, t, 3.6f * NM2M, 500.0f, hs.noseDir);
            syn_in(&in, &hs, MSL_LRM, 3.6f * NM2M, 2.0f, -1.0f, &z.v);
            in.tdX = f < 100 ? hg_px(400.0f) : xb + e_ftoi(1.9f * (float)sin((float)f * 0.7f));
            hudwpn_draw_in(&hs, &in);
            if (!s_dbg.bar || s_dbg.barX < 0 || s_dbg.barX > s_W || s_dbg.barY0 < 0 || s_dbg.barY1 > s_H) { TFAIL("hud: FAIL zone chase bar off screen"); fails++; break; }
            if (prev >= 0 && s_dbg.barLeft != prev) ch++;
            prev = s_dbg.barLeft;
        }
        log_printf(LOG_INFO, "hud: zone chase bar (chaseAse 0) side jitter changes=%d bar x=%d y=%d..%d", ch, s_dbg.barX, s_dbg.barY0, s_dbg.barY1);
        if (ch > 1) { TFAIL("hud: FAIL zone chase bar side flicker"); fails++; }
        s_chaseAse = keep;
    }
    return fails;
}

/* ---- part T16b: the full weapon HUD in the chase view */
/* chase camera like camera.cpp with lag / roll lag / shake: camera frame = the nose frame turned by yaw / pitch and
   rolled; eye chaseDist behind / chaseUp above along it; looking at the nose point chaseLook ahead (+ shake angles) */
static void ts_chase_cam(HudState *hs, float fov, float lagYaw, float lagPitch, float roll, float shX, float shY)
{
    Vec3 nf = hs->noseDir, wu = v3(0.0f, 1.0f, 0.0f), nr, nu, cf, cu, cr, eye, at;
    const Param *pd = params_find("camera", "chaseDist"), *pu = params_find("camera", "chaseUp"), *pl = params_find("camera", "chaseLook");
    float D = pd ? *(const float *)pd->ptr : 24.0f, U = pu ? *(const float *)pu->ptr : 6.0f, L = pl ? *(const float *)pl->ptr : 40.0f, dl;
    nr = v3_norm(v3_cross(wu, nf));
    if (hs->upDir.y < 0.0f) nr = v3_scale(nr, -1.0f);
    nu = v3_norm(v3_cross(nf, nr));
    nr = v3_norm(v3_cross(nu, nf));                  /* (ts_state's frames have bank 0 here: nose up = path up) */
    cf = v3_norm(v3_add(nf, v3_add(v3_scale(nr, (float)tan(E_DEG2RAD(lagYaw))), v3_scale(nu, (float)tan(E_DEG2RAD(lagPitch))))));
    cr = v3_norm(v3_cross(nu, cf));
    cu = v3_norm(v3_cross(cf, cr));
    cu = v3_norm(v3_add(v3_scale(cu, (float)cos(E_DEG2RAD(roll))), v3_scale(cr, (float)sin(E_DEG2RAD(roll)))));
    eye = v3_add(v3_madd(hs->pos, cf, -D), v3_scale(cu, U));
    at = v3_madd(hs->pos, nf, L);
    dl = v3_len(v3_sub(at, eye));
    at = v3_madd(v3_madd(at, cr, dl * (float)tan(E_DEG2RAD(shX))), cu, dl * (float)tan(E_DEG2RAD(shY)));
    hs->eye = eye;
    m4_look_lh(&hs->viewM, eye, v3_madd(eye, v3_norm(v3_sub(at, eye)), 1000.0f), cu);
    m4_persp_lh(&hs->projM, 2.0f * (float)atan(tan(E_DEG2RAD(fov) * 0.5f) * 0.75f), 4.0f / 3.0f, 1.0f, 18000.0f);
}

static Vec3 nose_dir(const HudState *hs, float azDeg, float elDeg)   /* off the NOSE frame (bank 0) */
{
    Vec3 nf = hs->noseDir, nr = v3_norm(v3_cross(v3(0.0f, 1.0f, 0.0f), nf)), nu = v3_norm(v3_cross(nf, nr));
    return v3_norm(v3_add(nf, v3_add(v3_scale(nr, (float)tan(E_DEG2RAD(azDeg))), v3_scale(nu, (float)tan(E_DEG2RAD(elDeg))))));
}

static int test_chase(void)
{
    HudState hs;
    WpnIn in;
    SynZone z;
    const HudLayout *lay = hud_layout();
    const Param *pg = params_find("hud", "aseGapMin600");
    float k = hg_k(), t = 2000.0f, Rpx = lay->aseR * k, Rlim = (lay->aseR + lay->aseOut) * k, maxReach = 0.0f, aimOff = 0.0f, ex, ey;
    int f, fails = 0, ch, px = -1, py = -1, maxStep = 0, rev = 0, lastDir = 0, minGap, bs[4], ba[4], gs = 1 << 30, ga = 1 << 30;
    int cyMin = 1 << 30, cyMax = -1, rdBad = 0, rdTop = 0, rdBot = 0, slotBad = 0, i, scaleBad = 0;
    float fitErr = 0.0f, dotErr = 0.0f;
    u32 jr = 8086u;
    static const float dcase[5][4] = {
        { 10.0f, 0.0f, 0.5f, 0.0f }, { 0.0f, 10.0f, 0.0f, -0.5f }, { 40.0f, 0.0f, 1.0f, 0.0f }, { -7.5f, 0.0f, -0.375f, 0.0f }, { 0.0f, -16.0f, 0.0f, 0.8f } };
    minGap = (int)ceil((pg ? *(const float *)pg->ptr : 6.0f) * (float)hg_set_lines() / 600.0f);
    box_px(4, lay->chSpdX1, 1, lay->chYc, bs);
    box_px(5, lay->chAltX0, 0, lay->chYc, ba);
    /* 1. camera lag / roll lag / shake at a fixed FOV: the circle never moves; how far the live aim cross wanders */
    hudwpn_reset();
    hs.time = t;
    ts_state(&hs, VIEW_CHASE, v3(0.0f, 5000.0f, 0.0f), 0.0f, 0.0f, 0.0f, 2.0f);
    syn_start(&z, MSL_LRM, 19400.0f, 7600.0f, 800.0f, t, 21u, 0.0f);
    ch = 0;
    for (f = 0; f < 600; f++) {
        float yaw = 3.0f * (float)sin((float)f * 0.021f), pit = 2.5f * (float)sin((float)f * 0.033f + 1.0f), roll = 6.0f * (float)sin((float)f * 0.017f);
        hs.time = (t += 1.0f / 60.0f);
        ts_chase_cam(&hs, 70.0f, yaw, pit, roll, 0.4f * syn_rand(&jr), 0.4f * syn_rand(&jr));
        syn_step(&z, t, 8.0f * NM2M, 500.0f, hs.noseDir);
        syn_in(&in, &hs, MSL_LRM, 8.0f * NM2M, 3.0f, 2.0f, &z.v);
        hudwpn_draw_in(&hs, &in);
        if (px >= 0 && (s_dbg.aseCx != px || s_dbg.aseCy != py)) ch++;
        px = s_dbg.aseCx; py = s_dbg.aseCy;
        if (hud_project_point(&hs, v3_madd(hs.pos, hs.noseDir, 1000.0f), &ex, &ey)) {
            float d = (float)sqrt(((float)px - ex) * ((float)px - ex) + ((float)py - ey) * ((float)py - ey));
            if (d > aimOff) aimOff = d;
        }
    }
    log_printf(LOG_INFO, "hud: zone chase stability: camera lag +-3/2.5 deg, roll lag +-6, shake +-0.4 for 600 frames: circle centre %d,%d moved %d times; the live aim cross wandered up to %.1f px from it",
               px, py, ch, (double)aimOff);
    if (ch) { TFAIL("hud: FAIL zone chase circle moves with the camera lag (%d)", ch); fails++; }
    /* 2. FOV 70 -> 85 -> 70 (speed / AB FOV): whole-pixel steps, no back-and-forth, = the steady aim cross */
    hudwpn_reset();
    px = -1; ch = 0;
    for (f = 0; f <= 600; f++) {
        float fov = 70.0f + 15.0f * (0.5f - 0.5f * (float)cos(2.0f * E_PI * (float)f / 600.0f)), xv, yv;
        int d;
        hs.time = (t += 1.0f / 60.0f);
        ts_chase_cam(&hs, fov, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        syn_step(&z, t, 8.0f * NM2M, 500.0f, hs.noseDir);
        syn_in(&in, &hs, MSL_LRM, 8.0f * NM2M, 3.0f, 2.0f, &z.v);
        hudwpn_draw_in(&hs, &in);
        hud_chase_ase(&hs, &xv, &yv);
        if (yv > lay->chAseY0 + 0.5f && yv < lay->chAseY1 - 0.5f && hud_project_point(&hs, v3_madd(hs.pos, hs.noseDir, 1000.0f), &ex, &ey)) {
            float e = (float)sqrt(((float)s_dbg.aseCx - ex) * ((float)s_dbg.aseCx - ex) + ((float)s_dbg.aseCy - ey) * ((float)s_dbg.aseCy - ey));
            if (e > fitErr) fitErr = e;
        }
        if (px >= 0 && s_dbg.aseCy != py) {
            d = s_dbg.aseCy - py;
            ch++;
            if (abs(d) > maxStep) maxStep = abs(d);
            if (lastDir && (d > 0) != (lastDir > 0)) rev++;
            lastDir = d;
        }
        px = s_dbg.aseCx; py = s_dbg.aseCy;
        if (py < cyMin) cyMin = py;
        if (py > cyMax) cyMax = py;
    }
    log_printf(LOG_INFO, "hud: zone chase fov sweep 70..85..70: centre y %d..%d px, %d one-pixel moves (max step %d, reversals %d), max distance to the lag-free aim cross %.2f px",
               cyMin, cyMax, ch, maxStep, rev, (double)fitErr);
    if (maxStep > 1 || rev > 1 || fitErr > 1.0f) { TFAIL("hud: FAIL zone chase anchor (step %d, reversals %d, fit %.2f px)", maxStep, rev, (double)fitErr); fails++; }
    /* 3. geometry at both ends of the clamp range (FOV 40 -> top 200, FOV 110 -> bottom 216): circle vs the chase boxes,
          readouts vs the altitude box / circle / screen, MISSILE + LOCK home slots vs the circle */
    for (i = 0; i < 2; i++) {
        float fov = i ? 110.0f : 40.0f;
        hudwpn_reset();
        syn_start(&z, MSL_LRM, 19400.0f, 7600.0f, 800.0f, t, 22u + (u32)i, 0.0f);
        for (f = 0; f < 600; f++) {
            float R = f < 150 ? 3.6f * NM2M : (0.2f + (float)(f - 150) / 450.0f * 16.0f) * NM2M, cx, cy, g1, g2;
            hs.time = (t += 1.0f / 60.0f);
            ts_chase_cam(&hs, fov, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            syn_step(&z, t, R, 500.0f, nose_dir(&hs, 30.0f * (float)sin((float)f * 0.05f), 30.0f * (float)cos((float)f * 0.031f)));
            syn_in(&in, &hs, MSL_LRM, R, 2.0f, -1.0f, &z.v);
            in.tdHeading = (float)f * 0.05f;
            in.nThreats = 0;
            in.rwrLock = 1;
            hudwpn_draw_in(&hs, &in);
            cx = (float)s_dbg.aseCx; cy = (float)s_dbg.aseCy;
            if (s_dbg.aseReach > maxReach) maxReach = s_dbg.aseReach;
            g1 = rect_dist(bs, cx, cy) - s_dbg.aseReach; g2 = rect_dist(ba, cx, cy) - s_dbg.aseReach;
            if ((int)floor(g1) < gs) gs = (int)floor(g1);
            if ((int)floor(g2) < ga) ga = (int)floor(g2);
            if (s_dbg.rdX1) {
                float dy = (float)s_dbg.rdY0 - cy, xr = dy < s_dbg.aseReach ? cx + (float)sqrt(s_dbg.aseReach * s_dbg.aseReach - dy * dy) : cx;
                int r[4];
                r[0] = s_dbg.rdX0; r[1] = s_dbg.rdY0; r[2] = s_dbg.rdX1; r[3] = s_dbg.rdY1;
                if (rect_hit(r, ba, 2) || (float)s_dbg.rdX0 < xr + 2.0f || s_dbg.rdX1 > s_W || s_dbg.rdY1 > s_H) rdBad++;
                rdTop = s_dbg.rdY0; rdBot = s_dbg.rdY1;
            }
            {
                int mr[4], lr[4];
                slot_rect(s_slotMslCh, 0, HF_LARGE, "MISSILE", mr);
                slot_rect(s_slotLockCh, 0, HF_SMALL, "LOCK", lr);
                if (rect_dist(mr, cx, cy) < s_dbg.aseReach + 2.0f || rect_dist(lr, cx, cy) < s_dbg.aseReach + 2.0f) slotBad++;
            }
        }
        log_printf(LOG_INFO, "hud: zone chase geometry fov=%.0f centre %d,%d reach max %.2f px (limit %.2f) readouts y %d..%d (alt box bottom %d)",
                   (double)fov, s_dbg.aseCx, s_dbg.aseCy, (double)maxReach, (double)Rlim, rdTop, rdBot, ba[3]);
    }
    log_printf(LOG_INFO, "hud: zone chase geometry set=%d spdBox=%d,%d-%d,%d altBox=%d,%d-%d,%d gapSpd=%d gapAlt=%d min=%d readout overlaps=%d MISSILE/LOCK on the circle=%d",
               hg_set_lines(), bs[0], bs[1], bs[2], bs[3], ba[0], ba[1], ba[2], ba[3], gs, ga, minGap, rdBad, slotBad);
    if (maxReach > Rlim + 0.5f || gs < minGap || ga < minGap) { TFAIL("hud: FAIL zone chase circle too close to the boxes"); fails++; }
    if (rdBad || slotBad) { TFAIL("hud: FAIL zone chase readouts / slots overlap (%d / %d)", rdBad, slotBad); fails++; }
    /* 4. steering dot: angle nose -> lead (the circle edge = aseDegLrm), direction on the screen axes */
    hudwpn_reset();
    for (i = 0; i < 5; i++) {
        float e, cx, cy;
        hs.time = (t += 1.0f / 60.0f);
        ts_chase_cam(&hs, 70.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        z.v.leadDir = nose_dir(&hs, dcase[i][0], dcase[i][1]);
        z.v.leadValid = 1;
        syn_in(&in, &hs, MSL_LRM, 3.6f * NM2M, 2.0f, -1.0f, &z.v);
        memset(&s_st.dotSnap, 0, sizeof(Snap));
        hudwpn_draw_in(&hs, &in);
        cx = (float)s_dbg.aseCx; cy = (float)s_dbg.aseCy;
        e = (float)sqrt(((float)s_dbg.dotX - cx - dcase[i][2] * Rpx) * ((float)s_dbg.dotX - cx - dcase[i][2] * Rpx) +
                        ((float)s_dbg.dotY - cy - dcase[i][3] * Rpx) * ((float)s_dbg.dotY - cy - dcase[i][3] * Rpx));
        if (e > dotErr) dotErr = e;
    }
    log_printf(LOG_INFO, "hud: zone chase steering dot max error %.2f px (5 cases)", (double)dotErr);
    if (dotErr > 1.0f) { TFAIL("hud: FAIL zone chase steering dot"); fails++; }
    /* 5. longer ranges (T21a: player LRM ~2x, radar ~40 NM): scale 30 for Rmax 39 km, pinned + flashing beyond it; scale
          40 + ticks clamped at 355 deg for an Rmax beyond the largest scale */
    {
        static const float cs[4][3] = { { 39000.0f, 38.0f, 30.0f }, { 39000.0f, 20.0f, 30.0f }, { 70000.0f, 39.0f, 40.0f }, { 70000.0f, 20.0f, 40.0f } };
        for (i = 0; i < 4; i++) {
            hudwpn_reset();
            syn_start(&z, MSL_LRM, cs[i][0], cs[i][0] * 0.4f, 1200.0f, t, 30u + (u32)i, 0.0f);
            for (f = 0; f < 120; f++) {
                hs.time = (t += 1.0f / 60.0f);
                ts_chase_cam(&hs, 70.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
                syn_step(&z, t, cs[i][1] * NM2M, 600.0f, hs.noseDir);
                syn_in(&in, &hs, MSL_LRM, cs[i][1] * NM2M, 2.0f, -1.0f, &z.v);
                hudwpn_draw_in(&hs, &in);
            }
            log_printf(LOG_INFO, "hud: zone chase long range rmax=%.0f m R=%.0f NM: scale %.0f NM pointer %.0f deg%s rmax tick %.0f deg rne %.0f deg reach %.2f px",
                       (double)cs[i][0], (double)cs[i][1], (double)s_dbg.scaleNm, (double)s_dbg.ptrAng, s_dbg.ptrPinned ? " pinned" : "",
                       (double)s_dbg.rmaxAng, (double)s_dbg.rneAng, (double)s_dbg.aseReach);
            if (s_dbg.scaleNm != cs[i][2] || (s_dbg.ptrPinned != 0) != (cs[i][1] > cs[i][2]) || s_dbg.aseReach > Rlim + 0.5f) scaleBad++;
        }
        if (scaleBad) { TFAIL("hud: FAIL zone chase long-range scale (%d)", scaleBad); fails++; }
    }
    return fails;
}

/* ---- part 6: cockpit camera precision far from the origin (camera.cpp fix, T15 finding) */
static float px_err(Vec3 v, float P00, float P11, float ex, float ey)
{
    float x = (float)s_W * 0.5f + v.x / v.z * P00 - ex, y = (float)s_H * 0.5f - v.y / v.z * P11 - ey;
    return (float)sqrt(x * x + y * y);
}

static int test_camera(void)
{
    static const float pts[3][2] = { { 0.0f, 0.0f }, { 0.20f, -0.12f }, { -0.31f, 0.18f } };
    Aircraft a;
    Camera c;
    PilotInput pin;
    int fr, i, fails = 0;
    float maxNew = 0.0f, maxOld = 0.0f, maxPt = 0.0f, P00, P11;
    memset(&a, 0, sizeof(a));
    a.flags = AC_ALIVE | AC_PLAYER;
    flight_init(&a.fs, v3(52000.0f, 3000.0f, 47000.0f), E_DEG2RAD(37.0f), 200.0f);
    a.fs.onGround = 0; a.fs.gearPos = 0.0f; a.fs.thr = THR_NORMAL;
    a.fs.alpha = E_DEG2RAD(3.0f);
    a.prev = a.fs;
    memset(&c, 0, sizeof(c));
    cam_reset(&c, &a);
    c.mode = VIEW_COCKPIT;
    c.snap = 1;
    for (fr = 0; fr < 600; fr++) {
        float alpha = (float)fmod((double)fr * 0.37, 1.0);
        Vec3 pos, f, u;
        Quat qn;
        Mat4 old;
        if ((fr & 1) == 0) {
            a.prev = a.fs;
            memset(&pin, 0, sizeof(pin));
            pin.thr = THR_NORMAL;
            pin.ur = 0.25f * (float)sin((float)fr * 0.013f);
            pin.up = 0.15f;
            flight_step(&a.fs, &pin, SIM_DT);
        }
        cam_update(&c, &a, alpha, 1.0f / 60.0f);
        cam_ac_pose(&a, alpha, &pos, &qn);
        f = q_rotate(qn, v3(0.0f, 0.0f, 1.0f));
        u = q_rotate(qn, v3(0.0f, 1.0f, 0.0f));
        m4_look_lh(&old, c.eye, v3_add(c.eye, f), u);   /* the old construction, for comparison */
        P00 = c.proj.m[0][0] * (float)s_W * 0.5f;
        P11 = c.proj.m[1][1] * (float)s_H * 0.5f;
        for (i = 0; i < 3; i++) {
            Vec3 db = v3(pts[i][0], pts[i][1], 1.0f), dw = q_rotate(qn, db);
            float ex = (float)s_W * 0.5f + pts[i][0] * P00, ey = (float)s_H * 0.5f - pts[i][1] * P11, e;
            if (c.buffetDeg > 1e-5f) continue;      /* (none at 389 kt: the transonic bell is ~1e-28 deg) */
            e = px_err(m4_mul_dir(&c.view, dw), P00, P11, ex, ey);
            if (e > maxNew) maxNew = e;
            e = px_err(m4_mul_dir(&old, dw), P00, P11, ex, ey);
            if (e > maxOld) maxOld = e;
            e = px_err(m4_mul_point(&c.view, v3_madd(c.eye, v3_norm(dw), 5000.0f)), P00, P11, ex, ey);
            if (e > maxPt) maxPt = e;
        }
    }
    log_printf(LOG_INFO, "hud: zone camera cockpit at %.0f km from the origin, 600 frames: nose-frame directions max error %.4f px (old eye+unit-forward matrix %.3f px), point at 5 km %.4f px",
               (double)(v3_len(a.fs.pos) / 1000.0f), (double)maxNew, (double)maxOld, (double)maxPt);
    if (maxNew > 0.01f || maxPt > 0.01f) { TFAIL("hud: FAIL zone camera precision"); fails++; }
    return fails;
}

/* ---- part 7: the real pipeline (radar + target lock + dlz_work + weapons + hudtgt + hudwpn on a local Game) */
static Game s_lg;
static void live_sensor(const Game *g, int obs, WpnSensor *out)
{
    int td = radar_designated(obs), lk = radar_lock(obs);
    memset(out, 0, sizeof(*out));
    out->tdId = td;
    out->lock = td < 0 ? WLOCK_NONE : (lk == LOCK_TRACK ? WLOCK_TRACK : (lk == LOCK_MEMORY ? WLOCK_MEMORY : WLOCK_NONE));
    if (out->lock == WLOCK_MEMORY) radar_track(g, obs, td, &out->memPos, &out->memVel);
}

static void live_place(int i, Vec3 pos, float hdg, float V)
{
    Aircraft *a = &s_lg.ac[i];
    flight_init(&a->fs, pos, hdg, V);
    a->fs.onGround = 0; a->fs.gearPos = 0.0f; a->fs.thr = THR_NORMAL; a->fs.alpha = 0.0f;
    a->prev = a->fs;
    memset(&a->in, 0, sizeof(a->in));
    a->in.thr = THR_NORMAL;
}

static void live_cam(HudState *tmp, Camera *cam, float alpha)
{
    Vec3 pos;
    Quat qn;
    cam_ac_pose(&s_lg.ac[0], alpha, &pos, &qn);
    memset(cam, 0, sizeof(*cam));
    ts_cam(tmp, VIEW_COCKPIT, pos, qn);
    cam->mode = VIEW_COCKPIT; cam->eye = tmp->eye; cam->q = qn; cam->view = tmp->viewM; cam->proj = tmp->projM;
    cam->fovDeg = 65.0f; cam->nearM = 1.0f; cam->farM = 18000.0f;
}

typedef struct LiveEng { int fired, sawMsl, done, wrongKind, actUps, prevAct, actSeen, ttgSeen, switchOk, hit, backTof; u32 escTick, doneTick; } LiveEng;

static int test_live(void)
{
    int tick, i, fails = 0, viol = 0, frames = 0, eng = 0, readyFrame = -1, shootFrame = -1, flashFrame = -1;
    u32 evIdx;
    LiveEng E[2];
    SegT seg;
    memset(&seg, 0, sizeof(seg));
    memset(E, 0, sizeof(E));
    E[0].switchOk = E[1].switchOk = -1;
    E[0].prevAct = E[1].prevAct = -1;
    memset(&s_lg, 0, sizeof(s_lg));
    rnd_seed(&s_lg.rng, 1616u);
    s_lg.terrain = NULL;                            /* no island in -hudtest: sea plane, LOS clear */
    s_lg.numAircraft = 2;
    s_lg.tick = 1;
    for (i = 0; i < MAX_AIRCRAFT; i++) {
        s_lg.ac[i].id = i;
        s_lg.ac[i].side = i ? SIDE_RED : SIDE_BLUE;
        s_lg.ac[i].flags = i < 2 ? AC_ALIVE : 0;
        s_lg.ac[i].hp = 100.0f;
    }
    s_lg.ac[0].flags |= AC_PLAYER;
    live_place(0, v3(0.0f, 5000.0f, 0.0f), 0.0f, 260.0f);
    live_place(1, v3(600.0f, 5200.0f, 23000.0f), E_PI, 260.0f);
    radar_reset(&s_lg);
    weapons_set_sensor(live_sensor);
    weapons_reset(&s_lg);
    weapons_select(PLAYER_ID, MSL_LRM);
    dlz_reset(&s_lg);
    hudwpn_reset();
    hudtgt_reset();
    hud_reset();
    evIdx = weapons_event_count();
    /* engagement 0: launch on steady SHOOT at <= 0.85 Rmax, target straight (ACT long enough to watch);
       engagement 1: restart head-on at 11.5 km, launch on flashing SHOOT, the target breaks after 0.5 s */
    for (tick = 0; tick < 120 * 120 && eng < 2; tick++) {
        PilotInput pin[2];
        LiveEng *L = &E[eng];
        memset(pin, 0, sizeof(pin));
        pin[0].thr = pin[1].thr = THR_NORMAL;
        if (eng == 1 && L->fired && s_lg.tick > L->escTick) {
            pin[1].thr = THR_FAST;
            if (s_lg.tick < L->escTick + 50) pin[1].ur = 1.0f; else pin[1].up = 1.0f;
        }
        for (i = 0; i < 2; i++) {
            Aircraft *a = &s_lg.ac[i];
            a->prev = a->fs;
            a->in = pin[i];
            flight_step(&a->fs, &pin[i], SIM_DT);
        }
        weapons_tick(&s_lg);
        radar_tick(&s_lg);
        s_lg.tick++;
        while (evIdx < weapons_event_count()) {
            const WpnEvent *e = weapons_event(evIdx++);
            if (e && e->type == WPE_HIT && e->owner == PLAYER_ID) L->hit = 1;
        }
        if (radar_designated(PLAYER_ID) < 0 && radar_contact_state(PLAYER_ID, 1) != CS_NONE) radar_designate(&s_lg, PLAYER_ID, 1);
        if ((tick & 3) == 3) {                                   /* a 30 fps frame */
            HudState tmp, hs;
            Camera cam;
            TgtSym syms[TGT_MAX];
            WpnIn in;
            int n, want;
            float alpha = (float)((tick >> 2) & 3) * 0.25f + 0.1f;
            dlz_work(&s_lg, 1500);
            live_cam(&tmp, &cam, alpha);
            hud_state_fill(&hs, &s_lg, &cam, alpha);
            n = hudtgt_build(&s_lg, alpha, syms, TGT_MAX);
            hudtgt_draw_list(&hs, syms, n, 0);
            hudwpn_build(&s_lg, alpha, &hs, &in);
            hudwpn_draw_in(&hs, &in);
            frames++;
            if (s_dbg.shoot >= 1 && !oracle_shoot(&in.dlz, &in)) viol++;
            if (s_dbg.shoot == 2 && !oracle_ne(&in.dlz, &in)) viol++;
            seg_track(&seg, hs.time, s_dbg.shoot);
            if (readyFrame < 0 && in.dlz.ready) readyFrame = frames;
            if (shootFrame < 0 && s_dbg.shoot >= 1) {
                shootFrame = frames;
                log_printf(LOG_INFO, "hud: zone live SHOOT at R=%.0f m rmax=%.0f rne=%.0f rmin=%.0f now=%d/%d cycles=%u", (double)in.dlz.range,
                           (double)in.dlz.rmax, (double)in.dlz.rne, (double)in.dlz.rmin, in.dlz.nowValid, in.dlz.nowHit, in.dlz.cycles);
            }
            if (flashFrame < 0 && s_dbg.shoot == 2) flashFrame = frames;
            if (!L->fired && ((eng == 0 && s_dbg.shoot >= 1 && in.dlz.range <= 0.85f * in.dlz.rmax) || (eng == 1 && s_dbg.shoot == 2))) {
                int slot = weapons_fire(&s_lg, PLAYER_ID);
                if (slot >= 0) {
                    L->fired = 1;
                    L->escTick = s_lg.tick + 60;
                    log_printf(LOG_INFO, "hud: zone live %d launch on SHOOT %s: R=%.0f m Rne=%.0f Rmax=%.0f tof=%.1f slot=%d", eng,
                               s_dbg.shoot == 2 ? "flashing" : "steady", (double)in.dlz.range, (double)in.dlz.rne, (double)in.dlz.rmax,
                               (double)in.dlz.tof, slot);
                }
            }
            if (L->fired && !L->done) {
                if (in.msl) {
                    int v = atoi(s_dbg.thirdStr);
                    L->sawMsl = 1;
                    want = in.mslAct >= 0.0f ? TL_ACT : TL_TTG;
                    if (s_dbg.third != want) L->wrongKind++;
                    if (want == TL_ACT) { L->actSeen++; if (L->prevAct >= 0 && v > L->prevAct) L->actUps++; L->prevAct = v; }
                    if (want == TL_TTG) {
                        if (!L->ttgSeen) {
                            L->switchOk = s_dbg.third == TL_TTG;
                            log_printf(LOG_INFO, "hud: zone live %d activation: third line \"%s\" (missile act=%.2f ttg=%.2f) tick %u", eng, s_dbg.thirdStr,
                                       (double)in.mslAct, (double)in.mslTtg, s_lg.tick);
                        }
                        L->ttgSeen++;
                    }
                    if ((frames % 15) == 0) log_printf(LOG_INFO, "hud: zone live %d third=\"%s\" act=%.2f ttg=%.2f R=%.0f", eng, s_dbg.thirdStr,
                                                       (double)in.mslAct, (double)in.mslTtg, (double)in.dlz.range);
                } else if (L->sawMsl) { L->done = 1; L->doneTick = s_lg.tick; }
            }
            if (L->done && s_dbg.third != TL_ACT && s_dbg.third != TL_TTG) L->backTof = 1;
            if (L->done && s_lg.tick > L->doneTick + 120) {
                log_printf(LOG_INFO, "hud: zone live %d done: hit=%d act frames=%d (ups %d) ttg frames=%d switchOk=%d wrongKind=%d back=%d",
                           eng, L->hit, L->actSeen, L->actUps, L->ttgSeen, L->switchOk, L->wrongKind, L->backTof);
                eng++;
                if (eng == 1) {
                    live_place(0, v3(0.0f, 5000.0f, 0.0f), 0.0f, 260.0f);
                    live_place(1, v3(400.0f, 5100.0f, 11500.0f), E_PI, 260.0f);
                }
            }
        }
    }
    weapons_set_sensor(NULL);
    log_printf(LOG_INFO, "hud: zone live frames=%d violations=%d shoot changes=%d minHold=%.2f s ready@%d SHOOT@%d flash@%d hits %d/%d",
               frames, viol, seg.changes, (double)seg.minHold, readyFrame, shootFrame, flashFrame, E[0].hit, E[1].hit);
    if (viol || eng < 2 || shootFrame < 0 || flashFrame < 0) { TFAIL("hud: FAIL zone live pipeline (violations %d, engagements %d)", viol, eng); fails++; }
    for (i = 0; i < 2; i++) {
        LiveEng *L = &E[i];
        if (!L->fired || L->wrongKind || L->actUps || !L->done || !L->backTof || (L->actSeen && L->switchOk != 1) || (i == 0 && (L->actSeen < 30 || L->ttgSeen < 5))) {
            TFAIL("hud: FAIL zone live engagement %d (ACT/TTG line)", i);
            fails++;
        }
        if (!L->hit) log_printf(LOG_INFO, "hud: zone live NOTE: engagement %d missed (T13 owns the honesty statistics)", i);
    }
    return fails;
}

/* ---- part 8: review scenes (screenshots) + cost */
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

static void frame_end_shot(const char *shot)
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

enum { V_CP = 1, V_CH = 2 };
typedef struct ShotCase {
    const char *name; int views, sel, td; float rNm; int msl; float act, ttg; int srmLock, heat, threats, rwr;
} ShotCase;
static const ShotCase s_cases[] = {
    { "far",        V_CP | V_CH, MSL_LRM,  1, 13.0f, 0,  0.0f,  0.0f, -1, 0, 0, 0 },
    { "rmax",       V_CP | V_CH, MSL_LRM,  1,  8.0f, 0,  0.0f,  0.0f, -1, 0, 0, 0 },
    { "rne",        V_CP | V_CH, MSL_LRM,  1,  3.6f, 0,  0.0f,  0.0f, -1, 0, 0, 0 },
    { "act",        V_CP | V_CH, MSL_LRM,  1,  6.0f, 1, 12.3f, -1.0f, -1, 0, 0, 0 },
    { "ttg",        V_CP | V_CH, MSL_LRM,  1,  2.4f, 1, -1.0f,  4.2f, -1, 0, 0, 0 },
    { "rmin",       V_CP | V_CH, MSL_LRM,  1,  0.3f, 0,  0.0f,  0.0f, -1, 0, 0, 0 },
    { "srm_search", V_CP | V_CH, MSL_SRM, -1,  0.0f, 0,  0.0f,  0.0f, -1, 1, 0, 0 },
    { "srm_lock",   V_CP | V_CH, MSL_SRM,  1,  1.3f, 0,  0.0f,  0.0f,  1, 0, 0, 0 },
    { "visual",     V_CP | V_CH, MSL_LRM, -1,  0.0f, 0,  0.0f,  0.0f, -1, 0, 0, 0 },
    { "warn",       V_CP | V_CH, MSL_LRM,  1,  8.0f, 0,  0.0f,  0.0f, -1, 0, 3, 1 },
    { "visual_warn", V_CP | V_CH, MSL_LRM, -1, 0.0f, 0,  0.0f,  0.0f, -1, 0, 3, 1 },   /* T18: VISUAL + LOCK + arrows */
};

/* one frame of a scene: T07 HUD, T15 symbology (TD box), weapon HUD, minimap; render = inside a real frame */
static void scene_frame(const ShotCase *sc, int view, float t, float T, SynZone *z, HudState *hs, int render, const char *shot)
{
    WpnIn in;
    TgtSym s[3];
    int n = 0, i;
    float R = sc->rNm * NM2M;
    hs->time = t;
    ts_state(hs, view, v3(1500.0f, 15300.0f * FT2M, -2000.0f), 40.0f, 2.6f, 0.0f, 2.6f);
    hs->time = t;
    if (sc->td >= 0) syn_step(z, t, R, 520.0f, cam_dir(hs, 4.0f, -2.0f));
    syn_in(&in, hs, sc->sel, sc->td >= 0 ? R : 3000.0f, 3.0f, -1.5f, sc->td >= 0 ? &z->v : NULL);
    if (sc->msl) {
        in.msl = 1; in.mslType = MSL_LRM; in.mslSerial = 5u;
        in.mslAct = sc->act >= 0.0f ? sc->act + (T - t) : -1.0f;
        in.mslTtg = sc->ttg >= 0.0f ? sc->ttg + (T - t) : (sc->act + (T - t) + 6.0f);
    }
    if (sc->srmLock >= 0) { in.srmLock = 1; in.srmLockTd = 1; in.srmLockPos = in.tdPos; }
    in.srmNear = sc->heat;
    in.rwrLock = sc->rwr;
    for (i = 0; i < sc->threats && i < 3; i++) {
        static const float brg[3] = { 62.0f, -115.0f, 168.0f }, dist[3] = { 3500.0f, 6000.0f, 9000.0f };
        float b = E_DEG2RAD(hs->headingDeg + brg[i]);
        in.threatPos[i] = v3(hs->pos.x + dist[i] * (float)sin(b), hs->pos.y - 300.0f, hs->pos.z + dist[i] * (float)cos(b));
        in.threatSerial[i] = 20 + i;
        in.nThreats++;
    }
    if (sc->td >= 0) {
        s[n].id = 1; s[n].state = CS_HOSTILE; s[n].flags = CVF_IDENTIFIED | CVF_DESIGNATED | CVF_LOCKED | CVF_SELECTED;
        s[n].stateAge = 5.0f; s[n].pos = in.tdPos; s[n].headingRad = in.tdHeading; n++;
    }
    s[n].id = 2; s[n].state = CS_DETECTED; s[n].flags = 0; s[n].stateAge = 5.0f;
    s[n].pos = v3_madd(hs->pos, cam_dir(hs, -14.0f, 2.0f), 21.0f * NM2M); s[n].headingRad = 3.0f; n++;
    if (render) {
        if (!plat_pump() || !r_begin_frame(0x6A8FC0)) return;
        r_screen_size(&s_W, &s_H);
        draw_bg(hs);
    }
    hud_draw_state(hs);
    hudtgt_draw_list(hs, s, n, render);
    in.tdBox = hudtgt_td(&in.tdX, &in.tdY, &in.tdClamped, &in.tdHalf);
    hudwpn_draw_in(hs, &in);
    if (render) {
        for (i = 0; i < in.nThreats; i++) {
            MapContact m;
            m.x = in.threatPos[i].x; m.z = in.threatPos[i].z; m.headingRad = 0.0f; m.kind = MAPC_MSL_HOSTILE; m.flags = 0;
            map_draw_contact(&m);
        }
        map_draw_state(hs, NULL);
        frame_end_shot(shot);
    }
}

static void zone_for(const ShotCase *sc, SynZone *z, float t)
{
    if (sc->sel == MSL_SRM) syn_start(z, MSL_SRM, 5000.0f, 2700.0f, 350.0f, t, 1u, 0.0f);
    else syn_start(z, MSL_LRM, 19400.0f, 7600.0f, 800.0f, t, 1u, 0.0f);
}

static int test_shots(void)
{
    int c, v, f, fails = 0;
    char shot[64];
    for (c = 0; c < E_COUNT(s_cases); c++) for (v = 0; v < 2; v++) {
        const ShotCase *sc = &s_cases[c];
        HudState hs;
        SynZone z;
        float T = 20.0f, t0 = T - 3.0f;
        if (!(sc->views & (v ? V_CH : V_CP))) continue;
        hudwpn_reset();
        hudtgt_reset();
        hud_reset();
        zone_for(sc, &z, t0);
        memset(&hs, 0, sizeof(hs));
        if (v) e_snprintf(shot, (int)sizeof(shot), "shots\\chase_wpn_%s_%d.bmp", sc->name, s_H);   /* T16b */
        else e_snprintf(shot, (int)sizeof(shot), "shots\\wpn_cockpit_%s_%d.bmp", sc->name, s_H);
        for (f = 0; f <= 180; f++) scene_frame(sc, v ? VIEW_CHASE : VIEW_COCKPIT, t0 + (float)f / 60.0f, T, &z, &hs, f >= 178, f == 180 ? shot : NULL);
        log_printf(LOG_INFO, "hud: zone shot %s %s shoot=%d drawn=%d brk=%d circle=%d(%d,%d) ticks=%d scale=%.0f ptr=%.0f%s vc=\"%s\" rng=\"%s\" third=\"%s\" seek=%d(%d,%d)lock=%d visual=%d msl=%d lock=%d arrows=%d bar=%d wl=\"%s\" quads=%d draws=%d ms=%.3f",
                   v ? "chase" : "cockpit", sc->name, s_dbg.shoot, s_dbg.shootDrawn, s_dbg.brk, s_dbg.circle, s_dbg.aseCx, s_dbg.aseCy, s_dbg.ticks, (double)s_dbg.scaleNm,
                   (double)s_dbg.ptrAng, s_dbg.ptrPinned ? "p" : "", s_dbg.vc, s_dbg.rng, s_dbg.thirdStr, s_dbg.seek, s_dbg.seekX, s_dbg.seekY,
                   s_dbg.seekLock, s_dbg.visual, s_dbg.msl, s_dbg.lock, s_dbg.arrows, s_dbg.bar, s_dbg.wl, s_quads, s_draws, (double)s_ms);
        /* expected states of the review scenes */
        if (!strcmp(sc->name, "rmax") && s_dbg.shoot != 1) { TFAIL("hud: FAIL zone shot rmax: SHOOT not steady"); fails++; }
        if ((!strcmp(sc->name, "rne") || !strcmp(sc->name, "srm_lock")) && (s_dbg.shoot != 2 || !s_dbg.shootDrawn)) { TFAIL("hud: FAIL zone shot %s: SHOOT not flashing (on phase)", sc->name); fails++; }
        if (!strcmp(sc->name, "far") && s_dbg.shoot) { TFAIL("hud: FAIL zone shot far: SHOOT outside the zone"); fails++; }
        if (!strcmp(sc->name, "rmin") && (!s_dbg.brk || s_dbg.shoot)) { TFAIL("hud: FAIL zone shot rmin: no breakaway X"); fails++; }
        if (!strcmp(sc->name, "act") && !strstr(s_dbg.thirdStr, "ACT")) { TFAIL("hud: FAIL zone shot act: \"%s\"", s_dbg.thirdStr); fails++; }
        if (!strcmp(sc->name, "ttg") && !strstr(s_dbg.thirdStr, "TTG")) { TFAIL("hud: FAIL zone shot ttg: \"%s\"", s_dbg.thirdStr); fails++; }
        if (!strcmp(sc->name, "visual") && !s_dbg.visual) { TFAIL("hud: FAIL zone shot visual"); fails++; }
        if (!strcmp(sc->name, "warn") && (!s_dbg.mslDrawn || !s_dbg.lock || s_dbg.arrows != 3)) { TFAIL("hud: FAIL zone shot warn"); fails++; }
        if (v == 1 && sc->td >= 0 && (!s_dbg.circle || s_dbg.bar)) { TFAIL("hud: FAIL zone shot %s chase: no circle (or a DLZ bar too)", sc->name); fails++; }
        if (v == 1 && sc->td >= 0 && !s_dbg.rdX1) { TFAIL("hud: FAIL zone shot %s chase: no readouts", sc->name); fails++; }
    }
    return fails;
}

static int test_cost(void)
{
    static const int cases[2] = { 9, 2 };          /* warn (most elements), rne */
    int v, ci, f;
    for (v = 0; v < 2; v++) for (ci = 0; ci < 2; ci++) {
        const ShotCase *sc = &s_cases[cases[ci]];
        HudState hs;
        SynZone z;
        float T = 40.0f, t0 = T - 5.0f, mx = 0.0f;
        double sum = 0.0;
        int cnt = 0, q = 0, dr = 0;
        hudwpn_reset();
        hudtgt_reset();
        hud_reset();
        zone_for(sc, &z, t0);
        memset(&hs, 0, sizeof(hs));
        for (f = 0; f < 300; f++) {
            scene_frame(sc, v ? VIEW_CHASE : VIEW_COCKPIT, t0 + (float)f / 60.0f, T, &z, &hs, f >= 60, NULL);
            if (f >= 100) { sum += s_ms; cnt++; if (s_ms > mx) mx = s_ms; q = s_quads; dr = s_draws; }
        }
        log_printf(LOG_INFO, "hud: zone cost %s %s weapon HUD avg=%.3f ms max=%.3f quads=%d draws=%d (frames %d)", v ? "chase" : "cockpit", sc->name,
                   cnt ? sum / cnt : 0.0, (double)mx, q, dr, cnt);
    }
    return 0;
}

/* ---- part T18: MISSILE / LOCK never on the TD cluster (box, SHOOT, label, bar), threat arrows never on the texts */
static int capsule_hits(float tx, float ty, float a, float L, float W, const int *r)   /* the dart triangle vs a rect */
{
    float sx = (float)sin(a), sy = -(float)cos(a), u, v;
    for (u = 0.0f; u <= L; u += 1.0f)
        for (v = -W * u / L; v <= W * u / L; v += 1.0f) {
            float qx = tx - sx * u - sy * v, qy = ty - sy * u + sx * v;   /* perp = (-sy, sx) */
            if (qx > (float)r[0] && qx < (float)r[2] && qy > (float)r[1] && qy < (float)r[3]) return 1;
        }
    return 0;
}

static void ko_frame(HudState *hs, WpnIn *in, float t, int sel, int td, float tdx, float tdy, float brgDeg, int lock)
{
    DlzView v;
    memset(&v, 0, sizeof(v));
    v.valid = 1; v.ready = 1; v.target = 1; v.type = sel; v.range = 12000.0f; v.rmax = 19000.0f; v.rne = 7000.0f; v.rmin = 800.0f;
    v.tof = 20.0f; v.cycles = 3;
    hs->time = t;
    syn_in(in, hs, sel, 12000.0f, 0.0f, 0.0f, td ? &v : NULL);
    if (td) { in->tdBox = 1; in->tdX = e_ftoi(tdx); in->tdY = e_ftoi(tdy); in->tdHalf = s_boxHalf; }
    in->rwrLock = lock;
    if (brgDeg > -999.0f) {
        in->nThreats = 1;
        in->threatPos[0] = v3_add(hs->pos, v3(4000.0f * (float)sin(E_DEG2RAD(brgDeg)), 0.0f, 4000.0f * (float)cos(E_DEG2RAD(brgDeg))));
        in->threatSerial[0] = 77;
    }
    hud_draw_state(hs);
    hudwpn_draw_in(hs, in);
}

static int test_keepout(void)
{
    HudState hs;
    WpnIn in;
    float t = 900.0f, k = hg_k(), gx, gy, a, L, W;
    int view, i, f, fails = 0, w, h, ox, oy;
    if (!hg_sprite_info(s_sprThreat, &w, &h, &ox, &oy)) { ox = hg_px(19.0f); oy = hg_px(9.0f); }
    L = (float)ox; W = (float)oy;
    for (view = 0; view < 2; view++) {
        int vw = view ? VIEW_CHASE : VIEW_COCKPIT, ov = 0, frames = 0, moves = 0, chg, prev, ovA = 0, nA = 0, pushes = 0, cl[6][4], ncl, c;
        int below = 0, shootOv = 0;
        const char *vn = view ? "chase" : "cockpit";
        u32 jr = 4711u + (u32)view;
        hudwpn_reset();
        ts_state(&hs, vw, v3(0.0f, 4000.0f, 0.0f), 0.0f, 0.0f, 0.0f, 0.0f);
        /* 1. the TD box over a 20-unit grid of the screen (MISSILE + LOCK on, zone shown): no overlap in any frame */
        for (gy = 60.0f; gy <= 420.0f; gy += 20.0f)
            for (gx = 40.0f; gx <= 600.0f; gx += 20.0f)
                for (f = 0; f < 3; f++) {
                    t += 1.0f / 60.0f;
                    ko_frame(&hs, &in, t, MSL_LRM, 1, gx * k, gy * k, 180.0f, 1);
                    ncl = td_cluster(&in, s_dbg.zone, cl);
                    frames++;
                    if (s_dbg.mslSlot || s_dbg.lockSlot) moves++;
                    for (c = 0; c < ncl; c++)
                        if (rect_hit(s_dbg.mslRect, cl[c], 0) || rect_hit(s_dbg.lockRect, cl[c], 0)) { ov++; break; }
                    if (view && s_dbg.bar && s_dbg.shootBelow) {   /* chase: SHOOT below the box vs the bar's third line */
                        int sr[4], br[4];
                        text_rect(s_dbg.shootX, s_dbg.shootY, HF_LARGE, 1, "SHOOT", sr);
                        br[0] = s_dbg.barX - 3 * hg_adv(HF_SMALL); br[2] = s_dbg.barX + 3 * hg_adv(HF_SMALL);
                        br[1] = s_dbg.barY1 + hg_px(4.0f); br[3] = br[1] + hg_cap(HF_SMALL);
                        below++;
                        if (rect_hit(sr, br, 0)) shootOv++;
                    }
                }
        /* 2. jitter on a slot boundary: the TD box slides towards LOCK's home from the left, then +-1.9 px noise */
        {
            float y = view ? (LOCK_Y_CH + 4.0f) * k : (LOCK_Y_CP + 4.0f) * k, x, xc = -1.0f;
            hudwpn_reset();
            for (x = 150.0f * k; x < 320.0f * k; x += 0.25f) {
                t += 1.0f / 60.0f;
                ko_frame(&hs, &in, t, MSL_LRM, 1, x, y, -1000.0f, 1);
                if (s_dbg.lockSlot) { xc = x; break; }
            }
            chg = 0; prev = s_dbg.lockSlot;
            for (i = 0; i < 300 && xc > 0.0f; i++) {
                t += 1.0f / 60.0f;
                ko_frame(&hs, &in, t, MSL_LRM, 1, xc + 1.9f * syn_rand(&jr), y + 1.9f * syn_rand(&jr), -1000.0f, 1);
                if (s_dbg.lockSlot != prev) { chg++; prev = s_dbg.lockSlot; }
            }
            log_printf(LOG_INFO, "hud: zone keepout %s slots frames=%d overlaps=%d moved=%d | LOCK boundary x=%.1f px jitter changes=%d | SHOOT below %d frames, on the bar text %d",
                       vn, frames, ov, moves, (double)xc, chg, below, shootOv);
            if (shootOv) { TFAIL("hud: FAIL zone keepout %s SHOOT on the DLZ bar text (%d frames)", vn, shootOv); fails++; }
            if (ov || xc < 0.0f || chg > 1 || !moves) { TFAIL("hud: FAIL zone keepout %s slots (overlaps %d, boundary %.1f, changes %d)", vn, ov, (double)xc, chg); fails++; }
        }
        /* 3. threat arrow at every bearing (0.5 deg) with zone readouts / VISUAL, MISSILE, LOCK: the dart never touches a
              text or a box; on the first push boundary +-0.6 deg of noise switches at most once */
        for (c = 0; c < 2; c++) {
            float ac = -1.0f;
            char rng[224];
            int lastP = 0;
            rng[0] = 0;
            hudwpn_reset();
            for (a = 0.0f; a < 360.0f; a += 0.5f) {
                int bx[12][4], nb, q, hit = 0;
                t += 1.0f / 60.0f;
                ko_frame(&hs, &in, t, MSL_LRM, c == 0, 330.0f * k, 200.0f * k, a, 1);
                nb = hudwpn_keepout(bx, 4, vw);
                nb += hud_keepout(bx + nb, 12 - nb, vw, hs.time);
                for (q = 0; q < nb; q++) if (capsule_hits(s_dbg.arrowX[0], s_dbg.arrowY[0], E_DEG2RAD(a), L, W, bx[q])) hit = 1;
                if (hit) { ovA++; if (ovA <= 4) log_printf(LOG_INFO, "hud: zone keepout %s arrow hit at %.1f deg (case %d) tip %.1f,%.1f", vn, (double)a, c, (double)s_dbg.arrowX[0], (double)s_dbg.arrowY[0]); }
                if (s_dbg.arrowPushed) pushes++;
                if (s_dbg.arrowPushed && ac < 0.0f) ac = a;
                if ((s_dbg.arrowPushed != 0) != lastP && (int)strlen(rng) < 200) {
                    char e[16];
                    e_snprintf(e, (int)sizeof(e), "%s%.0f", lastP ? "-" : " ", (double)a);
                    e_strlcpy(rng + strlen(rng), e, (int)(sizeof(rng) - strlen(rng)));
                    lastP = s_dbg.arrowPushed != 0;
                }
                nA++;
            }
            chg = 0;
            if (ac >= 0.0f) {
                hudwpn_reset();
                ko_frame(&hs, &in, t, MSL_LRM, c == 0, 330.0f * k, 200.0f * k, ac - 2.0f, 1);
                prev = s_dbg.arrowPushed;
                for (i = 0; i < 300; i++) {
                    t += 1.0f / 60.0f;
                    ko_frame(&hs, &in, t, MSL_LRM, c == 0, 330.0f * k, 200.0f * k, ac - 0.25f + 0.6f * syn_rand(&jr), 1);
                    if (s_dbg.arrowPushed != prev) { chg++; prev = s_dbg.arrowPushed; }
                }
            }
            log_printf(LOG_INFO, "hud: zone keepout %s arrows case=%s bearings=%d hits=%d pushed=%d (deg:%s) first push %.1f deg jitter changes=%d",
                       vn, c == 0 ? "zone" : "visual", nA, ovA, pushes, rng, (double)ac, chg);
            if (ovA || chg > 1) { TFAIL("hud: FAIL zone keepout %s arrows (hits %d, changes %d)", vn, ovA, chg); fails++; }
            ovA = 0; nA = 0; pushes = 0;
        }
    }
    hudwpn_reset();
    return fails;
}

int hudwpn_test(void)
{
    int r;
    s_fail = 0;
    r_screen_size(&s_W, &s_H);
    sprites_init();
    {
        int w, h, ox, oy;
        s_boxHalf = hg_sprite_info(hg_sprite_id("tgt_box"), &w, &h, &ox, &oy) ? oy : hg_px(11.0f);
    }
    log_printf(LOG_INFO, "hud: zone test screen=%dx%d set=%d k=%.4f aseR=%.0f aseOuter=%.0f boxHalf=%d", s_W, s_H, hg_set_lines(), (double)hg_k(),
               (double)hud_layout()->aseR, (double)hud_layout()->aseOut, s_boxHalf);
    test_approach(MSL_LRM, VIEW_COCKPIT);
    test_approach(MSL_SRM, VIEW_COCKPIT);
    test_approach(MSL_LRM, VIEW_CHASE);             /* T16b: the same honesty / flicker rules in the chase view */
    test_approach(MSL_SRM, VIEW_CHASE);
    test_hover(0, VIEW_COCKPIT);
    test_hover(1, VIEW_COCKPIT);
    test_hover(2, VIEW_COCKPIT);
    test_hover(0, VIEW_CHASE);
    test_hover(1, VIEW_CHASE);
    test_countdown(VIEW_COCKPIT);
    test_countdown(VIEW_CHASE);
    test_seeker_warn();
    test_geometry();
    test_chase();
    test_camera();
    test_live();
    test_keepout();
    test_shots();
    test_cost();
    r = s_fail;
    hudwpn_reset();
    hudtgt_reset();
    log_printf(LOG_INFO, "hud: zone test failures=%d", r);
    return r;
}
