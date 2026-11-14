/* missile.cpp - missile model + guidance (DESIGN 12.2, 12.3): the ONE step function shared by live missiles
   (weapons.cpp) and the launch-zone fly-outs (dlz.cpp). Portable. Owner: T12. Model notes: missile.h.
   Game mechanics with invented numbers ([lrm] [srm]); tuned towards DESIGN 12.7's gameplay targets with the
   fly-out table of tests_missile.cpp (`simh test missile table` prints it with the loaded ini).
   T12 tuning vs the 12.7 start values: LRM dragK 1.1e-4 -> 0.9e-4, armSec 0.8 -> 1.3, fuzeM 15 -> 25, minSpeed 250 ->
   390; SRM dragK 2.5e-4 -> 1.4e-4, inducedK 0.05 -> 0.3, armSec 0.4 -> 0.65, fuzeM 10 -> 20, minSpeed 220 -> 470,
   trackMul 1.05. Why: minSpeed (missile "out of energy") is what separates head-on Rmax from tail Rmax; inducedK
   makes the SRM pay for chasing a breaking target (more would kill crossing shots: 1.0 gave beam Rmax 0); the
   fuze radius must stay well above
   the guidance discretisation (33 m per step at the fly-out's dt 1/30) so hit/miss is decided by kinematics, not by
   a few metres of end-game noise (live 1/120 and fly-out 1/30 then agree). */
#include "missile.h"
#include "params.h"
#include "e_terrain.h"

static MissileParams s_mp[MSLK_COUNT];            /* [lrm] [srm] [lrm_player] (T21a) */

