/* e_terrain_view.cpp - T04 terrain/sea viewer: screenshots + bench of e_terrain_draw outside jet.exe (the scene
   is T08's). Windows only; links engine\engine.lib. Owner: T04.
   Build (in the VM, after `w98 build jet` made engine.lib):  w98 build jet /f engine\world\e_terrain_view.mak
   Run:  M:\dev\jet\engine\world\e_terrain_view.exe [-view NAME | -camat X Y Z HDG PITCH | -all] [options]
     -all            every preset view: shot tv_<name>.bmp + bench (fixed heading) + 8-heading sweep
     -view NAME      runway | runway_sea | crater | sea | coast_low | threshold | slope (see s_views)
     -shot FILE      BMP after -frames N (default 24) warm-up frames (written next to the exe)
     -bench N        N timed frames per view (default 0 = none; -all uses 60), -sweep 1 adds the 8 headings
     -window         windowed 640x480 (default fullscreen 800x600x16 = Low), -res 640|800|1024
     -lodscale F (0.85) -far M (18000) -dist 0|1 -skirts 0|1|2 -merge L -shallowlod 3|4 -seabias N -foambias N
     -nobase -nosky -nosea -time T (sea scroll time, s) -wbuf 0|1 -fogfix 0|1 (default 1)
     -vp N           draw into a 1/N viewport (CPU cost without fill; bench only)
     -pair D         with -view: tv_pair_a.bmp + tv_pair_b.bmp with the camera moved D m sideways (z-fight diff)
   Output: e_terrain_view.log next to the exe ("bench:" and "terrain:" lines); all runs exit by themselves. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "e_base.h"
#include "e_math.h"
#include "e_log.h"
#include "e_cfg.h"
#include "e_file.h"
#include "e_prof.h"
#include "e_rend.h"
#include "e_plat.h"
#include "e_tex.h"
#include "e_mesh.h"
#include "e_world.h"
#include "e_terrain.h"

typedef struct TvView { const char *name; float x, y, z, hdg, pitch; } TvView;
static const TvView s_views[] = {
    { "runway",     -2250.0f,   62.0f, -3980.0f,  45.0f,  -3.0f },   /* 50 m over the runway toward the volcano */
    { "runway_sea", -2250.0f,   62.0f, -3980.0f, 270.0f,  -6.0f },   /* 50 m over the runway out to sea (coast) */
    { "crater",       192.0f, 3308.0f,   544.0f, 230.0f, -15.0f },   /* 1500 m over the crater */
    { "sea",        -6751.0f, 8000.0f, -6559.0f,  45.0f, -15.0f },   /* 8000 m over the sea 10 km SW */
    { "coast_low",  -5600.0f,   40.0f, -4300.0f,  80.0f,  -2.0f },   /* 40 m over the sea toward the coast */
    { "threshold",   -890.0f,   15.5f, -3980.0f, 270.0f,   0.0f },   /* cockpit on the threshold */
    { "slope",       3200.0f,  700.0f, -2200.0f, 310.0f,  -4.0f },   /* volcano flank across LOD changes (cracks) */
};

static Terrain s_t;
static World s_sky, s_base;
static int s_skyOk, s_baseOk, s_noSea, s_w = 800, s_h = 600, s_vpDiv = 1;
static float s_lodScale = TERRAIN_LODSCALE_LOW, s_far = 18000.0f, s_time = 0.0f;
static u32 s_fog = 0xC4D4E4;
static char s_dir[E_PATH];

static void make_view(WView *wv, Vec3 eye, float hdg, float pitch)
{
    float cp = (float)cos(E_DEG2RAD(pitch)), sp = (float)sin(E_DEG2RAD(pitch));
    Vec3 at = v3(eye.x + 1000.0f * cp * (float)sin(E_DEG2RAD(hdg)), eye.y + 1000.0f * sp,
                 eye.z + 1000.0f * cp * (float)cos(E_DEG2RAD(hdg)));
    float fovY = 2.0f * (float)atan(tan(E_DEG2RAD(70.0f) * 0.5f) * 0.75f);
    world_view_lookat(wv, eye, at, fovY, (float)s_w / (float)s_h, s_far, 0.0f);
}

typedef struct FrameMs { float terrain, sea, base, frame, sel; } FrameMs;

