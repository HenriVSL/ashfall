/* menu.cpp - frontend, pause menu, results screen, -mscript (DESIGN 14.5). Windows side. Owner: T19.
   See menu.h for the app-state contract. Screens (virtual 640x480, k = h / 480, centred horizontally; A10 layouts):
     MAIN      logo (90,30) + panel 232..408 x 268..392: FLY / OPTIONS / RECORDS / QUIT
     FLY       logo + panel: FREE FLIGHT / TRAINING / SORTIE (Enter starts it) + DIFFICULTY < EASY NORMAL HARD >
     OPTIONS   veil + panel 96..544 x 52..428, tabs CONTROLS / VIDEO / SOUND (cursor on the tab row: Left/Right)
               CONTROLS: 13 actions of DESIGN 5.4 with key capture (Enter, then a key; Esc cancels; Del = default;
               a key used by another action is swapped with it; Esc / F5 / F6 / F9-F12 and the pause keys are
               reserved), INVERT PITCH, CONTROL STYLE (Expert / Novice)
               VIDEO: PRESET (Low / High), RESOLUTION (device modes >= 800x600, 4:3, 16 bit), DISPLAY (fullscreen /
               window), DEFAULT VIEW (cockpit / chase): the first three apply at the next start
               SOUND: MASTER, ENGINE, WARNING TONES, COMBAT EFFECTS (10 steps; live)
     RECORDS   logo + panel: best SORTIE score per difficulty
     PAUSE     veil + panel 176..464 with the small logo: RESUME / RESTART ON RUNWAY / RESTART MISSION (not FREE) /
               OPTIONS / QUIT TO MENU
     RESULTS   panel 120..520 x 40..440 over the frozen flight: stats, score breakdown, SCORE, BEST, NEW RECORD,
               CONTINUE / FLY AGAIN
   Keys: arrows (held: repeat after 0.4 s every 0.08 s), Enter/Space = OK, Esc/Backspace = back, Del = default.
   Sounds: data/sfx/ui_move / ui_ok / ui_back (A5) x [sound] master.
   Draw: one RB_PREMUL ui batch per kit page (the A10 kit and logo quads go straight to ui_quad4_px, which batches
   by texture), then the HUD-font text (hg_*, emitted at hg_end on top). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "menu.h"
#include "params.h"
#include "mission.h"
#include "pilot.h"
#include "hud_font.h"
#include "e_log.h"
#include "e_file.h"
#include "e_image.h"
#include "e_tex.h"
#include "e_ui.h"
#include "e_input.h"
#include "e_audio.h"
#include "e_rend.h"
#include "e_plat.h"

/* ================================================================ constants */
enum { SCR_MAIN, SCR_FLY, SCR_OPTIONS, SCR_RECORDS, SCR_PAUSE, SCR_RESULTS, SCR_N };
enum { TAB_CONTROLS, TAB_VIDEO, TAB_SOUND, TAB_N };
enum { IK_ACTION, IK_VALUE, IK_KEY, IK_SLIDER, IK_TABS };
enum {
    I_FLY, I_OPTIONS, I_RECORDS, I_QUIT,
    I_FREE, I_TRAINING, I_SORTIE, I_DIFF,
    I_TABS, I_KEY0, I_INVERT = I_KEY0 + 16, I_STYLE,
    I_PRESET, I_RES, I_DISPLAY, I_VIEW,
    I_VOL0, I_RESUME = I_VOL0 + 4, I_RUNWAY, I_RESTART, I_POPTIONS, I_QUITMENU,
    I_CONTINUE, I_AGAIN, I_BACK
};
enum { MAX_ITEMS = 24, NBIND = 13, MAX_MODES = 24, FK_MAX = 32, LOGO_TILES = 4 };

#define C_GREEN 0xFF40FF60u
#define C_WHITE 0xFFF0F4F0u
#define C_EMBER 0xFFFF8C28u
#define C_CAPT  0xFFFFC860u
#define C_GREY  0xFF788880u
#define C_RED   0xFFFF5040u

typedef struct MItem { int id, kind; char label[32]; } MItem;

/* bindable actions (DESIGN 5.4), by [controls] name: main defines the action ids */
static const char *const s_bindName[NBIND] = { "pitchUp", "pitchDown", "rollLeft", "rollRight", "fast", "slow",
    "target", "fire", "weapon", "flare", "view", "look", "mapRange" };
static const char *const s_bindLabel[NBIND] = { "PITCH UP", "PITCH DOWN", "ROLL LEFT", "ROLL RIGHT",
    "FAST - AFTERBURNER", "SLOW - BRAKE", "TARGET / LOCK", "FIRE", "NEXT WEAPON", "FLARES", "VIEW",
    "LOOK AT TARGET", "MAP RANGE" };
static int       s_bindAct[NBIND];                  /* e_input action ids (-1 = not defined) */
static InBinding s_bindDef[NBIND][2];               /* default slots 0 and 1 */
static int       s_pauseAct = -1;

static const char *const s_modeName[3] = { "FREE FLIGHT", "TRAINING", "SORTIE" };
static const char *const s_diffName[3] = { "EASY", "NORMAL", "HARD" };
static const char *const s_modeDesc[3][2] = {
    { "NO ENEMIES AND NO TIME LIMIT.", "TAKE OFF AND LEARN THE JET AND THE HUD." },
    { "THREE DRONES THAT DO NOT SHOOT BACK.", "UNLIMITED MISSILES - LEARN THE LAUNCH ZONES." },
    { "WAVES OF BANDITS ATTACK THE ISLAND.", "THREE LIVES - SCORE AND RECORDS." },
};

/* ================================================================ state */
static Cfg   *s_ini;
static char   s_iniPath[E_PATH], s_savePath[E_PATH];
static int    s_inited;
static int    s_scr = SCR_MAIN, s_tab = TAB_CONTROLS, s_fromPause;
static int    s_cur[SCR_N], s_curTab[TAB_N];
static int    s_flyDiff = DIFF_NORMAL;
static int    s_pauseMode = MODE_FREE;
static int    s_dirty;
static u32    s_t;                                  /* menu ticks (fades, blinking) */
static u32    s_enterT;                             /* tick of the last menu_enter(APP_TITLE) */
static int    s_cap = -1;                           /* binding row being captured (index into s_bindName) */
static char   s_msg[64]; static u32 s_msgUntil; static int s_msgWarn;
static int    s_hit[MK_COUNT], s_rep[4], s_inj[MK_COUNT];
static int    s_capInj, s_capInjVk;
/* video modes */
static int    s_modeW[MAX_MODES], s_modeH[MAX_MODES], s_nModes;
/* results snapshot */
static MissionResults s_res;                        /* T20: mission_results + records_submit */
static int    s_resMode, s_resDiff, s_resBest = -1, s_resNew;
/* frame mapping */
static float  s_k = 1.0f;
static int    s_ox, s_sw = 640, s_sh = 480;
/* sounds */
static ASnd   s_sndMove, s_sndOk, s_sndBack;

/* ---- A10 kit */
typedef struct FkSpr { char name[16]; int x, y, w, h, ox, oy, page, kind, l, t, r, b, tint; } FkSpr;
static FkSpr  s_fk[FK_MAX];
static int    s_nfk, s_fkLines, s_fkPageW = 256, s_fkPageH = 256;
static RTex   s_fkTex[2];
static int    s_idPanel = -1, s_idTitle = -1, s_idBar = -1, s_idKeycap = -1, s_idKeycapHot = -1, s_idTab = -1,
              s_idTabOn = -1, s_idMarker = -1, s_idArrowL = -1, s_idArrowR = -1, s_idSeg = -1, s_idSegOff = -1,
              s_idDivider = -1, s_idWhite = -1, s_idCheckOn = -1, s_idCheckOff = -1;
/* ---- logo */
typedef struct LogoTile { char tex[24]; int texW, texH, u0, v0, w, h, offX, offY; RTex t; } LogoTile;
typedef struct LogoSet { char name[8]; int native, pxW, pxH, n, minH; LogoTile tile[LOGO_TILES]; } LogoSet;
static LogoSet s_logoSet[2];
static int    s_nLogoSets, s_logoUse = -1, s_logoLoaded = -1;
static float  s_logoPlace[4] = { 90.0f, 30.0f, 460.0f, 100.0f };
static char   s_smallTexName[24]; static int s_smallW = 256, s_smallH = 64, s_smallU0, s_smallV0 = 4, s_smallUw = 256,
              s_smallVh = 56;
static RTex   s_smallTex;

/* ---- -mscript */
static int    s_scriptOn;
static float  s_fpsMs[64]; static int s_fpsN, s_fpsPos;

static void records_refresh(void);

/* ================================================================ small helpers */
static int vx(float v) { return s_ox + e_ftoi(v * s_k); }
static int vy(float v) { return e_ftoi(v * s_k); }

static void frame_map(void)
{
    r_screen_size(&s_sw, &s_sh);
    if (s_sw <= 0 || s_sh <= 0) { s_sw = 640; s_sh = 480; }
    s_k = (float)s_sh / 480.0f;
    s_ox = (s_sw - e_ftoi(640.0f * s_k)) / 2;
    if (s_ox < 0) s_ox = 0;
}

static void play(ASnd s)
{
    const Param *p = params_find("sound", "master");
    float m = p ? *(const float *)p->ptr : 0.7f;
    if (s) au_play(s, 0.6f * m, 0.0f, 1.0f, 0);
}

static void set_msg(const char *m, int warn)
{
    e_strlcpy(s_msg, m, (int)sizeof(s_msg));
    s_msgUntil = s_t + (u32)(SIM_HZ * 5 / 2);
    s_msgWarn = warn;
    log_printf(LOG_INFO, "menu: msg \"%s\"", m);
}

static char *next_line(char **p)
{
    char *s = *p, *e;
    if (!s || !*s) return NULL;
    e = s;
    while (*e && *e != '\n' && *e != '\r') e++;
    if (*e) { *e++ = 0; while (*e == '\n' || *e == '\r') e++; }
    *p = e;
    return s;
}

static int split_words(char *s, char **w, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        w[n++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = 0;
    }
    return n;
}

/* ================================================================ ini updater (keeps comments, order, CRLF)
   Rewrites one key of a file line by line: the first "key=" line of the section gets the new value; a missing key
   goes after the last key line of its section (or right after the header); a missing section is appended. Every
   other line is written back verbatim (line endings normalised to CRLF). Unlike Cfg (1024 lines) it has no size
   limit, so the generated jet.ini (> 1000 lines with the comments) can never lose its tail. */
