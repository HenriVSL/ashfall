/* tests_target.cpp - -test target (DESIGN 11.3, 11.4, 5.3, 16.1). Portable. Owner: T11.
   S short select order by angle off the nose, the frozen cycle (taps < 2 s apart) and its rebuild, S long
   designate / break lock / designate with no selection, the select range, lock gimbal loss (lock MEMORY 1 s after
   the last good scan, designation dropped 3 s later), re-lock from lock MEMORY, a lock held outside the scan volume,
   kills with autoReselect / autoRedesignate, the display kick, and the S key end to end through pilot_update's
   short/long classifier. Same straight-line harness as tests_radar.cpp; code defaults of every param. */
#include "tests.h"
#include "params.h"
#include "radar.h"
#include "target.h"
#include "e_terrain.h"
#include "e_file.h"
#include "e_log.h"

static Terrain s_ter;
static int s_terInit, s_terOk;
static Game s_g;
static int s_move[MAX_AIRCRAFT];
static PilotInput s_in;

static void terrain_once(void)
{
    char path[E_PATH];
    if (s_terInit) return;
    data_path(path, (int)sizeof(path), "island/jet.isl");
    s_terOk = terrain_load(&s_ter, path);
    s_terInit = 1;
    log_printf(LOG_INFO, "test: target terrain loaded=%d path=%s", s_terOk, path);
}

static void g_start(int n)
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
    s_g.ac[0].flags |= AC_PLAYER;
    radar_reset(&s_g);
    target_reset(&s_g);
    memset(&s_in, 0, sizeof(s_in));
}

static void place(int i, Vec3 pos, float hdgDeg, float Vms)
{
    Aircraft *a = &s_g.ac[i];
    flight_init(&a->fs, pos, E_DEG2RAD(hdgDeg), Vms);
    a->fs.alpha = 0.0f;
    a->prev = a->fs;
}

static void set_heading(int i, float hdgDeg)
{
    flight_set_path(&s_g.ac[i].fs, E_DEG2RAD(hdgDeg), 0.0f, 0.0f);
    s_g.ac[i].fs.alpha = 0.0f;
}

static Vec3 rel_pos(int i, float R, float azDeg)
{
    const FlightState *fs = &s_g.ac[i].fs;
    float h = fs->heading + E_DEG2RAD(azDeg);
    return v3(fs->pos.x + R * (float)sin(h), fs->pos.y, fs->pos.z + R * (float)cos(h));
}

static Vec3 sea_point(float y)
{
    Vec3 p = terrain_base(&s_ter)->airSpawn[0];
    p.y = y;
    return p;
}

/* one session-order tick: target_update (step 2) with s_in, move, radar_tick (step 8), tick++; s_in is cleared */
static void g_tick(void)
{
    int i;
    target_update(&s_g, &s_in);
    memset(&s_in, 0, sizeof(s_in));
    for (i = 0; i < s_g.numAircraft; i++) {
        Aircraft *a = &s_g.ac[i];
        a->prev = a->fs;
        if (s_move[i] && (a->flags & AC_ALIVE))
            a->fs.pos = v3_madd(a->fs.pos, q_rotate(a->fs.qPath, v3(0.0f, 0.0f, 1.0f)), a->fs.V * SIM_DT);
    }
    radar_tick(&s_g);
    s_g.tick++;
}

static void g_ticks(int n) { while (n-- > 0) g_tick(); }
static void tap(void) { s_in.tgtShort = 1; g_tick(); }
static void hold(void) { s_in.tgtLong = 1; g_tick(); }

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

/* player at sea heading north + 4 enemies at 10 NM, azimuth +30 (id 1), +10 (2), -20 (3), +45 (4); all detected */
static void four(void)
{
    g_start(5);
    place(0, sea_point(3000.0f), 0.0f, 250.0f);
    place(1, rel_pos(0, 10.0f * NM2M, 30.0f), 180.0f, 250.0f);
    place(2, rel_pos(0, 10.0f * NM2M, 10.0f), 180.0f, 250.0f);
    place(3, rel_pos(0, 10.0f * NM2M, -20.0f), 180.0f, 250.0f);
    place(4, rel_pos(0, 10.0f * NM2M, 45.0f), 180.0f, 250.0f);
    g_ticks(13);
}

