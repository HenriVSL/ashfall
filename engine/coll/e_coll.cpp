/* e_coll.cpp - collision world (DESIGN 2.5, 5.3, 5.4). Portable. Owner: T06.
   Triangles live in a sparse 4 m bucket grid (open-addressing hash of non-empty buckets, so a 4 km world costs
   memory only where there are triangles); walls in a sparse 16 m grid. All allocation happens at create time.
   Ray-triangle: stored plane + three in-plane edge planes (PHYSICS.md 2.4 cheap path), ~25 flops per triangle. */
#include "e_coll.h"
#include "e_log.h"

enum { TRI_CELL_M = 4, WALL_CELL_M = 16 };
#define EDGE_EPS 1e-4f                              /* 0.1 mm: closes shared edges */
#define WALL_END_MARGIN 0.25f

typedef struct CTri {                               /* internal, ray-test friendly */
    Vec3 n; float d;
    Vec3 en[3]; float eo[3];                        /* inside: dot(en[i], p) >= eo[i] - EDGE_EPS */
    int surface;
} CTri;

typedef struct CBucket { s32 bx, bz; s32 first, count; } CBucket;   /* first == -1: empty slot */

typedef struct CGrid {
    float cell, inv;
    int cap;                                        /* power of two */
    CBucket *b;
    int *idx; int nidx;
} CGrid;

struct CollWorld {
    int isPlane; float planeY, planeHalf; int planeSurf;
    int ntri; CollTri *tri; CTri *ct;
    CGrid tg;
    int nwall; WallSeg *wall;
    CGrid wg;
    int nbox; CollBox *box;
};

/* ------------------------------------------------------------------ helpers */
static int floor_i(float x)
{
    int i = (int)x;
    if ((float)i > x) i--;
    return i;
}

