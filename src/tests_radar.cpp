/* tests_radar.cpp - -test radar (DESIGN 11.1-11.3, 11.5, 10.7.4, 16.1). Portable. Owner: T11.
   Scan volume and aspect ranges, terrain masking behind the volcano of the real island (MEMORY 0.5 s after the last
   seen scan, NONE 6 s later), identification timing (1.5 s inside 15 NM, instant inside 5 NM, never beyond 15 NM),
   MEMORY extrapolation and reacquire with the ID kept, dead contacts (SPLASH time, designation cleared), the
   display latch (fast sim changes shown >= 0.3 s apart, random flicker stress), RWR locks + MISSILE rule, and the
   radar tick cost for 6 aircraft over the island. Aircraft are moved here in straight lines (no flight model), so
   the geometry is exact. Code defaults of every param (snapshot, empty ini, restore). */
#include "tests.h"
#include "params.h"
#include "radar.h"
#include "target.h"
#include "e_terrain.h"
#include "e_file.h"
#include "e_log.h"
#include "e_prof.h"

static Terrain s_ter;
static int s_terInit, s_terOk;
static Game s_g;
static int s_move[MAX_AIRCRAFT];
static int s_enemyObs;                              /* T21a: 1 = observer 0 without AC_PLAYER (the enemy radar's ranges) */

static void terrain_once(void)
{
    char path[E_PATH];
    if (s_terInit) return;
    data_path(path, (int)sizeof(path), "island/jet.isl");
    s_terOk = terrain_load(&s_ter, path);
    s_terInit = 1;
    log_printf(LOG_INFO, "test: radar terrain loaded=%d path=%s", s_terOk, path);
}

/* player (id 0, blue) + n-1 enemies (red), all parked until placed */
static void r_start(int n)
{
    int i;
    terrain_once();
    memset(&s_g, 0, sizeof(s_g));
    s_g.terrain = &s_ter;
    s_g.numAircraft = n;
    s_g.tick = 1;
    for (i = 0; i < MAX_AIRCRAFT; i++) {
        s_g.ac[i].id = i;
        s_g.ac[i].side = i ? SIDE_RED : SIDE_BLUE;
        s_g.ac[i].flags = i < n ? AC_ALIVE : 0;
        s_g.ac[i].hp = 100.0f;
        s_move[i] = 0;
    }
    if (!s_enemyObs) s_g.ac[0].flags |= AC_PLAYER;
    radar_reset(&s_g);
    target_reset(&s_g);
}

static void place(int i, Vec3 pos, float hdgDeg, float Vms, int move)
{
    Aircraft *a = &s_g.ac[i];
    flight_init(&a->fs, pos, E_DEG2RAD(hdgDeg), Vms);
    a->fs.alpha = 0.0f;
    a->prev = a->fs;
    s_move[i] = move;
}

static void set_heading(int i, float hdgDeg)
{
    flight_set_path(&s_g.ac[i].fs, E_DEG2RAD(hdgDeg), 0.0f, 0.0f);
    s_g.ac[i].fs.alpha = 0.0f;
}

/* position at range R from aircraft i, body azimuth az (deg, + right) / elevation el (deg, + up), level heading */
static Vec3 rel_pos(int i, float R, float azDeg, float elDeg)
{
    const FlightState *fs = &s_g.ac[i].fs;
    float h = fs->heading + E_DEG2RAD(azDeg), e = E_DEG2RAD(elDeg);
    return v3(fs->pos.x + R * (float)sin(h) * (float)cos(e), fs->pos.y + R * (float)sin(e),
              fs->pos.z + R * (float)cos(h) * (float)cos(e));
}

/* one tick: move (straight lines), radar, tick++ */
static void r_tick(void)
{
    int i;
    for (i = 0; i < s_g.numAircraft; i++) {
        Aircraft *a = &s_g.ac[i];
        a->prev = a->fs;
        if (s_move[i] && (a->flags & AC_ALIVE))
            a->fs.pos = v3_madd(a->fs.pos, q_rotate(a->fs.qPath, v3(0.0f, 0.0f, 1.0f)), a->fs.V * SIM_DT);
    }
    radar_tick(&s_g);
    s_g.tick++;
}

