/* e_file.cpp - see e_file.h. Portable. Owner: T01. */
#include "e_file.h"
#include "e_log.h"

#if defined(_WIN32)
#define SEP '\\'
#define OTHER_SEP '/'
#else
#define SEP '/'
#define OTHER_SEP '\\'
#endif

static char s_base[E_PATH];

static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

void *file_load(const char *path, int *size)
{
    FILE *f;
    long len;
    char *buf;
    if (size) *size = 0;
    if (!path) return NULL;
    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); log_printf(LOG_ERR, "err: file out of memory path=%s size=%ld", path, len); return NULL; }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        log_printf(LOG_ERR, "err: file read failed path=%s", path);
        return NULL;
    }
    fclose(f);
    buf[len] = 0;
    if (size) *size = (int)len;
    return buf;
}

int file_save(const char *path, const void *p, int size)
{
    FILE *f;
    int ok;
    if (!path || size < 0) return 0;
    f = fopen(path, "wb");
    if (!f) return 0;
    ok = size == 0 || fwrite(p, 1, (size_t)size, f) == (size_t)size;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

int file_exists(const char *path)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return 0;
    fclose(f);
    return 1;
}

int chunk_open(ChunkFile *cf, const void *data, int size, u32 tag)
{
    const u8 *d = (const u8 *)data;
    u32 crc, want;
    memset(cf, 0, sizeof(*cf));
    if (!d || size < 16) { log_printf(LOG_ERR, "err: chunk file too small size=%d", size); return 0; }
    if (rd32(d) != tag) {
        log_printf(LOG_ERR, "err: chunk file bad tag got=%.4s want=%c%c%c%c", (const char *)d,
                   (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF), (char)((tag >> 16) & 0xFF), (char)(tag >> 24));
        return 0;
    }
    want = rd32(d + 12);
    crc = e_crc32(d + 16, size - 16);
    if (crc != want) { log_printf(LOG_ERR, "err: chunk file crc mismatch got=%08x want=%08x", crc, want); return 0; }
    cf->data = d;
    cf->size = size;
    cf->tag = tag;
    cf->version = rd32(d + 4);
    return 1;
}

int chunk_count(const ChunkFile *cf) { return cf->data ? (int)rd32(cf->data + 8) : 0; }

const u8 *chunk_at(const ChunkFile *cf, int idx, char id[4], int *size)
{
    const u8 *p, *end;
    u32 sz;
    int i = 0;
    if (size) *size = 0;
    if (!cf->data) return NULL;
    p = cf->data + 16;
    end = cf->data + cf->size;
    while (p + 8 <= end) {
        sz = rd32(p + 4);
        if (sz > (u32)(end - p - 8)) {
            log_printf(LOG_ERR, "err: chunk overruns file id=%.4s size=%u", (const char *)p, sz);
            return NULL;
        }
        if (i == idx) {
            if (id) memcpy(id, p, 4);
            if (size) *size = (int)sz;
            return p + 8;
        }
        i++;
        p += 8 + ((sz + 3u) & ~3u);
    }
    return NULL;
}

const u8 *chunk_find(const ChunkFile *cf, const char id[4], int *size)
{
    char got[4];
    int i, sz;
    const u8 *p;
    if (size) *size = 0;
    for (i = 0; (p = chunk_at(cf, i, got, &sz)) != NULL; i++) {
        if (!memcmp(got, id, 4)) {
            if (size) *size = sz;
            return p;
        }
    }
    return NULL;
}

static int is_abs(const char *p)
{
    return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
}

void path_join(char *out, int n, const char *dir, const char *rel)
{
    int i, len;
    if (n <= 0) return;
    if (!rel) rel = "";
    if (!dir || !dir[0] || is_abs(rel)) {
        e_strlcpy(out, rel, n);
    } else {
        len = (int)strlen(dir);
        if (dir[len - 1] == '/' || dir[len - 1] == '\\') e_snprintf(out, n, "%s%s", dir, rel);
        else e_snprintf(out, n, "%s%c%s", dir, SEP, rel);
    }
    for (i = 0; out[i]; i++) if (out[i] == OTHER_SEP) out[i] = SEP;
}

void file_set_base_dir(const char *dir) { e_strlcpy(s_base, dir ? dir : "", (int)sizeof(s_base)); }
const char *file_base_dir(void) { return s_base; }

void base_path(char *out, int n, const char *rel) { path_join(out, n, s_base, rel); }

void data_path(char *out, int n, const char *rel)
{
    char d[E_PATH];
    path_join(d, (int)sizeof(d), s_base, "data");
    path_join(out, n, d, rel);
}
