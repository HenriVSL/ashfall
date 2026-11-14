/* e_terrain_draw.cpp - terrain chunks, sea ring, shallow sea, foam (DESIGN 8.2, 8.3, 9.1b). Render side (links the
   renderer); not part of tools/simh. Owner: T04. Notes: engine/world/e_terrain.md.
   terrain_gpu_init: reorders every chunk-LOD vertex block IN PLACE (t->verts / t->idx) so each batch's vertices are
   contiguous, top vertices first and skirt-bottom vertices last (the batch's indices become relative to its first
   vertex). Then a batch draws with r_draw_mem straight from that block (transforms only its own vertices, no
   skirt bottoms when its skirts are skipped), or is copied into a per-material merge buffer (cfg.mergeLod).
   terrain_draw: terrain_select (LOD, hysteresis, frustum, skirt flags), then per material (texture) one pass over the
   visible chunks: <= 5 texture changes, draws = materials + direct batches.
   sea_draw (call after the opaque scene, before the late sky): 1. shallow sea = flat fans over every water chunk
   (stored chunk with a sample below sea level), RB_BLEND with Z write and zBias seaZBias, vertex alpha
   seaShallowAlpha over shallow water ramping to 1 at seaDeepFrom..seaDeepTo m depth (so it meets the opaque sea
   without a seam); fans use 256 m (LOD <= 3) or 512 m lattices, the 256 m fan drops the edge midpoint toward a 512 m
   neighbour (no T-junctions); 2. opaque ring (radii 0.4/1.5/4/9/18 km + 1.06 x far, 16 segments, per-quad frustum
   cull), coplanar with the fans and Z-masked by them, so the shelf terrain shows through the fans only; 3. foam
   strips of the chunks at mesh LOD 0-2, RB_ADD with a black fog (fades out instead of turning fog-coloured).
   Sea UVs: (world xz - camera xz snapped to the period) / period + scroll (camera-relative, 9.1b UV rule). */
#include "e_terrain.h"
#include "e_world.h"
#include "e_rend.h"
#include "e_tex.h"
#include "e_log.h"

enum { MERGE_V = 16384, MERGE_I = 3 * 16384, RING_MAX = 8, SEG_MAX = 32, SH_MAXV = 9 * 512, SH_MAXI = 24 * 512 };

typedef struct GBatch {
    const RVert1 *v;                                /* first vertex of the batch (reordered block) */
    const u16 *idx;                                 /* indices relative to v: top triangles, then skirts S E N W */
    int nvTop, nvAll, nIdxTop, nIdxAll, mat;
    int sideOk;                                     /* skirts sorted by side: sk0/skn valid */
    int sk0[4], skn[4];                             /* index offset / count of each side's skirt triangles */
} GBatch;

struct TerrainGpu {
    GBatch *b;                                      /* per BTCH record */
    RTex tex[TERRAIN_MAX_MATS];
    RMaterial rm[TERRAIN_MAX_MATS];
    RTex seaTex;
    RMaterial seaOpaque, seaShallow, seaFog, foam;
    int reordered, shared;
};

static RVert1 s_mv[MERGE_V];
static u16 s_mi[MERGE_I];
static int s_mnv, s_mni;
static const GBatch *s_list[TERRAIN_MAX_MATS][2048];
static u8 s_listSkirt[TERRAIN_MAX_MATS][2048], s_listLod[TERRAIN_MAX_MATS][2048];
static int s_nList[TERRAIN_MAX_MATS];
static RVert1 s_sv[SH_MAXV];
static u16 s_si[SH_MAXI];

static void gpu_free_hook(Terrain *t) { terrain_gpu_free(t); }

/* ---------------------------------------------------------------- init: reorder blocks, textures, materials */
static u16 s_sideTmp[3 * 65536];

