/* e_terrain.cpp - jet.isl loader, height/class/normal queries, LOS + segment march, LOD selection (DESIGN 8.2, 8.6).
   Portable (VC6 /W3, clang -std=c++98 -Wall; simh links it). Owner: T04. Rules: e_terrain.h header, e_terrain.md.
   Without the file (or on a bad file) the stub behaviour stays: the world is open sea at y = 0 and a default runway
   at the origin heading north, terrain_load returns 0.
   LOS / segment march (DESIGN 8.6 target <= 30 us for a 30 km ray on the VM): the segment is clipped to the grid,
   then walked with a 2D DDA over 2048 m super blocks (max of 8 x 8 HMAX blocks); super blocks the ray passes above
   are skipped, the rest are walked over 256 m HMAX blocks the same way, and only blocks the ray dips into are walked
   cell by cell; a cell is tested exactly (piecewise-linear ray height minus the two planar triangles) only when the
   ray is below the cell's highest corner there. All skip tests keep a 5 cm margin, so they are conservative; the
   exact cell test is the same function the reference uses (terrain_segment_ref). */
#include "e_terrain.h"
#include "e_world.h"
#include "e_log.h"
#include "e_file.h"

#define T_EPS 0.05f                                 /* conservative margin of the skip tests (m) */

/* floor without the CRT (VC6 floor() and (int) casts reload the FPU control word: ~100 cycles each) */
static int ffloor(float x)
{
    int i = e_ftoi(x);
    if ((float)i > x) i--;
    return i;
}

/* ================================================================ defaults (no file) */
static void default_base(TerrainBase *b)
{
    int i;
    memset(b, 0, sizeof(*b));
    b->rwyHeadingDeg = 0.0f;
    b->rwyLength = 2800.0f;
    b->rwyWidth = 45.0f;
    b->threshold = v3(0.0f, 0.0f, -1300.0f);
    for (i = 0; i < 4; i++) {
        float a = (float)i * 0.5f * E_PI;
        b->airSpawn[i] = v3(12000.0f * (float)sin(a), 1500.0f, 12000.0f * (float)cos(a));
        b->airSpawnHeadingDeg[i] = E_RAD2DEG(a) + 180.0f;
        if (b->airSpawnHeadingDeg[i] >= 360.0f) b->airSpawnHeadingDeg[i] -= 360.0f;
    }
}

static u32 rd_u32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u16 rd_u16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }
static float rd_f32(const u8 *p) { float v; memcpy(&v, p, 4); return v; }

static int bad(Terrain *t, const char *what, int a, int b)
{
    log_printf(LOG_ERR, "err: terrain bad file what=%s a=%d b=%d", what, a, b);
    terrain_free(t);
    default_base(&t->base);
    return 0;
}

static float hgt(const Terrain *t, int i, int j) { return t->hMin + (float)t->hght[j * t->gridN + i] * t->hScale; }

int terrain_tri_side(const Terrain *t, const TerrainChunk *ch, const RVert1 *a, const RVert1 *b, const RVert1 *c)
{
    float X0 = t->x0 + (float)(ch->cx * t->chunkCells) * t->cell, Z0 = t->z0 + (float)(ch->cz * t->chunkCells) * t->cell;
    float S = (float)t->chunkCells * t->cell, e = 0.25f;
    if (fabs(a->z - Z0) < e && fabs(b->z - Z0) < e && fabs(c->z - Z0) < e) return TSIDE_S;
    if (fabs(a->x - X0 - S) < e && fabs(b->x - X0 - S) < e && fabs(c->x - X0 - S) < e) return TSIDE_E;
    if (fabs(a->z - Z0 - S) < e && fabs(b->z - Z0 - S) < e && fabs(c->z - Z0 - S) < e) return TSIDE_N;
    if (fabs(a->x - X0) < e && fabs(b->x - X0) < e && fabs(c->x - X0) < e) return TSIDE_W;
    return -1;
}

