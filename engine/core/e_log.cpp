/* e_log.cpp - see e_log.h. Portable. Owner: T01. */
#include "e_log.h"

static FILE *s_f;
static int   s_level = LOG_ERR;
static int   s_isStdout;
static int   s_auto = 1;
static int   s_opened;                           /* log_open was called */
static int   s_failed;                           /* fopen failed once: don't retry per line */
static char  s_path[E_PATH];
static int   s_count[4];

static int ensure_open(void)
{
    if (s_f) return 1;
    if (!s_opened || s_failed) return 0;
    s_f = fopen(s_path, "w");
    if (!s_f) { s_failed = 1; return 0; }
    return 1;
}

int log_open(const char *path, int level)
{
    log_close();
    s_level = level < LOG_ERR ? LOG_ERR : level;
    s_opened = 1;
    s_failed = 0;
    if (!path || !strcmp(path, "-")) {
        s_f = stdout;
        s_isStdout = 1;
        return 1;
    }
    e_strlcpy(s_path, path, (int)sizeof(s_path));
    if (s_level > LOG_ERR) return ensure_open();   /* LOG_ERR only: create on the first error */
    return 1;
}

void log_close(void)
{
    if (s_f && !s_isStdout) fclose(s_f);
    else if (s_f) fflush(s_f);
    s_f = NULL;
    s_isStdout = 0;
    s_opened = 0;
}

void log_printf(int level, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    int n;
    if (level > s_level) return;
    if (!ensure_open()) return;
    va_start(ap, fmt);
    n = e_vsnprintf(buf, (int)sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(buf);             /* truncated: keep what fits */
    if (n == 0 || buf[n - 1] != '\n') { buf[n++] = '\n'; buf[n] = 0; }
    fputs(buf, s_f);
    if (level >= LOG_ERR && level <= LOG_DEBUG) s_count[level]++;
    if (s_auto || level <= LOG_WARN) fflush(s_f);
}

void log_flush(void) { if (s_f) fflush(s_f); }
int  log_level(void) { return s_level; }
void log_set_autoflush(int on) { s_auto = on; }
int  log_lines(int level) { return (level >= 0 && level <= 3) ? s_count[level] : 0; }