static void r_ticks(int n) { while (n-- > 0) r_tick(); }

static Vec3 sea_point(float y)                      /* open sea: air spawn 0 (26 km off the island centre) */
{
    Vec3 p = terrain_base(&s_ter)->airSpawn[0];
    p.y = y;
    return p;
}

/* the first event of a type for (obs, id) at or after seq0; tick or 0 */
static u32 ev_tick(u32 seq0, int type, int obs, int id, int *arg)
{
    RadarEvent e;
    u32 s;
    for (s = seq0; s < radar_event_seq(); s++)
        if (radar_event_get(s, &e) && e.type == type && e.obs == obs && e.id == id) {
            if (arg) *arg = e.arg;
            return e.tick;
        }
    return 0;
}

/* ---------------------------------------------------------------- scan volume */
static void t_volume(void)
{
    static const float az[] = { 0.0f, 59.0f, 61.0f, -59.0f, -61.0f, 180.0f, 0.0f, 0.0f, 0.0f, 0.0f, 30.0f };
    static const float el[] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 44.0f, 46.0f, -44.0f, -46.0f, 40.0f };
    static const int want[] = { 1, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1 };
    int k, bad = 0, badState = 0;
    char det[160];
    det[0] = 0;
    for (k = 0; k < E_COUNT(az); k++) {
        RadarGeom G;
        int st;
        r_start(2);
        place(0, sea_point(3000.0f), 0.0f, 250.0f, 0);
        place(1, rel_pos(0, 2800.0f, az[k], el[k]), 90.0f, 250.0f, 0);
        radar_geom(&s_g, 0, 1, 1, &G);
        if (G.inVolume != want[k] || !G.inRange || !G.los) { bad++; e_snprintf(det, (int)sizeof(det), "az=%g el=%g vol=%d", (double)az[k], (double)el[k], G.inVolume); }
        r_ticks(13);
        st = radar_contact_state(0, 1);
        if ((st != CS_NONE) != want[k]) badState++;
    }
    t_check(bad == 0, "volume_geom", "bad=%d %s", bad, det);
    t_check(badState == 0, "volume_detect", "bad=%d (11 directions: +-59 in / +-61 out, el +-44 in / +-46 out, behind out)", badState);
}

/* ---------------------------------------------------------------- aspect ranges (head-on, beam, tail) */
/* T21a: player = the player's radar (playerDetRangeNm 40: 40/50/40 NM), else an enemy observer (detRangeNm 20) */
static void t_aspect(int player)
{
    static const float tgtHdg[] = { 180.0f, 90.0f, 0.0f };        /* observer heads north: target head-on, beam, tail */
    static const float baseNm[] = { 20.0f, 25.0f, 20.0f };
    static const char *const name[] = { "headon", "beam", "tail" };
    int k, s;
    s_enemyObs = !player;
    for (k = 0; k < 3; k++) {
        float nmIn = baseNm[k] * (player ? 2.0f : 1.0f) - 0.1f, nmOut = baseNm[k] * (player ? 2.0f : 1.0f) + 0.1f;
        int det[2];
        RadarGeom G;
        for (s = 0; s < 2; s++) {
            r_start(2);
            place(0, sea_point(3000.0f), 0.0f, 250.0f, 0);
            place(1, rel_pos(0, (s ? nmOut : nmIn) * NM2M, 0.0f, 0.0f), tgtHdg[k], 250.0f, 0);
            radar_geom(&s_g, 0, 1, 0, &G);
            r_ticks(13);
            det[s] = radar_contact_state(0, 1) != CS_NONE;
        }
        {
            char what[48];
            e_snprintf(what, (int)sizeof(what), "aspect_%s_%s_%gnm", player ? "player" : "enemy", name[k],
                       (double)(baseNm[k] * (player ? 2.0f : 1.0f)));
            t_check(det[0] && !det[1], what, "in(%.1f NM)=%d out(%.1f NM)=%d rdet=%.0f m", (double)nmIn, det[0],
                    (double)nmOut, det[1], (double)G.rdet);
        }
    }
    s_enemyObs = 0;
}

