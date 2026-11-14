/* scene.cpp - frame composition (DESIGN 9.4): fog, terrain, base, aircraft, sea, late sky, clouds, dots, presets.
   See scene.h. Windows/render side. Owner: T08 (T09: preset values in s_presets, T17: aircraft list, T22: polish).
   Rules followed (HANDOFF T02/T04/A1/A2/A7): fog colour through terrain_fog_565fix for r_set_fog, the terrain
   config, World.sky.fog and every World.skyBot[]; fog end = fogEndFrac (0.9) x far; terrain_gpu_init after r_init;
   one sea_draw per frame before the late sky; the light set is data/sky/light.txt (world_sky_load "light"); jet
   LOD0 has no moving parts (drawn at their tags: world = rotX(angle) * translate(tag) * body, + = trailing edge
   up), LOD1/2 are self-contained; enemy livery = texture jet_aggr on the jet and the four surfaces (nozzle, gear
   and flame share jet_parts / flame). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "scene.h"
#include "params.h"
#include "e_log.h"
#include "e_prof.h"
#include "e_plat.h"
#include "e_rend.h"
#include "e_tex.h"
#include "e_image.h"
#include "e_world.h"
#include "e_terrain.h"
#include "e_ui.h"
#include "e_file.h"
#include "hud_font.h"
#include "world.h"
#include "weapons.h"
#include "missile.h"
#include "scatter.h"                                /* A11: island vegetation sprites */

/* ---------------------------------------------------------------- presets (DESIGN 9.3; T09 tunes) */
static const ScenePreset s_presets[2] = {
    /* name   far      fogStart fogEnd lodScale              sky clouds deck glow texKB  fps acLod */
    /* T10b: x2 for the 3x world of T03b (island 34 km): Low 38 km / fog 7.6-34.2 km, High 56 km / 11.2-50.4 km at
       sea level; both grow with the camera altitude ([scene] hazeScaleM, farMaxM) */
    { "low",  38000.0f, 7600.0f, 0.9f, TERRAIN_LODSCALE_LOW,  4, 16,    0,   0,   6144,  30, 1.0f },
    { "high", 56000.0f, 11200.0f, 0.9f, TERRAIN_LODSCALE_HIGH, 8, 48,    1,   1,   12288, 60, 1.5f },
};
static ScenePreset s_base;                          /* the chosen preset (scene_preset_init) */
static ScenePreset s_eff;                           /* + [scene] overrides (scene_preset) */

static float s_pFar, s_pFogStart, s_pFogEndFrac, s_pLodScale, s_pAcLod;
static int   s_pClouds, s_pDeck, s_pGlow, s_pSkyTiles, s_pProps;
static float s_acMeshDist, s_acDotFrom, s_acDotDist, s_dotAlpha, s_baseDist, s_baseLodDist, s_cloudFade;
static float s_hazeH, s_farMax, s_baseAltK;         /* T10b: altitude-aware view distances */
static int   s_skyFirst;
static const Param s_sceneTab[] = {
    { "scene", "farM", P_FLOAT, &s_pFar, "0", 0, 64000, "far plane at sea level (m); 0 = preset (Low 38000, High 56000)" },
    { "scene", "fogStartM", P_FLOAT, &s_pFogStart, "0", 0, 100000, "fog start at sea level (m); 0 = preset (Low 7600, High 11200)" },
    { "scene", "fogEndFrac", P_FLOAT, &s_pFogEndFrac, "0", 0, 1, "fog end as a fraction of the far plane; 0 = preset 0.9 (T02)" },
    { "scene", "lodScale", P_FLOAT, &s_pLodScale, "0", 0, 4, "terrain LOD scale, larger = finer; 0 = preset (Low 0.85, High 1.5)" },
    { "scene", "acLodScale", P_FLOAT, &s_pAcLod, "0", 0, 4, "jet mesh LOD distances x this; 0 = preset" },
    { "scene", "clouds", P_INT, &s_pClouds, "-1", -1, 64, "cumulus puffs; -1 = preset (Low 16, High 48)" },
    { "scene", "cloudDeck", P_INT, &s_pDeck, "-1", -1, 1, "6 km cloud deck; -1 = preset (High only)" },
    { "scene", "sunGlow", P_INT, &s_pGlow, "-1", -1, 1, "sun glow billboard; -1 = preset (High only)" },
    { "scene", "skyTiles", P_INT, &s_pSkyTiles, "-1", -1, 8, "sky panorama tiles 4 or 8; -1 = preset", P_RESTART },
    { "scene", "props", P_BOOL, &s_pProps, "1", 0, 1, "M1 scenery: 2 parked + 1 circling aggressor jet (render only)" },
    { "scene", "acMeshDist", P_FLOAT, &s_acMeshDist, "2500", 300, 10000, "aircraft meshes drawn within this range (m)" },
    { "scene", "acDotFrom", P_FLOAT, &s_acDotFrom, "1500", 0, 10000, "2x2 aircraft dot drawn from this range (m), under the mesh" },
    { "scene", "acDotDist", P_FLOAT, &s_acDotDist, "4000", 0, 20000, "aircraft dot faded out at this range (m)" },
    { "scene", "dotAlpha", P_FLOAT, &s_dotAlpha, "0.7", 0, 1, "aircraft dot opacity" },
    { "scene", "baseDist", P_FLOAT, &s_baseDist, "6000", 500, 20000, "airbase (runway, buildings) drawn within (m)" },
    { "scene", "baseLodDist", P_FLOAT, &s_baseLodDist, "1500", 0, 20000, "airbase cells: detailed LOD (runway marks) within (m)" },
    { "scene", "cloudFadeM", P_FLOAT, &s_cloudFade, "300", 0, 3000, "cloud puffs fade out within this distance of the camera (m)" },
    /* T10b: view distances that follow the camera altitude (the "ball" fix, HANDOFF T10b) */
    { "scene", "hazeScaleM", P_FLOAT, &s_hazeH, "8000", 0, 50000, "altitude haze: far plane and fog distances x h/(H(1-exp(-h/H))) at camera altitude h (thinner air up high; x1.2 at 3 km, x1.6 at 8 km, x2.2 at 15 km); this is H (m), 0 = fixed sea-level distances" },
    { "scene", "farMaxM", P_FLOAT, &s_farMax, "62000", 10000, 64000, "cap of the altitude-scaled far plane (m), fog scales with it. Hard limit 64000: the Voodoo3 fog table / W wrap for depths beyond ~65.5 km (unfogged dark band above the horizon), terrain chunks overhang the far plane by up to ~1.8 km" },
    { "scene", "skyFirst", P_BOOL, &s_skyFirst, "1", 0, 1, "1 = sky panorama drawn first without Z, geometry always covers it; 0 = late sky (saves fill, but its dome overdraws the ground seen from altitude: the 'ball')" },
    { "scene", "baseAltK", P_FLOAT, &s_baseAltK, "1.0", 0, 4, "airbase draw distance grows by this x camera altitude (seen from high up)" },
};
/* T22: fast aircraft path, jet shadows */
static int   s_acFast, s_shadowOn;
static float s_shadowAgl, s_shadowAlpha, s_shadowSeaK, s_shadowSunK, s_shadowDist, s_shadowLift, s_wreckLod;
static float s_lodMul = 1.0f;                       /* draw_jet LOD distance factor (wrecks: s_wreckLod) */
static const Param s_t22Tab[] = {
    { "scene", "acFast", P_BOOL, &s_acFast, "1", 0, 1, "T22: jets drawn with own CPU lighting of only the triangles that face the camera, the jet + moving parts + rail missiles batched into one draw per material (same look, about half the vertices and triangles); 0 = engine mesh_draw_lit per part" },
    { "scene", "shadow", P_BOOL, &s_shadowOn, "1", 0, 1, "T22: jet shadow (planform from the jet mesh) on the ground, runway and sea when low" },
    { "scene", "shadowAglM", P_FLOAT, &s_shadowAgl, "300", 0, 3000, "shadow fades out by this height above the surface (m)" },
    { "scene", "shadowAlpha", P_FLOAT, &s_shadowAlpha, "0.6", 0, 1, "shadow darkness on land and runway" },
    { "scene", "shadowSeaK", P_FLOAT, &s_shadowSeaK, "0.45", 0, 1, "shadow darkness factor on the sea" },
    { "scene", "shadowSunK", P_FLOAT, &s_shadowSunK, "1", 0, 1, "1 = shadow cast along the sun (true offset: ~1.2 x height at the 40 deg sun), 0 = straight down" },
    { "scene", "shadowDistM", P_FLOAT, &s_shadowDist, "2500", 100, 10000, "shadows drawn within this range of the camera (m)" },
    { "scene", "wreckLodScale", P_FLOAT, &s_wreckLod, "0.3", 0.05f, 2, "T22: falling wrecks use the jet LOD distances x this (tumbling + burning: LOD1 from ~90 m, LOD2 from ~270 m)" },
    { "scene", "shadowLiftM", P_FLOAT, &s_shadowLift, "0.5", 0, 5, "shadow drawn this far above the surface (+ zBias 2; runway ribbons are 0.2 m up)" },
};
void scene_params(void) { params_add(s_sceneTab, E_COUNT(s_sceneTab)); params_add(s_t22Tab, E_COUNT(s_t22Tab)); }

