"""Shared style: named colours (muted Finnish summer evening, DESIGN 8.2), light defaults, colour helpers.

Colours are (r, g, b) tuples of ints 0..255. Helpers accept tuples of 3 or 4 values and return ints.

    from assetkit import palette as P
    P.C["punamulta"]                    # (140, 40, 30)
    P.shade(P.C["asphalt"], 1.2)        # brighter
    P.lerp(P.C["spruce"], P.C["pine"], 0.5)
    P.argb(P.C["trim_white"])           # 0xFFE1DCCD (vertex colour u32)
    P.q565(P.C["field"])                # what the colour looks like in RGB565
"""

import math

KEY = (255, 0, 255)
"""Transparent colour key (CLAUDE.md 8). Never use it (or anything close) as a real colour."""

# Named colours. Muted, slightly warm; no saturated primaries except signs.
C = {
    # light set (fi_summer evening, P: the sky task A5 may refine these in data/sky/<set>.txt)
    "sun": (255, 196, 140),
    "ambient": (110, 125, 160),
    "fog": (205, 170, 160),
    "horizon": (205, 170, 160),
    "zenith": (92, 120, 170),
    # road and ground
    "asphalt": (52, 52, 56),
    "asphalt_worn": (64, 63, 66),
    "line_white": (215, 212, 200),
    "line_yellow": (215, 175, 50),
    "gravel": (128, 118, 102),
    "dirt": (110, 90, 66),
    "sand_shore": (180, 165, 130),
    "grass": (96, 120, 52),
    "grass_dry": (140, 140, 70),
    "field": (170, 175, 80),
    "rye": (190, 172, 96),
    "forest_floor": (70, 70, 45),
    "moss": (85, 100, 50),
    "rock_granite": (120, 115, 112),
    "rock_dark": (80, 78, 80),
    "lake": (60, 90, 120),
    "lake_deep": (40, 62, 90),
    # vegetation
    "spruce": (30, 55, 35),
    "pine": (55, 80, 45),
    "pine_bark": (130, 80, 55),
    "birch_bark": (225, 222, 210),
    "birch_mark": (40, 38, 36),
    "birch_leaf": (110, 140, 60),
    "bush": (70, 95, 45),
    # buildings
    "punamulta": (140, 40, 30),
    "punamulta_dark": (100, 30, 24),
    "trim_white": (225, 220, 205),
    "wall_yellow": (205, 170, 90),
    "roof_tin": (110, 112, 115),
    "roof_tile": (150, 60, 40),
    "window": (40, 50, 60),
    "wood_grey": (130, 120, 105),
    "wood_dark": (70, 55, 40),
    "concrete": (150, 148, 140),
    "bale_white": (230, 230, 225),
    # signs (the only saturated colours)
    "sign_yellow": (240, 190, 20),
    "sign_red": (200, 30, 30),
    "sign_black": (20, 20, 20),
    "sign_white": (240, 240, 235),
    "sign_blue": (20, 70, 160),
    # car glass
    "glass": (40, 50, 62),
}