static float ms_since(s64 t0) { return (float)((double)(plat_qpc() - t0) * 1000.0 / (double)plat_qpf()); }

static int frame(const WView *wv, FrameMs *ms, const char *shot)
{
    TerrainDrawCfg *c = terrain_draw_cfg();
    s64 t0 = plat_qpc(), t1;
    if (!plat_pump()) return 0;
    if (!r_begin_frame(s_fog | (s_skyOk ? R_ZONLY : 0))) { r_device_lost_check(); return 1; }
    if (s_vpDiv > 1) r_set_viewport(0, 0, s_w / s_vpDiv, s_h / s_vpDiv);   /* -vp: same clip work, ~no fill */
    c->fogRGB = s_fog; c->fogStart = 0.2f * s_far; c->fogEnd = 0.9f * s_far;
    r_set_fog(1, s_fog, c->fogStart, c->fogEnd);
    t1 = plat_qpc();
    terrain_draw(&s_t, wv, s_lodScale, s_far);
    ms->terrain = ms_since(t1);
    t1 = plat_qpc();
    if (s_baseOk) { world_draw_opaque(&s_base, wv, NULL); world_draw_alpha(&s_base, wv, NULL); }
    ms->base = ms_since(t1);
    t1 = plat_qpc();
    if (!s_noSea) sea_draw(&s_t, wv, s_time);
    ms->sea = ms_since(t1);
    if (s_skyOk) world_draw_sky(&s_sky, wv);
    r_end_frame();
    if (shot) {
        char p[E_PATH];
        path_join(p, (int)sizeof(p), s_dir, shot);
        log_printf(LOG_INFO, "shot: path=%s ok=%d", p, r_save_bmp(p));
    }
    r_present();
    ms->frame = ms_since(t0);
    return 1;
}

static void log_stats(const char *name, int hdg)
{
    const TerrainStats *s = terrain_stats();
    log_printf(LOG_INFO, "terrain: view=%s hdg=%d tris=%d verts=%d skirt=%d foam=%d lods=%d/%d/%d/%d/%d skirtChunks=%d "
               "draws=%d sea=%d shallow=%d seaDraws=%d total=%d", name, hdg, s->tris, s->verts, s->skirtTris, s->foamTris,
               s->chunksByLod[0], s->chunksByLod[1], s->chunksByLod[2], s->chunksByLod[3], s->chunksByLod[4],
               s->skirtChunks, s->draws, s->seaTris, s->shallowTris, s->seaDraws,
               s->tris + s->foamTris + s->seaTris + s->shallowTris);
}

/* warm up, shot, bench at the fixed heading, optional 8-heading sweep */
static void run_view(const char *name, Vec3 eye, float hdg, float pitch, const char *shot, int warm, int bench, int sweep)
{
    WView wv;
    FrameMs ms;
    int i, h;
    make_view(&wv, eye, hdg, pitch);
    for (i = 0; i < warm; i++) if (!frame(&wv, &ms, NULL)) return;
    if (!frame(&wv, &ms, shot)) return;
    log_stats(name, (int)hdg);
    for (h = -1; h < (sweep ? 8 : 0); h++) {
        float hh = h < 0 ? hdg : (float)(h * 45);
        double sT = 0, sS = 0, sB = 0, sF = 0, wT = 0, wS = 0, sL = 0;
        int n = 0;
        if (!bench) break;
        make_view(&wv, eye, hh, pitch);
        for (i = 0; i < 4; i++) frame(&wv, &ms, NULL);
        for (i = 0; i < bench; i++) {
            if (!frame(&wv, &ms, NULL)) return;
            {
                s64 q = plat_qpc();
                terrain_select(&s_t, &wv, s_lodScale, s_far, 1, NULL);   /* selection alone (part of terrain_ms) */
                sL += ms_since(q);
            }
            sT += ms.terrain; sS += ms.sea; sB += ms.base; sF += ms.frame; n++;
            if (ms.terrain > wT) wT = ms.terrain;
            if (ms.sea > wS) wS = ms.sea;
        }
        if (h >= 0) log_stats(name, (int)hh);
        log_printf(LOG_INFO, "bench: view=%s hdg=%d vp=%d frames=%d terrain_ms=%.2f sea_ms=%.2f terrain_sea_ms=%.2f "
                   "select_ms=%.2f worst_terrain=%.2f worst_sea=%.2f base_ms=%.2f frame_ms=%.2f tris=%d", name, (int)hh,
                   s_vpDiv, n, sT / n, sS / n, (sT + sS) / n, sL / n, wT, wS, sB / n, sF / n, terrain_stats()->tris);
    }
}

