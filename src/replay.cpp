/* replay.cpp - see replay.h. Portable. Owner: T01 (skeleton), T08 (complete: key records, tests on the real session). */
#include "replay.h"
#include "params.h"
#include "tests.h"
#include "e_log.h"
#include "e_file.h"
#include "e_terrain.h"
#include "takeoff.h"

enum { RPL_VERSION = 2, RPL_SNAP_MAX = 65536, KB = 12 };   /* KB = packed key record bytes */

static FILE *s_f;
static int s_mode;                                  /* 1 recording, 2 playing */
static ReplayStats s_st;
static u8 s_last[KB]; static int s_haveLast; static u32 s_rep;    /* recording: repeat run */
static u8 s_cur[KB]; static u32 s_left;             /* playback: current keys + repeats left */
static int s_ended;
static u32 s_nextTick;                              /* recording: tick after the last recorded one */
static char s_snap[RPL_SNAP_MAX];

/* ---------------------------------------------------------------- packing */
static void put_u16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put_u32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 get_u16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 get_u32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static s16 q16(float v)
{
    v = E_CLAMP(v, -1.0f, 1.0f) * 32767.0f;
    return (s16)(v >= 0.0f ? (int)(v + 0.5f) : -(int)(-v + 0.5f));
}

static u32 sat16(int v) { return v < 0 ? 0u : (v > 65535 ? 65535u : (u32)v); }

static void pack(const PilotKeys *k, int cmd, u8 *b)
{
    u32 bits = (k->up ? 1u : 0u) | (k->down ? 2u : 0u) | (k->left ? 4u : 0u) | (k->right ? 8u : 0u) |
               (k->fast ? 16u : 0u) | (k->slow ? 32u : 0u) | (k->fire ? 64u : 0u) | (k->flare ? 128u : 0u) |
               (k->wpnNext ? 256u : 0u) | (k->view ? 512u : 0u) | (k->look ? 1024u : 0u) | (k->joyValid ? 2048u : 0u) |
               (((u32)cmd & 15u) << 12);
    put_u16(b, bits);
    put_u16(b + 2, sat16(k->tgtHeld));
    put_u16(b + 4, sat16(k->tgtReleased));
    put_u16(b + 6, (u32)(u16)q16(k->joyValid ? k->joyX : 0.0f));
    put_u16(b + 8, (u32)(u16)q16(k->joyValid ? k->joyY : 0.0f));
    put_u16(b + 10, 0);
}

static void unpack(const u8 *b, PilotKeys *k, int *cmd)
{
    u32 bits = get_u16(b);
    memset(k, 0, sizeof(*k));
    k->up = (int)(bits & 1u); k->down = (int)((bits >> 1) & 1u); k->left = (int)((bits >> 2) & 1u);
    k->right = (int)((bits >> 3) & 1u); k->fast = (int)((bits >> 4) & 1u); k->slow = (int)((bits >> 5) & 1u);
    k->fire = (int)((bits >> 6) & 1u); k->flare = (int)((bits >> 7) & 1u); k->wpnNext = (int)((bits >> 8) & 1u);
    k->view = (int)((bits >> 9) & 1u); k->look = (int)((bits >> 10) & 1u); k->joyValid = (int)((bits >> 11) & 1u);
    k->tgtHeld = (int)get_u16(b + 2);
    k->tgtReleased = (int)get_u16(b + 4);
    k->joyX = (float)(s16)get_u16(b + 6) / 32767.0f;
    k->joyY = (float)(s16)get_u16(b + 8) / 32767.0f;
    if (cmd) *cmd = (int)((bits >> 12) & 15u);
}

void replay_quantise_keys(PilotKeys *k)
{
    u8 b[KB];
    pack(k, 0, b);
    unpack(b, k, NULL);
}

u32 replay_state_hash(const Game *g)
{
    int i;
    u32 h = E_HASH_INIT;
    if (!g) return 0;
    for (i = 0; i < g->numAircraft; i++) {
        const FlightState *fs = &g->ac[i].fs;
        h = e_hash(&fs->pos, (int)sizeof(fs->pos), h);
        h = e_hash(&fs->qPath, (int)sizeof(fs->qPath), h);
        h = e_hash(&fs->V, (int)sizeof(fs->V), h);
        h = e_hash(&fs->n, (int)sizeof(fs->n), h);
        h = e_hash(&g->ac[i].flags, (int)sizeof(g->ac[i].flags), h);
    }
    return h;
}