# ---- 1.2 additions (T02c): the M2/M3 art tasks' proposed colours, merged as NEW names only (the values above
# are unchanged, so existing outputs stay identical). Source task in brackets; where two tasks proposed the same
# name with different values, the second got a more specific name (noted).
C.update({
    # vegetation [A3 gen_vegetation VC]
    "spruce_shadow": (18, 36, 25),
    "spruce_lit": (50, 82, 54),
    "spruce_tip": (96, 110, 62),        # sunlit young shoots on top
    "pine_lit": (78, 104, 54),
    "pine_tip": (142, 134, 72),         # warm evening light on the crown tops
    "pine_bark_grey": (98, 84, 74),     # lower trunk
    "pine_bark_orange": (196, 118, 68),  # upper trunk ("pine_bark" stays the default)
    "birch_lit": (132, 160, 70),
    "birch_tip": (178, 182, 100),
    "juniper": (40, 64, 50),
    "forest_dark": (16, 26, 18),        # gaps deep inside a forest edge
    # road and ground [A2 gen_ground XC]
    "asphalt_dust": (96, 90, 80),       # gravel dust on the asphalt edge band
    "asphalt_new": (44, 44, 47),        # fresh asphalt (bridge deck, repair strips)
    "bitumen": (30, 30, 32),            # crack sealing
    "aggregate": (112, 108, 104),       # exposed granite chips in worn asphalt
    "kerb_red": (178, 46, 38),          # racing kerb red (muted sign_red)
    "kerb_white": (222, 219, 208),
    "granite_pink": (150, 120, 110),    # feldspar in Finnish granite (A2; A4's greyer one is "granite_boulder")
    "lichen": (150, 156, 122),          # reindeer / rock lichen (A2; A4's paler one is "lichen_pale")
    "blueberry": (48, 74, 36),          # blueberry shrub
    "needles": (92, 76, 54),            # needle litter
    "mud": (66, 56, 42),                # damp ditch soil
    "flower_white": (205, 205, 185),
    "flower_yellow": (200, 175, 60),
    "flower_purple": (120, 90, 130),
    "water_sky": (150, 140, 150),       # evening sky glints on the lake
    # buildings and props [A4 gen_buildings PAL]
    "church_cream": (236, 228, 202),    # pale ochre-white church boards
    "sauna_brown": (96, 66, 44),        # tarred/brown sauna boards
    "plinth_grey": (132, 128, 120),     # concrete / fieldstone plinth
    "granite_boulder": (134, 129, 128),  # grey granite with a faint red cast (A4's "granite_pink")
    "lichen_pale": (168, 172, 150),     # pale grey-green lichen on boulders (A4's "lichen")
    "galvanised": (170, 174, 176),      # sign posts, guard rail
    "straw": (198, 172, 98),            # hay / straw bales
    "pole_brown": (86, 64, 46),         # creosoted power pole
    "door_brown": (104, 70, 44),        # painted farmhouse door
    "glass_top": (58, 70, 96),          # window glass reflecting the zenith
    "glass_low": (150, 126, 128),       # ... and the warm horizon
    "reflector": (245, 160, 40),        # amber edge-post reflector
    "roof_dark": (64, 64, 70),          # church roof (tint of roof_tin)
    "brass": (196, 160, 84),            # church cross
    # the fi_evening light set as A5 built it (data/sky/fi_evening.txt is authoritative; the "fog", "horizon",
    # "zenith" defaults above stay as they were, so older pre-lit outputs don't change) [A5 gen_sky]
    "fi_evening_zenith": (84, 113, 168),
    "fi_evening_top": (100, 129, 180),     # top row of the sky tiles
    "fi_evening_horizon": (225, 185, 162),  # clear sky at +2 deg
    "fi_evening_fog": (203, 174, 153),     # = average of the horizon band (fogged terrain blends into the sky)
    "fi_evening_skyfill": (122, 136, 176),
})

LIGHT_SETS = {
    "fi_evening": {                      # A5's data/sky/fi_evening.txt (copy for tools; the txt is authoritative)
        "zenith": (84, 113, 168), "top": (100, 129, 180), "horizon": (225, 185, 162), "fog": (203, 174, 153),
        "sun": (255, 196, 140), "ambient": (110, 125, 160), "skyfill": (122, 136, 176), "hemi": 0.35,
        "sun_azim": -45.0, "sun_elev": 6.0, "fogStart": 440.0, "fogEnd": 800.0,
        "vHorizon": 0.8125, "elevLo": -6.0, "elevHi": 28.0,
    },
}
"""Named light sets: P.LIGHT_SETS["fi_evening"]["fog"]. sunDir = dir_from_angles(sun_azim, sun_elev)."""

UI = {
    # HUD / frontend colours [A6 gen_ui UI]. Separate dict: the names are generic ("red", "edge", "white").
    "ink": (10, 11, 16),            # glyph/panel outlines
    "drop": (0, 0, 0),              # drop shadow
    "fill_top": (255, 255, 255),    # glyph fill gradient (tint multiplies it)
    "fill_bot": (218, 220, 224),
    "panel": (17, 23, 35),          # panel glass (drawn with vertex alpha ~0xB0)
    "panel_hi": (46, 58, 80),       # panel top highlight rows
    "edge": (192, 200, 214),        # panel edge / frame light
    "edge_mid": (118, 126, 140),
    "accent": (242, 138, 38),       # evening-sun orange
    "accent_lo": (206, 84, 26),
    "red": (222, 40, 30),           # dial red zone
    "face": (30, 34, 44),           # dial face centre
    "face_rim": (12, 14, 20),       # dial face edge
    "chrome_hi": (236, 238, 242),
    "chrome_lo": (70, 76, 88),
    "tick": (238, 236, 228),
    "lake": (52, 100, 158),         # Finnish lake blue (logo swoosh)
    "lake_hi": (150, 196, 236),
    "logo_top": (255, 230, 120),    # logo letter gradient: low evening sun
    "logo_mid": (248, 150, 42),
    "logo_bot": (186, 42, 30),
    "logo_ink": (20, 18, 40),
    "logo_side": (110, 30, 26),     # logo extrusion
    "key_cap": (150, 158, 176),     # menu key cap rim light
    "white": (255, 255, 255),
}
"""HUD/frontend colours (A6): P.UI["accent"]."""

