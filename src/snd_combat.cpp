/* snd_combat.cpp - combat sound logic over e_audio (DESIGN 15, data/sfx/README_combat.md). See snd_combat.h.
   Windows side. Owner: T17. Volumes are README_combat's suggested e_audio volumes x the [sound] group volume; the
   master (au_master) is set by snd.cpp. Positional: volume by distance law, pan by the camera's right vector,
   doppler from the range rate; delayed explosions through a small queue (d / 340 m/s). */
#include "snd_combat.h"
#include "params.h"
#include "radar.h"
#include "weapons.h"
#include "missile.h"
#include "world.h"
#include "mission.h"
#include "hud_wpn.h"
#include "e_audio.h"
#include "e_file.h"
#include "e_log.h"
#include "e_plat.h"

#define SC_FLOOR 0.0002f                            /* -74 dB: inaudible, the loop keeps running */
#define SC_DB4   0.631f                             /* -4 dB (anti-fatigue) */
#define SC_DB3   0.708f                             /* -3 dB (under PULL UP) */
#define SC_DB6   0.501f                             /* -6 dB (growl under the missile warning) */
#define SC_SOUND 340.0f

static float s_volWarn, s_volCombat;
static const Param s_tab[] = {
    { "sound", "warn", P_FLOAT, &s_volWarn, "1.0", 0, 2, "warning tones: RWR lock / missile / waterfall, SRM growl + lock tone, lock beep, NO MSL click" },
    { "sound", "combat", P_FLOAT, &s_volCombat, "1.0", 0, 2, "combat effects: launches, missile motors, pass-bys, explosions, hit, flares, kill chime" },
};
static int s_paramsAdded;
void sndc_params(void) { if (!s_paramsAdded) { params_add(s_tab, E_COUNT(s_tab)); s_paramsAdded = 1; } }

enum { C_GROWL, C_SRMLOCK, C_RWRLOCK, C_RWRMSL, C_MOTOR, C_LAUNCH, C_PASS, C_LOCKBEEP, C_SEARCH, C_RWRLAUNCH, C_FLARE, C_HIT,
       C_KILL, C_JETPASS, C_EXPLAIR, C_NOMSL, C_EXPLFAR, C_EXPLNEAR, C_COUNT };
static const char *const s_files[C_COUNT] = { "srm_growl", "srm_lock", "rwr_lock", "rwr_missile", "msl_motor", "msl_launch",
    "msl_pass", "lock_beep", "rwr_search", "rwr_launch", "flare", "hit", "kill", "jet_pass", "expl_air", "no_msl", "expl_far",
    "expl_near" };
enum { MOTORS = 3, DELAYED = 16 };
typedef struct Delayed { int snd; float vol, pan, pitch; s64 dueUs; } Delayed;

static ASnd   s_snd[C_COUNT];
static AVoice s_growl, s_srmLock, s_motor[MOTORS];  /* kept looping at the floor from init */
static AVoice s_rwrLock, s_rwrMsl, s_rwrLaunch;     /* started at position 0 when needed */
static int    s_ok, s_paused;
static s64    s_lastUs;
static u32    s_wpnSeen, s_radSeen, s_wevSeen;
static int    s_lastHits, s_lastKills;
static Delayed s_del[DELAYED];
/* warning state */
static int    s_lockOn, s_lockStop;                 /* rwr_lock voice; stop countdown frames */
static int    s_mslOn, s_mslStop;                   /* missile warning active; stop countdown */
static s64    s_mslStartUs, s_mslFullUs;            /* warning start, last "full volume" reset */
static int    s_mslLoopStarted;
static int    s_srmLockStop;
static s64    s_lastSearchUs, s_lastBeepUs;
static int    s_lastBeepId = -1;
static s64    s_jetPassUs[MAX_AIRCRAFT];

static float wv(void) { return s_volWarn; }
static float cv(void) { return s_volCombat; }
static float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