#define MP_PARAMS(S, I, DROP, BSEC, BACC, SUSEC, SUACC, DRAG, WAVE, IND, INDK2, GMAX, GREF, NAV, GIM, ACQ, ACT, SEEK, \
                  LTAIL, LHEAD, TRK, ARM, FUZE, MAXS, MINV, LAUNCH) \
    { S, "dropSec", P_FLOAT, &s_mp[I].dropSec, DROP, 0.0f, 2.0f, "s: free drop off the rail before the motor lights" }, \
    { S, "boostSec", P_FLOAT, &s_mp[I].boostSec, BSEC, 0.0f, 20.0f, "s: motor boost burn time" }, \
    { S, "boostAcc", P_FLOAT, &s_mp[I].boostAcc, BACC, 0.0f, 2000.0f, "m/s^2: motor boost acceleration" }, \
    { S, "sustainSec", P_FLOAT, &s_mp[I].sustainSec, SUSEC, 0.0f, 20.0f, "s: motor sustain burn after the boost (0 = single-stage)" }, \
    { S, "sustainAcc", P_FLOAT, &s_mp[I].sustainAcc, SUACC, 0.0f, 2000.0f, "m/s^2: motor sustain acceleration" }, \
    { S, "dragK", P_FLOAT, &s_mp[I].dragK, DRAG, 0.0f, 0.01f, "1/m: drag deceleration = dragK x rho x V^2 x cd(Mach)" }, \
    { S, "waveDragK", P_FLOAT, &s_mp[I].waveDragK, WAVE, 0.0f, 3.0f, "transonic wave-drag peak multiplier (0 = off)" }, \
    { S, "inducedK", P_FLOAT, &s_mp[I].inducedK, IND, 0.0f, 1.0f, "deceleration per m/s^2 of lateral acceleration" }, \
    { S, "inducedK2", P_FLOAT, &s_mp[I].inducedK2, INDK2, 0.0f, 0.01f, "deceleration per (m/s^2)^2 of lateral acceleration (quadratic, 0 = off)" }, \
    { S, "gMax", P_FLOAT, &s_mp[I].gMax, GMAX, 1.0f, 100.0f, "g: lateral acceleration limit at/above gRefSpeed" }, \
    { S, "gRefSpeed", P_FLOAT, &s_mp[I].gRefSpeed, GREF, 50.0f, 2000.0f, "m/s: below this the limit falls with (V/gRefSpeed)^2" }, \
    { S, "navN", P_FLOAT, &s_mp[I].navN, NAV, 1.0f, 10.0f, "proportional navigation constant" }, \
    { S, "gimbalDeg", P_FLOAT, &s_mp[I].gimbalDeg, GIM, 5.0f, 90.0f, "deg: seeker gimbal limit (nose -> target)" }, \
    { S, "acqConeDeg", P_FLOAT, &s_mp[I].acqConeDeg, ACQ, 0.5f, 90.0f, "deg: LRM active search cone / SRM lock circle at the boresight" }, \
    { S, "activeM", P_FLOAT, &s_mp[I].activeM, ACT, 0.0f, 50000.0f, "m: LRM goes active when its estimated distance is below this" }, \
    { S, "seekM", P_FLOAT, &s_mp[I].seekM, SEEK, 0.0f, 50000.0f, "m: LRM seeker acquisition range" }, \
    { S, "lockTailM", P_FLOAT, &s_mp[I].lockTailM, LTAIL, 0.0f, 50000.0f, "m: SRM heat lock range from behind the target" }, \
    { S, "lockHeadM", P_FLOAT, &s_mp[I].lockHeadM, LHEAD, 0.0f, 50000.0f, "m: SRM heat lock range nose-on" }, \
    { S, "abLockMul", P_FLOAT, &s_mp[I].abLockMul, "1.2", 1.0f, 3.0f, "SRM lock range factor vs a target in afterburner" }, \
    { S, "trackMul", P_FLOAT, &s_mp[I].trackMul, TRK, 1.0f, 5.0f, "seeker keeps tracking out to acquisition range x this" }, \
    { S, "armSec", P_FLOAT, &s_mp[I].armSec, ARM, 0.0f, 5.0f, "s: fuze arms after this" }, \
    { S, "fuzeM", P_FLOAT, &s_mp[I].fuzeM, FUZE, 1.0f, 100.0f, "m: proximity fuze radius" }, \
    { S, "maxSec", P_FLOAT, &s_mp[I].maxSec, MAXS, 1.0f, 200.0f, "s: self-destruct age" }, \
    { S, "minSpeed", P_FLOAT, &s_mp[I].minSpeed, MINV, 0.0f, 1000.0f, "m/s: dead below this after the boost" }, \
    { S, "damageEnemy", P_FLOAT, &s_mp[I].damageEnemy, "100", 0.0f, 1000.0f, "hit damage vs an enemy (hit event; mission applies)" }, \
    { S, "damagePlayer", P_FLOAT, &s_mp[I].damagePlayer, "55", 0.0f, 1000.0f, "hit damage vs the player (hit event; mission applies)" }, \
    { S, "launchSec", P_FLOAT, &s_mp[I].launchSec, LAUNCH, 0.0f, 3.0f, "s: straight flight after launch before guidance" }, \
    { S, "maddogSec", P_FLOAT, &s_mp[I].maddogSec, "1.5", 0.0f, 10.0f, "s: LRM without TD flies straight, then goes active" }, \
    { S, "searchSec", P_FLOAT, &s_mp[I].searchSec, "2.0", 0.0f, 10.0f, "s: SRM fired without lock searches this long, then unguided" }, \
    { S, "searchConeDeg", P_FLOAT, &s_mp[I].searchConeDeg, "12", 0.5f, 90.0f, "deg: SRM search cone after a launch without lock" }, \
    { S, "lostSec", P_FLOAT, &s_mp[I].lostSec, "0.3", 0.0f, 5.0f, "s: target outside gimbal/LOS this long = seeker lost" }, \
    { S, "memSec", P_FLOAT, &s_mp[I].memSec, "1.5", 0.0f, 10.0f, "s: lost seeker flies to the last known point, then unguided" }, \
    { S, "losSec", P_FLOAT, &s_mp[I].losSec, "0.1", 0.01f, 1.0f, "s: seeker terrain LOS check period" }, \
    { S, "densityAlt", P_BOOL, &s_mp[I].densityAlt, "1", 0.0f, 1.0f, "1 = drag x exp(-alt/8500)" }, \
    { S, "apnK", P_FLOAT, &s_mp[I].apnK, "1.0", 0.0f, 3.0f, "augmented PN: x navN/2 x target acceleration (0 = pure PN)" }

/* [lrm_player] tuned values (T21b realism pass, `simh test missile table`): the player's radar missile flies a
   realistic AMRAAM profile (GDD aim_120_amraam_simulation_gdd.md): a two-stage boost->sustain motor, a transonic
   wave-drag peak, quadratic (lift-induced) drag, a kinematic loft, an earlier active seeker and a 60 deg gimbal.
   It still delivers the player's ~2x envelope of T21a (user M2 play-test), now via loft + energy management rather
   than one big flat boost. Enemy [lrm] and everyone's [srm] are unchanged (sustainSec/waveDragK/inducedK2/loftDeg = 0
   reproduce the T12 model bit-for-bit). */
#define T21B_LRMP_BSEC   "2.5"
#define T21B_LRMP_BACC   "300"
#define T21B_LRMP_SUSEC  "5.5"
#define T21B_LRMP_SUACC  "74"
#define T21B_LRMP_DRAG   "0.00007"
#define T21B_LRMP_WAVE   "0.45"
#define T21B_LRMP_IND2   "0.0001"
#define T21B_LRMP_MAXS   "60"
#define T21B_LRMP_MINV   "390"

