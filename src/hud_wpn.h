/* hud_wpn.h - weapon HUD: launch-zone (ASE) circle, SHOOT, breakaway X, Vc/RNG/TOF -> ACT -> TTG readouts, chase DLZ
   bar, SRM seeker circle + tone requests, VISUAL (maddog), weapon line, MISSILE / threat arrows / LOCK warnings
   (DESIGN 10.6, 10.7, HUD.md 2.1). Windows/render side. Owner: T16.

   Cockpit (LRM or SRM selected with a TD whose zone dlz_view publishes):
     ASE circle   centred on the waterline, radius [hud] aseRadius (T07 layout constant, 60 virtual); everything
                  attached (Rmax / Rne / Rmin ticks, range pointer, aspect pointer) stays within aseOuter (8) beyond
                  the radius, so the circle never touches the speed / altitude boxes (user rule; -hudtest layout
                  checks the gap for every atlas set). Range scale clockwise from 12 o'clock: angle = 360 x R /
                  Rscale, Rscale = smallest of {2,3,5,8,10,15,20,30,40} NM >= 1.2 x Rmax (hysteresis). Rmax tick
                  outward, Rne outward double tick, Rmin inward; ticks only once the first LIVE Rmax is published
                  (dlz ready). Range pointer = ptr_tri on the ring pointing inward at the current range (pinned at
                  355 deg and flashing beyond Rscale). Aspect pointer = thin outward line at the target's heading
                  relative to ours (12 o'clock = same heading). The ring is ONE quad from a texture generated for the
                  atlas set (premultiplied white, baked shadow like line_sh) when it fits 256x256, else a polyline.
     steering dot dot4, offset from the centre = angle nose -> dlz leadDir, circle edge = aseDegLrm / aseDegSrm.
     SHOOT        large text above the TD box (below it when the box is near the top of the HUD frame / screen):
                  steady inside the zone, flashing 3 Hz inside the no-escape zone. Shown ONLY when the published
                  zone says so: dlz ready && Rmin < range <= Rmax && the NOW fly-out (if any) hits [&& SRM: the seeker
                  is locked on the TD]; flashing additionally range <= Rne && the NOWESC fly-out (if any) still hits
                  && the TD is not in radar memory. Entering needs the condition with a band (range <= Rmax x (1 -
                  wpnShootBand), >= Rmin x (1 + 2 band)) held wpnShootOnSec and through the end of a live dlz cycle;
                  leaving is immediate (honest + no flicker).
     breakaway X  breakaway_x sprite on the waterline when range < Rmin x (1 + band) (off at 1 + 2 band, 0.3 s).
     readouts     right of the circle, numbers right-aligned so a 4-digit one starts at x 396: "960 Vc" (y 290),
                  "19.9 RNG" (y 304; < 1 NM: hundreds of ft), third line "23 TOF" (NOW fly-out time) / "17ACT" (LRM in the air at this target, counting down whole
                  seconds: follows the missile's estimate down only, max 2 Hz) -> "11TTG" from the frame the missile goes
                  active / SRM "6TTG". Vc/RNG 5 Hz, TOF 2 Hz; dead band + 2-update reversal rule (10.7.3).
   SRM seeker: seeker sprite (3 deg) at the waterline while searching, slaved onto the locked target (the TD or the
     boresight lock), flashing when outside the HUD frame; tone requests: growl while searching with a heat target
     near, high lock tone while locked (hudwpn_tones). Lock display latched (0.3 s).
   VISUAL: LRM selected without a TD: dashed circle (radius aseRadius: a 20 deg circle would cross the boxes) +
     "VISUAL" under the weapon line.
   Weapon line: cockpit centre (320, 368) "AIM-120 16" / "AIM-9 40" ([hud] wpnNameLrm/Srm); chase: block bottom
     right (560, 410..455): name, count (large), "FLR 40", "TONE" / "VISUAL".
   Chase (T16b, [hud] chaseAse 1): the SAME circle + ticks + pointers + steering dot + breakaway X + Vc/RNG/third-line
     readouts, centred on the steady-state aim cross (hud_chase_ase: where the 1 km nose point sits with the chase
     camera's distance/height/look point at this frame's FOV, without the camera lag or shake; y clamped 200..216
     virtual, sticky whole pixel), readouts at x 396 from 44 below the centre; steering dot = angle nose -> lead on
     the screen axes; VISUAL = dashed circle there + "VISUAL" in the weapon block; SRM seeker on the live aim cross.
     chaseAse 0 = T16's compact vertical DLZ bar (50 x 6) right of the TD box with the third line under it.
   Warnings (red, both views): "MISSILE" large, flashing 4 Hz while a hostile missile homes on the player
     (radar_rwr_missile_warn rule), cockpit (320, 155) between the heading caret and the circle, chase (320, 98) above the circle;
     up to 3 threat arrows on a ring (wpnThreatRing 160) around the screen centre at the missile's relative bearing
     (12 o'clock = ahead, RWR style), the nearest at full alpha; "LOCK" small while an enemy has the player locked,
     cockpit (320, 318) under the circle, chase (320, 116).
   Stability (10.7): positions from the render state and the 3D frame's matrices; moving dots rounded with a
   0.75 px sticky band; every on/off decision has hysteresis or a latch; flashing phase from sim time; strings built
   only when the value changes. Draw order: main draws hudtgt (TD box, hudtgt_td) before hudwpn. */
