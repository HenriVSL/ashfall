/* params.cpp - see params.h. Portable. Owner: T01. */
#include "params.h"
#include "e_log.h"
#include "game.h"
#include "pilot.h"
#include "flight.h"
#include "takeoff.h"
#include "world.h"
#include "radar.h"
#include "target.h"
#include "missile.h"
#include "weapons.h"
#include "dlz.h"
#include "ai.h"
#include "mission.h"

static Param s_reg[PARAMS_MAX];
static int s_num;
static int s_reloads, s_lastChanged;

/* ---------------------------------------------------------------- [dev] */
static int s_devKeys;
static float s_devTest;
static const Param s_devTab[] = {
    { "dev", "keys", P_BOOL, &s_devKeys, "0", 0.0f, 1.0f, "1 = dev keys F9 (air respawn) and F10 (feel overlay) without -dev" },
    { "dev", "testValue", P_FLOAT, &s_devTest, "1.0", -1000.0f, 1000.0f,
      "does nothing; edit it and press F5 to check live reload (params: reload dev.testValue ...)" },
};
int params_dev_keys(void) { return s_devKeys; }
float params_dev_test_value(void) { return s_devTest; }

void params_register_all(void)
{
    game_params();                                  /* [video] (restart only) */
    pilot_params();
    flight_params();
    takeoff_params();
    world_params();
    radar_params();
    target_params();
    missile_params();                               /* [lrm] [srm] */
    weapons_params();
    dlz_params();
    ai_params();
    mission_params();
    params_add(s_devTab, E_COUNT(s_devTab));
}

/* ---------------------------------------------------------------- tables */
float ptable_eval(const PTable *t, float x)
{
    int i;
    float f;
    if (!t || t->n <= 0) return 0.0f;
    if (x <= t->x[0]) return t->y[0];
    if (x >= t->x[t->n - 1]) return t->y[t->n - 1];
    for (i = 1; i < t->n; i++)
        if (x < t->x[i]) {
            f = (x - t->x[i - 1]) / (t->x[i] - t->x[i - 1]);
            return t->y[i - 1] + (t->y[i] - t->y[i - 1]) * f;
        }
    return t->y[t->n - 1];
}

int ptable_parse(const char *s, PTable *out)
{
    PTable t;
    char *end;
    double v;
    memset(&t, 0, sizeof(t));
    if (!s) return 0;
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == ',') s++;
        if (!*s) break;
        if (t.n >= PTABLE_MAX) return 0;
        v = strtod(s, &end);
        if (end == s) return 0;
        t.x[t.n] = (float)v;
        s = end;
        while (*s == ' ' || *s == '\t') s++;
        if (*s != ':') return 0;
        s++;
        v = strtod(s, &end);
        if (end == s) return 0;
        t.y[t.n] = (float)v;
        s = end;
        if (t.n > 0 && !(t.x[t.n] > t.x[t.n - 1])) return 0;
        t.n++;
        while (*s == ' ' || *s == '\t') s++;
        if (*s && *s != ',') return 0;
    }
    if (t.n == 0) return 0;
    *out = t;
    return 1;
}

int ptable_str(const PTable *t, char *out, int n)
{
    int i, used = 0, w;
    if (n <= 0) return 0;
    out[0] = 0;
    for (i = 0; i < t->n; i++) {
        w = e_snprintf(out + used, n - used, "%s%g:%g", i ? "," : "", (double)t->x[i], (double)t->y[i]);
        if (w < 0) return used;
        used += w;
    }
    return used;
}

/* ---------------------------------------------------------------- registry */
const Param *params_find(const char *sec, const char *key)
{
    int i;
    for (i = 0; i < s_num; i++)
        if (!e_stricmp(s_reg[i].sec, sec) && !e_stricmp(s_reg[i].key, key)) return &s_reg[i];
    return NULL;
}

int params_value_str(const Param *p, char *out, int n)
{
    if (!p || n <= 0) return 0;
    switch (p->type) {
    case P_FLOAT: return e_snprintf(out, n, "%g", (double)*(float *)p->ptr);
    case P_INT:   return e_snprintf(out, n, "%d", *(int *)p->ptr);
    case P_BOOL:  return e_snprintf(out, n, "%d", *(int *)p->ptr ? 1 : 0);
    case P_TABLE: return ptable_str((const PTable *)p->ptr, out, n);
    case P_STR:   e_strlcpy(out, (const char *)p->ptr, n); return (int)strlen(out);
    }
    out[0] = 0;
    return 0;
}

