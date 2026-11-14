/* tests_pilot.cpp - -test pilot (DESIGN 5.2, 5.3, 16.1). Portable. Owner: T05.
   Axis ramps, reversal, release, invertPitch, joystick, throttle states, event edges, and the short/long press
   classifier with scripted key timelines (10 ms tap, 290 ms, 310 ms, 2 s hold, tap during focus loss, sub-poll
   tap). Runs with the code defaults of [pilot] (snapshot / empty ini / restore, as -test flight). */
#include "tests.h"
#include "params.h"
#include "e_log.h"

static float run_keys(PilotState *ps, PilotKeys *k, int ticks, PilotInput *out)
{
    int i;
    for (i = 0; i < ticks; i++) pilot_update(ps, k, out);
    return out->up;
}

/* ticks of holding the given keys (set by the caller) until the axis reaches target */
static int ticks_to(PilotState *ps, PilotKeys *k, int pitch, float target)
{
    PilotInput o;
    int i;
    for (i = 1; i <= 600; i++) {
        float a;
        pilot_update(ps, k, &o);
        a = pitch ? o.up : o.ur;
        if ((float)fabs(a - target) <= 1e-4f) return i;
    }
    return -1;
}

static void t_axes(void)
{
    PilotState ps;
    PilotKeys k;
    PilotInput o;
    int t1, t2, t;
    /* pitch: two stages (0 -> 0.35 at 3.0/s = 14 ticks, then to 1 at 1.8/s: full at ~57 ticks = 0.48 s) */
    pilot_reset(&ps);
    memset(&k, 0, sizeof(k));
    k.up = 1;
    t1 = ticks_to(&ps, &k, 1, 0.35f);
    t2 = ticks_to(&ps, &k, 1, 1.0f) + t1;
    t_check(t1 >= 13 && t1 <= 15, "pitch_stage1_0.35", "ticks=%d (want 14 = 0.117 s)", t1);
    t_check(t2 >= 56 && t2 <= 59, "pitch_full", "ticks=%d (want ~57 = 0.48 s)", t2);
    /* one tick of Up = 3.0/120 */
    pilot_reset(&ps);
    pilot_update(&ps, &k, &o);
    t_near(o.up, 3.0f / 120.0f, 1e-5f, "pitch_one_tick");
    /* release from full: 0 after 1/6 s (20 ticks) */
    pilot_reset(&ps);
    run_keys(&ps, &k, 120, &o);
    k.up = 0;
    t = ticks_to(&ps, &k, 1, 0.0f);
    t_check(t >= 19 && t <= 21, "pitch_release_full", "ticks=%d (want 20)", t);
    /* reversal: from full up, Down: back to 0 at pitchCross (15 ticks), then the stages downwards */
    k.up = 1;
    pilot_reset(&ps);
    run_keys(&ps, &k, 120, &o);
    k.up = 0;
    k.down = 1;
    t = ticks_to(&ps, &k, 1, 0.0f);
    t_check(t >= 14 && t <= 16, "pitch_cross_to_zero", "ticks=%d (want 15)", t);
    t = ticks_to(&ps, &k, 1, -0.35f);
    t_check(t >= 13 && t <= 15, "pitch_cross_then_fine", "ticks=%d (want 14)", t);
    /* both keys = no command (returns to 0) */
    k.up = 1;
    run_keys(&ps, &k, 120, &o);
    t_check(o.up == 0.0f, "pitch_both_keys_zero", "up=%.3f", (double)o.up);
    /* roll: one stage, full in 20 ticks (6/s), release in 12 ticks (10/s) */
    pilot_reset(&ps);
    memset(&k, 0, sizeof(k));
    k.right = 1;
    t = ticks_to(&ps, &k, 0, 1.0f);
    t_check(t >= 19 && t <= 21, "roll_full", "ticks=%d (want 20 = 0.17 s)", t);
    k.right = 0;
    t = ticks_to(&ps, &k, 0, 0.0f);
    t_check(t >= 11 && t <= 13, "roll_release", "ticks=%d (want 12)", t);
    k.left = 1;
    t = ticks_to(&ps, &k, 0, -1.0f);
    t_check(t >= 19 && t <= 21, "roll_left_full", "ticks=%d", t);
    k.left = 0;
    k.right = 1;
    t = ticks_to(&ps, &k, 0, 0.0f);
    t_check(t >= 9 && t <= 11, "roll_cross_to_zero", "ticks=%d (want 10)", t);
    /* the generic step: never overshoots */
    t_near(pilot_axis_step(0.99f, 1, 0.35f, 3.0f, 1.8f, 6.0f, 8.0f, 0.1f), 1.0f, 1e-6f, "axis_no_overshoot");
    t_near(pilot_axis_step(0.01f, 0, 0.35f, 3.0f, 1.8f, 6.0f, 8.0f, 0.1f), 0.0f, 1e-6f, "axis_release_no_overshoot");
    /* stage boundary inside one step: 0.3 + (0.05 at 3/s = 1/60 s) + rest at 1.8/s */
    t_near(pilot_axis_step(0.30f, 1, 0.35f, 3.0f, 1.8f, 6.0f, 8.0f, 0.1f), 0.35f + 1.8f * (0.1f - 0.05f / 3.0f), 1e-5f,
           "axis_stage_split");
}

