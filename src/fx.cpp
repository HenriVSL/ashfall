/* fx.cpp - effects (DESIGN 9.5). Windows/render side. Sections by owner:
     T08: afterburner flame (jet_flame.msh: unlit vertex colours, RB_ADD, no Z write, fog off, length by ab, random
          u offset per frame = flicker) and the nozzle glow billboard (glow_nozzle.bmp, additive) for every visible
          jet in afterburner (own jet in chase view only). Fog is off on additive effects, so they fade by distance
          by hand (fxFadeFrom..acMeshDist).
     T17: combat effects (trails, explosions, flares, smoke, debris) - add a section below.
     T22: polish section at the end (vapour, contrails, explosion layers, burning debris, smoke columns, cloud haze,
          camera shake source) + small hooks in the T08 / T17 code (flame shock diamonds, fx_reset, explosion_add,
          wreck impacts, culling + one ribbon helper for every trail).
   Render-only: own RNG, reads the interpolated aircraft state, never writes game state. */
#include "fx.h"
#include "scene.h"
#include "params.h"
#include "e_log.h"
#include "e_mesh.h"
#include "e_tex.h"
#include "e_rend.h"
#include "world.h"
#include "e_prof.h"

/* ================================================================ T08: afterburner flame + nozzle glow */
static Mesh  s_flame;
static int   s_flameOk;
static RTex  s_glowTex;
static RMaterial s_flameMat[4], s_glowMat;
static u32   s_rng = 0xF1A3Eu;
static float s_flameLen, s_flameGlow, s_glowSize, s_glowBase, s_fadeFrom;
static const Param s_fxTab[] = {
    { "fx", "flameLen", P_FLOAT, &s_flameLen, "1.0", 0, 3, "afterburner flame length multiplier" },
    { "fx", "flameBright", P_FLOAT, &s_flameGlow, "1.0", 0, 2, "afterburner flame brightness" },
    { "fx", "glowSize", P_FLOAT, &s_glowSize, "3.2", 0, 20, "nozzle glow billboard size at full afterburner (m)" },
    { "fx", "glowBase", P_FLOAT, &s_glowBase, "0.08", 0, 1, "nozzle glow brightness without afterburner (hot nozzle)" },
    { "fx", "fadeFrom", P_FLOAT, &s_fadeFrom, "1200", 0, 10000, "flame/glow start fading at this range (m), gone at acMeshDist" },
};
static int s_paramsAdded;

static float frand(void) { s_rng = s_rng * 1664525u + 1013904223u; return (float)((s_rng >> 8) & 0xFFFF) / 65536.0f; }

static void fx_cue_params_fwd(void);
static void draw_streaks(const Game *g, const Camera *c, float alpha);
static void fxc_init(void);                         /* T17 section */
static void fxc_free(void);
static void fxp_tick(const Game *g);                /* T22 section */
static void fxp_reset(void);
static void fxp_params(void);
static int  s_flameDiamonds = 1;                    /* T22 param [fx] flameDiamonds */
static float s_diamondK = 0.55f;
static int s_secFx = -1;                            /* T10b: e_prof section "fx" (bench report) */

int fx_init(void)
{
    int i;
    if (!s_paramsAdded) { params_add(s_fxTab, E_COUNT(s_fxTab)); s_paramsAdded = 1; }
    fx_cue_params_fwd();
    fxp_params();                                   /* T22 */
    s_flameOk = mesh_load(&s_flame, "jet_flame", 0);
    if (s_flameOk) {
        for (i = 0; i < s_flame.numMats && i < 4; i++) {
            s_flameMat[i] = s_flame.mats[i].rm;
            s_flameMat[i].blend = RB_ADD;
            s_flameMat[i].zWrite = 0;
            s_flameMat[i].zTest = 1;
            s_flameMat[i].fog = 0;
            s_flameMat[i].cullNone = 1;
        }
    }
    s_glowTex = tex_get("glow_nozzle", 0, RT_CLAMP | RT_NOMIP);
    memset(&s_glowMat, 0, sizeof(s_glowMat));
    s_glowMat.tex0 = s_glowTex;
    s_glowMat.blend = RB_ADD;
    s_glowMat.zTest = 1;
    s_glowMat.zWrite = 0;
    s_glowMat.fog = 0;
    s_glowMat.cullNone = 1;
    s_glowMat.clampUV = 1;
    log_printf(LOG_INFO, "fx: init flame=%d glow=%d", s_flameOk, s_glowTex != 0);
    fxc_init();                                     /* T17 */
    return 1;
}

/* fx_init registers [fx] lazily (main registers params before fx_init: -dumpini needs them earlier) */
void fx_params(void) { if (!s_paramsAdded) { params_add(s_fxTab, E_COUNT(s_fxTab)); s_paramsAdded = 1; } fx_cue_params_fwd(); fxp_params(); }

void fx_free(void)
{
    if (s_flameOk) mesh_free(&s_flame);
    if (s_glowTex) tex_release(s_glowTex);
    s_flameOk = 0;
    s_glowTex = 0;
    fxc_free();                                     /* T17 */
}

void fx_tick(const Game *g) { fx_combat_tick(g); fxp_tick(g); }   /* T17 combat effects, T22 polish */

static void draw_flame_one(const Camera *c, Vec3 pos, Quat q, float ab, float nozzle)
{
    static RVert1 v[256];
    static u16 idx[512];
    const Mesh *jet = scene_jet_mesh();
    const MeshTag *tn = jet ? mesh_tag(jet, "nozzle") : NULL;
    Vec3 np = tn ? tn->pos : v3(0.0f, 0.05f, -7.15f), wp;
    float d = v3_len(v3_sub(pos, c->eye)), fade, k;
    Mat3 r;
    Mat4 body, loc, w;
    int p, nv, ni, i, j;
    fade = 1.0f;
    {
        float meshDist = 2500.0f;
        const Param *pm = params_find("scene", "acMeshDist");
        if (pm) meshDist = *(const float *)pm->ptr;
        if (d > meshDist) return;
        if (d > s_fadeFrom) fade = 1.0f - (d - s_fadeFrom) / (meshDist - s_fadeFrom + 1.0f);
    }
    q_to_mat3(q, &r);
    m4_from_m3_pos(&body, &r, pos);
    r_set_world(NULL);
    /* flame cone: scale x/y with the nozzle, z by 0.3 + 0.7 ab (A1) */
    if (s_flameOk && ab > 0.02f) {
        float sxy = 1.0f + 0.18f * nozzle, sz = (0.3f + 0.7f * ab) * s_flameLen * (0.92f + 0.16f * frand());
        float uoff = frand(), bright = E_CLAMP(ab * s_flameGlow * fade, 0.0f, 1.0f);
        u32 tint;
        k = bright * 255.0f;
        tint = 0xFF000000u | ((u32)e_ftoi(k) << 16) | ((u32)e_ftoi(k) << 8) | (u32)e_ftoi(k);
        m4_ident(&loc);
        loc.m[0][0] = sxy; loc.m[1][1] = sxy; loc.m[2][2] = sz;
        loc.m[3][0] = np.x; loc.m[3][1] = np.y; loc.m[3][2] = np.z;
        m4_mul(&w, &loc, &body);
        for (p = 0; p < s_flame.lod[0].numParts; p++) {
            int part = s_flame.lod[0].firstPart + p, mat = s_flame.parts[part].mat;
            ni = 0;
            nv = mesh_append_part(&s_flame, 0, part, &w, tint, v, 256, idx, 512, 0, &ni);
            if (nv <= 0 || ni <= 0) continue;
            for (j = 0; j < nv; j++) v[j].u += uoff;
            r_set_material(&s_flameMat[mat >= 0 && mat < 4 ? mat : 0]);
            r_draw_mem(RFMT_V1, v, nv, idx, ni);
        }
    }
    /* nozzle glow billboard, a little behind the exit */
    if (s_glowTex) {
        float a = E_CLAMP((s_glowBase + (1.0f - s_glowBase) * ab) * fade, 0.0f, 1.0f), hs;
        Vec3 rr, uu;
        u32 col;
        if (a < 0.01f) return;
        wp = v3_add(pos, q_rotate(q, v3(np.x, np.y, np.z - 0.4f)));
        hs = 0.5f * s_glowSize * (0.55f + 0.45f * ab) * (0.95f + 0.1f * frand());
        rr = v3_scale(v3(c->view.m[0][0], c->view.m[1][0], c->view.m[2][0]), hs);
        uu = v3_scale(v3(c->view.m[0][1], c->view.m[1][1], c->view.m[2][1]), hs);
        k = a * 255.0f;
        col = 0xFF000000u | ((u32)e_ftoi(k) << 16) | ((u32)e_ftoi(k * 0.85f) << 8) | (u32)e_ftoi(k * 0.7f);
        nv = 1;
        if (s_flameDiamonds && s_flameOk && ab > 0.25f) nv = 4;   /* T22: 3 shock diamonds along the flame */
        for (p = 0; p < nv; p++) {
            Vec3 cp = wp, r2 = rr, u2 = uu;
            u32 cc = col;
            if (p > 0) {
                float sz = (0.3f + 0.7f * ab) * s_flameLen, kk = E_CLAMP((ab - 0.25f) / 0.5f, 0.0f, 1.0f) * fade * s_diamondK;
                float dk = (0.75f - 0.17f * (float)p) * (0.85f + 0.3f * frand());
                cp = v3_add(pos, q_rotate(q, v3(np.x, np.y, np.z - (0.55f + 0.85f * (float)p) * sz)));
                r2 = v3_scale(rr, (0.20f - 0.035f * (float)p) / (hs > 0.01f ? hs : 0.01f) * (1.0f + 0.18f * nozzle));
                u2 = v3_scale(uu, (0.20f - 0.035f * (float)p) / (hs > 0.01f ? hs : 0.01f) * (1.0f + 0.18f * nozzle));
                k = kk * dk * 255.0f;
                cc = 0xFF000000u | ((u32)e_ftoi(k) << 16) | ((u32)e_ftoi(k * 0.80f) << 8) | (u32)e_ftoi(k * 0.62f);
            }
            for (i = 0; i < 4; i++) {
                float sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? -1.0f : 1.0f;
                RVert1 *o = &v[p * 4 + i];
                o->x = cp.x + r2.x * sx + u2.x * sy; o->y = cp.y + r2.y * sx + u2.y * sy; o->z = cp.z + r2.z * sx + u2.z * sy;
                o->color = cc;
                o->u = (i & 1) ? 1.0f : 0.0f;
                o->v = (i & 2) ? 1.0f : 0.0f;
            }
            idx[p * 6 + 0] = (u16)(p * 4); idx[p * 6 + 1] = (u16)(p * 4 + 1); idx[p * 6 + 2] = (u16)(p * 4 + 2);
            idx[p * 6 + 3] = (u16)(p * 4 + 2); idx[p * 6 + 4] = (u16)(p * 4 + 1); idx[p * 6 + 5] = (u16)(p * 4 + 3);
        }
        r_set_material(&s_glowMat);
        r_draw_mem(RFMT_V1, v, nv * 4, idx, nv * 6);
    }
}

void fx_draw(const Game *g, const Camera *c, float alpha)
{
    int i;
    if (s_secFx < 0) s_secFx = prof_section("fx");
    prof_begin(s_secFx);
    r_set_proj(&c->proj);
    r_set_view(&c->view);
    for (i = 0; i < g->numAircraft; i++) {
        const Aircraft *a = &g->ac[i];
        Vec3 pos;
        Quat q;
        float ab, noz;
        if (!(a->flags & AC_ALIVE)) continue;
        if (i == PLAYER_ID && (c->mode != VIEW_CHASE || c->fixed)) continue;   /* own jet: chase view only */
        ab = a->prev.ab + (a->fs.ab - a->prev.ab) * alpha;
        noz = a->prev.vis.nozzle + (a->fs.vis.nozzle - a->prev.vis.nozzle) * alpha;
        cam_ac_pose(a, alpha, &pos, &q);
        draw_flame_one(c, pos, q, ab, noz);
    }
    draw_streaks(g, c, alpha);                      /* T10b */
    r_set_world(NULL);
    fx_combat_draw(g, c, alpha);                    /* T17 */
    prof_end(s_secFx);
}

/* ================================================================ T10b: speed cues (streaks) */
/* Air particles: static points in the air on a world lattice of period streakBoxM (deterministic hash), each shown
   at its copy nearest a box centre ahead of the camera, so they have true parallax and stream past at the jet's
   speed. Drawn as thin camera-facing streaks from the point forward along the flight path (length = V x
   streakLenS, the motion blur of one frame), alpha full at the point, 0 at the far end, faded near the camera and
   towards the box edge. Count by speed (streakFromKt..streakFullKt) with more near the ground (streakLowAltM,
   streakLowBoost). One draw, untextured, RB_BLEND, no Z write. Master switch [fx] speedCues (also camera FOV/buffet
   and the wind boost). Render only. */
