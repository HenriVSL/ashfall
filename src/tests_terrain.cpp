/* tests_terrain.cpp - -test terrain (DESIGN 8.6, 16.1) and `simh island`. Portable. Owner: T04.
   -test terrain [quick]: loads data/island/jet.isl and checks the file facts, terrain_height against the HGHT grid
   and the LOD0 render mesh, outside-grid rules, classes, normals, terrain_los / terrain_segment_hit against the
   exact reference on 1000 random rays (0 mismatches), dense sampling (no missed contact), segment hit points, the
   LOS timing of 30 km rays (gate <= 30 us average on the VM; Mac timing is info only) and the LOD selection
   triangle counts at the ENGINE 2.7 viewpoints (Low defaults within budget).
   simh island [lodScale]: prints the selection table (views x lodScale x distMode x skirtMode). */
#include "tests.h"
#include "e_log.h"
#include "e_file.h"
#include "e_prof.h"
#include "e_world.h"
#include "e_terrain.h"

static Terrain s_t;

static int load_island(void)
{
    char p[E_PATH];
    if (s_t.loaded) return 1;
    data_path(p, (int)sizeof(p), "island/jet.isl");
    return terrain_load(&s_t, p);
}

/* reference height: the two triangles of the cell in double precision */
static double ref_height(const Terrain *t, double x, double z)
{
    double fx = (x - t->x0) / t->cell, fz = (z - t->z0) / t->cell, u, w, h00, h10, h01, h11;
    int i, j, N = t->gridN;
    if (fx < 0.0 || fz < 0.0 || fx > N - 1 || fz > N - 1) return -60.0;
    i = (int)fx; j = (int)fz;
    if (i > N - 2) i = N - 2;
    if (j > N - 2) j = N - 2;
    u = fx - i; w = fz - j;
    h00 = t->hMin + t->hght[j * N + i] * (double)t->hScale;
    h10 = t->hMin + t->hght[j * N + i + 1] * (double)t->hScale;
    h01 = t->hMin + t->hght[(j + 1) * N + i] * (double)t->hScale;
    h11 = t->hMin + t->hght[(j + 1) * N + i + 1] * (double)t->hScale;
    if (w >= u) return h00 + w * (h01 - h00) + u * (h11 - h01);
    return h00 + u * (h10 - h00) + w * (h11 - h10);
}

static double bilinear(const Terrain *t, double x, double z)
{
    double fx = (x - t->x0) / t->cell, fz = (z - t->z0) / t->cell, u, w, h00, h10, h01, h11;
    int i = (int)fx, j = (int)fz, N = t->gridN;
    if (i < 0 || j < 0 || i > N - 2 || j > N - 2) return -60.0;
    u = fx - i; w = fz - j;
    h00 = t->hMin + t->hght[j * N + i] * (double)t->hScale;
    h10 = t->hMin + t->hght[j * N + i + 1] * (double)t->hScale;
    h01 = t->hMin + t->hght[(j + 1) * N + i] * (double)t->hScale;
    h11 = t->hMin + t->hght[(j + 1) * N + i + 1] * (double)t->hScale;
    return (h00 * (1 - u) + h10 * u) * (1 - w) + (h01 * (1 - u) + h11 * u) * w;
}