static int reorder_block(Terrain *t, const TerrainChunk *ch, GBatch *gb, int vFirst, int vCount, int bFirst,
                         int bCount, int *remap, RVert1 *tmp)
{
    RVert1 *blk = t->verts + vFirst;
    int i, m, pos = 0, shared = 0;
    for (i = 0; i < vCount; i++) remap[i] = -1;
    for (m = 0; m < bCount && !shared; m++) {
        const TerrainBatchRec *br = &t->batches[bFirst + m];
        GBatch *g = &gb[bFirst + m];
        int start = pos, pass;
        u32 q, nTop = br->iCount - 3u * br->skirtTris;
        for (pass = 0; pass < 2 && !shared; pass++) {
            u32 q0 = pass ? nTop : 0, q1 = pass ? br->iCount : nTop;
            for (q = q0; q < q1; q++) {
                int v = t->idx[br->iFirst + q];
                if (remap[v] < 0) { remap[v] = pos; tmp[pos] = blk[v]; pos++; }
                else if (remap[v] < start) { shared = 1; break; }
            }
            if (pass == 0) g->nvTop = pos - start;
        }
        g->nvAll = pos - start;
    }
    if (shared) return 0;
    for (m = 0; m < bCount; m++) {                  /* indices relative to the batch's first vertex */
        const TerrainBatchRec *br = &t->batches[bFirst + m];
        GBatch *g = &gb[bFirst + m];
        u16 *ix = t->idx + br->iFirst;
        int start = -1, sd, nTop = (int)br->iCount - 3 * (int)br->skirtTris, o;
        u32 q;
        for (q = 0; q < br->iCount; q++) {
            int r = remap[ix[q]];
            if (start < 0 || r < start) start = r;
        }
        if (start < 0) start = 0;
        /* skirt triangles grouped by side (classified on the original positions), into s_sideTmp */
        o = 0;
        g->sideOk = 1;
        for (sd = 0; sd < 4; sd++) {
            int tt;
            g->sk0[sd] = nTop + o;
            for (tt = nTop; tt < (int)br->iCount; tt += 3) {
                int side = terrain_tri_side(t, ch, blk + ix[tt], blk + ix[tt + 1], blk + ix[tt + 2]);
                if (side < 0) g->sideOk = 0;
                if (side != sd) continue;
                s_sideTmp[o] = ix[tt]; s_sideTmp[o + 1] = ix[tt + 1]; s_sideTmp[o + 2] = ix[tt + 2];
                o += 3;
            }
            g->skn[sd] = nTop + o - g->sk0[sd];
        }
        if (o == (int)br->iCount - nTop) memcpy(ix + nTop, s_sideTmp, (size_t)o * sizeof(u16));
        else g->sideOk = 0;
        for (q = 0; q < br->iCount; q++) ix[q] = (u16)(remap[ix[q]] - start);
        g->v = blk + start;
    }
    for (i = 0; i < vCount; i++) if (remap[i] < 0) tmp[pos++] = blk[i];   /* unreferenced: keep at the end */
    memcpy(blk, tmp, (size_t)vCount * sizeof(RVert1));
    return 1;
}

