/* e_image.cpp - see e_image.h. Portable (VC6 + clang). Owner: T05. */
#include "e_image.h"
#include "e_file.h"
#include "e_log.h"

static u32 rd16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

int img_level_w(const EImage *img, int level) { int w = img->w >> level; return w < 1 ? 1 : w; }
int img_level_h(const EImage *img, int level) { int h = img->h >> level; return h < 1 ? 1 : h; }

int img_kb(const EImage *img, int bytesPerTexel)
{
    int i, n = 0;
    for (i = 0; i < img->levels; i++) n += img_level_w(img, i) * img_level_h(img, i) * bytesPerTexel;
    return (n + 1023) / 1024;
}

int img_alloc(EImage *img, int w, int h, int flags)
{
    memset(img, 0, sizeof(*img));
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    img->pix[0] = (u32 *)calloc((size_t)w * (size_t)h, 4);
    if (!img->pix[0]) return 0;
    img->w = w;
    img->h = h;
    img->levels = 1;
    img->flags = flags;
    return 1;
}

void img_free(EImage *img)
{
    int i;
    for (i = 0; i < 12; i++) if (img->pix[i]) free(img->pix[i]);
    memset(img, 0, sizeof(*img));
}

/* decode an in-memory BMP into ARGB8888 top-down; alpha 255 */
static int decode_bmp(const u8 *d, int size, u32 **out, int *ow, int *oh, const char *path)
{
    u32 off, hsz, comp, bpp, nPal;
    s32 w, h;
    int topDown, x, y, rowBytes, ah;
    u32 pal[256];
    u32 *pix;
    if (size < 54 || d[0] != 'B' || d[1] != 'M') {
        log_printf(LOG_ERR, "err: img not a BMP path=%s", path);
        return 0;
    }
    off = rd32(d + 10);
    hsz = rd32(d + 14);
    w = (s32)rd32(d + 18);
    h = (s32)rd32(d + 22);
    bpp = rd16(d + 28);
    comp = rd32(d + 30);
    nPal = hsz >= 40 ? rd32(d + 46) : 0;
    topDown = h < 0;
    ah = topDown ? -h : h;
    if (w <= 0 || ah <= 0 || w > 4096 || ah > 4096 || (comp != 0 && !(comp == 3 && bpp == 32)) ||
        (bpp != 8 && bpp != 24 && bpp != 32)) {
        log_printf(LOG_ERR, "err: img unsupported BMP path=%s w=%d h=%d bpp=%u comp=%u", path, (int)w, (int)h,
                   (unsigned)bpp, (unsigned)comp);
        return 0;
    }
    rowBytes = (int)(((u32)w * bpp + 31) / 32 * 4);
    if ((s64)off + (s64)rowBytes * ah > (s64)size) {
        log_printf(LOG_ERR, "err: img truncated BMP path=%s", path);
        return 0;
    }
    if (bpp == 8) {
        u32 i, palOff = 14 + hsz;
        if (nPal == 0 || nPal > 256) nPal = 256;
        memset(pal, 0, sizeof(pal));
        for (i = 0; i < nPal && palOff + i * 4 + 4 <= (u32)size; i++) {
            const u8 *p = d + palOff + i * 4;
            pal[i] = 0xFF000000u | ((u32)p[2] << 16) | ((u32)p[1] << 8) | (u32)p[0];
        }
    }
    pix = (u32 *)malloc((size_t)w * (size_t)ah * 4);
    if (!pix) return 0;
    for (y = 0; y < ah; y++) {
        const u8 *src = d + off + (size_t)rowBytes * (size_t)(topDown ? y : ah - 1 - y);
        u32 *dst = pix + (size_t)y * (size_t)w;
        if (bpp == 24) {
            for (x = 0; x < w; x++, src += 3) dst[x] = 0xFF000000u | ((u32)src[2] << 16) | ((u32)src[1] << 8) | src[0];
        } else if (bpp == 32) {
            for (x = 0; x < w; x++, src += 4) dst[x] = 0xFF000000u | ((u32)src[2] << 16) | ((u32)src[1] << 8) | src[0];
        } else {
            for (x = 0; x < w; x++) dst[x] = pal[src[x]];
        }
    }
    *out = pix;
    *ow = (int)w;
    *oh = ah;
    return 1;
}

