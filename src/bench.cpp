/* bench.cpp - -bench routes and report (DESIGN 16.4). See bench.h. Windows side. Owner: T08, T09. */
#include "bench.h"
#include "fx.h"
#include "scene.h"
#include "takeoff.h"
#include "params.h"
#include "e_log.h"
#include "e_prof.h"
#include "e_plat.h"
#include "e_rend.h"
#include "e_terrain.h"
#include "world.h"
#include "radar.h"
#include "weapons.h"
#include "missile.h"
#include "ai.h"

enum { BENCH_WARMUP_US = 3000000, WP_MAX = 40, BH_N = 2000 };

static int   s_on, s_secs, s_route, s_phase, s_done, s_frames;
static s64   s_t0, s_tMeasure;
static Vec3  s_wp[WP_MAX]; static int s_nwp, s_wpi;
static float s_minAgl = 50.0f;
static int   s_hist[BH_N + 1], s_histN;
static double s_sum[8]; static int s_max[8];          /* terrain sea base ac clouds draws texch tris */
static float s_minAlt = 1e9f, s_maxAlt = -1e9f;
static double s_farSum; static float s_farMaxEff;     /* T10b: altitude-aware far plane actually used */
static double s_cbMslSum, s_cbFxSum, s_cbConSum; static int s_cbMslMax, s_cbFxMax, s_cbExplMax;   /* T17 route combat */

const char *bench_route_name(int r)
{
    switch (r) {
    case BENCH_ROUTE_LOW: return "low";
    case BENCH_ROUTE_VOLCANO: return "volcano";
    case BENCH_ROUTE_HIGH: return "high";
    case BENCH_ROUTE_CLOUDS: return "clouds";
    case BENCH_ROUTE_COMBAT: return "combat";
    }
    return "?";
}

int bench_active(void) { return s_on; }

/* ---------------------------------------------------------------- T17: route combat (DESIGN 16.4) */
/* Scripted, repeatable load: the player circles at 3000 m (r 6 km, clockwise) over the sea west of the island; four
   aggressor jets fly the same circle (TRAINING drone brains), two 1.3 / 2.5 km ahead of it and two behind; the bench
   makes the player and the two behind launch alternately (player: radar designation + LRM / SRM in turn at the one
   nearest its nose; enemies: radar lock on the player + LRM, so MISSILE / LOCK show) every 0.75 s while fewer than
   9 missiles fly, flares every 2 s; a hit enemy is destroyed (explosion + debris
   + wreck through fx) and comes back 3 s later ahead of the player; hits on the player do nothing (FREE mode, no
   mission director). Stores unlimited (T18: the player's are topped up to the normal loadout, so the HUD shows the
   real counts, not 99). HUD fully loaded: contacts, TD + launch zone, MISSILE / LOCK, trails. */
enum { CB_ENEMIES = 4 };
static Vec3  s_cbC;                                 /* circle centre */
static float s_cbR = 6000.0f;
static u32   s_cbTick, s_cbDeadTick[MAX_AIRCRAFT];
static u32   s_cbEv;
static int   s_cbShooter, s_cbPendFire = -1, s_cbPendTick, s_cbType;

static float cb_phase(int id)                      /* 1: 0.5 km ahead (SRM), 2: 1.3 km ahead (LRM), 3/4: 1.3 / 2.5 km behind */
{
    return id == 1 ? E_DEG2RAD(5.0f) : (id == 2 ? E_DEG2RAD(12.0f) : -E_DEG2RAD(12.0f * (float)(id - 2)));
}

