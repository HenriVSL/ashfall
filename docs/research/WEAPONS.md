# WEAPONS research: HAFU, radar, missiles, DLZ/LAR for an arcade jet (Win98, VC6, P-II 233)

Status: written from public knowledge (DCS guides, Chuck's Guides, Hoggit wiki, Wikipedia, open
literature on missile guidance). No live web fetching was done in this pass, so URLs are
"where to look", not verified quotes. Items marked **(unsure)** are from memory or estimates.
Numbers marked **(game)** are our own recommendation, not real-world data.

Sources (public):
- DCS F/A-18C Early Access Guide (Eagle Dynamics, PDF in the DCS install `Doc/`), chapters on SA page, radar, AIM-120, AIM-9.
- DCS F-16C Viper Early Access Guide (same place).
- Chuck's Guides: https://chucksguides.com (DCS F/A-18C, F-16C guides; "Air-to-Air" sections).
- Hoggit wiki: https://wiki.hoggitworld.com (AIM-120, AIM-9, RWR, radar pages).
- Wikipedia: AIM-120 AMRAAM, AIM-9 Sidewinder, Proportional navigation, Missile engagement zone, MIL-STD-2525 (APP-6 / NATO joint military symbology).
- MIL-STD-2525 (public): https://www.jcs.mil/ (search "MIL-STD-2525D"), and APP-6 for NATO frames.
- Zarchan, "Tactical and Strategic Missile Guidance" (book; PN formulas are textbook).
- JSBSim https://github.com/JSBSim-Team/jsbsim (aircraft FDM, not missile-specific); Falcon BMS manual (public PDF; RWS/TWS/STT/DLZ chapter).

---------------------------------------------------------------------------------------------

## 1. HAFU symbology

HAFU = "Hostile, Assumed friend, Friend, Unknown" frame set, shown on the F/A-18 SA page, the
radar page (as contacts) and in the HUD/HMD (as the TDC box/ID). It is the Link-16 / MIL-STD-2525
air-track frame idea: the **shape of the frame carries the identity**, the **inside carries
extra data** (and in the Hornet the symbol is split into two halves for sensor source).

### 1.1 Frame shapes (MIL-STD-2525 / APP-6 air domain)

| Identity | Frame shape (air) | Real colour (2525 / SA page) | DCS Hornet SA page |
|---|---|---|---|
| Unknown | Cloverleaf (4-lobed "quatrefoil"); in a sensor-split HAFU simplified to a rectangle-ish top | Yellow | Yellow (unknown / "pending") **(unsure of exact shade)** |
| Pending | Cloverleaf, dotted | Yellow | rare |
| Friend | Rounded top arch (half-circle/"dome") on a flat base | Blue (2525), cyan/blue-green on Hornet displays | Green on SA page **(unsure; DCS Hornet uses green for friendly and red for hostile, blue is the 2525 default)** |
| Neutral | Square | Green | rare |
| Hostile | Diamond (upside-down chevron: peaked "roof" top) | Red | Red |
| Assumed friend | Dome like friend, with a "?" | Blue | Green-ish |
| Suspect | Diamond, dotted | Red/orange | - |
| Ambiguous (HAFU mixed) | Frame is shown as the **upper half in the colour/shape of one source, lower half another** | Mixed | Upper/lower differ |

Important detail for the HAFU in the Hornet: the symbol is a **half-frame pair**. Upper half is
**own-ship sensor** (radar, IFF interrogator, NCTR). Lower half is **off-board** (Link 16 /
datalink from AWACS or wingmen). When both agree the halves join into the full friend/hostile
frame. When they disagree (e.g. own sensor says unknown, datalink says hostile) you see
two different half-shapes: this is the "ambiguous" case.

Rule of thumb that matches what DCS draws:
- Upper half = OWN sensor. Lower half = OFFBOARD. Left empty = no data from that source.
- Own radar sees contact but no ID: **upper half yellow (unknown)**, lower half empty.
- Datalink says hostile, own radar has no ID yet: **lower half red**, upper half empty/yellow.
- Both say hostile: full red diamond.
- Extra marks: a **vector/velocity line** from the symbol showing heading; **altitude number**
  under or next to it (in 1000s of ft); **callsign / track number**; a **box/ring** around the
  designated target (L&S in Hornet is a solid square, DT2 is a dashed/other style); **bullseye
  info** text. **(unsure on exact glyphs)**

DCS Hornet colour specifics **(unsure)**: SA page uses yellow for unknown, red for hostile,
green for friend; radar page uses white/green for tracks with ID overlays. We don't need
DCS-accurate: user asked for yellow = unknown, red = hostile.

### 1.2 Mapping to a game (user wants no ground radar, own radar identifies)

Because there is no AWACS/datalink, the lower half is unused. Two options:
- **Simple**: only the upper-half concept. Symbols: yellow cloverleaf-ish (draw as a yellow
  square rotated slightly or a simple "U" shape) = unknown; red diamond = hostile; (optional)
  blue/green dome = friendly wingman.
- **Fancy**: keep the half-frame idea for flavour: while an ID is "in progress" the top half
  fills red from yellow (a short animation) and then the full diamond closes. Cheap to draw
  with 5-6 line segments.

Suggested contact state machine (per enemy, owned by the radar module):

| State | Display | Condition to enter | Condition to leave |
|---|---|---|---|
| UNDETECTED | none | outside radar volume / out of range / below detect probability | detected (see 2.3) |
| DETECTED (unknown) | yellow symbol, in the radar scope and minimap | inside scan volume, range < Rdetect(aspect) | lost for >N s -> becomes a "memory" ghost then deleted |
| IDENTIFYING | yellow, with a small progress arc | in radar track and range < Rid | timer completes |
| HOSTILE | red diamond | in-range, time on track >= T_id **or** within hard range R_id_near | lost (goes to memory red ghost) |
| MEMORY | dim/dashed symbol, extrapolated | lost radar for < 8 s **(game)** | deleted after timeout or reacquired |
| DESIGNATED (L&S) | red diamond + box | player long-presses S | player cancels / lost / target dead |