static u32 cell_hash(s32 bx, s32 bz)
{
    u32 h = (u32)bx * 73856093u ^ (u32)bz * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

static const CBucket *grid_find(const CGrid *g, s32 bx, s32 bz)
{
    u32 m, i;
    if (!g->b) return NULL;
    m = (u32)g->cap - 1u;
    i = cell_hash(bx, bz) & m;
    for (;;) {
        const CBucket *b = &g->b[i];
        if (b->first < 0) return NULL;
        if (b->bx == bx && b->bz == bz) return b;
        i = (i + 1u) & m;
    }
}

static CBucket *grid_insert(CGrid *g, s32 bx, s32 bz)  /* table must have free slots */
{
    u32 m = (u32)g->cap - 1u, i = cell_hash(bx, bz) & m;
    for (;;) {
        CBucket *b = &g->b[i];
        if (b->first < 0) { b->bx = bx; b->bz = bz; b->first = 0; b->count = 0; return b; }
        if (b->bx == bx && b->bz == bz) return b;
        i = (i + 1u) & m;
    }
}

/* build a sparse grid over items given by their XZ boxes */
static int grid_build(CGrid *g, float cell, int n, const float *box /* n*4: x0 z0 x1 z1 */)
{
    int i, pairs = 0, cap, used = 0, x, z;
    memset(g, 0, sizeof(*g));
    g->cell = cell;
    g->inv = 1.0f / cell;
    if (n <= 0) return 1;
    for (i = 0; i < n; i++) {
        int x0 = floor_i(box[i * 4 + 0] * g->inv), z0 = floor_i(box[i * 4 + 1] * g->inv);
        int x1 = floor_i(box[i * 4 + 2] * g->inv), z1 = floor_i(box[i * 4 + 3] * g->inv);
        pairs += (x1 - x0 + 1) * (z1 - z0 + 1);
    }
    /* distinct buckets <= pairs; size the table for load <= 0.5 of a cheap upper bound, then count */
    cap = 64;
    while (cap < pairs * 2 && cap < (1 << 24)) cap <<= 1;
    g->cap = cap;
    g->b = (CBucket *)malloc(sizeof(CBucket) * (size_t)cap);
    g->idx = (int *)malloc(sizeof(int) * (size_t)(pairs > 0 ? pairs : 1));
    if (!g->b || !g->idx) return 0;
    for (i = 0; i < cap; i++) { g->b[i].first = -1; g->b[i].count = 0; }
    for (i = 0; i < n; i++) {
        int x0 = floor_i(box[i * 4 + 0] * g->inv), z0 = floor_i(box[i * 4 + 1] * g->inv);
        int x1 = floor_i(box[i * 4 + 2] * g->inv), z1 = floor_i(box[i * 4 + 3] * g->inv);
        for (z = z0; z <= z1; z++)
            for (x = x0; x <= x1; x++) grid_insert(g, x, z)->count++;
    }
    for (i = 0; i < cap; i++) {                     /* prefix sums; reuse count as a fill cursor */
        if (g->b[i].first < 0) continue;
        g->b[i].first = used;
        used += g->b[i].count;
        g->b[i].count = 0;
    }
    for (i = 0; i < n; i++) {
        int x0 = floor_i(box[i * 4 + 0] * g->inv), z0 = floor_i(box[i * 4 + 1] * g->inv);
        int x1 = floor_i(box[i * 4 + 2] * g->inv), z1 = floor_i(box[i * 4 + 3] * g->inv);
        for (z = z0; z <= z1; z++)
            for (x = x0; x <= x1; x++) {
                CBucket *b = grid_insert(g, x, z);
                g->idx[b->first + b->count++] = i;
            }
    }
    g->nidx = used;
    return 1;
}

static void grid_free(CGrid *g)
{
    free(g->b);
    free(g->idx);
    memset(g, 0, sizeof(*g));
}

/* ------------------------------------------------------------------ triangles */
static void tri_prepare(CollTri *t, CTri *c)
{
    Vec3 e0, e1, e2, n;
    float l;
    e0 = v3_sub(t->b, t->a);
    e1 = v3_sub(t->c, t->a);
    n = v3_cross(e0, e1);
    if (n.y < 0.0f) {                               /* make the normal face up: swap b and c */
        Vec3 tmp = t->b; t->b = t->c; t->c = tmp;
        n = v3_scale(n, -1.0f);
    }
    l = v3_len(n);
    if (l < 1e-12f) {                               /* degenerate: never hit */
        memset(c, 0, sizeof(*c));
        c->n = v3(0.0f, 1.0f, 0.0f);
        c->d = 0.0f;
        c->eo[0] = c->eo[1] = c->eo[2] = 1e30f;
        t->n = c->n; t->d = 0.0f;
        c->surface = t->surface;
        return;
    }
    n = v3_scale(n, 1.0f / l);
    t->n = n;
    t->d = -v3_dot(n, t->a);
    c->n = n;
    c->d = t->d;
    /* edge planes: inward normals in the triangle plane (n x edge points inward for this winding) */
    e0 = v3_sub(t->b, t->a);
    e1 = v3_sub(t->c, t->b);
    e2 = v3_sub(t->a, t->c);
    c->en[0] = v3_norm(v3_cross(n, e0));
    c->en[1] = v3_norm(v3_cross(n, e1));
    c->en[2] = v3_norm(v3_cross(n, e2));
    /* orientation check: the opposite vertex must be inside; flip if the cross product convention disagrees */
    if (v3_dot(c->en[0], v3_sub(t->c, t->a)) < 0.0f) {
        c->en[0] = v3_scale(c->en[0], -1.0f);
        c->en[1] = v3_scale(c->en[1], -1.0f);
        c->en[2] = v3_scale(c->en[2], -1.0f);
    }
    c->eo[0] = v3_dot(c->en[0], t->a);
    c->eo[1] = v3_dot(c->en[1], t->b);
    c->eo[2] = v3_dot(c->en[2], t->c);
    c->surface = t->surface;
}

static int tri_ray(const CTri *c, Vec3 o, Vec3 dir, float maxT, float *tOut)
{
    float den = c->n.x * dir.x + c->n.y * dir.y + c->n.z * dir.z;
    float t;
    Vec3 p;
    if (den > -1e-6f) return 0;                     /* back face or parallel */
    t = -(c->n.x * o.x + c->n.y * o.y + c->n.z * o.z + c->d) / den;
    if (t < 0.0f || t > maxT) return 0;
    p.x = o.x + dir.x * t; p.y = o.y + dir.y * t; p.z = o.z + dir.z * t;
    if (c->en[0].x * p.x + c->en[0].y * p.y + c->en[0].z * p.z < c->eo[0] - EDGE_EPS) return 0;
    if (c->en[1].x * p.x + c->en[1].y * p.y + c->en[1].z * p.z < c->eo[1] - EDGE_EPS) return 0;
    if (c->en[2].x * p.x + c->en[2].y * p.y + c->en[2].z * p.z < c->eo[2] - EDGE_EPS) return 0;
    *tOut = t;
    return 1;
}

/* ------------------------------------------------------------------ walls */
static void wall_prepare(WallSeg *ws, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        WallSeg *w = &ws[i];
        float dx = w->x1 - w->x0, dz = w->z1 - w->z0, l;
        if (w->nx == 0.0f && w->nz == 0.0f) { w->nx = dz; w->nz = -dx; }
        l = (float)sqrt(w->nx * w->nx + w->nz * w->nz);
        if (l > 1e-9f) { w->nx /= l; w->nz /= l; }
        if (w->prev >= n) w->prev = -1;
        if (w->next >= n) w->next = -1;
    }
    for (i = 0; i < n; i++) {
        WallSeg *w = &ws[i];
        float l;
        if (w->n0x == 0.0f && w->n0z == 0.0f) {
            w->n0x = w->nx; w->n0z = w->nz;
            if (w->prev >= 0) { w->n0x += ws[w->prev].nx; w->n0z += ws[w->prev].nz; }
        }
        if (w->n1x == 0.0f && w->n1z == 0.0f) {
            w->n1x = w->nx; w->n1z = w->nz;
            if (w->next >= 0) { w->n1x += ws[w->next].nx; w->n1z += ws[w->next].nz; }
        }
        l = (float)sqrt(w->n0x * w->n0x + w->n0z * w->n0z);
        if (l > 1e-9f) { w->n0x /= l; w->n0z /= l; } else { w->n0x = w->nx; w->n0z = w->nz; }
        l = (float)sqrt(w->n1x * w->n1x + w->n1z * w->n1z);
        if (l > 1e-9f) { w->n1x /= l; w->n1z /= l; } else { w->n1x = w->nx; w->n1z = w->nz; }
    }
}