static const Param s_tab[] = {
    MP_PARAMS("lrm", 0, "0.15", "3.0", "200", "0", "0", "0.00009", "0", "0.05", "0", "30", "500", "4.0", "50", "25", "7000",
              "9000", "8000", "5000", "1.5", "1.3", "25", "45", "390", "0.3"),
    { "lrm", "loftDeg", P_FLOAT, &s_mp[0].loftDeg, "0", 0.0f, 40.0f, "deg: midcourse loft climb angle (0 = off)" },
    { "lrm", "loftRangeM", P_FLOAT, &s_mp[0].loftRangeM, "15000", 0.0f, 80000.0f, "m: loft engaged while the estimated distance > this" },
    { "lrm", "loftAltM", P_FLOAT, &s_mp[0].loftAltM, "12000", 0.0f, 30000.0f, "m: loft fades to 0 at/above this launch altitude" },
    MP_PARAMS("srm", 1, "0.10", "2.0", "250", "0", "0", "0.00014", "0", "0.3", "0", "40", "450", "4.0", "40", "3", "0",
              "0", "8000", "5000", "1.05", "0.65", "20", "20", "470", "0.1"),
    /* T21a+T21b: the PLAYER's radar missile (user M2 play-test 2026-11-14: "the radar guided should go double distance";
       enemy missiles unchanged). Same model and phases as [lrm]; T21B_LRMP_* below are the tuned realism differences. */
    MP_PARAMS("lrm_player", 2, "0.15", T21B_LRMP_BSEC, T21B_LRMP_BACC, T21B_LRMP_SUSEC, T21B_LRMP_SUACC, T21B_LRMP_DRAG,
              T21B_LRMP_WAVE, "0", T21B_LRMP_IND2, "30", "500", "4.0", "60", "25", "18000", "22000", "8000", "5000",
              "1.5", "1.4", "25", T21B_LRMP_MAXS, T21B_LRMP_MINV, "0.3"),
    { "lrm_player", "loftDeg", P_FLOAT, &s_mp[2].loftDeg, "16", 0.0f, 40.0f, "deg: midcourse loft climb angle (0 = off)" },
    { "lrm_player", "loftRangeM", P_FLOAT, &s_mp[2].loftRangeM, "15000", 0.0f, 80000.0f, "m: loft engaged while the estimated distance > this" },
    { "lrm_player", "loftAltM", P_FLOAT, &s_mp[2].loftAltM, "12000", 0.0f, 30000.0f, "m: loft fades to 0 at/above this launch altitude" },
};

void missile_params(void)
{
    s_mp[0].type = MSL_LRM;
    s_mp[1].type = MSL_SRM;
    s_mp[2].type = MSL_LRM;
    params_add(s_tab, E_COUNT(s_tab));
    s_mp[1].loftDeg = 0.0f;
    s_mp[1].loftRangeM = 0.0f;
    s_mp[1].loftAltM = 0.0f;
}

const MissileParams *missile_params_of(int type)
{
    return &s_mp[type == MSL_SRM ? 1 : type == MSLK_LRM_PLAYER ? 2 : 0];
}

float missile_burn_end(const MissileParams *mp)
{
    return mp->dropSec + mp->boostSec + mp->sustainSec;
}

int missile_kind(int type, int player) { return type == MSL_LRM && player ? MSLK_LRM_PLAYER : type; }
int missile_kind_type(int kind) { return kind == MSLK_LRM_PLAYER ? MSL_LRM : kind; }
const char *missile_kind_name(int kind)
{
    return kind == MSLK_LRM_PLAYER ? "lrm_player" : kind == MSL_SRM ? "srm" : "lrm";
}

const char *missile_phase_name(int phase)
{
    static const char *const n[] = { "LAUNCH", "MIDCOURSE", "ACTIVE", "TERMINAL", "LOST", "DEAD" };
    return phase >= 0 && phase <= MPH_DEAD ? n[phase] : "?";
}

const char *missile_result_name(int res)
{
    static const char *const n[] = { "none", "hit", "ground", "expired", "flare", "selfdestruct" };
    return res >= 0 && res <= MRES_SELFDESTRUCT ? n[res] : "?";
}

/* ---------------------------------------------------------------- helpers
   Hot path note (VM, VC6): e_math's v3_* are out-of-line calls returning structs; missile_step runs up to ~1500
   times per frame in T13's fly-outs, so it uses these inline component macros (no aliasing of o with b). */
