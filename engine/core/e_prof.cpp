/* e_prof.cpp - see e_prof.h. Portable. Owner: T01. */
#include "e_prof.h"

enum { HIST_N = 1000, HIST_US = 250 };              /* 0.25 ms buckets up to 250 ms; the last one is open */

typedef struct ProfSec {
    char name[24];
    s64 start, frameSum, total, worst;
    s64 frameCount, countTotal, countMax;
    int open;
} ProfSec;

static s64 (*s_now)(void);
static ProfSec s_sec[PROF_MAX];
static int s_nSec;
static s64 s_lastFrame, s_firstFrame, s_winStart, s_periodUs, s_sumUs, s_worstUs;
static int s_frames, s_winFrames, s_haveLast;
static float s_minFps1s, s_lastMs;
static int s_hist[HIST_N];

void prof_init(s64 (*now_us)(void))
{
    s_now = now_us;
    s_nSec = 0;
    memset(s_sec, 0, sizeof(s_sec));
    prof_reset();
}

s64 prof_now_us(void) { return s_now ? s_now() : 0; }

void prof_reset(void)
{
    int i;
    for (i = 0; i < s_nSec; i++) {
        s_sec[i].frameSum = s_sec[i].total = s_sec[i].worst = 0;
        s_sec[i].frameCount = s_sec[i].countTotal = s_sec[i].countMax = 0;
        s_sec[i].open = 0;
    }
    memset(s_hist, 0, sizeof(s_hist));
    s_frames = s_winFrames = s_haveLast = 0;
    s_sumUs = s_worstUs = 0;
    s_minFps1s = 0.0f;
    s_lastMs = 0.0f;
    s_lastFrame = s_firstFrame = s_winStart = prof_now_us();
}

void prof_set_period_us(s64 us) { s_periodUs = us; }

int prof_section(const char *name)
{
    int i;
    for (i = 0; i < s_nSec; i++) if (!strcmp(s_sec[i].name, name)) return i;
    if (s_nSec >= PROF_MAX) return -1;
    memset(&s_sec[s_nSec], 0, sizeof(ProfSec));
    e_strlcpy(s_sec[s_nSec].name, name, (int)sizeof(s_sec[0].name));
    return s_nSec++;
}

void prof_begin(int id)
{
    if (id < 0 || id >= s_nSec) return;
    s_sec[id].start = prof_now_us();
    s_sec[id].open = 1;
}

void prof_end(int id)
{
    if (id < 0 || id >= s_nSec || !s_sec[id].open) return;
    s_sec[id].frameSum += prof_now_us() - s_sec[id].start;
    s_sec[id].open = 0;
}

void prof_count(int id, int n)
{
    if (id < 0 || id >= s_nSec) return;
    s_sec[id].frameCount += n;
}

void prof_frame(void)
{
    s64 now = prof_now_us(), dt;
    int i, b;
    for (i = 0; i < s_nSec; i++) {
        ProfSec *s = &s_sec[i];
        s->total += s->frameSum;
        if (s->frameSum > s->worst) s->worst = s->frameSum;
        s->countTotal += s->frameCount;
        if (s->frameCount > s->countMax) s->countMax = s->frameCount;
        s->frameSum = 0;
        s->frameCount = 0;
    }
    if (!s_haveLast) {                              /* first frame only starts the clock */
        s_haveLast = 1;
        s_lastFrame = s_firstFrame = s_winStart = now;
        s_winFrames = 0;
        return;
    }
    dt = now - s_lastFrame;
    s_lastFrame = now;
    s_frames++;
    s_sumUs += dt;
    if (dt > s_worstUs) s_worstUs = dt;
    s_lastMs = (float)dt * 0.001f;
    b = (int)(dt / HIST_US);
    if (b < 0) b = 0;
    if (b >= HIST_N) b = HIST_N - 1;
    s_hist[b]++;
    s_winFrames++;
    if (now - s_winStart >= 1000000) {
        float fps = (float)s_winFrames * 1000000.0f / (float)(now - s_winStart);
        if (s_minFps1s == 0.0f || fps < s_minFps1s) s_minFps1s = fps;
        s_winStart = now;
        s_winFrames = 0;
    }
}

