/* bench.h - -bench N -benchroute R [-benchview cockpit|chase]: autopilot routes, report bench.txt (DESIGN 16.4).
   Windows side. Owner: T08 (routes + report), T09 (gate tuning), T17 (route combat).
   Routes (the player jet flies them on a terrain-following autopilot through session_set_autopilot, NORMAL thrust):
     low      60 m: back and forth along the runway line, 3.5 km inland to 9 km out to sea (>= 50 m AGL)
     volcano  2300 m, back and forth across the crater (12 km legs, the volcano ahead most of the time)
     high     8000 m, back and forth over the island centre (14 km each side)
     clouds   2900 m (cumulus band 1800-4000 m), back and forth N-S over the island (14 km each side)
   Timing: 3 s warm-up (real time), then N s measured; prof + r_pace statistics; bench.txt next to the exe =
   header lines (route, preset, resolution, pacing, scene counters, gates 15 / 30 fps) + prof_write_report. */
#ifndef BENCH_H
#define BENCH_H

#include "game.h"

int   bench_init(int secs, int route);              /* after session_start: places the jet, starts the autopilot */
void  bench_frame(void);                            /* after every rendered frame (after prof_frame) */
int   bench_done(void);
int   bench_write(const char *path);

/* ---- T08 ---- */
const char *bench_route_name(int route);
int   bench_active(void);

#endif
