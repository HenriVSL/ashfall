# F/A-18C-style HUD: reference and implementation notes

## Verification (read first)

This file was first written from memory, then checked against the real documents:

- **[EA]** Eagle Dynamics, "DCS: F/A-18C Early Access Guide" (English, 424 pages, 2024 edition). Page numbers below are the
  printed page numbers (= PDF page numbers).
  https://www.digitalcombatsimulator.com/upload/iblock/ea9/lxf69u2uk1fhqq7ndb55z9egzabe8hdg/DCS%20FA-18C%20Early%20Access%20Guide%20EN.pdf
- **[EA-old]** older 156-page EA Guide (no AIM-120 chapter; AIM-9/AIM-7 HUD text identical to [EA]); mirror:
  https://throughtheinferno.com/wp-content/files/DCS%20FA-18C%20Early%20Access%20Guide%20EN.pdf
- Pixel positions were measured from the guide's own HUD screenshots (Fig. 22 p.77, Fig. 155 p.292, Fig. 157 p.296).
  They are approximate (+-1 deg). The HUD in those shots is the DCS cockpit view.
- NOT read: Chuck's Guides (https://chucksguides.com/aircraft/dcs/fa-18c, searched but not fetched), Hoggit wiki, NATOPS.
  So nothing here relies on them.

What was WRONG in the first version, now fixed:
1. **There is NO DLZ "staple" on the Hornet HUD.** (That is the F-16.) The Hornet HUD's launch zone is a circle:
   the NIRD/ASE circle, with Rmin/Rne/Rmax tick marks and a range pointer around it (section 2). The vertical
   staple-like scale is on the radar (DDI) page only.
2. The ASE circle is a fixed-size circle centred on the waterline (not range-scaled); the steering dot's gain changes instead.
3. Time-to-active: shown as "NNACT" (seconds) pre-activity, switching to "NNTTG" (time to go) once the seeker is active
   ([EA] p.293, 296-297). My earlier "T"/"A" prefix guess was wrong in form (suffix, not prefix).
4. A bank angle scale DOES exist (ticks 5, 15, 30, 45 deg + pointer), [EA] p.77-78. Hidden by REJ 1.
5. "LOSE" cue: NOT found anywhere in the guide. Removed. (The real lost-lock indications are: TD box hashed = MEM mode,
   TD box flashes = target outside HUD FOV, "GO STT" cue for AIM-7.)
6. HUD total FOV is 20 deg: [EA] p.171 "GACQ ... 20 deg dashed circle on the HUD that encompasses the entire HUD field of view".
7. Peak G is only shown when above 4 G; heading scale is a 30 deg window (not +-15 guess; same thing, now confirmed).
8. AIM-9 seeker circle size is still not stated in the guide (see unknowns). BST acquisition circle = 3.3 deg; HMD seeker FOV = 5 deg.
9. Gun cross is 2 deg ABOVE the waterline (not "waterline ~4 deg").

Still unknown / unverified (marked UNSURE below): AIM-9 HUD seeker circle diameter, ASE circle diameter in degrees,
pitch ladder rung spacing (5 deg per guide screenshots; labels are the numbers 5/10), HUD stroke widths.

---------------------------------------------------------------------------
## 1. Basic HUD (NAV and A/A, same set; [EA] p.77-78, Fig. 22)

Independent of master mode EXCEPT bank scale, vertical velocity, heading scale ([EA] p.77).
Brightness-reject REJ 1 removes Mach, G, bank scale and pointer, airspeed box, altitude box, peak G; REJ 2 additionally
removes heading scale, caret, NAV range, timer ([EA] p.49).

```
 clock/time (bottom-left)                         
                   [ heading tape: 30 deg window, ticks every 5, labels every 10 ]
                      250      260      270          caret at centre (T under it if true hdg)
                        |  .  |  .  |  .  |            
                           5 ┌__      __┐ 5     <- +5 deg ladder rung (hooks point DOWN)
   ┌───────┐
   │  337  │  (airspeed box, CAS kt)                          -70   (vertical velocity, ft/min)
   └───────┘                       ( circle = NIRD/ASE        ┌───────┐
   α  2.6     (AOA, deg)            when a missile is        │ 25410 │ (altitude box; "R" next to it
   M  0.84    (Mach)                selected, see sec 2)     └───────┘  if radar alt; flashing "B" if
   G  1.0     (current g)                                                radar invalid)
      1.6     (peak g, only > 4 g)          o <- velocity vector (VV)
   5 L----  ----J 5     (ladder)        
  07:16:56 EI                     / . ' \   <- bank scale arc (ticks 5/15/30/45) with pointer
  10 L----,  Y  ,----J 10 (negative rung: dashed, hooks UP)          
                              AB 10      <- weapon type + count, bottom centre
```

