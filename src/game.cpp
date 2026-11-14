/* game.cpp - g_game, g_args, [video] params, the command-line parser (DESIGN 2.7). Portable.
   Owner: T01; append-only for later tasks (T08 owns it with main.cpp after T01).
   T08 appended the portable session (session_*: DESIGN 2.6 tick order, crash/respawn flow) and the -script runner
   (script_*), see game.h.
   Order in main: params_register_all -> params_load(ini) -> args_parse (video defaults come from the [video]
   params, switches win). Switches that are parsed but not implemented yet are listed by args_log_not_yet
   ("args: not yet -x (Txx)"); the task that implements one removes it from s_notYet. */
#include "game.h"
#include "params.h"
#include "e_log.h"
#include "e_file.h"
#include "e_terrain.h"
#include "takeoff.h"
#include "world.h"
#include "radar.h"
#include "target.h"
#include "weapons.h"
#include "dlz.h"
#include "ai.h"
#include "mission.h"

Game g_game;
Args g_args;

float speed_of_sound(float h) { return h <= 11000.0f ? 340.3f - 0.00405f * h : 295.7f; }

const char *const g_argSwitches[] = {
    "-window", "-fullscreen", "-res", "-bpp", "-preset", "-headless", "-mode", "-diff", "-seed", "-start",
    "-log", "-logfile", "-exitafter", "-shot", "-camat", "-script", "-record", "-replay", "-telemetry", "-test",
    "-bench", "-benchroute", "-aifight", "-hudtest", "-dumpini", "-dev", "-nocap", "-fps", "-nosound", "-nojoy",
    "-benchview", "-mscript", "-ini"
};
const int g_numArgSwitches = E_COUNT(g_argSwitches);

/* switch -> task that implements it (T01 implements the rest) */
static const char *const s_notYet[][2] = {
    { "-none-", "-" },                              /* T17: -aifight (T14) and -diff, -mode training|sortie implemented */
};

/* ---------------------------------------------------------------- [video] (read at start only) */
static int  s_vFullscreen, s_vWidth, s_vHeight, s_vBpp, s_vFps, s_vWbuf, s_vShowFps;
static char s_vPreset[PARAM_STR];
static const Param s_videoTab[] = {
    { "video", "fullscreen", P_BOOL, &s_vFullscreen, "1", 0, 1, "1 = fullscreen, 0 = window (-window / -fullscreen)", P_RESTART },
    { "video", "width", P_INT, &s_vWidth, "800", 320, 2048, "screen width in pixels (-res W H); 800x600 = Low on the VM (minimum)", P_RESTART },
    { "video", "height", P_INT, &s_vHeight, "600", 240, 1536, "screen height in pixels (the T40 target: 1400x1050)", P_RESTART },
    { "video", "bpp", P_INT, &s_vBpp, "16", 16, 32, "colour depth 16 or 32", P_RESTART },
    { "video", "preset", P_STR, s_vPreset, "low", 0, 0, "low (VM, 30 fps) or high (real PC, 60 fps) (-preset)", P_RESTART },
    { "video", "fpsTarget", P_INT, &s_vFps, "0", 0, 120, "frame rate cap; 0 = from the preset (low 30, high 60)", P_RESTART },
    { "video", "wbuffer", P_BOOL, &s_vWbuf, "1", 0, 1, "use the W-buffer when the card has one (DESIGN 9.1)", P_RESTART },
    { "video", "showFps", P_BOOL, &s_vShowFps, "0", 0, 1, "fps overlay at start (F11 toggles; -fps)", P_RESTART },
};
void game_params(void) { params_add(s_videoTab, E_COUNT(s_videoTab)); }

/* ---------------------------------------------------------------- parser */
static void copy_arg(char *dst, int n, const char *sw, int idx)
{
    const char *s = arg_str(sw, idx, NULL);
    if (s) e_strlcpy(dst, s, n);
}

static int parse_mode(const char *s, int def)
{
    if (!s) return def;
    if (!e_stricmp(s, "free")) return MODE_FREE;
    if (!e_stricmp(s, "training")) return MODE_TRAINING;
    if (!e_stricmp(s, "sortie")) return MODE_SORTIE;
    log_printf(LOG_WARN, "warn: args bad -mode %s (free|training|sortie)", s);
    return def;
}