void scene_preset_init(const char *name)
{
    s_base = s_presets[(name && !e_stricmp(name, "high")) ? 1 : 0];
    log_printf(LOG_INFO, "scene: preset name=%s far=%.0f fog=%.0f..%.0f lodScale=%.2f sky=%d clouds=%d deck=%d glow=%d texkb=%d fps=%d",
               s_base.name, (double)s_base.farM, (double)s_base.fogStartM, (double)(s_base.fogEndFrac * s_base.farM),
               (double)s_base.lodScale, s_base.skyTiles, s_base.clouds, s_base.cloudDeck, s_base.sunGlow, s_base.texBudgetKB,
               s_base.fpsTarget);
}

const ScenePreset *scene_preset(void)
{
    if (!s_base.name[0]) s_base = s_presets[0];
    s_eff = s_base;
    if (s_pFar > 0.0f) s_eff.farM = s_pFar;
    if (s_pFogStart > 0.0f) s_eff.fogStartM = s_pFogStart;
    if (s_pFogEndFrac > 0.0f) s_eff.fogEndFrac = s_pFogEndFrac;
    if (s_pLodScale > 0.0f) s_eff.lodScale = s_pLodScale;
    if (s_pAcLod > 0.0f) s_eff.acLodScale = s_pAcLod;
    if (s_pClouds >= 0) s_eff.clouds = s_pClouds;
    if (s_pDeck >= 0) s_eff.cloudDeck = s_pDeck;
    if (s_pGlow >= 0) s_eff.sunGlow = s_pGlow;
    if (s_pSkyTiles > 0) s_eff.skyTiles = s_pSkyTiles <= 4 ? 4 : 8;
    if (s_eff.fogStartM > 0.9f * s_eff.fogEndFrac * s_eff.farM) s_eff.fogStartM = 0.5f * s_eff.fogEndFrac * s_eff.farM;
    return &s_eff;
}

float scene_far_m(void) { return scene_preset()->farM; }

/* T10b: no geometry may reach a view depth of ~65.5 km: the 86Box Voodoo3 (3dfx DX6 driver) fog table and W values
   wrap there (measured: far 80 km from 15 km altitude shows an unfogged dark sea band above the horizon, 62 km is
   clean). The renderer only rejects triangles wholly beyond far, so far + the largest overhang (a terrain chunk,
   ~1.8 km) must stay below that. */
#define SCENE_FAR_LIMIT 64000.0f

/* T10b: haze factor for camera altitude h: optical depth of an exponential atmosphere (scale height H) along a slant
   path down to the sea, relative to the same path length at sea level, inverted: h / (H (1 - exp(-h/H))). 1 at the
   sea, grows ~linearly up high. The far plane and the fog scale with it, so from 15 km the ground is seen through
   ~25 % haze instead of 90 %, and the far edge of the world stays fully fogged into the sky. */
float scene_alt_factor(float eyeY)
{
    float h = eyeY > 0.0f ? eyeY : 0.0f, x;
    if (s_hazeH <= 1.0f || h < 1.0f) return 1.0f;
    x = h / s_hazeH;
    if (x < 1e-3f) return 1.0f + 0.5f * x;
    return x / (1.0f - (float)exp(-x));
}

void scene_view_dist(float eyeY, float *farM, float *fogStart, float *fogEnd)
{
    const ScenePreset *p = scene_preset();
    float k = scene_alt_factor(eyeY), f = p->farM * k;
    float cap = s_farMax > p->farM ? s_farMax : p->farM;
    if (cap > SCENE_FAR_LIMIT) cap = SCENE_FAR_LIMIT;
    if (f > cap) { f = cap; k = cap / p->farM; }
    if (k < 1.0f) k = 1.0f;
    if (farM) *farM = f;
    if (fogStart) *fogStart = p->fogStartM * k;
    if (fogEnd) *fogEnd = p->fogEndFrac * f;
}

float scene_far_at(float eyeY) { float f; scene_view_dist(eyeY, &f, NULL, NULL); return f; }

/* largest eye distance of a point inside the view frustum (the far plane's corners): radial culls (terrain groups,
   sea ring, clouds) must reach this far, the planar far plane does the rest. Spheres of radius farM leave a circle
   (the "ball") in the screen corners and from altitude. */
float scene_far_radial(const Camera *c)
{
    float tx = c->proj.m[0][0] > 1e-4f ? 1.0f / c->proj.m[0][0] : 1.0f;
    float ty = c->proj.m[1][1] > 1e-4f ? 1.0f / c->proj.m[1][1] : 1.0f;
    return c->farM * (float)sqrt(1.0f + tx * tx + ty * ty);
}

/* ---------------------------------------------------------------- state */
static int   s_ok;
static World s_sky, s_baseW;
static int   s_skyOk, s_baseOk;
static u32   s_fog = 0xC4D4E4;
static Vec3  s_sunDir;
static u32   s_sunRGB = 0xFFFFF4E2, s_ambRGB = 0xFF8496B2;
static const Terrain *s_ter;
static Mesh  s_jet, s_stab[2], s_flap[2], s_nozzle, s_gear;
static int   s_jetOk, s_partsOk;
static RTex  s_texAggr;
static Mesh  s_msl[2];                              /* T17: lrm.msh, srm.msh (A8; origin = rail point, nose +Z) */
static int   s_mslOk[2];
static const MeshTag *s_tStab[2], *s_tFlap[2], *s_tNozzle, *s_tGear[3];
static int   s_secTerrain, s_secSea, s_secBase, s_secAc, s_secSky, s_secClouds;
static SceneStats s_st;
static int   s_glowOn = -1;
static void  shadow_init(void);                     /* T22 (jet shadows, below) */
static void  t22_free(void);
static float s_cloudNear;                           /* T22: camera inside / near a cloud puff, 0..1 (last frame) */

/* clouds */
enum { CLOUD_MAX = 64 };
typedef struct Puff { Vec3 c; float size; int cell; float d2; } Puff;
static Puff  s_puff[CLOUD_MAX];
static int   s_order[CLOUD_MAX];
static RTex  s_texPuffs, s_texDeck;
static RMaterial s_matPuff, s_matDeck, s_matDot;
static RVert1 s_cv[CLOUD_MAX * 4 + 16];
static u16   s_ci[CLOUD_MAX * 6 + 24];

/* props */
enum { PROP_MAX = 3 };
static int   s_nProps;
static Vec3  s_propPos[2]; static float s_propHdg[2];
static float s_circR = 20000.0f, s_circAlt = 1500.0f, s_circV = 200.0f, s_circCx, s_circCz;   /* T10b: 3x world */

const Mesh *scene_jet_mesh(void) { return s_jetOk ? &s_jet : NULL; }
const SceneStats *scene_stats(void) { return &s_st; }
Vec3 scene_sun_dir(void) { return s_sunDir; }

static float ms_since(s64 t0) { return (float)((double)(plat_qpc() - t0) * 1000.0 / (double)plat_qpf()); }

static u32 lcg(u32 *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }
static float lcg01(u32 *s) { return (float)(lcg(s) & 0xFFFF) / 65536.0f; }
static float sm01(float a, float b, float x)
{                                                   /* T22: smoothstep */
    float t = b > a ? E_CLAMP((x - a) / (b - a), 0.0f, 1.0f) : (x >= b ? 1.0f : 0.0f);
    return t * t * (3.0f - 2.0f * t);
}


/* ---------------------------------------------------------------- props (render only) */
int scene_prop_count(void) { return s_pProps ? s_nProps + 1 : 0; }

int scene_prop_pose(int i, float t, Vec3 *pos, Quat *q, float *gearPos)
{
    if (!s_pProps) return 0;
    if (i < s_nProps) {
        *pos = s_propPos[i];
        *q = flight_path_quat(s_propHdg[i], 0.0f, 0.0f);
        *gearPos = 1.0f;
        return 1;
    }
    if (i == s_nProps) {                            /* circling the island clockwise at s_circAlt */
        float a = t * s_circV / s_circR, hdg = (float)atan2(cos(a), -sin(a));
        float bank = (float)atan(s_circV * s_circV / (GRAV * s_circR));
        *pos = v3(s_circCx + s_circR * (float)sin(a), s_circAlt, s_circCz + s_circR * (float)cos(a));
        *q = q_mul(flight_path_quat(hdg, 0.0f, bank), q_axis_angle(v3(1.0f, 0.0f, 0.0f), -E_DEG2RAD(3.0f)));
        *gearPos = 0.0f;
        return 1;
    }
    return 0;
}

