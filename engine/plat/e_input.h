/* e_input.h - keyboard (GetAsyncKeyState), DirectInput joystick (winmm fallback), action bindings (DESIGN 2.5,
   5.5). Windows side. Owner: T08 (initial header + stub by T01). Key codes are Windows VK_* values.

   Keyboard: in_poll reads GetAsyncKeyState for the "watched" keys only (every bound key, every key ever asked
   through in_key/in_key_hit (its first query returns 0), all keys while in_capture or in_watch_all is active), so a
   poll costs a few dozen calls instead of 256. With a window, keys and joysticks read as idle while another window
   has the focus (in_set_focus_gate(0) turns that off, the self-test does).
   Joysticks: up to IN_MAX_JOYS. DirectInput (dinput.dll loaded at run time, IDirectInput / IDirectInputDevice2,
   DIRECTINPUT_VERSION 0x0500, own data format = DIJOYSTATE, axes set to -1000..1000, deadzone 0: the game applies
   its own); without DirectInput or devices: winmm joyGetPosEx. Axis order in JoyState.axis: 0 X, 1 Y, 2 Z, 3 Rx,
   4 Ry, 5 Rz, 6 slider 0, 7 slider 1 (winmm: X Y Z -> 0 1 2, R -> 5 (rudder), U -> 3, V -> 4). Missing axes read 0.
   pov = -1 centred, else hundredths of degrees clockwise from up (0..35999).
   Bindings: IN_SLOTS per action (slot 0 = key, slot 1 = alternative key, slot 2 = controller by convention; any kind
   fits any slot). Binding codes:
     IN_KEY      code = VK
     IN_JOYBTN   code = IN_JOYCODE(joy, button 0..31)
     IN_JOYAXIS  code = IN_JOYCODE(joy, axis 0..7 * 4 + IN_AXIS_FULL / IN_AXIS_POS / IN_AXIS_NEG)
     IN_JOYPOV   code = IN_JOYCODE(joy, direction 0 up, 1 right, 2 down, 3 left) (diagonals press both)
   invert flips an axis before it is used (full: v -> -v; halves read the flipped value).
   Values: key/button/POV 0 or 1; half axis max(0, +-v); a full-axis binding gives in_action = (v + 1) / 2 (pedal
   on its own axis: bind it full, invert if it rests at +1) and in_axis = v. in_action = max over the slots,
   in_axis = the full-axis binding with the largest |v| (0 if none), in_action_hit = in_action crossed 0.5 upwards
   between the last two polls.
   Ini [controls]: one key per action name, value = up to IN_SLOTS bindings separated by ',':
     none | key NAME (LEFT, A, F1, SPACE, ... or 0x41) | j0 b3 | j0 x | j0 x+ | j0 y- | j0 pov up ; append " inv"
   e.g. "throttle=key UP, key W, j0 y-". Axis names x y z rx ry rz s0 s1. Unknown/garbled entries keep the default
   and log a warn: line. */
#ifndef E_INPUT_H
#define E_INPUT_H

#include "e_cfg.h"

enum { IN_MAX_ACTIONS = 32, IN_KEY = 1, IN_JOYBTN = 2, IN_JOYAXIS = 3, IN_JOYPOV = 4 };
int   in_init(void *hwnd, int useJoystick);        /* DirectInput 5 interfaces; winmm joyGetPosEx fallback */
void  in_shutdown(void);
void  in_poll(void);                               /* once per 120 Hz tick; keys via GetAsyncKeyState */
int   in_key(int vk); int in_key_hit(int vk);      /* held / pressed since last poll */
int   in_joy_count(void); const char *in_joy_name(int i);
typedef struct JoyState { int valid; float axis[8]; u32 buttons; int pov; } JoyState;  /* axes -1..1 */
const JoyState *in_joy(int i);
void  in_bind(int action, int slot, int kind, int code, int invert);  /* slot 0..IN_SLOTS-1; kind 0 = clear slot */
float in_action(int action);                       /* 0..1 (axis half or full per binding) */
int   in_action_hit(int action);
float in_axis(int action);                         /* -1..1 for full-axis bindings (steering) */
void  in_bind_load(const Cfg *c); void in_bind_save(Cfg *c);
int   in_capture(int *kind, int *code);            /* options menu: next pressed key/button/axis */

