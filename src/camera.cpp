/* camera.cpp - cockpit, chase, wreck and -camat cameras (DESIGN 7). See camera.h. Windows/render side. Owner: T08
   (T22: explosion / hit shake from fx_cam_shake, [camera] shakeExpl; chase geometry unchanged). */
#include "camera.h"
#include "scene.h"
#include "params.h"
#include "world.h"
#include "e_rend.h"
#include "fx.h"

static float s_fovCockpit, s_fovChase, s_fovAB, s_chaseDist, s_chaseUp, s_chaseLook, s_tauRot, s_tauRoll;
static float s_shakeG, s_shakeAB, s_minAgl, s_wreckDist, s_wreckUp, s_wreckRate;
static int   s_startView;
static float s_fovSpeed, s_fovSpeedCockpit, s_fovFromKt, s_fovToKt, s_tauFov;          /* T10b speed cues */
static float s_bufSpeed, s_bufFromKt, s_bufToKt, s_bufTrans, s_bufMachW, s_bufLowAlt, s_bufLowAltM, s_bufCockpit, s_bufHz;
static float s_shakeExpl;                           /* T22 */
static Vec3  s_eyeTag = { 0.0f, 0.98f, 4.35f };     /* A1 cam_cockpit tag; the jet.msh tag wins when loaded */

static const Param s_camTab[] = {
    { "camera", "startView", P_INT, &s_startView, "1", 0, 1, "view at start: 0 = cockpit (HUD only), 1 = chase (C toggles)" },
    { "camera", "fovCockpit", P_FLOAT, &s_fovCockpit, "65", 40, 100, "cockpit horizontal FOV (deg); 65 = the HUD scale" },
    { "camera", "fovChase", P_FLOAT, &s_fovChase, "70", 40, 110, "chase horizontal FOV (deg)" },
    { "camera", "fovAB", P_FLOAT, &s_fovAB, "5", 0, 20, "extra chase FOV at full afterburner (deg): speed feel" },
    { "camera", "chaseDist", P_FLOAT, &s_chaseDist, "24", 8, 80, "chase eye distance behind the jet (m)" },
    { "camera", "chaseUp", P_FLOAT, &s_chaseUp, "6", 0, 30, "chase eye height above the jet in the camera frame (m)" },
    { "camera", "chaseLook", P_FLOAT, &s_chaseLook, "40", 0, 200, "chase look point ahead of the jet along the nose (m)" },
    { "camera", "tauCamRot", P_FLOAT, &s_tauRot, "0.10", 0, 1, "chase camera direction lag (s): the jet leads the camera" },
    { "camera", "tauCamRoll", P_FLOAT, &s_tauRoll, "0.16", 0, 1, "chase camera roll lag (s)" },
    { "camera", "shakeG", P_FLOAT, &s_shakeG, "0.12", 0, 2, "shake (deg) per g above 7 g" },
    { "camera", "shakeAB", P_FLOAT, &s_shakeAB, "0.04", 0, 1, "shake (deg) at full afterburner" },
    { "camera", "shakeExpl", P_FLOAT, &s_shakeExpl, "1.0", 0, 3, "T22: x the explosion / hit shake from fx ([fx] shakeExplDeg); 0 = off" },
    { "camera", "chaseMinAgl", P_FLOAT, &s_minAgl, "3", 0, 50, "chase eye kept this high above the ground/sea (m)" },
    { "camera", "wreckDist", P_FLOAT, &s_wreckDist, "70", 10, 400, "wreck camera orbit radius (m)" },
    { "camera", "wreckUp", P_FLOAT, &s_wreckUp, "28", 2, 200, "wreck camera height above the wreck (m)" },
    { "camera", "wreckRate", P_FLOAT, &s_wreckRate, "18", 0, 180, "wreck camera orbit rate (deg/s)" },
    /* T10b: speed sensation (render only; [fx] speedCues = 0 turns all of it off) */
    { "camera", "fovSpeed", P_FLOAT, &s_fovSpeed, "10", 0, 30, "extra chase FOV (deg) at fovSpeedToKt: the world rushes past faster at the screen edges" },
    { "camera", "fovSpeedCockpit", P_FLOAT, &s_fovSpeedCockpit, "0", 0, 20, "extra cockpit FOV (deg) at speed; 0 keeps the HUD at its 65 deg scale" },
    { "camera", "fovSpeedFromKt", P_FLOAT, &s_fovFromKt, "350", 0, 2000, "speed FOV starts here (kt)" },
    { "camera", "fovSpeedToKt", P_FLOAT, &s_fovToKt, "1050", 1, 2000, "speed FOV reaches fovSpeed here (kt)" },
    { "camera", "tauFov", P_FLOAT, &s_tauFov, "0.6", 0, 5, "speed/afterburner FOV smoothing (s)" },
    { "camera", "buffetSpeed", P_FLOAT, &s_bufSpeed, "0.10", 0, 2, "high-speed buffet (deg) at buffetToKt" },
    { "camera", "buffetFromKt", P_FLOAT, &s_bufFromKt, "550", 0, 2000, "buffet starts at this speed (kt)" },
    { "camera", "buffetToKt", P_FLOAT, &s_bufToKt, "1050", 1, 2000, "full high-speed buffet at this speed (kt)" },
    { "camera", "buffetTransonic", P_FLOAT, &s_bufTrans, "0.22", 0, 2, "transonic buffet peak (deg) at Mach 1.0" },
    { "camera", "buffetMachW", P_FLOAT, &s_bufMachW, "0.05", 0.005f, 0.5f, "transonic band half width (Mach): bell around 1.0" },
    { "camera", "buffetLowAlt", P_FLOAT, &s_bufLowAlt, "0.8", 0, 4, "buffet x (1 + this) at the ground (dense air)" },
    { "camera", "buffetLowAltM", P_FLOAT, &s_bufLowAltM, "400", 1, 5000, "low-altitude buffet boost fades out by this height (m)" },
    { "camera", "buffetCockpit", P_FLOAT, &s_bufCockpit, "0.6", 0, 2, "buffet factor in the cockpit view" },
    { "camera", "buffetHz", P_FLOAT, &s_bufHz, "9", 1, 30, "buffet main frequency (Hz; smooth noise, not per-frame jitter)" },
};
void cam_params(void) { params_add(s_camTab, E_COUNT(s_camTab)); }