static void props_init(const Terrain *t)
{
    const TerrainBase *b = terrain_base(t);
    int i;
    float hr;
    s_nProps = 0;
    s_circCx = t ? t->centreX : 0.0f;
    s_circCz = t ? t->centreZ : 0.0f;
    if (!b) return;
    hr = E_DEG2RAD(b->rwyHeadingDeg);
    for (i = 0; i < b->nBoxes && s_nProps < 2; i++) {
        const Aabb *bx = &b->box[i];
        Vec3 c = v3(0.5f * (bx->lo.x + bx->hi.x), 0.0f, 0.5f * (bx->lo.z + bx->hi.z)), d;
        float sx = bx->hi.x - bx->lo.x, sz = bx->hi.z - bx->lo.z, h = bx->hi.y - bx->lo.y, along, half, gy;
        if (!((sx > 35.0f && sz > 25.0f) || (sz > 35.0f && sx > 25.0f)) || h < 10.0f || h > 20.0f) continue;   /* hangars */
        /* towards the runway centre line (nearest point), parked 30 m in front of the door, nose out */
        along = (c.x - b->rwyX) * (float)sin(hr) + (c.z - b->rwyZ) * (float)cos(hr);
        d = v3(b->rwyX + along * (float)sin(hr) - c.x, 0.0f, b->rwyZ + along * (float)cos(hr) - c.z);
        if (v3_len(d) < 1.0f) continue;
        d = v3_norm(d);
        half = (float)fabs(d.x) > (float)fabs(d.z) ? 0.5f * sx : 0.5f * sz;
        c = v3_madd(c, d, half + 30.0f);
        gy = terrain_surface(t, c.x, c.z);
        if (gy < b->rwyHeight - 1.0f) gy = b->rwyHeight;
        s_propPos[s_nProps] = v3(c.x, gy + 0.2f + 2.047f, c.z);   /* apron ribbon 0.2 m, gear down CG 2.047 m */
        s_propHdg[s_nProps] = (float)atan2(d.x, d.z);
        log_printf(LOG_INFO, "scene: prop parked=%d pos=%.0f,%.1f,%.0f hdg=%.0f", s_nProps, (double)c.x,
                   (double)s_propPos[s_nProps].y, (double)c.z, (double)E_RAD2DEG(s_propHdg[s_nProps]));
        s_nProps++;
    }
}

/* ---------------------------------------------------------------- init */
static void clouds_init(void)
{
    u32 seed = 0xC10D5u;
    int i;
    Vec3 cc = v3(0.0f, 0.0f, 0.0f);
    for (i = 0; i < CLOUD_MAX; i++) {               /* deterministic: clusters of 4 puffs 8-35 km from the island centre (T10b: 3x world) */
        Puff *p = &s_puff[i];
        float a, r;
        if ((i & 3) == 0) {                         /* new cluster centre (Low's 16 = 4 whole clusters) */
            a = lcg01(&seed) * 6.2832f;
            r = 8000.0f + 27000.0f * (float)sqrt(lcg01(&seed));
            cc = v3(s_circCx + r * (float)sin(a), 2000.0f + 1600.0f * lcg01(&seed), s_circCz + r * (float)cos(a));
        }
        a = lcg01(&seed) * 6.2832f;
        r = (i & 3) ? 500.0f + 900.0f * lcg01(&seed) : 0.0f;
        p->c = v3(cc.x + r * (float)sin(a), cc.y + (lcg01(&seed) - 0.5f) * 400.0f, cc.z + r * (float)cos(a));
        p->size = 450.0f + 650.0f * lcg01(&seed);
        p->cell = (int)(lcg01(&seed) * 3.999f);
    }
    s_texPuffs = tex_get("cloud_puffs", IMG_ALPHA, RT_CLAMP);
    s_texDeck = 0;
    memset(&s_matPuff, 0, sizeof(s_matPuff));
    s_matPuff.tex0 = s_texPuffs;
    s_matPuff.blend = RB_BLEND;
    s_matPuff.cullNone = 1;
    s_matPuff.zTest = 1;
    s_matPuff.zWrite = 0;
    s_matPuff.fog = 1;
    s_matPuff.clampUV = 1;
    s_matDeck = s_matPuff;
    s_matDeck.clampUV = 0;
}

static int load_mesh(Mesh *m, const char *name)
{
    int ok = mesh_load(m, name, 0);
    if (!ok) log_printf(LOG_WARN, "warn: scene mesh missing name=%s", name);
    return ok;
}

int scene_init(const Game *g)
{
    const ScenePreset *p = scene_preset();
    TerrainDrawCfg *tc = terrain_draw_cfg();
    s64 t0 = plat_qpc();
    int i, tiles;
    s_ter = g ? g->terrain : NULL;
    memset(&s_st, 0, sizeof(s_st));
    s_secTerrain = prof_section("terrain");
    s_secSea = prof_section("sea");
    s_secBase = prof_section("base");
    s_secAc = prof_section("aircraft");
    s_secSky = prof_section("sky");
    s_secClouds = prof_section("clouds");
    if (s_ter) terrain_gpu_init((Terrain *)s_ter);
    /* sky + light set */
    memset(&s_sky, 0, sizeof(s_sky));
    tiles = world_sky_load(&s_sky, p->skyTiles, "light");
    s_skyOk = tiles > 0;
    world_set_sky_late(1);
    if (s_sky.sky.fog) s_fog = s_sky.sky.fog & 0xFFFFFF;
    s_fog = terrain_fog_565fix(s_fog);
    s_sky.sky.fog = (s_sky.sky.fog & 0xFF000000u) | s_fog;
    for (i = 0; i < 32; i++) s_sky.skyBot[i] = terrain_fog_565fix(s_sky.skyBot[i]);
    s_sunDir = v3_len(s_sky.sky.sunDir) > 0.1f ? v3_norm(s_sky.sky.sunDir) : v3_norm(v3(-0.587f, 0.643f, -0.492f));
    if (s_sky.sky.sun) s_sunRGB = 0xFF000000u | s_sky.sky.sun;
    if (s_sky.sky.ambient) s_ambRGB = 0xFF000000u | s_sky.sky.ambient;
    s_glowOn = -1;
    tc->fogRGB = s_fog;
    /* airbase (runway ribbons, marks, A7 buildings) */
    memset(&s_baseW, 0, sizeof(s_baseW));
    s_baseOk = world_load(&s_baseW, "island/base.trk", 0);
    /* jet + moving parts */
    s_jetOk = load_mesh(&s_jet, "jet");
    s_partsOk = load_mesh(&s_stab[0], "jet_stab_l") & load_mesh(&s_stab[1], "jet_stab_r") &
                load_mesh(&s_flap[0], "jet_flap_l") & load_mesh(&s_flap[1], "jet_flap_r") &
                load_mesh(&s_nozzle, "jet_nozzle") & load_mesh(&s_gear, "jet_gear");
    if (s_jetOk) {
        s_tStab[0] = mesh_tag(&s_jet, "hinge_stab_l"); s_tStab[1] = mesh_tag(&s_jet, "hinge_stab_r");
        s_tFlap[0] = mesh_tag(&s_jet, "hinge_flap_l"); s_tFlap[1] = mesh_tag(&s_jet, "hinge_flap_r");
        s_tNozzle = mesh_tag(&s_jet, "hinge_nozzle");
        s_tGear[0] = mesh_tag(&s_jet, "gear_n"); s_tGear[1] = mesh_tag(&s_jet, "gear_l"); s_tGear[2] = mesh_tag(&s_jet, "gear_r");
        if (!s_tStab[0] || !s_tStab[1] || !s_tFlap[0] || !s_tFlap[1] || !s_tNozzle || !s_tGear[0] || !s_tGear[1] || !s_tGear[2])
            s_partsOk = 0;
    }
    s_texAggr = tex_get("jet_aggr", 0, 0);
    s_mslOk[0] = load_mesh(&s_msl[0], "lrm");
    s_mslOk[1] = load_mesh(&s_msl[1], "srm");
    props_init(s_ter);
    clouds_init();
    shadow_init();                                  /* T22 */
    memset(&s_matDot, 0, sizeof(s_matDot));
    s_ok = 1;
    log_printf(LOG_INFO, "scene: init sky_tiles=%d base=%d jet=%d parts=%d props=%d fog=%06X sun=%.2f,%.2f,%.2f ms=%.0f tex_kb=%d",
               tiles, s_baseOk, s_jetOk, s_partsOk, s_nProps + 1, (unsigned)s_fog, (double)s_sunDir.x, (double)s_sunDir.y,
               (double)s_sunDir.z, (double)ms_since(t0), r_tex_kb());
    return 1;
}

void scene_free(void)
{
    int i;
    if (!s_ok) return;
    if (s_baseOk) world_free(&s_baseW);
    world_free(&s_sky);
    if (s_jetOk) mesh_free(&s_jet);
    for (i = 0; i < 2; i++) { mesh_free(&s_stab[i]); mesh_free(&s_flap[i]); }
    mesh_free(&s_nozzle);
    mesh_free(&s_gear);
    for (i = 0; i < 2; i++) if (s_mslOk[i]) { mesh_free(&s_msl[i]); s_mslOk[i] = 0; }
    if (s_texAggr) tex_release(s_texAggr);
    if (s_texPuffs) tex_release(s_texPuffs);
    s_texAggr = s_texPuffs = 0;
    t22_free();
    s_ok = 0;
}

u32 scene_clear_flags(void) { return s_fog | (s_skyOk ? R_ZONLY : 0); }

/* ---------------------------------------------------------------- aircraft */
static void wview_from_cam(WView *wv, const Camera *c, float viewDist, float lodDist)
{
    memset(wv, 0, sizeof(*wv));
    wv->eye = c->eye;
    wv->view = c->view;
    wv->proj = c->proj;
    cam_frustum(c, &wv->fr);
    wv->viewDist = viewDist;
    wv->lodDist = lodDist;
    wv->objScale = 1.0f;
    wv->lodScale = 1.0f;
    wv->treeDensity = 1.0f;
}