static void t_invert_joy(void)
{
    PilotState ps;
    PilotKeys k;
    PilotInput o;
    const Param *p = params_find("pilot", "invertPitch");
    int *inv = p ? (int *)p->ptr : NULL, save;
    pilot_reset(&ps);
    memset(&k, 0, sizeof(k));
    k.up = 1;
    run_keys(&ps, &k, 10, &o);
    t_check(o.up > 0.0f, "default_up_arrow_nose_up", "up=%.3f", (double)o.up);
    if (inv) {
        save = *inv;
        *inv = 1;
        pilot_reset(&ps);
        run_keys(&ps, &k, 10, &o);
        t_check(o.up < 0.0f, "invert_pitch_up_arrow_nose_down", "up=%.3f", (double)o.up);
        *inv = save;
    }
    /* joystick: straight mapping with deadzone + expo, no ramps; keys take over inside the deadzone */
    pilot_reset(&ps);
    memset(&k, 0, sizeof(k));
    k.joyValid = 1;
    k.joyY = 1.0f;
    k.joyX = -1.0f;
    pilot_update(&ps, &k, &o);
    t_check(o.up == 1.0f && o.ur == -1.0f, "joy_full_one_tick", "up=%.3f ur=%.3f", (double)o.up, (double)o.ur);
    k.joyY = 0.5f;
    k.joyX = 0.03f;
    pilot_update(&ps, &k, &o);
    t_near(o.up, pilot_joy_shape(0.5f), 1e-6f, "joy_half_shaped");
    t_check(pilot_joy_shape(0.5f) < 0.5f && pilot_joy_shape(0.5f) > 0.3f, "joy_expo_range", "v=%.3f",
            (double)pilot_joy_shape(0.5f));
    t_check(o.ur > -1.0f && o.ur < 0.0f, "joy_deadzone_keyboard_continues", "ur=%.3f (returns at rollReturn)",
            (double)o.ur);
}

static void t_throttle_events(void)
{
    PilotState ps;
    PilotKeys k;
    PilotInput o;
    int i, fires = 0, flares = 0, views = 0, wpn = 0;
    pilot_reset(&ps);
    memset(&k, 0, sizeof(k));
    pilot_update(&ps, &k, &o);
    t_check(o.thr == THR_NORMAL, "thr_none_normal", NULL);
    k.fast = 1;
    pilot_update(&ps, &k, &o);
    t_check(o.thr == THR_FAST, "thr_a_fast", NULL);
    k.slow = 1;
    pilot_update(&ps, &k, &o);
    t_check(o.thr == THR_NORMAL, "thr_a_and_z_normal", NULL);
    k.fast = 0;
    pilot_update(&ps, &k, &o);
    t_check(o.thr == THR_SLOW, "thr_z_slow", NULL);
    memset(&k, 0, sizeof(k));
    k.fire = k.flare = k.view = k.wpnNext = 1;
    for (i = 0; i < 130; i++) {
        pilot_update(&ps, &k, &o);
        fires += o.fire;
        flares += o.flare;
        views += o.view;
        wpn += o.wpnNext;
    }
    t_check(fires == 1 && views == 1 && wpn == 1, "edges_once_per_press", "fire=%d view=%d wpn=%d", fires, views, wpn);
    t_check(flares == 3, "flare_repeat_500ms", "bursts=%d in 130 ticks (want 3: ticks 1, 61, 121)", flares);
    pilot_seed_axes(&ps, 0.4f, -0.2f);
    memset(&k, 0, sizeof(k));
    k.up = 1;
    pilot_update(&ps, &k, &o);
    t_check(o.up > 0.4f && o.ur > -0.2f && o.ur < 0.0f, "seed_axes_continuous", "up=%.3f ur=%.3f", (double)o.up,
            (double)o.ur);
}

