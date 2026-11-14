/* e_world.cpp - world loader (.trk, DESIGN 4.5; payload layouts: tools/trackc/trkfile.py + assetkit/export.py),
   cell/batch/instance culling (4.7), material-sorted drawing (3.5), placeholder sky. Owner: T07.
   Loading and world_visible are portable; everything that calls the renderer is inside #ifndef E_NO_RENDER so the
   Mac harness (physh, -DE_NO_RENDER) links the loader. See e_world.h for the rules. */
#include "e_world.h"
#include "e_file.h"
#include "e_log.h"
#ifndef E_NO_RENDER
#include "e_tex.h"
#include "e_image.h"
#endif

enum { INFO_SIZE = 136, SKY_SIZE = 312, MATL_REC = 20, CELL_HDR = 20, CELL_REC = 52, BTCH_REC = 24, INST_REC = 40 };

static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static s32 rds(const u8 *p) { return (s32)rd32(p); }
static float rdf(const u8 *p) { float f; u32 u = rd32(p); memcpy(&f, &u, 4); return f; }

static void trk_path(char *out, int n, const char *path)
{
    if (file_exists(path)) { e_strlcpy(out, path, n); return; }
    if (!strchr(path, '/') && !strchr(path, '\\') && !strchr(path, '.')) {
        char rel[E_PATH];
        e_snprintf(rel, (int)sizeof(rel), "tracks/%s.trk", path);
        data_path(out, n, rel);
        return;
    }
    data_path(out, n, path);
}

const u8 *world_chunk(const World *w, const char id[4], int *size)
{
    const u8 *d = (const u8 *)w->fileData;
    int off = 16, n, i;
    if (size) *size = 0;
    if (!d || w->fileSize < 16) return NULL;
    n = (int)rd32(d + 8);
    for (i = 0; i < n && off + 8 <= w->fileSize; i++) {
        int sz = (int)rd32(d + off + 4);
        if (sz < 0 || off + 8 + sz > w->fileSize) return NULL;
        if (!memcmp(d + off, id, 4)) {
            if (size) *size = sz;
            return d + off + 8;
        }
        off += 8 + ((sz + 3) & ~3);
    }
    return NULL;
}

static int world_fail(World *w, const char *path, const char *why)
{
    log_printf(LOG_ERR, "err: world load failed path=%s reason=%s", path, why);
    world_free(w);
    return 0;
}

static void aabb_empty(Aabb *b)
{
    b->lo = v3(1e30f, 1e30f, 1e30f);
    b->hi = v3(-1e30f, -1e30f, -1e30f);
}

static void aabb_add(Aabb *b, const float *p)
{
    if (p[0] < b->lo.x) b->lo.x = p[0];
    if (p[1] < b->lo.y) b->lo.y = p[1];
    if (p[2] < b->lo.z) b->lo.z = p[2];
    if (p[0] > b->hi.x) b->hi.x = p[0];
    if (p[1] > b->hi.y) b->hi.y = p[1];
    if (p[2] > b->hi.z) b->hi.z = p[2];
}

#ifndef E_NO_RENDER
static void world_render_create(World *w)
{
    int i, okVb = 0;
    for (i = 0; i < w->numMats; i++) {
        MeshMat *mm = &w->mats[i];
        int imgFlags = mm->blend == RB_ALPHATEST ? IMG_KEYED : (mm->blend == RB_BLEND ? IMG_ALPHA : 0);
        if (mm->tex[0]) mm->rm.tex0 = tex_get(mm->tex, imgFlags, 0);
        if (mm->tex1[0]) mm->rm.tex1 = tex_get(mm->tex1, 0, 0);
        if (!mm->rm.tex1) mm->rm.stage1 = RS1_NONE;
    }
    w->detailOn = 1;
    for (i = 0; i < w->numPools; i++) {
        w->poolVb[i] = r_vb_create(w->poolFmt[i], w->poolVerts[i], w->poolCount[i]);
        if (w->poolVb[i]) okVb++;
        else log_printf(LOG_ERR, "err: world vb failed pool=%d verts=%d", i, w->poolCount[i]);
    }
    log_printf(LOG_INFO, "world: render vbs=%d/%d tex_kb=%d", okVb, w->numPools, r_tex_kb());
}

static void world_render_free(World *w)
{
    int i;
    for (i = 0; i < 8; i++) if (w->skyTex[i]) tex_release(w->skyTex[i]);
    if (w->glowTex) tex_release(w->glowTex);
    if (w->poolVb) for (i = 0; i < w->numPools; i++) if (w->poolVb[i]) r_vb_free(w->poolVb[i]);
    if (w->mats) for (i = 0; i < w->numMats; i++) {
        if (w->mats[i].rm.tex0) tex_release(w->mats[i].rm.tex0);
        if (w->mats[i].rm.tex1) tex_release(w->mats[i].rm.tex1);
    }
}
#endif