/* ---------------------------------------------------------------- terrain mask: behind the volcano */
static void t_volcano(void)
{
    int i, j, best = 0, n, uBest = -1;
    float hBest, sx, sz;
    u32 seq0, tBlock = 0, tMem = 0, tNone = 0, lastSeenBefore = 0, tIdent;
    if (!s_terOk) { log_printf(LOG_INFO, "test: radar volcano skipped (no island)"); return; }
    n = s_ter.gridN;
    for (j = 0; j < n; j++) for (i = 0; i < n; i++)  /* highest sample (integer compare: same pick on every FPU) */
        if ((int)s_ter.hght[j * n + i] > uBest) { uBest = (int)s_ter.hght[j * n + i]; best = j * n + i; }
    hBest = s_ter.hMin + (float)uBest * s_ter.hScale;
    sx = s_ter.x0 + (float)(best % n) * s_ter.cell;
    sz = s_ter.z0 + (float)(best / n) * s_ter.cell;
    r_start(2);
    place(0, v3(sx - 16000.0f, 1500.0f, sz), 90.0f, 250.0f, 0);     /* observer parked west of the summit, looking east */
    place(1, v3(sx + 8000.0f, 1500.0f, sz + 12000.0f), 180.0f, 600.0f, 1);   /* target runs south behind the cone */
    seq0 = radar_event_seq();
    for (i = 0; i < 120 * 60 && !tNone; i++) {
        u32 T;
        int clear, st;
        r_tick();
        T = s_g.tick - 1;                           /* the tick the radar just ran, with these positions */
        clear = terrain_los(&s_ter, &s_g.ac[0].fs.pos, &s_g.ac[1].fs.pos);
        st = radar_contact_state(0, 1);
        if (!tMem) {
            if (clear) tBlock = 0;
            else if (!tBlock && st != CS_NONE) tBlock = T;
            if (clear && ((T + 7u) % 12u) == 0 && st != CS_NONE) lastSeenBefore = T;   /* this pair's scan slot */
            if (st == CS_MEMORY) tMem = T;
        } else if (st == CS_NONE) tNone = T;
    }
    tIdent = ev_tick(seq0, REV_IDENT, 0, 1, NULL);
    log_printf(LOG_INFO, "test: radar volcano summit=%.0f,%.0f h=%.0f ident=%u block=%u lastSeen=%u memory=%u none=%u",
               (double)sx, (double)sz, (double)hBest, tIdent, tBlock, lastSeenBefore, tMem, tNone);
    t_check(tIdent > 0 && tBlock > tIdent, "volcano_ident_before_mask", "ident=%u block=%u", tIdent, tBlock);
    t_check(tMem > 0 && tMem - lastSeenBefore == 60, "volcano_memory_0p5s_after_last_seen", "dt=%d ticks",
            (int)(tMem - lastSeenBefore));
    t_check(tMem > tBlock && tMem - tBlock >= 48 && tMem - tBlock <= 60, "volcano_memory_0p4_0p5s_after_mask",
            "dt=%d ticks", (int)(tMem - tBlock));
    t_check(tNone > 0 && tNone - tMem == 720, "volcano_none_6s_after_memory", "dt=%d ticks", (int)(tNone - tMem));
}

/* ---------------------------------------------------------------- identification timing */
static u32 first_change(int want, int maxTicks)
{
    int i;
    for (i = 0; i < maxTicks; i++) {
        r_tick();
        if (radar_contact_state(0, 1) == want) return s_g.tick - 1;
    }
    return 0;
}