float prof_last_frame_ms(void) { return s_lastMs; }
float prof_avg_frame_ms(void) { return s_frames ? (float)s_sumUs * 0.001f / (float)s_frames : 0.0f; }
int   prof_frames(void) { return s_frames; }

float prof_section_avg_ms(int id)
{
    int n = s_frames + (s_haveLast ? 1 : 0);
    if (id < 0 || id >= s_nSec || n == 0) return 0.0f;
    return (float)s_sec[id].total * 0.001f / (float)n;
}

static float pct_ms(float p)
{
    int i, acc = 0, want = (int)((float)s_frames * p + 0.5f);
    if (want < 1) want = 1;
    for (i = 0; i < HIST_N; i++) {
        acc += s_hist[i];
        if (acc >= want) return (float)(i + 1) * (float)HIST_US * 0.001f;   /* bucket upper edge */
    }
    return (float)s_worstUs * 0.001f;
}

int prof_write_report(const char *path, const char *header)
{
    FILE *f = fopen(path, "w");
    int i, n = s_frames + (s_haveLast ? 1 : 0);
    float secs = (float)s_sumUs * 1e-6f;
    if (!f) return 0;
    if (header && header[0]) {
        fputs(header, f);
        if (header[strlen(header) - 1] != '\n') fputc('\n', f);
    }
    fprintf(f, "bench: frames=%d secs=%.2f fps_avg=%.2f fps_min1s=%.2f\n", s_frames, (double)secs,
            secs > 0.0f ? (double)((float)s_frames / secs) : 0.0, (double)s_minFps1s);
    fprintf(f, "bench: frame_ms avg=%.2f p50=%.2f p95=%.2f p99=%.2f worst=%.2f\n", (double)prof_avg_frame_ms(),
            (double)pct_ms(0.50f), (double)pct_ms(0.95f), (double)pct_ms(0.99f), (double)s_worstUs * 0.001);
    if (s_periodUs > 0) {
        int per[9];
        memset(per, 0, sizeof(per));
        for (i = 0; i < HIST_N; i++) {
            s64 mid = (s64)i * HIST_US + HIST_US / 2;
            int k = (int)((mid + s_periodUs / 2) / s_periodUs);
            if (k < 1) k = 1;
            if (k > 8) k = 8;
            per[k] += s_hist[i];
        }
        fprintf(f, "bench: hist_periods period_us=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d 8+=%d\n", (int)s_periodUs,
                per[1], per[2], per[3], per[4], per[5], per[6], per[7], per[8]);
    }
    {
        int b5[51], k;
        memset(b5, 0, sizeof(b5));
        for (i = 0; i < HIST_N; i++) {
            k = (i * HIST_US) / 5000;
            if (k > 50) k = 50;
            b5[k] += s_hist[i];
        }
        fprintf(f, "bench: hist_ms");
        for (k = 0; k < 51; k++) {
            if (!b5[k]) continue;
            if (k < 50) fprintf(f, " %d-%d=%d", k * 5, k * 5 + 4, b5[k]);
            else fprintf(f, " 250+=%d", b5[k]);
        }
        fprintf(f, "\n");
    }
    for (i = 0; i < s_nSec; i++) {
        const ProfSec *s = &s_sec[i];
        if (s->total > 0 || s->worst > 0)
            fprintf(f, "bench: section name=%s avg_ms=%.3f worst_ms=%.3f\n", s->name,
                    n ? (double)s->total * 0.001 / (double)n : 0.0, (double)s->worst * 0.001);
        if (s->countTotal > 0)
            fprintf(f, "bench: count name=%s per_frame=%.1f max=%d\n", s->name,
                    n ? (double)s->countTotal / (double)n : 0.0, (int)s->countMax);
    }
    i = !ferror(f);
    fclose(f);
    return i;
}
