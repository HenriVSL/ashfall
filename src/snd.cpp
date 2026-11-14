/* snd.cpp - flight sound logic over e_audio (DESIGN 15, A5 mix law in data/sfx/README.md). Windows side. Owner: T08.
   Loops (started once at full silence and kept looping, -74 dB floor, so nothing restarts audibly): eng_idle, eng_mil,
   eng_ab (engine: lagged spool N, equal-power idle/mil crossfade in pitch-ratio space, AB layer), wind (V, G),
   brake (speed brake), roll (runway roll; fades out 0.3 s after liftoff). PULL UP tone loop starts at position 0
   while the warning shows. One-shots: ab_light when ab starts rising, gear_up / gear_clunk / beep / expl_near from
   the world event queue (own read index). Master [sound] master 0.7 (AB + wind + a pull sums to ~1.4x full scale).
   Reads game state only; paused = everything at the floor. */
#include "snd.h"
#include "params.h"
#include "world.h"
#include "takeoff.h"
#include "e_audio.h"
#include "e_file.h"
#include "e_log.h"
#include "e_plat.h"
#include "fx.h"

#define SND_FLOOR 0.0002f                           /* -74 dB: inaudible, the loop keeps running */

static float s_master, s_volEngine, s_volWind, s_volBrake, s_volRoll, s_volPull, s_volFx, s_cockpitEngine;
static float s_windUMax, s_windCap, s_windLowAlt, s_windLowAltM, s_windTrans, s_windMachW;   /* T10b speed cues */
static const Param s_sndTab[] = {
    { "sound", "master", P_FLOAT, &s_master, "0.7", 0, 1, "master volume (A5: keep ~0.7, AB + wind + pull sums to ~1.4x)" },
    { "sound", "engine", P_FLOAT, &s_volEngine, "1.0", 0, 2, "engine loops (idle / military / afterburner)" },
    { "sound", "wind", P_FLOAT, &s_volWind, "1.0", 0, 2, "wind rush (speed and G)" },
    { "sound", "brake", P_FLOAT, &s_volBrake, "1.0", 0, 2, "speed brake buffet (Z)" },
    { "sound", "roll", P_FLOAT, &s_volRoll, "1.0", 0, 2, "runway roll rumble" },
    { "sound", "pullup", P_FLOAT, &s_volPull, "0.55", 0, 1, "PULL UP warning tone" },
    { "sound", "effects", P_FLOAT, &s_volFx, "1.0", 0, 2, "one-shots: afterburner light, gear, beeps, explosions" },
    { "sound", "cockpitEngine", P_FLOAT, &s_cockpitEngine, "0.8", 0, 2, "engine volume factor in the cockpit view" },
    /* T10b: wind rises further with speed ([fx] speedCues = 0: the plain A5 law) */
    { "sound", "windUMax", P_FLOAT, &s_windUMax, "1.6", 1.25, 2.5, "wind law u = V/800 kt is clamped here (A5: 1.25): keeps rising to ~1280 kt" },
    { "sound", "windCap", P_FLOAT, &s_windCap, "0.95", 0.5, 1.5, "wind volume cap (A5: 0.8)" },
    { "sound", "windLowAlt", P_FLOAT, &s_windLowAlt, "0.35", 0, 2, "wind louder by this fraction at the ground (dense air, speed feel down low)" },
    { "sound", "windLowAltM", P_FLOAT, &s_windLowAltM, "500", 1, 5000, "low-altitude wind boost fades out by this height (m)" },
    { "sound", "windTransonic", P_FLOAT, &s_windTrans, "0.25", 0, 1, "transonic rumble: wind volume flutter depth around Mach 1" },
    { "sound", "windMachW", P_FLOAT, &s_windMachW, "0.06", 0.005f, 0.5f, "transonic band half width (Mach)" },
};
void snd_params(void) { params_add(s_sndTab, E_COUNT(s_sndTab)); }

enum { S_IDLE, S_MIL, S_AB, S_WIND, S_BRAKE, S_ROLL, S_PULL, S_ABLIGHT, S_GEARUP, S_CLUNK, S_EXPL, S_BEEP, S_COUNT };
static const char *const s_files[S_COUNT] = { "eng_idle", "eng_mil", "eng_ab", "wind", "brake", "roll", "pullup",
                                              "ab_light", "gear_up", "gear_clunk", "expl_near", "beep" };
static ASnd   s_snd[S_COUNT];
static AVoice s_loop[S_PULL + 1];                   /* engine, wind, brake, roll loops (+ pullup voice) */
static int    s_ok, s_paused;
static float  s_N = 0.65f, s_prevAb, s_rollFade, s_alive = 1.0f;
static s64    s_lastUs;
static u32    s_seen;

