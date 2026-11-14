# Jet HUD layout as built (T07, 2026-11-13)

Authoritative for the flight HUD and minimap (DESIGN 10.2-10.4 give the intent; where they differ, this file wins).
All positions are **virtual 640x480 units**; on screen they are multiplied by k = screen height / 480 and rounded to
whole pixels (4:3 modes only). The atlas set is picked by the screen height (480 / 600 / 768 / 1050, the largest
set <= height) and drawn 1:1, so text and symbols are crisp at every resolution. Colours: HUD green 0xFF40FF60
(x `[hud] bright`), hostile red 0xFFFF3030, unknown yellow 0xFFFFE040. Every glyph, sprite and line carries the A4c
baked drop shadow (one RB_PREMUL quad each).

Screenshots: `shots/hud_<view>_<case>_<screen h>.png` (480, 600, 768, 1200 = the 1050 set at k 2.5; the VM has no
1400x1050 mode), written as BMP by `jet.exe -hudtest layout` and converted to PNG (cases: level, climb60
= 32 deg climb + 60 deg bank, inverted, g9_350 = 9 G at 350 kt, aoa15 = 15 deg alpha with the VV clamped), and
`shots/hud_numbers.bmp` by `-hudtest numbers`. The cockpit `level` shot also shows a **dummy launch-zone circle**
(T16 draws the real one) to make the box gap visible.

## Cockpit view

| # | Element | Position / size (virtual) | Behaviour |
|---|---|---|---|
| F | HUD frame (conformal clip box) | x 170..470, **y 152**..384 | top moved down from 116 so the ladder never crosses the heading tape/caret |
| 1 | Heading tape | ticks hang up from y 141, +-120 wide (8 px/deg, 30 deg window), long tick 7 every 10 deg with a 3-digit label above (small font, 360 not 000), short tick 4 at 5 deg; caret (tip) at y 144 | scrolls every frame from the interpolated path heading; ticks snapped to whole pixels (crisp); labels hidden when they would cross the window edge |
| 2 | Speed box | right edge x 244, vertical centre y 228; size from the large font: 4 digits x adv - gap + 2 x pad (3) + 2 x stroke wide, cap + 2 x pad (2.5) + 2 x stroke tall | knots (true airspeed), 10 Hz; number **centred horizontally and vertically** (A4b recipe); box size never changes |
| 3 | Altitude box | left edge x 396, centre y 228; 5 digits | feet MSL in 10 ft steps, 10 Hz, centred |
| 4 | Vertical velocity | right-aligned to the altitude box's right edge, 4 above it (small font) | ft/min in 10 ft/min steps, 5 Hz |
| 5 | AOA / Mach / G / peak G | left-aligned at the speed box's left edge, from 10 under the box, 14 per row: `@ 2.6` (@ = alpha glyph), `M 0.84`, `G 1.0`, peak `9.2` (indented 2 digits) | 5 Hz; peak shown once above 4 g, cleared after 5 s below 2 g |
| 6 | Waterline | sprite at (320, 240) | fixed = boresight |
| 7 | Velocity vector | conformal (projection of the path direction) | clamped 16 / 12 inside the frame and flashing 3 Hz when clamped |
| 8 | Pitch ladder | conformal; rungs every 5 deg within `ladderWindow` 30 deg of the VV; horizon +-110 with a 24 centre gap; rungs from +-12 to +-52; positive solid with 5 hooks at the outer ends towards the horizon; negative = 3 dashes of 10 per side with hooks up (towards the horizon); signed numbers 61 out from the centre, unrotated, whole pixels | centred laterally on the VV (great circle through the VV and the zenith), rotated by bank, clipped to the frame; **kept out of the speed/altitude boxes, the VVI and the AOA/M/G stack** (lines are cut around those boxes, numbers that would touch them are skipped) |
| 9 | Bank scale | arc centre (320, 240), r 112; ticks 5 / 15 deg (5 long) and 30 / 45 deg (9 long) each side, outward | pointer = `ptr_tri` inside the arc pointing out, at the bank angle (right bank -> right), pinned at 47 deg |
| 10 | Weapon line | centre (320, 368), small font | from `HudState.weapon` (M2) |
| 11 | Messages | large font centred at (320, 168) | `hud_message(text, secs)` queue of 4, one at a time |
| 12 | Warnings | `PULL UP` large at (320, 300) flashing 4 Hz; `RETURN TO AREA` large at (320, 186) | `HudState.warn` bits |
| 13 | Clock | (178, 372) mm:ss | `HudState.clockSec >= 0` |
| ASE | Launch-zone circle (T16) | centre (320, 240), **radius `[hud] aseRadius` 60** (was 66), ticks/pointer up to `aseOuter` 8 beyond | user rule: clear gap to both boxes; `-hudtest layout` checks every set |