int world_load(World *w, const char *trkPath, int flags)
{
    char full[E_PATH];
    ChunkFile cf;
    const u8 *c, *colt, *colg, *wall, *boxs, *indx;
    int sz, i, nColt, nColg, nWall, nBoxs, nIndx, noRender = (flags & WL_NO_RENDER) ? 1 : 0;
    memset(w, 0, sizeof(*w));
    if (!trkPath || !trkPath[0]) return world_fail(w, "(null)", "no path");
    trk_path(full, (int)sizeof(full), trkPath);
    w->fileData = file_load(full, &w->fileSize);
    if (!w->fileData) return world_fail(w, full, "cannot read");
    if (!chunk_open(&cf, w->fileData, w->fileSize, E_TAG('R', 'T', 'R', 'K'))) return world_fail(w, full, "bad container");
    /* INFO */
    c = world_chunk(w, "INFO", &sz);
    if (!c || sz < INFO_SIZE) return world_fail(w, full, "no INFO");
    memcpy(w->name, c, 32);
    w->name[31] = 0;
    /* SKY */
    c = world_chunk(w, "SKY ", &sz);
    if (c && sz >= SKY_SIZE && sizeof(WSky) == 304) {
        memcpy(&w->sky, c, 304);
        for (i = 0; i < 8; i++) w->sky.tile[i][31] = 0;
        w->fogStart = rdf(c + 304);
        w->fogEnd = rdf(c + 308);
    } else {
        log_printf(LOG_WARN, "warn: world no SKY chunk name=%s", w->name);
    }
    /* TEXL + MATL */
    c = world_chunk(w, "TEXL", &sz);
    w->numTex = c ? sz / 32 : 0;
    if (w->numTex) {
        w->texNames = (char (*)[32])malloc((size_t)w->numTex * 32);
        if (!w->texNames) return world_fail(w, full, "out of memory");
        memcpy(w->texNames, c, (size_t)w->numTex * 32);
        for (i = 0; i < w->numTex; i++) w->texNames[i][31] = 0;
    }
    c = world_chunk(w, "MATL", &sz);
    w->numMats = c ? sz / MATL_REC : 0;
    if (w->numMats <= 0) return world_fail(w, full, "no MATL");
    w->mats = (MeshMat *)calloc((size_t)w->numMats, sizeof(MeshMat));
    if (!w->mats) return world_fail(w, full, "out of memory");
    for (i = 0; i < w->numMats; i++) {
        const u8 *r = c + i * MATL_REC;
        MeshMat *mm = &w->mats[i];
        int t0 = rds(r), t1 = rds(r + 4);
        if (t0 >= 0 && t0 < w->numTex) e_strlcpy(mm->tex, w->texNames[t0], 32);
        if (t1 >= 0 && t1 < w->numTex) e_strlcpy(mm->tex1, w->texNames[t1], 32);
        mm->blend = r[8]; mm->stage1 = r[9]; mm->cullNone = r[10]; mm->flags = r[11];
        mm->rm.blend = mm->blend;
        mm->rm.stage1 = mm->stage1;
        mm->rm.cullNone = mm->cullNone;
        mm->rm.zTest = 1;
        mm->rm.zWrite = (mm->blend == RB_OPAQUE || mm->blend == RB_ALPHATEST) ? 1 : 0;
        mm->rm.fog = 1;
        mm->rm.zBias = r[12] > 2 ? 2 : r[12];
        /* T16: sharper mips on alpha-tested foliage/fences and the road markings (A2/A3 asks; quarter mip steps) */
        mm->rm.lodBias = (s8)((mm->blend == RB_ALPHATEST || !strncmp(mm->tex, "road_", 5)) ? -2 : 0);
    }
    /* CELL: records used in place (WCell layout) */
    c = world_chunk(w, "CELL", &sz);
    if (!c || sz < CELL_HDR || sizeof(WCell) != CELL_REC) return world_fail(w, full, "no CELL");
    w->gx0 = rds(c); w->gz0 = rds(c + 4); w->gw = rds(c + 8); w->gh = rds(c + 12); w->numCells = rds(c + 16);
    if (w->gw <= 0 || w->gh <= 0 || w->gw * w->gh > 65536 || w->numCells < 0 || w->numCells > w->gw * w->gh ||
        CELL_HDR + w->numCells * CELL_REC > sz)
        return world_fail(w, full, "bad CELL");
    w->cells = (WCell *)(c + CELL_HDR);
    w->grid = (WCell **)calloc((size_t)(w->gw * w->gh), sizeof(WCell *));
    if (!w->grid) return world_fail(w, full, "out of memory");
    for (i = 0; i < w->numCells; i++) {
        WCell *ce = &w->cells[i];
        int gx = ce->cx - w->gx0, gz = ce->cz - w->gz0;
        if (gx < 0 || gz < 0 || gx >= w->gw || gz >= w->gh) return world_fail(w, full, "cell outside grid");
        w->grid[gz * w->gw + gx] = ce;
    }
    /* VPOL */
    c = world_chunk(w, "VPOL", &sz);
    if (!c || sz < 4) return world_fail(w, full, "no VPOL");
    w->numPools = (int)rd32(c);
    if (w->numPools < 0 || w->numPools > 65536) return world_fail(w, full, "bad VPOL");
    w->poolVb = (RVBuf *)calloc((size_t)w->numPools + 1, sizeof(RVBuf));
    w->poolFmt = (int *)calloc((size_t)w->numPools + 1, sizeof(int));
    w->poolCount = (int *)calloc((size_t)w->numPools + 1, sizeof(int));
    w->poolVerts = (const u8 **)calloc((size_t)w->numPools + 1, sizeof(const u8 *));
    if (!w->poolVb || !w->poolFmt || !w->poolCount || !w->poolVerts) return world_fail(w, full, "out of memory");
    {
        int off = 4;
        for (i = 0; i < w->numPools; i++) {
            int fmt, cnt, stride;
            if (off + 8 > sz) return world_fail(w, full, "VPOL truncated");
            fmt = (int)rd32(c + off); cnt = (int)rd32(c + off + 4);
            stride = fmt == RFMT_V2 ? (int)sizeof(RVert2) : (int)sizeof(RVert1);
            if ((fmt != RFMT_V1 && fmt != RFMT_V2) || cnt < 0 || cnt > 65535 || off + 8 + cnt * stride > sz)
                return world_fail(w, full, "bad pool");
            w->poolFmt[i] = fmt;
            w->poolCount[i] = cnt;
            w->poolVerts[i] = c + off + 8;
            off += 8 + cnt * stride;
        }
    }
    /* BTCH + INDX */
    indx = world_chunk(w, "INDX", &sz);
    nIndx = indx ? sz / 2 : 0;
    c = world_chunk(w, "BTCH", &sz);
    w->numBatches = c ? sz / BTCH_REC : 0;
    w->batches = (WBatch *)calloc((size_t)w->numBatches + 1, sizeof(WBatch));
    w->batchBox = (Aabb *)calloc((size_t)w->numBatches + 1, sizeof(Aabb));
    if (!w->batches || !w->batchBox) return world_fail(w, full, "out of memory");
    for (i = 0; i < w->numBatches; i++) {
        const u8 *r = c + i * BTCH_REC;
        WBatch *b = &w->batches[i];
        int pool = rds(r + 12), first = (int)rd32(r + 16), n = (int)rd32(r + 20), k, stride;
        const u16 *ix;
        b->cell = rds(r); b->lod = rds(r + 4); b->mat = rds(r + 8);
        if (b->cell < 0 || b->cell >= w->numCells || b->lod < 0 || b->lod > 1 || b->mat < 0 || b->mat >= w->numMats ||
            pool < 0 || pool >= w->numPools || first < 0 || n < 0 || (n % 3) || first + n > nIndx)
            return world_fail(w, full, "bad batch");
        b->fmt = w->poolFmt[pool];
        b->vb = pool;                               /* pool index until the VBs exist (world_render_create) */
        ix = (const u16 *)(indx + (size_t)first * 2);
        b->idx = ix;
        b->nIdx = n;
        stride = b->fmt == RFMT_V2 ? (int)sizeof(RVert2) : (int)sizeof(RVert1);
        aabb_empty(&w->batchBox[i]);
        for (k = 0; k < n; k++) {
            if ((int)ix[k] >= w->poolCount[pool]) return world_fail(w, full, "batch index out of range");
            aabb_add(&w->batchBox[i], (const float *)(w->poolVerts[pool] + (size_t)ix[k] * (size_t)stride));
        }
        w->totalTris[b->lod] += n / 3;
    }
    for (i = 0; i < w->numCells; i++) {
        const WCell *ce = &w->cells[i];
        int l;
        for (l = 0; l < 2; l++)
            if (ce->batch0[l] < 0 || ce->nBatch[l] < 0 || ce->batch0[l] + ce->nBatch[l] > w->numBatches)
                return world_fail(w, full, "bad cell batch range");
    }
    /* MSHL + INST */
    c = world_chunk(w, "MSHL", &sz);
    w->numMeshes = c ? sz / 32 : 0;
    if (w->numMeshes) {
        w->meshes = (Mesh *)calloc((size_t)w->numMeshes, sizeof(Mesh));
        if (!w->meshes) return world_fail(w, full, "out of memory");
        for (i = 0; i < w->numMeshes; i++) {
            char nm[32];
            memcpy(nm, c + i * 32, 32);
            nm[31] = 0;
            if (!mesh_load(&w->meshes[i], nm, noRender)) log_printf(LOG_WARN, "warn: world mesh missing name=%s", nm);
        }
    }
    c = world_chunk(w, "INST", &sz);
    w->numInst = c ? sz / INST_REC : 0;
    if (w->numInst) {
        w->inst = (WInst *)calloc((size_t)w->numInst, sizeof(WInst));
        w->instColor = (u32 *)calloc((size_t)w->numInst, sizeof(u32));
        if (!w->inst || !w->instColor) return world_fail(w, full, "out of memory");
        for (i = 0; i < w->numInst; i++) {
            const u8 *r = c + i * INST_REC;
            WInst *in = &w->inst[i];
            Mat3 rot;
            float sc = rdf(r + 20);
            int k;
            in->mesh = rds(r);
            m3_from_yaw(&rot, rdf(r + 16));
            m4_from_m3_pos(&in->xf, &rot, v3(rdf(r + 4), rdf(r + 8), rdf(r + 12)));
            for (k = 0; k < 3; k++) {
                in->xf.m[k][0] *= sc; in->xf.m[k][1] *= sc; in->xf.m[k][2] *= sc;
            }
            in->cell = (u16)rds(r + 28);
            in->drawDist = rdf(r + 32);
            in->flags = (u16)rd32(r + 36);
            w->instColor[i] = rd32(r + 24) ? rd32(r + 24) : 0xFFFFFFFFu;
            if (in->mesh >= 0 && in->mesh < w->numMeshes && w->meshes[in->mesh].numLods) {
                const Mesh *m = &w->meshes[in->mesh];
                Vec3 ctr = v3_scale(v3_add(m->box.lo, m->box.hi), 0.5f);
                in->c = m4_mul_point(&in->xf, ctr);
                in->r = v3_len(v3_sub(m->box.hi, ctr)) * sc;
            } else {
                in->mesh = -1;
                in->c = v3(rdf(r + 4), rdf(r + 8), rdf(r + 12));
                in->r = 0.0f;
            }
        }
    }
    /* collision */
    colt = world_chunk(w, "COLT", &nColt);
    colg = world_chunk(w, "COLG", &nColg);
    wall = world_chunk(w, "WALL", &nWall);
    boxs = world_chunk(w, "BOXS", &nBoxs);
    if (colt && nColt > 0) {
        w->coll = coll_create(colt, nColt, colg, nColg, wall, nWall, boxs, nBoxs);
        if (!w->coll) return world_fail(w, full, "coll_create failed");
    } else {
        log_printf(LOG_WARN, "warn: world no COLT chunk name=%s", w->name);
    }
#ifndef E_NO_RENDER
    if (!noRender) world_render_create(w);
#endif
    if (noRender || !w->poolVb) for (i = 0; i < w->numBatches; i++) w->batches[i].vb = 0;
#ifndef E_NO_RENDER
    else for (i = 0; i < w->numBatches; i++) w->batches[i].vb = w->poolVb[w->batches[i].vb];
#else
    else for (i = 0; i < w->numBatches; i++) w->batches[i].vb = 0;
#endif
    log_printf(LOG_INFO, "world: load name=%s path=%s bytes=%d cells=%d grid=%dx%d@%d,%d batches=%d pools=%d mats=%d "
               "tex=%d meshes=%d inst=%d tris_lod0=%d tris_lod1=%d colltris=%d walls=%d render=%d",
               w->name, full, w->fileSize, w->numCells, w->gw, w->gh, w->gx0, w->gz0, w->numBatches, w->numPools,
               w->numMats, w->numTex, w->numMeshes, w->numInst, w->totalTris[0], w->totalTris[1],
               w->coll ? coll_num_tris(w->coll) : 0, w->coll ? coll_num_walls(w->coll) : 0, noRender ? 0 : 1);
    return 1;
}

