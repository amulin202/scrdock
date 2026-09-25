/* Exercise the production state machine; substitute OS process/UI boundaries. */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static bool timers[16], fail_launch;
static int launches, posted;
static wchar_t last_command[2048];

static UINT_PTR WINAPI fake_set_timer(HWND h, UINT_PTR id, UINT ms, TIMERPROC cb)
{
    (void) h; (void) ms; (void) cb;
    if (id < 16) timers[id] = true;
    return id;
}
static BOOL WINAPI fake_kill_timer(HWND h, UINT_PTR id)
{
    (void) h;
    if (id < 16) timers[id] = false;
    return TRUE;
}
static BOOL WINAPI fake_create_process(LPCWSTR app, LPWSTR cmd,
    LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
    DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si,
    LPPROCESS_INFORMATION pi)
{
    (void) app; (void) pa; (void) ta; (void) inherit;
    (void) flags; (void) env; (void) dir; (void) si;
    launches++;
    wcscpy_s(last_command, 2048, cmd);
    if (fail_launch) { SetLastError(ERROR_FILE_NOT_FOUND); return FALSE; }
    pi->hProcess = CreateEventW(NULL, TRUE, TRUE, NULL);
    pi->hThread = CreateEventW(NULL, TRUE, TRUE, NULL);
    pi->dwProcessId = 1234;
    return pi->hProcess && pi->hThread;
}
static BOOL WINAPI fake_post(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void) h; (void) msg; (void) wp; (void) lp;
    posted++;
    return TRUE;
}
static BOOL WINAPI fake_invalidate(HWND h, const RECT *r, BOOL erase)
{ (void) h; (void) r; (void) erase; return TRUE; }
static BOOL WINAPI fake_is_window(HWND h) { return h != NULL; }
static BOOL WINAPI fake_show(HWND h, int cmd) { (void) h; (void) cmd; return TRUE; }
static BOOL WINAPI fake_foreground(HWND h) { (void) h; return TRUE; }
static HWINEVENTHOOK WINAPI fake_hook(DWORD first, DWORD last, HMODULE module,
    WINEVENTPROC cb, DWORD pid, DWORD tid, DWORD flags)
{
    (void) first; (void) last; (void) module; (void) cb;
    (void) pid; (void) tid; (void) flags;
    return NULL;
}

#define SetTimer fake_set_timer
#define KillTimer fake_kill_timer
#define CreateProcessW fake_create_process
#define PostMessageW fake_post
#define InvalidateRect fake_invalidate
#define IsWindow fake_is_window
#define ShowWindow fake_show
#define SetForegroundWindow fake_foreground
#define SetWinEventHook fake_hook
#include "../scrdock.c"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); \
} } while (0)

static void reset_state(void)
{
    if (g.hProc) CloseHandle(g.hProc);
    if (g.hErr) CloseHandle(g.hErr);
    free(g.devList);
    for (int i = 0; i < SES_MAX; i++) ses_free(i);
    ZeroMemory(&g, sizeof(g));
    ZeroMemory(&m, sizeof(m));
    ZeroMemory(g_wifiMap, sizeof(g_wifiMap));
    ZeroMemory(timers, sizeof(timers));
    g_active = -1;
    g.cfg.auto_reconnect = TRUE;
    g.cfg.reconnect_attempts = 2;
    wcscpy_s(g.scrcpyPath, MAX_PATH, L"scrcpy.exe");
    fail_launch = false;
    launches = posted = 0;
    last_command[0] = L'\0';
}

static void session(int idx, const wchar_t *serial, bool active)
{
    struct Session *s = &g_ses[idx];
    s->used = true;
    s->pid = (DWORD) (100 + idx);
    s->launchTick = GetTickCount64();
    wcscpy_s(s->serial, 64, serial);
    if (active) {
        g_active = idx;
        g.launched = true;
        g.pid = s->pid;
        g.hProc = CreateEventW(NULL, TRUE, TRUE, NULL);
        wcscpy_s(g.serial, 128, serial);
    }
}

static void online(const wchar_t *serial)
{
    if (!g.devList) g.devList = calloc(1, sizeof(*g.devList));
    CHECK(g.devList != NULL);
    g.devList->count = 1;
    wcscpy_s(g.devList->v[0].serial, 64, serial);
    wcscpy_s(g.devList->v[0].state, 16, L"device");
}