| Element | Content / behaviour | Approx. position (deg from waterline, HUD FOV 20 deg; measured from screenshots) | Source |
|---|---|---|---|
| Heading scale | 30 deg moving scale, caret fixed at centre, magnetic or true ("T" below caret) | y about +4.5 deg (upper area), width +-8 deg visible in shot, labels 3 digits in the screenshot (250 260 270) | [EA] p.77 |
| Airspeed | Calibrated airspeed kt, in a BOX, left | x about -5 deg, y about +1 deg, above the AOA/Mach/G stack | [EA] p.77, Fig. 22 |
| Altitude | Baro or radar alt in a BOX, right; "R" next to it for radar; flashing "B" if radar invalid; baro setting shown below box for 5 s on change | x about +5 deg, y about +1 deg | [EA] p.77-78 |
| Vertical velocity | ft/min, signed number above the altitude box | x +5 deg, y about +2.5 deg | [EA] p.77, Fig. 22 |
| AOA | "α" symbol + true AOA in degrees, left, below airspeed | x about -8.5 deg, y about -3 deg | [EA] p.77 |
| Mach | "M 0.84" below AOA | x -8.5, y about -4.2 | [EA] p.77 |
| Current G | "G 1.0" below Mach | x -8.5, y about -5.3 | [EA] p.77 |
| Peak G | number below G; only shows peak above 4 G | x -8.5, y about -6.4 | [EA] p.77 |
| Velocity vector | small circle with ticks; flashes if data not accurate; can be caged to centre with a ghost VV | conformal | [EA] p.78 |
| Pitch ladder | conformal; VV position on it = flight path angle, waterline = pitch. Positive rungs solid with hooks toward the horizon, negative rungs dashed. Numbers at rung ends (5, 10, ...). Rung every 5 deg in the shots (5 and 10 visible). Centre gap in rungs | conformal; half-length about +-4 deg, ~1.5 deg centre gap (from shots, UNSURE) | Fig. 22 p.77 |
| Bank angle scale | arc of tick marks at 5, 15, 30, 45 deg with centre caret/pointer, lower middle of the HUD | y about -6 to -8 deg, arc radius about 4 deg (UNSURE exact) | [EA] p.78 |
| Weapon status | weapon type + count ("AB10" = AIM-120B x10 in shot; "AIM-9 n", "GUN 578") bottom centre; selected/armed cue | x 0, y about -9 deg | [EA] p.292-297 figs; p.119-124, p.129 |
| Time/steerpoint info (timer "07:16:56 EI") | bottom-left | x -7, y -7.5 | Fig. 155 |
| Fuel | NOT on the HUD (not in the guide's list) | - | [EA] p.77-78 |

Colour: monochrome green (anything near RGB 0,255,70). Strokes are thin (1 px in the guide's screenshots at ~1080p
HUD size). GAME: use 2 px strokes with a dark 1 px offset shadow for readability at 640x480.

### 1.1 Mapping to 640x480

Assumption: 65 deg horizontal FOV, focal length f = 320 / tan(32.5 deg) = 502 px, about 8.8 px/deg at centre
(use px = f*tan(angle) for conformal items; ladder rungs 5 deg apart = 44 px).
The real HUD is only 20 deg; at 8.8 px/deg that would be 176 px wide: too small. GAME: scale the layout by 1.5 and
use a ~30 deg x 24 deg virtual HUD frame (265 x 212 px) for non-conformal items. Items below use the real offsets x 1.5.

| Element | HUD offset (deg, x1.5 scaled) | 640x480 px (centre 320,240; y down) |
|---|---|---|
| Heading tape | y +6.7 | y about 181; width 30 deg real = 45 deg scaled -> clamp to about 270 px (+-135) |
| Airspeed box | x -7.5, y +1.5 | right edge at x about 262, y about 227 |
| Altitude box | x +7.5, y +1.5 | left edge at x about 378, y about 227 |
| Vertical velocity | x +7.5, y +4 | x 378, y 205 |
| AOA / Mach / G / peak G | x -13, y -4.5 / -6.3 / -8 / -9.6 | x 205, y 280 / 295 / 311 / 325 |
| Weapon + count | y -13.5 | x 320, y 360 |
| Bank scale | y -9..-12, radius 6 | arc center (320, 345), radius 53 |
| NIRD/ASE circle (missile selected) | radius about 5 deg x1.5 UNSURE | radius about 66 px at 640x480 |
| Vc / range readout | x +7, y -6.5 | x 382, y 297 |
| Time to active / TTG | x +7, y -11 | x 382, y 337 |

At 1024x768+ scale by height/480.

---------------------------------------------------------------------------
## 2. A/A weapon symbology

### 2.1 AIM-120 / AIM-7 / AIM-9 radar-locked HUD (NIRD/ASE circle, NOT a staple)
Source: [EA] p.292-297 (AIM-120), p.274-276 (AIM-9), p.139-142 old guide (AIM-7).

```
            TD box (square; hostile = rotated 45 deg = diamond with inverted V)
            SHOOT   <- above the TD box
                        ____
                     .-'  o  '-.   top (12 o'clock) = 0 range; relative range increases CLOCKWISE
        aspect ptr-> |    +    |   <- Rmin, Rne, Rmax tick marks inside/outside the ring
                      '-._ _.-'    <- target range pointer rides the ring
        steering dot: "." moves around centre; put it INSIDE the circle
                              960 Vc    <- closure rate (kt)   right of circle
                              19.9 RNG  <- range (NM; below 1 NM shows 100s of ft)
                              17 ACT    <- time to active (s); after activation: 11 TTG
```
Circle is centred on the aircraft waterline.

| Element | Behaviour | Source |
|---|---|---|
| NIRD/ASE circle | Fixed-size circle centred on waterline. Range markers around it; relative range starts at 12 o'clock and increases clockwise. It does NOT change size with the engagement: the steering dot's rate of change is adjusted instead | [EA] p.292 |
| Rmin | Computed minimum launch range tick | p.292 |
| Rne | No-escape range: target stays within Rmax even if it turns 180 deg instantly | p.292 |
| Rmax | Max range vs a non-manoeuvring locked target | p.292 |
| Raero | Diamond outside the ring; max aerodynamic range when own speed > missile speed and missile can still pull 5 g | p.293 |
| Target aspect pointer | Shows relative target heading on the ring | p.292 |
| Steering dot | Lead angle steering; fly it into the ASE circle for a good shot (less g pulled by missile). Flashes when within 15 deg of radar azimuth limit or 5 deg of elevation limit | p.293, 295 |
| TD box | Line of sight to the locked target; flashes if target outside HUD FOV; hashed when radar in MEM (memory) mode; hostile = 45-deg diamond (+ inverted V for AIM-9) | p.293 |
| Target locator line | When TD is outside the HUD, a line/arrow attached to the gun cross (waterline) points toward the target with degrees-to-target next to it | p.293, 124 |
| SHOOT cue | Word "SHOOT" above the TD. Solid when target within Rmax; flashing when within Rne | p.293, 295 |
| Breakaway X | Displayed when range < Rmin | p.293 |
| Vc / RNG | Closure rate and range numbers right of the circle (kt, NM; under 1 NM range shown in 100s of ft) | Fig. 155; p.124 |
| Time of flight (pre-launch) | Estimated seconds for the missile to reach the target | p.294 |
| AIM-120 time to active (ACT) | After launch: "NNACT" counts seconds until the missile's own seeker goes active (pitbull). On activation the same field changes to "NNTTG" (time to go to impact). On the radar page the fly-out pyramid shows time and an "A" once active | p.293, 296-297 |
| Visual launch (no lock) | A dashed AIM-120 FOV circle and "VISUAL" at bottom centre; weapon type "AB/AC" + count | p.289-290 |
| AIM-9 time of flight | before launch seconds; after launch "TTG" with a "SW" suffix | p.276 |
| Maximum aspect cue (radar page) | 1-18 shot quality number | p.294 |

### 2.2 DLZ on the RADAR page (not HUD)
Vertical scale on the right of the radar (DDI) page with Rmax / Rne / Rmin marks, a caret for target range, closure
(Vc) number left of the caret, fly-out/TTG pyramid ([EA] p.294, 296-297). GAME: this is the staple-like display, but it is
a head-down element; we can borrow it as an optional mini bar in the chase HUD.

### 2.3 AIM-9 (Sidewinder)
| Element | Behaviour | Source |
|---|---|---|
| Seeker boresight reticle | no radar lock, no ACM: a circle at boresight; put it over the target until the high-pitched lock tone; angle within 15 deg of boresight; trigger fires | [EA] p.273 |
| Seeker circle | Shows seeker head position. Flashes if outside HUD FOV. When slaved to the radar lock the TD box and seeker circle coincide. Diameter: UNSURE (not stated; HMD seeker FOV reticle is 5 deg; BST acquisition circle 3.3 deg) | p.276 |
| Caged/uncaged | Cage/uncage button toggles seeker between L&S target and boresight. Uncaged seeker tracks target within its FOV | p.273, throttle section |
| Lock tone | low growl vs high-pitched lock tone when locked; tone is the primary lock indication besides SHOOT | p.265, 273 |
| SHOOT cue | above TD, flashing in Rne | p.276 |
| ACM sub-mode cue | text "ACM" when in ACM sub mode | p.276 |

### 2.4 Gun and ACM
Source: [EA] p.263-268 (A/A GUNS HUD), p.171 (ACM modes).
| Item | Behaviour |
|---|---|
| Gun cross | Fixed, centred in azimuth, 2 deg ABOVE the waterline (boresight) |
| Funnel (radar not tracking) | Fixed 2000 ft range assumed; 12.5 mil stadiametric reticle for a 25 ft wingspan; pippers for 1000 and 2000 ft |
| Radar tracking: gun reticle | Circle showing predicted impact based on manoeuvring; target range as a clockwise arc inside the reticle; max gun range cue on the arc |
| FORESIGHT 1G / 9G cues | two horizontal lines with centre ticks (upper longer = target 1G, lower shorter = 9G) showing target manoeuvre potential |
| BATR cue | bullet-at-target-range, shows where each bullet is when it reaches target range |
| Rounds | "GUN 578" legend; XXX on last rounds |
| SHOOT | Shown until miss distance > 30 ft; needs STT, target inside gun Rmax, target centre within 20 ft of the 1G-9G line |
| ACM modes | GACQ (automatic with guns): dashed 20 deg circle = whole HUD, 5 NM. BST: dashed 3.3 deg circle at boresight, 10 NM. VACQ: two vertical dashed lines -13..+46 deg, 5 NM. WACQ: rectangle in lower right of HUD, 10 NM, slewable. All auto-lock the first target found ([EA] p.171) |

---------------------------------------------------------------------------
## 3. Reduced HUD for a 3rd-person chase view

Not verified against Ace Combat 7 in this pass (UNSURE; based on memory: AC7 shows a centre crosshair, target boxes with
lock-on state and distance, missile/weapon status bottom right, speed/alt, radar). Suggested minimal set for our game:

1. Velocity vector (flight path marker) at the projected 3D point the jet flies toward. Keep.
2. Gun cross/pipper at the nose direction (2 deg above waterline convention is unnecessary in chase view; put it at the projected nose line). Keep.
3. Target box (TD) on the selected enemy: square = locked, diamond = hostile (all enemies hostile: just use the diamond), flashing/hashed states (off-screen = flash, lost = hashed). Keep.
4. Off-screen target locator line + degrees from the gun cross. Keep (this is the most useful element in chase view).
5. SHOOT cue (solid in Rmax, flashing in Rne) above the TD. Keep.
6. Range + Vc readout right of the target box/circle (3 digits max). Keep.
7. NIRD/ASE circle with steering dot only for the AIM-120-like long range missile: optional. For arcade, replace with a lock-on ring that fills as lock tone builds.
8. Time to active / TTG counter ("NNACT" -> "NNTTG") under the readout. Keep for the long-range missile.
9. Airspeed box (left) and altitude box (right) numbers. Keep; drop vertical velocity.
10. Weapon line bottom centre ("AIM-120 6", "GUN 578"). Keep.
11. Heading tape: keep small (optional). Pitch ladder: keep only 0 and +-10 deg rungs, low alpha (attitude cue is needed in third person).
12. Cut: Mach/G/AOA/peak G (keep G/AOA only if the game has stall or G-LOC), bank scale, REJ logic, ACM text, Raero/Rne ticks (unless the circle is used).

---------------------------------------------------------------------------
## 4. Rendering advice (Direct3D 6, 640x480)

- Everything is drawn in screen space as D3DTLVERTEX (rhw = 1) triangles in one alpha-blended batch with a single glyph
  atlas texture. No GDI. No reliance on D3D line width: draw strokes as quads (2 px) so all drivers match.
- Include a solid white 2x2 texel region in the atlas so lines/ticks/boxes use the same texture and one draw call.
- Stroke font: define glyphs as polylines on a 5x7 grid (digits, capitals), render once into the atlas via the Pillow
  pipeline at two sizes (small for 640x480, 2x for 1024+). One textured quad per character at runtime (~100-150 quads
  per frame). Point-sample the atlas; tint the vertex colour for HUD green / warning red / flashing.
- Conformal geometry (ladder rungs, horizon, VV, TD box, locator line) is recomputed every frame; ladder rungs only within
  about +-15 deg and clipped to the HUD frame.
- Rates for text updates (build digit strings only at this rate, cache vertices):
  | Item | Rate |
  |---|---|
  | geometry (ladder, VV, boxes, tape scroll) | every frame |
  | airspeed, altitude, heading digits | 10 Hz |
  | Mach, G, AOA, vertical velocity | 5-10 Hz |
  | peak G | on change |
  | range, Vc | 5 Hz |
  | ACT / TTG counter | 1-2 Hz (whole seconds) |
  | weapon line, master text | on event |
  | flashing cues (SHOOT in Rne, off-screen TD, seeker circle) | 2-4 Hz toggle |

### 4.1 Atlas list
Glyphs: 0-9, minus, plus, period, colon, slash, degree; letters A-Z (need: SHOOT, GUN, AIM, ACT, TTG, RNG, VC, MAX, R, B,
M, G, T, X, VISUAL, ACM, MEM, PULL UP, WARNING, STALL, FUEL, BINGO); the alpha (AOA) symbol; "kt" optional.
Symbols (sprites or procedural quads):
1. Velocity vector (circle + 2 side ticks + top tick) and ghost VV.
2. Waterline/aircraft reference ("Y"/W shape) and bank pointer caret.
3. Gun cross, gun reticle ring, funnel arc, pipper dots.
4. TD box (square) and TD diamond, hashed (dashed) versions, inverted V hostile mark.
5. NIRD/ASE ring, ring range ticks (Rmin/Rne/Rmax), range pointer, steering dot, aspect pointer, Raero diamond.
6. AIM-9 seeker circle, AIM-120 FOV dashed circle, ACM circle (3.3 deg).
7. Target locator line arrow head (and its degree number).
8. Speed/altitude box outlines (procedural), heading tape tick (short/long), heading caret, T marker.
9. Pitch ladder: rung segments solid/dashed, hooks up/down, number labels.
10. Bank scale tick marks (5/15/30/45 positions, L/R).
11. Breakaway X.
12. Missile warning arrow (not in this guide; game-specific).

## 5. Key decisions
- Hornet HUD = NIRD/ASE circle on the waterline for launch zone (no HUD staple). Use the staple only as an optional mini bar.
- 20 deg real HUD FOV; we enlarge the non-conformal layout by 1.5x, keep conformal items true-projected.
- Boxes: airspeed left, altitude right at the waterline height; AOA/M/G/peak G stack left under the speed box; weapon
  text bottom centre; bank arc at the bottom; heading tape on top.
- ACT/TTG shown as "17ACT" -> "11TTG" under the Vc/RNG readout; SHOOT above TD solid in Rmax, flashing in Rne.
- Remaining UNSURE: ASE and AIM-9 seeker circle diameters, exact ladder geometry, stroke widths, Chuck's Guide/Hoggit confirmations.
