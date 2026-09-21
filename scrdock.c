/*
 * scrdock — floating toolbar for scrcpy (Win32, pure C)
 *
 * - Auto-launches scrcpy.exe from its own directory (or attaches to a
 *   running instance), then docks to the right edge of the scrcpy window.
 * - Follows the scrcpy window via SetWinEventHook(WINEVENT_OUTOFCONTEXT):
 *   the hook is installed on the UI thread that runs the message loop, and
 *   the callback is always invoked on that same thread while it pumps
 *   messages (hard constraint: no cross-thread callbacks, never stop pumping).
 * - Simple controls are injected with "adb shell input keyevent".
 *   Android keycodes: HOME=3 BACK=4 VOL_UP=24 VOL_DOWN=25 POWER=26
 *   APP_SWITCH=187 (same mapping QtScrcpy uses).
 * - Screenshot: "adb exec-out screencap -p" redirected to a file on a worker
 *   thread (validated against the PNG magic, with a /sdcard + pull fallback).
 *
 * Build: see build.bat (cl /W4 /O2 /utf-8).
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <process.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WM_APP_DEVREADY (WM_APP + 1)
#define WM_APP_SHOTDONE (WM_APP + 2)

#define TIMER_HUNT  1  /* 200 ms: poll for the scrcpy window            */
#define TIMER_SYNC  2  /* 500 ms: safety net (missed events, pid polls) */
#define TIMER_FLASH 3  /* 1.2 s: screenshot result flash                */
#define TIMER_TIP   4  /* 350 ms: tooltip dwell delay                   */

#define IDM_REDOCK 100
#define IDM_EXIT   101

/* Dock metrics in 96-dpi units, scaled at runtime. */
#define DOCK_GAP_X96 0
#define DOCK_YOFF96  30

/* Layout metrics in 96-dpi units. */
#define BTN_SIZE_96 40
#define BTN_GAP_96  6
#define PAD_96      6

enum {
    ID_HOME, ID_BACK, ID_APPSW, ID_POWER, ID_VOLUP, ID_VOLDN,
    ID_NOTIF, ID_CC, ID_PIN, ID_SHOT, ID_CLOSE, ID_COUNT
};

struct Btn {
    const wchar_t *glyph;  /* Segoe MDL2 Assets codepoint */
    const wchar_t *label;  /* ASCII fallback if the font is missing */
    const wchar_t *tip;
};

static const struct Btn kBtns[ID_COUNT] = {
    { L"\uE80F", L"HOME", L"主屏幕 Home" },
    { L"\uE72B", L"BACK", L"返回 Back" },
    { L"\uE7C4", L"REC",  L"最近任务 App switch" },
    { L"\uE7E8", L"PWR",  L"电源 Power（会灭屏，镜像继续）" },
    { L"\uE767", L"V+",   L"音量+ Volume up" },
    { L"\uE74F", L"V-",   L"音量− Volume down" },
    { L"\uE7E7", L"BELL", L"展开通知栏" },
    { L"\uE713", L"CC",   L"展开控制中心" },
    { L"\uE718", L"PIN",  L"窗口置顶（scrcpy + 工具栏）" },
    { L"\uE722", L"SHOT", L"截图 Screenshot" },
    { L"\uE8BB", L"X",    L"关闭（同时退出 scrcpy）" },
};

static const int kKeycode[ID_COUNT] = { 3, 4, 187, 26, 24, 25, 0, 0, 0, 0, 0 };

/* scrcpy shortcut keys (MOD=lalt, pinned at launch; see launch_scrcpy). */
static const WORD kVk[ID_COUNT] = { 'H', 'B', 'S', 'P', VK_UP, VK_DOWN, 'N', 0, 0, 0, 0 };

struct Config {
    wchar_t serial[128];
    wchar_t scrcpy_args[512];
    wchar_t adb[MAX_PATH];
    BOOL close_on_exit;
    int control;             /* 0=auto 1=shortcut 2=adb */
};

struct Scrdock {
    HWND hwnd;              /* toolbar window                       */
    HWND tips;              /* tooltip control                      */
    HWND target;            /* scrcpy SDL window                    */
    DWORD pid;              /* scrcpy pid                           */
    HANDLE hProc;           /* scrcpy process (NULL in fallback)    */
    HWINEVENTHOOK hookObj;  /* EVENT_OBJECT_* range                 */
    HWINEVENTHOOK hookSys;  /* EVENT_SYSTEM_MINIMIZE* range         */
    HFONT fGlyph;           /* Segoe MDL2 Assets                    */
    HFONT fUi;              /* Segoe UI (fallback labels)           */
    HANDLE hNul;            /* inheritable NUL writer for adb       */
    int hover, pressed, flashBtn;
    bool flashOk;
    bool tracking;          /* TrackMouseEvent armed                */
    bool manual;            /* user dragged the toolbar away        */
    bool shotBusy;
    bool launched;          /* we started scrcpy (vs attached)      */
    bool shown;
    bool everDocked;
    bool closing;
    bool adbOk;
    bool glyphsOk;
    bool devNone;
    bool notifFallback;
    bool pinned;             /* window always-on-top toggle       */
    int scrW, scrH;         /* device screen size for swipe fallback */
    int tipShownFor;        /* button index whose tooltip is visible  */
    POINT mousePt;          /* last mouse position (screen coords)    */
    bool pinned_mod;        /* we launched scrcpy with --shortcut-mod=lalt */
    ULONGLONG launchTick;
    wchar_t exeDir[MAX_PATH];
    wchar_t scrcpyPath[MAX_PATH];
    wchar_t adbPath[MAX_PATH];
    wchar_t serial[128];
    wchar_t tipStatus[MAX_PATH + 32];
    RECT btnRc[ID_COUNT];
    int w, h;               /* window size (dpi-scaled)             */
    RECT lastDock;          /* dedupe                               */
    struct Config cfg;
};

static struct Scrdock g;
static HINSTANCE g_hInst;

/* ------------------------------------------------------------------ */
/* small utilities                                                     */

static void dbg_log(const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 512, fmt, ap);
    va_end(ap);
    OutputDebugStringW(buf);
}

static void init_dpi(void)
{
    /* Must run before any window is created, and must reach per-monitor
     * awareness: scrcpy/SDL3 is per-monitor DPI aware, and only matching
     * awareness keeps GetWindowRect(target) in true screen coordinates. */
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
        }
    }
}

static void get_exe_dir(wchar_t out[MAX_PATH])
{
    DWORD n = GetModuleFileNameW(NULL, out, MAX_PATH);
    wchar_t *p = (n > 0 && n < MAX_PATH) ? wcsrchr(out, L'\\') : NULL;
    if (p) {
        *p = L'\0';
    } else {
        wcscpy(out, L".");
    }
}

