/* main.cpp - WinMain, init order, 120 Hz fixed-tick loop, app states, live keys, pause menu, replays, overlays
   (DESIGN 2.5-2.7, 5.4, 16). Windows. Owner: T01 (skeleton), T08 (M1 flow), T17 (modes), T19 (frontend states).
   Loop: integer QPC accumulator in units of (QPC ticks * SIM_HZ), so one sim tick = exactly qpf units (no rounding
   drift); clamped to 0.25 s after hitches, at most 8 ticks per rendered frame, render interpolation alpha.
   -headless runs the sim flat out (no window, null renderer, no sound/input; stops after 120 s without -exitafter).
   M1 flow (T08): loading screen -> start_flight (session_start: runway READY, or -start air) -> FLIGHT (FREE).
   The sim is the portable session in game.cpp (session_tick); its controls per tick come from, in this order:
   -replay (recorded PilotKeys + command), -script, the -bench autopilot (keys idle), live keys (e_input actions,
   rebindable in [controls]). Every interactive flight records last.rpl (-record FILE elsewhere); live param
   changes (F5 reload, F6 Novice/Expert toggle, script param lines) write a PARAMS record, so replays stay exact.
   Keys: Esc/P pause menu (resume / restart on runway / quit), F5 reload jet.ini, F6 Expert/Novice, C view, Tab map
   range, F11 fps, F12 screenshot shots/jet_NNN.bmp; dev (-dev or [dev] keys=1): F9 air respawn, F10 feel overlay.
   Logs: jet.log for play; -test/-headless/-exitafter/-script/-replay/-dumpini/-hudtest -> jet_test.log; -bench ->
   jet_bench.log; -logfile NAME overrides (DESIGN 16.6: never overwrite the player's log from a test).
   Exit codes: 0 normal, 1 init failure (or replay hash differences), -test: number of failures (-1 unknown / not
   implemented), -script: number of failed expect lines, -aifight: failed checks.
   M2 (T17): -mode free|training|sortie, -diff 0|1|2 (default free / Normal). The mission director (mission.h: drones,
   waves, damage, lives, respawns) is on for every flight started here. -headless with a combat mode = the robot
   sortie: the player is flown by T14's robot (ai_robot_hook, A held through every takeoff); the run stops when the
   mission ends and always logs a "mission: result" line. Combat HUD (hudtgt/hudwpn, params + init + reset), combat
   sound (snd_combat), fx (fx.cpp T17 section) and mission messages are wired here. V held = look at the TD target
   (cockpit: the head turns up to 120 deg, the flight HUD is hidden meanwhile; chase: the camera swings behind the jet
   to frame jet + target). Results panel after MISSION COMPLETE / FAILED; pause menu adds RESTART MISSION in TRAINING
   and SORTIE. -aifight N = T14's headless robot-vs-waves statistics (simh_aifight).
   M3 (T19): plain jet.exe (no -mode/-start/-script/-replay/-bench/-camat/-record/-telemetry/-exitafter/-headless)
   starts at the TITLE (menu.h): loading screen, then the frontend over an in-engine fly-by (a FREE session that is
   drawn but never ticked; only its clock runs for the clouds). The menu returns APP_LOADING with g_args.mode/diff
   -> start_flight. Pause = menu_pause_* (resume / restart on runway / restart mission / options / quit to menu).
   A SORTIE that ends (COMPLETE / FAILED) freezes 3 s after the end into APP_RESULTS (the sim stops, so remaining
   enemies stop fighting: T17 open issue 5); the results screen is drawn over the frozen scene; CONTINUE -> title,
   FLY AGAIN -> a new flight. Every mode switch keeps the old direct start (benches, tests, batch launchers).
   -mscript FILE runs a menu flow (menu.cpp "menu script"; log jet_test.log; exit code = failures); -ini FILE uses
   another ini (exe-relative), e.g. the t19_jet.ini a menu script saved. Window caption "Ashfall". */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "e_base.h"
#include "e_math.h"
#include "e_log.h"
#include "e_cfg.h"
#include "e_file.h"
#include "e_prof.h"
#include "e_plat.h"
#include "e_rend.h"
#include "e_input.h"
#include "e_audio.h"
#include "e_ui.h"
#include "e_tex.h"
#include "e_terrain.h"
#include "game.h"
#include "params.h"
#include "pilot.h"
#include "flight.h"
#include "takeoff.h"
#include "world.h"
#include "radar.h"
#include "target.h"
#include "weapons.h"
#include "dlz.h"
#include "ai.h"
#include "mission.h"
#include "replay.h"
#include "tests.h"
#include "camera.h"
#include "scene.h"
#include "hud.h"
#include "hud_font.h"
#include "hud_map.h"
#include "hud_tgt.h"
#include "hud_wpn.h"
#include "fx.h"
#include "snd.h"
#include "snd_combat.h"
#include "menu.h"
#include "scatter.h"
#include "bench.h"

static Cfg   *s_ini;
static char   s_iniPath[E_PATH];
static int    s_app = APP_NONE;
static int    s_headless;
static u32    s_tick;                               /* sim ticks since start (all app states) */
static u32    s_exitTicks;                          /* -exitafter in ticks, 0 = never */
static int    s_frames;
static int    s_quit;
static const char *s_quitReason = "none";
static int    s_exitCode;
static int    s_shotDone[ARGS_MAX_SHOTS];
static int    s_secSim, s_secDraw, s_secPresent, s_secHud, s_secDlz;   /* T18: dlz section */
static int    s_scene;                              /* scene_init succeeded */
static Camera s_cam;
static s64    s_lastFrameUs;
static Terrain s_terrain;
static int    s_terrainLoaded;
static u32    s_exeCrc;
static int    s_playing, s_recording, s_scripted, s_bench;
static int    s_paused;
static int    s_pendingCmd;                         /* SES_CMD_* for the next tick (pause menu, F9) */
static int    s_lastRespawns;
static char   s_lastMsg[32]; static u32 s_msgTick;  /* sticky takeoff messages ("A: TAKEOFF", "YOU HAVE CONTROL") */
static int    s_feel;                               /* F10 overlay */
static int    s_shotReq;                            /* F12 */
static PilotKeys s_lastKeys;
/* T17: combat */
static int    s_robot;                              /* headless combat mode: the robot player flies */
static u32    s_misMsg;                             /* mission messages forwarded to the HUD */
static int    s_restartMission;                     /* pause menu RESTART MISSION */
static float  s_lookK;                              /* V look-at-target blend 0..1 */
static Vec3   s_lookTp;                             /* last look target position */
static int    s_lookHave;
static int    s_sideCam;                            /* dev: -benchview side (bench screenshots of trails/flares/smoke) */
/* T19: frontend */
static Camera s_titleCam;                           /* title fly-by */
static u32    s_titleTicks;                         /* ticks spent on the title (fly-by clock) */
static int    s_wantTitle;                          /* plain start: title screen instead of a direct flight */

/* feel overlay history (4 s at 120 Hz) */
enum { FEEL_N = 480 };
static float s_hUp[FEEL_N], s_hUr[FEEL_N], s_hN[FEEL_N];
static int   s_hPos;