/* ================================================================ load */
int terrain_load(Terrain *t, const char *path)
{
    ChunkFile cf;
    const u8 *p;
    int sz, i, k, n, N, nb;
    memset(t, 0, sizeof(*t));
    default_base(&t->base);
    t->foamMat = -1;
    if (!path || !file_exists(path)) {
        log_printf(LOG_WARN, "warn: terrain file missing path=%s: flat sea, default runway", path ? path : "");
        return 0;
    }
    t->fileData = (u8 *)file_load(path, &t->fileSize);
    if (!t->fileData) return bad(t, "read", 0, 0);
    if (!chunk_open(&cf, t->fileData, t->fileSize, E_TAG('R', 'I', 'S', 'L'))) return bad(t, "container", 0, 0);
    /* INFO */
    p = chunk_find(&cf, "INFO", &sz);
    if (!p || sz < 84 || rd_u32(p) != 1) return bad(t, "INFO", sz, p ? (int)rd_u32(p) : -1);
    t->gridN = (int)rd_u32(p + 4);
    t->cell = rd_f32(p + 8);
    t->x0 = rd_f32(p + 12); t->z0 = rd_f32(p + 16);
    t->hMin = rd_f32(p + 20); t->hScale = rd_f32(p + 24);
    t->chunkCells = (int)rd_u32(p + 28);
    t->nChunkX = (int)rd_u32(p + 32); t->nChunkZ = (int)rd_u32(p + 36);
    t->nLods = (int)rd_u32(p + 40);
    t->seaLevel = rd_f32(p + 44);
    for (k = 0; k < TERRAIN_LODS; k++) t->lodEdge[k] = rd_f32(p + 48 + 4 * k);
    t->nMats = (int)rd_u32(p + 68);
    t->centreX = rd_f32(p + 72); t->centreZ = rd_f32(p + 76); t->radius = rd_f32(p + 80);
    N = t->gridN;
    if (N < 65 || N > 4097 || ((N - 1) & 63) || t->cell <= 0.0f || t->nLods != TERRAIN_LODS || t->chunkCells != 16 ||
        t->nChunkX * t->chunkCells != N - 1 || t->nChunkZ * t->chunkCells != N - 1 || t->nMats <= 0 ||
        t->nMats > TERRAIN_MAX_MATS)
        return bad(t, "INFO values", N, t->nMats);
    t->invCell = 1.0f / t->cell;
    t->nBlocksX = (N - 1) / 8;
    t->nSuperX = t->nBlocksX / 8;
    if (t->nSuperX > 8) return bad(t, "grid too large for superMax", N, t->nSuperX);
    /* HGHT HMAX CLAS (used in place: chunk data is 4-byte aligned) */
    p = chunk_find(&cf, "HGHT", &sz);
    if (!p || sz != N * N * 2) return bad(t, "HGHT", sz, N * N * 2);
    t->hght = (const u16 *)p;
    p = chunk_find(&cf, "HMAX", &sz);
    if (!p || sz != t->nBlocksX * t->nBlocksX * 2) return bad(t, "HMAX", sz, t->nBlocksX);
    t->hmax = (const u16 *)p;
    p = chunk_find(&cf, "CLAS", &sz);
    if (!p || sz != (N - 1) * (N - 1)) return bad(t, "CLAS", sz, (N - 1) * (N - 1));
    t->clas = p;
    /* MATL */
    p = chunk_find(&cf, "MATL", &sz);
    if (!p || sz != t->nMats * 40) return bad(t, "MATL", sz, t->nMats);
    for (i = 0; i < t->nMats; i++) {
        memcpy(t->mats[i].tex, p + 40 * i, 32);
        t->mats[i].tex[31] = 0;
        t->mats[i].flags = rd_u32(p + 40 * i + 32);
        t->mats[i].tint = rd_u32(p + 40 * i + 36);
        if ((t->mats[i].flags & TMF_BLEND_MASK) == RB_ADD && t->foamMat < 0) t->foamMat = i;
    }
    /* BTCH VERT INDX (VERT/INDX are reordered in place by terrain_gpu_init) */
    p = chunk_find(&cf, "BTCH", &sz);
    if (!p || sz % 12) return bad(t, "BTCH", sz, 12);
    t->batches = (const TerrainBatchRec *)p;
    t->nBatches = sz / 12;
    p = chunk_find(&cf, "VERT", &sz);
    if (!p || sz % 24) return bad(t, "VERT", sz, 24);
    t->verts = (RVert1 *)p;
    t->nVerts = sz / 24;
    p = chunk_find(&cf, "INDX", &sz);
    if (!p || sz % 2) return bad(t, "INDX", sz, 2);
    t->idx = (u16 *)p;
    t->nIdx = sz / 2;
    for (i = 0; i < t->nBatches; i++) {
        const TerrainBatchRec *b = &t->batches[i];
        if ((int)b->mat >= t->nMats || b->iCount % 3 || b->iFirst + b->iCount > (u32)t->nIdx ||
            3u * b->skirtTris > b->iCount)
            return bad(t, "BTCH record", i, (int)b->mat);
    }
    /* CHNK */
    p = chunk_find(&cf, "CHNK", &sz);
    if (!p || sz % 108 || sz / 108 > t->nChunkX * t->nChunkZ) return bad(t, "CHNK", sz, 108);
    n = sz / 108;
    t->chunks = (TerrainChunk *)calloc((size_t)(n > 0 ? n : 1), sizeof(TerrainChunk));
    t->chunkAt = (short *)malloc((size_t)(t->nChunkX * t->nChunkZ) * sizeof(short));
    t->selLod = (u8 *)malloc((size_t)(n > 0 ? n : 1));
    t->selFlags = (u8 *)calloc((size_t)(n > 0 ? n : 1), 1);
    t->visList = (int *)malloc((size_t)(n > 0 ? 2 * n : 2) * sizeof(int));
    t->blockMax = (float *)malloc((size_t)(t->nBlocksX * t->nBlocksX) * sizeof(float));
    t->cull = (float *)malloc((size_t)(n > 0 ? n : 1) * 9 * sizeof(float));
    t->nGroupsX = (t->nChunkX + 3) / 4;
    t->nGroupsZ = (t->nChunkZ + 3) / 4;
    t->grpStart = (int *)calloc((size_t)(t->nGroupsX * t->nGroupsZ + 1), sizeof(int));
    t->grpList = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    t->grpBox = (float *)malloc((size_t)(t->nGroupsX * t->nGroupsZ) * 6 * sizeof(float));
    t->prevLod = (u8 *)malloc((size_t)(n > 0 ? n : 1));
    if (!t->chunks || !t->chunkAt || !t->selLod || !t->selFlags || !t->visList || !t->blockMax || !t->cull ||
        !t->grpStart || !t->grpList || !t->grpBox || !t->prevLod)
        return bad(t, "alloc", n, 0);
    t->nChunks = n;
    memset(t->selLod, TERRAIN_NOLOD, (size_t)(n > 0 ? n : 1));
    for (i = 0; i < t->nChunkX * t->nChunkZ; i++) t->chunkAt[i] = -1;
    for (i = 0; i < n; i++) {
        TerrainChunk *c = &t->chunks[i];
        const u8 *q = p + 108 * i;
        int a, b, I0, J0, below = 0, above = 0;
        c->cx = rd_u16(q);
        c->cz = rd_u16(q + 2);
        c->box.lo = v3(rd_f32(q + 4), rd_f32(q + 8), rd_f32(q + 12));
        c->box.hi = v3(rd_f32(q + 16), rd_f32(q + 20), rd_f32(q + 24));
        c->centre = v3_scale(v3_add(c->box.lo, c->box.hi), 0.5f);
        {
            float *f = t->cull + 9 * i;
            f[0] = c->box.lo.x; f[1] = c->box.lo.y; f[2] = c->box.lo.z;
            f[3] = c->box.hi.x; f[4] = c->box.hi.y; f[5] = c->box.hi.z;
            f[6] = c->centre.x; f[7] = c->centre.y; f[8] = c->centre.z;
        }
        if (c->cx >= t->nChunkX || c->cz >= t->nChunkZ || t->chunkAt[c->cz * t->nChunkX + c->cx] >= 0)
            return bad(t, "CHNK cell", c->cx, c->cz);
        t->chunkAt[c->cz * t->nChunkX + c->cx] = (short)i;
        for (k = 0; k < TERRAIN_LODS; k++) {
            const u8 *r = q + 28 + 16 * k;
            c->vFirst[k] = (int)rd_u32(r);
            c->vCount[k] = (int)rd_u32(r + 4);
            c->bFirst[k] = (int)rd_u32(r + 8);
            c->bCount[k] = (int)rd_u32(r + 12);
            if (c->vCount[k] <= 0 || c->vCount[k] > 65535 || c->vFirst[k] + c->vCount[k] > t->nVerts ||
                c->bCount[k] <= 0 || c->bFirst[k] + c->bCount[k] > t->nBatches)
                return bad(t, "CHNK range", i, k);
        }
        for (k = 0; k < TERRAIN_LODS; k++) {      /* LOD k aliases the coarsest LOD with the same record */
            int j;
            c->meshLod[k] = (u8)k;
            for (j = k + 1; j < TERRAIN_LODS; j++)
                if (c->vFirst[j] == c->vFirst[k] && c->bFirst[j] == c->bFirst[k]) c->meshLod[k] = (u8)j;
        }
        for (k = 0; k < TERRAIN_LODS; k++) {
            int m, q;
            c->tris[k] = c->trisNoSkirt[k] = c->foamTris[k] = 0;
            for (q = 0; q < 4; q++) c->skirtSide[k][q] = 0;
            for (m = 0; m < c->bCount[k]; m++) {
                const TerrainBatchRec *br = &t->batches[c->bFirst[k] + m];
                const RVert1 *vb = t->verts + c->vFirst[k];
                u32 tt, nt = br->iCount / 3;
                if ((int)br->mat == t->foamMat) { c->foamTris[k] += (int)nt; continue; }
                c->tris[k] += (int)nt;
                c->trisNoSkirt[k] += (int)nt - (int)br->skirtTris;
                for (tt = nt - br->skirtTris; tt < nt; tt++) {
                    const u16 *ix = t->idx + br->iFirst + 3 * tt;
                    int sd;
                    if (ix[0] >= c->vCount[k] || ix[1] >= c->vCount[k] || ix[2] >= c->vCount[k])
                        return bad(t, "INDX value", i, k);
                    sd = terrain_tri_side(t, c, vb + ix[0], vb + ix[1], vb + ix[2]);
                    if (sd < 0) return bad(t, "skirt side", i, k);
                    c->skirtSide[k][sd]++;
                }
            }
        }
        I0 = c->cx * t->chunkCells;
        J0 = c->cz * t->chunkCells;
        for (b = 0; b <= t->chunkCells; b++)
            for (a = 0; a <= t->chunkCells; a++) {
                if (hgt(t, I0 + a, J0 + b) < t->seaLevel) below++;
                else above++;
            }
        c->water = (u8)(below > 0);
        c->underwater = (u8)(above == 0);
    }
    for (i = 0; i < n; i++)                        /* water chunks: the sea fan at y = 0 is part of the box */
        if (t->chunks[i].water && t->cull[9 * i + 4] < t->seaLevel + 1.0f) t->cull[9 * i + 4] = t->seaLevel + 1.0f;
    {                                              /* 4 x 4 groups: counting sort of the chunks, group boxes */
        int ng = t->nGroupsX * t->nGroupsZ, g;
        for (i = 0; i < n; i++) t->grpStart[(t->chunks[i].cz / 4) * t->nGroupsX + t->chunks[i].cx / 4 + 1]++;
        for (g = 0; g < ng; g++) t->grpStart[g + 1] += t->grpStart[g];
        for (g = 0; g < ng; g++) {
            float *bx = t->grpBox + 6 * g;
            bx[0] = bx[1] = bx[2] = 1e30f; bx[3] = bx[4] = bx[5] = -1e30f;
        }
        for (i = 0; i < n; i++) {                  /* chunks are sorted by (cz, cx): fill in order */
            int gi = (t->chunks[i].cz / 4) * t->nGroupsX + t->chunks[i].cx / 4, q;
            float *bx = t->grpBox + 6 * gi;
            const float *f = t->cull + 9 * i;
            for (q = 0; q < 3; q++) {
                if (f[q] < bx[q]) bx[q] = f[q];
                if (f[3 + q] > bx[3 + q]) bx[3 + q] = f[3 + q];
            }
        }
        {
            int *fill = (int *)calloc((size_t)ng, sizeof(int));
            if (!fill) return bad(t, "alloc groups", ng, 0);
            for (i = 0; i < n; i++) {
                int gi = (t->chunks[i].cz / 4) * t->nGroupsX + t->chunks[i].cx / 4;
                t->grpList[t->grpStart[gi] + fill[gi]++] = i;
            }
            free(fill);
        }
    }
    memset(t->prevLod, TERRAIN_NOLOD, (size_t)(n > 0 ? n : 1));
    for (i = 0; i < n; i++) {                      /* neighbours S E N W */
        static const int ox[4] = { 0, 1, 0, -1 }, oz[4] = { -1, 0, 1, 0 };
        TerrainChunk *c = &t->chunks[i];
        for (k = 0; k < 4; k++) {
            int x = c->cx + ox[k], z = c->cz + oz[k];
            c->nb[k] = (short)((x < 0 || z < 0 || x >= t->nChunkX || z >= t->nChunkZ) ? -1 : t->chunkAt[z * t->nChunkX + x]);
        }
    }
    /* block / super block maxima (metres) */
    nb = t->nBlocksX;
    for (i = 0; i < 64; i++) t->superMax[i] = -1e9f;
    t->hTop = -1e9f;
    for (i = 0; i < nb * nb; i++) {
        float h = t->hMin + (float)t->hmax[i] * t->hScale;
        int s = (i / nb / 8) * t->nSuperX + (i % nb) / 8;
        t->blockMax[i] = h;
        if (h > t->superMax[s]) t->superMax[s] = h;
        if (h > t->hTop) t->hTop = h;
    }
    /* BASE */
    p = chunk_find(&cf, "BASE", &sz);
    if (!p || sz < 108) return bad(t, "BASE", sz, 108);
    {
        TerrainBase *b = &t->base;
        int nbx = (int)rd_u32(p + 104);
        memset(b, 0, sizeof(*b));
        b->rwyX = rd_f32(p); b->rwyZ = rd_f32(p + 4); b->rwyHeadingDeg = rd_f32(p + 8);
        b->rwyLength = rd_f32(p + 12); b->rwyWidth = rd_f32(p + 16); b->rwyHeight = rd_f32(p + 20);
        b->threshold = v3(rd_f32(p + 24), rd_f32(p + 28), rd_f32(p + 32));
        b->thresholdHeadingDeg = rd_f32(p + 36);
        for (i = 0; i < 4; i++) {
            const u8 *s = p + 40 + 16 * i;
            b->airSpawn[i] = v3(rd_f32(s), rd_f32(s + 4), rd_f32(s + 8));
            b->airSpawnHeadingDeg[i] = rd_f32(s + 12);
        }
        if (nbx < 0 || sz != 108 + 24 * nbx) return bad(t, "BASE boxes", sz, nbx);
        if (nbx > TERRAIN_MAX_BOXES) {
            log_printf(LOG_WARN, "warn: terrain base boxes=%d kept=%d", nbx, TERRAIN_MAX_BOXES);
            nbx = TERRAIN_MAX_BOXES;
        }
        b->nBoxes = nbx;
        for (i = 0; i < nbx; i++) {
            const u8 *s = p + 108 + 24 * i;
            b->box[i].lo = v3(rd_f32(s), rd_f32(s + 4), rd_f32(s + 8));
            b->box[i].hi = v3(rd_f32(s + 12), rd_f32(s + 16), rd_f32(s + 20));
        }
    }
    /* MAPI (optional) */
    p = chunk_find(&cf, "MAPI", &sz);
    if (p && sz >= 48) {
        memcpy(t->mapTex, p, 32);
        t->mapTex[31] = 0;
        for (i = 0; i < 4; i++) t->mapRect[i] = rd_f32(p + 32 + 4 * i);
    }
    t->loaded = 1;
    {
        int nw = 0, nu = 0;
        for (i = 0; i < n; i++) { nw += t->chunks[i].water; nu += t->chunks[i].underwater; }
        log_printf(LOG_INFO, "terrain: loaded path=%s bytes=%d grid=%d cell=%.0f chunks=%d water=%d underwater=%d "
                   "batches=%d verts=%d idx=%d mats=%d foam=%d top=%.1f", path, t->fileSize, N, (double)t->cell, n, nw,
                   nu, t->nBatches, t->nVerts, t->nIdx, t->nMats, t->foamMat, (double)t->hTop);
        log_printf(LOG_INFO, "terrain: base rwy=%.1f,%.1f hdg=%.1f len=%.0f wid=%.0f h=%.2f thr=%.1f,%.2f,%.1f boxes=%d "
                   "map=%s", (double)t->base.rwyX, (double)t->base.rwyZ, (double)t->base.rwyHeadingDeg,
                   (double)t->base.rwyLength, (double)t->base.rwyWidth, (double)t->base.rwyHeight,
                   (double)t->base.threshold.x, (double)t->base.threshold.y, (double)t->base.threshold.z,
                   t->base.nBoxes, t->mapTex);
    }
    return 1;
}

