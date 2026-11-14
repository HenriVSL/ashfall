/* hud_font.h - HUD graphics layer: stroke text + symbols from the A4/A4b/A4c atlas sets, textured lines, GDI
   fallback (DESIGN 10.1). Windows/render side. Owner: T07. Used by hud.cpp, hud_map.cpp and later hud_tgt.cpp (T15)
   and hud_wpn.cpp (T16).

   Atlas: hfont_init picks the set by screen height (480/600/768/1050: the largest set <= height, [hud] atlasSet
   overrides), loads data/tex/hud_atlas_<L>.txt and its pages (colour BMP + _a.bmp loaded with image flags 0 and
   combined: IMG_ALPHA's bleed breaks premultiplied texels, DESIGN 9.1b). Texels are premultiplied white with the
   drop shadow baked in (A4c), so every glyph, sprite and line is ONE quad drawn with RB_PREMUL; the vertex colour
   tints the shape, the shadow stays black. Colours passed to hg_* are PREMULTIPLIED (hg_col() makes one).

   Coordinates: hg_* take SCREEN PIXELS. The layout is designed in the virtual 640x480 space; hg_px(v) = round(v * k)
   with k = screen h / 480 (4:3 modes: x and y use the same k, origin top-left). The atlas is drawn 1:1 (texel =
   pixel) at whole-pixel positions, so text stays crisp at every set. Text y = cap top; text x = ink left / centre /
   right (align 0/1/2); ink width = sum(adv) - gap (monospaced digits, so number columns never wobble).

   Frame protocol: hg_begin() .. draw calls .. hg_end(). Calls are RECORDED and emitted in hg_end sorted by atlas page
   (one draw call per page: the 1050 set has 3 pages). hg_flush() emits what is recorded so far (before drawing with
   a foreign texture, e.g. the minimap island via hg_image_quad). The clip box (hg_clip) applies to lines (centre
   line clipped against the box shrunk by the stroke profile, so nothing is drawn outside it), text and sprites
   (axis-aligned quads cut with matching uv; rotated sprites are dropped when a corner is outside).

   GDI fallback (no atlas files): text through an e_ui Courier New font, lines untextured, sprites as small outline
   boxes, RB_BLEND, no shadow. Logged as "hud: atlas missing, GDI fallback". */
#ifndef HUD_FONT_H
#define HUD_FONT_H

#include "e_base.h"
#include "e_rend.h"

enum { HF_SMALL = 0, HF_LARGE = 1 };

void  hfont_params(void);                           /* [hud] atlasSet (called by hud_params) */
int   hfont_init(void);                             /* after r_init; 1 = atlas, 0 = GDI fallback (still usable) */
void  hfont_free(void);
void  hfont_text(float x, float y, int size, u32 argb, int align, const char *s);   /* virtual 640x480 units; needs
                                                       hg_begin/hg_end around it (cap top at y) */
float hfont_width(int size, const char *s);         /* ink width in virtual units */

/* ---- pixel layer (T07) ---- */
int   hg_ready(void);                               /* 1 = atlas loaded, 0 = GDI fallback */
int   hg_set_lines(void);                           /* atlas set (480/600/768/1050), 0 = fallback */
float hg_k(void);                                   /* screen px per virtual unit (h / 480) */
int   hg_px(float v);                               /* round(v * k) */
float hg_pxf(float v);                              /* v * k */
int   hg_stroke(void);                              /* line stroke px of the set */
int   hg_shadow_off(void);                          /* baked shadow offset px (0 = none) */
int   hg_screen_w(void);
int   hg_screen_h(void);
u32   hg_col(u32 argb, float a);                    /* straight ARGB x a -> premultiplied (RGB and alpha scaled) */

void  hg_begin(void);                               /* ui_begin + RB_PREMUL; clears the record and the clip */
void  hg_flush(void);                               /* emit the recorded quads now */
void  hg_end(void);                                 /* emit + ui_end */
void  hg_clip(int x0, int y0, int x1, int y1);      /* px, [x0,x1) x [y0,y1); x1 <= x0 = no clip */
void  hg_exclude(int x0, int y0, int x1, int y1);   /* add a keep-out box (max 6, px): lines are cut around it
                                                       (with the stroke margin), text/sprites touching it are skipped */
void  hg_exclude_clear(void);
void  hg_line(float x0, float y0, float x1, float y1, u32 argb);   /* px, set stroke, ends extended by stroke/2 */
void  hg_line_ext(float x0, float y0, float x1, float y1, float extPx, u32 argb);   /* explicit end extension */
void  hg_line_w(float x0, float y0, float x1, float y1, float widthScale, u32 argb);   /* thinner/thicker: the
                                                       profile is squeezed (widthScale < 1 = finer, softer) */
void  hg_rect(float x0, float y0, float x1, float y1, u32 argb);   /* solid (white texel), no shadow */
void  hg_box(int x0, int y0, int x1, int y1, u32 argb);   /* outline, stroke INSIDE [x0,x1) x [y0,y1), crisp */
void  hg_tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 argb);   /* solid triangle, no clip */
int   hg_text(int x, int y, int font, u32 argb, int align, const char *s);   /* px; returns the ink width */
int   hg_text_w(int font, const char *s);           /* ink width px */
int   hg_cap(int font);                             /* cap height px */
int   hg_adv(int font);                             /* digit advance px */
int   hg_gap(int font);                             /* empty px after the ink inside adv */
int   hg_sprite_id(const char *name);               /* -1 = not in this set */
void  hg_sprite(int id, float ax, float ay, u32 argb);   /* anchor at (ax, ay) px (rounded to whole px) */
void  hg_sprite_rot(int id, float ax, float ay, float angRad, u32 argb);   /* rotated about the anchor (+ =
                                                       clockwise on screen), anchor not rounded */
void  hg_sprite_scaled(int id, float ax, float ay, float scale, u32 argb);  /* scaled about the anchor */
int   hg_sprite_info(int id, int *w, int *h, int *ox, int *oy);   /* shape size (shadow excluded); 0 = bad id */
void  hg_nine(int id, int x0, int y0, int x1, int y1, u32 argb);  /* 9-slice frame (map_frame): corners 1:1 */
void  hg_image_quad(RTex t, const RVert2D v[4], int blend);   /* foreign texture quad (px): flushes, draws, back */

int   hfont_set_metrics(int lines, int font, int *cap, int *adv, int *gap, int *stroke);   /* parse another set's
                                                       txt (no textures): layout checks for every set; 0 = missing */

/* stats / test hooks */
int   hg_quads_last(void);                          /* quads emitted by the last hg_end (all flushes) */
int   hg_draws_last(void);                          /* draw batches (texture switches) of the last hg_end */
void  hg_bounds_reset(void);                        /* bounding box of everything recorded since the reset */
int   hg_bounds(float *x0, float *y0, float *x1, float *y1);   /* 0 = nothing recorded */

#endif