#define VSUB(o, a, b)     ((o).x = (a).x - (b).x, (o).y = (a).y - (b).y, (o).z = (a).z - (b).z)
#define VDOT(a, b)        ((a).x * (b).x + (a).y * (b).y + (a).z * (b).z)
#define VMADD(o, a, b, s) ((o).x = (a).x + (b).x * (s), (o).y = (a).y + (b).y * (s), (o).z = (a).z + (b).z * (s))
#define VSCALE(o, a, s)   ((o).x = (a).x * (s), (o).y = (a).y * (s), (o).z = (a).z * (s))
#define VCROSS(o, a, b)   ((o).x = (a).y * (b).z - (a).z * (b).y, (o).y = (a).z * (b).x - (a).x * (b).z, \
                           (o).z = (a).x * (b).y - (a).y * (b).x)
#define VZERO(o)          ((o).x = (o).y = (o).z = 0.0f)
static inline float fsqrt(float x) { return (float)sqrt(x); }

/* cos of a degree value: pure memo (same result as computing it; params change rarely) */
static float cos_deg(float d)
{
    static float memD[8] = { 1e30f, 1e30f, 1e30f, 1e30f, 1e30f, 1e30f, 1e30f, 1e30f }, memC[8];
    static int memNext;
    int i;
    for (i = 0; i < 8; i++) if (memD[i] == d) return memC[i];
    i = memNext;
    memNext = (memNext + 1) & 7;
    memC[i] = (float)cos(E_DEG2RAD(d));
    memD[i] = d;
    return memC[i];
}

int missile_ground_hit(const struct Terrain *t, const Vec3 *a, const Vec3 *b, Vec3 *hit)
{
    float ymin = a->y < b->y ? a->y : b->y;
    if (!t || !t->loaded) {
        float k;
        if (ymin > 0.0f) return 0;
        k = a->y <= 0.0f ? 0.0f : a->y / (a->y - b->y);
        if (hit) { *hit = v3_lerp(*a, *b, k); hit->y = 0.0f; }
        return 1;
    }
    if (ymin > t->seaLevel) {
        float blk = 8.0f * t->cell - 2.0f, bm, h;
        if (ymin > t->hTop + 0.5f) return 0;
        if ((float)fabs(a->x - b->x) < blk && (float)fabs(a->z - b->z) < blk) {
            /* a box smaller than one HMAX block touches at most the 2 x 2 blocks of its corners */
            bm = terrain_block_max(t, a->x, a->z);
            h = terrain_block_max(t, b->x, b->z); if (h > bm) bm = h;
            h = terrain_block_max(t, a->x, b->z); if (h > bm) bm = h;
            h = terrain_block_max(t, b->x, a->z); if (h > bm) bm = h;
            if (ymin > bm + 0.5f) return 0;
        }
    }
    return terrain_segment_hit(t, a, b, hit);
}

float missile_heat_range(const MissileParams *mp, const TrackView *tgt, Vec3 from)
{
    Vec3 d;
    float vl2, dl2, tail, R;
    if (mp->type != MSL_SRM) return mp->seekM;
    if (tgt->isFlare) return mp->lockTailM;
    vl2 = VDOT(tgt->vel, tgt->vel);
    VSUB(d, from, tgt->pos);
    dl2 = VDOT(d, d);
    tail = 0.5f;
    if (vl2 > 1.0f && dl2 > 1.0f)                   /* 0 = we are in front of its nose, 1 = right behind it */
        tail = 0.5f * (1.0f - VDOT(tgt->vel, d) / fsqrt(vl2 * dl2));
    R = mp->lockHeadM + (mp->lockTailM - mp->lockHeadM) * tail;
    if (tgt->ab > 0.5f) R *= mp->abLockMul;
    return R;
}

int missile_searching(const Missile *m)
{
    if (m->phase != MPH_ACTIVE || (m->flags & MF_UNGUIDED)) return 0;
    return 1;
}

int missile_try_acquire(Missile *m, const MissileParams *mp, const TrackView *tgt, const struct Terrain *t)
{
    Vec3 r, axis;
    float dist, cone, range;
    if (!tgt || !tgt->valid) return 0;
    r = v3_sub(tgt->pos, m->pos);
    dist = v3_len(r);
    if (dist < 1.0f) return 0;
    range = missile_heat_range(mp, tgt, m->pos);
    if (dist > range) return 0;
    if (v3_dot(r, m->dir) < dist * cos_deg(mp->gimbalDeg)) return 0;
    if (m->type == MSL_LRM && !(m->flags & MF_MADDOG)) {
        axis = v3_sub(m->aimPos, m->pos);
        if (v3_len(axis) < 1.0f) axis = m->dir; else axis = v3_norm(axis);
        cone = mp->acqConeDeg;
    } else {
        axis = m->dir;
        cone = m->type == MSL_SRM ? mp->searchConeDeg : mp->acqConeDeg;
    }
    if (v3_dot(r, axis) < dist * cos_deg(cone)) return 0;
    if (!terrain_los((const Terrain *)t, &m->pos, &tgt->pos)) return 0;
    m->phase = MPH_TERMINAL;
    m->target = tgt->id;
    m->aimPos = tgt->pos;
    m->aimVel = tgt->vel;
    m->aimAcc = v3(0.0f, 0.0f, 0.0f);
    m->aimFresh = 1;
    m->losOk = 1;
    m->lostT = 0.0f;
    m->memT = 0.0f;
    m->flags |= MF_ACQUIRED | MF_SEEN;
    if (tgt->isFlare) m->flags |= MF_DECOYED;
    m->lastDist = dist;
    if (dist < m->minDist) m->minDist = dist;
    m->openT = 0.0f;
    return 1;
}