float coll_wall_dist(const WallSeg *w, float x, float z, float *tOut)
{
    float dx = w->x1 - w->x0, dz = w->z1 - w->z0, l2 = dx * dx + dz * dz, t;
    t = l2 > 1e-12f ? ((x - w->x0) * dx + (z - w->z0) * dz) / l2 : 0.0f;
    if (tOut) *tOut = t;
    /* distance from the segment's own line; the smoothed normal only bends the push-out direction */
    return (x - w->x0) * w->nx + (z - w->z0) * w->nz;
}

int coll_wall_point(const WallSeg *w, float x, float z, float maxDepth, float *depth, float *nxo, float *nzo)
{
    float dx = w->x1 - w->x0, dz = w->z1 - w->z0, len = (float)sqrt(dx * dx + dz * dz), t, dist, tc, nx, nz, l;
    float m;
    if (len < 1e-6f) return 0;
    dist = coll_wall_dist(w, x, z, &t);
    m = WALL_END_MARGIN / len;
    if (t < -m || t > 1.0f + m) return 0;
    if (dist >= 0.0f || dist <= -maxDepth) return 0;
    tc = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    nx = w->n0x + (w->n1x - w->n0x) * tc;
    nz = w->n0z + (w->n1z - w->n0z) * tc;
    l = (float)sqrt(nx * nx + nz * nz);
    if (l > 1e-9f) { nx /= l; nz /= l; } else { nx = w->nx; nz = w->nz; }
    *depth = -dist;
    *nxo = nx;
    *nzo = nz;
    return 1;
}