enum { STREAK_MAX = 128 };
static int   s_speedCues, s_streaks;
static float s_stFromKt, s_stFullKt, s_stLowAlt, s_stLowBoost, s_stBox, s_stLen, s_stWidthPx, s_stAlpha, s_stNear;
static const Param s_cueTab[] = {
    { "fx", "speedCues", P_BOOL, &s_speedCues, "1", 0, 1, "speed sensation cues master switch: air streaks, speed FOV + buffet ([camera]), wind boost ([sound])" },
    { "fx", "streaks", P_INT, &s_streaks, "32", 0, 64, "air streak particles at full speed (doubled near the ground with streakLowBoost 1)" },
    { "fx", "streakFromKt", P_FLOAT, &s_stFromKt, "250", 0, 2000, "streaks start at this speed (kt)" },
    { "fx", "streakFullKt", P_FLOAT, &s_stFullKt, "900", 1, 2000, "full streak count at this speed (kt)" },
    { "fx", "streakLowAltM", P_FLOAT, &s_stLowAlt, "600", 1, 10000, "below this height above the ground/sea the count grows (m)" },
    { "fx", "streakLowBoost", P_FLOAT, &s_stLowBoost, "1.0", 0, 4, "extra count fraction at the ground (1 = double)" },
    { "fx", "streakBoxM", P_FLOAT, &s_stBox, "150", 20, 1000, "size of the particle box around the camera (m): smaller = denser and faster-looking" },
    { "fx", "streakLenS", P_FLOAT, &s_stLen, "0.030", 0, 0.2f, "streak length = speed x this (s)" },
    { "fx", "streakWidthPx", P_FLOAT, &s_stWidthPx, "1.6", 0.5, 8, "streak width on screen (px)" },
    { "fx", "streakAlpha", P_FLOAT, &s_stAlpha, "0.40", 0, 1, "streak opacity (white)" },
    { "fx", "streakNearM", P_FLOAT, &s_stNear, "8", 0, 100, "no streaks closer than this; fade in to 2x (m)" },
};
static int s_cueParams;
static RMaterial s_streakMat;
static RVert1 s_stv[STREAK_MAX * 4];
static u16 s_sti[STREAK_MAX * 6];
static int s_stDrawn;

void fx_cue_params(void)
{
    if (s_cueParams) return;
    params_add(s_cueTab, E_COUNT(s_cueTab));
    s_cueParams = 1;
    memset(&s_streakMat, 0, sizeof(s_streakMat));
    s_streakMat.blend = RB_BLEND;
    s_streakMat.zTest = 1;
    s_streakMat.zWrite = 0;
    s_streakMat.fog = 0;
    s_streakMat.cullNone = 1;
}

int fx_speed_cues(void) { return s_speedCues; }
int fx_streaks_drawn(void) { return s_stDrawn; }

static float hash01(u32 x)
{
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return (float)(x & 0xFFFFFF) / 16777216.0f;
}

static float smooth01(float a, float b, float x)
{
    float t = b > a ? E_CLAMP((x - a) / (b - a), 0.0f, 1.0f) : (x >= b ? 1.0f : 0.0f);
    return t * t * (3.0f - 2.0f * t);
}

static void draw_streaks(const Game *g, const Camera *c, float alpha)
{
    const Aircraft *a = &g->ac[PLAYER_ID];
    Vec3 pos, vel, f, ctr, vdir;
    Quat q;
    float V, kt, agl, k, P, pxk, halfBox;
    int i, n, nv = 0, ni = 0, sw = 0, sh = 0;
    s_stDrawn = 0;
    if (!s_speedCues || s_streaks <= 0 || c->fixed || !(a->flags & AC_ALIVE)) return;
    V = a->prev.V + (a->fs.V - a->prev.V) * alpha;
    kt = V * MS2KT;
    k = s_stFullKt > s_stFromKt ? (kt - s_stFromKt) / (s_stFullKt - s_stFromKt) : 1.0f;
    if (k <= 0.0f) return;
    if (k > 1.0f) k = 1.0f;
    cam_ac_pose(a, alpha, &pos, &q);
    agl = pos.y - (g->terrain ? world_surface(g, pos.x, pos.z) : 0.0f);
    k *= 1.0f + s_stLowBoost * (1.0f - smooth01(0.0f, s_stLowAlt, agl));
    n = (int)ceil((float)s_streaks * k);           /* the last one fades with the fraction (no popping) */
    if (n > STREAK_MAX) n = STREAK_MAX;
    if (n <= 0) return;
    r_screen_size(&sw, &sh);
    if (sw <= 0) return;
    vdir = q_rotate(q_slerp(a->prev.qPath, a->fs.qPath, alpha), v3(0.0f, 0.0f, 1.0f));
    vel = v3_scale(vdir, V);
    P = s_stBox;
    halfBox = 0.5f * P;
    f = v3(c->view.m[0][2], c->view.m[1][2], c->view.m[2][2]);
    ctr = v3_madd(c->eye, f, 0.45f * P);
    pxk = s_stWidthPx * 0.5f / (0.5f * (float)sw * (c->proj.m[0][0] > 0.01f ? c->proj.m[0][0] : 1.0f));   /* half width per m of depth */
    for (i = 0; i < n; i++) {
        Vec3 b, p, e, d, side, tail;
        float dist, fa, hw, depth;
        u32 col0, col1;
        b = v3(hash01((u32)i * 3u + 0x51u) * P, hash01((u32)i * 3u + 0x52u) * P, hash01((u32)i * 3u + 0x53u) * P);
        p.x = b.x + P * (float)floor((ctr.x - b.x) / P + 0.5f);
        p.y = b.y + P * (float)floor((ctr.y - b.y) / P + 0.5f);
        p.z = b.z + P * (float)floor((ctr.z - b.z) / P + 0.5f);
        e = v3_sub(p, c->eye);
        depth = v3_dot(e, f);
        if (depth < s_stNear) continue;
        dist = v3_len(e);
        fa = s_stAlpha * smooth01(s_stNear, 2.0f * s_stNear, dist) * (1.0f - smooth01(0.6f * halfBox, halfBox, v3_len(v3_sub(p, ctr))));
        fa *= E_CLAMP((float)s_streaks * k - (float)i, 0.0f, 1.0f);
        if (fa < 0.01f) continue;
        tail = v3_madd(p, vel, s_stLen);            /* where the point was relative to the camera one exposure ago */
        d = v3_sub(tail, p);
        side = v3_cross(d, e);
        if (v3_len(side) < 1e-4f) continue;
        hw = depth * pxk;
        side = v3_scale(v3_norm(side), hw);
        col0 = ((u32)e_ftoi(fa * 255.0f) << 24) | 0xF4F8FF;
        col1 = 0x00F4F8FF;
        {
            RVert1 *v = &s_stv[nv];
            v[0].x = p.x - side.x; v[0].y = p.y - side.y; v[0].z = p.z - side.z; v[0].color = col0;
            v[1].x = p.x + side.x; v[1].y = p.y + side.y; v[1].z = p.z + side.z; v[1].color = col0;
            v[2].x = tail.x - side.x; v[2].y = tail.y - side.y; v[2].z = tail.z - side.z; v[2].color = col1;
            v[3].x = tail.x + side.x; v[3].y = tail.y + side.y; v[3].z = tail.z + side.z; v[3].color = col1;
            v[0].u = v[0].v = v[1].u = v[1].v = v[2].u = v[2].v = v[3].u = v[3].v = 0.0f;
        }
        s_sti[ni + 0] = (u16)nv; s_sti[ni + 1] = (u16)(nv + 1); s_sti[ni + 2] = (u16)(nv + 2);
        s_sti[ni + 3] = (u16)(nv + 2); s_sti[ni + 4] = (u16)(nv + 1); s_sti[ni + 5] = (u16)(nv + 3);
        nv += 4; ni += 6;
    }
    if (!ni) return;
    r_set_world(NULL);
    r_set_material(&s_streakMat);
    r_draw_mem(RFMT_V1, s_stv, nv, s_sti, ni);
    s_stDrawn = ni / 6;
}

static void fx_cue_params_fwd(void) { fx_cue_params(); }

/* ================================================================ T17: combat effects (append here) */
/* DESIGN 9.5 within the 9.3 caps, A8 recipes (HANDOFF A8; sizes x EXPL_SCALE 1.5 = the arcade scale A8 recommends).
   Everything is render-only: spawned in fx_tick from the weapons / world event rings (own read indices, restart
   detection) and the mission counters, timed in SIM ticks (pause freezes them, replays look the same), drawn in
   fx_draw after the scene and the T08 flame. Own RNG (s_rng), never the sim's.
     trails      camera-facing ribbon per missile (keyed by MissileView.serial) from points sampled every 0.2 s while
                 the motor burns (dropSec + boostSec), head = the missile; width 0.8 -> 3.5 m (>= 2 px on screen),
                 alpha 0.85 (1 - age/life)^1.5 (life 7 s LRM / 5 s SRM), lit near-white, RB_BLEND fogged, within 6 km;
                 motor glow: flare sprite 1.4 m (>= 3 px) tinted, RB_ADD, fog off + manual fade
     explosions  fire billboards (fx_fire premul atlas: core + delayed satellites, 4-frame cross-fade, random roll and
                 mirror), smoke puffs (fx_smoke, rising, growing, darker -> lighter), flash (flare tex RB_ADD), debris
                 shards (keyed, gravity, spin); far explosions (core < 5 px) = one orange dot; near ones (< 400 m) a
                 short screen flash. Kill = full explosion + debris + a falling burning wreck (jet mesh tumbling,
                 fire + smoke trail, impact explosion / splash). Player hit = small burst + red screen flash.
                 Missile into the ground = small explosion, into the sea = water splash. Self-destruct = air burst.
     flares      flare sprite 4.5 m (>= 3 px), flicker +-15 %, fades over the last 0.4 s, + a short grey ribbon trail
     damaged     the player at hp <= damagedAt (mission): smoke puffs from the nozzle every 0.1 s
   Caps (9.3): Low 4 explosions / 16 trail points, High 8 / 24. */
#include "weapons.h"
#include "missile.h"
#include "mission.h"
#include "e_image.h"
#include "e_ui.h"
#include "e_terrain.h"

/* T22: the arcade scale is the param [fx] explScale (was EXPL_SCALE 1.5); FIRE_PER = the fire quads of a full
   explosion's first fireball, FIRE_MAX adds T22's secondary blooms; PUFF_MAX 256 for High (Low keeps T17's 128) */
static float s_explScale = 1.7f;
enum { TRAIL_MAX = 24, TRAIL_PTS = 24, EXPL_MAX = 8, FIRE_PER = 7, FIRE_MAX = 12, PUFF_MAX = 256, DEBRIS_MAX = 64, WRECK_MAX = 6,
       SPLASH_MAX = 8, FTR_PTS = 6, QB_MAX = 256 };
static int  s_puffCap = 128;
static int  fx_hi(void);                            /* T22: High preset? */
struct Expl;
static void fxp_explosion(struct Expl *e, float scale, int full);                    /* T22: layers of a new explosion */
static void fxp_column(const Game *g, Vec3 p, float scale);                         /* T22: wreck smoke column */
static void fxp_shake(Vec3 p, float amp);
static void fxp_draw_ribbons(const Game *g, const Camera *c, float alpha);
static void fxp_draw_shapes(const Game *g, const Camera *c, float alpha);
static void fxp_draw_fire(float alpha);
static void fxp_draw_wisps(void);
static void fxp_draw_haze(void);
static int  vis_code(Vec3 p, float r);
enum { PT_PTS_MAX = 50 };
static u32  lit(u32 rgb);                            /* T17 helper, used by puff_add since T22 */                           /* T22: longest ribbon (contrail 48 + head) */
typedef struct TrailPt { Vec3 p; u32 tick; float dist; } TrailPt;
typedef struct Trail { int on, type, n, seen, burning; u32 serial; TrailPt pt[TRAIL_PTS]; Vec3 head, headPrev; u32 lastTick; } Trail;
typedef struct FireQ { Vec3 off; float delay, life, size, roll; int mirror; } FireQ;
typedef struct Expl { int on, nFire; Vec3 p; u32 tick; float scale; FireQ f[FIRE_MAX]; } Expl;
typedef struct Puff { int on, cell, mirror; Vec3 p, vel; u32 tick; float delay, life, s0, s1, roll, a0; u32 c0, c1; float d;
                      float cr, sr; u32 l0, l1; } Puff;   /* T22: cos/sin of roll and the lit colours, set at spawn */
typedef struct Debris { int on, cell; Vec3 p, v; u32 tick; float life, size, spin, roll; } Debris;
typedef struct Wreck { int on; Vec3 p, v, pPrev; Quat q, qPrev; Vec3 spinAx; float spin; u32 tick, lastSmoke; } Wreck;
typedef struct Splash { int on, n; Vec3 p; u32 tick; float h[3], off[3]; } Splash;
typedef struct FTrail { int on; float lastAge; int n; Vec3 pt[FTR_PTS]; float ptAge[FTR_PTS]; } FTrail;

static Trail  s_tr[TRAIL_MAX];
static Expl   s_ex[EXPL_MAX];
static Puff   s_pf[PUFF_MAX];
static Debris s_db[DEBRIS_MAX];
static Wreck  s_wr[WRECK_MAX];
static Splash s_sp[SPLASH_MAX];
static FTrail s_ft[MAX_FLARES];
static int    s_order[PUFF_MAX];
static u32    s_cWpn, s_cWev;                       /* event read indices */
static int    s_cHits;                              /* mission playerHits seen */
static int    s_cDamaged; static u32 s_cLastDmgPuff;
static u32    s_flashTick, s_hitFlashTick, s_curTick;
static float  s_flashK;
static RTex   s_tFire, s_tSmoke, s_tTrail, s_tSplash, s_tFlare, s_tDebris, s_tDot;
static RMaterial s_mFire, s_mSmoke, s_mTrail, s_mSplash, s_mAdd, s_mDebris, s_mDot;
static RVert1 s_qv[QB_MAX * 4];
static u16    s_qi[QB_MAX * 6];
static int    s_qn;
static const RMaterial *s_qm;
static int    s_cOk;
static Vec3   s_camR, s_camU, s_camF, s_camEye;
static float  s_tanX = 0.7f, s_tanY = 0.53f, s_kX = 1.22f, s_kY = 1.13f;   /* T22: frustum test (tan half FOV, sphere k) */
static float  s_fogS = 7600.0f, s_fogE = 34200.0f, s_pxPerM1 = 1.0f;   /* px per metre at 1 m depth */
static u32    s_sunC = 0xFFFFF4E2, s_ambC = 0xFF8496B2;
static int    s_explCap = 4, s_trailPts = 16;
static int    s_cStats[4];                          /* last frame: trails, explosions, puffs, quads */