void missile_launch(Missile *m, int type, int owner, int target, Vec3 pos, Vec3 dir, float V, const TrackView *aim,
                    int flags, float losPhase)
{
    const MissileParams *mp = missile_params_of(type);
    memset(m, 0, sizeof(*m));
    m->type = missile_kind_type(type);
    m->kind = type;
    m->owner = owner;
    m->target = target;
    m->pos = pos;
    m->prevPos = pos;
    m->endPos = pos;
    m->dir = v3_norm(dir);
    m->V = V;
    m->launchY = pos.y;
    m->phase = MPH_LAUNCH;
    m->flags = flags;
    m->result = MRES_NONE;
    if (aim && aim->valid) {
        m->aimPos = aim->pos;
        m->aimVel = aim->vel;
        m->lastDist = v3_len(v3_sub(aim->pos, pos));
    } else {
        m->aimPos = v3_madd(pos, m->dir, 20000.0f);
        m->aimVel = v3(0.0f, 0.0f, 0.0f);
        m->lastDist = 1.0e9f;
    }
    m->minDist = m->lastDist;
    if (losPhase < 0.0f) losPhase = 0.0f;
    m->losTimer = losPhase * mp->losSec;
    m->losOk = 1;                                   /* launched on a lock / seeker tone: LOS was there */
    m->rollAge = -100.0f;
    m->rhoY = -1.0e9f;
    m->rho = 1.0f;
}

/* seeker sees tgt this step: in gimbal, in tracking range, last LOS check clear */
static inline int seeker_sees(const Missile *m, const MissileParams *mp, const TrackView *tgt)
{
    Vec3 r;
    float dist, range;
    if (!tgt || !tgt->valid || !m->losOk) return 0;
    VSUB(r, tgt->pos, m->pos);
    dist = fsqrt(VDOT(r, r));
    range = missile_heat_range(mp, tgt, m->pos) * mp->trackMul;
    if (dist > range) return 0;
    if (dist < 1.0f) return 1;
    return VDOT(r, m->dir) >= dist * cos_deg(mp->gimbalDeg);
}

static inline void aim_from(Missile *m, const TrackView *tgt, float dt)
{
    if (m->aimFresh && dt > 0.0f) {
        float k = dt / 0.1f, inv = 1.0f / dt;       /* ~0.1 s smoothing (same at 1/30 and 1/120) */
        if (k > 1.0f) k = 1.0f;
        m->aimAcc.x += ((tgt->vel.x - m->aimVel.x) * inv - m->aimAcc.x) * k;
        m->aimAcc.y += ((tgt->vel.y - m->aimVel.y) * inv - m->aimAcc.y) * k;
        m->aimAcc.z += ((tgt->vel.z - m->aimVel.z) * inv - m->aimAcc.z) * k;
    } else {
        VZERO(m->aimAcc);
    }
    m->aimPos = tgt->pos;
    m->aimVel = tgt->vel;
    m->aimFresh = 1;
}
static inline void aim_extrapolate(Missile *m, float dt)
{
    VMADD(m->aimPos, m->aimPos, m->aimVel, dt);
    VZERO(m->aimAcc);
    m->aimFresh = 0;
}