static void exit_background(int idx, DWORD code)
{
    if (g_ses[idx].hProc) CloseHandle(g_ses[idx].hProc);
    g_ses[idx].hProc = NULL;
    wndproc(NULL, WM_APP_SESEXIT, (WPARAM) idx, (LPARAM) code);
}

static void test_attachment(void)
{
    reset_state();
    g.attached = true;
    g.adbOk = true;
    g.cfg.control = 1; /* even a forced shortcut preference is unverified */
    wcscpy_s(g.serial, 128, L"A");
    CHECK(!use_shortcut_controls());
    CHECK(adb_work_new() == NULL);
    m.frame = (HWND) (INT_PTR) 1;
    for (int id = ID_HOME; id <= ID_VOLDN; id++) button_fire(id);
    CHECK(launches == 0 && posted == 0);
    CHECK(wcsstr(g.tipStatus, L"设备未确认") != NULL);
    m.frame = NULL;
    g.adbOk = false;
    g.serial[0] = L'\0';
    struct DevList *dl = calloc(1, sizeof(*dl));
    CHECK(dl != NULL);
    dl->count = 1;
    wcscpy_s(dl->v[0].serial, 64, L"A");
    wcscpy_s(dl->v[0].state, 16, L"device");
    devlist_apply(dl);
    CHECK(g.serial[0] == L'\0');
    CHECK(launch_scrcpy_ex(L"B", true));
    CHECK(!g.attached && wcscmp(g.serial, L"B") == 0);
    CHECK(use_shortcut_controls());
    puts("PASS attachment: block unverified controls, require explicit device launch");
}

static void test_disconnect(void)
{
    reset_state();
    session(0, L"A", true);
    HANDLE original = g.hProc;
    CHECK(!ses_disconnect_selected(L"B"));
    CHECK(g.hProc == original && g_active == 0 && g_ses[0].used);
    CHECK(WaitForSingleObject(original, 0) == WAIT_OBJECT_0);
    CHECK(ses_disconnect_selected(NULL));
    CHECK(!g.hProc && g_active == -1 && !g_ses[0].used);

    session(0, L"A", true);
    session(1, L"B", false);
    g_ses[1].reconnPending = true;
    CHECK(ses_disconnect_selected(L"B"));
    CHECK(g_active == 0 && g.hProc && !g_ses[1].used);
    session(1, L"B", false);
    g_ses[1].hProc = CreateEventW(NULL, TRUE, TRUE, NULL);
    g_ses[1].target = (HWND) (INT_PTR) 1;
    CHECK(ses_disconnect_selected(L"B"));
    CHECK(g_ses[1].stopRequested && posted == 1);
    exit_background(1, 2);
    CHECK(!g_ses[1].used && g_active == 0);
    puts("PASS disconnect: unknown selection preserves active session; explicit stop cancels retries");
}

static void test_background_reconnect(void)
{
    reset_state();
    session(0, L"A", true);
    session(1, L"B", false);
    HANDLE active = g.hProc;
    exit_background(1, 2);
    CHECK(g_ses[1].used && g_ses[1].reconnPending && timers[TIMER_BG_RECONN]);
    online(L"B");
    for (int attempt = 1; attempt <= 2; attempt++) {
        wndproc(NULL, WM_TIMER, TIMER_BG_RECONN, 0);
        CHECK(g_ses[1].hProc && !g_ses[1].reconnPending);
        CHECK(g_ses[1].reconnAttempts == attempt);
        CHECK(g_active == 0 && g.hProc == active && wcscmp(g.serial, L"A") == 0);
        CHECK(wcsstr(last_command, L" -s B") != NULL);
        exit_background(1, 2);
    }
    CHECK(!g_ses[1].used && launches == 2);
    ses_reconnect_background();
    CHECK(!timers[TIMER_BG_RECONN]);
    puts("PASS background reconnect: exit code reaches handler, bounded retries preserve active device");
}

static void test_exit_policy(void)
{
    for (DWORD code = 0; code <= 1; code++) {
        reset_state();
        session(1, L"B", false);
        exit_background(1, code);
        CHECK(!g_ses[1].used && !timers[TIMER_BG_RECONN]);
    }
    reset_state();
    session(1, L"B", false);
    g.cfg.auto_reconnect = FALSE;
    exit_background(1, 2);
    CHECK(!g_ses[1].used);
    reset_state();
    session(1, L"B", false);
    g_ses[1].launchTick = GetTickCount64() - 11000;
    g_ses[1].reconnAttempts = 2;
    exit_background(1, 2);
    CHECK(g_ses[1].reconnPending && g_ses[1].reconnAttempts == 0);
    puts("PASS exit policy: normal close, startup failure, disabled retries and stable-session budget");
}

