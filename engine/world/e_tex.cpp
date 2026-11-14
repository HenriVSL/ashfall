/* e_tex.cpp - see e_tex.h. Owner: T05. Up to 512 named entries; linear search (loads only, never per frame). */
#include "e_tex.h"
#include "e_image.h"
#include "e_log.h"

enum { TEX_CACHE_MAX = 512 };
typedef struct TexEntry { char name[32]; int imgFlags, rtFlags, refs, kb; RTex t; } TexEntry;
static TexEntry s_ent[TEX_CACHE_MAX];

static TexEntry *find_name(const char *name, int imgFlags, int rtFlags)
{
    int i;
    for (i = 0; i < TEX_CACHE_MAX; i++)
        if (s_ent[i].t && s_ent[i].imgFlags == imgFlags && s_ent[i].rtFlags == rtFlags && !e_stricmp(s_ent[i].name, name))
            return &s_ent[i];
    return NULL;
}

static TexEntry *find_handle(RTex t)
{
    int i;
    if (!t) return NULL;
    for (i = 0; i < TEX_CACHE_MAX; i++) if (s_ent[i].t == t) return &s_ent[i];
    return NULL;
}

static TexEntry *free_slot(void)
{
    int i;
    for (i = 0; i < TEX_CACHE_MAX; i++) if (!s_ent[i].t) return &s_ent[i];
    log_printf(LOG_ERR, "err: tex cache full (%d)", TEX_CACHE_MAX);
    return NULL;
}

static RTex add_entry(const char *name, const EImage *img, int imgFlags, int rtFlags)
{
    TexEntry *e = free_slot();
    int before = r_tex_kb();
    RTex t;
    if (!e) return 0;
    t = r_tex_create(img, rtFlags);
    if (!t) {
        log_printf(LOG_ERR, "err: tex create failed name=%s %dx%d", name, img->w, img->h);
        return 0;
    }
    e_strlcpy(e->name, name, (int)sizeof(e->name));
    e->imgFlags = imgFlags;
    e->rtFlags = rtFlags;
    e->refs = 1;
    e->t = t;
    e->kb = r_tex_kb() - before;
    log_printf(LOG_INFO, "tex: load name=%s %dx%d levels=%d flags=%d kb=%d total_kb=%d budget_kb=%d", name, img->w, img->h,
               (rtFlags & RT_NOMIP) ? 1 : img->levels, imgFlags, e->kb, r_tex_kb(), r_tex_budget_kb());
    return t;
}

RTex tex_get(const char *name, int imgFlags, int rtFlags)
{
    TexEntry *e;
    EImage img;
    char rel[E_PATH];
    RTex t;
    if (!name || !name[0]) return 0;
    e = find_name(name, imgFlags, rtFlags);
    if (e) { e->refs++; return e->t; }
    e_snprintf(rel, (int)sizeof(rel), "tex/%s.bmp", name);
    if (!img_load(&img, rel, imgFlags)) return 0;
    if (!(rtFlags & RT_NOMIP)) img_gen_mips(&img);
    t = add_entry(name, &img, imgFlags, rtFlags);
    img_free(&img);
    return t;
}

RTex tex_add(const char *name, const EImage *img, int rtFlags)
{
    TexEntry *e;
    if (!name || !img) return 0;
    e = find_name(name, img->flags, rtFlags);
    if (e) { e->refs++; return e->t; }
    return add_entry(name, img, img->flags, rtFlags);
}

void tex_release(RTex t)
{
    TexEntry *e = find_handle(t);
    if (!e) return;
    if (--e->refs > 0) return;
    r_tex_free(e->t);
    memset(e, 0, sizeof(*e));
}

void tex_release_all(void)
{
    int i, n = 0;
    for (i = 0; i < TEX_CACHE_MAX; i++) {
        if (!s_ent[i].t) continue;
        r_tex_free(s_ent[i].t);
        memset(&s_ent[i], 0, sizeof(TexEntry));
        n++;
    }
    if (n) log_printf(LOG_INFO, "tex: release_all freed=%d renderer_kb=%d", n, r_tex_kb());
}

int tex_reload_all(void)
{
    r_device_lost_check();
    return 1;
}

int tex_count(void)
{
    int i, n = 0;
    for (i = 0; i < TEX_CACHE_MAX; i++) if (s_ent[i].t) n++;
    return n;
}

void tex_log_stats(void)
{
    int i, n = 0, kb = 0;
    for (i = 0; i < TEX_CACHE_MAX; i++) if (s_ent[i].t) { n++; kb += s_ent[i].kb; }
    log_printf(LOG_INFO, "tex: cache entries=%d kb=%d renderer_textures=%d renderer_kb=%d budget_kb=%d", n, kb,
               r_tex_count(), r_tex_kb(), r_tex_budget_kb());
}