int sndc_init(void)
{
    char path[E_PATH], rel[64];
    int i, n = 0;
    memset(s_snd, 0, sizeof(s_snd));
    s_growl = s_srmLock = s_rwrLock = s_rwrMsl = s_rwrLaunch = 0;
    memset(s_motor, 0, sizeof(s_motor));
    if (!s_paramsAdded) sndc_params();
    if (!au_ok()) { log_printf(LOG_INFO, "sndc: no audio (silent)"); return 0; }
    for (i = 0; i < C_COUNT; i++) {
        e_snprintf(rel, (int)sizeof(rel), "sfx/%s.wav", s_files[i]);
        data_path(path, (int)sizeof(path), rel);
        s_snd[i] = au_load(path);
        if (s_snd[i]) n++;
        else log_printf(LOG_WARN, "warn: sndc missing %s", path);
    }
    if (s_snd[C_GROWL]) s_growl = au_play(s_snd[C_GROWL], SC_FLOOR, 0.0f, 1.0f, 1);
    if (s_snd[C_SRMLOCK]) s_srmLock = au_play(s_snd[C_SRMLOCK], SC_FLOOR, 0.0f, 1.0f, 1);
    for (i = 0; i < MOTORS; i++) if (s_snd[C_MOTOR]) s_motor[i] = au_play(s_snd[C_MOTOR], SC_FLOOR, 0.0f, 1.0f, 1);
    s_ok = 1;
    sndc_reset();
    log_printf(LOG_INFO, "sndc: init sounds=%d/%d", n, C_COUNT);
    return 1;
}

static void stop_voice(AVoice *v) { if (*v) { au_stop(*v); *v = 0; } }

void sndc_free(void)
{
    int i;
    stop_voice(&s_growl); stop_voice(&s_srmLock); stop_voice(&s_rwrLock); stop_voice(&s_rwrMsl); stop_voice(&s_rwrLaunch);
    for (i = 0; i < MOTORS; i++) stop_voice(&s_motor[i]);
    s_ok = 0;
}

void sndc_reset(void)
{
    s_wpnSeen = weapons_event_count();
    s_radSeen = radar_event_seq();
    s_wevSeen = world_event_seq();
    s_lastHits = mission_stats()->playerHits;
    s_lastKills = mission_stats()->kills;
    memset(s_del, 0, sizeof(s_del));
    memset(s_jetPassUs, 0, sizeof(s_jetPassUs));
    stop_voice(&s_rwrLock); stop_voice(&s_rwrMsl); stop_voice(&s_rwrLaunch);
    s_lockOn = s_lockStop = s_mslOn = s_mslStop = s_mslLoopStarted = s_srmLockStop = 0;
    s_lastBeepId = -1;
}

void sndc_set_paused(int on) { s_paused = on; }

static void play(int id, float vol, float pan, float pitch)
{
    if (s_snd[id] && vol > 0.002f) au_play(s_snd[id], vol, pan, pitch, 0);
}

static void play_delayed(int id, float vol, float pan, float pitch, float delaySec)
{
    int i;
    if (delaySec <= 0.01f) { play(id, vol, pan, pitch); return; }
    for (i = 0; i < DELAYED; i++)
        if (!s_del[i].snd) {
            s_del[i].snd = id + 1;                  /* +1: 0 = free */
            s_del[i].vol = vol; s_del[i].pan = pan; s_del[i].pitch = pitch;
            s_del[i].dueUs = plat_now_us() + (s64)(delaySec * 1e6f);
            return;
        }
}

/* listener = camera; pan from the camera's right vector */
static float pan_of(const Camera *c, Vec3 p)
{
    Vec3 r = v3(c->view.m[0][0], c->view.m[1][0], c->view.m[2][0]), d = v3_sub(p, c->eye);
    float l = v3_len(d);
    return l > 1.0f ? clampf(0.8f * v3_dot(d, r) / l, -0.8f, 0.8f) : 0.0f;
}

static Vec3 ac_vel(const FlightState *fs) { return q_rotate(fs->qPath, v3(0.0f, 0.0f, fs->V)); }

