/* tests.cpp - -test dispatcher, check helpers, the `math` and `params` self-tests (DESIGN 16.1). Portable.
   Owner: T01. The math test is the racer's (core math, cfg, chunk files; copied 2026-11-13); area tests live in
   tests_<area>.cpp (one file per owning task). New test: add a row to s_tests (orchestrator approval) or call
   tests_register from main/simh. */
#include "tests.h"
#include "params.h"
#include "replay.h"
#include "e_log.h"
#include "e_file.h"

/* ---------------------------------------------------------------- registry */
typedef struct TestEnt { const char *name; TestFn fn; int portable; } TestEnt;

static TestEnt s_tests[40] = {
    { "math", test_math, 1 },
    { "params", test_params, 1 },
    { "flight", test_flight, 1 },
    { "pilot", test_pilot, 1 },
    { "takeoff", test_takeoff, 1 },
    { "terrain", test_terrain, 1 },
    { "world", test_world, 1 },
    { "radar", test_radar, 1 },
    { "target", test_target, 1 },
    { "missile", test_missile, 1 },
    { "dlz", test_dlz, 1 },
    { "ai", test_ai, 1 },
    { "mission", test_mission, 1 },
    { "determinism", test_determinism, 1 },
    { "replay", test_replay, 1 },
};
static int s_numTests = 15;

int tests_register(const char *name, TestFn fn, int portable)
{
    int i;
    for (i = 0; i < s_numTests; i++)
        if (!e_stricmp(s_tests[i].name, name)) { s_tests[i].fn = fn; s_tests[i].portable = portable; return 1; }
    if (s_numTests >= E_COUNT(s_tests)) return 0;
    s_tests[s_numTests].name = name;
    s_tests[s_numTests].fn = fn;
    s_tests[s_numTests].portable = portable;
    s_numTests++;
    return 1;
}

void tests_list(void)
{
    int i;
    for (i = 0; i < s_numTests; i++)
        log_printf(LOG_INFO, "test: list name=%s portable=%d", s_tests[i].name, s_tests[i].portable);
}

int tests_run(const char *name, const char *arg)
{
    int i, r, total = 0, ran = 0;
    if (!name || !name[0]) return -1;
    if (!e_stricmp(name, "all")) {
        for (i = 0; i < s_numTests; i++) {
            if (!s_tests[i].portable) continue;
            r = s_tests[i].fn(arg);
            if (r == TEST_NOT_IMPL) {
                log_printf(LOG_INFO, "test: %s skipped=not_implemented", s_tests[i].name);
                continue;
            }
            ran++;
            total += r;
        }
        log_printf(LOG_INFO, "test: all done tests=%d fail=%d", ran, total);
        return total;
    }
    for (i = 0; i < s_numTests; i++) {
        if (e_stricmp(s_tests[i].name, name)) continue;
        r = s_tests[i].fn(arg);
        if (r == TEST_NOT_IMPL) log_printf(LOG_INFO, "test: %s result=not_implemented", name);
        return r;
    }
    log_printf(LOG_ERR, "err: test unknown name=%s", name);
    tests_list();
    return -1;
}

/* ---------------------------------------------------------------- check helpers */
static const char *s_cur = "?";
static int s_checks, s_fails;

void t_begin(const char *test)
{
    s_cur = test;
    s_checks = s_fails = 0;
}

int t_check(int ok, const char *what, const char *fmt, ...)
{
    char det[256];
    det[0] = 0;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        if (e_vsnprintf(det, (int)sizeof(det), fmt, ap) < 0) det[sizeof(det) - 1] = 0;
        va_end(ap);
    }
    s_checks++;
    if (!ok) s_fails++;
    log_printf(ok ? LOG_INFO : LOG_ERR, "test: %s %s %s%s%s", s_cur, what, ok ? "ok" : "FAIL", det[0] ? " " : "", det);
    return ok;
}

int t_near(float got, float want, float tol, const char *what)
{
    float d = got - want;
    if (d < 0.0f) d = -d;
    return t_check(d <= tol, what, "got=%.6g want=%.6g tol=%.3g", (double)got, (double)want, (double)tol);
}

int t_end(void)
{
    log_printf(s_fails ? LOG_ERR : LOG_INFO, "test: %s done checks=%d fail=%d", s_cur, s_checks, s_fails);
    return s_fails;
}