void replay_stats(ReplayStats *st) { *st = s_st; }
int replay_active(void) { return s_mode; }

/* ---------------------------------------------------------------- record */
static void wr(const void *p, int n)
{
    if (s_f && n > 0) { fwrite(p, 1, (size_t)n, s_f); s_st.bytes += (u32)n; }
}

static void flush_repeat(void)
{
    u8 b[3];
    while (s_rep > 0) {
        u32 n = s_rep > 65535 ? 65535 : s_rep;
        b[0] = 'R';
        put_u16(b + 1, n);
        wr(b, 3);
        s_rep -= n;
    }
}

int replay_record_open(const char *path, const ReplaySetup *rs, u32 exeCrc)
{
    u8 h[40];
    int sn;
    if (s_mode) { log_printf(LOG_WARN, "warn: replay already active mode=%d", s_mode); return 0; }
    sn = params_snapshot(s_snap, (int)sizeof(s_snap));
    if (sn < 0) { log_printf(LOG_ERR, "err: replay params snapshot too big"); return 0; }
    s_f = fopen(path, "wb");
    if (!s_f) { log_printf(LOG_ERR, "err: replay cannot write path=%s", path); return 0; }
    memset(&s_st, 0, sizeof(s_st));
    memcpy(h, "JRPL", 4);
    put_u32(h + 4, RPL_VERSION);
    put_u32(h + 8, exeCrc);
    put_u32(h + 12, (u32)rs->mode);
    put_u32(h + 16, (u32)rs->diff);
    put_u32(h + 20, (u32)rs->start);
    put_u32(h + 24, rs->seed);
    put_u32(h + 28, REPLAY_HASH_EVERY);
    put_u32(h + 32, params_hash());
    put_u32(h + 36, (u32)sn);
    wr(h, 40);
    wr(s_snap, sn);
    s_st.snapBytes = (u32)sn;
    s_mode = 1;
    s_haveLast = 0;
    s_nextTick = 0;
    s_rep = 0;
    log_printf(LOG_INFO, "replay: record path=%s seed=%u params=%08x snap=%d", path, rs->seed, params_hash(), sn);
    return 1;
}

void replay_record_tick(u32 tick, const PilotKeys *k, int cmd, const Game *g)
{
    u8 b[1 + KB];
    if (s_mode != 1) return;
    if ((tick % REPLAY_HASH_EVERY) == 0) {
        flush_repeat();
        b[0] = 'H';
        put_u32(b + 1, tick);
        put_u32(b + 5, replay_state_hash(g));
        wr(b, 9);
        s_st.hashes++;
        fflush(s_f);
        s_haveLast = 0;                             /* the next keys are written in full after a hash */
    }
    s_nextTick = tick + 1;
    pack(k, cmd, b + 1);
    if (s_haveLast && !memcmp(b + 1, s_last, KB)) s_rep++;
    else {
        flush_repeat();
        b[0] = 'K';
        wr(b, 1 + KB);
        memcpy(s_last, b + 1, KB);
        s_haveLast = 1;
    }
    s_st.ticks++;
}

void replay_record_params(u32 tick)
{
    u8 b[9];
    int sn;
    if (s_mode != 1) return;
    sn = params_snapshot(s_snap, (int)sizeof(s_snap));
    if (sn < 0) return;
    flush_repeat();
    b[0] = 'P';
    put_u32(b + 1, tick);
    put_u32(b + 5, (u32)sn);
    wr(b, 9);
    wr(s_snap, sn);
    s_st.snapBytes += (u32)sn;
    s_haveLast = 0;
    s_st.paramRecords++;
    log_printf(LOG_INFO, "replay: params tick=%u bytes=%d", tick, sn);
}

void replay_record_close(const Game *g)
{
    u8 b[9];
    if (s_mode != 1) return;
    flush_repeat();
    if (g) {
        b[0] = 'H';
        put_u32(b + 1, s_nextTick);                 /* the state before the tick that would come next */
        put_u32(b + 5, replay_state_hash(g));
        wr(b, 9);
        s_st.hashes++;
    }
    b[0] = 'E';
    put_u32(b + 1, s_st.ticks);
    wr(b, 5);
    fclose(s_f);
    s_f = NULL;
    s_mode = 0;
    log_printf(LOG_INFO, "replay: recorded ticks=%u hashes=%u params=%u bytes=%u", s_st.ticks, s_st.hashes,
               s_st.paramRecords, s_st.bytes);
}