void terrain_free(Terrain *t)
{
    if (!t) return;
    if (t->gpu && t->gpuFree) t->gpuFree(t);       /* render side (simh never links e_terrain_draw.cpp) */
    free(t->fileData);
    free(t->chunks);
    free(t->chunkAt);
    free(t->selLod);
    free(t->selFlags);
    free(t->visList);
    free(t->blockMax);
    free(t->cull);
    free(t->grpStart);
    free(t->grpList);
    free(t->grpBox);
    free(t->prevLod);
    memset(t, 0, sizeof(*t));
    t->foamMat = -1;
}

/* ================================================================ point queries */
/* height inside cell (i, j) at local (u, w) in [0, 1]: triangles (SW, NW, NE) where w >= u, else (SW, NE, SE) */
static float cell_h(const Terrain *t, int i, int j, float u, float w)
{
    const u16 *r0 = t->hght + j * t->gridN + i, *r1 = r0 + t->gridN;
    float h00 = (float)r0[0], h10 = (float)r0[1], h01 = (float)r1[0], h11 = (float)r1[1], h;
    if (w >= u) h = h00 + w * (h01 - h00) + u * (h11 - h01);
    else h = h00 + u * (h10 - h00) + w * (h11 - h10);
    return t->hMin + h * t->hScale;
}