static void check_heights(void)
{
    const Terrain *t = &s_t;
    int i, j, N = t->gridN, bad = 0, c, k, m, ntri = 0, badMesh = 0;
    float worst = 0.0f, worstMesh = 0.0f;
    double worstBil = 0.0;
    ERand r;
    for (j = 0; j < N; j++)
        for (i = 0; i < N; i++) {
            float x = t->x0 + (float)i * t->cell, z = t->z0 + (float)j * t->cell;
            float h = terrain_height(t, x, z), want = t->hMin + (float)t->hght[j * N + i] * t->hScale;
            float e = (float)fabs(h - want);
            if (e > worst) worst = e;
            if (e > 0.002f) bad++;
        }
    t_check(bad == 0, "height_grid", "samples=%d bad=%d worst=%.4f", N * N, bad, (double)worst);
    rnd_seed(&r, 4401);
    bad = 0; worst = 0.0f;
    for (k = 0; k < 20000; k++) {
        float x = rnd_range(&r, t->x0, t->x0 + (float)(N - 1) * t->cell);
        float z = rnd_range(&r, t->z0, t->z0 + (float)(N - 1) * t->cell);
        double want = ref_height(t, x, z), d = fabs((double)terrain_height(t, x, z) - want), db;
        db = fabs(bilinear(t, x, z) - want);
        if (db > worstBil) worstBil = db;
        if ((float)d > worst) worst = (float)d;
        if (d > 0.005) bad++;
    }
    t_check(bad == 0, "height_triangles", "points=20000 bad=%d worst=%.4f bilinear_vs_mesh_worst=%.2f m", bad,
            (double)worst, worstBil);
    /* against the LOD0 render mesh itself: vertices and points inside the triangles */
    for (c = 0; c < t->nChunks; c++) {
        const TerrainChunk *ch = &t->chunks[c];
        if (ch->meshLod[0] != 0) continue;
        for (m = 0; m < ch->bCount[0]; m++) {
            const TerrainBatchRec *b = &t->batches[ch->bFirst[0] + m];
            int nt = (int)b->iCount / 3 - (int)b->skirtTris, q;
            if ((int)b->mat == t->foamMat) continue;
            for (q = 0; q < nt; q++) {
                const u16 *ix = t->idx + b->iFirst + 3 * q;
                const RVert1 *v0 = t->verts + ch->vFirst[0] + ix[0], *v1 = t->verts + ch->vFirst[0] + ix[1],
                             *v2 = t->verts + ch->vFirst[0] + ix[2];
                float bw[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.3333f, 0.3333f, 0.3334f }, { 0.6f, 0.3f, 0.1f } };
                int s;
                ntri++;
                for (s = 0; s < 3; s++) {
                    float x = bw[s][0] * v0->x + bw[s][1] * v1->x + bw[s][2] * v2->x;
                    float y = bw[s][0] * v0->y + bw[s][1] * v1->y + bw[s][2] * v2->y;
                    float z = bw[s][0] * v0->z + bw[s][1] * v1->z + bw[s][2] * v2->z;
                    float e = (float)fabs(terrain_height(t, x, z) - y);
                    if (e > worstMesh) worstMesh = e;
                    if (e > 0.03f) badMesh++;
                }
            }
        }
    }
    t_check(badMesh == 0 && ntri > 100000, "height_mesh", "lod0_tris=%d points=%d bad=%d worst=%.4f", ntri, 3 * ntri,
            badMesh, (double)worstMesh);
}

static void check_misc(void)
{
    const Terrain *t = &s_t;
    const TerrainBase *b = terrain_base(t);
    Vec3 n;
    int cls[8], i, N = t->gridN;
    float ox = t->x0 - 1000.0f, zTop = t->z0 + (float)(N - 1) * t->cell + 200.0f;   /* outside the grid (T03b) */
    float sx = t->x0 + 300.0f, sz = t->z0 + 300.0f;                                   /* sea inside the grid */
    t_check(terrain_height(t, ox, 0.0f) == TERRAIN_OUTSIDE_H && terrain_height(t, 0.0f, zTop) == -60.0f,
            "outside_grid", "h=%.1f", (double)terrain_height(t, ox, 0.0f));
    t_check(terrain_surface(t, ox, 0.0f) == 0.0f && terrain_surface(t, b->rwyX, b->rwyZ) > 11.0f,
            "surface", "sea=%.2f runway=%.2f", (double)terrain_surface(t, ox, 0.0f),
            (double)terrain_surface(t, b->rwyX, b->rwyZ));
    t_near(terrain_height(t, b->rwyX, b->rwyZ), b->rwyHeight, 0.05f, "runway_height");
    t_check(terrain_class(t, b->rwyX, b->rwyZ) == TCLASS_RUNWAY, "class_runway", "class=%d",
            terrain_class(t, b->rwyX, b->rwyZ));
    t_check(terrain_class(t, ox, 0.0f) == TCLASS_WATER && terrain_class(t, sx, sz) == TCLASS_WATER,
            "class_sea", NULL);
    memset(cls, 0, sizeof(cls));
    for (i = 0; i < (N - 1) * (N - 1); i++) cls[t->clas[i] & 7]++;
    log_printf(LOG_INFO, "test: terrain classes water=%d sand=%d grass=%d rock=%d ash=%d lava=%d runway=%d apron=%d",
               cls[0], cls[1], cls[2], cls[3], cls[4], cls[5], cls[6], cls[7]);
    n = terrain_normal(t, b->rwyX, b->rwyZ);
    t_check(n.y > 0.999f, "normal_runway", "n=%.3f,%.3f,%.3f", (double)n.x, (double)n.y, (double)n.z);
    {   /* normal vs finite differences on a slope of the volcano */
        float x = 600.0f, z = 900.0f, e = 2.0f, hx, hz;
        Vec3 nn = terrain_normal(t, x, z), fd;
        hx = (terrain_height(t, x + e, z) - terrain_height(t, x - e, z)) / (2.0f * e);
        hz = (terrain_height(t, x, z + e) - terrain_height(t, x, z - e)) / (2.0f * e);
        fd = v3_norm(v3(-hx, 1.0f, -hz));
        t_check(v3_dot(nn, fd) > 0.98f, "normal_slope", "n=%.3f,%.3f,%.3f fd=%.3f,%.3f,%.3f", (double)nn.x,
                (double)nn.y, (double)nn.z, (double)fd.x, (double)fd.y, (double)fd.z);
    }
    t_check(terrain_block_max(t, 192.0f, 544.0f) >= terrain_height(t, 192.0f, 544.0f) &&
            terrain_area_max(t, -500.0f, -200.0f, 900.0f, 1300.0f) > 1700.0f, "block_max", "area=%.1f top=%.1f",
            (double)terrain_area_max(t, -500.0f, -200.0f, 900.0f, 1300.0f), (double)t->hTop);
}