int terrain_gpu_init(Terrain *t)
{
    struct TerrainGpu *g;
    int *remap;
    RVert1 *tmp;
    int c, k, m, ok = 1, nShared = 0, nBlocks = 0;
    if (!t) return 0;
    if (t->gpu) return 1;
    g = (struct TerrainGpu *)calloc(1, sizeof(*g));
    if (!g) return 0;
    g->b = (GBatch *)calloc((size_t)(t->nBatches > 0 ? t->nBatches : 1), sizeof(GBatch));   /* no island: sea only */
    remap = (int *)malloc(65536 * sizeof(int));
    tmp = (RVert1 *)malloc(65536 * sizeof(RVert1));
    if (!g->b || !remap || !tmp) {
        free(g->b); free(g); free(remap); free(tmp);
        log_printf(LOG_ERR, "err: terrain gpu alloc");
        return 0;
    }
    for (c = 0; c < t->nChunks; c++) {
        const TerrainChunk *ch = &t->chunks[c];
        for (k = 0; k < TERRAIN_LODS; k++) {
            if (ch->meshLod[k] != k) continue;      /* alias of a coarser record: done with that one */
            for (m = 0; m < ch->bCount[k]; m++) {   /* defaults (used as is if the block cannot be reordered) */
                const TerrainBatchRec *br = &t->batches[ch->bFirst[k] + m];
                GBatch *gb = &g->b[ch->bFirst[k] + m];
                gb->v = t->verts + ch->vFirst[k];
                gb->idx = t->idx + br->iFirst;
                gb->nvTop = gb->nvAll = ch->vCount[k];
                gb->nIdxAll = (int)br->iCount;
                gb->nIdxTop = (int)br->iCount - 3 * (int)br->skirtTris;
                gb->mat = br->mat;
            }
            nBlocks++;
            if (!reorder_block(t, ch, g->b, ch->vFirst[k], ch->vCount[k], ch->bFirst[k], ch->bCount[k], remap, tmp)) {
                nShared++;
                for (m = 0; m < ch->bCount[k]; m++) {
                    GBatch *gb = &g->b[ch->bFirst[k] + m];
                    gb->v = t->verts + ch->vFirst[k];
                    gb->nvTop = gb->nvAll = ch->vCount[k];
                    gb->sideOk = 0;
                }
            }
        }
    }
    free(remap);
    free(tmp);
    g->reordered = nBlocks - nShared;
    g->shared = nShared;
    for (m = 0; m < t->nMats; m++) {
        RMaterial *rm = &g->rm[m];
        u32 f = t->mats[m].flags;
        memset(rm, 0, sizeof(*rm));
        g->tex[m] = tex_get(t->mats[m].tex, 0, 0);
        if (!g->tex[m]) ok = 0;
        rm->tex0 = g->tex[m];
        rm->blend = (u8)(f & TMF_BLEND_MASK);
        rm->cullNone = (u8)((f & TMF_NOCULL) ? 1 : 0);
        rm->zTest = 1;
        rm->zWrite = (u8)(rm->blend == RB_OPAQUE || rm->blend == RB_ALPHATEST);
        rm->fog = 1;
        rm->zBias = (u8)((f & TMF_ZBIAS1) ? 1 : 0);
    }
    g->seaTex = tex_get("sea", 0, 0);
    if (!g->seaTex) ok = 0;
    memset(&g->seaOpaque, 0, sizeof(RMaterial));
    g->seaOpaque.tex0 = g->seaTex;
    g->seaOpaque.blend = RB_OPAQUE;
    g->seaOpaque.cullNone = 1;
    g->seaOpaque.zTest = 1;
    g->seaOpaque.zWrite = 1;
    g->seaOpaque.fog = 1;
    g->seaShallow = g->seaOpaque;
    g->seaShallow.blend = RB_BLEND;
    g->seaFog = g->seaShallow;                      /* fog completion band: untextured, no Z write */
    g->seaFog.tex0 = 0;
    g->seaFog.zWrite = 0;
    if (t->foamMat >= 0) {
        g->foam = g->rm[t->foamMat];
        g->foam.blend = RB_ADD;
        g->foam.zWrite = 0;
        g->foam.cullNone = 1;
    }
    t->gpu = g;
    t->gpuFree = gpu_free_hook;
    log_printf(LOG_INFO, "terrain: gpu_init blocks=%d reordered=%d shared_fallback=%d textures_ok=%d tex_kb=%d", nBlocks,
               g->reordered, g->shared, ok, r_tex_kb());
    return 1;
}

void terrain_gpu_free(Terrain *t)
{
    int m;
    if (!t || !t->gpu) return;
    for (m = 0; m < TERRAIN_MAX_MATS; m++) if (t->gpu->tex[m]) tex_release(t->gpu->tex[m]);
    if (t->gpu->seaTex) tex_release(t->gpu->seaTex);
    free(t->gpu->b);
    free(t->gpu);
    t->gpu = NULL;
}

/* ---------------------------------------------------------------- merge buffer */
static int s_draws, s_verts;

static void merge_flush(void)
{
    if (s_mni >= 3) { r_draw_mem(RFMT_V1, s_mv, s_mnv, s_mi, s_mni); s_draws++; s_verts += s_mnv; }
    s_mnv = s_mni = 0;
}

static void merge_add(const RVert1 *v, int nv, const u16 *idx, int ni)
{
    int i;
    if (nv > MERGE_V || ni > MERGE_I) { r_draw_mem(RFMT_V1, v, nv, idx, ni); s_draws++; s_verts += nv; return; }
    if (s_mnv + nv > MERGE_V || s_mni + ni > MERGE_I) merge_flush();
    memcpy(s_mv + s_mnv, v, (size_t)nv * sizeof(RVert1));
    for (i = 0; i < ni; i++) s_mi[s_mni + i] = (u16)(idx[i] + s_mnv);
    s_mnv += nv;
    s_mni += ni;
}