static void hinge_mat(Mat4 *m, Vec3 tag, float angRad)    /* rotX(angle) then translate(tag) (row vectors) */
{
    float c = (float)cos(angRad), s = (float)sin(angRad);
    m4_ident(m);
    m->m[1][1] = c; m->m[1][2] = s;
    m->m[2][1] = -s; m->m[2][2] = c;
    m->m[3][0] = tag.x; m->m[3][1] = tag.y; m->m[3][2] = tag.z;
}

/* ---------------------------------------------------------------- T22: fast aircraft path ([scene] acFast) */
/* T18 measured the own jet in the chase view at 4.9-5.5 ms on the VM: the backend transforms every vertex it is given
   (~1.5 us each) and submits every triangle (~1 us), and mesh_draw_lit issues one draw per part and material (jet 2,
   4 surfaces, nozzle, gear legs, up to 8 rail missiles x 2 = ~20 draws per LOD0 jet). This path keeps the look
   (same lighting formula as mesh_draw_lit) but
     - drops the triangles that face away from the camera on the CPU (plane test per triangle in mesh/part space,
       planes precomputed once; cull-none materials keep all), so only their vertices are lit and transformed;
     - transforms the moving parts and rail missiles into body space on the CPU and appends everything into one
       vertex/index batch per material, so one LOD0 jet costs ~4 draws (skin, nozzle/gear, missile body, fins).
   Moving-part lighting uses the sun in the part's own frame (mesh_draw_lit lit them with the body-frame sun). */
enum { FB_MAX = 6, FB_V = 3072, FB_I = 6144, FM_MAX = 12, FV_MAX = 8192 };
typedef struct FBatch { RMaterial rm; int nv, ni; } FBatch;
typedef struct FMesh { const Mesh *m; float *pl; } FMesh;   /* pl: 4 floats (outward normal, d) per index position */
static FBatch s_fb[FB_MAX];
static RVert1 s_fbv[FB_MAX][FB_V];
static u16   s_fbi[FB_MAX][FB_I];
static int   s_nfb;
static const Mat4 *s_fbWorld;
static FMesh s_fm[FM_MAX];
static int   s_nfm;
static u32   s_vStamp[FV_MAX], s_stampN;
static u16   s_vMap[FV_MAX];
static float s_fAr, s_fAg, s_fAb, s_fSr, s_fSg, s_fSb;  /* light set, 0..255 */

static const FMesh *fm_get(const Mesh *m)
{
    int i, l, k, j;
    FMesh *f;
    for (i = 0; i < s_nfm; i++) if (s_fm[i].m == m) return &s_fm[i];
    if (s_nfm >= FM_MAX || !m || m->numIdx <= 0 || m->numVerts > FV_MAX || !m->normals) return NULL;
    f = &s_fm[s_nfm];
    f->pl = (float *)malloc(sizeof(float) * 4 * (size_t)m->numIdx);
    if (!f->pl) return NULL;
    memset(f->pl, 0, sizeof(float) * 4 * (size_t)m->numIdx);
    f->m = m;
    for (l = 0; l < m->numLods; l++) {
        const MeshLod *ld = &m->lod[l];
        for (k = ld->firstPart; k < ld->firstPart + ld->numParts; k++) {
            const MeshPart *pt = &m->parts[k];
            for (j = pt->firstIdx; j + 2 < pt->firstIdx + pt->numIdx && j + 2 < m->numIdx; j += 3) {
                int a = ld->firstVert + m->idx[j], b = ld->firstVert + m->idx[j + 1], c = ld->firstVert + m->idx[j + 2];
                Vec3 pa, pb, pc, n, ns;
                float *p = f->pl + (size_t)j * 4;
                if (a >= m->numVerts || b >= m->numVerts || c >= m->numVerts) continue;
                pa = v3(m->verts[a].x, m->verts[a].y, m->verts[a].z);
                pb = v3(m->verts[b].x, m->verts[b].y, m->verts[b].z);
                pc = v3(m->verts[c].x, m->verts[c].y, m->verts[c].z);
                n = v3_cross(v3_sub(pb, pa), v3_sub(pc, pa));
                ns = v3_add(v3_add(m->normals[a], m->normals[b]), m->normals[c]);
                if (v3_dot(n, ns) < 0.0f) n = v3_scale(n, -1.0f);    /* outward = the vertex normals' side */
                p[0] = n.x; p[1] = n.y; p[2] = n.z; p[3] = v3_dot(n, pa);
            }
        }
    }
    s_nfm++;
    return f;
}

static int rm_same(const RMaterial *a, const RMaterial *b)
{
    return a->tex0 == b->tex0 && a->tex1 == b->tex1 && a->blend == b->blend && a->stage1 == b->stage1 && a->cullNone == b->cullNone &&
           a->zWrite == b->zWrite && a->zTest == b->zTest && a->fog == b->fog && a->clampUV == b->clampUV && a->zBias == b->zBias &&
           a->lodBias == b->lodBias;
}

static void fb_flush(void)
{
    int pass, i;
    if (!s_nfb) return;
    r_set_world(s_fbWorld);
    for (pass = 0; pass < 2; pass++)                /* opaque / alpha-tested first, blended after */
        for (i = 0; i < s_nfb; i++) {
            FBatch *b = &s_fb[i];
            int op = b->rm.blend == RB_OPAQUE || b->rm.blend == RB_ALPHATEST;
            if (!b->ni || op != (pass == 0)) continue;
            r_set_material(&b->rm);
            r_draw_mem(RFMT_V1, s_fbv[i], b->nv, s_fbi[i], b->ni);
        }
    r_set_world(NULL);
    s_nfb = 0;
}

static int fb_get(const RMaterial *rm)
{
    int i;
    for (i = 0; i < s_nfb; i++) if (rm_same(&s_fb[i].rm, rm)) return i;
    if (s_nfb >= FB_MAX) fb_flush();
    s_fb[s_nfb].rm = *rm;
    s_fb[s_nfb].nv = s_fb[s_nfb].ni = 0;
    return s_nfb++;
}

/* append the camera-facing triangles of one LOD, placed by loc (part -> body; NULL = body); eyeL / sunL in the
   mesh's own frame; returns triangles kept, -1 if the mesh can't take this path */
static int fast_append(const Mesh *m, int lod, const Mat4 *loc, Vec3 eyeL, Vec3 sunL, RTex texOverride)
{
    const FMesh *f = fm_get(m);
    const MeshLod *ld;
    int k, j, e, kept = 0;
    if (!f || !m->numLods) return -1;
    if (lod < 0) lod = 0;
    if (lod >= m->numLods) lod = m->numLods - 1;
    ld = &m->lod[lod];
    for (k = ld->firstPart; k < ld->firstPart + ld->numParts; k++) {
        const MeshPart *pt = &m->parts[k];
        RMaterial rm = m->mats[pt->mat].rm;
        int bi;
        FBatch *b;
        if (texOverride && mesh_tex_override_ok(m, pt->mat)) rm.tex0 = texOverride;
        if (rm.stage1 == RS1_ENVADD) rm.stage1 = RS1_NONE;
        bi = fb_get(&rm);
        b = &s_fb[bi];
        s_stampN++;
        for (j = pt->firstIdx; j + 2 < pt->firstIdx + pt->numIdx && j + 2 < m->numIdx; j += 3) {
            if (!rm.cullNone) {
                const float *p = f->pl + (size_t)j * 4;
                if (p[0] * eyeL.x + p[1] * eyeL.y + p[2] * eyeL.z - p[3] <= 0.0f) continue;
            }
            if (b->nv + 3 > FB_V || b->ni + 3 > FB_I) {     /* full: draw everything so far, start again */
                fb_flush();
                bi = fb_get(&rm);
                b = &s_fb[bi];
                s_stampN++;
            }
            for (e = 0; e < 3; e++) {
                int vi = ld->firstVert + m->idx[j + e];
                if (vi >= m->numVerts) vi = m->numVerts - 1;
                if (s_vStamp[vi] != s_stampN) {
                    const RVert1 *s = &m->verts[vi];
                    Vec3 n = m->normals[vi];
                    RVert1 *o = &s_fbv[bi][b->nv];
                    float d = n.x * sunL.x + n.y * sunL.y + n.z * sunL.z, h = 0.75f + 0.25f * n.y;
                    u32 c = s->color;
                    float cr = (float)((c >> 16) & 0xFF) * (1.0f / 255.0f), cg = (float)((c >> 8) & 0xFF) * (1.0f / 255.0f);
                    float cb = (float)(c & 0xFF) * (1.0f / 255.0f);
                    int ir, ig, ib;
                    if (d < 0.0f) d = 0.0f;
                    ir = e_ftoi((s_fAr * h + s_fSr * d) * cr); ig = e_ftoi((s_fAg * h + s_fSg * d) * cg); ib = e_ftoi((s_fAb * h + s_fSb * d) * cb);
                    ir = ir < 0 ? 0 : (ir > 255 ? 255 : ir); ig = ig < 0 ? 0 : (ig > 255 ? 255 : ig); ib = ib < 0 ? 0 : (ib > 255 ? 255 : ib);
                    if (loc) {
                        Vec3 q = m4_mul_point(loc, v3(s->x, s->y, s->z));
                        o->x = q.x; o->y = q.y; o->z = q.z;
                    } else { o->x = s->x; o->y = s->y; o->z = s->z; }
                    o->u = s->u; o->v = s->v;
                    o->color = (c & 0xFF000000u) | ((u32)ir << 16) | ((u32)ig << 8) | (u32)ib;
                    s_vStamp[vi] = s_stampN;
                    s_vMap[vi] = (u16)b->nv++;
                }
                s_fbi[bi][b->ni++] = s_vMap[vi];
            }
            kept++;
        }
    }
    return kept;
}