static void cb_place(int id, float phaseAhead)
{
    Aircraft *a = &g_game.ac[id], *pl = &g_game.ac[PLAYER_ID];
    float ap = (float)atan2(pl->fs.pos.x - s_cbC.x, pl->fs.pos.z - s_cbC.z) + phaseAhead, r = s_cbR + (float)((id * 397) % 800) - 400.0f;
    Vec3 pos = v3(s_cbC.x + r * (float)sin(ap), s_cbC.y - 300.0f + (float)((id * 211) % 600), s_cbC.z + r * (float)cos(ap));
    memset(a, 0, sizeof(*a));
    a->id = id; a->side = SIDE_RED; a->flags = AC_ALIVE; a->livery = 1; a->hp = 100.0f;
    flight_init(&a->fs, pos, ap + 1.5707963f, flight_speed_target(THR_NORMAL, pos.y));
    a->fs.onGround = 0; a->fs.gearPos = 0.0f; a->fs.thr = THR_NORMAL;
    a->prev = a->fs;
    a->in.thr = THR_NORMAL;
    world_reset_aircraft(id);
    if (g_game.numAircraft < id + 1) g_game.numAircraft = id + 1;
    ai_spawn_drone(&g_game, id, s_cbC.x, s_cbC.z, r, pos.y);
    weapons_set_stores(id, 99, 99, 99);
    s_cbDeadTick[id] = 0;
}

static const Param *s_cbStore[3];                   /* T18: [weapons] lrmCount / srmCount / flareCount (found once) */

static void cb_topup(void)                          /* T18: player stores = the [weapons] loadout, refilled */
{
    int nl, ns, nf;
    const WpnState *w = weapons_state(PLAYER_ID);
    if (!s_cbStore[0]) {
        s_cbStore[0] = params_find("weapons", "lrmCount");
        s_cbStore[1] = params_find("weapons", "srmCount");
        s_cbStore[2] = params_find("weapons", "flareCount");
    }
    nl = s_cbStore[0] ? *(const int *)s_cbStore[0]->ptr : 16;
    ns = s_cbStore[1] ? *(const int *)s_cbStore[1]->ptr : 40;
    nf = s_cbStore[2] ? *(const int *)s_cbStore[2]->ptr : 40;
    if (w->lrm != nl || w->srm != ns || w->flares < nf) weapons_set_stores(PLAYER_ID, nl, ns, nf);
}

static void combat_init(void)
{
    int i;
    float cx = g_game.terrain ? g_game.terrain->centreX : 0.0f, cz = g_game.terrain ? g_game.terrain->centreZ : 0.0f;
    s_cbC = v3(cx - 42000.0f, 3000.0f, cz);
    s_cbTick = 0;
    s_cbShooter = 0;
    s_cbPendFire = -1;
    s_cbType = MSL_LRM;
    memset(s_cbDeadTick, 0, sizeof(s_cbDeadTick));
    cb_topup();
    for (i = 1; i <= CB_ENEMIES; i++) cb_place(i, cb_phase(i));
    s_cbEv = weapons_event_count();
}