static RTex load_premul(const char *name)           /* colour + _a without bleed (A8, hud_font.cpp load_page) */
{
    char p1[E_PATH], p2[E_PATH], tn[48];
    EImage rgb, al;
    RTex t = 0;
    int i, ok = 0;
    e_snprintf(p1, (int)sizeof(p1), "tex/%s.bmp", name);
    e_snprintf(p2, (int)sizeof(p2), "tex/%s_a.bmp", name);
    if (!img_load(&rgb, p1, 0)) return 0;
    if (img_load(&al, p2, 0)) {
        if (al.w == rgb.w && al.h == rgb.h) {
            for (i = 0; i < rgb.w * rgb.h; i++) rgb.pix[0][i] = (rgb.pix[0][i] & 0xFFFFFFu) | ((al.pix[0][i] >> 16 & 0xFF) << 24);
            ok = 1;
        }
        img_free(&al);
    }
    rgb.flags = IMG_ALPHA;
    if (ok) { e_snprintf(tn, (int)sizeof(tn), "%s_premul", name); t = tex_add(tn, &rgb, RT_CLAMP | RT_NOMIP); }
    img_free(&rgb);
    return t;
}

static void mat_init(RMaterial *m, RTex t, int blend, int fog, int clamp)
{
    memset(m, 0, sizeof(*m));
    m->tex0 = t; m->blend = (u8)blend; m->zTest = 1; m->zWrite = 0; m->fog = (u8)fog; m->cullNone = 1; m->clampUV = (u8)clamp;
}

static void fxc_init(void)
{
    int i;
    s_tFire = load_premul("fx_fire");
    s_tSmoke = tex_get("fx_smoke", IMG_ALPHA, RT_CLAMP);
    s_tTrail = tex_get("trail", IMG_ALPHA, 0);
    s_tSplash = tex_get("splash", IMG_ALPHA, RT_CLAMP);
    s_tFlare = tex_get("flare", 0, RT_CLAMP);
    s_tDebris = tex_get("debris", IMG_KEYED, RT_CLAMP);
    s_tDot = tex_get("dot", 0, RT_CLAMP);
    mat_init(&s_mFire, s_tFire, RB_PREMUL, 0, 1);
    mat_init(&s_mSmoke, s_tSmoke, RB_BLEND, 1, 1);
    mat_init(&s_mTrail, s_tTrail, RB_BLEND, 1, 0);
    mat_init(&s_mSplash, s_tSplash, RB_BLEND, 1, 1);
    mat_init(&s_mAdd, s_tFlare, RB_ADD, 0, 1);
    mat_init(&s_mDebris, s_tDebris, RB_ALPHATEST, 1, 1);
    mat_init(&s_mDot, s_tDot, RB_ADD, 0, 1);
    for (i = 0; i < QB_MAX; i++) {
        s_qi[i * 6 + 0] = (u16)(i * 4); s_qi[i * 6 + 1] = (u16)(i * 4 + 1); s_qi[i * 6 + 2] = (u16)(i * 4 + 2);
        s_qi[i * 6 + 3] = (u16)(i * 4 + 2); s_qi[i * 6 + 4] = (u16)(i * 4 + 1); s_qi[i * 6 + 5] = (u16)(i * 4 + 3);
    }
    s_cOk = 1;
    log_printf(LOG_INFO, "fx: combat init fire=%d smoke=%d trail=%d splash=%d flare=%d debris=%d dot=%d", s_tFire != 0, s_tSmoke != 0,
               s_tTrail != 0, s_tSplash != 0, s_tFlare != 0, s_tDebris != 0, s_tDot != 0);
}

static void fxc_free(void)
{
    if (s_tSmoke) tex_release(s_tSmoke);
    if (s_tTrail) tex_release(s_tTrail);
    if (s_tSplash) tex_release(s_tSplash);
    if (s_tFlare) tex_release(s_tFlare);
    if (s_tDebris) tex_release(s_tDebris);
    if (s_tDot) tex_release(s_tDot);
    s_tFire = s_tSmoke = s_tTrail = s_tSplash = s_tFlare = s_tDebris = s_tDot = 0;
    s_cOk = 0;
}

void fx_reset(void)
{
    memset(s_tr, 0, sizeof(s_tr)); memset(s_ex, 0, sizeof(s_ex)); memset(s_pf, 0, sizeof(s_pf)); memset(s_db, 0, sizeof(s_db));
    memset(s_wr, 0, sizeof(s_wr)); memset(s_sp, 0, sizeof(s_sp)); memset(s_ft, 0, sizeof(s_ft));
    s_cWpn = weapons_event_count();
    s_cWev = world_event_seq();
    s_cHits = mission_stats()->playerHits;
    s_cDamaged = 0;
    s_flashTick = s_hitFlashTick = 0;
    fxp_reset();                                    /* T22 */
}

static float frange(float a, float b) { return a + (b - a) * frand(); }
static float ages(u32 t0) { return s_curTick >= t0 ? (float)(s_curTick - t0) / (float)SIM_HZ : 0.0f; }

/* ---------------------------------------------------------------- spawning */
static void puff_add(Vec3 p, Vec3 vel, float delay, float life, float s0, float s1, u32 c0, u32 c1, float a0, int cell)
{
    int i, best = 0;
    float oldest = -1.0f;
    for (i = 0; i < s_puffCap && i < PUFF_MAX; i++) {
        float a;
        if (!s_pf[i].on) { best = i; break; }
        a = ages(s_pf[i].tick) / (s_pf[i].life + s_pf[i].delay);
        if (a > oldest) { oldest = a; best = i; }
    }
    s_pf[best].on = 1; s_pf[best].p = p; s_pf[best].vel = vel; s_pf[best].tick = s_curTick; s_pf[best].delay = delay;
    s_pf[best].life = life; s_pf[best].s0 = s0; s_pf[best].s1 = s1; s_pf[best].c0 = c0; s_pf[best].c1 = c1; s_pf[best].a0 = a0;
    s_pf[best].cell = cell; s_pf[best].mirror = frand() < 0.5f; s_pf[best].roll = frange(-0.44f, 0.44f);
    s_pf[best].cr = (float)cos(s_pf[best].roll); s_pf[best].sr = (float)sin(s_pf[best].roll);
    s_pf[best].l0 = lit(c0); s_pf[best].l1 = lit(c1);
}

static void explosion_add(Vec3 p, float scale, int fire, int smoke, int debris, float flash)
{
    int i, best = -1;
    Expl *e;
    float k = s_explScale * scale;
    for (i = 0; i < s_explCap && i < EXPL_MAX; i++) if (!s_ex[i].on) { best = i; break; }
    if (best < 0) {                                 /* cap: replace the oldest */
        u32 old = 0xFFFFFFFFu;
        for (i = 0; i < s_explCap && i < EXPL_MAX; i++) if (s_ex[i].tick < old) { old = s_ex[i].tick; best = i; }
    }
    if (best < 0) return;
    e = &s_ex[best];
    memset(e, 0, sizeof(*e));
    e->on = 1; e->p = p; e->tick = s_curTick; e->scale = k;
    e->nFire = fire > FIRE_PER ? FIRE_PER : fire;
    for (i = 0; i < e->nFire; i++) {
        FireQ *f = &e->f[i];
        if (i == 0) { f->off = v3(0.0f, 0.0f, 0.0f); f->delay = 0.0f; f->size = 9.0f * k; }
        else {
            Vec3 d = v3_norm(v3(frange(-1.0f, 1.0f), frange(-0.6f, 1.0f), frange(-1.0f, 1.0f)));
            f->off = v3_scale(d, frange(3.0f, 6.0f) * k);
            f->delay = frange(0.02f, 0.25f);
            f->size = frange(5.0f, 8.0f) * k;
        }
        f->life = frange(1.1f, 1.5f);
        f->roll = frange(0.0f, 6.2831853f);
        f->mirror = frand() < 0.5f;
    }
    for (i = 0; i < smoke; i++) {                   /* smoke: 0.25-0.6 s in, life 5-7 s, 8 -> 30 m, rising 3 m/s, drifting out */
        Vec3 d = v3_norm(v3(frange(-1.0f, 1.0f), frange(0.0f, 0.8f), frange(-1.0f, 1.0f)));
        puff_add(v3_madd(p, d, 2.0f * k), v3_add(v3_scale(d, 2.0f * k), v3(0.0f, 3.0f, 0.0f)), frange(0.25f, 0.6f), frange(5.0f, 7.0f),
                 8.0f * scale * s_explScale * 0.67f, 30.0f * scale * s_explScale * 0.67f, 0x342F2C, 0x767472, 0.88f, i & 1);
    }
    for (i = 0; i < debris; i++) {
        int j;
        for (j = 0; j < DEBRIS_MAX && s_db[j].on; j++) {}
        if (j >= DEBRIS_MAX) break;
        s_db[j].on = 1; s_db[j].p = p; s_db[j].tick = s_curTick;
        s_db[j].v = v3_scale(v3_norm(v3(frange(-1.0f, 1.0f), frange(-0.3f, 1.0f), frange(-1.0f, 1.0f))), frange(25.0f, 55.0f));
        s_db[j].life = frange(2.0f, 3.5f); s_db[j].size = frange(0.6f, 1.6f); s_db[j].spin = frange(-12.6f, 12.6f);
        s_db[j].roll = frange(0.0f, 6.28f); s_db[j].cell = (int)(frand() * 15.99f);
    }
    if (flash > 0.0f) {
        float d = v3_len(v3_sub(p, s_camEye));
        if (d < 400.0f) { s_flashTick = s_curTick; s_flashK = flash * (1.0f - d / 400.0f); }
    }
    fxp_explosion(e, scale, fire >= FIRE_PER && debris >= 8);   /* T22: blooms, fire -> smoke, burning debris */
    fxp_shake(p, scale * (fire >= FIRE_PER ? 1.0f : 0.5f));
}

static void splash_add(Vec3 p, float scale)
{
    int i, j;
    for (i = 0; i < SPLASH_MAX && s_sp[i].on; i++) {}
    if (i >= SPLASH_MAX) i = 0;
    s_sp[i].on = 1; s_sp[i].p = v3(p.x, 0.0f, p.z); s_sp[i].tick = s_curTick; s_sp[i].n = scale > 0.7f ? 3 : 2;
    for (j = 0; j < 3; j++) { s_sp[i].h[j] = frange(26.0f, 38.0f) * scale; s_sp[i].off[j] = frange(-6.0f, 6.0f) * scale; }
    puff_add(v3(p.x, 6.0f * scale, p.z), v3(0.0f, 4.0f, 0.0f), 0.2f, 3.0f, 10.0f * scale, 30.0f * scale, 0xF4F4F4, 0xFFFFFF, 0.6f, 2);
}

static int over_sea(const Game *g, Vec3 p)
{
    return !g->terrain || terrain_height(g->terrain, p.x, p.z) < 0.5f;
}

/* impact on the surface: splash at sea, explosion on land */
static void impact(const Game *g, Vec3 p, float scale)
{
    if (over_sea(g, p)) splash_add(p, scale);
    else explosion_add(v3(p.x, world_surface(g, p.x, p.z) + 2.0f, p.z), 0.6f * scale, 4, 3, 0, 0.6f);
}

static void wreck_add(const Aircraft *a)
{
    int i;
    for (i = 0; i < WRECK_MAX && s_wr[i].on; i++) {}
    if (i >= WRECK_MAX) return;
    s_wr[i].on = 1; s_wr[i].p = s_wr[i].pPrev = a->fs.pos;
    s_wr[i].v = q_rotate(a->fs.qPath, v3(0.0f, 0.0f, a->fs.V * 0.6f));
    s_wr[i].q = s_wr[i].qPrev = a->fs.qPath;
    s_wr[i].spinAx = v3_norm(v3(frange(-0.3f, 0.3f), frange(-0.2f, 0.2f), 1.0f));
    s_wr[i].spin = frange(-4.0f, 4.0f);
    s_wr[i].tick = s_wr[i].lastSmoke = s_curTick;
}