# Light defaults (match C above). Directions are unit vectors pointing FROM the surface TOWARD the light.
SUN_ELEV_DEG = 6.0
SUN_AZIM_DEG = -45.0        # compass-style yaw: 0 = north (+Z), positive toward east (+X); -45 = north-west


def dir_from_angles(azim_deg, elev_deg):
    """Unit vector toward a light at compass yaw azim (0 = +Z north, 90 = +X east) and elevation."""
    a = math.radians(azim_deg)
    e = math.radians(elev_deg)
    return (math.sin(a) * math.cos(e), math.sin(e), math.cos(a) * math.cos(e))


SUN_DIR = dir_from_angles(SUN_AZIM_DEG, SUN_ELEV_DEG)
"""Toward the low evening sun in the north-west (DESIGN 4.12)."""

KEY_DIR = dir_from_angles(-145.0, 50.0)
"""Object 'studio' key light used by mesh.prelight(mode='object'): from front-left-above in mesh space
(mesh front = -Z, so the light comes from -Z/-X/+Y). Gives readable face contrast independent of instance yaw."""

FOG_START_FRAC = 0.40       # Low preset; High 0.55 (DESIGN 3.7)


# ---------------------------------------------------------------------------------------------------- helpers

def clamp8(v):
    v = int(round(v))
    return 0 if v < 0 else (255 if v > 255 else v)


def rgb(c):
    """Normalise any (r, g, b[, a]) of ints/floats to an (r, g, b) int tuple."""
    return (clamp8(c[0]), clamp8(c[1]), clamp8(c[2]))


def lerp(a, b, t):
    """Linear mix of two colours, t in [0, 1]."""
    return (clamp8(a[0] + (b[0] - a[0]) * t), clamp8(a[1] + (b[1] - a[1]) * t), clamp8(a[2] + (b[2] - a[2]) * t))


def shade(c, f):
    """Multiply brightness by f (0.8 = 20% darker)."""
    return (clamp8(c[0] * f), clamp8(c[1] * f), clamp8(c[2] * f))


def mul(a, b):
    """Per-channel multiply of two colours (b acts as a 0..255 filter)."""
    return (clamp8(a[0] * b[0] / 255.0), clamp8(a[1] * b[1] / 255.0), clamp8(a[2] * b[2] / 255.0))


def add(a, b):
    return (clamp8(a[0] + b[0]), clamp8(a[1] + b[1]), clamp8(a[2] + b[2]))


def luma(c):
    """Perceived brightness 0..255 (Rec. 601)."""
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]


def desaturate(c, amount):
    """amount 0 = unchanged, 1 = grey of the same luma."""
    y = luma(c)
    return lerp(c, (y, y, y), amount)


def warm(c, amount):
    """Shift toward the evening sun colour (amount 0..1, multiplicative)."""
    return lerp(c, mul(c, C["sun"]), amount)


def jitter(c, rng, amount):
    """Random brightness jitter: rng = assetkit.rng.Rng, amount e.g. 0.08 = +-8%."""
    return shade(c, 1.0 + (rng.random() * 2.0 - 1.0) * amount)


def argb(c, a=255):
    """Pack to a D3D DIFFUSE u32 0xAARRGGBB (the RVert colour field)."""
    r, g, b = rgb(c)
    if len(c) > 3:
        a = clamp8(c[3])
    return ((a & 255) << 24) | (r << 16) | (g << 8) | b


def from_argb(v):
    """u32 0xAARRGGBB -> (r, g, b, a)."""
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255, (v >> 24) & 255)


def hexstr(c):
    return "#%02x%02x%02x" % rgb(c)


def _q(v, bits):
    m = (1 << bits) - 1
    q = (clamp8(v) * m + 127) // 255
    return (q * 255 + m // 2) // m


def q565(c):
    """Colour as it survives RGB565 (opaque textures)."""
    return (_q(c[0], 5), _q(c[1], 6), _q(c[2], 5))


def q555(c):
    """RGB555 / ARGB1555 colour part (keyed textures)."""
    return (_q(c[0], 5), _q(c[1], 5), _q(c[2], 5))


def q4444(c):
    """ARGB4444 colour part (smooth alpha textures)."""
    return (_q(c[0], 4), _q(c[1], 4), _q(c[2], 4))


def is_near_key(c, tol=40):
    """True for colours close to the magenta key (would read as key-tinted)."""
    return min(c[0], c[2]) - c[1] > tol and c[0] > 128 and c[2] > 128