static void combat_step(PilotInput *in)
{
    Game *g = &g_game;
    int i, best = -1;
    float bestA = 1e9f;
    Vec3 f;
    s_cbTick++;
    cb_topup();
    while (s_cbEv < weapons_event_count()) {        /* a hit enemy is destroyed (fx: explosion, debris, wreck) */
        const WpnEvent *e = weapons_event(s_cbEv++);
        if (e && e->type == WPE_HIT && e->victim > 0 && e->victim < g->numAircraft && (g->ac[e->victim].flags & AC_ALIVE)) {
            g->ac[e->victim].flags &= ~AC_ALIVE;
            g->ac[e->victim].hp = 0.0f;
            s_cbDeadTick[e->victim] = s_cbTick;
        }
    }
    for (i = 1; i <= CB_ENEMIES; i++)
        if (!(g->ac[i].flags & AC_ALIVE) && s_cbDeadTick[i] && s_cbTick - s_cbDeadTick[i] > 3u * SIM_HZ)
            cb_place(i, cb_phase(i));
    if (s_cbPendFire >= 0 && s_cbTick >= (u32)s_cbPendTick) {   /* weapons reads the radar sensor in its own tick */
        if (s_cbPendFire == PLAYER_ID) weapons_select(PLAYER_ID, s_cbType);
        else weapons_select(s_cbPendFire, MSL_LRM);
        weapons_fire(g, s_cbPendFire);
        if (s_cbPendFire == PLAYER_ID) cb_topup();
        s_cbPendFire = -1;
    }
    if ((s_cbTick % 90u) == 0 && weapons_live_count() < 9) {
        if (s_cbShooter == 0) {                      /* player: SRM at the near one (1), LRM at the far one (2) */
            s_cbType = s_cbType == MSL_LRM ? MSL_SRM : MSL_LRM;
            best = s_cbType == MSL_SRM ? 1 : 2;
            if (!(g->ac[best].flags & AC_ALIVE)) best = best == 1 ? 2 : 1;
            f = q_rotate(g->ac[PLAYER_ID].fs.qPath, v3(0.0f, 0.0f, 1.0f));
            if (g->ac[best].flags & AC_ALIVE) bestA = (float)acos(E_CLAMP(v3_dot(v3_norm(v3_sub(g->ac[best].fs.pos, g->ac[PLAYER_ID].fs.pos)), f), -1.0f, 1.0f));
            if ((g->ac[best].flags & AC_ALIVE) && bestA < E_DEG2RAD(60.0f)) {
                radar_designate(g, PLAYER_ID, best);
                s_cbPendFire = PLAYER_ID;
                s_cbPendTick = (int)s_cbTick + 2;
            }
        } else if (g->ac[s_cbShooter].flags & AC_ALIVE) {   /* an enemy locks the player and fires */
            radar_designate(g, s_cbShooter, PLAYER_ID);
            s_cbPendFire = s_cbShooter;
            s_cbPendTick = (int)s_cbTick + 2;
        }
        s_cbShooter = s_cbShooter == 0 ? ((s_cbTick / 90u) % 4u < 2u ? 3 : 4) : 0;   /* player, enemy 3, player, enemy 4 */
    }
    if ((s_cbTick % 240u) == 0) in->flare = 1;
    if (s_cbTick == 20u * SIM_HZ) g->ac[PLAYER_ID].hp = 40.0f;   /* from 20 s: "damaged" smoke trail (fx) is measured too */
}

/* terrain-following route autopilot (runs inside session_tick, tick step 4) */
static void bench_ap(const Game *g, int id, PilotInput *in)
{
    const FlightState *fs = &g->ac[id].fs;
    const Terrain *t = g->terrain;
    Vec3 w, f;
    float dx, dz, hdg, altCmd, gamma, tmax = 0.0f, x0, x1, z0, z1, look = 2500.0f;
    if (s_nwp <= 0) return;
    w = s_wp[s_wpi];
    dx = w.x - fs->pos.x;
    dz = w.z - fs->pos.z;
    if (dx * dx + dz * dz < 700.0f * 700.0f) {
        s_wpi = (s_wpi + 1) % s_nwp;
        w = s_wp[s_wpi];
        dx = w.x - fs->pos.x;
        dz = w.z - fs->pos.z;
    }
    hdg = (float)atan2(dx, dz);
    f = v3((float)sin(fs->heading), 0.0f, (float)cos(fs->heading));
    if (t) {
        x0 = E_MINF(fs->pos.x, fs->pos.x + f.x * look) - 300.0f; x1 = E_MAXF(fs->pos.x, fs->pos.x + f.x * look) + 300.0f;
        z0 = E_MINF(fs->pos.z, fs->pos.z + f.z * look) - 300.0f; z1 = E_MAXF(fs->pos.z, fs->pos.z + f.z * look) + 300.0f;
        tmax = terrain_area_max(t, x0, z0, x1, z1);
    }
    altCmd = E_MAXF(w.y, tmax + s_minAgl);
    gamma = E_CLAMP((altCmd - fs->pos.y) * 0.004f, -0.20f, 0.35f);
    ap_steer(fs, hdg, gamma, E_DEG2RAD(60.0f), in);
    in->thr = THR_NORMAL;
    if (s_route == BENCH_ROUTE_COMBAT) combat_step(in);
}