static int parse_route(const char *s)
{
    if (!s) return BENCH_ROUTE_LOW;
    if (!e_stricmp(s, "volcano")) return BENCH_ROUTE_VOLCANO;
    if (!e_stricmp(s, "high")) return BENCH_ROUTE_HIGH;
    if (!e_stricmp(s, "clouds")) return BENCH_ROUTE_CLOUDS;
    if (!e_stricmp(s, "combat")) return BENCH_ROUTE_COMBAT;
    return BENCH_ROUTE_LOW;
}

void args_parse(Args *a)
{
    int i, n;
    const char *s;
    memset(a, 0, sizeof(*a));

    /* ini ([video], [dev]) */
    a->fullscreen = s_vFullscreen;
    a->width = s_vWidth;
    a->height = s_vHeight;
    a->bpp = s_vBpp;
    e_strlcpy(a->preset, s_vPreset[0] ? s_vPreset : "low", (int)sizeof(a->preset));
    a->fpsTarget = s_vFps;
    a->wbuffer = s_vWbuf;
    a->fps = s_vShowFps;
    a->dev = params_dev_keys();
    a->mode = MODE_FREE;
    a->diff = DIFF_NORMAL;
    a->start = START_RUNWAY;

    /* switches */
    if (arg_has("-window")) a->fullscreen = 0;
    if (arg_has("-fullscreen")) a->fullscreen = 1;
    if (arg_has("-res")) { a->width = arg_int("-res", 0, a->width); a->height = arg_int("-res", 1, a->height); }
    a->bpp = arg_int("-bpp", 0, a->bpp);
    copy_arg(a->preset, (int)sizeof(a->preset), "-preset", 0);
    if (e_stricmp(a->preset, "low") && e_stricmp(a->preset, "high")) {
        log_printf(LOG_WARN, "warn: args bad preset %s (low|high), using low", a->preset);
        e_strlcpy(a->preset, "low", (int)sizeof(a->preset));
    }
    if (a->fpsTarget <= 0) a->fpsTarget = !e_stricmp(a->preset, "high") ? 60 : 30;
    a->headless = arg_has("-headless");
    a->mode = parse_mode(arg_str("-mode", 0, NULL), a->mode);
    a->diff = E_CLAMP(arg_int("-diff", 0, a->diff), DIFF_EASY, DIFF_HARD);
    s = arg_str("-seed", 0, NULL);
    if (s) {
        char *end;
        unsigned long v = strtoul(s, &end, 0);
        if (end != s) { a->seed = (u32)v; a->haveSeed = 1; }
    }
    s = arg_str("-start", 0, NULL);
    if (s && !e_stricmp(s, "air")) a->start = START_AIR;
    else if (s && e_stricmp(s, "runway")) log_printf(LOG_WARN, "warn: args bad -start %s (runway|air)", s);
    if (arg_has("-log")) a->logLevel = arg_int("-log", 0, 1);
    if (a->logLevel < 0) a->logLevel = 0;
    copy_arg(a->logFile, E_PATH, "-logfile", 0);
    a->exitAfter = arg_float("-exitafter", 0, 0.0f);
    if (arg_has("-test")) {
        copy_arg(a->test, (int)sizeof(a->test), "-test", 0);
        s = arg_str("-test", 1, NULL);
        if (s && s[0] != '-') e_strlcpy(a->testArg, s, (int)sizeof(a->testArg));
    }
    copy_arg(a->hudtest, (int)sizeof(a->hudtest), "-hudtest", 0);
    copy_arg(a->dumpini, E_PATH, "-dumpini", 0);
    if (arg_has("-dumpini") && !a->dumpini[0]) e_strlcpy(a->dumpini, "jet.ini", E_PATH);
    if (arg_has("-dev")) a->dev = 1;
    a->nocap = arg_has("-nocap");
    if (arg_has("-fps")) a->fps = 1;
    a->nosound = arg_has("-nosound") || a->headless;
    a->nojoy = arg_has("-nojoy") || a->headless;

    n = arg_count("-shot");
    for (i = 0; i < n && a->numShots < ARGS_MAX_SHOTS; i++) {
        const char *t = arg_nth("-shot", i, 0), *p = arg_nth("-shot", i, 1);
        float tv;
        if (!t || !p || !e_parse_float(t, &tv)) continue;
        a->shotTime[a->numShots] = tv;
        e_strlcpy(a->shotPath[a->numShots], p, E_PATH);
        a->numShots++;
    }
    if (arg_has("-camat")) {
        a->haveCamat = 1;
        for (i = 0; i < 5; i++) a->camat[i] = arg_float("-camat", i, 0.0f);
    }
    copy_arg(a->script, E_PATH, "-script", 0);
    copy_arg(a->record, E_PATH, "-record", 0);
    copy_arg(a->replay, E_PATH, "-replay", 0);
    copy_arg(a->telemetry, E_PATH, "-telemetry", 0);
    a->bench = arg_has("-bench") ? arg_int("-bench", 0, 30) : 0;
    a->benchRoute = parse_route(arg_str("-benchroute", 0, NULL));
    a->aifight = arg_has("-aifight") ? arg_int("-aifight", 0, 10) : 0;
    s = arg_str("-benchview", 0, NULL);
    a->benchView = (s && !e_stricmp(s, "cockpit")) ? VIEW_COCKPIT : VIEW_CHASE;
}