/* top triangles + the skirt triangles of the sides in mask (TSEL_SKIRT_S << side) */
static void merge_add_sides(const GBatch *gb, int mask)
{
    int i, s, n = gb->nIdxTop, base;
    for (s = 0; s < 4; s++) if (mask & (TSEL_SKIRT_S << s)) n += gb->skn[s];
    if (gb->nvAll > MERGE_V || n > MERGE_I) {
        r_draw_mem(RFMT_V1, gb->v, gb->nvAll, gb->idx, gb->nIdxAll);
        s_draws++; s_verts += gb->nvAll;
        return;
    }
    if (s_mnv + gb->nvAll > MERGE_V || s_mni + n > MERGE_I) merge_flush();
    base = s_mnv;
    memcpy(s_mv + s_mnv, gb->v, (size_t)gb->nvAll * sizeof(RVert1));
    s_mnv += gb->nvAll;
    for (i = 0; i < gb->nIdxTop; i++) s_mi[s_mni++] = (u16)(gb->idx[i] + base);
    for (s = 0; s < 4; s++) {
        const u16 *ix;
        if (!(mask & (TSEL_SKIRT_S << s))) continue;
        ix = gb->idx + gb->sk0[s];
        for (i = 0; i < gb->skn[s]; i++) s_mi[s_mni++] = (u16)(ix[i] + base);
    }
}

static void set_camera(const WView *v)
{
    r_set_proj(&v->proj);
    r_set_view(&v->view);
    r_set_world(NULL);
}

/* ---------------------------------------------------------------- terrain */
void terrain_draw(const Terrain *t, const struct WView *view, float lodScale, float farDist)
{
    const TerrainDrawCfg *c = terrain_draw_cfg();
    TerrainStats *st = terrain_stats_rw();
    const struct TerrainGpu *g;
    int i, m, n;
    memset(st, 0, sizeof(*st));
    if (!t || !t->loaded || !t->gpu || !view) return;
    g = t->gpu;
    n = terrain_select(t, view, lodScale, farDist, 1, st);
    for (m = 0; m < TERRAIN_MAX_MATS; m++) s_nList[m] = 0;
    for (i = 0; i < n; i++) {
        int ci = t->visList[i], lod = t->selLod[ci], b;
        const TerrainChunk *ch = &t->chunks[ci];
        for (b = 0; b < ch->bCount[lod]; b++) {
            const GBatch *gb = &g->b[ch->bFirst[lod] + b];
            int mm = gb->mat;
            if (mm == t->foamMat || mm < 0 || mm >= TERRAIN_MAX_MATS || s_nList[mm] >= 2048) continue;
            s_listSkirt[mm][s_nList[mm]] = t->selFlags[ci];
            s_listLod[mm][s_nList[mm]] = ch->meshLod[lod];
            s_list[mm][s_nList[mm]++] = gb;
        }
    }
    set_camera(view);
    s_draws = s_verts = 0;
    s_mnv = s_mni = 0;
    for (m = 0; m < t->nMats; m++) {
        if (!s_nList[m]) continue;
        r_set_material(&g->rm[m]);
        for (i = 0; i < s_nList[m]; i++) {
            const GBatch *gb = s_list[m][i];
            int fl = s_listSkirt[m][i], sk = fl & TSEL_SKIRT, ml = s_listLod[m][i];
            int nv = sk ? gb->nvAll : gb->nvTop, ni = sk ? gb->nIdxAll : gb->nIdxTop;
            if (ni < 3) continue;
            if (sk && gb->sideOk && ml >= c->mergeLod) merge_add_sides(gb, fl);
            else if (ml < c->mergeLod) {            /* finer than mergeLod: draw in place (no copy, all skirts) */
                r_draw_mem(RFMT_V1, gb->v, nv, gb->idx, ni);
                s_draws++; s_verts += nv;
            } else merge_add(gb->v, nv, gb->idx, ni);
        }
        merge_flush();
    }
    st->draws = s_draws;
    st->verts = s_verts;
}

/* ---------------------------------------------------------------- sea */
typedef struct SeaCtx {
    Vec3 eye; float snapX, snapZ, invP, su, sv, farDist, shadeK, invFar2;
    float rOut;                                     /* T10b: ring outer radius (covers the whole frustum to farDist) */
    Vec3 fwd; float seaY;                           /* T10b: camera forward (view depth), sea level */
} SeaCtx;

static void sea_vert(const SeaCtx *s, RVert1 *o, float x, float z, int alpha)
{
    float dx = x - s->eye.x, dz = z - s->eye.z, q = (dx * dx + dz * dz) * s->invFar2;
    float f = 1.0f - s->shadeK * (q < 1.0f ? q : 1.0f);   /* darkens with horizontal distance squared (no sqrt; T10b:
                                                             without the eye height, which shaded a disc from altitude) */
    int g = e_ftoi(255.0f * f);
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    o->x = x; o->y = 0.0f; o->z = z;
    o->color = ((u32)alpha << 24) | ((u32)g << 16) | ((u32)g << 8) | (u32)g;
    o->u = (x - s->snapX) * s->invP + s->su;
    o->v = (z - s->snapZ) * s->invP + s->sv;
}