int bench_init(int secs, int route)
{
    const Terrain *t = g_game.terrain;
    const TerrainBase *b = t ? terrain_base(t) : NULL;
    float cx = t ? t->centreX : 0.0f, cz = t ? t->centreZ : 0.0f, hdg = 0.0f, V = 0.8f * 340.0f;
    Vec3 start;
    s_on = 1;
    s_secs = secs > 0 ? secs : 30;
    s_route = route;
    s_phase = 0;
    s_done = 0;
    s_frames = 0;
    s_nwp = s_wpi = 0;
    memset(s_hist, 0, sizeof(s_hist));
    s_histN = 0;
    memset(s_sum, 0, sizeof(s_sum));
    memset(s_max, 0, sizeof(s_max));
    switch (route) {
    case BENCH_ROUTE_VOLCANO:                       /* T10b (3x world): back and forth over the crater at 3600 m (~500 m over the 3077 m summit) */
        start = v3(cx - 12000.0f, 3600.0f, cz - 300.0f);
        hdg = E_DEG2RAD(90.0f);
        s_wp[s_nwp++] = v3(cx + 12000.0f, 3600.0f, cz + 300.0f);
        s_wp[s_nwp++] = v3(cx - 12000.0f, 3600.0f, cz - 300.0f);
        break;
    case BENCH_ROUTE_HIGH:                          /* T10b: 12000 m (altitude-scaled far ~1.9 x), back and forth over the island centre */
        start = v3(cx - 25000.0f, 12000.0f, cz - 500.0f);
        hdg = E_DEG2RAD(90.0f);
        s_wp[s_nwp++] = v3(cx + 25000.0f, 12000.0f, cz + 500.0f);
        s_wp[s_nwp++] = v3(cx - 25000.0f, 12000.0f, cz - 500.0f);
        break;
    case BENCH_ROUTE_CLOUDS:                        /* T10b: 3000 m (in the 2000-3600 m puff band) on a 20 km square around the
                                                       island centre, through the 8-35 km cloud ring, island always in view */
        start = v3(cx - 20000.0f, 3000.0f, cz - 20000.0f);
        hdg = 0.0f;
        s_wp[s_nwp++] = v3(cx - 20000.0f, 3000.0f, cz + 20000.0f);
        s_wp[s_nwp++] = v3(cx + 20000.0f, 3000.0f, cz + 20000.0f);
        s_wp[s_nwp++] = v3(cx + 20000.0f, 3000.0f, cz - 20000.0f);
        s_wp[s_nwp++] = v3(cx - 20000.0f, 3000.0f, cz - 20000.0f);
        break;
    case BENCH_ROUTE_COMBAT:                        /* T17: circle r 6 km at 3000 m over the sea, 4 enemies, missiles */
        {
            int k;
            s_cbC = v3(cx - 42000.0f, 3000.0f, cz);
            start = v3(s_cbC.x, 3000.0f, s_cbC.z + s_cbR);
            hdg = E_DEG2RAD(90.0f);                 /* clockwise at the north point */
            for (k = 1; k <= 12; k++) {
                float a = E_DEG2RAD(30.0f * (float)k);
                s_wp[s_nwp++] = v3(s_cbC.x + s_cbR * (float)sin(a), 3000.0f, s_cbC.z + s_cbR * (float)cos(a));
            }
        }
        break;
    default:                                        /* low: 60 m along the runway line, sea <-> island */
        {
            Vec3 th = b ? b->threshold : v3(-890.0f, 12.2f, -3980.0f);
            float rh = b ? E_DEG2RAD(b->rwyHeadingDeg) : E_DEG2RAD(270.0f), fx = (float)sin(rh), fz = (float)cos(rh);
            start = v3(th.x - 3000.0f * fx, 62.0f, th.z - 3000.0f * fz);   /* over the island, heading down the runway */
            if (t) start.y = E_MAXF(start.y, terrain_area_max(t, start.x - 300.0f, start.z - 300.0f, start.x + 300.0f,
                                                               start.z + 300.0f) + s_minAgl);   /* T03b: hills inland */
            hdg = rh;
            s_wp[s_nwp++] = v3(th.x + 9000.0f * fx, 60.0f, th.z + 9000.0f * fz);   /* out to sea past the coast */
            s_wp[s_nwp++] = v3(th.x - 3500.0f * fx, 60.0f, th.z - 3500.0f * fz);   /* back over the runway, inland */
        }
        break;
    }
    session_place_air(&g_game, start, hdg, V);
    session_set_autopilot(bench_ap);
    if (route == BENCH_ROUTE_COMBAT) combat_init();
    s_t0 = plat_now_us();
    log_printf(LOG_INFO, "bench: start route=%s secs=%d wps=%d start=%.0f,%.0f,%.0f hdg=%.0f", bench_route_name(route), s_secs,
               s_nwp, (double)start.x, (double)start.y, (double)start.z, (double)E_RAD2DEG(hdg));
    return 1;
}

