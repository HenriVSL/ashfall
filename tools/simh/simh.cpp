/* simh.cpp - Mac-side headless harness: links every portable file (DESIGN 2.8, 16.3). Owner: T01; other tasks add
   ONE dispatch line each for their command (T05 feel, T13 shots/dlztable, T14 aifight, T04 island) and report it.
   Output = the same log lines as jet.exe, on stdout. Run from the jet folder (data paths are relative to the
   current directory): make -C tools/simh && tools/simh/simh test math
   Commands:
     simh test <name> [arg]       self-test (tests.cpp), exit code = failures (255 = unknown / not implemented)
     simh dumpini <file>          portable params with defaults (jet.exe -dumpini also has the Windows modules)
     simh feel                    feel card (T05)
     simh shots N ...             launch-zone honesty Monte Carlo (T13)
     simh dlztable                AI launch-zone tables (T13)
     simh aifight N seed diff     robot fights (T14)
     simh island                  island file checks (T04)
   Options (anywhere): -log N (1 info default, 2 debug), -ini FILE (default jet.ini). */
#include "e_base.h"
#include "e_log.h"
#include "e_cfg.h"
#include "e_file.h"
#include "e_prof.h"
#include "game.h"
#include "params.h"
#include "tests.h"
#include <time.h>

static s64 clock_us(void) { return (s64)clock() * 1000000 / (s64)CLOCKS_PER_SEC; }

static int exit_code(int r) { return r < 0 ? 255 : (r > 125 ? 125 : r); }

static void usage(void)
{
    printf("usage: simh test <name> [arg] | dumpini <file> | feel | shots N | dlztable | aifight N seed diff | island"
           "  [-log N] [-ini FILE]\n");
}

int main(int argc, char **argv)
{
    const char *cmd;
    int r = TEST_NOT_IMPL, level;
    Cfg *ini;
    args_init(argc, argv);
    level = arg_has("-log") ? arg_int("-log", 0, 1) + 1 : LOG_INFO;
    log_open("-", level);
    prof_init(clock_us);
    file_set_base_dir("");
    params_register_all();
    ini = cfg_load(arg_str("-ini", 0, "jet.ini"));
    params_load(ini);
    args_parse(&g_args);
    if (argc < 2) { usage(); cfg_free(ini); return 2; }
    cmd = argv[1];
    if (!strcmp(cmd, "test") && argc >= 3) {
        r = tests_run(argv[2], argc > 3 && argv[3][0] != '-' ? argv[3] : NULL);
        log_printf(LOG_INFO, "simh: test=%s result=%d", argv[2], r);
    } else if (!strcmp(cmd, "dumpini") && argc >= 3) {
        r = params_dump_ini(argv[2], NULL) ? 0 : 1;
    } else if (!strcmp(cmd, "feel")) {
        r = feel_card_print();
    } else if (!strcmp(cmd, "shots")) {
        r = simh_shots(argc - 2, argv + 2);
    } else if (!strcmp(cmd, "dlztable")) {
        r = simh_dlztable(argc - 2, argv + 2);
    } else if (!strcmp(cmd, "aifight")) {
        r = simh_aifight(argc - 2, argv + 2);
    } else if (!strcmp(cmd, "island")) {
        r = simh_island(argc - 2, argv + 2);
    } else {
        usage();
        cfg_free(ini);
        log_close();
        return 2;
    }
    cfg_free(ini);
    log_close();
    return exit_code(r);
}