/* parses text into p's storage (clamped). Returns 1 ok, 0 = bad (storage untouched), 2 = ok but clamped. */
static int apply_text(const Param *p, const char *text)
{
    int clampOn = p->hi > p->lo, iv, i, clamped = 0;
    float fv;
    PTable t;
    switch (p->type) {
    case P_FLOAT:
        if (!e_parse_float(text, &fv)) return 0;
        if (clampOn && (fv < p->lo || fv > p->hi)) { fv = E_CLAMP(fv, p->lo, p->hi); clamped = 1; }
        *(float *)p->ptr = fv;
        break;
    case P_INT:
        if (!e_parse_int(text, &iv)) return 0;
        if (clampOn && ((float)iv < p->lo || (float)iv > p->hi)) {
            iv = (float)iv < p->lo ? (int)p->lo : (int)p->hi;
            clamped = 1;
        }
        *(int *)p->ptr = iv;
        break;
    case P_BOOL:
        if (!e_parse_int(text, &iv)) return 0;
        if (iv != 0 && iv != 1) clamped = 1;
        *(int *)p->ptr = iv ? 1 : 0;
        break;
    case P_TABLE:
        if (!ptable_parse(text, &t)) return 0;
        if (clampOn)
            for (i = 0; i < t.n; i++)
                if (t.y[i] < p->lo || t.y[i] > p->hi) { t.y[i] = E_CLAMP(t.y[i], p->lo, p->hi); clamped = 1; }
        *(PTable *)p->ptr = t;
        break;
    case P_STR:
        if ((int)strlen(text) >= PARAM_STR) clamped = 1;
        e_strlcpy((char *)p->ptr, text, PARAM_STR);
        break;
    default:
        return 0;
    }
    return clamped ? 2 : 1;
}

static void set_default(const Param *p)
{
    if (!apply_text(p, p->def ? p->def : ""))
        log_printf(LOG_ERR, "err: params bad default %s.%s=%s", p->sec, p->key, p->def ? p->def : "");
}

int params_add(const Param *tab, int n)
{
    int i, ok = 1;
    for (i = 0; i < n; i++) {
        Param *slot = (Param *)params_find(tab[i].sec, tab[i].key);
        if (!slot) {
            if (s_num >= PARAMS_MAX) {
                log_printf(LOG_ERR, "err: params registry full key=%s.%s", tab[i].sec, tab[i].key);
                ok = 0;
                continue;
            }
            slot = &s_reg[s_num++];
        }
        *slot = tab[i];
        set_default(slot);
    }
    return ok;
}

void params_clear(void) { s_num = 0; }
int params_count(void) { return s_num; }
int params_reload_count(void) { return s_reloads; }
int params_last_changed(void) { return s_lastChanged; }

/* mode 0 = start (no per-key log), 1 = reload (log changes, keep P_RESTART keys), 2 = restore (log changes) */
static int load_one(const Param *p, const char *text, int mode)
{
    char old[256], now[256];
    int r, changed;
    params_value_str(p, old, (int)sizeof(old));
    if (mode == 1 && (p->flags & P_RESTART)) {
        /* restart-only key: compare the ini text with the running value, never apply */
        if (strcmp(old, text)) {
            log_printf(LOG_INFO, "params: reload %s.%s old=%s new=%s (restart needed, not applied)", p->sec, p->key,
                       old, text);
        }
        return 0;
    }
    r = apply_text(p, text);
    if (!r) {
        log_printf(LOG_WARN, "warn: params bad value %s.%s=%s (keeping %s)", p->sec, p->key, text, old);
        return 0;
    }
    params_value_str(p, now, (int)sizeof(now));
    if (r == 2) log_printf(LOG_WARN, "warn: params clamped %s.%s=%s -> %s", p->sec, p->key, text, now);
    changed = strcmp(old, now) != 0;
    if (changed && mode == 1) log_printf(LOG_INFO, "params: reload %s.%s old=%s new=%s", p->sec, p->key, old, now);
    if (changed && mode == 2) log_printf(LOG_INFO, "params: restore %s.%s old=%s new=%s", p->sec, p->key, old, now);
    return changed;
}

static int load_all(const Cfg *c, int mode)
{
    int i, n = 0, fromIni = 0;
    char def[256];
    for (i = 0; i < s_num; i++) {
        const Param *p = &s_reg[i];
        const char *v = cfg_str(c, p->sec, p->key, NULL);
        if (v) fromIni++;
        else {
            e_strlcpy(def, p->def ? p->def : "", (int)sizeof(def));
            v = def;                                /* key removed from the ini: back to the default */
        }
        n += load_one(p, v, mode);
    }
    if (mode == 0) log_printf(LOG_INFO, "params: load keys=%d from_ini=%d changed=%d", s_num, fromIni, n);
    return n;
}

int params_load(const Cfg *c) { return load_all(c, 0); }

int params_reload(Cfg *c, const char *path)
{
    int r = cfg_reload(c, path), n;
    if (r < 0) {
        log_printf(LOG_WARN, "warn: params reload failed path=%s", path ? path : "");
        return -1;
    }
    n = load_all(c, 1);
    s_reloads++;
    s_lastChanged = n;
    log_printf(LOG_INFO, "params: reload changed=%d cfg_changed=%d count=%d", n, r, s_reloads);
    return n;
}