void cam_ac_pose(const Aircraft *a, float alpha, Vec3 *pos, Quat *qNose)
{
    Quat qp = q_slerp(a->prev.qPath, a->fs.qPath, alpha);
    float aoa = a->prev.alpha + (a->fs.alpha - a->prev.alpha) * alpha;
    *pos = v3_lerp(a->prev.pos, a->fs.pos, alpha);
    *qNose = q_norm(q_mul(qp, q_axis_angle(v3(1.0f, 0.0f, 0.0f), -aoa)));
}

void cam_frustum(const Camera *c, Frustum *fr)
{
    Mat4 vp;
    m4_mul(&vp, &c->view, &c->proj);
    frustum_from_viewproj(fr, &vp);
}

void cam_reset(Camera *c, const Aircraft *ac)
{
    int fixed = c->fixed;
    Vec3 fe = c->fixedEye;
    float fh = c->fixedHdg, fp = c->fixedPitch;
    memset(c, 0, sizeof(*c));
    c->mode = s_startView ? VIEW_CHASE : VIEW_COCKPIT;
    c->fovDeg = s_fovCockpit;
    c->nearM = 1.0f;
    c->farM = scene_far_m();
    c->q = q_ident();
    c->snap = 1;
    c->shakeSeed = 0x5EEDu;
    c->fixed = fixed; c->fixedEye = fe; c->fixedHdg = fh; c->fixedPitch = fp;
    if (ac) { c->eye = ac->fs.pos; c->q = ac->fs.qPath; }
}

void cam_toggle(Camera *c)
{
    c->mode = c->mode == VIEW_COCKPIT ? VIEW_CHASE : VIEW_COCKPIT;
    c->snap = 1;
}

void cam_set_fixed(Camera *c, Vec3 eye, float hdgDeg, float pitchDeg)
{
    c->fixed = 1;
    c->fixedEye = eye;
    c->fixedHdg = hdgDeg;
    c->fixedPitch = pitchDeg;
}

static float shake_rand(Camera *c)                 /* render-only noise, -1..1 (never touches the sim RNG) */
{
    c->shakeSeed = c->shakeSeed * 1664525u + 1013904223u;
    return (float)((c->shakeSeed >> 9) & 0xFFFF) / 32767.5f - 1.0f;
}

/* T10b: smooth buffet noise -1..1 on one axis: three incommensurate sines (render clock), no per-frame jitter */
static float buffet_noise(float t, int axis)
{
    float w = 6.2831853f * s_bufHz, o = axis ? 1.7f : 0.0f;
    return 0.55f * (float)sin(w * t + o) + 0.30f * (float)sin(w * 1.618f * t + 2.1f + o) +
           0.15f * (float)sin(w * 2.414f * t + 4.3f + 2.0f * o);
}

