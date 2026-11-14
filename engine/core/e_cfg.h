/* e_cfg.h - own ini parser/writer + command-line arguments (DESIGN 2.5). Portable. Owner: T01.
   Ini rules: [section] lines, key=value lines, whole-line comments starting with ';' or '#', blank lines;
   CRLF or LF. Keys and sections match case-insensitively; spaces around keys/values are trimmed. Text before the
   first [section] belongs to section "" (sec = "" or NULL). No inline comments: everything after '=' is the value.
   A duplicate key keeps its first position and its last value. cfg_save writes CRLF and keeps the order of
   sections, keys, comments and blank lines; new keys go after the last key of their section, new sections at
   the end. Limits: 1024 lines (keys + comments + blank lines, so >= 512 keys), 64 sections, key 47 chars,
   value 255 chars (longer is truncated with a warn: line).
   Args: argv[0] is the program name and is skipped (C convention). Switches compare case-insensitively.
   arg_int/arg_float return def when the value token is missing or not a complete number (so "-log -window"
   gives the default for -log, while "-camat -10 2 3" reads -10). Values are counted from 0 after the switch. */
#ifndef E_CFG_H
#define E_CFG_H

#include "e_base.h"

typedef struct Cfg Cfg;                            /* opaque; up to 512 keys, 32 sections */
Cfg  *cfg_load(const char *path);                  /* missing file -> empty cfg, never NULL */
int   cfg_save(const Cfg *c, const char *path);
void  cfg_free(Cfg *c);
int   cfg_int(const Cfg *c, const char *sec, const char *key, int def);
float cfg_float(const Cfg *c, const char *sec, const char *key, float def);
const char *cfg_str(const Cfg *c, const char *sec, const char *key, const char *def);
int   cfg_floats(const Cfg *c, const char *sec, const char *key, float *out, int max); /* "1, 2.5, 3" */
void  cfg_set_int(Cfg *c, const char *sec, const char *key, int v);
void  cfg_set_float(Cfg *c, const char *sec, const char *key, float v);
void  cfg_set_str(Cfg *c, const char *sec, const char *key, const char *v);
void  args_init(int argc, char **argv);            /* main.cpp splits lpCmdLine (quotes honoured) */
int   arg_has(const char *sw);                     /* "-log" */
int   arg_int(const char *sw, int idx, int def);   /* value idx after the switch: "-shot 3 a.bmp" */
float arg_float(const char *sw, int idx, float def);
const char *arg_str(const char *sw, int idx, const char *def);

/* ---- additions (T01) ---- */
Cfg  *cfg_parse(const char *text, int len);        /* same as cfg_load from a memory buffer */
int   cfg_has(const Cfg *c, const char *sec, const char *key);
int   cfg_num_keys(const Cfg *c);                  /* key lines (not comments) */
/* iterate keys of one section: returns the key name of the idx-th key (0-based) or NULL; *val gets the value */
const char *cfg_key_at(const Cfg *c, const char *sec, int idx, const char **val);
int   args_split(char *cmdline, char **argv, int max);   /* in-place split; "quoted args" kept whole; returns count */
int   arg_count(const char *sw);                   /* occurrences of a repeatable switch ("-shot") */
const char *arg_nth(const char *sw, int occ, int idx);   /* value idx after the occ-th occurrence, NULL if absent */
int   arg_argc(void); const char *arg_argv(int i); /* raw access (i = 0 is the program name) */
int   arg_unknown(const char *const *known, int nKnown, char *out, int n);  /* list switches not in known[] */
int   e_parse_int(const char *s, int *out);        /* 1 if the whole string (spaces trimmed) is an int (dec or 0x hex) */
int   e_parse_float(const char *s, float *out);    /* 1 if the whole string is a number */

/* ---- additions (jet T01) ---- */
/* Re-read the file into c (in place, so pointers to c stay valid). Returns the number of keys whose value changed,
   appeared or disappeared (each logged as "cfg: changed [sec] key old=... new=..." at LOG_INFO), or -1 if the file
   could not be read (c is left unchanged). Keys are compared case-insensitively, values exactly (trimmed). */
int   cfg_reload(Cfg *c, const char *path);

#endif
