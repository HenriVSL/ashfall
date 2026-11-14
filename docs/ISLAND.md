# The island (A6 colour pass + A11 detail, 2026-11-14)

Source: `island/island.txt` (values), compiler `tools/islandc/` (T03/T03b, A11 additions), textures
`tools/art/gen_ground.py` (A3 + A11 `t_forest`, `t_scrub`), sprites `tools/art/gen_veg.py`, base props
`tools/art/gen_props.py`, runtime sprites `src/scatter.cpp`. Review sheet: `tools/islandc/out/review_*.png`.

## Concept

A green Atlantic volcanic island in the Azores / Madeira mood, 34 x 34 km, one big active cone (summit 3077 m,
crater rim r 1250 m) with six satellite cones, cliffs on the weather side, and the airbase on the dry west cape. The
look is driven by one idea: **the trade wind blows from the ENE** (az 65, `moisture 65 0.6`), the same side as the
cliffs and the swell. So:

- **Windward (N, E, NE)**: wet. Dense laurel / broadleaf forest (`t_forest` canopy texture) from the coast up to the
  treeline (~1250 m), dark rectangular cedar (cryptomeria) plantations inside it, clearings, a cloud-forest belt
  around 900 m where the forest is densest. Seen from anywhere it is the dark half of the island.
- **Leeward (W, SW, S)**: drier. Lighter pastures with yellow-olive tones, woodlots, dry scrub patches (`t_scrub`),
  the airbase cape with mowed grass.
- **Lowlands below ~300 m on gentle slopes**: a farmland patchwork in districts (`fields 300 7 0.55`): brick-like
  plots of 2-6 x 2-5 terrain quads (160-480 m) in strips, five plot types (pasture, hay, crop on `t_grass`; stubble,
  ploughed on `t_scrub`), with hedgerows and black basalt stone walls on the plot borders (scatter sprites).
- **Upland pastures (up to ~960 m)**: subtle stone-walled plots, pasture and hay only.
- **Heath belt 1150-1400 m** (`scrub_belt`): brown heath under the ash, then the bare cone: grey ash with red scoria
  patches, rock on the steep ridges, sulphur-stained inner crater rim, dark crater floor.
- **Lava**: glowing flows (`t_lava`) only above 1700 m and in the crater (`lava_cool 1700`): the volcano is active
  at the top. Below that the six flows are cooled black basalt (rock), and under ~935 m old flows overgrown with
  dark heath; they stay readable as dark tongues down the slopes from every altitude.
- **Coast**: patchy beaches, ~45 % black volcanic sand (more where flows reach the sea), turquoise shelf, palm groups
  along the low coast, cliffs on the ENE side, rock outcrops on lowland knolls.
- **Villages**: 8 small white-walled villages with terracotta roofs and a church tower in the farm districts near
  the coast (sprites; positions in `tools/islandc/out/review_top.png`).

Why: the user's island looked "monotonous green" at 80 m cells. A believable climate gradient gives large-scale
structure readable from 10 km (dark windward forest vs light leeward pasture vs brown heath vs grey cone), the
plots and forest texture give mid-scale structure from 1-3 km, and the sprites give the low-level detail and speed
sense below 300 m. It stays a green volcanic island first: grass + forest are ~74 % of the land.

## Landmarks for navigation

- The cone and crater (everything points at it), the six dark lava tongues (NNE, ESE (longest, 7 km), SSE, SSW, WSW, WNW).
- Airbase: west cape, runway 27/09 (heading 270 takeoff out to sea), hangars, tower, tank farm, radome, fences.
- The dark windward forest (NE quadrant) vs the light SW pastures: tells your side of the island at a glance.
- Satellite cones (NE 820 m, SE 640 m, NW 520 m, W 420 m, S 380 m, N 560 m), two islets (E and NW).
- Villages (white specks + church) along the farmland coasts.

## Flight rules kept

- Runway plateau 12 m flat, embankments grassy; no scatter within 350 m of the runway / taxiway / apron / buildings,
  and none in the climb-out corridor (5 km past the end, +-600 -> 1500 m). Nothing in the scatter is taller than
  ~17 m (cedars at max scale), far below the 150 m climb-out limit; terrain rule unchanged (report: climb-out max
  terrain 46 m).
- "Hide behind the volcano" works as before (geometry unchanged); sprites have no collision and no LOS effect.

## Materials (8 = TERRAIN_MAX_MATS)

`t_sand t_grass t_rock t_ash t_lava foam t_forest t_scrub`. CLAS keeps the 8 game classes (forest and scrub are
`grass` for impacts/minimap); the material sub-class only picks the texture. Vertex colour = class/variant tint x
baked light; A11 halved the colour window of LOD >= 1 vertices (`chunks.py`) so plot and forest edges survive to
LOD1-2 (beyond that the island shows its broad zones only: the renderer's coarse grids can't carry 300 m plots).
Tints: `island.txt` (A3 convention, each texture divides by its own mean).

## Scatter (sprites)

`data/island/veg.sct` (islandc `scatter.py`): ~74k instances in 32 x 32 cells of 1280 m (max ~320 per cell), 9
sprites from `veg_atlas.bmp` (256x256 keyed): grove, cedar, palms, tree, shrub, rock, hedge + wall (fixed
orientation along plot borders), houses. Densities in `island.txt` (`scatter NAME DENSITY`). Runtime: Low 1600 m /
600 sprites max / thinned to 35 % at the range, High 3500 m / 2400 / 60 %; `[scatter]` keys override. Nothing is
drawn when the camera is higher above the ground than the range.

## Base props

`gen_props.py`: truck (fuel bowser), tug, jeep, apron light masts, windsock (`prop` keys, no collision boxes) and a
2.4 m chain-link fence (base perimeter, tank farm, radome; `fence` keys) in base.trk.

## How to change

- Look: tints and the A11 keys in `island.txt` (documented in `tools/islandc/classes.py` / `scatter.py`
  docstrings); `python3 tools/islandc/islandc.py island/island.txt` (~55 s with the review sheet), `--check`.
- Sprites: `gen_veg.py` (`python3 tools/build_assets.py --only veg`), then rebuild the island (it reads
  `tex/veg_atlas.txt`).
