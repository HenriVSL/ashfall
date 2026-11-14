# Jet performance notes

VM A (86Box Pentium II 233, Voodoo3, Win98 SE), Low preset, 800x600x16 fullscreen, 120 Hz emulated refresh, 30 fps
display lock (divisor 4). Bench command (window must be visible, no VNC connected):

```
jet.exe -bench 60 -benchroute <low|volcano|high|clouds|combat> -preset low [-benchview cockpit]
```

The report goes to `bench.txt`; `section` lines are CPU ms per frame (avg / worst). `draw` contains every scene
section, the HUD and (since T18) `dlz`; `present` is mostly the vblank wait. Gate (TASKS T18 / DESIGN 9.3):
fps_avg >= 29.8 and missed <= 2 % on all five routes (the user's VM floor is 15 fps; the 30 fps lock is the
nice-to-have that the benches check).

M1 history: T08 first measured the four flight routes (29.97-29.98 fps), T10b re-measured them for the 3x world
(29.96-29.98 fps; reports in `tests/t08/logs/`, `tests/t10b/logs/`). There was no separate T09 run.

## M2 (T18, 2026-11-14)

Reports: `tests/t18/base_*.txt` (build at the start of T18, i.e. T17's code + the `dlz` section) and
`tests/t18/t18_*.txt` (final T18 code). One 60 s run each; run-to-run noise is about +-0.3 % missed.

| route / view | fps_avg | missed % | low1 fps | draw ms avg/worst | sim | terrain | aircraft | fx | HUD avg/worst | DLZ | tris avg/max |
|---|---|---|---|---|---|---|---|---|---|---|---|
| low (chase) | 29.97 | 0.22 | 29.81 | 12.49 / 26.2 | 0.48 | 1.65 | 5.08 | 0.43 | 0.78 / 1.10 | 0.01 | 2461 / 5982 |
| volcano (chase) | 29.97 | 0.28 | 29.81 | 15.91 / 25.7 | 0.49 | 6.13 | 4.90 | 0.31 | 0.77 / 5.49 | 0.01 | 3604 / 4537 |
| high (chase) | 29.97 | 0.28 | 29.81 | 13.17 / 21.2 | 0.48 | 3.07 | 4.90 | 0.29 | 0.79 / 1.02 | 0.01 | 2291 / 2705 |
| clouds (chase) | 29.98 | 0.17 | 29.81 | 12.43 / 17.8 | 0.48 | 2.08 | 4.92 | 0.31 | 0.81 / 1.14 | 0.01 | 2254 / 2541 |
| **combat chase** | **29.94** | **0.61** | 29.81 | 16.71 / 25.4 | 1.44 | 1.52 | 5.47 | 3.89 | 1.20 / 3.32 | 1.06 | 2285 / 3701 |
| **combat cockpit** | **29.94** | **0.61** | 29.72 | 12.16 / 20.5 | 1.43 | 1.28 | 0.76 | 3.76 | 1.80 / 6.71 | 1.15 | 1103 / 2569 |

Start of T18 (same routes, `base_*`): low 29.96 / 0.39 %, volcano 29.98 / 0.11, high 29.96 / 0.33, clouds 29.97 /
0.22, combat chase 29.89 / 1.11 (a second run 29.92 / 0.95), combat cockpit 29.94 / 0.72. **All pass the gate before
and after.** Combat load in the final runs: 7.1-7.3 missiles avg / 9 max, fx 218-220 quads avg / 292 max, 4
explosions max, 2 contacts.

### Budgets (DESIGN 9.3, CPU per frame on the VM)

| item | budget | measured (worst route avg) | verdict |
|---|---|---|---|
| sim (4 ticks) | <= 5 ms | 1.44 ms (combat; flight routes 0.48) | ok |
| scene (draw minus HUD/DLZ/fx) | <= 15 ms | ~10.6 ms combat chase, ~14.8 volcano | ok |
| HUD + minimap | <= 1.5 ms | flight routes 0.77-0.81; combat chase 1.20; **combat cockpit 1.80** | over in combat cockpit: accepted (below) |
| DLZ | <= 1.5 ms | 1.06-1.15 ms avg (time cap `[dlz] maxUsFrame` 1400 us, doubled for the first 0.25 s of a new target) | ok |
| total tris / draws | <= 7k / 90 | 5982 max (low route), 54 draws max | ok |

Worst single-frame section times (HUD 5-7 ms, fx 14 ms, DLZ 4 ms) are not reproducible frame to frame: they come
with the emulator's occasional stalls (the same frames show up in every section at once; the DLZ cap is checked
between solver chunks, so it cannot run 4 ms by itself).

