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
 * - Device tracking via a light "adb devices" poll (1.5 s, change-driven);
 *   the manager window (设备/参数/路径 tabs) picks the target device, edits
 *   options and paths, and shows scrcpy's stderr on failures. scrcpy is
 *   always launched with -s <serial>; an unexpected disconnect (scrcpy exit
 *   code 2) auto-reconnects once the device is back (bounded attempts).
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
#include <commctrl.h>
#include <commdlg.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

#define WM_APP_DEVLIST  (WM_APP + 1)  /* wp: struct DevList* (owned)   */
#define WM_APP_DEVMETA  (WM_APP + 2)  /* wp: struct DevMetaBatch*      */
#define WM_APP_SHOTDONE (WM_APP + 3)
#define WM_APP_PROCEXIT (WM_APP + 4)  /* wp: exit code, lp: proc gen   */
#define WM_APP_DETECTED (WM_APP + 5)  /* wp: heap wchar[] text         */
#define WM_APP_WIFIDONE (WM_APP + 6)  /* wp: heap WifiResult*          */
#define WM_APP_SESEXIT  (WM_APP + 7)  /* wp: session index             */

#define TIMER_HUNT  1  /* 200 ms: poll for the scrcpy window            */
#define TIMER_SYNC  2  /* 500 ms: safety net (missed events, pid polls) */
#define TIMER_FLASH 3  /* 1.2 s: screenshot result flash                */
#define TIMER_TIP   4  /* 350 ms: tooltip dwell delay                   */
#define TIMER_RECONN 5 /* 1.5 s: one-shot reconnect delay               */

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
    ID_NOTIF, ID_CC, ID_PIN, ID_SHOT, ID_MGR, ID_CLOSE, ID_COUNT
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
    { L"\uE712", L"···",  L"管理：设备 / 参数 / 路径" },
    { L"\uE8BB", L"X",    L"关闭（同时退出 scrcpy）" },
};

static const int kKeycode[ID_COUNT] = { 3, 4, 187, 26, 24, 25, 0, 0, 0, 0, 0, 0 };

/* scrcpy shortcut keys (MOD=lalt, pinned at launch; see launch_scrcpy). */
static const WORD kVk[ID_COUNT] =
    { 'H', 'B', 'S', 'P', VK_UP, VK_DOWN, 'N', 0, 0, 0, 0, 0 };

#define DEV_MAX 8

struct DevInfo {
    wchar_t serial[64];
    wchar_t state[16];       /* device / offline / unauthorized / ... */
};

struct DevList {
    int count;
    struct DevInfo v[DEV_MAX];
};

struct DevMeta {
    wchar_t serial[64];
    wchar_t model[64];
    wchar_t android[16];
    int battery;             /* percent, -1 = unknown */
    wchar_t ip[20];          /* wlan address of USB devices, empty = n/a */
};

struct DevMetaBatch {
    int count;
    int scrW, scrH;          /* wm size of the selected device (0 = skip) */
    struct DevMeta v[DEV_MAX];
};

struct Config {
    wchar_t serial[128];
    wchar_t scrcpy[MAX_PATH];   /* empty = auto-locate scrcpy.exe       */
    wchar_t scrcpy_args[512];
    wchar_t adb[MAX_PATH];
    BOOL close_on_exit;
    int control;             /* 0=auto 1=shortcut 2=adb */
    int bitrate;             /* Mbps;  0 = scrcpy default               */
    int max_size;            /* 0 = default                             */
    int max_fps;             /* 0 = default                             */
    BOOL turn_screen_off;
    BOOL stay_awake;
    BOOL no_audio;
    BOOL show_touches;
    BOOL auto_reconnect;
    int reconnect_attempts;
    BOOL wireless;           /* launch with --tcpip (USB -> WiFi switch) */
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
    /* manager + device tracking */
    struct DevList *devList;    /* latest track-devices snapshot (owned)  */
    struct DevMetaBatch devMeta;
    bool metaBusy;
    bool metaPending;
    HANDLE hTrack;          /* device-poller thread handle              */
    HANDLE trackStop;       /* manual-reset stop event for the poller   */
    bool trackOn;           /* tracker thread started                   */
    HANDLE hErr;            /* scrcpy stderr pipe, read end             */
    char errBuf[4096];      /* rolling tail of scrcpy stderr (UTF-8)    */
    unsigned procGen;       /* bumped per launch; tags stale PROCEXIT    */
    bool reconnPending;
    int reconnAttempts;
    bool bootLaunch;        /* launch after the first DEVLIST (serial)  */
    wchar_t errLine[256];   /* last ERROR line from scrcpy stderr       */
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

/* ---- multi-session model ------------------------------------------------
 *
 * Up to SES_MAX scrcpy sessions run side by side, one per device. The
 * ACTIVE session's handles live in g.* (pid/hProc/hErr/target/gen/...), so
 * every existing code path (toolbar, shortcuts, dock, reconnect) keeps
 * operating on exactly one session with no changes. Background sessions
 * keep their handles in their slot; promoting a slot swaps the fields
 * between g.* and the previous active slot (ses_become_active). */

#define SES_MAX 4

struct Session {
    bool used;
    wchar_t serial[64];
    DWORD pid;
    HANDLE hProc;            /* NULL while this slot IS the active one */
    HANDLE hErr;
    HWND target;
    unsigned gen;
    ULONGLONG launchTick;
    int reconnAttempts;
    bool reconnPending;
};

static struct Session g_ses[SES_MAX];
static int g_active = -1;   /* slot whose handles currently live in g.* */
static unsigned g_genSeq;   /* unique generation counter across sessions */

static int ses_find(const wchar_t *serial)
{
    int i;
    for (i = 0; i < SES_MAX; i++) {
        if (g_ses[i].used && wcscmp(g_ses[i].serial, serial) == 0) {
            return i;
        }
    }
    return -1;
}

static int ses_free_slot(void)
{
    int i;
    for (i = 0; i < SES_MAX; i++) {
        if (!g_ses[i].used) {
            return i;
        }
    }
    return -1;
}

static void ses_free(int idx)
{
    if (idx < 0 || idx >= SES_MAX || !g_ses[idx].used) {
        return;
    }
    if (g_ses[idx].hErr) {
        CloseHandle(g_ses[idx].hErr);
    }
    if (g_ses[idx].hProc) {
        CloseHandle(g_ses[idx].hProc);
    }
    ZeroMemory(&g_ses[idx], sizeof(g_ses[idx]));
    if (g_active == idx) {
        g_active = -1;
    }
}

static int ses_first_used(void)
{
    int i;
    for (i = 0; i < SES_MAX; i++) {
        if (g_ses[i].used) {
            return i;
        }
    }
    return -1;
}

/* forward declarations for cross-section calls (single translation unit) */
static void hooks_install(DWORD pid);
static void hooks_remove(void);
static void hunting_start(void);
static void err_drain(bool final);
static void mgr_open(void);
static void mgr_refresh(void);
static void mgr_toggle(void);
static void meta_spawn(void);
static bool dev_online(const wchar_t *serial);
static void reconn_arm(void);
static void ses_become_active(int idx);

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
    g.cfg.scrcpy[0] = L'\0';
    g.cfg.scrcpy_args[0] = L'\0';
    g.cfg.adb[0] = L'\0';
    g.cfg.close_on_exit = TRUE;
    g.cfg.control = 0;
    g.cfg.bitrate = 0;
    g.cfg.max_size = 0;
    g.cfg.max_fps = 0;
    g.cfg.turn_screen_off = FALSE;
    g.cfg.stay_awake = FALSE;
    g.cfg.no_audio = FALSE;
    g.cfg.show_touches = FALSE;
    g.cfg.auto_reconnect = TRUE;
    g.cfg.reconnect_attempts = 5;
    g.cfg.wireless = FALSE;

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
    GetPrivateProfileStringA("scrdock", "scrcpy", "", buf, sizeof(buf), ini_a);
    MultiByteToWideChar(CP_ACP, 0, buf, -1, g.cfg.scrcpy, MAX_PATH);
    g.cfg.bitrate = GetPrivateProfileIntA("scrdock", "bitrate", 0, ini_a);
    g.cfg.max_size = GetPrivateProfileIntA("scrdock", "max_size", 0, ini_a);
    g.cfg.max_fps = GetPrivateProfileIntA("scrdock", "max_fps", 0, ini_a);
    g.cfg.turn_screen_off =
        GetPrivateProfileIntA("scrdock", "turn_screen_off", 0, ini_a) != 0;
    g.cfg.stay_awake =
        GetPrivateProfileIntA("scrdock", "stay_awake", 0, ini_a) != 0;
    g.cfg.no_audio =
        GetPrivateProfileIntA("scrdock", "no_audio", 0, ini_a) != 0;
    g.cfg.show_touches =
        GetPrivateProfileIntA("scrdock", "show_touches", 0, ini_a) != 0;
    g.cfg.auto_reconnect =
        GetPrivateProfileIntA("scrdock", "auto_reconnect", 1, ini_a) != 0;
    g.cfg.reconnect_attempts =
        GetPrivateProfileIntA("scrdock", "reconnect_attempts", 5, ini_a);
    g.cfg.wireless =
        GetPrivateProfileIntA("scrdock", "wireless", 0, ini_a) != 0;
}

/* Persist the config. Called on the UI thread only (manager buttons and
 * device selection). Empty values remove the key, so a fresh ini keeps
 * exactly the non-default settings. */