static int shallow_alpha(const Terrain *t, int I, int J, const TerrainDrawCfg *c)
{
    float h = t->hMin + (float)t->hght[J * t->gridN + I] * t->hScale, depth = t->seaLevel - h, a, k;
    if (depth <= c->seaDeepFrom) a = c->seaShallowAlpha;
    else if (depth >= c->seaDeepTo) a = 1.0f;
    else {
        k = (depth - c->seaDeepFrom) / (c->seaDeepTo - c->seaDeepFrom);
        a = c->seaShallowAlpha + (1.0f - c->seaShallowAlpha) * k;
    }
    return e_ftoi(a * 255.0f);
}

static int fan_level(const Terrain *t, int ci, const TerrainDrawCfg *c)
{
    int l = t->selLod[ci];
    if (l == TERRAIN_NOLOD) return -1;
    return (c->shallowLod <= 3 && l <= 3) ? 3 : 4;
}

/* shallow sea fans over the visible water chunks; returns triangles. Vertices are emitted only when a triangle uses
   them; chunks deep at every lattice point add nothing (the opaque ring shows the same there). */
static int draw_shallow(const Terrain *t, const WView *v, const SeaCtx *sc, const TerrainDrawCfg *c, int *draws)
{
    static const int bnd[8] = { 0, 1, 2, 5, 8, 7, 6, 3 };      /* boundary of the 3 x 3 lattice, midpoints 1 5 7 3 */
    static const int mid[4] = { 1, 5, 7, 3 };                  /* S E N W */
    int i, nv = 0, ni = 0, tris = 0;
    for (i = 0; i < t->nChunks; i++) {
        const TerrainChunk *ch = &t->chunks[i];
        int lvl, I0, J0, s, a[9], vi[9], tri[8][3], nt = 0, k, step, deep = 1;
        if (!ch->water || (lvl = fan_level(t, i, c)) < 0) continue;   /* NOLOD = not visible (select's sea box) */
        I0 = ch->cx * t->chunkCells;
        J0 = ch->cz * t->chunkCells;
        step = t->chunkCells / 2;
        for (k = 0; k < 9; k++) {                   /* lattice (k % 3, k / 3) at 256 m */
            if (lvl == 4 && ((k & 1) || k == 4)) a[k] = 255;      /* 512 m fan: corners only */
            else a[k] = shallow_alpha(t, I0 + (k % 3) * step, J0 + (k / 3) * step, c);
            if (a[k] < 255) deep = 0;
        }
        if (deep) continue;
        if (nv + 9 > SH_MAXV || ni + 24 > SH_MAXI) break;
        if (lvl == 4) {                             /* 512 m: corners 0 2 6 8, triangles (0, 6, 8) (0, 8, 2) */
            if (a[0] < 255 || a[6] < 255 || a[8] < 255) { tri[nt][0] = 0; tri[nt][1] = 6; tri[nt][2] = 8; nt++; }
            if (a[0] < 255 || a[8] < 255 || a[2] < 255) { tri[nt][0] = 0; tri[nt][1] = 8; tri[nt][2] = 2; nt++; }
        } else {                                    /* 256 m fan around the centre (4) */
            int ring[8], nr = 0;
            for (k = 0; k < 8; k++) {
                int id = bnd[k], d = 0;
                for (s = 0; s < 4; s++) {
                    int nj = ch->nb[s];
                    if (id == mid[s] && nj >= 0 && t->chunks[nj].water && fan_level(t, nj, c) == 4) d = 1;
                }
                if (!d) ring[nr++] = id;
            }
            for (k = 0; k < nr; k++) {
                int p = ring[k], q = ring[(k + 1) % nr];
                if (a[4] == 255 && a[p] == 255 && a[q] == 255) continue;
                tri[nt][0] = 4; tri[nt][1] = p; tri[nt][2] = q; nt++;
            }
        }
        for (k = 0; k < 9; k++) vi[k] = -1;
        for (k = 0; k < nt; k++)
            for (s = 0; s < 3; s++) {
                int id = tri[k][s];
                if (vi[id] < 0) {
                    vi[id] = nv;
                    sea_vert(sc, &s_sv[nv++], t->x0 + (float)(I0 + (id % 3) * step) * t->cell,
                             t->z0 + (float)(J0 + (id / 3) * step) * t->cell, a[id]);
                }
                s_si[ni++] = (u16)vi[id];
            }
        tris += nt;
    }
    if (ni >= 3) {
        RMaterial m = t->gpu->seaShallow;
        m.zBias = (u8)c->seaZBias;
        r_set_material(&m);
        r_draw_mem(RFMT_V1, s_sv, nv, s_si, ni);
        (*draws)++;
    }
    return tris;
}