/* ---------------------------------------------------------------- jet input actions ([controls], DESIGN 5.4) */
enum { JA_PITCH_UP, JA_PITCH_DOWN, JA_ROLL_LEFT, JA_ROLL_RIGHT, JA_FAST, JA_SLOW, JA_TARGET, JA_FIRE, JA_WEAPON,
       JA_FLARE, JA_VIEW, JA_LOOK, JA_MAP, JA_PAUSE, JA_STICK_X, JA_STICK_Y, JA_COUNT };
static const char *const s_jaName[JA_COUNT] = { "pitchUp", "pitchDown", "rollLeft", "rollRight", "fast", "slow",
    "target", "fire", "weapon", "flare", "view", "look", "mapRange", "pause", "stickX", "stickY" };
static const char *const s_jaBind[JA_COUNT][IN_SLOTS] = {
    { "key UP", "none", "none" }, { "key DOWN", "none", "none" }, { "key LEFT", "none", "none" },
    { "key RIGHT", "none", "none" }, { "key A", "none", "none" }, { "key Z", "none", "none" },
    { "key S", "none", "none" }, { "key SPACE", "none", "none" }, { "key D", "none", "none" },
    { "key F", "none", "none" }, { "key C", "none", "none" }, { "key V", "none", "none" },
    { "key TAB", "none", "none" }, { "key ESC", "key P", "none" }, { "none", "none", "j0 x" }, { "none", "none", "j0 y" },
};

static void jet_input_setup(void)
{
    int a, s;
    InBinding b;
    in_bind_clear();
    for (a = 0; a < IN_MAX_ACTIONS; a++) in_action_define(a, a < JA_COUNT ? s_jaName[a] : "");
    for (a = 0; a < JA_COUNT; a++)
        for (s = 0; s < IN_SLOTS; s++)
            if (in_binding_parse(s_jaBind[a][s], &b)) in_bind(a, s, b.kind, b.code, b.invert);
}

static const char *app_name(int a)
{
    switch (a) {
    case APP_NONE: return "none";
    case APP_TITLE: return "title";
    case APP_MENU: return "menu";
    case APP_LOADING: return "loading";
    case APP_FLIGHT: return "flight";
    case APP_RESULTS: return "results";
    case APP_IDLE: return "idle";
    case APP_QUIT: return "quit";
    }
    return "?";
}

static void quit(const char *reason)
{
    if (!s_quit) {
        s_quit = 1;
        s_quitReason = reason;
    }
}

static void set_app(int a)
{
    if (a == s_app) return;
    log_printf(LOG_INFO, "boot: app from=%s to=%s tick=%u", app_name(s_app), app_name(a), s_tick);
    s_app = a;
    if (a == APP_QUIT) quit("app");
}

static void arg_path(char *out, const char *p)
{
    if (p[0] == '\\' || p[0] == '/' || (p[0] && p[1] == ':')) e_strlcpy(out, p, E_PATH);
    else base_path(out, E_PATH, p);
}

/* ---------------------------------------------------------------- session start (APP_LOADING) */
static void start_flight(void)
{
    char path[E_PATH];
    ReplaySetup rs;
    Aircraft *pl;
    memset(&rs, 0, sizeof(rs));
    rs.mode = g_args.mode;
    rs.diff = g_args.diff;
    rs.start = g_args.start;
    rs.seed = g_args.haveSeed ? g_args.seed : (u32)plat_qpc();
    if (!s_terrainLoaded) {
        s64 t0 = plat_qpc();
        int ok;
        data_path(path, (int)sizeof(path), "island/jet.isl");
        ok = terrain_load(&s_terrain, path);
        s_terrainLoaded = 1;
        log_printf(LOG_INFO, "boot: terrain loaded=%d path=%s ms=%.0f", ok, path,
                   (double)(plat_qpc() - t0) * 1000.0 / (double)plat_qpf());
    }
    if (g_args.replay[0]) {                         /* -replay FILE: the file's setup and params */
        arg_path(path, g_args.replay);
        s_playing = replay_play_open(path, &rs, s_exeCrc);
        if (!s_playing) { s_exitCode = 1; quit("replay_open"); }
    }
    mission_set_director(1);                        /* T17: drones, waves, damage, lives (FREE: nothing) */
    session_start(&g_game, &s_terrain, rs.mode, rs.diff, rs.start, rs.seed);
    pl = &g_game.ac[PLAYER_ID];
    s_misMsg = 0;
    s_robot = (s_headless || menu_script_robot()) && !s_playing && rs.mode != MODE_FREE && !g_args.bench;
    if (s_robot) {                                  /* headless robot sortie / training (T14 robot, T17 acceptance 2) */
        session_set_autopilot(ai_robot_hook);
        ai_spawn_robot(&g_game, PLAYER_ID);
        log_printf(LOG_INFO, "boot: robot player on mode=%d diff=%d", rs.mode, rs.diff);
    }
    if (g_args.script[0] && !s_playing) {
        arg_path(path, g_args.script);
        s_scripted = script_open(path);
        if (!s_scripted) s_exitCode = 1;
    }
    if (g_args.bench && !s_headless && !s_playing) {
        s_bench = bench_init(g_args.bench, g_args.benchRoute);
    }
    if (!s_playing) {                               /* -record FILE, or last.rpl for every interactive flight */
        int autoRec = !s_headless && !s_bench && !s_scripted && g_args.exitAfter <= 0.0f;
        if (g_args.record[0] || autoRec) {
            if (g_args.record[0]) arg_path(path, g_args.record);
            else base_path(path, (int)sizeof(path), "last.rpl");
            s_recording = replay_record_open(path, &rs, s_exeCrc);
        }
    }
    if (g_args.telemetry[0]) {
        arg_path(path, g_args.telemetry);
        log_printf(LOG_INFO, "boot: telemetry path=%s ok=%d", path, flight_telemetry_open(path));
    }
    log_printf(LOG_INFO, "boot: flight_start mode=%d start=%d seed=%u replay=%d record=%d script=%d bench=%d pilot=%s",
               rs.mode, rs.start, rs.seed, s_playing, s_recording, s_scripted, s_bench, pilot_mode_name(pilot_mode()));
    if (!s_headless) {
        if (!s_scene) s_scene = scene_init(&g_game);
        cam_reset(&s_cam, pl);
        if (g_args.haveCamat) {
            cam_set_fixed(&s_cam, v3(g_args.camat[0], g_args.camat[1], g_args.camat[2]), g_args.camat[3], g_args.camat[4]);
            log_printf(LOG_INFO, "boot: camat %.0f %.0f %.0f hdg=%.0f pitch=%.0f", (double)g_args.camat[0],
                       (double)g_args.camat[1], (double)g_args.camat[2], (double)g_args.camat[3], (double)g_args.camat[4]);
        }
        if (s_bench) s_cam.mode = g_args.benchView;
        {
            const char *bv = arg_str("-benchview", 0, NULL);
            s_sideCam = s_bench && bv && !e_stricmp(bv, "side");
            if (s_sideCam) s_cam.mode = VIEW_CHASE;
        }
        hud_reset();
        hudtgt_reset();
        hudwpn_reset();
        fx_reset();
        sndc_reset();
        s_lookK = 0.0f;
        s_lookHave = 0;
    }
    s_lastRespawns = session_respawns();
    s_paused = 0;
    if (!s_headless) {
        menu_flight_started(rs.mode, rs.diff);      /* T19: best score before this flight (NEW RECORD) */
        menu_enter(APP_FLIGHT);                     /* frees the title logo tiles */
    }
    if (s_headless && !s_exitTicks && !s_playing) {
        s_exitTicks = (u32)(120 * SIM_HZ);
        log_printf(LOG_INFO, "boot: headless without -exitafter: stopping after 120 s");
    }
    set_app(APP_FLIGHT);
}

