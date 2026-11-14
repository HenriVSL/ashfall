/* e_base.cpp - RNG, hashes, string helpers (DESIGN 2.5). Portable. Owner: T01. */
#include "e_base.h"

/* ---- RNG: xorshift32 (Marsaglia 13/17/5). Seed is mixed so that small seeds differ quickly. */
void rnd_seed(ERand *r, u32 seed)
{
    u32 s = seed * 2654435761u + 0x6D2B79F5u;
    s ^= s >> 15;
    if (s == 0) s = 1;
    r->s = s;
}

u32 rnd_u32(ERand *r)
{
    u32 x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

float rnd_f01(ERand *r)
{
    return (float)(rnd_u32(r) >> 8) * (1.0f / 16777216.0f);
}

float rnd_range(ERand *r, float a, float b)
{
    return a + (b - a) * rnd_f01(r);
}

/* ---- hashes */
u32 e_hash(const void *p, int n, u32 h)
{
    const u8 *b = (const u8 *)p;
    int i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

static u32 s_crcTab[256];
static int s_crcInit;

u32 e_crc32(const void *p, int n)
{
    const u8 *b = (const u8 *)p;
    u32 c = 0xFFFFFFFFu;
    int i, k;
    if (!s_crcInit) {
        for (i = 0; i < 256; i++) {
            u32 v = (u32)i;
            for (k = 0; k < 8; k++) v = (v & 1) ? (0xEDB88320u ^ (v >> 1)) : (v >> 1);
            s_crcTab[i] = v;
        }
        s_crcInit = 1;
    }
    for (i = 0; i < n; i++) c = s_crcTab[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- strings */
int e_vsnprintf(char *buf, int n, const char *fmt, va_list ap)
{
    int r;
    if (n <= 0) return -1;
#if defined(_MSC_VER)
    r = _vsnprintf(buf, (size_t)n, fmt, ap);
    buf[n - 1] = 0;
    if (r < 0 || r >= n) return -1;
#else
    r = vsnprintf(buf, (size_t)n, fmt, ap);
    if (r < 0 || r >= n) return -1;
#endif
    return r;
}

int e_snprintf(char *buf, int n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = e_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int e_stricmp(const char *a, const char *b)
{
    int ca, cb;
    for (;;) {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb || !ca) return ca - cb;
    }
}

void e_strlcpy(char *dst, const char *src, int n)
{
    int i;
    if (n <= 0) return;
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}
