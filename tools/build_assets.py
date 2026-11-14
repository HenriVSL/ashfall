#!/usr/bin/env python3
"""Rebuild every generated asset into data/ (DESIGN 8.8). Thin wrapper around assetkit.build.

    python3 tools/build_assets.py                    # all generators + the island, then validate data/
    python3 tools/build_assets.py --only jet,sfx     # some generators (name = gen_<name>.py, "island", or a kind)
    python3 tools/build_assets.py --check            # rebuild into a temp dir, compare md5 with data/ (writes nothing)
    python3 tools/build_assets.py --list             # show discovered generators
--only imports only the named generators; a generator that fails to import fails alone (the others still run).
Docs: engine/tools/assetkit/README.md

Jet addition (T01): the island is one more generator named "island" (kind "track"): island/island.txt compiled by
tools/islandc/islandc.py (T03). islandc protocol (same shape as the kit's trackc protocol):
    islandc.build_island(island_txt_path, out_dir, kit) -> list of written paths relative to out_dir
    (e.g. ["island/jet.isl", "island/base.trk", "tex/map_island.bmp", "tex/map_island_a.bmp", "island/report.txt"])
Skipped with a note while islandc.py or build_island() doesn't exist yet.
"""
import os
import sys
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "engine", "tools"))

from assetkit import build  # noqa: E402

ISLANDC = os.path.join(ROOT, "tools", "islandc", "islandc.py")
ISLAND_SRC = os.path.join(ROOT, "island", "island.txt")

_kit_find = build._find
_kit_run_gen = build.run_gen
_islandc = []          # cached adapter (one import per run)


def _find(root):
    gens = _kit_find(root)
    src = os.path.join(root, "island", "island.txt")
    if os.path.exists(src):
        data = os.path.join(root, "data")
        gens.append(build.Gen("island", "track", os.path.join(root, "tools", "islandc", "islandc.py"), data,
                              os.path.join(data, "island"), src))
    return gens


class _IslandAdapter(object):
    """Looks like a trackc module to assetkit.build.run_gen (build_track -> islandc.build_island)."""

    def __init__(self, mod):
        self.mod = mod
        if hasattr(mod, "build_island"):
            self.build_track = mod.build_island


def _island_adapter(root):
    if _islandc:
        return _islandc[0]
    path = os.path.join(root, "tools", "islandc", "islandc.py")
    ad = None
    if os.path.exists(path):
        d = os.path.dirname(path)
        if d not in sys.path:
            sys.path.insert(0, d)
        try:
            ad = _IslandAdapter(build._load_module(path, "islandc"))
        except (Exception, SystemExit):
            sys.modules.pop("islandc", None)
            ad = build.ImportFailure(traceback.format_exc())
    _islandc.append(ad)
    return ad


def _run_gen(gen, root, check=False, previews=True, verbose=False, keep_temp=False, trackc=None):
    if gen.kind == "track" and gen.name == "island":
        trackc = _island_adapter(root)
    return _kit_run_gen(gen, root, check, previews, verbose, keep_temp, trackc)


build._find = _find
build.run_gen = _run_gen

if __name__ == "__main__":
    sys.exit(build.main(sys.argv[1:], root=ROOT))