/* T10b: view depth of a sea-plane point (sc->fwd = camera forward, world space) */
static float ring_w(const SeaCtx *sc, float x, float z)
{
    return sc->fwd.x * (x - sc->eye.x) + sc->fwd.y * (sc->seaY - sc->eye.y) + sc->fwd.z * (z - sc->eye.z);
}

/* opaque ring mesh under the camera, then (with cfg fog) the fog completion band: the Voodoo fog table tops out at
   ~96 %, which leaves a 565 step where the dark sea meets the sky's fog-coloured bottom, so a fog-coloured strip on
   the sea plane blends in from 0 at view depth w = 0.85 x fogEnd to 1 at w = farDist.
   T10b: the band follows the view depth w (what the W fog table and the planar far plane use), not the horizontal
   radius, and the ring reaches the far plane's corners (sc->rOut): radial limits drew a disc / an arc horizon seen
   from altitude (the user's "ball"). Returns triangles (both passes). */
static int draw_ring(const Terrain *t, const WView *v, const SeaCtx *sc, const TerrainDrawCfg *c, int *draws)
{
    static const float base[] = { 400.0f, 1500.0f, 4000.0f, 9000.0f, 18000.0f };
    float rad[RING_MAX], cs[SEG_MAX], sn[SEG_MAX], fe0 = c->fogEnd > 0.0f ? 0.85f * c->fogEnd : 1e30f;
    float wLim = 0.998f * sc->farDist;
    int nr = 0, segs = c->seaSegs, r, s, nv, ni = 0, tris = 0;
    if (segs < 6) segs = 6;
    if (segs > SEG_MAX) segs = SEG_MAX;
    for (r = 0; r < 5; r++) {
        if (base[r] >= sc->farDist || base[r] >= fe0) break;
        rad[nr++] = base[r];
    }
    if (fe0 < sc->farDist) rad[nr++] = fe0;
    if (!nr || sc->farDist > rad[nr - 1]) rad[nr++] = sc->farDist;
    rad[nr++] = sc->rOut > sc->farDist * 1.06f ? sc->rOut : sc->farDist * 1.06f;
    for (s = 0; s < segs; s++) {
        float a = 2.0f * E_PI * (float)s / (float)segs;
        cs[s] = (float)cos(a); sn[s] = (float)sin(a);
    }
    sea_vert(sc, &s_sv[0], sc->eye.x, sc->eye.z, 255);
    nv = 1;
    for (r = 0; r < nr; r++)
        for (s = 0; s < segs; s++) sea_vert(sc, &s_sv[nv++], sc->eye.x + rad[r] * sn[s], sc->eye.z + rad[r] * cs[s], 255);
    for (r = 0; r < nr; r++) {
        float r0 = r ? rad[r - 1] : 0.0f, r1 = rad[r];
        for (s = 0; s < segs; s++) {
            int s1 = (s + 1) % segs, a1 = 1 + r * segs + s, b1 = 1 + r * segs + s1;
            Aabb box;
            float xs[4], zs[4];
            int q;
            xs[0] = r0 * sn[s]; zs[0] = r0 * cs[s]; xs[1] = r0 * sn[s1]; zs[1] = r0 * cs[s1];
            xs[2] = r1 * sn[s]; zs[2] = r1 * cs[s]; xs[3] = r1 * sn[s1]; zs[3] = r1 * cs[s1];
            box.lo = v3(1e30f, t->seaLevel - 0.5f, 1e30f);
            box.hi = v3(-1e30f, t->seaLevel + 0.5f, -1e30f);
            for (q = 0; q < 4; q++) {
                float x = sc->eye.x + xs[q], z = sc->eye.z + zs[q];
                if (x < box.lo.x) box.lo.x = x;
                if (x > box.hi.x) box.hi.x = x;
                if (z < box.lo.z) box.lo.z = z;
                if (z > box.hi.z) box.hi.z = z;
            }
            if (!frustum_aabb(&v->fr, &box)) continue;
            {   /* T10b: clip to w <= far here (the renderer only rejects beyond far, and the Voodoo's fog/W wrap for
                   w beyond ~65.5 km: unclipped ring quads showed as an unfogged dark band above the horizon) */
                int poly[4], np = 0, k, out = 0, all = 1;
                if (r == 0) { poly[np++] = 0; poly[np++] = a1; poly[np++] = b1; }
                else {
                    poly[np++] = 1 + (r - 1) * segs + s; poly[np++] = a1; poly[np++] = b1;
                    poly[np++] = 1 + (r - 1) * segs + s1;
                }
                for (k = 0; k < np; k++) {
                    float w = ring_w(sc, s_sv[poly[k]].x, s_sv[poly[k]].z);
                    if (w > wLim) out++;
                    else all = 0;
                }
                if (all) continue;                  /* entirely beyond the far plane */
                if (!out) {
                    for (k = 1; k + 1 < np; k++) {
                        s_si[ni++] = (u16)poly[0]; s_si[ni++] = (u16)poly[k]; s_si[ni++] = (u16)poly[k + 1];
                        tris++;
                    }
                } else if (nv + 6 < SH_MAXV && ni + 12 < SH_MAXI) {
                    int cp[8], nc = 0;
                    for (k = 0; k < np; k++) {
                        const RVert1 *pa = &s_sv[poly[k]], *pb = &s_sv[poly[(k + 1) % np]];
                        float wa = ring_w(sc, pa->x, pa->z) - wLim, wb = ring_w(sc, pb->x, pb->z) - wLim;
                        if (wa <= 0.0f) cp[nc++] = poly[k];
                        if ((wa <= 0.0f) != (wb <= 0.0f)) {
                            float u = wa / (wa - wb);
                            sea_vert(sc, &s_sv[nv], pa->x + (pb->x - pa->x) * u, pa->z + (pb->z - pa->z) * u, 255);
                            cp[nc++] = nv++;
                        }
                    }
                    for (k = 1; k + 1 < nc; k++) {
                        s_si[ni++] = (u16)cp[0]; s_si[ni++] = (u16)cp[k]; s_si[ni++] = (u16)cp[k + 1];
                        tris++;
                    }
                }
            }
        }
    }
    if (ni >= 3) {
        r_set_material(&t->gpu->seaOpaque);
        r_draw_mem(RFMT_V1, s_sv, nv, s_si, ni);
        (*draws)++;
    }
    if (fe0 >= sc->farDist) return tris;
    /* fog completion band: a strip on the sea plane between the lines w = fe0 and w = farDist (w = depth along the
       view direction f; on the plane w = w0 + hl * d, d = horizontal distance along f's horizontal direction g),
       4 pieces along d, sideways +-rOut; alpha linear in w at every vertex. zBias 2 over the coplanar ring and the
       shallow fans (bias 1), so it fogs both. Looking straight down (hl ~ 0) w is the same everywhere: one square. */
    {
        enum { BAND_PIECES = 4 };
        float fx = v->view.m[0][2], fy = v->view.m[1][2], fz = v->view.m[2][2];
        float hl = (float)sqrt(fx * fx + fz * fz), R = rad[nr - 1], w0 = fy * (t->seaLevel - sc->eye.y);
        float gx, gz, d0, d1, span = sc->farDist - fe0;
        RMaterial bm = t->gpu->seaFog;
        int k;
        if (hl > 1e-4f) {
            gx = fx / hl; gz = fz / hl;
            d0 = (fe0 - w0) / hl; d1 = (sc->farDist - w0) / hl;
            if (d0 < -R) d0 = -R;
            if (d1 > R) d1 = R;
        } else {
            gx = 0.0f; gz = 1.0f; d0 = -R; d1 = R;
        }
        if (d1 <= d0 || span <= 1.0f) return tris;
        if (hl <= 1e-4f && w0 <= fe0) return tris;
        nv = 0; ni = 0;
        for (k = 0; k <= BAND_PIECES; k++) {
            float d = d0 + (d1 - d0) * (float)k / (float)BAND_PIECES, w = w0 + hl * d, a = (w - fe0) / span;
            float cx = sc->eye.x + gx * d, cz = sc->eye.z + gz * d;
            u32 col = ((u32)e_ftoi(255.0f * E_CLAMP(a, 0.0f, 1.0f)) << 24) | (c->fogRGB & 0xFFFFFF);
            for (s = 0; s < 2; s++) {
                float sd = s ? R : -R;
                RVert1 *o = &s_sv[nv++];
                o->x = cx - gz * sd; o->y = t->seaLevel; o->z = cz + gx * sd;
                o->color = col; o->u = o->v = 0.0f;
            }
            if (k) {
                int a0 = nv - 4;
                s_si[ni++] = (u16)a0; s_si[ni++] = (u16)(a0 + 2); s_si[ni++] = (u16)(a0 + 3);
                s_si[ni++] = (u16)a0; s_si[ni++] = (u16)(a0 + 3); s_si[ni++] = (u16)(a0 + 1);
                tris += 2;
            }
        }
        bm.zBias = 2;
        r_set_material(&bm);
        r_draw_mem(RFMT_V1, s_sv, nv, s_si, ni);
        (*draws)++;
    }
    return tris;
}