/* buffet amplitude (deg) and speed FOV fraction 0..1 of the player state */
static float speed_cues(const Aircraft *ac, float *fovK)
{
    float kt = ac->fs.V * MS2KT, m = ac->fs.mach, agl, a, b, x;
    *fovK = 0.0f;
    if (!fx_speed_cues()) return 0.0f;
    *fovK = s_fovToKt > s_fovFromKt ? E_CLAMP((kt - s_fovFromKt) / (s_fovToKt - s_fovFromKt), 0.0f, 1.0f) : 0.0f;
    *fovK = *fovK * *fovK * (3.0f - 2.0f * *fovK);
    a = s_bufToKt > s_bufFromKt ? E_CLAMP((kt - s_bufFromKt) / (s_bufToKt - s_bufFromKt), 0.0f, 1.0f) : 0.0f;
    x = (m - 1.0f) / (s_bufMachW > 0.005f ? s_bufMachW : 0.005f);
    b = s_bufSpeed * a * a + s_bufTrans * (float)exp(-x * x);
    agl = ac->fs.pos.y - (g_game.terrain ? world_surface(&g_game, ac->fs.pos.x, ac->fs.pos.z) : 0.0f);
    b *= 1.0f + s_bufLowAlt * E_CLAMP(1.0f - agl / (s_bufLowAltM > 1.0f ? s_bufLowAltM : 1.0f), 0.0f, 1.0f);
    if (ac->fs.onGround) b = 0.0f;
    return b;
}

/* T16: view matrix from an eye and a forward DIRECTION (= m4_look_lh with at - eye = dir). The cockpit used to pass
   at = eye + unit forward: at 5-60 km world coordinates that float sum quantised the direction (0.3-2.5 px of
   frame-to-frame wobble of the whole view and every conformal HUD symbol; found by T15). The direction itself is
   exact, so build from it. */
static void look_dir(Mat4 *out, Vec3 eye, Vec3 dir, Vec3 up)
{
    Vec3 z = v3_norm(dir);
    Vec3 x = v3_norm(v3_cross(up, z));
    Vec3 y = v3_cross(z, x);
    out->m[0][0] = x.x; out->m[0][1] = y.x; out->m[0][2] = z.x; out->m[0][3] = 0.0f;
    out->m[1][0] = x.y; out->m[1][1] = y.y; out->m[1][2] = z.y; out->m[1][3] = 0.0f;
    out->m[2][0] = x.z; out->m[2][1] = y.z; out->m[2][2] = z.z; out->m[2][3] = 0.0f;
    out->m[3][0] = -v3_dot(x, eye); out->m[3][1] = -v3_dot(y, eye); out->m[3][2] = -v3_dot(z, eye); out->m[3][3] = 1.0f;
}

static void set_proj(Camera *c)
{
    int w = 0, h = 0;
    float aspect;
    r_screen_size(&w, &h);
    aspect = (w > 0 && h > 0) ? (float)w / (float)h : 4.0f / 3.0f;
    c->farM = scene_far_at(c->eye.y);               /* T10b: grows with altitude (scene.cpp, the "ball" fix) */
    m4_persp_lh(&c->proj, 2.0f * (float)atan(tan(E_DEG2RAD(c->fovDeg) * 0.5f) / aspect), aspect, c->nearM, c->farM);
}