/* ------------------------------------------------------------------ create / free */
static CollWorld *world_alloc(void)
{
    CollWorld *w = (CollWorld *)malloc(sizeof(CollWorld));
    if (w) memset(w, 0, sizeof(*w));
    return w;
}

static int build_tris(CollWorld *w, const CollTri *t, int n)
{
    int i;
    float *box;
    if (n <= 0) return 1;
    w->tri = (CollTri *)malloc(sizeof(CollTri) * (size_t)n);
    w->ct = (CTri *)malloc(sizeof(CTri) * (size_t)n);
    box = (float *)malloc(sizeof(float) * 4 * (size_t)n);
    if (!w->tri || !w->ct || !box) { free(box); return 0; }
    memcpy(w->tri, t, sizeof(CollTri) * (size_t)n);
    w->ntri = n;
    for (i = 0; i < n; i++) {
        const CollTri *q = &w->tri[i];
        tri_prepare(&w->tri[i], &w->ct[i]);
        box[i * 4 + 0] = E_MINF(q->a.x, E_MINF(q->b.x, q->c.x));
        box[i * 4 + 1] = E_MINF(q->a.z, E_MINF(q->b.z, q->c.z));
        box[i * 4 + 2] = E_MAXF(q->a.x, E_MAXF(q->b.x, q->c.x));
        box[i * 4 + 3] = E_MAXF(q->a.z, E_MAXF(q->b.z, q->c.z));
    }
    i = grid_build(&w->tg, (float)TRI_CELL_M, n, box);
    free(box);
    return i;
}

static int build_walls(CollWorld *w, const WallSeg *ws, int n)
{
    int i;
    float *box;
    if (n <= 0) return 1;
    w->wall = (WallSeg *)malloc(sizeof(WallSeg) * (size_t)n);
    box = (float *)malloc(sizeof(float) * 4 * (size_t)n);
    if (!w->wall || !box) { free(box); return 0; }
    memcpy(w->wall, ws, sizeof(WallSeg) * (size_t)n);
    w->nwall = n;
    wall_prepare(w->wall, n);
    for (i = 0; i < n; i++) {
        const WallSeg *s = &w->wall[i];
        box[i * 4 + 0] = E_MINF(s->x0, s->x1) - WALL_END_MARGIN;
        box[i * 4 + 1] = E_MINF(s->z0, s->z1) - WALL_END_MARGIN;
        box[i * 4 + 2] = E_MAXF(s->x0, s->x1) + WALL_END_MARGIN;
        box[i * 4 + 3] = E_MAXF(s->z0, s->z1) + WALL_END_MARGIN;
    }
    i = grid_build(&w->wg, (float)WALL_CELL_M, n, box);
    free(box);
    return i;
}