static int load_file(const char *path, u32 **pix, int *w, int *h, char *used, int usedN)
{
    char full[E_PATH];
    void *data;
    int size, ok;
    if (file_exists(path)) e_strlcpy(full, path, (int)sizeof(full));
    else data_path(full, (int)sizeof(full), path);
    if (used) e_strlcpy(used, full, usedN);
    data = file_load(full, &size);
    if (!data) return 0;
    ok = decode_bmp((const u8 *)data, size, pix, w, h, full);
    free(data);
    return ok;
}

void img_bleed(EImage *img)
{
    int w = img->w, h = img->h, n = w * h, head = 0, tail = 0, i;
    u32 *p = img->pix[0];
    u16 *dist;
    int *queue;
    if (!p || n <= 0) return;
    dist = (u16 *)malloc((size_t)n * sizeof(u16));
    queue = (int *)malloc((size_t)n * sizeof(int));
    if (!dist || !queue) { free(dist); free(queue); return; }
    for (i = 0; i < n; i++) {
        if (p[i] >> 24) { dist[i] = 0; queue[tail++] = i; }
        else dist[i] = 0xFFFF;
    }
    if (tail == 0 || tail == n) { free(dist); free(queue); return; }   /* nothing opaque / nothing to fill */
    /* BFS from the opaque texels (wrapping neighbours: most textures tile). Each texel gets the average colour of
       its neighbours from the previous BFS ring, keeping alpha 0. */
    while (head < tail) {
        int t = queue[head++], tx = t % w, ty = t / w, dx, dy, cnt = 0;
        u32 r = 0, g = 0, b = 0;
        u16 dt = dist[t];
        for (dy = -1; dy <= 1; dy++) for (dx = -1; dx <= 1; dx++) {
            int nx, ny, ni;
            if (!dx && !dy) continue;
            nx = tx + dx; ny = ty + dy;
            if (nx < 0) nx += w; else if (nx >= w) nx -= w;
            if (ny < 0) ny += h; else if (ny >= h) ny -= h;
            ni = ny * w + nx;
            if (dt > 0 && dist[ni] < dt) {
                u32 c = p[ni];
                r += (c >> 16) & 0xFF; g += (c >> 8) & 0xFF; b += c & 0xFF;
                cnt++;
            }
            if (dist[ni] == 0xFFFF) {
                dist[ni] = (u16)(dt < 0xFFFE ? dt + 1 : 0xFFFE);
                queue[tail++] = ni;
            }
        }
        if (dt > 0 && cnt > 0)
            p[t] = (p[t] & 0xFF000000u) | ((r / cnt) << 16) | ((g / cnt) << 8) | (b / cnt);
    }
    free(dist);
    free(queue);
}

void img_apply_key(EImage *img)
{
    int i, n = img->w * img->h;
    u32 *p = img->pix[0];
    if (!p) return;
    for (i = 0; i < n; i++) {
        u32 c = p[i], r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        if (r >= 250 && g <= 6 && b >= 250) p[i] = 0;     /* RGB(255,0,255) key (tolerant of rounding) */
    }
    img_bleed(img);
}

int img_load(EImage *img, const char *path, int flags)
{
    u32 *pix = NULL, *apix = NULL;
    int w, h, aw, ah;
    char used[E_PATH];
    memset(img, 0, sizeof(*img));
    if (!path || !load_file(path, &pix, &w, &h, used, (int)sizeof(used))) {
        log_printf(LOG_ERR, "err: img load failed path=%s", path ? path : "(null)");
        return 0;
    }
    img->w = w;
    img->h = h;
    img->levels = 1;
    img->flags = flags;
    img->pix[0] = pix;
    if (flags & IMG_ALPHA) {
        char ap[E_PATH];
        int n = (int)strlen(used);
        e_strlcpy(ap, used, (int)sizeof(ap));
        if (n > 4 && !e_stricmp(ap + n - 4, ".bmp")) ap[n - 4] = 0;
        n = (int)strlen(ap);
        e_strlcpy(ap + n, "_a.bmp", (int)sizeof(ap) - n);
        if (load_file(ap, &apix, &aw, &ah, NULL, 0) && aw == w && ah == h) {
            int i;
            for (i = 0; i < w * h; i++) pix[i] = (pix[i] & 0x00FFFFFFu) | (((apix[i] >> 16) & 0xFF) << 24);
            img_bleed(img);
        } else {
            log_printf(LOG_WARN, "warn: img alpha file missing or wrong size path=%s", ap);
        }
        if (apix) free(apix);
    }
    if (flags & IMG_KEYED) img_apply_key(img);
    log_printf(LOG_DEBUG, "img: load path=%s %dx%d flags=%d", used, w, h, flags);
    return 1;
}

