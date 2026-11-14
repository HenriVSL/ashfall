/* e_plat.cpp - see e_plat.h. Windows (VC6). Owner: T01. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <float.h>
#include "e_plat.h"
#include "e_log.h"

#define PLAT_FPU_PC   _PC_24                        /* DESIGN 2.9; _PC_53 if spike/RESULTS.md says so */
#define PLAT_FPU_MASK (_MCW_PC | _MCW_RC)
#define PLAT_FPU_WANT (PLAT_FPU_PC | _RC_NEAR)

enum { MAX_HOOKS = 8 };

static HINSTANCE s_inst;
static HWND      s_hwnd;
static int       s_quit, s_fullscreen, s_noDialogs, s_classDone;
static char      s_class[64];
static char      s_exeDir[MAX_PATH];
static LONGLONG  s_qpf;
static PlatMsgHook s_hooks[MAX_HOOKS];

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int i;
    long res = 0;
    for (i = 0; i < MAX_HOOKS; i++)
        if (s_hooks[i] && s_hooks[i](hwnd, msg, (unsigned)wp, (long)lp, &res)) return (LRESULT)res;
    switch (msg) {
    case WM_CLOSE:
        s_quit = 1;
        return 0;
    case WM_SETCURSOR:
        if (s_fullscreen && LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_F4) break;                     /* Alt+F4 closes */
        return 0;                                   /* no menu beep on Alt / F10 */
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU || (wp & 0xFFF0) == SC_SCREENSAVE || (wp & 0xFFF0) == SC_MONITORPOWER) return 0;
        break;
    case WM_DESTROY:
        s_hwnd = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int plat_init(void *hInstance, const PlatInit *pi)
{
    WNDCLASS wc;
    DWORD style, exStyle;
    RECT r;
    int x, y, w, h;
    const char *title = (pi && pi->title) ? pi->title : "Jet";
    s_inst = (HINSTANCE)hInstance;
    if (!s_inst) s_inst = GetModuleHandle(NULL);
    e_strlcpy(s_class, (pi && pi->wndClass) ? pi->wndClass : "JetWnd", (int)sizeof(s_class));
    s_fullscreen = pi ? pi->fullscreen : 0;
    s_quit = 0;
    if (!s_classDone) {
        memset(&wc, 0, sizeof(wc));
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = s_inst;
        wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = s_class;
        if (!RegisterClass(&wc)) {
            log_printf(LOG_ERR, "err: plat RegisterClass failed gle=%lu", (unsigned long)GetLastError());
            return 0;
        }
        s_classDone = 1;
    }
    w = (pi && pi->width > 0) ? pi->width : 640;
    h = (pi && pi->height > 0) ? pi->height : 480;
    if (s_fullscreen) {
        style = WS_POPUP;
        exStyle = WS_EX_TOPMOST;
        x = y = 0;
    } else {
        style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        exStyle = 0;
        r.left = 0; r.top = 0; r.right = w; r.bottom = h;
        AdjustWindowRectEx(&r, style, FALSE, exStyle);
        w = r.right - r.left;
        h = r.bottom - r.top;
        x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
    }
    s_hwnd = CreateWindowEx(exStyle, s_class, title, style, x, y, w, h, NULL, NULL, s_inst, NULL);
    if (!s_hwnd) {
        log_printf(LOG_ERR, "err: plat CreateWindow failed gle=%lu", (unsigned long)GetLastError());
        return 0;
    }
    if (!(pi && pi->hidden)) {
        ShowWindow(s_hwnd, SW_SHOWNORMAL);          /* twice: the first call may get the STARTUPINFO show flag */
        ShowWindow(s_hwnd, SW_SHOWNORMAL);
        UpdateWindow(s_hwnd);
        SetForegroundWindow(s_hwnd);
    }
    log_printf(LOG_INFO, "plat: window class=%s %dx%d fullscreen=%d hidden=%d", s_class,
               pi ? pi->width : 0, pi ? pi->height : 0, s_fullscreen, pi ? pi->hidden : 0);
    return 1;
}

void *plat_hwnd(void) { return s_hwnd; }
void *plat_hinstance(void) { return s_inst ? s_inst : GetModuleHandle(NULL); }

int plat_pump(void)
{
    MSG m;
    while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) s_quit = 1;
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return !s_quit;
}

void plat_request_quit(void) { s_quit = 1; }
int  plat_quit_requested(void) { return s_quit; }

int plat_active(void)
{
    if (!s_hwnd || IsIconic(s_hwnd)) return 0;
    return GetForegroundWindow() == s_hwnd;
}

void plat_client_size(int *w, int *h)
{
    RECT r;
    *w = *h = 0;
    if (s_hwnd && GetClientRect(s_hwnd, &r)) { *w = r.right - r.left; *h = r.bottom - r.top; }
}

s64 plat_qpf(void)
{
    if (!s_qpf) {
        LARGE_INTEGER f;
        if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) f.QuadPart = 1000;   /* never on Win98 PCs */
        s_qpf = f.QuadPart;
    }
    return s_qpf;
}

s64 plat_qpc(void)
{
    LARGE_INTEGER q;
    if (plat_qpf() == 1000) return (s64)timeGetTime();
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

s64 plat_now_us(void)
{
    s64 q = plat_qpc(), f = plat_qpf();
    return (q / f) * 1000000 + ((q % f) * 1000000) / f;   /* no overflow for years of uptime */
}

void plat_sleep(int ms) { Sleep((DWORD)(ms < 0 ? 0 : ms)); }

const char *plat_exe_dir(void)
{
    if (!s_exeDir[0]) {
        char *slash;
        DWORD n = GetModuleFileName(NULL, s_exeDir, sizeof(s_exeDir) - 1);
        s_exeDir[n < sizeof(s_exeDir) ? n : sizeof(s_exeDir) - 1] = 0;
        slash = strrchr(s_exeDir, '\\');
        if (slash) *slash = 0;
        else s_exeDir[0] = 0;
        if (!s_exeDir[0]) strcpy(s_exeDir, ".");
    }
    return s_exeDir;
}

void plat_fpu_setup(void) { _controlfp(PLAT_FPU_WANT, PLAT_FPU_MASK); }
int  plat_fpu_ok(void) { return (_controlfp(0, 0) & PLAT_FPU_MASK) == PLAT_FPU_WANT; }
unsigned plat_fpu_cw(void) { return (unsigned)_controlfp(0, 0); }

void plat_set_no_dialogs(int on) { s_noDialogs = on; }

void plat_error_box(const char *msg)
{
    log_printf(LOG_ERR, "err: %s", msg);
    log_flush();
    if (s_noDialogs) return;
    MessageBox(s_hwnd, msg, "Jet", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

int plat_add_msg_hook(PlatMsgHook fn)
{
    int i;
    for (i = 0; i < MAX_HOOKS; i++) if (s_hooks[i] == fn) return 1;
    for (i = 0; i < MAX_HOOKS; i++) if (!s_hooks[i]) { s_hooks[i] = fn; return 1; }
    return 0;
}

void plat_remove_msg_hook(PlatMsgHook fn)
{
    int i;
    for (i = 0; i < MAX_HOOKS; i++) if (s_hooks[i] == fn) s_hooks[i] = NULL;
}

void plat_shutdown(void)
{
    MSG m;
    if (s_hwnd) DestroyWindow(s_hwnd);
    s_hwnd = NULL;
    while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); }
    if (s_classDone) UnregisterClass(s_class, s_inst);
    s_classDone = 0;
}