void missile_estimates(const Missile *m, const MissileParams *mp, float *act, float *ttg)
{
    Vec3 r, vm;
    float dist, vEst, closing, bLeft;
    *act = -1.0f;
    *ttg = -1.0f;
    if (m->phase == MPH_DEAD || (m->flags & MF_UNGUIDED)) return;
    if (m->type == MSL_LRM && (m->flags & MF_MADDOG) && m->phase != MPH_TERMINAL) return;
    if (m->type == MSL_SRM && m->phase == MPH_ACTIVE) return;
    r = v3_sub(m->aimPos, m->pos);
    dist = v3_len(r);
    if (dist < 1.0f) { *ttg = 0.0f; return; }
    vEst = m->V;
    bLeft = mp->dropSec + mp->boostSec - m->age;
    if (bLeft > 0.0f) vEst += 0.5f * mp->boostAcc * (bLeft < mp->boostSec ? bLeft : mp->boostSec);
    bLeft = mp->dropSec + mp->boostSec + mp->sustainSec - m->age;
    if (bLeft > 0.0f && mp->sustainSec > 0.0f)
        vEst += 0.5f * mp->sustainAcc * (bLeft < mp->sustainSec ? bLeft : mp->sustainSec);
    vm = v3_scale(m->dir, vEst);
    closing = -v3_dot(r, v3_sub(m->aimVel, vm)) / dist;
    if (closing < 30.0f) return;
    *ttg = dist / closing;
    if (m->type == MSL_LRM && (m->phase == MPH_LAUNCH || m->phase == MPH_MIDCOURSE)) {
        *act = (dist - mp->activeM) / closing;
        if (*act < 0.0f) *act = 0.0f;
    }
}