int snd_init(void)
{
    char path[E_PATH], rel[64];
    int i, n = 0;
    memset(s_snd, 0, sizeof(s_snd));
    memset(s_loop, 0, sizeof(s_loop));
    if (!au_ok()) { log_printf(LOG_INFO, "snd: no audio (silent)"); return 0; }
    for (i = 0; i < S_COUNT; i++) {
        e_snprintf(rel, (int)sizeof(rel), "sfx/%s.wav", s_files[i]);
        data_path(path, (int)sizeof(path), rel);
        s_snd[i] = au_load(path);
        if (s_snd[i]) n++;
        else log_printf(LOG_WARN, "warn: snd missing %s", path);
    }
    for (i = S_IDLE; i <= S_ROLL; i++)
        if (s_snd[i]) s_loop[i] = au_play(s_snd[i], SND_FLOOR, 0.0f, 1.0f, 1);
    s_ok = 1;
    s_N = 0.65f;
    s_lastUs = 0;
    s_seen = 0;
    log_printf(LOG_INFO, "snd: init sounds=%d/%d", n, S_COUNT);
    return 1;
}

void snd_free(void)
{
    int i;
    for (i = 0; i <= S_PULL; i++) if (s_loop[i]) au_stop(s_loop[i]);
    memset(s_loop, 0, sizeof(s_loop));
    s_ok = 0;
}

void snd_set_paused(int on) { s_paused = on; }

static void one_shot(int id, float vol)
{
    if (s_snd[id]) au_play(s_snd[id], vol * s_volFx, 0.0f, 1.0f, 0);
}

static void loop_set(int id, float vol, float pitch)
{
    if (s_loop[id]) au_set(s_loop[id], vol > SND_FLOOR ? vol : SND_FLOOR, 0.0f, pitch);
}

