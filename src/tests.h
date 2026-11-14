/* tests.h - -test NAME dispatcher and check helpers (DESIGN 16.1). Portable (jet.exe and tools/simh).
   Owner: T01 (dispatcher, math, params). Each area test lives in its own file owned by its task
   (tests_<area>.cpp; T01 created stubs returning TEST_NOT_IMPL so the table below links from day one).
   A test function returns its number of failures, or TEST_NOT_IMPL (-1). tests_run returns the same, and -1 for an
   unknown name. Output: one "test: <name> <what> ok|FAIL [details]" line per check and a closing
   "test: <name> done checks=N fail=F" line (LOG_INFO; -test forces at least -log 1).
   Windows-only tests (input, audio, hud) are registered by main.cpp with tests_register(..., 0).
   "all" runs every portable test that is implemented and sums the failures. */
#ifndef TESTS_H
#define TESTS_H

#include "game.h"

enum { TEST_NOT_IMPL = -1 };
typedef int (*TestFn)(const char *arg);
int   tests_run(const char *name, const char *arg);
int   tests_register(const char *name, TestFn fn, int portable);   /* replaces a same-name entry; 0 if full */
void  tests_list(void);                            /* "test: list name=... portable=..." lines */

/* checks: log one line, count it; return ok */
void  t_begin(const char *test);                   /* resets the counters, sets the name used in lines */
int   t_check(int ok, const char *what, const char *fmt, ...);     /* fmt may be NULL: no details */
int   t_near(float got, float want, float tol, const char *what);  /* |got - want| <= tol */
int   t_end(void);                                 /* logs the done line, returns failures */

/* test functions (portable); file (owner) */
int   test_math(const char *arg);                  /* tests.cpp (T01) */
int   test_params(const char *arg);                /* tests.cpp (T01) */
int   test_flight(const char *arg);                /* tests_flight.cpp (T05) */
int   test_pilot(const char *arg);                 /* tests_pilot.cpp (T05) */
int   test_takeoff(const char *arg);               /* tests_takeoff.cpp (T06) */
int   test_world(const char *arg);                 /* tests_world.cpp (T06) */
int   test_terrain(const char *arg);               /* tests_terrain.cpp (T04) */
int   test_radar(const char *arg);                 /* tests_radar.cpp (T11) */
int   test_target(const char *arg);                /* tests_target.cpp (T11) */
int   test_missile(const char *arg);               /* tests_missile.cpp (T12) */
int   test_dlz(const char *arg);                   /* tests_dlz.cpp (T13) */
int   test_ai(const char *arg);                    /* tests_ai.cpp (T14) */
int   test_mission(const char *arg);               /* tests_mission.cpp (T17/T20) */
int   test_determinism(const char *arg);           /* replay.cpp (T08) */
int   test_replay(const char *arg);                /* replay.cpp (T01/T08) */

/* simh commands provided by area files (tools/simh/simh.cpp dispatches; stubs return TEST_NOT_IMPL) */
int   feel_card_print(void);                       /* tests_flight.cpp (T05): simh feel */
int   simh_shots(int argc, char **argv);           /* tests_dlz.cpp (T13): simh shots N */
int   simh_dlztable(int argc, char **argv);        /* tests_dlz.cpp (T13): simh dlztable */
int   simh_aifight(int argc, char **argv);         /* tests_ai.cpp (T14): simh aifight N seed diff */
int   simh_island(int argc, char **argv);          /* tests_terrain.cpp (T04): simh island */

#endif
