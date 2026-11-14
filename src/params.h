/* params.h - ini-backed tunable registry, live reload (F5), -dumpini writer, replay snapshots (DESIGN 2.5).
   Portable. Owner: T01.

   Each module declares its tunables in a static table and registers it once from its xxx_params() function:
       static float s_turnBoost; static PTable s_rollTab;
       static const Param s_tab[] = {
           { "flight", "turnBoost", P_FLOAT, &s_turnBoost, "1.0", 0.2f, 3.0f, "multiplies the G limit" },
           { "flight", "rollRateTable", P_TABLE, &s_rollTab, "150:110,250:180,400:220,700:200", 0, 720,
             "max roll rate (deg/s) vs true airspeed (kt)" },
       };
       void flight_params(void) { params_add(s_tab, E_COUNT(s_tab)); }
   Registration order (params_register_all + main's Windows modules) = order in -dumpini. A module's storage holds
   the default right after params_add, so code works even without params_load.
   Types: P_FLOAT (float*), P_INT (int*), P_BOOL (int*, 0/1), P_TABLE (PTable*, "x:y,x:y" piecewise linear, up to
   PTABLE_MAX points, x strictly increasing; lo/hi clamp the y values), P_STR (char[PARAM_STR], lo/hi unused).
   lo/hi clamp numbers (lo == hi = no clamp). Bad values are clamped or replaced by the default and logged
   ("warn: params ..."), never fatal. flags: P_RESTART = read at start only (F5 logs "restart needed" and keeps the
   running value), e.g. [video].
   Live reload (F5): params_reload(cfg, path) = cfg_reload + params_load; one line per changed key:
       params: reload flight.rollRateTable old=150:110,... new=150:120,...
   and a closing "params: reload changed=N". Units belong in the key name or the help text. */
#ifndef PARAMS_H
#define PARAMS_H

#include "e_base.h"
#include "e_cfg.h"

enum { P_FLOAT, P_INT, P_BOOL, P_TABLE, P_STR };
enum { P_RESTART = 1 };
enum { PTABLE_MAX = 12, PARAM_STR = 32, PARAMS_MAX = 768 };   /* T17: 512 -> 768 (~560 keys in M2) */

typedef struct PTable { int n; float x[PTABLE_MAX], y[PTABLE_MAX]; } PTable;
typedef struct Param {
    const char *sec, *key;
    int type;
    void *ptr;
    const char *def;                                /* default as ini text */
    float lo, hi;
    const char *help;
    int flags;                                      /* P_RESTART; may be left out of the initializer (0) */
} Param;

int   params_add(const Param *tab, int n);          /* registers + sets defaults; 0 if the registry is full */
void  params_register_all(void);                    /* every PORTABLE module's xxx_params() (params.cpp); main adds
                                                       the Windows modules (camera, hud, snd, scene) after it */
void  params_clear(void);                           /* tests: forget every registration */
int   params_count(void);
int   params_load(const Cfg *c);                    /* ini values (missing key = default); returns changed count */
int   params_reload(Cfg *c, const char *path);      /* F5: cfg_reload + apply live keys + change log; -1 = no file */
int   params_dump_ini(const char *path, const Cfg *extra);   /* defaults + help comments, CRLF; extra = more
                                                       sections appended verbatim (e.g. [controls]), may be NULL */
int   params_snapshot(char *buf, int n);            /* "sec.key=value\n" for every key (current values); returns
                                                       length, or -1 if n is too small */
int   params_restore(const char *text, int len);    /* applies a snapshot (replays); returns keys applied */
u32   params_hash(void);                            /* FNV of the snapshot text (replay header / tests) */
const Param *params_find(const char *sec, const char *key);
int   params_value_str(const Param *p, char *out, int n);    /* current value as ini text */
int   params_reload_count(void);                    /* F5 reloads so far (HUD "INI RELOADED (n)" uses the last N) */
int   params_last_changed(void);                    /* keys changed by the last reload */

float ptable_eval(const PTable *t, float x);        /* piecewise linear, clamped at both ends; empty table = 0 */
int   ptable_parse(const char *s, PTable *out);     /* 1 = ok ("x:y,x:y", increasing x) */
int   ptable_str(const PTable *t, char *out, int n);

/* [dev] (params.cpp): dev.keys (F9/F10 without -dev), dev.testValue (F5 reload check, -test params) */
int   params_dev_keys(void);
float params_dev_test_value(void);

#endif