/* ---------------------------------------------------------------- playback */
static int rd(void *p, int n)
{
    if (!s_f || n <= 0) return 0;
    if ((int)fread(p, 1, (size_t)n, s_f) != n) return 0;
    s_st.bytes += (u32)n;
    return 1;
}

int replay_play_open(const char *path, ReplaySetup *rs, u32 exeCrc)
{
    u8 h[40];
    u32 crc, sn;
    if (s_mode) { log_printf(LOG_WARN, "warn: replay already active mode=%d", s_mode); return 0; }
    s_f = fopen(path, "rb");
    if (!s_f) { log_printf(LOG_ERR, "err: replay cannot read path=%s", path); return 0; }
    memset(&s_st, 0, sizeof(s_st));
    if (!rd(h, 40) || memcmp(h, "JRPL", 4) || get_u32(h + 4) != RPL_VERSION) {
        log_printf(LOG_ERR, "err: replay bad header path=%s", path);
        fclose(s_f); s_f = NULL;
        return 0;
    }
    crc = get_u32(h + 8);
    if (exeCrc && crc && crc != exeCrc)
        log_printf(LOG_WARN, "warn: replay recorded by another build crc=%08x exe=%08x (hashes may differ)", crc, exeCrc);
    rs->mode = (int)get_u32(h + 12);
    rs->diff = (int)get_u32(h + 16);
    rs->start = (int)get_u32(h + 20);
    rs->seed = get_u32(h + 24);
    sn = get_u32(h + 36);
    if (sn >= (u32)sizeof(s_snap) || !rd(s_snap, (int)sn)) {
        log_printf(LOG_ERR, "err: replay bad params snapshot bytes=%u", sn);
        fclose(s_f); s_f = NULL;
        return 0;
    }
    params_restore(s_snap, (int)sn);
    if (params_hash() != get_u32(h + 32))
        log_printf(LOG_WARN, "warn: replay params hash differs file=%08x now=%08x (keys added/removed?)", get_u32(h + 32),
                   params_hash());
    s_mode = 2;
    s_left = 0;
    s_ended = 0;
    memset(s_cur, 0, sizeof(s_cur));
    log_printf(LOG_INFO, "replay: play path=%s seed=%u mode=%d", path, rs->seed, rs->mode);
    return 1;
}

int replay_play_tick(u32 tick, PilotKeys *k, int *cmd, const Game *g)
{
    u8 c, b[KB];
    if (cmd) *cmd = 0;
    if (s_mode != 2 || s_ended) return 0;
    if (s_left > 0) {
        s_left--;
        unpack(s_cur, k, cmd);
        if (cmd) *cmd = 0;                          /* a command is a one-tick pulse: never repeated */
        s_st.ticks++;
        return 1;
    }
    for (;;) {
        if (!rd(&c, 1)) { s_ended = 1; return 0; }
        if (c == 'K') {
            if (!rd(s_cur, KB)) { s_ended = 1; return 0; }
            unpack(s_cur, k, cmd);
            s_st.ticks++;
            return 1;
        }
        if (c == 'R') {
            if (!rd(b, 2)) { s_ended = 1; return 0; }
            s_left = get_u16(b);
            if (s_left > 0) {
                s_left--;
                unpack(s_cur, k, cmd);
                if (cmd) *cmd = 0;
                s_st.ticks++;
                return 1;
            }
            continue;
        }
        if (c == 'H') {
            u32 t, hv, now;
            if (!rd(b, 8)) { s_ended = 1; return 0; }
            t = get_u32(b);
            hv = get_u32(b + 4);
            now = replay_state_hash(g);
            s_st.hashes++;
            if (t != tick || hv != now) {
                if (!s_st.diffs) s_st.firstDiffTick = tick;
                s_st.diffs++;
                log_printf(LOG_WARN, "hash: tick=%u DIFF file_tick=%u file=%08x now=%08x", tick, t, hv, now);
            } else log_printf(LOG_DEBUG, "hash: tick=%u ok %08x", tick, now);
            continue;
        }
        if (c == 'P') {
            u32 n;
            if (!rd(b, 8)) { s_ended = 1; return 0; }
            n = get_u32(b + 4);
            if (n >= (u32)sizeof(s_snap) || !rd(s_snap, (int)n)) { s_ended = 1; return 0; }
            params_restore(s_snap, (int)n);
            s_st.paramRecords++;
            log_printf(LOG_INFO, "replay: params applied tick=%u bytes=%u", tick, n);
            continue;
        }
        if (c == 'E') { rd(b, 4); s_ended = 1; return 0; }
        log_printf(LOG_ERR, "err: replay bad record 0x%02x at tick=%u", c, tick);
        s_ended = 1;
        return 0;
    }
}