static int draw_foam(const Terrain *t, const TerrainDrawCfg *c, int *draws)
{
    const struct TerrainGpu *g = t->gpu;
    RMaterial fm = g->foam;
    int i, tris = 0;
    if (t->foamMat < 0) return 0;
    fm.zBias = (u8)c->foamZBias;
    s_mnv = s_mni = 0;
    s_draws = 0;
    for (i = 0; i < t->nVis; i++) {
        int ci = t->visList[i], lod = t->selLod[ci], b;
        const TerrainChunk *ch = &t->chunks[ci];
        if (ch->meshLod[lod] > 2) continue;
        for (b = 0; b < ch->bCount[lod]; b++) {
            const GBatch *gb = &g->b[ch->bFirst[lod] + b];
            if (gb->mat != t->foamMat || gb->nIdxAll < 3) continue;
            if (!tris) {
                r_set_material(&fm);
                if (c->fogEnd > 0.0f) r_set_fog(1, 0x000000, c->fogStart, c->fogEnd);
            }
            merge_add(gb->v, gb->nvAll, gb->idx, gb->nIdxAll);
            tris += gb->nIdxAll / 3;
        }
    }
    merge_flush();
    if (tris && c->fogEnd > 0.0f) r_set_fog(1, c->fogRGB, c->fogStart, c->fogEnd);
    *draws += s_draws;
    return tris;
}