static void trails_tick(const Game *g)
{
    static MissileView mv[MAX_MISSILES];
    int n = weapons_missiles(mv, MAX_MISSILES), i, k;
    for (k = 0; k < TRAIL_MAX; k++) s_tr[k].seen = 0;
    for (i = 0; i < n; i++) {
        const MissileView *m = &mv[i];
        const MissileParams *mp = missile_params_of(m->type);
        Trail *t = NULL;
        int burning = mp ? m->age < mp->dropSec + mp->boostSec : m->age < 3.0f;
        for (k = 0; k < TRAIL_MAX; k++) if (s_tr[k].on && s_tr[k].serial == m->serial) { t = &s_tr[k]; break; }
        if (!t) {
            if (!burning) continue;
            for (k = 0; k < TRAIL_MAX && s_tr[k].on; k++) {}
            if (k >= TRAIL_MAX) {                   /* replace the trail with the oldest last point */
                u32 old = 0xFFFFFFFFu; int b = 0;
                for (k = 0; k < TRAIL_MAX; k++) if (s_tr[k].lastTick < old) { old = s_tr[k].lastTick; b = k; }
                k = b;
            }
            t = &s_tr[k];
            memset(t, 0, sizeof(*t));
            t->on = 1; t->serial = m->serial; t->type = m->type;
        }
        t->seen = 1;
        t->burning = burning && (mp ? m->age > mp->dropSec : 1);
        t->head = m->pos; t->headPrev = m->prevPos;
        if (t->burning && (t->n == 0 || g->tick - t->lastTick >= 24u)) {
            TrailPt *pt;
            if (t->n >= s_trailPts) { memmove(&t->pt[0], &t->pt[1], (size_t)(t->n - 1) * sizeof(TrailPt)); t->n--; }
            pt = &t->pt[t->n];
            pt->p = m->pos;
            pt->tick = g->tick;
            pt->dist = t->n ? t->pt[t->n - 1].dist + v3_len(v3_sub(m->pos, t->pt[t->n - 1].p)) : 0.0f;
            t->n++;
            t->lastTick = g->tick;
        }
    }
    for (k = 0; k < TRAIL_MAX; k++) {
        Trail *t = &s_tr[k];
        float life = t->type == MSL_SRM ? 5.0f : 7.0f;
        if (!t->on) continue;
        if (!t->seen) t->burning = 0;
        while (t->n > 0 && (float)(g->tick - t->pt[0].tick) / (float)SIM_HZ > life) {
            memmove(&t->pt[0], &t->pt[1], (size_t)(t->n - 1) * sizeof(TrailPt));
            t->n--;
        }
        if (t->n == 0 && !t->seen) t->on = 0;
    }
}

static void flares_tick(const Game *g)
{
    static FlareView fv[MAX_FLARES];
    int n = weapons_flares_view(fv, MAX_FLARES), i, k, seen[MAX_FLARES];
    (void)g;
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < n; i++) {
        FTrail *f;
        if (fv[i].slot < 0 || fv[i].slot >= MAX_FLARES) continue;
        f = &s_ft[fv[i].slot];
        seen[fv[i].slot] = 1;
        if (!f->on || fv[i].age < f->lastAge) { memset(f, 0, sizeof(*f)); f->on = 1; }
        if (f->n == 0 || fv[i].age - f->ptAge[f->n - 1] >= 0.2f) {
            if (f->n >= FTR_PTS) { memmove(&f->pt[0], &f->pt[1], (FTR_PTS - 1) * sizeof(Vec3)); memmove(&f->ptAge[0], &f->ptAge[1], (FTR_PTS - 1) * sizeof(float)); f->n--; }
            f->pt[f->n] = fv[i].pos; f->ptAge[f->n] = fv[i].age; f->n++;
        }
        f->lastAge = fv[i].age;
    }
    for (k = 0; k < MAX_FLARES; k++) if (!seen[k]) s_ft[k].on = 0;
}

static void wrecks_tick(const Game *g)
{
    int i;
    for (i = 0; i < WRECK_MAX; i++) {
        Wreck *w = &s_wr[i];
        float surf;
        if (!w->on) continue;
        w->pPrev = w->p; w->qPrev = w->q;
        w->v = v3_scale(w->v, 1.0f - 0.25f * SIM_DT);
        w->v.y -= GRAV * SIM_DT;
        w->p = v3_madd(w->p, w->v, SIM_DT);
        w->q = q_norm(q_mul(w->q, q_axis_angle(w->spinAx, w->spin * SIM_DT)));
        if (s_curTick - w->lastSmoke >= 14u) {      /* burning smoke trail */
            w->lastSmoke = s_curTick;
            puff_add(w->p, v3(0.0f, 1.0f, 0.0f), 0.0f, frange(3.0f, 4.5f), 5.0f, 16.0f, 0x2C2826, 0x5A5856, 0.8f, (int)(frand() * 1.99f));
        }
        surf = world_surface(g, w->p.x, w->p.z);
        if (w->p.y <= surf + 1.0f || ages(w->tick) > 40.0f) {
            impact(g, v3(w->p.x, surf, w->p.z), 1.0f);
            fxp_column(g, v3(w->p.x, surf, w->p.z), 1.0f);   /* T22: burning wreck smoke column */
            w->on = 0;
        }
    }
}

void fx_combat_tick(const Game *g)
{
    WorldEvent we;
    const MissionStats *ms = mission_stats();
    const Aircraft *pl = &g->ac[PLAYER_ID];
    s_curTick = g->tick;
    if (s_cWpn > weapons_event_count()) s_cWpn = 0;            /* a new session reset the rings */
    if (s_cWev > world_event_seq()) s_cWev = 0;
    if (ms->playerHits < s_cHits) s_cHits = 0;
    while (s_cWpn < weapons_event_count()) {
        const WpnEvent *e = weapons_event(s_cWpn++);
        if (!e) continue;
        if (e->type == WPE_HIT && e->victim > 0 && e->victim < g->numAircraft) {   /* enemy destroyed (one hit) */
            explosion_add(e->pos, 1.0f, FIRE_PER, 4, 10, 1.0f);
            wreck_add(&g->ac[e->victim]);
        } else if (e->type == WPE_GROUND) {
            impact(g, e->pos, 0.5f);
        } else if (e->type == WPE_SELFDESTRUCT || e->type == WPE_FLARE_HIT) {
            explosion_add(e->pos, 0.35f, 3, 2, 0, 0.3f);
        }
    }
    if (ms->playerHits != s_cHits) {                /* the player took a hit (mission applied it) */
        s_cHits = ms->playerHits;
        if (pl->flags & AC_ALIVE) { explosion_add(pl->fs.pos, 0.4f, 3, 2, 3, 0.0f); s_hitFlashTick = s_curTick; fxp_shake(pl->fs.pos, 1.2f); }
        else explosion_add(pl->fs.pos, 1.2f, FIRE_PER, 4, 10, 1.0f);    /* killed */
    }
    while (s_cWev < world_event_seq()) {
        if (!world_event_get(s_cWev++, &we) || !(we.type & WEV_CRASH_ANY)) continue;
        if (we.type == WEV_CRASH_SEA) splash_add(we.pos, 1.2f);
        else explosion_add(v3_add(we.pos, v3(0.0f, 3.0f, 0.0f)), 1.0f, FIRE_PER, 4, 8, 1.0f);
        fxp_column(g, we.pos, 1.0f);                /* T22: crash site smoke column */
    }
    trails_tick(g);
    flares_tick(g);
    wrecks_tick(g);
    /* damaged: smoke from the nozzle every 4 ticks. The puffs keep 85 % of the jet's velocity (arcade: smoke left
       still in the air passes the chase camera 24 m behind in 0.1 s at 500 kt and is never seen), so the plume
       streams back ~50 m over its 1.2 s */
    if ((ms->damaged || pl->hp <= 50.0f) && (pl->flags & AC_ALIVE) && s_curTick - s_cLastDmgPuff >= 4u) {   /* hp: bench */
        Vec3 nz = v3_add(pl->fs.pos, q_rotate(pl->fs.qPath, v3(0.0f, 0.0f, -7.6f)));
        s_cLastDmgPuff = s_curTick;
        puff_add(nz, v3_scale(q_rotate(pl->fs.qPath, v3(0.0f, 0.0f, 1.0f)), 0.85f * pl->fs.V), 0.0f, 1.2f, 2.5f, 9.0f, 0x504E4C, 0x6A6866,
                 0.75f, (int)(frand() * 1.99f));
    }
}

/* ---------------------------------------------------------------- drawing helpers */
static void qb_flush(void)
{
    if (s_qn && s_qm) {
        r_set_material(s_qm);
        r_draw_mem(RFMT_V1, s_qv, s_qn * 4, s_qi, s_qn * 6);
        s_cStats[3] += s_qn;
    }
    s_qn = 0;
}
static void qb_mat(const RMaterial *m) { if (m != s_qm) { qb_flush(); s_qm = m; } }

/* quad from 4 corners (0 1 / 2 3 order) */
static void qb_quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, float u0, float v0, float u1, float v1, u32 ca, u32 cb, u32 cc, u32 cd)
{
    RVert1 *v;
    if (s_qn >= QB_MAX) qb_flush();
    v = &s_qv[s_qn * 4];
    v[0].x = a.x; v[0].y = a.y; v[0].z = a.z; v[0].u = u0; v[0].v = v0; v[0].color = ca;
    v[1].x = b.x; v[1].y = b.y; v[1].z = b.z; v[1].u = u1; v[1].v = v0; v[1].color = cb;
    v[2].x = c.x; v[2].y = c.y; v[2].z = c.z; v[2].u = u0; v[2].v = v1; v[2].color = cc;
    v[3].x = d.x; v[3].y = d.y; v[3].z = d.z; v[3].u = u1; v[3].v = v1; v[3].color = cd;
    s_qn++;
}

/* camera-facing billboard: centre, half size, roll (rad), atlas cell (-1 = whole texture), mirror, colour */
static void qb_bill_cs(Vec3 p, float hs, float cr, float sr, int cell, int mirror, u32 col)   /* T22: roll as cos/sin */
{
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f, t;
    Vec3 r = v3_scale(v3_add(v3_scale(s_camR, cr), v3_scale(s_camU, sr)), hs);
    Vec3 u = v3_scale(v3_sub(v3_scale(s_camU, cr), v3_scale(s_camR, sr)), hs);
    if (cell >= 0) {
        u0 = (float)(cell & 1) * 0.5f + 0.004f; v0 = (float)(cell >> 1) * 0.5f + 0.004f;
        u1 = u0 + 0.492f; v1 = v0 + 0.492f;
    }
    if (mirror) { t = u0; u0 = u1; u1 = t; }
    qb_quad(v3_add(v3_sub(p, r), u), v3_add(v3_add(p, r), u), v3_sub(v3_sub(p, r), u), v3_sub(v3_add(p, r), u), u0, v0, u1, v1, col, col, col, col);
}

static void qb_bill(Vec3 p, float hs, float roll, int cell, int mirror, u32 col)
{
    if (vis_code(p, 1.42f * hs)) return;            /* T22: off screen (before the trig) */
    qb_bill_cs(p, hs, (float)cos(roll), (float)sin(roll), cell, mirror, col);
}

static float depth_of(Vec3 p) { return v3_dot(v3_sub(p, s_camEye), s_camF); }
static float fog_fade(Vec3 p)                       /* manual fog for fog-off (premul/add) effects */
{
    float w = depth_of(p), t;
    if (w <= s_fogS) return 1.0f;
    if (w >= s_fogE) return 0.0f;
    t = (w - s_fogS) / (s_fogE - s_fogS);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}
static float min_px(Vec3 p, float px) { float w = depth_of(p); return w > 0.1f ? px * 0.5f * w / s_pxPerM1 : 0.0f; }   /* half size for px */

static u32 premul_col(float k)
{
    u32 b = (u32)e_ftoi(E_CLAMP(k, 0.0f, 1.0f) * 255.0f);
    return (b << 24) | (b << 16) | (b << 8) | b;
}
static u32 rgb_scale(u32 rgb, float k, float a)     /* straight colour x k, alpha a */
{
    u32 r = (u32)e_ftoi(E_CLAMP((float)((rgb >> 16) & 255) * k, 0.0f, 255.0f));
    u32 g = (u32)e_ftoi(E_CLAMP((float)((rgb >> 8) & 255) * k, 0.0f, 255.0f));
    u32 b = (u32)e_ftoi(E_CLAMP((float)(rgb & 255) * k, 0.0f, 255.0f));
    return ((u32)e_ftoi(E_CLAMP(a, 0.0f, 1.0f) * 255.0f) << 24) | (r << 16) | (g << 8) | b;
}
static u32 lerp_rgb(u32 a, u32 b, float t)
{
    int i;
    u32 o = 0;
    for (i = 0; i < 24; i += 8) {
        float x = (float)((a >> i) & 255), y = (float)((b >> i) & 255);
        o |= (u32)e_ftoi(x + (y - x) * t) << i;
    }
    return o;
}
static u32 lit(u32 rgb)                             /* smoke/trail light: base x (ambient 0.75 + sun 0.55) / 1.05 */
{
    int i;
    u32 o = 0;
    for (i = 0; i < 24; i += 8) {
        float l = ((float)((s_ambC >> i) & 255) * 0.75f + (float)((s_sunC >> i) & 255) * 0.55f) / (1.05f * 255.0f);
        o |= (u32)e_ftoi(E_CLAMP((float)((rgb >> i) & 255) * l, 0.0f, 255.0f)) << i;
    }
    return o;
}

/* T22: view-frustum out-code of a sphere (0 = maybe visible): billboards and ribbon segments that are wholly off
   screen are not built (the backend would transform their vertices and only then reject them) */
static int vis_code(Vec3 p, float r)
{
    Vec3 e = v3_sub(p, s_camEye);
    float z = v3_dot(e, s_camF), x = v3_dot(e, s_camR), y = v3_dot(e, s_camU);
    int code = 0;
    if (z < -r) code |= 1;
    if (x > z * s_tanX + r * s_kX) code |= 2;
    if (x < -z * s_tanX - r * s_kX) code |= 4;
    if (y > z * s_tanY + r * s_kY) code |= 8;
    if (y < -z * s_tanY - r * s_kY) code |= 16;
    return code;
}

/* camera-facing ribbon through n points (oldest first) + head; width / alpha per point from age.
   T22: ka = per-point alpha factor (NULL = 1), near fade from nearA to nearB metres (T17 trails 15..80), fadeIn = alpha
   ramp over the first seconds of a point's age (0 = none), minPx = minimum width on screen; segments wholly off
   screen are skipped. alpha = a0 * ka * (1 - age/life)^1.5 (x the end-on and near fades). */