/* random ray of class k (0 low over the island, 1 anywhere, 2 across the volcano) */
static void random_ray(const Terrain *t, ERand *r, int k, Vec3 *a, Vec3 *b)
{
    float cx = t->centreX, cz = t->centreZ;
    if (k == 0) {
        float x0 = cx + rnd_range(r, -7000.0f, 7000.0f), z0 = cz + rnd_range(r, -7000.0f, 7000.0f);
        float x1 = cx + rnd_range(r, -7000.0f, 7000.0f), z1 = cz + rnd_range(r, -7000.0f, 7000.0f);
        *a = v3(x0, terrain_surface(t, x0, z0) + rnd_range(r, 5.0f, 400.0f), z0);
        *b = v3(x1, terrain_surface(t, x1, z1) + rnd_range(r, 5.0f, 400.0f), z1);
    } else if (k == 1) {
        *a = v3(rnd_range(r, -9500.0f, 9500.0f), rnd_range(r, 1.0f, 2500.0f), rnd_range(r, -9500.0f, 9500.0f));
        *b = v3(rnd_range(r, -9500.0f, 9500.0f), rnd_range(r, 1.0f, 2500.0f), rnd_range(r, -9500.0f, 9500.0f));
    } else {
        float a0 = rnd_range(r, 0.0f, 2.0f * E_PI), a1 = a0 + E_PI + rnd_range(r, -0.8f, 0.8f);
        float r0 = rnd_range(r, 2500.0f, 8000.0f), r1 = rnd_range(r, 2500.0f, 8000.0f);
        *a = v3(192.0f + r0 * (float)sin(a0), rnd_range(r, 900.0f, 2300.0f), 544.0f + r0 * (float)cos(a0));
        *b = v3(192.0f + r1 * (float)sin(a1), rnd_range(r, 900.0f, 2300.0f), 544.0f + r1 * (float)cos(a1));
    }
}