static void dir_of(const wchar_t *path, wchar_t *out, size_t cch)
{
    wcsncpy(out, path, cch - 1);
    out[cch - 1] = L'\0';
    wchar_t *p = wcsrchr(out, L'\\');
    if (p) {
        *p = L'\0';
    }
}

static bool file_exists(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* Try exeDir\name, then extraDir\name, then the PATH. */
static bool find_tool(const wchar_t *name, const wchar_t *extra_dir,
                      wchar_t out[MAX_PATH])
{
    wchar_t buf[MAX_PATH];
    swprintf(buf, MAX_PATH, L"%s\\%s", g.exeDir, name);
    if (file_exists(buf)) {
        wcsncpy(out, buf, MAX_PATH - 1);
        out[MAX_PATH - 1] = L'\0';
        return true;
    }
    if (extra_dir && extra_dir[0]) {
        swprintf(buf, MAX_PATH, L"%s\\%s", extra_dir, name);
        if (file_exists(buf)) {
            wcsncpy(out, buf, MAX_PATH - 1);
            out[MAX_PATH - 1] = L'\0';
            return true;
        }
    }
    wchar_t *file_part = NULL;
    if (SearchPathW(NULL, name, L".exe", MAX_PATH, out, &file_part) > 0) {
        return true;
    }
    return false;
}

static void ini_load(void)
{
    wchar_t ini[MAX_PATH];
    char ini_a[MAX_PATH];
    char buf[1024];
    swprintf(ini, MAX_PATH, L"%s\\scrdock.ini", g.exeDir);

    g.cfg.serial[0] = L'\0';
    g.cfg.scrcpy_args[0] = L'\0';
    g.cfg.adb[0] = L'\0';
    g.cfg.close_on_exit = TRUE;
    g.cfg.control = 0;

    if (!file_exists(ini)) {
        return;
    }
    WideCharToMultiByte(CP_ACP, 0, ini, -1, ini_a, MAX_PATH, NULL, NULL);

    GetPrivateProfileStringA("scrdock", "serial", "", buf, sizeof(buf), ini_a);
    MultiByteToWideChar(CP_ACP, 0, buf, -1, g.cfg.serial, 128);
    GetPrivateProfileStringA("scrdock", "scrcpy_args", "", buf, sizeof(buf), ini_a);
    MultiByteToWideChar(CP_ACP, 0, buf, -1, g.cfg.scrcpy_args, 512);
    GetPrivateProfileStringA("scrdock", "adb", "", buf, sizeof(buf), ini_a);
    MultiByteToWideChar(CP_ACP, 0, buf, -1, g.cfg.adb, MAX_PATH);
    g.cfg.close_on_exit =
        GetPrivateProfileIntA("scrdock", "close_scrcpy_on_exit", 1, ini_a) != 0;
    {
        char ctl[16];
        GetPrivateProfileStringA("scrdock", "control", "auto", ctl, sizeof(ctl), ini_a);
        if (_stricmp(ctl, "shortcut") == 0) {
            g.cfg.control = 1;
        } else if (_stricmp(ctl, "adb") == 0) {
            g.cfg.control = 2;
        } else {
            g.cfg.control = 0;
        }
    }
}

static UINT wnd_dpi(HWND hwnd)
{
    UINT dpi = GetDpiForWindow(hwnd);
    return dpi ? dpi : 96;
}

/* ------------------------------------------------------------------ */
/* scrcpy process management                                           */

static DWORD find_scrcpy_pid(void)
{
    DWORD pid = 0;
    FILETIME newest = { 0, 0 };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"scrcpy.exe") != 0) {
                continue;
            }
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, pe.th32ProcessID);
            if (h) {
                FILETIME create, exit_t, kernel_t, user_t;
                if (GetProcessTimes(h, &create, &exit_t, &kernel_t, &user_t)
                        && (pid == 0 || CompareFileTime(&create, &newest) > 0)) {
                    newest = create;
                    pid = pe.th32ProcessID;
                }
                CloseHandle(h);
            } else if (pid == 0) {
                pid = pe.th32ProcessID;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

struct FindWinCtx {
    DWORD pid;
    HWND sdl;
    HWND any;
};

static BOOL CALLBACK enum_windows_cb(HWND h, LPARAM lp)
{
    struct FindWinCtx *c = (struct FindWinCtx *) lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) {
        return TRUE;
    }
    wchar_t cls[64];
    int n = GetClassNameW(h, cls, 64);
    if (n > 0 && wcscmp(cls, L"SDL_app") == 0) {
        /* Accept even while still hidden: SDL creates the window early but
         * scrcpy only shows it on the first video frame. */
        c->sdl = h;
        return FALSE;
    }
    if (!c->any && IsWindowVisible(h)) {
        c->any = h;
    }
    return TRUE;
}

static HWND find_scrcpy_window(DWORD pid)
{
    struct FindWinCtx c;
    ZeroMemory(&c, sizeof(c));
    c.pid = pid;
    EnumWindows(enum_windows_cb, (LPARAM) &c);
    return c.sdl ? c.sdl : c.any;
}