static void ribbon2(const Vec3 *p, const float *age, const float *ka, int n, float life, float w0, float w1, float tauW, u32 rgb,
                    float a0, float vScale, const float *dist, float nearA, float nearB, float fadeIn, float minPx)
{
    static int cc[PT_PTS_MAX + 2];
    int i, prevCode = 0, prevOk = 0;
    Vec3 prevL = v3(0.0f, 0.0f, 0.0f), prevR = prevL;
    u32 prevC = 0;
    float prevV = 0.0f, rc = 0.5f * E_MAXF(w0, w1);
    if (n < 2) return;
    if (n > PT_PTS_MAX + 2) n = PT_PTS_MAX + 2;
    for (i = 0; i < n; i++) cc[i] = vis_code(p[i], rc + min_px(p[i], minPx));   /* T22: cheap conservative pass */
    for (i = 0; i < n; i++) {
        if ((i == 0 || (cc[i] & cc[i - 1])) && (i + 1 >= n || (cc[i] & cc[i + 1]))) { prevOk = 0; continue; }   /* both quads off screen */
        Vec3 t = i + 1 < n ? v3_sub(p[i + 1], p[i]) : v3_sub(p[i], p[i - 1]), e = v3_sub(p[i], s_camEye), side;
        float a = age[i] / life, w, hw, al, v, om;
        u32 col;
        int code;
        if (a > 1.0f) a = 1.0f;
        if (a < 0.0f) a = 0.0f;
        w = w0 + (w1 - w0) * (1.0f - (float)exp(-age[i] / tauW));
        hw = E_MAXF(0.5f * w, min_px(p[i], minPx));
        om = 1.0f - a;
        al = a0 * om * (float)sqrt(om);
        if (ka) al *= ka[i];
        if (fadeIn > 0.0f && age[i] < fadeIn) al *= age[i] > 0.0f ? age[i] / fadeIn : 0.0f;
        side = v3_cross(t, e);
        {   /* seen end-on, many segments stack (flying through the own launch smoke): fade by the view angle; and fade
               out near the camera */
            float tl = v3_len(t), el = v3_len(e), sn = (tl > 1e-3f && el > 1e-3f) ? v3_len(side) / (tl * el) : 1.0f;
            al *= E_CLAMP(0.15f + 1.7f * sn, 0.0f, 1.0f) * E_CLAMP((el - nearA) / (nearB - nearA > 0.1f ? nearB - nearA : 0.1f), 0.0f, 1.0f);
        }
        if (v3_len(side) < 1e-4f) side = s_camR;
        side = v3_scale(v3_norm(side), hw);
        col = rgb_scale(rgb, 1.0f, al);
        v = dist ? dist[i] / vScale : (float)i * 0.25f;
        code = vis_code(p[i], hw);
        if (i > 0 && prevOk && !(code & prevCode) && (al > 0.004f || (prevC >> 24) > 0))
            qb_quad(prevL, prevR, v3_sub(p[i], side), v3_add(p[i], side), 0.0f, prevV, 1.0f, v, prevC, prevC, col, col);
        prevL = v3_sub(p[i], side); prevR = v3_add(p[i], side); prevC = col; prevV = v; prevCode = code; prevOk = 1;
    }
}

static void ribbon(const Vec3 *p, const float *age, int n, float life, float w0, float w1, float tauW, u32 rgb, float a0, float vScale,
                   const float *dist)
{
    ribbon2(p, age, NULL, n, life, w0, w1, tauW, rgb, a0, vScale, dist, 15.0f, 80.0f, 0.0f, 2.0f);
}

static void draw_trails(float alpha)
{
    static Vec3 pts[TRAIL_PTS + 1];
    static float ag[TRAIL_PTS + 1], ds[TRAIL_PTS + 1];
    int k, i, n;
    u32 col = lit(0xF0F0EC);
    qb_mat(&s_mTrail);
    for (k = 0; k < TRAIL_MAX; k++) {
        const Trail *t = &s_tr[k];
        float life = t->type == MSL_SRM ? 5.0f : 7.0f, nearest = 1e9f;
        if (!t->on || t->n == 0) continue;
        for (i = 0, n = 0; i < t->n; i++) {
            pts[n] = t->pt[i].p;
            ag[n] = ((float)s_curTick + alpha - (float)t->pt[i].tick) / (float)SIM_HZ;
            ds[n] = t->pt[i].dist;
            nearest = E_MINF(nearest, v3_len(v3_sub(pts[n], s_camEye)));
            n++;
        }
        if (t->burning && t->seen) {                /* head = the missile now */
            Vec3 h = v3_lerp(t->headPrev, t->head, alpha);
            pts[n] = h; ag[n] = 0.0f; ds[n] = ds[n - 1] + v3_len(v3_sub(h, pts[n - 1])); n++;
        }
        if (nearest > 6000.0f) continue;               /* DESIGN 9.1: trails within 6 km */
        ribbon(pts, ag, n, life, 0.8f, 3.5f, 1.2f, col, 0.85f, 12.0f, ds);
        s_cStats[0]++;
    }
}

static void draw_flare_trails(void)
{
    static float ag[FTR_PTS];
    int k, i;
    u32 col = lit(0xC8C8C8);
    qb_mat(&s_mTrail);
    for (k = 0; k < MAX_FLARES; k++) {
        const FTrail *f = &s_ft[k];
        if (!f->on || f->n < 2) continue;
        for (i = 0; i < f->n; i++) ag[i] = f->lastAge - f->ptAge[i];
        ribbon(f->pt, ag, f->n, 1.2f, 0.6f, 2.8f, 0.5f, col, 0.8f, 6.0f, NULL);
    }
}

static int puff_cmp_insert(int n, int idx)
{
    int j;
    for (j = n; j > 0 && s_pf[s_order[j - 1]].d < s_pf[idx].d; j--) s_order[j] = s_order[j - 1];
    s_order[j] = idx;
    return n + 1;
}

static void draw_puffs(void)
{
    int i, n = 0;
    for (i = 0; i < PUFF_MAX; i++) {
        Puff *p = &s_pf[i];
        float a;
        if (!p->on) continue;
        a = ages(p->tick);
        if (a > p->delay + p->life) { p->on = 0; continue; }
        if (a < p->delay) continue;
        { Vec3 e = v3_sub(p->p, s_camEye); p->d = v3_dot(e, e); }   /* T22: squared distance (sort key) */
        n = puff_cmp_insert(n, i);
    }
    qb_mat(&s_mSmoke);
    for (i = 0; i < n; i++) {
        const Puff *p = &s_pf[s_order[i]];
        float t = ages(p->tick) - p->delay, u = t / p->life, g = 1.0f - (1.0f - u) * (1.0f - u), size, al;
        Vec3 pos = v3_madd(p->p, p->vel, t);
        size = p->s0 + (p->s1 - p->s0) * g;
        if (vis_code(pos, 0.71f * size)) continue;  /* T22: cull before the colour math */
        al = p->a0 * E_CLAMP(t / 0.35f, 0.0f, 1.0f) * (1.0f - u) * (float)sqrt(sqrt(1.0f - u));   /* T22: (1-u)^1.25, was pow 1.3 */
        if (p->d < (2.2f * size + 2.0f) * (2.2f * size + 2.0f))   /* fade out when the camera is inside it */
            al *= E_CLAMP(((float)sqrt(p->d) - 0.6f * size) / (1.5f * size + 1.0f), 0.0f, 1.0f);
        if (al < 0.01f) continue;
        qb_bill_cs(pos, 0.5f * size, p->cr, p->sr, p->cell, p->mirror, rgb_scale(lerp_rgb(p->l0, p->l1, E_MINF(1.0f, u / 0.6f)), 1.0f, al));
    }
    s_cStats[2] = n;
}

static void draw_explosions(float alpha)
{
    int i, k;
    float tick = (float)s_curTick + alpha;
    qb_mat(&s_mFire);
    for (i = 0; i < EXPL_MAX; i++) {
        Expl *e = &s_ex[i];
        float age, ff = fog_fade(e->p), d;
        if (!e->on) continue;
        age = (tick - (float)e->tick) / (float)SIM_HZ;
        if (age > 2.0f) { e->on = 0; continue; }
        d = depth_of(e->p);
        if (d > 0.1f && 9.0f * e->scale * s_pxPerM1 / d < 5.0f) {   /* far: one orange dot */
            float k2 = ff * (1.0f - E_CLAMP(age / 1.5f, 0.0f, 1.0f));
            qb_flush(); qb_mat(&s_mDot);
            qb_bill(e->p, min_px(e->p, 4.0f), 0.0f, -1, 0, rgb_scale(0xFFA040, k2, 1.0f));
            qb_flush(); qb_mat(&s_mFire);
            s_cStats[1]++;
            continue;
        }
        for (k = 0; k < e->nFire; k++) {
            const FireQ *f = &e->f[k];
            float t = age - f->delay, u, fr, w, gsz, fade, size;
            int fi;
            if (t < 0.0f || t > f->life) continue;
            u = t / f->life;
            fr = u * 4.0f;
            fi = (int)fr;
            w = fr - (float)fi;
            gsz = 0.35f + 0.65f * (1.0f - (1.0f - u) * (1.0f - u));
            size = f->size * (1.0f + 1.4f * gsz);
            fade = (u < 0.72f ? 1.0f : 1.0f - (u - 0.72f) / 0.28f) * ff;
            if (fi < 4) qb_bill(v3_add(e->p, f->off), 0.5f * size, f->roll, fi, f->mirror, premul_col((1.0f - w) * fade));
            if (fi + 1 < 4 && w > 0.02f) qb_bill(v3_add(e->p, f->off), 0.5f * size, f->roll, fi + 1, f->mirror, premul_col(w * fade));
        }
        if (age < 0.16f) {                          /* flash */
            qb_flush(); qb_mat(&s_mAdd);
            qb_bill(e->p, 0.5f * (42.0f - 26.0f * age / 0.16f) * e->scale / 1.5f, 0.0f, -1, 0, rgb_scale(0xFFE0B0, ff * (1.0f - age / 0.16f), 1.0f));
            qb_flush(); qb_mat(&s_mFire);
        }
        s_cStats[1]++;
    }
    /* burning wrecks: fire frames 2/3 at low weight */
    for (i = 0; i < WRECK_MAX; i++) {
        const Wreck *w = &s_wr[i];
        Vec3 p;
        if (!w->on) continue;
        p = v3_lerp(w->pPrev, w->p, alpha);
        qb_bill(p, 5.0f, frand() * 6.28f, 2 + (s_curTick / 6u) % 2u, 0, premul_col(0.7f * fog_fade(p)));
    }
}

static void draw_debris(float alpha)
{
    int i;
    u32 col = 0xFF000000u | lit(0x9A9A9A);
    qb_mat(&s_mDebris);
    for (i = 0; i < DEBRIS_MAX; i++) {
        Debris *d = &s_db[i];
        float t;
        Vec3 p;
        int cx, cy;
        if (!d->on) continue;
        t = ((float)s_curTick + alpha - (float)d->tick) / (float)SIM_HZ;
        if (t > d->life) { d->on = 0; continue; }
        p = v3_add(v3_madd(d->p, d->v, t), v3(0.0f, -0.5f * GRAV * t * t, 0.0f));
        cx = d->cell & 3; cy = d->cell >> 2;
        {
            float cr = (float)cos(d->roll + d->spin * t), sr = (float)sin(d->roll + d->spin * t), hs = 0.5f * d->size;
            Vec3 r = v3_scale(v3_add(v3_scale(s_camR, cr), v3_scale(s_camU, sr)), hs);
            Vec3 u = v3_scale(v3_sub(v3_scale(s_camU, cr), v3_scale(s_camR, sr)), hs);
            float u0 = (float)cx * 0.25f + 0.01f, v0 = (float)cy * 0.25f + 0.01f;
            qb_quad(v3_add(v3_sub(p, r), u), v3_add(v3_add(p, r), u), v3_sub(v3_sub(p, r), u), v3_sub(v3_add(p, r), u), u0, v0, u0 + 0.23f, v0 + 0.23f,
                    col, col, col, col);
        }
    }
}

static void draw_splashes(void)
{
    int i, j;
    u32 base = lit(0xFFFFFF);
    Vec3 r = v3_norm(v3(s_camR.x, 0.0f, s_camR.z));
    qb_mat(&s_mSplash);
    for (i = 0; i < SPLASH_MAX; i++) {
        Splash *s = &s_sp[i];
        float t = ages(s->tick), gh, al;
        if (!s->on) continue;
        if (t > 3.5f) { s->on = 0; continue; }
        gh = 1.0f - (1.0f - E_MINF(1.0f, t / 0.6f)) * (1.0f - E_MINF(1.0f, t / 0.6f));
        al = 0.95f * (t < 1.0f ? 1.0f : 1.0f - (t - 1.0f) / 2.5f);
        for (j = 0; j < s->n; j++) {
            float h = s->h[j] * gh, hw = 0.45f * s->h[j];
            Vec3 c = v3_madd(s->p, r, s->off[j]), b = v3(c.x, -0.06f * s->h[j], c.z), tp = v3(c.x, b.y + h, c.z);
            u32 col = rgb_scale(base, 1.0f, al);
            float u0 = (j & 1) ? 1.0f : 0.0f, u1 = 1.0f - u0;
            qb_quad(v3_madd(tp, r, -hw), v3_madd(tp, r, hw), v3_madd(b, r, -hw), v3_madd(b, r, hw), u0, 0.0f, u1, 1.0f, col, col, col, col);
        }
    }
}