/* one mesh at loc (part -> body) in the jet's body frame: fast batch, or the engine path */
static int jet_piece(const Mesh *m, int lod, const Mat4 *loc, const Mat4 *body, RTex tex, Vec3 sunB, Vec3 eyeB, const Camera *c,
                     int rigid)
{
    if (s_acFast) {
        Vec3 eyeL = eyeB, sunL = sunB;
        int n;
        if (loc) {
            if (rigid) {                            /* rotation + translation: exact inverse */
                Mat4 inv;
                m4_inverse_rt(&inv, loc);
                eyeL = m4_mul_point(&inv, eyeB);
                sunL = m4_mul_dir(&inv, sunB);
            } else {                                /* nozzle: diagonal scale + translation */
                eyeL = v3_sub(eyeB, v3(loc->m[3][0], loc->m[3][1], loc->m[3][2]));
                eyeL.x /= loc->m[0][0] > 1e-3f ? loc->m[0][0] : 1.0f;
                eyeL.y /= loc->m[1][1] > 1e-3f ? loc->m[1][1] : 1.0f;
                eyeL.z /= loc->m[2][2] > 1e-3f ? loc->m[2][2] : 1.0f;
            }
        }
        n = fast_append(m, lod, loc, eyeL, sunL, tex);
        if (n >= 0) return n;
    }
    {
        Mat4 w;
        if (loc) m4_mul(&w, loc, body); else w = *body;
        mesh_draw_lit(m, &w, lod, tex, sunB, s_sunRGB, s_ambRGB, 0, &c->view);
        return mesh_lod_tris(m, lod);
    }
}

/* T17: missiles on the rails of a LOD0 jet (A1 tags rail_0..7: 0/1 wingtip SRM, 2..7 pylons LRM). What is shown:
   min(2, srm) wingtip SRMs and min(6, lrm) LRMs filled from the inner pylons out, minus rails that just fired
   (WpnState.railEmpty > 0). LOD1 missile (20 tris) beyond 10 m (A8: 8 x 112 lit tris is a VM cost). */
static int s_railAc = -1;                           /* aircraft id whose rails draw_jet shows (-1 = none, props) */
static int draw_rails(const Camera *c, const Mat4 *body, Vec3 sunB, Vec3 eyeB, float d)
{
    static const int lrmOrder[6] = { 6, 7, 4, 5, 2, 3 };
    const WpnState *w;
    int k, tris = 0, nl, ns, show[RAIL_COUNT];
    char tn[8];
    if (s_railAc < 0 || !s_jetOk) return 0;
    w = weapons_state(s_railAc);
    memset(show, 0, sizeof(show));
    nl = w->lrm < 6 ? w->lrm : 6;
    ns = w->srm < 2 ? w->srm : 2;
    for (k = 0; k < nl; k++) show[lrmOrder[k]] = 1;
    for (k = 0; k < ns; k++) show[k] = 1;
    for (k = 0; k < RAIL_COUNT; k++) {
        const MeshTag *t;
        int type = k < 2 ? MSL_SRM : MSL_LRM, lod;
        Mat4 loc;
        if (!show[k] || w->railEmpty[k] > 0.0f || !s_mslOk[type]) continue;
        e_snprintf(tn, (int)sizeof(tn), "rail_%d", k);
        t = mesh_tag(&s_jet, tn);
        if (!t) continue;
        m4_ident(&loc);
        loc.m[3][0] = t->pos.x; loc.m[3][1] = t->pos.y; loc.m[3][2] = t->pos.z;
        lod = d < 10.0f ? 0 : 1;
        if (lod >= s_msl[type].numLods) lod = s_msl[type].numLods - 1;
        tris += jet_piece(&s_msl[type], lod, &loc, body, 0, sunB, eyeB, c, 1);   /* T22: fast batch */
    }
    return tris;
}

/* one jet at pos / qNose; vis may be NULL (surfaces neutral); returns triangles drawn (T22: [scene] acFast draws
   only the camera-facing ones, batched per material: jet_piece / fast_append above) */
static int draw_jet(const Camera *c, const Frustum *fr, Vec3 pos, Quat q, const FlightVisual *vis, float gearPos,
                    RTex livery)
{
    const ScenePreset *p = &s_eff;
    float d = v3_len(v3_sub(pos, c->eye));
    Mat3 r;
    Mat4 body, loc;
    Vec3 sunB, eyeB;
    int lod, tris, i;
    if (!s_jetOk || d > s_acMeshDist || !frustum_sphere(fr, pos, 9.0f)) return 0;
    lod = mesh_lod(&s_jet, d, p->acLodScale * s_lodMul);
    q_to_mat3(q, &r);
    m4_from_m3_pos(&body, &r, pos);
    sunB = m3_tmul(&r, s_sunDir);
    eyeB = m3_tmul(&r, v3_sub(c->eye, pos));
    s_nfb = 0;
    s_fbWorld = &body;
    s_fAr = (float)((s_ambRGB >> 16) & 0xFF); s_fAg = (float)((s_ambRGB >> 8) & 0xFF); s_fAb = (float)(s_ambRGB & 0xFF);
    s_fSr = (float)((s_sunRGB >> 16) & 0xFF); s_fSg = (float)((s_sunRGB >> 8) & 0xFF); s_fSb = (float)(s_sunRGB & 0xFF);
    tris = jet_piece(&s_jet, lod, NULL, &body, livery, sunB, eyeB, c, 1);
    if (lod != 0 || !s_partsOk) { fb_flush(); return tris; }
    tris += draw_rails(c, &body, sunB, eyeB, d);
    for (i = 0; i < 2; i++) {
        hinge_mat(&loc, s_tStab[i]->pos, vis ? (i ? vis->stabR : vis->stabL) : 0.0f);
        tris += jet_piece(&s_stab[i], 0, &loc, &body, livery, sunB, eyeB, c, 1);
        hinge_mat(&loc, s_tFlap[i]->pos, vis ? (i ? vis->flapR : vis->flapL) : 0.0f);
        tris += jet_piece(&s_flap[i], 0, &loc, &body, livery, sunB, eyeB, c, 1);
    }
    {                                               /* nozzle petals open with the afterburner (scale x/y) */
        float sc = 1.0f + 0.18f * (vis ? E_CLAMP(vis->nozzle, 0.0f, 1.0f) : 0.0f);
        m4_ident(&loc);
        loc.m[0][0] = sc; loc.m[1][1] = sc;
        loc.m[3][0] = s_tNozzle->pos.x; loc.m[3][1] = s_tNozzle->pos.y; loc.m[3][2] = s_tNozzle->pos.z;
        tris += jet_piece(&s_nozzle, 0, &loc, &body, 0, sunB, eyeB, c, 0);
    }
    if (gearPos > 0.001f) {                         /* nose leg swings aft, mains forward (A1) */
        float up = E_DEG2RAD((1.0f - E_CLAMP(gearPos, 0.0f, 1.0f)) * 90.0f);
        for (i = 0; i < 3; i++) {
            hinge_mat(&loc, s_tGear[i]->pos, i == 0 ? up : -up);
            tris += jet_piece(&s_gear, 0, &loc, &body, 0, sunB, eyeB, c, 1);
        }
    }
    fb_flush();
    return tris;
}

static void lerp_vis(FlightVisual *o, const FlightVisual *a, const FlightVisual *b, float t)
{
    o->stabL = a->stabL + (b->stabL - a->stabL) * t;
    o->stabR = a->stabR + (b->stabR - a->stabR) * t;
    o->flapL = a->flapL + (b->flapL - a->flapL) * t;
    o->flapR = a->flapR + (b->flapR - a->flapR) * t;
    o->nozzle = a->nozzle + (b->nozzle - a->nozzle) * t;
    o->vapour = a->vapour + (b->vapour - a->vapour) * t;
}