/* grid cell + local coordinates of (x, z); 0 = outside the grid */
static int locate(const Terrain *t, float x, float z, int *i, int *j, float *u, float *w)
{
    float fx = (x - t->x0) * t->invCell, fz = (z - t->z0) * t->invCell;
    int M = t->gridN - 1, ii, jj;
    if (!(fx >= 0.0f && fz >= 0.0f && fx <= (float)M && fz <= (float)M)) return 0;
    ii = ffloor(fx); jj = ffloor(fz);
    if (ii > M - 1) ii = M - 1;
    if (jj > M - 1) jj = M - 1;
    *i = ii; *j = jj;
    *u = fx - (float)ii; *w = fz - (float)jj;
    return 1;
}

float terrain_height(const Terrain *t, float x, float z)
{
    int i, j;
    float u, w;
    if (!t || !t->loaded || !locate(t, x, z, &i, &j, &u, &w)) return TERRAIN_OUTSIDE_H;
    return cell_h(t, i, j, u, w);
}

float terrain_surface(const Terrain *t, float x, float z)
{
    float h = terrain_height(t, x, z), s = t ? t->seaLevel : 0.0f;
    return h > s ? h : s;
}

int terrain_class(const Terrain *t, float x, float z)
{
    int i, j;
    float u, w;
    if (!t || !t->loaded || !locate(t, x, z, &i, &j, &u, &w)) return TCLASS_WATER;
    return t->clas[j * (t->gridN - 1) + i];
}