void world_free(World *w)
{
    int i;
#ifndef E_NO_RENDER
    world_render_free(w);
#endif
    if (w->meshes) {
        for (i = 0; i < w->numMeshes; i++) mesh_free(&w->meshes[i]);
        free(w->meshes);
    }
    if (w->coll) coll_free(w->coll);
    if (w->inst) free(w->inst);
    if (w->instColor) free(w->instColor);
    if (w->batches) free(w->batches);
    if (w->batchBox) free(w->batchBox);
    if (w->poolVb) free(w->poolVb);
    if (w->poolFmt) free(w->poolFmt);
    if (w->poolCount) free(w->poolCount);
    if (w->poolVerts) free((void *)w->poolVerts);
    if (w->mats) free(w->mats);
    if (w->texNames) free(w->texNames);
    if (w->grid) free(w->grid);
    if (w->fileData) free(w->fileData);
    memset(w, 0, sizeof(*w));
}

/* ================================================================ culling (portable) */
void world_view_lookat(WView *v, Vec3 eye, Vec3 at, float fovY, float aspect, float viewDist, float lodDist)
{
    Mat4 vp;
    memset(v, 0, sizeof(*v));
    v->eye = eye;
    m4_look_lh(&v->view, eye, at, v3(0.0f, 1.0f, 0.0f));
    m4_persp_lh(&v->proj, fovY, aspect > 0.0f ? aspect : 1.333f, 1.0f, viewDist + 50.0f);
    m4_mul(&vp, &v->view, &v->proj);
    frustum_from_viewproj(&v->fr, &vp);
    v->viewDist = viewDist;
    v->lodDist = lodDist;
    v->objScale = 1.0f;
    v->lodScale = 1.0f;
    v->treeDensity = 1.0f;
}

static float aabb_dist(const Aabb *b, Vec3 p)
{
    float dx = b->lo.x - p.x, dy = b->lo.y - p.y, dz = b->lo.z - p.z, t;
    if (dx < 0.0f) { t = p.x - b->hi.x; dx = t > 0.0f ? t : 0.0f; }
    if (dy < 0.0f) { t = p.y - b->hi.y; dy = t > 0.0f ? t : 0.0f; }
    if (dz < 0.0f) { t = p.z - b->hi.z; dz = t > 0.0f ? t : 0.0f; }
    return (float)sqrt(dx * dx + dy * dy + dz * dz);
}

static int floor_div(float x, int d)
{
    return (int)floor(x / (float)d);
}

int world_visible(const World *w, const WView *v, int pass, int *out, int maxOut, int *instIdx, int *nInstOut,
                  int maxInst, WDrawStats *st)
{
    static int s_tmp[WORLD_MAX_VISIBLE];
    int cnt[257];
    int x0, x1, z0, z1, cx, cz, n = 0, ni = 0, i, cells = 0, tris = 0, sortable;
    float vd = v->viewDist > 0.0f ? v->viewDist : 300.0f;
    float objScale = v->objScale > 0.0f ? v->objScale : 1.0f;
    float dens = v->treeDensity > 0.0f ? v->treeDensity : 1.0f;
    if (st) memset(st, 0, sizeof(*st));
    if (nInstOut) *nInstOut = 0;
    if (!w->grid || maxOut <= 0) return 0;
    if (maxOut > WORLD_MAX_VISIBLE) maxOut = WORLD_MAX_VISIBLE;
    x0 = floor_div(v->eye.x - vd, WORLD_CELL) - w->gx0; x1 = floor_div(v->eye.x + vd, WORLD_CELL) - w->gx0;
    z0 = floor_div(v->eye.z - vd, WORLD_CELL) - w->gz0; z1 = floor_div(v->eye.z + vd, WORLD_CELL) - w->gz0;
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 >= w->gw) x1 = w->gw - 1;
    if (z1 >= w->gh) z1 = w->gh - 1;
    for (cz = z0; cz <= z1; cz++) {
        for (cx = x0; cx <= x1; cx++) {
            const WCell *ce = w->grid[cz * w->gw + cx];
            float d;
            int lod, b, cellUsed = 0;
            if (!ce) continue;
            d = aabb_dist(&ce->box, v->eye);
            if (d > vd || !frustum_aabb(&v->fr, &ce->box)) continue;
            lod = d < v->lodDist ? 0 : 1;
            for (b = ce->batch0[lod]; b < ce->batch0[lod] + ce->nBatch[lod]; b++) {
                const WBatch *bt = &w->batches[b];
                int alpha = w->mats[bt->mat].blend != RB_OPAQUE;
                if (alpha != (pass ? 1 : 0)) continue;
                if (aabb_dist(&w->batchBox[b], v->eye) > vd || !frustum_aabb(&v->fr, &w->batchBox[b])) continue;
                if (n >= maxOut) break;
                s_tmp[n++] = b;
                tris += bt->nIdx / 3;
                cellUsed = 1;
            }
            if (pass == 0 && instIdx) {
                for (i = ce->inst0; i < ce->inst0 + ce->nInst && i < w->numInst; i++) {
                    const WInst *in = &w->inst[i];
                    float di;
                    if (in->mesh < 0) continue;
                    if ((in->flags & WI_THIN) && (float)(in->flags & 0xFF) >= dens * 256.0f) continue;
                    di = v3_len(v3_sub(in->c, v->eye)) - in->r;
                    if (di > in->drawDist * objScale || di > vd || !frustum_sphere(&v->fr, in->c, in->r)) continue;
                    if (ni >= maxInst) break;
                    instIdx[ni++] = i;
                    cellUsed = 1;
                }
            }
            cells += cellUsed;
        }
    }
    /* counting sort by material (stable) */
    sortable = w->numMats <= 256;
    if (sortable) {
        memset(cnt, 0, sizeof(cnt));
        for (i = 0; i < n; i++) cnt[w->batches[s_tmp[i]].mat + 1]++;
        for (i = 1; i <= w->numMats; i++) cnt[i] += cnt[i - 1];
        for (i = 0; i < n; i++) out[cnt[w->batches[s_tmp[i]].mat]++] = s_tmp[i];
    } else {
        memcpy(out, s_tmp, (size_t)n * sizeof(int));
    }
    if (nInstOut) *nInstOut = ni;
    if (st) {
        st->cells = cells;
        st->batches = n;
        st->inst = ni;
        st->tris = tris;
        if (instIdx) {
            for (i = 0; i < ni; i++) {
                const WInst *in = &w->inst[instIdx[i]];
                const Mesh *m = &w->meshes[in->mesh];
                st->tris += mesh_lod_tris(m, mesh_lod(m, v3_len(v3_sub(in->c, v->eye)), v->lodScale));
            }
        }
    }
    return n;
}