void cam_update(Camera *c, const Aircraft *ac, float alpha, float frameDt)
{
    Vec3 pos, f, u, at, eye;
    Quat qn;
    float dt = E_CLAMP(frameDt, 0.0f, 0.1f), surf, shake;
    if (!ac) return;
    if (c->fixed) {                                 /* -camat */
        float cp = (float)cos(E_DEG2RAD(c->fixedPitch)), sp = (float)sin(E_DEG2RAD(c->fixedPitch));
        f = v3(cp * (float)sin(E_DEG2RAD(c->fixedHdg)), sp, cp * (float)cos(E_DEG2RAD(c->fixedHdg)));
        c->eye = c->fixedEye;
        c->fovDeg = s_fovChase;
        c->nearM = 1.0f;
        set_proj(c);
        look_dir(&c->view, c->eye, f, v3(0.0f, 1.0f, 0.0f));
        return;
    }
    cam_ac_pose(ac, alpha, &pos, &qn);
    f = q_rotate(qn, v3(0.0f, 0.0f, 1.0f));
    u = q_rotate(qn, v3(0.0f, 1.0f, 0.0f));
    if (!(ac->flags & AC_ALIVE)) {                  /* wreck orbit (DESIGN 6.3) */
        float a;
        if (!c->wreck) { c->wreck = 1; c->wreckT = 0.0f; }
        else c->wreckT += dt;
        a = E_DEG2RAD(c->wreckT * s_wreckRate) + (float)atan2(-f.x, -f.z);
        eye = v3(pos.x + s_wreckDist * (float)sin(a), pos.y + s_wreckUp, pos.z + s_wreckDist * (float)cos(a));
        if (g_game.terrain) {
            surf = world_surface(&g_game, eye.x, eye.z) + 5.0f;
            if (eye.y < surf) eye.y = surf;
        }
        c->eye = eye;
        c->fovDeg = s_fovChase;
        c->nearM = 1.5f;
        set_proj(c);
        m4_look_lh(&c->view, eye, pos, v3(0.0f, 1.0f, 0.0f));
        c->snap = 1;
        return;
    }
    c->wreck = 0;
    shake = s_shakeG * E_CLAMP(ac->fs.n - 7.0f, 0.0f, 2.5f) + s_shakeAB * ac->fs.ab + s_shakeExpl * fx_cam_shake();   /* T22: + explosions */
    {   /* T10b: speed FOV (smoothed) and buffet clock */
        float fk, kf = s_tauFov > 0.001f ? 1.0f - (float)exp(-dt / s_tauFov) : 1.0f;
        c->buffetDeg = speed_cues(ac, &fk);
        if (c->snap) { c->fovSpeedK = fk; c->fovAbK = ac->fs.ab; }
        else c->fovSpeedK += (fk - c->fovSpeedK) * kf;
        c->buffetT += dt;
        if (c->buffetT > 1000.0f) c->buffetT -= 1000.0f;
    }
    if (c->mode == VIEW_COCKPIT) {
        const Mesh *jm = scene_jet_mesh();
        const MeshTag *et = jm ? mesh_tag(jm, "cam_cockpit") : NULL;
        c->eye = v3_add(pos, q_rotate(qn, et ? et->pos : s_eyeTag));
        c->q = qn;
        c->fovDeg = s_fovCockpit + s_fovSpeedCockpit * c->fovSpeedK;
        c->nearM = 1.0f;
        at = f;                                     /* T16: a DIRECTION (look_dir), offsets as before */
        if (shake > 0.0f) {                         /* tiny: the HUD is conformal to this view anyway */
            Vec3 r = q_rotate(qn, v3(1.0f, 0.0f, 0.0f));
            at = v3_madd(v3_madd(at, r, E_DEG2RAD(shake) * shake_rand(c)), u, E_DEG2RAD(shake) * shake_rand(c));
        }
        if (c->buffetDeg > 0.0f) {                  /* T10b buffet: smooth, mostly vertical */
            Vec3 r = q_rotate(qn, v3(1.0f, 0.0f, 0.0f));
            float bd = E_DEG2RAD(c->buffetDeg * s_bufCockpit);
            at = v3_madd(v3_madd(at, u, bd * buffet_noise(c->buffetT, 0)), r, 0.5f * bd * buffet_noise(c->buffetT, 1));
        }
        set_proj(c);
        look_dir(&c->view, c->eye, at, u);
        c->fwd = f; c->up = u;
        c->snap = 0;
        return;
    }
    /* chase */
    if (c->snap) { c->fwd = f; c->up = u; c->snap = 0; }
    else {
        float kr = s_tauRot > 0.001f ? 1.0f - (float)exp(-dt / s_tauRot) : 1.0f;
        float kl = s_tauRoll > 0.001f ? 1.0f - (float)exp(-dt / s_tauRoll) : 1.0f;
        c->fwd = v3_norm(v3_lerp(c->fwd, f, kr));
        c->up = v3_norm(v3_lerp(c->up, u, kl));
    }
    c->up = v3_norm(v3_sub(c->up, v3_scale(c->fwd, v3_dot(c->up, c->fwd))));
    eye = v3_add(v3_madd(pos, c->fwd, -s_chaseDist), v3_scale(c->up, s_chaseUp));
    if (g_game.terrain) {
        surf = world_surface(&g_game, eye.x, eye.z) + s_minAgl;
        if (eye.y < surf) eye.y = surf;
    }
    at = v3_madd(pos, f, s_chaseLook);
    if (shake > 0.0f) {
        float d = v3_len(v3_sub(at, eye)) * E_DEG2RAD(shake);
        Vec3 r = v3_norm(v3_cross(c->up, c->fwd));
        at = v3_madd(v3_madd(at, r, d * shake_rand(c)), c->up, d * shake_rand(c));
    }
    if (c->buffetDeg > 0.0f) {                      /* T10b buffet: smooth, mostly vertical */
        float d = v3_len(v3_sub(at, eye)) * E_DEG2RAD(c->buffetDeg);
        Vec3 r = v3_norm(v3_cross(c->up, c->fwd));
        at = v3_madd(v3_madd(at, c->up, d * buffet_noise(c->buffetT, 0)), r, 0.5f * d * buffet_noise(c->buffetT, 1));
    }
    c->eye = eye;
    c->fovAbK += (ac->fs.ab - c->fovAbK) * (s_tauFov > 0.001f ? 1.0f - (float)exp(-dt / s_tauFov) : 1.0f);
    c->fovDeg = s_fovChase + s_fovAB * c->fovAbK + s_fovSpeed * c->fovSpeedK;
    c->nearM = 1.5f;
    set_proj(c);
    m4_look_lh(&c->view, eye, at, c->up);
}