CollWorld *coll_create(const void *colt, int ncolt, const void *colg, int ncolg,
                       const void *wall, int nwall, const void *boxs, int nboxs)
{
    CollWorld *w = world_alloc();
    int nt = colt ? ncolt / (int)sizeof(CollTri) : 0, nw = 0, nb = 0;
    const WallSeg *ws = NULL;
    (void)colg; (void)ncolg;
    if (!w) return NULL;
    if (wall && nwall >= 4) {
        u32 cnt;
        memcpy(&cnt, wall, 4);
        if ((int)cnt >= 0 && 4 + (int)cnt * (int)sizeof(WallSeg) <= nwall) {
            nw = (int)cnt;
            ws = (const WallSeg *)((const u8 *)wall + 4);
        } else {
            log_printf(LOG_WARN, "warn: coll WALL chunk size=%d count=%u inconsistent, walls ignored", nwall, cnt);
        }
    }
    if (boxs && nboxs >= 4) {
        u32 cnt;
        memcpy(&cnt, boxs, 4);
        if ((int)cnt >= 0 && 4 + (int)cnt * (int)sizeof(CollBox) <= nboxs) {
            nb = (int)cnt;
            w->box = (CollBox *)malloc(sizeof(CollBox) * (size_t)(nb > 0 ? nb : 1));
            if (w->box && nb > 0) memcpy(w->box, (const u8 *)boxs + 4, sizeof(CollBox) * (size_t)nb);
            w->nbox = nb;
        }
    }
    if (!build_tris(w, (const CollTri *)colt, nt) || !build_walls(w, ws, nw)) {
        log_printf(LOG_ERR, "err: coll out of memory tris=%d walls=%d", nt, nw);
        coll_free(w);
        return NULL;
    }
    log_printf(LOG_INFO, "coll: world tris=%d buckets_idx=%d walls=%d boxes=%d", w->ntri, w->tg.nidx, w->nwall, w->nbox);
    return w;
}

CollWorld *coll_create_plane(float y, int surface, float halfSize)
{
    CollWorld *w = world_alloc();
    if (!w) return NULL;
    w->isPlane = 1;
    w->planeY = y;
    w->planeSurf = surface;
    w->planeHalf = halfSize > 0.0f ? halfSize : 1e9f;
    return w;
}

CollWorld *coll_create_tris(const CollTri *t, int n, const WallSeg *ws, int nw)
{
    CollWorld *w = world_alloc();
    if (!w) return NULL;
    if (!build_tris(w, t, n) || !build_walls(w, ws, nw)) { coll_free(w); return NULL; }
    return w;
}

void coll_free(CollWorld *w)
{
    if (!w) return;
    free(w->tri);
    free(w->ct);
    grid_free(&w->tg);
    free(w->wall);
    grid_free(&w->wg);
    free(w->box);
    free(w);
}

int coll_num_tris(const CollWorld *w) { return w ? w->ntri : 0; }
const CollTri *coll_tri(const CollWorld *w, int i) { return (w && i >= 0 && i < w->ntri) ? &w->tri[i] : NULL; }
int coll_num_walls(const CollWorld *w) { return w ? w->nwall : 0; }

/* ------------------------------------------------------------------ rays */
static int plane_ray(const CollWorld *w, Vec3 o, Vec3 dir, float maxT, CollHit *hit)
{
    float t;
    Vec3 p;
    if (dir.y > -1e-6f) return 0;
    t = (w->planeY - o.y) / dir.y;
    if (t < 0.0f || t > maxT) return 0;
    p = v3_madd(o, dir, t);
    if ((float)fabs(p.x) > w->planeHalf || (float)fabs(p.z) > w->planeHalf) return 0;
    hit->t = t;
    hit->pos = p;
    hit->n = v3(0.0f, 1.0f, 0.0f);
    hit->surface = w->planeSurf;
    hit->tri = -1;
    return 1;
}

static void fill_hit(const CollWorld *w, int tri, Vec3 o, Vec3 dir, float t, CollHit *hit)
{
    hit->t = t;
    hit->pos = v3_madd(o, dir, t);
    hit->n = w->ct[tri].n;
    hit->surface = w->ct[tri].surface;
    hit->tri = tri;
}

static int bucket_ray(const CollWorld *w, const CBucket *b, Vec3 o, Vec3 dir, float maxT, float *best, int *bestTri)
{
    int k, found = 0;
    float t;
    for (k = 0; k < b->count; k++) {
        int ti = w->tg.idx[b->first + k];
        if (tri_ray(&w->ct[ti], o, dir, *best, &t)) { *best = t; *bestTri = ti; found = 1; }
    }
    (void)maxT;
    return found;
}