void sea_draw(const Terrain *t, const struct WView *view, float time)
{
    const TerrainDrawCfg *c = terrain_draw_cfg();
    TerrainStats *st = terrain_stats_rw();
    SeaCtx sc;
    float P = c->seaPeriod > 1.0f ? c->seaPeriod : 128.0f, su, sv;
    int draws = 0;
    if (!view || !t || !t->gpu) return;
    set_camera(view);
    sc.eye = view->eye;
    sc.invP = 1.0f / P;
    sc.snapX = (float)floor(view->eye.x / P) * P;
    sc.snapZ = (float)floor(view->eye.z / P) * P;
    su = time * c->seaScrollU; sv = time * c->seaScrollV;
    sc.su = su - (float)floor(su);
    sc.sv = sv - (float)floor(sv);
    sc.farDist = view->viewDist > 100.0f ? view->viewDist : 18000.0f;
    {   /* T10b: ring outer radius = the far plane's corner distance / cos(half a segment) (chords stay outside) */
        float tx = view->proj.m[0][0] > 1e-4f ? 1.0f / view->proj.m[0][0] : 1.0f;
        float ty = view->proj.m[1][1] > 1e-4f ? 1.0f / view->proj.m[1][1] : 1.0f;
        int segs = c->seaSegs < 6 ? 6 : (c->seaSegs > SEG_MAX ? SEG_MAX : c->seaSegs);
        sc.rOut = 1.01f * sc.farDist * (float)sqrt(1.0f + tx * tx + ty * ty) / (float)cos(E_PI / (float)segs);
    }
    sc.fwd = v3(view->view.m[0][2], view->view.m[1][2], view->view.m[2][2]);
    sc.seaY = t->seaLevel;
    sc.shadeK = 1.0f - c->seaFarShade;
    sc.invFar2 = 1.0f / (sc.farDist * sc.farDist);
    st->shallowTris = t->loaded ? draw_shallow(t, view, &sc, c, &draws) : 0;
    st->seaTris = draw_ring(t, view, &sc, c, &draws);
    st->foamTris = t->loaded ? draw_foam(t, c, &draws) : 0;
    st->seaDraws = draws;
}