static int app(HINSTANCE inst)
{
    char base[E_PATH], lp[E_PATH], path[E_PATH], *p;
    PlatInit pi;
    RInit ri;
    TerrainDrawCfg *c = terrain_draw_cfg();
    int i, fs, bench, sweep, warm, all;
    const char *vn, *shot;
    e_strlcpy(s_dir, plat_exe_dir(), (int)sizeof(s_dir));
    e_strlcpy(base, s_dir, (int)sizeof(base));
    p = strrchr(base, '\\');
    if (p && !e_stricmp(p + 1, "world")) { *p = 0; p = strrchr(base, '\\'); if (p && !e_stricmp(p + 1, "engine")) *p = 0; }
    file_set_base_dir(base);
    path_join(lp, (int)sizeof(lp), s_dir, "e_terrain_view.log");
    log_open(lp, arg_has("-debug") ? LOG_DEBUG : LOG_INFO);
    log_set_autoflush(1);
    log_printf(LOG_INFO, "tview: boot build=\"%s %s\" base=%s", __DATE__, __TIME__, base);
    plat_set_no_dialogs(1);
    plat_fpu_setup();
    prof_init(plat_now_us);
    fs = !arg_has("-window");
    i = arg_int("-res", 0, fs ? 800 : 640);
    if (i == 640) { s_w = 640; s_h = 480; } else if (i == 1024) { s_w = 1024; s_h = 768; } else { s_w = 800; s_h = 600; }
    s_lodScale = arg_float("-lodscale", 0, TERRAIN_LODSCALE_LOW);
    s_far = arg_float("-far", 0, 18000.0f);
    s_time = arg_float("-time", 0, 0.0f);
    s_noSea = arg_has("-nosea");
    s_vpDiv = arg_int("-vp", 0, 1);
    if (arg_has("-dist")) c->distMode = arg_int("-dist", 0, 1);
    if (arg_has("-skirts")) c->skirtMode = arg_int("-skirts", 0, 0);
    if (arg_has("-merge")) c->mergeLod = arg_int("-merge", 0, 0);
    if (arg_has("-shallowlod")) c->shallowLod = arg_int("-shallowlod", 0, 3);
    if (arg_has("-seabias")) c->seaZBias = arg_int("-seabias", 0, 1);
    if (arg_has("-foambias")) c->foamZBias = arg_int("-foambias", 0, 2);
    memset(&pi, 0, sizeof(pi));
    pi.title = "Jet terrain view";
    pi.wndClass = "JetTerrainView";
    pi.width = s_w; pi.height = s_h; pi.fullscreen = fs;
    if (!plat_init(inst, &pi)) { log_printf(LOG_ERR, "tview: window failed"); log_close(); return 1; }
    ShowWindow((HWND)plat_hwnd(), SW_SHOWNORMAL);
    ShowWindow((HWND)plat_hwnd(), SW_SHOWNORMAL);
    SetForegroundWindow((HWND)plat_hwnd());
    memset(&ri, 0, sizeof(ri));
    ri.width = s_w; ri.height = s_h; ri.bpp = 16; ri.fullscreen = fs; ri.fpsTarget = 30;
    ri.wantWBuffer = arg_int("-wbuf", 0, 1); ri.filter = 0; ri.maxTexSize = 256; ri.texBudgetKB = 6 * 1024;
    ri.noCap = 1;
    if (!r_init(plat_hwnd(), &ri)) { log_printf(LOG_ERR, "tview: r_init failed"); plat_shutdown(); log_close(); return 1; }
    plat_fpu_setup();
    data_path(path, (int)sizeof(path), "island/jet.isl");
    {
        s64 t0 = plat_qpc();
        int ok = terrain_load(&s_t, path);
        float lm = ms_since(t0);
        t0 = plat_qpc();
        terrain_gpu_init(&s_t);
        log_printf(LOG_INFO, "tview: terrain ok=%d load_ms=%.0f gpu_ms=%.0f", ok, (double)lm, (double)ms_since(t0));
    }
    memset(&s_sky, 0, sizeof(s_sky));
    if (!arg_has("-nosky")) {
        s_skyOk = world_sky_load(&s_sky, 4, "light") > 0;
        world_set_sky_late(1);
        if (s_sky.sky.fog) s_fog = s_sky.sky.fog & 0xFFFFFF;
        if (arg_int("-fogfix", 0, 1)) {             /* 565 match of fog / sky lower band / panorama (e_terrain.h) */
            s_fog = terrain_fog_565fix(s_fog);
            s_sky.sky.fog = (s_sky.sky.fog & 0xFF000000u) | s_fog;
            for (i = 0; i < 32; i++) s_sky.skyBot[i] = terrain_fog_565fix(s_sky.skyBot[i]);   /* lower band start */
        }
    }
    memset(&s_base, 0, sizeof(s_base));
    if (!arg_has("-nobase")) s_baseOk = world_load(&s_base, "island/base.trk", 0);
    log_printf(LOG_INFO, "tview: res=%dx%d fs=%d sky=%d base=%d fog=%06X lodScale=%.2f far=%.0f dist=%d skirts=%d merge=%d "
               "shallowLod=%d seaBias=%d foamBias=%d tex_kb=%d", s_w, s_h, fs, s_skyOk, s_baseOk, (unsigned)s_fog,
               (double)s_lodScale, (double)s_far, c->distMode, c->skirtMode, c->mergeLod, c->shallowLod, c->seaZBias,
               c->foamZBias, r_tex_kb());
    warm = arg_int("-frames", 0, 24);
    all = arg_has("-all");
    bench = arg_int("-bench", 0, all ? 60 : 0);
    sweep = arg_int("-sweep", 0, all ? 1 : 0);
    shot = arg_str("-shot", 0, NULL);
    vn = arg_str("-view", 0, NULL);
    for (i = 0; i < E_COUNT(s_views); i++) {
        const TvView *v = &s_views[i];
        char sn[64];
        if (!all && (!vn || e_stricmp(vn, v->name) || arg_has("-pair"))) continue;
        e_snprintf(sn, (int)sizeof(sn), "tv_%s.bmp", v->name);
        run_view(v->name, v3(v->x, v->y, v->z), v->hdg, v->pitch, all ? sn : shot, warm, bench, sweep);
    }
    if (arg_has("-pair")) {                       /* z-fight check: two shots, camera moved by -pair metres sideways */
        float d = arg_float("-pair", 0, 0.05f);
        for (i = 0; i < E_COUNT(s_views); i++) {
            const TvView *v = &s_views[i];
            float hr = E_DEG2RAD(v->hdg + 90.0f);
            if (!vn || e_stricmp(vn, v->name)) continue;
            run_view(v->name, v3(v->x, v->y, v->z), v->hdg, v->pitch, "tv_pair_a.bmp", warm, 0, 0);
            run_view(v->name, v3(v->x + d * (float)sin(hr), v->y, v->z + d * (float)cos(hr)), v->hdg, v->pitch,
                     "tv_pair_b.bmp", warm, 0, 0);
        }
    }
    if (arg_has("-camat")) {
        Vec3 e = v3(arg_float("-camat", 0, 0.0f), arg_float("-camat", 1, 100.0f), arg_float("-camat", 2, 0.0f));
        run_view("camat", e, arg_float("-camat", 3, 0.0f), arg_float("-camat", 4, 0.0f), shot, warm, bench, sweep);
    }
    log_printf(LOG_INFO, "tview: done");
    if (s_baseOk) world_free(&s_base);
    world_free(&s_sky);
    terrain_free(&s_t);
    tex_release_all();
    r_shutdown();
    plat_shutdown();
    log_close();
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdLine, int show)
{
    static char buf[1024];
    char *argv[64];
    int argc;
    (void)prev; (void)show;
    argv[0] = (char *)"e_terrain_view.exe";
    e_strlcpy(buf, cmdLine ? cmdLine : "", (int)sizeof(buf));
    argc = 1 + args_split(buf, argv + 1, E_COUNT(argv) - 1);
    args_init(argc, argv);
    return app(inst);
}