int coll_ray(const CollWorld *w, Vec3 orig, Vec3 dir, float maxT, CollHit *hit)
{
    float best = maxT, len, tx, tz, dtx, dtz, tcur;
    int bestTri = -1, bx, bz, ex, ez, sx, sz, guard;
    const CBucket *b;
    if (!w) return 0;
    if (w->isPlane) return plane_ray(w, orig, dir, maxT, hit);
    if (!w->tg.b) return 0;
    bx = floor_i(orig.x * w->tg.inv);
    bz = floor_i(orig.z * w->tg.inv);
    ex = floor_i((orig.x + dir.x * maxT) * w->tg.inv);
    ez = floor_i((orig.z + dir.z * maxT) * w->tg.inv);
    len = (float)sqrt(dir.x * dir.x + dir.z * dir.z);
    if ((bx == ex && bz == ez) || len < 1e-9f) {
        b = grid_find(&w->tg, bx, bz);
        if (b) bucket_ray(w, b, orig, dir, maxT, &best, &bestTri);
    } else {
        /* 2D DDA over the buckets crossed by the ray's XZ projection (t = ray parameter) */
        sx = dir.x > 0.0f ? 1 : -1;
        sz = dir.z > 0.0f ? 1 : -1;
        dtx = (float)fabs(dir.x) > 1e-12f ? w->tg.cell / (float)fabs(dir.x) : 1e30f;
        dtz = (float)fabs(dir.z) > 1e-12f ? w->tg.cell / (float)fabs(dir.z) : 1e30f;
        tx = (float)fabs(dir.x) > 1e-12f ?
             (((float)(bx + (sx > 0 ? 1 : 0)) * w->tg.cell - orig.x) / dir.x) : 1e30f;
        tz = (float)fabs(dir.z) > 1e-12f ?
             (((float)(bz + (sz > 0 ? 1 : 0)) * w->tg.cell - orig.z) / dir.z) : 1e30f;
        tcur = 0.0f;
        for (guard = 0; guard < 4096; guard++) {
            b = grid_find(&w->tg, bx, bz);
            if (b) bucket_ray(w, b, orig, dir, maxT, &best, &bestTri);
            if (bx == ex && bz == ez) break;
            if (tx < tz) { tcur = tx; tx += dtx; bx += sx; } else { tcur = tz; tz += dtz; bz += sz; }
            if (tcur > best || tcur > maxT) break;  /* nearest hit already closer than the next bucket */
        }
    }
    if (bestTri < 0) return 0;
    fill_hit(w, bestTri, orig, dir, best, hit);
    return 1;
}

int coll_ray_cached(const CollWorld *w, CollCache *cc, Vec3 orig, Vec3 dir, float maxT, CollHit *hit)
{
    int bx, bz, ex, ez, k, bestTri = -1;
    float best = maxT, t;
    if (!w || !cc) return coll_ray(w, orig, dir, maxT, hit);
    if (w->isPlane || !w->tg.b) return coll_ray(w, orig, dir, maxT, hit);
    bx = floor_i(orig.x * w->tg.inv);
    bz = floor_i(orig.z * w->tg.inv);
    ex = floor_i((orig.x + dir.x * maxT) * w->tg.inv);
    ez = floor_i((orig.z + dir.z * maxT) * w->tg.inv);
    if (bx != ex || bz != ez) return coll_ray(w, orig, dir, maxT, hit);   /* slanted across a bucket edge: rare */
    if (cc->n < -1 || cc->bx != bx || cc->bz != bz) {
        const CBucket *b = grid_find(&w->tg, bx, bz);
        cc->bx = bx;
        cc->bz = bz;
        if (!b) cc->n = 0;
        else if (b->count > E_COUNT(cc->tri)) cc->n = -1;   /* too many: use the bucket directly */
        else {
            cc->n = b->count;
            for (k = 0; k < b->count; k++) cc->tri[k] = w->tg.idx[b->first + k];
        }
    }
    if (cc->n < 0) return coll_ray(w, orig, dir, maxT, hit);
    for (k = 0; k < cc->n; k++) {
        int ti = cc->tri[k];
        if (tri_ray(&w->ct[ti], orig, dir, best, &t)) { best = t; bestTri = ti; }
    }
    if (bestTri < 0) return 0;
    fill_hit(w, bestTri, orig, dir, best, hit);
    return 1;
}