static int ini_line_is_key(const char *b, const char *e, const char *key)
{
    const char *eq = b, *ke;
    int n = (int)strlen(key);
    while (b < e && (*b == ' ' || *b == '\t')) b++;
    if (b >= e || *b == ';' || *b == '#' || *b == '[') return 0;
    while (eq < e && *eq != '=') eq++;
    if (eq >= e) return 0;
    ke = eq;
    while (ke > b && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
    if ((int)(ke - b) != n) return 0;
    return _strnicmp(b, key, (size_t)n) == 0;
}

static int ini_line_section(const char *b, const char *e, char *out, int n)
{
    const char *c;
    int len;
    while (b < e && (*b == ' ' || *b == '\t')) b++;
    if (b >= e || *b != '[') return 0;
    c = b + 1;
    while (c < e && *c != ']') c++;
    len = (int)(c - b - 1);
    if (len > n - 1) len = n - 1;
    memcpy(out, b + 1, (size_t)len);
    out[len] = 0;
    return 1;
}

static int ini_write_key(const char *path, const char *sec, const char *key, const char *val)
{
    int size = 0, cap, used = 0, inSec = 0, done = 0, secSeen = 0, insertAt = -1, ok;
    char *src = (char *)file_load(path, &size), *dst, cur[64];
    const char *p, *end, *eol, *le;
    char line[400];
    if (!src) { src = (char *)malloc(1); if (!src) return 0; src[0] = 0; size = 0; }
    cap = size * 2 + 1024;
    dst = (char *)malloc((size_t)cap);
    if (!dst) { free(src); return 0; }
    e_snprintf(line, (int)sizeof(line), "%s=%s\r\n", key, val);
    p = src; end = src + size;
    while (p < end) {
        eol = p;
        while (eol < end && *eol != '\n') eol++;
        le = eol;
        if (le > p && le[-1] == '\r') le--;
        if (ini_line_section(p, le, cur, (int)sizeof(cur))) {
            if (inSec && !done && insertAt < 0) insertAt = used;   /* empty section: right after the header */
            inSec = !e_stricmp(cur, sec);
            if (inSec) { secSeen = 1; insertAt = -1; }
            memcpy(dst + used, p, (size_t)(le - p)); used += (int)(le - p);
            dst[used++] = '\r'; dst[used++] = '\n';
            if (inSec) insertAt = used;
        } else if (inSec && !done && ini_line_is_key(p, le, key)) {
            int n = (int)strlen(line);
            memcpy(dst + used, line, (size_t)n); used += n;
            done = 1;
        } else {
            int isKey = 0;
            const char *b = p;
            while (b < le && (*b == ' ' || *b == '\t')) b++;
            if (b < le && *b != ';' && *b != '#' && memchr(b, '=', (size_t)(le - b))) isKey = 1;
            memcpy(dst + used, p, (size_t)(le - p)); used += (int)(le - p);
            dst[used++] = '\r'; dst[used++] = '\n';
            if (inSec && isKey) insertAt = used;
        }
        p = eol < end ? eol + 1 : end;
    }
    if (!done) {
        int n = (int)strlen(line);
        if (secSeen && insertAt >= 0) {
            memmove(dst + insertAt + n, dst + insertAt, (size_t)(used - insertAt));
            memcpy(dst + insertAt, line, (size_t)n);
            used += n;
        } else {
            used += e_snprintf(dst + used, cap - used, "%s[%s]\r\n%s", used > 0 ? "\r\n" : "", sec, line);
        }
    }
    ok = file_save(path, dst, used);
    free(dst);
    free(src);
    if (!ok) log_printf(LOG_WARN, "warn: menu cannot save path=%s key=%s.%s", path, sec, key);
    return ok;
}

/* ================================================================ settings (live params + saved ini) */
static const char *param_text(const char *sec, const char *key, char *buf, int n)
{
    const Param *p = params_find(sec, key);
    buf[0] = 0;
    if (p) params_value_str(p, buf, n);
    return buf;
}

static float param_f(const char *sec, const char *key, float def)
{
    const Param *p = params_find(sec, key);
    if (!p) return def;
    if (p->type == P_FLOAT) return *(const float *)p->ptr;
    if (p->type == P_INT || p->type == P_BOOL) return (float)*(const int *)p->ptr;
    return def;
}

/* apply one setting live and save it */
static void setting_set(const char *sec, const char *key, const char *val)
{
    char line[160];
    e_snprintf(line, (int)sizeof(line), "%s.%s=%s\n", sec, key, val);
    if (params_find(sec, key)) params_restore(line, (int)strlen(line));
    if (s_ini) cfg_set_str(s_ini, sec, key, val);
    ini_write_key(s_savePath, sec, key, val);
    s_dirty = 1;
    log_printf(LOG_INFO, "menu: set %s.%s=%s save=%s", sec, key, val, s_savePath);
}

static void setting_set_f(const char *sec, const char *key, float v)
{
    char b[32];
    e_snprintf(b, (int)sizeof(b), "%.2f", (double)v);
    setting_set(sec, key, b);
}

static void setting_set_i(const char *sec, const char *key, int v)
{
    char b[32];
    e_snprintf(b, (int)sizeof(b), "%d", v);
    setting_set(sec, key, b);
}

int menu_params_dirty(void) { int d = s_dirty; s_dirty = 0; return d; }

void menu_set_ini(Cfg *ini, const char *iniPath)
{
    s_ini = ini;
    e_strlcpy(s_iniPath, iniPath ? iniPath : "", (int)sizeof(s_iniPath));
    e_strlcpy(s_savePath, s_iniPath, (int)sizeof(s_savePath));
}

/* ================================================================ A10 kit + logo loading */
static RTex load_premul(const char *name)           /* hud_font.cpp load_page: colour + _a, flags 0, combined */
{
    char p1[E_PATH], p2[E_PATH], tn[48];
    EImage rgb, al;
    RTex t = 0;
    int i, ok = 0;
    e_snprintf(p1, (int)sizeof(p1), "tex/%s.bmp", name);
    e_snprintf(p2, (int)sizeof(p2), "tex/%s_a.bmp", name);
    if (!img_load(&rgb, p1, 0)) { log_printf(LOG_WARN, "warn: menu texture missing %s", p1); return 0; }
    if (img_load(&al, p2, 0)) {
        if (al.w == rgb.w && al.h == rgb.h) {
            for (i = 0; i < rgb.w * rgb.h; i++) rgb.pix[0][i] = (rgb.pix[0][i] & 0xFFFFFFu) | ((al.pix[0][i] >> 16 & 0xFF) << 24);
            ok = 1;
        }
        img_free(&al);
    }
    rgb.flags = IMG_ALPHA;
    if (ok) {
        e_snprintf(tn, (int)sizeof(tn), "%s_premul", name);
        t = tex_add(tn, &rgb, RT_CLAMP | RT_NOMIP);
    }
    img_free(&rgb);
    return t;
}

static int fk_find(const char *name)
{
    int i;
    for (i = 0; i < s_nfk; i++) if (!strcmp(s_fk[i].name, name)) return i;
    return -1;
}

static int fk_load(int lines)
{
    char rel[64], path[E_PATH], *txt, *p, *ln;
    int size = 0, a[16], n, pages = 0, i;
    e_snprintf(rel, (int)sizeof(rel), "tex\\front_%d.txt", lines);
    data_path(path, (int)sizeof(path), rel);
    txt = (char *)file_load(path, &size);
    if (!txt) return 0;
    s_nfk = 0;
    p = txt;
    while ((ln = next_line(&p)) != NULL) {
        char name[32], tex[48];
        if (ln[0] == '#' || !ln[0]) continue;
        if (!strncmp(ln, "set ", 4)) {
            if (sscanf(ln + 4, "%d %d %d %d", &a[0], &a[1], &a[2], &a[3]) == 4) { s_fkPageW = a[2]; s_fkPageH = a[3]; }
            continue;
        }
        if (sscanf(ln, "page %d %47s", &a[0], tex) == 2) {
            if (a[0] >= 0 && a[0] < 2) { s_fkTex[a[0]] = load_premul(tex); pages++; }
            continue;
        }
        memset(a, 0, sizeof(a));
        n = sscanf(ln, "%31s %d %d %d %d %d %d %d %d %d %d %d %d %d", name, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5],
                   &a[6], &a[7], &a[8], &a[9], &a[10], &a[11], &a[12]);
        if (n >= 9 && s_nfk < FK_MAX) {
            FkSpr *s = &s_fk[s_nfk++];
            e_strlcpy(s->name, name, (int)sizeof(s->name));
            s->x = a[0]; s->y = a[1]; s->w = a[2]; s->h = a[3]; s->ox = a[4]; s->oy = a[5]; s->page = a[6];
            s->kind = a[7]; s->l = a[8]; s->t = a[9]; s->r = a[10]; s->b = a[11]; s->tint = a[12];
            if (s->page < 0 || s->page > 1) s->page = 0;
        }
    }
    free(txt);
    s_fkLines = lines;
    s_idPanel = fk_find("panel"); s_idTitle = fk_find("title"); s_idBar = fk_find("bar");
    s_idKeycap = fk_find("keycap"); s_idKeycapHot = fk_find("keycap_hot"); s_idTab = fk_find("tab");
    s_idTabOn = fk_find("tab_on"); s_idMarker = fk_find("marker"); s_idArrowL = fk_find("arrow_l");
    s_idArrowR = fk_find("arrow_r"); s_idSeg = fk_find("seg"); s_idSegOff = fk_find("seg_off");
    s_idDivider = fk_find("divider"); s_idWhite = fk_find("white"); s_idCheckOn = fk_find("check_on");
    s_idCheckOff = fk_find("check_off");
    for (i = 0; i < pages && i < 2; i++) if (!s_fkTex[i]) return 0;
    return pages > 0 && s_idPanel >= 0 && s_idWhite >= 0;
}

