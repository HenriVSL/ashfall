/* e_prof.h - timing sections, counters, frame-interval histogram, bench report (DESIGN 2.5, 9.3). Portable (the
   clock comes in through prof_init). Owner: T01.
   Sections: prof_begin/prof_end add the elapsed time to the section's per-frame sum; prof_frame closes a frame
   (per-section frame sums -> average/worst, frame interval -> histogram). The same id can also take counts
   (prof_count adds to its per-frame count: tris, draws...). Ids are 0..PROF_MAX-1; prof_section returns the
   existing id for a name that is already registered, -1 if the table is full (begin/end/count ignore -1).
   Report lines (bench.txt, after the caller's header text):
     bench: frames=N secs=S fps_avg=F fps_min1s=F
     bench: frame_ms avg=A p50=.. p95=.. p99=.. worst=..
     bench: hist_periods 1=N 2=N ... 8+=N        (only when prof_set_period_us was called: refresh periods)
     bench: hist_ms 0-4=N 5-9=N ...                (5 ms buckets, non-empty only)
     bench: section name=X avg_ms=A worst_ms=W    (per frame)
     bench: count name=X per_frame=A max=M */
#ifndef E_PROF_H
#define E_PROF_H

#include "e_base.h"

enum { PROF_MAX = 24 };
void prof_init(s64 (*now_us)(void));               /* plat_now_us in the exe, a clock() shim in physh */
int  prof_section(const char *name);               /* register once, returns id */
void prof_begin(int id); void prof_end(int id);
void prof_frame(void);                              /* end of a rendered frame: histograms */
void prof_count(int id, int n);                     /* counters (tris, draws...) */
int  prof_write_report(const char *path, const char *header); /* bench.txt (9.3) */

/* ---- additions (T01) ---- */
void  prof_reset(void);                             /* clear statistics (end of a bench warm-up); keeps sections */
void  prof_set_period_us(s64 us);                   /* refresh period for the hist_periods line (0 = off) */
float prof_last_frame_ms(void);                     /* interval of the last closed frame (FPS overlay) */
float prof_avg_frame_ms(void);
int   prof_frames(void);
float prof_section_avg_ms(int id);                  /* average per frame so far */
s64   prof_now_us(void);                            /* the clock given to prof_init (0 before) */

#endif