static void ini_save(void)
{
    wchar_t ini[MAX_PATH];
    char ini_a[MAX_PATH];
    char val[1100];

    swprintf(ini, MAX_PATH, L"%s\\scrdock.ini", g.exeDir);
    WideCharToMultiByte(CP_ACP, 0, ini, -1, ini_a, MAX_PATH, NULL, NULL);

    WideCharToMultiByte(CP_ACP, 0, g.cfg.serial, -1, val, 128, NULL, NULL);
    WritePrivateProfileStringA("scrdock", "serial",
                               g.cfg.serial[0] ? val : NULL, ini_a);
    WideCharToMultiByte(CP_ACP, 0, g.cfg.scrcpy, -1, val, sizeof(val), NULL, NULL);
    WritePrivateProfileStringA("scrdock", "scrcpy",
                               g.cfg.scrcpy[0] ? val : NULL, ini_a);
    WideCharToMultiByte(CP_ACP, 0, g.cfg.scrcpy_args, -1, val, sizeof(val), NULL, NULL);
    WritePrivateProfileStringA("scrdock", "scrcpy_args",
                               g.cfg.scrcpy_args[0] ? val : NULL, ini_a);
    WideCharToMultiByte(CP_ACP, 0, g.cfg.adb, -1, val, sizeof(val), NULL, NULL);
    WritePrivateProfileStringA("scrdock", "adb",
                               g.cfg.adb[0] ? val : NULL, ini_a);
    WritePrivateProfileStringA("scrdock", "close_scrcpy_on_exit",
                               g.cfg.close_on_exit ? "1" : "0", ini_a);
    const char *ctl = (g.cfg.control == 1) ? "shortcut"
                    : (g.cfg.control == 2) ? "adb" : "auto";
    WritePrivateProfileStringA("scrdock", "control", ctl, ini_a);
    _itoa(g.cfg.bitrate, val, 10);
    WritePrivateProfileStringA("scrdock", "bitrate",
                               g.cfg.bitrate > 0 ? val : NULL, ini_a);
    _itoa(g.cfg.max_size, val, 10);
    WritePrivateProfileStringA("scrdock", "max_size",
                               g.cfg.max_size > 0 ? val : NULL, ini_a);
    _itoa(g.cfg.max_fps, val, 10);
    WritePrivateProfileStringA("scrdock", "max_fps",
                               g.cfg.max_fps > 0 ? val : NULL, ini_a);
    WritePrivateProfileStringA("scrdock", "turn_screen_off",
                               g.cfg.turn_screen_off ? "1" : NULL, ini_a);
    WritePrivateProfileStringA("scrdock", "stay_awake",
                               g.cfg.stay_awake ? "1" : NULL, ini_a);
    WritePrivateProfileStringA("scrdock", "no_audio",
                               g.cfg.no_audio ? "1" : NULL, ini_a);
    WritePrivateProfileStringA("scrdock", "show_touches",
                               g.cfg.show_touches ? "1" : NULL, ini_a);
    WritePrivateProfileStringA("scrdock", "auto_reconnect",
                               g.cfg.auto_reconnect ? "1" : "0", ini_a);
    _itoa(g.cfg.reconnect_attempts, val, 10);
    WritePrivateProfileStringA("scrdock", "reconnect_attempts", val, ini_a);
    WritePrivateProfileStringA("scrdock", "wireless",
                               g.cfg.wireless ? "1" : NULL, ini_a);
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

/* Saturating formatted append (plain swprintf returns -1 on truncation,
 * which would corrupt the running index). */
static void cmd_catf(wchar_t *out, size_t cch, int *n, const wchar_t *fmt, ...)
{
    if (*n < 0 || (size_t) *n >= cch - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int k = vswprintf(out + *n, cch - (size_t) *n, fmt, ap);
    va_end(ap);
    *n = (k < 0) ? (int) (cch - 1) : *n + k;
}

/* Compose the scrcpy command line: -s <serial> so the session can never
 * silently drift to another device, then the structured options, then the
 * raw scrcpy_args tail (scrcpy applies options in order, so the tail wins
 * and can override anything above). */
static void build_scrcpy_cmd(wchar_t *cmd, size_t cch, const wchar_t *serial)
{
    int n = 0;
    cmd[0] = L'\0';
    cmd_catf(cmd, cch, &n, L"\"%s\"", g.scrcpyPath);
    if (serial[0]) {
        cmd_catf(cmd, cch, &n, L" -s %s", serial);
    }
    /* Pin MOD=Left-Alt so the toolbar can send instant MOD+key shortcuts to
     * the scrcpy window (see send_scrcpy_shortcut). Skipped if the user
     * already passes --shortcut-mod of their own. */
    g.pinned_mod = (wcsstr(g.cfg.scrcpy_args, L"--shortcut-mod") == NULL);
    if (g.pinned_mod) {
        cmd_catf(cmd, cch, &n, L" --shortcut-mod=lalt");
    }
    if (g.cfg.max_size > 0) {
        cmd_catf(cmd, cch, &n, L" --max-size=%d", g.cfg.max_size);
    }
    if (g.cfg.bitrate > 0) {
        cmd_catf(cmd, cch, &n, L" --video-bit-rate=%dM", g.cfg.bitrate);
    }
    if (g.cfg.max_fps > 0) {
        cmd_catf(cmd, cch, &n, L" --max-fps=%d", g.cfg.max_fps);
    }
    if (g.cfg.turn_screen_off) {
        cmd_catf(cmd, cch, &n, L" --turn-screen-off");
    }
    if (g.cfg.stay_awake) {
        cmd_catf(cmd, cch, &n, L" --stay-awake");
    }
    if (g.cfg.no_audio) {
        cmd_catf(cmd, cch, &n, L" --no-audio");
    }
    if (g.cfg.show_touches) {
        cmd_catf(cmd, cch, &n, L" --show-touches");
    }
    if (g.cfg.wireless && (!serial[0] || !wcschr(serial, L':'))) {
        /* USB -> WiFi switch; on an ip:port serial it would be redundant */
        cmd_catf(cmd, cch, &n, L" --tcpip");
    }
    if (g.cfg.scrcpy_args[0]) {
        cmd_catf(cmd, cch, &n, L" %s", g.cfg.scrcpy_args);
    }
}

/* Launch a new scrcpy session for `serial`.
 * activate=true: the session becomes the toolbar-controlled one (fields in
 * g.*, hooks installed, hunting started). activate=false: it runs in the
 * background slot for that serial. */
static bool launch_scrcpy_ex(const wchar_t *serial, bool activate)
{
    wchar_t cmd[1400];
    wchar_t dir[MAX_PATH];
    struct Session *slot = NULL;
    int idx;

    /* slot bookkeeping: one session per serial (resolved before touching
     * the active state below) */
    idx = ses_find(serial);
    if (idx < 0) {
        idx = ses_free_slot();
        if (idx < 0) {
            return false; /* session table full */
        }
        ZeroMemory(&g_ses[idx], sizeof(g_ses[idx]));
        g_ses[idx].used = true;
        wcsncpy(g_ses[idx].serial, serial, 63);
    }
    slot = &g_ses[idx];
    if (slot->hErr) {
        CloseHandle(slot->hErr);
        slot->hErr = NULL;
    }
    if (slot->hProc) {
        CloseHandle(slot->hProc);
        slot->hProc = NULL;
    }

    if (activate) {
        KillTimer(g.hwnd, TIMER_RECONN);
        g.reconnPending = false;
        if (g.hErr) {
            err_drain(true);
        }
        if (g_active >= 0 && g_active != idx) {
            /* the running active session moves to the background and keeps
             * running; its handles go back to its slot (waited by run_loop) */
            struct Session *cur = &g_ses[g_active];
            cur->hProc = g.hProc;
            cur->hErr = g.hErr;
            cur->target = g.target;
            cur->pid = g.pid;
            cur->gen = g.procGen;
            cur->launchTick = g.launchTick;
            cur->reconnAttempts = g.reconnAttempts;
            cur->reconnPending = false; /* paused while in the background */
            wcsncpy(cur->serial, g.serial, 63);
            cur->serial[63] = L'\0';
        } else if (g.hProc) {
            /* same session relaunched: retire its previous child */
            CloseHandle(g.hProc);
            g.hProc = NULL;
        }
        g.errBuf[0] = '\0';
        g.errLine[0] = L'\0';
    }

    build_scrcpy_cmd(cmd, 1400, serial);
    dir_of(g.scrcpyPath, dir, MAX_PATH);

    /* stderr -> pipe: launch failures become visible instead of vanishing
     * into the void of CREATE_NO_WINDOW (e.g. "Multiple (2) ADB devices"). */
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (CreatePipe(&rd, &wr, &sa, 0)) {
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    } else {
        rd = wr = NULL;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    if (wr) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = g.hNul;
        si.hStdError = wr;
        si.hStdInput = NULL;
    }

    /* CREATE_NO_WINDOW: scrcpy.exe is a console app; never flash a console.
     * Working directory = scrcpy dir so a portable dist finds scrcpy-server. */
    if (!CreateProcessW(NULL, cmd, NULL, NULL, wr != NULL, CREATE_NO_WINDOW,
                        NULL, dir[0] ? dir : NULL, &si, &pi)) {
        dbg_log(L"launch_scrcpy: CreateProcessW failed (%lu)", GetLastError());
        if (rd) {
            CloseHandle(rd);
        }
        if (wr) {
            CloseHandle(wr);
        }
        if (!g_ses[idx].pid) {
            ses_free(idx); /* nothing ever ran on this fresh slot */
        }
        return false;
    }
    CloseHandle(pi.hThread);
    if (wr) {
        CloseHandle(wr); /* the child holds the only write end now */
    }
    slot->pid = pi.dwProcessId;
    slot->launchTick = GetTickCount64();
    slot->gen = ++g_genSeq;
    slot->reconnAttempts = 0;
    slot->reconnPending = false;

    if (activate) {
        g.hErr = rd;
        g.hProc = pi.hProcess;
        g.pid = slot->pid;
        g.launched = true;
        g.launchTick = slot->launchTick;
        g.procGen = slot->gen;
        g.reconnAttempts = 0;
        g.reconnPending = false;
        wcsncpy(g.serial, serial, 127);
        g.serial[127] = L'\0';
        g_active = idx;

        /* a brand new process: re-target the window hooks and hunt for the
         * SDL window (the toolbar re-docks when it appears) */
        g.target = NULL;
        hooks_install(g.pid);
        hunting_start();
    } else {
        slot->hErr = rd;
        slot->hProc = pi.hProcess;
    }
    return true;
}

static bool launch_scrcpy(void)
{
    return launch_scrcpy_ex(g.serial, true);
}

/* Close scrcpy gracefully (WM_CLOSE -> SDL3 quit-on-last-window-close),
 * pumping messages while waiting: the EVENT_OBJECT_DESTROY callback for the
 * dying window is marshaled to this same thread. Always closes the child we
 * launched (used both for toolbar exit and for switching devices). */
static void close_scrcpy_and_wait(void)
{
    if (!g.launched || !g.hProc) {
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
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    /* re-post: the outer loop must still see it */
                    PostQuitMessage((int) msg.wParam);
                    return;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
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

/* ------------------------------------------------------------------ */
/* device tracking: resident "adb track-devices" reader + metadata      */

static bool dev_online(const wchar_t *serial)
{
    int i;
    if (!g.devList || !serial[0]) {
        return false;
    }
    for (i = 0; i < g.devList->count; i++) {
        if (wcscmp(g.devList->v[i].serial, serial) == 0
                && wcscmp(g.devList->v[i].state, L"device") == 0) {
            return true;
        }
    }
    return false;
}

static int dev_count_online(void)
{
    int i, n = 0;
    if (g.devList) {
        for (i = 0; i < g.devList->count; i++) {
            if (wcscmp(g.devList->v[i].state, L"device") == 0) {
                n++;
            }
        }
    }
    return n;
}

/* ---- scrcpy stderr tail (failure surfacing) ---- */

static void err_append(const char *s, size_t n)
{
    size_t len = strlen(g.errBuf);
    if (len + n >= sizeof(g.errBuf)) {
        size_t drop = len + n + 1 - sizeof(g.errBuf);
        memmove(g.errBuf, g.errBuf + drop, len - drop + 1);
        len = strlen(g.errBuf);
    }
    memcpy(g.errBuf + len, s, n);
    g.errBuf[len + n] = '\0';
}

/* Pump the stderr pipe into the rolling buffer (non-blocking) so a verbose
 * session can never fill the pipe and stall scrcpy; on the final drain the
 * read handle is closed as well. UI thread only. */
static void err_drain(bool final)
{
    if (!g.hErr) {
        return;
    }
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g.hErr, NULL, 0, NULL, &avail, NULL) || !avail) {
            break;
        }
        char chunk[512];
        DWORD want = avail < sizeof(chunk) ? avail : (DWORD) sizeof(chunk);
        DWORD rd = 0;
        if (!ReadFile(g.hErr, chunk, want, &rd, NULL) || !rd) {
            break;
        }
        err_append(chunk, rd);
    }
    if (final) {
        CloseHandle(g.hErr);
        g.hErr = NULL;
    }
}

/* Last "ERROR" line from scrcpy's stderr, else the buffer tail. */
static void err_last_line(wchar_t *out, size_t cch)
{
    const char *base = g.errBuf;
    const char *found = NULL;
    const char *p = g.errBuf;
    while ((p = strstr(p, "ERROR")) != NULL) {
        found = p;
        p += 5;
    }
    if (!found) {
        size_t len = strlen(g.errBuf);
        if (len > 200) {
            base = g.errBuf + len - 200;
        }
        found = base;
    }
    const char *e = strchr(found, '\n');
    size_t n = e ? (size_t) (e - found) : strlen(found);
    if (n > 200) {
        n = 200;
    }
    int w = MultiByteToWideChar(CP_UTF8, 0, found, (int) n, out, (int) cch - 1);
    if (w < 0) {
        w = 0;
    }
    out[w] = L'\0';
    while (w > 0 && (out[w - 1] == L'\r' || out[w - 1] == L'\n'
                     || out[w - 1] == L' ')) {
        out[--w] = L'\0';
    }
}

/* ---- device polling ---- */

/* "adb devices" output lines: "List of devices attached", then one
 * "<serial>\t<state>" per device (state: device/offline/unauthorized/...). */
static void devlist_parse(const char *text, struct DevList *dl)
{
    char *dup = _strdup(text ? text : "");
    if (!dup) {
        return;
    }
    char *ctx = NULL;
    char *line = strtok_s(dup, "\r\n", &ctx);
    while (line && dl->count < DEV_MAX) {
        char sp[64], st[16];
        if (sscanf(line, "%63s %15s", sp, st) == 2
                && strncmp(sp, "List", 4) != 0) {
            MultiByteToWideChar(CP_ACP, 0, sp, -1,
                                dl->v[dl->count].serial, 64);
            MultiByteToWideChar(CP_ACP, 0, st, -1,
                                dl->v[dl->count].state, 16);
            dl->count++;
        }
        line = strtok_s(NULL, "\r\n", &ctx);
    }
    free(dup);
}

/* Poll "adb devices" every 1.5 s and post a fresh DevList when it changed.
 *
 * Why polling instead of "adb track-devices": the CLI output of
 * track-devices has NO framing between snapshots (verified empirically: the
 * initial update is just "serial\tstate\r\n" with no blank-line separator),
 * so a stream parser cannot tell where one snapshot ends. Speaking the adb
 * smart-socket protocol directly would fix that, but a light poll also
 * survives adb-server restarts for free. */
static unsigned __stdcall track_thread(void *arg)
{
    (void) arg;
    struct DevList last;
    bool have_last = false;
    wchar_t cmd[MAX_PATH + 32];
    char buf[2048];
    DWORD ec = 0;

    swprintf(cmd, MAX_PATH + 32, L"\"%s\" devices", g.adbPath);
    for (;;) {
        buf[0] = '\0';
        if (capture_sync(cmd, buf, sizeof(buf), 10000, &ec)) {
            struct DevList dl;
            ZeroMemory(&dl, sizeof(dl));
            devlist_parse(buf, &dl);
            if (!have_last || memcmp(&last, &dl, sizeof(dl)) != 0) {
                struct DevList *heap_dl = malloc(sizeof(*heap_dl));
                if (heap_dl) {
                    *heap_dl = dl;
                    last = dl;
                    have_last = true;
                    if (g.hwnd) {
                        PostMessageW(g.hwnd, WM_APP_DEVLIST,
                                     (WPARAM) heap_dl, 0);
                    } else {
                        free(heap_dl);
                    }
                }
            }
        }
        if (g.trackStop
                && WaitForSingleObject(g.trackStop, 1500) == WAIT_OBJECT_0) {
            break;
        }
    }
    return 0;
}

static void track_start(void)
{
    if (g.trackOn || !g.adbOk || !g.hwnd) {
        return;
    }
    g.trackStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g.trackStop) {
        return;
    }
    uintptr_t th = _beginthreadex(NULL, 0, track_thread, NULL, 0, NULL);
    if (th) {
        g.trackOn = true;
        g.hTrack = (HANDLE) th;
    } else {
        CloseHandle(g.trackStop);
        g.trackStop = NULL;
    }
}

static void track_stop(void)
{
    if (g.trackStop) {
        SetEvent(g.trackStop);
    }
    if (g.hTrack) {
        WaitForSingleObject(g.hTrack, 2000);
        CloseHandle(g.hTrack);
        g.hTrack = NULL;
    }
    if (g.trackStop) {
        CloseHandle(g.trackStop);
        g.trackStop = NULL;
    }
    g.trackOn = false;
}

/* ---- device metadata (model / android / battery / wm size) ---- */

struct MetaWork {
    int count;
    wchar_t sel[64];
    wchar_t serials[DEV_MAX][64];
};