Vec3 terrain_normal(const Terrain *t, float x, float z)
{
    int i, j;
    float u, w, h00, h10, h01, h11, c;
    Vec3 n;
    if (!t || !t->loaded || !locate(t, x, z, &i, &j, &u, &w)) return v3(0.0f, 1.0f, 0.0f);
    h00 = hgt(t, i, j); h10 = hgt(t, i + 1, j); h01 = hgt(t, i, j + 1); h11 = hgt(t, i + 1, j + 1);
    c = t->cell;
    if (w >= u) n = v3(-(h11 - h01), c, -(h01 - h00));        /* dh/dx = (h11 - h01) / c, dh/dz = (h01 - h00) / c */
    else n = v3(-(h10 - h00), c, -(h11 - h10));
    return v3_norm(n);
}

float terrain_block_max(const Terrain *t, float x, float z)
{
    int i, j;
    float u, w;
    if (!t || !t->loaded || !locate(t, x, z, &i, &j, &u, &w)) return TERRAIN_OUTSIDE_H;
    return t->blockMax[(j >> 3) * t->nBlocksX + (i >> 3)];
}

float terrain_area_max(const Terrain *t, float x0, float z0, float x1, float z1)
{
    int bx0, bz0, bx1, bz1, bx, bz, nb;
    float m = TERRAIN_OUTSIDE_H, s = 1.0f;
    if (!t || !t->loaded) return m;
    nb = t->nBlocksX;
    s = t->invCell * 0.125f;
    if (x1 < x0) { float q = x0; x0 = x1; x1 = q; }
    if (z1 < z0) { float q = z0; z0 = z1; z1 = q; }
    bx0 = (int)floor((x0 - t->x0) * s); bx1 = (int)floor((x1 - t->x0) * s);
    bz0 = (int)floor((z0 - t->z0) * s); bz1 = (int)floor((z1 - t->z0) * s);
    if (bx1 < 0 || bz1 < 0 || bx0 >= nb || bz0 >= nb) return m;
    if (bx0 < 0) bx0 = 0;
    if (bz0 < 0) bz0 = 0;
    if (bx1 >= nb) bx1 = nb - 1;
    if (bz1 >= nb) bz1 = nb - 1;
    for (bz = bz0; bz <= bz1; bz++)
        for (bx = bx0; bx <= bx1; bx++)
            if (t->blockMax[bz * nb + bx] > m) m = t->blockMax[bz * nb + bx];
    return m;
}

const TerrainBase *terrain_base(const Terrain *t) { return &t->base; }

/* ================================================================ segment march */
typedef struct Seg {
    float gx, gz, dx, dz;                           /* grid space (cells): position at t = 0, change over t 0..1 */
    float idx, idz;                                 /* 1 / dx, 1 / dz (0 when the axis does not move) */
    float y, dy;                                    /* world height at t = 0, change */
    float t0, t1;                                   /* part inside the grid */
} Seg;

/* clip [t0, t1] to lo <= p + t d <= hi; 0 = empty */
static int slab(float p, float d, float lo, float hi, float *t0, float *t1)
{
    if (d > 1e-12f || d < -1e-12f) {
        float a = (lo - p) / d, b = (hi - p) / d;
        if (a > b) { float q = a; a = b; b = q; }
        if (a > *t0) *t0 = a;
        if (b < *t1) *t1 = b;
        return *t0 <= *t1;
    }
    return p >= lo && p <= hi;
}

/* slab [lo, lo + 1] with a precomputed inverse direction (0 = no motion on that axis) */
static int slab_inv(float p, float id, float lo, float *t0, float *t1)
{
    if (id != 0.0f) {
        float a = (lo - p) * id, b = (lo + 1.0f - p) * id;
        if (a > b) { float q = a; a = b; b = q; }
        if (a > *t0) *t0 = a;
        if (b < *t1) *t1 = b;
        return *t0 <= *t1;
    }
    return p >= lo && p <= lo + 1.0f;
}

static int seg_setup(const Terrain *t, const Vec3 *a, const Vec3 *b, Seg *s)
{
    float M = (float)(t->gridN - 1);
    s->gx = (a->x - t->x0) * t->invCell;
    s->gz = (a->z - t->z0) * t->invCell;
    s->dx = (b->x - a->x) * t->invCell;
    s->dz = (b->z - a->z) * t->invCell;
    s->y = a->y;
    s->dy = b->y - a->y;
    s->t0 = 0.0f;
    s->t1 = 1.0f;
    s->idx = (s->dx > 1e-12f || s->dx < -1e-12f) ? 1.0f / s->dx : 0.0f;
    s->idz = (s->dz > 1e-12f || s->dz < -1e-12f) ? 1.0f / s->dz : 0.0f;
    if (!slab(s->gx, s->dx, 0.0f, M, &s->t0, &s->t1)) return 0;
    if (!slab(s->gz, s->dz, 0.0f, M, &s->t0, &s->t1)) return 0;
    return 1;
}