static void logo_parse(void)
{
    char path[E_PATH], *txt, *p, *ln, nm[24], tx[24];
    int size = 0, a[10];
    data_path(path, (int)sizeof(path), "tex\\logo.txt");
    txt = (char *)file_load(path, &size);
    s_nLogoSets = 0;
    if (!txt) { log_printf(LOG_WARN, "warn: menu logo.txt missing"); return; }
    p = txt;
    while ((ln = next_line(&p)) != NULL) {
        if (ln[0] == '#' || !ln[0]) continue;
        if (!strncmp(ln, "place ", 6)) {
            if (sscanf(ln + 6, "%d %d %d %d", &a[0], &a[1], &a[2], &a[3]) == 4) {
                s_logoPlace[0] = (float)a[0]; s_logoPlace[1] = (float)a[1]; s_logoPlace[2] = (float)a[2]; s_logoPlace[3] = (float)a[3];
            }
        } else if (!strncmp(ln, "set ", 4)) {
            if (sscanf(ln + 4, "%23s %d %d %d %d %d", nm, &a[0], &a[1], &a[2], &a[3], &a[4]) == 6 && s_nLogoSets < 2) {
                LogoSet *s = &s_logoSet[s_nLogoSets++];
                memset(s, 0, sizeof(*s));
                e_strlcpy(s->name, nm, (int)sizeof(s->name));
                s->native = a[0]; s->pxW = a[1]; s->pxH = a[2]; s->minH = a[4];
            }
        } else if (!strncmp(ln, "tile ", 5)) {
            int k;
            if (sscanf(ln + 5, "%23s %d %23s %d %d %d %d %d %d %d %d", nm, &a[0], tx, &a[1], &a[2], &a[3], &a[4], &a[5], &a[6],
                       &a[7], &a[8]) != 11) continue;
            for (k = 0; k < s_nLogoSets; k++) {
                LogoSet *s = &s_logoSet[k];
                LogoTile *t;
                if (strcmp(s->name, nm) || s->n >= LOGO_TILES) continue;
                t = &s->tile[s->n++];
                memset(t, 0, sizeof(*t));
                e_strlcpy(t->tex, tx, (int)sizeof(t->tex));
                t->texW = a[1]; t->texH = a[2]; t->u0 = a[3]; t->v0 = a[4]; t->w = a[5]; t->h = a[6]; t->offX = a[7]; t->offY = a[8];
            }
        } else if (!strncmp(ln, "small ", 6)) {
            if (sscanf(ln + 6, "%23s %d %d %d %d %d %d", tx, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 7) {
                e_strlcpy(s_smallTexName, tx, (int)sizeof(s_smallTexName));
                s_smallW = a[0]; s_smallH = a[1]; s_smallU0 = a[2]; s_smallV0 = a[3]; s_smallUw = a[4]; s_smallVh = a[5];
            }
        }
    }
    free(txt);
}

static void logo_free_tiles(void)
{
    int k, i;
    for (k = 0; k < s_nLogoSets; k++)
        for (i = 0; i < s_logoSet[k].n; i++)
            if (s_logoSet[k].tile[i].t) { tex_release(s_logoSet[k].tile[i].t); s_logoSet[k].tile[i].t = 0; }
    s_logoLoaded = -1;
}

static void logo_load_tiles(void)
{
    int k, best = -1, i;
    for (k = 0; k < s_nLogoSets; k++)                /* the set with the largest minScreenH <= screen h */
        if (s_logoSet[k].minH <= s_sh && (best < 0 || s_logoSet[k].minH > s_logoSet[best].minH)) best = k;
    s_logoUse = best;
    if (best == s_logoLoaded) return;
    logo_free_tiles();
    if (best < 0) return;
    for (i = 0; i < s_logoSet[best].n; i++) s_logoSet[best].tile[i].t = load_premul(s_logoSet[best].tile[i].tex);
    s_logoLoaded = best;
    log_printf(LOG_INFO, "menu: logo set=%s tiles=%d screen_h=%d", s_logoSet[best].name, s_logoSet[best].n, s_sh);
}

/* ================================================================ drawing primitives (screen px) */
static void quad_uv(RTex t, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 col)
{
    RVert2D v[4];
    float du, dv;
    if (!t || x1 <= x0 || y1 <= y0) return;
    du = (u1 - u0) / (x1 - x0);
    dv = (v1 - v0) / (y1 - y0);
    if (x0 < 0.0f) { u0 -= x0 * du; x0 = 0.0f; }   /* clip to the screen (no guard band), uv follow */
    if (y0 < 0.0f) { v0 -= y0 * dv; y0 = 0.0f; }
    if (x1 > (float)s_sw) { u1 -= (x1 - (float)s_sw) * du; x1 = (float)s_sw; }
    if (y1 > (float)s_sh) { v1 -= (y1 - (float)s_sh) * dv; y1 = (float)s_sh; }
    if (x1 <= x0 || y1 <= y0) return;
    v[0].x = x0; v[0].y = y0; v[0].u = u0; v[0].v = v0;
    v[1].x = x1; v[1].y = y0; v[1].u = u1; v[1].v = v0;
    v[2].x = x1; v[2].y = y1; v[2].u = u1; v[2].v = v1;
    v[3].x = x0; v[3].y = y1; v[3].u = u0; v[3].v = v1;
    v[0].color = v[1].color = v[2].color = v[3].color = col;
    ui_quad4_px(t, v);
}

/* texel rect (sx, sy, sw, sh) of the kit page -> screen rect */
static void fk_part(const FkSpr *s, int sx, int sy, int sw, int sh, float x0, float y0, float x1, float y1, u32 col)
{
    float iw = 1.0f / (float)s_fkPageW, ih = 1.0f / (float)s_fkPageH;
    if (sw <= 0 || sh <= 0) return;
    quad_uv(s_fkTex[s->page], x0, y0, x1, y1, (float)sx * iw, (float)sy * ih, (float)(sx + sw) * iw, (float)(sy + sh) * ih, col);
}

static u32 fk_col(const FkSpr *s, u32 argb, float a)  /* tint 1: tinted shape; tint 0: baked colours, fade only */
{
    return s->tint ? hg_col(argb, a) : hg_col(0xFFFFFFFFu, a);
}

static void fk_sprite(int id, float ax, float ay, u32 argb, float a)
{
    const FkSpr *s;
    float x0, y0;
    if (id < 0) return;
    s = &s_fk[id];
    x0 = (float)(e_ftoi(ax) - s->ox);
    y0 = (float)(e_ftoi(ay) - s->oy);
    fk_part(s, s->x, s->y, s->w, s->h, x0, y0, x0 + (float)s->w, y0 + (float)s->h, fk_col(s, argb, a));
}

static void fk_nine(int id, int x0, int y0, int x1, int y1, u32 argb, float a)
{
    const FkSpr *s;
    int l, t, r, b, cx[4], cy[4], sx[4], sy[4], i, j;
    u32 col;
    if (id < 0) return;
    s = &s_fk[id];
    col = fk_col(s, argb, a);
    l = s->l; t = s->t; r = s->r; b = s->b;
    if (x1 - x0 < l + r) x1 = x0 + l + r;
    if (y1 - y0 < t + b) y1 = y0 + t + b;
    cx[0] = x0; cx[1] = x0 + l; cx[2] = x1 - r; cx[3] = x1;
    cy[0] = y0; cy[1] = y0 + t; cy[2] = y1 - b; cy[3] = y1;
    sx[0] = s->x; sx[1] = s->x + l; sx[2] = s->x + s->w - r; sx[3] = s->x + s->w;
    sy[0] = s->y; sy[1] = s->y + t; sy[2] = s->y + s->h - b; sy[3] = s->y + s->h;
    for (j = 0; j < 3; j++)
        for (i = 0; i < 3; i++)
            fk_part(s, sx[i], sy[j], sx[i + 1] - sx[i], sy[j + 1] - sy[j], (float)cx[i], (float)cy[j], (float)cx[i + 1],
                    (float)cy[j + 1], col);
}

static void fk_three(int id, int x0, int x1, int y0, u32 argb, float a)   /* horizontal 3-slice, height 1:1 */
{
    const FkSpr *s;
    int l, r;
    u32 col;
    if (id < 0) return;
    s = &s_fk[id];
    col = fk_col(s, argb, a);
    l = s->l; r = s->r;
    if (x1 - x0 < l + r) x1 = x0 + l + r;
    fk_part(s, s->x, s->y, l, s->h, (float)x0, (float)y0, (float)(x0 + l), (float)(y0 + s->h), col);
    fk_part(s, s->x + l, s->y, s->w - l - r, s->h, (float)(x0 + l), (float)y0, (float)(x1 - r), (float)(y0 + s->h), col);
    fk_part(s, s->x + s->w - r, s->y, r, s->h, (float)(x1 - r), (float)y0, (float)x1, (float)(y0 + s->h), col);
}

static void fk_rect(int x0, int y0, int x1, int y1, u32 argb, float a)   /* solid, from the kit's white texels */
{
    const FkSpr *s;
    if (s_idWhite < 0) return;
    s = &s_fk[s_idWhite];
    fk_part(s, s->x + 1, s->y + 1, s->w - 2, s->h - 2, (float)x0, (float)y0, (float)x1, (float)y1, hg_col(argb, a));
}

static int fk_h(int id) { return id >= 0 ? s_fk[id].h : 0; }

static void veil(float a) { fk_rect(0, 0, s_sw, s_sh, 0xFF000000u, a); }

static void draw_logo_title(float a)
{
    const LogoSet *s;
    float f, X, Y;
    int i;
    if (s_logoUse < 0 || s_logoLoaded != s_logoUse) return;
    s = &s_logoSet[s_logoUse];
    f = (float)s_sh / (float)s->native;
    X = (float)vx(s_logoPlace[0]);
    Y = (float)vy(s_logoPlace[1]);
    for (i = 0; i < s->n; i++) {
        const LogoTile *t = &s->tile[i];
        float x0 = X + (float)t->offX * f, y0 = Y + (float)t->offY * f;
        quad_uv(t->t, x0, y0, x0 + (float)t->w * f, y0 + (float)t->h * f, (float)t->u0 / (float)t->texW,
                (float)t->v0 / (float)t->texH, (float)(t->u0 + t->w) / (float)t->texW, (float)(t->v0 + t->h) / (float)t->texH,
                hg_col(0xFFFFFFFFu, a));
    }
}

static void draw_logo_small(float cx, float y, float wv, float a)   /* virtual units: centre x, top y, width */
{
    float w = wv * s_k, h = w * (float)s_smallVh / (float)s_smallUw, x0 = (float)vx(cx) - 0.5f * w, y0 = (float)vy(y);
    if (!s_smallTex) return;
    x0 = (float)e_ftoi(x0);
    quad_uv(s_smallTex, x0, y0, x0 + w, y0 + h, (float)s_smallU0 / (float)s_smallW, (float)s_smallV0 / (float)s_smallH,
            (float)(s_smallU0 + s_smallUw) / (float)s_smallW, (float)(s_smallV0 + s_smallVh) / (float)s_smallH,
            hg_col(0xFFFFFFFFu, a));
}

/* text at virtual coordinates (cap top at y) */
static int text(float x, float y, int font, u32 argb, float a, int align, const char *s)
{
    return hg_text(vx(x), vy(y), font, hg_col(argb, a), align, s);
}

/* panel + optional title strip with its text */
static void panel(float x0, float y0, float x1, float y1, const char *title, float a)
{
    fk_nine(s_idPanel, vx(x0), vy(y0), vx(x1), vy(y1), 0xFFFFFFFFu, a);
    if (title && s_idTitle >= 0) {
        int tx0 = vx(x0 + 4.0f), tx1 = vx(x1 - 4.0f), ty = vy(y0 + 4.0f);
        fk_three(s_idTitle, tx0, tx1, ty, 0xFFFFFFFFu, a);
        hg_text(tx0 + s_fk[s_idTitle].l + vx(4.0f) - s_ox, ty + (fk_h(s_idTitle) - hg_cap(HF_LARGE)) / 2, HF_LARGE,
                hg_col(C_WHITE, a), 0, title);
    }
}

/* selection bar centred on a text row (cap top y, virtual) + the chevron */
static void sel_bar(float x0, float x1, float y, float markerX, float a)
{
    int capMid = vy(y) + hg_cap(HF_LARGE) / 2, bh = fk_h(s_idBar);
    fk_three(s_idBar, vx(x0), vx(x1), capMid - bh / 2, 0xFFFFFFFFu, a);
    fk_sprite(s_idMarker, (float)vx(markerX), (float)capMid, C_EMBER, a);
}

static void divider(float x0, float x1, float y, float a)
{
    fk_three(s_idDivider, vx(x0), vx(x1), vy(y), C_GREEN, 0.8f * a);
}

static void hint(const char *h, float y, float a)
{
    if (s_msg[0] && s_t < s_msgUntil) text(320.0f, y, HF_SMALL, s_msgWarn ? C_EMBER : C_CAPT, a, 1, s_msg);
    else text(320.0f, y, HF_SMALL, C_GREY, a, 1, h);
}

/* ================================================================ bindings */
static void key_label(const InBinding *b, char *out, int n)
{
    const char *nm;
    if (!b || b->kind == 0) { e_strlcpy(out, "---", n); return; }
    if (b->kind != IN_KEY) { e_strlcpy(out, "JOY", n); return; }
    nm = in_key_name(b->code);
    if (nm) e_strlcpy(out, nm, n);
    else e_snprintf(out, n, "0X%02X", b->code);
}

/* [controls] straight from the file: e_cfg keeps only 1024 lines and the generated jet.ini is longer, so cfg_load drops
   its tail, where [controls] sits (main calls this after in_bind_load; also -mscript "reload"). */
static Cfg *cfg_load_section(const char *path, const char *sec)   /* one [sec] of a file, no line limit */
{
    int size = 0, n = (int)strlen(sec);
    char *t = (char *)file_load(path, &size), *p, *q;
    Cfg *c = NULL;
    if (!t) return cfg_parse("", 0);
    for (p = t; *p; p++) {
        if (*p != '[' || (p > t && p[-1] != '\n') || _strnicmp(p + 1, sec, (size_t)n) || p[1 + n] != ']') continue;
        q = p + 2 + n;
        while (*q && !(q[-1] == '\n' && *q == '[')) q++;   /* up to the next section header */
        c = cfg_parse(p, (int)(q - p));
        break;
    }
    free(t);
    return c ? c : cfg_parse("", 0);
}

void menu_load_controls(const char *path)
{
    Cfg *c = cfg_load_section(path, "controls");
    in_bind_load(c);
    log_printf(LOG_INFO, "menu: controls loaded from path=%s keys=%d", path, cfg_num_keys(c));
    cfg_free(c);
}

void menu_input_defaults(void)
{
    int i, a;
    for (i = 0; i < NBIND; i++) {
        s_bindAct[i] = -1;
        for (a = 0; a < IN_MAX_ACTIONS; a++)
            if (!e_stricmp(in_action_name(a), s_bindName[i])) { s_bindAct[i] = a; break; }
        if (s_bindAct[i] >= 0) {
            s_bindDef[i][0] = *in_binding(s_bindAct[i], 0);
            s_bindDef[i][1] = *in_binding(s_bindAct[i], 1);
        } else memset(s_bindDef[i], 0, sizeof(s_bindDef[i]));
    }
    s_pauseAct = -1;
    for (a = 0; a < IN_MAX_ACTIONS; a++) if (!e_stricmp(in_action_name(a), "pause")) { s_pauseAct = a; break; }
}

static void bind_save(int i)
{
    char line[160], one[48];
    int s, used = 0, a = s_bindAct[i];
    if (a < 0) return;
    line[0] = 0;
    for (s = 0; s < IN_SLOTS; s++) {
        in_binding_str(in_binding(a, s), one, (int)sizeof(one));
        used += e_snprintf(line + used, (int)sizeof(line) - used, "%s%s", s ? ", " : "", one);
    }
    if (s_ini) cfg_set_str(s_ini, "controls", s_bindName[i], line);
    ini_write_key(s_savePath, "controls", s_bindName[i], line);
    log_printf(LOG_INFO, "menu: bind %s=%s save=%s", s_bindName[i], line, s_savePath);
}

static int key_reserved(int vk)
{
    int s;
    if (vk == VK_ESCAPE || vk == VK_F5 || vk == VK_F6 || vk == VK_F9 || vk == VK_F10 || vk == VK_F11 || vk == VK_F12) return 1;
    if (s_pauseAct >= 0)
        for (s = 0; s < IN_SLOTS; s++) {
            const InBinding *b = in_binding(s_pauseAct, s);
            if (b->kind == IN_KEY && b->code == vk) return 1;
        }
    return 0;
}

/* row i gets key vk in slot 0 (b = the new binding); another action holding vk (slot 0 or 1) is swapped */
static void bind_apply(int i, InBinding nb)
{
    InBinding old, alt;
    int j, s;
    char m[64], kn[24];
    if (s_bindAct[i] < 0) return;
    old = *in_binding(s_bindAct[i], 0);
    alt = *in_binding(s_bindAct[i], 1);
    if (nb.kind == IN_KEY && key_reserved(nb.code)) {
        key_label(&nb, kn, (int)sizeof(kn));
        e_snprintf(m, (int)sizeof(m), "%s IS RESERVED", kn);
        set_msg(m, 1);
        play(s_sndBack);
        return;
    }
    m[0] = 0;
    for (j = 0; j < NBIND; j++) {
        if (j == i || s_bindAct[j] < 0) continue;
        for (s = 0; s < 2; s++) {
            const InBinding *b = in_binding(s_bindAct[j], s);
            if (b->kind != nb.kind || b->code != nb.code) continue;
            if (s == 0) in_bind(s_bindAct[j], 0, old.kind, old.code, old.invert);   /* swap */
            else in_bind(s_bindAct[j], 1, 0, 0, 0);
            bind_save(j);
            key_label(&old, kn, (int)sizeof(kn));
            if (s == 0) e_snprintf(m, (int)sizeof(m), "SWAPPED: %s IS NOW %s", s_bindLabel[j], kn);
            else e_snprintf(m, (int)sizeof(m), "REMOVED FROM %s", s_bindLabel[j]);
            log_printf(LOG_INFO, "menu: bind conflict action=%s slot=%d with=%s", s_bindName[j], s, s_bindName[i]);
        }
    }
    in_bind(s_bindAct[i], 0, nb.kind, nb.code, nb.invert);
    if (alt.kind == nb.kind && alt.code == nb.code) in_bind(s_bindAct[i], 1, 0, 0, 0);
    bind_save(i);
    if (m[0]) set_msg(m, 0);
    play(s_sndOk);
}

static void bind_default(int i)
{
    const InBinding *d0 = &s_bindDef[i][0], *d1 = &s_bindDef[i][1];
    if (s_bindAct[i] < 0) return;
    bind_apply(i, *d0);
    in_bind(s_bindAct[i], 1, d1->kind, d1->code, d1->invert);
    bind_save(i);
}

static void capture_start(int i)
{
    s_cap = i;
    in_watch_all(1);
    in_capture_begin();
    s_capInj = 0;
    log_printf(LOG_INFO, "menu: capture start action=%s", s_bindName[i]);
}

static void capture_end(void)
{
    s_cap = -1;
    in_capture_end();
    in_watch_all(0);
}

static void capture_tick(void)
{
    int kind = 0, code = 0;
    InBinding nb;
    if (s_capInj) { kind = IN_KEY; code = s_capInjVk; s_capInj = 0; }
    else if (s_inj[MK_BACK]) { kind = IN_KEY; code = VK_ESCAPE; }
    else if (!in_capture(&kind, &code)) return;
    if (kind != IN_KEY) { in_capture_begin(); return; }   /* keyboard only (user 2026-11-14: no joystick) */
    if (code == VK_ESCAPE) {
        log_printf(LOG_INFO, "menu: capture cancel action=%s", s_bindName[s_cap]);
        capture_end();
        play(s_sndBack);
        return;
    }
    nb.kind = IN_KEY; nb.code = code; nb.invert = 0;
    {
        int i = s_cap;
        capture_end();
        bind_apply(i, nb);
    }
}

/* ================================================================ video modes */
static void modes_build(void)
{
    int i, j, w, h, b, n = r_mode_count();
    s_nModes = 0;
    for (i = 0; i < n; i++) {
        if (!r_mode_at(i, &w, &h, &b)) continue;
        if (b != 16 || w < 800 || h < 600 || w * 3 != h * 4) continue;   /* the game runs 16 bit, HUD layout 4:3 */
        for (j = 0; j < s_nModes; j++) if (s_modeW[j] == w && s_modeH[j] == h) break;
        if (j < s_nModes || s_nModes >= MAX_MODES) continue;
        s_modeW[s_nModes] = w; s_modeH[s_nModes] = h; s_nModes++;
    }
    if (s_nModes == 0) { s_modeW[0] = 800; s_modeH[0] = 600; s_nModes = 1; }
    for (i = 1; i < s_nModes; i++)                   /* sort by width */
        for (j = i; j > 0 && s_modeW[j] < s_modeW[j - 1]; j--) {
            int t = s_modeW[j]; s_modeW[j] = s_modeW[j - 1]; s_modeW[j - 1] = t;
            t = s_modeH[j]; s_modeH[j] = s_modeH[j - 1]; s_modeH[j - 1] = t;
        }
    log_printf(LOG_INFO, "menu: modes device=%d listed=%d first=%dx%d last=%dx%d", n, s_nModes, s_modeW[0], s_modeH[0],
               s_modeW[s_nModes - 1], s_modeH[s_nModes - 1]);
}

static int mode_index(void)
{
    int w = (int)param_f("video", "width", 800.0f), h = (int)param_f("video", "height", 600.0f), i;
    for (i = 0; i < s_nModes; i++) if (s_modeW[i] == w && s_modeH[i] == h) return i;
    return -1;
}

/* ================================================================ records (T20's records.ini API, mission.h) */
int menu_records_best(int diff)
{
    RecordRow row;
    if (diff < 0 || diff > 2) return -1;
    return records_get(diff, &row) ? row.score : -1;
}

void menu_flight_started(int mode, int diff)      /* kept for main; T20's records_submit reports prevBest itself */
{
    (void)mode; (void)diff;
}

/* ================================================================ items per screen */
static void add(MItem *it, int *n, int id, int kind, const char *label)
{
    if (*n >= MAX_ITEMS) return;
    it[*n].id = id;
    it[*n].kind = kind;
    e_strlcpy(it[*n].label, label, (int)sizeof(it[*n].label));
    (*n)++;
}

static int build(int scr, MItem *it)
{
    int n = 0, i;
    switch (scr) {
    case SCR_MAIN:
        add(it, &n, I_FLY, IK_ACTION, "FLY"); add(it, &n, I_OPTIONS, IK_ACTION, "OPTIONS");
        add(it, &n, I_RECORDS, IK_ACTION, "RECORDS"); add(it, &n, I_QUIT, IK_ACTION, "QUIT");
        break;
    case SCR_FLY:
        add(it, &n, I_FREE, IK_ACTION, s_modeName[0]); add(it, &n, I_TRAINING, IK_ACTION, s_modeName[1]);
        add(it, &n, I_SORTIE, IK_ACTION, s_modeName[2]); add(it, &n, I_DIFF, IK_VALUE, "DIFFICULTY");
        break;
    case SCR_OPTIONS:
        add(it, &n, I_TABS, IK_TABS, "TABS");
        if (s_tab == TAB_CONTROLS) {
            for (i = 0; i < NBIND; i++) add(it, &n, I_KEY0 + i, IK_KEY, s_bindLabel[i]);
            add(it, &n, I_INVERT, IK_VALUE, "INVERT PITCH");
            add(it, &n, I_STYLE, IK_VALUE, "CONTROL STYLE");
        } else if (s_tab == TAB_VIDEO) {
            add(it, &n, I_PRESET, IK_VALUE, "PRESET"); add(it, &n, I_RES, IK_VALUE, "RESOLUTION");
            add(it, &n, I_DISPLAY, IK_VALUE, "DISPLAY"); add(it, &n, I_VIEW, IK_VALUE, "DEFAULT VIEW");
        } else {
            add(it, &n, I_VOL0 + 0, IK_SLIDER, "MASTER"); add(it, &n, I_VOL0 + 1, IK_SLIDER, "ENGINE");
            add(it, &n, I_VOL0 + 2, IK_SLIDER, "WARNING TONES"); add(it, &n, I_VOL0 + 3, IK_SLIDER, "COMBAT EFFECTS");
        }
        break;
    case SCR_RECORDS:
        add(it, &n, I_BACK, IK_ACTION, "BACK");
        break;
    case SCR_PAUSE:
        add(it, &n, I_RESUME, IK_ACTION, "RESUME"); add(it, &n, I_RUNWAY, IK_ACTION, "RESTART ON RUNWAY");
        if (s_pauseMode != MODE_FREE) add(it, &n, I_RESTART, IK_ACTION, "RESTART MISSION");
        add(it, &n, I_POPTIONS, IK_ACTION, "OPTIONS"); add(it, &n, I_QUITMENU, IK_ACTION, "QUIT TO MENU");
        break;
    case SCR_RESULTS:
        add(it, &n, I_CONTINUE, IK_ACTION, "CONTINUE"); add(it, &n, I_AGAIN, IK_ACTION, "FLY AGAIN");
        break;
    }
    return n;
}

/* value text of an IK_VALUE item */
static void value_text(int id, char *out, int n)
{
    char b[32];
    int m;
    out[0] = 0;
    switch (id) {
    case I_DIFF: e_strlcpy(out, s_diffName[s_flyDiff], n); break;
    case I_INVERT: e_strlcpy(out, param_f("pilot", "invertPitch", 0.0f) > 0.5f ? "ON" : "OFF", n); break;
    case I_STYLE: e_strlcpy(out, !e_stricmp(param_text("pilot", "mode", b, (int)sizeof(b)), "novice") ? "NOVICE" : "EXPERT", n); break;
    case I_PRESET: e_strlcpy(out, !e_stricmp(param_text("video", "preset", b, (int)sizeof(b)), "high") ? "HIGH" : "LOW", n); break;
    case I_RES:
        m = mode_index();
        if (m >= 0) e_snprintf(out, n, "%dX%d", s_modeW[m], s_modeH[m]);
        else e_snprintf(out, n, "%dX%d", (int)param_f("video", "width", 800.0f), (int)param_f("video", "height", 600.0f));
        break;
    case I_DISPLAY: e_strlcpy(out, param_f("video", "fullscreen", 1.0f) > 0.5f ? "FULLSCREEN" : "WINDOW", n); break;
    case I_VIEW: e_strlcpy(out, param_f("camera", "startView", 1.0f) > 0.5f ? "CHASE" : "COCKPIT", n); break;
    }
}

static const char *const s_volKey[4] = { "master", "engine", "warn", "combat" };

static int vol_steps(int i)                         /* 0..10 */
{
    float v = param_f("sound", s_volKey[i], 1.0f);
    int s = e_ftoi(v * 10.0f);
    return E_CLAMP(s, 0, 10);
}

/* left/right (dir -1/+1, 0 = Enter: next) on a value/slider item */
static void change_value(int id, int dir)
{
    int step = dir == 0 ? 1 : dir, m;
    char b[32];
    switch (id) {
    case I_DIFF: s_flyDiff = (s_flyDiff + 3 + step) % 3; break;
    case I_INVERT: setting_set_i("pilot", "invertPitch", param_f("pilot", "invertPitch", 0.0f) > 0.5f ? 0 : 1); break;
    case I_STYLE:
        setting_set("pilot", "mode", !e_stricmp(param_text("pilot", "mode", b, (int)sizeof(b)), "novice") ? "expert" : "novice");
        break;
    case I_PRESET:
        setting_set("video", "preset", !e_stricmp(param_text("video", "preset", b, (int)sizeof(b)), "high") ? "low" : "high");
        break;
    case I_RES:
        m = mode_index();
        m = m < 0 ? 0 : (m + s_nModes + step) % s_nModes;
        setting_set_i("video", "width", s_modeW[m]);
        setting_set_i("video", "height", s_modeH[m]);
        break;
    case I_DISPLAY: setting_set_i("video", "fullscreen", param_f("video", "fullscreen", 1.0f) > 0.5f ? 0 : 1); break;
    case I_VIEW: setting_set_i("camera", "startView", param_f("camera", "startView", 1.0f) > 0.5f ? 0 : 1); break;
    default:
        if (id >= I_VOL0 && id < I_VOL0 + 4) {
            int s = vol_steps(id - I_VOL0) + (dir == 0 ? 1 : dir);
            if (dir == 0 && s > 10) s = 0;
            s = E_CLAMP(s, 0, 10);
            setting_set_f("sound", s_volKey[id - I_VOL0], (float)s * 0.1f);
        }
        break;
    }
    play(s_sndMove);
}

/* ================================================================ input */
static void keys_update(void)
{
    static const int vks[4] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
    int k;
    for (k = 0; k < 4; k++) {
        int h = in_key_hit(vks[k]);
        if (in_key(vks[k])) {
            s_rep[k]++;
            if (s_rep[k] > 48 && ((s_rep[k] - 48) % 10) == 0) h = 1;
        } else s_rep[k] = 0;
        s_hit[k] = h;
    }
    s_hit[MK_OK] = in_key_hit(VK_RETURN) | in_key_hit(VK_SPACE);
    s_hit[MK_BACK] = in_key_hit(VK_ESCAPE) | in_key_hit(VK_BACK);
    s_hit[MK_DEL] = in_key_hit(VK_DELETE);
    s_hit[MK_PAUSE] = 0;
    if (!plat_active() && !s_scriptOn) memset(s_hit, 0, sizeof(s_hit));
    for (k = 0; k < MK_COUNT; k++) s_hit[k] |= s_inj[k];
}

static void go(int scr)
{
    log_printf(LOG_INFO, "menu: screen from=%s to=%s",
               s_scr == SCR_MAIN ? "main" : (s_scr == SCR_FLY ? "fly" : (s_scr == SCR_OPTIONS ? "options" : (s_scr == SCR_RECORDS ? "records" : (s_scr == SCR_PAUSE ? "pause" : "results")))),
               scr == SCR_MAIN ? "main" : (scr == SCR_FLY ? "fly" : (scr == SCR_OPTIONS ? "options" : (scr == SCR_RECORDS ? "records" : (scr == SCR_PAUSE ? "pause" : "results")))));
    s_scr = scr;
    if (scr == SCR_OPTIONS) { s_cur[SCR_OPTIONS] = 0; s_msg[0] = 0; }
    if (scr == SCR_RECORDS) records_refresh();
}

/* generic list navigation; returns the activated item index (OK) or -1 */
static int nav(const MItem *it, int n)
{
    int *cur = &s_cur[s_scr], old = *cur;
    if (n <= 0) return -1;
    if (*cur >= n) *cur = n - 1;
    if (s_hit[MK_UP]) *cur = (*cur + n - 1) % n;
    if (s_hit[MK_DOWN]) *cur = (*cur + 1) % n;
    if (*cur != old) play(s_sndMove);
    if (s_hit[MK_OK]) return *cur;
    return -1;
}

/* OPTIONS (title or pause); returns 1 when it was left with Esc */
static int options_tick(void)
{
    MItem it[MAX_ITEMS];
    int n, sel, id;
    if (s_cap >= 0) { capture_tick(); return 0; }
    n = build(SCR_OPTIONS, it);
    if (s_cur[SCR_OPTIONS] >= n) s_cur[SCR_OPTIONS] = n - 1;
    id = it[s_cur[SCR_OPTIONS]].id;
    if (s_hit[MK_BACK]) { play(s_sndBack); return 1; }
    if (id == I_TABS && (s_hit[MK_LEFT] || s_hit[MK_RIGHT])) {
        s_tab = (s_tab + TAB_N + (s_hit[MK_LEFT] ? -1 : 1)) % TAB_N;
        log_printf(LOG_INFO, "menu: tab %s", s_tab == TAB_CONTROLS ? "controls" : (s_tab == TAB_VIDEO ? "video" : "sound"));
        play(s_sndMove);
        return 0;
    }
    if (it[s_cur[SCR_OPTIONS]].kind == IK_VALUE || it[s_cur[SCR_OPTIONS]].kind == IK_SLIDER) {
        if (s_hit[MK_LEFT]) change_value(id, -1);
        if (s_hit[MK_RIGHT]) change_value(id, 1);
    }
    if (s_hit[MK_DEL] && id >= I_KEY0 && id < I_KEY0 + NBIND) { bind_default(id - I_KEY0); return 0; }
    sel = nav(it, n);
    if (sel < 0) return 0;
    id = it[sel].id;
    if (id == I_TABS) { s_tab = (s_tab + 1) % TAB_N; play(s_sndMove); }
    else if (id >= I_KEY0 && id < I_KEY0 + NBIND) { capture_start(id - I_KEY0); play(s_sndOk); }
    else if (it[sel].kind == IK_VALUE || it[sel].kind == IK_SLIDER) change_value(id, 0);
    return 0;
}

static int title_tick(void)
{
    MItem it[MAX_ITEMS];
    int n, sel, id;
    if (s_scr == SCR_OPTIONS) {
        if (options_tick()) go(SCR_MAIN);
        return APP_TITLE;
    }
    n = build(s_scr, it);
    if (s_scr == SCR_FLY) {
        id = it[s_cur[SCR_FLY] < n ? s_cur[SCR_FLY] : 0].id;
        if (id == I_DIFF && (s_hit[MK_LEFT] || s_hit[MK_RIGHT])) change_value(I_DIFF, s_hit[MK_LEFT] ? -1 : 1);
    }
    if (s_hit[MK_BACK]) {
        play(s_sndBack);
        if (s_scr == SCR_MAIN) s_cur[SCR_MAIN] = 3;   /* Esc on the main screen: cursor to QUIT */
        else go(SCR_MAIN);
        return APP_TITLE;
    }
    sel = nav(it, n);
    if (sel < 0) return APP_TITLE;
    id = it[sel].id;
    switch (id) {
    case I_FLY: play(s_sndOk); go(SCR_FLY); break;
    case I_OPTIONS: play(s_sndOk); s_fromPause = 0; go(SCR_OPTIONS); break;
    case I_RECORDS: play(s_sndOk); go(SCR_RECORDS); break;
    case I_QUIT: play(s_sndBack); log_printf(LOG_INFO, "menu: quit"); return APP_QUIT;
    case I_BACK: play(s_sndBack); go(SCR_MAIN); break;
    case I_DIFF: change_value(I_DIFF, 0); break;
    case I_FREE: case I_TRAINING: case I_SORTIE:
        play(s_sndOk);
        g_args.mode = id == I_FREE ? MODE_FREE : (id == I_TRAINING ? MODE_TRAINING : MODE_SORTIE);
        g_args.diff = s_flyDiff;
        log_printf(LOG_INFO, "menu: fly mode=%d diff=%d start=%d", g_args.mode, g_args.diff, g_args.start);
        return APP_LOADING;
    }
    return APP_TITLE;
}

static int results_tick(void)
{
    MItem it[MAX_ITEMS];
    int n = build(SCR_RESULTS, it), sel = nav(it, n);
    if (s_hit[MK_BACK]) { play(s_sndBack); return APP_TITLE; }
    if (sel < 0) return APP_RESULTS;
    play(s_sndOk);
    log_printf(LOG_INFO, "menu: results choice=%s", it[sel].label);
    return it[sel].id == I_AGAIN ? APP_LOADING : APP_TITLE;
}

int menu_tick(int app)
{
    s_t++;
    keys_update();
    if (app == APP_TITLE) return title_tick();
    if (app == APP_RESULTS) return results_tick();
    return app;
}

void menu_pause_open(int mode)
{
    s_pauseMode = mode;
    s_scr = SCR_PAUSE;
    s_cur[SCR_PAUSE] = 0;
    s_msg[0] = 0;
}

int menu_pause_tick(int pauseHit)
{
    MItem it[MAX_ITEMS];
    int n, sel;
    s_t++;
    keys_update();
    if (s_scr == SCR_OPTIONS) {
        if (s_cap < 0 && pauseHit && !s_hit[MK_BACK]) s_hit[MK_BACK] = 1;   /* P also leaves the options */
        if (options_tick()) go(SCR_PAUSE);
        return MP_STAY;
    }
    if (pauseHit || s_hit[MK_BACK]) { play(s_sndBack); return MP_RESUME; }
    n = build(SCR_PAUSE, it);
    sel = nav(it, n);
    if (sel < 0) return MP_STAY;
    play(s_sndOk);
    log_printf(LOG_INFO, "menu: pause choice=%s", it[sel].label);
    switch (it[sel].id) {
    case I_RESUME: return MP_RESUME;
    case I_RUNWAY: return MP_RUNWAY;
    case I_RESTART: return MP_RESTART_MISSION;
    case I_POPTIONS: s_fromPause = 1; go(SCR_OPTIONS); return MP_STAY;
    case I_QUITMENU: return MP_QUIT_MENU;
    }
    return MP_STAY;
}

/* ================================================================ screens: drawing */
static int s_recBest[3] = { -1, -1, -1 };

static RecordRow s_recRow[3];

static void records_refresh(void)
{
    int d;
    records_load();
    for (d = 0; d < 3; d++) {
        if (!records_get(d, &s_recRow[d])) memset(&s_recRow[d], 0, sizeof(s_recRow[d]));
        s_recBest[d] = s_recRow[d].valid ? s_recRow[d].score : -1;
    }
}

static float fade_in(void)
{
    float a = (float)(s_t - s_enterT) / (0.4f * (float)SIM_HZ);
    return E_CLAMP(a, 0.0f, 1.0f);
}

/* centred list rows (main / pause / results style) */
static void list_rows(const MItem *it, int n, int cur, float y0, float dy, float bx0, float bx1, float mx, float a)
{
    int i;
    for (i = 0; i < n; i++) if (i == cur) sel_bar(bx0, bx1, y0 + dy * (float)i, mx, a);
    for (i = 0; i < n; i++) text(320.0f, y0 + dy * (float)i, HF_LARGE, i == cur ? C_WHITE : C_GREEN, a, 1, it[i].label);
}

static void value_arrows(float xl, float xr, float y, float a)
{
    int mid = vy(y) + hg_cap(HF_LARGE) / 2;
    fk_sprite(s_idArrowL, (float)vx(xl), (float)mid, C_GREEN, a);
    fk_sprite(s_idArrowR, (float)vx(xr), (float)mid, C_GREEN, a);
}

static void draw_main(float a)
{
    MItem it[MAX_ITEMS];
    int n = build(SCR_MAIN, it);
    draw_logo_title(a);
    panel(232.0f, 268.0f, 408.0f, 392.0f, NULL, a);
    list_rows(it, n, s_cur[SCR_MAIN], 284.0f, 24.0f, 240.0f, 400.0f, 254.0f, a);
    hint("ARROWS SELECT     ENTER OK     ESC BACK", 452.0f, a);
}

static void draw_fly(float a)
{
    MItem it[MAX_ITEMS];
    char v[32];
    int n = build(SCR_FLY, it), cur = s_cur[SCR_FLY], i, dm;
    draw_logo_title(a);
    panel(176.0f, 238.0f, 464.0f, 440.0f, "FLY", a);
    for (i = 0; i < 3; i++) if (i == cur) sel_bar(188.0f, 452.0f, 280.0f + 24.0f * (float)i, 200.0f, a);
    if (cur == 3) sel_bar(188.0f, 452.0f, 362.0f, 200.0f, a);
    for (i = 0; i < 3; i++) text(320.0f, 280.0f + 24.0f * (float)i, HF_LARGE, i == cur ? C_WHITE : C_GREEN, a, 1, it[i].label);
    divider(196.0f, 444.0f, 352.0f, a);
    text(208.0f, 362.0f, HF_LARGE, cur == 3 ? C_WHITE : C_GREEN, a, 0, "DIFFICULTY");
    value_text(I_DIFF, v, (int)sizeof(v));
    value_arrows(352.0f, 456.0f, 362.0f, a);
    text(404.0f, 362.0f, HF_LARGE, C_WHITE, a, 1, v);
    dm = cur < 3 ? cur : 2;
    text(320.0f, 396.0f, HF_SMALL, C_GREEN, 0.85f * a, 1, s_modeDesc[dm][0]);
    text(320.0f, 410.0f, HF_SMALL, C_GREEN, 0.85f * a, 1, s_modeDesc[dm][1]);
    (void)n;
    hint("ENTER TAKE OFF     LEFT RIGHT DIFFICULTY     ESC BACK", 452.0f, a);
}

static float opt_row_y(int r)                       /* item index (0 = tabs) -> cap top */
{
    if (s_tab == TAB_CONTROLS) return r - 1 < NBIND ? 112.0f + 17.0f * (float)(r - 1) : 344.0f + 17.0f * (float)(r - 1 - NBIND);
    return 124.0f + 26.0f * (float)(r - 1);
}

static void draw_options(float a, int overFlight)
{
    static const char *const tabName[TAB_N] = { "CONTROLS", "VIDEO", "SOUND" };
    static const float tx[TAB_N][2] = { { 112.0f, 212.0f }, { 218.0f, 318.0f }, { 324.0f, 424.0f } };
    MItem it[MAX_ITEMS];
    char v[48];
    int n = build(SCR_OPTIONS, it), cur = s_cur[SCR_OPTIONS], i, tabMid;
    veil(overFlight ? 0.40f : 0.35f);
    panel(96.0f, 52.0f, 544.0f, 428.0f, "OPTIONS", a);
    tabMid = (vy(86.0f) + vy(104.0f)) / 2;
    for (i = 0; i < TAB_N; i++) {
        fk_nine(i == s_tab ? s_idTabOn : s_idTab, vx(tx[i][0]), vy(86.0f), vx(tx[i][1]), vy(104.0f), 0xFFFFFFFFu, a);
        hg_text((vx(tx[i][0]) + vx(tx[i][1])) / 2, tabMid - hg_cap(HF_SMALL) / 2, HF_SMALL,
                hg_col(i == s_tab ? C_WHITE : C_GREEN, a), 1, tabName[i]);
    }
    if (cur == 0) fk_sprite(s_idMarker, (float)vx(106.0f), (float)tabMid, C_EMBER, a);
    if (cur > 0 && cur < n) sel_bar(112.0f, 532.0f, opt_row_y(cur), 118.0f, a);
    if (s_tab == TAB_CONTROLS) divider(116.0f, 524.0f, 335.0f, a);
    for (i = 1; i < n; i++) {
        float y = opt_row_y(i);
        int sel = i == cur, id = it[i].id;
        text(124.0f, y, HF_LARGE, sel ? C_WHITE : C_GREEN, a, 0, it[i].label);
        if (it[i].kind == IK_KEY) {
            int k = id - I_KEY0, capturing = s_cap == k;
            fk_nine(capturing ? s_idKeycapHot : s_idKeycap, vx(400.0f), vy(y - 1.0f), vx(520.0f), vy(y + 14.0f), 0xFFFFFFFFu, a);
            if (capturing) {
                if (((s_t / 20) & 3) != 3)
                    hg_text(vx(460.0f), vy(y - 1.0f) + (vy(y + 14.0f) - vy(y - 1.0f) - hg_cap(HF_SMALL)) / 2, HF_SMALL,
                            hg_col(C_CAPT, a), 1, "PRESS A KEY");
            } else {
                key_label(s_bindAct[k] >= 0 ? in_binding(s_bindAct[k], 0) : NULL, v, (int)sizeof(v));
                text(460.0f, y, HF_LARGE, C_WHITE, a, 1, v);
            }
        } else if (it[i].kind == IK_VALUE) {
            value_text(id, v, (int)sizeof(v));
            if (s_tab == TAB_CONTROLS) { value_arrows(408.0f, 512.0f, y, a); text(460.0f, y, HF_LARGE, C_WHITE, a, 1, v); }
            else { value_arrows(388.0f, 520.0f, y, a); text(454.0f, y, HF_LARGE, C_WHITE, a, 1, v); }
        } else if (it[i].kind == IK_SLIDER) {
            int s, on = vol_steps(id - I_VOL0);
            for (s = 0; s < 10; s++)
                fk_sprite(s < on ? s_idSeg : s_idSegOff, (float)vx(400.0f + 12.0f * (float)s), (float)vy(y + 1.0f),
                          sel ? C_WHITE : C_GREEN, a);
        }
    }
    if (s_tab == TAB_VIDEO) {
        char b[16];
        int changed = e_stricmp(param_text("video", "preset", b, (int)sizeof(b)), g_args.preset) ||
                      (int)param_f("video", "width", 800.0f) != s_sw || (int)param_f("video", "height", 600.0f) != s_sh ||
                      ((int)param_f("video", "fullscreen", 1.0f) != 0) != (g_args.fullscreen != 0);
        text(320.0f, 238.0f, HF_SMALL, changed ? C_EMBER : C_GREY, a, 1, "PRESET / RESOLUTION / DISPLAY APPLY AT THE NEXT START");
        if (arg_has("-res") || arg_has("-preset") || arg_has("-window") || arg_has("-fullscreen"))
            text(320.0f, 254.0f, HF_SMALL, C_GREY, a, 1, "(COMMAND-LINE SWITCHES OVERRIDE THESE)");
    }
    divider(116.0f, 524.0f, 392.0f, a);
    if (s_cap >= 0) hint("PRESS THE NEW KEY     ESC CANCEL", 404.0f, a);
    else if (cur == 0) hint("LEFT RIGHT SWITCH TAB     DOWN SELECT     ESC BACK", 404.0f, a);
    else if (s_tab == TAB_CONTROLS) hint(cur > NBIND ? "LEFT RIGHT CHANGE     ESC BACK" : "ENTER CHANGE     DEL DEFAULT     ESC BACK", 404.0f, a);
    else hint("LEFT RIGHT CHANGE     ESC BACK", 404.0f, a);
}

static void draw_records(float a)
{
    MItem it[MAX_ITEMS];
    char v[32];
    int n = build(SCR_RECORDS, it), d;
    draw_logo_title(a);
    panel(176.0f, 238.0f, 464.0f, 432.0f, "RECORDS", a);
    text(320.0f, 278.0f, HF_SMALL, C_EMBER, a, 1, "SORTIE  -  BEST SCORE");
    for (d = 0; d < 3; d++) {
        float y = 296.0f + 30.0f * (float)d;
        text(204.0f, y, HF_LARGE, C_GREEN, a, 0, s_diffName[d]);
        if (s_recBest[d] >= 0) e_snprintf(v, (int)sizeof(v), "%d", s_recBest[d]);
        else e_strlcpy(v, "---", (int)sizeof(v));
        text(436.0f, y, HF_LARGE, C_WHITE, a, 2, v);
        if (s_recRow[d].valid) {
            char l[64];
            e_snprintf(l, (int)sizeof(l), "%s   %s   %d KILLS   %02d:%02d", s_recRow[d].date,
                       s_recRow[d].outcome == MIS_COMPLETE ? "COMPLETE" : "FAILED", s_recRow[d].kills,
                       (int)s_recRow[d].timeSec / 60, (int)s_recRow[d].timeSec % 60);
            text(204.0f, y + 16.0f, HF_SMALL, C_GREY, a, 0, l);
        }
    }
    divider(196.0f, 444.0f, 384.0f, a);
    list_rows(it, n, 0, 398.0f, 24.0f, 240.0f, 400.0f, 254.0f, a);
    hint("ENTER / ESC BACK", 452.0f, a);
}

void menu_pause_draw(void)
{
    MItem it[MAX_ITEMS];
    int n;
    float y1;
    frame_map();
    hg_begin();
    if (s_scr == SCR_OPTIONS) draw_options(1.0f, 1);
    else {
        n = build(SCR_PAUSE, it);
        y1 = 214.0f + 25.0f * (float)n + 22.0f;
        veil(0.40f);
        panel(176.0f, 128.0f, 464.0f, y1, NULL, 1.0f);
        draw_logo_small(320.0f, 142.0f, 260.0f, 1.0f);
        list_rows(it, n, s_cur[SCR_PAUSE], 214.0f, 25.0f, 188.0f, 452.0f, 202.0f, 1.0f);
        text(320.0f, y1 - 18.0f, HF_SMALL, C_GREY, 1.0f, 1, "ESC RESUME");
    }
    hg_end();
}

static void stat_row(float y, const char *label, const char *val, float a)
{
    text(144.0f, y, HF_LARGE, C_GREEN, a, 0, label);
    text(496.0f, y, HF_LARGE, C_WHITE, a, 2, val);
}

static void score_row(float y, const char *label, int v, float a)
{
    char b[24];
    e_snprintf(b, (int)sizeof(b), "%d", v);
    text(144.0f, y, HF_SMALL, C_GREEN, a, 0, label);
    text(496.0f, y, HF_SMALL, C_WHITE, a, 2, b);
}

static void draw_results(float a)
{
    MItem it[MAX_ITEMS];
    char b[48], l[48];
    int n = build(SCR_RESULTS, it), fired = s_res.fired[0] + s_res.fired[1], hits = s_res.hits[0] + s_res.hits[1];
    const char *title = s_res.outcome == MIS_COMPLETE ? "MISSION COMPLETE" : (s_res.outcome == MIS_FAILED ? "MISSION FAILED" : "MISSION OVER");
    veil(0.25f);
    panel(120.0f, 40.0f, 520.0f, 440.0f, title, a);
    e_snprintf(b, (int)sizeof(b), "%s  -  %s", s_resMode == MODE_SORTIE ? "SORTIE" : (s_resMode == MODE_TRAINING ? "TRAINING" : "FREE"),
               s_diffName[E_CLAMP(s_resDiff, 0, 2)]);
    text(508.0f, 44.0f + (float)(fk_h(s_idTitle) - hg_cap(HF_SMALL)) / (2.0f * s_k), HF_SMALL, C_GREEN, a, 2, b);
    e_snprintf(b, (int)sizeof(b), "%02d:%02d", (int)s_res.timeSec / 60, (int)s_res.timeSec % 60);
    stat_row(82.0f, "TIME", b, a);
    e_snprintf(b, (int)sizeof(b), "%d / %d", s_res.kills, s_res.spawned);
    stat_row(103.0f, "KILLS", b, a);
    e_snprintf(b, (int)sizeof(b), "%d / %d", s_res.hits[0], s_res.fired[0]);
    stat_row(124.0f, "AIM-120  HITS / FIRED", b, a);
    e_snprintf(b, (int)sizeof(b), "%d / %d", s_res.hits[1], s_res.fired[1]);
    stat_row(145.0f, "AIM-9  HITS / FIRED", b, a);
    e_snprintf(b, (int)sizeof(b), "%d", s_res.deaths);
    stat_row(166.0f, "DEATHS", b, a);
    divider(140.0f, 500.0f, 190.0f, a);
    /* DESIGN 14.4 breakdown (T20's ScoreParts; total = the mission's score) */
    e_snprintf(l, (int)sizeof(l), "KILLS  %d X 1000", s_res.kills);
    score_row(202.0f, l, s_res.parts.kill, a);
    e_snprintf(l, (int)sizeof(l), "BVR BONUS  %d X 300", s_res.killsBvr);
    score_row(220.0f, l, s_res.parts.bvr, a);
    e_snprintf(l, (int)sizeof(l), "DEATHS  %d X -1500", s_res.deaths);
    score_row(238.0f, l, s_res.parts.death, a);
    score_row(256.0f, "TIME BONUS", s_res.parts.time, a);
    e_snprintf(l, (int)sizeof(l), "ACCURACY  %d / %d", hits, fired);
    score_row(274.0f, l, s_res.parts.accuracy, a);
    divider(140.0f, 500.0f, 298.0f, a);
    e_snprintf(b, (int)sizeof(b), "%d", s_res.score);
    stat_row(310.0f, "SCORE", b, a);
    e_snprintf(l, (int)sizeof(l), "BEST - %s", s_diffName[E_CLAMP(s_resDiff, 0, 2)]);
    if (s_resBest >= 0) e_snprintf(b, (int)sizeof(b), "%d", s_resBest);
    else e_strlcpy(b, "---", (int)sizeof(b));
    stat_row(332.0f, l, b, a);
    if (s_resNew) text(496.0f, 352.0f, HF_SMALL, C_EMBER, a, 2, "NEW RECORD");
    list_rows(it, n, s_cur[SCR_RESULTS], 384.0f, 24.0f, 240.0f, 400.0f, 254.0f, a);
}

void menu_draw(int app)
{
    float a;
    frame_map();
    hg_begin();
    if (app == APP_RESULTS) draw_results(1.0f);
    else if (app == APP_TITLE) {
        a = fade_in();
        switch (s_scr) {
        case SCR_FLY: draw_fly(a); break;
        case SCR_OPTIONS: draw_options(a, 0); break;
        case SCR_RECORDS: draw_records(a); break;
        default: draw_main(a); break;
        }
    }
    hg_end();
}

void menu_draw_loading(const char *msg)
{
    int i;
    for (i = 0; i < 2; i++) {                       /* both buffers, so the message stays during the load */
        if (!r_begin_frame(0x0C1420)) { r_device_lost_check(); continue; }
        frame_map();
        hg_begin();
        draw_logo_small(320.0f, 170.0f, 300.0f, 1.0f);
        text(320.0f, 262.0f, HF_LARGE, C_GREEN, 1.0f, 1, msg);
        hg_end();
        r_end_frame();
        r_present();
    }
}

/* ================================================================ title fly-by camera
   A slow orbit around the volcano (island.txt cone_center 900 2000, summit ~3000 m) from 27-31 km out (over the sea,
   the island radius is ~17 km) at 1.05-1.55 km altitude, looking slightly down: sea in front, the coast and the volcano
   under the logo, sky above. One lap ~10 minutes, starting south-west of the cone (the airbase cape on the left). */
void menu_backdrop_camera(Camera *c, float t)
{
    const float cxv = 900.0f, czv = 2000.0f;
    float ang = E_DEG2RAD(222.0f) + 0.0105f * t, R = 29000.0f + 2000.0f * (float)sin(0.031f * t);
    float ey = 1300.0f + 250.0f * (float)sin(0.047f * t + 1.0f), dx, dz, d, hdg, pitch;
    Vec3 eye;
    eye = v3(cxv + R * (float)sin(ang), ey, czv + R * (float)cos(ang));
    dx = cxv - eye.x; dz = czv - eye.z;
    d = (float)sqrt(dx * dx + dz * dz);
    hdg = E_RAD2DEG((float)atan2(dx, dz)) + 8.0f * (float)sin(0.023f * t);
    pitch = E_RAD2DEG((float)atan2(1200.0f - ey, d)) - 5.5f;
    cam_set_fixed(c, eye, hdg, pitch);
}

/* ================================================================ init / enter */
int menu_init(void)
{
    static const int sets[4] = { 480, 600, 768, 1050 };
    char path[E_PATH];
    int i, ok = 0;
    frame_map();
    for (i = 3; i >= 0 && !ok; i--) if (sets[i] <= s_sh || i == 0) ok = fk_load(sets[i]);
    logo_parse();
    if (s_smallTexName[0]) s_smallTex = load_premul(s_smallTexName);
    data_path(path, (int)sizeof(path), "sfx/ui_move.wav"); s_sndMove = au_load(path);
    data_path(path, (int)sizeof(path), "sfx/ui_ok.wav"); s_sndOk = au_load(path);
    data_path(path, (int)sizeof(path), "sfx/ui_back.wav"); s_sndBack = au_load(path);
    modes_build();
    records_load();                                 /* T20: records.ini ([easy] [normal] [hard]) */
    s_flyDiff = (g_args.diff >= 0 && g_args.diff <= 2) ? g_args.diff : DIFF_NORMAL;
    s_inited = 1;
    log_printf(LOG_INFO, "menu: init kit=%d set=%d sprites=%d logo_sets=%d small=%d screen=%dx%d save=%s", ok, s_fkLines, s_nfk,
               s_nLogoSets, s_smallTex != 0, s_sw, s_sh, s_savePath);
    return ok;
}

void menu_free(void)
{
    int i;
    logo_free_tiles();
    for (i = 0; i < 2; i++) if (s_fkTex[i]) { tex_release(s_fkTex[i]); s_fkTex[i] = 0; }
    if (s_smallTex) { tex_release(s_smallTex); s_smallTex = 0; }
    if (s_cap >= 0) capture_end();
    s_inited = 0;
}

void menu_enter(int app)
{
    frame_map();
    if (app == APP_TITLE) {
        s_scr = SCR_MAIN;
        s_enterT = s_t;
        if (s_cap >= 0) capture_end();
        logo_load_tiles();
        records_refresh();
    } else if (app == APP_RESULTS) {
        memset(&s_res, 0, sizeof(s_res));
        if (mission_results(&g_game, &s_res)) records_submit(&s_res);   /* saves records.ini on a new record */
        s_resMode = s_res.mode;
        s_resDiff = s_res.diff;
        s_scr = SCR_RESULTS;
        s_cur[SCR_RESULTS] = 0;
        s_resNew = s_res.newRecord;
        s_resBest = s_res.newRecord ? s_res.score : s_res.prevBest;
        log_printf(LOG_INFO, "menu: results valid=%d outcome=%s score=%d prev_best=%d best=%d new=%d", s_res.valid,
                   mission_outcome_name(s_res.outcome), s_res.score, s_res.prevBest, s_resBest, s_resNew);
    } else if (app == APP_FLIGHT) {
        if (s_cap >= 0) capture_end();
        logo_free_tiles();                           /* A10: load only the menu's set, free it in flight */
    }
}

const char *menu_screen_name(int app, int paused)
{
    if (app == APP_LOADING) return "loading";
    if (app == APP_RESULTS) return "results";
    if (app == APP_FLIGHT) {
        if (!paused) return "flight";
        return s_scr == SCR_OPTIONS ? "options" : "pause";
    }
    if (app == APP_TITLE)
        switch (s_scr) {
        case SCR_FLY: return "fly";
        case SCR_OPTIONS: return "options";
        case SCR_RECORDS: return "records";
        default: return "title";
        }
    if (app == APP_QUIT) return "quit";
    return "none";
}

/* ================================================================ menu script (-mscript FILE)
   One command per line, '#' comments, ';' separates commands on a line. Commands run in order on the 120 Hz sim
   clock (every app state, also while a flight runs):
     wait S                        pause the script S seconds
     key NAME [N]                  press a menu key N times (0.15 s apart): up down left right ok back del pause
                                   (pause = the pause action in a flight)
     bind KEYNAME                  answer the open key capture with this key (e.g. bind G, bind ESC = cancel)
     setcfg SEC KEY VALUE          set a param live + save it (like a menu change)
     arg start air|runway          start position of the next flight (default runway)
     fly KEY on|off                hold / release a flight key in a flight (UP DOWN LEFT RIGHT A Z SPACE D F C V)
     robot on|off                  the robot (T14) flies the player in TRAINING / SORTIE flights started after this
     reload                        re-read the saved ini (params + [controls]) as a fresh start would
     expect screen NAME            title fly options records pause results flight loading
     expect tab NAME               controls video sound
     expect item LABEL             the cursor is on this item (underscores = spaces: RESTART_ON_RUNWAY)
     expect cfg SEC KEY VALUE      the SAVED file has this value
     expect param SEC KEY VALUE    the live param has this value
     expect bind ACTION KEYNAME    [controls] action (pitchUp, fire ...) slot 0 is this key (e.g. G, SPACE, none)
     expect outcome NAME           mission outcome running|complete|failed
     expect fps MIN                average fps of the last 64 rendered frames >= MIN
     expect msg TEXT               the last menu message contains TEXT (underscores = spaces)
     waitfor screen|outcome NAME T wait up to T s for it (FAIL on timeout)
     shot FILE.bmp                 screenshot of the next rendered frame (exe-relative; %W = screen width)
     log TEXT                      "mscript: log ..." line
     quit                          leave the game (also 5 s after the last command)
   Output: "test: mscript expect ok|FAIL line= ..." per check, "test: mscript done checks= fail=" at the end; the exit
   code is the failure count. While a script runs, settings are saved to t19_jet.ini (a copy of the running ini). */
enum { MS_WAIT, MS_KEY, MS_BIND, MS_SETCFG, MS_ARG, MS_FLY, MS_ROBOT, MS_RELOAD, MS_EXPECT, MS_WAITFOR, MS_SHOT, MS_LOG, MS_QUIT };
enum { MS_MAX = 400 };
typedef struct MCmd { int op, line, count; float num; char a[4][48]; char tail[100]; } MCmd;
static MCmd *s_ms;
static int   s_msN, s_msPc, s_msWait, s_msRep, s_msWaitT, s_msEnd, s_msChecks, s_msFails, s_msDone;
static char  s_msShot[E_PATH]; static int s_msShotDue;
static u32   s_msTick;
static int   s_msRobot;
static PilotKeys s_msKeys;
static int   s_msApp, s_msPaused;

static int mk_from(const char *s)
{
    if (!e_stricmp(s, "up")) return MK_UP;
    if (!e_stricmp(s, "down")) return MK_DOWN;
    if (!e_stricmp(s, "left")) return MK_LEFT;
    if (!e_stricmp(s, "right")) return MK_RIGHT;
    if (!e_stricmp(s, "ok") || !e_stricmp(s, "enter")) return MK_OK;
    if (!e_stricmp(s, "back") || !e_stricmp(s, "esc")) return MK_BACK;
    if (!e_stricmp(s, "del") || !e_stricmp(s, "delete")) return MK_DEL;
    if (!e_stricmp(s, "pause")) return MK_PAUSE;
    return -1;
}

int menu_script_load(const char *path)
{
    char *buf, *p, *ln, *w[16], src[E_PATH];
    int size = 0, line = 0, n, k, bad = 0;
    buf = (char *)file_load(path, &size);
    if (!buf) { log_printf(LOG_ERR, "err: mscript cannot read path=%s", path); s_msFails++; s_msChecks++; s_scriptOn = 1; return 0; }
    s_ms = (MCmd *)calloc(MS_MAX, sizeof(MCmd));
    if (!s_ms) { free(buf); return 0; }
    s_msN = 0;
    p = buf;
    while ((ln = next_line(&p)) != NULL) {
        char *cmd = ln, *semi, *h;
        line++;
        h = strchr(ln, '#');
        if (h) *h = 0;
        while (cmd && *cmd) {
            MCmd *c;
            int op = -1;
            semi = strchr(cmd, ';');
            if (semi) *semi++ = 0;
            n = split_words(cmd, w, 16);
            cmd = semi;
            if (n <= 0) continue;
            if (!e_stricmp(w[0], "wait")) op = MS_WAIT;
            else if (!e_stricmp(w[0], "key")) op = MS_KEY;
            else if (!e_stricmp(w[0], "bind")) op = MS_BIND;
            else if (!e_stricmp(w[0], "setcfg")) op = MS_SETCFG;
            else if (!e_stricmp(w[0], "arg")) op = MS_ARG;
            else if (!e_stricmp(w[0], "fly")) op = MS_FLY;
            else if (!e_stricmp(w[0], "robot")) op = MS_ROBOT;
            else if (!e_stricmp(w[0], "reload")) op = MS_RELOAD;
            else if (!e_stricmp(w[0], "expect")) op = MS_EXPECT;
            else if (!e_stricmp(w[0], "waitfor")) op = MS_WAITFOR;
            else if (!e_stricmp(w[0], "shot")) op = MS_SHOT;
            else if (!e_stricmp(w[0], "log")) op = MS_LOG;
            else if (!e_stricmp(w[0], "quit")) op = MS_QUIT;
            if (op < 0 || s_msN >= MS_MAX) { log_printf(LOG_ERR, "err: mscript bad line=%d cmd=%s", line, w[0]); bad++; continue; }
            c = &s_ms[s_msN++];
            c->op = op;
            c->line = line;
            c->count = 1;
            for (k = 1; k < n && k <= 4; k++) e_strlcpy(c->a[k - 1], w[k], 48);
            if (op == MS_WAIT) e_parse_float(c->a[0], &c->num);
            if (op == MS_WAITFOR) e_parse_float(c->a[2], &c->num);
            if (op == MS_EXPECT && !e_stricmp(c->a[0], "fps")) e_parse_float(c->a[1], &c->num);
            if (op == MS_KEY && c->a[1][0]) { k = atoi(c->a[1]); c->count = k > 0 ? k : 1; }
            {   /* value of setcfg / expect cfg|param = the rest of the line (may contain spaces) */
                int first = op == MS_SETCFG ? 3 : (op == MS_EXPECT && (!e_stricmp(c->a[0], "cfg") || !e_stricmp(c->a[0], "param")) ? 4 : n);
                for (k = first; k < n; k++) {
                    if (k > first) e_strlcpy(c->tail + strlen(c->tail), " ", (int)sizeof(c->tail) - (int)strlen(c->tail));
                    e_strlcpy(c->tail + strlen(c->tail), w[k], (int)sizeof(c->tail) - (int)strlen(c->tail));
                }
            }
        }
    }
    free(buf);
    s_scriptOn = 1;
    s_msPc = 0;
    /* settings go to t19_jet.ini, a copy of the running ini (unless that already is the file) */
    e_strlcpy(src, s_iniPath, (int)sizeof(src));
    base_path(s_savePath, (int)sizeof(s_savePath), "t19_jet.ini");
    if (e_stricmp(src, s_savePath)) {
        int sz = 0;
        void *d = file_load(src, &sz);
        if (d) { file_save(s_savePath, d, sz); free(d); }
    }
    {
        char rp[E_PATH];
        base_path(rp, (int)sizeof(rp), "t19_records.ini");
        records_set_path(rp);                       /* tests never touch the player's records.ini */
    }
    log_printf(LOG_INFO, "mscript: load path=%s commands=%d bad=%d save=%s records=t19_records.ini", path, s_msN, bad, s_savePath);
    if (bad) { s_msFails += bad; s_msChecks += bad; }
    return 1;
}

int menu_script_active(void) { return s_scriptOn; }
int menu_script_hit(int mk) { return (s_scriptOn && mk >= 0 && mk < MK_COUNT) ? s_inj[mk] : 0; }
int menu_script_robot(void) { return s_scriptOn && s_msRobot; }

void menu_script_fly_keys(PilotKeys *k)
{
    if (!s_scriptOn) return;
    k->up |= s_msKeys.up; k->down |= s_msKeys.down; k->left |= s_msKeys.left; k->right |= s_msKeys.right;
    k->fast |= s_msKeys.fast; k->slow |= s_msKeys.slow; k->fire |= s_msKeys.fire; k->wpnNext |= s_msKeys.wpnNext;
    k->flare |= s_msKeys.flare; k->view |= s_msKeys.view; k->look |= s_msKeys.look;
}

int menu_script_take_shot(char *path, int n)
{
    if (!s_msShotDue) return 0;
    s_msShotDue = 0;
    e_strlcpy(path, s_msShot, n);
    return 1;
}

void menu_script_note_fps(float ms)
{
    if (!s_scriptOn || ms <= 0.0f) return;
    s_fpsMs[s_fpsPos] = ms;
    s_fpsPos = (s_fpsPos + 1) % 64;
    if (s_fpsN < 64) s_fpsN++;
}

static void check(const MCmd *c, int ok, const char *what, const char *got)
{
    s_msChecks++;
    if (!ok) s_msFails++;
    log_printf(LOG_INFO, "test: mscript expect %s line=%d %s %s %s %s got=\"%s\" t=%.2f", ok ? "ok" : "FAIL", c->line, what,
               c->a[1], c->a[2], c->tail[0] ? c->tail : c->a[3], got, (double)s_msTick / SIM_HZ);
}

static void unders(char *s) { for (; *s; s++) if (*s == '_') *s = ' '; }

static int cond_true(const MCmd *c, char *got, int n)
{
    const char *what = c->a[0];
    char want[100];
    e_strlcpy(want, c->a[1], (int)sizeof(want));
    unders(want);
    if (!e_stricmp(what, "screen")) { e_strlcpy(got, menu_screen_name(s_msApp, s_msPaused), n); return !e_stricmp(got, want); }
    if (!e_stricmp(what, "tab")) {
        e_strlcpy(got, s_tab == TAB_CONTROLS ? "controls" : (s_tab == TAB_VIDEO ? "video" : "sound"), n);
        return !e_stricmp(got, want);
    }
    if (!e_stricmp(what, "item")) {
        MItem it[MAX_ITEMS];
        int scr = s_scr, cnt, cur;
        if (s_msApp == APP_RESULTS) scr = SCR_RESULTS;
        cnt = build(scr, it);
        cur = s_cur[scr];
        e_strlcpy(got, (cur >= 0 && cur < cnt) ? it[cur].label : "-", n);
        return !e_stricmp(got, want);
    }
    if (!e_stricmp(what, "cfg")) {
        Cfg *f = cfg_load_section(s_savePath, c->a[1]);
        e_strlcpy(got, cfg_str(f, c->a[1], c->a[2], "(missing)"), n);
        cfg_free(f);
        return !e_stricmp(got, c->tail);
    }
    if (!e_stricmp(what, "param")) {
        const Param *p = params_find(c->a[1], c->a[2]);
        float a = 0.0f, b = 0.0f;
        if (!p) { e_strlcpy(got, "(no param)", n); return 0; }
        params_value_str(p, got, n);
        if (e_parse_float(got, &a) && e_parse_float(c->tail, &b)) return (float)fabs(a - b) < 1e-4f;
        return !e_stricmp(got, c->tail);
    }
    if (!e_stricmp(what, "bind")) {
        int i;
        for (i = 0; i < NBIND; i++) if (!e_stricmp(s_bindName[i], c->a[1])) break;
        if (i >= NBIND || s_bindAct[i] < 0) { e_strlcpy(got, "(no action)", n); return 0; }
        key_label(in_binding(s_bindAct[i], 0), got, n);
        if (!strcmp(got, "---")) e_strlcpy(got, "none", n);
        e_strlcpy(want, c->a[2], (int)sizeof(want));
        return !e_stricmp(got, want);
    }
    if (!e_stricmp(what, "outcome")) { e_strlcpy(got, mission_outcome_name(mission_stats()->outcome), n); return !e_stricmp(got, want); }
    if (!e_stricmp(what, "fps")) {
        float sum = 0.0f, fps;
        int i;
        for (i = 0; i < s_fpsN; i++) sum += s_fpsMs[i];
        fps = (s_fpsN > 0 && sum > 0.0f) ? 1000.0f * (float)s_fpsN / sum : 0.0f;
        e_snprintf(got, n, "%.2f frames=%d", (double)fps, s_fpsN);
        return s_fpsN >= 16 && fps >= c->num;
    }
    if (!e_stricmp(what, "msg")) {
        e_strlcpy(got, s_msg, n);
        return strstr(s_msg, want) != NULL;
    }
    e_strlcpy(got, "unknown", n);
    return 0;
}

static int fly_key(const char *k, int on)
{
    if (!e_stricmp(k, "UP")) s_msKeys.up = on;
    else if (!e_stricmp(k, "DOWN")) s_msKeys.down = on;
    else if (!e_stricmp(k, "LEFT")) s_msKeys.left = on;
    else if (!e_stricmp(k, "RIGHT")) s_msKeys.right = on;
    else if (!e_stricmp(k, "A")) s_msKeys.fast = on;
    else if (!e_stricmp(k, "Z")) s_msKeys.slow = on;
    else if (!e_stricmp(k, "SPACE")) s_msKeys.fire = on;
    else if (!e_stricmp(k, "D")) s_msKeys.wpnNext = on;
    else if (!e_stricmp(k, "F")) s_msKeys.flare = on;
    else if (!e_stricmp(k, "C")) s_msKeys.view = on;
    else if (!e_stricmp(k, "V")) s_msKeys.look = on;
    else return 0;
    return 1;
}

int menu_script_tick(int app, int paused)
{
    char got[100];
    int guard = 0;
    if (!s_scriptOn) return 0;
    memset(s_inj, 0, sizeof(s_inj));
    s_msApp = app;
    s_msPaused = paused;
    s_msTick++;
    if (s_msWait > 0) { s_msWait--; return 0; }
    if (s_msShotDue) return 0;                      /* a requested screenshot is taken before anything else happens */
    while (s_msPc < s_msN && guard++ < 64) {
        MCmd *c = &s_ms[s_msPc];
        switch (c->op) {
        case MS_WAIT:
            s_msWait = e_ftoi(c->num * (float)SIM_HZ);
            s_msPc++;
            log_printf(LOG_INFO, "mscript: line=%d wait %.2f screen=%s", c->line, (double)c->num, menu_screen_name(app, paused));
            return 0;
        case MS_KEY: {
            int k = mk_from(c->a[0]);
            if (k < 0) { check(c, 0, "key", c->a[0]); s_msPc++; break; }
            s_inj[k] = 1;
            log_printf(LOG_INFO, "mscript: line=%d key %s screen=%s", c->line, c->a[0], menu_screen_name(app, paused));
            if (++s_msRep >= c->count) { s_msRep = 0; s_msPc++; }
            s_msWait = 18;
            return 0; }
        case MS_BIND: {
            InBinding b;
            char spec[64];
            e_snprintf(spec, (int)sizeof(spec), "key %s", c->a[0]);
            if (in_binding_parse(spec, &b) && b.kind == IN_KEY) { s_capInjVk = b.code; s_capInj = 1; }
            else check(c, 0, "bind", c->a[0]);
            log_printf(LOG_INFO, "mscript: line=%d bind %s capture=%d", c->line, c->a[0], s_cap >= 0);
            s_msPc++;
            s_msWait = 12;
            return 0; }
        case MS_SETCFG:
            setting_set(c->a[0], c->a[1], c->tail);
            log_printf(LOG_INFO, "mscript: line=%d setcfg %s %s %s", c->line, c->a[0], c->a[1], c->tail);
            s_msPc++;
            break;
        case MS_ARG:
            if (!e_stricmp(c->a[0], "start")) g_args.start = !e_stricmp(c->a[1], "air") ? START_AIR : START_RUNWAY;
            log_printf(LOG_INFO, "mscript: line=%d arg %s %s", c->line, c->a[0], c->a[1]);
            s_msPc++;
            break;
        case MS_FLY:
            if (!fly_key(c->a[0], !e_stricmp(c->a[1], "on"))) check(c, 0, "fly", c->a[0]);
            log_printf(LOG_INFO, "mscript: line=%d fly %s %s", c->line, c->a[0], c->a[1]);
            s_msPc++;
            break;
        case MS_ROBOT:
            s_msRobot = !e_stricmp(c->a[0], "on");
            log_printf(LOG_INFO, "mscript: line=%d robot %d", c->line, s_msRobot);
            s_msPc++;
            break;
        case MS_RELOAD: {
            Cfg *f = cfg_load(s_savePath);
            int ch = params_load(f);
            cfg_free(f);
            menu_load_controls(s_savePath);
            log_printf(LOG_INFO, "mscript: line=%d reload path=%s changed=%d", c->line, s_savePath, ch);
            s_msPc++;
            break; }
        case MS_EXPECT:
            check(c, cond_true(c, got, (int)sizeof(got)), c->a[0], got);
            s_msPc++;
            break;
        case MS_WAITFOR:
            if (cond_true(c, got, (int)sizeof(got))) {
                log_printf(LOG_INFO, "mscript: line=%d waitfor %s %s after %.2f s", c->line, c->a[0], c->a[1], (double)s_msWaitT / SIM_HZ);
                s_msWaitT = 0;
                s_msPc++;
                break;
            }
            if (++s_msWaitT > e_ftoi(c->num * (float)SIM_HZ)) {
                check(c, 0, "waitfor", got);
                s_msWaitT = 0;
                s_msPc++;
                break;
            }
            return 0;
        case MS_SHOT: {                             /* "%W" in the name = screen width (one script, two resolutions) */
            const char *w = strstr(c->a[0], "%W");
            if (w) {
                char head[48];
                int hl = (int)(w - c->a[0]);
                e_strlcpy(head, c->a[0], hl + 1 < (int)sizeof(head) ? hl + 1 : (int)sizeof(head));
                e_snprintf(s_msShot, (int)sizeof(s_msShot), "%s%d%s", head, s_sw, w + 2);
            } else e_strlcpy(s_msShot, c->a[0], (int)sizeof(s_msShot));
            s_msShotDue = 1;
            s_msPc++;
            s_msWait = 6;
            return 0; }
        case MS_LOG:
            log_printf(LOG_INFO, "mscript: log line=%d %s %s %s %s screen=%s", c->line, c->a[0], c->a[1], c->a[2], c->a[3],
                       menu_screen_name(app, paused));
            s_msPc++;
            break;
        case MS_QUIT:
            log_printf(LOG_INFO, "mscript: line=%d quit", c->line);
            s_msPc = s_msN;
            return 1;
        default:
            s_msPc++;
            break;
        }
    }
    if (s_msPc >= s_msN && ++s_msEnd > 5 * SIM_HZ) return 1;
    return 0;
}

int menu_script_finish(void)
{
    if (!s_scriptOn || s_msDone) return s_msFails;
    s_msDone = 1;
    if (s_msPc < s_msN) {
        log_printf(LOG_INFO, "test: mscript expect FAIL line=%d unfinished commands=%d", s_ms[s_msPc].line, s_msN - s_msPc);
        s_msFails++;
        s_msChecks++;
    }
    log_printf(LOG_INFO, "test: mscript done checks=%d fail=%d", s_msChecks, s_msFails);
    free(s_ms);
    s_ms = NULL;
    return s_msFails;
}