/* T17: missiles in flight (DESIGN 9.1: mesh < 600 m; LOD0 < 120 m), lit like the jets */
static void draw_missiles(const Camera *c, const Frustum *fr, float alpha)
{
    static MissileView mv[MAX_MISSILES];
    int n = weapons_missiles(mv, MAX_MISSILES), i;
    for (i = 0; i < n; i++) {
        const MissileView *m = &mv[i];
        Vec3 pos = v3_lerp(m->prevPos, m->pos, alpha), f = m->dir, u, r;
        float d = v3_len(v3_sub(pos, c->eye));
        Mat3 rot;
        Mat4 w;
        int type = m->type == MSL_SRM ? 1 : 0, lod;
        if (!s_mslOk[type] || d > 600.0f || !frustum_sphere(fr, pos, 3.0f)) continue;
        u = (float)fabs(f.y) < 0.95f ? v3(0.0f, 1.0f, 0.0f) : v3(1.0f, 0.0f, 0.0f);
        r = v3_norm(v3_cross(u, f));
        u = v3_cross(f, r);
        rot.m[0][0] = r.x; rot.m[0][1] = r.y; rot.m[0][2] = r.z;
        rot.m[1][0] = u.x; rot.m[1][1] = u.y; rot.m[1][2] = u.z;
        rot.m[2][0] = f.x; rot.m[2][1] = f.y; rot.m[2][2] = f.z;
        m4_from_m3_pos(&w, &rot, pos);
        lod = d < 120.0f ? 0 : 1;
        if (lod >= s_msl[type].numLods) lod = s_msl[type].numLods - 1;
        mesh_draw_lit(&s_msl[type], &w, lod, 0, m3_tmul(&rot, s_sunDir), s_sunRGB, s_ambRGB, 0, &c->view);
        s_st.acTris += mesh_lod_tris(&s_msl[type], lod);
    }
}

static void draw_aircraft(const Game *g, const Camera *c, float alpha)
{
    Frustum fr;
    Vec3 pos;
    Quat q;
    FlightVisual vis;
    float gear, t = ((float)g->tick + alpha) / (float)SIM_HZ;
    int i, n = g->mode == MODE_FREE ? scene_prop_count() : 0;   /* T17: M1 props only in FREE flight */
    cam_frustum(c, &fr);
    s_railAc = -1;
    for (i = 0; i < n; i++) {
        int tr;
        if (!scene_prop_pose(i, t, &pos, &q, &gear)) continue;
        tr = draw_jet(c, &fr, pos, q, NULL, gear, s_texAggr);
        if (tr) { s_st.aircraftDrawn++; s_st.acTris += tr; }
    }
    for (i = 1; i < g->numAircraft; i++) {          /* enemies (M2) */
        const Aircraft *a = &g->ac[i];
        int tr;
        if (!(a->flags & AC_ALIVE)) continue;
        cam_ac_pose(a, alpha, &pos, &q);
        lerp_vis(&vis, &a->prev.vis, &a->fs.vis, alpha);
        s_railAc = i;
        tr = draw_jet(c, &fr, pos, q, &vis, a->fs.gearPos, a->livery ? s_texAggr : 0);
        if (tr) { s_st.aircraftDrawn++; s_st.acTris += tr; }
    }
    /* own jet last, chase view only (user: the cockpit view shows only the HUD) */
    if (c->mode == VIEW_CHASE && !c->fixed && (g->ac[PLAYER_ID].flags & AC_ALIVE)) {
        const Aircraft *a = &g->ac[PLAYER_ID];
        int tr;
        cam_ac_pose(a, alpha, &pos, &q);
        lerp_vis(&vis, &a->prev.vis, &a->fs.vis, alpha);
        s_railAc = PLAYER_ID;
        tr = draw_jet(c, &fr, pos, q, &vis, a->prev.gearPos + (a->fs.gearPos - a->prev.gearPos) * alpha, 0);
        if (tr) { s_st.aircraftDrawn++; s_st.acTris += tr; }
    }
    s_railAc = -1;
    draw_missiles(c, &fr, alpha);
}

/* ---------------------------------------------------------------- clouds */
static void cam_axes(const Camera *c, Vec3 *r, Vec3 *u, Vec3 *f)
{
    *r = v3(c->view.m[0][0], c->view.m[1][0], c->view.m[2][0]);
    *u = v3(c->view.m[0][1], c->view.m[1][1], c->view.m[2][1]);
    *f = v3(c->view.m[0][2], c->view.m[1][2], c->view.m[2][2]);
}

static void draw_clouds(const Camera *c, float farM)
{
    const ScenePreset *p = &s_eff;
    Vec3 r, u, f;
    int i, j, n = 0, nv = 0, ni = 0, cnt = E_CLAMP(p->clouds, 0, CLOUD_MAX);
    s_st.cloudQuads = 0;
    s_cloudNear = 0.0f;
    if (!s_texPuffs) return;
    cam_axes(c, &r, &u, &f);
    for (i = 0; i < cnt; i++) {                     /* T22: fly-through factor (fx.cpp: haze overlay + wisps) */
        const Puff *pf = &s_puff[i];
        Vec3 d = v3_sub(pf->c, c->eye);
        float h = 0.5f * pf->size, fd = s_cloudFade > 1.0f ? s_cloudFade : 1.0f, dd, k;
        if ((float)fabs(d.x) > h + fd || (float)fabs(d.z) > h + fd || (float)fabs(d.y) > h + fd) continue;
        d.y *= 1.25f;                               /* puffs are drawn 0.8 x as tall as wide */
        dd = v3_len(d);
        k = 1.0f - sm01(0.45f * h, h + 0.8f * fd, dd);
        if (k > s_cloudNear) s_cloudNear = k;
    }
    r_set_world(NULL);
    if (p->cloudDeck) {                             /* 6 km deck: one quad around the camera, UVs camera-relative */
        float tile = 3000.0f, half = farM, sx, sz, y = 6000.0f;
        if (!s_texDeck) { s_texDeck = tex_get("cloud_deck", IMG_ALPHA, 0); s_matDeck.tex0 = s_texDeck; }
        sx = (float)floor(c->eye.x / tile) * tile;
        sz = (float)floor(c->eye.z / tile) * tile;
        for (i = 0; i < 4; i++) {
            float x = c->eye.x + ((i & 1) ? half : -half), z = c->eye.z + ((i & 2) ? half : -half);
            RVert1 *v = &s_cv[i];
            v->x = x; v->y = y; v->z = z; v->color = 0xF0FFFFFF;
            v->u = (x - sx) / tile; v->v = (z - sz) / tile;
        }
        s_ci[0] = 0; s_ci[1] = 1; s_ci[2] = 2; s_ci[3] = 2; s_ci[4] = 1; s_ci[5] = 3;
        if (s_texDeck) { r_set_material(&s_matDeck); r_draw_mem(RFMT_V1, s_cv, 4, s_ci, 6); s_st.cloudQuads++; }
    }
    for (i = 0; i < cnt; i++) {                     /* cull, then sort far to near */
        Puff *pf = &s_puff[i];
        Vec3 d = v3_sub(pf->c, c->eye);
        float dd = v3_len(d);
        if (dd > farM + pf->size || v3_dot(d, f) < -pf->size || dd < 0.5f * pf->size + 1.0f) continue;
        if (v3_dot(d, f) + 0.5f * pf->size > c->farM) continue;   /* T10b: never past the far plane (fog wraps there) */
        pf->d2 = dd;
        for (j = n; j > 0 && s_puff[s_order[j - 1]].d2 < dd; j--) s_order[j] = s_order[j - 1];
        s_order[j] = i;
        n++;
    }
    for (j = 0; j < n; j++) {
        const Puff *pf = &s_puff[s_order[j]];
        float h = 0.5f * pf->size, a = E_CLAMP((pf->d2 - h) / (s_cloudFade > 1.0f ? s_cloudFade : 1.0f), 0.0f, 1.0f);
        float u0 = (float)(pf->cell & 1) * 0.5f + 0.002f, v0 = (float)(pf->cell >> 1) * 0.5f + 0.002f, w = 0.496f;
        u32 col = ((u32)e_ftoi(a * 255.0f) << 24) | 0xFFFFFF;
        Vec3 rr = v3_scale(r, h), uu = v3_scale(u, h * 0.8f);
        RVert1 *v = &s_cv[nv];
        if (a <= 0.01f) continue;
        v[0].x = pf->c.x - rr.x + uu.x; v[0].y = pf->c.y - rr.y + uu.y; v[0].z = pf->c.z - rr.z + uu.z; v[0].u = u0; v[0].v = v0;
        v[1].x = pf->c.x + rr.x + uu.x; v[1].y = pf->c.y + rr.y + uu.y; v[1].z = pf->c.z + rr.z + uu.z; v[1].u = u0 + w; v[1].v = v0;
        v[2].x = pf->c.x - rr.x - uu.x; v[2].y = pf->c.y - rr.y - uu.y; v[2].z = pf->c.z - rr.z - uu.z; v[2].u = u0; v[2].v = v0 + w;
        v[3].x = pf->c.x + rr.x - uu.x; v[3].y = pf->c.y + rr.y - uu.y; v[3].z = pf->c.z + rr.z - uu.z; v[3].u = u0 + w; v[3].v = v0 + w;
        v[0].color = v[1].color = v[2].color = v[3].color = col;
        s_ci[ni + 0] = (u16)nv; s_ci[ni + 1] = (u16)(nv + 1); s_ci[ni + 2] = (u16)(nv + 2);
        s_ci[ni + 3] = (u16)(nv + 2); s_ci[ni + 4] = (u16)(nv + 1); s_ci[ni + 5] = (u16)(nv + 3);
        nv += 4; ni += 6;
    }
    if (ni) {
        r_set_material(&s_matPuff);
        r_draw_mem(RFMT_V1, s_cv, nv, s_ci, ni);
        s_st.cloudQuads += ni / 6;
    }
}