### HUD decision (DESIGN 10.7.8: 1.5 ms)

The `hud` section is the whole overlay: flight HUD (T07 ~1.3 ms in the cockpit, ladder + text), target symbology
(T15, 0.25-0.33 ms), weapon HUD (T16, 0.2-0.3 ms) and the minimap. In combat cockpit it averages **1.80 ms** (T18's
new overlap rules add ~0.05-0.09 ms: slot choice, arrow placement and the ladder's extra keep-out boxes). Decision:
**accept 1.8 ms on the VM.** Reasons: (1) the fps gate holds with a large margin in the cockpit view (draw 12.2 ms of
a 33.3 ms slot); the frame-time risk is the chase view, where the HUD is 1.2 ms and the own jet (5.5 ms) and fx
(3.9 ms) dominate; (2) the user's VM floor is 15 fps and the real target (T40, Pentium M) has several times the CPU;
(3) the remaining levers cost readability or code risk for ~0.3 ms (pre-rendered ladder labels, dropping the shadow
quads, caching static text quads across frames). Cheap reductions done in T18: none needed beyond keeping the new
code light (arrow radius computed only when an arrow touches something; bench param lookups cached).

### AI think spikes (T17 open issue)

T17 saw single think ticks up to 1415 us in `-test ai` (9 of 3000 over 200 us). Cause, found with a nanosecond-clock
simh build: the spikes were exactly the thinks in which an enemy designates the player (INTERCEPT -> ATTACK_BVR,
`radar_designate`): radar.cpp logs DESIGNATE + LOCK at INFO, and `-test` runs with log autoflush on, so each line was
`fflush`ed to the Samba share inside the timed think (0.4-0.6 ms per line on the VM). With the radar log lines muted
those thinks are ordinary (Mac: worst 1.3 us vs 0.25 us median, first think after spawn). The other big single
values (e.g. 6042 us on a MERGE tick) are emulator stalls: a second identical run does not repeat them. The game
loop never autoflushes (it flushes once per sim second), so play never paid the spike. No AI code change was
needed; `-test ai timing` now runs with autoflush off and runs the deterministic scenario twice, counting each think
with the smaller of its two times. VM result: **avg 81.3 us, worst 154 us, 0 of 3000 above 200 us** (single-run
worst 205 / 226 us). `simh aifight 100 1 1`: K/D 5.22 before and after (identical: ai.cpp unchanged).

### Where the frame goes (levers for later, not needed for the gate)

- **Own jet, chase view: ~4.9-5.5 ms** on every route (LOD0 mesh + 6 moving parts with CPU lighting in
  `mesh_draw_lit`, rail missiles). Biggest single item. Lever: light the static LOD0 vertices once per frame per
  part (sun/ambient change slowly), or LOD1 parts for the own jet; scene.cpp owner (T22).
- **Combat fx: ~3.8 ms** (trails, explosions, flares; 218 quads avg). Lever: build trail ribbons in one pass per
  missile with a shared vertex buffer; fx.cpp owner (T22).
- **Terrain on the volcano route: 6.1 ms** (the big cone in view) - the slowest flight route still has 0.28 % missed.
- On the T40 (1400x1050, Radeon, T23) the CPU items shrink and fill rate becomes the question; High preset is
  checked there.

## M3 visual polish (T22, 2026-11-14)

Reports: `tests/t22/t22_*.txt` (final T22 code, with A11's scatter and T19/T20 code of the same time in the build),
A/B of the aircraft path `tests/t22/b_low_fast.txt` / `b_low_slow.txt` (`-script tests\t22\acslow.scr` sets
`[scene] acFast=0`). Gate (user, 2026-11-14): fps_avg >= 15 with no sustained stretch below 15; the 30 fps lock still
holds on every route.

| route / view | fps_avg | missed % | fps_min1s | draw ms avg/worst | sim | terrain | aircraft | fx (fx_wreck) | HUD | tris avg/max |
|---|---|---|---|---|---|---|---|---|---|---|
| low (chase) | 29.96 | 0.28 | 29.50 | 11.42 / 25.9 | 0.52 | 1.72 | 2.90 | 0.49 (0.01) | 1.02 | 2096 / 5984 |
| volcano (chase) | 29.96 | 0.33 | 29.74 | 14.26 / 20.7 | 0.53 | 6.34 | 2.60 | 0.35 | 1.03 | 3110 / 4047 |
| high (chase) | 29.97 | 0.22 | 29.50 | 11.49 / 17.2 | 0.53 | 3.17 | 2.59 | 0.44 | 1.03 | 1792 / 2202 |
| clouds (chase) | 29.98 | 0.11 | 29.74 | 10.44 / 19.5 | 0.52 | 2.14 | 2.57 | 0.36 | 0.98 | 1750 / 2034 |
| **combat chase** | **29.91** | **1.00** | 29.50 | 13.10 / 22.3 | 1.58 | 1.57 | 3.05 | 2.46 (0.20) | 1.33 | 1715 / 2796 |
| **combat cockpit** | **29.94** | **0.61** | 29.50 | 10.04 / 21.2 | 1.59 | 1.31 | 0.56 | 2.01 (0.18) | 1.83 | 1065 / 2257 |

vs T18: draw low 12.49 -> 11.42, combat chase 16.71 -> 13.10, combat cockpit 12.16 -> 10.04 ms, with all the T22
effects on (and A11's vegetation). The aircraft section now also contains the jet shadows.

- **Own jet / aircraft** (`[scene] acFast`, scene.cpp `jet_piece` / `fast_append`): only camera-facing triangles are
  lit and passed to the backend, and the jet + moving parts + rail missiles go out as one batch per material. Same
  route, A/B: aircraft **5.27 -> 2.84 ms**, ac_tris 1190 -> 603, draws 56 -> 36, draw 13.50 -> 10.74 ms (low route).
  Look unchanged (same lighting formula; parts now lit with the sun in their own frame).
- **Combat fx**: 3.89 -> 2.46 ms (chase). Found with temporary sub-sections: the cost was CPU math per trail point and
  per puff, not the quads (fx_quads 218 -> 47 after culling, but that alone saved only ~0.3 ms). Cheap wins: frustum
  out-codes before building billboards and ribbon segments (plus a conservative pre-pass per ribbon so off-screen
  points skip exp/sqrt/colour work), puff cos/sin + lit colours computed once at spawn, squared-distance puff sort,
  `pow` -> `sqrt`. Trails ~1.3 -> 0.65 ms, puffs ~1.1 -> 0.66 ms. Falling wrecks (`fx_wreck`, own section now)
  0.2 ms (`[scene] wreckLodScale` 0.3).
- **Polish costs** (VM, Low): vapour/sheets/cone and contrails only when they happen (high route contrail +0.1-0.15
  ms; a 9 g pull ~0.1 ms); shadows ~0.05 ms per jet (15 projected vertices, 2 world_surface samples each); cloud
  haze one full-screen quad + up to 6 wisps; explosion layers + columns are inside the combat fx numbers above
  (puff cap Low 128 unchanged, High 256).
- High preset (`-preset high`, combat 20 s at 800x600, VM, `t22_high_preset_combat_20s.txt`): runs, fx 2.5 ms; the
  60 fps check belongs to T23 on the T40.