static void meta_getprop(const wchar_t *serial, const char *prop,
                         wchar_t *out, size_t cch)
{
    wchar_t cmd[MAX_PATH + 160];
    char buf[256];
    DWORD ec = 0;
    /* %S: narrow string inside a wide format (MSVC wide printf) */
    swprintf(cmd, MAX_PATH + 160, L"\"%s\" -s %s shell getprop %S",
             g.adbPath, serial, prop);
    out[0] = L'\0';
    if (capture_sync(cmd, buf, sizeof(buf), 8000, &ec) && ec == 0) {
        char *e = buf + strlen(buf);
        while (e > buf && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ')) {
            *--e = '\0';
        }
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, out, (int) cch - 1);
    }
}

static int meta_battery(const wchar_t *serial)
{
    wchar_t cmd[MAX_PATH + 96];
    char buf[4096];
    DWORD ec = 0;
    swprintf(cmd, MAX_PATH + 96, L"\"%s\" -s %s shell dumpsys battery",
             g.adbPath, serial);
    if (capture_sync(cmd, buf, sizeof(buf), 8000, &ec)) {
        const char *p = strstr(buf, " level:");
        if (p) {
            int v = atoi(p + 7);
            if (v > 0 && v <= 100) {
                return v;
            }
        }
    }
    return -1;
}

/* wlan address of a USB-connected device: parse "src <ip>" from ip route */
static void meta_ip(const wchar_t *serial, wchar_t *out, size_t cch)
{
    wchar_t cmd[MAX_PATH + 96];
    char buf[1024];
    DWORD ec = 0;
    out[0] = L'\0';
    swprintf(cmd, MAX_PATH + 96, L"\"%s\" -s %s shell ip route",
             g.adbPath, serial);
    if (!capture_sync(cmd, buf, sizeof(buf), 8000, &ec)) {
        return;
    }
    const char *p = strstr(buf, " src ");
    if (!p) {
        return;
    }
    p += 5;
    char ip[24];
    int i = 0;
    while (p[i] && p[i] != ' ' && p[i] != '\r' && p[i] != '\n'
            && i < (int) sizeof(ip) - 1) {
        ip[i] = p[i];
        i++;
    }
    ip[i] = '\0';
    if (i >= 7) { /* shortest "x.x.x.x" */
        MultiByteToWideChar(CP_ACP, 0, ip, -1, out, (int) cch - 1);
    }
}

/* ---- remembered WiFi addresses ([devices] ini section) ---- */

struct WifiMap {
    wchar_t serial[64];      /* USB serial                             */
    wchar_t ip[24];          /* last known wlan address                */
};

static struct WifiMap g_wifiMap[DEV_MAX];

static void wifi_map_load(void)
{
    char buf[4096];
    wchar_t ini[MAX_PATH];
    char ini_a[MAX_PATH];
    swprintf(ini, MAX_PATH, L"%s\\scrdock.ini", g.exeDir);
    WideCharToMultiByte(CP_ACP, 0, ini, -1, ini_a, MAX_PATH, NULL, NULL);
    if (!file_exists(ini)) {
        return;
    }
    ZeroMemory(buf, sizeof(buf));
    GetPrivateProfileSectionA("devices", buf, sizeof(buf), ini_a);
    char *p = buf;
    int n = 0;
    while (*p && n < DEV_MAX) {
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            MultiByteToWideChar(CP_ACP, 0, p, -1,
                                g_wifiMap[n].serial, 64);
            MultiByteToWideChar(CP_ACP, 0, eq + 1, -1,
                                g_wifiMap[n].ip, 24);
            n++;
            *eq = '=';
        }
        p += strlen(p) + 1;
    }
}

static void wifi_map_set(const wchar_t *serial, const wchar_t *ip)
{
    int i;
    if (!serial[0] || !ip[0] || wcschr(serial, L':')) {
        return; /* only map USB serials */
    }
    for (i = 0; i < DEV_MAX; i++) {
        if (g_wifiMap[i].serial[0]
                && wcscmp(g_wifiMap[i].serial, serial) == 0) {
            if (wcscmp(g_wifiMap[i].ip, ip) == 0) {
                return;
            }
            wcsncpy(g_wifiMap[i].ip, ip, 23);
            g_wifiMap[i].ip[23] = L'\0';
            break;
        }
        if (!g_wifiMap[i].serial[0]) {
            wcsncpy(g_wifiMap[i].serial, serial, 63);
            g_wifiMap[i].serial[63] = L'\0';
            wcsncpy(g_wifiMap[i].ip, ip, 23);
            g_wifiMap[i].ip[23] = L'\0';
            break;
        }
    }
    wchar_t ini[MAX_PATH];
    char ini_a[MAX_PATH];
    char s_a[64], i_a[24];
    swprintf(ini, MAX_PATH, L"%s\\scrdock.ini", g.exeDir);
    WideCharToMultiByte(CP_ACP, 0, ini, -1, ini_a, MAX_PATH, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, serial, -1, s_a, sizeof(s_a), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, ip, -1, i_a, sizeof(i_a), NULL, NULL);
    WritePrivateProfileStringA("devices", s_a, i_a, ini_a);
}

static const wchar_t *wifi_ip_of(const wchar_t *serial)
{
    int i;
    for (i = 0; i < DEV_MAX; i++) {
        if (g_wifiMap[i].serial[0]
                && wcscmp(g_wifiMap[i].serial, serial) == 0) {
            return g_wifiMap[i].ip;
        }
    }
    return NULL;
}

/* the "ip:port" serial under which this USB device is reachable now */
static const wchar_t *wifi_serial_in_list(const wchar_t *usb_serial)
{
    const wchar_t *ip = wifi_ip_of(usb_serial);
    int i;
    if (!ip || !g.devList) {
        return NULL;
    }
    for (i = 0; i < g.devList->count; i++) {
        struct DevInfo *d = &g.devList->v[i];
        if (wcschr(d->serial, L':') && wcsstr(d->serial, ip)
                && wcscmp(d->state, L"device") == 0) {
            return d->serial;
        }
    }
    return NULL;
}

static unsigned __stdcall meta_thread(void *arg)
{
    struct MetaWork *w = (struct MetaWork *) arg;
    struct DevMetaBatch *mb = calloc(1, sizeof(*mb));
    int i;
    if (mb) {
        for (i = 0; i < w->count && mb->count < DEV_MAX; i++) {
            struct DevMeta *dm = &mb->v[mb->count];
            wcsncpy(dm->serial, w->serials[i], 63);
            dm->serial[63] = L'\0';
            meta_getprop(dm->serial, "ro.product.model", dm->model, 64);
            meta_getprop(dm->serial, "ro.build.version.release",
                         dm->android, 16);
            dm->battery = meta_battery(dm->serial);
            if (!wcschr(dm->serial, L':')) {
                meta_ip(dm->serial, dm->ip, 20);
            }
            mb->count++;
        }
        if (w->sel[0]) {
            wchar_t cmd[MAX_PATH + 96];
            char buf[512];
            DWORD ec = 0;
            swprintf(cmd, MAX_PATH + 96, L"\"%s\" -s %s shell wm size",
                     g.adbPath, w->sel);
            if (capture_sync(cmd, buf, sizeof(buf), 8000, &ec)) {
                parse_size_line(buf, &mb->scrW, &mb->scrH);
            }
        }
    }
    free(w);
    if (g.hwnd) {
        PostMessageW(g.hwnd, WM_APP_DEVMETA, (WPARAM) mb, 0);
    } else {
        free(mb);
    }
    return 0;
}

/* Refresh device metadata (model/version/battery + wm size) off-thread;
 * coalesced: while a fetch runs, one more request is remembered. */
static void meta_spawn(void)
{
    int i;
    if (g.metaBusy) {
        g.metaPending = true;
        return;
    }
    if (!g.devList || !g.adbOk) {
        return;
    }
    struct MetaWork *w = calloc(1, sizeof(*w));
    if (!w) {
        return;
    }
    for (i = 0; i < g.devList->count && w->count < DEV_MAX; i++) {
        if (wcscmp(g.devList->v[i].state, L"device") == 0) {
            wcsncpy(w->serials[w->count], g.devList->v[i].serial, 63);
            w->serials[w->count][63] = L'\0';
            w->count++;
        }
    }
    if (!w->count) {
        free(w);
        return;
    }
    wcsncpy(w->sel, g.serial, 63);
    w->sel[63] = L'\0';
    g.metaBusy = true;
    uintptr_t th = _beginthreadex(NULL, 0, meta_thread, w, 0, NULL);
    if (th) {
        CloseHandle((HANDLE) th);
    } else {
        g.metaBusy = false;
        free(w);
    }
}

/* ------------------------------------------------------------------ */
/* path/version detection ---- */