void replay_play_close(void)
{
    if (s_mode != 2) return;
    if (s_f) fclose(s_f);
    s_f = NULL;
    s_mode = 0;
    log_printf(LOG_INFO, "replay: done ticks=%u hashes=%u diff=%u first_diff=%u params=%u", s_st.ticks, s_st.hashes,
               s_st.diffs, s_st.firstDiffTick, s_st.paramRecords);
}

/* ---------------------------------------------------------------- tests */
static void tr_keys(u32 t, PilotKeys *k, int *cmd)
{
    memset(k, 0, sizeof(*k));
    k->up = (t / 50) % 3 == 0;
    k->left = (t / 70) % 2;
    k->right = !k->left && (t / 35) % 4 == 1;
    k->fast = (t / 200) % 3 == 1;
    k->slow = (t / 200) % 3 == 2;
    k->fire = (t % 97) < 3;
    k->tgtHeld = (t % 131) < 5 ? (int)(t % 131) + 1 : 0;   /* S held 5 ticks (each tick a new record) */
    k->tgtReleased = (t % 131) == 5 ? 5 : 0;
    k->joyValid = t > 900;
    k->joyX = k->joyValid ? 0.25f : 0.0f;
    k->joyY = k->joyValid ? -0.5f : 0.0f;
    *cmd = (t == 333) ? SES_CMD_RUNWAY : 0;
}

static void tr_step(Game *g, const PilotKeys *k, int cmd)
{
    Aircraft *a = &g->ac[0];
    a->fs.V += (float)k->up * 0.01f + (float)k->fast * 0.001f;
    a->fs.pos.x += (float)(k->right - k->left);
    a->fs.pos.z += a->fs.V * SIM_DT;
    if (cmd) a->fs.pos.y += 1.0f;
    g->tick++;
}

static Terrain s_rTer;
static int s_rTerInit, s_rTerOk;

static Terrain *rt_terrain(void)
{
    char path[E_PATH];
    if (!s_rTerInit) {
        data_path(path, (int)sizeof(path), "island/jet.isl");
        s_rTerOk = terrain_load(&s_rTer, path);
        s_rTerInit = 1;
        log_printf(LOG_INFO, "test: replay terrain loaded=%d path=%s", s_rTerOk, path);
    }
    return &s_rTer;
}

/* scripted controls of a busy flight (seconds): tap A, takeoff + climb-out, rolls and pulls after the handover,
   afterburner, a long push into the sea (crash, wreck, runway restart), a new takeoff roll, an air restart */
static void det_keys(u32 tick, PilotKeys *k, int *cmd)
{
    float t = (float)(tick - 1) / (float)SIM_HZ;
    memset(k, 0, sizeof(*k));
    *cmd = 0;
    k->fast = (t >= 1.0f && t < 1.2f) || (t >= 40.0f && t < 46.0f) || (t >= 78.0f && t < 78.2f);
    k->right = (t >= 34.0f && t < 34.4f) || (t >= 47.0f && t < 47.3f);
    k->up = (t >= 35.0f && t < 37.0f) || (t >= 42.0f && t < 44.5f);
    k->left = (t >= 38.0f && t < 38.6f);
    k->down = (t >= 50.0f && t < 75.0f);
    k->slow = (t >= 46.0f && t < 49.0f);
    if (tick == (u32)(85 * SIM_HZ) + 1u) *cmd = SES_CMD_AIR;
}