/* ---------------------------------------------------------------- the step (DESIGN 12.2) */
void missile_step(Missile *m, const MissileParams *mp, const TrackView *tgt, const struct Terrain *t, float dt)
{
    const Terrain *ter = (const Terrain *)t;
    Vec3 prev, r, vm, vr, om, a, up, hitP, d0, d1, dd;
    float rr, aMax, alen, thrust, rho, vratio, den, tc, dmin, tGround, segLen, dist;
    int doCheck, haveTgt, guided, ground, hitCand;

    if (!m || !mp || m->phase == MPH_DEAD) return;
    m->flags &= ~(MF_LOSCHECK | MF_ACQUIRED | MF_SEEN);
    prev = m->pos;
    m->prevPos = prev;
    m->age += dt;
    if (m->age >= mp->armSec) m->flags |= MF_ARMED;
    haveTgt = tgt && tgt->valid;

    /* seeker LOS cadence (DESIGN 12.2: 10 Hz, staggered by the launch phase) */
    doCheck = 0;
    m->losTimer -= dt;
    if (m->losTimer <= 0.0f) {
        m->losTimer += mp->losSec;
        if (m->losTimer <= 0.0f) m->losTimer = mp->losSec;
        doCheck = 1;
        m->flags |= MF_LOSCHECK;
    }

    /* ---- phases (DESIGN 12.3) */
    if (m->phase == MPH_LAUNCH && m->age >= mp->launchSec) {
        if (m->type == MSL_SRM) m->phase = (m->flags & MF_GUIDED) ? MPH_TERMINAL : MPH_ACTIVE;
        else m->phase = MPH_MIDCOURSE;
    }
    switch (m->phase) {
    case MPH_LAUNCH:
    case MPH_MIDCOURSE:
    case MPH_ACTIVE:
        if (m->type == MSL_LRM && !(m->flags & MF_MADDOG)) {
            if (haveTgt && tgt->datalink && !tgt->isFlare) { aim_from(m, tgt, dt); m->flags &= ~MF_INERTIAL; }
            else { aim_extrapolate(m, dt); m->flags |= MF_INERTIAL; }
        }
        if (m->phase == MPH_LAUNCH && m->type == MSL_SRM && (m->flags & MF_GUIDED) && haveTgt) aim_from(m, tgt, dt);
        if (m->phase == MPH_MIDCOURSE) {
            if (m->flags & MF_MADDOG) { if (m->age >= mp->maddogSec) m->phase = MPH_ACTIVE; }
            else {
                VSUB(r, m->aimPos, m->pos);
                if (VDOT(r, r) < mp->activeM * mp->activeM) m->phase = MPH_ACTIVE;
            }
        }
        if (m->phase == MPH_ACTIVE) {
            if (m->type == MSL_SRM && m->age > mp->launchSec + mp->searchSec) {
                m->phase = MPH_LOST;
                m->flags |= MF_UNGUIDED;
            } else if (doCheck && haveTgt && !tgt->isFlare) {
                missile_try_acquire(m, mp, tgt, t);
            }
        }
        break;
    case MPH_TERMINAL:
        if (haveTgt && doCheck) m->losOk = terrain_los(ter, &m->pos, &tgt->pos);
        if (seeker_sees(m, mp, tgt)) {
            aim_from(m, tgt, dt);
            m->lostT = 0.0f;
            m->flags |= MF_SEEN;
        } else {
            aim_extrapolate(m, dt);
            m->lostT += dt;
            if (m->lostT > mp->lostSec) { m->phase = MPH_LOST; m->memT = 0.0f; }
        }
        break;
    case MPH_LOST:
        aim_extrapolate(m, dt);
        if (!(m->flags & MF_UNGUIDED)) {
            m->memT += dt;
            if (doCheck && haveTgt && !((m->flags & MF_DECOYED) && !tgt->isFlare)) {
                m->losOk = terrain_los(ter, &m->pos, &tgt->pos);
                if (seeker_sees(m, mp, tgt)) {
                    m->phase = MPH_TERMINAL;
                    m->lostT = 0.0f;
                    aim_from(m, tgt, dt);
                    m->flags |= MF_SEEN | MF_ACQUIRED;
                }
            }
            if (m->phase == MPH_LOST && m->memT > mp->memSec) m->flags |= MF_UNGUIDED;
        }
        break;
    }

    /* ---- lateral acceleration: PN on the aim estimate (DESIGN 12.2: gravity acts on the speed only, the path is
       bent by aLat alone, so there is nothing to compensate) */
    VZERO(a);
    guided = !(m->flags & MF_UNGUIDED) &&
             ((m->phase == MPH_MIDCOURSE && !(m->flags & MF_MADDOG)) ||
              (m->phase == MPH_ACTIVE && m->type == MSL_LRM && !(m->flags & MF_MADDOG)) ||
              m->phase == MPH_TERMINAL || m->phase == MPH_LOST);
    if (guided) {
        VSUB(r, m->aimPos, m->pos);
        rr = VDOT(r, r);
        if (rr > 1.0f) {
            Vec3 cr;
            float k;
            if (m->phase == MPH_MIDCOURSE && m->type == MSL_LRM && !(m->flags & MF_MADDOG) &&
                mp->loftDeg > 0.0f && rr > mp->loftRangeM * mp->loftRangeM) {
                /* kinematic loft: elevate the midcourse aim point so pure PN climbs into thin air, then dives home
                   as the offset shrinks toward loftRangeM (GDD 5.1) */
                float ldist = fsqrt(rr), altF = 1.0f - m->launchY / mp->loftAltM, pitch;
                if (altF < 0.0f) altF = 0.0f; else if (altF > 1.0f) altF = 1.0f;
                pitch = mp->loftDeg * altF;
                r.y += (float)tan(E_DEG2RAD(pitch)) * (ldist - mp->loftRangeM);
                rr = VDOT(r, r);
            }
            VSCALE(vm, m->dir, m->V);
            VSUB(vr, m->aimVel, vm);
            VCROSS(cr, r, vr);
            VSCALE(om, cr, 1.0f / rr);
            VCROSS(cr, om, vm);
            VSCALE(a, cr, mp->navN);
            if (mp->apnK > 0.0f) {                  /* target acceleration across the LOS */
                k = 0.5f * mp->navN * mp->apnK;
                VMADD(up, m->aimAcc, r, -VDOT(m->aimAcc, r) / rr);
                VMADD(a, a, up, k);
            }
        }
    }
    {
        float ad = -VDOT(a, m->dir);                /* lateral only */
        VMADD(a, a, m->dir, ad);
    }
    vratio = m->V / mp->gRefSpeed;
    if (vratio > 1.0f) vratio = 1.0f;
    aMax = mp->gMax * GRAV * vratio * vratio;
    alen = fsqrt(VDOT(a, a));
    if (alen > aMax && alen > 0.0f) { float k = aMax / alen; VSCALE(a, a, k); alen = aMax; }
    m->aLat = alen;
    m->aLimit = aMax;

    /* ---- speed, direction, position */
    /* motor: boost [dropSec, dropSec+boostSec] then sustain [dropSec+boostSec, dropSec+boostSec+sustainSec]
       (the exact share of each interval inside this step: same impulse at any dt; sustainSec 0 = single stage) */
    {
        float b0 = mp->dropSec, b1 = mp->dropSec + mp->boostSec, c1 = b1 + mp->sustainSec, s0, s1, ov;
        thrust = 0.0f;
        s0 = m->age - dt; if (s0 < b0) s0 = b0;
        s1 = m->age;      if (s1 > b1) s1 = b1;
        ov = s1 - s0;     if (ov > 0.0f) thrust += mp->boostAcc * ov;
        s0 = m->age - dt; if (s0 < b1) s0 = b1;
        s1 = m->age;      if (s1 > c1) s1 = c1;
        ov = s1 - s0;     if (ov > 0.0f) thrust += mp->sustainAcc * ov;
        thrust /= dt;
    }
    rho = 1.0f;
    if (mp->densityAlt) {                           /* cached per missile, refreshed every 20 m of altitude */
        if ((float)fabs(m->pos.y - m->rhoY) > 20.0f) {
            m->rhoY = m->pos.y;
            m->rho = (float)exp(-m->pos.y / 8500.0f);
            if (m->rho > 1.5f) m->rho = 1.5f;
        }
        rho = m->rho;
    }
    {
        /* midpoint (RK2) speed: fly-outs at dt 1/30 track live missiles at 1/120 */
        float V0 = m->V, cd = 1.0f, base, Vm, V1;
        if (mp->waveDragK > 0.0f) {                 /* transonic wave-drag peak (GDD 4.2): bump around M 0.8-2.8 */
            float sos = 340.3f - 0.004f * (m->pos.y < 11000.0f ? m->pos.y : 11000.0f), M = V0 / sos, b;
            if (M < 0.80f) b = 0.0f;
            else if (M < 1.05f) b = (M - 0.80f) * 4.0f;                       /* 0 -> 1 */
            else if (M < 1.60f) b = 1.0f - 0.85f * (M - 1.05f) / 0.55f;       /* 1 -> 0.15 */
            else if (M < 2.80f) b = 0.15f - 0.15f * (M - 1.60f) / 1.20f;      /* 0.15 -> 0 */
            else b = 0.0f;
            cd = 1.0f + mp->waveDragK * b;
        }
        base = thrust - mp->inducedK * alen - mp->inducedK2 * alen * alen - GRAV * m->dir.y;
        Vm = V0 + 0.5f * dt * (base - mp->dragK * rho * cd * V0 * V0);
        if (Vm < 1.0f) Vm = 1.0f;
        V1 = V0 + dt * (base - mp->dragK * rho * cd * Vm * Vm);
        if (V1 < 1.0f) V1 = 1.0f;
        m->V = V1;
        {
            float k = dt / V0, l;
            VMADD(m->dir, m->dir, a, k);
            l = VDOT(m->dir, m->dir);
            if (l > 1e-12f) { l = 1.0f / fsqrt(l); VSCALE(m->dir, m->dir, l); } else m->dir = v3(0.0f, 1.0f, 0.0f);
            k = 0.5f * (V0 + V1) * dt;
            VMADD(m->pos, m->pos, m->dir, k);
        }
    }

    /* ---- fuze (closest approach of this step's relative motion), ground, expiry */
    hitCand = 0;
    tc = 2.0f;
    if (haveTgt && (m->flags & MF_ARMED) && !(m->flags & MF_UNGUIDED)) {
        Vec3 c;
        d0.x = tgt->pos.x - tgt->vel.x * dt - prev.x;
        d0.y = tgt->pos.y - tgt->vel.y * dt - prev.y;
        d0.z = tgt->pos.z - tgt->vel.z * dt - prev.z;
        VSUB(d1, tgt->pos, m->pos);
        VSUB(dd, d1, d0);
        den = VDOT(dd, dd);
        tc = den > 1e-6f ? -VDOT(d0, dd) / den : 1.0f;
        tc = E_CLAMP(tc, 0.0f, 1.0f);
        VMADD(c, d0, dd, tc);
        dmin = VDOT(c, c);
        if (dmin < mp->fuzeM * mp->fuzeM) hitCand = 1;
        if (dmin < m->minDist * m->minDist) m->minDist = fsqrt(dmin);
    }
    ground = missile_ground_hit(ter, &prev, &m->pos, &hitP);
    tGround = 2.0f;
    if (ground) {
        segLen = v3_len(v3_sub(m->pos, prev));
        tGround = segLen > 1e-4f ? v3_len(v3_sub(hitP, prev)) / segLen : 0.0f;
    }
    if (hitCand && tc <= tGround) {
        m->result = tgt->isFlare ? MRES_FLARE : MRES_HIT;
        m->pos = v3_lerp(prev, m->pos, tc);
        m->endPos = m->pos;
    } else if (ground) {
        m->result = MRES_GROUND;
        m->pos = hitP;
        m->endPos = hitP;
    }
    if (m->result == MRES_NONE && haveTgt && !tgt->isFlare) {
        VSUB(d1, tgt->pos, m->pos);
        dist = fsqrt(VDOT(d1, d1));
        if (dist < m->minDist) m->minDist = dist;
        if ((m->flags & MF_ARMED) && m->age > missile_burn_end(mp) && dist > m->lastDist) {
            m->openT += dt;
            if (m->openT >= 0.5f) m->flags |= MF_MISSED;
        } else {
            m->openT = 0.0f;
        }
        m->lastDist = dist;
    }
    if (m->result == MRES_NONE &&
        (m->age > mp->maxSec || (m->V < mp->minSpeed && m->age > missile_burn_end(mp)))) {
        m->result = MRES_EXPIRED;
        m->endPos = m->pos;
    }
    if (m->result != MRES_NONE) m->phase = MPH_DEAD;
}