/* ---------------------------------------------------------------- select order + frozen cycle */
static void t_select(void)
{
    int seq[6], i, ids[8], n, ok;
    ContactView cv;
    four();
    tap();
    t_check(target_selected() == 2, "select_nearest_nose", "sel=%d", target_selected());
    t_check(radar_view_get(&s_g, 0, 2, &cv) && (cv.flags & CVF_SELECTED), "select_flag_shown_at_once", "flags=%x", cv.flags);
    for (i = 0; i < 4; i++) { g_ticks(60); tap(); seq[i] = target_selected(); }
    t_check(seq[0] == 3 && seq[1] == 1 && seq[2] == 4 && seq[3] == 2, "select_cycle_by_angle",
            "order after 2: %d %d %d %d (want 3 1 4 2)", seq[0], seq[1], seq[2], seq[3]);
    n = target_cycle(ids, 8);
    t_check(n == 4 && ids[0] == 2 && ids[1] == 3 && ids[2] == 1 && ids[3] == 4, "select_cycle_list", "n=%d", n);
    /* frozen: turn 25 deg right (new angles: 1 5, 2 15, 4 20, 3 45); taps < 2 s apart keep the old order */
    g_ticks(60);
    set_heading(0, 25.0f);
    for (i = 0; i < 3; i++) { g_ticks(60); tap(); seq[i] = target_selected(); }
    t_check(seq[0] == 3 && seq[1] == 1 && seq[2] == 4, "select_cycle_frozen", "got %d %d %d (want 3 1 4)", seq[0], seq[1], seq[2]);
    /* a tap after 2.5 s rebuilds by the new angles (1, 2, 4, 3): next after 4 is 3 */
    g_ticks(300);
    tap();
    n = target_cycle(ids, 8);
    ok = n == 4 && ids[0] == 1 && ids[1] == 2 && ids[2] == 4 && ids[3] == 3;
    t_check(ok && target_selected() == 3, "select_rebuild_after_2s", "sel=%d list=%d,%d,%d,%d", target_selected(),
            ids[0], ids[1], ids[2], ids[3]);
    /* a dead contact is skipped in the frozen list */
    s_g.ac[1].flags &= ~AC_ALIVE;
    g_ticks(2);
    tap();
    t_check(target_selected() == 2, "select_skip_dead", "sel=%d (want 2: 1 is dead)", target_selected());
    /* no contacts: nothing selected */
    g_start(2);
    place(0, sea_point(3000.0f), 0.0f, 250.0f);
    place(1, rel_pos(0, 10.0f * NM2M, 170.0f), 0.0f, 250.0f);   /* behind: never detected */
    g_ticks(13);
    tap();
    t_check(target_selected() == -1, "select_none_without_contacts", NULL);
}

/* ---------------------------------------------------------------- select range (40 NM) */
static void t_range(void)
{
    char snap[64];
    e_snprintf(snap, (int)sizeof(snap), "radar.playerDetRangeNm=60\n");   /* T21a: the player's own radar key */
    params_restore(snap, (int)strlen(snap));
    g_start(3);
    place(0, sea_point(3000.0f), 0.0f, 250.0f);
    place(1, rel_pos(0, 45.0f * NM2M, 0.0f), 180.0f, 250.0f);
    place(2, rel_pos(0, 39.0f * NM2M, 40.0f), 180.0f, 250.0f);
    g_ticks(13);
    tap();
    t_check(radar_contact_state(0, 1) != CS_NONE && target_selected() == 2, "select_range_40nm",
            "45 NM state=%s sel=%d (want 2 at 39 NM)", radar_state_name(radar_contact_state(0, 1)), target_selected());
    e_snprintf(snap, (int)sizeof(snap), "radar.playerDetRangeNm=40\n");
    params_restore(snap, (int)strlen(snap));
}