static int run_session(Game *g, u32 ticks, u32 *hashes, int maxH, int record, const char *path, int paramAt,
                       int playback, ReplayStats *st)
{
    PilotKeys k;
    ReplaySetup rs;
    int cmd, nh = 0;
    u32 t;
    memset(&rs, 0, sizeof(rs));
    rs.mode = MODE_FREE; rs.start = START_RUNWAY; rs.seed = 777;
    if (playback) {
        if (!replay_play_open(path, &rs, 0)) return -1;
    }
    session_start(g, rt_terrain(), rs.mode, rs.diff, rs.start, rs.seed);
    if (record && !replay_record_open(path, &rs, 0)) return -1;
    for (t = 0; t < ticks; t++) {
        if (playback) {
            if (!replay_play_tick(g->tick, &k, &cmd, g)) break;
        } else {
            det_keys(g->tick, &k, &cmd);
            if (paramAt && g->tick == (u32)paramAt) {       /* live changes as after F5 / F6 */
                params_restore("flight.turnBoost=1.25\n", 22);
                if (record) replay_record_params(g->tick);
            }
            if (paramAt && g->tick == (u32)paramAt + 600u) {
                pilot_set_mode(PILOT_NOVICE);
                if (record) replay_record_params(g->tick);
            }
            replay_quantise_keys(&k);
            if (record) replay_record_tick(g->tick, &k, cmd, g);
        }
        if ((g->tick % REPLAY_HASH_EVERY) == 0 && hashes && nh < maxH) hashes[nh++] = replay_state_hash(g);
        session_tick(g, &k, cmd);
    }
    if (record) { replay_record_close(g); if (st) replay_stats(st); }
    if (playback) { replay_play_tick(g->tick, &k, &cmd, g); replay_play_close(); if (st) replay_stats(st); }
    return nh;
}

int test_replay(const char *arg)
{
    static Game g;
    static char snap[RPL_SNAP_MAX];
    ReplaySetup rs, rs2;
    ReplayStats st;
    PilotKeys k, got;
    char path[E_PATH];
    u32 t, bad = 0, recBytes;
    int cmd, gcmd, sn;
    float save = params_dev_test_value();
    const Param *pd;
    (void)arg;
    t_begin("replay");
    if (!params_count()) params_register_all();
    sn = params_snapshot(snap, (int)sizeof(snap));
    pd = params_find("dev", "testValue");
    base_path(path, (int)sizeof(path), "test_replay.tmp");

    /* 1. format round trip (synthetic sim) */
    memset(&g, 0, sizeof(g));
    g.numAircraft = 1;
    g.ac[0].fs.V = 200.0f;
    memset(&rs, 0, sizeof(rs));
    rs.mode = MODE_FREE; rs.seed = 1234; rs.start = START_AIR;
    t_check(replay_record_open(path, &rs, 0), "record_open", "path=%s", path);
    for (t = 0; t < 1000; t++) {
        if (t == 500 && pd) { *(float *)pd->ptr = 7.5f; replay_record_params(t); }
        tr_keys(t, &k, &cmd);
        replay_quantise_keys(&k);
        replay_record_tick(t, &k, cmd, &g);
        tr_step(&g, &k, cmd);
    }
    replay_record_close(&g);
    replay_stats(&st);
    recBytes = st.bytes - st.snapBytes;             /* T06 note: the params snapshots grow with every module */
    t_check(st.ticks == 1000 && st.hashes == 10 && st.paramRecords == 1, "record_counts", "ticks=%u hashes=%u params=%u bytes=%u",
            st.ticks, st.hashes, st.paramRecords, st.bytes);
    t_check(recBytes < 1000u * 13u / 4u, "repeat_compression", "bytes=%u snapshots=%u records=%u", st.bytes, st.snapBytes,
            recBytes);
    if (pd) *(float *)pd->ptr = 1.0f;
    memset(&g, 0, sizeof(g));
    g.numAircraft = 1;
    g.ac[0].fs.V = 200.0f;
    t_check(replay_play_open(path, &rs2, 0) && rs2.seed == 1234 && rs2.start == START_AIR, "play_open_setup", NULL);
    for (t = 0; t < 1000; t++) {
        tr_keys(t, &k, &cmd);
        replay_quantise_keys(&k);
        if (!replay_play_tick(t, &got, &gcmd, &g)) { bad++; break; }
        if (memcmp(&k, &got, sizeof(k)) || cmd != gcmd) bad++;
        if (t == 600 && pd && *(float *)pd->ptr != 7.5f) bad += 1000;
        tr_step(&g, &got, gcmd);
    }
    t_check(bad == 0, "keys_cmds_params_match", "bad=%u", bad);
    t_check(!replay_play_tick(t, &got, &gcmd, &g), "end_after_last_tick", NULL);
    replay_play_close();
    replay_stats(&st);
    t_check(st.diffs == 0 && st.hashes == 10, "hashes_ok", "hashes=%u diffs=%u", st.hashes, st.diffs);
    if (pd) *(float *)pd->ptr = save;

    /* 2. a real 60 s session (takeoff, manoeuvres, two live param changes) recorded and played back */
    if (sn > 0) params_restore(snap, sn);
    pilot_set_mode(PILOT_EXPERT);
    {
        static u32 hRec[80], hPlay[80];
        ReplayStats a, b;
        int na, nb, i, same = 1;
        na = run_session(&g, 60 * SIM_HZ, hRec, 80, 1, path, 45 * SIM_HZ + 1, 0, &a);
        t_check(na == 60 && a.paramRecords == 2 && a.ticks == 60u * SIM_HZ, "session_record", "hashes=%d params=%u ticks=%u bytes=%u "
                "records=%u alive=%d alt_ft=%.0f", na, a.paramRecords, a.ticks, a.bytes, a.bytes - a.snapBytes,
                (g.ac[0].flags & AC_ALIVE) ? 1 : 0, (double)(g.ac[0].fs.pos.y * M2FT));
        t_check(g.ac[0].fs.pos.y > 300.0f && !g.ac[0].fs.onGround, "session_flew", "alt_m=%.0f V=%.0fkt", (double)g.ac[0].fs.pos.y,
                (double)(g.ac[0].fs.V * MS2KT));
        if (sn > 0) params_restore(snap, sn);       /* playback must restore everything itself */
        pilot_set_mode(PILOT_EXPERT);
        nb = run_session(&g, 70 * SIM_HZ, hPlay, 80, 0, path, 0, 1, &b);
        for (i = 0; i < na && i < nb; i++) if (hRec[i] != hPlay[i]) same = 0;
        t_check(nb == na && b.diffs == 0 && b.hashes == (u32)na + 1u && b.paramRecords == 2 && same, "session_playback_hash_ok",
                "hashes=%u diffs=%u first_diff=%u params=%u ticks=%u", b.hashes, b.diffs, b.firstDiffTick, b.paramRecords, b.ticks);
        /* negative check: a different flight param at the start must show up in the hashes */
        {
            ReplayStats c;
            ReplaySetup rs3;
            PilotKeys kk;
            int cc;
            if (replay_play_open(path, &rs3, 0)) {
                params_restore("flight.turnBoost=0.6\n", 21);
                session_start(&g, rt_terrain(), rs3.mode, rs3.diff, rs3.start, rs3.seed);
                while (replay_play_tick(g.tick, &kk, &cc, &g)) session_tick(&g, &kk, cc);
                replay_play_close();
                replay_stats(&c);
                t_check(c.diffs > 0, "hash_detects_divergence", "diffs=%u first_diff=%u", c.diffs, c.firstDiffTick);
            } else t_check(0, "hash_detects_divergence", "open failed");
        }
    }
    remove(path);
    if (sn > 0) params_restore(snap, sn);
    return t_end();
}