/* T21a: player = the player's radar (playerIdRangeNm 25), else an enemy observer (idRangeNm 15); same cases shifted */
static void t_ident(int player)
{
    u32 tDet, tId, seq0;
    float idNm = player ? 25.0f : 15.0f;
    char w[64];
    const char *who = player ? "player" : "enemy";
    s_enemyObs = !player;
    /* 10 NM static (player 20 NM): ID exactly 1.5 s after detection */
    r_start(2);
    place(0, sea_point(3000.0f), 0.0f, 250.0f, 0);
    place(1, rel_pos(0, (idNm - 5.0f) * NM2M, 5.0f, 0.0f), 270.0f, 250.0f, 0);
    seq0 = radar_event_seq();
    tDet = first_change(CS_DETECTED, 24);
    tId = first_change(CS_HOSTILE, 400);
    e_snprintf(w, (int)sizeof(w), "ident_%s_%gnm_1p5s", who, (double)(idNm - 5.0f));
    t_check(tDet && tId && tId - tDet == 180, w, "detect=%u hostile=%u dt=%d", tDet, tId, (int)(tId - tDet));
    e_snprintf(w, (int)sizeof(w), "ident_%s_event", who);
    t_check(ev_tick(seq0, REV_IDENT, 0, 1, NULL) == tId, w, NULL);
    /* 4.9 NM: hostile on the first scan */
    r_start(2);
    place(0, sea_point(3000.0f), 0.0f, 250.0f, 0);
    place(1, rel_pos(0, 4.9f * NM2M, -10.0f, 0.0f), 0.0f, 250.0f, 0);
    seq0 = radar_event_seq();
    r_ticks(13);
    tDet = ev_tick(seq0, REV_DETECT, 0, 1, NULL);
    tId = ev_tick(seq0, REV_IDENT, 0, 1, NULL);
    e_snprintf(w, (int)sizeof(w), "ident_%s_instant_5nm", who);
    t_check(tDet && tId == tDet && radar_contact_state(0, 1) == CS_HOSTILE, w, "detect=%u ident=%u", tDet, tId);
    /* 16 NM (player 26): never identified */
    r_start(2);
    place(0, sea_point(3000.0f), 0.0f, 250.0f, 0);
    place(1, rel_pos(0, (idNm + 1.0f) * NM2M, 0.0f, 0.0f), 90.0f, 250.0f, 0);
    r_ticks(120 * 6);
    e_snprintf(w, (int)sizeof(w), "ident_%s_none_beyond_%gnm", who, (double)idNm);
    t_check(radar_contact_state(0, 1) == CS_DETECTED && !radar_identified(0, 1), w, "state=%s",
            radar_state_name(radar_contact_state(0, 1)));
    /* closing from 15.3 NM (player 25.3) head-on at 300 m/s: ID 1.5 s (+ < 1 scan) after crossing 15 (25) NM */
    r_start(2);
    place(0, sea_point(3000.0f), 0.0f, 0.0f, 0);
    place(1, rel_pos(0, (idNm + 0.3f) * NM2M, 0.0f, 0.0f), 180.0f, 300.0f, 1);
    {
        int i;
        u32 tCross = 0;
        tId = 0;
        for (i = 0; i < 120 * 10 && !tId; i++) {
            float R;
            r_tick();
            R = v3_len(v3_sub(s_g.ac[1].fs.pos, s_g.ac[0].fs.pos));   /* positions of the tick just run */
            if (!tCross && R <= idNm * NM2M) tCross = s_g.tick - 1;
            if (radar_contact_state(0, 1) == CS_HOSTILE) tId = s_g.tick - 1;
        }
        e_snprintf(w, (int)sizeof(w), "ident_%s_closing_1p5s_after_%gnm", who, (double)idNm);
        t_check(tCross && tId && tId - tCross >= 180 && tId - tCross < 192, w,
                "cross=%u hostile=%u dt=%d", tCross, tId, (int)(tId - tCross));
    }
    s_enemyObs = 0;
}