/* ---------------------------------------------------------------- T22: jet shadows ([scene] shadow*) */
/* The planform of jet.msh LOD1 (self-contained: fixed surfaces, wing without the flaperon notch), rasterised once at
   init into a 64 x 128 alpha texture (2 x 2 samples per texel, 3 x 3 blur) over the body rectangle x -5..5, z
   8.5..-8.0 (u across, v nose -> tail). Per frame a 3 x 5 grid of that rectangle (body y = 0) is projected along the
   sun (shadowSunK) onto the real surface (two iterations against world_surface), lifted shadowLiftM, and drawn
   RB_SHADOW (dest x (1 - a): no colour, so the runway, grass and sea keep their own tint), no Z write, zBias 2, fog
   off with a hand fade by range. Darkness fades out with height (shadowAglM) and is weaker on the sea. */
#define SH_W 64
#define SH_H 128
#define SH_X0 (-5.0f)
#define SH_X1 5.0f
#define SH_Z0 8.5f
#define SH_Z1 (-8.0f)
static RTex  s_texShadow;
static RMaterial s_matShadow;
static int   s_shadowsDrawn;

static float edge_fn(float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }

static void shadow_init(void)
{
    static u8 mask[SH_W * SH_H];
    EImage im;
    const MeshLod *ld;
    int lod, k, j, x, y, s;
    memset(&s_matShadow, 0, sizeof(s_matShadow));
    s_texShadow = 0;
    if (!s_jetOk || !s_jet.numLods) return;
    memset(mask, 0, sizeof(mask));
    lod = s_jet.numLods > 1 ? 1 : 0;
    ld = &s_jet.lod[lod];
    for (k = ld->firstPart; k < ld->firstPart + ld->numParts; k++) {
        const MeshPart *pt = &s_jet.parts[k];
        for (j = pt->firstIdx; j + 2 < pt->firstIdx + pt->numIdx; j += 3) {
            float px[3], py[3], x0, x1, y0, y1, ar;
            int e;
            for (e = 0; e < 3; e++) {
                const RVert1 *v = &s_jet.verts[ld->firstVert + s_jet.idx[j + e]];
                px[e] = (v->x - SH_X0) / (SH_X1 - SH_X0) * (float)SH_W;
                py[e] = (v->z - SH_Z0) / (SH_Z1 - SH_Z0) * (float)SH_H;
            }
            ar = edge_fn(px[0], py[0], px[1], py[1], px[2], py[2]);
            if (ar < 1e-4f && ar > -1e-4f) continue;
            x0 = E_MINF(px[0], E_MINF(px[1], px[2])); x1 = E_MAXF(px[0], E_MAXF(px[1], px[2]));
            y0 = E_MINF(py[0], E_MINF(py[1], py[2])); y1 = E_MAXF(py[0], E_MAXF(py[1], py[2]));
            for (y = (int)E_MAXF(0.0f, (float)floor(y0)); y < SH_H && (float)y <= y1; y++)
                for (x = (int)E_MAXF(0.0f, (float)floor(x0)); x < SH_W && (float)x <= x1; x++)
                    for (s = 0; s < 4; s++) {
                        float sx = (float)x + 0.25f + 0.5f * (float)(s & 1), sy = (float)y + 0.25f + 0.5f * (float)(s >> 1);
                        float w0 = edge_fn(px[1], py[1], px[2], py[2], sx, sy), w1 = edge_fn(px[2], py[2], px[0], py[0], sx, sy);
                        float w2 = edge_fn(px[0], py[0], px[1], py[1], sx, sy);
                        if (ar < 0.0f) { w0 = -w0; w1 = -w1; w2 = -w2; }
                        if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) mask[y * SH_W + x] |= (u8)(1 << s);
                    }
        }
    }
    if (!img_alloc(&im, SH_W, SH_H, IMG_ALPHA)) return;
    for (y = 0; y < SH_H; y++)
        for (x = 0; x < SH_W; x++) {
            int sum = 0, dx, dy;
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy, m;
                    if (xx < 0 || yy < 0 || xx >= SH_W || yy >= SH_H) continue;
                    m = mask[yy * SH_W + xx];
                    sum += (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1);
                }
            im.pix[0][y * SH_W + x] = (u32)(sum * 255 / 36) << 24;
        }
    for (x = 0; x < SH_W; x++) { im.pix[0][x] = 0; im.pix[0][(SH_H - 1) * SH_W + x] = 0; }   /* clamp-safe border */
    for (y = 0; y < SH_H; y++) { im.pix[0][y * SH_W] = 0; im.pix[0][y * SH_W + SH_W - 1] = 0; }
    s_texShadow = tex_add("t22_jet_shadow", &im, RT_CLAMP);
    img_free(&im);
    s_matShadow.tex0 = s_texShadow;
    s_matShadow.blend = RB_SHADOW;
    s_matShadow.zTest = 1;
    s_matShadow.zWrite = 0;
    s_matShadow.fog = 0;
    s_matShadow.cullNone = 1;
    s_matShadow.clampUV = 1;
    s_matShadow.zBias = 2;
}

static int shadow_one(const Game *g, const Camera *c, const Frustum *fr, Vec3 pos, Quat q)
{
    static RVert1 v[15];
    static u16 idx[48];
    Vec3 dir;
    float agl, d, a, sea;
    int i, j, ni = 0, seaN = 0;
    agl = pos.y - world_surface(g, pos.x, pos.z);
    d = v3_len(v3_sub(pos, c->eye));
    if (agl > s_shadowAgl || d > s_shadowDist + s_shadowAgl * 1.6f) return 0;
    a = s_shadowAlpha * (1.0f - sm01(0.35f * s_shadowAgl, s_shadowAgl, agl));
    dir = v3_lerp(v3(0.0f, 1.0f, 0.0f), s_sunDir, E_CLAMP(s_shadowSunK, 0.0f, 1.0f));
    if (dir.y < 0.15f) dir.y = 0.15f;
    dir = v3_norm(dir);
    for (j = 0; j < 5; j++)
        for (i = 0; i < 3; i++) {
            Vec3 b = v3(SH_X0 + (SH_X1 - SH_X0) * 0.5f * (float)i, 0.0f, SH_Z0 + (SH_Z1 - SH_Z0) * 0.25f * (float)j);
            Vec3 pp = v3_add(pos, q_rotate(q, b)), qq;
            float t = (pp.y - world_surface(g, pp.x, pp.z)) / dir.y, h;
            RVert1 *o = &v[j * 3 + i];
            qq = v3_madd(pp, dir, -t);
            t = (pp.y - world_surface(g, qq.x, qq.z)) / dir.y;
            qq = v3_madd(pp, dir, -t);
            h = world_surface(g, qq.x, qq.z);
            if (g->terrain && terrain_height(g->terrain, qq.x, qq.z) < 0.0f) seaN++;
            o->x = qq.x; o->y = h + s_shadowLift; o->z = qq.z;
            o->u = 0.5f * (float)i; o->v = 0.25f * (float)j;
        }
    if (!frustum_sphere(fr, v3(v[7].x, v[7].y, v[7].z), 14.0f)) return 0;
    d = v3_len(v3_sub(v3(v[7].x, v[7].y, v[7].z), c->eye));
    a *= 1.0f - sm01(0.6f * s_shadowDist, s_shadowDist, d);
    sea = 1.0f + (s_shadowSeaK - 1.0f) * (float)seaN / 15.0f;
    a *= sea;
    if (a < 0.01f) return 0;
    for (i = 0; i < 15; i++) v[i].color = ((u32)e_ftoi(E_CLAMP(a, 0.0f, 1.0f) * 255.0f) << 24) | 0x000000u;
    for (j = 0; j < 4; j++)
        for (i = 0; i < 2; i++) {
            int b = j * 3 + i;
            idx[ni++] = (u16)b; idx[ni++] = (u16)(b + 1); idx[ni++] = (u16)(b + 3);
            idx[ni++] = (u16)(b + 3); idx[ni++] = (u16)(b + 1); idx[ni++] = (u16)(b + 4);
        }
    r_set_material(&s_matShadow);
    r_draw_mem(RFMT_V1, v, 15, idx, ni);
    return 1;
}

static void draw_shadows(const Game *g, const Camera *c, float alpha)
{
    Frustum fr;
    Vec3 pos;
    Quat q;
    float gear, t = ((float)g->tick + alpha) / (float)SIM_HZ;
    int i, n = g->mode == MODE_FREE ? scene_prop_count() : 0;
    s_shadowsDrawn = 0;
    if (!s_shadowOn || !s_texShadow || !g->terrain) return;
    cam_frustum(c, &fr);
    r_set_world(NULL);
    for (i = 0; i < n; i++)
        if (scene_prop_pose(i, t, &pos, &q, &gear)) s_shadowsDrawn += shadow_one(g, c, &fr, pos, q);
    for (i = 0; i < g->numAircraft; i++) {          /* own jet in both views: its shadow can show in the cockpit too */
        if (!(g->ac[i].flags & AC_ALIVE)) continue;
        cam_ac_pose(&g->ac[i], alpha, &pos, &q);
        s_shadowsDrawn += shadow_one(g, c, &fr, pos, q);
    }
}

int scene_shadows_drawn(void) { return s_shadowsDrawn; }

