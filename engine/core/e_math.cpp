/* e_math.cpp - see e_math.h for conventions. Portable. Owner: T01. */
#include "e_math.h"

Vec3 v3(float x, float y, float z) { Vec3 r; r.x = x; r.y = y; r.z = z; return r; }
Vec3 v3_add(Vec3 a, Vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
Vec3 v3_sub(Vec3 a, Vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
Vec3 v3_scale(Vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
Vec3 v3_madd(Vec3 a, Vec3 b, float s) { return v3(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
float v3_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 v3_cross(Vec3 a, Vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
float v3_len(Vec3 a) { return (float)sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

Vec3 v3_norm(Vec3 a)
{
    float l = v3_len(a);
    if (l < 1e-12f) return v3(0.0f, 1.0f, 0.0f);
    return v3_scale(a, 1.0f / l);
}

Vec3 v3_lerp(Vec3 a, Vec3 b, float t) { return v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t); }

/* ---- quaternions */
Quat q_ident(void) { Quat q; q.x = q.y = q.z = 0.0f; q.w = 1.0f; return q; }

Quat q_axis_angle(Vec3 axis, float ang)
{
    Quat q;
    Vec3 n = v3_norm(axis);
    float s = (float)sin(ang * 0.5f);
    q.x = n.x * s; q.y = n.y * s; q.z = n.z * s;
    q.w = (float)cos(ang * 0.5f);
    return q;
}

Quat q_mul(Quat a, Quat b)
{
    Quat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

float q_dot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

Quat q_norm(Quat q)
{
    float l = (float)sqrt(q_dot(q, q));
    if (l < 1e-12f) return q_ident();
    l = 1.0f / l;
    q.x *= l; q.y *= l; q.z *= l; q.w *= l;
    return q;
}

Vec3 q_rotate(Quat q, Vec3 v)
{
    Vec3 u = v3(q.x, q.y, q.z);
    Vec3 t = v3_scale(v3_cross(u, v), 2.0f);       /* t = 2 u x v; v' = v + w t + u x t */
    return v3_add(v3_add(v, v3_scale(t, q.w)), v3_cross(u, t));
}

Quat q_slerp(Quat a, Quat b, float t)
{
    float d = q_dot(a, b), wa, wb;
    Quat r;
    if (d < 0.0f) { d = -d; b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }   /* shortest path */
    if (d > 0.9995f) {
        wa = 1.0f - t; wb = t;                      /* nearly equal: normalised lerp */
    } else {
        float th = (float)acos(d), s = (float)sin(th);
        wa = (float)sin((1.0f - t) * th) / s;
        wb = (float)sin(t * th) / s;
    }
    r.x = a.x * wa + b.x * wb; r.y = a.y * wa + b.y * wb;
    r.z = a.z * wa + b.z * wb; r.w = a.w * wa + b.w * wb;
    return q_norm(r);
}

Quat q_from_yaw(float yaw) { return q_axis_angle(v3(0.0f, 1.0f, 0.0f), yaw); }

void q_to_mat3(Quat q, Mat3 *o)
{
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    /* rows = rotated basis vectors (transpose of the column-vector rotation matrix) */
    o->m[0][0] = 1.0f - 2.0f * (yy + zz); o->m[0][1] = 2.0f * (xy + wz);        o->m[0][2] = 2.0f * (xz - wy);
    o->m[1][0] = 2.0f * (xy - wz);        o->m[1][1] = 1.0f - 2.0f * (xx + zz); o->m[1][2] = 2.0f * (yz + wx);
    o->m[2][0] = 2.0f * (xz + wy);        o->m[2][1] = 2.0f * (yz - wx);        o->m[2][2] = 1.0f - 2.0f * (xx + yy);
}

Quat q_from_mat3(const Mat3 *m)
{
    /* R[i][j] = m[j][i] (column-vector rotation matrix) */
    float r00 = m->m[0][0], r11 = m->m[1][1], r22 = m->m[2][2];
    float r01 = m->m[1][0], r10 = m->m[0][1], r02 = m->m[2][0], r20 = m->m[0][2], r12 = m->m[2][1], r21 = m->m[1][2];
    float tr = r00 + r11 + r22, s;
    Quat q;
    if (tr > 0.0f) {
        s = (float)sqrt(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s; q.x = (r21 - r12) / s; q.y = (r02 - r20) / s; q.z = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        s = (float)sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        q.w = (r21 - r12) / s; q.x = 0.25f * s; q.y = (r01 + r10) / s; q.z = (r02 + r20) / s;
    } else if (r11 > r22) {
        s = (float)sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        q.w = (r02 - r20) / s; q.x = (r01 + r10) / s; q.y = 0.25f * s; q.z = (r12 + r21) / s;
    } else {
        s = (float)sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        q.w = (r10 - r01) / s; q.x = (r02 + r20) / s; q.y = (r12 + r21) / s; q.z = 0.25f * s;
    }
    return q_norm(q);
}

/* ---- Mat3 */
void m3_ident(Mat3 *m)
{
    memset(m, 0, sizeof(*m));
    m->m[0][0] = m->m[1][1] = m->m[2][2] = 1.0f;
}

void m3_from_yaw(Mat3 *m, float yaw) { q_to_mat3(q_from_yaw(yaw), m); }

Vec3 m3_mul(const Mat3 *m, Vec3 v)
{
    return v3(v.x * m->m[0][0] + v.y * m->m[1][0] + v.z * m->m[2][0],
              v.x * m->m[0][1] + v.y * m->m[1][1] + v.z * m->m[2][1],
              v.x * m->m[0][2] + v.y * m->m[1][2] + v.z * m->m[2][2]);
}

Vec3 m3_tmul(const Mat3 *m, Vec3 v)
{
    return v3(v.x * m->m[0][0] + v.y * m->m[0][1] + v.z * m->m[0][2],
              v.x * m->m[1][0] + v.y * m->m[1][1] + v.z * m->m[1][2],
              v.x * m->m[2][0] + v.y * m->m[2][1] + v.z * m->m[2][2]);
}

void m3_orthonormalize(Mat3 *m)
{
    Vec3 f = v3_norm(v3(m->m[2][0], m->m[2][1], m->m[2][2]));
    Vec3 u = v3(m->m[1][0], m->m[1][1], m->m[1][2]);
    Vec3 r = v3_norm(v3_cross(u, f));
    u = v3_cross(f, r);
    m->m[0][0] = r.x; m->m[0][1] = r.y; m->m[0][2] = r.z;
    m->m[1][0] = u.x; m->m[1][1] = u.y; m->m[1][2] = u.z;
    m->m[2][0] = f.x; m->m[2][1] = f.y; m->m[2][2] = f.z;
}

/* ---- Mat4 */
void m4_ident(Mat4 *m)
{
    memset(m, 0, sizeof(*m));
    m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1.0f;
}

void m4_mul(Mat4 *out, const Mat4 *a, const Mat4 *b)
{
    Mat4 r;
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
    *out = r;
}

void m4_from_m3_pos(Mat4 *out, const Mat3 *r, Vec3 pos)
{
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) out->m[i][j] = r->m[i][j];
        out->m[i][3] = 0.0f;
    }
    out->m[3][0] = pos.x; out->m[3][1] = pos.y; out->m[3][2] = pos.z; out->m[3][3] = 1.0f;
}

void m4_look_lh(Mat4 *out, Vec3 eye, Vec3 at, Vec3 up)
{
    Vec3 z = v3_norm(v3_sub(at, eye));
    Vec3 x = v3_norm(v3_cross(up, z));
    Vec3 y = v3_cross(z, x);
    out->m[0][0] = x.x; out->m[0][1] = y.x; out->m[0][2] = z.x; out->m[0][3] = 0.0f;
    out->m[1][0] = x.y; out->m[1][1] = y.y; out->m[1][2] = z.y; out->m[1][3] = 0.0f;
    out->m[2][0] = x.z; out->m[2][1] = y.z; out->m[2][2] = z.z; out->m[2][3] = 0.0f;
    out->m[3][0] = -v3_dot(x, eye); out->m[3][1] = -v3_dot(y, eye); out->m[3][2] = -v3_dot(z, eye); out->m[3][3] = 1.0f;
}

void m4_persp_lh(Mat4 *out, float fovY, float aspect, float zn, float zf)
{
    float h = 1.0f / (float)tan(fovY * 0.5f);
    float q = zf / (zf - zn);
    memset(out, 0, sizeof(*out));
    out->m[0][0] = h / aspect;
    out->m[1][1] = h;
    out->m[2][2] = q;
    out->m[2][3] = 1.0f;
    out->m[3][2] = -zn * q;
}

void m4_inverse_rt(Mat4 *out, const Mat4 *in)
{
    Mat4 r;
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) r.m[i][j] = in->m[j][i];
        r.m[i][3] = 0.0f;
    }
    for (j = 0; j < 3; j++)
        r.m[3][j] = -(in->m[3][0] * in->m[j][0] + in->m[3][1] * in->m[j][1] + in->m[3][2] * in->m[j][2]);
    r.m[3][3] = 1.0f;
    *out = r;
}

Vec3 m4_mul_point(const Mat4 *m, Vec3 p)
{
    return v3(p.x * m->m[0][0] + p.y * m->m[1][0] + p.z * m->m[2][0] + m->m[3][0],
              p.x * m->m[0][1] + p.y * m->m[1][1] + p.z * m->m[2][1] + m->m[3][1],
              p.x * m->m[0][2] + p.y * m->m[1][2] + p.z * m->m[2][2] + m->m[3][2]);
}

Vec3 m4_mul_dir(const Mat4 *m, Vec3 d)
{
    return v3(d.x * m->m[0][0] + d.y * m->m[1][0] + d.z * m->m[2][0],
              d.x * m->m[0][1] + d.y * m->m[1][1] + d.z * m->m[2][1],
              d.x * m->m[0][2] + d.y * m->m[1][2] + d.z * m->m[2][2]);
}

void m4_mul_vec4(const Mat4 *m, const float in[4], float out[4])
{
    float r[4];
    int j;
    for (j = 0; j < 4; j++)
        r[j] = in[0] * m->m[0][j] + in[1] * m->m[1][j] + in[2] * m->m[2][j] + in[3] * m->m[3][j];
    for (j = 0; j < 4; j++) out[j] = r[j];
}

/* ---- frustum (Gribb/Hartmann, D3D clip space 0 <= z <= w) */
static void set_plane(Plane *p, float a, float b, float c, float d)
{
    float l = (float)sqrt(a * a + b * b + c * c);
    if (l < 1e-12f) l = 1.0f;
    l = 1.0f / l;
    p->a = a * l; p->b = b * l; p->c = c * l; p->d = d * l;
}

void frustum_from_viewproj(Frustum *f, const Mat4 *vp)
{
    const float (*m)[4] = vp->m;
    /* column j = (m[0][j], m[1][j], m[2][j], m[3][j]) */
    set_plane(&f->p[0], m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0]);   /* left */
    set_plane(&f->p[1], m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]);   /* right */
    set_plane(&f->p[2], m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1]);   /* bottom */
    set_plane(&f->p[3], m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]);   /* top */
    set_plane(&f->p[4], m[0][2], m[1][2], m[2][2], m[3][2]);                                           /* near */
    set_plane(&f->p[5], m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2]);   /* far */
}

int frustum_aabb(const Frustum *f, const Aabb *b)
{
    int i;
    for (i = 0; i < 6; i++) {
        const Plane *p = &f->p[i];
        float x = p->a >= 0.0f ? b->hi.x : b->lo.x;     /* corner furthest along the normal */
        float y = p->b >= 0.0f ? b->hi.y : b->lo.y;
        float z = p->c >= 0.0f ? b->hi.z : b->lo.z;
        if (p->a * x + p->b * y + p->c * z + p->d < 0.0f) return 0;
    }
    return 1;
}

int frustum_sphere(const Frustum *f, Vec3 c, float r)
{
    int i;
    for (i = 0; i < 6; i++) {
        const Plane *p = &f->p[i];
        if (p->a * c.x + p->b * c.y + p->c * c.z + p->d < -r) return 0;
    }
    return 1;
}

float wrap_pi(float a)
{
    const float twoPi = 2.0f * E_PI;
    if (a >= -E_PI && a < E_PI) return a;
    a = (float)fmod(a + E_PI, twoPi);
    if (a < 0.0f) a += twoPi;
    a -= E_PI;
    if (a >= E_PI) a -= twoPi;                       /* float rounding at the upper edge */
    return a;
}