/* ---------------------------------------------------------------- math */
static int v3_close(Vec3 a, Vec3 b, float tol)
{
    return (float)fabs(a.x - b.x) <= tol && (float)fabs(a.y - b.y) <= tol && (float)fabs(a.z - b.z) <= tol;
}

static int m4_is_ident(const Mat4 *m, float tol)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            if ((float)fabs(m->m[i][j] - (i == j ? 1.0f : 0.0f)) > tol) return 0;
    return 1;
}

static Quat rand_quat(ERand *r)
{
    Vec3 axis = v3(rnd_range(r, -1.0f, 1.0f), rnd_range(r, -1.0f, 1.0f), rnd_range(r, -1.0f, 1.0f));
    return q_axis_angle(axis, rnd_range(r, -3.0f, 3.0f));
}

static int test_math_quat(void)
{
    ERand r;
    Quat q, q2, a, b;
    Mat3 m;
    Vec3 v, w1, w2;
    int i, ok;
    float worst;

    v = q_rotate(q_from_yaw(E_DEG2RAD(90.0f)), v3(0.0f, 0.0f, 1.0f));
    t_check(v3_close(v, v3(1.0f, 0.0f, 0.0f), 1e-5f), "yaw90_forward_is_plus_x", "v=(%.4f %.4f %.4f)",
            (double)v.x, (double)v.y, (double)v.z);
    v = q_rotate(q_from_yaw(E_DEG2RAD(90.0f)), v3(1.0f, 0.0f, 0.0f));
    t_check(v3_close(v, v3(0.0f, 0.0f, -1.0f), 1e-5f), "yaw90_right_is_minus_z", NULL);

    m3_from_yaw(&m, 0.7f);
    t_check(v3_close(m3_mul(&m, v3(0.0f, 0.0f, 1.0f)), v3((float)sin(0.7f), 0.0f, (float)cos(0.7f)), 1e-5f),
            "m3_from_yaw_forward_row", NULL);

    rnd_seed(&r, 12345);
    worst = 0.0f;
    ok = 1;
    for (i = 0; i < 50; i++) {
        q = rand_quat(&r);
        q_to_mat3(q, &m);
        q2 = q_from_mat3(&m);
        if (q_dot(q, q2) < 0.0f) { q2.x = -q2.x; q2.y = -q2.y; q2.z = -q2.z; q2.w = -q2.w; }
        {
            float d = (float)fabs(q.x - q2.x) + (float)fabs(q.y - q2.y) + (float)fabs(q.z - q2.z) + (float)fabs(q.w - q2.w);
            if (d > worst) worst = d;
        }
        v = v3(rnd_range(&r, -5.0f, 5.0f), rnd_range(&r, -5.0f, 5.0f), rnd_range(&r, -5.0f, 5.0f));
        if (!v3_close(m3_mul(&m, v), q_rotate(q, v), 1e-4f)) ok = 0;
        if (!v3_close(m3_tmul(&m, m3_mul(&m, v)), v, 1e-4f)) ok = 0;
    }
    t_check(worst < 1e-4f, "quat_mat3_roundtrip_x50", "worst=%.3g", (double)worst);
    t_check(ok, "m3_mul_matches_q_rotate_and_tmul_inverts_x50", NULL);

    a = rand_quat(&r); b = rand_quat(&r);
    v = v3(0.3f, -1.2f, 2.5f);
    w1 = q_rotate(q_mul(a, b), v);
    w2 = q_rotate(a, q_rotate(b, v));
    t_check(v3_close(w1, w2, 1e-4f), "q_mul_composes_b_then_a", NULL);

    q = q_slerp(a, b, 0.0f);
    t_check((float)fabs(q_dot(q, a)) > 0.99999f, "slerp_t0_is_a", NULL);
    q = q_slerp(a, b, 1.0f);
    t_check((float)fabs(q_dot(q, b)) > 0.99999f, "slerp_t1_is_b", NULL);
    q = q_slerp(q_ident(), q_from_yaw(1.0f), 0.5f);
    t_check((float)fabs(q_dot(q, q_from_yaw(0.5f))) > 0.99999f, "slerp_half_angle", NULL);

    m3_from_yaw(&m, 0.4f);
    m.m[1][0] += 0.05f; m.m[0][2] -= 0.03f; m.m[2][1] += 0.02f;
    m3_orthonormalize(&m);
    {
        Vec3 r0 = v3(m.m[0][0], m.m[0][1], m.m[0][2]), r1 = v3(m.m[1][0], m.m[1][1], m.m[1][2]);
        Vec3 r2 = v3(m.m[2][0], m.m[2][1], m.m[2][2]);
        float e = (float)fabs(v3_dot(r0, r1)) + (float)fabs(v3_dot(r1, r2)) + (float)fabs(v3_dot(r0, r2)) +
                  (float)fabs(v3_len(r0) - 1.0f) + (float)fabs(v3_len(r1) - 1.0f) + (float)fabs(v3_len(r2) - 1.0f);
        t_check(e < 1e-4f, "m3_orthonormalize", "err=%.3g", (double)e);
        t_check(v3_close(v3_cross(r1, r2), r0, 1e-4f), "m3_rows_left_handed_right_eq_up_x_fwd", NULL);
    }
    return 0;
}