#ifndef HUD_WPN_H
#define HUD_WPN_H

#include "game.h"
#include "camera.h"

void  hudwpn_draw(const Game *g, const Camera *c, float alpha);

/* ---- added by T16 ---- */
#include "dlz.h"
struct HudState;

enum { WPN_THREATS_MAX = 8 };
typedef struct WpnIn {                              /* everything the weapon HUD reads (hudwpn_build or a test) */
    int sel;                                        /* MSL_LRM / MSL_SRM selected */
    int lrm, srm, flares;                           /* stores */
    int td;                                         /* designated aircraft id, -1 = none */
    int tdMemory;                                   /* TD lock in memory (dashed) */
    Vec3 tdPos;                                     /* render position of the TD target */
    float tdHeading;                                /* its world heading (rad) */
    int tdBox, tdX, tdY, tdClamped, tdHalf;         /* TD box drawn this frame (hudtgt_td) */
    DlzView dlz;                                    /* copy of dlz_view() */
    int msl;                                        /* 1 = own live missile at the TD (weapons_latest_at) */
    int mslType; u32 mslSerial;
    float mslAct, mslTtg;                           /* MissileView.act / .ttg (s, -1 = none / unknown) */
    int srmLock;                                    /* aircraft id the SRM seeker is locked on, -1 none */
    int srmLockTd, srmNear;
    Vec3 srmLockPos;                                /* render position of that aircraft */
    int rwrLock;                                    /* 1 = an enemy has the player locked */
    int nThreats;                                   /* hostile missiles homing on the player that warn */
    Vec3 threatPos[WPN_THREATS_MAX];                /* render positions */
    int threatSerial[WPN_THREATS_MAX];              /* stable ids (MissileView.serial) */
} WpnIn;

typedef struct HudWpnTones {                        /* sound requests of the last hudwpn draw (T17 plays them) */
    int growl;                                      /* SRM searching with a heat target near the seeker circle */
    int srmLock;                                    /* SRM seeker locked: high lock tone */
    int rwrLock;                                    /* LOCK shown: RWR lock tone */
    int missile;                                    /* MISSILE shown: missile warning tone */
} HudWpnTones;

void  hudwpn_params(void);                          /* [hud] wpn* keys: call from main's register_params after
                                                       hud_params (the code defaults work without it) */
void  hudwpn_reset(void);                           /* new flight: latches, countdowns, caches */
int   hudwpn_init(void);                            /* optional, after hud_init: sprites + the ring texture now (else
                                                       made at the first TD: ~1 frame of work); 1 = textured ring */
void  hudwpn_build(const Game *g, float alpha, const struct HudState *hs, WpnIn *in);   /* live modules -> WpnIn */
void  hudwpn_draw_in(const struct HudState *hs, const WpnIn *in);   /* the core (own hg_begin/hg_end batch) */
void  hudwpn_tones(HudWpnTones *out);
float hudwpn_ms_last(void);                         /* CPU ms of the last hudwpn_draw_in */
int   hudwpn_test(void);                            /* -hudtest zone (DESIGN 10.8): exit code = failures */
int   hudwpn_keepout(int (*box)[4], int maxBoxes, int view);   /* T18: px rects of the fixed weapon texts drawn
                                                       last frame in this view (readout block, weapon line + VISUAL,
                                                       chase weapon block, MISSILE, LOCK): the flight HUD keeps its
                                                       ladder out of them */

#endif