/* ================================================================ drawing */
#ifndef E_NO_RENDER
static int s_vis[WORLD_MAX_VISIBLE];
static int s_vis2[WORLD_MAX_VISIBLE];
static int s_visInst[WORLD_MAX_VISIBLE];
static int s_visInstSorted[WORLD_MAX_VISIBLE];
static u8  s_visLod[WORLD_MAX_VISIBLE];
static int s_nVisInst, s_instValid;
static const World *s_instWorld;
static Mat4 s_instView;
static Vec3 s_instEye;

/* T15: small instance meshes (trees, posts) are pre-transformed into one merged dynamic draw per (mesh, LOD, part) */
enum { MERGE_MAX_V = 4096, MERGE_MAX_I = 12288, MERGE_SMALL = 64 };
static RVert1 s_mv[MERGE_MAX_V];
static u16 s_mi[MERGE_MAX_I];
static int s_mnv, s_mni;

static void merge_flush(void)
{
    if (s_mni > 0) r_draw_mem(RFMT_V1, s_mv, s_mnv, s_mi, s_mni);
    s_mnv = s_mni = 0;
}

static void draw_instances(const World *w, const WView *v, int pass)
{
    static int cnt[1026];
    int n = s_nVisInst, i, g, keys, sorted = 0;
    int passMask = pass ? MESH_PASS_ALPHA : MESH_PASS_OPAQUE;
    if (n <= 0) return;
    for (i = 0; i < n; i++) {
        const WInst *in = &w->inst[s_visInst[i]];
        s_visLod[i] = (u8)mesh_lod(&w->meshes[in->mesh], v3_len(v3_sub(in->c, v->eye)), v->lodScale);
    }
    keys = w->numMeshes * 4;
    if (keys > 0 && keys <= 1024) {                 /* counting sort by (mesh, lod) */
        static int tmpLod[WORLD_MAX_VISIBLE];
        memset(cnt, 0, (size_t)(keys + 1) * sizeof(int));
        for (i = 0; i < n; i++) cnt[w->inst[s_visInst[i]].mesh * 4 + s_visLod[i] + 1]++;
        for (i = 1; i <= keys; i++) cnt[i] += cnt[i - 1];
        for (i = 0; i < n; i++) {
            int k = cnt[w->inst[s_visInst[i]].mesh * 4 + s_visLod[i]]++;
            s_visInstSorted[k] = s_visInst[i];
            tmpLod[k] = s_visLod[i];
        }
        for (i = 0; i < n; i++) s_visLod[i] = (u8)tmpLod[i];
        sorted = 1;
    }
    if (!sorted) memcpy(s_visInstSorted, s_visInst, (size_t)n * sizeof(int));
    for (g = 0; g < n;) {
        const WInst *first = &w->inst[s_visInstSorted[g]];
        const Mesh *m = &w->meshes[first->mesh];
        int lod = s_visLod[g], e = g + 1, k;
        while (e < n && w->inst[s_visInstSorted[e]].mesh == first->mesh && s_visLod[e] == lod) e++;
        if (sorted && m->lod[lod].numVerts <= MERGE_SMALL) {
            const MeshLod *l = &m->lod[lod];
            for (k = l->firstPart; k < l->firstPart + l->numParts; k++) {
                const MeshPart *pt = &m->parts[k];
                RMaterial rm = m->mats[pt->mat].rm;
                int pp = rm.blend == RB_OPAQUE ? MESH_PASS_OPAQUE : MESH_PASS_ALPHA;
                if (!(pp & passMask)) continue;
                r_set_world(NULL);
                r_set_material(&rm);
                for (i = g; i < e; i++) {
                    const WInst *in = &w->inst[s_visInstSorted[i]];
                    int ni, nv = mesh_append_part(m, lod, k, &in->xf, w->instColor[s_visInstSorted[i]], s_mv + s_mnv,
                                                  MERGE_MAX_V - s_mnv, s_mi + s_mni, MERGE_MAX_I - s_mni, s_mnv, &ni);
                    if (!nv && s_mni) {
                        merge_flush();
                        nv = mesh_append_part(m, lod, k, &in->xf, w->instColor[s_visInstSorted[i]], s_mv, MERGE_MAX_V,
                                              s_mi, MERGE_MAX_I, 0, &ni);
                    }
                    s_mnv += nv;
                    s_mni += nv ? ni : 0;
                }
                merge_flush();
            }
        } else if (m->numLods && m->lodVb[lod]) {
            /* T23: parts outer, instances inner: one texture change per (mesh, LOD, part) instead of one per part of
               every instance (village/farm buildings have 3-5 parts; T16 measured 176 texture changes on High) */
            const MeshLod *l = &m->lod[lod];
            for (k = l->firstPart; k < l->firstPart + l->numParts; k++) {
                const MeshPart *pt = &m->parts[k];
                RMaterial rm = m->mats[pt->mat].rm;
                int pp = rm.blend == RB_OPAQUE ? MESH_PASS_OPAQUE : MESH_PASS_ALPHA;
                if (!(pp & passMask)) continue;
                r_set_material(&rm);
                for (i = g; i < e; i++) {
                    const WInst *in = &w->inst[s_visInstSorted[i]];
                    u32 tint = w->instColor[s_visInstSorted[i]];
                    if (tint == 0 || tint == 0xFFFFFFFFu) {
                        r_set_world(&in->xf);
                        r_draw_vb(m->lodVb[lod], m->idx + pt->firstIdx, pt->numIdx);
                    } else {
                        int ni, nv;
                        r_set_world(NULL);
                        nv = mesh_append_part(m, lod, k, &in->xf, tint, s_mv, MERGE_MAX_V, s_mi, MERGE_MAX_I, 0, &ni);
                        if (nv) r_draw_mem(RFMT_V1, s_mv, nv, s_mi, ni);
                    }
                }
            }
            r_set_world(NULL);
        }
        g = e;
    }
    r_set_world(NULL);
}

static void draw_list(const World *w, const WView *v, int pass, WDrawStats *st)
{
    int n, i;
    if (pass == 0) {
        n = world_visible(w, v, 0, s_vis, WORLD_MAX_VISIBLE, s_visInst, &s_nVisInst, WORLD_MAX_VISIBLE, st);
        s_instWorld = w;
        s_instView = v->view;
        s_instEye = v->eye;
        s_instValid = 1;
    } else {
        n = world_visible(w, v, 1, s_vis, WORLD_MAX_VISIBLE, NULL, NULL, 0, st);
        if (!s_instValid || s_instWorld != w || memcmp(&s_instView, &v->view, sizeof(Mat4)) ||
            memcmp(&s_instEye, &v->eye, sizeof(Vec3))) {
            world_visible(w, v, 0, s_vis2, WORLD_MAX_VISIBLE, s_visInst, &s_nVisInst, WORLD_MAX_VISIBLE, NULL);
            s_instWorld = w;
            s_instView = v->view;
            s_instEye = v->eye;
            s_instValid = 1;
        }
    }
    r_set_proj(&v->proj);
    r_set_view(&v->view);
    r_set_world(NULL);
    for (i = 0; i < n; i++) {
        const WBatch *b = &w->batches[s_vis[i]];
        if (!b->vb) continue;
        r_set_material(&w->mats[b->mat].rm);
        r_draw_vb(b->vb, b->idx, b->nIdx);
    }
    if (w->numInst) draw_instances(w, v, pass);
    r_set_world(NULL);
}

void world_draw_opaque(const World *w, const WView *v, WDrawStats *st) { draw_list(w, v, 0, st); }
void world_draw_alpha(const World *w, const WView *v, WDrawStats *st) { draw_list(w, v, 1, st); }

