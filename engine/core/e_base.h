/* e_base.h - basic types, macros, deterministic RNG, hashes, small string helpers (DESIGN 2.5, 2.9).
   Portable: VC6 (/W3) and clang (-std=c++98 -Wall). Owner: T01 (append-only for others).
   Notes:
   - e_snprintf/e_vsnprintf are functions (not a bare _snprintf macro) so the output is always 0-terminated,
     also on VC6 where _snprintf leaves the buffer unterminated on overflow. Return value = chars written,
     or -1 if truncated (both platforms).
   - e_ftoi rounds to nearest with the current x87 rounding mode (round-to-nearest-even after plat_fpu_setup);
     the portable fallback rounds halves away from zero. Use it for render/UI conversions, not where exact
     .5 ties matter. */
#ifndef E_BASE_H
#define E_BASE_H

#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

typedef unsigned char u8;   typedef signed char s8;
typedef unsigned short u16; typedef short s16;
typedef unsigned int u32;   typedef int s32;
#if defined(_MSC_VER)
typedef __int64 s64; typedef unsigned __int64 u64;
#else
typedef long long s64; typedef unsigned long long u64;
#endif

#define E_PI          3.14159265f
#define E_DEG2RAD(d)  ((d) * (E_PI / 180.0f))
#define E_RAD2DEG(r)  ((r) * (180.0f / E_PI))
#define E_CLAMP(x,a,b) ((x) < (a) ? (a) : ((x) > (b) ? (b) : (x)))
#define E_MINF(a,b)   ((a) < (b) ? (a) : (b))      /* never name anything min/max (windows.h macros) */
#define E_MAXF(a,b)   ((a) > (b) ? (a) : (b))
#define E_COUNT(a)    ((int)(sizeof(a) / sizeof((a)[0])))

/* deterministic RNG (xorshift32); the only RNG in sim code */
typedef struct ERand { u32 s; } ERand;
void  rnd_seed(ERand *r, u32 seed);
u32   rnd_u32(ERand *r);
float rnd_f01(ERand *r);                           /* [0,1) */
float rnd_range(ERand *r, float a, float b);       /* [a,b) */

#define E_HASH_INIT 2166136261u                    /* FNV-1a offset basis: start value for e_hash chains */
u32   e_hash(const void *p, int n, u32 h);         /* FNV-1a, chainable; state hashes for determinism tests */
u32   e_crc32(const void *p, int n);               /* IEEE 802.3 (zlib) CRC32; "123456789" -> 0xCBF43926 */

/* ---- additions (T01) ---- */
enum { E_PATH = 260 };                             /* path buffer size (= MAX_PATH) */
#define E_TAG(a,b,c,d) ((u32)(u8)(a) | ((u32)(u8)(b) << 8) | ((u32)(u8)(c) << 16) | ((u32)(u8)(d) << 24))

int   e_snprintf(char *buf, int n, const char *fmt, ...);        /* always 0-terminated (n > 0) */
int   e_vsnprintf(char *buf, int n, const char *fmt, va_list ap);
int   e_stricmp(const char *a, const char *b);                    /* ASCII case-insensitive compare */
void  e_strlcpy(char *dst, const char *src, int n);               /* copy, always 0-terminated (n > 0) */

inline int e_ftoi(float f)                         /* round to nearest; VC6 avoids the slow _ftol */
{
#if defined(_MSC_VER) && defined(_M_IX86)
    int i;
    __asm {
        fld f
        fistp i
    }
    return i;
#else
    return (int)(f >= 0.0f ? f + 0.5f : f - 0.5f);
#endif
}

#endif