static void draw_additive(const Game *g, float alpha)
{
    static MissileView mv[MAX_MISSILES];
    static FlareView fv[MAX_FLARES];
    int n, i;
    (void)g;
    qb_mat(&s_mAdd);
    n = weapons_missiles(mv, MAX_MISSILES);         /* motor glows while the motor burns */
    for (i = 0; i < n; i++) {
        const MissileParams *mp = missile_params_of(mv[i].type);
        Vec3 p;
        float len = mv[i].type == MSL_SRM ? 2.85f : 3.65f, hs, f;
        if (mp && (mv[i].age < mp->dropSec || mv[i].age > mp->dropSec + mp->boostSec)) continue;
        p = v3_madd(v3_lerp(mv[i].prevPos, mv[i].pos, alpha), mv[i].dir, -0.5f * len);
        if (v3_len(v3_sub(p, s_camEye)) > 8000.0f) continue;
        f = fog_fade(p);
        hs = E_MAXF(0.7f, min_px(p, 3.0f));
        qb_bill(p, hs, frand() * 6.28f, -1, 0, rgb_scale(0xFFDCAA, f * (0.85f + 0.3f * frand()), 1.0f));
    }
    n = weapons_flares_view(fv, MAX_FLARES);        /* flares: 4.5 m, flicker, fade over the last 0.4 s */
    for (i = 0; i < n; i++) {
        Vec3 p = v3_lerp(fv[i].prevPos, fv[i].pos, alpha);
        float burn = fv[i].burn > 0.1f ? fv[i].burn : 3.0f, left = burn - fv[i].age, k;
        if (v3_len(v3_sub(p, s_camEye)) > 8000.0f) continue;
        k = fog_fade(p) * (0.85f + 0.3f * frand()) * E_CLAMP(left / 0.4f, 0.0f, 1.0f);
        qb_bill(p, E_MAXF(2.25f, min_px(p, 3.0f)), frand() * 6.28f, -1, 0, rgb_scale(0xFFFFFF, k, 1.0f));
    }
}

static void draw_screen_flash(void)
{
    float a = 0.0f, t;
    u32 col = 0xFFE6B4;
    int sw = 0, sh = 0;
    if (s_flashTick) {
        t = ages(s_flashTick);
        if (t < 0.15f) a = 0.5f * s_flashK * (1.0f - t / 0.15f); else s_flashTick = 0;
    }
    if (s_hitFlashTick) {
        t = ages(s_hitFlashTick);
        if (t < 0.25f) { float b = 0.35f * (1.0f - t / 0.25f); if (b > a) { a = b; col = 0xFF4020; } }
        else s_hitFlashTick = 0;
    }
    if (a < 0.01f) return;
    r_screen_size(&sw, &sh);
    ui_begin(640, 480);
    ui_rect(0.0f, 0.0f, 640.0f, 480.0f, ((u32)e_ftoi(a * 255.0f) << 24) | col);
    ui_end();
}

void fx_combat_draw(const Game *g, const Camera *c, float alpha)
{
    const SceneStats *ss = scene_stats();
    const ScenePreset *pr = scene_preset();
    int i, wr = 0;
    if (!s_cOk) return;
    memset(s_cStats, 0, sizeof(s_cStats));
    s_curTick = g->tick;
    s_explCap = (pr && !e_stricmp(pr->name, "high")) ? 8 : 4;
    s_trailPts = (pr && !e_stricmp(pr->name, "high")) ? TRAIL_PTS : 16;
    s_puffCap = (pr && !e_stricmp(pr->name, "high")) ? PUFF_MAX : 128;   /* T22 */
    s_camR = v3(c->view.m[0][0], c->view.m[1][0], c->view.m[2][0]);
    s_camU = v3(c->view.m[0][1], c->view.m[1][1], c->view.m[2][1]);
    s_camF = v3(c->view.m[0][2], c->view.m[1][2], c->view.m[2][2]);
    s_camEye = c->eye;
    if (ss && ss->fogEnd > ss->fogStart) { s_fogS = ss->fogStart; s_fogE = ss->fogEnd; }
    {
        int sw = 0, sh = 0;
        r_screen_size(&sw, &sh);
        s_pxPerM1 = 0.5f * (float)(sw > 0 ? sw : 800) * c->proj.m[0][0];
        s_tanX = c->proj.m[0][0] > 1e-3f ? 1.0f / c->proj.m[0][0] : 1.0f;   /* T22: frustum test */
        s_tanY = c->proj.m[1][1] > 1e-3f ? 1.0f / c->proj.m[1][1] : 1.0f;
        s_kX = (float)sqrt(1.0f + s_tanX * s_tanX);
        s_kY = (float)sqrt(1.0f + s_tanY * s_tanY);
    }
    scene_light(&s_sunC, &s_ambC);
    {
        static int secW = -1;                       /* T22: bench section "fx_wreck" (part of "fx") */
        if (secW < 0) secW = prof_section("fx_wreck");
        prof_begin(secW);
        for (i = 0; i < WRECK_MAX; i++)             /* falling wrecks: the jet mesh (opaque, before the blends) */
            if (s_wr[i].on) {
                Vec3 p = v3_lerp(s_wr[i].pPrev, s_wr[i].p, alpha);
                if (v3_len(v3_sub(p, c->eye)) < 2500.0f) wr += scene_draw_wreck(c, p, q_slerp(s_wr[i].qPrev, s_wr[i].q, alpha));
            }
        prof_end(secW);
    }
    r_set_proj(&c->proj);
    r_set_view(&c->view);
    r_set_world(NULL);
    s_qm = NULL;
    s_qn = 0;
    draw_debris(alpha);
    draw_splashes();
    draw_trails(alpha);
    draw_flare_trails();
    fxp_draw_ribbons(g, c, alpha);                  /* T22: vapour, contrails, burning debris smoke (trail batch) */
    fxp_draw_shapes(g, c, alpha);                   /* T22: wing vapour sheets, transonic vapour cone (trail texture) */
    draw_puffs();
    fxp_draw_wisps();                               /* T22: cloud wisps streaming past inside a cloud (smoke batch) */
    draw_explosions(alpha);
    fxp_draw_fire(alpha);                           /* T22: burning debris heads, column base fires (fire batch) */
    draw_additive(g, alpha);
    qb_flush();
    r_set_world(NULL);
    fxp_draw_haze();                                /* T22: cloud fly-through haze (full screen, before the HUD) */
    draw_screen_flash();
    (void)wr;
}

int fx_combat_stats(int *trails, int *explosions, int *puffs)
{
    if (trails) *trails = s_cStats[0];
    if (explosions) *explosions = s_cStats[1];
    if (puffs) *puffs = s_cStats[2];
    return s_cStats[3];
}

/* ================================================================ T22: polish (append here) */
/* Render-only like the rest of fx (own RNG, sim ticks for timing, never writes game state). Low / High = the scene
   preset (fx_hi); everything has a [fx] switch.
     vapour      wingtip vortex trails from both tips (A1 rail_0/1 tags, 1.2 m aft) while pulling G: amount =
                 smoothstep(vapourGFrom..vapourGFull, n) (or the sim's vis.vapour), x humidity (weaker high up); points
                 sampled into a ring (Low 12 / High 24 over vapourLife), ribbon 0.3 -> 1.6 m, faded near the camera
                 only within 3-12 m so the chase view sees them stream past. Plus wing-top vapour sheets (n > 6) and
                 the transonic vapour cone (bell around Mach 1, vapourConeMachW), both textured with the trail
                 texture (u across = soft edges, random v offset per frame = flowing), within vapourDistM.
     contrail    above contrailAltM (fade-in 600 m below): from the nozzle, points every life/N (Low 24 pts / 6 s, High
                 48 / 12 s), ribbon 1 -> contrailWidthEnd m, fogged, drawn within contrailDistM (they persist after
                 the jet is gone).
     explosions  (hooks in explosion_add) secondary fire blooms 0.25-0.6 s late, every first-fireball fire quad turns
                 into a dark soot puff where it dies (Low 4 / High 7), burning debris (Low 3 / High 5) = a small
                 fireball head + a dark smoke ribbon arcing down; camera shake for near ones (fx_cam_shake).
     columns     where a burning wreck hits the ground / sea (and every crash site): a smoke column that rises
                 (columnRise) and leans with the wind (windDeg / windMs) for columnSecs (Low 20 / High 35 s on land,
                 half on the sea), fire at its base for the first ~half on land; puffs fogged, so it shows from far.
     clouds      scene_cloud_near(): full-screen haze (cloudHaze) inside a puff + cloud wisps streaming past.
   Costs (VM): see docs/PERF.md (T22). */
enum { PT_PTS = 48, BD_MAX = 24, BD_PTS = 14, COL_MAX = 4 };
typedef struct PTrail { int n; Vec3 pt[PT_PTS]; u32 tk[PT_PTS]; float k[PT_PTS]; } PTrail;
typedef struct ACPol { PTrail tip[2], con; u32 lastV, lastC; Vec3 lastPos; int have; float vapK, conK; } ACPol;
typedef struct BDeb { int on, n; Vec3 p0, v0; u32 tick, last; float life, size; Vec3 pt[BD_PTS]; u32 ptk[BD_PTS]; } BDeb;
typedef struct Column { int on, sea; Vec3 p; u32 tick, last; float dur, scale; } Column;
static ACPol  s_ap[MAX_AIRCRAFT];
static BDeb   s_bd[BD_MAX];
static Column s_col[COL_MAX];
static float  s_shAmp; static u32 s_shTick;
static Vec3   s_tipL = { -4.55f, -0.10f, -1.75f }, s_tipR = { 4.55f, -0.10f, -1.75f };
static int    s_tipsOk;

static int   s_vapOn, s_conOn, s_explLayers, s_colOn, s_burnDeb, s_wisps;
static float s_vapGFrom, s_vapGFull, s_vapLife, s_vapAlpha, s_vapW0, s_vapW1, s_vapDist, s_vapHumAlt, s_vapSheet, s_vapCone, s_vapConeW;
static float s_conAlt, s_conAlpha, s_conW1, s_conDist, s_conSecs;
static float s_colSecs, s_colRise, s_windDeg, s_windMs, s_hazeMax, s_wispAlpha, s_shakeDeg;
static const Param s_polTab[] = {
    { "fx", "vapour", P_BOOL, &s_vapOn, "1", 0, 1, "T22: wingtip vapour trails at high G, wing-top vapour sheets, transonic vapour cone" },
    { "fx", "vapourGFrom", P_FLOAT, &s_vapGFrom, "5.0", 0, 12, "wingtip vapour starts at this load factor (g)" },
    { "fx", "vapourGFull", P_FLOAT, &s_vapGFull, "8.0", 0, 12, "full wingtip vapour at this load factor (g)" },
    { "fx", "vapourLife", P_FLOAT, &s_vapLife, "0.6", 0.1f, 3, "wingtip vapour trail lifetime (s)" },
    { "fx", "vapourAlpha", P_FLOAT, &s_vapAlpha, "0.6", 0, 1, "wingtip vapour opacity" },
    { "fx", "vapourWidth", P_FLOAT, &s_vapW0, "0.3", 0.05f, 5, "wingtip vapour width at the tip (m)" },
    { "fx", "vapourWidthEnd", P_FLOAT, &s_vapW1, "1.6", 0.05f, 10, "wingtip vapour width when it dissolves (m)" },
    { "fx", "vapourDistM", P_FLOAT, &s_vapDist, "1500", 0, 5000, "vapour drawn for jets within this range (m)" },
    { "fx", "vapourHumidAltM", P_FLOAT, &s_vapHumAlt, "9000", 100, 30000, "vapour weakens with altitude (drier air): x0.4 at this height (m)" },
    { "fx", "vapourSheet", P_FLOAT, &s_vapSheet, "0.55", 0, 1, "wing-top vapour sheet opacity at 8.5 g (from 6 g); 0 = off" },
    { "fx", "vapourCone", P_FLOAT, &s_vapCone, "0.5", 0, 1, "transonic vapour cone opacity at Mach 1.0; 0 = off" },
    { "fx", "vapourConeMachW", P_FLOAT, &s_vapConeW, "0.035", 0.005f, 0.3f, "vapour cone Mach band half width (bell around Mach 1.0)" },
    { "fx", "contrail", P_BOOL, &s_conOn, "1", 0, 1, "T22: engine contrails at high altitude" },
    { "fx", "contrailAltM", P_FLOAT, &s_conAlt, "8000", 0, 30000, "contrails above this altitude (m; fade in over the 600 m below)" },
    { "fx", "contrailAlpha", P_FLOAT, &s_conAlpha, "0.85", 0, 1, "contrail opacity" },
    { "fx", "contrailWidthEnd", P_FLOAT, &s_conW1, "12", 1, 60, "contrail width when old (m; 1 m at the nozzle)" },
    { "fx", "contrailDistM", P_FLOAT, &s_conDist, "15000", 0, 40000, "contrails drawn within this range (m)" },
    { "fx", "contrailSecs", P_FLOAT, &s_conSecs, "0", 0, 60, "contrail lifetime (s); 0 = preset (Low 6, High 12)" },
    { "fx", "explScale", P_FLOAT, &s_explScale, "1.7", 0.5f, 4, "explosion size scale (arcade; T17 used 1.5)" },
    { "fx", "explLayers", P_BOOL, &s_explLayers, "1", 0, 1, "T22: explosion layers: secondary fire blooms, fireball turning into soot, burning debris trails" },
    { "fx", "burnDebris", P_INT, &s_burnDeb, "-1", -1, 8, "burning debris pieces per kill; -1 = preset (Low 3, High 5)" },
    { "fx", "column", P_BOOL, &s_colOn, "1", 0, 1, "T22: smoke column where a wreck hits the ground or sea (crash sites too)" },
    { "fx", "columnSecs", P_FLOAT, &s_colSecs, "0", 0, 120, "smoke column duration on land (s; half on the sea); 0 = preset (Low 20, High 35)" },
    { "fx", "columnRise", P_FLOAT, &s_colRise, "11", 0, 40, "smoke column rise speed (m/s)" },
    { "fx", "windDeg", P_FLOAT, &s_windDeg, "60", 0, 360, "smoke drifts toward this heading (deg)" },
    { "fx", "windMs", P_FLOAT, &s_windMs, "4", 0, 30, "smoke drift speed (m/s)" },
    { "fx", "cloudHaze", P_FLOAT, &s_hazeMax, "0.7", 0, 1, "T22: haze opacity deep inside a cloud puff (the puff itself fades out near the camera)" },
    { "fx", "cloudWisps", P_INT, &s_wisps, "-1", -1, 16, "cloud wisps streaming past near / inside a cloud; -1 = preset (Low 6, High 12)" },
    { "fx", "wispAlpha", P_FLOAT, &s_wispAlpha, "0.45", 0, 1, "cloud wisp opacity" },
    { "fx", "flameDiamonds", P_BOOL, &s_flameDiamonds, "1", 0, 1, "T22: three shock diamonds in the afterburner flame" },
    { "fx", "diamondBright", P_FLOAT, &s_diamondK, "0.55", 0, 2, "shock diamond brightness" },
    { "fx", "shakeExplDeg", P_FLOAT, &s_shakeDeg, "0.8", 0, 5, "camera shake (deg) from an explosion at the camera, gone by 700 m; hits on the own jet 1.2 x ([camera] shakeExpl scales)" },
};
static int s_polParams;