/* ---------------------------------------------------------------- live input -> PilotKeys */
static void read_live_keys(PilotKeys *k)
{
    memset(k, 0, sizeof(*k));
    k->up = in_action(JA_PITCH_UP) >= 0.5f;
    k->down = in_action(JA_PITCH_DOWN) >= 0.5f;
    k->left = in_action(JA_ROLL_LEFT) >= 0.5f;
    k->right = in_action(JA_ROLL_RIGHT) >= 0.5f;
    k->fast = in_action(JA_FAST) >= 0.5f;
    k->slow = in_action(JA_SLOW) >= 0.5f;
    k->tgtHeld = in_action_held_ticks(JA_TARGET);
    k->tgtReleased = in_action_released(JA_TARGET);
    k->fire = in_action(JA_FIRE) >= 0.5f;
    k->wpnNext = in_action(JA_WEAPON) >= 0.5f;
    k->flare = in_action(JA_FLARE) >= 0.5f;
    k->view = in_action(JA_VIEW) >= 0.5f;
    k->look = in_action(JA_LOOK) >= 0.5f;
    if (in_axis_bound(JA_STICK_X) || in_axis_bound(JA_STICK_Y)) {   /* DirectInput Y + = stick back = nose up */
        k->joyValid = 1;
        k->joyX = in_axis(JA_STICK_X);
        k->joyY = in_axis(JA_STICK_Y);
    }
    menu_script_fly_keys(k);                        /* T19: -mscript "fly KEY on" */
}

/* a live param change (F5, F6, script): replays need the PARAMS record at this tick */
static void params_changed(void)
{
    if (s_recording && replay_active() == 1) replay_record_params(g_game.tick);
}

static void sticky_messages(void)
{
    const char *m = takeoff_message(session_takeoff(), &g_game);
    if (!m) { s_lastMsg[0] = 0; return; }
    if (strcmp(m, s_lastMsg) || g_game.tick >= s_msgTick) {
        hud_message(m, 1.0f);                       /* re-queued every second while it applies (one log line/s) */
        e_strlcpy(s_lastMsg, m, (int)sizeof(s_lastMsg));
        s_msgTick = g_game.tick + SIM_HZ;
    }
}

/* ---------------------------------------------------------------- one flight tick (DESIGN 2.6 order: game.cpp) */
static void flight_tick(void)
{
    PilotKeys keys;
    int cmd = s_pendingCmd;
    Aircraft *pl = &g_game.ac[PLAYER_ID];
    s_pendingCmd = 0;
    if (s_playing) {
        if (!replay_play_tick(g_game.tick, &keys, &cmd, &g_game)) {
            ReplayStats st;
            replay_play_close();
            replay_stats(&st);
            s_playing = 0;
            if (st.diffs) s_exitCode = 1;
            quit("replay_end");
            return;
        }
    } else {
        if (s_scripted) {
            int scmd = 0;
            if (script_step(&g_game, &keys, &scmd)) params_changed();
            if (scmd) cmd = scmd;
        } else if (!s_headless && !s_bench) read_live_keys(&keys);
        else memset(&keys, 0, sizeof(keys));
        if (s_robot && takeoff_active(session_takeoff())) keys.fast = 1;   /* the robot's takeoff: A held */
        replay_quantise_keys(&keys);
        if (s_recording) replay_record_tick(g_game.tick, &keys, cmd, &g_game);
    }
    if ((g_game.tick % REPLAY_HASH_EVERY) == 0)
        log_printf(LOG_DEBUG, "sim: tick=%u hash=%08x", g_game.tick, replay_state_hash(&g_game));
    session_tick(&g_game, &keys, cmd);
    s_lastKeys = keys;
    if (s_robot && session_respawns() != s_lastRespawns) {   /* new life: the robot brain again (T14 note 6) */
        s_lastRespawns = session_respawns();
        ai_spawn_robot(&g_game, PLAYER_ID);
    }
    if (s_robot && s_headless && mission_stats()->outcome != MIS_RUNNING) quit("mission_end");
    flight_telemetry_row(g_game.tick - 1, &keys, &pl->in, &pl->fs);
    if (!s_headless) {
        if (session_pilot_out()->view && !s_bench) cam_toggle(&s_cam);
        if (session_respawns() != s_lastRespawns) {  /* respawn / restart: snap camera and HUD */
            s_lastRespawns = session_respawns();
            s_cam.snap = 1;
            hud_reset();
            hudwpn_reset();
        }
        sticky_messages();
        while (s_misMsg < mission_msg_count()) {    /* T17: WAVE n, DAMAGED, MISSION COMPLETE ... */
            float secs = 2.0f;
            const char *m = mission_msg(s_misMsg++, &secs);
            if (m) hud_message(m, secs);
        }
        s_hUp[s_hPos] = pl->in.up;
        s_hUr[s_hPos] = pl->in.ur;
        s_hN[s_hPos] = pl->fs.n;
        s_hPos = (s_hPos + 1) % FEEL_N;
        fx_tick(&g_game);
    }
    if (s_scripted && script_done(&g_game) && s_headless && !s_exitTicks) quit("script_end");
}

/* ---------------------------------------------------------------- T19: title backdrop, results, quit to menu */
static void load_terrain(void)
{
    char path[E_PATH];
    s64 t0;
    int ok;
    if (s_terrainLoaded) return;
    t0 = plat_qpc();
    data_path(path, (int)sizeof(path), "island/jet.isl");
    ok = terrain_load(&s_terrain, path);
    s_terrainLoaded = 1;
    log_printf(LOG_INFO, "boot: terrain loaded=%d path=%s ms=%.0f", ok, path, (double)(plat_qpc() - t0) * 1000.0 / (double)plat_qpf());
}

/* the title screen: a FREE session as the fly-by backdrop (drawn, never ticked) */
static void go_title(void)
{
    if (s_recording) { replay_record_close(&g_game); s_recording = 0; }
    if (s_app == APP_FLIGHT && mission_director() && g_game.mode != MODE_FREE && mission_stats()->outcome == MIS_RUNNING)
        mission_log_result(&g_game, "menu");
    flight_telemetry_close();
    s_paused = 0;
    s_robot = 0;
    s_pendingCmd = 0;
    load_terrain();
    session_start(&g_game, &s_terrain, MODE_FREE, DIFF_NORMAL, START_RUNWAY, 1);
    if (!s_scene) s_scene = scene_init(&g_game);
    fx_reset();
    sndc_reset();
    memset(&s_titleCam, 0, sizeof(s_titleCam));
    cam_reset(&s_titleCam, &g_game.ac[PLAYER_ID]);
    s_titleTicks = 0;
    set_app(APP_TITLE);
    menu_enter(APP_TITLE);
}

/* T17 open issue 5 / T19: a SORTIE that ended (COMPLETE / FAILED) ends the flight into the results screen 3 s later
   (the sim stops: the remaining enemies stop fighting). Live interactive flights only (replays, scripts, benches and
   headless runs keep their T17 behaviour). */
static int results_due(void)
{
    const MissionStats *ms = mission_stats();
    if (s_headless || s_playing || s_bench || s_scripted || !mission_director() || g_game.mode != MODE_SORTIE) return 0;
    if (ms->outcome == MIS_RUNNING) return 0;
    return g_game.tick >= ms->endTick + 3 * SIM_HZ;
}