/* exact test of cell (i, j) within [s->t0, s->t1]: first t with ray height <= mesh height; 1 = contact */
static int cell_hit(const Terrain *t, const Seg *s, int i, int j, float *tHit)
{
    float c0 = s->t0, c1 = s->t1, tm[3], f[3], den;
    int n = 0, k;
    if (!slab_inv(s->gx, s->idx, (float)i, &c0, &c1)) return 0;
    if (!slab_inv(s->gz, s->idz, (float)j, &c0, &c1)) return 0;
    tm[n++] = c0;
    den = s->dx - s->dz;                            /* crossing of the SW-NE diagonal: (gx - i) = (gz - j) */
    if (den > 1e-12f || den < -1e-12f) {
        float td = ((float)(i - j) - (s->gx - s->gz)) / den;
        if (td > c0 && td < c1) tm[n++] = td;
    }
    tm[n++] = c1;
    for (k = 0; k < n; k++) {
        float u = s->gx + tm[k] * s->dx - (float)i, w = s->gz + tm[k] * s->dz - (float)j;
        u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
        w = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
        f[k] = s->y + tm[k] * s->dy - cell_h(t, i, j, u, w);
    }
    for (k = 0; k + 1 < n; k++) {
        if (f[k] <= 0.0f) { *tHit = tm[k]; return 1; }
        if (f[k + 1] <= 0.0f) { *tHit = tm[k] + (tm[k + 1] - tm[k]) * f[k] / (f[k] - f[k + 1]); return 1; }
    }
    return 0;
}

/* 2D DDA over a grid of `size` cells per unit of the segment's grid space (1 = cells, 8 = blocks, 64 = supers) */
typedef struct Dda { int i, j, si, sj, n, steps; float tMx, tMz, tDx, tDz, t, tEnd; } Dda;

static void dda_start(Dda *d, const Seg *s, float L, int n, float tA, float tB)
{
    float il = 1.0f / L, px = (s->gx + tA * s->dx) * il, pz = (s->gz + tA * s->dz) * il;
    d->i = ffloor(px); d->j = ffloor(pz);
    if (d->i < 0) d->i = 0;
    if (d->j < 0) d->j = 0;
    if (d->i > n - 1) d->i = n - 1;
    if (d->j > n - 1) d->j = n - 1;
    d->n = n;
    d->t = tA;
    d->tEnd = tB;
    d->steps = 2 * n + 4;
    if (s->idx > 0.0f) { d->si = 1; d->tMx = ((float)(d->i + 1) * L - s->gx) * s->idx; d->tDx = L * s->idx; }
    else if (s->idx < 0.0f) { d->si = -1; d->tMx = ((float)d->i * L - s->gx) * s->idx; d->tDx = -L * s->idx; }
    else { d->si = 0; d->tMx = 1e30f; d->tDx = 0.0f; }
    if (s->idz > 0.0f) { d->sj = 1; d->tMz = ((float)(d->j + 1) * L - s->gz) * s->idz; d->tDz = L * s->idz; }
    else if (s->idz < 0.0f) { d->sj = -1; d->tMz = ((float)d->j * L - s->gz) * s->idz; d->tDz = -L * s->idz; }
    else { d->sj = 0; d->tMz = 1e30f; d->tDz = 0.0f; }
}

/* next cell (i, j) and its t interval [*a, *b]; 0 = done */
static int dda_next(Dda *d, int *i, int *j, float *a, float *b)
{
    float e;
    if (d->steps-- <= 0 || d->t > d->tEnd || d->i < 0 || d->j < 0 || d->i >= d->n || d->j >= d->n) return 0;
    e = d->tMx < d->tMz ? d->tMx : d->tMz;
    if (e > d->tEnd) e = d->tEnd;
    *i = d->i; *j = d->j; *a = d->t; *b = e;
    if (e >= d->tEnd) { d->t = d->tEnd + 1.0f; return 1; }
    if (d->tMx < d->tMz) { d->i += d->si; d->t = d->tMx; d->tMx += d->tDx; }
    else { d->j += d->sj; d->t = d->tMz; d->tMz += d->tDz; }
    return 1;
}

static float ymin2(const Seg *s, float a, float b)
{
    float ya = s->y + a * s->dy, yb = s->y + b * s->dy;
    return ya < yb ? ya : yb;
}

/* first terrain contact along the segment (fast path); 1 = contact */
static int march(const Terrain *t, const Vec3 *a, const Vec3 *b, float *tHit)
{
    Seg s;
    Dda ds, db, dc;
    int si, sj, bi, bj, ci, cj, nb = t->nBlocksX, N = t->gridN;
    float sa, sb, ba, bb, ca, cb;
    if (!t->loaded) return 0;
    if ((a->y > b->y ? b->y : a->y) > t->hTop + T_EPS) return 0;
    if (!seg_setup(t, a, b, &s)) return 0;
    if (ymin2(&s, s.t0, s.t1) > t->hTop + T_EPS) return 0;
    if (s.idx == 0.0f && s.idz == 0.0f) {          /* vertical: one cell */
        int i = ffloor(s.gx), j = ffloor(s.gz);
        if (i > N - 2) i = N - 2;
        if (j > N - 2) j = N - 2;
        return cell_hit(t, &s, i, j, tHit);
    }
    dda_start(&ds, &s, 64.0f, t->nSuperX, s.t0, s.t1);
    while (dda_next(&ds, &si, &sj, &sa, &sb)) {
        if (ymin2(&s, sa, sb) - T_EPS > t->superMax[sj * t->nSuperX + si]) continue;
        dda_start(&db, &s, 8.0f, nb, sa, sb);
        while (dda_next(&db, &bi, &bj, &ba, &bb)) {
            if (ymin2(&s, ba, bb) - T_EPS > t->blockMax[bj * nb + bi]) continue;
            dda_start(&dc, &s, 1.0f, N - 1, ba, bb);
            while (dda_next(&dc, &ci, &cj, &ca, &cb)) {
                const u16 *r0 = t->hght + cj * N + ci, *r1 = r0 + N;
                u16 m = r0[0];
                if (r0[1] > m) m = r0[1];
                if (r1[0] > m) m = r1[0];
                if (r1[1] > m) m = r1[1];
                if (ymin2(&s, ca, cb) - T_EPS > t->hMin + (float)m * t->hScale) continue;
                if (cell_hit(t, &s, ci, cj, tHit)) return 1;
            }
        }
    }
    return 0;
}