void args_log(const Args *a)
{
    int i;
    char unk[256];
    log_printf(LOG_INFO, "boot: args fullscreen=%d res=%dx%d bpp=%d preset=%s fpsTarget=%d wbuffer=%d nocap=%d fps=%d",
               a->fullscreen, a->width, a->height, a->bpp, a->preset, a->fpsTarget, a->wbuffer, a->nocap, a->fps);
    log_printf(LOG_INFO, "boot: args headless=%d exitafter=%.2f log=%d test=%s hudtest=%s dumpini=%s dev=%d nosound=%d nojoy=%d",
               a->headless, (double)a->exitAfter, a->logLevel, a->test[0] ? a->test : "-",
               a->hudtest[0] ? a->hudtest : "-", a->dumpini[0] ? a->dumpini : "-", a->dev, a->nosound, a->nojoy);
    log_printf(LOG_INFO, "boot: args mode=%d diff=%d start=%d seed=%u haveseed=%d bench=%d route=%d aifight=%d",
               a->mode, a->diff, a->start, a->seed, a->haveSeed, a->bench, a->benchRoute, a->aifight);
    log_printf(LOG_INFO, "boot: args shots=%d camat=%d script=%s record=%s replay=%s telemetry=%s",
               a->numShots, a->haveCamat, a->script[0] ? a->script : "-", a->record[0] ? a->record : "-",
               a->replay[0] ? a->replay : "-", a->telemetry[0] ? a->telemetry : "-");
    for (i = 0; i < a->numShots; i++)
        log_printf(LOG_INFO, "boot: args shot=%d t=%.2f path=%s", i, (double)a->shotTime[i], a->shotPath[i]);
    if (arg_unknown(g_argSwitches, g_numArgSwitches, unk, (int)sizeof(unk)) > 0)
        log_printf(LOG_WARN, "warn: unknown switch %s", unk);
}

void args_log_not_yet(const Args *a)
{
    int i;
    for (i = 0; i < E_COUNT(s_notYet); i++)
        if (arg_has(s_notYet[i][0])) log_printf(LOG_INFO, "args: not yet %s (%s)", s_notYet[i][0], s_notYet[i][1]);
    (void)a;
}

/* ================================================================ session (T08, DESIGN 2.6) */
static PilotState   s_sesPilot[MAX_AIRCRAFT];
static TakeoffState s_sesTakeoff;
static PilotInput   s_sesOut;
static int          s_sesBits, s_sesRespawns;
static SessionApFn  s_sesAp;

PilotState *session_pilot(int id) { return &s_sesPilot[id >= 0 && id < MAX_AIRCRAFT ? id : 0]; }
TakeoffState *session_takeoff(void) { return &s_sesTakeoff; }
const PilotInput *session_pilot_out(void) { return &s_sesOut; }
int session_world_bits(void) { return s_sesBits; }
int session_respawns(void) { return s_sesRespawns; }
void session_set_autopilot(SessionApFn fn) { s_sesAp = fn; }