static void go_results(void)
{
    if (s_recording) { replay_record_close(&g_game); s_recording = 0; }
    log_printf(LOG_INFO, "menu: flight ends into results outcome=%s tick=%u", mission_outcome_name(mission_stats()->outcome), g_game.tick);
    s_paused = 0;
    set_app(APP_RESULTS);
    menu_enter(APP_RESULTS);
}

/* T17: RESTART MISSION = a fresh session (new seed), recording restarted (last.rpl = the new flight) */
static void restart_mission(void)
{
    s_restartMission = 0;
    if (s_recording) { replay_record_close(&g_game); s_recording = 0; }
    log_printf(LOG_INFO, "menu: restart mission mode=%d diff=%d tick=%u", g_game.mode, g_game.diff, g_game.tick);
    g_args.haveSeed = 0;
    start_flight();
}

/* ---------------------------------------------------------------- one 120 Hz tick */
static void sim_tick(void)
{
    static int fpuWarned;
    int appBefore = s_app;                          /* T19: a screen entered in this tick gets its keys from the next */
    if (!plat_fpu_ok()) {
        if (!fpuWarned) log_printf(LOG_WARN, "warn: fpu control word changed cw=0x%05x tick=%u", plat_fpu_cw(), s_tick);
        fpuWarned = 1;
        plat_fpu_setup();
    }
    if (!s_headless) {
        int pauseHit;
        in_poll();
        if (menu_script_active() && menu_script_tick(s_app, s_paused)) quit("mscript");
        pauseHit = (in_action_hit(JA_PAUSE) && plat_active()) || menu_script_hit(MK_PAUSE);
        if (in_key_hit(VK_F11)) g_args.fps = !g_args.fps;
        if (in_key_hit(VK_F12)) s_shotReq = 1;
        if (s_app == APP_FLIGHT && !s_bench) {
            if (s_paused) {                         /* T19: pause menu (menu.cpp) */
                int r = menu_pause_tick(pauseHit);
                if (menu_params_dirty()) params_changed();   /* options changed a param: replay PARAMS record */
                if (r == MP_RESUME) { s_paused = 0; log_printf(LOG_INFO, "menu: resume tick=%u", g_game.tick); }
                else if (r == MP_RUNWAY) { if (!s_playing) s_pendingCmd = SES_CMD_RUNWAY; s_paused = 0; }
                else if (r == MP_RESTART_MISSION) { if (!s_playing) s_restartMission = 1; s_paused = 0; }
                else if (r == MP_QUIT_MENU) {
                    if (s_playing) quit("menu_quit");
                    else go_title();
                }
            } else if (pauseHit) {
                s_paused = 1;
                menu_pause_open(g_game.mode);
                log_printf(LOG_INFO, "menu: pause tick=%u", g_game.tick);
            }
            if (!s_playing) {
                if (in_key_hit(VK_F5)) {            /* DESIGN 2.5: live reload at a tick boundary */
                    char buf[32];
                    int n = params_reload(s_ini, s_iniPath);
                    if (n >= 0) e_snprintf(buf, (int)sizeof(buf), "INI RELOADED (%d)", n);
                    else e_strlcpy(buf, "INI RELOAD FAILED", (int)sizeof(buf));
                    hud_message(buf, 2.0f);
                    snd_ui(0);
                    params_changed();
                }
                if (in_key_hit(VK_F6)) {            /* T05: Expert <-> Novice (a param: replays get a PARAMS record) */
                    int m = pilot_toggle_mode();
                    hud_message(pilot_mode_name(m), 2.0f);
                    snd_ui(0);
                    params_changed();
                }
                if (g_args.dev && in_key_hit(VK_F9)) s_pendingCmd = SES_CMD_AIR;
            }
            if (g_args.dev && in_key_hit(VK_F10)) s_feel = !s_feel;
            if (in_action_hit(JA_MAP) && s_app == APP_FLIGHT) map_cycle_range();
        }
        if (s_bench && pauseHit) quit("escape");
    }
    switch (s_app) {
    case APP_TITLE:
    case APP_MENU:
    case APP_RESULTS: {
        int next;
        if (s_app != APP_RESULTS) { g_game.tick++; s_titleTicks++; }   /* the backdrop's clock (clouds, props) */
        if (appBefore != s_app) break;              /* the Enter of QUIT TO MENU must not also press FLY */
        next = menu_tick(s_app == APP_MENU ? APP_TITLE : s_app);
        if (next == APP_TITLE && s_app == APP_RESULTS) go_title();
        else if (next == APP_LOADING) { set_app(APP_LOADING); menu_draw_loading("PREPARING FLIGHT"); }
        else if (next == APP_QUIT) set_app(APP_QUIT);
        break; }
    case APP_LOADING:
        start_flight();
        break;
    case APP_FLIGHT:
        if (s_restartMission) restart_mission();
        if (!s_paused) flight_tick();
        if (s_app == APP_FLIGHT && !s_paused && results_due()) go_results();
        break;
    case APP_IDLE:
        if ((s_tick % SIM_HZ) == 0) log_printf(LOG_INFO, "boot: idle ticks=%u frames=%d", s_tick, s_frames);
        break;
    default:
        break;
    }
    s_tick++;
    if ((s_tick % SIM_HZ) == 0) log_flush();
    if (s_exitTicks && s_tick >= s_exitTicks) quit("exitafter");
}

/* ---------------------------------------------------------------- overlays (HUD font, hg_*) */
static const char *thr_name(int t) { return t == THR_FAST ? "FAST" : (t == THR_SLOW ? "SLOW" : "NORMAL"); }

