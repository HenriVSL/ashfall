/* target.cpp - the player's target selection/designation from S short/long (DESIGN 11.4). Portable. Owner: T11.
   Rules: target.h. Lock and designation state: radar.* (this module only calls radar_designate). */
#include "target.h"
#include "radar.h"
#include "params.h"
#include "e_log.h"

static float s_selNm, s_cycleSec;
static int s_autoReselect, s_autoRedes;

static const Param s_tab[] = {
    { "radar", "selectRangeNm", P_FLOAT, &s_selNm, "40", 1.0f, 200.0f, "S: contacts within this range (NM) can be selected" },
    { "radar", "cycleSec", P_FLOAT, &s_cycleSec, "2.0", 0.1f, 10.0f,
      "S taps closer together than this walk a frozen list (s); a later tap rebuilds it by angle off the nose" },
    { "radar", "autoReselect", P_BOOL, &s_autoReselect, "1", 0, 1,
      "1 = when the designated (or selected) contact dies, select the contact nearest the nose" },
    { "radar", "autoRedesignate", P_BOOL, &s_autoRedes, "0", 0, 1,
      "1 = after a kill also designate (lock) the newly selected contact (Ace Combat style)" },
};

void target_params(void) { params_add(s_tab, E_COUNT(s_tab)); }

static int s_sel, s_prevDes, s_prevSel;
static int s_cycle[MAX_AIRCRAFT], s_nCycle;
static u32 s_lastShort;                             /* tick of the last S short (0 = none) */

void target_reset(Game *g)
{
    (void)g;
    s_sel = s_prevDes = s_prevSel = -1;
    s_nCycle = 0;
    s_lastShort = 0;
    radar_set_selected(PLAYER_ID, -1);
}

int target_selected(void) { return s_sel; }
int target_designated(void) { return radar_designated(PLAYER_ID); }

int target_cycle(int *ids, int maxIds)
{
    int i;
    for (i = 0; i < s_nCycle && i < maxIds; i++) ids[i] = s_cycle[i];
    return s_nCycle;
}

/* selectable: DETECTED / HOSTILE / MEMORY within selectRangeNm (sim state, extrapolated MEMORY position) */
static int candidate(const Game *g, int obs, int id, float *offNose)
{
    int cs = radar_contact_state(obs, id);
    Vec3 p, d, b, f;
    float R;
    Quat q;
    if (cs != CS_DETECTED && cs != CS_HOSTILE && cs != CS_MEMORY) return 0;
    if (!radar_track(g, obs, id, &p, NULL)) return 0;
    d = v3_sub(p, g->ac[obs].fs.pos);
    R = v3_len(d);
    if (R > s_selNm * NM2M) return 0;
    if (offNose) {
        q = flight_nose(&g->ac[obs].fs);
        f = q_rotate(q, v3(0.0f, 0.0f, 1.0f));
        b = v3_scale(d, R > 1e-3f ? 1.0f / R : 0.0f);
        *offNose = (float)acos(E_CLAMP(v3_dot(f, b), -1.0f, 1.0f));
    }
    return 1;
}

/* candidates sorted by angle off the nose (ties by id); returns the count */
static int build_list(const Game *g, int obs, int *ids, int exclude)
{
    float ang[MAX_AIRCRAFT], a;
    int n = 0, id, i;
    for (id = 0; id < g->numAircraft; id++) {
        if (id == obs || id == exclude || !candidate(g, obs, id, &a)) continue;
        for (i = n; i > 0 && ang[i - 1] > a; i--) { ang[i] = ang[i - 1]; ids[i] = ids[i - 1]; }
        ang[i] = a;
        ids[i] = id;
        n++;
    }
    return n;
}

int target_nearest_nose(const Game *g, int obs, int exclude)
{
    int ids[MAX_AIRCRAFT];
    return build_list(g, obs, ids, exclude) > 0 ? ids[0] : -1;
}