void snd_frame(const Game *g, const Camera *c)
{
    const Aircraft *pl = &g->ac[PLAYER_ID];
    const FlightState *fs = &pl->fs;
    const TakeoffState *ts = session_takeoff();
    const WorldStatus *ws = world_status(PLAYER_ID);
    s64 now = plat_now_us();
    float dt = s_lastUs ? (float)(now - s_lastUs) * 1e-6f : 0.0f;
    float tgt, tau, t, wi, wm, wab, kv, mach, kt, u, gl, eng;
    int alive = (pl->flags & AC_ALIVE) != 0, i;
    WorldEvent ev;
    s_lastUs = now;
    if (!s_ok) return;
    au_master(s_master);
    dt = E_CLAMP(dt, 0.0f, 0.1f);
    if (s_paused) {
        for (i = S_IDLE; i <= S_ROLL; i++) loop_set(i, 0.0f, 1.0f);
        if (s_loop[S_PULL]) { au_stop(s_loop[S_PULL]); s_loop[S_PULL] = 0; }
        return;
    }
    /* world events (own read index; the queue restarts at every session start) */
    if (s_seen > world_event_seq()) s_seen = 0;
    while (s_seen < world_event_seq()) {
        if (world_event_get(s_seen, &ev) && ev.ac == PLAYER_ID) {
            switch (ev.type) {
            case WEV_GEAR_UP: one_shot(S_GEARUP, 0.35f); break;
            case WEV_GEAR_LOCKED: one_shot(S_CLUNK, 0.5f); break;
            case WEV_HANDOVER: case WEV_RETURN_AREA: one_shot(S_BEEP, 0.5f); break;
            case WEV_CRASH_GROUND: case WEV_CRASH_SEA: case WEV_CRASH_BOX: case WEV_CRASH_AIR: one_shot(S_EXPL, 1.0f); break;
            default: break;
            }
        }
        s_seen++;
    }
    /* engine spool N (README): READY 0.65, SLOW 0.74, NORMAL 0.90, FAST 1.0; tau 0.8 s up, 1.5 s down */
    if (ts && ts->phase == TO_READY) tgt = 0.65f;
    else tgt = fs->thr == THR_FAST ? 1.0f : (fs->thr == THR_SLOW ? 0.74f : 0.90f);
    tau = tgt > s_N ? 0.8f : 1.5f;
    s_N += (tgt - s_N) * (1.0f - (float)exp(-dt / tau));
    s_alive += ((alive ? 1.0f : 0.0f) - s_alive) * (1.0f - (float)exp(-dt / (alive ? 0.4f : 0.15f)));
    t = E_CLAMP((float)(log(s_N / 0.65f) / log(1.0 / 0.65)), 0.0f, 1.0f);
    wi = (float)cos(t * 1.5707963f);
    wm = (float)sin(t * 1.5707963f);
    wab = (float)sin(E_CLAMP(fs->ab, 0.0f, 1.0f) * 1.5707963f);
    wm *= 1.0f - 0.35f * fs->ab;
    mach = E_CLAMP(fs->mach, 0.2f, 1.8f);
    kv = 1.0f + 0.06f * (mach - 0.8f);
    eng = s_volEngine * s_alive * (c && c->mode == VIEW_COCKPIT ? s_cockpitEngine : 1.0f);
    loop_set(S_IDLE, wi * eng, s_N / 0.65f * kv);
    loop_set(S_MIL, wm * eng, s_N * kv);
    loop_set(S_AB, wab * eng, s_N * kv);
    if (fs->ab > 0.02f && s_prevAb <= 0.02f && alive) one_shot(S_ABLIGHT, 0.8f);
    s_prevAb = fs->ab;
    /* wind (V kt, G): A5 law; T10b speed cues extend u and the cap, add a low-altitude boost and a transonic flutter */
    kt = fs->V * MS2KT;
    gl = fs->n;
    if (fx_speed_cues()) {
        float agl = fs->pos.y - (g->terrain ? world_surface(g, fs->pos.x, fs->pos.z) : 0.0f), lo, x, fl, vol;
        static float s_wt;
        lo = 1.0f + s_windLowAlt * E_CLAMP(1.0f - agl / (s_windLowAltM > 1.0f ? s_windLowAltM : 1.0f), 0.0f, 1.0f);
        if (fs->onGround) lo = 1.0f;
        u = E_CLAMP(kt / 800.0f, 0.0f, s_windUMax);
        x = (fs->mach - 1.0f) / (s_windMachW > 0.005f ? s_windMachW : 0.005f);
        s_wt += dt;
        if (s_wt > 1000.0f) s_wt -= 1000.0f;
        fl = 1.0f + s_windTrans * (float)exp(-x * x) * (0.6f * (float)sin(6.2831853f * 7.0f * s_wt) +
                                                         0.4f * (float)sin(6.2831853f * 11.3f * s_wt + 1.3f));
        vol = E_MINF(s_windCap, 0.55f * u * u * lo * (1.0f + 0.12f * E_MAXF(0.0f, gl - 1.5f))) * fl;
        loop_set(S_WIND, s_volWind * s_alive * vol, 0.7f + 0.5f * u + 0.02f * E_MAXF(0.0f, gl - 1.0f));
    } else {
        u = E_CLAMP(kt / 800.0f, 0.0f, 1.25f);
        loop_set(S_WIND, s_volWind * s_alive * E_MINF(0.8f, 0.55f * u * u * (1.0f + 0.12f * E_MAXF(0.0f, gl - 1.5f))),
                 0.7f + 0.5f * u + 0.02f * E_MAXF(0.0f, gl - 1.0f));
    }
    /* speed brake */
    loop_set(S_BRAKE, s_volBrake * s_alive * 0.5f * fs->brake * E_CLAMP(kt / 450.0f, 0.3f, 1.2f),
             0.85f + 0.25f * E_CLAMP(kt / 600.0f, 0.0f, 1.2f));
    /* runway roll: on the ground, fades 0.3 s after liftoff */
    if (fs->onGround && alive) s_rollFade = 1.0f;
    else s_rollFade = E_MAXF(0.0f, s_rollFade - dt / 0.3f);
    {
        float r = E_CLAMP(fs->V / 40.0f, 0.0f, 1.0f);
        loop_set(S_ROLL, s_volRoll * s_rollFade * 0.8f * (float)pow(r, 0.6f), E_CLAMP(fs->V / 40.0f, 0.25f, 2.2f));
    }
    /* PULL UP tone */
    if (ws && ws->pullUp && alive) {
        if (!s_loop[S_PULL] && s_snd[S_PULL]) s_loop[S_PULL] = au_play(s_snd[S_PULL], s_volPull, 0.0f, 1.0f, 1);
        else if (s_loop[S_PULL]) au_set(s_loop[S_PULL], s_volPull, 0.0f, 1.0f);
    } else if (s_loop[S_PULL]) {
        au_stop(s_loop[S_PULL]);
        s_loop[S_PULL] = 0;
    }
}

void snd_ui(int which)
{
    if (!s_ok) return;
    if (which == 0) one_shot(S_BEEP, 0.5f);
}
