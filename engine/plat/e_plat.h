/* e_plat.h - window, message pump, focus, QPC timing, paths, x87 control word (DESIGN 2.5, 2.9). Windows side;
   the header stays windows.h-free (HWND/HINSTANCE as void*). Owner: T01.
   Window: class "JetWnd" (PlatInit.wndClass, NULL = default), title "Jet" (NULL = default). Fullscreen = a
   topmost WS_POPUP of width x height at (0,0) for the renderer's exclusive mode; windowed = caption + system menu
   with a client area of width x height, centred. hidden = 1 creates the window without showing it.
   FPU (2.9): PC24 + round-to-nearest (spike fpu.txt: DDSCL_FPUSETUP gives PC24 after CreateDevice). Change
   PLAT_FPU_PC in e_plat.cpp if spike/RESULTS.md recommends PC53; every caller follows. */
#ifndef E_PLAT_H
#define E_PLAT_H

#include "e_base.h"

typedef struct PlatInit { const char *title, *wndClass; int width, height, fullscreen; int hidden; } PlatInit;
int   plat_init(void *hInstance, const PlatInit *pi);   /* creates the window only; renderer owns DirectDraw */
void *plat_hwnd(void);                             /* HWND as void* so headers stay windows.h-free */
int   plat_pump(void);                             /* 0 = quit requested */
int   plat_active(void);                           /* has focus and not minimised */
s64   plat_qpc(void); s64 plat_qpf(void);
s64   plat_now_us(void);                           /* QPC in microseconds, integer math only */
void  plat_sleep(int ms);
const char *plat_exe_dir(void);
void  plat_fpu_setup(void);                        /* _controlfp(_PC_24 | _RC_NEAR, _MCW_PC | _MCW_RC) */
int   plat_fpu_ok(void);                           /* 1 if control word is still ours */
void  plat_error_box(const char *msg);
void  plat_shutdown(void);

/* ---- additions (T01) ---- */
void *plat_hinstance(void);                        /* HINSTANCE (DirectInput needs it) */
unsigned plat_fpu_cw(void);                        /* current x87 control word (_controlfp(0, 0)) for logs */
void  plat_set_no_dialogs(int on);                 /* 1: plat_error_box only logs (headless/test/automated runs) */
void  plat_request_quit(void);                     /* next plat_pump returns 0 */
int   plat_quit_requested(void);
/* Window message hooks (renderer: WM_ACTIVATEAPP/WM_PAINT; input: WM_CHAR...). A hook returns 1 if it handled the
   message (then *result is returned from the window procedure), 0 to pass it on. Up to 8 hooks, called in order. */
typedef int (*PlatMsgHook)(void *hwnd, unsigned msg, unsigned wp, long lp, long *result);
int   plat_add_msg_hook(PlatMsgHook fn);           /* 0 if the table is full */
void  plat_remove_msg_hook(PlatMsgHook fn);
void  plat_client_size(int *w, int *h);            /* current client area (0,0 without a window) */

#endif
