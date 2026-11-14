/* e_log.h - jet.log: levels, one grep-friendly fact per line ("tag: key=value ...", DESIGN 9.1). Portable.
   Owner: T01. log_open(NULL or "-", level) writes to stdout (physh). With level LOG_ERR (no -log switch) the file
   is only created when the first error is written. A missing '\n' at the end of a line is added. */
#ifndef E_LOG_H
#define E_LOG_H

#include "e_base.h"

enum { LOG_ERR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 };
int  log_open(const char *path, int level);       /* level from -log [N]; errors are always logged */
void log_close(void);
void log_printf(int level, const char *fmt, ...); /* one fact per line: "tag: key=value key=value" */
void log_flush(void);
int  log_level(void);
#define LOGI log_printf          /* usage: LOGI(LOG_INFO, "race: start mode=%d laps=%d", m, l); */

/* ---- additions (T01) ---- */
void log_set_autoflush(int on);                    /* 1 (default): fflush after every line; main turns it off in the loop */
int  log_lines(int level);                         /* lines written so far at exactly this level (tests) */

#endif
