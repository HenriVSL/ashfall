# Notes for the next AI (recent changes, neutral summary)

This is a short handoff describing the recent work in this codebase, written in generic terms
so it's safe to read without any domain-specific context. It covers what changed, where to
look, and how to verify it.

## 1. Long-range guided projectile — flight realism

The player's long-range self-guided projectile now flies a more physically plausible profile,
instead of one big constant push for the whole burn.

- **Two-stage propulsion**: a short hard "boost", then a longer gentler "sustain", then coast.
  New tunables: `sustainSec` / `sustainAcc` (section `[lrm_player]` in `jet.ini`).
- **Speed-dependent air resistance**: a small extra drag peak in the transonic band
  (`waveDragK`), so the projectile has to push through a brief high-drag regime.
- **Turn-dependent energy loss**: a quadratic term (`inducedK2`) so hard turns near the end
  bleed speed faster. This is what keeps the "guaranteed hit" boundary honest.
- **Climbing long-shot path (loft)**: long shots now climb into thinner air to save energy,
  then dive back in (`loftDeg` / `loftAltM` / `loftRangeM`).
- **Earlier self-guidance**: the projectile's onboard guidance switches on further out, and
  its steering field of view is wider.

Everything is tunable and only affects the player's projectile. The AI opponents' projectiles
are unchanged (the new knobs default to "off" for them, so their behaviour is bit-for-bit the
same as before).

Key files: `src/missile.h` + `src/missile.cpp` (physics), `src/dlz.cpp` (reach estimates),
`src/fx.cpp` (burn/glow timing). Tunables: `jet.ini` section `[lrm_player]`.

## 2. Launch-region gauge (the "is this a good shot" dial)

The cockpit dial that shows whether a shot is good was reworked into a "normalised" radial gauge:

- The "max range" mark is fixed at 6 o'clock, "min range" at 2:30, and a moving caret sweeps
  the arc between them as the target closes.
- The steering-error circle now scales with distance: tight at long range (needs precise aim),
  wider up close.
- A fourth "max reach" tick shows the projectile's absolute reach independent of how fast the
  target is closing.
- An "IN RANGE" text cue shows while the target is inside the launch window.

Key file: `src/hud_wpn.cpp`. Tunables: `[hud]` `aseDegLrm` / `aseDegLrmMin` (and the `...Srm`
pair), plus `wpnShootBand` / `wpnShootOnSec`.

## 3. Detection range — checked, no change needed

The player's detection system (40 NM) already out-ranges the projectile (~22 NM head-on), and
the AI opponents' detection (20 NM) matches their unchanged projectile (~10 NM). So you see and
lock before you can shoot, which is the intended flow. Verified consistent; left as-is.

## Verification

- `make -C tools/simh` — clean, no warnings.
- `tools/simh/simh test all` — passes (result 0).
- The HUD changes (`src/hud_wpn.cpp`) are render-side and are **not** covered by the Mac
  harness; they need the Windows VM build plus `-hudtest zone` and `-hudtest layout` to verify.

## Files touched

- `src/missile.h`, `src/missile.cpp` — projectile physics + new tunables
- `src/dlz.h`, `src/dlz.cpp` — launch-boundary solver + the new "max reach" value
- `src/hud_wpn.cpp` — gauge rework (normalised dial, steering circle, reach tick, IN RANGE cue)
- `src/fx.cpp` — burn timing now uses the full two-stage burn
- `src/tests_dlz.cpp`, `src/tests_missile.cpp` — updated expectations
- `data/dlz.tab`, `jet.ini` — regenerated
- `docs/DESIGN.md`, `STATUS.md` — documented (see the T21b and T21c log lines)