int terrain_los(const Terrain *t, const Vec3 *a, const Vec3 *b)
{
    float th;
    float sea = t ? t->seaLevel : 0.0f;
    if (a->y <= sea || b->y <= sea) return 0;
    if (!t) return 1;
    return !march(t, a, b, &th);
}

int terrain_segment_hit(const Terrain *t, const Vec3 *a, const Vec3 *b, Vec3 *hit)
{
    float th = 2.0f, ts = 2.0f, tt, sea = t ? t->seaLevel : 0.0f;
    if (a->y <= sea) ts = 0.0f;
    else if (b->y <= sea) ts = (a->y - sea) / (a->y - b->y);
    if (t && march(t, a, b, &tt)) th = tt;
    if (ts < th) th = ts;
    if (th > 1.0f) return 0;
    if (hit) {
        *hit = v3_lerp(*a, *b, th);
        if (th == ts) hit->y = sea;
    }
    return 1;
}

int terrain_segment_ref(const Terrain *t, const Vec3 *a, const Vec3 *b, float *tHit)
{
    Seg s;
    int i, j, i0, i1, M, found = 0;
    float best = 2.0f, th;
    if (!t || !t->loaded || !seg_setup(t, a, b, &s)) return 0;
    M = t->gridN - 1;
    {
        float xa = s.gx + s.t0 * s.dx, xb = s.gx + s.t1 * s.dx;
        if (xa > xb) { float q = xa; xa = xb; xb = q; }
        i0 = (int)floor(xa) - 1; i1 = (int)floor(xb) + 1;
    }
    if (i0 < 0) i0 = 0;
    if (i1 > M - 1) i1 = M - 1;
    for (i = i0; i <= i1; i++) {
        float c0 = s.t0, c1 = s.t1, za, zb;
        int j0, j1;
        if (!slab(s.gx, s.dx, (float)i - 0.01f, (float)(i + 1) + 0.01f, &c0, &c1)) continue;
        za = s.gz + c0 * s.dz; zb = s.gz + c1 * s.dz;
        if (za > zb) { float q = za; za = zb; zb = q; }
        j0 = (int)floor(za) - 1; j1 = (int)floor(zb) + 1;
        if (j0 < 0) j0 = 0;
        if (j1 > M - 1) j1 = M - 1;
        for (j = j0; j <= j1; j++)
            if (cell_hit(t, &s, i, j, &th) && th < best) { best = th; found = 1; }
    }
    if (found && tHit) *tHit = best;
    return found;
}

/* ================================================================ LOD selection (portable core of terrain_draw) */
static TerrainDrawCfg s_cfg;
static int s_cfgInit;
static TerrainStats s_stats;

void terrain_draw_cfg_defaults(TerrainDrawCfg *c)
{
    memset(c, 0, sizeof(*c));
    c->lodK = 28.6f;
    c->hyst = 0.10f;
    c->distMode = 1;
    c->skirtMode = 0;
    c->mergeLod = 0;
    c->seaPeriod = 128.0f;
    c->seaScrollU = -0.87f * 0.012f;
    c->seaScrollV = -0.50f * 0.012f;
    c->seaShallowAlpha = 0.65f;
    c->seaDeepFrom = 30.0f;
    c->seaDeepTo = 58.0f;
    c->seaFarShade = 0.80f;
    c->seaSegs = 16;
    c->seaZBias = 1;
    c->foamZBias = 2;
    c->shallowLod = 3;
}

TerrainDrawCfg *terrain_draw_cfg(void)
{
    if (!s_cfgInit) { terrain_draw_cfg_defaults(&s_cfg); s_cfgInit = 1; }
    return &s_cfg;
}

const TerrainStats *terrain_stats(void) { return &s_stats; }

u32 terrain_fog_565fix(u32 rgb)
{
    int r = (int)((rgb >> 16) & 0xFF) + 8, g = (int)((rgb >> 8) & 0xFF) + 4, b = (int)(rgb & 0xFF) + 8;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (rgb & 0xFF000000u) | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}
TerrainStats *terrain_stats_rw(void) { return &s_stats; }