static void t22_free(void)
{
    int i;
    if (s_texShadow) tex_release(s_texShadow);
    s_texShadow = 0;
    for (i = 0; i < s_nfm; i++) free(s_fm[i].pl);  /* fast-path planes */
    s_nfm = 0;
}
float scene_cloud_near(void) { return s_cloudNear; }

/* ---------------------------------------------------------------- frame */
void scene_draw(const Game *g, const Camera *c, float alpha)
{
    const ScenePreset *p = scene_preset();
    TerrainDrawCfg *tc = terrain_draw_cfg();
    WView wv, wb;
    WDrawStats bs;
    float farM, fogStart, fogEnd, farR, seconds = ((float)g->tick + alpha) / (float)SIM_HZ;
    s64 t0;
    if (!s_ok) return;
    if (s_glowOn != p->sunGlow) { world_set_sun_glow(&s_sky, p->sunGlow); s_glowOn = p->sunGlow; }
    s_st.aircraftDrawn = s_st.acTris = s_st.dots = 0;
    /* T10b: altitude-aware distances; far = the camera's far plane (camera.cpp uses scene_far_at too) */
    scene_view_dist(c->eye.y, &farM, &fogStart, &fogEnd);
    if (c->farM > 100.0f) farM = c->farM;
    if (fogEnd > 0.98f * farM) fogEnd = 0.98f * farM;
    if (fogStart > 0.9f * fogEnd) fogStart = 0.9f * fogEnd;
    farR = scene_far_radial(c);
    s_st.farM = farM; s_st.fogStart = fogStart; s_st.fogEnd = fogEnd;
    wview_from_cam(&wv, c, farM, 0.0f);
    r_set_proj(&c->proj);
    r_set_view(&c->view);
    r_set_world(NULL);
    r_set_fog(1, s_fog, fogStart, fogEnd);
    tc->fogRGB = s_fog; tc->fogStart = fogStart; tc->fogEnd = fogEnd;
    /* sky first (T10b default): no Z, so every piece of geometry covers it; the late sky (T16 fill saving) puts its
       dome at the far-plane radius and paints over ground farther than that, which is the whole view from altitude */
    world_set_sky_late(s_skyFirst ? 0 : 1);
    if (s_skyFirst) {
        t0 = plat_qpc(); prof_begin(s_secSky);
        if (s_skyOk) world_draw_sky(&s_sky, &wv);
        r_set_proj(&c->proj); r_set_view(&c->view); r_set_world(NULL);
        r_set_fog(1, s_fog, fogStart, fogEnd);
        prof_end(s_secSky); s_st.skyMs = ms_since(t0);
    }
    /* terrain: radial cull at the frustum's corner distance, the planar far plane culls the rest */
    t0 = plat_qpc(); prof_begin(s_secTerrain);
    if (s_ter) terrain_draw(s_ter, &wv, p->lodScale, farR);
    prof_end(s_secTerrain); s_st.terrainMs = ms_since(t0);
    s_st.terrainTris = terrain_stats()->tris;
    scatter_draw(c);                                /* A11: vegetation / rocks / villages (own prof section) */
    /* airbase */
    t0 = plat_qpc(); prof_begin(s_secBase);
    memset(&bs, 0, sizeof(bs));
    if (s_baseOk) {
        WDrawStats b2;
        memset(&b2, 0, sizeof(b2));
        wb = wv;
        wb.viewDist = s_baseDist + s_baseAltK * E_MAXF(0.0f, c->eye.y);
        if (wb.viewDist > farR) wb.viewDist = farR;
        wb.lodDist = s_baseLodDist;
        world_draw_opaque(&s_baseW, &wb, &bs);
        world_draw_alpha(&s_baseW, &wb, &b2);
        bs.tris += b2.tris;
    }
    prof_end(s_secBase); s_st.baseMs = ms_since(t0);
    s_st.baseTris = bs.tris;
    /* aircraft */
    t0 = plat_qpc(); prof_begin(s_secAc);
    r_set_proj(&c->proj); r_set_view(&c->view);
    draw_aircraft(g, c, alpha);
    r_set_world(NULL);
    prof_end(s_secAc); s_st.acMs = ms_since(t0);
    /* sea: shallow + ring + fog band + foam */
    t0 = plat_qpc(); prof_begin(s_secSea);
    if (s_ter) sea_draw(s_ter, &wv, seconds);
    prof_end(s_secSea); s_st.seaMs = ms_since(t0);
    s_st.seaTris = terrain_stats()->seaTris + terrain_stats()->shallowTris + terrain_stats()->foamTris;
    /* T22: jet shadows on the ground / runway / sea (after the sea so they darken the water too) */
    t0 = plat_qpc(); prof_begin(s_secAc);
    draw_shadows(g, c, alpha);
    prof_end(s_secAc); s_st.acMs += ms_since(t0);
    /* late sky (fills only pixels no geometry covered) */
    if (!s_skyFirst) {
        t0 = plat_qpc(); prof_begin(s_secSky);
        r_set_fog(1, s_fog, fogStart, fogEnd);
        if (s_skyOk) world_draw_sky(&s_sky, &wv);
        prof_end(s_secSky); s_st.skyMs = ms_since(t0);
    }
    /* clouds */
    t0 = plat_qpc(); prof_begin(s_secClouds);
    r_set_proj(&c->proj); r_set_view(&c->view);
    r_set_fog(1, s_fog, fogStart, fogEnd);
    draw_clouds(c, farR);
    prof_end(s_secClouds); s_st.cloudMs = ms_since(t0);
}

/* 2x2 px dark dots for aircraft between acDotFrom and acDotDist (DESIGN 9.1b): tail-on jets never vanish */
static int dot_one(const Camera *c, const Mat4 *vp, Vec3 pos, int sw, int sh)
{
    float in[4], out[4], d = v3_len(v3_sub(pos, c->eye)), a, x, y, k;
    if (d < s_acDotFrom || d > s_acDotDist) return 0;
    in[0] = pos.x; in[1] = pos.y; in[2] = pos.z; in[3] = 1.0f;
    m4_mul_vec4(vp, in, out);
    if (out[3] <= 1.0f) return 0;
    x = (out[0] / out[3] * 0.5f + 0.5f) * (float)sw;
    y = (0.5f - out[1] / out[3] * 0.5f) * (float)sh;
    if (x < 1.0f || y < 1.0f || x > (float)sw - 3.0f || y > (float)sh - 3.0f) return 0;
    if (s_ter && !terrain_los(s_ter, &c->eye, &pos)) return 0;
    a = s_dotAlpha * E_CLAMP((s_acDotDist - d) / (s_acDotDist - s_acDotFrom + 1.0f) * 2.0f, 0.0f, 1.0f);
    k = 480.0f / (float)sh;                         /* ui virtual units per pixel */
    ui_rect((float)floor(x) * k, (float)floor(y) * k, 2.0f * k, 2.0f * k, ((u32)e_ftoi(a * 255.0f) << 24) | 0x101418);
    return 1;
}

void scene_draw_dots(const Game *g, const Camera *c, float alpha)
{
    Mat4 vp;
    Vec3 pos;
    Quat q;
    float gear, t = ((float)g->tick + alpha) / (float)SIM_HZ;
    int i, n = 0, sw = 0, sh = 0;
    r_screen_size(&sw, &sh);
    if (sw <= 0 || sh <= 0) return;
    m4_mul(&vp, &c->view, &c->proj);
    ui_begin(640, 480);
    for (i = 0; g->mode == MODE_FREE && i < scene_prop_count(); i++)
        if (scene_prop_pose(i, t, &pos, &q, &gear)) n += dot_one(c, &vp, pos, sw, sh);
    for (i = 1; i < g->numAircraft; i++) {
        if (!(g->ac[i].flags & AC_ALIVE)) continue;
        cam_ac_pose(&g->ac[i], alpha, &pos, &q);
        n += dot_one(c, &vp, pos, sw, sh);
    }
    ui_end();
    s_st.dots = n;
}

void scene_draw_loading(const char *text)
{
    int i;
    for (i = 0; i < 2; i++) {                       /* both buffers, so the message stays during the load */
        if (!r_begin_frame(0x0C1420)) { r_device_lost_check(); continue; }
        hg_begin();
        hg_text(hg_px(320.0f), hg_px(220.0f), HF_LARGE, hg_col(0xFF40FF60, 1.0f), 1, text);
        hg_text(hg_px(320.0f), hg_px(250.0f), HF_SMALL, hg_col(0xFF40FF60, 0.7f), 1, "ASHFALL  ISLAND INTERCEPT");
        hg_end();
        r_end_frame();
        r_present();
    }
}

/* ---------------------------------------------------------------- T17 */
void scene_light(u32 *sunRGB, u32 *ambRGB) { if (sunRGB) *sunRGB = s_sunRGB; if (ambRGB) *ambRGB = s_ambRGB; }

int scene_draw_wreck(const Camera *c, Vec3 pos, Quat q)
{
    Frustum fr;
    int tr;
    cam_frustum(c, &fr);
    r_set_proj(&c->proj);
    r_set_view(&c->view);
    s_railAc = -1;
    s_lodMul = s_wreckLod;                          /* T22: wrecks at coarser LODs */
    tr = draw_jet(c, &fr, pos, q, NULL, 0.0f, s_texAggr);
    s_lodMul = 1.0f;
    r_set_world(NULL);
    return tr;
}