static void ses_runway(Game *g, const char *why)
{
    Aircraft *pl = &g->ac[PLAYER_ID];
    pilot_reset(&s_sesPilot[PLAYER_ID]);
    takeoff_park(&s_sesTakeoff, pl, g);
    s_sesRespawns++;
    log_printf(LOG_INFO, "session: restart runway why=%s tick=%u respawns=%d", why, g->tick, s_sesRespawns);
}

static void ses_air(Game *g, const char *why)
{
    Aircraft *pl = &g->ac[PLAYER_ID];
    pilot_reset(&s_sesPilot[PLAYER_ID]);
    world_reset_aircraft(PLAYER_ID);
    world_air_start(g, pl, pl->fs.pos);
    pl->fs.onGround = 0;
    takeoff_reset(&s_sesTakeoff, pl, g);            /* not on the ground: phase DONE */
    s_sesRespawns++;
    log_printf(LOG_INFO, "session: air start why=%s tick=%u pos=%.0f,%.0f,%.0f hdg=%.1f", why, g->tick,
               (double)pl->fs.pos.x, (double)pl->fs.pos.y, (double)pl->fs.pos.z, (double)E_RAD2DEG(pl->fs.heading));
}

void session_start(Game *g, struct Terrain *t, int mode, int diff, int start, u32 seed)
{
    Aircraft *pl;
    int i;
    memset(g, 0, sizeof(*g));
    g->mode = mode;
    g->diff = diff;
    g->start = start;
    g->seed = seed;
    rnd_seed(&g->rng, seed);
    g->terrain = t;
    g->tick = 1;
    g->numAircraft = 1;
    pl = &g->ac[PLAYER_ID];
    pl->id = PLAYER_ID;
    pl->side = SIDE_BLUE;
    pl->flags = AC_ALIVE | AC_PLAYER;
    pl->hp = 100.0f;
    world_reset(g);
    for (i = 0; i < MAX_AIRCRAFT; i++) pilot_reset(&s_sesPilot[i]);
    memset(&s_sesTakeoff, 0, sizeof(s_sesTakeoff));
    memset(&s_sesOut, 0, sizeof(s_sesOut));
    s_sesBits = 0;
    s_sesRespawns = 0;
    s_sesAp = NULL;
    takeoff_bind_pilot(&s_sesTakeoff, &s_sesPilot[PLAYER_ID]);
    if (start == START_AIR) {
        const TerrainBase *b = terrain_base(t);
        world_air_start(g, pl, b ? b->threshold : v3(0.0f, 0.0f, 0.0f));
        pl->fs.onGround = 0;
    } else {
        pl->fs.onGround = 1;                        /* takeoff_reset parks it on the threshold (READY) */
    }
    takeoff_reset(&s_sesTakeoff, pl, g);
    pl->prev = pl->fs;
    radar_reset(g);
    target_reset(g);
    weapons_reset(g);
    dlz_reset(g);
    ai_reset(g);
    mission_start(g);
    log_printf(LOG_INFO, "session: start mode=%d diff=%d start=%s seed=%u pos=%.1f,%.1f,%.1f hdg=%.1f phase=%s", mode, diff,
               start == START_AIR ? "air" : "runway", seed, (double)pl->fs.pos.x, (double)pl->fs.pos.y,
               (double)pl->fs.pos.z, (double)E_RAD2DEG(pl->fs.heading), takeoff_phase_name(s_sesTakeoff.phase));
}

void session_place_air(Game *g, Vec3 pos, float headingRad, float speedMs)
{
    Aircraft *pl = &g->ac[PLAYER_ID];
    pilot_reset(&s_sesPilot[PLAYER_ID]);
    world_reset_aircraft(PLAYER_ID);
    flight_init(&pl->fs, pos, headingRad, speedMs);
    pl->fs.onGround = 0;
    pl->fs.gearPos = 0.0f;
    pl->flags = (pl->flags | AC_ALIVE) & ~AC_ONGROUND;
    pl->hp = 100.0f;
    takeoff_reset(&s_sesTakeoff, pl, g);
    pl->prev = pl->fs;
    log_printf(LOG_INFO, "session: place pos=%.0f,%.0f,%.0f hdg=%.1f V=%.0fkt", (double)pos.x, (double)pos.y, (double)pos.z,
               (double)E_RAD2DEG(headingRad), (double)(speedMs * MS2KT));
}

