# Flight tuning: what each setting does

This page is for tuning the flying by hand. Every number here is a line in `jet.ini`, under `[pilot]` (how the
keys move the controls) or `[flight]` (what the jet does with them).

**How to tune**

1. Open `jet.ini`, either on the Mac (`Shared/dev/jet/jet.ini`) or in the VM (`M:\dev\jet\jet.ini`, for example in
   Pica). If `[flight]` and `[pilot]` aren't in the file yet, add the section header and only the lines you want to
   change. A missing line just means "use the default".
2. Change a number, save, and press **F5** in the game. The change takes effect at once, mid-flight. The screen
   shows "INI RELOADED (n)", and `jet.log` gets one line per changed key.
3. **F10** shows the feel overlay: the key axes, G, the G limit, speed vs target speed, and roll and turn rates.
4. On the Mac, `tools/simh/simh feel` prints the feel card (turn rates, roll times, speed-up times) for whatever is
   in `jet.ini` right now. So you can see what a change does before flying it. A line marked `DIFF` is more than
   10 % away from the starting value. After tuning, that's expected and fine.

Change one or two things at a time. Values outside the allowed range are clamped and logged, so they never crash
the game.

## How the controls work (the idea)

- **The arrow keys don't jump to full.** Holding a key moves a "virtual stick" smoothly towards full; letting go
  brings it back. A short tap gives a small nudge, a long hold gives a full pull.
- **Up/Down sets the pull (G), not the nose angle.** With no key held, the jet keeps flying where it points, like
  a fly-by-wire jet (auto-trim). Holding Up pulls up to the G limit for the current speed.
- **The G limit depends on speed.** Slow means tight turns and fast means wide turns. Above about Mach 1.1 the limit
  goes down (9 g, then 7 g at Mach 1.6). That's why turning in afterburner is slow.
- **Left/Right sets the roll rate.** With no key held the jet keeps its bank. Below 4 degrees of bank it levels the
  wings by itself (the "detent").
- **A bank turns the jet a little by itself**, and the nose slowly drops. For a level turn, bank and hold Up.
- **Throttle:** nothing held = cruise (Mach 0.8). **A** = afterburner, towards Mach 1.6. **Z** = speed brake, down to
  350 kt quickly, then slowly down to the 150 kt floor. The jet can never fly slower than 150 kt (no stalls). At
  the floor, a climbing nose sinks towards the horizon by itself.

## Two control modes: Expert and Novice

`[pilot] mode = expert` (default) or `novice`. In the game, the dev key **F6** switches between them mid-flight
(it works now). F5 puts back whatever `jet.ini` says. Both modes fly the very same jet with the same speeds,
G limits and turn rates. Novice is a helper that works the stick for you.