ASE circle <-> box gap (from `-hudtest layout`, minimum `aseGapMin600` 6 px at 600 lines scaled per set):

| set | circle r / outer (px) | speed box (px) | altitude box (px) | gap speed / alt (px) | min |
|---|---|---|---|---|---|
| 480 | 60.0 / 68.0 | 193,218-244,239 | 396,218-458,239 | 8 / 8 | 5 |
| 600 | 75.0 / 85.0 | 241,272-305,298 | 495,272-573,298 | 10 / 10 | 7 |
| 768 | 96.0 / 108.8 | 310,348-390,383 | 634,348-731,383 | 13 / 13 | 8 |
| 1050 | 131.3 / 148.8 | 425,476-534,522 | 866,476-998,522 | 17 / 17 | 11 |

## Chase view

| Element | Position | Notes |
|---|---|---|
| Aim cross | projection of the point 1 km along the nose | |
| Velocity vector | conformal, clamped 20 inside the screen, flashing when clamped | |
| Horizon + 10 / -10 rungs | conformal, `chaseAlpha` 0.5, horizon +-90 with a 24 gap, clipped 20 inside the screen | centred on the VV; kept out of the boxes |
| Speed / altitude boxes | speed right edge x 198, altitude left edge x 442, centre y 228 | same boxes and numbers as the cockpit |
| G | under the speed box (6 below), `chaseMach` adds Mach under it | |
| Heading tape | ticks from y 34, +-60 wide (`chaseHeading` 1) | |
| Messages, warnings, weapon line, clock | as cockpit | |

## Minimap (both views)

| Element | Position / behaviour |
|---|---|
| Square | x 6, y 338, 136 x 136 (`mapX mapY mapSize`), background black at `mapBgAlpha` 0.38 |
| Scan wedge | +-60 deg from the nose, green at `mapWedgeAlpha` 0.15 |
| Island | `map_island.bmp` rotated heading-up from the islandc MAPI rect (`mapImgX0..Z1`), clipped to the square, alpha 0.85 |
| Runway | white line from `terrain_base` (or the T03 defaults) |
| Area circle | 30 km around (320, 512) (`mapAreaX/Z/R`), half-alpha green, drawn when any part is in view |
| Own jet | `map_jet` at the centre (heading up) |
| Frame | `map_frame` 9-slice |
| Texts | range ("20") top-left, flares ("F40") top-right, small font |
| Contacts | `map_draw_contact` (T15) / all enemies while `mapShowAll` = 1: `map_arch` yellow unknown, `map_chev` red hostile, `_dash` + half alpha in memory, 6 heading tick, green square when designated; missiles = `dot` (own green, hostile red blinking 4 Hz); RWR ticks on the frame (`map_draw_rwr`) |
| Range | 10 / 20 / 40 NM = half the square (`mapRange`, `map_cycle_range()` for Tab) |

## Cost

`-hudtest ladder` on VM A, 800x600 fullscreen, 600 set (CPU ms, mean of 120 frames; test logs in `shots/logs/`):

| view | HUD ms | minimap ms | total (max) | quads | draws |
|---|---|---|---|---|---|
| cockpit (message, weapon line, clock, 4 map contacts) | 0.955 | 0.360 | **1.315** (1.380) | 180 | HUD 1 per atlas page + map 3 |
| chase | 0.526 | 0.360 | **0.886** (1.001) | 139 | same |