/* explosion at p: expl_air within 4 km (0.95, x 1 km / d beyond 1 km), else expl_far; delayed by d / 340 */
static void explosion(const Camera *c, Vec3 p, int ground)
{
    float d = v3_len(v3_sub(p, c->eye)), vol;
    if (d > 12000.0f) return;
    if (!ground && d < 4000.0f) {
        vol = 0.95f * (d > 1000.0f ? 1000.0f / d : 1.0f);
        play_delayed(C_EXPLAIR, vol * cv(), pan_of(c, p), 1.0f, d / SC_SOUND);
    } else {
        vol = 0.8f * (d > 1500.0f ? 1500.0f / d : 1.0f);
        play_delayed(C_EXPLFAR, vol * cv(), pan_of(c, p), 1.0f, d / SC_SOUND);
    }
}

static void loop_set(AVoice v, float vol, float pan, float pitch) { if (v) au_set(v, vol > SC_FLOOR ? vol : SC_FLOOR, pan, pitch); }

/* continuous tones: 3 frames of falling volume, then stop (README: a hard stop mid-cycle can tick) */
static void fade_stop(AVoice *v, int *frames, float vol)
{
    if (!*v) { *frames = 0; return; }
    if (*frames <= 0) *frames = 3;
    (*frames)--;
    if (*frames > 0) au_set(*v, vol * (*frames == 2 ? 0.5f : 0.25f), 0.0f, 1.0f);
    else stop_voice(v);
}

static void events(const Game *g, const Camera *c)
{
    const Aircraft *pl = &g->ac[PLAYER_ID];
    RadarEvent re;
    WorldEvent we;
    s64 now = plat_now_us();
    const MissionStats *ms = mission_stats();
    /* restart detection (a new session resets the rings) */
    if (s_wpnSeen > weapons_event_count()) s_wpnSeen = 0;
    if (s_radSeen > radar_event_seq()) s_radSeen = 0;
    if (s_wevSeen > world_event_seq()) s_wevSeen = 0;
    if (ms->playerHits < s_lastHits) s_lastHits = 0;
    if (ms->kills < s_lastKills) s_lastKills = 0;
    while (s_wpnSeen < weapons_event_count()) {
        const WpnEvent *e = weapons_event(s_wpnSeen++);
        float d;
        if (!e) continue;
        d = v3_len(v3_sub(e->pos, c->eye));
        switch (e->type) {
        case WPE_LAUNCH:
            if (e->owner == PLAYER_ID) play(C_LAUNCH, 0.95f * cv(), 0.0f, e->mtype == MSL_SRM ? 1.04f : 1.0f);
            else play(C_LAUNCH, 0.95f * cv() * (d > 300.0f ? 300.0f / d : 1.0f), pan_of(c, e->pos), e->mtype == MSL_SRM ? 1.04f : 1.0f);
            break;
        case WPE_FLARES:
            if (e->owner == PLAYER_ID) play(C_FLARE, 1.0f * cv(), 0.0f, 1.0f);
            else play(C_FLARE, cv() * (d > 100.0f ? 100.0f / d : 1.0f), pan_of(c, e->pos), 1.0f);
            break;
        case WPE_EMPTY:
            if (e->owner == PLAYER_ID) play(C_NOMSL, 0.70f * wv(), 0.0f, 1.0f);
            break;
        case WPE_PASS:
            if (e->victim == PLAYER_ID && (pl->flags & AC_ALIVE)) {
                float miss = v3_len(v3_sub(e->pos, pl->fs.pos));
                play(C_PASS, 0.85f * cv() * E_MAXF(0.4f, E_MINF(1.0f, 50.0f / E_MAXF(miss, 1.0f))), pan_of(c, e->pos), 1.0f);
            }
            break;
        case WPE_HIT:
            if (e->victim != PLAYER_ID) explosion(c, e->pos, 0);
            break;
        case WPE_GROUND:
            explosion(c, e->pos, 1);
            break;
        case WPE_SELFDESTRUCT:
            if (d < 4000.0f) play_delayed(C_EXPLAIR, 0.4f * cv() * (d > 1000.0f ? 1000.0f / d : 1.0f), pan_of(c, e->pos), 1.2f, d / SC_SOUND);
            break;
        default:
            break;
        }
    }
    /* own damage / death (mission counters: hits while invulnerable do not count) */
    if (ms->playerHits != s_lastHits) {
        if (pl->flags & AC_ALIVE) play(C_HIT, 1.0f * cv(), 0.0f, 1.0f);
        else play(C_EXPLNEAR, 1.0f * cv(), 0.0f, 1.0f);
        s_lastHits = ms->playerHits;
    }
    if (ms->kills != s_lastKills) {                 /* SPLASH chime, not delayed by distance */
        play_delayed(C_KILL, 0.75f * wv(), 0.0f, 1.0f, 0.35f);
        s_lastKills = ms->kills;
    }
    /* radar: own lock beep, enemy radars detecting the player (waterfall) */
    while (s_radSeen < radar_event_seq()) {
        if (!radar_event_get(s_radSeen++, &re)) continue;
        if (re.type == REV_LOCK && re.obs == PLAYER_ID) {
            if (!(re.id == s_lastBeepId && now - s_lastBeepUs < 2000000)) play(C_LOCKBEEP, 0.45f * wv(), 0.0f, 1.0f);
            s_lastBeepId = re.id;
            s_lastBeepUs = now;
        } else if (re.type == REV_DETECT && re.id == PLAYER_ID && re.obs != PLAYER_ID) {
            if (!s_rwrLock && !s_rwrMsl && !s_rwrLaunch && now - s_lastSearchUs >= 1000000 && (pl->flags & AC_ALIVE)) {
                play(C_SEARCH, 0.25f * wv(), 0.0f, 1.0f);
                s_lastSearchUs = now;
            }
        } else if (re.type == REV_LAUNCH && re.id == PLAYER_ID) {
            s_mslFullUs = now;                      /* a new missile: anti-fatigue back to full */
        }
    }
    /* enemy crashes (own crash: snd.cpp's expl_near) */
    while (s_wevSeen < world_event_seq()) {
        if (!world_event_get(s_wevSeen++, &we)) continue;
        if ((we.type & WEV_CRASH_ANY) && we.ac != PLAYER_ID) explosion(c, we.pos, 1);
    }
}