int test_determinism(const char *arg)
{
    static Game g;
    static u32 h1[100], h2[100];
    static char snap[RPL_SNAP_MAX];
    int n1, n2, i, same = 1, firstBad = -1, sn, resp1, alive1;
    float x1;
    (void)arg;
    t_begin("determinism");
    if (!params_count()) params_register_all();
    sn = params_snapshot(snap, (int)sizeof(snap));
    pilot_set_mode(PILOT_EXPERT);
    n1 = run_session(&g, 90 * SIM_HZ, h1, 100, 0, NULL, 0, 0, NULL);
    resp1 = session_respawns();
    alive1 = (g.ac[0].flags & AC_ALIVE) != 0;
    x1 = g.ac[0].fs.pos.x;
    n2 = run_session(&g, 90 * SIM_HZ, h2, 100, 0, NULL, 0, 0, NULL);
    for (i = 0; i < n1 && i < n2; i++) if (h1[i] != h2[i]) { same = 0; if (firstBad < 0) firstBad = i; }
    t_check(n1 == 90 && n2 == 90 && same, "two_runs_same_hashes", "hashes=%d/%d first_bad_s=%d", n1, n2, firstBad);
    t_check(resp1 >= 2 && session_respawns() == resp1 && alive1, "crash_restart_air_paths", "respawns=%d alive=%d",
            resp1, alive1);
    t_check(g.ac[0].fs.pos.x == x1, "final_state_equal", "x=%.3f/%.3f", (double)x1, (double)g.ac[0].fs.pos.x);
    if (sn > 0) params_restore(snap, sn);
    return t_end();
}