static void fxp_params(void)
{
    if (s_polParams) return;
    params_add(s_polTab, E_COUNT(s_polTab));
    s_polParams = 1;
}

static int fx_hi(void) { const ScenePreset *pr = scene_preset(); return pr && !e_stricmp(pr->name, "high"); }

static void fxp_reset(void)
{
    memset(s_ap, 0, sizeof(s_ap));
    memset(s_bd, 0, sizeof(s_bd));
    memset(s_col, 0, sizeof(s_col));
    s_shAmp = 0.0f; s_shTick = 0;
}

float fx_cam_shake(void)
{
    float t;
    if (!s_shTick) return 0.0f;
    t = ages(s_shTick);
    if (t >= 0.6f) { s_shTick = 0; return 0.0f; }
    t = 1.0f - t / 0.6f;
    return s_shAmp * t * t;
}

static void fxp_shake(Vec3 p, float amp)
{
    float d = v3_len(v3_sub(p, s_camEye)), v, cur = fx_cam_shake();
    if (d >= 700.0f) return;
    v = s_shakeDeg * amp * (1.0f - d / 700.0f) * (1.0f - d / 700.0f);
    if (v > cur) { s_shAmp = v; s_shTick = s_curTick ? s_curTick : 1u; }
}

static float hum_k(float alt) { return 1.0f - 0.6f * smooth01(0.0f, s_vapHumAlt, alt); }

static float vap_k(const FlightState *fs)
{
    float g;
    if (!s_vapOn || fs->onGround) return 0.0f;
    g = smooth01(s_vapGFrom, s_vapGFull, fs->n);
    if (fs->vis.vapour > g) g = fs->vis.vapour;
    return g * hum_k(fs->pos.y);
}

static float con_k(const FlightState *fs) { return s_conOn ? smooth01(s_conAlt - 600.0f, s_conAlt, fs->pos.y) : 0.0f; }

static void pt_push(PTrail *t, Vec3 p, u32 tick, float k)
{
    if (t->n >= PT_PTS) {
        memmove(&t->pt[0], &t->pt[1], (PT_PTS - 1) * sizeof(Vec3));
        memmove(&t->tk[0], &t->tk[1], (PT_PTS - 1) * sizeof(u32));
        memmove(&t->k[0], &t->k[1], (PT_PTS - 1) * sizeof(float));
        t->n--;
    }
    t->pt[t->n] = p; t->tk[t->n] = tick; t->k[t->n] = k; t->n++;
}

static void pt_age(PTrail *t, u32 tick, float life, int maxPts)
{
    int drop = 0;
    while (drop < t->n && (float)(tick - t->tk[drop]) / (float)SIM_HZ > life) drop++;
    if (t->n - drop > maxPts) drop = t->n - maxPts;
    if (drop <= 0) return;
    memmove(&t->pt[0], &t->pt[drop], (size_t)(t->n - drop) * sizeof(Vec3));
    memmove(&t->tk[0], &t->tk[drop], (size_t)(t->n - drop) * sizeof(u32));
    memmove(&t->k[0], &t->k[drop], (size_t)(t->n - drop) * sizeof(float));
    t->n -= drop;
}

static float con_life(void) { return s_conSecs > 0.1f ? s_conSecs : (fx_hi() ? 12.0f : 6.0f); }

static void fxp_tick(const Game *g)
{
    int hi = fx_hi(), i, s, vpts = hi ? 24 : 12, cpts = hi ? PT_PTS : 24, vint, cint;
    float clife = con_life();
    vint = e_ftoi(s_vapLife * (float)SIM_HZ / (float)vpts); if (vint < 1) vint = 1;
    cint = e_ftoi(clife * (float)SIM_HZ / (float)cpts); if (cint < 1) cint = 1;
    if (!s_tipsOk) {                                 /* A1 tags: wingtip rails 0 (left) / 1 (right); vortex 1.2 m aft */
        const Mesh *jm = scene_jet_mesh();
        const MeshTag *t0 = jm ? mesh_tag(jm, "rail_0") : NULL, *t1 = jm ? mesh_tag(jm, "rail_1") : NULL;
        if (t0 && t1) { s_tipL = v3(t0->pos.x - 0.05f, t0->pos.y + 0.08f, t0->pos.z - 1.2f); s_tipR = v3(t1->pos.x + 0.05f, t1->pos.y + 0.08f, t1->pos.z - 1.2f); }
        s_tipsOk = 1;
    }
    for (i = 0; i < g->numAircraft && i < MAX_AIRCRAFT; i++) {
        const Aircraft *a = &g->ac[i];
        ACPol *P = &s_ap[i];
        Vec3 pos, nz;
        Quat q;
        if (!(a->flags & AC_ALIVE)) { P->vapK = P->conK = 0.0f; P->have = 0; }
        else {
            cam_ac_pose(a, 1.0f, &pos, &q);
            if (P->have && v3_len(v3_sub(pos, P->lastPos)) > 300.0f) {   /* respawn / teleport: drop the old trails */
                P->tip[0].n = P->tip[1].n = 0;
                P->con.n = 0;
            }
            P->lastPos = pos; P->have = 1;
            P->vapK = vap_k(&a->fs);
            P->conK = con_k(&a->fs);
            if ((P->vapK > 0.01f || (P->tip[0].n && P->tip[0].k[P->tip[0].n - 1] > 0.01f)) && g->tick - P->lastV >= (u32)vint) {
                P->lastV = g->tick;
                pt_push(&P->tip[0], v3_add(pos, q_rotate(q, s_tipL)), g->tick, P->vapK);
                pt_push(&P->tip[1], v3_add(pos, q_rotate(q, s_tipR)), g->tick, P->vapK);
            }
            if ((P->conK > 0.01f || (P->con.n && P->con.k[P->con.n - 1] > 0.01f)) && g->tick - P->lastC >= (u32)cint) {
                P->lastC = g->tick;
                nz = v3_add(pos, q_rotate(q, v3(0.0f, 0.05f, -7.6f)));
                pt_push(&P->con, nz, g->tick, P->conK);
            }
        }
        for (s = 0; s < 2; s++) pt_age(&P->tip[s], g->tick, s_vapLife, vpts);
        pt_age(&P->con, g->tick, clife, cpts);
    }
    for (i = 0; i < BD_MAX; i++) {                   /* burning debris: sample the arc, age the smoke */
        BDeb *b = &s_bd[i];
        float t, life = 1.6f;
        int drop = 0;
        if (!b->on) continue;
        t = ages(b->tick);
        if (t < b->life && (b->n == 0 || s_curTick - b->last >= 8u)) {
            Vec3 p = v3_add(v3_madd(b->p0, b->v0, t), v3(0.0f, -0.5f * GRAV * t * t, 0.0f));
            if (b->n >= BD_PTS) { memmove(&b->pt[0], &b->pt[1], (BD_PTS - 1) * sizeof(Vec3)); memmove(&b->ptk[0], &b->ptk[1], (BD_PTS - 1) * sizeof(u32)); b->n--; }
            b->pt[b->n] = p; b->ptk[b->n] = s_curTick; b->n++;
            b->last = s_curTick;
            if (p.y < world_surface(g, p.x, p.z)) b->life = t;   /* hit the ground: stop burning */
        }
        while (drop < b->n && (float)(s_curTick - b->ptk[drop]) / (float)SIM_HZ > life) drop++;
        if (drop) {
            memmove(&b->pt[0], &b->pt[drop], (size_t)(b->n - drop) * sizeof(Vec3));
            memmove(&b->ptk[0], &b->ptk[drop], (size_t)(b->n - drop) * sizeof(u32));
            b->n -= drop;
        }
        if (t >= b->life && b->n == 0) b->on = 0;
    }
    for (i = 0; i < COL_MAX; i++) {                  /* smoke columns */
        Column *c = &s_col[i];
        float age, iv = hi ? 0.5f : 0.8f, life = hi ? 15.0f : 12.0f, fade;
        Vec3 wind, p, v;
        if (!c->on) continue;
        age = ages(c->tick);
        if (age > c->dur) { c->on = 0; continue; }
        if (c->last && (float)(s_curTick - c->last) / (float)SIM_HZ < iv) continue;
        c->last = s_curTick ? s_curTick : 1u;
        wind = v3((float)sin(E_DEG2RAD(s_windDeg)) * s_windMs, 0.0f, (float)cos(E_DEG2RAD(s_windDeg)) * s_windMs);
        fade = 1.0f - smooth01(0.65f * c->dur, c->dur, age);
        p = v3(c->p.x + frange(-5.0f, 5.0f) * c->scale, c->p.y + frange(3.0f, 7.0f), c->p.z + frange(-5.0f, 5.0f) * c->scale);
        v = v3_add(v3_scale(wind, frange(0.6f, 1.0f)), v3(frange(-0.6f, 0.6f), s_colRise * frange(0.85f, 1.15f), frange(-0.6f, 0.6f)));
        if (c->sea) puff_add(p, v, 0.0f, life * 0.8f, 12.0f * c->scale, (hi ? 70.0f : 58.0f) * c->scale, 0x5C5A58, 0xC8C8C6, 0.72f * fade, (int)(frand() * 1.99f));
        else puff_add(p, v, 0.0f, life, 14.0f * c->scale, (hi ? 85.0f : 70.0f) * c->scale, 0x2A2624, 0x8C8A88, 0.84f * fade, (int)(frand() * 1.99f));
    }
}

static void fxp_explosion(struct Expl *e, float scale, int full)
{
    int hi = fx_hi(), j, nsec, nsm, nb, nPrim;
    float k;
    if (!s_explLayers || !e) return;
    k = e->scale;
    nPrim = e->nFire;
    nsec = full ? (hi ? 5 : 3) : (scale >= 0.5f ? 1 : 0);
    for (j = 0; j < nsec && e->nFire < FIRE_MAX; j++) {   /* secondary blooms */
        FireQ *f = &e->f[e->nFire++];
        Vec3 d = v3_norm(v3(frange(-1.0f, 1.0f), frange(-0.4f, 1.0f), frange(-1.0f, 1.0f)));
        f->off = v3_scale(d, frange(6.0f, 11.0f) * k);
        f->delay = frange(0.25f, 0.6f);
        f->size = frange(4.0f, 6.5f) * k;
        f->life = frange(0.8f, 1.1f);
        f->roll = frange(0.0f, 6.2831853f);
        f->mirror = frand() < 0.5f;
    }
    nsm = scale >= 0.3f ? (hi ? nPrim : (nPrim < 4 ? nPrim : 4)) : 0;
    for (j = 0; j < nsm; j++) {                      /* each first-fireball quad turns into soot where it dies */
        const FireQ *f = &e->f[j];
        Vec3 o = v3_len(f->off) > 0.1f ? v3_norm(f->off) : v3(0.0f, 1.0f, 0.0f);
        puff_add(v3_add(e->p, f->off), v3_add(v3_scale(o, 1.2f), v3(0.0f, 2.5f, 0.0f)), f->delay + 0.55f * f->life, frange(5.5f, 8.0f),
                 1.4f * f->size, 3.2f * f->size, 0x1E1A18, 0x6A6866, 0.9f, j & 1);
    }
    nb = full ? (s_burnDeb >= 0 ? s_burnDeb : (hi ? 5 : 3)) : 0;
    for (j = 0; j < nb; j++) {                       /* burning debris with smoke trails */
        int i;
        BDeb *b;
        for (i = 0; i < BD_MAX && s_bd[i].on; i++) {}
        if (i >= BD_MAX) break;
        b = &s_bd[i];
        memset(b, 0, sizeof(*b));
        b->on = 1; b->p0 = e->p; b->tick = s_curTick;
        b->v0 = v3_scale(v3_norm(v3(frange(-1.0f, 1.0f), frange(0.15f, 1.0f), frange(-1.0f, 1.0f))), frange(28.0f, 50.0f) * E_MINF(1.3f, scale));
        b->life = frange(1.8f, 3.0f);
        b->size = frange(2.0f, 3.2f) * E_MINF(1.0f, scale) * 1.2f;
    }
}