static void draw_feel(void)
{
    const Aircraft *pl = &g_game.ac[PLAYER_ID];
    const FlightState *fs = &pl->fs;
    char buf[96];
    u32 col = hg_col(0xFFFFFFFF, 0.9f), cU = hg_col(0xFF60C0FF, 1.0f), cR = hg_col(0xFFFFC040, 1.0f), cN = hg_col(0xFFFF6060, 1.0f);
    int x = hg_px(6.0f), y = hg_px(6.0f), dy = hg_cap(HF_SMALL) + hg_px(4.0f), i, gx0, gy0, gw, gh;
    float sx, sy;
    e_snprintf(buf, (int)sizeof(buf), "MODE %s  THR %s  KEYS %s%s%s%s%s%s", pilot_mode_name(pilot_mode()), thr_name(fs->thr),
               s_lastKeys.up ? "U" : "-", s_lastKeys.down ? "D" : "-", s_lastKeys.left ? "L" : "-", s_lastKeys.right ? "R" : "-",
               s_lastKeys.fast ? "A" : "-", s_lastKeys.slow ? "Z" : "-");
    hg_text(x, y, HF_SMALL, col, 0, buf); y += dy;
    e_snprintf(buf, (int)sizeof(buf), "UP %+.2f  UR %+.2f  ROLL %.0f DEG/S", (double)pl->in.up, (double)pl->in.ur,
               (double)E_RAD2DEG(fs->p));
    hg_text(x, y, HF_SMALL, col, 0, buf); y += dy;
    e_snprintf(buf, (int)sizeof(buf), "N %.1f  NCMD %.1f  NLIM %.1f  NHOLD %.2f  PEAK %.1f", (double)fs->n, (double)fs->nCmd,
               (double)fs->nLim, (double)fs->nHold, (double)fs->peakG);
    hg_text(x, y, HF_SMALL, col, 0, buf); y += dy;
    e_snprintf(buf, (int)sizeof(buf), "V %.0f KT  VT %.0f KT  M %.2f  VS %.0f FT/M", (double)(fs->V * MS2KT), (double)(fs->Vt * MS2KT),
               (double)fs->mach, (double)(fs->vs * M2FT * 60.0f));
    hg_text(x, y, HF_SMALL, col, 0, buf); y += dy;
    e_snprintf(buf, (int)sizeof(buf), "TURN %.1f  PITCH %.1f DEG/S  AOA %.1f", (double)E_RAD2DEG(fs->turnRate),
               (double)E_RAD2DEG(fs->pitchRate), (double)E_RAD2DEG(fs->alpha));
    hg_text(x, y, HF_SMALL, col, 0, buf); y += dy;
    e_snprintf(buf, (int)sizeof(buf), "BANK %.0f  GAMMA %.1f  CAP %d  AB %.2f  BRK %.2f  %s", (double)E_RAD2DEG(fs->bank),
               (double)E_RAD2DEG(fs->gamma), fs->capped, (double)fs->ab, (double)fs->brake,
               takeoff_phase_name(session_takeoff()->phase));
    hg_text(x, y, HF_SMALL, col, 0, buf); y += dy;
    /* 4 s graph: up (blue), ur (orange), n / 9 (red); zero line */
    gx0 = x; gy0 = y + hg_px(4.0f); gw = hg_px(160.0f); gh = hg_px(40.0f);
    hg_rect((float)gx0, (float)gy0, (float)(gx0 + gw), (float)(gy0 + gh), hg_col(0xFF000000, 0.45f));
    hg_line((float)gx0, (float)(gy0 + gh / 2), (float)(gx0 + gw), (float)(gy0 + gh / 2), hg_col(0xFFFFFFFF, 0.3f));
    sx = (float)gw / (float)(FEEL_N / 8);
    sy = 0.5f * (float)gh;
    for (i = 0; i < FEEL_N / 8 - 1; i++) {
        int a = (s_hPos + i * 8) % FEEL_N, b = (s_hPos + (i + 1) * 8) % FEEL_N;
        float xa = (float)gx0 + sx * (float)i, xb = xa + sx, yc = (float)gy0 + sy;
        hg_line_w(xa, yc - sy * s_hUp[a], xb, yc - sy * s_hUp[b], 0.5f, cU);
        hg_line_w(xa, yc - sy * s_hUr[a], xb, yc - sy * s_hUr[b], 0.5f, cR);
        hg_line_w(xa, yc - sy * E_CLAMP(s_hN[a] / 9.0f, -1.0f, 1.0f), xb, yc - sy * E_CLAMP(s_hN[b] / 9.0f, -1.0f, 1.0f), 0.5f, cN);
    }
}

/* T17: V look-at-target (DESIGN 7). Post-processes the camera of cam_update (camera.cpp untouched: its chase smoothing
   keeps running underneath, so letting go of V swings back smoothly). Cockpit: the head turns towards the TD target,
   at most 120 deg off the nose. Chase: the eye moves behind the jet on the line from the target, so jet and target
   are both in frame. Blend 0..1 with 0.15 s smoothing. */
static void look_camera(Camera *c, float alpha, float frameDt)
{
    const Aircraft *pl = &g_game.ac[PLAYER_ID];
    int td = radar_designated(PLAYER_ID), want;
    float k, dt = E_CLAMP(frameDt, 0.0f, 0.1f);
    Vec3 f0, u0, d, up, eye, pos;
    Quat qn;
    ContactView cv;
    want = session_pilot_out()->look && (pl->flags & AC_ALIVE) && !c->fixed && !c->wreck && !s_bench;
    if (want && td >= 0 && radar_view_get(&g_game, PLAYER_ID, td, &cv)) {
        radar_view_pos(&g_game, &cv, alpha, &s_lookTp);
        s_lookHave = 1;
    } else if (td < 0) want = 0;
    k = dt > 0.0f ? 1.0f - (float)exp(-dt / 0.15f) : 1.0f;
    s_lookK += ((want && s_lookHave ? 1.0f : 0.0f) - s_lookK) * k;
    if (s_lookK < 0.005f || !s_lookHave) { s_lookK = want ? s_lookK : 0.0f; return; }
    f0 = v3(c->view.m[0][2], c->view.m[1][2], c->view.m[2][2]);
    u0 = v3(c->view.m[0][1], c->view.m[1][1], c->view.m[2][1]);
    cam_ac_pose(pl, alpha, &pos, &qn);
    if (c->mode == VIEW_COCKPIT) {
        Vec3 nose = q_rotate(qn, v3(0.0f, 0.0f, 1.0f)), t = v3_norm(v3_sub(s_lookTp, c->eye));
        float ang = (float)acos(E_CLAMP(v3_dot(nose, t), -1.0f, 1.0f)), lim = E_DEG2RAD(120.0f);
        if (ang > lim) {                            /* turn the head only 120 deg towards the target */
            Vec3 ax = v3_cross(nose, t);
            if (v3_len(ax) < 1e-4f) ax = q_rotate(qn, v3(0.0f, 1.0f, 0.0f));
            t = q_rotate(q_axis_angle(v3_norm(ax), lim), nose);
        }
        d = v3_norm(v3_lerp(f0, t, s_lookK));
        up = v3_norm(v3_lerp(u0, v3(0.0f, 1.0f, 0.0f), s_lookK));
        eye = c->eye;
    } else {
        Vec3 los = v3_sub(s_lookTp, pos), dir;
        float L = v3_len(los);
        dir = L > 1.0f ? v3_scale(los, 1.0f / L) : f0;
        eye = v3_add(v3_madd(pos, dir, -30.0f), v3(0.0f, 7.0f, 0.0f));
        if (g_game.terrain) { float s = world_surface(&g_game, eye.x, eye.z) + 3.0f; if (eye.y < s) eye.y = s; }
        eye = v3_lerp(c->eye, eye, s_lookK);
        d = v3_norm(v3_lerp(f0, v3_norm(v3_sub(s_lookTp, eye)), s_lookK));
        up = v3_norm(v3_lerp(u0, v3(0.0f, 1.0f, 0.0f), s_lookK));
    }
    up = v3_norm(v3_sub(up, v3_scale(d, v3_dot(up, d))));
    c->eye = eye;
    m4_look_lh(&c->view, eye, v3_madd(eye, d, 1000.0f), up);   /* x1000: no float quantisation of the direction (T15) */
}

/* dev (-benchview side): beside the jet looking back along its path, so what it leaves behind (trails, flares,
   smoke) is in view; HUD hidden. Bench only. */
static void side_camera(Camera *c, float alpha)
{
    Vec3 pos, f, r, eye, at;
    Quat qn;
    const Aircraft *pl = &g_game.ac[PLAYER_ID];
    if (!s_sideCam || !s_bench || !(pl->flags & AC_ALIVE)) return;
    cam_ac_pose(pl, alpha, &pos, &qn);
    f = q_rotate(qn, v3(0.0f, 0.0f, 1.0f));
    r = v3_norm(v3_cross(v3(0.0f, 1.0f, 0.0f), f));
    eye = v3_add(v3_madd(v3_madd(pos, r, 32.0f), f, 14.0f), v3(0.0f, 7.0f, 0.0f));
    at = v3_madd(pos, f, -45.0f);
    c->eye = eye;
    m4_look_lh(&c->view, eye, v3_madd(eye, v3_norm(v3_sub(at, eye)), 1000.0f), v3(0.0f, 1.0f, 0.0f));
}