void bench_frame(void)
{
    s64 now = plat_now_us();
    RStats rs;
    const SceneStats *ss = scene_stats();
    float ms, y;
    int v[8], i, h;
    if (!s_on || s_done) return;
    if (s_phase == 0) {
        if (now - s_t0 < BENCH_WARMUP_US) { r_stats(&rs, 1); return; }
        prof_reset();
        r_pace_stats(NULL, 1);
        r_stats(&rs, 1);
        s_tMeasure = now;
        s_phase = 1;
        log_printf(LOG_INFO, "bench: measuring");
        return;
    }
    r_stats(&rs, 1);
    ms = prof_last_frame_ms();
    if (!(ms >= 0.0f && ms < 100000.0f)) ms = 0.0f;
    h = (int)(ms * 10.0f);
    h = h < 0 ? 0 : (h > BH_N ? BH_N : h);
    s_hist[h]++;
    s_histN++;
    v[0] = ss->terrainTris; v[1] = ss->seaTris; v[2] = ss->baseTris; v[3] = ss->acTris; v[4] = ss->cloudQuads;
    v[5] = rs.draws; v[6] = rs.texChanges; v[7] = rs.tris;
    for (i = 0; i < 8; i++) { s_sum[i] += v[i]; if (v[i] > s_max[i]) s_max[i] = v[i]; }
    y = g_game.ac[PLAYER_ID].fs.pos.y;
    if (y < s_minAlt) s_minAlt = y;
    if (y > s_maxAlt) s_maxAlt = y;
    s_farSum += ss->farM;
    if (s_route == BENCH_ROUTE_COMBAT) {
        int ml = weapons_live_count(), tr = 0, ex = 0, pf = 0, fq = fx_combat_stats(&tr, &ex, &pf), nc = 0, j;
        for (j = 1; j < g_game.numAircraft; j++) if (radar_contact_state(PLAYER_ID, j) != CS_NONE) nc++;
        s_cbMslSum += ml; if (ml > s_cbMslMax) s_cbMslMax = ml;
        s_cbFxSum += fq; if (fq > s_cbFxMax) s_cbFxMax = fq;
        if (ex > s_cbExplMax) s_cbExplMax = ex;
        s_cbConSum += nc;
    }
    if (ss->farM > s_farMaxEff) s_farMaxEff = ss->farM;
    s_frames++;
    if (now - s_tMeasure >= (s64)s_secs * 1000000) s_done = 1;
}

int bench_done(void) { return s_on && s_done; }

static float pct_ms(float p)
{
    int i, acc = 0, want = (int)((float)s_histN * p + 0.5f);
    if (want < 1) want = 1;
    for (i = 0; i <= BH_N; i++) { acc += s_hist[i]; if (acc >= want) return (float)i * 0.1f + 0.05f; }
    return (float)BH_N * 0.1f;
}