- **Expert** (everything above): Left/Right rolls, Up/Down pulls. You can loop, fly inverted, and do anything.
- **Novice** (Ace Combat's "Normal"/novice style): **Left/Right = turn.** The jet banks to 70 degrees by itself and
  pulls exactly enough to keep the turn level. Let go, and it rolls back to wings level (in under a second).
  **Up/Down = climb/dive.** The nose moves up or down relative to the horizon. Let go, and it keeps that climb or
  dive angle. You can't climb or dive steeper than 75 degrees, so there are no loops and no flying upside down.
  **Up + Left together** = a much tighter, climbing turn (it pulls up to the G limit). The Up arrow is nose up here
  too, and `invertPitch` works the same way.

At cruise a plain Novice turn (70 degrees of bank, about 2.9 g) takes about **60 s** for a full circle (6 deg/s).
Adding Up makes it about **21 deg/s** while climbing. Expert's hardest level turn is 19 deg/s at 9 g.

| Key (`[pilot]`) | Default | Plain meaning |
|---|---|---|
| `mode` | expert | `expert` or `novice` |
| `noviceBankDeg` | 80 | Bank for a held Left/Right. More = tighter level turn (80 = about 2x the turn rate, 5.8 g); less = gentler |
| `noviceBankGain` | 5 | How firmly the bank is steered to its target. Higher = snappier, lower = softer |
| `noviceRollRateDeg` | 90 | Fastest automatic roll (deg/s), into a turn and back to level |
| `noviceGammaRateDeg` | 15 | How fast Up/Down changes the climb angle (deg/s). Lower = gentler, finer aiming |
| `novicePitchLimitDeg` | 75 | Steepest climb or dive |
| `noviceGammaGain` | 2.0 | How firmly the climb angle is held |
| `noviceAutoLevelPitch` | 0 | 1 = after you let go of Up/Down the jet eases back to level flight (instead of keeping the climb) |
| `noviceLevelGain` | 0.5 | With auto-level on: how fast it levels (1/s) |

Novice symptoms: turns too lazy -> raise `noviceBankDeg` (75-80) or hold Up while turning; rolls into turns too
abruptly -> lower `noviceRollRateDeg` (60) or `noviceBankGain` (3); climbs/dives too twitchy -> lower
`noviceGammaRateDeg` (10).

## Symptom -> what to change

| You feel | Change |
|---|---|
| Turns too slow at cruise (500 kt) | raise `gLimitTable` points at 330 and 700 (e.g. `330:10,700:10`), or `turnBoost` = 1.1 (everywhere) |
| Turns too slow in afterburner | raise the `1020:` and `2000:` points of `gLimitTable` (user wish: ~7 g at M1.6) |
| Turns too tight / too easy everywhere | `turnBoost` = 0.9 |
| Slow-speed turns too tight | lower the `150:` and `200:` points of `gLimitTable` |
| Small taps of Up/Down move the nose too much | lower `pitchRateFine` (e.g. 2.0), or lower `pitchFine` |
| Small taps do almost nothing | raise `pitchRateFine` (e.g. 4.0) |
| Holding Up takes too long to reach full pull | raise `pitchRate` (2.5 = full pull after ~0.38 s) |
| Full pull comes on too suddenly | lower `pitchRate` (1.2 = full after ~0.66 s), or raise `tauG` (0.2) |
| Nose keeps moving a bit after letting go of Up | raise `pitchReturn` (10) |
| Rolls too fast / hard to stop at the bank I want | lower `rollRateTable` (e.g. `150:90,250:150,400:180,700:170`) or lower `rollRateIn` (4) |
| Small bank corrections too coarse | set `rollFine` = 0.3 (a fine first stage for roll, like pitch) |
| Rolls too slow | raise `rollRateTable` points |
| Roll starts/stops feel mushy | lower `tauRoll` (0.03) or raise `rollReturn` |
| Wings snap level when I want a small bank | lower `detentDeg` (2) or set it to 0 (off) |
| Wings don't level by themselves enough | raise `detentDeg` (6) or `detentGain` (3) |
| Banking without pulling turns too much / nose drops too fast | lower `latGrav` (0.5) |
| Takes too long to slow down after letting go of A | raise `decOver` (e.g. 25) |
| Speed brake (Z) too strong / too weak | `decBrake` (m/s^2, 28 = about 13 s from Mach 1.6 to 350 kt) |
| Z below 350 kt too slow / too fast | `decSlow` (2.5 = about 40 s from 350 to 150 kt) |
| Afterburner speeds up too slowly | raise `accAB` (25) |
| Jet runs out of climb too low / too high | `ceilingM` (e.g. 14000 or 17000); `thrustFullM` moves where the fade starts |
| Zoom climbs go too high / stop too abruptly | raise / lower `zoomDrag` (e.g. 5 or 1) |
| Climbing costs too much speed | lower `energyTrade` (0.7) or raise `accMil` |
| Hard turns bleed too much speed | lower `inducedPerG` (0.5) |
| Speed bleeds too little in hard turns | raise `inducedPerG` (1.5) |
| Up arrow should be nose DOWN (flight-sim style) | `[pilot] invertPitch=1` |
| S long press (designate) too quick / too slow | `[pilot] longPressMs` (300) |

## `[pilot]`: how the keys move the controls

The "axis" is the virtual stick position from -1 to +1. Rates are in "axis per second": 3.0 means the axis moves
from 0 to full in 1/3 s.

| Key | Default | Plain meaning |
|---|---|---|
| `pitchFine` | 0.35 | Pitch first stage. The axis moves slowly (at `pitchRateFine`) up to this point, then faster (at `pitchRate`). Taps stay in the fine stage. 0 turns the two stages off |
| `pitchRateFine` | 3.0 | Speed of the fine stage. 3.0: 0 to 0.35 in 0.12 s. A 0.1 s tap of Up at 500 kt moves the nose about 0.4 degrees |
| `pitchRate` | 1.8 | Speed after the fine stage. 1.8: full pull about 0.5 s after pressing |
| `pitchReturn` | 6.0 | How fast the pull goes away when you let go (full to 0 in 0.17 s) |
| `pitchCross` | 8.0 | How fast the axis comes back when you press the opposite key |
| `rollRateIn` | 6.0 | How fast the roll command builds (full in 0.17 s) |
| `rollReturn` | 10.0 | How fast it goes away when you let go. High means the bank stops right where you leave it |
| `rollCross` | 12.0 | Back to 0 when you press the opposite roll key |
| `rollFine`, `rollRateFine` | 0, 2.0 | Optional fine first stage for roll, like pitch. Off by default; try `rollFine=0.3` |
| `invertPitch` | 0 | 0: Up arrow = nose up (default). 1: Up arrow = nose down |
| `longPressMs` | 300 | S held at least this long = designate (long press); shorter = next target |
| `flareRepeatMs` | 500 | F held: one flare burst on the press, then another every 0.5 s |
| `joyExpo`, `joyDead` | 0.3, 0.05 | Joystick only: softer response around the centre, and the dead zone |

## `[flight]`: what the jet does

**Turning**

| Key | Default | Plain meaning |
|---|---|---|
| `gLimitTable` | `150:4.5,200:6,250:7.5,300:8.5,330:9,700:9,1020:7,2000:6` | The G you get at full pull, by speed (`knots:g` pairs; values in between are blended). This is the main "how hard it turns" setting |
| `turnBoost` | 1.0 | Multiplies the whole table: 1.2 = every turn 20 % tighter |
| `negGLimit` | -3.0 | How hard Down can push (negative G) |
| `pitchRateCapDeg` | 40 | Safety cap on how fast the nose can come round (deg/s). The G table normally limits first |
| `tauG` | 0.10 | How quickly the G builds after the key (seconds). Higher = softer |
| `latGrav` | 1.0 | How much a bank alone turns the jet (and drops the nose). 0 = banking alone does nothing |

**Rolling**

| Key | Default | Plain meaning |
|---|---|---|
| `rollRateTable` | `150:110,250:180,400:220,700:200` | Roll speed at full roll key (deg/s) by speed (kt). At 500 kt: 90 degrees of bank in ~0.57 s, a full roll in ~1.8 s |
| `tauRoll` | 0.06 | Roll start/stop softness (seconds) |
| `detentDeg`, `detentGain` | 4, 1.5 | Wings-level help: below 4 degrees of bank with no roll key, the wings level themselves. 0 = off |

**Speed**

| Key | Default | Plain meaning |
|---|---|---|
| `machNormal` | 0.80 | Cruise speed with no throttle key |
| `machFast` | 1.60 | Afterburner (A held) speed |
| `vManKt` | 350 | Z: the speed brake works down to this speed... |
| `vFloorKt` | 150 | ...then the jet slowly slows to this floor. It never flies slower |
| `vManBandKt` | 10 | The speed brake fades out over the last 10 kt above 350, so it doesn't jolt |
| `accMil`, `accAB` | 6, 18 | Acceleration at normal power / in afterburner (m/s^2). 18 = cruise to Mach 1.6 in ~14 s |
| `decOver` | 15 | Slowing down when faster than the target, e.g. after letting go of A or after a dive (m/s^2). orchestrator default 15 (user: letting go of A returns to normal speed; 5 took ~55 s) |
| `decBrake` | 28 | Speed brake strength (m/s^2): Mach 1.6 to 350 kt in ~13 s |
| `decSlow` | 2.5 | Below 350 kt with Z: about 40 s down to the floor |
| `tauV` | 2.5 | How softly the speed settles on its target (seconds) |
| `energyTrade` | 1.0 | Climbs cost speed and dives give speed (1 = like gravity, 0 = never) |
| `inducedPerG` | 0.9 | Speed lost in hard turns: 0.9 m/s^2 for every g above 1 (a 9 g turn at cruise loses ~30 kt in 5 s) |
| `vMaxMach` | 2.0 | Absolute top speed |

**Floor speed**

| Key | Default | Plain meaning |
|---|---|---|
| `floorBandKt` | 20 | Within 20 kt of the floor speed, a climbing nose starts to sink |
| `floorDropDeg` | 8 | How fast it sinks (deg/s), towards the horizon. Pushing down still works. Half of it is also used above the ceiling |

**Altitude: thinner air, less thrust** (T10a, after the M1 play-test)

The engine loses thrust gradually as the air thins, like a real jet. Up to 6 km nothing changes. Above that, speeding
up and climbing get slowly weaker, and at the **service ceiling (15.5 km, about 51 000 ft)** the engine has nothing
left for climbing, so the climb fades out by itself. A fast zoom can coast a little higher (about 17.5 km at most),
then the jet runs out of energy and the nose drops gently. **Gravity always works**: pointing the nose down always
gains speed, at any height, at any throttle (a straight-down dive from 17 km goes from 150 to about 245 kt in 5 s).
Braking (Z) works at any height.

| Key | Default | Plain meaning |
|---|---|---|
| `thrustFullM` | 6000 | Full engine thrust up to this altitude (m). Raise it to keep low-altitude performance higher up |
| `ceilingM` | 15500 | Service ceiling (m): the height where the climb fades to nothing, even in afterburner. Lower = the jet tops out lower |
| `thrustLapseExp` | 0.7 | Shape of the fade between the two (thrust follows air density to this power). Little effect; 0.5-1.5 are all sensible |
| `thinAirDecel` | 0.5 | Above the ceiling the jet slowly loses speed in level flight (m/s^2). Kept small (max 1.5) so dives always speed up |
| `zoomDrag` | 3.0 | Above the ceiling, climbing costs extra energy: higher = zoom climbs stop sooner (0 = coast up to ~20 km). Never acts in level flight or dives |

How strong the engine is at each height (`simh feel` prints this table for your jet.ini; "climb" = the best steady
climb rate, an arcade jet climbs far better than a real one):

| Altitude | Thrust left | Afterburner climb | Normal-power climb | Afterburner top speed (Mach 1.6) |
|---|---|---|---|---|
| 0-6 km (0-19 700 ft) | 100 % | 166 000-180 000 ft/min | 29 000-31 000 ft/min | 1058-983 kt |
| 8 km (26 200 ft) | 75 % | 122 000 | 21 000 | 958 kt |
| 10 km (32 800 ft) | 53 % | 83 000 | 14 000 | 932 kt |
| 12 km (39 400 ft) | 32 % | 49 000 | 8 400 | 920 kt |
| 14 km (45 900 ft) | 12 % | 18 600 | 3 200 | 920 kt |
| 15 km (49 200 ft) | 4 % | 5 900 | 1 000 | 920 kt |
| 15.5 km (50 900 ft) and up | 0 | 0 | 0 | (only by diving) |

Top speeds are Mach numbers, so in knots they are lower up high (the speed of sound is lower in cold air): Mach 1.6
is 1058 kt at sea level but 920 kt above 11 km. The HUD shows true airspeed: 1000 kt really is 1852 km/h, the jet
really covers 514 m every second.

**Looks only** (no effect on how it flies)

| Key | Default | Plain meaning |
|---|---|---|
| `alpha1gDeg`, `alphaRefKt`, `alphaMinDeg`, `alphaMaxDeg`, `tauAlpha` | 2, 300, -6, 20, 0.15 | How far the nose points above the flight path (the HUD velocity vector vs the nose); bigger when slow and pulling |
| `stabMaxDeg`, `stabRollDeg`, `flapMaxDeg`, `surfTau` | 20, 8, 20, 0.08 | How far the tail and wing surfaces move in the chase view |
| `abOnSec`, `abOffSec`, `brakeSec` | 0.3, 0.6, 0.5 | Afterburner flame and speed brake fade times |

## The feel card (defaults)

`simh feel` measures these by actually flying the model:

| Situation | Default |
|---|---|
| Level turn at cruise (510 kt, 9 g) | 19 deg/s, a full circle in ~19 s |
| Level turn at 350 kt (Z), 9 g | 28 deg/s, a circle in ~13 s |
| Level turn at the 150 kt floor, 4.5 g | 32 deg/s |
| Level turn at Mach 1.6, 7 g | 7.4 deg/s, a circle in ~49 s |
| Roll to 90 degrees / full roll, 500 kt | 0.56 s / 1.83 s |
| Up key: full pull after | 0.48 s |
| 0.1 s tap of Up at 500 kt | 0.4 degrees of nose movement |
| Cruise -> Mach 1.6 (A), sea level | 14 s |
| Mach 1.6 -> 350 kt (Z) | 13 s, then 350 -> 155 kt in ~41 s |
| 30 degree climb at cruise power | loses ~3 kt/s at first, then settles ~25 kt below cruise |
| Afterburner climb held at 30 degrees from 3 km | tops out at ~17.5 km (zoom above the 15.5 km ceiling), then descends |
| Afterburner straight-down dive from 17 km at 150 kt | ~207 kt after 3 s, ~244 kt after 5 s |

Recordings: `jet.exe -telemetry file.csv` writes one row per tick (keys, axes, G, speed, angles), so a flight can be
read back as numbers. Every flight also writes `last.rpl` once replays are in (T08).