/* ---------------------------------------------------------------- designate / break / designate without selection */
static void t_designate(void)
{
    u32 seq0;
    ContactView cv;
    four();
    tap();                                          /* 2 */
    seq0 = radar_event_seq();
    hold();
    t_check(radar_designated(0) == 2 && radar_lock(0) == LOCK_TRACK && ev_tick(seq0, REV_DESIGNATE, 0, 2, NULL) &&
            ev_tick(seq0, REV_LOCK, 0, 2, NULL), "designate_selected", "des=%d lock=%d", radar_designated(0), radar_lock(0));
    t_check(radar_view_get(&s_g, 0, 2, &cv) && (cv.flags & CVF_DESIGNATED) && (cv.flags & CVF_LOCKED),
            "designate_box_shown_at_once", "flags=%x", cv.flags);
    tap();                                          /* selection moves on (3), designation stays */
    t_check(target_selected() == 3 && radar_designated(0) == 2, "designate_stays_while_cycling", "sel=%d des=%d",
            target_selected(), radar_designated(0));
    hold();                                         /* S long on another contact moves the lock */
    t_check(radar_designated(0) == 3 && radar_lock(0) == LOCK_TRACK, "designate_moves", "des=%d", radar_designated(0));
    hold();                                         /* S long on the designated one: break lock, selection stays */
    t_check(radar_designated(0) == -1 && radar_lock(0) == LOCK_NONE && target_selected() == 3, "designate_break_lock",
            "des=%d sel=%d", radar_designated(0), target_selected());
    target_reset(&s_g);
    g_tick();
    hold();                                         /* no selection: nearest the nose, designated */
    t_check(target_selected() == 2 && radar_designated(0) == 2 && radar_lock(0) == LOCK_TRACK,
            "designate_without_selection", "sel=%d des=%d", target_selected(), radar_designated(0));
}

/* ---------------------------------------------------------------- lock gimbal loss, re-lock, lock outside the volume */
static void t_lock(void)
{
    u32 seq0, tOut, tMem, tDrop, lastOk = 0;
    int i, arg = -1;
    ContactView cv;
    four();
    tap(); tap();                                   /* 2, then 3 (-20 deg) */
    g_ticks(60); tap();                             /* 1 (+30 deg) */
    hold();
    t_check(radar_designated(0) == 1 && radar_lock(0) == LOCK_TRACK, "lock_setup", "des=%d", radar_designated(0));
    /* turn 50 deg left: contact 1 at 80 deg off the nose, beyond the 70 deg gimbal (and the 60 deg volume) */
    seq0 = radar_event_seq();
    g_ticks(12);
    set_heading(0, -50.0f);
    tOut = s_g.tick;
    for (i = 0; i < 120 * 6; i++) g_tick();
    tMem = ev_tick(seq0, REV_LOCK_MEMORY, 0, 1, NULL);
    tDrop = ev_tick(seq0, REV_UNDESIGNATE, 0, 1, &arg);
    {   /* last good lock scan = the pair's last scan slot before tOut (pair 0,1 slot: (tick + 7) % 12 == 0) */
        u32 t;
        for (t = tOut - 1; t + 12 >= tOut; t--) if (((t + 7u) % 12u) == 0) { lastOk = t; break; }
    }
    t_check(tMem && tMem - lastOk == 120 && tMem - tOut <= 120 && tMem - tOut > 108, "lock_gimbal_loss_memory_1s",
            "lastOk=%u out=%u memory=%u dt=%d", lastOk, tOut, tMem, (int)(tMem - tOut));
    t_check(tDrop && tDrop - tMem == 360 && arg == 1 && radar_designated(0) == -1, "lock_memory_3s_then_drop",
            "drop=%u dt=%d arg=%d", tDrop, (int)(tDrop - tMem), arg);
    /* re-lock: designate 1 again (it is in MEMORY / NONE now, out of the gimbal -> LOCK_MEMORY), look back in time */
    set_heading(0, 0.0f);
    g_ticks(13);
    target_reset(&s_g);
    tap(); tap(); g_ticks(60); tap();               /* 2, 3, 1 */
    hold();
    set_heading(0, -50.0f);
    g_ticks(150);                                   /* lock MEMORY after 1 s */
    t_check(radar_lock(0) == LOCK_MEMORY && radar_view_get(&s_g, 0, 1, &cv) && (cv.flags & CVF_LOCKMEM),
            "lock_memory_dashed", "lock=%d flags=%x", radar_lock(0), cv.flags);
    set_heading(0, 0.0f);
    g_ticks(13);
    t_check(radar_lock(0) == LOCK_TRACK && radar_designated(0) == 1, "lock_reacquired_from_memory", "lock=%d",
            radar_lock(0));
    /* a held lock keeps the track outside the scan volume: contact 4 at 65 deg (volume 60, gimbal 70) */
    target_reset(&s_g);
    radar_designate(&s_g, 0, -1);
    radar_designate(&s_g, 0, 4);
    set_heading(0, -20.0f);
    g_ticks(240);
    t_check(radar_lock(0) == LOCK_TRACK && radar_contact_state(0, 4) == CS_HOSTILE && radar_contact_state(0, 1) != CS_MEMORY,
            "lock_outside_volume_keeps_track", "lock=%d state4=%s", radar_lock(0), radar_state_name(radar_contact_state(0, 4)));
    {
        RadarGeom G;
        radar_geom(&s_g, 0, 4, 0, &G);
        t_check(!G.inVolume && G.inGimbal, "lock_outside_volume_geometry", "offNose=%.1f deg vol=%d gimbal=%d",
                (double)E_RAD2DEG(G.offNose), G.inVolume, G.inGimbal);
    }
}