static void draw_overlays(void)
{
    char buf[48];
    int alive = (g_game.ac[PLAYER_ID].flags & AC_ALIVE) != 0;
    hg_begin();
    if (s_app == APP_FLIGHT && !alive && !s_cam.fixed) {
        hg_text(hg_px(320.0f), hg_px(150.0f), HF_LARGE, hg_col(0xFFFF5040, 1.0f), 1, "CRASH");
        hg_text(hg_px(320.0f), hg_px(176.0f), HF_SMALL, hg_col(0xFFFFFFFF, 0.9f), 1, "RESTART ON THE RUNWAY");
    }
    if (s_feel && s_app == APP_FLIGHT) draw_feel();
    if (s_playing) hg_text(hg_px(634.0f), hg_px(6.0f), HF_SMALL, hg_col(0xFFFFD040, 1.0f), 2, "REPLAY");
    if (g_args.fps) {
        float ms = prof_last_frame_ms();
        e_snprintf(buf, (int)sizeof(buf), "%.1f FPS", ms > 0.0f ? 1000.0 / (double)ms : 0.0);
        hg_text(hg_px(634.0f), hg_px(s_playing ? 20.0f : 6.0f), HF_SMALL, hg_col(0xFFFFFF60, 1.0f), 2, buf);
    }
    hg_end();
    if (s_paused && s_app == APP_FLIGHT) menu_pause_draw();   /* T19 */
}

static void save_shot_f12(void)
{
    char rel[64], path[E_PATH];
    int n;
    base_path(path, (int)sizeof(path), "shots");
    CreateDirectoryA(path, NULL);
    for (n = 1; n < 1000; n++) {
        e_snprintf(rel, (int)sizeof(rel), "shots\\jet_%03d.bmp", n);
        base_path(path, (int)sizeof(path), rel);
        if (!file_exists(path)) break;
    }
    log_printf(LOG_INFO, "shot: f12 path=%s ok=%d", path, r_save_bmp(path));
}

static void draw_frame(float alpha)
{
    int i;
    s64 now = plat_now_us();
    float frameDt = s_lastFrameUs ? (float)(now - s_lastFrameUs) * 1e-6f : 0.0f;
    s_lastFrameUs = now;
    if (s_paused) frameDt = 0.0f;
    prof_begin(s_secDraw);
    if (s_app == APP_RESULTS) frameDt = 0.0f;       /* the flight is frozen under the results */
    if (!r_begin_frame((s_app == APP_FLIGHT || s_app == APP_TITLE || s_app == APP_RESULTS) && s_scene ? scene_clear_flags() : 0x000000)) {
        prof_end(s_secDraw);
        r_device_lost_check();
        plat_sleep(10);
        return;
    }
    if (s_app == APP_FLIGHT) {
        const Aircraft *pl = &g_game.ac[PLAYER_ID];
        const WorldStatus *ws = world_status(PLAYER_ID);
        prof_begin(s_secDlz);
        dlz_work(&g_game, 1500);                    /* per frame (DESIGN 2.6); budget from the preset (T13/T18) */
        prof_end(s_secDlz);
        cam_update(&s_cam, pl, alpha, frameDt);
        look_camera(&s_cam, alpha, frameDt);        /* T17: V */
        side_camera(&s_cam, alpha);                 /* T17 dev: -benchview side */
        if (s_scene) scene_draw(&g_game, &s_cam, alpha);
        fx_draw(&g_game, &s_cam, alpha);
        if (s_scene) scene_draw_dots(&g_game, &s_cam, alpha);
        if ((pl->flags & AC_ALIVE) && !s_cam.fixed && !s_sideCam) {
            prof_begin(s_secHud);
            int lookHide = s_cam.mode == VIEW_COCKPIT && s_lookK > 0.3f;   /* the HUD glass stays with the nose */
            hud_set_extras((ws && ws->pullUp ? HUDW_PULLUP : 0) | (ws && ws->areaWarn ? HUDW_RETURN : 0), mission_clock(&g_game), "",
                           g_game.mode == MODE_FREE ? -1 : weapons_state(PLAYER_ID)->flares);
            if (!lookHide) hud_draw(&g_game, &s_cam, alpha);
            hudtgt_draw(&g_game, &s_cam, alpha);
            if (!lookHide) hudwpn_draw(&g_game, &s_cam, alpha);
            map_draw(&g_game, alpha);
            prof_end(s_secHud);
        }
    } else if (s_app == APP_TITLE) {                /* T19: fly-by backdrop + frontend */
        menu_backdrop_camera(&s_titleCam, ((float)s_titleTicks + alpha) / (float)SIM_HZ);
        cam_update(&s_titleCam, &g_game.ac[PLAYER_ID], alpha, frameDt);
        if (s_scene) scene_draw(&g_game, &s_titleCam, alpha);
        menu_draw(APP_TITLE);
    } else if (s_app == APP_RESULTS) {              /* T19: the frozen flight under the results screen */
        cam_update(&s_cam, &g_game.ac[PLAYER_ID], 1.0f, 0.0f);
        if (s_scene) scene_draw(&g_game, &s_cam, 1.0f);
        fx_draw(&g_game, &s_cam, 1.0f);
        if (s_scene) scene_draw_dots(&g_game, &s_cam, 1.0f);
        menu_draw(APP_RESULTS);
    }
    draw_overlays();
    r_end_frame();
    {
        char sp[E_PATH], rel[E_PATH];
        if (menu_script_take_shot(rel, (int)sizeof(rel))) {   /* -mscript "shot FILE" */
            arg_path(sp, rel);
            log_printf(LOG_INFO, "shot: path=%s tick=%u ok=%d screen=%s", sp, s_tick, r_save_bmp(sp), menu_screen_name(s_app, s_paused));
        }
    }
    for (i = 0; i < g_args.numShots; i++) {        /* -shot T file: first frame at or after sim time T */
        if (s_shotDone[i] || (float)s_tick < g_args.shotTime[i] * (float)SIM_HZ) continue;
        {
            char path[E_PATH];
            int ok;
            arg_path(path, g_args.shotPath[i]);
            ok = r_save_bmp(path);
            log_printf(LOG_INFO, "shot: path=%s tick=%u ok=%d", path, s_tick, ok);
        }
        s_shotDone[i] = 1;
    }
    if (s_shotReq) { save_shot_f12(); s_shotReq = 0; }
    snd_set_paused(s_paused || s_app != APP_FLIGHT);
    sndc_set_paused(s_paused || s_app != APP_FLIGHT);
    snd_frame(&g_game, &s_cam);
    sndc_frame(&g_game, &s_cam);
    au_update();
    prof_end(s_secDraw);
    prof_begin(s_secPresent);
    r_present();
    prof_end(s_secPresent);
    prof_frame();
    menu_script_note_fps(prof_last_frame_ms());
    if (s_bench) {
        bench_frame();
        if (bench_done()) {
            char path[E_PATH];
            base_path(path, (int)sizeof(path), "bench.txt");
            bench_write(path);
            quit("bench");
        }
    }
    s_frames++;
}