static void check_los(void)
{
    const Terrain *t = &s_t;
    ERand r;
    int i, mis = 0, misT = 0, clear = 0, blocked = 0, missed = 0, hitBad = 0, nTerr = 0, s;
    float worstT = 0.0f, worstHit = 0.0f;
    rnd_seed(&r, 4402);
    for (i = 0; i < 1000; i++) {
        Vec3 a, b, h;
        float tr = 2.0f, len;
        int fast, ref, hit;
        random_ray(t, &r, i % 3, &a, &b);
        fast = terrain_los(t, &a, &b);
        ref = !(terrain_segment_ref(t, &a, &b, &tr) || a.y <= t->seaLevel || b.y <= t->seaLevel);
        if (fast != ref) {
            mis++;
            if (mis <= 5) log_printf(LOG_INFO, "test: terrain los_mismatch a=%.2f,%.2f,%.2f b=%.2f,%.2f,%.2f fast=%d ref=%d",
                                     (double)a.x, (double)a.y, (double)a.z, (double)b.x, (double)b.y, (double)b.z, fast, ref);
        }
        if (ref) clear++; else blocked++;
        hit = terrain_segment_hit(t, &a, &b, &h);
        len = v3_len(v3_sub(b, a));
        if (hit != !ref) misT++;
        if (hit && tr <= 1.0f) {                    /* terrain contact: point on the surface, same t as the reference */
            float th = v3_len(v3_sub(h, a)) / (len > 0.0f ? len : 1.0f);
            float e = (float)fabs(th - tr) * len, eh = (float)fabs(h.y - terrain_height(t, h.x, h.z));
            if (a.y <= terrain_height(t, a.x, a.z)) eh = th > 0.0f ? 1e3f : 0.0f;   /* starts underground: t = 0 */
            nTerr++;
            if (e > worstT) worstT = e;
            if (eh > worstHit) worstHit = eh;
            if (e > 0.1f || eh > 0.1f) {
                hitBad++;
                if (hitBad <= 3) log_printf(LOG_INFO, "test: terrain hit_bad a=%.2f,%.2f,%.2f b=%.2f,%.2f,%.2f t=%.5f ref=%.5f hit=%.2f,%.2f,%.2f ground=%.2f",
                                            (double)a.x, (double)a.y, (double)a.z, (double)b.x, (double)b.y, (double)b.z, (double)th, (double)tr,
                                            (double)h.x, (double)h.y, (double)h.z, (double)terrain_height(t, h.x, h.z));
            }
        }
        if (i < 200 && ref) {                       /* dense sampling must never find a contact on a clear ray */
            int ns = (int)(len / 2.0f) + 2;
            for (s = 0; s <= ns; s++) {
                Vec3 p = v3_lerp(a, b, (float)s / (float)ns);
                if (p.y <= terrain_height(t, p.x, p.z)) { missed++; break; }
            }
        }
    }
    t_check(mis == 0, "los_vs_reference", "rays=1000 mismatches=%d clear=%d blocked=%d", mis, clear, blocked);
    t_check(clear > 150 && blocked > 150, "los_ray_mix", "clear=%d blocked=%d", clear, blocked);
    t_check(misT == 0, "segment_vs_los", "mismatches=%d", misT);
    t_check(hitBad == 0 && nTerr > 100, "segment_hit_point", "terrain_hits=%d bad=%d worst_dt_m=%.3f worst_dh=%.3f",
            nTerr, hitBad, (double)worstT, (double)worstHit);
    t_check(missed == 0, "los_dense_sampling", "rays=200 step=2m missed=%d", missed);
    {   /* fixed cases */
        Vec3 a = v3(600.0f, 4000.0f, 900.0f), b = v3(600.0f, -100.0f, 900.0f), h;
        int hit = terrain_segment_hit(t, &a, &b, &h);
        t_check(hit && (float)fabs(h.y - terrain_height(t, 600.0f, 900.0f)) < 0.05f, "segment_vertical_land",
                "hit=%d y=%.2f ground=%.2f", hit, (double)h.y, (double)terrain_height(t, 600.0f, 900.0f));
        a = v3(t->x0 + 300.0f, 500.0f, t->z0 + 300.0f); b = v3(t->x0 + 300.0f, -50.0f, t->z0 + 300.0f);
        hit = terrain_segment_hit(t, &a, &b, &h);
        t_check(hit && h.y == 0.0f, "segment_sea", "hit=%d y=%.2f", hit, (double)h.y);
        a = v3(-4000.0f, t->hTop + 1000.0f, -6000.0f); b = v3(5000.0f, t->hTop + 1000.0f, 7000.0f);
        t_check(terrain_los(t, &a, &b) && !terrain_segment_hit(t, &a, &b, NULL), "los_high_clear", NULL);
        a = v3(192.0f - 6000.0f, 900.0f, 544.0f); b = v3(192.0f + 6000.0f, 900.0f, 544.0f);
        t_check(!terrain_los(t, &a, &b), "los_volcano_blocks", NULL);
        a = v3(t->x0 - 2000.0f, 50.0f, 0.0f); b = v3(t->x0 + 2000.0f, 60.0f, 3000.0f);
        t_check(terrain_los(t, &a, &b), "los_outside_grid", NULL);
    }
}