void img_gen_mips(EImage *img)
{
    int l;
    for (l = img->levels; l < 12; l++) {
        int pw = img_level_w(img, l - 1), ph = img_level_h(img, l - 1), w, h, x, y;
        const u32 *src = img->pix[l - 1];
        u32 *dst;
        if (pw == 1 && ph == 1) break;
        w = pw > 1 ? pw / 2 : 1;
        h = ph > 1 ? ph / 2 : 1;
        dst = (u32 *)malloc((size_t)w * (size_t)h * 4);
        if (!dst) break;
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            int sx0 = x * 2, sy0 = y * 2, sx1 = sx0 + 1 < pw ? sx0 + 1 : sx0, sy1 = sy0 + 1 < ph ? sy0 + 1 : sy0;
            u32 c[4], sa = 0, sr = 0, sg = 0, sb = 0, ur = 0, ug = 0, ub = 0;
            int k;
            c[0] = src[sy0 * pw + sx0]; c[1] = src[sy0 * pw + sx1];
            c[2] = src[sy1 * pw + sx0]; c[3] = src[sy1 * pw + sx1];
            for (k = 0; k < 4; k++) {
                u32 a = c[k] >> 24, r = (c[k] >> 16) & 0xFF, g = (c[k] >> 8) & 0xFF, b = c[k] & 0xFF;
                sa += a; sr += r * a; sg += g * a; sb += b * a;
                ur += r; ug += g; ub += b;
            }
            if (sa > 0) { sr = (sr + sa / 2) / sa; sg = (sg + sa / 2) / sa; sb = (sb + sa / 2) / sa; }
            else { sr = (ur + 2) / 4; sg = (ug + 2) / 4; sb = (ub + 2) / 4; }
            dst[y * w + x] = (((sa + 2) / 4) << 24) | (sr << 16) | (sg << 8) | sb;
        }
        img->pix[l] = dst;
        img->levels = l + 1;
    }
}

int img_save_bmp24(const char *path, const u32 *argb, int w, int h)
{
    FILE *f;
    u8 hdr[54], *row;
    int rowBytes = (w * 3 + 3) & ~3, x, y, ok;
    u32 size = 54u + (u32)rowBytes * (u32)h;
    if (!path || !argb || w <= 0 || h <= 0) return 0;
    f = fopen(path, "wb");
    if (!f) {
        log_printf(LOG_ERR, "err: img save failed path=%s", path);
        return 0;
    }
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
#define W32(o, v) do { u32 v_ = (u32)(v); hdr[o] = (u8)v_; hdr[o + 1] = (u8)(v_ >> 8); hdr[o + 2] = (u8)(v_ >> 16); hdr[o + 3] = (u8)(v_ >> 24); } while (0)
    W32(2, size); W32(10, 54); W32(14, 40); W32(18, w); W32(22, h);
    hdr[26] = 1; hdr[28] = 24;
    W32(34, (u32)rowBytes * (u32)h); W32(38, 2835); W32(42, 2835);
#undef W32
    fwrite(hdr, 1, 54, f);
    row = (u8 *)calloc((size_t)rowBytes, 1);
    if (!row) { fclose(f); return 0; }
    for (y = h - 1; y >= 0; y--) {
        const u32 *s = argb + (size_t)y * (size_t)w;
        for (x = 0; x < w; x++) {
            row[x * 3] = (u8)s[x];
            row[x * 3 + 1] = (u8)(s[x] >> 8);
            row[x * 3 + 2] = (u8)(s[x] >> 16);
        }
        fwrite(row, 1, (size_t)rowBytes, f);
    }
    free(row);
    ok = !ferror(f);
    fclose(f);
    return ok;
}