int params_dump_ini(const char *path, const Cfg *extra)
{
    FILE *f;
    int i, j, ok;
    const char *lastSec = NULL;
    if (!path) return 0;
    f = fopen(path, "wb");
    if (!f) { log_printf(LOG_WARN, "warn: params dumpini cannot write path=%s", path); return 0; }
    fprintf(f, "; jet.ini - every tunable with its default (generated by jet.exe -dumpini, DESIGN 2.5).\r\n");
    fprintf(f, "; Edit a value and press F5 in the game to reload it live ([video] needs a restart).\r\n");
    fprintf(f, "; Bad values are clamped to the range shown and logged in jet.log; a removed key = its default.\r\n");
    for (i = 0; i < s_num; i++) {
        const Param *p = &s_reg[i];
        /* group by section in first-registration order: write each section once with all its keys */
        int seen = 0;
        for (j = 0; j < i; j++) if (!e_stricmp(s_reg[j].sec, p->sec)) { seen = 1; break; }
        if (seen) continue;
        fprintf(f, "\r\n[%s]\r\n", p->sec);
        lastSec = p->sec;
        for (j = i; j < s_num; j++) {
            const Param *q = &s_reg[j];
            char rng[64];
            if (e_stricmp(q->sec, lastSec)) continue;
            rng[0] = 0;
            if (q->type == P_BOOL) e_snprintf(rng, (int)sizeof(rng), " (0/1)");
            else if (q->type != P_STR && q->hi > q->lo)
                e_snprintf(rng, (int)sizeof(rng), " (%s%g..%g)", q->type == P_TABLE ? "x:y, y " : "", (double)q->lo,
                           (double)q->hi);
            else if (q->type == P_TABLE) e_snprintf(rng, (int)sizeof(rng), " (x:y,x:y)");
            fprintf(f, "; %s%s%s\r\n", q->help ? q->help : "", rng, (q->flags & P_RESTART) ? " [restart]" : "");
            fprintf(f, "%s=%s\r\n", q->key, q->def ? q->def : "");
        }
    }
    if (extra) {
        /* extra sections (keys only; Cfg has no section iterator, so the known non-param sections are listed) */
        static const char *const extraSecs[] = { "controls" };
        const char *key, *val = NULL;
        int s, idx;
        for (s = 0; s < E_COUNT(extraSecs); s++) {
            if (!cfg_key_at(extra, extraSecs[s], 0, &val)) continue;
            fprintf(f, "\r\n[%s]\r\n", extraSecs[s]);
            idx = 0;
            while ((key = cfg_key_at(extra, extraSecs[s], idx++, &val)) != NULL) fprintf(f, "%s=%s\r\n", key, val);
        }
    }
    ok = !ferror(f);
    if (fclose(f) != 0) ok = 0;
    log_printf(LOG_INFO, "params: dumpini path=%s keys=%d ok=%d", path, s_num, ok);
    return ok;
}

int params_snapshot(char *buf, int n)
{
    int i, used = 0, w;
    char v[256];
    if (n <= 0) return -1;
    buf[0] = 0;
    for (i = 0; i < s_num; i++) {
        params_value_str(&s_reg[i], v, (int)sizeof(v));
        w = e_snprintf(buf + used, n - used, "%s.%s=%s\n", s_reg[i].sec, s_reg[i].key, v);
        if (w < 0) return -1;
        used += w;
    }
    return used;
}

int params_restore(const char *text, int len)
{
    const char *p = text, *end = text + (len > 0 ? len : 0), *eol, *dot, *eq;
    char sec[48], key[48], val[256];
    int n = 0, l;
    const Param *pr;
    if (!text) return 0;
    while (p < end) {
        eol = p;
        while (eol < end && *eol != '\n') eol++;
        dot = p;
        while (dot < eol && *dot != '.') dot++;
        eq = dot;
        while (eq < eol && *eq != '=') eq++;
        if (dot < eol && eq < eol) {
            l = (int)(dot - p); if (l > 47) l = 47; memcpy(sec, p, (size_t)l); sec[l] = 0;
            l = (int)(eq - dot - 1); if (l > 47) l = 47; memcpy(key, dot + 1, (size_t)l); key[l] = 0;
            l = (int)(eol - eq - 1); if (l > 255) l = 255; memcpy(val, eq + 1, (size_t)l); val[l] = 0;
            if (l > 0 && val[l - 1] == '\r') val[l - 1] = 0;
            pr = params_find(sec, key);
            if (pr) { load_one(pr, val, 2); n++; }
            else log_printf(LOG_WARN, "warn: params restore unknown key=%s.%s", sec, key);
        }
        p = eol < end ? eol + 1 : end;
    }
    return n;
}

u32 params_hash(void)
{
    int i;
    u32 h = E_HASH_INIT;
    char v[256];
    for (i = 0; i < s_num; i++) {
        params_value_str(&s_reg[i], v, (int)sizeof(v));
        h = e_hash(s_reg[i].sec, (int)strlen(s_reg[i].sec), h);
        h = e_hash(s_reg[i].key, (int)strlen(s_reg[i].key), h);
        h = e_hash(v, (int)strlen(v), h);
    }
    return h;
}
