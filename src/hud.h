/* hud.h - flight HUD (DESIGN 10.1-10.3, 10.7, 10.8). Windows/render side. Owner: T07.

   The HUD reads only a HudState filled by the caller (flight values, interpolated orientation, camera matrices), so
   it draws the same over the test scenes (-hudtest) and over the real scene (T08). hud_draw(g, c, alpha) is the
   convenience path for main: hud_state_fill + the extras set with hud_set_extras + hud_draw_state.

   Layout: virtual 640x480 (docs/HUD_LAYOUT.md is authoritative), placed at x k = screen h / 480 with whole-pixel
   text and symbols (hud_font.h). Cockpit: heading tape, speed box (left) and altitude box (right) with the numbers
   centred in both directions, VVI, AOA/Mach/G/peak G stack, waterline, velocity vector (clamped + flashing at the HUD
   frame), pitch ladder (conformal, centred on the VV, rotated by bank, clipped to the frame), bank scale, weapon line,
   messages, warnings, clock. Chase: aim cross, VV, horizon + 10 deg rungs at half alpha, boxes further apart, G, small
   heading tape, messages, warnings. The launch-zone (ASE) circle belongs to T16: its centre/radius are layout values
   here (hud_layout) so the boxes keep a clear gap from it (user rule 2026-11-13; checked by -hudtest layout).

   Stability (10.7): numbers update at fixed rates (speed/alt/heading 10 Hz; Mach/G/AOA/VVI 5 Hz) with a
   quantisation dead band and a reversal needs 2 consecutive updates; strings are rebuilt only on change; flashing
   phases come from HudState.time (sim time). Conformal symbols use the camera matrices of the 3D frame. */
#ifndef HUD_H
#define HUD_H

#include "game.h"
#include "camera.h"

enum { HUDW_PULLUP = 1, HUDW_RETURN = 2 };          /* HudState.warn bits */

typedef struct HudState {
    int view;                                       /* VIEW_COCKPIT / VIEW_CHASE */
    float time;                                     /* sim seconds (rates, flashing, messages) */
    Mat4 viewM, projM;                              /* the 3D frame's camera matrices (Camera.view / .proj) */
    Vec3 eye;                                       /* camera position (m) */
    Vec3 pos;                                       /* own aircraft position (m), interpolated */
    Vec3 velDir;                                    /* unit flight-path (velocity) direction, world */
    Vec3 noseDir;                                   /* unit nose (boresight) direction, world */
    Vec3 upDir;                                     /* unit lift (path up) direction, world */
    float speedKt, altFt, vsFpm, mach, g, aoaDeg;
    float headingDeg;                               /* 0..360 of the flight path (tape, minimap) */
    float bankDeg;                                  /* + = right wing down (bank scale) */
    int onGround;
    u32 warn;                                       /* HUDW_* */
    float clockSec;                                 /* mission clock, < 0 = hidden */
    char weapon[24];                                /* weapon line ("AIM-120 16"), "" = none (M2) */
    int flares;                                     /* minimap "F40", < 0 = hidden */
} HudState;

typedef struct HudLayout {                          /* virtual 640x480 units, after hud_params (read-only) */
    float frameX0, frameY0, frameX1, frameY1;       /* cockpit HUD frame = conformal clip box */
    float wlX, wlY;                                 /* waterline / boresight */
    float aseX, aseY, aseR, aseOut;                 /* T16 launch-zone circle: centre, radius, outer extent (ticks,
                                                       pointer) beyond the radius */
    float spdX1, spdYc, altX0, altYc;               /* cockpit boxes: speed box right edge, altitude box left edge,
                                                       vertical centres (the box size comes from the font) */
    float chSpdX1, chAltX0, chYc;                   /* chase boxes */
    float chAseX, chAseY0, chAseY1;                 /* T16b: chase launch-zone circle centre x and the y range it is
                                                       clamped to (hud_chase_ase); radius / outer as the cockpit */
} HudLayout;

void  hud_params(void);                             /* [hud] (+ hud_font, hud_map tables) */
int   hud_init(void);
void  hud_free(void);
void  hud_draw(const Game *g, const Camera *c, float alpha);
int   hud_test(const char *name);                   /* -hudtest NAME (10.8): exit code = failures, -1 = unknown */

/* ---- added by T07 ---- */
void  hud_state_fill(HudState *hs, const Game *g, const Camera *c, float alpha);   /* player 0, interpolated */
void  hud_draw_state(const HudState *hs);           /* draws the flight HUD (own hg_begin/hg_end batch) */
void  hud_set_extras(u32 warn, float clockSec, const char *weapon, int flares);   /* used by hud_draw */
void  hud_message(const char *text, float secs);    /* queue (4): large text at (320,168), one at a time */
void  hud_reset(void);                              /* new flight: clears messages, number caches, peak G */
const HudLayout *hud_layout(void);
int   hud_project_dir(const HudState *hs, Vec3 d, float *sx, float *sy);    /* direction -> screen px (1 = in
                                                       front); for T15/T16 conformal symbols */
int   hud_project_point(const HudState *hs, Vec3 p, float *sx, float *sy);  /* world point -> screen px */
float hud_ms_last(void);                            /* CPU ms of the last hud_draw_state */

/* ---- added by T18 ---- */
int   hud_keepout(int (*box)[4], int maxBoxes, int view, float time);   /* the speed / altitude boxes with their text
                                                       stacks (px x0 y0 x1 y1) of the last hud_draw_state, if it drew
                                                       this view at this HudState.time (else 0): hud_wpn's threat
                                                       arrows keep out of them */

/* ---- added by T16b ---- */
void  hud_chase_ase(const HudState *hs, float *cxV, float *cyV);  /* chase launch-zone circle centre (virtual units): the
                                                       STEADY-STATE aim cross (where the 1 km nose point sits with the
                                                       chase camera's chaseDist / chaseUp / chaseLook and no lag or shake
                                                       at this frame's FOV), x chAseX, y clamped to chAseY0..chAseY1: moves
                                                       only with the (smoothed) FOV, never with the camera lag */

#endif