static void press_short(const Game *g)
{
    int frozen = s_nCycle > 0 && s_sel >= 0 && s_lastShort && (int)(g->tick - s_lastShort) < (int)(s_cycleSec * SIM_HZ + 0.5f);
    int i, pos = -1;
    s_lastShort = g->tick;
    if (frozen) {
        for (i = 0; i < s_nCycle; i++) if (s_cycle[i] == s_sel) pos = i;
        for (i = 1; i <= s_nCycle; i++) {          /* next still-valid entry, wrapping (may come back to s_sel) */
            int k = s_cycle[(pos + i + s_nCycle) % s_nCycle];
            if (candidate(g, PLAYER_ID, k, NULL)) {
                s_sel = k;
                log_printf(LOG_INFO, "target: select id=%d (cycle %d/%d) tick=%u", s_sel, (pos + i) % s_nCycle + 1, s_nCycle, g->tick);
                return;
            }
        }
    }
    s_nCycle = build_list(g, PLAYER_ID, s_cycle, -1);
    if (s_nCycle == 0) {
        s_sel = -1;
        log_printf(LOG_INFO, "target: select none (no contacts) tick=%u", g->tick);
        return;
    }
    pos = 0;
    for (i = 0; i < s_nCycle; i++) if (s_cycle[i] == s_sel) pos = (i + 1) % s_nCycle;
    s_sel = s_cycle[pos];
    log_printf(LOG_INFO, "target: select id=%d (new list %d/%d) tick=%u", s_sel, pos + 1, s_nCycle, g->tick);
}

static void press_long(Game *g)
{
    int des = radar_designated(PLAYER_ID), lock;
    if (s_sel < 0 || !candidate(g, PLAYER_ID, s_sel, NULL)) s_sel = target_nearest_nose(g, PLAYER_ID, -1);
    if (s_sel < 0) {
        log_printf(LOG_INFO, "target: designate none (no contacts) tick=%u", g->tick);
        return;
    }
    if (s_sel == des) {
        radar_designate(g, PLAYER_ID, -1);
        log_printf(LOG_INFO, "target: break lock id=%d tick=%u", s_sel, g->tick);
        return;
    }
    lock = radar_designate(g, PLAYER_ID, s_sel);
    log_printf(LOG_INFO, "target: designate id=%d lock=%d tick=%u", s_sel, lock, g->tick);
}

void target_update(Game *g, const PilotInput *in)
{
    int des, cs, kick = 0;
    if (!(g->ac[PLAYER_ID].flags & AC_ALIVE)) {
        if (s_sel >= 0 || s_nCycle) target_reset(g);
        return;
    }
    /* designated contact killed (the radar already cleared the designation) */
    if (s_prevDes >= 0 && radar_contact_state(PLAYER_ID, s_prevDes) == CS_DEAD) {
        if (s_autoReselect) {
            s_sel = target_nearest_nose(g, PLAYER_ID, s_prevDes);
            log_printf(LOG_INFO, "target: kill id=%d reselect id=%d tick=%u", s_prevDes, s_sel, g->tick);
            if (s_sel >= 0 && s_autoRedes) radar_designate(g, PLAYER_ID, s_sel);
        } else if (s_sel == s_prevDes) s_sel = -1;
        s_nCycle = 0;
        kick = 1;
    }
    /* selected contact dead or gone */
    if (s_sel >= 0) {
        cs = radar_contact_state(PLAYER_ID, s_sel);
        if (cs == CS_DEAD) {
            s_sel = s_autoReselect ? target_nearest_nose(g, PLAYER_ID, s_sel) : -1;
            kick = 1;
        } else if (cs == CS_NONE) {
            s_sel = -1;
            kick = 1;
        }
    }
    if (in && in->tgtShort) { press_short(g); kick = 1; }
    if (in && in->tgtLong) { press_long(g); kick = 1; }
    des = radar_designated(PLAYER_ID);
    s_prevDes = des;
    if (s_sel != s_prevSel) kick = 1;
    s_prevSel = s_sel;
    radar_set_selected(PLAYER_ID, s_sel);
    if (kick) radar_view_kick(PLAYER_ID);
}
