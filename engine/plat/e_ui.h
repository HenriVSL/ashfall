/* e_ui.h - GDI-built glyph atlases (ARGB4444), 2D quad batches, text in a virtual 640x480 space (DESIGN 2.5, 3.11).
   Windows side. Owner: T05 (initial header + stub by T01).
   font_create renders the face once with GDI (4x supersampled, so smoothing does not depend on the Win98 "smooth
   fonts" setting) into one atlas texture: Latin-1 32..126 + 160..255 (falls back to ASCII + Finnish/Swedish letters,
   then ASCII 32..95, when the atlas would exceed the device's max texture size); missing glyphs draw as '?'.
   px = character height in screen pixels (callers scale design sizes with ui_px). outlinePx > 0 adds a black outline.
   Text colour = glyph white x argb. ui_text: (x, y) = top-left of the text line in virtual units (align 0 left,
   1 centre, 2 right of x); '\n' starts a new line. Positions are snapped to whole pixels; glyphs are drawn at their
   pixel size (not scaled). ui_rect/ui_image take virtual units. Quads are clipped to the screen (no guard band).
   All 2D uses RB_BLEND; batches flush on texture change and in ui_end. Call between r_begin_frame and r_end_frame. */
#ifndef E_UI_H
#define E_UI_H

#include "e_rend.h"

typedef struct Font { RTex tex; int texW, texH, height, ascent; u8 adv[256], gw[256], gh[256]; u16 gu[256], gv[256];
                      /* additions (T05) */ s8 ox[256]; int glyphs; } Font;
int   font_create(Font *f, const char *face, int px, int bold, int outlinePx);  /* GDI -> ARGB4444 atlas, Latin-1 */
void  font_free(Font *f);
int   font_width(const Font *f, const char *s);
void  ui_begin(int virtW, int virtH);              /* HUD space 640x480 mapped to the screen (3.11) */
void  ui_text(const Font *f, float x, float y, u32 argb, int align, const char *s);   /* align 0 L, 1 C, 2 R */
void  ui_rect(float x, float y, float w, float h, u32 argb);
void  ui_image(RTex t, float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 argb);
void  ui_end(void);                                /* flush batches (one r_draw_2d per texture) */

/* ---- additions (T05) ---- */
int   ui_px(float designPx);                       /* design size at 480 lines -> screen pixels (h/480 scale) */
float ui_scale(void);                              /* current virtual -> screen scale (after ui_begin) */
void  ui_text_px(const Font *f, int px, int py, u32 argb, const char *s);   /* screen pixels, no mapping */

/* ---- additions (jet T01, DESIGN 2.3) ---- */
void  ui_line(float x0, float y0, float x1, float y1, float w, u32 argb);  /* virtual units; quad of width w
                                                      (>= 1 screen px), clipped (Cohen-Sutherland) to the clip box */
void  ui_set_blend(int blend);                     /* RB_BLEND (default after ui_begin), RB_ADD or RB_PREMUL
                                                      (jet T02: premultiplied colours, ONE/INVSRCALPHA); flushes */
void  ui_clip(float x, float y, float w, float h); /* clip box in virtual units for rect/image/text/line;
                                                      w or h <= 0 = whole screen (default after ui_begin) */

/* ---- additions (jet T07, orchestrator-approved append) ---- */
void  ui_quad4_px(RTex t, const RVert2D v[4]);     /* one quad in SCREEN pixels (pixel edges; the backend subtracts
                                                      0.5), vertices in order 0-1-2-3 (triangles 0,1,2 + 0,2,3) with
                                                      their own uv/colour; NOT clipped (caller clips); batched with
                                                      the current blend; flushes on texture change */
void  ui_line_tex(RTex t, float x0, float y0, float x1, float y1, float u, float v0, float v1, float widthPx,
                  float centrePx, u32 argb);       /* textured line (virtual units): a quad widthPx screen px wide
                                                      across the line whose texture v runs v0 -> v1 across it (u fixed:
                                                      an atlas line-profile cell); the line sits centrePx from the v0
                                                      side; +v points to the screen down/right side (baked shadows fall
                                                      down-right). Centre line clipped to the clip box shrunk by the
                                                      larger side. Same batch as glyph quads of the same texture. */
int   ui_quad_count(int reset);                    /* quads submitted since the last reset (all ui_* calls) */

#endif