/* nearest heat target angle off the nose (deg) for the growl signal; 99 = none */
static float nearest_angle(const Game *g)
{
    const Aircraft *pl = &g->ac[PLAYER_ID];
    Vec3 f = q_rotate(pl->fs.qPath, v3(0.0f, 0.0f, 1.0f));
    float best = 99.0f;
    int i;
    for (i = 1; i < g->numAircraft; i++) {
        const Aircraft *a = &g->ac[i];
        Vec3 d;
        float l, ang;
        if (!(a->flags & AC_ALIVE)) continue;
        d = v3_sub(a->fs.pos, pl->fs.pos);
        l = v3_len(d);
        if (l < 1.0f || l > 9000.0f) continue;
        ang = E_RAD2DEG((float)acos(clampf(v3_dot(d, f) / l, -1.0f, 1.0f)));
        if (ang < best) best = ang;
    }
    return best;
}

static void tones(const Game *g, const Camera *c)
{
    const Aircraft *pl = &g->ac[PLAYER_ID];
    const WorldStatus *ws = world_status(PLAYER_ID);
    HudWpnTones t;
    s64 now = plat_now_us();
    int alive = (pl->flags & AC_ALIVE) != 0 && !c->fixed, slots[8], n, i;
    float mslVol, nearest = 1e9f;
    memset(&t, 0, sizeof(t));
    if (alive) hudwpn_tones(&t);
    /* ---- missile warning: rwr_launch onset, then the rwr_missile loop */
    if (t.missile) {
        if (!s_mslOn) {
            s_mslOn = 1;
            s_mslStop = 0;
            s_mslLoopStarted = 0;
            s_mslStartUs = s_mslFullUs = now;
            stop_voice(&s_rwrMsl);
            if (s_snd[C_RWRLAUNCH]) s_rwrLaunch = au_play(s_snd[C_RWRLAUNCH], 0.45f * wv(), 0.0f, 1.0f, 0);
        }
        n = weapons_threats(PLAYER_ID, slots, 8);
        for (i = 0; i < n; i++) {
            const Missile *m = weapons_missile(slots[i]);
            if (m) nearest = E_MINF(nearest, v3_len(v3_sub(m->pos, pl->fs.pos)));
        }
        if (nearest < 2000.0f) s_mslFullUs = now;
        mslVol = 0.45f * wv() * (now - s_mslFullUs > 2000000 ? SC_DB4 : 1.0f) * (ws && ws->pullUp ? SC_DB3 : 1.0f);
        if (!s_mslLoopStarted && (!s_rwrLaunch || !au_playing(s_rwrLaunch) || now - s_mslStartUs >= 500000)) {
            stop_voice(&s_rwrLaunch);
            if (s_snd[C_RWRMSL]) s_rwrMsl = au_play(s_snd[C_RWRMSL], mslVol, 0.0f, 1.0f, 1);
            s_mslLoopStarted = 1;
        }
        if (s_rwrLaunch) au_set(s_rwrLaunch, mslVol, 0.0f, 1.0f);
        if (s_rwrMsl) au_set(s_rwrMsl, mslVol, 0.0f, 1.0f);
    } else if (s_mslOn) {
        stop_voice(&s_rwrLaunch);
        fade_stop(&s_rwrMsl, &s_mslStop, 0.45f * wv());
        if (!s_rwrMsl) s_mslOn = 0;
    }
    /* ---- RWR lock tone (muted while the missile warning plays) */
    if (t.rwrLock && !s_mslOn) {
        if (!s_rwrLock && s_snd[C_RWRLOCK]) s_rwrLock = au_play(s_snd[C_RWRLOCK], 0.30f * wv(), 0.0f, 1.0f, 1);
        else if (s_rwrLock) au_set(s_rwrLock, 0.30f * wv(), 0.0f, 1.0f);
        s_lockStop = 0;
    } else if (s_rwrLock) {
        fade_stop(&s_rwrLock, &s_lockStop, 0.30f * wv());
    }
    /* ---- SRM growl / lock tone (never together) */
    if (t.srmLock) {
        loop_set(s_growl, 0.0f, 0.0f, 1.0f);
        loop_set(s_srmLock, 0.25f * wv(), 0.0f, 1.0f);
        s_srmLockStop = 0;
    } else {
        if (s_srmLockStop < 3) {                    /* falling volume, the loop keeps running at the floor */
            s_srmLockStop++;
            loop_set(s_srmLock, 0.25f * wv() * (s_srmLockStop == 1 ? 0.5f : (s_srmLockStop == 2 ? 0.25f : 0.0f)), 0.0f, 1.0f);
        } else loop_set(s_srmLock, 0.0f, 0.0f, 1.0f);
        if (t.growl) {
            float s = clampf(1.0f - nearest_angle(g) / 12.0f, 0.0f, 1.0f);
            float vol = 0.65f * (float)pow(10.0, -6.0 * (1.0 - s) / 20.0) * wv() * (s_mslOn ? SC_DB6 : 1.0f);
            loop_set(s_growl, vol, 0.0f, 1.0f + 0.25f * s);
        } else loop_set(s_growl, 0.0f, 0.0f, 1.0f);
    }
}