/* ---------------------------------------------------------------- memory, extrapolation, reacquire */
static void t_memory(void)
{
    u32 tLost, tMem, tNone, seq0;
    Vec3 p, v, truth;
    int kind, i;
    r_start(2);
    place(0, sea_point(3000.0f), 0.0f, 0.0f, 0);
    place(1, rel_pos(0, 4.0f * NM2M, 20.0f, 0.0f), 300.0f, 250.0f, 1);
    r_ticks(24);
    t_check(radar_contact_state(0, 1) == CS_HOSTILE, "memory_setup_hostile", NULL);
    seq0 = radar_event_seq();
    set_heading(0, 180.0f);                         /* look away: out of the volume */
    tLost = s_g.tick;
    tMem = first_change(CS_MEMORY, 120);
    t_check(tMem && tMem - tLost <= 60 && tMem - tLost > 48, "memory_after_0p5s", "dt=%d ticks", (int)(tMem - tLost));
    r_ticks(240);                                   /* 2 s in memory: extrapolation follows the straight target */
    kind = radar_track(&s_g, 0, 1, &p, &v);
    truth = s_g.ac[1].fs.pos;
    t_check(kind == 2 && v3_len(v3_sub(p, truth)) < 3.0f, "memory_extrapolation", "kind=%d err=%.2f m", kind,
            (double)v3_len(v3_sub(p, truth)));
    {   /* the view's render position at alpha 1 = the last tick's position (continuity with the tracked lerp) */
        ContactView cv;
        Vec3 rp;
        int ok = radar_view_get(&s_g, 0, 1, &cv);
        radar_view_pos(&s_g, &cv, 1.0f, &rp);
        t_check(ok && cv.state == CS_MEMORY && (cv.flags & CVF_IDENTIFIED) && v3_len(v3_sub(rp, truth)) < 3.0f,
                "memory_view_pos", "state=%s err=%.2f m", radar_state_name(cv.state), (double)v3_len(v3_sub(rp, truth)));
    }
    set_heading(0, 0.0f);                           /* look back: previous state (HOSTILE, ID kept) */
    for (i = 0; i < 24; i++) r_tick();
    t_check(radar_contact_state(0, 1) == CS_HOSTILE && ev_tick(seq0, REV_REACQUIRE, 0, 1, NULL) > 0,
            "memory_reacquire_keeps_id", "state=%s", radar_state_name(radar_contact_state(0, 1)));
    set_heading(0, 180.0f);
    tMem = first_change(CS_MEMORY, 120);
    tNone = first_change(CS_NONE, 900);
    t_check(tMem && tNone && tNone - tMem == 720 && !radar_identified(0, 1), "memory_6s_then_none",
            "dt=%d ticks", (int)(tNone - tMem));
}

/* ---------------------------------------------------------------- dead contact */
static void t_dead(void)
{
    u32 seq0, tDead, tNone;
    int arg = -1, lock;
    ContactView cv;
    r_start(2);
    place(0, sea_point(3000.0f), 0.0f, 250.0f, 0);
    place(1, rel_pos(0, 6.0f * NM2M, 0.0f, 0.0f), 90.0f, 250.0f, 0);
    r_ticks(13);
    lock = radar_designate(&s_g, 0, 1);
    t_check(lock == LOCK_TRACK && radar_designated(0) == 1, "dead_setup_lock", "lock=%d", lock);
    seq0 = radar_event_seq();
    s_g.ac[1].flags &= ~AC_ALIVE;
    r_tick();
    tDead = ev_tick(seq0, REV_DEAD, 0, 1, NULL);
    t_check(radar_contact_state(0, 1) == CS_DEAD && tDead == s_g.tick - 1, "dead_next_tick", NULL);
    t_check(radar_designated(0) == -1 && ev_tick(seq0, REV_UNDESIGNATE, 0, 1, &arg) && arg == 2, "dead_undesignate",
            "arg=%d", arg);
    t_check(radar_view_get(&s_g, 0, 1, &cv) && cv.state == CS_DEAD, "dead_view_splash", NULL);
    tNone = first_change(CS_NONE, 400);
    t_check(tNone - tDead == 240 && !radar_view_get(&s_g, 0, 1, &cv), "dead_removed_after_2s", "dt=%d", (int)(tNone - tDead));
}