void session_tick(Game *g, const PilotKeys *keys, int cmd)
{
    PilotInput in, own;
    Aircraft *pl = &g->ac[PLAYER_ID];
    int i;
    if (cmd == SES_CMD_RUNWAY) ses_runway(g, "cmd");
    else if (cmd == SES_CMD_AIR) ses_air(g, "cmd");
    /* 1 input -> shaped player input (mode-aware: Expert / Novice) */
    pilot_update_fs(&s_sesPilot[PLAYER_ID], keys, &pl->fs, &in);
    s_sesOut = in;
    /* 2 targeting */
    target_update(g, &in);
    /* 3 enemies */
    for (i = 1; i < g->numAircraft; i++) {
        if (!(g->ac[i].flags & AC_ALIVE)) continue;
        if (((g->tick + (u32)i * 5u) % 12u) == 0) ai_think(g, i);   /* T17 (T14 finding): i*12 % 12 = 0 made every
                                                       enemy think on the same tick; phases 5,10,3,8,1 for ids 1-5 */
        ai_steer(g, i, &g->ac[i].in);
    }
    /* 4 takeoff automation owns the input while active; area turn-back; bench autopilot */
    if (pl->flags & AC_ALIVE) {
        if (takeoff_tick(&s_sesTakeoff, pl, &in, &own, g)) in = own;
        world_override_input(g, PLAYER_ID, &in);
        if (s_sesAp) s_sesAp(g, PLAYER_ID, &in);
    }
    pl->in = in;
    /* 5 flight model for every aircraft */
    for (i = 0; i < g->numAircraft; i++) {
        Aircraft *a = &g->ac[i];
        a->prev = a->fs;
        if (!(a->flags & AC_ALIVE)) continue;
        flight_step(&a->fs, &a->in, SIM_DT);
    }
    /* 6-9 */
    s_sesBits = world_check(g);
    weapons_tick(g);
    radar_tick(g);
    mission_tick(g);
    /* crash flow (DESIGN 6.3): wreck camera for wreckSec, then the runway again */
    if (!(pl->flags & AC_ALIVE) && world_respawn_due(g, PLAYER_ID)) ses_runway(g, "crash");
    g->tick++;
}

/* ================================================================ -script runner (T08, DESIGN 16.2) */
enum { SCR_MAX = 1024 };
enum { SO_KEYDOWN, SO_KEYUP, SO_EXPECT, SO_PARAM, SO_RESTART, SO_AIR, SO_END };
typedef struct ScrLine { u32 tick; int op, key, lineNo; char a[24], cmp[4], b[64]; } ScrLine;
static ScrLine s_scr[SCR_MAX];
static int s_scrN, s_scrPos, s_scrOn, s_scrFails, s_scrChecks;
static int s_scrKey[12];                            /* script key state: up down left right A Z S SPACE D F C V */
static PressCounter s_scrS;
static const char *const s_scrKeyNames[12] = { "UP", "DOWN", "LEFT", "RIGHT", "A", "Z", "S", "SPACE", "D", "F", "C", "V" };

static int scr_key_index(const char *n)
{
    int i;
    for (i = 0; i < 12; i++) if (!e_stricmp(n, s_scrKeyNames[i])) return i;
    return -1;
}