int bench_write(const char *path)
{
    char hdr[2048];
    RPace pc;
    const ScenePreset *p = scene_preset();
    int sw = 0, sh = 0, n = s_frames > 0 ? s_frames : 1, k, used, ok;
    float p99 = pct_ms(0.99f), secs = (float)(plat_now_us() - s_tMeasure) * 1e-6f, fps = secs > 0.0f ? (float)s_frames / secs : 0.0f;
    float missPct;
    r_screen_size(&sw, &sh);
    r_pace_stats(&pc, 0);
    missPct = pc.presents > 0 ? 100.0f * (float)pc.misses / (float)pc.presents : 0.0f;
    used = e_snprintf(hdr, (int)sizeof(hdr),
        "bench: jet route=%s preset=%s res=%dx%d view=%s secs=%d frames=%d build=\"%s %s\"\n"
        "bench: fps_avg=%.2f low1_fps=%.2f p99_ms=%.2f p50_ms=%.2f\n"
        "bench: pace presents=%d misses=%d missed_pct=%.2f divisor=%d refresh=%.2f vblank=%d worst_blanks=%d "
        "hist=%d/%d/%d/%d/%d/%d/%d/%d\n"
        "bench: scene terrain_tris=%.0f/%d sea_tris=%.0f/%d base_tris=%.0f/%d ac_tris=%.0f/%d cloud_quads=%.0f/%d "
        "draws=%.0f/%d tex_changes=%.0f/%d tris=%.0f/%d (avg/max)\n"
        "bench: route alt_min=%.0f alt_max=%.0f far=%.0f fog=%.0f..%.0f lodScale=%.2f clouds=%d\n",
        bench_route_name(s_route), p->name, sw, sh, g_args.benchView == VIEW_COCKPIT ? "cockpit" : "chase", s_secs, s_frames,
        __DATE__, __TIME__, (double)fps, p99 > 0.0f ? 1000.0 / (double)p99 : 0.0, (double)p99, (double)pct_ms(0.5f),
        pc.presents, pc.misses, (double)missPct, pc.divisor, (double)pc.refreshHz, pc.vblankOk, pc.worstBlanks,
        pc.hist[1], pc.hist[2], pc.hist[3], pc.hist[4], pc.hist[5], pc.hist[6], pc.hist[7], pc.hist[8],
        s_sum[0] / n, s_max[0], s_sum[1] / n, s_max[1], s_sum[2] / n, s_max[2], s_sum[3] / n, s_max[3], s_sum[4] / n, s_max[4],
        s_sum[5] / n, s_max[5], s_sum[6] / n, s_max[6], s_sum[7] / n, s_max[7],
        (double)s_minAlt, (double)s_maxAlt, (double)p->farM, (double)p->fogStartM, (double)(p->fogEndFrac * p->farM),
        (double)p->lodScale, p->clouds);
    k = used > 0 ? used : 0;
    used = e_snprintf(hdr + k, (int)sizeof(hdr) - k, "bench: far_eff avg=%.0f max=%.0f (T10b: altitude-aware far plane; far/fog above = sea level)\n",
                      s_farSum / n, (double)s_farMaxEff);
    k += used > 0 ? used : 0;
    if (s_route == BENCH_ROUTE_COMBAT) {
        used = e_snprintf(hdr + k, (int)sizeof(hdr) - k, "bench: combat missiles_avg=%.1f missiles_max=%d fx_quads_avg=%.0f fx_quads_max=%d "
                          "explosions_max=%d contacts_avg=%.1f\n", s_cbMslSum / n, s_cbMslMax, s_cbFxSum / n, s_cbFxMax, s_cbExplMax,
                          s_cbConSum / n);
        k += used > 0 ? used : 0;
    }
    e_snprintf(hdr + k, (int)sizeof(hdr) - k, "bench: gate15 pass=%d gate30 pass=%d (fps_avg >= 14.9 / 29.8 and missed <= 2 %%)\n",
               fps >= 14.9f && missPct <= 2.0f, fps >= 29.8f && missPct <= 2.0f);
    ok = prof_write_report(path, hdr);
    log_printf(LOG_INFO, "bench: route=%s fps_avg=%.2f low1=%.2f missed_pct=%.2f terrain_tris=%.0f draws=%.0f written=%d path=%s",
               bench_route_name(s_route), (double)fps, p99 > 0.0f ? 1000.0 / (double)p99 : 0.0, (double)missPct,
               s_sum[0] / n, s_sum[5] / n, ok, path);
    return ok;
}