static unsigned __stdcall detect_thread(void *arg)
{
    (void) arg;
    wchar_t *out = malloc(512 * sizeof(wchar_t));
    if (!out) {
        return 0;
    }
    wchar_t cmd[MAX_PATH + 32];
    char buf[512];
    DWORD ec = 0;
    wchar_t line[128];
    out[0] = L'\0';
    if (file_exists(g.scrcpyPath)) {
        swprintf(cmd, MAX_PATH + 32, L"\"%s\" --version", g.scrcpyPath);
        if (capture_sync(cmd, buf, sizeof(buf), 10000, &ec) && ec == 0) {
            char *nl = strchr(buf, '\n');
            if (nl) {
                *nl = '\0';
            }
            MultiByteToWideChar(CP_UTF8, 0, buf, -1, line, 127);
            swprintf(out + wcslen(out), 512 - wcslen(out),
                     L"scrcpy: %s\r\n", line);
        } else {
            wcscat(out, L"scrcpy: 存在但无法运行\r\n");
        }
    } else {
        wcscat(out, L"scrcpy: 未找到（请设置路径）\r\n");
    }
    if (g.adbPath[0] && file_exists(g.adbPath)) {
        swprintf(cmd, MAX_PATH + 32, L"\"%s\" version", g.adbPath);
        if (capture_sync(cmd, buf, sizeof(buf), 10000, &ec)) {
            char *nl = strchr(buf, '\n');
            if (nl) {
                *nl = '\0';
            }
            MultiByteToWideChar(CP_UTF8, 0, buf, -1, line, 127);
            swprintf(out + wcslen(out), 512 - wcslen(out), L"adb: %s", line);
        }
    } else {
        wcscat(out, L"adb: 未找到");
    }
    if (g.hwnd) {
        PostMessageW(g.hwnd, WM_APP_DETECTED, (WPARAM) out, 0);
    } else {
        free(out);
    }
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
    const wchar_t *txt = ((i == ID_SHOT || i == ID_MGR) && g.tipStatus[0])
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

    bool disabled = !g.adbOk && i != ID_CLOSE && i != ID_MGR;
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
     * Contiguous object-event range covers DESTROY/SHOW/HIDE/.../LOCATIONCHANGE.
     * Remove first: relaunching must not leak the previous hook pair. */
    hooks_remove();
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
/* manager window (设备 / 参数 / 路径)                                  */

#define MGR_W_96 600
#define MGR_H_96 528

enum { MPAGE_DEV, MPAGE_OPT, MPAGE_PATH, MPAGE_COUNT };

enum {
    MID_LV = 1, MID_REFRESH, MID_CONNECT, MID_REMEMBER, MID_DISCONNECT,
    MID_CHK_WIRELESS, MID_WIFI_ED, MID_WIFI_GO, MID_PAIR,
    MID_BITRATE, MID_MAXSIZE, MID_MAXFPS,
    MID_CHK_SCREENOFF, MID_CHK_STAYAWAKE, MID_CHK_NOAUDIO, MID_CHK_TOUCH,
    MID_CHK_RECONN, MID_RECONN_N, MID_CHK_CLOSEEXIT, MID_EXTRA,
    MID_OPT_SAVE,
    MID_SCRCPY_ED, MID_SCRCPY_BR, MID_ADB_ED, MID_ADB_BR,
    MID_DETECT, MID_PATH_SAVE,
    MID_PAIR_ADDR, MID_PAIR_CODE, MID_PAIR_GO
};

static struct {
    HWND frame;
    int tabSel;                 /* active page index                      */
    RECT tabRc[MPAGE_COUNT];    /* self-drawn tab strip cells (client)    */
    RECT hdrRc;                 /* self-drawn title bar                   */
    RECT closeRc;               /* self-drawn close cell in the header    */
    HWND page[MPAGE_COUNT];
    HWND lv, refresh, connect, remember, disconnect, status, errview;
    HWND chkWireless, wifiEd, wifiGo, pairBtn;
    HWND bitrate, maxsize, maxfps, chkScreenOff, chkStayAwake, chkNoAudio,
         chkTouch, chkReconn, reconnN, chkCloseExit, extra, optSave;
    HWND scrcpyEd, scrcpyBr, adbEd, adbBr, detect, pathSave, detectOut;
    HBRUSH brBg;                /* page background (dark)                 */
    HBRUSH brEdit;              /* edit background                        */
    HFONT font;                 /* 10.5pt Microsoft YaHei (manager)       */
    HFONT fontB;                /* semibold variant (title/tabs/labels)   */
} m;

/* Uniform 96-dpi control metrics: one height for inputs/checkboxes, one for
 * buttons, a fixed row pitch, and a shared label column. Everything on a
 * page aligns to this grid. */
#define MG_ROW_H_96   30
#define MG_BTN_H_96   32
#define MG_PITCH_96   42
#define MG_LBL_X_96   0
#define MG_LBL_W_96   170
#define MG_CTL_X_96   178

/* 10.5pt (五号) Microsoft YaHei: pixel size = 10.5 * dpi / 72 = 21*dpi/144 */
static HFONT mgr_font_make(bool bold, UINT dpi)
{
    return CreateFontW(-MulDiv(21, dpi, 144), 0, 0, 0,
                       bold ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
}

#define ISCHK(h) ((h) && SendMessageW((h), BM_GETCHECK, 0, 0) == BST_CHECKED)

static HWND mkctl(HWND parent, const wchar_t *cls, DWORD style,
                  const wchar_t *text, int x, int y, int w, int h,
                  int id, UINT dpi)
{
    /* push buttons become owner-drawn (dark theme); checkboxes stay native */
    if (wcscmp(cls, L"BUTTON") == 0 && !(style & BS_AUTOCHECKBOX)) {
        style |= BS_OWNERDRAW;
    }
    HWND ctl = CreateWindowExW(0, cls, text ? text : L"",
                               WS_CHILD | WS_VISIBLE | style,
                               MulDiv(x, dpi, 96), MulDiv(y, dpi, 96),
                               MulDiv(w, dpi, 96), MulDiv(h, dpi, 96),
                               parent, (HMENU) (INT_PTR) id, g_hInst, NULL);
    if (ctl) {
        SendMessageW(ctl, WM_SETFONT,
                     (WPARAM) (m.font ? m.font : g.fUi), TRUE);
    }
    return ctl;
}

/* grid helpers: a label row and a flat edit, both MG_ROW_H_96. Labels use
 * SS_CENTERIMAGE: without it a STATIC draws its text at the TOP of the rect
 * while edits/buttons center theirs — that is the classic "labels sit too
 * high" misalignment. */
static HWND mgr_label(HWND page, UINT dpi, const wchar_t *text,
                      int x, int y, int w)
{
    return mkctl(page, L"STATIC", SS_NOPREFIX | SS_CENTERIMAGE, text,
                 x, y, w, MG_ROW_H_96, 0, dpi);
}

static HWND mgr_edit(HWND page, UINT dpi, const wchar_t *text,
                     int x, int y, int w, DWORD extra, int id)
{
    return mkctl(page, L"EDIT",
                 ES_AUTOHSCROLL | WS_TABSTOP | extra,
                 text, x, y, w, MG_ROW_H_96, id, dpi);
}

static HWND mgr_check(HWND page, UINT dpi, const wchar_t *text,
                      int x, int y, int w, int id)
{
    return mkctl(page, L"BUTTON", BS_AUTOCHECKBOX | WS_TABSTOP,
                 text, x, y, w, MG_ROW_H_96, id, dpi);
}

static HWND mgr_button(HWND page, UINT dpi, const wchar_t *text,
                       int x, int y, int w, int id)
{
    return mkctl(page, L"BUTTON", WS_TABSTOP, text, x, y, w, MG_BTN_H_96,
                 id, dpi);
}

/* ---- dark "floating window" theme, matching the toolbar palette ---- */

/* DWM dark border + rounded corners; the window itself is frameless with a
 * self-drawn header (a dark native title bar keeps near-black caption text
 * on some builds — unusable, so we do not use WS_CAPTION at all). */
static void mgr_dark_titlebar(HWND hwnd)
{
    typedef HRESULT (WINAPI *PFN_DSWA)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) {
        return;
    }
    PFN_DSWA set = (PFN_DSWA) (void *) GetProcAddress(dwm,
                                                      "DwmSetWindowAttribute");
    if (set) {
        BOOL dark = TRUE;
        if (set(hwnd, 20, &dark, sizeof(dark)) != 0) {
            set(hwnd, 19, &dark, sizeof(dark)); /* 1809/1903 attribute id */
        }
        DWORD round = 2; /* DWMWCP_ROUND */
        set(hwnd, 33, &round, sizeof(round));
    }
    FreeLibrary(dwm);
}

/* owner-draw push button: rounded dark cell like the toolbar buttons */
static void draw_dark_button(DRAWITEMSTRUCT *di)
{
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    bool sel = (di->itemState & ODS_SELECTED) != 0;
    bool dis = (di->itemState & ODS_DISABLED) != 0;
    /* fill the whole item first: the BUTTON class background brush is a
     * light system color and would peek through the rounded corners */
    HBRUSH back = CreateSolidBrush(RGB(32, 32, 32));
    FillRect(dc, &rc, back);
    DeleteObject(back);
    COLORREF bg = dis ? RGB(38, 38, 38) : sel ? RGB(87, 87, 87)
                  : RGB(55, 55, 58);
    HBRUSH br = CreateSolidBrush(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_br = SelectObject(dc, br);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
    SelectObject(dc, old_br);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    DeleteObject(br);
    wchar_t txt[64];
    int n = GetWindowTextW(di->hwndItem, txt, 64);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, dis ? RGB(120, 120, 120) : RGB(240, 240, 240));
    HGDIOBJ old_f = SelectObject(dc, m.font ? m.font : g.fUi);
    DrawTextW(dc, txt, n, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_f);
}

/* page container: dark background + dark-color routing for all children */
static LRESULT CALLBACK mgr_page_proc(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        {
            HDC dc = (HDC) wp;
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH br = m.brBg ? m.brBg : GetStockBrush(BLACK_BRUSH);
            FillRect(dc, &rc, br);
        }
        return 1;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        {
            HDC dc = (HDC) wp;
            bool is_edit = (msg == WM_CTLCOLOREDIT)
                || (GetWindowLongPtrW((HWND) lp, GWL_STYLE) & ES_READONLY);
            SetBkColor(dc, is_edit ? RGB(43, 43, 43) : RGB(32, 32, 32));
            SetTextColor(dc, RGB(230, 230, 230));
            return (LRESULT) (is_edit ? m.brEdit : m.brBg);
        }
    case WM_CTLCOLORBTN:
        /* owner-draw buttons: dark background behind the rounded corners */
        return (LRESULT) (m.brBg ? m.brBg : GetStockBrush(BLACK_BRUSH));
    case WM_DRAWITEM:
        if (wp) { /* control id != 0 */
            draw_dark_button((DRAWITEMSTRUCT *) lp);
            return TRUE;
        }
        break;
    case WM_COMMAND:
    case WM_NOTIFY:
        /* buttons/edits/listview are children of the PAGE; their
         * notifications arrive here and must reach the frame's handler */
        if (m.frame) {
            SendMessageW(m.frame, msg, wp, lp);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static struct DevMeta *meta_find(const wchar_t *serial)
{
    int i;
    for (i = 0; i < g.devMeta.count; i++) {
        if (wcscmp(g.devMeta.v[i].serial, serial) == 0) {
            return &g.devMeta.v[i];
        }
    }
    return NULL;
}

/* friendlier text for the common adb states */
static const wchar_t *dev_state_text(const wchar_t *state)
{
    if (wcscmp(state, L"device") == 0) {
        return L"在线";
    }
    if (wcscmp(state, L"offline") == 0) {
        return L"离线";
    }
    if (wcscmp(state, L"unauthorized") == 0) {
        return L"待授权";
    }
    return state;
}

static const wchar_t *dev_conn_text(const wchar_t *serial)
{
    if (wcsncmp(serial, L"emulator-", 9) == 0) {
        return L"模拟器";
    }
    if (wcschr(serial, L':')) {
        return L"网络";
    }
    return L"USB";
}

static void mgr_list_reload(void)
{
    int i;
    if (!m.lv || !g.devList) {
        return;
    }
    SendMessageW(m.lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(m.lv);
    int selItem = -1;
    for (i = 0; i < g.devList->count; i++) {
        struct DevInfo *d = &g.devList->v[i];
        struct DevMeta *dm = meta_find(d->serial);
        wchar_t battery[16];
        wchar_t android[32];
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.iSubItem = 0;
        it.pszText = (LPWSTR) dev_state_text(d->state);
        it.lParam = (LPARAM) i;
        ListView_InsertItem(m.lv, &it);
        ListView_SetItemText(m.lv, i, 1, d->serial);
        ListView_SetItemText(m.lv, i, 2, dm ? dm->model : (LPWSTR) L"—");
        if (dm && dm->android[0]) {
            swprintf(android, 32, L"Android %s", dm->android);
            ListView_SetItemText(m.lv, i, 3, android);
        } else {
            ListView_SetItemText(m.lv, i, 3, (LPWSTR) L"—");
        }
        ListView_SetItemText(m.lv, i, 4, (LPWSTR) dev_conn_text(d->serial));
        if (dm && dm->battery >= 0) {
            swprintf(battery, 16, L"%d%%", dm->battery);
            ListView_SetItemText(m.lv, i, 5, battery);
        } else {
            ListView_SetItemText(m.lv, i, 5, (LPWSTR) L"—");
        }
        {
            int sx = ses_find(d->serial);
            ListView_SetItemText(m.lv, i, 6,
                sx < 0 ? (LPWSTR) L""
                : (sx == g_active ? (LPWSTR) L"●当前" : (LPWSTR) L"●"));
        }
        if (wcscmp(d->serial, g.serial) == 0) {
            selItem = i;
        }
    }
    if (selItem >= 0) {
        ListView_SetItemState(m.lv, selItem,
                              LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }
    SendMessageW(m.lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(m.lv, NULL, FALSE);
}

static void mgr_update_status(void)
{
    if (!m.status) {
        return;
    }
    wchar_t run[320];
    if (g.hProc) {
        swprintf(run, 320, L"运行中 (pid %lu)", g.pid);
    } else if (g.reconnPending) {
        swprintf(run, 320, L"等待设备上线重连 (%d/%d)",
                 g.reconnAttempts, g.cfg.reconnect_attempts);
    } else if (g.errLine[0]) {
        swprintf(run, 320, L"已退出: %s", g.errLine);
    } else if (g.launched) {
        wcscpy(run, L"已退出");
    } else {
        wcscpy(run, L"未由本工具启动");
    }
    wchar_t txt[560];
    {
        int i, used = 0;
        for (i = 0; i < SES_MAX; i++) {
            if (g_ses[i].used) {
                used++;
            }
        }
        swprintf(txt, 560, L"目标设备: %s   |   会话 %d/%d   |   scrcpy: %s",
                 g.serial[0] ? g.serial : L"（未选择）", used, SES_MAX, run);
    }
    SetWindowTextW(m.status, txt);
    wchar_t tail[1200];
    MultiByteToWideChar(CP_UTF8, 0, g.errBuf, -1, tail, 1199);
    tail[1199] = L'\0';
    if (m.errview) {
        SetWindowTextW(m.errview, tail);
    }
}

static void mgr_refresh(void)
{
    if (!m.frame) {
        return;
    }
    mgr_list_reload();
    mgr_update_status();
}

/* selected list row's serial (true on success) */
static bool mgr_selected_serial(wchar_t *out, size_t cch)
{
    LVITEMW vi;
    int it, di;
    if (!m.lv || !g.devList) {
        return false;
    }
    it = ListView_GetNextItem(m.lv, -1, LVNI_SELECTED);
    if (it < 0) {
        return false;
    }
    ZeroMemory(&vi, sizeof(vi));
    vi.mask = LVIF_PARAM;
    vi.iItem = it;
    if (!ListView_GetItem(m.lv, &vi)) {
        return false;
    }
    di = (int) vi.lParam;
    if (di < 0 || di >= g.devList->count) {
        return false;
    }
    wcsncpy(out, g.devList->v[di].serial, cch - 1);
    out[cch - 1] = L'\0';
    return true;
}

/* 双击列表项 = 连接该设备（已有会话则切换为活动会话） */
static void mgr_connect_selected(void)
{
    wchar_t serial[64];
    if (!mgr_selected_serial(serial, 64)) {
        return;
    }
    if (ISCHK(m.remember)) {
        wcsncpy(g.cfg.serial, serial, 127);
        g.cfg.serial[127] = L'\0';
    } else {
        g.cfg.serial[0] = L'\0';
    }
    ini_save();

    int idx = ses_find(serial);
    if (idx >= 0 && idx != g_active) {
        ses_become_active(idx); /* running: just switch the toolbar to it */
        swprintf(g.tipStatus, MAX_PATH + 32, L"已切换到 %s", serial);
        return;
    }
    g.reconnAttempts = 0;
    g.procGen++; /* invalidate any queued PROCEXIT from the old child */
    if (!launch_scrcpy_ex(serial, true)) {
        wcscpy(g.tipStatus, L"启动失败：会话数已达上限（4）或 scrcpy 启动错误");
    } else {
        swprintf(g.tipStatus, MAX_PATH + 32, L"已连接 %s", serial);
    }
    mgr_refresh();
}

/* normalize multiline edit content into a single command-line tail */
static void mgr_read_extra(void)
{
    wchar_t buf[600];
    if (!m.extra) {
        return;
    }
    GetWindowTextW(m.extra, buf, 600);
    wchar_t *p = buf;
    while (*p) {
        if (*p == L'\r' || *p == L'\n') {
            *p = L' ';
        }
        p++;
    }
    /* trim trailing spaces */
    p = buf + wcslen(buf);
    while (p > buf && p[-1] == L' ') {
        *--p = L'\0';
    }
    wcsncpy(g.cfg.scrcpy_args, buf, 511);
    g.cfg.scrcpy_args[511] = L'\0';
}

static void mgr_opt_save(void)
{
    wchar_t t[64];
    GetWindowTextW(m.bitrate, t, 64);
    g.cfg.bitrate = _wtoi(t);
    GetWindowTextW(m.maxsize, t, 64);
    g.cfg.max_size = _wtoi(t);
    GetWindowTextW(m.maxfps, t, 64);
    g.cfg.max_fps = _wtoi(t);
    g.cfg.turn_screen_off = ISCHK(m.chkScreenOff);
    g.cfg.stay_awake = ISCHK(m.chkStayAwake);
    g.cfg.no_audio = ISCHK(m.chkNoAudio);
    g.cfg.show_touches = ISCHK(m.chkTouch);
    g.cfg.auto_reconnect = ISCHK(m.chkReconn);
    g.cfg.close_on_exit = ISCHK(m.chkCloseExit);
    GetWindowTextW(m.reconnN, t, 64);
    g.cfg.reconnect_attempts = _wtoi(t);
    if (g.cfg.reconnect_attempts < 1) {
        g.cfg.reconnect_attempts = 1;
    }
    if (g.cfg.reconnect_attempts > 99) {
        g.cfg.reconnect_attempts = 99;
    }
    mgr_read_extra();
    ini_save();
    wcscpy(g.tipStatus, L"参数已保存，下次连接生效");
}

static void mgr_path_save(void)
{
    wchar_t buf[MAX_PATH];
    if (m.scrcpyEd) {
        GetWindowTextW(m.scrcpyEd, buf, MAX_PATH);
        buf[MAX_PATH - 1] = L'\0';
        wchar_t *p = buf + wcslen(buf);
        while (p > buf && p[-1] == L' ') {
            *--p = L'\0';
        }
        wcsncpy(g.cfg.scrcpy, buf, MAX_PATH - 1);
        g.cfg.scrcpy[MAX_PATH - 1] = L'\0';
    }
    if (m.adbEd) {
        GetWindowTextW(m.adbEd, buf, MAX_PATH);
        buf[MAX_PATH - 1] = L'\0';
        wchar_t *p = buf + wcslen(buf);
        while (p > buf && p[-1] == L' ') {
            *--p = L'\0';
        }
        wcsncpy(g.cfg.adb, buf, MAX_PATH - 1);
        g.cfg.adb[MAX_PATH - 1] = L'\0';
    }
    ini_save();

    if (g.cfg.scrcpy[0]) {
        wcsncpy(g.scrcpyPath, g.cfg.scrcpy, MAX_PATH - 1);
        g.scrcpyPath[MAX_PATH - 1] = L'\0';
    } else {
        find_tool(L"scrcpy.exe", NULL, g.scrcpyPath);
    }
    if (g.cfg.adb[0]) {
        wcsncpy(g.adbPath, g.cfg.adb, MAX_PATH - 1);
        g.adbPath[MAX_PATH - 1] = L'\0';
        g.adbOk = file_exists(g.adbPath);
    } else {
        wchar_t scrcpy_dir[MAX_PATH];
        scrcpy_dir[0] = L'\0';
        dir_of(g.scrcpyPath, scrcpy_dir, MAX_PATH);
        g.adbOk = find_tool(L"adb.exe", scrcpy_dir, g.adbPath);
    }

    /* restart the device tracker with the (possibly new) adb */
    track_stop();
    track_start();
    wcscpy(g.tipStatus, L"路径已保存并应用");
}

static void mgr_browse(HWND ctrl)
{
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m.frame;
    ofn.lpstrFilter = L"程序 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择程序";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn) && ctrl) {
        SetWindowTextW(ctrl, file);
    }
}

static void mgr_add_lv_columns(HWND lv)
{
    struct { const wchar_t *t; int w; } cols[] = {
        { L"状态",   56 },
        { L"序列号", 140 },
        { L"型号",   110 },
        { L"系统",   80 },
        { L"连接",   56 },
        { L"电量",   48 },
        { L"投屏",   56 },
    };
    int i;
    for (i = 0; i < 7; i++) {
        LVCOLUMNW c;
        ZeroMemory(&c, sizeof(c));
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (LPWSTR) cols[i].t;
        c.cx = MulDiv(cols[i].w, wnd_dpi(m.frame), 96);
        ListView_InsertColumn(lv, i, &c);
    }
}

static void mgr_create_dev_page(HWND page, UINT dpi)
{
    m.lv = mkctl(page, WC_LISTVIEWW,
                 LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS
                 | LVS_NOSORTHEADER | WS_TABSTOP | WS_BORDER,
                 NULL, 0, 0, 560, 220, MID_LV, dpi);
    ListView_SetExtendedListViewStyle(m.lv,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(m.lv, RGB(32, 32, 32));
    ListView_SetTextBkColor(m.lv, RGB(32, 32, 32));
    ListView_SetTextColor(m.lv, RGB(235, 235, 235));
    mgr_add_lv_columns(m.lv);

    m.refresh = mgr_button(page, dpi, L"刷新", 0, 232, 90, MID_REFRESH);
    m.connect = mgr_button(page, dpi, L"连接所选", 102, 232, 120,
                           MID_CONNECT);
    m.disconnect = mgr_button(page, dpi, L"断开", 234, 232, 90,
                              MID_DISCONNECT);
    /* same height as the buttons so the row has one baseline */
    m.remember = mkctl(page, L"BUTTON", BS_AUTOCHECKBOX | WS_TABSTOP,
                       L"记住此设备 (serial=)", 336, 232, 216,
                       MG_BTN_H_96, MID_REMEMBER, dpi);
    SendMessageW(m.remember, BM_SETCHECK,
                 g.cfg.serial[0] ? BST_CHECKED : BST_UNCHECKED, 0);

    /* wireless row: --tcpip toggle / manual adb connect / pairing */
    m.chkWireless = mkctl(page, L"BUTTON", BS_AUTOCHECKBOX | WS_TABSTOP,
                          L"无线(--tcpip)", 0, 276, 118,
                          MG_BTN_H_96, MID_CHK_WIRELESS, dpi);
    SendMessageW(m.chkWireless, BM_SETCHECK,
                 g.cfg.wireless ? BST_CHECKED : BST_UNCHECKED, 0);
    m.wifiEd = mkctl(page, L"EDIT", ES_AUTOHSCROLL | WS_TABSTOP,
                     L"", 120, 276, 260, MG_BTN_H_96, MID_WIFI_ED, dpi);
    m.wifiGo = mgr_button(page, dpi, L"连接", 398, 276, 80, MID_WIFI_GO);
    m.pairBtn = mgr_button(page, dpi, L"配对…", 486, 276, 74, MID_PAIR);

    m.status = mkctl(page, L"STATIC", SS_NOPREFIX | SS_CENTERIMAGE, L"",
                     0, 318, 560, 40, 0, dpi);
    m.errview = mkctl(page, L"EDIT",
                      ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY
                      | WS_VSCROLL | WS_TABSTOP,
                      L"", 0, 362, 560, 56, MID_LV + 100, dpi);
}

static void mgr_create_opt_page(HWND page, UINT dpi)
{
    wchar_t t[32];
    int y = 0;
    mgr_label(page, dpi, L"码率 Mbps（0=默认）", MG_LBL_X_96, y, MG_LBL_W_96);
    _itow(g.cfg.bitrate, t, 10);
    m.bitrate = mgr_edit(page, dpi, t, MG_CTL_X_96, y, 80,
                         ES_NUMBER, MID_BITRATE);

    y += MG_PITCH_96;
    mgr_label(page, dpi, L"最大尺寸（0=默认）", MG_LBL_X_96, y, MG_LBL_W_96);
    _itow(g.cfg.max_size, t, 10);
    m.maxsize = mgr_edit(page, dpi, t, MG_CTL_X_96, y, 80,
                         ES_NUMBER, MID_MAXSIZE);

    y += MG_PITCH_96;
    mgr_label(page, dpi, L"最大帧率（0=默认）", MG_LBL_X_96, y, MG_LBL_W_96);
    _itow(g.cfg.max_fps, t, 10);
    m.maxfps = mgr_edit(page, dpi, t, MG_CTL_X_96, y, 80,
                        ES_NUMBER, MID_MAXFPS);

    y += MG_PITCH_96 + 10;
    m.chkScreenOff = mgr_check(page, dpi, L"息屏镜像", 0, y, 130,
                               MID_CHK_SCREENOFF);
    m.chkStayAwake = mgr_check(page, dpi, L"保持唤醒", 140, y, 130,
                               MID_CHK_STAYAWAKE);
    m.chkNoAudio = mgr_check(page, dpi, L"禁用音频", 280, y, 130,
                             MID_CHK_NOAUDIO);
    m.chkTouch = mgr_check(page, dpi, L"显示触摸点", 420, y, 140,
                           MID_CHK_TOUCH);
    SendMessageW(m.chkScreenOff, BM_SETCHECK,
                 g.cfg.turn_screen_off ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(m.chkStayAwake, BM_SETCHECK,
                 g.cfg.stay_awake ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(m.chkNoAudio, BM_SETCHECK,
                 g.cfg.no_audio ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(m.chkTouch, BM_SETCHECK,
                 g.cfg.show_touches ? BST_CHECKED : BST_UNCHECKED, 0);

    y += MG_PITCH_96 + 4;
    m.chkReconn = mgr_check(page, dpi, L"断线自动重连", 0, y, 150,
                            MID_CHK_RECONN);
    SendMessageW(m.chkReconn, BM_SETCHECK,
                 g.cfg.auto_reconnect ? BST_CHECKED : BST_UNCHECKED, 0);
    mgr_label(page, dpi, L"次数", 160, y, 46);
    _itow(g.cfg.reconnect_attempts, t, 10);
    m.reconnN = mgr_edit(page, dpi, t, 212, y, 60, ES_NUMBER, MID_RECONN_N);
    m.chkCloseExit = mgr_check(page, dpi, L"工具栏退出时关闭 scrcpy",
                               284, y, 276, MID_CHK_CLOSEEXIT);
    SendMessageW(m.chkCloseExit, BM_SETCHECK,
                 g.cfg.close_on_exit ? BST_CHECKED : BST_UNCHECKED, 0);

    y += MG_PITCH_96 + 10;
    mgr_label(page, dpi, L"附加参数（追加到命令行末尾，可覆盖以上选项）:",
              0, y, 560);
    y += MG_ROW_H_96 + 6;
    m.extra = mkctl(page, L"EDIT",
                    ES_MULTILINE | ES_AUTOHSCROLL | WS_TABSTOP,
                    g.cfg.scrcpy_args, 0, y, 560, 56, MID_EXTRA, dpi);

    y += 56 + 14;
    m.optSave = mgr_button(page, dpi, L"保存", 0, y, 110, MID_OPT_SAVE);
    y += MG_BTN_H_96 + 8;
    mkctl(page, L"STATIC", SS_NOPREFIX | SS_CENTERIMAGE,
          L"改动保存后于下次连接生效。",
          0, y, 560, 24, 0, dpi);
}

static void mgr_create_path_page(HWND page, UINT dpi)
{
    /* every row here shares one 32px height at the same top, so the label,
     * edit and button boxes line up exactly (one common center line) */
    HWND lbl;
    int y = 0;
    lbl = mkctl(page, L"STATIC", SS_NOPREFIX | SS_CENTERIMAGE,
                L"scrcpy.exe", MG_LBL_X_96, y, 90, MG_BTN_H_96, 0, dpi);
    /* semibold: lowercase Latin reads lighter than CJK at the same size */
    SendMessageW(lbl, WM_SETFONT, (WPARAM) m.fontB, TRUE);
    m.scrcpyEd = mkctl(page, L"EDIT", ES_AUTOHSCROLL | WS_TABSTOP,
                       g.cfg.scrcpy[0] ? g.cfg.scrcpy : g.scrcpyPath,
                       96, y, 398, MG_BTN_H_96, MID_SCRCPY_ED, dpi);
    m.scrcpyBr = mgr_button(page, dpi, L"浏览", 504, y, 56,
                            MID_SCRCPY_BR);

    y += MG_PITCH_96;
    lbl = mkctl(page, L"STATIC", SS_NOPREFIX | SS_CENTERIMAGE,
                L"adb.exe", MG_LBL_X_96, y, 90, MG_BTN_H_96, 0, dpi);
    SendMessageW(lbl, WM_SETFONT, (WPARAM) m.fontB, TRUE);
    m.adbEd = mkctl(page, L"EDIT", ES_AUTOHSCROLL | WS_TABSTOP,
                    g.cfg.adb[0] ? g.cfg.adb : g.adbPath,
                    96, y, 398, MG_BTN_H_96, MID_ADB_ED, dpi);
    m.adbBr = mgr_button(page, dpi, L"浏览", 504, y, 56, MID_ADB_BR);

    y += MG_PITCH_96 + 10;
    m.detect = mgr_button(page, dpi, L"检测版本", 0, y, 110, MID_DETECT);
    m.detectOut = mkctl(page, L"STATIC", SS_NOPREFIX | SS_CENTERIMAGE, L"",
                        122, y, 438, MG_BTN_H_96, 0, dpi);

    y += MG_BTN_H_96 + 16;
    mkctl(page, L"STATIC", SS_NOPREFIX | SS_CENTERIMAGE,
          L"留空 = 自动查找（本工具目录 → scrcpy 目录 → PATH）。\n"
          L"保存后立即生效；更换 adb 会重启设备监测。",
          0, y, 560, 48, 0, dpi);

    y += 44 + 18;
    m.pathSave = mgr_button(page, dpi, L"保存并应用", 0, y, 130,
                            MID_PATH_SAVE);
}

static void mgr_show_page(int idx)
{
    int i;
    for (i = 0; i < MPAGE_COUNT; i++) {
        ShowWindow(m.page[i], i == idx ? SW_SHOW : SW_HIDE);
    }
}
/* ---- adb connect / adb pair / mdns discovery (worker) ---- */

struct WifiWork {
    int mode;                /* 0 = connect, 1 = pair, 2 = discover only  */
    wchar_t a[80];           /* address (connect: ip[:port], pair: ip:port) */
    wchar_t b[16];           /* pairing code                            */
};

struct WifiResult {
    int pair;                /* 1 = report into the pairing dialog      */
    wchar_t text[256];
    wchar_t addr[80];        /* discovered address (prefills the edit)  */
};

/* Parse "adb mdns services" output: lines look like
 *   <name> _adb-tls-connect._tcp 192.168.1.9:37123
 * Collect the ip:port tail of every connect service. */
static int mdns_parse(const char *buf, wchar_t addrs[][80], int max)
{
    char *dup = _strdup(buf ? buf : "");
    int n = 0;
    if (!dup) return 0;
    char *ctx = NULL;
    for (char *line = strtok_s(dup, "\r\n", &ctx); line && n < max;
            line = strtok_s(NULL, "\r\n", &ctx)) {
        char name[256], type[80], addr[256];
        if (sscanf(line, "%255s %79s %255s", name, type, addr) != 3
                || (strcmp(type, "_adb-tls-connect._tcp") != 0
                    && strcmp(type, "_adb-tls-connect._tcp.") != 0)) continue;
        size_t len = strlen(addr);
        char *colon = strrchr(addr, ':');
        if (len >= 80 || !colon || colon == addr || !colon[1]) continue;
        /* Only numeric network endpoints; never pass arbitrary output as args. */
        if (strspn(addr, "0123456789abcdefABCDEF:.[]") != len) continue;
        char *tail;
        long port = strtol(colon + 1, &tail, 10);
        if (*tail || port < 1 || port > 65535) continue;
        wchar_t value[80];
        if (!MultiByteToWideChar(CP_ACP, 0, addr, -1, value, 80)) continue;
        int i;
        for (i = 0; i < n && wcscmp(addrs[i], value); i++) {}
        if (i == n) wcscpy(addrs[n++], value);
    }
    free(dup);
    return n;
}

static void wifi_append(struct WifiResult *r, const wchar_t *text)
{
    wcsncat_s(r->text, 256, text, _TRUNCATE);
}

/* mDNS advertises endpoints, not pairing trust. adb connect checks trust. */
static void mdns_discover_and_connect(struct WifiResult *r)
{
    wchar_t cmd[MAX_PATH + 32];
    char buf[4096];
    DWORD ec = 1;
    wchar_t addrs[4][80];
    int n = 0;
    swprintf(cmd, MAX_PATH + 32, L"\"%s\" mdns services", g.adbPath);
    for (int attempt = 0; attempt < 5 && n == 0; attempt++) {
        if (!capture_sync(cmd, buf, sizeof(buf), 8000, &ec) || ec != 0) {
            wifi_append(r, L"\r\nmDNS 查询失败或超时，请手动填地址连接");
            return;
        }
        n = mdns_parse(buf, addrs, 4);
        if (n == 0 && attempt < 4) Sleep(2000);
    }
    if (n == 0) {
        wifi_append(r, L"\r\n未发现设备，请确认无线调试已开启，或手动填地址连接");
        return;
    }
    wcscpy(r->addr, addrs[0]);
    if (n > 1) {
        wifi_append(r, L"\r\n发现多个无线端点，未自动连接；已填入第一项，请核对手机连接地址后点连接");
        return;
    }
    wchar_t ccmd[MAX_PATH + 200];
    char output[512];
    swprintf(ccmd, MAX_PATH + 200, L"\"%s\" connect %s", g.adbPath, addrs[0]);
    int captured = capture_sync(ccmd, output, sizeof(output), 15000, &ec);
    /* Some adb versions report connection failures with exit status zero. */
    int connected = captured && ec == 0
        && (strncmp(output, "connected to ", 13) == 0
            || strncmp(output, "already connected to ", 21) == 0);
    wifi_append(r, connected ? L"\r\n已连接 " : L"\r\n自动连接失败，请核对配对状态及地址：");
    wifi_append(r, addrs[0]);
}

static unsigned __stdcall wifi_cmd_thread(void *arg)
{
    struct WifiWork *w = (struct WifiWork *) arg;
    struct WifiResult *r = calloc(1, sizeof(*r));
    wchar_t cmd[MAX_PATH + 200];
    char out[512];
    DWORD ec = 1;

    if (r) {
        r->pair = (w->mode == 1);
        if (w->mode == 1) {
            swprintf(cmd, MAX_PATH + 200, L"\"%s\" pair %s %s",
                     g.adbPath, w->a, w->b);
        } else if (w->mode == 2) {
            cmd[0] = L'\0';
        } else {
            swprintf(cmd, MAX_PATH + 200, L"\"%s\" connect %s",
                     g.adbPath, w->a);
        }
        if (cmd[0]
                && capture_sync(cmd, out, sizeof(out), 20000, &ec)) {
            char *nl = strchr(out, '\n');
            if (nl) {
                *nl = '\0';
            }
            char *e = out + strlen(out);
            while (e > out && (e[-1] == '\r' || e[-1] == ' ')) {
                *--e = '\0';
            }
            if (out[0]) {
                MultiByteToWideChar(CP_UTF8, 0, out, -1, r->text, 255);
            }
        }
        if (!r->text[0] && w->mode != 2) {
            wcscpy(r->text, w->mode == 1 ? L"配对指令已执行（无输出）"
                                         : L"连接指令已执行（无输出）");
        }
        /* successful pairing (or an empty address box) → auto-discover:
         * mDNS yields the current connect port, separate from pairing */
        if (w->mode == 2 || (w->mode == 1 && ec == 0)) {
            mdns_discover_and_connect(r);
        }
    }
    free(w);
    if (g.hwnd) {
        PostMessageW(g.hwnd, WM_APP_WIFIDONE, (WPARAM) r, 0);
    } else {
        free(r);
    }
    return 0;
}

static void wifi_spawn_cmd(int mode, const wchar_t *a, const wchar_t *b)
{
    struct WifiWork *w = calloc(1, sizeof(*w));
    if (!w) {
        return;
    }
    w->mode = mode;
    if (a) {
        wcsncpy(w->a, a, 79);
    }
    if (b) {
        wcsncpy(w->b, b, 15);
    }
    uintptr_t th = _beginthreadex(NULL, 0, wifi_cmd_thread, w, 0, NULL);
    if (th) {
        CloseHandle((HANDLE) th);
    } else {
        free(w);
    }
}

/* ---- wireless pairing mini-dialog (Android 11+) ---- */

static struct {
    HWND frame;
    HWND edAddr, edCode, go, out;
    RECT hdrRc, closeRc;
} p;

static void pair_paint(HWND hwnd, HDC dc)
{
    RECT rc;
    UINT dpi = wnd_dpi(hwnd);
    GetClientRect(hwnd, &rc);
    /* header */
    RECT hdr = { 0, 0, rc.right, MulDiv(42, dpi, 96) };
    p.hdrRc = hdr;
    p.closeRc.left = rc.right - MulDiv(46, dpi, 96);
    p.closeRc.top = MulDiv(7, dpi, 96);
    p.closeRc.right = rc.right - MulDiv(10, dpi, 96);
    p.closeRc.bottom = MulDiv(37, dpi, 96);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(235, 235, 235));
    HGDIOBJ old_f = SelectObject(dc, m.fontB ? m.fontB : g.fUi);
    RECT tr = hdr;
    tr.left += MulDiv(16, dpi, 96);
    DrawTextW(dc, L"无线配对（Android 11+）", -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    HBRUSH br = CreateSolidBrush(RGB(43, 43, 43));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, br);
    RoundRect(dc, p.closeRc.left, p.closeRc.top,
              p.closeRc.right, p.closeRc.bottom, 6, 6);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    DeleteObject(br);
    if (g.glyphsOk) {
        SelectObject(dc, g.fGlyph);
        DrawTextW(dc, L"\uE8BB", -1, &p.closeRc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        DrawTextW(dc, L"X", -1, &p.closeRc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, old_f);
}

static LRESULT CALLBACK pair_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        {
            HDC dc = (HDC) wp;
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH br = CreateSolidBrush(RGB(32, 32, 32));
            FillRect(dc, &rc, br);
            DeleteObject(br);
        }
        return 1;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            pair_paint(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
    case WM_NCHITTEST:
        {
            POINT pt;
            pt.x = GET_X_LPARAM(lp);
            pt.y = GET_Y_LPARAM(lp);
            ScreenToClient(hwnd, &pt);
            if (PtInRect(&p.closeRc, pt)) {
                return HTCLIENT;
            }
            if (PtInRect(&p.hdrRc, pt)) {
                return HTCAPTION;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        {
            POINT pt;
            pt.x = GET_X_LPARAM(lp);
            pt.y = GET_Y_LPARAM(lp);
            if (PtInRect(&p.closeRc, pt)) {
                DestroyWindow(hwnd);
                return 0;
            }
        }
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        {
            HDC dc = (HDC) wp;
            bool is_edit = (msg == WM_CTLCOLOREDIT)
                || (GetWindowLongPtrW((HWND) lp, GWL_STYLE) & ES_READONLY);
            SetBkColor(dc, is_edit ? RGB(43, 43, 43) : RGB(32, 32, 32));
            SetTextColor(dc, RGB(230, 230, 230));
            return (LRESULT) (is_edit ? m.brEdit : m.brBg);
        }
    case WM_CTLCOLORBTN:
        return (LRESULT) (m.brBg ? m.brBg : GetStockBrush(BLACK_BRUSH));
    case WM_DRAWITEM:
        if (wp) {
            draw_dark_button((DRAWITEMSTRUCT *) lp);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == MID_PAIR_GO) {
            wchar_t a[80], c[16];
            GetWindowTextW(p.edAddr, a, 80);
            GetWindowTextW(p.edCode, c, 16);
            /* trim spaces */
            wchar_t *e = a + wcslen(a);
            while (e > a && e[-1] == L' ') {
                *--e = L'\0';
            }
            e = c + wcslen(c);
            while (e > c && e[-1] == L' ') {
                *--e = L'\0';
            }
            if (!a[0] || !c[0]) {
                SetWindowTextW(p.out,
                               L"请输入 手机\"无线调试\"页面显示的 ip:端口 和配对码");
                return 0;
            }
            SetWindowTextW(p.out, L"配对中…");
            wifi_spawn_cmd(1, a, c);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        ZeroMemory(&p, sizeof(p));
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void pair_open(HWND owner)
{
    if (p.frame && IsWindow(p.frame)) {
        SetForegroundWindow(p.frame);
        return;
    }
    UINT dpi = wnd_dpi(owner);
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = pair_proc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"scrdock_pair_cls";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    int w96 = 420, h96 = 300;
    RECT or_;
    GetWindowRect(owner, &or_);
    int mx = or_.left + 40;
    int my = or_.top + 80;
    p.frame = CreateWindowExW(0, L"scrdock_pair_cls", L"scrdock 配对",
                              WS_POPUP,
                              mx, my,
                              MulDiv(w96, dpi, 96), MulDiv(h96, dpi, 96),
                              owner, NULL, g_hInst, NULL);
    if (!p.frame) {
        return;
    }
    mgr_dark_titlebar(p.frame);

    HWND f = p.frame;
    mkctl(f, L"STATIC", SS_NOPREFIX | SS_CENTERIMAGE,
          L"配对地址 (ip:端口)", 16, 56, 130, MG_ROW_H_96, 0, dpi);
    p.edAddr = mkctl(f, L"EDIT", ES_AUTOHSCROLL | WS_TABSTOP,
                     L"", 152, 56, 240, MG_ROW_H_96, MID_PAIR_ADDR, dpi);
    mkctl(f, L"STATIC", SS_NOPREFIX | SS_CENTERIMAGE,
          L"配对码", 16, 100, 130, MG_ROW_H_96, 0, dpi);
    p.edCode = mkctl(f, L"EDIT", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
                     L"", 152, 100, 100, MG_ROW_H_96, MID_PAIR_CODE, dpi);
    p.go = mkctl(f, L"BUTTON", WS_TABSTOP, L"开始配对",
                 152, 148, 110, MG_BTN_H_96, MID_PAIR_GO, dpi);
    p.out = mkctl(f, L"STATIC", SS_NOPREFIX | SS_CENTERIMAGE, L"",
                  16, 196, 388, 56, 0, dpi);
    mkctl(f, L"STATIC", SS_NOPREFIX | SS_CENTERIMAGE,
          L"手机: 设置 → 开发者选项 → 无线调试 → 用配对码配对设备",
          16, 262, 388, MG_ROW_H_96, 0, dpi);
    ShowWindow(p.frame, SW_SHOW);
}


/* self-drawn tab strip, below the frameless header */
static void mgr_strip_layout(void)
{
    UINT dpi = wnd_dpi(m.frame);
    int i;
    RECT cli;
    GetClientRect(m.frame, &cli);
    m.hdrRc.left = 0;
    m.hdrRc.top = 0;
    m.hdrRc.right = cli.right;
    m.hdrRc.bottom = MulDiv(42, dpi, 96);
    m.closeRc.left = cli.right - MulDiv(46, dpi, 96);
    m.closeRc.top = MulDiv(7, dpi, 96);
    m.closeRc.right = cli.right - MulDiv(10, dpi, 96);
    m.closeRc.bottom = MulDiv(37, dpi, 96);
    for (i = 0; i < MPAGE_COUNT; i++) {
        int x = 12 + i * MulDiv(138, dpi, 96);
        m.tabRc[i].left = MulDiv(x, dpi, 96);
        m.tabRc[i].top = MulDiv(52, dpi, 96);
        m.tabRc[i].right = m.tabRc[i].left + MulDiv(130, dpi, 96);
        m.tabRc[i].bottom = m.tabRc[i].top + MulDiv(34, dpi, 96);
    }
}

static void mgr_header_paint(HDC dc)
{
    UINT dpi = wnd_dpi(m.frame);
    /* title text, light on dark */
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(235, 235, 235));
    HGDIOBJ old_f = SelectObject(dc, m.fontB ? m.fontB : g.fUi);
    RECT tr = m.hdrRc;
    tr.left += MulDiv(16, dpi, 96);
    tr.right -= MulDiv(56, dpi, 96);
    DrawTextW(dc, L"scrdock 管理", -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* close cell */
    RECT rc = m.closeRc;
    HBRUSH br = CreateSolidBrush(RGB(43, 43, 43));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_br = SelectObject(dc, br);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
    SelectObject(dc, old_br);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    DeleteObject(br);
    SetTextColor(dc, RGB(240, 240, 240));
    if (g.glyphsOk) {
        SelectObject(dc, g.fGlyph);
        DrawTextW(dc, L"\uE8BB", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        DrawTextW(dc, L"X", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, old_f);
}

static void mgr_strip_paint(HDC dc)
{
    static const wchar_t *names[MPAGE_COUNT] = {
        L"设备", L"参数", L"路径"
    };
    int i;
    for (i = 0; i < MPAGE_COUNT; i++) {
        RECT rc = m.tabRc[i];
        bool active = (i == m.tabSel);
        HBRUSH br = CreateSolidBrush(active ? RGB(44, 96, 160)
                                            : RGB(43, 43, 43));
        HPEN pen = CreatePen(PS_SOLID, 1, active ? RGB(70, 120, 180)
                                                 : RGB(70, 70, 70));
        HGDIOBJ old_pen = SelectObject(dc, pen);
        HGDIOBJ old_br = SelectObject(dc, br);
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
        SelectObject(dc, old_br);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        DeleteObject(br);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(240, 240, 240));
        HGDIOBJ old_f = SelectObject(dc, m.font ? m.font : g.fUi);
        DrawTextW(dc, names[i], -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old_f);
    }
}

static void mgr_switch_page(int idx)
{
    if (idx < 0 || idx >= MPAGE_COUNT || !m.frame) {
        return;
    }
    m.tabSel = idx;
    mgr_show_page(idx);
    InvalidateRect(m.frame, NULL, FALSE);
}

static LRESULT CALLBACK mgr_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
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
            HBRUSH br = CreateSolidBrush(RGB(32, 32, 32));
            FillRect(dc, &rc, br);
            DeleteObject(br);
            mgr_header_paint(dc);
            mgr_strip_paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
    case WM_NCHITTEST:
        {
            /* frameless: the header acts as the caption (dragging), except
             * the close cell which stays a normal click target */
            POINT pt;
            pt.x = GET_X_LPARAM(lp);
            pt.y = GET_Y_LPARAM(lp);
            ScreenToClient(hwnd, &pt);
            if (PtInRect(&m.closeRc, pt)) {
                return HTCLIENT;
            }
            if (PtInRect(&m.hdrRc, pt)) {
                return HTCAPTION;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        {
            POINT pt;
            pt.x = GET_X_LPARAM(lp);
            pt.y = GET_Y_LPARAM(lp);
            if (PtInRect(&m.closeRc, pt)) {
                DestroyWindow(hwnd);
                return 0;
            }
            int i;
            for (i = 0; i < MPAGE_COUNT; i++) {
                if (PtInRect(&m.tabRc[i], pt)) {
                    mgr_switch_page(i);
                    break;
                }
            }
        }
        return 0;
    case WM_NOTIFY:
        {
            NMHDR *nm = (NMHDR *) lp;
            if (nm->idFrom == MID_LV && nm->code == NM_DBLCLK) {
                mgr_connect_selected();
                return 0;
            }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case MID_REFRESH:
            meta_spawn();
            break;
        case MID_CONNECT:
            mgr_connect_selected();
            break;
        case MID_DISCONNECT:
            {
                wchar_t serial[64];
                int idx = -1;
                if (mgr_selected_serial(serial, 64)) {
                    idx = ses_find(serial);
                }
                if (idx < 0 || idx == g_active) {
                    /* active session (or none selected): graceful close,
                     * then follow the next remaining session if any */
                    g.reconnPending = false;
                    KillTimer(NULL, TIMER_RECONN);
                    close_scrcpy_and_wait();
                    int a = g_active;
                    g_active = -1;
                    if (a >= 0) {
                        g_ses[a].hProc = NULL; /* closed above */
                        g_ses[a].hErr = NULL;  /* drained by teardown path */
                        ses_free(a);
                    }
                    g.launched = false;
                    int next = ses_first_used();
                    if (next >= 0) {
                        ses_become_active(next);
                    }
                } else {
                    /* background session: close its window; the exit is
                     * cleaned up by WM_APP_SESEXIT */
                    struct Session *s = &g_ses[idx];
                    if (s->target && IsWindow(s->target)) {
                        PostMessageW(s->target, WM_CLOSE, 0, 0);
                    } else if (s->hProc) {
                        TerminateProcess(s->hProc, 1);
                    }
                }
                wcscpy(g.tipStatus, L"已断开会话");
                mgr_refresh();
                InvalidateRect(g.hwnd, NULL, FALSE);
            }
            break;
        case MID_CHK_WIRELESS:
            g.cfg.wireless = ISCHK(m.chkWireless);
            ini_save();
            wcscpy(g.tipStatus, g.cfg.wireless
                   ? L"无线已开启：下次连接经 USB 切到 WiFi (--tcpip)"
                   : L"无线已关闭");
            break;
        case MID_WIFI_GO:
            {
                wchar_t a[80];
                GetWindowTextW(m.wifiEd, a, 80);
                wchar_t *e = a + wcslen(a);
                while (e > a && e[-1] == L' ') {
                    *--e = L'\0';
                }
                if (a[0]) {
                    wchar_t addr[84];
                    if (!wcschr(a, L':')) {
                        swprintf(addr, 84, L"%s:5555", a);
                    } else {
                        wcsncpy(addr, a, 83);
                        addr[83] = L'\0';
                    }
                    SetWindowTextW(m.status, L"连接中…");
                    wifi_spawn_cmd(0, addr, NULL);
                } else {
                    /* empty box: discover paired devices over mDNS */
                    SetWindowTextW(m.status, L"正在发现无线设备…");
                    wifi_spawn_cmd(2, NULL, NULL);
                }
            }
            break;
        case MID_PAIR:
            if (m.frame) {
                pair_open(m.frame);
            }
            break;
        case MID_OPT_SAVE:
            mgr_opt_save();
            mgr_update_status();
            break;
        case MID_PATH_SAVE:
            mgr_path_save();
            mgr_refresh();
            break;
        case MID_DETECT:
            {
                uintptr_t th = _beginthreadex(NULL, 0, detect_thread,
                                              NULL, 0, NULL);
                if (th) {
                    CloseHandle((HANDLE) th);
                }
            }
            break;
        case MID_SCRCPY_BR:
            mgr_browse(m.scrcpyEd);
            break;
        case MID_ADB_BR:
            mgr_browse(m.adbEd);
            break;
        default:
            break;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (m.brBg) {
            DeleteObject(m.brBg);
        }
        if (m.brEdit) {
            DeleteObject(m.brEdit);
        }
        if (m.font) {
            DeleteObject(m.font);
        }
        if (m.fontB) {
            DeleteObject(m.fontB);
        }
        ZeroMemory(&m, sizeof(m));
        /* boot-failure state: the toolbar never docked and there is no
         * scrcpy session left — closing the manager should not leave a
         * hidden zombie holding the single-instance mutex */
        if (g.hwnd && !g.everDocked && !g.hProc) {
            DestroyWindow(g.hwnd);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void mgr_open(void)
{
    if (m.frame && IsWindow(m.frame)) {
        ShowWindow(m.frame, SW_RESTORE);
        SetForegroundWindow(m.frame);
        mgr_refresh();
        return;
    }

    UINT dpi = wnd_dpi(g.hwnd);
    m.tabSel = MPAGE_DEV;
    m.brBg = CreateSolidBrush(RGB(32, 32, 32));
    m.brEdit = CreateSolidBrush(RGB(43, 43, 43));
    m.font = mgr_font_make(false, dpi);   /* 10.5pt */
    m.fontB = mgr_font_make(true, dpi);

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = mgr_wndproc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"scrdock_mgr_cls";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc); /* already-registered is fine */

    WNDCLASSW wpCls;
    ZeroMemory(&wpCls, sizeof(wpCls));
    wpCls.lpfnWndProc = mgr_page_proc;
    wpCls.hInstance = g_hInst;
    wpCls.lpszClassName = L"scrdock_mgrpage_cls";
    RegisterClassW(&wpCls); /* no background brush: page paints itself */

    /* frameless floating window: self-drawn header (title + close), no
     * system caption — a dark native title bar keeps near-black caption
     * text on some Windows builds, so it is not used at all. Opened next
     * to the toolbar so it lands on the same monitor (same DPI). */
    DWORD style = WS_POPUP | WS_MINIMIZEBOX;
    int mx = CW_USEDEFAULT;
    int my = CW_USEDEFAULT;
    int mw = MulDiv(MGR_W_96, dpi, 96);
    int mh = MulDiv(MGR_H_96, dpi, 96);
    {
        RECT tr;
        if (g.hwnd && GetWindowRect(g.hwnd, &tr)) {
            HMONITOR mon = MonitorFromWindow(g.hwnd,
                                             MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                mx = tr.right + MulDiv(16, dpi, 96);
                my = tr.top;
                if (mx + mw > mi.rcWork.right) {
                    mx = mi.rcWork.right - mw;
                }
                if (my + mh > mi.rcWork.bottom) {
                    my = mi.rcWork.bottom - mh;
                }
                if (mx < mi.rcWork.left) {
                    mx = mi.rcWork.left;
                }
                if (my < mi.rcWork.top) {
                    my = mi.rcWork.top;
                }
            }
        }
    }
    m.frame = CreateWindowExW(0, L"scrdock_mgr_cls", L"scrdock 管理", style,
                              mx, my, mw, mh,
                              g.hwnd, NULL, g_hInst, NULL);
    if (!m.frame) {
        return;
    }
    mgr_dark_titlebar(m.frame);
    mgr_strip_layout();

    int i;
    for (i = 0; i < MPAGE_COUNT; i++) {
        m.page[i] = CreateWindowExW(0, L"scrdock_mgrpage_cls", L"",
                                    WS_CHILD, 0, 0, 0, 0,
                                    m.frame, NULL, g_hInst, NULL);
    }
    mgr_create_dev_page(m.page[MPAGE_DEV], dpi);
    mgr_create_opt_page(m.page[MPAGE_OPT], dpi);
    mgr_create_path_page(m.page[MPAGE_PATH], dpi);

    /* pages fill the area below the header and tab strip */
    int px = MulDiv(12, dpi, 96);
    int py = MulDiv(94, dpi, 96);
    int pw = MulDiv(MGR_W_96 - 24, dpi, 96);
    int ph = MulDiv(MGR_H_96 - 106, dpi, 96);
    for (i = 0; i < MPAGE_COUNT; i++) {
        SetWindowPos(m.page[i], NULL, px, py, pw, ph,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    mgr_show_page(MPAGE_DEV);

    ShowWindow(m.frame, SW_SHOW);
    mgr_refresh();
}

static void mgr_toggle(void)
{
    if (m.frame && IsWindow(m.frame)) {
        DestroyWindow(m.frame);
    } else {
        mgr_open();
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
    if (id == ID_MGR) {
        mgr_toggle();
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
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    AppendMenuW(menu, MF_STRING, IDM_REDOCK, L"重新吸附 (&D)");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出 (&X)");
    /* Tray-icon trick: popup menus dismiss correctly only for a foreground
     * window; briefly foreground, then release via the WM_NULL nudge. */
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

/* ------------------------------------------------------------------ */
/* device-list application + reconnect                                 */

static void reconn_arm(void)
{
    SetTimer(g.hwnd, TIMER_RECONN, 1500, NULL);
}

/* Promote a background session to be the toolbar-controlled one: swap the
 * fields between g.* and the previous active slot, re-target the hooks and
 * start hunting for this session's SDL window. */
static void ses_become_active(int idx)
{
    struct Session *s;
    if (idx < 0 || idx >= SES_MAX || !g_ses[idx].used) {
        return;
    }
    /* demote the current active session back into its slot */
    if (g_active >= 0 && g_active != idx) {
        struct Session *cur = &g_ses[g_active];
        cur->hProc = g.hProc;
        cur->hErr = g.hErr;
        cur->target = g.target;
        cur->pid = g.pid;
        cur->gen = g.procGen;
        cur->launchTick = g.launchTick;
        cur->reconnAttempts = g.reconnAttempts;
        cur->reconnPending = g.reconnPending;
        wcsncpy(cur->serial, g.serial, 63);
        cur->serial[63] = L'\0';
    }
    s = &g_ses[idx];
    g.hProc = s->hProc;
    g.hErr = s->hErr;
    g.pid = s->pid;
    g.procGen = s->gen;
    g.launchTick = s->launchTick;
    g.reconnAttempts = s->reconnAttempts;
    g.reconnPending = s->reconnPending;
    wcsncpy(g.serial, s->serial, 127);
    g.serial[127] = L'\0';
    g.launched = true;
    s->hProc = NULL;
    s->hErr = NULL;
    g_active = idx;
    g.target = NULL;
    KillTimer(g.hwnd, TIMER_RECONN);
    hooks_install(g.pid);
    hunting_start();
    if (g.reconnPending && !g.hProc && dev_online(g.serial)) {
        reconn_arm(); /* resume a pending reconnect of this session */
    }
    mgr_refresh();
    InvalidateRect(g.hwnd, NULL, FALSE);
}

/* UI-thread application of a fresh device snapshot (takes ownership). */
static void devlist_apply(struct DevList *dl)
{
    if (!dl) {
        return;
    }
    struct DevList *old = g.devList;
    bool changed = !old || old->count != dl->count
        || memcmp(old->v, dl->v,
                  sizeof(struct DevInfo) * (size_t) dl->count) != 0;
    g.devList = dl;
    free(old);

    /* Selection: a pinned serial (serial= in ini) always wins. In auto
     * mode, follow the first online device whenever ours disappears —
     * exactly one device attached means "just works" like before. */
    if (!g.cfg.serial[0] && !dev_online(g.serial)) {
        int i;
        g.serial[0] = L'\0';
        for (i = 0; i < g.devList->count; i++) {
            if (wcscmp(g.devList->v[i].state, L"device") == 0) {
                wcsncpy(g.serial, g.devList->v[i].serial, 127);
                g.serial[127] = L'\0';
                break;
            }
        }
    }
    g.devNone = (dev_count_online() == 0);

    if (g.bootLaunch) {
        /* first snapshot: the serial is resolved, so launch with -s */
        g.bootLaunch = false;
        if (!launch_scrcpy()) {
            swprintf(g.tipStatus, MAX_PATH + 32,
                     L"启动 scrcpy 失败 (code %lu)", GetLastError());
            mgr_open();
        }
    }

    /* the device came back while we were waiting to reconnect */
    if (g.reconnPending && !g.hProc && dev_online(g.serial)) {
        reconn_arm();
    }
    if (changed || g.devMeta.count == 0) {
        meta_spawn();
    }
    mgr_refresh();
    InvalidateRect(g.hwnd, NULL, FALSE);
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
    KillTimer(hwnd, TIMER_RECONN);
    if (g.hTrack) {
        track_stop(); /* a poller stuck inside adb is cut by process exit */
    }
    if (g.hErr) {
        CloseHandle(g.hErr);
        g.hErr = NULL;
    }
    if (g.fGlyph) {
        DeleteObject(g.fGlyph);
        g.fGlyph = NULL;
    }
    if (g.fUi) {
        DeleteObject(g.fUi);
        g.fUi = NULL;
    }
    if (g.cfg.close_on_exit) {
        close_scrcpy_and_wait();
    }
    {
        /* background sessions do not survive the toolbar either */
        int i;
        for (i = 0; i < SES_MAX; i++) {
            if (g_ses[i].used && g_ses[i].hProc) {
                TerminateProcess(g_ses[i].hProc, 1);
            }
            ses_free(i);
        }
    }
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
            if (g.hErr) {
                err_drain(false); /* keep the pipe from filling up */
            }
            if (!g.hProc && !g.launched) {
                /* attach mode without a handle: poll for process exit
                 * (a launched child is handled via WM_APP_PROCEXIT) */
                if (g.pid && !find_scrcpy_pid()) {
                    DestroyWindow(hwnd);
                    return 0;
                }
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
        case TIMER_RECONN:
            KillTimer(hwnd, TIMER_RECONN);
            if (!g.reconnPending || g.hProc) {
                break;
            }
            if (!dev_online(g.serial)) {
                /* keep trying to bring a WiFi device back (cheap, idempotent);
                 * DEVLIST re-arms immediately once it appears */
                if (wcschr(g.serial, L':')) {
                    adb_spawn(L" connect %s", g.serial);
                } else {
                    const wchar_t *wf = wifi_serial_in_list(g.serial);
                    if (wf) {
                        wcsncpy(g.serial, wf, 127);
                        g.serial[127] = L'\0';
                    } else {
                        const wchar_t *ip = wifi_ip_of(g.serial);
                        if (ip) {
                            adb_spawn(L" connect %s:5555", ip);
                        }
                    }
                }
                if (g.reconnAttempts < g.cfg.reconnect_attempts) {
                    reconn_arm(); /* retry loop until budget is spent */
                }
                break;
            }
            if (g.reconnAttempts >= g.cfg.reconnect_attempts) {
                g.reconnPending = false;
                wcscpy(g.tipStatus, L"重连次数已用完，请打开管理窗口手动连接");
                mgr_refresh();
                break;
            }
            g.reconnAttempts++;
            if (launch_scrcpy()) {
                swprintf(g.tipStatus, MAX_PATH + 32, L"已重连 (%d/%d)",
                         g.reconnAttempts, g.cfg.reconnect_attempts);
            } else {
                swprintf(g.tipStatus, MAX_PATH + 32,
                         L"重连失败: 无法启动 scrcpy (code %lu)",
                         GetLastError());
            }
            mgr_refresh();
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        default:
            break;
        }
        return 0;

    case WM_APP_DEVLIST:
        devlist_apply((struct DevList *) wp);
        return 0;

    case WM_APP_DEVMETA:
        {
            struct DevMetaBatch *mb = (struct DevMetaBatch *) wp;
            if (mb) {
                if (mb->scrW > 0 && mb->scrH > 0) {
                    g.scrW = mb->scrW;
                    g.scrH = mb->scrH;
                }
                int i;
                for (i = 0; i < mb->count; i++) {
                    if (mb->v[i].ip[0]) {
                        wifi_map_set(mb->v[i].serial, mb->v[i].ip);
                    }
                }
                g.devMeta = *mb;
                free(mb);
            }
            g.metaBusy = false;
            if (g.metaPending) {
                g.metaPending = false;
                meta_spawn();
            }
            mgr_refresh();
        }
        return 0;

    case WM_APP_DETECTED:
        {
            wchar_t *txt = (wchar_t *) wp;
            if (txt) {
                if (m.detectOut && IsWindow(m.detectOut)) {
                    SetWindowTextW(m.detectOut, txt);
                }
                free(txt);
            }
        }
        return 0;

    case WM_APP_WIFIDONE:
        {
            struct WifiResult *r = (struct WifiResult *) wp;
            if (r) {
                if (r->addr[0] && m.wifiEd && IsWindow(m.wifiEd)) {
                    SetWindowTextW(m.wifiEd, r->addr);
                }
                if (r->pair && p.out && IsWindow(p.out)) {
                    SetWindowTextW(p.out, r->text);
                } else if (m.status && IsWindow(m.status)) {
                    SetWindowTextW(m.status, r->text);
                }
                wcsncpy(g.tipStatus, r->text, MAX_PATH + 31);
                g.tipStatus[MAX_PATH + 31] = L'\0';
                free(r);
            }
        }
        return 0;

    case WM_APP_PROCEXIT:
        {
            if (g.closing || (unsigned) lp != g.procGen) {
                return 0; /* stale message from a previous child */
            }
            DWORD code = (DWORD) wp;
            err_drain(true);
            if (code != 0) {
                err_last_line(g.errLine, 256);
            }
            if (!g.launched) {
                /* attached (foreign) instance: follow it out, as before */
                DestroyWindow(hwnd);
                return 0;
            }
            g.target = NULL;
            /* scrcpy exit codes: 0 = closed, 1 = startup failure,
             * 2 = device disconnected while running */
            if (code == 2 && g.cfg.auto_reconnect
                    && g.reconnAttempts < g.cfg.reconnect_attempts) {
                if (GetTickCount64() - g.launchTick > 10000) {
                    g.reconnAttempts = 0; /* stable session: fresh budget */
                }
                g.reconnPending = true;
                /* wireless: after a --tcpip switch or a dropped WiFi link
                 * the device lives under an "ip:port" serial instead */
                if (!dev_online(g.serial)) {
                    const wchar_t *wf = wifi_serial_in_list(g.serial);
                    if (wf) {
                        wcsncpy(g.serial, wf, 127);
                        g.serial[127] = L'\0';
                    } else {
                        const wchar_t *base = wcschr(g.serial, L':')
                            ? g.serial : wifi_ip_of(g.serial);
                        if (base) {
                            wchar_t addr[80];
                            if (wcschr(base, L':')) {
                                wcsncpy(addr, base, 79);
                                addr[79] = L'\0';
                            } else {
                                swprintf(addr, 80, L"%s:5555", base);
                            }
                            adb_spawn(L" connect %s", addr);
                        }
                    }
                }
                swprintf(g.tipStatus, MAX_PATH + 32, L"设备断开，重连中 (%d/%d)",
                         g.reconnAttempts + 1, g.cfg.reconnect_attempts);
                if (dev_online(g.serial)) {
                    reconn_arm();
                }
                mgr_refresh();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (code == 0) {
                /* user closed the active scrcpy window: retire the session
                 * and follow another one, or exit with the last one */
                int a = g_active;
                g_active = -1;
                if (a >= 0) {
                    g_ses[a].hProc = NULL; /* closed by run_loop already */
                    g_ses[a].hErr = NULL;  /* drained above */
                    ses_free(a);
                }
                g.launched = false;
                {
                    int next = ses_first_used();
                    if (next >= 0) {
                        ses_become_active(next);
                        return 0;
                    }
                }
                DestroyWindow(hwnd);
                return 0;
            }
            /* failed (and not reconnecting): stay alive, surface the error */
            swprintf(g.tipStatus, MAX_PATH + 32, L"scrcpy 已退出 (code %lu)",
                     code);
            {
                int a = g_active;
                g_active = -1;
                if (a >= 0) {
                    g_ses[a].hProc = NULL;
                    g_ses[a].hErr = NULL;
                    ses_free(a);
                }
                g.launched = false;
                {
                    int next = ses_first_used();
                    if (next >= 0) {
                        ses_become_active(next);
                    }
                }
            }
            if (!g.everDocked) {
                mgr_open(); /* e.g. boot failure: the manager shows why */
            }
            mgr_refresh();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_APP_SESEXIT:
        {
            int idx = (int) wp;
            if (idx >= 0 && idx < SES_MAX && g_ses[idx].used) {
                swprintf(g.tipStatus, MAX_PATH + 32, L"会话已结束: %s",
                         g_ses[idx].serial);
                ses_free(idx);
                mgr_refresh();
            }
        }
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
        /* wait on the ACTIVE child (g.hProc) plus every background session */
        HANDLE hs[1 + SES_MAX];
        int map[1 + SES_MAX];
        int n = 0, i;
        if (g.hProc) {
            hs[n] = g.hProc;
            map[n] = -1;
            n++;
        }
        for (i = 0; i < SES_MAX; i++) {
            if (g_ses[i].used && g_ses[i].hProc) {
                hs[n] = g_ses[i].hProc;
                map[n] = i;
                n++;
            }
        }
        DWORD r = MsgWaitForMultipleObjectsEx(n, hs, INFINITE, QS_ALLINPUT,
                                              MWMO_INPUTAVAILABLE);
        if (n && r >= WAIT_OBJECT_0 && r < (DWORD) (WAIT_OBJECT_0 + n)) {
            int idx = map[r - WAIT_OBJECT_0];
            DWORD code = 0;
            GetExitCodeProcess(hs[r - WAIT_OBJECT_0], &code);
            if (idx < 0) {
                /* active child: the UI thread decides stay/reconnect/exit;
                 * procGen tags the message against a relaunched child */
                if (g.hwnd) {
                    PostMessageW(g.hwnd, WM_APP_PROCEXIT, (WPARAM) code,
                                 (LPARAM) g.procGen);
                }
                CloseHandle(g.hProc);
                g.hProc = NULL;
            } else {
                struct Session *s = &g_ses[idx];
                if (g.hwnd) {
                    PostMessageW(g.hwnd, WM_APP_SESEXIT, (WPARAM) idx, 0);
                }
                CloseHandle(s->hProc);
                s->hProc = NULL;
            }
            continue;
        }
        if (r == WAIT_OBJECT_0 + (DWORD) n) {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    return (int) msg.wParam;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
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

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    SetLastError(0);
    CreateMutexW(NULL, TRUE, L"Local\\scrdock_single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    get_exe_dir(g.exeDir);
    ini_load();
    wifi_map_load();

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    g.hNul = CreateFileW(L"NUL", GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                         OPEN_EXISTING, 0, NULL);

    g.hover = g.pressed = g.flashBtn = -1;
    g.tipShownFor = -1;

    bool have_scrcpy;
    if (g.cfg.scrcpy[0] && file_exists(g.cfg.scrcpy)) {
        wcsncpy(g.scrcpyPath, g.cfg.scrcpy, MAX_PATH - 1);
        g.scrcpyPath[MAX_PATH - 1] = L'\0';
        have_scrcpy = true;
    } else {
        have_scrcpy = find_tool(L"scrcpy.exe", NULL, g.scrcpyPath);
    }

    /* resolve adb BEFORE launching: the boot launch wants -s <serial>,
     * which is only known after the first track-devices snapshot */
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

    g.pid = find_scrcpy_pid();
    if (g.pid) {
        /* Attach to the running instance; keep it alive on toolbar close. */
        g.hProc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE
                              | PROCESS_QUERY_LIMITED_INFORMATION,
                              FALSE, g.pid);
        g.launched = false;
    } else if (!have_scrcpy) {
            MessageBoxW(NULL,
                        L"未找到 scrcpy.exe：\n"
                        L"请把 scrdock.exe 放到 scrcpy 所在目录，\n"
                        L"或确保 scrcpy 在 PATH 中。",
                        L"scrdock", MB_ICONERROR);
            return 1;
    } else if (g.adbOk) {
        /* adb known: wait for the first device snapshot, then launch with
         * -s <serial> (see devlist_apply) */
        g.bootLaunch = true;
    } else if (!launch_scrcpy()) {
            MessageBoxW(NULL, L"启动 scrcpy 失败。", L"scrdock", MB_ICONERROR);
            return 1;
    }

    register_and_create();
    if (!g.hwnd) {
        return 1;
    }

    SetTimer(g.hwnd, TIMER_SYNC, 500, NULL);

    if (g.pid) {
        /* attach mode: hook and hunt the existing window (a launched child
         * installs its own hooks inside launch_scrcpy) */
        hooks_install(g.pid);
        HWND t = find_scrcpy_window(g.pid);
        if (t) {
            adopt_target(t);
        } else {
            hunting_start();
        }
    }

    if (g.adbOk) {
        track_start();
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