static void test_failed_retries(void)
{
    for (int online_device = 0; online_device < 2; online_device++) {
        reset_state();
        session(0, L"A", true);
        session(1, L"B", false);
        exit_background(1, 2);
        fail_launch = true;
        if (online_device) online(L"B");
        for (int i = 0; i < 3; i++) ses_reconnect_background();
        CHECK(g_ses[1].used && !g_ses[1].hProc && !g_ses[1].reconnPending);
        CHECK(g_ses[1].reconnAttempts == 2 && !timers[TIMER_BG_RECONN]);
        CHECK(launches == (online_device ? 2 : 0));
        CHECK(g_active == 0 && g.hProc);
    }
    puts("PASS failed retries: offline devices and process creation failures stop at configured limit");
}

static void test_switch_pending(void)
{
    reset_state();
    session(0, L"A", true);
    CloseHandle(g.hProc);
    g.hProc = NULL;
    g.reconnPending = true;
    g.reconnAttempts = 1;
    session(1, L"B", false);
    g_ses[1].hProc = CreateEventW(NULL, TRUE, TRUE, NULL);
    ses_become_active(1);
    CHECK(g_active == 1 && g_ses[0].reconnPending && timers[TIMER_BG_RECONN]);
    ses_become_active(0);
    CHECK(g_active == 0 && !g.hProc && g.reconnPending && g.reconnAttempts == 1);
    CHECK(timers[TIMER_RECONN] && launches == 0);
    online(L"A");
    wndproc(NULL, WM_TIMER, TIMER_RECONN, 0);
    CHECK(g.hProc && !g.reconnPending && g.reconnAttempts == 2);
    CHECK(g_ses[1].hProc && launches == 1);
    puts("PASS session switch: pending retry and remaining budget survive promotion/demotion");
}

static void test_wifi_reconnect(void)
{
    reset_state();
    session(0, L"A", true);
    session(1, L"USB-B", false);
    wcscpy_s(g_wifiMap[0].serial, 64, L"USB-B");
    wcscpy_s(g_wifiMap[0].ip, 24, L"192.0.2.10");
    online(L"192.0.2.10:5555");
    exit_background(1, 2);
    ses_reconnect_background();
    CHECK(g_ses[1].hProc && g_ses[1].reconnAttempts == 1);
    CHECK(wcscmp(g_ses[1].serial, L"192.0.2.10:5555") == 0);
    CHECK(wcsstr(last_command, L" -s 192.0.2.10:5555") != NULL);
    CHECK(g_active == 0 && wcscmp(g.serial, L"A") == 0);
    puts("PASS wireless reconnect: remap serial in the existing background slot");
}

static void test_launch_preserves_sessions(void)
{
    reset_state();
    session(0, L"A", true);
    session(1, L"B", false);
    HANDLE active = g.hProc;
    fail_launch = true;
    ses_become_active(1);
    CHECK(g_active == 0 && g.hProc == active && g_ses[0].hProc == NULL);
    CHECK(wcscmp(g.serial, L"A") == 0 && g_ses[1].used && launches == 1);

    fail_launch = false;
    CloseHandle(g.hProc);
    g.hProc = NULL;
    g.reconnPending = true;
    g.reconnAttempts = 1;
    CHECK(launch_scrcpy_ex(L"B", true));
    CHECK(g_active == 1 && g.hProc && !g.reconnPending && g.reconnAttempts == 0);
    CHECK(g_ses[0].reconnPending && g_ses[0].reconnAttempts == 1);
    CHECK(timers[TIMER_BG_RECONN]);
    puts("PASS launch transitions: failure preserves active handles; new session retains old pending retry");
}

int main(void)
{
    test_attachment();
    test_disconnect();
    test_background_reconnect();
    test_exit_policy();
    test_failed_retries();
    test_switch_pending();
    test_wifi_reconnect();
    test_launch_preserves_sessions();
    reset_state();
    puts("PASS all session regression tests");
    return 0;
}