/* 30 km rays through the island area, class k: 0 mixed 300-3000 m, 1 low 30-400 m, 2 across the volcano rim */
static void timing_ray(const Terrain *t, ERand *r, int k, Vec3 *a, Vec3 *b)
{
    float ang = rnd_range(r, 0.0f, 2.0f * E_PI), off = rnd_range(r, -4000.0f, 4000.0f), y0, y1;
    float sx = (float)sin(ang), cz = (float)cos(ang);
    if (k == 0) { y0 = rnd_range(r, 300.0f, 3000.0f); y1 = rnd_range(r, 300.0f, 3000.0f); }
    else if (k == 1) { y0 = rnd_range(r, 30.0f, 400.0f); y1 = rnd_range(r, 30.0f, 400.0f); }
    else { y0 = rnd_range(r, 1700.0f, 2300.0f); y1 = rnd_range(r, 1700.0f, 2300.0f); off *= 0.2f; }
    *a = v3(t->centreX - 15000.0f * sx + off * cz, y0, t->centreZ - 15000.0f * cz - off * sx);
    *b = v3(t->centreX + 15000.0f * sx + off * cz, y1, t->centreZ + 15000.0f * cz - off * sx);
}

enum { TRAYS = 2000 };
static Vec3 s_ra[TRAYS], s_rb[TRAYS];

static void check_timing(int quick)
{
    const Terrain *t = &s_t;
    ERand r;
    s64 t0, t1, worst = 0, base;
    int i, k, n = quick ? 200 : TRAYS;
    double sumAll = 0.0, cls[3];
    if (!prof_now_us()) { log_printf(LOG_INFO, "test: terrain timing skipped=no_clock"); return; }
    t0 = prof_now_us();
    for (i = 0; i < 1000; i++) t1 = prof_now_us();
    base = (prof_now_us() - t0) / 1000;             /* cost of one clock read (Win98 QPC is slow) */
    rnd_seed(&r, 4403);
    for (k = 0; k < 3; k++) {
        int c = 0;
        for (i = 0; i < n; i++) timing_ray(t, &r, k, &s_ra[i], &s_rb[i]);
        t0 = prof_now_us();
        for (i = 0; i < n; i++) c += terrain_los(t, &s_ra[i], &s_rb[i]);
        t1 = prof_now_us();
        cls[k] = (double)(t1 - t0) / (double)n;
        sumAll += cls[k];
        for (i = 0; i < n && i < 300; i++) {      /* worst single ray (minus the clock read) */
            s64 s0 = prof_now_us(), s1;
            terrain_los(t, &s_ra[i], &s_rb[i]);
            s1 = prof_now_us() - s0 - base;
            if (s1 > worst) worst = s1;
        }
        log_printf(LOG_INFO, "test: terrain los_timing class=%s rays=%d clear=%d avg_us=%.1f",
                   k == 0 ? "mixed_300_3000m" : (k == 1 ? "low_30_400m" : "volcano_rim"), n, c, cls[k]);
    }
    log_printf(LOG_INFO, "test: terrain los_timing worst_single_us=%d clock_read_us=%d", (int)worst, (int)base);
#if defined(_WIN32)
    t_check(sumAll / 3.0 <= 30.0, "los_30km_timing", "avg_us=%.1f (classes %.1f %.1f %.1f) gate=30",
            sumAll / 3.0, cls[0], cls[1], cls[2]);
#else
    log_printf(LOG_INFO, "test: terrain los_30km_timing avg_us=%.2f (Mac, info only; the gate is on the VM)",
               sumAll / 3.0);
#endif
    {   /* height query cost (info) */
        float acc = 0.0f;
        t0 = prof_now_us();
        for (i = 0; i < 20000; i++) acc += terrain_height(t, (float)(i % 997) * 13.0f - 6000.0f, (float)(i % 613) * 17.0f - 5000.0f);
        t1 = prof_now_us();
        log_printf(LOG_INFO, "test: terrain height_timing calls=20000 avg_us=%.3f (sum %.0f)",
                   (double)(t1 - t0) / 20000.0, (double)acc);
    }
}

/* ---------------------------------------------------------------- LOD selection views (ENGINE 2.7 / T03 [views]) */
typedef struct ViewPt { const char *name; Vec3 eye; float pitch; } ViewPt;