/* Windows-side modules' tunables (portable ones: params_register_all) */
static void register_params(void)
{
    params_register_all();
    cam_params();
    hud_params();
    hudtgt_params();                                /* T15: [hud] tgt* keys */
    hudwpn_params();                                /* T16: [hud] wpn* keys */
    snd_params();
    sndc_params();                                  /* T17: [sound] warn / combat */
    scene_params();
    fx_params();
    scatter_params();                               /* A11: [scatter] */
}

static u32 exe_crc(void)
{
    char path[E_PATH];
    int size = 0;
    void *buf;
    u32 c;
    path_join(path, (int)sizeof(path), plat_exe_dir(), "jet.exe");
    buf = file_load(path, &size);
    if (!buf) return 0;
    c = e_crc32(buf, size);
    free(buf);
    return c;
}

/* ---------------------------------------------------------------- init / loop / shutdown */
static int app_main(HINSTANCE inst)
{
    char path[E_PATH];
    int level, r, w, h;
    PlatInit pi;
    RInit ri;
    s64 qpf, last, now, acc, accMax, t0;
    float alpha;

    file_set_base_dir(plat_exe_dir());
    base_path(s_iniPath, (int)sizeof(s_iniPath), arg_str("-ini", 0, "jet.ini"));   /* T19: -ini FILE */
    s_ini = cfg_load(s_iniPath);
    /* log level and file first (params_load already logs) */
    level = arg_has("-log") ? arg_int("-log", 0, 1) + 1 : LOG_ERR;   /* -log 1 = info, -log 2 = debug */
    if ((arg_has("-test") || arg_has("-script") || arg_has("-replay") || arg_has("-bench") || arg_has("-aifight") || arg_has("-mscript")) &&
        level < LOG_INFO)
        level = LOG_INFO;                           /* tests, scripts, replays and benches always write their lines */
    {
        const char *ln = "jet.log";
        if (arg_has("-test") || arg_has("-script") || arg_has("-replay") || arg_has("-headless") ||
            arg_has("-exitafter") || arg_has("-dumpini") || arg_has("-hudtest") || arg_has("-aifight") || arg_has("-mscript"))
            ln = "jet_test.log";
        if (arg_has("-bench")) ln = "jet_bench.log";
        ln = arg_str("-logfile", 0, ln);
        base_path(path, (int)sizeof(path), ln);
    }
    log_open(path, level < LOG_INFO ? LOG_INFO : level);   /* play: info lines too (feel play-test, DESIGN 16.5) */
    log_set_autoflush(1);
    log_printf(LOG_INFO, "boot: jet build=\"%s %s\" dir=%s log=%s", __DATE__, __TIME__, plat_exe_dir(), path);
    {
        char cl[512];
        int i, used = 0;
        cl[0] = 0;
        for (i = 1; i < arg_argc() && used < (int)sizeof(cl) - 2; i++) {
            int n = e_snprintf(cl + used, (int)sizeof(cl) - used, "%s%s", i > 1 ? " " : "", arg_argv(i));
            if (n < 0) break;
            used += n;
        }
        log_printf(LOG_INFO, "boot: cmdline \"%s\"", cl);
    }
    log_printf(LOG_INFO, "boot: cfg path=%s keys=%d", s_iniPath, cfg_num_keys(s_ini));
    register_params();
    params_load(s_ini);
    args_parse(&g_args);
    args_log(&g_args);
    args_log_not_yet(&g_args);
    s_headless = g_args.headless;
    scene_preset_init(g_args.preset);
    if (s_headless || g_args.test[0] || g_args.exitAfter > 0.0f || g_args.bench || g_args.dumpini[0] || g_args.hudtest[0] ||
        g_args.script[0] || g_args.replay[0] || g_args.aifight || arg_has("-mscript"))
        plat_set_no_dialogs(1);                     /* automated runs must never block on a message box */
    log_printf(LOG_INFO, "boot: fpu start cw=0x%05x", plat_fpu_cw());
    plat_fpu_setup();
    log_printf(LOG_INFO, "boot: fpu setup cw=0x%05x ok=%d", plat_fpu_cw(), plat_fpu_ok());
    prof_init(plat_now_us);
    s_secSim = prof_section("sim");
    s_secDraw = prof_section("draw");
    s_secPresent = prof_section("present");
    s_secHud = prof_section("hud");
    s_secDlz = prof_section("dlz");
    s_exeCrc = exe_crc();
    log_printf(LOG_INFO, "boot: qpc freq=%d params=%d hash=%08x exe_crc=%08x", (int)plat_qpf(), params_count(), params_hash(),
               s_exeCrc);

    /* -dumpini FILE: every key with its default and help (CRLF) + the default [controls], then exit */
    if (g_args.dumpini[0]) {
        Cfg *extra = cfg_parse("", 0);
        jet_input_setup();
        in_bind_save(extra);
        base_path(path, (int)sizeof(path), g_args.dumpini);
        r = params_dump_ini(path, extra) ? 0 : 1;
        cfg_free(extra);
        log_printf(LOG_INFO, "boot: dumpini path=%s result=%d", path, r);
        log_close();
        cfg_free(s_ini);
        return r;
    }

    /* -aifight N [-seed S] [-diff D]: T14's headless robot-vs-waves statistics (exit code = failed checks) */
    if (g_args.aifight > 0) {
        char n[16], sd[16], d[16];
        char *av[3];
        av[0] = n; av[1] = sd; av[2] = d;
        e_snprintf(n, 16, "%d", g_args.aifight);
        e_snprintf(sd, 16, "%u", g_args.haveSeed ? g_args.seed : 1u);
        e_snprintf(d, 16, "%d", g_args.diff >= 0 ? g_args.diff : 1);
        r = simh_aifight(3, av);
        log_printf(LOG_INFO, "boot: aifight n=%s seed=%s diff=%s result=%d", n, sd, d, r);
        log_close();
        cfg_free(s_ini);
        return r;
    }

    /* -test NAME: no window (Windows-only tests register here and create what they need) */
    if (g_args.test[0]) {
        r = tests_run(g_args.test, g_args.testArg[0] ? g_args.testArg : NULL);
        log_printf(LOG_INFO, "boot: test name=%s result=%d", g_args.test, r);
        log_close();
        cfg_free(s_ini);
        return r;
    }

    w = g_args.width > 0 ? g_args.width : 800;
    h = g_args.height > 0 ? g_args.height : 600;
    memset(&ri, 0, sizeof(ri));
    ri.width = w;
    ri.height = h;
    ri.bpp = g_args.bpp == 32 ? 32 : 16;
    ri.fullscreen = g_args.fullscreen;
    ri.fpsTarget = g_args.fpsTarget > 0 ? g_args.fpsTarget : scene_preset()->fpsTarget;
    ri.wantWBuffer = g_args.wbuffer;
    ri.filter = 0;
    ri.maxTexSize = 256;
    ri.texBudgetKB = scene_preset()->texBudgetKB;   /* DESIGN 9.3 */
    ri.noCap = g_args.nocap;
    if (s_headless) {
        if (!r_init_null(&ri)) { log_close(); cfg_free(s_ini); return 1; }
    } else {
        memset(&pi, 0, sizeof(pi));
        pi.title = "Ashfall";                      /* T19: the game's name (window caption) */
        pi.wndClass = "JetWnd";
        pi.width = w;
        pi.height = h;
        pi.fullscreen = g_args.fullscreen;
        if (!plat_init(inst, &pi)) {
            plat_error_box("Could not create the window. See jet.log.");
            log_close(); cfg_free(s_ini);
            return 1;
        }
        ShowWindow((HWND)plat_hwnd(), SW_SHOWNORMAL);   /* CLAUDE.md 5: a hidden window makes blits free */
        ShowWindow((HWND)plat_hwnd(), SW_SHOWNORMAL);
        SetForegroundWindow((HWND)plat_hwnd());
        if (!r_init(plat_hwnd(), &ri)) {
            plat_error_box("Direct3D 6 initialisation failed (no 3D accelerator?). See jet.log.");
            plat_shutdown();
            log_close(); cfg_free(s_ini);
            return 1;
        }
        plat_fpu_setup();                           /* D3D may have changed it (DDSCL_FPUSETUP) */
        log_printf(LOG_INFO, "boot: fpu after_rend cw=0x%05x ok=%d", plat_fpu_cw(), plat_fpu_ok());
        if (r_caps()->refreshHz > 0.0f) prof_set_period_us((s64)(1000000.0f / r_caps()->refreshHz));
        r = g_args.nosound ? 0 : au_init(plat_hwnd(), 22050);
        log_printf(LOG_INFO, "boot: audio ok=%d", r);
        r = in_init(plat_hwnd(), !g_args.nojoy);
        jet_input_setup();
        menu_input_defaults();                      /* T19: defaults for DEL in the controls list */
        in_bind_load(s_ini);                        /* [controls] */
        menu_load_controls(s_iniPath);              /* T19: again from the file (Cfg drops the tail of a long ini) */
        log_printf(LOG_INFO, "boot: input ok=%d joysticks=%d", r, in_joy_count());
        hfont_init();
        menu_set_ini(s_ini, s_iniPath);
        menu_init();                                /* T19: after hfont_init (A10 kit, logo, ui sounds, modes) */
        if (arg_has("-mscript")) {
            arg_path(path, arg_str("-mscript", 0, ""));
            menu_script_load(path);
        }
        hud_init();
        hudwpn_init();                              /* T16: ring texture now (79-88 ms on the VM), not at the first TD */
        map_init();
        fx_init();
        snd_init();
        sndc_init();
    }
    log_printf(LOG_INFO, "boot: rend backend=%s", r_backend_name());
    if (g_args.hudtest[0] && !s_headless) {         /* -hudtest NAME (T07, DESIGN 10.8): own frames, then exit */
        s_exitCode = hud_test(g_args.hudtest);
        log_printf(LOG_INFO, "boot: hudtest name=%s result=%d", g_args.hudtest, s_exitCode);
        quit("hudtest");
    }

    s_exitTicks = g_args.exitAfter > 0.0f ? (u32)e_ftoi(g_args.exitAfter * (float)SIM_HZ) : 0;
    if (s_exitTicks == 0 && g_args.exitAfter > 0.0f) s_exitTicks = 1;
    /* T19: plain jet.exe (or -mscript) starts at the title; any switch that implies a flight starts it directly */
    s_wantTitle = !s_headless && (arg_has("-mscript") ||
                  !(arg_has("-mode") || arg_has("-start") || arg_has("-diff") || g_args.script[0] || g_args.replay[0] ||
                    g_args.bench || g_args.haveCamat || g_args.record[0] || g_args.telemetry[0] || g_args.exitAfter > 0.0f ||
                    g_args.numShots > 0));
    if (!s_quit) {
        if (s_wantTitle) {
            menu_draw_loading("LOADING ISLAND");
            go_title();                             /* ~7-8 s over the share (jet.isl) */
        } else {
            set_app(APP_LOADING);
            if (!s_headless) menu_draw_loading("LOADING ISLAND");
            start_flight();                         /* ~7-8 s over the share (jet.isl) */
        }
    }
    log_printf(LOG_INFO, "boot: loop start app=%s headless=%d exitTicks=%u", app_name(s_app), s_headless, s_exitTicks);
    log_set_autoflush(0);                           /* flushed once per sim second and at exit */

    qpf = plat_qpf();
    accMax = qpf * SIM_HZ / 4;                      /* 0.25 s */
    acc = 0;
    t0 = last = plat_qpc();
    while (!s_quit) {
        int n = 0;
        if (s_headless) {
            prof_begin(s_secSim);
            sim_tick();
            prof_end(s_secSim);
            continue;
        }
        if (!plat_pump()) { quit("window"); break; }
        now = plat_qpc();
        acc += (now - last) * SIM_HZ;
        last = now;
        if (acc > accMax) acc = accMax;             /* after hitches: slow down, don't spiral */
        prof_begin(s_secSim);
        while (acc >= qpf && n < 8 && !s_quit) {
            sim_tick();
            acc -= qpf;
            n++;
        }
        prof_end(s_secSim);
        if (s_quit) break;
        alpha = (float)acc / (float)qpf;
        if (alpha > 1.0f) alpha = 1.0f;
        if (s_paused) alpha = 1.0f;
        draw_frame(alpha);
    }
    now = plat_qpc();
    log_set_autoflush(1);
    if (s_recording) replay_record_close(&g_game);
    if (s_playing) {
        ReplayStats st;
        replay_play_close();
        replay_stats(&st);
        if (st.diffs) s_exitCode = 1;
    }
    if (s_scripted) {
        PilotKeys k;
        int c = 0;
        script_step(&g_game, &k, &c);               /* lines due at the final state (e.g. "40.0 expect" with -exitafter 40) */
        log_printf(LOG_INFO, "script: result checks=%d fails=%d", script_checks(), script_fails());
        if (script_fails() > s_exitCode) s_exitCode = script_fails();
        script_close();
    }
    if (menu_script_active()) {
        int mf = menu_script_finish();
        if (mf > s_exitCode) s_exitCode = mf;
    }
    flight_telemetry_close();
    if (mission_director() && g_game.mode != MODE_FREE && mission_stats()->outcome == MIS_RUNNING && s_app == APP_FLIGHT)
        mission_log_result(&g_game, s_quitReason);  /* T17: a results line for every combat run */
    log_printf(LOG_INFO, "boot: exit reason=%s ticks=%u frames=%d simtime=%.3f realtime=%.3f", s_quitReason, s_tick,
               s_frames, (double)s_tick / (double)SIM_HZ, (double)(now - t0) / (double)qpf);

    if (!s_headless) {
        if (s_scene) scene_free();
        sndc_free();
        snd_free();
        fx_free();
        map_free();
        hud_free();
        hfont_free();
        menu_free();
        tex_release_all();
        in_shutdown();
        au_shutdown();
    }
    terrain_free(&s_terrain);
    r_shutdown();
    if (!s_headless) plat_shutdown();
    log_printf(LOG_INFO, "boot: shutdown ok code=%d", s_exitCode);
    log_close();
    cfg_free(s_ini);
    return s_exitCode;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdLine, int show)
{
    static char buf[1024];
    char *argv[64];
    int argc;
    (void)prev; (void)show;
    argv[0] = (char *)"jet.exe";
    e_strlcpy(buf, cmdLine ? cmdLine : "", (int)sizeof(buf));
    argc = 1 + args_split(buf, argv + 1, E_COUNT(argv) - 1);
    args_init(argc, argv);
    return app_main(inst);
}
