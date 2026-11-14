# Engine copy from the racer (T01, 2026-11-13)

Source: `../racer/engine/` and `../racer/sdk/` as of 2026-11-13 (racer file dates: core/coll/plat 2026-11-12,
newest `rend/r_d3d6.cpp` 2026-11-13 04:17, `world/e_world.cpp` 2026-11-13 05:28, `rend/e_rend.h` and
`world/e_mesh.cpp` 2026-11-13 01:04/01:05). The racer may keep changing its engine; port fixes by hand both ways
when they matter (pacing in `r_d3d6.cpp`, `e_input.cpp`). Never `#include` anything from `../racer`.

## What was copied

| Part | Files | Notes |
|---|---|---|
| `engine/core` | e_base e_math e_log e_cfg e_file e_image e_prof (.h/.cpp) | portable |
| `engine/coll` | e_coll | unused by jet, linked because `e_world.h` includes it |
| `engine/world` | e_tex e_mesh e_world | e_world = airbase `.trk` + sky only |
| `engine/rend` | e_rend r_backend.h r_null r_d3d6 | |
| `engine/plat` | e_plat e_input e_audio e_ui | |
| `engine/tools/assetkit` | whole package (copied by the orchestrator) | budget classes edited (below) |
| `sdk/` | DX6 headers, dx6guid.cpp, OWPL-LICENSE.txt, README (copied by the orchestrator) | unchanged |
| `engine/phys` | **dropped** (car physics) | |

Not copied: `engine/obj`, `engine/engine.lib`, `.pyc`, logs.

## New files (jet)

- `engine/world/e_terrain.h`, `e_terrain.cpp` (portable), `e_terrain_draw.cpp` (render): **stubs** by T01 with the
  DESIGN 8.6 API; owned by T04 from now on.

## Every jet edit to copied files

| File | Edit |
|---|---|
| `plat/e_plat.cpp`, `.h` | default window class `JetWnd`, title `Jet`, error box caption `Jet` (were Racer) |
| `core/e_log.h`, `core/e_file.h` | comments only (jet.log, jet.ini) |
| `core/e_cfg.h/.cpp` | appended `cfg_reload(Cfg*, path)`: re-reads the file in place, logs `cfg: changed [sec] key old= new=` per changed/added/removed key, returns the count (-1 = no file, cfg unchanged) |
| `plat/e_input.h/.cpp` | appended `in_action_held_ticks(a)` (polls held in a row) and `in_action_released(a)` (on the release poll: ticks held, a key tapped between two polls = 1; focus loss resets without an event). `Action` got `held, rel` fields, updated in `eval_actions` |
| `plat/e_ui.h/.cpp` | appended `ui_line(x0,y0,x1,y1,w,argb)` (virtual units; rotated quad, >= 1 px wide; Cohen-Sutherland clip of the centre line against the clip box shrunk by the half width, so the quad never needs r_draw_2d's screen clamp), `ui_set_blend(RB_BLEND/RB_ADD)` (flushes; reset by ui_begin), `ui_clip(x,y,w,h)` (virtual units, applies to rect/image/text/line; w or h <= 0 = screen; reset by ui_begin). `quad()` clips against the clip box instead of the screen |
| `rend/e_rend.h`, `r_backend.h`, `e_rend.cpp`, `r_d3d6.cpp` | appended `r_clear_z()` (Z-only `Clear2` of the whole target mid-frame, inside the scene; timed in the `r_clear` prof section); backend table field `clear_z` (NULL in r_null) |
| `world/e_world.h/.cpp` | sky loader for A2's names: the light set file is `<prefix>.txt`, `data\sky\<prefix>.txt`, else **`data\sky\light.txt`**; word keys `tile_prefix` / `tile_prefix_low` are parsed (new World fields `skyHiPrefix`, `skyLoPrefix`); High tiles = `<tile_prefix>_0..7` when `<prefix>_0` doesn't exist; Low (maxTiles <= 4) = `<tile_prefix_low>_0..3` first, then the racer's `<base>_low_0..3`, then halved High tiles; `fogStart`/`fogEnd` keys set `World.fogStart/fogEnd`. `world_set_sun_glow` loads **`tex\sun_glow.bmp`** first, then the racer's `tex\glow.bmp`, then the procedural glow (now cached as `sun_glow_proc`). So `world_sky_load(w, 8 or 4, "sky_hi")` or `(w, n, "light")` both load A2's sky |
| `tools/assetkit/validate.py`, `README.md` | budget classes `jet`, `jetpart` (hinge-origin warning), `missile`, `bldg`; prefixes `jetpart* jet* missile* msl_ hangar tower tank bunker bldg_`; texture rule `fx_*` 128/256 (DESIGN 8.8). Racer classes unchanged except prefix `bldg_` -> `bldg` |
| `rend/e_rend.h`, `r_d3d6.cpp`, `plat/e_ui.h/.cpp` | T02 (orchestrator-approved): appended blend mode `RB_PREMUL` (src ONE, dest INVSRCALPHA) for the premultiplied HUD atlas; `ui_set_blend` accepts it. Validated on the Voodoo3 (ValidateDevice ok, same fill cost as ADD/BLEND) |
| `plat/e_ui.h/.cpp` | T07 (orchestrator-approved append): `ui_quad4_px(tex, v[4])` (one screen-pixel quad with own uv/colour, unclipped, batched with the current blend), `ui_line_tex(tex, x0,y0,x1,y1, u, v0, v1, widthPx, centrePx, argb)` (textured line from an atlas line-profile cell, +v towards screen down/right so baked shadows fall down-right; clipped like ui_line), `ui_quad_count(reset)` (quads submitted, for HUD cost logs). The HUD (`src/hud_font.cpp`) records its quads and emits them through `ui_quad4_px` sorted by atlas page |