int terrain_select(const Terrain *t, const struct WView *view, float lodScale, float farDist, int hysteresis,
                   TerrainStats *st)
{
    const TerrainDrawCfg *c = terrain_draw_cfg();
    float D2[TERRAIN_LODS - 1], U2[TERRAIN_LODS - 1], far2 = farDist * farDist;
    int i, k, g, nv = 0, nIn = 0, *inList;
    Vec3 eye;
    if (st) memset(st, 0, sizeof(*st));
    if (!t || !t->loaded || !view) { if (t) ((Terrain *)t)->nVis = 0; return 0; }
    eye = view->eye;
    if (lodScale <= 0.0f) lodScale = 1.0f;
    for (k = 0; k < TERRAIN_LODS - 1; k++) {
        float d = lodScale * c->lodK * t->lodEdge[k], u = d * (1.0f + c->hyst);
        D2[k] = d * d; U2[k] = u * u;
    }
    inList = t->visList + t->nChunks;               /* second half of the buffer: chunks that got a LOD */
    memcpy(t->prevLod, t->selLod, (size_t)t->nChunks);
    memset(t->selLod, TERRAIN_NOLOD, (size_t)t->nChunks);
    memset(t->selFlags, 0, (size_t)t->nChunks);
    for (g = 0; g < t->nGroupsX * t->nGroupsZ; g++) {
        const float *gb = t->grpBox + 6 * g;
        float dx = 0.0f, dy = 0.0f, dz = 0.0f;
        int p, m, all = 1;
        if (t->grpStart[g + 1] == t->grpStart[g]) continue;
        if (eye.x < gb[0]) dx = gb[0] - eye.x; else if (eye.x > gb[3]) dx = eye.x - gb[3];
        if (eye.y < gb[1]) dy = gb[1] - eye.y; else if (eye.y > gb[4]) dy = eye.y - gb[4];
        if (eye.z < gb[2]) dz = gb[2] - eye.z; else if (eye.z > gb[5]) dz = eye.z - gb[5];
        if (dx * dx + dy * dy + dz * dz > far2) continue;
        for (p = 0; p < 6 && all >= 0; p++) {       /* -1 out, 0 intersecting, 1 inside every plane */
            const Plane *pl = &view->fr.p[p];
            float fa = pl->a * (pl->a >= 0.0f ? gb[3] : gb[0]) + pl->b * (pl->b >= 0.0f ? gb[4] : gb[1]) +
                       pl->c * (pl->c >= 0.0f ? gb[5] : gb[2]) + pl->d;
            float na = pl->a * (pl->a >= 0.0f ? gb[0] : gb[3]) + pl->b * (pl->b >= 0.0f ? gb[1] : gb[4]) +
                       pl->c * (pl->c >= 0.0f ? gb[2] : gb[5]) + pl->d;
            if (fa < 0.0f) all = -1;
            else if (na < 0.0f) all = 0;
        }
        if (all < 0) continue;
        for (m = t->grpStart[g]; m < t->grpStart[g + 1]; m++) {
            const float *f;
            float dn2, d2;
            int cur, in = 1;
            i = t->grpList[m];
            f = t->cull + 9 * i;
            dx = dy = dz = 0.0f;
            if (eye.x < f[0]) dx = f[0] - eye.x; else if (eye.x > f[3]) dx = eye.x - f[3];
            if (eye.y < f[1]) dy = f[1] - eye.y; else if (eye.y > f[4]) dy = eye.y - f[4];
            if (eye.z < f[2]) dz = f[2] - eye.z; else if (eye.z > f[5]) dz = eye.z - f[5];
            dn2 = dx * dx + dy * dy + dz * dz;
            if (dn2 > far2) continue;
            if (!all)
                for (p = 0; p < 6; p++) {           /* chunk frustum test only for groups on a plane */
                    const Plane *pl = &view->fr.p[p];
                    if (pl->a * (pl->a >= 0.0f ? f[3] : f[0]) + pl->b * (pl->b >= 0.0f ? f[4] : f[1]) +
                        pl->c * (pl->c >= 0.0f ? f[5] : f[2]) + pl->d < 0.0f) { in = 0; break; }
                }
            if (!in) continue;
            if (c->distMode) {
                dx = f[6] - eye.x; dy = f[7] - eye.y; dz = f[8] - eye.z;
                d2 = dx * dx + dy * dy + dz * dz;
            } else d2 = dn2;
            cur = t->prevLod[i];
            if (!hysteresis || cur == TERRAIN_NOLOD) {
                for (k = 0; k < TERRAIN_LODS - 1 && d2 >= D2[k]; k++) {}
            } else {
                k = cur;
                while (k < TERRAIN_LODS - 1 && d2 > U2[k]) k++;
                while (k > 0 && d2 < D2[k - 1]) k--;
            }
            t->selLod[i] = (u8)k;
            t->selFlags[i] = TSEL_VISIBLE;
            inList[nIn++] = i;
            t->visList[nv++] = i;
        }
    }
    ((Terrain *)t)->nVis = nv;
    /* restrict: neighbouring chunks differ by at most one LOD (T03 sized the skirts for +-1); refining only */
    {
        int pass, changed = 1;
        for (pass = 0; pass < 4 && changed; pass++) {
            changed = 0;
            for (k = 0; k < nIn; k++) {
                const TerrainChunk *ch;
                int l, n;
                i = inList[k];
                l = t->selLod[i];
                if (l < 2) continue;                /* LOD 0/1 can never be more than one finer than a neighbour */
                ch = &t->chunks[i];
                for (n = 0; n < 4; n++) {
                    int nj = ch->nb[n];
                    if (nj >= 0 && t->selLod[nj] + 1 < l) l = t->selLod[nj] + 1;   /* NOLOD (255) never lowers */
                }
                if (l != t->selLod[i]) { t->selLod[i] = (u8)l; changed = 1; }
            }
        }
    }
    for (k = 0; k < nv; k++) {
        const TerrainChunk *ch;
        int ci = t->visList[k], lod = t->selLod[ci], ml, n, mask = 0, skt = 0;
        ch = &t->chunks[ci];
        ml = ch->meshLod[lod];
        for (n = 0; n < 4; n++) {
            int nj = ch->nb[n], need = 0;
            if (c->skirtMode == 1) need = 1;
            else if (c->skirtMode == 0 && nj >= 0 && t->selLod[nj] != TERRAIN_NOLOD)
                need = t->chunks[nj].meshLod[t->selLod[nj]] != ml;
            if (need && ch->skirtSide[lod][n]) { mask |= TSEL_SKIRT_S << n; skt += ch->skirtSide[lod][n]; }
        }
        if (mask) t->selFlags[ci] |= (u8)(TSEL_SKIRT | mask);
        if (st) {
            st->chunksByLod[lod]++;
            st->tris += ch->trisNoSkirt[lod] + skt;
            st->skirtTris += skt;
            st->skirtChunks += mask != 0;
            if (ml <= 2) st->foamTris += ch->foamTris[lod];
        }
    }
    if (st) st->inRange = nIn;
    return nv;
}
