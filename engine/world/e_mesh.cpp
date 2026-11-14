/* e_mesh.cpp - .msh loader (DESIGN 8.3, layout fixed by T02's assetkit/export.py), LOD pick, static and CPU-lit
   drawing. Owner: T07. Loading is portable; everything that calls the renderer is inside #ifndef E_NO_RENDER so the
   Mac harness (physh, built with -DE_NO_RENDER) links the loader without a renderer. See e_mesh.h for the rules. */
#include "e_mesh.h"
#include "e_file.h"
#include "e_log.h"
#ifndef E_NO_RENDER
#include "e_tex.h"
#include "e_image.h"
#endif

enum { MSH_HDR = 60, MSH_MAT = 68, MSH_PART = 12, MSH_LOD = 20, MSH_TAG = 40, MSH_VERT = 24, MSH_NRM = 12 };
enum { MESH_LIT_MAX = 4096 };

static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static float rdf(const u8 *p) { float f; u32 u = rd32(p); memcpy(&f, &u, 4); return f; }

static void copy_name(char *dst, const u8 *src, int n)
{
    memcpy(dst, src, (size_t)n);
    dst[n - 1] = 0;
}

/* "v10gt" -> data\meshes\v10gt.msh; an existing path as is; else data-relative */
static void mesh_path(char *out, int n, const char *path)
{
    if (file_exists(path)) { e_strlcpy(out, path, n); return; }
    if (!strchr(path, '/') && !strchr(path, '\\') && !strchr(path, '.')) {
        char rel[E_PATH];
        e_snprintf(rel, (int)sizeof(rel), "meshes/%s.msh", path);
        data_path(out, n, rel);
        return;
    }
    data_path(out, n, path);
}

