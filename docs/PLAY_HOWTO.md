# Ashfall: Island Intercept - how to play

Windows 98 arcade flight combat. Keyboard only. (Project folder `dev/jet`, program `jet.exe` until the release.)

## Starting

| Launcher | What it does |
|---|---|
| `ashfall.bat` (or plain `jet.exe`) | **title screen** with the menus; resolution, preset, window/fullscreen, keys and volumes from `jet.ini` (OPTIONS) |
| `play.bat` | free flight straight away (800x600 Low, dev keys F9/F10) |
| `training.bat` | TRAINING straight away (3 drones that don't shoot back) |
| `sortie.bat [easy\|hard]` | SORTIE straight away (Normal without an argument) |

Loading the island takes about 8 seconds over the share.

## Menus

Arrows move (held arrows repeat), **Enter** (or Space) selects, **Esc** goes back. On the title screen Esc moves the
cursor to QUIT.

- **FLY**: FREE FLIGHT (no enemies), TRAINING (3 drones, unlimited missiles: learn the launch zones), SORTIE (waves
  of bandits, 3 lives, score and records). DIFFICULTY (Left/Right): EASY / NORMAL / HARD. Enter on a mode takes off
  from the runway.
- **OPTIONS**: the cursor starts on the tab row; Left/Right switches CONTROLS / VIDEO / SOUND, Down enters the list.
  - CONTROLS: Enter on an action, then press the new key (Esc cancels). A key that another action already uses is
    **swapped** (that action gets your old key; the message line says so). Esc, F5, F6, F9-F12 and the pause keys
    (Esc, P) are reserved. **Del** puts the selected action back to its default key. INVERT PITCH (Left/Right):
    OFF = Up arrow nose up (default). CONTROL STYLE: EXPERT / NOVICE (same as F6 in flight).
  - VIDEO: PRESET (LOW for the VM, HIGH for the ThinkPad T40), RESOLUTION (the 4:3 16-bit modes of the card, 800x600
    and up), DISPLAY (fullscreen / window), DEFAULT VIEW (cockpit / chase). Preset, resolution and display apply at
    the **next start** (the line turns orange when they differ from the running game). Command-line switches
    (`-res`, `-preset`, `-window`, `-fullscreen`, used by play/training/sortie.bat) override them.
  - SOUND: MASTER, ENGINE, WARNING TONES (RWR, lock and missile tones), COMBAT EFFECTS, 10 steps each, heard at once.
  - Every change is saved to `jet.ini` immediately; comments and all other keys stay as they are.
- **RECORDS**: best SORTIE score per difficulty.
- **QUIT**.

## In flight

Default keys (all rebindable in OPTIONS > CONTROLS):

| Key | Action |
|---|---|
| Up / Down | pitch nose up / down (INVERT PITCH swaps) |
| Left / Right | roll |
| A (hold) | FAST: afterburner; also starts the takeoff roll |
| Z (hold) | SLOW: speed brake |
| S tap / S hold | next target / lock the selected target (hold again on the locked target = break lock) |
| Space | fire the selected missile |
| D | next weapon (AIM-120 <-> AIM-9) |
| F | flares |
| C | cockpit / chase view |
| V (hold) | look at the locked target |
| Tab | minimap range |
| Esc or P | pause menu |
| F5 | reload jet.ini (live tuning) |
| F6 | Expert / Novice control style |
| F11 | fps counter |
| F12 | screenshot (`shots\jet_NNN.bmp`) |

**Pause menu**: RESUME, RESTART ON RUNWAY, RESTART MISSION (training and sortie), OPTIONS (the same options screen;
key changes work at once), QUIT TO MENU.

## Results

When a SORTIE ends (all waves down: MISSION COMPLETE; out of lives: MISSION FAILED) the flight freezes three seconds
later and the results screen shows time, kills, hits / fired per missile type, deaths, the score breakdown (kills x
1000, BVR bonus x 300, deaths x -1500, time bonus, accuracy), the score and the best score of that difficulty
(NEW RECORD in orange). CONTINUE goes back to the title, FLY AGAIN starts the same sortie again.

## For testers: command line

`-mode free|training|sortie`, `-diff 0|1|2`, `-start runway|air`, `-script`, `-replay`, `-bench`, `-camat`,
`-record`, `-telemetry`, `-exitafter`, `-shot` and `-headless` start a flight directly (no title), exactly as
before. `-mscript FILE` runs a menu flow from the title (`tests\t19\*.scr`; settings go to `t19_jet.ini`, never
to `jet.ini`); `-ini FILE` starts with another ini file. Every flight is recorded to `last.rpl`; the log is
`jet.log`.