Recommended ID numbers **(game)**:
- All enemies are hostile (no friend/foe ambiguity), but must be **identified**: T_id = 2.0 s of
  continuous track once inside R_id = 0.85 * Rdetect, or instantly when closer than 8 nm
  (real: NCTR needs a good-aspect, range <~ 30-50 nm; very approximate **(unsure)**).
- Real life: NCTR (non-cooperative target recognition, jet engine modulation) takes a few
  seconds, works best nose/tail aspect, can fail from the beam. Optional flavour: ID time x2
  when target aspect near 90 deg (beam).
- Show a "?" glyph or yellow until identified; HUD box and radar-warning logic treat unknown as
  hostile anyway for firing (arcade friendly: allow firing at yellow if you want; ROE gate is
  optional flavour).

3D view marker at "range X": the user wants a triangle/marker in the 3D view. HUD target
indicator rules (as in Ace Combat and the real HUD):
- On-screen: draw the HAFU-ish diamond/box at the projected target position, with range
  (nm) and closure, only if range > R_visual (e.g. 3 nm, where the model is large enough).
  Draw nothing/just a bracket when the actual model is clearly visible.
- Off-screen: draw an arrow/triangle at the screen edge pointing toward the target
  (standard "target steering" in Ace Combat); colour by ID.
- Real F/A-18: HUD shows the target designator box (TD box) with closure and range tick
  scales; "Allowable steering error" (ASE) circle for AIM-120 (see 4.5).

Minimap (bottom-left): a radar scope: own jet at centre (or lower-middle), heading-up,
range ring at e.g. 40 nm (zoomable 10/20/40/80). Draw each contact as its HAFU colour shape
and a heading tick. This is the real radar page "B-scan/PPI" simplified. Show the scan
cone as a wedge (+-60 deg). Alternatively radar-azimuth B-scope: x = azimuth, y = range, that is
what the real radar shows, but a PPI map reads better for a hobby game.

---------------------------------------------------------------------------------------------

## 2. Radar model for an arcade game

### 2.1 Real radar (F-16 AN/APG-68, F/A-18 AN/APG-73; public-ish numbers)

| Item | Real (approx, public) | Notes |
|---|---|---|
| Azimuth scan | +-10 / 20 / 30 / 60 / 70 deg selectable; max ~ +-60 (Hornet 60, Viper ~60) | narrower = faster refresh, more dwell |
| Elevation bars | 1, 2, 4 bars; bar height ~ 2-4 deg; DCS lets the pilot tilt the antenna +-60 deg | the radar sees only the bars, not a cone |
| Max detect range (fighter sized RCS ~ 5 m2, head-on) | ~ 60-80 nm RWS (APG-68 spec claims ~ 80 nm on large target)  **(unsure)** | DCS Hornet: ~ 50-60 nm display scale 80 nm |
| Range scale | 10/20/40/80/160 nm | |
| Detect range scaling | R ~ RCS^(1/4); target aspect changes RCS; look-down reduces detection (clutter) | R_det = R0 * (rcs/rcs0)^0.25 |
| Doppler notch | Pulse-Doppler radars lose targets at **beam** (target velocity component along radar line of sight ~ 0, "notching") | |
| Scan time | One 4-bar +-60 sweep ~ 4-6 s (RWS), narrow scan ~ 1-2 s | updates are per scan, track history extrapolated |
| Lock time | STT lock from RWS: ~ 1-2 s once targeted, plus missile "uplink" | |

Public reference: Chuck's Guide F/A-18C radar chapter; Falcon BMS manual ("RWS, TWS, STT").

### 2.2 Modes (concepts)

| Mode | What it is | Radar scan while tracking | Enemy RWR sees | Missile use |
|---|---|---|---|---|
| **RWS** (range while search) | Search, all contacts seen as blips; position updates only each scan, low-quality velocity. | Full | A normal radar ping ("search") | Can designate a contact to launch (AIM-120 in "L&S" with scan continuing, from FCS prediction in HOBS, "PLT"). |
| **TWS** (track while scan) | Keeps scanning but builds tracks on up to N (Hornet: 10) contacts, with ID quality for a couple designated for missiles | Continues scan (narrower) | Often "track-ish" ping, not a hard lock | Fire at L&S track; missile gets mid-course updates by the radar |
| **STT** (single target track) | Radar locks one target and points beam at it continuously; best data, hardest to defeat on missile side but target RWR shows "launch/lock" (steady tone) | Dedicated beam | **Lock tone, high priority** | Best for AIM-9 radar-slaved and AIM-120 terminal; for AIM-120 not needed except in some modes (Hornet needs STT to guide until active in some situations) |
| **"Designate" / L&S / DT2** | **L&S** = Launch & Steering target (the "primary" target the missile steering cue and DLZ are shown for). **DT2** = Dual-Target 2: the second most important track (shown with a different box), for firing a second AIM-120 at a second target. In TWS the player bumps targets with "TDC" slewing and "designate" to assign L&S. | | | |

Real gameplay chain: contact (RWS) -> radar cursor slewed over it / "TDC depress" -> becomes L&S
(Hornet shows a solid box), radar goes STT (or stays TWS) -> DLZ shows on the HUD -> launch.

### 2.3 Recommended simplified model for the game

Goal: feels like real, cheap to compute, no per-bar elevation simulation.

**Scan volume** (own aircraft body frame, "radar cone"):