void world_set_detail(World *w, int on)
{
    int i, n = 0;
    for (i = 0; i < w->numMats; i++) {
        MeshMat *mm = &w->mats[i];
        if (mm->stage1 != RS1_DETAIL || !mm->rm.tex1) continue;
        mm->rm.stage1 = (u8)(on ? RS1_DETAIL : RS1_NONE);
        n++;
    }
    w->detailOn = on;
    log_printf(LOG_INFO, "world: detail on=%d materials=%d", on, n);
}

static u32 lerp_rgb(u32 a, u32 b, float t)
{
    int i;
    u32 o = 0xFF000000u;
    for (i = 0; i < 24; i += 8) {
        float ca = (float)((a >> i) & 0xFF), cb = (float)((b >> i) & 0xFF);
        int c = e_ftoi(ca + (cb - ca) * t);
        o |= (u32)(c < 0 ? 0 : (c > 255 ? 255 : c)) << i;
    }
    return o;
}

/* ---------------------------------------------------------------- sky (T15, DESIGN 3.7) */
enum { SKY_SEG = 32, SKY_ROWS = 7 };

static float sky_elev(float e) { return (e > 3.2f || e < -3.2f) ? E_DEG2RAD(e) : e; }

static void sky_colours(const World *w, u32 *zen, u32 *hor, u32 *fog)
{
    *zen = w->sky.zenith; *hor = w->sky.horizon; *fog = w->sky.fog;
    if (!*zen && !*hor && !*fog) { *zen = 0x4F7FBF; *hor = 0xC8D4DC; *fog = 0xB4C3CD; }
    if (!*fog) *fog = *hor;
}

static void sky_params(World *w)
{
    float lo = sky_elev(w->sky.elevLo), hi = sky_elev(w->sky.elevHi), vh = w->sky.vHorizon;
    if (!(lo < -0.001f)) lo = E_DEG2RAD(-6.0f);
    if (!(hi > 0.01f)) hi = E_DEG2RAD(28.0f);
    if (hi > E_DEG2RAD(80.0f)) hi = E_DEG2RAD(80.0f);
    if (lo < E_DEG2RAD(-60.0f)) lo = E_DEG2RAD(-60.0f);
    if (!(vh > 0.0f && vh < 1.0f)) vh = hi / (hi - lo);
    w->skyElevLo = lo; w->skyElevHi = hi; w->skyVHor = vh;
}

static float sky_v(const World *w, float e)
{
    if (e <= 0.0f) return w->skyVHor + (1.0f - w->skyVHor) * (e / w->skyElevLo);
    return w->skyVHor * (1.0f - e / w->skyElevHi);
}

static Vec3 sky_dir(float az, float e)
{
    float ce = (float)cos(e);
    return v3((float)sin(az) * ce * 100.0f, (float)sin(e) * 100.0f, (float)cos(az) * ce * 100.0f);
}

/* load one panorama image: name as a path (+ .bmp), else data\sky\name.bmp, else data\tex\name.bmp */
static int sky_img(EImage *im, const char *name)
{
    char rel[E_PATH], full[E_PATH];
    e_snprintf(rel, (int)sizeof(rel), "%s.bmp", name);
    if (file_exists(rel)) return img_load(im, rel, 0);
    e_snprintf(rel, (int)sizeof(rel), "sky/%s.bmp", name);
    data_path(full, (int)sizeof(full), rel);
    if (file_exists(full)) return img_load(im, full, 0);
    e_snprintf(rel, (int)sizeof(rel), "tex/%s.bmp", name);
    data_path(full, (int)sizeof(full), rel);
    if (file_exists(full)) return img_load(im, full, 0);
    return 0;
}

static int sky_img_exists(const char *name)
{
    char rel[E_PATH], full[E_PATH];
    e_snprintf(rel, (int)sizeof(rel), "%s.bmp", name);
    if (file_exists(rel)) return 1;
    e_snprintf(rel, (int)sizeof(rel), "sky/%s.bmp", name);
    data_path(full, (int)sizeof(full), rel);
    if (file_exists(full)) return 1;
    e_snprintf(rel, (int)sizeof(rel), "tex/%s.bmp", name);
    data_path(full, (int)sizeof(full), rel);
    return file_exists(full);
}

/* two w x h tiles side by side, box-filtered to one w x h/2 tile (Low without _low_ files) */
static int sky_halve_pair(EImage *out, const EImage *a, const EImage *b)
{
    int x, y, w = a->w, h = a->h / 2;
    if (b->w != a->w || b->h != a->h || h < 1 || !img_alloc(out, w, h, 0)) return 0;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        const EImage *s = x < w / 2 ? a : b;
        int sx = (x % (w / 2)) * 2, sy = y * 2, c, k, sum;
        u32 o = 0xFF000000u;
        for (c = 0; c < 24; c += 8) {
            sum = 0;
            for (k = 0; k < 4; k++) sum += (int)((s->pix[0][(sy + (k >> 1)) * s->w + sx + (k & 1)] >> c) & 0xFF);
            o |= (u32)((sum + 2) / 4) << c;
        }
        out->pix[0][y * w + x] = o;
    }
    return 1;
}

static u32 avg_cols(const EImage *im, int x0, int x1, int y0, int y1)
{
    int x, y, c, n = 0;
    u32 sum[3] = { 0, 0, 0 };
    if (x1 <= x0) x1 = x0 + 1;
    for (y = y0; y < y1; y++) for (x = x0; x < x1; x++) {
        u32 p = im->pix[0][y * im->w + (x < im->w ? x : im->w - 1)];
        for (c = 0; c < 3; c++) sum[c] += (p >> (c * 8)) & 0xFF;
        n++;
    }
    if (!n) return 0xFF000000u;
    return 0xFF000000u | ((sum[2] / n) << 16) | ((sum[1] / n) << 8) | (sum[0] / n);
}

static void sky_base_name(char *out, int n, const char *tile)
{
    int len;
    e_strlcpy(out, tile, n);
    len = (int)strlen(out);
    while (len > 0 && out[len - 1] >= '0' && out[len - 1] <= '9') len--;
    if (len > 0 && out[len - 1] == '_') len--;
    out[len] = 0;
}

/* dev override (-sky SET): read the light set <SET>.txt (A5 format, data\sky\) into the SKY fields */
static void sky_read_set(World *w, const char *prefix)
{
    char rel[E_PATH], full[E_PATH], *txt, *line, *next;
    int size = 0, n = 0;
    w->skyHiPrefix[0] = 0;
    w->skyLoPrefix[0] = 0;
    e_snprintf(rel, (int)sizeof(rel), "%s.txt", prefix);
    if (file_exists(rel)) e_strlcpy(full, rel, (int)sizeof(full));
    else { e_snprintf(rel, (int)sizeof(rel), "sky/%s.txt", prefix); data_path(full, (int)sizeof(full), rel); }
    if (!file_exists(full)) data_path(full, (int)sizeof(full), "sky/light.txt");   /* jet T01: A2's set file */
    txt = (char *)file_load(full, &size);
    if (!txt) return;
    for (line = txt; line && *line; line = next) {
        char key[32], word[32];
        float a, b, c;
        int k;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (line[0] == '#') continue;
        key[0] = 0;
        word[0] = 0;
        if (sscanf(line, "%31s %31s", key, word) == 2) {   /* jet T01: word-valued keys */
            if (!strcmp(key, "tile_prefix")) { e_strlcpy(w->skyHiPrefix, word, (int)sizeof(w->skyHiPrefix)); n++; continue; }
            if (!strcmp(key, "tile_prefix_low")) { e_strlcpy(w->skyLoPrefix, word, (int)sizeof(w->skyLoPrefix)); n++; continue; }
        }
        k = sscanf(line, "%31s %f %f %f", key, &a, &b, &c);
        if (k < 2) continue;
#define SKYRGB(f) (k >= 4 ? (((u32)e_ftoi(a) & 0xFF) << 16) | (((u32)e_ftoi(b) & 0xFF) << 8) | ((u32)e_ftoi(c) & 0xFF) : f)
        if (!strcmp(key, "vHorizon")) w->sky.vHorizon = a;
        else if (!strcmp(key, "elevLo")) w->sky.elevLo = a;
        else if (!strcmp(key, "elevHi")) w->sky.elevHi = a;
        else if (!strcmp(key, "zenith")) w->sky.zenith = SKYRGB(w->sky.zenith);
        else if (!strcmp(key, "horizon")) w->sky.horizon = SKYRGB(w->sky.horizon);
        else if (!strcmp(key, "fog")) w->sky.fog = SKYRGB(w->sky.fog);
        else if (!strcmp(key, "sun")) w->sky.sun = SKYRGB(w->sky.sun);
        else if (!strcmp(key, "ambient")) w->sky.ambient = SKYRGB(w->sky.ambient);
        else if (!strcmp(key, "sunDir") && k >= 4) w->sky.sunDir = v3(a, b, c);
        else if (!strcmp(key, "fogStart")) w->fogStart = a;            /* jet T01 */
        else if (!strcmp(key, "fogEnd")) w->fogEnd = a;
        else continue;
#undef SKYRGB
        n++;
    }
    free(txt);
    log_printf(LOG_INFO, "sky: light set override path=%s keys=%d fog=%06x elev=%.1f..%.1f vhor=%.3f", full, n,
               w->sky.fog, (double)w->sky.elevLo, (double)w->sky.elevHi, (double)w->sky.vHorizon);
}

