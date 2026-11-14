# Jet M2 play-test: combat (TRAINING + 1-wave SORTIE)

This build adds the whole combat side: radar and targeting, the two missiles, flares, the launch zone, enemy
fighters that fly the same jet and shoot back, damage, lives, explosions and the combat sounds. What we want to know
from you: **is combat fun, readable and fair?** Can you tell what the HUD and the tones are telling you, does
"SHOOT" mean a hit, can you survive a missile with turns and flares, and how do the enemies feel?

Everything (flight feel included) is still tunable in `jet.ini` with F5, as in M1 (`docs/PLAYTEST_M1.md`).

## Starting

In VM A (double-click in Explorer, or Start > Run):

| Launcher | What it starts |
|---|---|
| **`M:\dev\jet\training.bat`** | **TRAINING**: runway start, three **drones** (they never shoot) fly circles ahead of the runway: one low and near (12 km, 1500 m), one at 20 km / 3500 m, one far and high (30 km / 6000 m). A drone you shoot down comes back 10 s later. Missiles and flares never run out. For learning the radar, the HUD and the launch zone. No lives, no end: quit with Esc. |
| **`M:\dev\jet\sortie.bat`** | **SORTIE**, Normal: runway start; after the takeoff handover "WAVE 1: 2 BANDITS BRG 240" appears and two enemy fighters come at you from 45-55 km at 4-6 km altitude (outside your AIM-120's reach: you get a beyond-visual-range approach). They have radar missiles and heat missiles and fire at you. Shoot both down: MISSION COMPLETE. You have 3 lives (3 missile hits = one life; a crash = one life). |
| `sortie.bat easy` / `sortie.bat hard` | the same on Easy (enemies fire from far away, react slowly) or Hard (3 bandits, they wait for close shots, turn hard, flare well, hide behind the volcano). Start > Run: `M:\dev\jet\sortie.bat hard` |
| `play.bat` | M1 free flight (no enemies), unchanged |

All launchers: fullscreen 800x600, Low preset, dev keys on. "LOADING ISLAND" takes about 10 s. Every flight writes
`last.rpl`, the log is `jet.log`.

The takeoff is as in M1: **A** (tap, or hold for afterburner), the jet climbs out by itself, "YOU HAVE CONTROL" at
1500 ft. You are invulnerable during the takeoff and for 3 s after the handover. After a death (or a crash) the
camera circles the wreck for 2.5 s and you start again on the runway with full missiles.

## Keys

| Key | What it does |
|---|---|
| Up / Down, Left / Right | pitch, roll (as M1; F6 = Expert / Novice) |
| A / Z | fast (afterburner) / slow (as M1) |
| **S tap** | **select** the next contact: the one nearest your nose first, then outward (taps less than 2 s apart walk through all of them). A green triangle sits above the selected one |
| **S hold** (~0.3 s) | **designate** (lock) the selected contact: green square = the TD box, lock beep. Hold S on the locked one again = break the lock. Nothing selected: holding S locks the contact nearest your nose |
| **D** | switch missile: **AIM-120** (radar, long range, 16) <-> **AIM-9** (heat, short range, 40) |
| **Space** | **fire**. AIM-120 with a lock: guided. Without a lock it goes "maddog" (straight ahead, then its own radar looks for anything). AIM-9: guided when the seeker tone is the high steady tone, else it searches by itself |
| **F** | **flares** (a burst of 2; hold = a burst every 0.5 s; 40 per life) |
| **V (hold)** | **look at the locked target**: in the cockpit your head turns towards it (up to 120 deg; the HUD glass stays with the nose, so it hides while you look); in chase view the camera swings behind your jet so jet and target are both in view |
| C | cockpit view (full HUD) / chase view (reduced HUD) |
| Tab | minimap range 10 / 20 / 40 NM |
| F5 / F6 / F10 / F11 / F12 | reload jet.ini / Expert-Novice / feel overlay / fps / screenshot `shots\jet_NNN.bmp` |
| Esc or P | pause: RESUME / RESTART ON RUNWAY / **RESTART MISSION** (new sortie or training from the start) / QUIT |

## What the HUD shows

**Contacts (both views).** Every aircraft your radar sees is a small **hollow diamond** (corner down) on it:
**yellow** = detected, not identified yet; **red** = identified hostile (takes 1.5 s within 25 NM, instant within
5 NM); your radar sees 40 NM (74 km) nose-on / tail-on, 50 NM from the side (enemy radars: 20 / 25 NM, ID 15 NM); **dashed, half bright** = radar memory (you lost it, e.g. behind the volcano or outside the radar cone: the
symbol shows where it should be now; after 6 s it is gone). Under the diamond: range in NM ("12.4", "17"; under 1
NM in hundreds of feet: "8"); the locked one also shows its altitude in thousands of feet. Off-screen contacts
within 20 NM: small coloured triangles at the screen edge pointing at them.
- **Triangle** above = selected (S tap). **Green square (TD box)** = designated/locked (S hold); dashed = the lock
  is in memory (it drops after 3 s if the radar does not find it again). In the cockpit the box stays inside the
  HUD frame; when the target is outside it the box sits on the frame edge flashing, and a **locator line** from the
  centre points at the target with the degrees off your nose.
- **Destroyed:** the diamond turns **grey** for 3 s with **SPLASH** above it; on the minimap a grey-red **X** for 15 s.
- **Minimap** (bottom left): HAFU symbols (yellow arch unknown, red chevron hostile, dashed memory, green square
  around the locked one), all enemies always (dim when your radar does not see them), your missiles green dots,
  enemy missiles red blinking dots, red ticks on the frame = the direction of an enemy that has you locked.
  "F40" top right = flares left.

**Weapons (cockpit).** Weapon line under the circle: "AIM-120 16" / "AIM-9 40".
- **Launch-zone circle** (AIM-120 or AIM-9 with a lock): the range scale goes clockwise from 12 o'clock. Ticks:
  **Rmax** (outward, the farthest shot that hits a target that keeps flying straight), **Rne** (double outward
  tick: inside it the target **cannot escape** by turning and running), **Rmin** (inward tick: too close). The
  triangle on the ring is the current range. These numbers are computed live by flying our own missile model
  against the target's current speed and heading, all the time: **they are honest**. A target behind the volcano
  collapses the zone (the missile would hit the mountain).
- **SHOOT** above the TD box: **steady** = inside Rmax (it hits if the target keeps flying straight); **flashing** =
  inside Rne (no escape: it hits whatever the target does, unless flares decoy it or terrain blocks it). No SHOOT =
  don't. **Big X** in the middle = too close (inside Rmin).
- **Steering dot** inside the circle: put it in the middle for the best shot (it points at the intercept).
- **Readouts** right of the circle: "960 Vc" closing speed (kt), "19.9 RNG" range (NM), and the third line: "23 TOF"
  (time of flight if you shoot now) -> after an AIM-120 launch "17ACT" counting down to the moment its own radar
  goes active, then "11TTG" (time to impact). After that the missile needs no lock any more (fire and forget).
- **AIM-9**: the small **seeker circle** sits in the middle; when it finds heat near the target it jumps onto it and
  the tone changes (below). With a lock it follows the target.
- **AIM-120 with no lock**: a dashed circle + "VISUAL": maddog.
- **Chase view**: no circle; a small vertical bar next to the TD box with the same Rmax / Rne / Rmin ticks and a
  caret for the range, the same SHOOT rules, the third line under it; weapon block bottom right (name, count, FLR).

**Warnings (red, both views).** **MISSILE** (flashing) = an enemy missile is homing on you (radar missiles from the
moment they are launched; heat missiles once within 5 km); up to 3 **red arrows** around the screen centre point at
the missiles (12 o'clock = ahead, 6 o'clock = behind you; the nearest is brightest). **LOCK** = an enemy radar has
you locked. **DAMAGED** = you took 2 hits (your jet trails smoke; one more hit and you are down).
**Clock** bottom left of the HUD frame (sortie): mission time.

## What the tones mean

| Tone | Meaning |
|---|---|
| short beeps + held high note | **your lock** (S hold succeeded) |
| low raspy **growl**, rising when you point at the target | AIM-9 selected, the seeker sees heat near it |
| high **steady tone** | AIM-9 **locked**: SHOOT if it says so |
| one falling 4-note **"waterfall"** (550-470 Hz) | a new enemy radar has found you |
| slow two-tone **455/555 Hz, 2 per second** | an enemy radar has you **locked** (LOCK) |
| fast two-tone **455/555 Hz, 5 per second** | **missile in the air at you** (MISSILE): turn, run, flares. It gets 4 dB quieter after 2 s and loud again when a missile is within 2 km or a new one is fired |
| whoosh-roar | your missile launch; other launches near you are positional |
| rocket hiss passing (doppler) | a missile flying past; a "miss" swoosh when an enemy missile misses you within 200 m |
| bang (delayed by distance), chime | a kill; the chime comes with SPLASH |
| thump | you are hit |
| jet roar passing | an enemy jet passes close |
| click | Space with that missile type empty |

## How to fight (suggestions, not rules)

- Climb out, look at the minimap (Tab to 40 NM: the wave starts 24-30 NM away): the bandits come from the bearing in
  the WAVE message. Tap S until the far one is selected, hold S to lock. AIM-120 is the default.
- Your AIM-120 reaches about **38 km (20 NM) head-on** at 3000 m against a target flying straight (more up high:
  ~59 km at 8 km altitude, ~31 km at 1 km), no-escape (flashing SHOOT) about **14 km** head-on, ~17 km from behind
  (steady SHOOT), ~0.9 km minimum. The enemies' AIM-120s are unchanged (~19 km head-on, no-escape ~7.5 km): you can
  shoot first. AIM-9 unchanged for everyone (~5 km lock range nose-on).
- Fly towards it with the steering dot centred. **Steady SHOOT** = you can fire now (a straight-flying target is
  hit); **flashing SHOOT** = it cannot get away. Fire, then watch ACT count down: once it says TTG you can turn
  away (crank) or lock the next one.
- When you hear the fast two-tone: the arrows show where the missile comes from. Turn so it is beside you (beam)
  or behind you, go fast (A), and drop flares (F) when it is close. The volcano blocks missiles too.
- Close in (under ~5 km), switch to AIM-9 (D): growl -> high steady tone -> SHOOT -> Space.

## What to judge (please note anything, even vaguely)

1. **Readability**: can you tell unknown / hostile / memory / locked apart at a glance, in both views, over sea,
   island and cloud? Is anything too small, too big, cluttered or flickering?
2. **The launch zone**: does steady / flashing SHOOT match what happens? Did a flashing-SHOOT shot ever miss (and
   was it flares or the volcano)? Did a no-SHOOT shot hit?
3. **The tones**: can you tell growl, AIM-9 lock, enemy lock and missile warning apart while flying in afterburner?
   Too loud / too quiet / annoying? (`[sound] warn` and `combat` in jet.ini scale them; F5.)
4. **Enemies**: too easy / too hard per difficulty? Do they fly believably? Do their missiles feel fair and
   dodgeable? Are 3 hits per life right (`[mission] missileDamage`)?
5. **Effects**: missile trails, explosions, flares, smoke when damaged, the falling burning wreck, water splashes.
   Too big, too small, too long?
6. **V look**: useful in the cockpit? in chase?
7. **Frame rate** (F11) when a lot happens.

## Reporting

Tell us what happened and roughly when; after an interesting flight copy **`last.rpl`** to `tests\user\` with a
name (e.g. `m2_missed_shoot.rpl`). We can replay it exactly (`jet.exe -replay tests\user\NAME.rpl`) and read every
number. `jet.log` has the mission lines (`mission: wave`, `kill`, `hit player`, `death`, `result`) and every missile
event (`wpn: LAUNCH/HIT/DECOY/...`). F12 screenshots go to `shots\`.

Useful jet.ini keys for quick changes (F5 applies them): `[mission] missileDamage lives sortieWaves` (set
`sortieWaves=3` for the full 3-wave sortie: Normal = 2, 3, 4 bandits), `[ai_easy] [ai_normal] [ai_hard]` (reaction,
G use, fire ranges, flares), `[weapons] lrmCount srmCount flareCount`, `[hud] mapShowAll`, `[sound] warn combat`.

## Known limits of M2

No menus yet (launchers instead; the results panel shows for 15 s after the sortie); the full 3-wave sortie, score
records and difficulty selection in a menu come in M3. Enemies are the same jet in the dark aggressor livery. On the
VM, heavy fights cost frame rate (T18 tunes); the real target is the T40.

## Round notes

(The user's notes and the changes made per round go here, T21.)

**Round 1 (2026-11-14, user):** "the IR feels good. The radar guided should go double distance. Enemy weapons should
stay the same and then it's well enough balanced for V1." Changes (T21a):
- **Your AIM-120** has its own numbers (`[lrm_player]` in jet.ini; the enemies keep `[lrm]`): a stronger motor, less
  drag, 60 s life. Co-altitude 3000 m, both at normal speed: head-on Rmax 19.4 -> **38.3 km**, Rne 7.2 -> **14.0 km**,
  tail-chase Rmax 6.8 -> 17.2 km, crossing (beam) 10.5 -> 24.0 km, Rmin 0.76 -> 0.89 km. The launch zone on the HUD
  flies this missile (still honest: 0.97 Rmax hits 40/40, 1.05 Rmax misses 39/40, inside 0.95 Rne 100 % vs every
  escape in the tests).
- **Your radar**: detection 20 -> **40 NM** head-on (50 NM beam), lock range 1.2x that, identification 15 -> **25 NM**
  (`[radar] playerDetRangeNm playerIdRangeNm`). Enemy radars unchanged (`detRangeNm idRangeNm`).
- **SORTIE waves** start 22-28 -> **45-55 km** away (`[mission] spawnMinKm spawnMaxKm`). TRAINING drones unchanged
  (12 / 20 / 30 km).
- Enemy weapons, the AIM-9 and the enemy AI's fire decisions unchanged.