static void view_points(const Terrain *t, ViewPt *v)
{
    const TerrainBase *b = terrain_base(t);
    v[0].name = "runway_50m"; v[0].eye = v3(b->rwyX, b->rwyHeight + 50.0f, b->rwyZ); v[0].pitch = 0.0f;
    {   /* over the highest HMAX block (T03b: was the T03 island's summit 192, 544) */
        int i, best = 0, nb = t->nBlocksX;
        float bs = 8.0f * t->cell;
        for (i = 1; i < nb * nb; i++) if (t->blockMax[i] > t->blockMax[best]) best = i;
        v[1].name = "crater_1500m";
        v[1].eye = v3(t->x0 + ((float)(best % nb) + 0.5f) * bs, t->hTop + 1500.0f, t->z0 + ((float)(best / nb) + 0.5f) * bs);
        v[1].pitch = -15.0f;
    }
    v[2].name = "sea_8000m"; v[2].eye = v3(t->centreX - 7071.0f, 8000.0f, t->centreZ - 7071.0f); v[2].pitch = -15.0f;
    v[3].name = "threshold"; v[3].eye = v3(b->threshold.x, b->threshold.y + 3.3f, b->threshold.z); v[3].pitch = 0.0f;
}

static void make_view(WView *wv, Vec3 eye, float yawDeg, float pitchDeg, float farDist)
{
    float cy = (float)cos(E_DEG2RAD(pitchDeg)), sy = (float)sin(E_DEG2RAD(pitchDeg));
    Vec3 at = v3(eye.x + 1000.0f * cy * (float)sin(E_DEG2RAD(yawDeg)), eye.y + 1000.0f * sy,
                 eye.z + 1000.0f * cy * (float)cos(E_DEG2RAD(yawDeg)));
    float fovY = 2.0f * (float)atan(tan(E_DEG2RAD(70.0f) * 0.5f) * 0.75f);
    world_view_lookat(wv, eye, at, fovY, 4.0f / 3.0f, farDist, 0.0f);
}

/* max / min over 8 headings; returns the max tris */
static int view_counts(const Terrain *t, const ViewPt *vp, float lodScale, float farDist, TerrainStats *mx,
                       TerrainStats *mn)
{
    int y;
    WView wv;
    TerrainStats st;
    mx->tris = -1; mn->tris = 1 << 30;
    for (y = 0; y < 360; y += 45) {
        make_view(&wv, vp->eye, (float)y, vp->pitch, farDist);
        terrain_select(t, &wv, lodScale, farDist, 0, &st);
        if (st.tris > mx->tris) *mx = st;
        if (st.tris < mn->tris) *mn = st;
    }
    return mx->tris;
}

static void print_views(const Terrain *t, float lodScale, int distMode, int skirtMode, float farDist, int maxTris,
                        int *over)
{
    TerrainDrawCfg *c = terrain_draw_cfg(), save = *c;
    ViewPt vp[4];
    int i;
    c->distMode = distMode;
    c->skirtMode = skirtMode;
    view_points(t, vp);
    for (i = 0; i < 4; i++) {
        TerrainStats mx, mn;
        view_counts(t, &vp[i], lodScale, farDist, &mx, &mn);
        log_printf(LOG_INFO, "terrain: view=%s lodScale=%.2f dist=%s skirts=%s far=%.0f tris_max=%d (skirt %d foam %d) "
                   "lods=%d/%d/%d/%d/%d skirtChunks=%d tris_min=%d", vp[i].name, (double)lodScale,
                   distMode ? "centre" : "nearest", skirtMode == 0 ? "changes" : (skirtMode == 1 ? "all" : "none"),
                   (double)farDist, mx.tris, mx.skirtTris, mx.foamTris, mx.chunksByLod[0], mx.chunksByLod[1],
                   mx.chunksByLod[2], mx.chunksByLod[3], mx.chunksByLod[4], mx.skirtChunks, mn.tris);
        if (over && i < 3 && mx.tris + mx.foamTris > maxTris) (*over)++;
    }
    *c = save;
}