int world_sky_load(World *w, int maxTiles, const char *prefix)
{
    EImage img[8], low[4];
    char names[8][E_PATH], base[E_PATH], nm[E_PATH];
    int nHigh, i, nUse = 0, useLow = 0, ok = 1, s, bestX = 0, totalW = 0;
    float best = -1.0f, sunAz, brightAz;
    const EImage *use[8];
    if (prefix && prefix[0]) sky_read_set(w, prefix);
    sky_params(w);
    for (i = 0; i < 8; i++) { if (w->skyTex[i]) tex_release(w->skyTex[i]); w->skyTex[i] = 0; }
    w->skyTiles = 0;
    memset(img, 0, sizeof(img));
    memset(low, 0, sizeof(low));
    if (prefix && prefix[0]) {
        const char *hp = prefix;
        e_snprintf(nm, (int)sizeof(nm), "%s_0", prefix);
        if (!sky_img_exists(nm) && w->skyHiPrefix[0]) hp = w->skyHiPrefix;   /* jet T01: tile_prefix of the set */
        for (nHigh = 0; nHigh < 8; nHigh++) {
            e_snprintf(names[nHigh], E_PATH, "%s_%d", hp, nHigh);
            if (!sky_img_exists(names[nHigh])) break;
        }
        e_strlcpy(base, hp, (int)sizeof(base));
    } else {
        nHigh = w->sky.nTiles < 0 ? 0 : (w->sky.nTiles > 8 ? 8 : w->sky.nTiles);
        for (i = 0; i < nHigh; i++) e_strlcpy(names[i], w->sky.tile[i], E_PATH);
        sky_base_name(base, (int)sizeof(base), nHigh ? names[0] : "");
    }
    if (nHigh <= 0) {
        log_printf(LOG_INFO, "sky: no panorama tiles (gradient dome) prefix=%s", prefix && prefix[0] ? prefix : "-");
        return 0;
    }
    if (maxTiles <= 4 && nHigh == 8) {              /* Low: 4 tiles of half height */
        char lowFmt[E_PATH];
        useLow = 0;
        if (prefix && prefix[0] && w->skyLoPrefix[0]) {     /* jet T01: tile_prefix_low of the set first */
            e_snprintf(lowFmt, (int)sizeof(lowFmt), "%s_", w->skyLoPrefix);
            useLow = 1;
            for (i = 0; i < 4 && useLow; i++) {
                e_snprintf(nm, (int)sizeof(nm), "%s%d", lowFmt, i);
                if (!sky_img_exists(nm)) useLow = 0;
            }
        }
        if (!useLow) {                              /* racer naming: <base>_low_<i> */
            e_snprintf(lowFmt, (int)sizeof(lowFmt), "%s_low_", base);
            useLow = 1;
            for (i = 0; i < 4 && useLow; i++) {
                e_snprintf(nm, (int)sizeof(nm), "%s%d", lowFmt, i);
                if (!sky_img_exists(nm)) useLow = 0;
            }
        }
        if (useLow) {
            for (i = 0; i < 4 && ok; i++) {
                e_snprintf(nm, (int)sizeof(nm), "%s%d", lowFmt, i);
                ok = sky_img(&low[i], nm);
            }
        } else {
            for (i = 0; i < 8 && ok; i++) ok = sky_img(&img[i], names[i]);
            for (i = 0; i < 4 && ok; i++) ok = sky_halve_pair(&low[i], &img[2 * i], &img[2 * i + 1]);
        }
        for (i = 0; i < 4; i++) use[i] = &low[i];
        nUse = 4;
    } else {
        for (i = 0; i < nHigh && ok; i++) ok = sky_img(&img[i], names[i]);
        for (i = 0; i < nHigh; i++) use[i] = &img[i];
        nUse = nHigh;
    }
    if (ok) {
        for (i = 0; i < nUse; i++) if (use[i]->w != use[0]->w || use[i]->h != use[0]->h || use[i]->w < 2 || use[i]->h < 2) ok = 0;
    }
    if (ok) {
        int tw = use[0]->w, th = use[0]->h, hy = e_ftoi(w->skyVHor * (float)th), x;
        totalW = tw * nUse;
        for (s = 0; s < SKY_SEG; s++) {         /* top/bottom row colours per sky segment */
            int x0 = s * totalW / SKY_SEG, x1 = (s + 1) * totalW / SKY_SEG, t = x0 / tw;
            w->skyTop[s] = avg_cols(use[t], x0 - t * tw, x1 - t * tw, 0, th > 4 ? 2 : 1);
            w->skyBot[s] = avg_cols(use[t], x0 - t * tw, x1 - t * tw, th > 4 ? th - 2 : th - 1, th);
        }
        if (hy < 1) hy = 1;
        for (x = 0; x < totalW; x++) {          /* sun check: brightest column above the horizon row */
            const EImage *im = use[x / tw];
            int y, xx = x % tw;
            float sum = 0.0f;
            for (y = 0; y < hy && y < th; y++) {
                u32 p = im->pix[0][y * tw + xx];
                sum += (float)(((p >> 16) & 0xFF) * 3 + ((p >> 8) & 0xFF) * 6 + (p & 0xFF));
            }
            if (sum > best) { best = sum; bestX = x; }
        }
        for (i = 0; i < nUse; i++) {
            char tn[40];
            e_snprintf(tn, (int)sizeof(tn), "sky%d_%.20s", i, useLow ? "low" : (nUse == 4 && nHigh == 8 ? "half" : "hi"));
            w->skyTex[i] = tex_add(tn, use[i], RT_CLAMP | RT_NOMIP);
            if (!w->skyTex[i]) ok = 0;
        }
    }
    for (i = 0; i < 8; i++) img_free(&img[i]);
    for (i = 0; i < 4; i++) img_free(&low[i]);
    if (!ok) {
        for (i = 0; i < 8; i++) { if (w->skyTex[i]) tex_release(w->skyTex[i]); w->skyTex[i] = 0; }
        log_printf(LOG_WARN, "warn: sky panorama load failed base=%s tiles=%d (gradient dome)", base, nHigh);
        return 0;
    }
    w->skyTiles = nUse;
    sunAz = E_RAD2DEG((float)atan2(w->sky.sunDir.x, w->sky.sunDir.z));
    if (sunAz < 0.0f) sunAz += 360.0f;
    brightAz = ((float)bestX + 0.5f) * 360.0f / (float)totalW;
    log_printf(LOG_INFO, "sky: panorama base=%s tiles=%d source=%s elev=%.1f..%.1f vhor=%.3f tex_kb=%d",
               base, nUse, useLow ? "low_files" : (nUse == 4 && nHigh == 8 ? "halved" : "high"),
               (double)E_RAD2DEG(w->skyElevLo), (double)E_RAD2DEG(w->skyElevHi), (double)w->skyVHor, r_tex_kb());
    log_printf(LOG_INFO, "sky: sun_check sun_az=%.1f sun_elev=%.1f pano_bright_az=%.1f diff=%.1f",
               (double)sunAz, (double)E_RAD2DEG((float)asin(E_CLAMP(v3_norm(w->sky.sunDir).y, -1.0f, 1.0f))),
               (double)brightAz, (double)(float)fabs(wrap_pi(E_DEG2RAD(brightAz - sunAz))) * (180.0 / 3.14159265));
    return nUse;
}