int script_open(const char *path)
{
    char *buf, *p, line[200];
    int size, i, lineNo = 0, bad = 0;
    s_scrN = s_scrPos = s_scrFails = s_scrChecks = 0;
    s_scrOn = 0;
    memset(s_scrKey, 0, sizeof(s_scrKey));
    memset(&s_scrS, 0, sizeof(s_scrS));
    buf = (char *)file_load(path, &size);
    if (!buf) { log_printf(LOG_ERR, "err: script cannot read path=%s", path); return 0; }
    p = buf;
    while (*p && s_scrN < SCR_MAX) {
        ScrLine *L = &s_scr[s_scrN];
        char cmd[16];
        float t;
        int n;
        for (i = 0; *p && *p != '\n' && i < (int)sizeof(line) - 1; p++) if (*p != '\r') line[i++] = *p;
        line[i] = 0;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        lineNo++;
        for (i = 0; line[i] == ' ' || line[i] == '\t'; i++) {}
        if (!line[i] || line[i] == '#') continue;
        memset(L, 0, sizeof(*L));
        n = sscanf(line + i, "%f %15s %23s %3s %63s", &t, cmd, L->a, L->cmp, L->b);
        if (n < 2 || t < 0.0f) { bad++; log_printf(LOG_WARN, "warn: script line %d not understood: %s", lineNo, line + i); continue; }
        L->tick = (u32)(t * (float)SIM_HZ + 0.5f) + 1u;
        L->lineNo = lineNo;
        if ((!e_stricmp(cmd, "keydown") || !e_stricmp(cmd, "keyup")) && n >= 3 && (L->key = scr_key_index(L->a)) >= 0)
            L->op = !e_stricmp(cmd, "keydown") ? SO_KEYDOWN : SO_KEYUP;
        else if (!e_stricmp(cmd, "expect") && n >= 5) L->op = SO_EXPECT;
        else if (!e_stricmp(cmd, "param") && n >= 3 && strchr(line + i, '=')) {
            L->op = SO_PARAM;
            e_strlcpy(L->b, line + i, (int)sizeof(L->b));   /* keep the full "sec.key=value" (may hold commas) */
            {
                char *q = strstr(L->b, L->a);
                if (q) memmove(L->b, q, strlen(q) + 1);
            }
        }
        else if (!e_stricmp(cmd, "restart")) L->op = SO_RESTART;
        else if (!e_stricmp(cmd, "air")) L->op = SO_AIR;
        else if (!e_stricmp(cmd, "end")) L->op = SO_END;
        else { bad++; log_printf(LOG_WARN, "warn: script line %d not understood: %s", lineNo, line + i); continue; }
        s_scrN++;
    }
    free(buf);
    s_scrOn = s_scrN > 0;
    log_printf(LOG_INFO, "script: open path=%s lines=%d bad=%d", path, s_scrN, bad);
    s_scrFails += bad;
    return s_scrOn;
}

static float scr_field(const Game *g, const char *f, int *ok)
{
    const FlightState *fs = &g->ac[PLAYER_ID].fs;
    *ok = 1;
    if (!e_stricmp(f, "kt")) return fs->V * MS2KT;
    if (!e_stricmp(f, "agl_ft")) return world_agl(g, fs->pos) * M2FT;
    if (!e_stricmp(f, "alt_ft")) return fs->pos.y * M2FT;
    if (!e_stricmp(f, "gear")) return fs->gearPos;
    if (!e_stricmp(f, "lat_m")) return (float)fabs(takeoff_centre_line_error(g, fs->pos));
    if (!e_stricmp(f, "onground")) return (float)fs->onGround;
    if (!e_stricmp(f, "alive")) return (g->ac[PLAYER_ID].flags & AC_ALIVE) ? 1.0f : 0.0f;
    if (!e_stricmp(f, "pullup")) return (float)world_status(PLAYER_ID)->pullUp;
    if (!e_stricmp(f, "gamma")) return E_RAD2DEG(fs->gamma);
    if (!e_stricmp(f, "pitch")) return E_RAD2DEG(fs->pitch);
    if (!e_stricmp(f, "bank")) return E_RAD2DEG(fs->bank);
    if (!e_stricmp(f, "respawns")) return (float)s_sesRespawns;
    *ok = 0;
    return 0.0f;
}

static int scr_compare(float a, const char *op, float b)
{
    if (!strcmp(op, "==")) return (float)fabs(a - b) < 1e-4f;
    if (!strcmp(op, "!=")) return (float)fabs(a - b) >= 1e-4f;
    if (!strcmp(op, "<")) return a < b;
    if (!strcmp(op, "<=")) return a <= b;
    if (!strcmp(op, ">")) return a > b;
    if (!strcmp(op, ">=")) return a >= b;
    return -1;
}