| Parameter | Value (game) | Comment |
|---|---|---|
| Azimuth half-angle | 60 deg | matches real max; the user's request |
| Elevation | +-30 deg (single "bar" approx: tilt fixed) or +-25 around pitch horizon; optionally auto-tilt toward designated target | skip antenna-tilt management; no manual tilt |
| Max range | 60 nm scope range (dispatch scale zooms 20/40/80) — but enemy detect range per aspect below | |
| Update rate | contact positions refresh each **radar sweep every 1.5 s** (staggered per contact) for flavour; in-between extrapolate | You can skip it: refresh every frame; the stepping makes it feel "radar-ish" and is *cheaper* if you only update a few contacts per frame |

Cone test per enemy (all vectors in own body frame after one 3x3 transform):

```
// d = enemy_pos - own_pos (world), R = |d|
// body-frame: b = Mt * d  (Mt = transposed own orientation)
az = atan2(b.x, b.z)      // can avoid atan2: compare b.x/b.z with tan(60deg)=1.732
el = asin(b.y / R)        // or compare b.y/R to sin(30deg)=0.5
inCone = (b.z > 0) && fabs(b.x) < 1.732*b.z && fabs(b.y) < 0.5*R
```
Cheap: no trig at runtime, two comparisons (z > 0, |x| < k*z, |y| < 0.5R).

**Detection range vs aspect** (game): R_det = R_nose * f(aspect) where aspect A = angle between
target's nose and the line to the radar (0 = nose-on/head-on to us, 180 = tail-on):

| Aspect (deg off target nose-on) | RCS factor approx | R_det multiplier (^0.25) |
|---|---|---|
| 0 (head-on) | 1.0 | 1.00 |
| 45 | 1.6 (more side area) | 1.12 |
| 90 (beam) | 3.0 | 1.31 but **Doppler notch** applies (see below) |
| 135 | 1.8 | 1.16 |
| 180 (tail) | 1.2 | 1.05 |

Real fighters' RCS is lowest nose-on, highest beam; that is the shape of the table **(unsure on
numbers; they are shaped plausibly, not sourced)**. Simplify to:
`Rdet = 45 nm * (1.0 + 0.25 * sin^2(aspect))` , min 20 nm for tail aspect on tiny targets.
Optionally enemy type scaling (bomber x1.3, stealthy fighter x0.5).

**Notching** (beam, ~90 deg aspect to our LOS): if |closure rate| < ~ 40 kt (i.e. target is flying
perpendicular to our line of sight) the radar loses the track after ~ 3 s *(unsure of exact
threshold; real notch is ~ 60-100 kt in the doppler filter of older radars)*. Gameplay: gives
the enemy a "beam" defensive manoeuvre. Cheap formula:
`closure = -dot(v_rel, d/R)`; if `fabs(closure) < 40 kt`: notchTimer += dt else notchTimer=0;
if notchTimer > 2.5 s: contact drops to MEMORY.

**Look-down / terrain**: ignore, or optional: if target altitude < 1000 ft AGL and we are above:
Rdet *= 0.7. Plus line-of-sight blocked by the volcano: one ray vs cone/heightmap
(cheap on a coarse grid). Radar and missile guidance respect LOS only if desired.

**Designate** (user wants S long press): states:

| Input | Action |
|---|---|
| S short | cycle through detected contacts sorted by (a) angle off nose or (b) range; "next" contact gets *selected* (TDC-like bracket, not locked) |
| S long (>= 0.4 s) | designate = L&S: box around contact; radar goes **STT-lock** on it |
| S long again on the same target / other key | un-designate |
| Space | fire the currently selected weapon at L&S (or at the selected contact if none) |