int coll_ground(const CollWorld *w, float x, float yTop, float z, float maxDrop, CollHit *hit)
{
    return coll_ray(w, v3(x, yTop, z), v3(0.0f, -1.0f, 0.0f), maxDrop, hit);
}

/* ------------------------------------------------------------------ walls / boxes near */
static float seg_dist2(const WallSeg *s, float x, float z)
{
    float dx = s->x1 - s->x0, dz = s->z1 - s->z0, l2 = dx * dx + dz * dz, t, qx, qz;
    t = l2 > 1e-12f ? ((x - s->x0) * dx + (z - s->z0) * dz) / l2 : 0.0f;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    qx = s->x0 + dx * t - x;
    qz = s->z0 + dz * t - z;
    return qx * qx + qz * qz;
}

int coll_walls_near(const CollWorld *w, float x, float z, float r, int *out, int maxOut)
{
    int n = 0, x0, z0, x1, z1, bx, bz, k, j, dup;
    float rr = (r + WALL_END_MARGIN) * (r + WALL_END_MARGIN);
    if (!w || !w->wg.b || maxOut <= 0) return 0;
    x0 = floor_i((x - r) * w->wg.inv); x1 = floor_i((x + r) * w->wg.inv);
    z0 = floor_i((z - r) * w->wg.inv); z1 = floor_i((z + r) * w->wg.inv);
    for (bz = z0; bz <= z1; bz++)
        for (bx = x0; bx <= x1; bx++) {
            const CBucket *b = grid_find(&w->wg, bx, bz);
            if (!b) continue;
            for (k = 0; k < b->count; k++) {
                int wi = w->wg.idx[b->first + k];
                dup = 0;
                for (j = 0; j < n; j++) if (out[j] == wi) { dup = 1; break; }
                if (dup) continue;
                if (seg_dist2(&w->wall[wi], x, z) > rr) continue;
                out[n++] = wi;
                if (n >= maxOut) return n;
            }
        }
    return n;
}

const WallSeg *coll_wall(const CollWorld *w, int i) { return (w && i >= 0 && i < w->nwall) ? &w->wall[i] : NULL; }

int coll_boxes_near(const CollWorld *w, float x, float z, float r, int *out, int maxOut)
{
    int i, n = 0;
    if (!w) return 0;
    for (i = 0; i < w->nbox && n < maxOut; i++) {     /* unused in 1.0: linear scan is fine */
        const CollBox *b = &w->box[i];
        float br = (float)sqrt(b->hx * b->hx + b->hz * b->hz) + r, dx = b->c.x - x, dz = b->c.z - z;
        if (dx * dx + dz * dz <= br * br) out[n++] = i;
    }
    return n;
}

/* ------------------------------------------------------------------ 2D OBB */
static void obb_corners(const Obb2 *o, float *px, float *pz)
{
    float rx = o->az, rz = -o->ax;                  /* right axis */
    float fx = o->ax * o->hz, fz = o->az * o->hz, sx = rx * o->hx, sz = rz * o->hx;
    px[0] = o->cx + fx + sx; pz[0] = o->cz + fz + sz;
    px[1] = o->cx + fx - sx; pz[1] = o->cz + fz - sz;
    px[2] = o->cx - fx - sx; pz[2] = o->cz - fz - sz;
    px[3] = o->cx - fx + sx; pz[3] = o->cz - fz + sz;
}

