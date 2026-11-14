/* e_math.h - vectors, quaternions, matrices, planes, frustum, AABB (DESIGN 2.3, 2.5). Portable. Owner: T01.
   Conventions (DESIGN 2.3): left-handed, +X right, +Y up, +Z forward. Row vectors: v' = v * M, translation in
   row 3, memory layout = D3DMATRIX (m[row][col], m[3][0..2] = translation). Mat3 rows = basis vectors of the
   rotated frame in world space (row 0 right, row 1 up, row 2 forward), so m3_mul(m, v) = v.x*r0 + v.y*r1 + v.z*r2
   (body -> world). Quaternions are (x,y,z,w) unit quaternions; q_rotate(q_mul(a,b), v) = q_rotate(a, q_rotate(b, v)).
   Yaw: about +Y, 0 = facing +Z, positive turns +Z toward +X: q_from_yaw(y) rotates (0,0,1) to (sin y, 0, cos y). */
#ifndef E_MATH_H
#define E_MATH_H

#include "e_base.h"

typedef struct Vec3 { float x, y, z; } Vec3;
typedef struct Quat { float x, y, z, w; } Quat;
typedef struct Mat3 { float m[3][3]; } Mat3;      /* rows = basis vectors right, up, forward */
typedef struct Mat4 { float m[4][4]; } Mat4;      /* D3D layout, row vectors */
typedef struct Plane { float a, b, c, d; } Plane; /* a*x+b*y+c*z+d >= 0 inside */
typedef struct Frustum { Plane p[6]; } Frustum;
typedef struct Aabb { Vec3 lo, hi; } Aabb;

Vec3  v3(float x, float y, float z);
Vec3  v3_add(Vec3 a, Vec3 b);  Vec3 v3_sub(Vec3 a, Vec3 b);  Vec3 v3_scale(Vec3 a, float s);
Vec3  v3_madd(Vec3 a, Vec3 b, float s);            /* a + b*s */
float v3_dot(Vec3 a, Vec3 b);  Vec3 v3_cross(Vec3 a, Vec3 b);
float v3_len(Vec3 a);          Vec3 v3_norm(Vec3 a);          /* norm of ~0 returns (0,1,0) */
Vec3  v3_lerp(Vec3 a, Vec3 b, float t);
Quat  q_ident(void);  Quat q_axis_angle(Vec3 axis, float ang);  Quat q_mul(Quat a, Quat b);
Quat  q_norm(Quat q); Vec3 q_rotate(Quat q, Vec3 v); Quat q_slerp(Quat a, Quat b, float t);
Quat  q_from_yaw(float yaw);  void q_to_mat3(Quat q, Mat3 *out);  Quat q_from_mat3(const Mat3 *m);
Vec3  m3_mul(const Mat3 *m, Vec3 v);              /* body -> world */
Vec3  m3_tmul(const Mat3 *m, Vec3 v);             /* world -> body (transpose) */
void  m3_orthonormalize(Mat3 *m);                 /* keeps forward (row 2), re-derives right and up */
void  m4_ident(Mat4 *m);  void m4_mul(Mat4 *out, const Mat4 *a, const Mat4 *b);   /* out = a*b (apply a first); out may alias */
void  m4_from_m3_pos(Mat4 *out, const Mat3 *r, Vec3 pos);   /* world matrix */
void  m4_look_lh(Mat4 *out, Vec3 eye, Vec3 at, Vec3 up);
void  m4_persp_lh(Mat4 *out, float fovY, float aspect, float zn, float zf);   /* D3D: z/w 0 at zn, 1 at zf; _34 = 1 */
void  m4_inverse_rt(Mat4 *out, const Mat4 *in);   /* rotation+translation only */
Vec3  m4_mul_point(const Mat4 *m, Vec3 p);        /* p * M with w = 1, no divide */
void  frustum_from_viewproj(Frustum *f, const Mat4 *viewProj);   /* planes normalised, pointing inward */
int   frustum_aabb(const Frustum *f, const Aabb *b);            /* 0 out, 1 in/intersect */
int   frustum_sphere(const Frustum *f, Vec3 c, float r);        /* 0 out, 1 in/intersect */
float wrap_pi(float a);                                          /* into [-pi, pi) */

/* ---- additions (T01) ---- */
Vec3  m4_mul_dir(const Mat4 *m, Vec3 d);          /* d * M without translation */
void  m4_mul_vec4(const Mat4 *m, const float in[4], float out[4]);   /* full 4D row-vector product */
void  m3_ident(Mat3 *m);
void  m3_from_yaw(Mat3 *m, float yaw);
float q_dot(Quat a, Quat b);

#endif
