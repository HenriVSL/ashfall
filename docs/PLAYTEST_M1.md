# Jet M1 play-test: free flight (flight feel + HUD)

This is the first playable build: one jet, the volcanic island, the airbase, the sky and the sea. There are no
enemies or weapons yet. What we want to know from you is how the **flying feels** and how the **HUD** reads.
Everything about the feel can be changed by numbers in `jet.ini` (see "Tuning it yourself").

## Starting

1. In VM A, run **`M:\dev\jet\play.bat`** (double-click it in Explorer, or Start > Run).
2. "LOADING ISLAND" shows for about 10 seconds (the island file comes over the network share).
3. You are parked on the runway threshold, heading west out to sea. The HUD says **A: TAKEOFF**.
4. Press **A** (a tap is enough; hold it for an afterburner takeoff). The jet rolls, rotates, lifts off and climbs
   out by itself. The gear comes up at 50 ft.
5. At 1500 ft and 300 kt the HUD says **YOU HAVE CONTROL** and the jet is yours. If you want it earlier, hold an
   arrow key for a moment once you are above 200 ft: you take over at once.

After a crash the camera circles the wreck for 2.5 s, then you are back on the runway (always the runway, as you
asked) and the automated takeoff runs again. Esc opens the pause menu: **Resume / Restart on runway / Quit**.

`play.bat` runs `jet.exe -fullscreen -res 800 600 -preset low -dev` (`-dev` turns on F9 and F10).

## Keys

| Key | What it does |
|---|---|
| Up / Down | pitch: Up = nose up (`[pilot] invertPitch = 1` swaps it) |
| Left / Right | roll |
| A (hold) | FAST: afterburner, towards Mach 1.6 (G limit falls at high speed). Also starts the takeoff |
| Z (hold) | SLOW: speed brake, down to manoeuvring speed (never below 150 kt) |
| nothing | NORMAL: back to cruise (~Mach 0.8) |
| C | cockpit view (HUD only) / chase view (reduced HUD). The game starts in chase view (`[camera] startView`) |
| Tab | minimap range 10 / 20 / 40 NM |
| F6 | switch **Expert / Novice** controls in flight (the HUD shows which) |
| F5 | reload `jet.ini` (live tuning, see below) |
| F10 | feel overlay: key axes, G, G limit, speed vs target, roll/turn/pitch rates, a 4-second graph |
| F9 | dev: restart in the air (nearest air start point, 2000 m), skips the takeoff |
| F11 | frame-rate counter |
| F12 | screenshot to `shots\jet_001.bmp`, `jet_002.bmp`, ... |
| Esc or P | pause menu (Up/Down, Enter; Esc again resumes) |

S, Space, D, F and V are already wired as controls but do nothing until the combat milestone.

### Expert and Novice

- **Expert** (the default): Left/Right roll the jet, Up/Down pull. You can loop and fly inverted. With no key held
  the jet keeps its bank and keeps flying where it points.
- **Novice** (Ace Combat's easy style): Left/Right = turn (the jet banks and pulls by itself, and levels the wings
  when you let go). Up/Down = climb/dive relative to the horizon (it holds the angle when you let go, max 75
  degrees, no loops). Up + Left together = a tighter climbing turn.

F6 switches in flight. To start in Novice, set `[pilot] mode=novice` in `jet.ini`.

## What to judge (please note anything that feels off, even vaguely)

Flight feel (top priority):
- **Roll:** does a tap give a small, precise bank change? Is a full roll fast enough, and does it stop where you
  let go?
- **Pitch:** taps for fine aiming vs. holding for a full pull. Too twitchy, or too lazy?
- **Turning:** turn rate at cruise, in afterburner (wide, about 7 g) and when slow (tight). Should the jet turn
  harder anywhere?
- **Throttle:** how quickly A accelerates, how quickly the jet slows back to cruise when you let go, and how Z
  feels.
- **Energy:** climbing costs speed and diving gains it. Is that noticeable enough? Too much?
- **Novice vs. Expert:** which do you prefer, and is Novice's auto-bank too strong or too weak?

Views and HUD:
- **Chase camera:** distance, height, and how much it lags in turns (the jet should lead the camera a little).
- **Cockpit HUD:** readability of the speed and altitude boxes, the pitch ladder, the velocity vector, the heading
  tape and the G/Mach/AOA numbers, over sky, sea and the island.
- **Minimap** (bottom left): size, position, usefulness of the island image and the range.

Everything else:
- The automated takeoff and the climb-out: too long, too short?
- Crash and restart flow, PULL UP warning (flashing + tone near the ground or sea), RETURN TO AREA (beyond ~26 km).
- Sounds: engine, afterburner, wind, speed brake, gear, runway roll.
- Frame rate (F11) and how the island, sea, clouds and the jet look.

The two parked jets on the apron and the one circling the island at about 1100 m are only scenery, there to help
judge the 3D look (they don't fly, collide or shoot).

## Tuning it yourself (F5)

1. Open `jet.ini` (Mac: `Shared/dev/jet/jet.ini`; VM: `M:\dev\jet\jet.ini`, for example in Pica). Every setting is
   listed with its default and a one-line explanation. Flying: `[pilot]` (how the keys move the controls) and
   `[flight]` (what the jet does). Camera: `[camera]`. HUD: `[hud]`. Sound: `[sound]`. Graphics: `[scene]`.
2. Change a number, save, and press **F5** in the game. It applies at once ("INI RELOADED (n)").
3. **F10** shows what the jet is actually doing (G, the G limit, rates).
4. **`docs/FLIGHT_TUNING.md`** explains every flight key in plain words, with a "symptom -> what to change" table.
   On the Mac, `tools/simh/simh feel` prints the turn/roll/speed card for the current `jet.ini`.

`[video]` settings (resolution, preset) need a restart. A deleted line means "use the default"; out-of-range values
are clamped, never fatal.

## Replays, logs, screenshots

- Every flight is recorded to **`last.rpl`** (overwritten at the next start). If a flight showed something
  interesting, copy `last.rpl` to `tests\user\` with a name, and tell us. Watch one with
  `jet.exe -replay last.rpl` (or `-replay last.rpl -window`). F5/F6 changes during a flight are in the replay too.
- **`jet.log`**: the play log (takeoff timeline, crashes, F5 changes).
- **`shots\jet_NNN.bmp`**: F12 screenshots.
- Optional telemetry for tuning: `jet.exe -fullscreen -res 800 600 -dev -telemetry flight.csv` writes one CSV row
  per sim tick (keys, axes, G, speed, angles).

## Known limits of M1

No enemies, weapons, radar or menus yet; no explosion effect on a crash (only the wreck camera); clouds are simple
billboards; the parked/circling jets are scenery. On the VM the game runs at 30 fps at 800x600 (Low preset); the
real target is the ThinkPad T40 at 1400x1050 (`-res 1400 1050 -preset high`, untested yet).

## Round notes

(The user's notes and the changes made per feel round go here, T10.)