/* nearest three missiles: motor loops with distance law, doppler, pan; jet pass-bys */
static void positional(const Game *g, const Camera *c, float alpha)
{
    static MissileView mv[MAX_MISSILES];
    const Aircraft *pl = &g->ac[PLAYER_ID];
    Vec3 lv = (pl->flags & AC_ALIVE) ? ac_vel(&pl->fs) : v3(0.0f, 0.0f, 0.0f);
    int n = weapons_missiles(mv, MAX_MISSILES), i, k, used[MAX_MISSILES], pick[MOTORS];
    float dist[MAX_MISSILES];
    s64 now = plat_now_us();
    for (i = 0; i < n; i++) {
        Vec3 p = v3_lerp(mv[i].prevPos, mv[i].pos, alpha);
        dist[i] = v3_len(v3_sub(p, c->eye));
        used[i] = 0;
        if (mv[i].owner == PLAYER_ID && mv[i].age < 2.0f) dist[i] = 1e9f;   /* msl_launch carries the first 2 s */
    }
    for (k = 0; k < MOTORS; k++) {
        int best = -1;
        for (i = 0; i < n; i++) if (!used[i] && dist[i] < 4000.0f && (best < 0 || dist[i] < dist[best])) best = i;
        pick[k] = best;
        if (best >= 0) used[best] = 1;
    }
    for (k = 0; k < MOTORS; k++) {
        if (pick[k] < 0) { loop_set(s_motor[k], 0.0f, 0.0f, 1.0f); continue; }
        {
            const MissileView *m = &mv[pick[k]];
            Vec3 p = v3_lerp(m->prevPos, m->pos, alpha), d = v3_sub(p, c->eye), rel = v3_sub(v3_scale(m->dir, m->V), lv);
            float l = E_MAXF(v3_len(d), 1.0f), rr = v3_dot(rel, d) / l, pitch = clampf(SC_SOUND / E_MAXF(SC_SOUND + rr, 50.0f), 0.6f, 1.6f);
            float fadeIn = m->owner == PLAYER_ID ? clampf((m->age - 2.0f) / 1.0f, 0.0f, 1.0f) : 1.0f;
            loop_set(s_motor[k], 0.65f * cv() * E_MINF(1.0f, 100.0f / l) * fadeIn, pan_of(c, p), pitch);
        }
    }
    /* jet pass-by: enemy closing, closest approach within 0.6 s and < 600 m */
    if (pl->flags & AC_ALIVE)
        for (i = 1; i < g->numAircraft; i++) {
            const Aircraft *a = &g->ac[i];
            Vec3 r, v;
            float vv, tca, miss;
            if (!(a->flags & AC_ALIVE)) continue;
            r = v3_sub(a->fs.pos, pl->fs.pos);
            if (v3_len(r) > 1500.0f) continue;
            v = v3_sub(ac_vel(&a->fs), lv);
            vv = v3_dot(v, v);
            if (vv < 1.0f) continue;
            tca = -v3_dot(r, v) / vv;
            if (tca <= 0.0f || tca > 0.6f) continue;
            miss = v3_len(v3_madd(r, v, tca));
            if (miss > 600.0f || now - s_jetPassUs[i] < 8000000) continue;
            s_jetPassUs[i] = now;
            play(C_JETPASS, 0.90f * cv() * E_MINF(1.0f, 150.0f / E_MAXF(miss, 1.0f)), pan_of(c, a->fs.pos),
                 clampf(0.9f + 0.2f * (float)sqrt(vv) / 800.0f, 0.9f, 1.1f));
        }
}

void sndc_frame(const Game *g, const Camera *c)
{
    s64 now = plat_now_us();
    int i;
    s_lastUs = now;
    if (!s_ok) return;
    if (s_paused) {
        loop_set(s_growl, 0.0f, 0.0f, 1.0f);
        loop_set(s_srmLock, 0.0f, 0.0f, 1.0f);
        for (i = 0; i < MOTORS; i++) loop_set(s_motor[i], 0.0f, 0.0f, 1.0f);
        stop_voice(&s_rwrLock); stop_voice(&s_rwrMsl); stop_voice(&s_rwrLaunch);
        s_lockOn = s_mslOn = s_mslLoopStarted = 0;
        return;
    }
    for (i = 0; i < DELAYED; i++)
        if (s_del[i].snd && now >= s_del[i].dueUs) {
            play(s_del[i].snd - 1, s_del[i].vol, s_del[i].pan, s_del[i].pitch);
            s_del[i].snd = 0;
        }
    events(g, c);
    tones(g, c);
    positional(g, c, 1.0f);
}