static int test_math_mat4(void)
{
    Mat4 w, wi, p, view, proj, vp;
    Mat3 r;
    Vec3 pos = v3(10.0f, 2.0f, -30.0f), v, e;
    float in[4], out[4];
    Frustum f;
    Aabb b;

    q_to_mat3(q_axis_angle(v3(0.2f, 1.0f, 0.1f), 1.1f), &r);
    m4_from_m3_pos(&w, &r, pos);
    m4_inverse_rt(&wi, &w);
    m4_mul(&p, &w, &wi);
    t_check(m4_is_ident(&p, 1e-4f), "m4_inverse_rt_times_world_is_ident", NULL);
    v = v3(1.0f, 2.0f, 3.0f);
    t_check(v3_close(m4_mul_point(&w, v), v3_add(m3_mul(&r, v), pos), 1e-4f), "m4_mul_point_is_rot_plus_pos", NULL);

    e = v3(5.0f, 3.0f, 5.0f);
    m4_look_lh(&view, e, v3(5.0f, 3.0f, 25.0f), v3(0.0f, 1.0f, 0.0f));
    t_check(v3_close(m4_mul_point(&view, e), v3(0.0f, 0.0f, 0.0f), 1e-4f), "look_eye_to_origin", NULL);
    t_check(v3_close(m4_mul_point(&view, v3(5.0f, 3.0f, 15.0f)), v3(0.0f, 0.0f, 10.0f), 1e-4f), "look_at_on_plus_z", NULL);
    t_check(v3_close(m4_mul_point(&view, v3(6.0f, 3.0f, 5.0f)), v3(1.0f, 0.0f, 0.0f), 1e-4f), "look_world_x_is_view_right", NULL);

    m4_persp_lh(&proj, E_DEG2RAD(60.0f), 4.0f / 3.0f, 1.0f, 450.0f);
    in[0] = 0.0f; in[1] = 0.0f; in[2] = 1.0f; in[3] = 1.0f;
    m4_mul_vec4(&proj, in, out);
    t_near(out[2] / out[3], 0.0f, 1e-5f, "persp_near_maps_to_0");
    t_near(out[3], 1.0f, 1e-6f, "persp_w_is_view_z");
    in[2] = 450.0f;
    m4_mul_vec4(&proj, in, out);
    t_near(out[2] / out[3], 1.0f, 1e-5f, "persp_far_maps_to_1");
    t_near(proj.m[2][3], 1.0f, 0.0f, "persp_m34_is_1_for_wbuffer");

    m4_mul(&vp, &view, &proj);
    frustum_from_viewproj(&f, &vp);
    b.lo = v3(4.0f, 2.0f, 50.0f); b.hi = v3(6.0f, 4.0f, 52.0f);
    t_check(frustum_aabb(&f, &b) == 1, "frustum_box_ahead_in", NULL);
    b.lo = v3(4.0f, 2.0f, -20.0f); b.hi = v3(6.0f, 4.0f, -10.0f);
    t_check(frustum_aabb(&f, &b) == 0, "frustum_box_behind_out", NULL);
    b.lo = v3(-200.0f, 2.0f, 40.0f); b.hi = v3(-150.0f, 4.0f, 50.0f);
    t_check(frustum_aabb(&f, &b) == 0, "frustum_box_far_left_out", NULL);
    b.lo = v3(4.0f, 2.0f, 400.0f); b.hi = v3(6.0f, 4.0f, 500.0f);
    t_check(frustum_aabb(&f, &b) == 1, "frustum_box_across_far_plane_in", NULL);
    b.lo = v3(4.0f, 2.0f, 600.0f); b.hi = v3(6.0f, 4.0f, 610.0f);
    t_check(frustum_aabb(&f, &b) == 0, "frustum_box_beyond_far_out", NULL);
    t_check(frustum_sphere(&f, v3(5.0f, 3.0f, 100.0f), 1.0f) == 1, "frustum_sphere_ahead_in", NULL);
    t_check(frustum_sphere(&f, v3(5.0f, 3.0f, 5.5f), 1.0f) == 1, "frustum_sphere_across_near_in", NULL);
    t_check(frustum_sphere(&f, v3(5.0f, 3.0f, 2.0f), 1.0f) == 0, "frustum_sphere_behind_near_out", NULL);
    return 0;
}