/* S timeline through press_counter_feed + pilot_update: returns short/long counts and the tick of the long */
static void s_timeline(int holdTicks, int *nShort, int *nLong, int *longTick)
{
    PilotState ps;
    PilotKeys k;
    PilotInput o;
    PressCounter c;
    int i;
    pilot_reset(&ps);
    memset(&k, 0, sizeof(k));
    memset(&c, 0, sizeof(c));
    *nShort = *nLong = 0;
    *longTick = -1;
    for (i = 0; i < holdTicks + 60; i++) {
        press_counter_feed(&c, i < holdTicks, &k.tgtHeld, &k.tgtReleased);
        pilot_update(&ps, &k, &o);
        *nShort += o.tgtShort;
        if (o.tgtLong) { (*nLong)++; *longTick = i + 1; }
    }
}

static void t_press(void)
{
    PressClass pc;
    int s, l, lt, ev, i, evs = 0;
    int L = pilot_long_press_ticks();
    t_check(L == 36, "long_press_ticks", "ticks=%d (300 ms)", L);
    s_timeline(1, &s, &l, &lt);                     /* 10 ms = 1.2 ticks -> 1 poll held */
    t_check(s == 1 && l == 0, "tap_10ms_short", "short=%d long=%d", s, l);
    s_timeline(35, &s, &l, &lt);                    /* 290 ms */
    t_check(s == 1 && l == 0, "hold_290ms_short", "short=%d long=%d", s, l);
    s_timeline(37, &s, &l, &lt);                    /* 310 ms */
    t_check(s == 0 && l == 1 && lt == 36, "hold_310ms_long_at_36", "short=%d long=%d tick=%d", s, l, lt);
    s_timeline(240, &s, &l, &lt);                   /* 2 s */
    t_check(s == 0 && l == 1 && lt == 36, "hold_2s_one_long", "short=%d long=%d tick=%d", s, l, lt);
    /* sub-poll tap: e_input reports released = 1 with no held poll */
    press_reset(&pc);
    ev = press_classify(&pc, 0, 1, L);
    t_check(ev == PRESS_SHORT, "subpoll_tap_short", "ev=%d", ev);
    /* focus loss while held: the counter drops to 0 without a release -> no event, then nothing on refocus */
    press_reset(&pc);
    for (i = 1; i <= 20; i++) evs += press_classify(&pc, i, 0, L) != PRESS_NONE;
    for (i = 0; i < 30; i++) evs += press_classify(&pc, 0, 0, L) != PRESS_NONE;
    t_check(evs == 0 && pc.held == 0 && !pc.longFired, "focus_loss_no_event", "events=%d", evs);
    /* focus loss after a long fired: no short later either */
    press_reset(&pc);
    for (i = 1; i <= 40; i++) evs += press_classify(&pc, i, 0, L) == PRESS_LONG;
    press_classify(&pc, 0, 0, L);
    ev = press_classify(&pc, 0, 0, L);
    t_check(evs == 1 && ev == PRESS_NONE && !pc.longFired, "focus_loss_after_long", "longs=%d", evs);
}

int test_pilot(const char *arg)
{
    static char snap[65536];
    int n;
    Cfg *empty;
    (void)arg;
    t_begin("pilot");
    n = params_snapshot(snap, (int)sizeof(snap));
    empty = cfg_parse("", 0);
    params_load(empty);
    cfg_free(empty);
    t_axes();
    t_invert_joy();
    t_throttle_events();
    t_press();
    if (n > 0) params_restore(snap, n);
    return t_end();
}