/* ---- additions (T08) ---- */
enum { IN_SLOTS = 3, IN_MAX_JOYS = 4, IN_NONE = 0 };
enum { IN_AXIS_FULL = 0, IN_AXIS_POS = 1, IN_AXIS_NEG = 2 };
#define IN_JOYCODE(joy, sub)  (((joy) << 8) | (sub))
#define IN_CODE_JOY(code)     (((code) >> 8) & 0xFF)
#define IN_CODE_SUB(code)     ((code) & 0xFF)
#define IN_AXISCODE(joy, axis, mode) IN_JOYCODE(joy, (axis) * 4 + (mode))

/* Default action set (driving game + menus). in_init binds the defaults; a game with other actions calls
   in_action_define() for its own ids (0..IN_MAX_ACTIONS-1), then in_bind / in_bind_load. */
enum {
    IA_STEER = 0,        /* full axis (controller) */
    IA_LEFT, IA_RIGHT,   /* digital steering (keyboard input shaping happens in player.cpp) */
    IA_THROTTLE, IA_BRAKE, IA_HANDBRAKE,
    IA_SHIFT_UP, IA_SHIFT_DOWN,
    IA_CAMERA, IA_LOOK_BACK, IA_RESET, IA_PAUSE,
    IA_MENU_UP, IA_MENU_DOWN, IA_MENU_LEFT, IA_MENU_RIGHT, IA_MENU_OK, IA_MENU_BACK,
    IA_COUNT
};
typedef struct InBinding { int kind, code, invert; } InBinding;
void  in_action_define(int action, const char *iniName);    /* name used in [controls]; NULL/"" = not saved */
const char *in_action_name(int action);            /* "" if undefined */
void  in_bind_defaults(void);                      /* clears every binding and sets the default table */
void  in_bind_clear(void);
const InBinding *in_binding(int action, int slot); /* never NULL (kind 0 = empty) */
int   in_binding_str(const InBinding *b, char *out, int n);   /* "key LEFT", "j0 y- inv", "none" */
int   in_binding_parse(const char *s, InBinding *out);        /* 1 = ok */
const char *in_key_name(int vk);                   /* "LEFT", "A", "F1"; NULL if unnamed (then 0x.. is used) */
int   in_axis_bound(int action);                   /* 1 if a full-axis binding of a connected joystick exists */
void  in_set_focus_gate(int on);                   /* default 1: idle input while the window is not foreground */
void  in_watch_all(int on);                        /* scan all 255 VKs every poll (test, key capture screens) */
void  in_capture_begin(void);                      /* reset capture baselines (in_capture also starts by itself) */
void  in_capture_end(void);
int   in_capture_invert(void);                    /* after in_capture returned an axis: 1 = bind it inverted */
const char *in_joy_backend(int i);                 /* "dinput", "winmm" or "" */
int   in_joy_axes(int i);                          /* bit mask of present axes (bit n = JoyState.axis[n]) */
int   in_joy_buttons(int i);                       /* number of buttons */
typedef struct InStats { int polls; int keyCalls; s64 pollUsSum; int pollUsMax; s64 joyUsSum; int joyUsMax; } InStats;
void  in_stats(InStats *out);                      /* counters since in_init (test/bench lines) */
void  in_debug_set_joy(int i, const JoyState *js, const char *name);   /* tests: fake joystick i (NULL = remove) */

/* ---- additions (jet T01, DESIGN 5.3) ---- */
int   in_action_held_ticks(int action);            /* polls in a row the action has been >= 0.5 (0 = not held) */
int   in_action_released(int action);              /* on the poll it was let go: ticks it was held (>= 1; a tap
                                                      between two polls = 1); else 0. Focus loss: no event */

#endif