static int test_math_misc(void)
{
    ERand r;
    u32 got[4];
    int i, ok;
    float lo = 1.0f, hi = 0.0f, x;
    Vec3 n;

    n = v3_norm(v3(0.0f, 0.0f, 0.0f));
    t_check(v3_close(n, v3(0.0f, 1.0f, 0.0f), 0.0f), "v3_norm_zero_is_up", NULL);
    t_near(v3_len(v3_norm(v3(3.0f, -4.0f, 12.0f))), 1.0f, 1e-6f, "v3_norm_unit");
    t_check(v3_close(v3_cross(v3(1.0f, 0.0f, 0.0f), v3(0.0f, 1.0f, 0.0f)), v3(0.0f, 0.0f, 1.0f), 0.0f), "cross_x_y_is_z", NULL);
    t_near(wrap_pi(3.0f * E_PI + 0.25f), -E_PI + 0.25f, 1e-5f, "wrap_pi_3pi");
    t_near(wrap_pi(-0.5f), -0.5f, 0.0f, "wrap_pi_identity");
    t_near(wrap_pi(-7.0f), -7.0f + 2.0f * E_PI, 1e-5f, "wrap_pi_neg");

    rnd_seed(&r, 1);
    for (i = 0; i < 4; i++) got[i] = rnd_u32(&r);
    t_check(got[0] == 0xae1b7e04u && got[1] == 0xfaa843c9u && got[2] == 0xa8f24e81u && got[3] == 0xc561dc10u,
            "rng_seed1_sequence", "got=%08x %08x %08x %08x", got[0], got[1], got[2], got[3]);
    rnd_seed(&r, 7);
    ok = 1;
    for (i = 0; i < 10000; i++) {
        x = rnd_f01(&r);
        if (x < 0.0f || x >= 1.0f) ok = 0;
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    t_check(ok && lo < 0.01f && hi > 0.99f, "rng_f01_range", "lo=%.4f hi=%.4f", (double)lo, (double)hi);
    t_check(e_crc32("123456789", 9) == 0xCBF43926u, "crc32_check_value", NULL);
    t_check(e_hash("a", 1, E_HASH_INIT) == 0xE40C292Cu, "fnv1a_a", NULL);
    t_check(e_hash("b", 1, e_hash("a", 1, E_HASH_INIT)) == e_hash("ab", 2, E_HASH_INIT), "fnv1a_chainable", NULL);
    t_check(e_ftoi(2.4f) == 2 && e_ftoi(-2.6f) == -3 && e_ftoi(7.0f) == 7, "e_ftoi_round", NULL);
    return 0;
}

static int test_math_cfg(void)
{
    static const char text[] =
        "; jet test ini\r\n"
        "top=1\n"
        "[Racer]\r\n"
        "  width = 640 \r\n"
        "# comment inside\n"
        "name=V10 GT\n"
        "ratios=2.66, 1.78,1.30 ; 1.00\n"
        "\n"
        "[keyboard]\n"
        "steerRate=2.5\n"
        "WIDTH=7\n";
    char path[E_PATH], *saved;
    Cfg *c, *c2;
    float fl[8];
    int n, size;

    c = cfg_parse(text, (int)strlen(text));
    t_check(cfg_int(c, "", "top", 0) == 1, "cfg_global_section", NULL);
    t_check(cfg_int(c, "racer", "WIDTH", 0) == 640, "cfg_trim_and_case", NULL);
    t_check(!strcmp(cfg_str(c, "Racer", "name", ""), "V10 GT"), "cfg_str_with_space", NULL);
    n = cfg_floats(c, "Racer", "ratios", fl, 8);
    t_check(n == 4 && (float)fabs(fl[2] - 1.30f) < 1e-6f, "cfg_floats", "n=%d", n);
    t_check(cfg_int(c, "keyboard", "width", 0) == 7 && cfg_int(c, "Racer", "missing", -5) == -5, "cfg_sections_separate_and_default", NULL);
    t_check(cfg_int(c, "Racer", "name", 9) == 9, "cfg_int_rejects_text", NULL);
    cfg_set_int(c, "Racer", "height", 480);
    cfg_set_float(c, "keyboard", "steerRate", 3.25f);
    cfg_set_str(c, "new", "k", "v");
    base_path(path, (int)sizeof(path), "test_cfg.tmp");
    t_check(cfg_save(c, path), "cfg_save", "path=%s", path);
    c2 = cfg_load(path);
    t_check(cfg_int(c2, "Racer", "height", 0) == 480 && cfg_int(c2, "Racer", "width", 0) == 640 &&
            (float)fabs(cfg_float(c2, "keyboard", "steerRate", 0.0f) - 3.25f) < 1e-6f &&
            !strcmp(cfg_str(c2, "new", "k", ""), "v") && cfg_num_keys(c2) == cfg_num_keys(c),
            "cfg_roundtrip_values", "keys=%d/%d", cfg_num_keys(c2), cfg_num_keys(c));
    saved = (char *)file_load(path, &size);
    t_check(saved && strstr(saved, "; jet test ini\r\ntop=1\r\n[Racer]\r\nwidth=640\r\n# comment inside\r\n"
                                   "name=V10 GT\r\nratios=2.66, 1.78,1.30 ; 1.00\r\nheight=480\r\n\r\n[keyboard]\r\n") != NULL,
            "cfg_save_keeps_order_comments_crlf", NULL);
    if (saved) free(saved);
    remove(path);
    cfg_free(c);
    cfg_free(c2);
    c = cfg_load(path);                              /* deleted: empty, never NULL */
    t_check(c != NULL && cfg_num_keys(c) == 0, "cfg_missing_file_is_empty", NULL);
    cfg_free(c);
    return 0;
}

static int test_math_file(void)
{
    u8 buf[64];
    ChunkFile cf;
    const u8 *p;
    char out[E_PATH], id[4];
    int sz, i;
    u32 crc;

    memset(buf, 0, sizeof(buf));
    memcpy(buf, "RTRK", 4); buf[4] = 1; buf[8] = 2;
    memcpy(buf + 16, "INFO", 4); buf[20] = 3; buf[24] = 'a'; buf[25] = 'b'; buf[26] = 'c';  /* size 3, pad to 4 */
    memcpy(buf + 28, "SKY ", 4); buf[32] = 4; buf[36] = 9;
    crc = e_crc32(buf + 16, 40 - 16);
    for (i = 0; i < 4; i++) buf[12 + i] = (u8)(crc >> (8 * i));
    t_check(chunk_open(&cf, buf, 40, E_TAG('R', 'T', 'R', 'K')) && cf.version == 1 && chunk_count(&cf) == 2,
            "chunk_open_header_crc", NULL);
    p = chunk_find(&cf, "SKY ", &sz);
    t_check(p && sz == 4 && p[0] == 9, "chunk_find_after_padding", NULL);
    t_check(chunk_find(&cf, "NONE", &sz) == NULL, "chunk_find_absent", NULL);
    p = chunk_at(&cf, 0, id, &sz);
    t_check(p && !memcmp(id, "INFO", 4) && sz == 3 && p[2] == 'c', "chunk_at_0", NULL);
    buf[30] ^= 1;
    t_check(!chunk_open(&cf, buf, 40, E_TAG('R', 'T', 'R', 'K')), "chunk_open_rejects_bad_crc", NULL);

    path_join(out, (int)sizeof(out), "dir", "tracks/x.trk");
#if defined(_WIN32)
    t_check(!strcmp(out, "dir\\tracks\\x.trk"), "path_join_native", "out=%s", out);
#else
    t_check(!strcmp(out, "dir/tracks/x.trk"), "path_join_native", "out=%s", out);
#endif
    path_join(out, (int)sizeof(out), "dir", "C:\\abs.txt");
#if defined(_WIN32)
    t_check(!strcmp(out, "C:\\abs.txt"), "path_join_absolute", "out=%s", out);
#else
    t_check(!strcmp(out, "C:/abs.txt"), "path_join_absolute", "out=%s", out);
#endif
    return 0;
}


int test_math(const char *arg)
{
    (void)arg;
    t_begin("math");
    test_math_quat();
    test_math_mat4();
    test_math_misc();
    test_math_cfg();
    test_math_file();
    t_near(speed_of_sound(0.0f), 340.3f, 1e-3f, "speed_of_sound_sea_level");
    t_near(speed_of_sound(12000.0f), 295.7f, 1e-3f, "speed_of_sound_above_11km");
    return t_end();
}

/* ---------------------------------------------------------------- params (DESIGN 2.5) */
static float s_pf; static int s_pi, s_pb; static PTable s_pt; static char s_ps[PARAM_STR]; static int s_pr;
static const Param s_ptab[] = {
    { "ptest", "f", P_FLOAT, &s_pf, "1.5", 0.0f, 10.0f, "a float" },
    { "ptest", "i", P_INT, &s_pi, "3", 1.0f, 5.0f, "an int" },
    { "ptest", "b", P_BOOL, &s_pb, "1", 0.0f, 1.0f, "a bool" },
    { "ptest", "t", P_TABLE, &s_pt, "150:110,250:180,400:220", 0.0f, 300.0f, "a table" },
    { "ptest2", "s", P_STR, s_ps, "low", 0.0f, 0.0f, "a string" },
    { "ptest2", "r", P_INT, &s_pr, "640", 0.0f, 0.0f, "restart only", P_RESTART },
};

static int write_text(const char *path, const char *text)
{
    return file_save(path, text, (int)strlen(text));
}

int test_params(const char *arg)
{
    PTable t;
    Cfg *c;
    char path[E_PATH], buf[4096], *txt;
    int n, size;
    u32 h1, h2;
    (void)arg;
    t_begin("params");

    /* tables */
    t_check(ptable_parse("150:110, 250:180 ,400:220", &t) && t.n == 3, "ptable_parse", "n=%d", t.n);
    t_near(ptable_eval(&t, 100.0f), 110.0f, 1e-4f, "ptable_clamp_low");
    t_near(ptable_eval(&t, 200.0f), 145.0f, 1e-4f, "ptable_lerp");
    t_near(ptable_eval(&t, 900.0f), 220.0f, 1e-4f, "ptable_clamp_high");
    t_check(!ptable_parse("1:2,1:3", &t) && !ptable_parse("abc", &t) && !ptable_parse("1;2", &t) &&
            !ptable_parse("", &t) && !ptable_parse("1:2,3", &t), "ptable_rejects_bad", NULL);
    ptable_parse("1:2,3.5:-4", &t);
    ptable_str(&t, buf, (int)sizeof(buf));
    t_check(!strcmp(buf, "1:2,3.5:-4"), "ptable_str", "got=%s", buf);

    /* registry with a private table (the real registrations are restored at the end) */
    params_clear();
    params_add(s_ptab, E_COUNT(s_ptab));
    t_check(params_count() == 6 && s_pf == 1.5f && s_pi == 3 && s_pb == 1 && s_pt.n == 3 && !strcmp(s_ps, "low") &&
            s_pr == 640, "defaults_after_add", NULL);
    c = cfg_parse("[ptest]\nf=20\ni=x\nb=0\nt=100:50,200:999\n[ptest2]\ns=high\nr=800\n",
                  (int)strlen("[ptest]\nf=20\ni=x\nb=0\nt=100:50,200:999\n[ptest2]\ns=high\nr=800\n"));
    n = params_load(c);
    t_check(s_pf == 10.0f, "float_clamped_to_hi", "f=%g", (double)s_pf);
    t_check(s_pi == 3, "bad_int_keeps_value", "i=%d", s_pi);
    t_check(s_pb == 0 && !strcmp(s_ps, "high") && s_pr == 800, "bool_str_restartkey_at_start", NULL);
    t_check(s_pt.n == 2 && s_pt.y[1] == 300.0f, "table_y_clamped", "y1=%g", (double)s_pt.y[1]);
    t_check(n == 5, "load_changed_count", "n=%d", n);
    cfg_free(c);

    /* dumpini: defaults + help, CRLF */
    base_path(path, (int)sizeof(path), "test_params.tmp");
    t_check(params_dump_ini(path, NULL), "dumpini_write", "path=%s", path);
    txt = (char *)file_load(path, &size);
    t_check(txt && strstr(txt, "\r\n[ptest]\r\n; a float (0..10)\r\nf=1.5\r\n") && strstr(txt, "[ptest2]\r\n") &&
            strstr(txt, "t=150:110,250:180,400:220\r\n") && strstr(txt, "[restart]") && !strstr(txt, "f=10\r\n"),
            "dumpini_content_crlf", NULL);
    if (txt) {
        int i, lf = 0, crlf = 0;
        for (i = 0; i < size; i++) if (txt[i] == '\n') { lf++; if (i > 0 && txt[i - 1] == '\r') crlf++; }
        t_check(lf > 0 && lf == crlf, "dumpini_all_lines_crlf", "lf=%d crlf=%d", lf, crlf);
        free(txt);
    }
    c = cfg_load(path);
    n = params_load(c);
    t_check(s_pf == 1.5f && s_pi == 3 && s_pb == 1 && s_pt.n == 3 && !strcmp(s_ps, "low"), "dumpini_loads_defaults",
            "changed=%d", n);

    /* F5 reload: edit the file, reload, only the edited key changes; restart keys stay */
    write_text(path, "[ptest]\r\nf=2.25\r\ni=3\r\nb=1\r\nt=150:110,250:180,400:220\r\n[ptest2]\r\ns=low\r\nr=1024\r\n");
    cfg_free(c);
    c = cfg_load(path);
    params_load(c);
    write_text(path, "[ptest]\r\nf=4\r\ni=3\r\nb=1\r\nt=150:110,250:180,400:220\r\n[ptest2]\r\ns=low\r\nr=2048\r\n");
    n = params_reload(c, path);
    t_check(n == 1 && s_pf == 4.0f, "reload_changed_one", "n=%d f=%g", n, (double)s_pf);
    t_check(s_pr == 1024, "reload_keeps_restart_key", "r=%d", s_pr);
    t_check(params_reload_count() >= 1 && params_last_changed() == 1, "reload_counters", NULL);
    write_text(path, "[ptest]\r\ni=4\r\n");          /* f removed -> back to default */
    n = params_reload(c, path);
    t_check(n == 2 && s_pf == 1.5f && s_pi == 4, "reload_removed_key_is_default", "n=%d f=%g i=%d", n, (double)s_pf, s_pi);
    t_check(cfg_reload(c, path) == 0, "cfg_reload_unchanged_is_0", NULL);
    remove(path);
    t_check(params_reload(c, path) == -1 && s_pi == 4, "reload_missing_file_keeps_values", NULL);
    cfg_free(c);

    /* snapshot / restore (replays) */
    h1 = params_hash();
    n = params_snapshot(buf, (int)sizeof(buf));
    t_check(n > 0 && strstr(buf, "ptest.t=150:110,250:180,400:220\n") != NULL, "snapshot_text", "len=%d", n);
    s_pf = 9.0f; s_pi = 1; s_pt.n = 1;
    h2 = params_hash();
    t_check(params_restore(buf, n) == 6 && params_hash() == h1 && h1 != h2 && s_pf == 1.5f, "restore_roundtrip", NULL);
    t_check(params_snapshot(buf, 8) == -1, "snapshot_too_small", NULL);

    params_clear();
    params_register_all();
    log_printf(LOG_INFO, "test: params registry restored keys=%d", params_count());
    return t_end();
}