static float obb_extent(const Obb2 *o, float nx, float nz)  /* half projection on axis n */
{
    float f = o->ax * nx + o->az * nz, r = o->az * nx - o->ax * nz;
    return (float)fabs(f) * o->hz + (float)fabs(r) * o->hx;
}

int coll_obb2_obb2(const Obb2 *a, const Obb2 *b, Contact2 *c)
{
    float ax[4], az[4], best = 1e30f, bnx = 0.0f, bnz = 0.0f, dx, dz, px[4], pz[4], d, ext, ov;
    int i, owner = 0, k;
    ax[0] = a->ax; az[0] = a->az; ax[1] = a->az; az[1] = -a->ax;
    ax[2] = b->ax; az[2] = b->az; ax[3] = b->az; az[3] = -b->ax;
    dx = a->cx - b->cx;
    dz = a->cz - b->cz;
    for (i = 0; i < 4; i++) {
        d = dx * ax[i] + dz * az[i];
        ext = obb_extent(a, ax[i], az[i]) + obb_extent(b, ax[i], az[i]);
        ov = ext - (float)fabs(d);
        if (ov <= 0.0f) return 0;
        if (ov < best) {
            best = ov;
            bnx = d >= 0.0f ? ax[i] : -ax[i];       /* from B toward A */
            bnz = d >= 0.0f ? az[i] : -az[i];
            owner = i < 2 ? 0 : 1;
        }
    }
    c->nx = bnx;
    c->nz = bnz;
    c->depth = best;
    if (owner == 1) {                               /* face of B: deepest corner of A (min along n) */
        float m1 = 1e30f, m2 = 1e30f; int i1 = 0, i2 = 0;
        obb_corners(a, px, pz);
        for (k = 0; k < 4; k++) {
            d = px[k] * bnx + pz[k] * bnz;
            if (d < m1) { m2 = m1; i2 = i1; m1 = d; i1 = k; } else if (d < m2) { m2 = d; i2 = k; }
        }
        if (m2 - m1 < 0.02f) { c->px = 0.5f * (px[i1] + px[i2]); c->pz = 0.5f * (pz[i1] + pz[i2]); }
        else { c->px = px[i1]; c->pz = pz[i1]; }
    } else {                                        /* face of A: deepest corner of B (max along n) */
        float m1 = -1e30f, m2 = -1e30f; int i1 = 0, i2 = 0;
        obb_corners(b, px, pz);
        for (k = 0; k < 4; k++) {
            d = px[k] * bnx + pz[k] * bnz;
            if (d > m1) { m2 = m1; i2 = i1; m1 = d; i1 = k; } else if (d > m2) { m2 = d; i2 = k; }
        }
        if (m1 - m2 < 0.02f) { c->px = 0.5f * (px[i1] + px[i2]); c->pz = 0.5f * (pz[i1] + pz[i2]); }
        else { c->px = px[i1]; c->pz = pz[i1]; }
    }
    return 1;
}

int coll_obb2_wall(const Obb2 *a, const WallSeg *w, Contact2 *c)
{
    float px[4], pz[4], depth, nx, nz, maxDepth = 2.0f * (a->hx + a->hz);
    int k, found = 0;
    obb_corners(a, px, pz);
    c->depth = 0.0f;
    for (k = 0; k < 4; k++) {
        if (!coll_wall_point(w, px[k], pz[k], maxDepth, &depth, &nx, &nz)) continue;
        if (depth > c->depth) {
            c->depth = depth; c->nx = nx; c->nz = nz; c->px = px[k]; c->pz = pz[k];
            found = 1;
        }
    }
    return found;
}

void coll_cache_reset(CollCache *cc)
{
    cc->bx = cc->bz = 0;
    cc->n = -2;
}