About 7.3 us per quad: every extra element costs; keep the quad count in mind (a 24-chord area circle on the map,
3 dashes per negative rung, no separate shadow pass).

## Target symbology (T15, 2026-11-13; `src/hud_tgt.*`, both views)

User rules: 3D-view contacts are hollow diamonds (HAFU only on the minimap), TD box around the designated diamond,
triangle for the selected one. Sizes are the atlas sprites of the set (600: diamond 14 px, box 26 px, `sel_tri`).
All offsets below are fixed integer offsets from the contact's rounded centre (cx, cy), so a contact's parts never
move against each other; `boxHalf` = half the `tgt_box` (13 px at 600), k = h / 480.

| Element | Position | Behaviour |
|---|---|---|
| Diamond | `tgt_diamond` at (cx, cy) = projected render position, rounded once | yellow unknown, red hostile; MEMORY = `tgt_diamond_dash` at `tgtMemAlpha` 0.5; drawn anywhere on screen (cockpit too), not inside the minimap square |
| Kill | grey diamond (0xFFB4B4B4) at the kill position for `killDiamondSec` 3 s, then fades in `tgtFadeSec` 0.8 s; "SPLASH" large green, centred, bottom `boxHalf + 4k` above the centre, `tgtSplashSec` 1.5 s | own timers per aircraft id (outlive radar's 2 s DEAD listing); on screen only |
| Selected | `sel_tri` tip at (cx, cy - boxHalf - 3k) | constant offset (clears the TD box; never moves when a designation comes or goes) |
| TD box | `tgt_box` at (cx, cy); `tgt_box_dash` while the lock is in memory | cockpit: inside the HUD frame only; outside it the box sits on the frame (inset boxHalf + 1) on the ray from the waterline, flashing 3 Hz; chase: same at the screen edge (inset boxHalf + 4k, stops at the minimap) |
| Locator line | from 8k to `tgtLocatorLen` 60k from the waterline (cockpit) / aim cross (chase), `loc_arrow` at the tip, degrees off the nose (small) centred 6k beyond the tip | only while the TD box is clamped; direction = shortest-rotation screen direction (continuous through "behind") |
| Label | small green, centred, cap top at (cx, cy + boxHalf + 2k); overlap: left edge at cx + boxHalf + 3k, vertically centred | range "8" (x100 ft < 1 NM) / "9.4" (< 10 NM) / "17"; designated adds altitude kft ("12.4  17"); MEMORY 60 % alpha; kept on screen at the edges |
| Declutter | designated + selected + `tgtLabelOthers` 2 nearest others (max 4) | holders change only when a candidate is 15 % nearer and the holder held >= 0.5 s |
| Overlap | centre distance < `tgtOverlapPx` 12k to an earlier-drawn diamond: label to the right | leaves at 15k; draw order: others by id, then selected, then designated (on top) |
| Off-screen arrows | `offscreen_tri` `tgtArrowInset` 20k inside the screen edge (or 10k outside the minimap square) on the ray from the screen centre, rotated towards the contact | contacts within `tgtArrowNm` 20 NM (leave at 20.5) that are not on screen, nearest `tgtArrowMax` 6, HAFU colour; not for the designated (locator) |
| Near | within `tgtHideM` 400 m (leave at 450): only the TD box | |

Hysteresis bands (each proven by `-hudtest contacts` "jitter" cases with +-1.9 px / range noise: 0 changes): on screen
enter 14k inside the edge, leave 6k; minimap square +6k enter / -2k leave; HUD frame (TD) boxHalf +-2k; range unit
1.0 NM (0.95/1.05) and 10 NM (9.7/10.3); numbers 5 Hz (`tgtLabelHz`), dead band 0.7 quanta, reversal needs 2 updates.

Minimap contacts (T15 queue, `map_draw_contact`): radar contacts with HAFU arch/chevron (+ `_dash` in MEMORY),
designated last with the green square; `[hud] mapShowAll` 1: enemies the radar has not detected drawn as a solid
arch at 40 % alpha (`MAPF_NORADAR`, `mapNoRadar`); killed enemies: grey-red X (two 7k strokes, `MAPC_KILL`) for
`killMapSec` 15 s; live missiles as dots (`mapMissiles`); RWR lock ticks on the frame (`mapRwr`).

Screenshots: `shots/tgt_<cockpit|chase>_<in|behind>_<h>.png` (`in` = designated hostile in the HUD frame + selected,
overlapping pair at 14 NM, MEMORY contact over terrain, 0.8 NM contact, unknown behind (arrow), fresh kill with
SPLASH, old kill only on the map; `behind` = designated contact behind: locator + clamped dashed TD box, 25 deg bank,
kill 2.4 s old (grey, no SPLASH)), `shots/tgt_pipe_<view>.png` (radar pipeline run, t = 12 s).

## Weapon HUD (T16, 2026-11-13; `src/hud_wpn.*`)

DESIGN 10.6 + HUD.md 2.1 (F/A-18C NIRD/ASE circle, not a staple). Virtual 640x480 units (x k = h / 480); colours HUD green,
warnings red; every glyph/sprite/line is one RB_PREMUL quad with the baked shadow. `-hudtest zone` writes
`shots/wpn_<cockpit|chase>_<case>_<600|768>.png` (cases: far, rmax, rne, act, ttg, rmin, srm_search, srm_lock, visual, warn).

### Cockpit (LRM or SRM selected with a TD whose zone dlz_view publishes)

| Element | Position / size | Behaviour |
|---|---|---|
| ASE circle | centre (320, 240) = waterline, radius `[hud] aseRadius` 60 (T07 constant) | ONE quad from a ring texture generated per atlas set (premultiplied white, `line` stroke profile, baked shadow; 256x256 at 600 and 768, 128 KB each); polyline (24-64 chords) when it does not fit 256 (1050 set at r 60 needs 268 px) or `wpnRingTex` 0 / GDI fallback. Drawn first in the batch (own draw call) |
| Range scale | clockwise from 12 o'clock, angle = 360 x R / Rscale, Rscale = smallest of {2,3,5,8,10,15,20,30,40} NM >= 1.2 Rmax | up when 1.2 Rmax > 1.08 x scale (at once beyond 1.15), down when below 0.92 x the next lower scale; >= 1 s between changes |
| Rmax tick | outward radial stroke from the ring to r + aseOuter - (shadow + 1 px) | only once the first LIVE Rmax is out (`DlzView.ready`); angles clamp at 355 deg |
| Rne tick | double: two radial strokes 3 x stroke px apart, to r + 0.75 aseOuter | |
| Rmin tick | inward 6 | |
| Range pointer | `ptr_tri` outside the ring, tip at r + stroke/2 (pulled in if its far corners would pass r + aseOuter), pointing inward | pinned at 355 deg and flashing 3 Hz beyond Rscale (back below 0.98 Rscale) |
| Aspect pointer | thin (0.6 profile) outward line r .. r + 0.55 aseOuter at the target heading minus ours (12 o'clock = same heading, 6 = head-on) | `[hud] aspectPointer` |
| Steering dot | `dot4`, offset = angle nose -> `DlzView.leadDir`, the ring = `aseDegLrm` 20 / `aseDegSrm` 15; clamped to the ring | sticky whole-pixel position (moves when > 0.75 px away) |
| Breakaway X | `breakaway_x` on the waterline | on at R < Rmin x (1 + band), off beyond Rmin x (1 + 2 band) after 0.3 s; SHOOT is off while it shows |
| Readouts | numbers right-aligned so a 4-digit number starts at x 396 (= altitude box left edge); cap tops 290 / 304 / 318 | "1010 Vc" (10 kt, 5 Hz), "19.9 RNG" (0.1 NM, < 1 NM in 100 ft: "18"; unit switch 0.95 / 1.05 NM; 5 Hz), third line "17 TOF" (NOW fly-out, 2 Hz, shown/hidden with a 0.3 s latch) / "17ACT" -> "9TTG" (own missile at the TD; the switch is on the activation frame) / SRM "6TTG". Dead band 0.7 quanta + 2-update reversal rule |
| SHOOT | large, centred on the TD box x, cap bottom 16 above the box top (T15's selection triangle in between) | below the box (cap top boxHalf + 14 under the centre) when the top would pass y 148 (cockpit) / 48 (chase); back 6 inside. Rules below |
| SRM seeker | `seeker` sprite (3 deg at the 65 deg FOV, 1:1) at the waterline while searching, on the locked target when locked | lock display latched (raw must differ 0.12 s, shown >= 0.3 s); outside the HUD frame: clamped to it and flashing 3 Hz (hysteresis 2) |
| VISUAL | LRM selected, no TD: dashed circle (16 dashes) radius aseRadius + "VISUAL" small at (320, 381) | DESIGN's 20 deg circle would be r ~183 and cross both boxes |
| Weapon line | small, centred (320, 368): "AIM-120 16" / "AIM-9 40" (`wpnNameLrm` / `wpnNameSrm`) | string rebuilt on change. T17: keep `hud_set_extras(.., "", ..)` (T07 would draw a second line) |

### SHOOT rules (honest: only what the published zone says)

raw = `ready` && Rmin < R <= Rmax && !(NOW flown && missed) [&& SRM: seeker locked on the TD]; flashing raw also needs
R <= Rne && !(NOWESC flown && escapable) && the TD not in radar memory. Display: off -> steady needs the banded condition
(R <= Rmax x (1 - `wpnShootBand` 0.03), R >= Rmin x (1 + 2 band)) for `wpnShootOnSec` 0.2 s AND a completed dlz cycle
since it started; steady -> flashing likewise with R <= Rne x (1 - band); any loss of raw (flash: of the no-escape part)
switches at once. Measured: 0 frames with SHOOT outside the published zone in every scenario; approach LRM 3 changes
(off -> steady 1.4 s after crossing Rmax -> flashing 1.8 s after Rne -> off at X), boundary hovers min hold 0.4-2 s.

### Chase view

| Element | Position | Behaviour |
|---|---|---|
| DLZ bar | spine at the TD box centre x + (max(SHOOT half width, boxHalf) + 4), 50 tall centred on the TD centre (bottom = 0, top = Rscale) | thin spine; zone line 3 beside it from Rmin to Rmax; ticks 6 wide (Rmax, Rne double 1.5 above/below, Rmin); caret `ptr_tri` 7 beyond the spine pointing at it (pinned at the top + flashing beyond Rscale). Mirrored to the left of the box when it would leave the screen (back with 20 margin) |
| Third line | small, centred under the bar (cap top bar bottom + 4) | "17 TOF" / "17ACT" / "9TTG" |
| SHOOT | as cockpit | |
| SRM seeker | at the aim cross (projection of 1 km along the nose) / the locked target, scaled to 3 deg at the chase FOV | screen-clamped + flashing outside |
| Weapon block | centred x 560: name small y 410, count large y 422, "FLR 40" small y 442, "TONE" (SRM lock) / "VISUAL" small y 455 | |

### Warnings (both views, red)

| Element | Position | Behaviour |
|---|---|---|
| MISSILE | large, centred (320, 155) cockpit (between the heading caret 144 and the circle's outer reach 172), (320, 120) chase | flashing 4 Hz while a hostile missile homes on the player and `radar_rwr_missile_warn` says so; on at once, off 0.5 s after the last one |
| Threat arrows | `threat_arrow` tips on a ring r `wpnThreatRing` 160 around (320, 240), at the missile's plan-view bearing relative to our heading (12 o'clock = ahead, RWR style), pointing out | max `wpnThreatMax` 3, nearest at full alpha (kept unless another is 10 % nearer), others `wpnThreatAlpha` 0.6 (hg_* cannot rotate and scale one sprite). 160 clears the boxes (corner 129), bank scale (121), weapon line and minimap |
| LOCK | small, centred (320, 318) cockpit (under the circle, over the bank pointer), (320, 138) chase | an enemy has the player locked; on after 0.1 s, off after 0.5 s, shown >= 0.3 s |

### Gap check (user rule) and cost

`-hudtest zone` measures the farthest drawn ASE element from the circle centre: 85.00 px at 600 (limit r + aseOuter 85.00),
108.80 px at 768 (108.80): gap to both boxes 10 / 13 px (minimum 7 / 8). Everything attached to the circle is bounded by
r + aseOuter by construction, so T07's `-hudtest layout` table (all four sets) remains the proof: 8 / 10 / 13 / 17 px, ok.

| view | case | weapon HUD ms (VM, 800x600, avg / max) | quads | draws |
|---|---|---|---|---|
| cockpit | warn (circle, ticks, SHOOT, readouts, MISSILE, LOCK, 3 arrows) | 0.289 / 0.40 | 46 | 2 (ring + atlas) |
| cockpit | rne (circle, flashing SHOOT, readouts) | 0.217 / 0.32 | 33 | 2 |
| chase | warn | 0.229 / 0.26 | 38 | 1 |
| chase | rne | 0.158 / 0.18 | 25 | 1 |

## T18 overlap rules (2026-11-14)

| Rule | As built | Hysteresis / proof |
|---|---|---|
| MISSILE / LOCK slots | each has 3 slots (text centre x, cap top y): cockpit MISSILE (320, 155) home, (212, 155) left, (428, 155) right; cockpit LOCK (320, 318) home, (236, 334) left, (404, 334) right (below the readout block, clear of the 30/45 deg bank ticks); chase MISSILE (320 / 170 / 470, 120), chase LOCK (320 / 170 / 470, 138). A text leaves its slot when the **TD cluster** touches it (2 px): TD box, SHOOT (with the gap to the box, whenever the zone is shown, drawn or not), the TD label (hud_tgt, `hudtgt_td_label`), the chase DLZ bar + third line. It goes to the free side away from the TD (8 px clear), and back home once home is 8 px clear | 6 px band (2 / 8). `-hudtest zone` part keepout: TD box over a 20-unit grid of the whole screen, both views: 0 overlaps in 1653 frames; on LOCK's boundary +-1.9 px noise for 300 frames: 0 changes |
| Ladder keep-out | `hg_exclude` holds 6 boxes: speed box + AOA/M/G stack and altitude box + VVI are one box each (chase: speed + G, altitude); the weapon HUD adds its fixed texts of the previous frame (`hudwpn_keepout`: cockpit readout block while the zone is shown (fixed 4-digit + "TTG" width), weapon line + VISUAL (widest name), MISSILE while warned (not by flash phase), LOCK while shown; chase weapon block, MISSILE, LOCK). Ladder lines are cut around them and ladder numbers touching them are skipped | boxes depend on states (zone, mslOn, lockOn), not on numbers or flash phases, so nothing pops at 4 Hz |
| Threat arrows | still tips on the 160 ring at the plan-view bearing, but an arrow whose bounding rectangle (dart length x 2 x half-width + shadow) touches a keep-out (flight HUD boxes + stacks of this frame via `hud_keepout`, the weapon texts and MISSILE / LOCK at their current slots) moves **outward along its bearing** to the first clear radius (max +60) | on when touching at 0 margin, off when clear by 4 units, per missile serial; the bearing is unchanged. Test: every 0.5 deg in both views, zone and VISUAL cases: 0 hits (exact dart triangle); +-0.6 deg noise on a push boundary: 1 change. Pushed bearings (cockpit, zone case): 71-97 (altitude box/VVI), 109-136 (readouts), 162-200 (weapon line), 241-255 (speed stack), 293-314 (MISSILE in its left slot) |

## T16b chase-view weapon HUD (2026-11-14)

User (M2 play-test): "the weapon HUD in the chase cam too". `[hud] chaseAse` 1 (default) gives the chase view the
cockpit's weapon HUD; 0 restores T16's compact DLZ bar beside the TD box (kept as an option, off: redundant with the circle).

| Element | Position (virtual, x k) | Behaviour |
|---|---|---|
| Circle centre | x 320, y = the **steady-state aim cross**: 240 - P11 x 240 x tan(atan(U/(D+L)) - atan(U/(D+1000))) with the camera's `chaseDist` D 24, `chaseUp` U 6, `chaseLook` L 40 and this frame's projection P11; clamped to y 200..216 (`HudLayout.chAseY0/Y1`); y on a sticky whole pixel (0.75 px band) | = where the aim cross sits in steady flight: 205 at FOV 70, ~214 at 85 (speed/AB FOV). Independent of the camera lag, roll lag, shake and buffet: it moves only with the camera's smoothed FOV, 1 px at a time. The live aim cross (T07) wanders inside it in manoeuvres (up to ~18 px at 600 in the lag test) |
| Ring, ticks, range pointer, aspect pointer | as cockpit (radius `aseRadius` 60, all within `aseOuter` 8), same ring texture | same scale / pin / hysteresis rules |
| Steering dot | offset = angle between the **nose** and `leadDir` (edge = `aseDegLrm/Srm`), direction = that offset on the camera's screen axes | the chase camera looks ~5 deg below the nose and lags it, so the camera frame is not used for the angle |
| Breakaway X | circle centre | |
| Readouts Vc / RNG / TOF -> ACT -> TTG | numbers right-aligned so a 4-digit one starts at x 396; first cap top 44 below the circle centre (244..260), rows 14 | clear of the chase altitude box (bottom ~239) and the jet's right wing; ladder keep-out + threat-arrow keep-out (`hudwpn_keepout` chase: readouts, weapon block, MISSILE, LOCK) |
| SHOOT | above the TD box as before (below it near the top, limit y 48) | |
| SRM seeker | on the LIVE aim cross while searching (conformal: the seeker looks along the nose), on the locked target when locked, 3 deg at the chase FOV | |
| VISUAL | dashed circle (radius aseRadius) at the circle centre; "VISUAL" in the weapon block (y 455) | |
| Weapon line | the chase weapon block bottom right (unchanged) | a centred line at 368 would sit on the jet |
| MISSILE / LOCK | chase home slots moved to (320, **98**) / (320, **116**) (were 120 / 138: inside the circle's reach 132..); side slots x 170 / 470 at the same heights | T18 slot rules unchanged |
| Threat arrows | unchanged (ring 160 around the screen centre; the circle's reach stays within 108 of it); pushed off the chase readouts too | |

Gap rule: `-hudtest layout` checks the chase circle over its whole clamp range (y 200..216) at every set: 54 / 67 / 86 /
118 px to the chase boxes at 480 / 600 / 768 / 1050 (minimum 5 / 7 / 8 / 11). `-hudtest zone` part `chase`: reach
85.00 px (600) = r + outer at both clamp ends, readouts never on the altitude box or the circle, MISSILE / LOCK homes
never on the circle.

Cost (`-hudtest zone`, VM A, 800x600, avg / max of 200 frames): chase warn (circle, ticks, SHOOT, readouts, MISSILE, LOCK,
3 arrows) **0.364 / 0.39 ms**, 51 quads, 2 draws (T16 chase bar: 0.229, 38 quads); chase rne 0.254 ms, 38 quads. Combat bench
chase (`-bench 60 -benchroute combat -preset low`): 29.94 fps, missed 0.56 %, hud section 1.26 ms (T18: 1.20).
Screenshots: `shots/chase_wpn_<case>_<600|768>.png` (synthetic `-hudtest zone` scenes: far, rmax, rne, act, ttg, rmin,
srm_search, srm_lock, visual, warn, visual_warn) and `shots/chase_wpn_game<1-3>_<600|768>.png` (combat bench route, real
scene with the jet model).