static void scr_expect(const Game *g, const ScrLine *L)
{
    float v, want;
    int ok, r = -1, ph;
    if (!e_stricmp(L->a, "phase")) {
        for (ph = TO_READY; ph <= TO_DONE; ph++) if (!e_stricmp(L->b, takeoff_phase_name(ph))) break;
        if (ph <= TO_DONE) r = scr_compare((float)s_sesTakeoff.phase, L->cmp, (float)ph);
        if (r >= 0) {
            s_scrChecks++;
            if (!r) s_scrFails++;
            log_printf(LOG_INFO, "script: l%d phase=%s %s %s %s", L->lineNo, takeoff_phase_name(s_sesTakeoff.phase), L->cmp,
                       L->b, r ? "ok" : "FAIL");
            return;
        }
    } else if (!e_stricmp(L->a, "mode")) {
        want = !e_stricmp(L->b, "NOVICE") ? (float)PILOT_NOVICE : (float)PILOT_EXPERT;
        r = scr_compare((float)pilot_mode(), L->cmp, want);
        if (r >= 0) {
            s_scrChecks++;
            if (!r) s_scrFails++;
            log_printf(LOG_INFO, "script: l%d mode=%s %s %s %s", L->lineNo, pilot_mode_name(pilot_mode()), L->cmp, L->b,
                       r ? "ok" : "FAIL");
            return;
        }
    } else {
        v = scr_field(g, L->a, &ok);
        if (ok) r = scr_compare(v, L->cmp, (float)atof(L->b));
        if (r >= 0) {
            s_scrChecks++;
            if (!r) s_scrFails++;
            log_printf(LOG_INFO, "script: l%d %s=%.2f %s %s %s", L->lineNo, L->a, (double)v, L->cmp, L->b, r ? "ok" : "FAIL");
            return;
        }
    }
    s_scrFails++;
    log_printf(LOG_INFO, "script: l%d bad expect %s %s %s FAIL", L->lineNo, L->a, L->cmp, L->b);
}

int script_step(const Game *g, PilotKeys *keys, int *cmd)
{
    int param = 0, held, rel;
    memset(keys, 0, sizeof(*keys));
    if (!s_scrOn) return 0;
    while (s_scrPos < s_scrN && s_scr[s_scrPos].tick <= g->tick) {
        const ScrLine *L = &s_scr[s_scrPos++];
        switch (L->op) {
        case SO_KEYDOWN: s_scrKey[L->key] = 1; break;
        case SO_KEYUP: s_scrKey[L->key] = 0; break;
        case SO_EXPECT: scr_expect(g, L); break;
        case SO_PARAM:
            if (params_restore(L->b, (int)strlen(L->b)) > 0) param = 1;
            log_printf(LOG_INFO, "script: l%d param %s tick=%u", L->lineNo, L->b, g->tick);
            break;
        case SO_RESTART: if (cmd) *cmd = SES_CMD_RUNWAY; break;
        case SO_AIR: if (cmd) *cmd = SES_CMD_AIR; break;
        default: break;
        }
    }
    keys->up = s_scrKey[0]; keys->down = s_scrKey[1]; keys->left = s_scrKey[2]; keys->right = s_scrKey[3];
    keys->fast = s_scrKey[4]; keys->slow = s_scrKey[5];
    press_counter_feed(&s_scrS, s_scrKey[6], &held, &rel);
    keys->tgtHeld = held; keys->tgtReleased = rel;
    keys->fire = s_scrKey[7]; keys->wpnNext = s_scrKey[8]; keys->flare = s_scrKey[9]; keys->view = s_scrKey[10];
    keys->look = s_scrKey[11];
    return param;
}

int script_fails(void) { return s_scrFails; }
int script_checks(void) { return s_scrChecks; }
int script_done(const Game *g) { (void)g; return !s_scrOn || s_scrPos >= s_scrN; }
void script_close(void)
{
    if (s_scrOn || s_scrN) log_printf(LOG_INFO, "script: done lines=%d applied=%d checks=%d fails=%d", s_scrN, s_scrPos,
                                      s_scrChecks, s_scrFails);
    s_scrOn = 0;
    s_scrN = s_scrPos = 0;
}

/* ================================================================ T17: mission-driven runway restart */
/* mission_tick (tick step 9) restarts the player on the runway after a missile death (DESIGN 14.2) through the same
   path as the pause menu / crash flow, so session_respawns() counts it (camera/HUD snap, weapons refill, robot). */
void session_runway(Game *g, const char *why) { ses_runway(g, why); }