void world_set_sun_glow(World *w, int on)
{
    w->sunGlow = on;
    if (on && !w->glowTex) {                        /* jet T01: A2's tex\sun_glow.bmp (premultiplied, RB_ADD) */
        char path[E_PATH];
        data_path(path, (int)sizeof(path), "tex/sun_glow.bmp");
        if (file_exists(path)) w->glowTex = tex_get("sun_glow", 0, RT_CLAMP | RT_NOMIP);
    }
    if (on && !w->glowTex) {                        /* T23: A6's glow (premultiplied, for RB_ADD) when present */
        char path[E_PATH];
        data_path(path, (int)sizeof(path), "tex/glow.bmp");
        if (file_exists(path)) w->glowTex = tex_get("glow", 0, RT_CLAMP | RT_NOMIP);
    }
    if (on && !w->glowTex) {
        EImage im;
        int x, y, n = 64;
        if (img_alloc(&im, n, n, 0)) {
            for (y = 0; y < n; y++) for (x = 0; x < n; x++) {
                float dx = ((float)x + 0.5f) / (float)n * 2.0f - 1.0f, dy = ((float)y + 0.5f) / (float)n * 2.0f - 1.0f;
                float d = (float)sqrt(dx * dx + dy * dy), a = 1.0f - d, c;
                int g;
                if (a < 0.0f) a = 0.0f;
                c = a * a * 0.75f + (d < 0.12f ? 0.25f * (1.0f - d / 0.12f) : 0.0f);   /* soft halo + small core */
                if (x == 0 || y == 0 || x == n - 1 || y == n - 1) c = 0.0f;
                g = e_ftoi(c * 255.0f);
                g = g < 0 ? 0 : (g > 255 ? 255 : g);
                im.pix[0][y * n + x] = 0xFF000000u | ((u32)g << 16) | ((u32)g << 8) | (u32)g;
            }
            w->glowTex = tex_add("sun_glow_proc", &im, RT_CLAMP | RT_NOMIP);
            img_free(&im);
        }
    }
}

static int s_skyLate;                               /* T16: world_set_sky_late */
void world_set_sky_late(int on) { s_skyLate = on; }

static void sky_mat(RMaterial *rm, RTex t)
{
    memset(rm, 0, sizeof(*rm));
    rm->zTest = (u8)(s_skyLate ? 1 : 0);            /* T16: drawn after the opaque pass, only where Z is still clear */
    rm->tex0 = t;
    rm->blend = RB_OPAQUE;
    rm->cullNone = 1;
    rm->clampUV = 1;
}