static void check_select(void)
{
    const Terrain *t = &s_t;
    TerrainDrawCfg *c = terrain_draw_cfg();
    int over = 0;
    WView wv;
    TerrainStats a, b;
    /* hysteresis: stepping back and forth across a switch distance must not flip */
    {
        const TerrainChunk *ch = &t->chunks[t->nChunks / 2];
        float D = c->lodK * t->lodEdge[1];
        Vec3 eye;
        int flips = 0, last = -1, ci = t->nChunks / 2, s;
        for (s = 0; s < 40; s++) {
            float off = D + ((s & 1) ? 0.04f : -0.04f) * D * 0.5f;   /* +-2 % around the LOD1/2 switch */
            eye = v3(ch->box.hi.x + off, ch->box.hi.y > 0.0f ? ch->box.hi.y : 0.0f, ch->centre.z);
            make_view(&wv, eye, 270.0f, 0.0f, 18000.0f);
            terrain_select(t, &wv, 1.0f, 18000.0f, s > 0, NULL);
            if (s > 1 && (int)t->selLod[ci] != last) flips++;
            last = t->selLod[ci];
        }
        t_check(flips == 0, "lod_hysteresis", "flips=%d lod=%d", flips, last);
    }
    make_view(&wv, v3(0.0f, 30000.0f, 0.0f), 0.0f, -90.0f + 0.1f, 18000.0f);
    terrain_select(t, &wv, 1.0f, 18000.0f, 0, &a);
    t_check(a.inRange == 0 && a.tris == 0, "select_beyond_far", "inRange=%d", a.inRange);
    make_view(&wv, v3(t->base.rwyX, t->base.rwyHeight + 50.0f, t->base.rwyZ), 90.0f, 0.0f, 18000.0f);
    c->skirtMode = 1; terrain_select(t, &wv, 1.0f, 18000.0f, 0, &a);
    c->skirtMode = 0; terrain_select(t, &wv, 1.0f, 18000.0f, 0, &b);
    t_check(b.tris < a.tris && b.skirtChunks < a.skirtChunks && b.skirtChunks > 0, "skirts_at_changes",
            "all=%d changes=%d skirtChunks=%d/%d", a.tris, b.tris, b.skirtChunks, a.skirtChunks);
    print_views(t, TERRAIN_LODSCALE_LOW, c->distMode, c->skirtMode, 18000.0f, 6000, &over);
    t_check(over == 0, "low_budget_views", "views over 6000 tris (terrain + foam, Low defaults)=%d", over);
}

int test_terrain(const char *arg)
{
    int quick = arg && !strcmp(arg, "quick");
    t_begin("terrain");
    if (!t_check(load_island(), "load", "path=data/island/jet.isl")) return t_end();
    t_check(s_t.gridN == 513 && s_t.nChunks > 100 && s_t.nMats == 8 && s_t.foamMat == 5 &&   /* A11: + t_forest, t_scrub after foam */
            s_t.nBlocksX == (s_t.gridN - 1) / 8,
            "file_facts", "grid=%d chunks=%d mats=%d foam=%d", s_t.gridN, s_t.nChunks, s_t.nMats, s_t.foamMat);
    t_check(s_t.base.rwyHeadingDeg == 270.0f && s_t.base.rwyLength == 2800.0f && s_t.base.nBoxes == 9,
            "base", "hdg=%.1f len=%.0f boxes=%d", (double)s_t.base.rwyHeadingDeg, (double)s_t.base.rwyLength,
            s_t.base.nBoxes);
    {
        int i, k, badIdx = 0;
        for (i = 0; i < s_t.nChunks; i++)
            for (k = 0; k < TERRAIN_LODS; k++) {
                const TerrainChunk *c = &s_t.chunks[i];
                int m;
                for (m = 0; m < c->bCount[k]; m++) {
                    const TerrainBatchRec *b = &s_t.batches[c->bFirst[k] + m];
                    u32 q;
                    for (q = 0; q < b->iCount; q++) if (s_t.idx[b->iFirst + q] >= (u32)c->vCount[k]) badIdx++;
                }
            }
        t_check(badIdx == 0, "index_ranges", "bad=%d", badIdx);
    }
    check_heights();
    check_misc();
    check_los();
    check_timing(quick);
    check_select();
    return t_end();
}

int simh_island(int argc, char **argv)
{
    static const float scales[] = { 1.0f, 0.85f, 0.7f, 0.6f, 1.3f, 1.5f, 2.0f };
    int i, dm;
    float only = argc > 0 ? (float)atof(argv[0]) : 0.0f;
    if (!load_island()) { log_printf(LOG_INFO, "simh: island load failed"); return 1; }
    for (i = 0; i < (int)(sizeof(scales) / sizeof(scales[0])); i++) {
        if (only > 0.0f && (float)fabs(only - scales[i]) > 0.001f) continue;
        for (dm = 0; dm < 2; dm++) {
            print_views(&s_t, scales[i], dm, 0, 18000.0f, 0, NULL);
            if (i == 0 && dm == 0) {
                print_views(&s_t, scales[i], dm, 1, 18000.0f, 0, NULL);
                print_views(&s_t, scales[i], dm, 0, 16200.0f, 0, NULL);
            }
        }
    }
    return 0;
}