Lock time **(game)**: 1.0 s from designate to STT lock for AIM-120 (progress ring); IR seeker: 0.5 s
growl-to-tone (AIM-9 needs the seeker to see the target, FOV ~ 4 deg for the "boresight", 
see 3.1; with "slave to radar" it's the same lock).
During lock: radar locks the target (even if the contact leaves the scan cone, STT keeps
tracking while target is within gimbal limit ~ 60 deg).
Breaking lock: if aspect/gimbal exceeds limit (target outside +-60 deg off nose) -> lock drops after 1 s.
Add a "notch" break and an enemy "chaff/jam" break option.

**Enemy RWR (radar warning receiver)** (real F-16/F-18 RWR/ALR-67: shows bearing of emitters;
tones: search = chirps, track/lock = steady tone, missile launch = fast tone/"LAUNCH" text):

| Own aircraft action | Enemy RWR sees (game) | Enemy AI reaction |
|---|---|---|
| Radar on, scan hits enemy | tick (search ping), they know bearing roughly | maybe turn to meet / increase alert |
| Designated (STT lock) | "spike" / lock tone | they begin defensive reaction (turn away, beam, dispense chaff) with reaction delay 1-2 s |
| Missile launched (AMRAAM active) | "missile launch" warning + after pitbull "MISSILE" | break turn, chaff, dive |
| IR missile (no radar needed) | **no RWR warning**; only MAW (missile approach warner) in some jets | flares when missile within ~ 2-3 s TTI if they have a visual |

Player's own RWR: the same table mirrored: enemy radar lock = tone + red "spike" bracket
on a bearing indicator; enemy launch = "MISSILE" warning + flashing bearing. Very cheap
(bearing = azimuth of enemy relative to own nose). That is also good for the minimap (draw
a red "launch" ring).

Recommended defaults to ship **(game)**: scan +-60 az, +-30 el; 1 s lock; 45-60 nm range scale
(player scale buttons 20/40/80); 2 s ID; notching on; RWR on.

---------------------------------------------------------------------------------------------

## 3. Missile kinematics

### 3.1 Public numbers (open-source: Wikipedia, DCS, Hoggit, FAS/Janes-type publicly quoted; classified data are not available; these are *quoted approximations*)

| Parameter | AIM-120B | AIM-120C-5/-7 | AIM-9M | AIM-9X |
|---|---|---|---|---|
| Length / weight | 3.66 m / 152 kg | 3.66 m / 161 kg | 2.9 m / 85.5 kg | 3.0 m / 85 kg |
| Motor | Single-stage solid, reduced smoke | Larger motor (C-5/-7 longer burn) | Single stage | Single stage + thrust vector control |
| Burn time (est.) | ~ 6-8 s **(unsure)** | ~ 8-10 s **(unsure)** | ~ 4-5 s | ~ 4-5 s |
| Max speed | ~ Mach 4 (peak, high alt launch) | ~ Mach 4 | ~ Mach 2.5 | ~ Mach 2.5 (unsure) |
| Max G | ~ 35-40 G at low alt (limited by aerodynamics at altitude; DCS Hoggit: ~ 30-40) | same | ~ 30 G (M); 60+ G (X, vector control) **(unsure)** | ~ 60 G **(unsure)** |
| Max range (launch from high, high-closure target) | ~ 40-50 nm (B) | ~ 60 nm quoted; lofted high | ~ 10 nm (4.3 nm tail chase low alt) | ~ 10-20 nm; 15+ nm (unsure) |
| Typical practical (no-escape) range | head-on ~ 25-30 nm | ~ 30 nm | ~ 3-5 nm | ~ 5-8 nm |
| Min range | ~ 1 nm (arming distance ~ 0.3 nm) | ~ 1 nm | ~ 0.3-0.5 nm (arming time ~ 1.5 s) | ~ 0.3 nm |
| Seeker | Inertial + datalink midcourse, **active radar** terminal (Pitbull) | same | IR (uncooled/ cooled InSb, rear-aspect heritage), all aspect in M | Imaging IR focal plane array, high off-boresight |
| Seeker FOV (instant) | active radar seeker ~ 4-5 deg half? (real unknown **(unsure)**); "gimbal" ~ +-50-60 deg | | ~ 2.5-5 deg half-angle instantaneous; gimbal ~ 25-40 deg from boresight with uncaged tracking | ~ 90 deg off boresight in HMD cued mode (X) ; instantaneous FOV ~ 4+ deg |
| Warhead | 23 kg WDU-33 | 20 kg WDU-41 | 9.4 kg | 9.4 kg |
| Lethal radius | ~ 8-10 m (proximity fuze) | | ~ 5-8 m | |
| Pitbull range ("goes active") | **~ 10-12 nm** from target, set by the missile's own search; if the shot < 10-12 nm it goes active immediately after launch | | n/a | n/a |

Public time-of-flight anchors (DCS-community measurements, order-of-magnitude):
- AIM-120C head-on at 25 nm, target 400 kt, own 500 kt, 25k ft: ~ 40-50 s TOF.
- AIM-120C pursuit shot (tail), range 12 nm: ~ 35-40 s.
- AIM-9M: head-on 3 nm: ~ 8 s; tail 1 nm: ~ 4 s.
- AIM-120 lofted: climbs ~ 30-45 deg, peaks 40-60k ft, saves drag in thin air, hits steeper.

These come from Hoggit "AIM-120" and "Missile TOF" community threads **(unsure; check yourself)**.
URL: https://wiki.hoggitworld.com/view/AIM-120C_AMRAAM

### 3.2 Game-ready numbers for our two missiles (recommended, simplified)

Pick values that *feel right* with a 120 Hz refresh and small island (~ 60-100 km play area? the
user didn't say; the table scales). All in SI inside code; nm/kt for UI.

| Parameter | "AMRAAM-like" (AIM-120) | "Sidewinder-like" (AIM-9) |
|---|---|---|
| Thrust phase | 8 s motor, accel ~ 22 g avg at launch (thrust 25 kN-ish/ mass 150 kg) ; total impulse choose to reach ~ Mach 3.5 | 4 s motor, ~ 20 g |
| Peak speed | ~ 1200 m/s (M 3.5 at altitude) | ~ 800 m/s (M 2.4) |
| Max lateral G | 35 g (drops with altitude: g_max = 35 * rho/rho0 clamp 8..) | 30 g (arcade: 35 g) |
| Seeker | Mid-course "inertial + uplink" to target position; **active** (own radar) at 10 nm | IR: lock before launch, gimbal 25 deg, FOV 3 deg, tracks hottest = engines |
| Max flight time | 60-80 s (self-destruct when speed < ~ 350 m/s (M 1) after motor out ) | 20 s |
| Arming time | 0.5 s | 0.7 s |
| Prox fuze radius | 9 m (game: 12 m, forgiving) | 7 m (game: 10 m) |
| Loadout | 4-6 AIM-120 + 2-4 AIM-9 (user's "arcade load" choice; see Section 6) | |

### 3.3 Missile dynamics: point mass with thrust, drag, G limit (cheap and good enough)

State per missile: pos p (vec3), vel v (vec3), t (float). Orientation is implicitly velocity
direction (no attitude simulation). Per physics step dt (use fixed 1/60 s, or even 1/30 s; ten
missiles at most):

```
speed = |v|; dir = v / speed
// thrust
a_thrust = (t < burn) ? (T / m) : 0                 // m/s^2 along dir
// drag: simple model  a_drag = (rho/rho0) * k0 * speed^2   + induced drag from turning
// k0: choose so that terminal coast decays: AMRAAM  loses ~ 25-30% speed in 10 s after burnout at sea level.
//   Cd*S/(m): ~ (0.3 * 0.03 m2)/(150 kg) -> 6e-5 *0.5*rho*v^2 ... lump as k0 ~ 3.5e-5 m^-1 (speed^2 m^2/s^2 * 3.5e-5 => at 600 m/s: 12.6 m/s^2 = 1.3 g)
rho_ratio = exp(-alt/8500)                           // cheap atmosphere: scale height ~ 8.5 km
a_drag = k0 * rho_ratio * speed * speed + k_ind * (a_lat*a_lat) / (rho_ratio * speed*speed+1)  // induced term optional
// guidance (PN), see 3.4 -> a_cmd (vec3, perpendicular to v)
a_lat = clamp(a_cmd, g_limit(alt,speed))             // g_limit = gmax * rho_ratio * (speed/600)^2, clamp [4g, gmax]
// integrate
v += (dir * (a_thrust - a_drag) + a_lat + g_vec) * dt     // gravity g_vec=(0,-9.81,0), cancelled by lift in reality; apply only 0.3 factor or full if you want arcs
v = renormalize speed to avoid numerical drift of turning (rotate dir by lateral accel but keep speed from thrust-drag)
p += v * dt
```
Good-enough approach: keep **speed scalar** and **direction vector** separate:
- speed_dot = a_thrust - a_drag - g*sin(climb)
- direction rotated by lateral accel a_lat/speed rad/s, no need for a rotation matrix: dir += (a_lat/speed)*dt * perp; renormalise.
Cost: ~ 60 flops per missile step. Trivial for 10 missiles.

Calibration targets (check that your model reproduces these numbers within 20-30%; tune k0):

| Test | Expected |
|---|---|
| AIM-120 launch from 25k ft, 450 kt, at tail-on stationary-ish target at 15 nm | hit within ~ 40-50 s; speed at impact ~ M 1.5-2 |
| AIM-120 at max 8k ft launch, head-on 20 nm | hit ~ 35-45 s (speed up to M 3 midcourse) |
| AIM-9 at sea level, 2 nm tail chase target 400 kt | hit ~ 6-8 s |
| AIM-9 max range tail-on from 20k ft | ~ 8 nm |
| Coasting AIM-120 after motor-out, speed loss | ~ 80-100 m/s each 5-6 s near sea level, much less at 40k ft |

Energy bleed in turns: induced drag dominates when pulling > 15 g: speed loss of 40-60% in a
30 g, 5 s turn. This is the single most important effect for an *honest escape model*: a missile
that must follow a hard-turning target runs out of energy. Make sure the model includes induced
drag (a_lat^2 term), else DLZ will be absurdly optimistic.

### 3.4 Guidance: proportional navigation (PN)

Textbook PN (Zarchan; Wikipedia "Proportional navigation"):

```
a_cmd = N * Vc * lambda_dot          (scalar, 2-D)
  N        navigation constant, 3 to 5 (real: 3-4 ; game: 4)
  Vc       closing velocity = -dR/dt  (m/s)
  lambda_dot  line-of-sight (LOS) rate (rad/s)
```
3-D vector form (cheap, no trig):

```
r = p_target - p_missile            // relative position (LOS vector)
vr = v_target - v_missile           // relative velocity
omega = (r x vr) / (r . r)          // LOS angular rate vector (rad/s)
Vc = -(r . vr) / |r|                // closing speed
// "True PN": acceleration perpendicular to the LOS   a = N * Vc * (omega x r_hat)
// "Pure PN" : perpendicular to missile velocity:     a = N * (omega x v_missile)       <-- simplest, robust. use this.
a_cmd = N * (omega x v_m)           // v_m = missile velocity vector
```
Pure PN (`a = N * omega x v_m`) needs only: one cross product, one dot, one divide, one scale.
No sqrt except for clamping. Excellent for a 233 MHz machine.

Variants:
- **Augmented PN (APN)**: add `0.5 * N * a_target_perp` — helps vs. manoeuvring targets; needs
  target acceleration estimate (from smoothed velocity differences). Optional (+10% hit rate).
- **Lead/ "collision course" initialisation**: at launch, midcourse "lead pursuit" : aim at the
  predicted intercept point  `t_go = R / Vc`, `p_aim = p_t + v_t * t_go` (clamped). PN converges
  anyway; this avoids a big initial turn that wastes energy.
- **Loft** (AIM-120, long range): if range > ~ 60% of Rmax and target altitude near own:
  command a climb: pitch the midcourse aim point up by `loft_angle` (e.g. 25-40 deg ) until
  the missile is x% of the way to the target or speed starts to bleed; then dive to PN. The
  climb gains time at lower air density (see drag law: rho_ratio) and extends range by ~ 30-50%
  (public claims ~ 2x for high-alt targets). Game: implement as an altitude command
  `h_cmd = h0 + loft_gain * R` (R in m, loft_gain ~ 0.15 up to cap 12 km above), then switch to PN
  when `R < 25 km` or `R < 1.2 * Rpursuit`. Pure PN doesn't need to change; only the pre-PN aim
  point.
- **Terminal limits**: g clamp, seeker gimbal limit (drop-lock if the angle between missile
  velocity (boresight) and LOS exceeds the gimbal; for AIM-9 seeker 25-30 deg, AIM-120 ~ 55 deg
  gimbal; consider "(game) 60 deg / 35 deg").

Midcourse for AIM-120 (real): inertial + datalink update from the launch aircraft's radar
(targets position/velocity); when range-to-target < Ract (pitbull ~ 10-12 nm), the missile
radar goes active and guides on its own. If the target is lost at pitbull (notch, chaff) the
missile can go for "home-on jam" or fly the last-known position (game: fly the last extrapolated
position, then self-destruct).
Game: keep `uplink` flag: while the shooter's radar maintains lock (L&S designated, within
gimbal), missile gets true target state each frame (+ noise). If lock is lost pre-pitbull,
the missile guides to last known position + extrapolation 3-5 s then goes "memory/ blind". After
pitbull it's independent. This is a good *tension mechanic*: "support the missile until pitbull"
("crank" turn: shooter turns 40-50 deg off to extend the target's no-escape zone while keeping
the lock within gimbal; flavour: stays within +-60 deg).

### 3.5 Countermeasures (simplified)

| CM | Works against | Real mechanism | Game model |
|---|---|---|---|
| **Flares** | IR seeker (AIM-9, IR missile) | Hot decoy; seeker may track the brighter flare if within FOV and the target is off-axis | Per missile per second probability when flare within the seeker FOV: p_decoy = base * f(sun, aspect, engine afterburn). Simple: each flare dropped while missile within 2 km and within the seeker FOV gives a roll: p = 0.35 per flare-burst (modulated: afterburner on -> 0.6, tail aspect -> 0.5, 9X -> 0.2). On success the missile retargets to the flare (guides to the flare point, falls). Spacing: 1 burst/s, limited stock. |
| **Chaff** | Radar seekers, esp. AIM-120 in terminal | Cloud of dipole strips, creates many returns; works best when **combined with notching** (the target at 90 deg: doppler cancels the target, the chaff cloud shows ~ zero doppler) | Chaff creates "cloud" objects with RCS ~ target for 5 s. Missile in **active** phase picks the cloud if: target aspect is beam (notch) *and* chaff dispensed within 2-3 s before the arrival *and* missile range < 3 km; otherwise ignore chaff. p(break) ~ 0.5 if both, else 0.1. |
| **Notch / beam** (manoeuvre) | Pulse-Doppler radars (own radar and missile) | Target flies perpendicular to the radar -> closure rate ~ 0 -> filtered with clutter | If |closure| < 40 kt (see 2.3): track lost after delay 1-3 s. Gameplay: AI "beam defence". AMRAAM mitigation: missile is *closing* anyway so the notch breaks only the shooter's radar lock -> missile loses uplink, still has pitbull. Good, forgiving rule: notching only defeats the *shooter's lock / uplink*, not a missile already active. |
| **ECM / jamming** | optional | skip | |
| **Break turn / dive** | Both | Bleeds missile energy, forces g limit | Naturally handled by missile model (G limit and induced drag) |
| **Terrain** | Both | Low altitude: radar look-down + terrain masking | If LOS blocked by the island -> radar contact lost; missile loses uplink |

Missile seeker pre-lock rules (AIM-9): lock on the contact inside the seeker cone (3 deg) at
launch, no mid-course. Hit % with flares is the *simplified arcade* mechanism. Real AIM-9X
imaging IR rejects flares with high probability (~ 90% claimed, unsure): represent via per-
missile "flare resistance" = 0.8 (9M ~ 0.3).

---------------------------------------------------------------------------------------------

## 4. DLZ / LAR (dynamic launch zone / launch acceptability region)

### 4.1 Definitions

| Symbol | Name (common) | Meaning |
|---|---|---|
| **Raero** (Rmax1 in Falcon? used differently by sources) | Max aerodynamic range | Longest range at which a launched missile can reach a **non-manoeuvring** target (target flies straight, constant speed/heading). Missile energy at arrival ~ the minimum to guide. Often shown as the top of the bar. |
| **Rmax** (also called Rpi? "Rmax2" in some, "Rpi" = range of **probable intercept**) | Max range against a **manoeuvring** target | Range where the target can run/turn as he likes but "we still get a *probability* of intercept": Often with a standard escape: the target turns away (180 deg) and runs at some speed. Rmax < Raero. **(definitions differ per document; in Falcon: Rmax1 = Raero vs non-manoeuvring, Rmax2 = vs a target that turns 180 and runs; terms vary: unsure.)** |
| **Rne** (or Rtr) | No-escape range | Range inside which the target **cannot escape** even if it breaks and runs at max speed (the "kill zone"). Rtr = "range of turn and run" is how DCS/older docs label the tail of the "arrow". Rne < Rmax. |
| **Rmin** | Min range | Closest distance at which the missile can still arm (fuze delay) and manoeuvre to hit: depends on aspect and target turn rate. |
| Rpi | Range of probable intercept | In AMRAAM publications, "Rpi" is "target does nothing, perfect shot" -> the same as Raero in practice. **(unsure)** |

DCS Hornet display (HUD): DLZ is a vertical bar on the right side of the HUD (or Hornet "ASE"):
- scale: min range to max range (scaling shown by the radar range scale / "Rmax2")
- carets: **Rmax1** (aero, top), **Rmax2** (manoeuvre), **Rne** (no-escape, "Rtr"), **Rmin** at the bottom
- a closure ("range to target") tick moves along the bar
- Falcon BMS: "ASE circle" (Allowable Steering Error) on the HUD with the "steering dot"; "DLZ: Rmax1, Rmax2, Rne, Rmin ticks; when you shoot below Rne: 'in no-escape zone'".
(Wikipedia/Hoggit: https://wiki.hoggitworld.com/view/DCS_F/A-18C_AIM-120 ; Chuck's Guide DCS Hornet "Air-to-Air missiles").

The user's wish: "if the target can turn X degrees and still escape, or is in the no-escape zone".
This is the **escape-angle / minimum-turn-away** concept: for the current geometry, find the
minimal heading change X of the target (turn + run) at which the missile *no longer* hits.
- Large X required (e.g. > 120 deg): the target is "in the zone / deep NEZ".
- Small X, e.g. 20 deg: the target can escape with a small jink (you are at the edge).
- X not achievable (no heading change saves it): **No-escape zone** (draw red, "NE").
Good UI: a DLZ bar plus a numeric "ESC 70" (the "turn X degrees and live") readout. Or colour
the target diamond: green = NE (no escape), yellow = escapable with big turn, red = out of range. 

### 4.2 How a real FCS does it (public-level)

Fire control computers on the F-16/F-18 do **not** fly a missile model each frame; they use
precomputed tables (curves/ polynomial fits from 6-DoF sim runs):

- Inputs: own Mach/altitude (or TAS & altitude), target altitude (or delta), target
  **aspect** (0 = nose-on, 180 = tail-on), closure (from speeds), maybe own and target g,
  type of missile. 
- Table lookup + multi-linear interpolation gives Rmax_head_on (aspect 0), Rmax_tail (aspect 180)
  and the shape in between; DLZ range = interpolation. Then scale with ratio of own/target speed.
- Rne: separate table (target manoeuvre assumption: 180 deg turn with 6-9 g, then run at the
  target's present speed (or max speed)).
- Rmin: simple function of speed and manoeuvre; AIM-9: table of seeker/ arm time.

Typical "factors" known publicly (rough): launch alt up 10k ft -> +~ 30% range; each +100 kt own
speed -> +~ 10-15% range; head-on to tail-chase ratio: Rmax_head/Rmax_tail ~ 3-4 (AIM-120).
**(unsure)**.

### 4.3 Options to compute it in the game

(a) **Precomputed 4-5-D table**: indexed by launch alt (e.g. 0,5k,10k,20k,30k ft), launch Mach
(0.6,0.9,1.2,1.6), target-aspect (0,45,90,135,180), target speed ratio, target altitude diff.
Number of entries: 5*4*5*3 = 300 floats per DLZ value (Raero, Rmax2, Rne, Rmin) per missile.
Build by running the game's own missile model offline (a tool run at start-up, or at
development time with the same C++ code, dumped into a header).
- Pros: zero per-frame cost; deterministic; same as real. 
- Cons: 4-D table construction is work; **drifts out of sync** when the missile model changes
  (regenerate); can't capture "this specific enemy turning at 6 g" unless dimensions added.

(b) **Fast simulated fly-out (recommended)**: run a few lightweight fly-outs of our own missile
model against a *predicted* target trajectory, spread across frames. The same code as live missile,
simplified (no atmosphere table lookups: use exp approx), larger dt (e.g. 0.1-0.2 s, 100-400 steps).
- Each fly-out: ~ 300 steps * ~ 100 flops = 30k flops: ~ 0.1 ms on a 233 MHz Pentium II. A
  bisection for Rmax needs ~ 8 fly-outs; ~ 1 ms per DLZ-solve. So we can compute 4 DLZ ticks (Raero, Rmax, Rne, Rmin) for the designated target at 2-4 Hz: ~ 5 ms per solve; split over 8 frames: negligible.
- Pros: honest (**the DLZ comes from the same model the missile uses**), automatically reflects
  altitude, speeds, loft, G limit, drag, enemy manoeuvre assumption; easy to add the
  "escape angle" computation (which a table can't do).
- Cons: some CPU; needs careful determinism (use a dedicated "sim lite" struct); noisy output
  near bounds -> smooth with a low-pass filter and hysteresis.

(c) **Analytic approximations**: closed-form-ish: Raero ~ (energy-based) e.g.
`R_aero = v_avg * t_burn + ...`: Combined: `Rmax_tail ~ R0 * (1 + k_alt * alt) * (v_m - v_t)/v_m`,
`Rmax_head ~ R0 * (1 + ..)* (1 + v_t/v_m)`; no-escape: `Rne ~ Rmax * 0.4`.
- Pros: trivially cheap. Cons: not honest vs. missile model, tuned by hand for each condition;
  hard to get the "turn X degrees" calc.

**Recommendation: (b) fly-out with a coarse model, plus (a)-style cache only if profiling
demands.** Use (c) only to seed the bisection (initial bracket) so there are ~ 6 iterations.

### 4.4 Fly-out design

Definitions:
- `hit(R0, manoeuvre)`: simulate missile (launched now from our state, aimed per the
  real guidance logic incl. loft) versus target following `manoeuvre` starting at range `R0` along
  the *current LOS direction*? (Trick: place the target at the **current bearing and elevation**
  from us with range R0; keeps aspect and geometry like the real situation. Target velocity =
  current target velocity; own state = current.)
- Returns hit/miss + time + final miss distance (to smooth).

Target manoeuvre models ("escape" behaviours) the fly-out supports:
1. **STRAIGHT**: target keeps velocity; (-> Raero).
2. **TURN-AND-RUN(X)**: the target turns away by X degrees (shortest way to put the missile further
   behind, i.e. rotates its velocity away from the missile direction) at the target's max
   sustained turn rate (e.g. 6-9 g -> 20-25 deg/s at 400 kt; for the game enemy: 18 deg/s), then
   keeps going straight at its **max speed** (accelerates by a cap).
   X = 180 gives the real-world "Rne" test: "turn 180 and run".
3. **BEAM / notch (optional)**: turn to 90 deg off, defeat uplink (only relevant if the missile
   isn't active yet): treat as "target lost for the missile if pitbull not yet reached".

Pseudo-code:

```
struct SimState { vec3 p, v; float t; ... };      // missile or target

// returns true if the missile gets within fuze radius before running out of energy/time
bool flyout(const Missile& m0, const Target& t0, float escapeX_deg, float dtSim /*0.1*/)
{
    Missile m = m0;  Target t = t0;
    float turnLeft = escapeX_deg * DEG2RAD;           // remaining heading change
    float tt = 0;
    while (tt < m.maxTime)                            // e.g. 60-80 s
    {
        // target model
        if (turnLeft > 0) {
            float rate = min(t.maxTurnRate, turnLeft/dtSim);
            rotate t.v around vertical axis (away from missile LOS side) by rate*dtSim;
            turnLeft -= rate*dtSim;
        }
        // after the turn: accelerate toward max speed
        t.speed = min(t.maxSpeed, t.speed + t.accel*dtSim);
        t.p += t.v*dtSim;
        // missile step: PN guidance + thrust/drag, same function as the real missile code
        stepMissile(m, t, dtSim);                     // uses uplink/pitbull logic, G-limit, gimbal
        float d = |t.p - m.p|;
        if (d < m.fuzeRadius) return true;
        if (m.speed < m.minSpeed && tt > m.burn) return false;   // dead missile
        if (missed (closing rate <0 && d increasing && tt>1)) return false;
        tt += dtSim;
    }
    return false;
}
```
Compute DLZ values:

```
// 1. Raero: bisection on R0 in [Rlo, Rhi] with escapeX = 0
// 2. Rmax2: the same with escape = X_std (e.g. 90-120 deg turn at the target's max turn rate)
// 3. Rne: the same with escapeX = 180 (and "run at max speed")   -> largest R where no choice of
//         X in {45,90,135,180} escapes: Rne = min over X of Rhit(X)
//    Because "no escape" = "hit for ALL X" -> Rne = min_X Rmax(X).
// 4. Rmin: smallest R where flyout hits (bisection in [0.2nm, 3 nm]) against a hard-manoeuvring target
//          (X=90 at max turn rate) -- or analytic: arming distance + turn radius.
```
The "turn X degrees and still escape" number the user wants:

```
// At current range R (live):
// escapeAngle = smallest X in {10,20,...,180} for which flyout(R, X) == MISS
//   (target survives by turning X degrees)
// If none -> "NO ESCAPE" (target in zone)
// If flyout(R, 0) == MISS -> "OUT OF RANGE" (can't reach even if target flies straight)
// Binary search over X with a monotonic assumption (bigger X -> more chance to escape): ~ 5 fly-outs.
```
Warning: monotonicity is not guaranteed (turning *into* the missile can kill the closure and
cause a miss, too), but for "turning away" it's roughly monotonic; the AI model just uses
"turn away" direction (rotate v away from the LOS to the shooter) and picks the side
(left/right) with the better geometry.

Cost bookkeeping: for the designated target: at 3 Hz: 4 bisection solves (Raero, Rmax2, Rne ->
min over 4 X values, Rmin) * 7 fly-outs * 300 steps * ~ 100 flops = 8.4 M flops => ~ 40-80 ms on
a P-II 233 (assume ~ 100-200 MFLOPS peak effectively 50): too heavy at once for one frame: **spread
it**: one fly-out per frame (~ 0.2 ms), so a complete DLZ refresh takes ~ 30 frames (0.5 s at 60 fps).
The dtSim can be 0.25 s for the lofty far fly-out (cheaper) and 0.05 s for the last 5 s (precision),
or use the DLZ only for the **three numbers** and do Rne/Rmax2 at lower rate (every 1 s).

Caching: the DLZ depends on own speed/alt, target speed/aspect: all change slowly (except when
the player manoeuvres). Smooth with `value += (new - value) * 0.3` and keep last good
values; mark "stale" if inputs changed > 20% (not strictly needed).

### 4.5 The HUD/UI output (what the player sees)

- DLZ bar (vertical, next to the target box in the HUD, or at right): min..max scale = 0..Raero*1.1:
  - top tick = Raero (white), "M" tick = Rmax2 (yellow), "NE" tick = Rne (red/ solid),
    bottom tick = Rmin; moving caret = current range (with closure rate sign: ^ closing, v opening).
  - Background fill: green between Rmax2 and Rne? Prefer a simple scheme: grey above Raero, white
    between Rmax2 and Raero ("can escape by turning away / out of the sure range"), **green** below
    Rne ("no escape zone"), red below Rmin.
- Numeric: "ESC 60" (the smallest turn that escapes) near the caret; "NE" when no escape.
- Launch cue: "SHOOT" flashing when range is between Rne and Rmax2 and the aim is within the
  ASE circle (Allowable Steering Error: the missile can reach the target only if our nose is within
  Y degrees of the lead cue; ASE circle of 5 deg radius, steering dot inside it = ok);
  Arcade version: "SHOOT" when lock + in Rmax2.
- IR missile (AIM-9): same DLZ concept simplified: "tone" = locked; "in range" = Rmax; no
  datalink. DLZ bar optional; could just show the range scale ticks and the seeker circle.

Required game logic data per target for the DLZ: relative position/velocity, target
max turn rate (per enemy type), max speed, own state, missile type; nothing else.

### 4.6 Speed-up idea: normalised fly-out table (hybrid)

Many frames only require **interpolated** values: precompute, at level load, a coarse table with
`flyout` by the same sim for the *current* own altitude band & speed band: R_max(aspect) for 7
aspects * 3 target speeds: 21 values * (7 bisection fly-outs) = 147 flyouts ~ 30 ms once (do it in the
loading screen or spread over a second). Then live DLZ = table lookup + small correction, and
the real flyout is only used for the designated target's "escape angle" display. This is the
best of (a) and (b). Keep it as an optimisation.

---------------------------------------------------------------------------------------------

## 5. Escape-angle / "can he turn and escape" - cheap closed-form (for AI and for display)

Geometry: target at range R, closing speed relative Vc, missile speed V_m (current, decaying),
target speed V_t, target turns away at ω_t (rad/s). Simple *energy-free* estimate of the
missile's chase:

- Time for missile to reach the target position if the target runs straight away: `t_run =