/* e_input.cpp - keyboard, DirectInput joysticks (winmm fallback), action bindings (DESIGN 2.5, 5.5). Windows.
   Owner: T08. See e_input.h for the semantics.
   - dinput.dll is loaded at run time (no dinput.lib / dxguid.lib, GUIDs defined here under our own names). We use
     IDirectInputA (DIRECTINPUT_VERSION 0x0500), IDirectInputDeviceA + IDirectInputDevice2A (Poll) and our own copy
     of c_dfDIJoystick, so nothing from the DirectX libraries is linked.
   - Joystick reads: DirectInput GetDeviceState (after Poll), re-Acquire on DIERR_INPUTLOST / NOTACQUIRED (retry at
     most every 500 ms), winmm joyGetPosEx otherwise. If the first 120 joystick reads average > 1 ms (analogue game
     ports time their axes), joysticks are read every 2nd poll (logged). */
#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0500
#include <windows.h>
#include <mmsystem.h>
#include <dinput.h>

#include "e_input.h"
#include "e_log.h"
#include "e_plat.h"

/* ---------------------------------------------------------------- GUIDs (values from VC6 DINPUT.H) */
static const GUID s_guidX   = { 0xA36D02E0, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID s_guidY   = { 0xA36D02E1, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID s_guidZ   = { 0xA36D02E2, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID s_guidRx  = { 0xA36D02F4, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID s_guidRy  = { 0xA36D02F5, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID s_guidRz  = { 0xA36D02E3, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID s_guidSl  = { 0xA36D02E4, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID s_guidPov = { 0xA36D02F2, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID s_iidDev2 = { 0x5944E682, 0xC92E, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

#define E_DIDFT_OPTIONAL 0x80000000   /* DX6 DIDFT_OPTIONAL; the DX5 c_dfDIJoystick uses the same bit */
#define AXF (DIDFT_AXIS | DIDFT_ANYINSTANCE | E_DIDFT_OPTIONAL)
#define POVF (DIDFT_POV | DIDFT_ANYINSTANCE | E_DIDFT_OPTIONAL)
#define BTNF (DIDFT_BUTTON | DIDFT_ANYINSTANCE | E_DIDFT_OPTIONAL)
#define JOFS(f) ((DWORD)FIELD_OFFSET(DIJOYSTATE, f))
#define BTN(n) { NULL, JOFS(rgbButtons) + (n), BTNF, 0 }

static DIOBJECTDATAFORMAT s_joyObjs[] = {
    { &s_guidX,  JOFS(lX),  AXF, 0 }, { &s_guidY,  JOFS(lY),  AXF, 0 }, { &s_guidZ,  JOFS(lZ),  AXF, 0 },
    { &s_guidRx, JOFS(lRx), AXF, 0 }, { &s_guidRy, JOFS(lRy), AXF, 0 }, { &s_guidRz, JOFS(lRz), AXF, 0 },
    { &s_guidSl, JOFS(rglSlider), AXF, 0 }, { &s_guidSl, JOFS(rglSlider) + 4, AXF, 0 },
    { &s_guidPov, JOFS(rgdwPOV), POVF, 0 }, { &s_guidPov, JOFS(rgdwPOV) + 4, POVF, 0 },
    { &s_guidPov, JOFS(rgdwPOV) + 8, POVF, 0 }, { &s_guidPov, JOFS(rgdwPOV) + 12, POVF, 0 },
    BTN(0),  BTN(1),  BTN(2),  BTN(3),  BTN(4),  BTN(5),  BTN(6),  BTN(7),
    BTN(8),  BTN(9),  BTN(10), BTN(11), BTN(12), BTN(13), BTN(14), BTN(15),
    BTN(16), BTN(17), BTN(18), BTN(19), BTN(20), BTN(21), BTN(22), BTN(23),
    BTN(24), BTN(25), BTN(26), BTN(27), BTN(28), BTN(29), BTN(30), BTN(31)
};
static DIDATAFORMAT s_joyFormat = {
    sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, sizeof(DIJOYSTATE),
    sizeof(s_joyObjs) / sizeof(s_joyObjs[0]), s_joyObjs
};
static const DWORD s_axisOfs[8] = { JOFS(lX), JOFS(lY), JOFS(lZ), JOFS(lRx), JOFS(lRy), JOFS(lRz),
                                    JOFS(rglSlider), JOFS(rglSlider) + 4 };

typedef HRESULT (WINAPI *DICreateFn)(HINSTANCE, DWORD, LPDIRECTINPUTA *, LPUNKNOWN);

/* ---------------------------------------------------------------- state */
enum { JB_NONE, JB_DINPUT, JB_WINMM, JB_FAKE };

typedef struct Joy {
    int backend;
    char name[64];
    LPDIRECTINPUTDEVICEA dev;
    LPDIRECTINPUTDEVICE2A dev2;
    int acquired;
    DWORD retryMs;
    UINT mmId;
    int mmMin[6], mmMax[6];         /* winmm X Y Z R U V ranges */
    int axesMask, nButtons, nPovs;
    int fails;
    JoyState st;
    JoyState fakeNext;              /* JB_FAKE: state applied by the next poll (so edges work like real reads) */
} Joy;

typedef struct Action { char name[24]; InBinding b[IN_SLOTS]; float val, prevVal, axis;
                        int held, rel;  /* jet T01: polls held (>= 0.5), release event = ticks held before */ } Action;

static int      s_init;
static HWND     s_hwnd;
static HMODULE  s_diDll;
static LPDIRECTINPUTA s_di;
static Joy      s_joy[IN_MAX_JOYS];
static int      s_numJoy;
static JoyState s_noJoy;
static int      s_joyEvery = 1, s_joyTick;
static int      s_joyReads;
static s64      s_joyFirstUs;

static u8       s_watch[256], s_down[256], s_prev[256], s_hit[256];
static int      s_watchAll, s_focusGate = 1, s_gated;

static Action   s_act[IN_MAX_ACTIONS];

static int      s_capturing, s_capArm, s_capInvert;
static JoyState s_capBase[IN_MAX_JOYS];
static JoyState s_prevJoy[IN_MAX_JOYS];

static InStats  s_stats;

static const char *const s_axisNames[8] = { "x", "y", "z", "rx", "ry", "rz", "s0", "s1" };
static const char *const s_povNames[4] = { "up", "right", "down", "left" };

/* ---------------------------------------------------------------- key names */
typedef struct KeyName { int vk; const char *name; } KeyName;
static const KeyName s_keyNames[] = {
    { VK_BACK, "BACK" }, { VK_TAB, "TAB" }, { VK_RETURN, "ENTER" }, { VK_SHIFT, "SHIFT" }, { VK_CONTROL, "CTRL" },
    { VK_MENU, "ALT" }, { VK_PAUSE, "PAUSE" }, { VK_CAPITAL, "CAPS" }, { VK_ESCAPE, "ESC" }, { VK_SPACE, "SPACE" },
    { VK_PRIOR, "PGUP" }, { VK_NEXT, "PGDN" }, { VK_END, "END" }, { VK_HOME, "HOME" }, { VK_LEFT, "LEFT" },
    { VK_UP, "UP" }, { VK_RIGHT, "RIGHT" }, { VK_DOWN, "DOWN" }, { VK_INSERT, "INS" }, { VK_DELETE, "DEL" },
    { VK_NUMPAD0, "NUM0" }, { VK_NUMPAD1, "NUM1" }, { VK_NUMPAD2, "NUM2" }, { VK_NUMPAD3, "NUM3" },
    { VK_NUMPAD4, "NUM4" }, { VK_NUMPAD5, "NUM5" }, { VK_NUMPAD6, "NUM6" }, { VK_NUMPAD7, "NUM7" },
    { VK_NUMPAD8, "NUM8" }, { VK_NUMPAD9, "NUM9" }, { VK_MULTIPLY, "NUMMUL" }, { VK_ADD, "NUMADD" },
    { VK_SUBTRACT, "NUMSUB" }, { VK_DECIMAL, "NUMDOT" }, { VK_DIVIDE, "NUMDIV" },
    { VK_F1, "F1" }, { VK_F2, "F2" }, { VK_F3, "F3" }, { VK_F4, "F4" }, { VK_F5, "F5" }, { VK_F6, "F6" },
    { VK_F7, "F7" }, { VK_F8, "F8" }, { VK_F9, "F9" }, { VK_F10, "F10" }, { VK_F11, "F11" }, { VK_F12, "F12" },
    { VK_NUMLOCK, "NUMLOCK" }, { VK_SCROLL, "SCROLL" },
    { 0xA0, "LSHIFT" }, { 0xA1, "RSHIFT" }, { 0xA2, "LCTRL" }, { 0xA3, "RCTRL" }, { 0xA4, "LALT" }, { 0xA5, "RALT" },
    { 0xBA, "SEMICOLON" }, { 0xBB, "EQUALS" }, { 0xBC, "COMMA" }, { 0xBD, "MINUS" }, { 0xBE, "PERIOD" },
    { 0xBF, "SLASH" }, { 0xC0, "GRAVE" }, { 0xDB, "LBRACKET" }, { 0xDC, "BACKSLASH" }, { 0xDD, "RBRACKET" },
    { 0xDE, "QUOTE" }
};

const char *in_key_name(int vk)
{
    static char one[36][2];
    int i;
    if (vk >= 'A' && vk <= 'Z') { i = vk - 'A'; one[i][0] = (char)vk; one[i][1] = 0; return one[i]; }
    if (vk >= '0' && vk <= '9') { i = 26 + vk - '0'; one[i][0] = (char)vk; one[i][1] = 0; return one[i]; }
    for (i = 0; i < E_COUNT(s_keyNames); i++)
        if (s_keyNames[i].vk == vk) return s_keyNames[i].name;
    return NULL;
}

static int key_from_name(const char *s)
{
    int i, v;
    if (!s || !s[0]) return 0;
    if (!s[1]) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    for (i = 0; i < E_COUNT(s_keyNames); i++)
        if (!e_stricmp(s_keyNames[i].name, s)) return s_keyNames[i].vk;
    if (e_parse_int(s, &v) && v > 0 && v < 256) return v;
    return 0;
}

/* ---------------------------------------------------------------- binding text */
int in_binding_str(const InBinding *b, char *out, int n)
{
    const char *kn;
    int joy, sub, r = 0;
    if (!b || !out || n <= 0) return 0;
    joy = IN_CODE_JOY(b->code);
    sub = IN_CODE_SUB(b->code);
    switch (b->kind) {
    case IN_KEY:
        kn = in_key_name(b->code & 0xFF);
        if (kn) r = e_snprintf(out, n, "key %s", kn);
        else r = e_snprintf(out, n, "key 0x%02X", b->code & 0xFF);
        break;
    case IN_JOYBTN:
        r = e_snprintf(out, n, "j%d b%d%s", joy, sub, b->invert ? " inv" : "");
        break;
    case IN_JOYAXIS:
        r = e_snprintf(out, n, "j%d %s%s%s", joy, s_axisNames[(sub >> 2) & 7],
                       (sub & 3) == IN_AXIS_POS ? "+" : (sub & 3) == IN_AXIS_NEG ? "-" : "", b->invert ? " inv" : "");
        break;
    case IN_JOYPOV:
        r = e_snprintf(out, n, "j%d pov %s", joy, s_povNames[sub & 3]);
        break;
    default:
        r = e_snprintf(out, n, "none");
        break;
    }
    return r < 0 ? 0 : 1;
}

/* split into up to 4 whitespace-separated tokens (lower-cased copies) */
static int tokens(const char *s, char tok[4][16])
{
    int nt = 0, k;
    while (*s && nt < 4) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        k = 0;
        while (*s && *s != ' ' && *s != '\t') {
            char c = *s++;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (k < 15) tok[nt][k++] = c;
        }
        tok[nt][k] = 0;
        nt++;
    }
    while (*s == ' ' || *s == '\t') s++;
    return *s ? -1 : nt;              /* -1: too many tokens */
}

int in_binding_parse(const char *s, InBinding *out)
{
    char tok[4][16];
    int nt, joy, v, i, len, mode, inv = 0;
    if (!s || !out) return 0;
    memset(out, 0, sizeof(*out));
    nt = tokens(s, tok);
    if (nt <= 0) return 0;
    if (nt == 1 && !strcmp(tok[0], "none")) return 1;
    if (!strcmp(tok[0], "key")) {
        if (nt != 2) return 0;
        v = key_from_name(tok[1]);
        if (!v) return 0;
        out->kind = IN_KEY; out->code = v;
        return 1;
    }
    if (tok[0][0] != 'j' || !e_parse_int(tok[0] + 1, &joy) || joy < 0 || joy >= IN_MAX_JOYS) return 0;
    if (nt >= 3 && !strcmp(tok[nt - 1], "inv")) { inv = 1; nt--; }
    if (nt == 3 && !strcmp(tok[1], "pov")) {
        for (i = 0; i < 4; i++) if (!strcmp(tok[2], s_povNames[i])) break;
        if (i == 4 || inv) return 0;
        out->kind = IN_JOYPOV; out->code = IN_JOYCODE(joy, i);
        return 1;
    }
    if (nt != 2) return 0;
    if (tok[1][0] == 'b' && e_parse_int(tok[1] + 1, &v)) {
        if (v < 0 || v > 31) return 0;
        out->kind = IN_JOYBTN; out->code = IN_JOYCODE(joy, v); out->invert = inv;
        return 1;
    }
    len = (int)strlen(tok[1]);
    mode = IN_AXIS_FULL;
    if (len > 1 && tok[1][len - 1] == '+') { mode = IN_AXIS_POS; tok[1][--len] = 0; }
    else if (len > 1 && tok[1][len - 1] == '-') { mode = IN_AXIS_NEG; tok[1][--len] = 0; }
    for (i = 0; i < 8; i++) if (!strcmp(tok[1], s_axisNames[i])) break;
    if (i == 8) return 0;
    out->kind = IN_JOYAXIS; out->code = IN_AXISCODE(joy, i, mode); out->invert = inv;
    return 1;
}

/* ---------------------------------------------------------------- actions + bindings */
static const char *const s_defNames[IA_COUNT] = {
    "steer", "left", "right", "throttle", "brake", "handbrake", "shiftUp", "shiftDown",
    "camera", "lookBack", "reset", "pause", "menuUp", "menuDown", "menuLeft", "menuRight", "menuOk", "menuBack"
};
/* defaults: slot 0 key, slot 1 alternative key, slot 2 controller (TASKS T08; A/Z shifting -> E/Q because WASD
   steering uses A, approved by the orchestrator 2026-11-12) */
static const char *const s_defBinds[IA_COUNT][IN_SLOTS] = {
    { "none",      "none",      "j0 x" },          /* steer */
    { "key LEFT",  "key A",     "j0 pov left" },   /* left */
    { "key RIGHT", "key D",     "j0 pov right" },  /* right */
    { "key UP",    "key W",     "j0 y-" },         /* throttle */
    { "key DOWN",  "key S",     "j0 y+" },         /* brake */
    { "key SPACE", "none",      "j0 b2" },         /* handbrake */
    { "key E",     "none",      "j0 b0" },         /* shiftUp */
    { "key Q",     "none",      "j0 b1" },         /* shiftDown */
    { "key C",     "none",      "j0 b3" },         /* camera */
    { "key B",     "none",      "j0 b4" },         /* lookBack */
    { "key R",     "none",      "j0 b5" },         /* reset */
    { "key ESC",   "key PAUSE", "none" },          /* pause */
    { "key UP",    "j0 pov up",    "j0 y-" },      /* menuUp */
    { "key DOWN",  "j0 pov down",  "j0 y+" },      /* menuDown */
    { "key LEFT",  "j0 pov left",  "j0 x-" },      /* menuLeft */
    { "key RIGHT", "j0 pov right", "j0 x+" },      /* menuRight */
    { "key ENTER", "none",      "j0 b0" },         /* menuOk */
    { "key ESC",   "key BACK",  "j0 b1" }          /* menuBack */
};

static void watch_bound_keys(void)
{
    int a, s;
    for (a = 0; a < IN_MAX_ACTIONS; a++)
        for (s = 0; s < IN_SLOTS; s++)
            if (s_act[a].b[s].kind == IN_KEY) s_watch[s_act[a].b[s].code & 0xFF] = 1;
}

void in_action_define(int action, const char *iniName)
{
    if (action < 0 || action >= IN_MAX_ACTIONS) return;
    e_strlcpy(s_act[action].name, iniName ? iniName : "", (int)sizeof(s_act[action].name));
}

const char *in_action_name(int action)
{
    if (action < 0 || action >= IN_MAX_ACTIONS) return "";
    return s_act[action].name;
}

void in_bind_clear(void)
{
    int a;
    for (a = 0; a < IN_MAX_ACTIONS; a++) memset(s_act[a].b, 0, sizeof(s_act[a].b));
}

void in_bind(int action, int slot, int kind, int code, int invert)
{
    InBinding *b;
    if (action < 0 || action >= IN_MAX_ACTIONS || slot < 0 || slot >= IN_SLOTS) return;
    b = &s_act[action].b[slot];
    if (kind < IN_KEY || kind > IN_JOYPOV) { memset(b, 0, sizeof(*b)); return; }
    b->kind = kind; b->code = code; b->invert = invert ? 1 : 0;
    if (kind == IN_KEY) { b->code &= 0xFF; s_watch[b->code] = 1; }
}

void in_bind_defaults(void)
{
    int a, s;
    InBinding b;
    in_bind_clear();
    for (a = 0; a < IA_COUNT; a++) {
        if (!s_act[a].name[0]) in_action_define(a, s_defNames[a]);
        for (s = 0; s < IN_SLOTS; s++)
            if (in_binding_parse(s_defBinds[a][s], &b)) in_bind(a, s, b.kind, b.code, b.invert);
    }
}

const InBinding *in_binding(int action, int slot)
{
    static InBinding none;
    if (action < 0 || action >= IN_MAX_ACTIONS || slot < 0 || slot >= IN_SLOTS) return &none;
    return &s_act[action].b[slot];
}

void in_bind_load(const Cfg *c)
{
    int a, s, n;
    const char *v, *p;
    char part[64];
    InBinding b;
    if (!c) return;
    for (a = 0; a < IN_MAX_ACTIONS; a++) {
        if (!s_act[a].name[0] || !cfg_has(c, "controls", s_act[a].name)) continue;
        v = cfg_str(c, "controls", s_act[a].name, "");
        for (s = 0; s < IN_SLOTS; s++) {
            n = 0;
            while (*v == ' ') v++;
            p = v;
            while (*p && *p != ',') p++;
            if (p > v || *p == ',') {
                n = (int)(p - v);
                if (n > (int)sizeof(part) - 1) n = (int)sizeof(part) - 1;
                memcpy(part, v, n);
                part[n] = 0;
                while (n > 0 && part[n - 1] == ' ') part[--n] = 0;
                if (!n) in_bind(a, s, 0, 0, 0);    /* ",," = empty slot */
                else if (in_binding_parse(part, &b)) in_bind(a, s, b.kind, b.code, b.invert);
                else log_printf(LOG_WARN, "warn: input bad binding action=%s slot=%d text=\"%s\" (kept default)",
                                s_act[a].name, s, part);
                v = *p == ',' ? p + 1 : p;
            } else {
                in_bind(a, s, 0, 0, 0);            /* fewer entries than slots: the rest is empty */
            }
        }
    }
    watch_bound_keys();
}

void in_bind_save(Cfg *c)
{
    int a, s, used;
    char line[200], one[48];
    if (!c) return;
    for (a = 0; a < IN_MAX_ACTIONS; a++) {
        if (!s_act[a].name[0]) continue;
        used = 0;
        line[0] = 0;
        for (s = 0; s < IN_SLOTS; s++) {
            in_binding_str(&s_act[a].b[s], one, (int)sizeof(one));
            used += e_snprintf(line + used, (int)sizeof(line) - used, "%s%s", s ? ", " : "", one);
            if (used < 0 || used >= (int)sizeof(line)) break;
        }
        cfg_set_str(c, "controls", s_act[a].name, line);
    }
}

/* ---------------------------------------------------------------- binding evaluation */
static const JoyState *joy_live(int j)
{
    if (j < 0 || j >= IN_MAX_JOYS || s_joy[j].backend == JB_NONE || !s_joy[j].st.valid || s_gated) return NULL;
    return &s_joy[j].st;
}

static int pov_dir(int pov, int dir)        /* 1 if pov (hundredths of degrees) presses dir; 45 deg each side */
{
    int d;
    if (pov < 0) return 0;
    d = pov - dir * 9000;
    while (d > 18000) d -= 36000;
    while (d < -18000) d += 36000;
    return d >= -4500 && d <= 4500;
}

/* value 0..1 for in_action; *axis gets the full-axis value (-1..1) and *isAxis = 1 for full-axis bindings */
static float bind_value(const InBinding *b, float *axis, int *isAxis)
{
    const JoyState *js;
    int sub;
    float v;
    *isAxis = 0;
    switch (b->kind) {
    case IN_KEY:
        return s_down[b->code & 0xFF] ? 1.0f : 0.0f;
    case IN_JOYBTN:
        js = joy_live(IN_CODE_JOY(b->code));
        if (!js) return 0.0f;
        return (js->buttons >> (IN_CODE_SUB(b->code) & 31)) & 1 ? 1.0f : 0.0f;
    case IN_JOYPOV:
        js = joy_live(IN_CODE_JOY(b->code));
        if (!js) return 0.0f;
        return pov_dir(js->pov, IN_CODE_SUB(b->code) & 3) ? 1.0f : 0.0f;
    case IN_JOYAXIS:
        js = joy_live(IN_CODE_JOY(b->code));
        if (!js) return 0.0f;
        sub = IN_CODE_SUB(b->code);
        v = js->axis[(sub >> 2) & 7];
        if (b->invert) v = -v;
        switch (sub & 3) {
        case IN_AXIS_POS: return v > 0.0f ? v : 0.0f;
        case IN_AXIS_NEG: return v < 0.0f ? -v : 0.0f;
        }
        *axis = v;
        *isAxis = 1;
        return (v + 1.0f) * 0.5f;
    }
    return 0.0f;
}

static void eval_actions(void)
{
    int a, s, isAxis, tap;
    float v, ax, best, bestAx;
    for (a = 0; a < IN_MAX_ACTIONS; a++) {
        Action *ac = &s_act[a];
        best = 0.0f;
        bestAx = 0.0f;
        for (s = 0; s < IN_SLOTS; s++) {
            if (!ac->b[s].kind) continue;
            ax = 0.0f;
            v = bind_value(&ac->b[s], &ax, &isAxis);
            if (v > best) best = v;
            if (isAxis && (ax > 0.0f ? ax : -ax) > (bestAx > 0.0f ? bestAx : -bestAx)) bestAx = ax;
        }
        ac->prevVal = ac->val;
        ac->val = E_CLAMP(best, 0.0f, 1.0f);
        ac->axis = E_CLAMP(bestAx, -1.0f, 1.0f);
        /* jet T01: hold counter + release event; a key tapped entirely between two polls counts as held 1 tick;
           focus loss (gate) resets the counter without a release event */
        tap = 0;
        for (s = 0; s < IN_SLOTS; s++)
            if (ac->b[s].kind == IN_KEY && s_hit[ac->b[s].code & 0xFF] && !s_down[ac->b[s].code & 0xFF]) tap = 1;
        if (s_gated) { ac->held = 0; ac->rel = 0; }
        else if (ac->val >= 0.5f) { ac->held++; ac->rel = 0; }
        else { ac->rel = ac->held > 0 ? ac->held : tap; ac->held = 0; }
    }
}

float in_action(int action)
{
    if (action < 0 || action >= IN_MAX_ACTIONS) return 0.0f;
    return s_act[action].val;
}

int in_action_hit(int action)
{
    if (action < 0 || action >= IN_MAX_ACTIONS) return 0;
    return s_act[action].val >= 0.5f && s_act[action].prevVal < 0.5f;
}

int in_action_held_ticks(int action)
{
    if (action < 0 || action >= IN_MAX_ACTIONS) return 0;
    return s_act[action].held;
}

int in_action_released(int action)
{
    if (action < 0 || action >= IN_MAX_ACTIONS) return 0;
    return s_act[action].rel;
}

float in_axis(int action)
{
    if (action < 0 || action >= IN_MAX_ACTIONS) return 0.0f;
    return s_act[action].axis;
}

int in_axis_bound(int action)
{
    int s;
    if (action < 0 || action >= IN_MAX_ACTIONS) return 0;
    for (s = 0; s < IN_SLOTS; s++) {
        const InBinding *b = &s_act[action].b[s];
        int j = IN_CODE_JOY(b->code);
        if (b->kind == IN_JOYAXIS && (IN_CODE_SUB(b->code) & 3) == IN_AXIS_FULL && j < IN_MAX_JOYS &&
            s_joy[j].backend != JB_NONE && s_joy[j].st.valid)
            return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- joysticks: DirectInput */
static BOOL CALLBACK di_enum_cb(LPCDIDEVICEINSTANCEA inst, LPVOID ctx)
{
    LPDIRECTINPUTDEVICEA dev = NULL;
    Joy *j;
    HRESULT hr;
    (void)ctx;
    if (s_numJoy >= IN_MAX_JOYS) return DIENUM_STOP;
    hr = s_di->CreateDevice(inst->guidInstance, &dev, NULL);
    if (hr != DI_OK || !dev) {
        log_printf(LOG_WARN, "warn: input dinput CreateDevice failed hr=0x%08lX name=\"%s\"", (unsigned long)hr,
                   inst->tszProductName);
        return DIENUM_CONTINUE;
    }
    j = &s_joy[s_numJoy];
    memset(j, 0, sizeof(*j));
    j->dev = dev;
    e_strlcpy(j->name, inst->tszProductName[0] ? inst->tszProductName : inst->tszInstanceName, (int)sizeof(j->name));
    j->backend = JB_DINPUT;
    s_numJoy++;
    return DIENUM_CONTINUE;
}

static void di_setup(Joy *j)
{
    HRESULT hr;
    int a;
    DIPROPRANGE rg;
    DIPROPDWORD dz;
    DIDEVCAPS caps;
    DIDEVICEOBJECTINSTANCEA doi;
    if (j->dev->QueryInterface(s_iidDev2, (void **)&j->dev2) != S_OK) j->dev2 = NULL;
    hr = j->dev->SetDataFormat(&s_joyFormat);
    if (hr != DI_OK) log_printf(LOG_WARN, "warn: input dinput SetDataFormat hr=0x%08lX", (unsigned long)hr);
    hr = j->dev->SetCooperativeLevel(s_hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    if (hr != DI_OK) log_printf(LOG_WARN, "warn: input dinput SetCooperativeLevel hr=0x%08lX", (unsigned long)hr);
    for (a = 0; a < 8; a++) {
        memset(&doi, 0, sizeof(doi));
        doi.dwSize = sizeof(doi);
        if (j->dev->GetObjectInfo(&doi, s_axisOfs[a], DIPH_BYOFFSET) != DI_OK) continue;
        j->axesMask |= 1 << a;
        memset(&rg, 0, sizeof(rg));
        rg.diph.dwSize = sizeof(rg);
        rg.diph.dwHeaderSize = sizeof(rg.diph);
        rg.diph.dwObj = s_axisOfs[a];
        rg.diph.dwHow = DIPH_BYOFFSET;
        rg.lMin = -1000;
        rg.lMax = 1000;
        hr = j->dev->SetProperty(DIPROP_RANGE, &rg.diph);
        if (hr != DI_OK && hr != DI_PROPNOEFFECT)
            log_printf(LOG_WARN, "warn: input dinput axis=%s range hr=0x%08lX", s_axisNames[a], (unsigned long)hr);
    }
    memset(&dz, 0, sizeof(dz));
    dz.diph.dwSize = sizeof(dz);
    dz.diph.dwHeaderSize = sizeof(dz.diph);
    dz.diph.dwObj = 0;
    dz.diph.dwHow = DIPH_DEVICE;
    dz.dwData = 0;
    j->dev->SetProperty(DIPROP_DEADZONE, &dz.diph);   /* the game applies its own deadzone (DESIGN 5.5) */
    memset(&caps, 0, sizeof(caps));
    caps.dwSize = sizeof(caps);
    if (j->dev->GetCapabilities(&caps) == DI_OK) {
        j->nButtons = (int)caps.dwButtons > 32 ? 32 : (int)caps.dwButtons;
        j->nPovs = (int)caps.dwPOVs;
    }
    hr = j->dev->Acquire();
    j->acquired = hr == DI_OK || hr == S_FALSE;
    if (!j->acquired) log_printf(LOG_WARN, "warn: input dinput Acquire hr=0x%08lX", (unsigned long)hr);
}

static int di_read(Joy *j)
{
    DIJOYSTATE js;
    HRESULT hr;
    int a, b;
    DWORD now;
    if (!j->acquired) {
        now = timeGetTime();
        if ((int)(now - j->retryMs) < 500) return 0;
        j->retryMs = now;
        hr = j->dev->Acquire();
        j->acquired = hr == DI_OK || hr == S_FALSE;
        if (!j->acquired) return 0;
    }
    if (j->dev2) j->dev2->Poll();
    hr = j->dev->GetDeviceState(sizeof(js), &js);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        j->acquired = 0;
        j->retryMs = timeGetTime() - 500;      /* retry right away once */
        return 0;
    }
    if (hr != DI_OK) return 0;
    {
        const LONG *ax[8];
        ax[0] = &js.lX; ax[1] = &js.lY; ax[2] = &js.lZ; ax[3] = &js.lRx; ax[4] = &js.lRy; ax[5] = &js.lRz;
        ax[6] = &js.rglSlider[0]; ax[7] = &js.rglSlider[1];
        for (a = 0; a < 8; a++) {
            float v = (j->axesMask >> a) & 1 ? (float)*ax[a] * 0.001f : 0.0f;
            j->st.axis[a] = E_CLAMP(v, -1.0f, 1.0f);
        }
    }
    j->st.buttons = 0;
    for (b = 0; b < 32; b++) if (js.rgbButtons[b] & 0x80) j->st.buttons |= 1u << b;
    j->st.pov = (j->nPovs > 0 && LOWORD(js.rgdwPOV[0]) != 0xFFFF) ? (int)(js.rgdwPOV[0] % 36000) : -1;
    return 1;
}

/* ---------------------------------------------------------------- joysticks: winmm */
static void mm_scan(void)
{
    UINT n, id;
    JOYINFOEX ji;
    JOYCAPS caps;
    Joy *j;
    n = joyGetNumDevs();
    if (n > 16) n = 16;
    for (id = 0; id < n && s_numJoy < IN_MAX_JOYS; id++) {
        memset(&ji, 0, sizeof(ji));
        ji.dwSize = sizeof(ji);
        ji.dwFlags = JOY_RETURNALL;
        if (joyGetPosEx(id, &ji) != JOYERR_NOERROR) continue;
        memset(&caps, 0, sizeof(caps));
        if (joyGetDevCaps(id, &caps, sizeof(caps)) != JOYERR_NOERROR) continue;
        j = &s_joy[s_numJoy];
        memset(j, 0, sizeof(*j));
        j->backend = JB_WINMM;
        j->mmId = id;
        e_strlcpy(j->name, caps.szPname[0] ? caps.szPname : "winmm joystick", (int)sizeof(j->name));
        j->mmMin[0] = (int)caps.wXmin; j->mmMax[0] = (int)caps.wXmax;
        j->mmMin[1] = (int)caps.wYmin; j->mmMax[1] = (int)caps.wYmax;
        j->mmMin[2] = (int)caps.wZmin; j->mmMax[2] = (int)caps.wZmax;
        j->mmMin[3] = (int)caps.wRmin; j->mmMax[3] = (int)caps.wRmax;
        j->mmMin[4] = (int)caps.wUmin; j->mmMax[4] = (int)caps.wUmax;
        j->mmMin[5] = (int)caps.wVmin; j->mmMax[5] = (int)caps.wVmax;
        j->axesMask = 0x03;
        if (caps.wCaps & JOYCAPS_HASZ) j->axesMask |= 1 << 2;
        if (caps.wCaps & JOYCAPS_HASR) j->axesMask |= 1 << 5;
        if (caps.wCaps & JOYCAPS_HASU) j->axesMask |= 1 << 3;
        if (caps.wCaps & JOYCAPS_HASV) j->axesMask |= 1 << 4;
        j->nButtons = (int)caps.wNumButtons > 32 ? 32 : (int)caps.wNumButtons;
        j->nPovs = (caps.wCaps & JOYCAPS_HASPOV) ? 1 : 0;
        s_numJoy++;
    }
}

static float mm_norm(DWORD pos, int lo, int hi)
{
    float v;
    if (hi <= lo) return 0.0f;
    v = ((float)(int)pos - (float)lo) / (float)(hi - lo) * 2.0f - 1.0f;
    return E_CLAMP(v, -1.0f, 1.0f);
}

static int mm_read(Joy *j)
{
    JOYINFOEX ji;
    memset(&ji, 0, sizeof(ji));
    ji.dwSize = sizeof(ji);
    ji.dwFlags = JOY_RETURNALL;
    if (joyGetPosEx(j->mmId, &ji) != JOYERR_NOERROR) return 0;
    memset(j->st.axis, 0, sizeof(j->st.axis));
    j->st.axis[0] = mm_norm(ji.dwXpos, j->mmMin[0], j->mmMax[0]);
    j->st.axis[1] = mm_norm(ji.dwYpos, j->mmMin[1], j->mmMax[1]);
    if (j->axesMask & (1 << 2)) j->st.axis[2] = mm_norm(ji.dwZpos, j->mmMin[2], j->mmMax[2]);
    if (j->axesMask & (1 << 5)) j->st.axis[5] = mm_norm(ji.dwRpos, j->mmMin[3], j->mmMax[3]);
    if (j->axesMask & (1 << 3)) j->st.axis[3] = mm_norm(ji.dwUpos, j->mmMin[4], j->mmMax[4]);
    if (j->axesMask & (1 << 4)) j->st.axis[4] = mm_norm(ji.dwVpos, j->mmMin[5], j->mmMax[5]);
    j->st.buttons = (u32)ji.dwButtons;
    j->st.pov = (j->nPovs && ji.dwPOV != JOY_POVCENTERED && ji.dwPOV < 36000) ? (int)ji.dwPOV : -1;
    return 1;
}

static void joy_read_all(void)
{
    int i, ok;
    s64 t0, us;
    if (++s_joyTick < s_joyEvery) return;
    s_joyTick = 0;
    t0 = plat_now_us();
    for (i = 0; i < IN_MAX_JOYS; i++) {
        Joy *j = &s_joy[i];
        if (j->backend == JB_NONE) continue;
        if (j->backend == JB_FAKE) { j->st = j->fakeNext; continue; }
        ok = j->backend == JB_DINPUT ? di_read(j) : mm_read(j);
        if (ok) { j->st.valid = 1; j->fails = 0; }
        else if (++j->fails >= 3) {               /* unplugged / lost: read as idle until it comes back */
            if (j->st.valid) log_printf(LOG_WARN, "warn: input joy=%d lost name=\"%s\"", i, j->name);
            memset(&j->st, 0, sizeof(j->st));
            j->st.pov = -1;
        }
    }
    us = plat_now_us() - t0;
    if (s_numJoy > 0) {
        s_stats.joyUsSum += us;
        if (us > s_stats.joyUsMax) s_stats.joyUsMax = (int)us;
        if (s_joyReads < 120) {
            s_joyFirstUs += us;
            if (++s_joyReads == 120 && s_joyFirstUs / 120 > 1000) {
                s_joyEvery = 2;
                log_printf(LOG_INFO, "input: joystick reads avg_us=%d > 1000, reading every 2nd poll",
                           (int)(s_joyFirstUs / 120));
            }
        }
    }
}

/* ---------------------------------------------------------------- init / poll */
int in_init(void *hwnd, int useJoystick)
{
    HRESULT hr;
    DICreateFn create;
    int i, a, b;
    in_shutdown();
    memset(s_joy, 0, sizeof(s_joy));
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_down, 0, sizeof(s_down));
    memset(s_prev, 0, sizeof(s_prev));
    memset(s_hit, 0, sizeof(s_hit));
    s_noJoy.pov = -1;
    s_hwnd = (HWND)hwnd;
    s_numJoy = 0;
    s_joyEvery = 1; s_joyTick = 0; s_joyReads = 0; s_joyFirstUs = 0;
    s_init = 1;
    in_bind_defaults();
    if (!useJoystick) {
        log_printf(LOG_INFO, "input: joystick disabled (-nojoy)");
    } else {
        s_diDll = LoadLibraryA("dinput.dll");
        create = s_diDll ? (DICreateFn)GetProcAddress(s_diDll, "DirectInputCreateA") : NULL;
        if (!create) {
            log_printf(LOG_INFO, "input: dinput.dll not available, using winmm");
        } else if (!s_hwnd) {
            log_printf(LOG_INFO, "input: no window, DirectInput skipped, using winmm");
        } else {
            hr = create((HINSTANCE)plat_hinstance(), 0x0500, &s_di, NULL);
            if (hr != DI_OK || !s_di) {
                log_printf(LOG_WARN, "warn: input DirectInputCreate hr=0x%08lX, using winmm", (unsigned long)hr);
                s_di = NULL;
            } else {
                hr = s_di->EnumDevices(DIDEVTYPE_JOYSTICK, di_enum_cb, NULL, DIEDFL_ATTACHEDONLY);
                log_printf(LOG_INFO, "input: dinput ok enum_hr=0x%08lX joysticks=%d", (unsigned long)hr, s_numJoy);
                for (i = 0; i < s_numJoy; i++) di_setup(&s_joy[i]);
            }
        }
        if (s_numJoy == 0) mm_scan();
    }
    for (i = 0; i < s_numJoy; i++) {
        Joy *j = &s_joy[i];
        char axes[48];
        int used = 0;
        axes[0] = 0;
        for (a = 0; a < 8; a++)
            if ((j->axesMask >> a) & 1) {
                b = e_snprintf(axes + used, (int)sizeof(axes) - used, "%s%s", used ? "," : "", s_axisNames[a]);
                if (b > 0) used += b;
            }
        j->st.pov = -1;
        log_printf(LOG_INFO, "input: joy=%d backend=%s name=\"%s\" axes=%s buttons=%d povs=%d", i,
                   j->backend == JB_DINPUT ? "dinput" : "winmm", j->name, axes, j->nButtons, j->nPovs);
    }
    if (useJoystick && s_numJoy == 0) log_printf(LOG_INFO, "input: no joystick");
    if (s_numJoy) joy_read_all();
    log_printf(LOG_INFO, "input: init ok joysticks=%d", s_numJoy);
    return 1;
}

void in_shutdown(void)
{
    int i;
    for (i = 0; i < IN_MAX_JOYS; i++) {
        Joy *j = &s_joy[i];
        if (j->dev) {
            j->dev->Unacquire();
            if (j->dev2) j->dev2->Release();
            j->dev->Release();
        }
        memset(j, 0, sizeof(*j));
    }
    s_numJoy = 0;
    if (s_di) s_di->Release();
    s_di = NULL;
    if (s_diDll) FreeLibrary(s_diDll);
    s_diDll = NULL;
    s_init = 0;
}

void in_poll(void)
{
    int vk, all;
    SHORT r;
    s64 t0 = plat_now_us(), us;
    s_gated = s_focusGate && s_hwnd && GetForegroundWindow() != s_hwnd;
    all = s_watchAll || s_capturing;
    memcpy(s_prev, s_down, sizeof(s_down));
    for (vk = 1; vk < 256; vk++) {
        if (!s_watch[vk] && !(all && vk >= 8)) { s_down[vk] = 0; s_hit[vk] = 0; continue; }
        r = GetAsyncKeyState(vk);
        s_stats.keyCalls++;
        if (s_gated) { s_down[vk] = 0; s_hit[vk] = 0; continue; }
        s_down[vk] = (r & 0x8000) ? 1 : 0;
        s_hit[vk] = (u8)((s_down[vk] && !s_prev[vk]) || ((r & 1) && !s_prev[vk]));   /* bit 0: tapped between polls */
    }
    {
        int i;
        for (i = 0; i < IN_MAX_JOYS; i++) s_prevJoy[i] = s_joy[i].st;
    }
    if (s_init) joy_read_all();
    eval_actions();
    us = plat_now_us() - t0;
    s_stats.polls++;
    s_stats.pollUsSum += us;
    if (us > s_stats.pollUsMax) s_stats.pollUsMax = (int)us;
    if (s_capArm > 0) s_capArm--;
}

int in_key(int vk)
{
    vk &= 0xFF;
    if (!s_watch[vk]) { s_watch[vk] = 1; return 0; }
    return s_down[vk];
}

int in_key_hit(int vk)
{
    vk &= 0xFF;
    if (!s_watch[vk]) { s_watch[vk] = 1; return 0; }
    return s_hit[vk];
}

int in_joy_count(void) { return s_numJoy; }

const char *in_joy_name(int i)
{
    if (i < 0 || i >= IN_MAX_JOYS || s_joy[i].backend == JB_NONE) return "";
    return s_joy[i].name;
}

const JoyState *in_joy(int i)
{
    if (i < 0 || i >= IN_MAX_JOYS || s_joy[i].backend == JB_NONE) return &s_noJoy;
    return &s_joy[i].st;
}

const char *in_joy_backend(int i)
{
    if (i < 0 || i >= IN_MAX_JOYS) return "";
    switch (s_joy[i].backend) {
    case JB_DINPUT: return "dinput";
    case JB_WINMM: return "winmm";
    case JB_FAKE: return "fake";
    }
    return "";
}

int in_joy_axes(int i) { return (i >= 0 && i < IN_MAX_JOYS) ? s_joy[i].axesMask : 0; }
int in_joy_buttons(int i) { return (i >= 0 && i < IN_MAX_JOYS) ? s_joy[i].nButtons : 0; }

void in_set_focus_gate(int on) { s_focusGate = on ? 1 : 0; }
void in_watch_all(int on) { s_watchAll = on ? 1 : 0; }
void in_stats(InStats *out) { if (out) *out = s_stats; }

void in_debug_set_joy(int i, const JoyState *js, const char *name)
{
    Joy *j;
    if (i < 0 || i >= IN_MAX_JOYS) return;
    j = &s_joy[i];
    if (j->backend != JB_NONE && j->backend != JB_FAKE) return;   /* never replace a real device */
    if (!js) {
        if (j->backend == JB_FAKE) {
            memset(j, 0, sizeof(*j));
            while (s_numJoy > 0 && s_joy[s_numJoy - 1].backend == JB_NONE) s_numJoy--;
        }
        return;
    }
    if (j->backend == JB_NONE) {
        memset(j, 0, sizeof(*j));
        j->backend = JB_FAKE;
        j->axesMask = 0xFF;
        j->nButtons = 32;
        j->nPovs = 1;
        if (i >= s_numJoy) s_numJoy = i + 1;
    }
    if (name || !j->name[0]) e_strlcpy(j->name, name ? name : "fake", (int)sizeof(j->name));
    j->fakeNext = *js;
}

/* ---------------------------------------------------------------- capture (options menu) */
void in_capture_begin(void)
{
    int i;
    s_capturing = 1;
    s_capArm = 2;                     /* skip 2 polls: keys not watched before have no previous state */
    s_capInvert = 0;
    for (i = 0; i < IN_MAX_JOYS; i++) s_capBase[i] = s_joy[i].st;
}

void in_capture_end(void) { s_capturing = 0; }

int in_capture_invert(void) { return s_capInvert; }

int in_capture(int *kind, int *code)
{
    int vk, i, a, b, d;
    if (!s_capturing) { in_capture_begin(); return 0; }
    if (s_capArm > 0 || s_gated) return 0;
    for (vk = 8; vk < 256; vk++) {
        if (!s_hit[vk]) continue;
        if (kind) *kind = IN_KEY;
        if (code) *code = vk;
        s_capInvert = 0;
        s_capturing = 0;
        return 1;
    }
    for (i = 0; i < IN_MAX_JOYS; i++) {
        const JoyState *js = joy_live(i);
        const JoyState *pv = &s_prevJoy[i];
        const JoyState *base = &s_capBase[i];
        if (!js) continue;
        for (b = 0; b < 32; b++)
            if (((js->buttons >> b) & 1) && !((pv->buttons >> b) & 1)) {
                if (kind) *kind = IN_JOYBTN;
                if (code) *code = IN_JOYCODE(i, b);
                s_capInvert = 0; s_capturing = 0;
                return 1;
            }
        for (d = 0; d < 4; d++)
            if (pov_dir(js->pov, d) && !pov_dir(pv->pov, d)) {
                if (kind) *kind = IN_JOYPOV;
                if (code) *code = IN_JOYCODE(i, d);
                s_capInvert = 0; s_capturing = 0;
                return 1;
            }
        for (a = 0; a < 8; a++) {
            float v = js->axis[a], b0 = base->axis[a], dv = v - b0;
            if ((dv > 0.0f ? dv : -dv) < 0.6f) continue;
            if (kind) *kind = IN_JOYAXIS;
            if (b0 > 0.7f || b0 < -0.7f) {        /* rests at an end (pedal): full axis, inverted if it rests at +1 */
                if (code) *code = IN_AXISCODE(i, a, IN_AXIS_FULL);
                s_capInvert = b0 > 0.0f;
            } else {                               /* centred (stick/wheel): the half that moved */
                if (code) *code = IN_AXISCODE(i, a, dv > 0.0f ? IN_AXIS_POS : IN_AXIS_NEG);
                s_capInvert = 0;
            }
            s_capturing = 0;
            return 1;
        }
    }
    return 0;
}
