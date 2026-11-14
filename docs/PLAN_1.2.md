# Ashfall 1.2 plan: "Island Control" (draft for the user, 2026-11-14)

Goal: the enemy is no longer something that appears on the map. Island Control's ground radar picks up unknown
aircraft far out, calls them on the radio, and the player goes out, identifies them with the jet's own radar and
destroys them. Bigger airspace, real ingress, a radio log on the HUD.

## Where we are (1.1)

- Area: RETURN TO AREA warning at 75 km from the island centre, turn-back at 90 km.
- Sortie waves spawn 45-55 km out (4-6 km altitude) with an attack order towards the player; the minimap shows every
  enemy from the moment it spawns (`[hud] mapShowAll=1`), so they effectively pop in.
- Player radar 40 NM (74 km) detect / 25 NM ID; enemy radar 20 NM.

## 1. Bigger airspace (T25)

- Area warning 75 -> ~130 km, turn-back 90 -> ~150 km (params `[world] areaWarnKm/areaTurnKm`). The island stays
  in the middle; outside it is open sea (cheap to draw).
- Minimap ranges 10 / 20 / 40 -> add **80 NM**; Tab cycles 10/20/40/80.
- "Bullseye" = the island summit: all radio calls give bearing/range from the base or the player (see 3).
- Check: robot sorties and the aifight harness, far plane/fog unchanged, enemy AI keeps inside the box.

## 2. Ingress: enemies fly in (T25)

- Each wave spawns **outside every radar** (~160-180 km from the base) on an ingress route towards the island: cruise
  ~Mach 0.85 at 5-8 km in formation (2-4 ships), heading for the base or a waypoint near it.
- New AI state **INGRESS** before PATROL/INTERCEPT: fly the route; switch to INTERCEPT when their own radar sees the
  player or when they reach the island; react to missiles as today (DEFEND).
- Timing target: first radio call ~45-60 s after the takeoff handover; merge window 3-4 minutes later unless the player
  pushes out to meet them. Later waves start their ingress while the previous wave is still fighting (staggered), so
  there is always something on the scope.
- Optional variety per wave: different ingress bearings, altitudes, one low-level group that hides under the radar
  until close (comms: "lost contact, last seen ...").

## 3. Island Control: ground radar + comms terminal (T26, T27)

**Ground radar (T26):** a radar at the base with long range (~120 NM high, less for low flyers: radar horizon) and a
slow sweep (~6-10 s). Its tracks feed the player's minimap as **yellow unknown** symbols (dimmer, updated each sweep,
no ID). Only the player's own radar (inside 25 NM) turns them red (hostile), as today. `mapShowAll` default off: what
you see is what Island Control and your radar see.

**Comms terminal (T27):** a translucent "terminal" panel bottom-right (both views; the chase weapon block moves up or
left), HUD green, 4-5 lines, typewriter text with a short radio click/static per message, newest at the bottom, old
lines fade. Example traffic (callsign "Viper 1", control "Island Control"):

- Takeoff: "Viper 1, Island Control, runway 27 cleared for takeoff."
- Detection: "Viper 1, Island Control: unknown aircraft, two contacts, bearing 025, 95 miles, angels 20, heading 205.
  Investigate." -> yellow icons on the minimap.
- Updates every ~30 s while unknown: "Unknowns now bearing 030, 70 miles, closing."
- ID by the player: "Viper 1, contacts confirmed hostile. Cleared to engage."
- Kills: "Splash one, good kill." / "Group bearing 025 destroyed."
- Threats: "Viper 1, bandits closing on the base, bearing 110, 30 miles!"; "Missile launch, check six!" (optional, the
  RWR already does this).
- Area: "Viper 1, you are leaving the area, turn back."
- Waves: "New contacts, bearing 160, 110 miles." / "Picture clear. All hostiles destroyed. Return to base."
- Failure/death: "Viper 1 is down ... Viper 1, new aircraft ready on runway 27."

The comms module is an event queue (mission/radar/AI post messages with priority, rate limits, no spam), so later
modes can reuse it. Text only (no voice), generated click/static sounds (A13).

## 4. Ideas for the "identify" part (pick any)

1. **Not every unknown is hostile**: now and then an unknown turns out to be a civilian airliner or a friendly patrol
   (ID shows it as green/white on the minimap and "FRIENDLY" text). Shooting it = big score penalty / mission
   failed. Makes ID matter and fits "identify and destroy". Needs: one airliner model (or the same jet, white) and a
   neutral flight path.
2. **Strike package on the base**: some waves are attackers (slower, low, heading straight for the base) with escorts.
   If an attacker reaches the base, the base takes damage ("Island Control is under attack!"); too much damage = mission
   failed. Gives a reason to go out and intercept early (BVR) instead of waiting over the island.
3. **Rearm at the base**: low on missiles -> land on runway 27 to rearm (automated landing like the automated takeoff).
   Bigger task; only if the sortie gets long.
4. **Wingman** (later): an AI wingman who takes the other target of a pair, with simple radio commands.

My recommendation for 1.2: 1-3 (airspace, ingress, Island Control + terminal) plus idea 2 (strike package on the base)
because it gives ingress a purpose. Idea 1 (civilians) as 1.3 if the ID step feels thin.

## 5. Task list (orchestrated as before: Opus agents, art/sound separately)

| Task | What | Owns (main files) |
|---|---|---|
| T25 | Airspace 130/150 km, minimap 80 NM, ingress spawns + INGRESS AI state, staggered waves, harness timing | `mission.*`, `ai.*`, world params, `hud_map.*` |
| T26 | Island Control ground radar: tracks, sweep, radar horizon, minimap unknowns, mapShowAll off | `radar.*`, `hud_tgt.*`/`hud_map.*` |
| T27 | Comms terminal: event queue, message texts, typewriter panel, layout move of the chase weapon block | new `comms.*`, `hud_comms.*`, `hud_wpn.cpp` (layout) |
| A13 | Radio click / static / beep sounds | `tools/sfx` |
| T28 | Strike package on the base (attackers + escorts, base damage, fail condition) | `mission.*`, `ai.*` |
| T29 | Integration, jet.ini, play-test guide 1.2, benches (15 fps floor), installer 1.2 | `main.cpp`, docs |

Also still open from 1.1: T40 engine-audio fix to confirm by ear; far-terrain detail texture (undecided); T40 High
preset tuning; hedge/wall sprites look like strips up close.