static bool launch_scrcpy(void)
{
    wchar_t cmd[1152];
    wchar_t dir[MAX_PATH];

    /* Pin MOD=Left-Alt so the toolbar can send instant MOD+key shortcuts to
     * the scrcpy window (see send_scrcpy_shortcut). Skipped if the user
     * already passes --shortcut-mod of their own. */
    g.pinned_mod = (wcsstr(g.cfg.scrcpy_args, L"--shortcut-mod") == NULL);
    if (g.cfg.scrcpy_args[0]) {
        swprintf(cmd, 1152, L"\"%s\"%s %s", g.scrcpyPath,
                 g.pinned_mod ? L" --shortcut-mod=lalt" : L"",
                 g.cfg.scrcpy_args);
    } else {
        swprintf(cmd, 1152, L"\"%s\"%s", g.scrcpyPath,
                 g.pinned_mod ? L" --shortcut-mod=lalt" : L"");
    }

    dir_of(g.scrcpyPath, dir, MAX_PATH);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    /* CREATE_NO_WINDOW: scrcpy.exe is a console app; never flash a console.
     * Working directory = scrcpy dir so a portable dist finds scrcpy-server. */
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, dir[0] ? dir : NULL, &si, &pi)) {
        dbg_log(L"launch_scrcpy: CreateProcessW failed (%lu)", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    g.hProc = pi.hProcess;
    g.pid = pi.dwProcessId;
    g.launched = true;
    g.launchTick = GetTickCount64();
    return true;
}

/* Close scrcpy gracefully (WM_CLOSE -> SDL3 quit-on-last-window-close),
 * pumping messages while waiting: the EVENT_OBJECT_DESTROY callback for the
 * dying window is marshaled to this same thread. */
static void close_scrcpy_and_wait(void)
{
    if (!g.cfg.close_on_exit || !g.launched || !g.hProc) {
        return;
    }
    if (WaitForSingleObject(g.hProc, 0) == WAIT_OBJECT_0) {
        return;
    }
    if (g.target && IsWindow(g.target)) {
        PostMessageW(g.target, WM_CLOSE, 0, 0);
    }
    ULONGLONG deadline = GetTickCount64() + 2000;
    for (;;) {
        if (WaitForSingleObject(g.hProc, 0) == WAIT_OBJECT_0) {
            break;
        }
        if (GetTickCount64() >= deadline) {
            TerminateProcess(g.hProc, 1);
            break;
        }
        DWORD r = MsgWaitForMultipleObjectsEx(1, &g.hProc, 50, QS_ALLINPUT,
                                              MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0 + 1) {
            MSG m;
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) {
                    /* re-post: the outer loop must still see it */
                    PostQuitMessage((int) m.wParam);
                    return;
                }
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* adb helpers                                                         */

/* Build "<quoted adb>" [-s serial] + formatted tail into out. */
static int adb_cmd_fmt(wchar_t *out, size_t cch, const wchar_t *tail_fmt, ...)
{
    int n = swprintf(out, cch, L"\"%s\"", g.adbPath);
    if (n < 0) {
        return 0;
    }
    if (g.serial[0]) {
        int k = swprintf(out + n, cch - (size_t) n, L" -s %s", g.serial);
        if (k < 0) {
            return n;
        }
        n += k;
    }
    if (tail_fmt && tail_fmt[0] == L' ') {
        va_list ap;
        va_start(ap, tail_fmt);
        int k = vswprintf(out + n, cch - (size_t) n, tail_fmt, ap);
        va_end(ap);
        if (k > 0) {
            n += k;
        }
    }
    return n;
}

/* Fire-and-forget adb call; never blocks the UI thread. */
static void adb_spawn(const wchar_t *tail_fmt, ...)
{
    wchar_t cmd[1100];
    wchar_t tail[512];
    va_list ap;

    if (!g.adbOk) {
        return;
    }
    va_start(ap, tail_fmt);
    vswprintf(tail, 512, tail_fmt, ap);
    va_end(ap);

    int n = swprintf(cmd, 1100, L"\"%s\"", g.adbPath);
    if (g.serial[0]) {
        n += swprintf(cmd + n, 1100 - (size_t) n, L" -s %s", g.serial);
    }
    n += swprintf(cmd + n, 1100 - (size_t) n, L"%s", tail);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    if (g.hNul) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = g.hNul;
        si.hStdError = g.hNul;
        si.hStdInput = NULL;
    }
    /* CREATE_NO_WINDOW only (DETACHED_PROCESS is mutually exclusive). */
    if (CreateProcessW(NULL, cmd, NULL, NULL, g.hNul != NULL, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        dbg_log(L"adb_spawn: CreateProcessW failed (%lu)", GetLastError());
    }
}

/* Run a command to completion, capturing stdout into a temp file, then
 * reading it back. Worker threads only (it waits). */
static bool capture_sync(const wchar_t *cmdline, char *out, DWORD cb_out,
                         DWORD timeout_ms, DWORD *exit_code)
{
    wchar_t tmp_dir[MAX_PATH];
    wchar_t tmp_file[MAX_PATH];
    SECURITY_ATTRIBUTES sa;
    HANDLE hf;
    DWORD n = GetTempPathW(MAX_PATH, tmp_dir);
    bool ok = false;

    if (!n || n >= MAX_PATH) {
        return false;
    }
    if (!GetTempFileNameW(tmp_dir, L"sd", 0, tmp_file)) {
        return false;
    }
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    hf = CreateFileW(tmp_file, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                     TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        DeleteFileW(tmp_file);
        return false;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hf;
    si.hStdError = g.hNul ? g.hNul : hf;
    si.hStdInput = NULL;

    if (CreateProcessW(NULL, (LPWSTR) cmdline, NULL, NULL, TRUE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) {
            DWORD ec = 0;
            GetExitCodeProcess(pi.hProcess, &ec);
            if (exit_code) {
                *exit_code = ec;
            }
            ok = true;
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(hf);

    if (ok && out && cb_out > 0) {
        HANDLE hr = CreateFileW(tmp_file, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hr != INVALID_HANDLE_VALUE) {
            DWORD rd = 0;
            out[0] = '\0';
            ReadFile(hr, out, cb_out - 1, &rd, NULL);
            out[rd] = '\0';
            CloseHandle(hr);
        }
    }
    DeleteFileW(tmp_file);
    return ok;
}

/* Redirect the command's stdout straight into path (raw handle: no CRT
 * text mode in between, so binary output stays byte-exact). */
static bool run_redirect_to_file(const wchar_t *cmdline, const wchar_t *path,
                                 DWORD timeout_ms)
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    HANDLE hf = CreateFileW(path, GENERIC_WRITE, 0, &sa, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = false;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hf;
    si.hStdError = g.hNul ? g.hNul : hf;
    si.hStdInput = NULL;
    if (CreateProcessW(NULL, (LPWSTR) cmdline, NULL, NULL, TRUE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) {
            DWORD ec = 0;
            GetExitCodeProcess(pi.hProcess, &ec);
            ok = (ec == 0);
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(hf);
    return ok;
}

static bool file_is_png(const wchar_t *path)
{
    unsigned char hdr[8];
    DWORD rd = 0;
    LARGE_INTEGER sz;
    sz.QuadPart = 0;
    HANDLE hf = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        return false;
    }
    GetFileSizeEx(hf, &sz);
    bool ok = sz.QuadPart > 14
        && ReadFile(hf, hdr, 8, &rd, NULL) && rd == 8
        && hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G'
        && hdr[4] == 0x0D && hdr[5] == 0x0A && hdr[6] == 0x1A && hdr[7] == 0x0A;
    CloseHandle(hf);
    return ok;
}

static bool make_shot_path(wchar_t *path, size_t cch)
{
    wchar_t prof[MAX_PATH];
    wchar_t dir[MAX_PATH];
    SYSTEMTIME st;

    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        return false;
    }
    swprintf(dir, MAX_PATH, L"%s\\Pictures\\scrdock", prof);
    CreateDirectoryW(dir, NULL); /* ignore ERROR_ALREADY_EXISTS */
    GetLocalTime(&st);
    swprintf(path, cch, L"%s\\sd_%04u%02u%02u_%02u%02u%02u.png", dir,
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return true;
}

/* ------------------------------------------------------------------ */
/* worker threads (never touch windows directly; PostMessage back)     */

static bool parse_size_line(const char *buf, int *w, int *h)
{
    const char *p = strstr(buf, "Override size:");
    if (!p) {
        p = strstr(buf, "Physical size:");
    }
    if (!p) {
        return false;
    }
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    unsigned a = 0, b = 0;
    if (sscanf(p + 1, " %ux%u", &a, &b) == 2 && a && b) {
        *w = (int) a;
        *h = (int) b;
        return true;
    }
    return false;
}

static unsigned __stdcall device_probe_thread(void *arg)
{
    (void) arg;
    wchar_t cmd[600];
    char buf[4096];
    char serials[4][64];
    int count = 0;
    DWORD ec = 0;

    if (!g.adbOk || !g.hwnd) {
        return 0;
    }

    /* "adb devices -l" (no -s here by definition) */
    swprintf(cmd, 600, L"\"%s\" devices -l", g.adbPath);
    if (!capture_sync(cmd, buf, sizeof(buf), 30000, &ec)) {
        PostMessageW(g.hwnd, WM_APP_DEVREADY, 0, 0);
        return 0;
    }

    /* lines: "<serial>\t<state> ..." — count entries in state "device" */
    char *ctx = NULL;
    char *line = strtok_s(buf, "\r\n", &ctx);
    bool first = true;
    while (line && count < 4) {
        if (first) {
            first = false; /* "List of devices attached" */
        } else if (line[0]) {
            char sp[128];
            char st[32];
            if (sscanf(line, "%127s %31s", sp, st) == 2
                    && strcmp(st, "device") == 0) {
                strncpy(serials[count], sp, 63);
                serials[count][63] = '\0';
                count++;
            }
        }
        line = strtok_s(NULL, "\r\n", &ctx);
    }

    if (count == 0) {
        PostMessageW(g.hwnd, WM_APP_DEVREADY, 0, 0);
        return 0;
    }
    if (!g.serial[0] && !g.cfg.serial[0]) {
        MultiByteToWideChar(CP_ACP, 0, serials[0], -1, g.serial, 128);
    }

    if (count >= 1) {
        wchar_t serial_tmp[128];
        wchar_t *serial = g.serial[0] ? g.serial : g.cfg.serial;
        if (!serial[0]) {
            MultiByteToWideChar(CP_ACP, 0, serials[0], -1, serial_tmp, 128);
            serial = serial_tmp;
        }
        swprintf(cmd, 600, L"\"%s\" -s %s shell wm size", g.adbPath, serial);
        if (capture_sync(cmd, buf, sizeof(buf), 10000, &ec)) {
            int w = 0, h = 0;
            if (parse_size_line(buf, &w, &h)) {
                g.scrW = w;
                g.scrH = h;
            }
        }
    }

    PostMessageW(g.hwnd, WM_APP_DEVREADY, (WPARAM) (count == 1 ? 1 : 2), 0);
    return 0;
}

static unsigned __stdcall notif_thread(void *arg)
{
    int settings = (int) (intptr_t) arg;
    if (!g.notifFallback) {
        wchar_t cmd[600];
        char tmp[64];
        DWORD ec = 1;
        adb_cmd_fmt(cmd, 600, L" shell cmd statusbar expand-%s",
                    settings ? L"settings" : L"notifications");
        if (capture_sync(cmd, tmp, sizeof(tmp), 8000, &ec) && ec == 0) {
            return 0;
        }
        g.notifFallback = true;
        dbg_log(L"panel: cmd statusbar failed (ec=%lu), falling back to swipe", ec);
    }
    if (g.scrW > 0 && g.scrH > 0) {
        /* notifications pull from top-center; control center from top-right */
        int x = settings ? (g.scrW * 9) / 10 : g.scrW / 2;
        adb_spawn(L" shell input swipe %d 1 %d %d 300",
                  x, x, (g.scrH * 2) / 3);
    }
    return 0;
}

static unsigned __stdcall screenshot_thread(void *arg)
{
    wchar_t *path = (wchar_t *) arg;
    bool ok = false;

    if (path && g.adbOk) {
        wchar_t cmd[700];
        adb_cmd_fmt(cmd, 700, L" exec-out screencap -p");
        ok = run_redirect_to_file(cmd, path, 15000) && file_is_png(path);
        if (!ok) {
            /* Device-side fallback: screencap to /sdcard then pull.
             * "adb pull" writes the file itself, so do NOT redirect its
             * stdout to the same path — capture and discard, then validate. */
            char tmp[128];
            DWORD ec = 1;
            adb_cmd_fmt(cmd, 700, L" shell screencap -p /sdcard/scrdock_tmp.png");
            if (capture_sync(cmd, tmp, sizeof(tmp), 10000, &ec) && ec == 0) {
                adb_cmd_fmt(cmd, 700, L" pull /sdcard/scrdock_tmp.png \"%s\"", path);
                if (capture_sync(cmd, tmp, sizeof(tmp), 15000, &ec) && ec == 0) {
                    ok = file_is_png(path);
                }
            }
            adb_spawn(L" shell rm -f /sdcard/scrdock_tmp.png");
        }
    }

    if (path && ok) {
        swprintf(g.tipStatus, MAX_PATH + 32, L"已保存: %s", path);
    }
    if (g.hwnd) {
        PostMessageW(g.hwnd, WM_APP_SHOTDONE, (WPARAM) ok, 0);
    }
    free(path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* UI: layout, painting, tooltips                                      */

static void ensure_fonts(UINT dpi)
{
    HDC dc = GetDC(g.hwnd);
    if (g.fGlyph) {
        DeleteObject(g.fGlyph);
    }
    if (g.fUi) {
        DeleteObject(g.fUi);
    }
    g.fGlyph = CreateFontW(-MulDiv(18, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    g.fUi = CreateFontW(-MulDiv(11, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g.glyphsOk = false;
    if (dc && g.fGlyph) {
        HGDIOBJ old = SelectObject(dc, g.fGlyph);
        wchar_t face[64];
        int n = GetTextFaceW(dc, 64, face);
        if (n > 0 && wcscmp(face, L"Segoe MDL2 Assets") == 0) {
            g.glyphsOk = true;
        }
        SelectObject(dc, old);
    }
    if (dc) {
        ReleaseDC(g.hwnd, dc);
    }
}

static void layout_compute(void)
{
    UINT dpi = wnd_dpi(g.hwnd);
    int btn = MulDiv(BTN_SIZE_96, dpi, 96);
    int gap = MulDiv(BTN_GAP_96, dpi, 96);
    int pad = MulDiv(PAD_96, dpi, 96);
    int i;

    g.w = pad * 2 + btn;
    g.h = pad * 2 + ID_COUNT * btn + (ID_COUNT - 1) * gap;

    int y = pad;
    for (i = 0; i < ID_COUNT; i++) {
        g.btnRc[i].left = pad;
        g.btnRc[i].top = y;
        g.btnRc[i].right = pad + btn;
        g.btnRc[i].bottom = y + btn;
        y += btn + gap;
    }
}

static wchar_t g_tipText[MAX_PATH + 40];

/* Window procedure of the self-drawn tooltip popup. */
static LRESULT CALLBACK tip_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(40, 40, 40));
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(90, 90, 90));
            HPEN old_pen = SelectObject(dc, pen);
            HGDIOBJ old_br = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1);
            SelectObject(dc, old_br);
            SelectObject(dc, old_pen);
            DeleteObject(pen);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(235, 235, 235));
            HGDIOBJ old_f = SelectObject(dc, g.fUi);
            RECT tr = rc;
            tr.left += 10;
            tr.top += 5;
            tr.right -= 10;
            tr.bottom -= 5;
            DrawTextW(dc, g_tipText, -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, old_f);
            EndPaint(hwnd, &ps);
            return 0;
        }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void tooltip_window_create(void)
{
    /* Self-drawn tooltip: the common-controls tooltip (TTF_SUBCLASS relays,
     * and even TTM_ADDTOOL on this frameless WS_EX_NOACTIVATE popup) proved
     * unreliable here, so we own the window and the painting. */
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = tip_wndproc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"scrdock_tip_cls";
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    g.tips = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             L"scrdock_tip_cls", L"", WS_POPUP,
                             0, 0, 10, 10, g.hwnd, NULL, g_hInst, NULL);
}

static void tip_position(int w, int h)
{
    int x = g.mousePt.x + 14;
    int y = g.mousePt.y + 20;
    HMONITOR mon = MonitorFromPoint(g.mousePt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        if (x + w > mi.rcWork.right) {
            x = mi.rcWork.right - w;
        }
        if (x < mi.rcWork.left) {
            x = mi.rcWork.left;
        }
        if (y + h > mi.rcWork.bottom) {
            y = g.mousePt.y - 20 - h; /* flip above the cursor */
        }
        if (y < mi.rcWork.top) {
            y = mi.rcWork.top;
        }
    }
    SetWindowPos(g.tips, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void tip_show(int i)
{
    if (!g.tips || i < 0) {
        return;
    }
    const wchar_t *txt = (i == ID_SHOT && g.tipStatus[0])
        ? g.tipStatus : kBtns[i].tip;
    if (i == ID_PIN) {
        txt = g.pinned ? L"取消窗口置顶" : L"窗口置顶（scrcpy + 工具栏）";
    }
    wcsncpy(g_tipText, txt, MAX_PATH + 39);
    g_tipText[MAX_PATH + 39] = L'\0';

    HDC dc = GetDC(g.tips);
    HGDIOBJ old_f = SelectObject(dc, g.fUi);
    RECT tr = { 0, 0, 0, 0 };
    DrawTextW(dc, g_tipText, -1, &tr, DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, old_f);
    ReleaseDC(g.tips, dc);

    int w = (tr.right - tr.left) + 20;
    int h = (tr.bottom - tr.top) + 10;
    tip_position(w, h);
    InvalidateRect(g.tips, NULL, FALSE);
    g.tipShownFor = i;
}

static void tip_hide(void)
{
    if (!g.tips || g.tipShownFor < 0) {
        return;
    }
    ShowWindow(g.tips, SW_HIDE);
    g.tipShownFor = -1;
}

static int hit_test(POINT pt)
{
    int i;
    for (i = 0; i < ID_COUNT; i++) {
        if (pt.x >= g.btnRc[i].left && pt.x < g.btnRc[i].right
                && pt.y >= g.btnRc[i].top && pt.y < g.btnRc[i].bottom) {
            return i;
        }
    }
    return -1;
}

static int hit_test_lparam(LPARAM lp)
{
    POINT pt;
    pt.x = GET_X_LPARAM(lp);
    pt.y = GET_Y_LPARAM(lp);
    return hit_test(pt);
}

static void draw_button(HDC dc, int i)
{
    RECT rc = g.btnRc[i];
    RECT tr = rc;
    COLORREF bg;

    if (g.flashBtn == i) {
        bg = g.flashOk ? RGB(47, 79, 47) : RGB(95, 47, 47);
    } else if (g.pinned && i == ID_PIN) {
        bg = RGB(44, 96, 160);
    } else if (g.pressed == i) {
        bg = RGB(87, 87, 87);
    } else if (g.hover == i) {
        bg = RGB(63, 63, 63);
    } else {
        bg = RGB(43, 43, 43);
    }

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
    HBRUSH br = CreateSolidBrush(bg);
    HPEN old_pen = SelectObject(dc, pen);
    HBRUSH old_br = SelectObject(dc, br);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_br);
    DeleteObject(pen);
    DeleteObject(br);

    bool disabled = !g.adbOk && i != ID_CLOSE;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? RGB(120, 120, 120) : RGB(240, 240, 240));

    const wchar_t *txt;
    if (g.flashBtn == i && g.flashOk && i == ID_SHOT) {
        txt = L"\uE73E"; /* checkmark */
    } else if (g.glyphsOk) {
        txt = kBtns[i].glyph;
    } else {
        txt = kBtns[i].label;
        tr.left -= 6;
        tr.right += 6;
    }
    HGDIOBJ old_font = SelectObject(dc, g.glyphsOk ? g.fGlyph : g.fUi);
    DrawTextW(dc, txt, -1, &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);
}

static void paint_ui(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ old_bm = SelectObject(mem, bm);

    HBRUSH bg = CreateSolidBrush(RGB(32, 32, 32));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    int i;
    for (i = 0; i < ID_COUNT; i++) {
        draw_button(mem, i);
    }

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bm);
    DeleteObject(bm);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

/* ------------------------------------------------------------------ */
/* follow logic (SetWinEventHook + dock math)                          */

static void dock_reposition(bool force)
{
    if (!g.hwnd || g.manual || !g.target || !IsWindow(g.target)) {
        return;
    }
    UINT dpi = wnd_dpi(g.hwnd);
    RECT r;
    if (!GetWindowRect(g.target, &r)) {
        return;
    }
    LONG x = r.right + MulDiv(DOCK_GAP_X96, dpi, 96);
    LONG y = r.top + MulDiv(DOCK_YOFF96, dpi, 96);

    HMONITOR mon = MonitorFromWindow(g.target, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        if (x + g.w > mi.rcWork.right) {
            x = mi.rcWork.right - g.w;
        }
        if (x < mi.rcWork.left) {
            x = mi.rcWork.left;
        }
        if (y < mi.rcWork.top) {
            y = mi.rcWork.top;
        }
        if (y + g.h > mi.rcWork.bottom) {
            y = mi.rcWork.bottom - g.h;
        }
    }
    if (!force && x == g.lastDock.left && y == g.lastDock.top) {
        return;
    }
    g.lastDock.left = x;
    g.lastDock.top = y;
    g.lastDock.right = x + g.w;
    g.lastDock.bottom = y + g.h;
    g.everDocked = true;
    /* insert directly above the scrcpy window (owner) */
    SetWindowPos(g.hwnd, g.target, x, y, 0, 0,
                 SWP_NOACTIVATE | SWP_NOSIZE
                 | SWP_NOSENDCHANGING);
}

/* Toggle HWND_TOPMOST on BOTH the scrcpy window and the toolbar. */
static void apply_pin_state(void)
{
    if (g.target && IsWindow(g.target)) {
        SetWindowPos(g.target, g.pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    }
    if (g.hwnd) {
        SetWindowPos(g.hwnd, g.pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    }
}

static void show_scrdock(void)
{
    if (g.hwnd && !g.shown) {
        g.shown = true;
        ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
    }
}

static void adopt_target(HWND hwnd)
{
    g.target = hwnd;
    KillTimer(g.hwnd, TIMER_HUNT);
    /* Tie our z-order to the scrcpy window (QtScrcpy uses the same idea via
     * Qt::Tool): an owned popup always stays ABOVE its owner — including a
     * fullscreen owner — but sinks with it when unrelated windows cover it,
     * instead of floating above the whole desktop like WS_EX_TOPMOST. */
    SetWindowLongPtrW(g.hwnd, GWLP_HWNDPARENT, (LONG_PTR) hwnd);
    if (g.pinned) {
        apply_pin_state();
    }
    show_scrdock();
    dock_reposition(true);
}

static void hunting_start(void)
{
    g.target = NULL;
    SetTimer(g.hwnd, TIMER_HUNT, 200, NULL);
}

/* Runs on the UI thread only: installed with WINEVENT_OUTOFCONTEXT, the
 * system marshals events into the queue of the thread that called
 * SetWinEventHook, and the callback fires while that thread pumps messages. */
static VOID CALLBACK win_event_cb(HWINEVENTHOOK hook, DWORD ev, HWND hwnd,
                                  LONG id_object, LONG id_child,
                                  DWORD id_event, DWORD time)
{
    (void) hook; (void) id_event; (void) time;

    /* LOCATIONCHANGE also fires for OBJID_CURSOR (every mouse move) and for
     * child objects; filter or this runs thousands of times per minute. */
    if (id_object != OBJID_WINDOW || id_child != CHILDID_SELF) {
        return;
    }

    switch (ev) {
    case EVENT_OBJECT_LOCATIONCHANGE: /* move AND resize */
        if (hwnd == g.target) {
            dock_reposition(false);
        }
        break;
    case EVENT_OBJECT_SHOW: /* hidden SDL window becomes visible (1st frame) */
        if (hwnd == g.target) {
            dock_reposition(true);
        } else if (!g.target) {
            wchar_t cls[64];
            if (GetClassNameW(hwnd, cls, 64) > 0
                    && wcscmp(cls, L"SDL_app") == 0) {
                adopt_target(hwnd);
            }
        }
        break;
    case EVENT_OBJECT_DESTROY:
        if (hwnd == g.target) {
            hunting_start(); /* final exit is decided by the process handle */
        }
        break;
    case EVENT_SYSTEM_MINIMIZESTART:
        if (hwnd == g.target && g.shown) {
            ShowWindow(g.hwnd, SW_HIDE);
        }
        break;
    case EVENT_SYSTEM_MINIMIZEEND:
        if (hwnd == g.target && g.shown) {
            ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
            dock_reposition(true);
        }
        break;
    default:
        break;
    }
}

static void hooks_install(DWORD pid)
{
    /* 3rd param (hmodWinEventProc) must be NULL for WINEVENT_OUTOFCONTEXT.
     * Contiguous object-event range covers DESTROY/SHOW/HIDE/.../LOCATIONCHANGE. */
    g.hookObj = SetWinEventHook(EVENT_OBJECT_DESTROY,
                                EVENT_OBJECT_LOCATIONCHANGE,
                                NULL, win_event_cb, pid, 0,
                                WINEVENT_OUTOFCONTEXT);
    g.hookSys = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART,
                                EVENT_SYSTEM_MINIMIZEEND,
                                NULL, win_event_cb, pid, 0,
                                WINEVENT_OUTOFCONTEXT);
}

static void hooks_remove(void)
{
    if (g.hookObj) {
        UnhookWinEvent(g.hookObj);
        g.hookObj = NULL;
    }
    if (g.hookSys) {
        UnhookWinEvent(g.hookSys);
        g.hookSys = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* actions                                                             */

#ifndef MAPVK_VK_TO_VSC_EX
#define MAPVK_VK_TO_VSC_EX 4
#endif

/* Post one key message with a proper lParam (scancode + extended bit), so
 * SDL3 maps it exactly like a real keypress. */
static void post_key(HWND hwnd, bool down, WORD vk, bool alt_held)
{
    UINT vsc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    LPARAM lp = 1; /* repeat count */
    if (vsc & 0xE000) {
        lp |= (LPARAM) 1 << 24; /* extended key (arrows etc.) */
    }
    lp |= (LPARAM)(vsc & 0xFF) << 16;
    if (alt_held) {
        lp |= (LPARAM) 1 << 29; /* context code for WM_SYSKEY* */
    }
    if (!down) {
        lp |= (LPARAM) 1 << 30; /* previous key state */
        lp |= (LPARAM) 1 << 31; /* transition state */
    }
    PostMessageW(hwnd, down ? WM_SYSKEYDOWN : WM_SYSKEYUP, vk, lp);
}

/* Instant control: post MOD(lalt)+key straight into the scrcpy window.
 * scrcpy processes key events without focus gating (app/src/input_manager.c
 * sc_input_manager_process_key) and injects over its own control socket in
 * about a frame — no adb spawn, no per-key "input" app_process startup
 * (the 300ms+ that made adb keyevent feel sluggish). */
static bool send_scrcpy_shortcut_ex(WORD vk, int presses)
{
    HWND t = g.target;
    if (!t || !IsWindow(t) || !vk) {
        return false;
    }
    post_key(t, true, VK_LMENU, false);
    int i;
    for (i = 0; i < presses; i++) {
        post_key(t, true, vk, true);
        post_key(t, false, vk, true);
    }
    post_key(t, false, VK_LMENU, true);
    return true;
}

static bool send_scrcpy_shortcut(WORD vk)
{
    /* Single press; scrcpy double-press shortcuts (e.g. MOD+n+n = expand
     * settings panel) use _ex(vk, 2) — input_manager.c tracks key_repeat. */
    return send_scrcpy_shortcut_ex(vk, 1);
}

/* auto: use shortcuts only when we launched scrcpy with our pinned MOD
 * (a foreign instance may use a different --shortcut-mod, in which case
 * the keys would be forwarded to the device as typed characters). */
static bool use_shortcut_controls(void)
{
    if (g.cfg.control == 1) {
        return true;
    }
    if (g.cfg.control == 2) {
        return false;
    }
    return g.launched && g.pinned_mod;
}

static void button_fire(int id)
{
    if (id == ID_CLOSE) {
        DestroyWindow(g.hwnd);
        return;
    }
    if (id == ID_PIN) {
        g.pinned = !g.pinned;
        apply_pin_state();
        InvalidateRect(g.hwnd, NULL, FALSE);
        return;
    }
    if (!g.adbOk && !use_shortcut_controls()) {
        return;
    }
    switch (id) {
    case ID_HOME:
    case ID_BACK:
    case ID_APPSW:
    case ID_POWER:
    case ID_VOLUP:
    case ID_VOLDN:
        if (use_shortcut_controls() && send_scrcpy_shortcut(kVk[id])) {
            break;
        }
        adb_spawn(L" shell input keyevent %d", kKeycode[id]);
        break;
    case ID_NOTIF:
        if (use_shortcut_controls() && send_scrcpy_shortcut(kVk[ID_NOTIF])) {
            break;
        }
        {
            uintptr_t th = _beginthreadex(NULL, 0, notif_thread,
                                          (void *) (intptr_t) 0, 0, NULL);
            if (th) {
                CloseHandle((HANDLE) th);
            }
        }
        break;
    case ID_CC:
        /* adb-only: the MOD+n+n shortcut ALWAYS expands the notification
         * panel on the first press (scrcpy input_manager.c counts key
         * presses), which pops both panels on MIUI. cmd statusbar
         * expand-settings opens the control center alone. */
        {
            uintptr_t th = _beginthreadex(NULL, 0, notif_thread,
                                          (void *) (intptr_t) 1, 0, NULL);
            if (th) {
                CloseHandle((HANDLE) th);
            }
        }
        break;
    case ID_SHOT:
        {
            if (g.shotBusy) {
                break;
            }
            wchar_t *path = malloc(sizeof(wchar_t) * MAX_PATH);
            if (!path) {
                break;
            }
            if (!make_shot_path(path, MAX_PATH)) {
                free(path);
                break;
            }
            g.shotBusy = true;
            uintptr_t th = _beginthreadex(NULL, 0, screenshot_thread, path,
                                          0, NULL);
            if (th) {
                CloseHandle((HANDLE) th);
            } else {
                g.shotBusy = false;
                free(path);
            }
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* window procedure / loop                                             */

static void show_menu(HWND hwnd, int x, int y)
{
    HMENU m = CreatePopupMenu();
    if (!m) {
        return;
    }
    AppendMenuW(m, MF_STRING, IDM_REDOCK, L"重新吸附 (&D)");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出 (&X)");
    /* Tray-icon trick: popup menus dismiss correctly only for a foreground
     * window; briefly foreground, then release via the WM_NULL nudge. */
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, x, y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void teardown(HWND hwnd)
{
    if (g.closing) {
        return;
    }
    g.closing = true;
    hooks_remove();
    KillTimer(hwnd, TIMER_HUNT);
    KillTimer(hwnd, TIMER_SYNC);
    KillTimer(hwnd, TIMER_FLASH);
    KillTimer(hwnd, TIMER_TIP);
    if (g.fGlyph) {
        DeleteObject(g.fGlyph);
        g.fGlyph = NULL;
    }
    if (g.fUi) {
        DeleteObject(g.fUi);
        g.fUi = NULL;
    }
    close_scrcpy_and_wait();
    PostQuitMessage(0);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g.hwnd = hwnd;
        ensure_fonts(wnd_dpi(hwnd));
        layout_compute();
        SetWindowPos(hwnd, NULL, 0, 0, g.w, g.h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        tooltip_window_create();
        return 0;

    case WM_NCHITTEST:
        {
            /* lParam is in SCREEN coordinates here; convert to client. */
            POINT pt;
            pt.x = GET_X_LPARAM(lp);
            pt.y = GET_Y_LPARAM(lp);
            ScreenToClient(hwnd, &pt);
            /* Buttons stay HTCLIENT (clicks); the background is HTCAPTION so
             * Windows provides free dragging of the frameless toolbar. */
            if (g.shown && hit_test(pt) >= 0) {
                return HTCLIENT;
            }
            return HTCAPTION;
        }

    case WM_NCLBUTTONDBLCLK:
        g.manual = false;
        dock_reposition(true);
        return 0;

    case WM_LBUTTONDBLCLK:
        /* Over a button: the first click already fired; ignore. */
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_MOUSEMOVE:
        if (!g.tracking) {
            /* Arm TME_LEAVE only when the REAL cursor is inside the window:
             * arming it while the cursor is elsewhere makes Windows deliver
             * an immediate WM_MOUSELEAVE, wiping the hover state (this also
             * covers synthetic/programmatic WM_MOUSEMOVE input). */
            POINT rp;
            GetCursorPos(&rp);
            ScreenToClient(hwnd, &rp);
            if (rp.x >= 0 && rp.y >= 0 && rp.x < g.w && rp.y < g.h) {
                TRACKMOUSEEVENT t;
                t.cbSize = sizeof(t);
                t.dwFlags = TME_LEAVE;
                t.hwndTrack = hwnd;
                t.dwHoverTime = HOVER_DEFAULT;
                TrackMouseEvent(&t);
                g.tracking = true;
            }
        }
        {
            POINT mpt;
            mpt.x = GET_X_LPARAM(lp);
            mpt.y = GET_Y_LPARAM(lp);
            ClientToScreen(hwnd, &mpt);
            g.mousePt = mpt;
            int h = hit_test_lparam(lp);
            if (h != g.hover) {
                g.hover = h;
                tip_hide();
                InvalidateRect(hwnd, NULL, FALSE);
                if (h >= 0) {
                    /* mimic normal tooltip dwell before showing */
                    KillTimer(hwnd, TIMER_TIP);
                    SetTimer(hwnd, TIMER_TIP, 350, NULL);
                }
            } else if (g.tipShownFor >= 0) {
                /* follow the cursor while visible */
                RECT rc;
                GetWindowRect(g.tips, &rc);
                tip_position(rc.right - rc.left, rc.bottom - rc.top);
            }
        }
        return 0;

    case WM_MOUSELEAVE:
        g.tracking = false;
        tip_hide();
        if (g.hover != -1) {
            g.hover = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
        g.pressed = hit_test_lparam(lp);
        if (g.pressed >= 0) {
            SetCapture(hwnd); /* transient, for press-cancel only */
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        {
            int up = hit_test_lparam(lp);
            int down = g.pressed;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            g.pressed = -1;
            if (up >= 0 && up == down) {
                button_fire(up);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_CAPTURECHANGED:
        g.pressed = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_CONTEXTMENU:
        show_menu(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_REDOCK:
            g.manual = false;
            dock_reposition(true);
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        default:
            break;
        }
        return 0;

    case WM_ENTERSIZEMOVE:
    case WM_EXITSIZEMOVE:
        /* Fires around the user's drag of the toolbar: after the drag the
         * toolbar is in manual mode until re-docked. */
        if (msg == WM_EXITSIZEMOVE) {
            g.manual = true;
        }
        return 0;

    case WM_PAINT:
        paint_ui(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DPICHANGED:
        ensure_fonts(HIWORD(wp));
        layout_compute();
        {
            RECT *sug = (RECT *) lp;
            SetWindowPos(hwnd, NULL, sug->left, sug->top,
                         sug->right - sug->left, sug->bottom - sug->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_TIMER:
        switch (wp) {
        case TIMER_HUNT:
            {
                HWND t = find_scrcpy_window(g.pid);
                if (t) {
                    adopt_target(t);
                }
            }
            break;
        case TIMER_SYNC:
            if (g.target && !IsWindow(g.target)) {
                hunting_start();
            }
            if (!g.hProc) {
                /* attach mode without a handle: poll for process exit */
                if (!find_scrcpy_pid()) {
                    DestroyWindow(hwnd);
                    return 0;
                }
            } else if (WaitForSingleObject(g.hProc, 0) == WAIT_OBJECT_0) {
                DestroyWindow(hwnd);
                return 0;
            }
            dock_reposition(false);
            break;
        case TIMER_FLASH:
            KillTimer(hwnd, TIMER_FLASH);
            g.flashBtn = -1;
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case TIMER_TIP:
            KillTimer(hwnd, TIMER_TIP);
            if (g.hover >= 0) {
                tip_show(g.hover);
            }
            break;
        default:
            break;
        }
        return 0;

    case WM_APP_DEVREADY:
        g.devNone = (wp == 0);
        if (wp == 2 && !g.cfg.serial[0]) {
            dbg_log(L"multiple devices attached; set serial= in scrdock.ini");
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_APP_SHOTDONE:
        g.shotBusy = false;
        g.flashBtn = ID_SHOT;
        g.flashOk = (wp != 0);
        if (!g.flashOk) {
            wcscpy(g.tipStatus, L"截图失败");
        }
        KillTimer(hwnd, TIMER_FLASH);
        SetTimer(hwnd, TIMER_FLASH, 1200, NULL);
        InvalidateRect(hwnd, NULL, FALSE);
        if (g.tipShownFor == ID_SHOT && g.tips) {
            tip_show(ID_SHOT); /* refresh the visible tip with the result */
        }
        return 0;

    case WM_DESTROY:
        teardown(hwnd);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int run_loop(void)
{
    for (;;) {
        DWORD r = MsgWaitForMultipleObjectsEx(g.hProc ? 1 : 0, &g.hProc,
                                              INFINITE, QS_ALLINPUT,
                                              MWMO_INPUTAVAILABLE);
        if (g.hProc && r == WAIT_OBJECT_0) {
            break; /* scrcpy exited */
        }
        if (r == WAIT_OBJECT_0 + (g.hProc ? 1u : 0u)) {
            MSG m;
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) {
                    return (int) m.wParam;
                }
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        } else if (r == WAIT_FAILED) {
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */

static void register_and_create(void)
{
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"scrdock_cls";
    wc.style = CS_DBLCLKS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(1)); /* from scrdock.rc */
    RegisterClassW(&wc);

    /* Created hidden; shown (SW_SHOWNOACTIVATE) once the scrcpy window is
     * adopted, so the toolbar materializes directly in its docked spot.
     * NOT WS_EX_TOPMOST: ownership of the scrcpy window (set in adopt_target)
     * keeps us above it without floating above unrelated windows. */
    g.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             L"scrdock_cls", L"scrdock", WS_POPUP,
                             100, 100, 52, 420,
                             NULL, NULL, g_hInst, NULL);
}

int WINAPI wWinMain(HINSTANCE h_inst, HINSTANCE h_prev, PWSTR cmd_line,
                    int n_show)
{
    (void) h_prev; (void) cmd_line; (void) n_show;
    g_hInst = h_inst;

    init_dpi();

    SetLastError(0);
    CreateMutexW(NULL, TRUE, L"Local\\scrdock_single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    get_exe_dir(g.exeDir);
    ini_load();

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    g.hNul = CreateFileW(L"NUL", GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                         OPEN_EXISTING, 0, NULL);

    g.hover = g.pressed = g.flashBtn = -1;
    g.tipShownFor = -1;

    bool have_scrcpy = find_tool(L"scrcpy.exe", NULL, g.scrcpyPath);
    g.pid = find_scrcpy_pid();

    if (g.pid) {
        /* Attach to the running instance; keep it alive on toolbar close. */
        g.hProc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE
                              | PROCESS_QUERY_LIMITED_INFORMATION,
                              FALSE, g.pid);
        g.launched = false;
    } else {
        if (!have_scrcpy) {
            MessageBoxW(NULL,
                        L"未找到 scrcpy.exe：\n"
                        L"请把 scrdock.exe 放到 scrcpy 所在目录，\n"
                        L"或确保 scrcpy 在 PATH 中。",
                        L"scrdock", MB_ICONERROR);
            return 1;
        }
        if (!launch_scrcpy()) {
            MessageBoxW(NULL, L"启动 scrcpy 失败。", L"scrdock", MB_ICONERROR);
            return 1;
        }
    }

    if (g.cfg.adb[0]) {
        wcsncpy(g.adbPath, g.cfg.adb, MAX_PATH - 1);
        g.adbPath[MAX_PATH - 1] = L'\0';
        g.adbOk = file_exists(g.adbPath);
    } else {
        wchar_t scrcpy_dir[MAX_PATH];
        scrcpy_dir[0] = L'\0';
        if (have_scrcpy) {
            dir_of(g.scrcpyPath, scrcpy_dir, MAX_PATH);
        }
        g.adbOk = find_tool(L"adb.exe", scrcpy_dir, g.adbPath);
    }
    if (g.cfg.serial[0]) {
        wcsncpy(g.serial, g.cfg.serial, 127);
        g.serial[127] = L'\0';
    }

    register_and_create();
    if (!g.hwnd) {
        return 1;
    }

    hooks_install(g.pid);
    SetTimer(g.hwnd, TIMER_SYNC, 500, NULL);

    HWND t = find_scrcpy_window(g.pid);
    if (t) {
        adopt_target(t);
    } else {
        hunting_start();
    }

    if (g.adbOk) {
        uintptr_t th = _beginthreadex(NULL, 0, device_probe_thread, NULL,
                                      0, NULL);
        if (th) {
            CloseHandle((HANDLE) th);
        }
    }

    int rc = run_loop();

    if (IsWindow(g.hwnd)) {
        DestroyWindow(g.hwnd);
    }
    if (g.launched && !g.everDocked
            && GetTickCount64() - g.launchTick < 5000) {
        MessageBoxW(NULL,
                    L"scrcpy 启动后立即退出。\n"
                    L"请检查设备是否已连接（可先手动运行 scrcpy 查看报错）。",
                    L"scrdock", MB_ICONWARNING);
    }

    if (g.hProc) {
        CloseHandle(g.hProc);
        g.hProc = NULL;
    }
    if (g.hNul && g.hNul != INVALID_HANDLE_VALUE) {
        CloseHandle(g.hNul);
        g.hNul = NULL;
    }
    return rc;
}
