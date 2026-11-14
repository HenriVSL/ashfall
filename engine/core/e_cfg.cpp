/* e_cfg.cpp - see e_cfg.h for the ini rules. Portable. Owner: T01. */
#include "e_cfg.h"
#include "e_log.h"
#include "e_file.h"

enum { CFG_MAX_ENT = 2048, CFG_MAX_SEC = 64, CFG_KEYLEN = 48, CFG_VALLEN = 256, CFG_SECLEN = 48 };

typedef struct CfgEnt {
    s16 sec;                                       /* section index */
    s16 isKey;                                     /* 0 = raw line (comment/blank/garbage) kept in val */
    char key[CFG_KEYLEN];
    char val[CFG_VALLEN];
} CfgEnt;

struct Cfg {
    int nSec;
    char sec[CFG_MAX_SEC][CFG_SECLEN];
    int nEnt;
    CfgEnt ent[CFG_MAX_ENT];
};

static Cfg *cfg_new(void)
{
    Cfg *c = (Cfg *)malloc(sizeof(Cfg));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->nSec = 1;                                   /* section 0 = "" (before the first header) */
    return c;
}

static int find_sec(const Cfg *c, const char *sec)
{
    int i;
    if (!sec) sec = "";
    for (i = 0; i < c->nSec; i++) if (!e_stricmp(c->sec[i], sec)) return i;
    return -1;
}

static int add_sec(Cfg *c, const char *sec)
{
    int i = find_sec(c, sec);
    if (i >= 0) return i;
    if (c->nSec >= CFG_MAX_SEC) { log_printf(LOG_WARN, "warn: cfg too many sections sec=%s", sec); return -1; }
    e_strlcpy(c->sec[c->nSec], sec, CFG_SECLEN);
    return c->nSec++;
}

static int find_ent(const Cfg *c, const char *sec, const char *key)
{
    int s = find_sec(c, sec), i;
    if (s < 0 || !key) return -1;
    for (i = 0; i < c->nEnt; i++)
        if (c->ent[i].sec == s && c->ent[i].isKey && !e_stricmp(c->ent[i].key, key)) return i;
    return -1;
}

static CfgEnt *insert_ent(Cfg *c, int s, int isKey, int afterLastKey)
{
    int pos = -1, lastAny = -1, i;
    if (c->nEnt >= CFG_MAX_ENT) { log_printf(LOG_WARN, "warn: cfg full lines=%d", c->nEnt); return NULL; }
    for (i = 0; i < c->nEnt; i++) {
        if (c->ent[i].sec != s) continue;
        lastAny = i;
        if (c->ent[i].isKey) pos = i;
    }
    if (!afterLastKey || pos < 0) pos = lastAny;   /* parser: append in file order; cfg_set: after the last key */
    pos = pos < 0 ? c->nEnt : pos + 1;
    if (pos < c->nEnt) memmove(&c->ent[pos + 1], &c->ent[pos], (size_t)(c->nEnt - pos) * sizeof(CfgEnt));
    c->nEnt++;
    memset(&c->ent[pos], 0, sizeof(CfgEnt));
    c->ent[pos].sec = (s16)s;
    c->ent[pos].isKey = (s16)isKey;
    return &c->ent[pos];
}

static void set_val(CfgEnt *e, const char *v)
{
    if ((int)strlen(v) >= CFG_VALLEN) log_printf(LOG_WARN, "warn: cfg value truncated key=%s", e->key);
    e_strlcpy(e->val, v, CFG_VALLEN);
}