/* ---------------------------------------------------------------- display latch */
static void t_latch(void)
{
    u32 tDet = 0, tSim = 0, tDisp = 0;
    int i, prevDisp, changes = 0, minGap = 99999, simChanges = 0, prevSim;
    u32 lastChange = 0;
    ERand r;
    ContactView cv;
    /* (a) detected at 5.0x NM, hostile 1 scan later (inside 5 NM): shown 0.3 s after the DETECTED symbol */
    r_start(2);
    place(0, sea_point(3000.0f), 0.0f, 0.0f, 0);
    place(1, rel_pos(0, 5.0f * NM2M + 15.0f, 0.0f, 0.0f), 180.0f, 300.0f, 0);
    for (i = 0; i < 400 && !tDisp; i++) {
        r_tick();
        if (!tDet && radar_contact_state(0, 1) == CS_DETECTED) { tDet = s_g.tick - 1; s_move[1] = 1; }
        if (!tSim && radar_contact_state(0, 1) == CS_HOSTILE) tSim = s_g.tick - 1;
        if (radar_view_get(&s_g, 0, 1, &cv) && cv.state == CS_HOSTILE) tDisp = s_g.tick - 1;
    }
    t_check(tDet && tSim == tDet + 12 && tDisp == tDet + 36, "latch_fast_ident", "detect=%u simHostile=+%d shown=+%d",
            tDet, (int)(tSim - tDet), (int)(tDisp - tDet));
    /* (b) stress: the observer looks away / back at random intervals (1-60 ticks) for 60 s */
    r_start(2);
    place(0, sea_point(3000.0f), 0.0f, 0.0f, 0);
    place(1, rel_pos(0, 8.0f * NM2M, 10.0f, 0.0f), 90.0f, 200.0f, 1);
    rnd_seed(&r, 1234u);
    prevDisp = CS_NONE;
    prevSim = CS_NONE;
    {
        int left = 0, away = 0;
        for (i = 0; i < 120 * 60; i++) {
            int d;
            if (--left <= 0) { away = !away; set_heading(0, away ? 180.0f : 0.0f); left = 1 + (int)(rnd_f01(&r) * 60.0f); }
            r_tick();
            d = radar_view_get(&s_g, 0, 1, &cv) ? cv.state : CS_NONE;
            if (radar_contact_state(0, 1) != prevSim) { simChanges++; prevSim = radar_contact_state(0, 1); }
            if (d != prevDisp) {
                if (lastChange && prevDisp != CS_NONE && (int)(s_g.tick - lastChange) < minGap) minGap = (int)(s_g.tick - lastChange);
                lastChange = s_g.tick;
                changes++;
                prevDisp = d;
            }
        }
    }
    t_check(changes > 4 && minGap >= 36, "latch_stress_min_gap_0p3s", "display changes=%d sim changes=%d min gap=%d ticks",
            changes, simChanges, minGap);
    /* (c) flags: designate / undesignate at random 1-30 tick intervals without a kick (AI-style): displayed flags
       change at most every 0.3 s */
    r_start(2);
    place(0, sea_point(3000.0f), 0.0f, 0.0f, 0);
    place(1, rel_pos(0, 8.0f * NM2M, 10.0f, 0.0f), 90.0f, 0.0f, 0);
    r_ticks(13);
    {
        int left = 0, on = 0, prevF = -1, fChanges = 0, simF = 0, fGap = 99999;
        u32 lastF = 0;
        for (i = 0; i < 120 * 30; i++) {
            int f;
            if (--left <= 0) { on = !on; radar_designate(&s_g, 0, on ? 1 : -1); simF++; left = 1 + (int)(rnd_f01(&r) * 30.0f); }
            r_tick();
            f = radar_view_get(&s_g, 0, 1, &cv) ? (cv.flags & (CVF_DESIGNATED | CVF_LOCKED | CVF_LOCKMEM)) : 0;
            if (f != prevF) {
                if (lastF && (int)(s_g.tick - lastF) < fGap) fGap = (int)(s_g.tick - lastF);
                if (prevF >= 0) { lastF = s_g.tick; fChanges++; }
                prevF = f;
            }
        }
        t_check(fChanges > 4 && fChanges < simF && fGap >= 36, "latch_flags_min_gap_0p3s",
                "displayed flag changes=%d designation changes=%d min gap=%d ticks", fChanges, simF, fGap);
    }
}