/* ---------------------------------------------------------------- kills: autoReselect / autoRedesignate */
static void t_kill(void)
{
    char snap[64];
    four();
    hold();                                         /* designate 2 (nearest the nose) */
    s_g.ac[2].flags &= ~AC_ALIVE;
    g_ticks(3);
    t_check(radar_designated(0) == -1 && target_selected() == 3, "kill_clears_and_reselects", "des=%d sel=%d (want -1, 3)",
            radar_designated(0), target_selected());
    e_snprintf(snap, (int)sizeof(snap), "radar.autoRedesignate=1\n");
    params_restore(snap, (int)strlen(snap));
    hold();                                         /* designate 3 */
    s_g.ac[3].flags &= ~AC_ALIVE;
    g_ticks(3);
    t_check(target_selected() == 1 && radar_designated(0) == 1 && radar_lock(0) == LOCK_TRACK, "kill_autoredesignate",
            "sel=%d des=%d", target_selected(), radar_designated(0));
    e_snprintf(snap, (int)sizeof(snap), "radar.autoRedesignate=0\nradar.autoReselect=0\n");
    params_restore(snap, (int)strlen(snap));
    s_g.ac[1].flags &= ~AC_ALIVE;
    g_ticks(3);
    t_check(target_selected() == -1 && radar_designated(0) == -1, "kill_no_autoreselect", "sel=%d", target_selected());
    e_snprintf(snap, (int)sizeof(snap), "radar.autoReselect=1\n");
    params_restore(snap, (int)strlen(snap));
    /* player dead: everything cleared */
    four();
    hold();
    s_g.ac[0].flags &= ~AC_ALIVE;
    g_ticks(2);
    t_check(target_selected() == -1 && radar_designated(0) == -1 && radar_contact_state(0, 2) == CS_NONE,
            "player_dead_clears", NULL);
}

/* ---------------------------------------------------------------- S end to end through pilot_update */
static void t_keys(void)
{
    PilotState ps;
    PilotKeys k;
    PressCounter pc;
    int i, shorts = 0, longs = 0, longAt = -1;
    four();
    pilot_reset(&ps);
    memset(&k, 0, sizeof(k));
    memset(&pc, 0, sizeof(pc));
    /* tap: 5 ticks down */
    for (i = 0; i < 12; i++) {
        press_counter_feed(&pc, i < 5, &k.tgtHeld, &k.tgtReleased);
        pilot_update(&ps, &k, &s_in);
        shorts += s_in.tgtShort;
        longs += s_in.tgtLong;
        g_tick();
    }
    t_check(shorts == 1 && longs == 0 && target_selected() == 2, "keys_tap_selects", "short=%d long=%d sel=%d", shorts,
            longs, target_selected());
    /* hold 60 ticks: long at the 36th held tick, no short on release */
    shorts = longs = 0;
    for (i = 0; i < 80; i++) {
        press_counter_feed(&pc, i < 60, &k.tgtHeld, &k.tgtReleased);
        pilot_update(&ps, &k, &s_in);
        shorts += s_in.tgtShort;
        if (s_in.tgtLong) { longs++; longAt = i; }
        g_tick();
    }
    t_check(shorts == 0 && longs == 1 && longAt == 35 && radar_designated(0) == 2, "keys_hold_designates_at_0p3s",
            "short=%d long=%d at held tick %d des=%d", shorts, longs, longAt + 1, radar_designated(0));
}

int test_target(const char *arg)
{
    static char snap[65536];
    int n, fails;
    Cfg *empty;
    (void)arg;
    t_begin("target");
    n = params_snapshot(snap, (int)sizeof(snap));
    empty = cfg_parse("", 0);
    params_load(empty);
    cfg_free(empty);
    terrain_once();
    t_select();
    t_range();
    t_designate();
    t_lock();
    t_kill();
    t_keys();
    if (n > 0) params_restore(snap, n);
    fails = t_end();
    return fails;
}