static int is_space(int ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

/* copies [b, e) trimmed into out */
static void copy_trim(char *out, int n, const char *b, const char *e)
{
    int len;
    while (b < e && is_space((unsigned char)*b)) b++;
    while (e > b && is_space((unsigned char)e[-1])) e--;
    len = (int)(e - b);
    if (len > n - 1) len = n - 1;
    memcpy(out, b, (size_t)len);
    out[len] = 0;
}

Cfg *cfg_parse(const char *text, int len)
{
    Cfg *c = cfg_new();
    const char *p = text, *end = text + (len > 0 ? len : 0), *eol, *b, *eq;
    int cur = 0;
    char key[CFG_KEYLEN], val[CFG_VALLEN * 2], name[CFG_SECLEN];
    CfgEnt *e;
    if (!c) return NULL;
    if (!text) return c;
    while (p < end) {
        eol = p;
        while (eol < end && *eol != '\n') eol++;
        b = p;
        while (b < eol && (*b == ' ' || *b == '\t')) b++;
        if (b < eol && *b == '[') {
            const char *cl = b + 1;
            while (cl < eol && *cl != ']') cl++;
            copy_trim(name, CFG_SECLEN, b + 1, cl);
            { int ns = add_sec(c, name); if (ns >= 0) cur = ns; }
        } else {
            eq = b;
            while (eq < eol && *eq != '=') eq++;
            if (b < eol && *b != ';' && *b != '#' && eq < eol && eq > b) {
                copy_trim(key, CFG_KEYLEN, b, eq);
                copy_trim(val, (int)sizeof(val), eq + 1, eol);
                {
                    int i = find_ent(c, c->sec[cur], key);
                    e = i >= 0 ? &c->ent[i] : insert_ent(c, cur, 1, 0);
                }
                if (e) { e_strlcpy(e->key, key, CFG_KEYLEN); set_val(e, val); }
            } else {
                e = insert_ent(c, cur, 0, 0);    /* comment, blank or unparsable line: kept verbatim */
                if (e) copy_trim(e->val, CFG_VALLEN, p, eol);   /* trims CR and indentation */
            }
        }
        p = eol < end ? eol + 1 : end;
    }
    return c;
}

Cfg *cfg_load(const char *path)
{
    int size = 0;
    char *data = path ? (char *)file_load(path, &size) : NULL;
    Cfg *c = cfg_parse(data, data ? size : 0);
    if (data) free(data);
    if (!c) {
        log_printf(LOG_ERR, "err: cfg out of memory path=%s", path ? path : "");
        c = cfg_new();
    }
    return c;
}

int cfg_save(const Cfg *c, const char *path)
{
    FILE *f;
    int s, i, ok;
    if (!c || !path) return 0;
    f = fopen(path, "wb");
    if (!f) { log_printf(LOG_WARN, "warn: cfg save failed path=%s", path); return 0; }
    for (s = 0; s < c->nSec; s++) {
        int any = 0;
        for (i = 0; i < c->nEnt; i++) if (c->ent[i].sec == s) { any = 1; break; }
        if (s > 0) fprintf(f, "[%s]\r\n", c->sec[s]);
        else if (!any) continue;
        for (i = 0; i < c->nEnt; i++) {
            const CfgEnt *e = &c->ent[i];
            if (e->sec != s) continue;
            if (e->isKey) fprintf(f, "%s=%s\r\n", e->key, e->val);
            else fprintf(f, "%s\r\n", e->val);
        }
    }
    ok = !ferror(f);
    if (fclose(f) != 0) ok = 0;
    return ok;
}

void cfg_free(Cfg *c) { if (c) free(c); }

int e_parse_int(const char *s, int *out)
{
    char *end;
    long v;
    if (!s) return 0;
    while (is_space((unsigned char)*s)) s++;
    if (!*s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) v = (long)strtoul(s, &end, 16);
    else if ((s[0] == '-' || s[0] == '+') && s[1] == '0' && (s[2] == 'x' || s[2] == 'X')) {
        v = (long)strtoul(s + 1, &end, 16);
        if (s[0] == '-') v = -v;
    } else v = strtol(s, &end, 10);
    if (end == s) return 0;
    while (is_space((unsigned char)*end)) end++;
    if (*end) return 0;
    *out = (int)v;
    return 1;
}

int e_parse_float(const char *s, float *out)
{
    char *end;
    double v;
    if (!s) return 0;
    while (is_space((unsigned char)*s)) s++;
    if (!*s) return 0;
    v = strtod(s, &end);
    if (end == s) return 0;
    while (is_space((unsigned char)*end)) end++;
    if (*end) return 0;
    *out = (float)v;
    return 1;
}

int cfg_has(const Cfg *c, const char *sec, const char *key) { return c && find_ent(c, sec, key) >= 0; }

const char *cfg_str(const Cfg *c, const char *sec, const char *key, const char *def)
{
    int i = c ? find_ent(c, sec, key) : -1;
    return i >= 0 ? c->ent[i].val : def;
}

int cfg_int(const Cfg *c, const char *sec, const char *key, int def)
{
    int v;
    const char *s = cfg_str(c, sec, key, NULL);
    if (!s) return def;
    if (!e_parse_int(s, &v)) {
        log_printf(LOG_WARN, "warn: cfg not an int sec=%s key=%s value=%s", sec ? sec : "", key, s);
        return def;
    }
    return v;
}

float cfg_float(const Cfg *c, const char *sec, const char *key, float def)
{
    float v;
    const char *s = cfg_str(c, sec, key, NULL);
    if (!s) return def;
    if (!e_parse_float(s, &v)) {
        log_printf(LOG_WARN, "warn: cfg not a number sec=%s key=%s value=%s", sec ? sec : "", key, s);
        return def;
    }
    return v;
}

int cfg_floats(const Cfg *c, const char *sec, const char *key, float *out, int max)
{
    const char *s = cfg_str(c, sec, key, NULL), *p;
    char *end;
    int n = 0;
    if (!s) return 0;
    p = s;
    while (*p && n < max) {
        /* skip separators (comma, space, colon, semicolon, tab ...) up to the next number */
        while (*p && !((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.')) p++;
        if (!*p) break;
        {
            double v = strtod(p, &end);
            if (end == p) { p++; continue; }
            out[n++] = (float)v;
            p = end;
        }
    }
    return n;
}

void cfg_set_str(Cfg *c, const char *sec, const char *key, const char *v)
{
    int i, s;
    CfgEnt *e;
    if (!c || !key) return;
    if (!sec) sec = "";
    if (!v) v = "";
    i = find_ent(c, sec, key);
    if (i >= 0) { set_val(&c->ent[i], v); return; }
    s = add_sec(c, sec);
    if (s < 0) return;
    e = insert_ent(c, s, 1, 1);
    if (!e) return;
    e_strlcpy(e->key, key, CFG_KEYLEN);
    set_val(e, v);
}

void cfg_set_int(Cfg *c, const char *sec, const char *key, int v)
{
    char b[32];
    e_snprintf(b, (int)sizeof(b), "%d", v);
    cfg_set_str(c, sec, key, b);
}

void cfg_set_float(Cfg *c, const char *sec, const char *key, float v)
{
    char b[48];
    e_snprintf(b, (int)sizeof(b), "%g", (double)v);
    cfg_set_str(c, sec, key, b);
}

int cfg_num_keys(const Cfg *c)
{
    int i, n = 0;
    if (!c) return 0;
    for (i = 0; i < c->nEnt; i++) if (c->ent[i].isKey) n++;
    return n;
}

const char *cfg_key_at(const Cfg *c, const char *sec, int idx, const char **val)
{
    int s, i, n = 0;
    if (!c) return NULL;
    s = find_sec(c, sec);
    if (s < 0) return NULL;
    for (i = 0; i < c->nEnt; i++) {
        if (c->ent[i].sec != s || !c->ent[i].isKey) continue;
        if (n++ == idx) {
            if (val) *val = c->ent[i].val;
            return c->ent[i].key;
        }
    }
    return NULL;
}

/* ---- reload (jet T01) */
static int cfg_diff_into(const Cfg *a, const Cfg *b, int logIt)
{
    /* keys in b that are new or changed vs a, plus keys of a missing in b */
    int i, n = 0;
    for (i = 0; i < b->nEnt; i++) {
        const CfgEnt *e = &b->ent[i];
        int j;
        if (!e->isKey) continue;
        j = find_ent(a, b->sec[e->sec], e->key);
        if (j >= 0 && !strcmp(a->ent[j].val, e->val)) continue;
        n++;
        if (logIt) log_printf(LOG_INFO, "cfg: changed [%s] %s old=%s new=%s", b->sec[e->sec], e->key,
                              j >= 0 ? a->ent[j].val : "(none)", e->val);
    }
    for (i = 0; i < a->nEnt; i++) {
        const CfgEnt *e = &a->ent[i];
        if (!e->isKey || find_ent(b, a->sec[e->sec], e->key) >= 0) continue;
        n++;
        if (logIt) log_printf(LOG_INFO, "cfg: changed [%s] %s old=%s new=(none)", a->sec[e->sec], e->key, e->val);
    }
    return n;
}

int cfg_reload(Cfg *c, const char *path)
{
    int size = 0, n;
    char *data;
    Cfg *fresh;
    if (!c || !path) return -1;
    data = (char *)file_load(path, &size);
    if (!data) {
        log_printf(LOG_WARN, "warn: cfg reload failed path=%s", path);
        return -1;
    }
    fresh = cfg_parse(data, size);
    free(data);
    if (!fresh) return -1;
    n = cfg_diff_into(c, fresh, 1);
    memcpy(c, fresh, sizeof(Cfg));
    cfg_free(fresh);
    log_printf(LOG_INFO, "cfg: reload path=%s changed=%d", path, n);
    return n;
}

/* ---- command-line arguments */
enum { ARG_MAX = 64, ARG_BUF = 2048 };
static char  s_argBuf[ARG_BUF];
static char *s_argv[ARG_MAX];
static int   s_argc;

int args_split(char *p, char **argv, int max)
{
    int n = 0;
    if (!p) return 0;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        if (*p == '"') {
            argv[n++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            argv[n++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        }
        if (*p) *p++ = 0;
    }
    return n;
}

void args_init(int argc, char **argv)
{
    int i, used = 0, len;
    s_argc = 0;
    for (i = 0; i < argc && s_argc < ARG_MAX; i++) {
        len = (int)strlen(argv[i]) + 1;
        if (used + len > ARG_BUF) break;
        memcpy(s_argBuf + used, argv[i], (size_t)len);
        s_argv[s_argc++] = s_argBuf + used;
        used += len;
    }
}

static int arg_find(const char *sw, int occ)
{
    int i;
    for (i = 1; i < s_argc; i++)
        if (!e_stricmp(s_argv[i], sw) && occ-- == 0) return i;
    return -1;
}

int arg_has(const char *sw) { return arg_find(sw, 0) >= 0; }

int arg_count(const char *sw)
{
    int i, n = 0;
    for (i = 1; i < s_argc; i++) if (!e_stricmp(s_argv[i], sw)) n++;
    return n;
}

const char *arg_nth(const char *sw, int occ, int idx)
{
    int i = arg_find(sw, occ);
    if (i < 0 || idx < 0 || i + 1 + idx >= s_argc) return NULL;
    return s_argv[i + 1 + idx];
}

int arg_int(const char *sw, int idx, int def)
{
    int v;
    return e_parse_int(arg_nth(sw, 0, idx), &v) ? v : def;
}

float arg_float(const char *sw, int idx, float def)
{
    float v;
    return e_parse_float(arg_nth(sw, 0, idx), &v) ? v : def;
}

const char *arg_str(const char *sw, int idx, const char *def)
{
    const char *s = arg_nth(sw, 0, idx);
    return s ? s : def;
}

int arg_argc(void) { return s_argc; }
const char *arg_argv(int i) { return (i >= 0 && i < s_argc) ? s_argv[i] : NULL; }

int arg_unknown(const char *const *known, int nKnown, char *out, int n)
{
    int i, k, bad = 0, used = 0;
    float f;
    if (n > 0) out[0] = 0;
    for (i = 1; i < s_argc; i++) {
        const char *a = s_argv[i];
        if (a[0] != '-' || e_parse_float(a, &f)) continue;   /* values, negative numbers */
        for (k = 0; k < nKnown; k++) if (!e_stricmp(a, known[k])) break;
        if (k < nKnown) continue;
        bad++;
        if (used < n - 1) {
            int w = e_snprintf(out + used, n - used, "%s%s", used ? " " : "", a);
            used = w < 0 ? n - 1 : used + w;
        }
    }
    return bad;
}
