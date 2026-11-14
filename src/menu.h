/* menu.h - frontend: title, FLY, OPTIONS (controls with key capture, video, sound), RECORDS, pause menu, results
   screen, loading screen, -mscript menu flows (DESIGN 14.5). Windows side. Owner: T19 (stub by T01).

   App states (game.h APP_*), driven by main.cpp:
     APP_TITLE    the frontend: logo over the island fly-by (main draws the scene with menu_backdrop_camera) and the
                  screens MAIN (FLY / OPTIONS / RECORDS / QUIT), FLY (FREE FLIGHT / TRAINING / SORTIE + DIFFICULTY),
                  OPTIONS (tabs CONTROLS / VIDEO / SOUND), RECORDS. menu_tick returns APP_LOADING when a flight is
                  chosen (g_args.mode / g_args.diff are set; start = runway) or APP_QUIT.
     APP_FLIGHT   paused: menu_pause_tick / menu_pause_draw (RESUME, RESTART ON RUNWAY, RESTART MISSION (not FREE),
                  OPTIONS, QUIT TO MENU); OPTIONS from the pause is the same options screen over the frozen flight.
     APP_RESULTS  after a SORTIE ends (main freezes the sim there): MISSION COMPLETE / FAILED with time, kills,
                  hits / fired per missile type, deaths, the score breakdown (DESIGN 14.4), SCORE, BEST per
                  difficulty, NEW RECORD; CONTINUE (-> APP_TITLE) / FLY AGAIN (-> APP_LOADING, same mode + diff).
   Settings: every change is applied at once (params_restore on the live param, in_bind for keys) and written to the
   ini file main loaded (menu_set_ini) with a line-based updater that keeps every comment, blank line, key order and
   unknown key (CRLF). [video] keys (preset, resolution, display) apply at the next start. While a -mscript runs, the
   settings go to t19_jet.ini (copied from the running ini first) so tests never touch the player's jet.ini.
   Records: T20's API (mission.h): records_load/records_get for the RECORDS screen, mission_results + records_submit
   once when the results screen opens (never in tests/headless; a -mscript uses t19_records.ini).
   Look: A10 menu kit data/tex/front_<480|600|768|1050> (9-slice panel, 3-slice title / bar, keycaps, tabs, chevron,
   arrows, volume segments) + logo tile sets (lo / hi, freed while flying) + the small logo, HUD fonts for text,
   everything premultiplied (RB_PREMUL), laid out in the virtual 640x480 space (k = h / 480, centred horizontally). */
#ifndef MENU_H
#define MENU_H

#include "game.h"
#include "camera.h"

int   menu_init(void);                              /* after r_init + hfont_init: kit, small logo, ui sounds, modes */
void  menu_free(void);
int   menu_tick(int app);                           /* APP_TITLE / APP_RESULTS: returns the next APP_* state */
void  menu_draw(int app);                           /* 2D layer of APP_TITLE / APP_RESULTS (scene drawn by main) */

/* ---- added by T19 ---- */
enum { MP_STAY, MP_RESUME, MP_RUNWAY, MP_RESTART_MISSION, MP_QUIT_MENU };
enum { MK_UP, MK_DOWN, MK_LEFT, MK_RIGHT, MK_OK, MK_BACK, MK_DEL, MK_PAUSE, MK_COUNT };   /* menu keys (-mscript) */

void  menu_set_ini(struct Cfg *ini, const char *iniPath);   /* before menu_init: main's Cfg + the file it came from */
void  menu_input_defaults(void);                    /* call after the game's default bindings, before in_bind_load:
                                                       remembers them for DEL = default in the controls list */
void  menu_load_controls(const char *path);         /* [controls] read from the file itself (Cfg keeps 1024 lines
                                                       and drops the tail of the long generated jet.ini) */
void  menu_enter(int app);                          /* main switched to APP_TITLE (main screen, logo tiles loaded),
                                                       APP_RESULTS (snapshot of mission_stats) or APP_FLIGHT (logo
                                                       tiles freed) */
void  menu_flight_started(int mode, int diff);      /* start of every flight: the best score before it (NEW RECORD) */
void  menu_backdrop_camera(Camera *c, float timeSec);   /* title fly-by: slow orbit around the volcano */
void  menu_draw_loading(const char *text);          /* one presented frame pair: logo + text (island load) */
void  menu_pause_open(int mode);                    /* pause opened in a flight of this MODE_* */
int   menu_pause_tick(int pauseHit);                /* while paused; pauseHit = the pause action (Esc/P): MP_* */
void  menu_pause_draw(void);
int   menu_params_dirty(void);                      /* 1 once after a setting changed (main: replay PARAMS record) */
const char *menu_screen_name(int app, int paused);  /* "title" "fly" "options" "records" "pause" "results" "flight"
                                                       "loading" (logs, -mscript) */
int   menu_records_best(int diff);                  /* best SORTIE score for DIFF_* (records_get), or -1 (none yet) */

/* -mscript FILE (menu flows; command list in menu.cpp "menu script"). main: menu_script_load after menu_init,
   menu_script_tick once per sim tick before the app logic (1 = quit), menu_script_hit(MK_PAUSE) for the pause in a
   flight, menu_script_fly_keys to OR the script's held flight keys into the live keys, menu_script_robot (the
   robot flies the player in combat modes), menu_script_take_shot after a rendered frame, menu_script_finish at exit
   (failure count = exit code). */
int   menu_script_load(const char *path);
int   menu_script_active(void);
int   menu_script_tick(int app, int paused);
int   menu_script_hit(int mk);
void  menu_script_fly_keys(PilotKeys *k);
int   menu_script_robot(void);
int   menu_script_take_shot(char *path, int n);
int   menu_script_finish(void);
void  menu_script_note_fps(float ms);               /* main: last frame time (ms), for "expect fps" on the title */

#endif