void world_draw_sky(const World *w, const WView *v)
{
    static RVert1 vt[(SKY_SEG + 1) * 8 + 4];
    static u16 ix[SKY_SEG * 8 * 6];
    float rowE[SKY_ROWS];
    u32 zen, hor, fog, top[SKY_SEG + 1], bot[SKY_SEG + 1];
    RMaterial rm;
    Mat4 rot;
    u8 segVis[SKY_SEG];
    int r, s, t, n, nv, base;
    float lo = w->skyTiles ? w->skyElevLo : E_DEG2RAD(-6.0f), hi = w->skyTiles ? w->skyElevHi : E_DEG2RAD(28.0f);
    float eLow;
    sky_colours(w, &zen, &hor, &fog);
    rowE[0] = lo; rowE[1] = lo * 0.5f; rowE[2] = 0.0f;
    rowE[3] = hi * 0.25f; rowE[4] = hi * 0.5f; rowE[5] = hi * 0.75f; rowE[6] = hi;
    rot = v->view;
    rot.m[3][0] = rot.m[3][1] = rot.m[3][2] = 0.0f;
    {   /* T15: draw only the sky segments near the view azimuth (clipping off-screen sky cost ~1 ms on the VM) */
        float fx = v->view.m[0][2], fy = v->view.m[1][2], fz = v->view.m[2][2], faz = (float)atan2(fx, fz);
        float halfH = v->proj.m[0][0] > 0.01f ? (float)atan(1.0f / v->proj.m[0][0]) : E_PI;
        float lim = halfH + E_DEG2RAD(5.625f) + E_DEG2RAD(15.0f);
        for (s = 0; s < SKY_SEG; s++) {
            float c = 2.0f * E_PI * ((float)s + 0.5f) / (float)SKY_SEG;
            segVis[s] = (fy > 0.35f || fy < -0.35f || (float)fabs(wrap_pi(c - faz)) <= lim) ? 1 : 0;
        }
    }
    r_set_proj(&v->proj);
    r_set_view(&rot);
    if (s_skyLate) {                                /* T16: dome radius 100 m -> just inside the far plane (viewDist + 50) */
        Mat4 sc;
        float k = ((v->viewDist > 70.0f ? v->viewDist : 70.0f) + 25.0f) / 100.0f;
        memset(&sc, 0, sizeof(sc));
        sc.m[0][0] = sc.m[1][1] = sc.m[2][2] = k;
        sc.m[3][3] = 1.0f;
        r_set_world(&sc);
    } else {
        r_set_world(NULL);
    }
    for (s = 0; s <= SKY_SEG; s++) {
        if (w->skyTiles) { top[s] = w->skyTop[s % SKY_SEG] | 0xFF000000u; bot[s] = w->skyBot[s % SKY_SEG] | 0xFF000000u; }
        else { top[s] = lerp_rgb(hor, zen, (float)pow(E_RAD2DEG(hi) / 90.0f, 0.6f)); bot[s] = lerp_rgb(fog, hor, 0.5f); }
    }
    /* panorama band: one draw per tile (textured), or one vertex-coloured band without tiles */
    if (w->skyTiles) {
        int spt = SKY_SEG / w->skyTiles;
        float inset = 0.5f / 256.0f;
        for (t = 0; t < w->skyTiles; t++) {
            n = 0; nv = 0;
            for (r = 0; r < SKY_ROWS; r++) {
                float vv = sky_v(w, rowE[r]);
                for (s = 0; s <= spt; s++) {
                    float az = 2.0f * E_PI * (float)(t * spt + s) / (float)SKY_SEG;
                    Vec3 d = sky_dir(az, rowE[r]);
                    RVert1 *o = &vt[nv++];
                    o->x = d.x; o->y = d.y; o->z = d.z;
                    o->color = 0xFFFFFFFFu;
                    o->u = inset + (1.0f - 2.0f * inset) * (float)s / (float)spt;
                    o->v = E_CLAMP(vv, 0.002f, 0.998f);
                }
            }
            for (r = 0; r + 1 < SKY_ROWS; r++) for (s = 0; s < spt; s++) {
                u16 a = (u16)(r * (spt + 1) + s), b = (u16)(a + 1), c = (u16)(a + spt + 1), d = (u16)(c + 1);
                if (!segVis[t * spt + s]) continue;
                ix[n++] = a; ix[n++] = c; ix[n++] = b;
                ix[n++] = b; ix[n++] = c; ix[n++] = d;
            }
            if (!n) continue;
            sky_mat(&rm, w->skyTex[t]);
            r_set_material(&rm);
            r_draw_mem(RFMT_V1, vt, nv, ix, n);
        }
    } else {
        n = 0; nv = 0;
        for (r = 0; r < SKY_ROWS; r++) {
            float e = rowE[r];
            u32 col = e < 0.0f ? lerp_rgb(hor, fog, e / lo) : lerp_rgb(hor, zen, (float)pow(E_RAD2DEG(e) / 90.0f, 0.6f));
            for (s = 0; s <= SKY_SEG; s++) {
                Vec3 d = sky_dir(2.0f * E_PI * (float)s / (float)SKY_SEG, e);
                RVert1 *o = &vt[nv++];
                o->x = d.x; o->y = d.y; o->z = d.z; o->color = col; o->u = o->v = 0.0f;
            }
        }
        for (r = 0; r + 1 < SKY_ROWS; r++) for (s = 0; s < SKY_SEG; s++) {
            u16 a = (u16)(r * (SKY_SEG + 1) + s), b = (u16)(a + 1), c = (u16)(a + SKY_SEG + 1), d = (u16)(c + 1);
            if (!segVis[s]) continue;
            ix[n++] = a; ix[n++] = c; ix[n++] = b;
            ix[n++] = b; ix[n++] = c; ix[n++] = d;
        }
        sky_mat(&rm, 0);
        r_set_material(&rm);
        r_draw_mem(RFMT_V1, vt, nv, ix, n);
    }
    /* zenith cap (3 rings + apex) and lower band (2 rings + nadir), vertex colours, one draw */
    {
        float capE[3], ce[3] = { 0.0f, 0.55f, 0.9f };
        int ring, apexTop, apexBot, rb;
        capE[0] = hi; capE[1] = hi + (E_PI * 0.5f - hi) * 0.45f; capE[2] = E_DEG2RAD(80.0f);
        if (capE[2] <= capE[1]) capE[2] = capE[1] + 0.02f;
        eLow = lo > E_DEG2RAD(-28.0f) ? E_DEG2RAD(-30.0f) : lo - E_DEG2RAD(4.0f);
        nv = 0; n = 0;
        for (ring = 0; ring < 3; ring++) for (s = 0; s <= SKY_SEG; s++) {
            Vec3 d = sky_dir(2.0f * E_PI * (float)s / (float)SKY_SEG, capE[ring]);
            RVert1 *o = &vt[nv++];
            o->x = d.x; o->y = d.y; o->z = d.z; o->u = o->v = 0.0f;
            o->color = lerp_rgb(top[s], zen, ce[ring]);
        }
        apexTop = nv;
        vt[nv].x = 0.0f; vt[nv].y = 100.0f; vt[nv].z = 0.0f; vt[nv].u = vt[nv].v = 0.0f; vt[nv].color = zen | 0xFF000000u;
        nv++;
        rb = nv;
        for (ring = 0; ring < 2; ring++) for (s = 0; s <= SKY_SEG; s++) {
            Vec3 d = sky_dir(2.0f * E_PI * (float)s / (float)SKY_SEG, ring ? eLow : lo);
            RVert1 *o = &vt[nv++];
            o->x = d.x; o->y = d.y; o->z = d.z; o->u = o->v = 0.0f;
            o->color = ring ? (fog | 0xFF000000u) : bot[s];
        }
        apexBot = nv;
        vt[nv].x = 0.0f; vt[nv].y = -100.0f; vt[nv].z = 0.0f; vt[nv].u = vt[nv].v = 0.0f; vt[nv].color = fog | 0xFF000000u;
        nv++;
        for (ring = 0; ring < 2; ring++) for (s = 0; s < SKY_SEG; s++) {
            u16 a = (u16)(ring * (SKY_SEG + 1) + s), b = (u16)(a + 1), c = (u16)(a + SKY_SEG + 1), d = (u16)(c + 1);
            if (!segVis[s]) continue;
            ix[n++] = a; ix[n++] = c; ix[n++] = b; ix[n++] = b; ix[n++] = c; ix[n++] = d;
        }
        for (s = 0; s < SKY_SEG; s++) {
            u16 a = (u16)(2 * (SKY_SEG + 1) + s);
            if (!segVis[s]) continue;
            ix[n++] = a; ix[n++] = (u16)apexTop; ix[n++] = (u16)(a + 1);
        }
        base = rb;
        for (s = 0; s < SKY_SEG; s++) {
            u16 a = (u16)(base + s), b = (u16)(a + 1), c = (u16)(a + SKY_SEG + 1), d = (u16)(c + 1);
            if (!segVis[s]) continue;
            ix[n++] = a; ix[n++] = c; ix[n++] = b; ix[n++] = b; ix[n++] = c; ix[n++] = d;
        }
        for (s = 0; s < SKY_SEG; s++) {
            u16 a = (u16)(base + SKY_SEG + 1 + s);
            if (!segVis[s]) continue;
            ix[n++] = a; ix[n++] = (u16)apexBot; ix[n++] = (u16)(a + 1);
        }
        sky_mat(&rm, 0);
        r_set_material(&rm);
        if (n) r_draw_mem(RFMT_V1, vt, nv, ix, n);
    }
    /* sun glow: additive billboard toward sunDir (drawn before the world, so terrain and trees cover it) */
    if (w->sunGlow && w->glowTex) {
        Vec3 d = v3_norm(w->sky.sunDir), up = v3(0.0f, 1.0f, 0.0f), rt, u2, c;
        static const u16 qi[6] = { 0, 1, 2, 0, 2, 3 };
        RVert1 q[4];
        u32 sun = w->sky.sun ? w->sky.sun : 0xFFC48C, col = 0xFF000000u;
        float half = 100.0f * (float)tan(E_DEG2RAD(11.0f));
        int i, ch;
        if (d.y > -0.1f) {
            for (ch = 0; ch < 24; ch += 8) col |= (u32)(((sun >> ch) & 0xFF) * 56 / 255) << ch;   /* subtle: A5 paints the disc + glow */
            rt = v3_norm(v3_cross(up, d));
            u2 = v3_cross(d, rt);
            c = v3_scale(d, 100.0f);
            for (i = 0; i < 4; i++) {
                float sx = (i == 0 || i == 3) ? -1.0f : 1.0f, sy = i < 2 ? 1.0f : -1.0f;
                Vec3 p = v3_add(c, v3_add(v3_scale(rt, sx * half), v3_scale(u2, sy * half)));
                q[i].x = p.x; q[i].y = p.y; q[i].z = p.z; q[i].color = col;
                q[i].u = sx > 0.0f ? 1.0f : 0.0f; q[i].v = sy > 0.0f ? 0.0f : 1.0f;
            }
            sky_mat(&rm, w->glowTex);
            rm.blend = RB_ADD;
            r_set_material(&rm);
            r_draw_mem(RFMT_V1, q, 4, qi, 6);
        }
    }
    r_set_view(&v->view);
    r_set_world(NULL);                              /* T16: undo the late-sky dome scale */
}

void world_draw_view(const World *w, const WView *v, WDrawStats *st)
{
    WDrawStats a, b;
    float vd = v->viewDist > 0.0f ? v->viewDist : 300.0f, fe, fs;
    u32 fog = w->sky.fog ? w->sky.fog : 0xB4C3CD;
    fe = (w->fogEnd > 0.0f && w->fogEnd < vd) ? w->fogEnd : vd;
    fs = w->fogStart > 0.0f ? w->fogStart : 0.4f * fe;
    if (fs > 0.9f * fe) fs = 0.9f * fe;
    r_set_fog(1, fog, fs, fe);
    world_draw_sky(w, v);
    world_draw_opaque(w, v, &a);
    world_draw_alpha(w, v, &b);
    if (st) {
        st->cells = a.cells > b.cells ? a.cells : b.cells;
        st->batches = a.batches + b.batches;
        st->inst = a.inst + b.inst;
        st->tris = a.tris + b.tris;
    }
}
#else
void world_draw_opaque(const World *w, const WView *v, WDrawStats *st) { world_visible(w, v, 0, NULL, 0, NULL, NULL, 0, st); }
void world_draw_alpha(const World *w, const WView *v, WDrawStats *st) { (void)w; (void)v; if (st) memset(st, 0, sizeof(*st)); }
void world_draw_sky(const World *w, const WView *v) { (void)w; (void)v; }
void world_draw_view(const World *w, const WView *v, WDrawStats *st) { (void)w; (void)v; if (st) memset(st, 0, sizeof(*st)); }
int world_sky_load(World *w, int maxTiles, const char *prefix) { (void)w; (void)maxTiles; (void)prefix; return 0; }
void world_set_sun_glow(World *w, int on) { w->sunGlow = on; }
void world_set_detail(World *w, int on) { w->detailOn = on; }
#endif
