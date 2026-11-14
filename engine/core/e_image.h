/* e_image.h - BMP 24/8-bit load, magenta key, alpha file, mips (DESIGN 2.5, 3.3). Portable. Owner: T05
   (initial header + stub by T01).
   Pixels are ARGB8888 (0xAARRGGBB), rows top-down, level 0 = full size, level i = max(1, w>>i) x max(1, h>>i).
   img_load: path as given if the file exists, else relative to the data folder (data_path), so "tex/road_a.bmp"
   works from the game. Reads 24-bit, 32-bit (alpha ignored) and 8-bit paletted uncompressed BMPs, bottom-up or
   top-down. IMG_KEYED: RGB(255,0,255) texels get alpha 0 and their colour is replaced by the average of the nearest
   opaque texels (colour bleed), so bilinear filtering and mips never show magenta fringes. IMG_ALPHA: the alpha
   channel comes from "<name>_a.bmp" (greyscale, red channel used); a missing alpha file logs a warning, alpha 255.
   img_gen_mips: alpha-weighted 2x2 box filter down to 1x1 (keyed/alpha texels never bleed colour). */
#ifndef E_IMAGE_H
#define E_IMAGE_H

#include "e_base.h"

enum { IMG_KEYED = 1, IMG_ALPHA = 2 };             /* keyed: magenta -> alpha 0 (1555); alpha: *_a.bmp (4444) */
typedef struct EImage { int w, h, levels, flags; u32 *pix[12]; } EImage;  /* ARGB8888 per mip level */
int   img_load(EImage *img, const char *path, int flags);  /* path "tex/road_a.bmp"; IMG_ALPHA loads road_a_a.bmp */
void  img_gen_mips(EImage *img);                   /* alpha-weighted box filter; key-safe */
void  img_free(EImage *img);
int   img_save_bmp24(const char *path, const u32 *argb, int w, int h);

/* ---- additions (T05) ---- */
int   img_alloc(EImage *img, int w, int h, int flags);   /* level 0 only, zeroed; 0 on failure */
void  img_apply_key(EImage *img);                  /* level 0: magenta -> alpha 0 + colour bleed (img_load calls it) */
void  img_bleed(EImage *img);                      /* level 0: colour of alpha-0 texels from opaque neighbours */
int   img_level_w(const EImage *img, int level);
int   img_level_h(const EImage *img, int level);
int   img_kb(const EImage *img, int bytesPerTexel); /* all levels */

#endif