static void base_name(char *out, int n, const char *path)
{
    const char *b = path, *p;
    char *dot;
    for (p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    e_strlcpy(out, b, n);
    dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

static int mesh_fail(Mesh *m, void *file, const char *path, const char *why)
{
    log_printf(LOG_ERR, "err: mesh load failed path=%s reason=%s", path, why);
    if (file) free(file);
    mesh_free(m);
    return 0;
}

#ifndef E_NO_RENDER
static void mesh_render_create(Mesh *m)
{
    int i;
    for (i = 0; i < m->numMats; i++) {
        MeshMat *mm = &m->mats[i];
        RMaterial *rm = &mm->rm;
        int imgFlags = mm->blend == RB_ALPHATEST ? IMG_KEYED : (mm->blend == RB_BLEND ? IMG_ALPHA : 0);
        memset(rm, 0, sizeof(*rm));
        rm->blend = mm->blend;
        rm->stage1 = mm->stage1;
        rm->cullNone = mm->cullNone;
        rm->zTest = 1;
        rm->zWrite = (mm->blend == RB_OPAQUE || mm->blend == RB_ALPHATEST) ? 1 : 0;
        rm->fog = 1;
        rm->lodBias = (s8)(mm->blend == RB_ALPHATEST ? -2 : 0);   /* T16: sharper mips on alpha-tested parts */
        if (mm->tex[0]) rm->tex0 = tex_get(mm->tex, imgFlags, 0);
        if (mm->tex1[0]) rm->tex1 = tex_get(mm->tex1, 0, mm->stage1 == RS1_ENVADD ? (RT_CLAMP | RT_NOMIP) : 0);
        if (!rm->tex1) rm->stage1 = RS1_NONE;
    }
    for (i = 0; i < m->numLods; i++) {
        const MeshLod *l = &m->lod[i];
        m->lodVb[i] = r_vb_create(RFMT_V1, m->verts + l->firstVert, l->numVerts);
        if (!m->lodVb[i]) log_printf(LOG_ERR, "err: mesh vb failed name=%s lod=%d verts=%d", m->name, i, l->numVerts);
    }
    m->vb = m->lodVb[0];
}

static void mesh_render_free(Mesh *m)
{
    int i;
    for (i = 0; i < 4; i++) if (m->lodVb[i]) r_vb_free(m->lodVb[i]);
    for (i = 0; i < m->numMats; i++) {
        if (m->mats[i].rm.tex0) tex_release(m->mats[i].rm.tex0);
        if (m->mats[i].rm.tex1) tex_release(m->mats[i].rm.tex1);
    }
}
#endif

int mesh_load(Mesh *m, const char *path, int noRender)
{
    char full[E_PATH];
    u8 *f, *blk;
    const u8 *p;
    int size, i, need, nv, ni, nm, np, nl, nt;
    memset(m, 0, sizeof(*m));
    if (!path || !path[0]) return mesh_fail(m, NULL, "(null)", "no path");
    mesh_path(full, (int)sizeof(full), path);
    f = (u8 *)file_load(full, &size);
    if (!f) return mesh_fail(m, NULL, full, "cannot read");
    if (size < MSH_HDR || memcmp(f, "RMSH", 4) || rd32(f + 4) != 1) return mesh_fail(m, f, full, "bad header");
    nv = (int)rd32(f + 8); ni = (int)rd32(f + 12); nm = (int)rd32(f + 16);
    np = (int)rd32(f + 20); nl = (int)rd32(f + 24); nt = (int)rd32(f + 28);
    if (nv <= 0 || nv > 65535 || ni < 0 || nm <= 0 || nm > 64 || np <= 0 || nl <= 0 || nl > 4 || nt < 0 || nt > 1000)
        return mesh_fail(m, f, full, "bad counts");
    need = MSH_HDR + nm * MSH_MAT + np * MSH_PART + nl * MSH_LOD + nt * MSH_TAG + nv * (MSH_VERT + MSH_NRM) + ni * 2;
    if (size < need) return mesh_fail(m, f, full, "truncated");
    base_name(m->name, (int)sizeof(m->name), full);
    m->box.lo = v3(rdf(f + 32), rdf(f + 36), rdf(f + 40));
    m->box.hi = v3(rdf(f + 44), rdf(f + 48), rdf(f + 52));
    m->radius = rdf(f + 56);
    blk = (u8 *)malloc((size_t)nv * (sizeof(RVert1) + sizeof(Vec3)) + (size_t)ni * 2 + 4 +
                       (size_t)nm * sizeof(MeshMat) + (size_t)np * sizeof(MeshPart));
    if (!blk) return mesh_fail(m, f, full, "out of memory");
    m->mem = blk;
    m->mats = (MeshMat *)blk;                        blk += (size_t)nm * sizeof(MeshMat);
    m->parts = (MeshPart *)blk;                      blk += (size_t)np * sizeof(MeshPart);
    m->verts = (RVert1 *)blk;                        blk += (size_t)nv * sizeof(RVert1);
    m->normals = (Vec3 *)blk;                        blk += (size_t)nv * sizeof(Vec3);
    m->idx = (u16 *)blk;
    m->numVerts = nv; m->numIdx = ni; m->numMats = nm; m->numParts = np; m->numLods = nl;
    p = f + MSH_HDR;
    memset(m->mats, 0, (size_t)nm * sizeof(MeshMat));
    for (i = 0; i < nm; i++, p += MSH_MAT) {
        MeshMat *mm = &m->mats[i];
        copy_name(mm->tex, p, 32);
        copy_name(mm->tex1, p + 32, 32);
        mm->blend = p[64]; mm->stage1 = p[65]; mm->cullNone = p[66]; mm->flags = p[67];
    }
    for (i = 0; i < np; i++, p += MSH_PART) {
        m->parts[i].mat = (int)rd32(p); m->parts[i].firstIdx = (int)rd32(p + 4); m->parts[i].numIdx = (int)rd32(p + 8);
    }
    for (i = 0; i < nl; i++, p += MSH_LOD) {
        MeshLod *l = &m->lod[i];
        l->maxDist = rdf(p); l->firstPart = (int)rd32(p + 4); l->numParts = (int)rd32(p + 8);
        l->firstVert = (int)rd32(p + 12); l->numVerts = (int)rd32(p + 16);
    }
    for (i = 0; i < nt; i++, p += MSH_TAG) {
        MeshTag *t;
        if (i >= E_COUNT(m->tags)) continue;
        t = &m->tags[m->numTags++];
        copy_name(t->name, p, 16);
        t->pos = v3(rdf(p + 16), rdf(p + 20), rdf(p + 24));
        t->dir = v3(rdf(p + 28), rdf(p + 32), rdf(p + 36));
    }
    if (nt > E_COUNT(m->tags)) log_printf(LOG_WARN, "warn: mesh tags clipped name=%s tags=%d max=%d", m->name, nt, E_COUNT(m->tags));
    memcpy(m->verts, p, (size_t)nv * MSH_VERT);      p += nv * MSH_VERT;
    memcpy(m->normals, p, (size_t)nv * MSH_NRM);     p += nv * MSH_NRM;
    memcpy(m->idx, p, (size_t)ni * 2);
    free(f);
    /* validate: parts in range, LOD-relative indices inside their LOD's vertex range */
    for (i = 0; i < nl; i++) {
        const MeshLod *l = &m->lod[i];
        int k;
        if (l->firstPart < 0 || l->numParts < 0 || l->firstPart + l->numParts > np ||
            l->firstVert < 0 || l->numVerts <= 0 || l->firstVert + l->numVerts > nv)
            return mesh_fail(m, NULL, full, "bad lod");
        if (l->numVerts > MESH_LIT_MAX) log_printf(LOG_WARN, "warn: mesh lod over lit max name=%s lod=%d verts=%d", m->name, i, l->numVerts);
        for (k = l->firstPart; k < l->firstPart + l->numParts; k++) {
            const MeshPart *pt = &m->parts[k];
            int j;
            if (pt->mat < 0 || pt->mat >= nm || pt->firstIdx < 0 || pt->numIdx < 0 || pt->firstIdx + pt->numIdx > ni ||
                (pt->numIdx % 3))
                return mesh_fail(m, NULL, full, "bad part");
            for (j = 0; j < pt->numIdx; j++)
                if ((int)m->idx[pt->firstIdx + j] >= l->numVerts) return mesh_fail(m, NULL, full, "index out of range");
        }
    }
    if (!noRender) {
#ifndef E_NO_RENDER
        mesh_render_create(m);
#endif
    }
    log_printf(LOG_INFO, "mesh: load name=%s verts=%d tris=%d mats=%d lods=%d tags=%d lod_tris=%d/%d/%d render=%d",
               m->name, nv, ni / 3, nm, nl, m->numTags, mesh_lod_tris(m, 0), nl > 1 ? mesh_lod_tris(m, 1) : 0,
               nl > 2 ? mesh_lod_tris(m, 2) : 0, noRender ? 0 : 1);
    return 1;
}

void mesh_free(Mesh *m)
{
#ifndef E_NO_RENDER
    if (m->mats) mesh_render_free(m);
#endif
    if (m->mem) free(m->mem);
    memset(m, 0, sizeof(*m));
}

int mesh_lod(const Mesh *m, float dist, float lodScale)
{
    int i;
    if (lodScale <= 0.0f) lodScale = 1.0f;
    for (i = 0; i < m->numLods; i++) if (dist < m->lod[i].maxDist * lodScale) return i;
    return m->numLods > 0 ? m->numLods - 1 : 0;
}

const MeshTag *mesh_tag(const Mesh *m, const char *name)
{
    int i;
    for (i = 0; i < m->numTags; i++) if (!strcmp(m->tags[i].name, name)) return &m->tags[i];
    return NULL;
}

int mesh_lod_tris(const Mesh *m, int lod)
{
    int k, n = 0;
    if (lod < 0 || lod >= m->numLods) return 0;
    for (k = m->lod[lod].firstPart; k < m->lod[lod].firstPart + m->lod[lod].numParts; k++) n += m->parts[k].numIdx / 3;
    return n;
}

int mesh_tex_override_ok(const Mesh *m, int mat)
{
    if (mat < 0 || mat >= m->numMats || !m->mats[0].tex[0]) return 0;
    return !e_stricmp(m->mats[mat].tex, m->mats[0].tex);
}

/* ---- T15: tinted / merged static drawing helpers (portable part) ---- */
u32 mesh_tint_rgb(u32 c, u32 tint)
{
    u32 o = 0;
    int sh;
    if (tint == 0xFFFFFFFFu || tint == 0) return c;
    for (sh = 0; sh < 32; sh += 8) {
        u32 a = (c >> sh) & 0xFF, b = (tint >> sh) & 0xFF;
        o |= ((a * b + 127) / 255) << sh;
    }
    return o;
}

static void part_range(const Mesh *m, const MeshPart *pt, int *lo, int *hi)
{
    const u16 *src = m->idx + pt->firstIdx;
    int j;
    *lo = 65535; *hi = -1;
    for (j = 0; j < pt->numIdx; j++) {
        if ((int)src[j] < *lo) *lo = src[j];
        if ((int)src[j] > *hi) *hi = src[j];
    }
}

int mesh_append_part(const Mesh *m, int lod, int part, const Mat4 *xf, u32 tint, RVert1 *dst, int maxVerts,
                     u16 *dstIdx, int maxIdx, int baseVert, int *nIdx)
{
    const MeshLod *l;
    const MeshPart *pt;
    const u16 *src;
    int lo, hi, nv, j;
    if (nIdx) *nIdx = 0;
    if (lod < 0 || lod >= m->numLods) return 0;
    l = &m->lod[lod];
    if (part < l->firstPart || part >= l->firstPart + l->numParts) return 0;
    pt = &m->parts[part];
    part_range(m, pt, &lo, &hi);
    if (hi < lo) return 0;
    nv = hi - lo + 1;
    if (nv > maxVerts || pt->numIdx > maxIdx || baseVert + nv > 65535) return 0;
    for (j = 0; j < nv; j++) {
        const RVert1 *s = &m->verts[l->firstVert + lo + j];
        RVert1 *o = &dst[j];
        Vec3 p = m4_mul_point(xf, v3(s->x, s->y, s->z));
        o->x = p.x; o->y = p.y; o->z = p.z;
        o->color = mesh_tint_rgb(s->color, tint);
        o->u = s->u; o->v = s->v;
    }
    src = m->idx + pt->firstIdx;
    for (j = 0; j < pt->numIdx; j++) dstIdx[j] = (u16)(src[j] - lo + baseVert);
    if (nIdx) *nIdx = pt->numIdx;
    return nv;
}

#ifndef E_NO_RENDER
void mesh_draw(const Mesh *m, const Mat4 *world, int lod, RTex tintTexOverride)
{
    const MeshLod *l;
    int k;
    if (!m->numLods) return;
    if (lod < 0) lod = 0;
    if (lod >= m->numLods) lod = m->numLods - 1;
    l = &m->lod[lod];
    if (!m->lodVb[lod]) return;
    r_set_world(world);
    for (k = l->firstPart; k < l->firstPart + l->numParts; k++) {
        const MeshPart *pt = &m->parts[k];
        RMaterial rm = m->mats[pt->mat].rm;
        if (tintTexOverride && mesh_tex_override_ok(m, pt->mat)) rm.tex0 = tintTexOverride;
        r_set_material(&rm);
        r_draw_vb(m->lodVb[lod], m->idx + pt->firstIdx, pt->numIdx);
    }
    r_set_world(NULL);
}

void mesh_draw_ex(const Mesh *m, const Mat4 *world, int lod, RTex texOverride, u32 tint, int passMask)
{
    static RVert1 s_tv[MESH_LIT_MAX];
    static u16 s_ti[MESH_LIT_MAX * 3];
    const MeshLod *l;
    int k, plain = (tint == 0 || tint == 0xFFFFFFFFu);
    if (!m->numLods) return;
    if (lod < 0) lod = 0;
    if (lod >= m->numLods) lod = m->numLods - 1;
    l = &m->lod[lod];
    if (!m->lodVb[lod]) return;
    r_set_world(world);
    for (k = l->firstPart; k < l->firstPart + l->numParts; k++) {
        const MeshPart *pt = &m->parts[k];
        RMaterial rm = m->mats[pt->mat].rm;
        int pass = rm.blend == RB_OPAQUE ? MESH_PASS_OPAQUE : MESH_PASS_ALPHA;
        if (!(passMask & pass)) continue;
        if (texOverride && mesh_tex_override_ok(m, pt->mat)) rm.tex0 = texOverride;
        r_set_material(&rm);
        if (plain) {
            r_draw_vb(m->lodVb[lod], m->idx + pt->firstIdx, pt->numIdx);
        } else {
            Mat4 id;
            int nv, ni;
            m4_ident(&id);
            nv = mesh_append_part(m, lod, k, &id, tint, s_tv, MESH_LIT_MAX, s_ti, MESH_LIT_MAX * 3, 0, &ni);
            if (nv) r_draw_mem(RFMT_V1, s_tv, nv, s_ti, ni);
        }
    }
    r_set_world(NULL);
}

static int clamp255(float f) { int i = e_ftoi(f); return i < 0 ? 0 : (i > 255 ? 255 : i); }

void mesh_draw_lit(const Mesh *m, const Mat4 *world, int lod, RTex texOverride,
                   Vec3 sunDirBody, u32 sunRGB, u32 ambRGB, int envMap, const Mat4 *view)
{
    static RVert2 s_v2[MESH_LIT_MAX];
    static RVert1 s_v1[MESH_LIT_MAX];
    static u16 s_idx[MESH_LIT_MAX * 3];
    const MeshLod *l;
    float ar = (float)((ambRGB >> 16) & 0xFF), ag = (float)((ambRGB >> 8) & 0xFF), ab = (float)(ambRGB & 0xFF);
    float sr = (float)((sunRGB >> 16) & 0xFF), sg = (float)((sunRGB >> 8) & 0xFF), sb = (float)(sunRGB & 0xFF);
    Mat4 wv;
    int i, k, nv, useEnv;
    if (!m->numLods) return;
    if (lod < 0) lod = 0;
    if (lod >= m->numLods) lod = m->numLods - 1;
    l = &m->lod[lod];
    nv = l->numVerts;
    if (nv > MESH_LIT_MAX) return;
    useEnv = envMap && view;
    if (useEnv) m4_mul(&wv, world, view);
    for (i = 0; i < nv; i++) {
        const RVert1 *s = &m->verts[l->firstVert + i];
        Vec3 n = m->normals[l->firstVert + i];
        float d = n.x * sunDirBody.x + n.y * sunDirBody.y + n.z * sunDirBody.z, h = 0.75f + 0.25f * n.y;
        u32 c = s->color;
        float cr = (float)((c >> 16) & 0xFF) * (1.0f / 255.0f), cg = (float)((c >> 8) & 0xFF) * (1.0f / 255.0f);
        float cb = (float)(c & 0xFF) * (1.0f / 255.0f);
        u32 col;
        if (d < 0.0f) d = 0.0f;
        col = (c & 0xFF000000u) | ((u32)clamp255((ar * h + sr * d) * cr) << 16) |
              ((u32)clamp255((ag * h + sg * d) * cg) << 8) | (u32)clamp255((ab * h + sb * d) * cb);
        if (useEnv) {
            RVert2 *o = &s_v2[i];
            Vec3 vn = v3_norm(m4_mul_dir(&wv, n));
            o->x = s->x; o->y = s->y; o->z = s->z; o->color = col; o->u = s->u; o->v = s->v;
            o->u2 = vn.x * 0.5f + 0.5f;
            o->v2 = -vn.y * 0.5f + 0.5f;
        } else {
            RVert1 *o = &s_v1[i];
            *o = *s;
            o->color = col;
        }
    }
    r_set_world(world);
    for (k = l->firstPart; k < l->firstPart + l->numParts; k++) {
        const MeshPart *pt = &m->parts[k];
        RMaterial rm = m->mats[pt->mat].rm;
        if (texOverride && mesh_tex_override_ok(m, pt->mat)) rm.tex0 = texOverride;
        if (!useEnv && rm.stage1 == RS1_ENVADD) rm.stage1 = RS1_NONE;
        r_set_material(&rm);
        {   /* the backend transforms every vertex it is given: pass only this part's vertex range */
            const u16 *src = m->idx + pt->firstIdx;
            int j, lo = 65535, hi = -1, n = pt->numIdx > MESH_LIT_MAX * 3 ? MESH_LIT_MAX * 3 : pt->numIdx;
            for (j = 0; j < n; j++) {
                if ((int)src[j] < lo) lo = src[j];
                if ((int)src[j] > hi) hi = src[j];
            }
            if (hi < lo) continue;
            for (j = 0; j < n; j++) s_idx[j] = (u16)(src[j] - lo);
            if (useEnv) r_draw_mem(RFMT_V2, s_v2 + lo, hi - lo + 1, s_idx, n);
            else r_draw_mem(RFMT_V1, s_v1 + lo, hi - lo + 1, s_idx, n);
        }
    }
    r_set_world(NULL);
}
#else
void mesh_draw_ex(const Mesh *m, const Mat4 *world, int lod, RTex texOverride, u32 tint, int passMask)
{
    (void)m; (void)world; (void)lod; (void)texOverride; (void)tint; (void)passMask;
}

void mesh_draw(const Mesh *m, const Mat4 *world, int lod, RTex tintTexOverride)
{
    (void)m; (void)world; (void)lod; (void)tintTexOverride;
}

void mesh_draw_lit(const Mesh *m, const Mat4 *world, int lod, RTex texOverride,
                   Vec3 sunDirBody, u32 sunRGB, u32 ambRGB, int envMap, const Mat4 *view)
{
    (void)m; (void)world; (void)lod; (void)texOverride; (void)sunDirBody; (void)sunRGB; (void)ambRGB;
    (void)envMap; (void)view;
}
#endif
