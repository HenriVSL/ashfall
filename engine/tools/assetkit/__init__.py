"""assetkit: the shared, deterministic Mac-side asset toolkit of the reusable engine.

Python 3 + Pillow only. Modules:
    rng       deterministic seeding (Rng(seed, *names)), value noise, fBm, ridged noise, hashes
    palette   shared colours (+ UI, LIGHT_SETS), light defaults, colour helpers, 16-bit quantise
    mesh      Geo (triangle soup with attributes), primitives, facade walls, MeshBuilder (materials, LODs, tags)
    image     BMP 24-bit/8-bit writer + reader, key threshold, atlas packer, mips, noise, 16-bit preview
    export    the only writers/readers of engine formats: .msh, .trk container, VPOL/BTCH/INDX pools
    validate  format + budget checks; CLI: python3 -m assetkit.validate <files>
    preview   software rasteriser to PNG: views(), turntable(), lods(), scene(), contact_sheet(), label()
    bake      bake 3D features into an atlas: per-texel position/normal/region, dilation, edge distance
    build     generator discovery + runner (python3 tools/build_assets.py)

Read README.md in this folder for the conventions and the generator template.
"""

__version__ = "1.2"

import os as _os

KIT_DIR = _os.path.dirname(_os.path.abspath(__file__))
"""Absolute path of this package folder."""


def project_root():
    """Project root = the folder that contains engine/ (two levels above engine/tools/assetkit).
    Override with the environment variable ASSETKIT_ROOT."""
    env = _os.environ.get("ASSETKIT_ROOT")
    if env:
        return _os.path.abspath(env)
    return _os.path.dirname(_os.path.dirname(_os.path.dirname(KIT_DIR)))
