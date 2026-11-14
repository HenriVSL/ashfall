/* e_tex.h - texture cache by name: refcount, budgets (DESIGN 2.5, 3.3). Windows side (uses the renderer).
   Owner: T05 (initial header + stub by T01).
   tex_get("road_a", imgFlags, rtFlags) loads data\tex\road_a.bmp (IMG_ALPHA: + road_a_a.bmp), generates mips unless
   RT_NOMIP and creates the texture; the same name+flags returns the cached handle with refcount+1. 0 = failed
   (err: logged). tex_reload_all is a no-op returning 1: the D3D6 backend keeps managed textures or sysmem copies
   and restores them itself in r_device_lost_check. */
#ifndef E_TEX_H
#define E_TEX_H

#include "e_rend.h"

RTex  tex_get(const char *name, int imgFlags, int rtFlags);  /* "road_a" -> data\tex\road_a.bmp, refcounted */
void  tex_release(RTex t);
void  tex_release_all(void);                      /* between races/menus */
int   tex_reload_all(void);                       /* after device loss when textures aren't managed */

/* ---- additions (T05) ---- */
RTex  tex_add(const char *name, const struct EImage *img, int rtFlags);  /* register a generated image under a name */
int   tex_count(void);                            /* cached entries */
void  tex_log_stats(void);                        /* "tex: cache entries=N kb=K renderer_kb=R budget_kb=B" */

#endif