static void fxp_column(const Game *g, Vec3 p, float scale)
{
    int i, best = -1, cap = fx_hi() ? COL_MAX : 2;
    u32 old = 0xFFFFFFFFu;
    Column *c;
    if (!s_colOn) return;
    for (i = 0; i < cap; i++) if (!s_col[i].on) { best = i; break; }
    if (best < 0) for (i = 0; i < cap; i++) if (s_col[i].tick < old) { old = s_col[i].tick; best = i; }
    if (best < 0) return;
    c = &s_col[best];
    memset(c, 0, sizeof(*c));
    c->on = 1;
    c->sea = over_sea(g, p);
    c->p = v3(p.x, world_surface(g, p.x, p.z), p.z);
    c->tick = s_curTick;
    c->scale = scale;
    c->dur = (s_colSecs > 0.1f ? s_colSecs : (fx_hi() ? 35.0f : 20.0f)) * (c->sea ? 0.5f : 1.0f);
}

/* ---------------------------------------------------------------- T22 drawing */
static int own_hidden(const Camera *c, int id) { return id == PLAYER_ID && (c->mode != VIEW_CHASE || c->fixed); }

static void fxp_draw_ribbons(const Game *g, const Camera *c, float alpha)
{
    static Vec3 pts[PT_PTS + 1];
    static float ag[PT_PTS + 1], kk[PT_PTS + 1];
    u32 vcol = lit(0xF2F4F6), ccol = 0xFBFCFD, scol = lit(0x48433F);   /* contrails: sunlit ice, brighter than the haze */
    float tk = (float)s_curTick + alpha, clife = con_life();
    int i, s, j, n;
    qb_mat(&s_mTrail);
    for (i = 0; i < g->numAircraft && i < MAX_AIRCRAFT; i++) {
        const Aircraft *a = &g->ac[i];
        const ACPol *P = &s_ap[i];
        Vec3 pos = P->lastPos;
        Quat q = q_ident();
        int alive = (a->flags & AC_ALIVE) != 0;
        if (own_hidden(c, i)) continue;
        if (alive) cam_ac_pose(a, alpha, &pos, &q);
        if ((P->tip[0].n || P->tip[1].n) && v3_len(v3_sub(pos, s_camEye)) < s_vapDist)
            for (s = 0; s < 2; s++) {
                const PTrail *t = &P->tip[s];
                for (j = 0, n = 0; j < t->n; j++, n++) { pts[n] = t->pt[j]; ag[n] = (tk - (float)t->tk[j]) / (float)SIM_HZ; kk[n] = t->k[j]; }
                if (alive && n) { pts[n] = v3_add(pos, q_rotate(q, s ? s_tipR : s_tipL)); ag[n] = 0.0f; kk[n] = P->vapK; n++; }
                ribbon2(pts, ag, kk, n, s_vapLife, s_vapW0, s_vapW1, 0.35f * s_vapLife, vcol, s_vapAlpha, 1.0f, NULL, 3.0f, 12.0f, 0.0f, 1.5f);
            }
        if (P->con.n) {
            const PTrail *t = &P->con;
            float d0 = v3_len(v3_sub(t->pt[0], s_camEye)), d1 = v3_len(v3_sub(t->pt[t->n - 1], s_camEye));
            if (E_MINF(d0, d1) < s_conDist) {
                for (j = 0, n = 0; j < t->n; j++, n++) { pts[n] = t->pt[j]; ag[n] = (tk - (float)t->tk[j]) / (float)SIM_HZ; kk[n] = t->k[j]; }
                if (alive) { pts[n] = v3_add(pos, q_rotate(q, v3(0.0f, 0.05f, -7.6f))); ag[n] = 0.0f; kk[n] = P->conK; n++; }
                ribbon2(pts, ag, kk, n, clife, 1.0f, s_conW1, 0.4f * clife, ccol, s_conAlpha, 1.0f, NULL, 15.0f, 60.0f, 0.15f, 2.0f);
            }
        }
    }
    for (i = 0; i < BD_MAX; i++) {                   /* burning debris smoke */
        const BDeb *b = &s_bd[i];
        float t;
        if (!b->on || !b->n) continue;
        for (j = 0, n = 0; j < b->n; j++, n++) { pts[n] = b->pt[j]; ag[n] = (tk - (float)b->ptk[j]) / (float)SIM_HZ; }
        t = (tk - (float)b->tick) / (float)SIM_HZ;
        if (t < b->life) { pts[n] = v3_add(v3_madd(b->p0, b->v0, t), v3(0.0f, -0.5f * GRAV * t * t, 0.0f)); ag[n] = 0.0f; n++; }
        ribbon2(pts, ag, NULL, n, 1.6f, 0.7f, 4.5f, 0.8f, scol, 0.75f, 1.0f, NULL, 8.0f, 30.0f, 0.0f, 1.5f);
    }
}

/* body-space point of an aircraft pose -> world */
static Vec3 bp(Vec3 pos, Quat q, float x, float y, float z) { return v3_add(pos, q_rotate(q, v3(x, y, z))); }

static void fxp_draw_shapes(const Game *g, const Camera *c, float alpha)
{
    static const float ringZ[3] = { 0.9f, -1.0f, -3.4f }, ringR[3] = { 1.25f, 2.9f, 4.7f };
    u32 base = lit(0xF4F6F8);
    int i, s, j, r;
    if (!s_vapOn || (s_vapSheet <= 0.0f && s_vapCone <= 0.0f)) return;
    qb_mat(&s_mTrail);
    for (i = 0; i < g->numAircraft; i++) {
        const Aircraft *a = &g->ac[i];
        Vec3 pos;
        Quat q;
        float n, m, hum, ks, kc, bell, x, d, vo;
        if (!(a->flags & AC_ALIVE) || a->fs.onGround || own_hidden(c, i)) continue;
        cam_ac_pose(a, alpha, &pos, &q);
        d = v3_len(v3_sub(pos, s_camEye));
        if (d > E_MINF(900.0f, s_vapDist) || vis_code(pos, 10.0f)) continue;
        n = a->prev.n + (a->fs.n - a->prev.n) * alpha;
        m = a->prev.mach + (a->fs.mach - a->prev.mach) * alpha;
        hum = hum_k(pos.y);
        x = (m - 1.0f) / s_vapConeW;
        bell = (x > -3.0f && x < 3.0f) ? (float)exp(-x * x) : 0.0f;
        ks = s_vapSheet * smooth01(6.0f, 8.5f, n) * hum * (1.0f + 0.6f * bell);
        kc = s_vapCone * bell * hum * (0.45f + 0.55f * smooth01(1.5f, 6.0f, n));
        if (ks > 0.01f)                              /* wing-top vapour sheets: 3 chord rows x root / tip, soft at both ends */
            for (s = -1; s <= 1; s += 2) {
                static const float zr[3] = { 1.6f, -0.3f, -2.6f }, zt[3] = { -0.9f, -1.9f, -3.0f };
                static const float yr[3] = { 0.40f, 0.80f, 1.35f }, yt[3] = { 0.20f, 0.45f, 0.85f };   /* rises aft: a blanket, not a decal */
                Vec3 pr[3], pt[3];
                u32 cm = rgb_scale(base, 1.0f, ks * (0.8f + 0.4f * frand())), c0 = cm & 0x00FFFFFFu;
                vo = frand();
                for (r = 0; r < 3; r++) { pr[r] = bp(pos, q, (float)s * 0.95f, yr[r], zr[r]); pt[r] = bp(pos, q, (float)s * 3.75f, yt[r], zt[r]); }
                qb_quad(pr[0], pt[0], pr[1], pt[1], 0.0f, vo, 1.0f, vo + 0.35f, c0, c0, cm, cm);
                qb_quad(pr[1], pt[1], pr[2], pt[2], 0.0f, vo + 0.35f, 1.0f, vo + 0.7f, cm, cm, c0, c0);
            }
        if (kc > 0.01f) {                            /* transonic vapour cone: 3 rings x 12 around the body axis */
            Vec3 rp[3][13];
            u32 cm = rgb_scale(base, 1.0f, kc * (0.85f + 0.3f * frand())), c0 = cm & 0x00FFFFFFu;
            vo = frand();
            for (r = 0; r < 3; r++)
                for (j = 0; j <= 12; j++) {
                    float an = 6.2831853f * (float)j / 12.0f;
                    rp[r][j] = bp(pos, q, ringR[r] * (float)cos(an), 0.25f + 0.92f * ringR[r] * (float)sin(an), ringZ[r]);
                }
            for (j = 0; j < 12; j++) {
                float u0 = (j & 1) ? 0.7f : 0.3f, u1 = (j & 1) ? 0.3f : 0.7f;
                qb_quad(rp[0][j], rp[0][j + 1], rp[1][j], rp[1][j + 1], u0, vo, u1, vo + 0.4f, c0, c0, cm, cm);
                qb_quad(rp[1][j], rp[1][j + 1], rp[2][j], rp[2][j + 1], u0, vo + 0.4f, u1, vo + 0.8f, cm, cm, c0, c0);
            }
        }
    }
}

static void fxp_draw_fire(float alpha)
{
    float tk = (float)s_curTick + alpha;
    int i;
    qb_mat(&s_mFire);
    for (i = 0; i < BD_MAX; i++) {                   /* burning debris heads */
        const BDeb *b = &s_bd[i];
        float t = (tk - (float)b->tick) / (float)SIM_HZ, u;
        Vec3 p;
        if (!b->on || t >= b->life || t < 0.0f) continue;
        u = t / b->life;
        p = v3_add(v3_madd(b->p0, b->v0, t), v3(0.0f, -0.5f * GRAV * t * t, 0.0f));
        qb_bill(p, E_MAXF(0.5f * b->size * (1.0f - 0.5f * u), min_px(p, 2.0f)), frand() * 6.28f, 1 + (int)((s_curTick / 5u + (u32)i) % 2u), 0,
                premul_col((1.0f - u * u) * fog_fade(p) * (0.85f + 0.3f * frand())));
    }
    for (i = 0; i < COL_MAX; i++) {                  /* fire at the base of a land column */
        const Column *c = &s_col[i];
        float age, w;
        if (!c->on || c->sea) continue;
        age = (tk - (float)c->tick) / (float)SIM_HZ;
        w = 0.8f * (1.0f - smooth01(0.35f * c->dur, 0.6f * c->dur, age)) * fog_fade(c->p);
        if (w < 0.01f) continue;
        qb_bill(v3(c->p.x, c->p.y + 4.0f, c->p.z), 5.5f * c->scale, frand() * 0.6f - 0.3f, 2 + (int)((s_curTick / 6u) % 2u), (s_curTick / 9u) & 1u,
                premul_col(w * (0.85f + 0.3f * frand())));
        qb_bill(v3(c->p.x + 2.0f, c->p.y + 6.5f, c->p.z - 1.5f), 3.8f * c->scale, frand() * 0.6f - 0.3f, 3 - (int)((s_curTick / 7u) % 2u), 0,
                premul_col(0.8f * w * (0.85f + 0.3f * frand())));
    }
}

/* cloud wisps: static points on a 140 m world lattice near the camera (like the T10b streaks), shown while the camera
   is near / inside a cloud puff, so the cloud streams past with parallax instead of being a flat grey screen */
static void fxp_draw_wisps(void)
{
    float cnear = scene_cloud_near(), P = 140.0f, half = 70.0f;
    int i, n = s_wisps >= 0 ? s_wisps : (fx_hi() ? 12 : 6);
    Vec3 ctr;
    u32 base;
    if (cnear < 0.02f || n <= 0 || s_wispAlpha <= 0.0f) return;
    base = lit(0xF0F2F4);
    ctr = v3_madd(s_camEye, s_camF, 0.4f * P);
    qb_mat(&s_mSmoke);
    for (i = 0; i < n; i++) {
        Vec3 b, p;
        float dist, fa, size;
        b = v3(hash01((u32)i * 5u + 0x91u) * P, hash01((u32)i * 5u + 0x92u) * P, hash01((u32)i * 5u + 0x93u) * P);
        p.x = b.x + P * (float)floor((ctr.x - b.x) / P + 0.5f);
        p.y = b.y + P * (float)floor((ctr.y - b.y) / P + 0.5f);
        p.z = b.z + P * (float)floor((ctr.z - b.z) / P + 0.5f);
        dist = v3_len(v3_sub(p, s_camEye));
        size = 30.0f + 28.0f * hash01((u32)i * 5u + 0x94u);
        fa = s_wispAlpha * cnear * smooth01(8.0f, 0.6f * size, dist) * (1.0f - smooth01(0.55f * half, half, v3_len(v3_sub(p, ctr))));
        if (fa < 0.01f) continue;
        qb_bill(p, 0.5f * size, hash01((u32)i * 5u + 0x95u) * 6.28f, 2 + (i & 1), i & 1, rgb_scale(base, 1.0f, fa));
    }
}

static void fxp_draw_haze(void)
{
    float cnear = scene_cloud_near(), a = s_hazeMax * smooth01(0.25f, 1.0f, cnear);
    u32 col;
    if (a < 0.01f) return;
    col = lit(0xF2F4F8);
    ui_begin(640, 480);
    ui_rect(0.0f, 0.0f, 640.0f, 480.0f, ((u32)e_ftoi(E_CLAMP(a, 0.0f, 1.0f) * 255.0f) << 24) | (col & 0xFFFFFF));
    ui_end();
}