/* ---------------------------------------------------------------- RWR */
static void t_rwr(void)
{
    u32 seq0;
    int lock, arg = -1;
    RwrView rv;
    r_start(3);
    place(0, sea_point(3000.0f), 0.0f, 250.0f, 0);
    place(1, rel_pos(0, 8.0f * NM2M, 0.0f, 0.0f), 180.0f, 250.0f, 0);     /* enemy straight ahead (north), facing us */
    place(2, rel_pos(0, 8.0f * NM2M, 90.0f, 0.0f), 0.0f, 250.0f, 0);      /* enemy east, looking north: we are off its gimbal */
    r_ticks(13);
    seq0 = radar_event_seq();
    lock = radar_designate(&s_g, 1, 0);
    t_check(lock == LOCK_TRACK && radar_locked_by(0) == 2u && ev_tick(seq0, REV_LOCK, 1, 0, NULL) > 0, "rwr_lock",
            "lock=%d mask=%x", lock, radar_locked_by(0));
    lock = radar_designate(&s_g, 2, 0);
    t_check(lock == LOCK_MEMORY && radar_locked_by(0) == 2u, "rwr_no_lock_outside_gimbal", "lock=%d", lock);
    radar_rwr(&s_g, 0, &rv);
    t_check(rv.nLocks == 1 && rv.lockId[0] == 1 && (float)fabs(rv.lockBearing[0]) < 0.01f &&
            (float)fabs(rv.lockRange[0] - 8.0f * NM2M) < 5.0f, "rwr_view", "n=%d brg=%.3f rng=%.0f", rv.nLocks,
            (double)rv.lockBearing[0], (double)rv.lockRange[0]);
    r_ticks(120 * 5);                               /* enemy 2's designation times out: lock memory 3 s */
    t_check(radar_designated(2) == -1 && ev_tick(seq0, REV_UNDESIGNATE, 2, 0, &arg) && arg == 1, "rwr_memory_timeout",
            "arg=%d", arg);
    radar_rwr_launch(&s_g, 1, 0, 0);
    t_check(ev_tick(seq0, REV_LAUNCH, 1, 0, &arg) > 0 && arg == 0, "rwr_launch_event", NULL);
    t_check(radar_rwr_missile_warn(0, 30000.0f, 30000.0f) && !radar_rwr_missile_warn(1, 8000.0f, 6000.0f) &&
            radar_rwr_missile_warn(1, 8000.0f, 4900.0f) && radar_rwr_missile_warn(1, 4000.0f, 4500.0f), "rwr_missile_rule", NULL);
    radar_designate(&s_g, 1, -1);
    t_check(radar_locked_by(0) == 0u, "rwr_unlock", NULL);
}

/* ---------------------------------------------------------------- cost */
static void t_timing(void)
{
    int i, n = 1200;
    s64 t0, t1, worst = 0, sum = 0;
    float cx = s_ter.loaded ? s_ter.centreX : 0.0f, cz = s_ter.loaded ? s_ter.centreZ : 0.0f;
    const RadarStats *st;
    u32 los0, scans0;
    r_start(6);
    place(0, v3(cx - 20000.0f, 2000.0f, cz), 90.0f, 250.0f, 1);
    for (i = 1; i < 6; i++)
        place(i, v3(cx + 4000.0f * (float)(i - 3), 1200.0f + 300.0f * (float)i, cz - 9000.0f + 3600.0f * (float)i),
              270.0f, 250.0f, 1);
    r_ticks(24);
    st = radar_stats();
    los0 = st->losCalls;
    scans0 = st->scans;
    for (i = 0; i < n; i++) {
        t0 = prof_now_us();
        r_tick();
        t1 = prof_now_us() - t0;
        sum += t1;
        if (t1 > worst) worst = t1;
    }
    log_printf(LOG_INFO, "test: radar timing 6 aircraft over the island: avg_us_per_tick=%.1f worst_us=%d scans=%u los=%u "
               "(%d ticks)", (double)sum / (double)n, (int)worst, st->scans - scans0, st->losCalls - los0, n);
    t_check((float)sum / (float)n <= 300.0f, "timing_tick_6ac_le_0p3ms", "avg_us=%.1f worst_us=%d",
            (double)sum / (double)n, (int)worst);
}

int test_radar(const char *arg)
{
    static char snap[65536];
    int n, fails;
    Cfg *empty;
    (void)arg;
    t_begin("radar");
    n = params_snapshot(snap, (int)sizeof(snap));
    empty = cfg_parse("", 0);
    params_load(empty);
    cfg_free(empty);
    terrain_once();
    t_volume();
    t_aspect(0);
    t_aspect(1);
    t_volcano();
    t_ident(0);
    t_ident(1);
    t_memory();
    t_dead();
    t_latch();
    t_rwr();
    t_timing();
    if (n > 0) params_restore(snap, n);
    fails = t_end();
    return fails;
}
