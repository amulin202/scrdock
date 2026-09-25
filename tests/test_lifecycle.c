/* Real Win32 windows/message loop. Each case runs in a bounded child process. */
#include "../scrdock.c"

static const wchar_t *test_mode;
static int test_result = -1;
static HWND original_manager;
static ULONGLONG click_deadline;
static wchar_t expected_click_result[128];

static VOID CALLBACK check_wireless_click(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    (void) msg; (void) time;
    bool success = wcscmp(g.tipStatus, expected_click_result) == 0;
    if (!success && GetTickCount64() < click_deadline) return;
    test_result = success ? 0 : 11;
    KillTimer(hwnd, id);
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

static int run_wireless_click(const wchar_t *mode)
{
    g_hInst = GetModuleHandleW(NULL);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    register_and_create();
    g.adbOk = true;
    GetModuleFileNameW(NULL, g.adbPath, MAX_PATH);
    g.devList = calloc(1, sizeof(*g.devList));
    if (!g.devList) return 12;
    g.devList->count = 1;
    wcscpy_s(g.devList->v[0].serial, 64, L"192.0.2.10:5555");
    wcscpy_s(g.devList->v[0].state, 16, L"device");
    mgr_open();
    wchar_t field[80];
    GetWindowTextW(m.wifiPort, field, 80);
    if (wcscmp(field, L"5555") != 0) return 14;
    bool connecting = wcscmp(mode, L"wireless-click") != 0;
    if (connecting) {
        struct WifiResult *r = calloc(1, sizeof(*r));
        if (!r) return 15;
        wcscpy_s(r->addr, 80, L"192.0.2.10:51234");
        wndproc(g.hwnd, WM_APP_WIFIDONE, (WPARAM) r, 0);
        GetWindowTextW(m.wifiEd, field, 80);
        if (wcscmp(field, L"192.0.2.10") != 0) return 16;
        GetWindowTextW(m.wifiPort, field, 80);
        if (wcscmp(field, L"51234") != 0) return 17;
        SetWindowTextW(m.wifiPort, L"0");
        SendMessageW(m.wifiGo, BM_CLICK, 0, 0);
        GetWindowTextW(m.status, field, 80);
        if (!wcsstr(field, L"1–65535")) return 19;
        const wchar_t *port = wcscmp(mode, L"wireless-custom") == 0 ? L"6000" : L"5555";
        SetWindowTextW(m.wifiPort, port);
        swprintf(expected_click_result, 128, L"connected to 192.0.2.10:%s", port);
    } else {
        wcscpy_s(expected_click_result, 128, L"已断开无线连接: 192.0.2.10:5555");
    }
    ListView_SetItemState(m.lv, 0, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    click_deadline = GetTickCount64() + 2500;
    SetTimer(g.hwnd, 901, 50, check_wireless_click);
    /* Exercise BUTTON -> page WM_COMMAND -> manager -> worker -> result. */
    SendMessageW(connecting ? m.wifiGo : m.wifiDisconnect, BM_CLICK, 0, 0);
    run_loop();
    free(g.devList);
    return test_result;
}

static VOID CALLBACK check_window_state(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    (void) msg; (void) time;
    KillTimer(hwnd, id);
    if (wcscmp(test_mode, L"docked") == 0) {
        test_result = !g.closing && IsWindow(g.hwnd) && !m.frame ? 0 : 6;
    } else {
        test_result = !g.closing && IsWindow(m.frame) && IsWindowVisible(m.frame) ? 0 : 7;
        if (wcscmp(test_mode, L"activate-existing") == 0 && m.frame != original_manager) {
            test_result = 8;
        }
        if (wcscmp(test_mode, L"activate-new") == 0
                && (!IsWindow(m.wifiDisconnect)
                    || GetDlgCtrlID(m.wifiDisconnect) != MID_WIFI_DISCONNECT)) {
            test_result = 10;
        }
    }
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
}

static int run_case(const wchar_t *mode)
{
    if (wcsncmp(mode, L"wireless-", 9) == 0) return run_wireless_click(mode);
    test_mode = mode;
    g_hInst = GetModuleHandleW(NULL);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    register_and_create();
    if (!g.hwnd) return 2;
    WNDCLASSW wc = {0};
    wc.hInstance = g_hInst;
    wc.lpfnWndProc = mgr_wndproc;
    wc.lpszClassName = L"scrdock_mgr_lifecycle_test";
    if (!RegisterClassW(&wc)) return 3;
    m.frame = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP,
                              0, 0, 100, 100, g.hwnd, NULL, g_hInst, NULL);
    if (!m.frame) return 4;
    original_manager = m.frame;
    if (wcscmp(mode, L"pending") == 0) {
        /* A live handle does not imply that a usable mirror window exists. */
        g.hProc = CreateEventW(NULL, TRUE, FALSE, NULL);
    }
    if (wcscmp(mode, L"lost") == 0) g.everDocked = true;
    bool keep_running = wcscmp(mode, L"docked") == 0 || wcsncmp(mode, L"activate-", 9) == 0;
    HWND mirror = NULL;
    if (keep_running) {
        mirror = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
                                  0, 0, 100, 100, NULL, NULL, g_hInst, NULL);
        if (!mirror) return 9;
        g.target = mirror;
        g.everDocked = true;
        SetTimer(g.hwnd, 900, 100, check_window_state);
    }
    if (wcsncmp(mode, L"activate-", 9) == 0) {
        if (wcscmp(mode, L"activate-new") == 0) DestroyWindow(m.frame);
        activate_instance_window(g.hwnd);
    } else {
        PostMessageW(m.frame, WM_CLOSE, 0, 0);
    }
    int result = run_loop();
    if (g.hProc) CloseHandle(g.hProc);
    if (mirror) DestroyWindow(mirror);
    if (!g.closing || IsWindow(g.hwnd)) return 5;
    if (keep_running && test_result != 0) return test_result;
    return result;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 3 && wcscmp(argv[1], L"connect") == 0) {
        if (wcscmp(argv[2], L"192.0.2.10:5555") != 0
                && wcscmp(argv[2], L"192.0.2.10:6000") != 0) return 18;
        wprintf(L"connected to %s\n", argv[2]);
        return 0;
    }
    /* Stand in for adb in the click test; validate the exact endpoint. */
    if (argc == 3 && wcscmp(argv[1], L"disconnect") == 0) {
        return wcscmp(argv[2], L"192.0.2.10:5555") == 0 ? 0 : 13;
    }
    if (argc == 2) return run_case(argv[1]);
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return 1;
    const wchar_t *cases[] = {L"empty", L"pending", L"lost", L"docked",
                             L"activate-existing", L"activate-new", L"wireless-click",
                             L"wireless-default", L"wireless-custom"};
    int failures = 0;
    for (int i = 0; i < (int) (sizeof(cases) / sizeof(cases[0])); i++) {
        wchar_t cmd[MAX_PATH + 64];
        swprintf(cmd, MAX_PATH + 64, L"\"%s\" %s", exe, cases[i]);
        STARTUPINFOW si = {0};
        PROCESS_INFORMATION pi = {0};
        si.cb = sizeof(si);
        if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                            NULL, NULL, &si, &pi)) return 1;
        DWORD status = WaitForSingleObject(pi.hProcess, 5000), code = 1;
        if (status == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        else {
            TerminateProcess(pi.hProcess, 100);
            WaitForSingleObject(pi.hProcess, 2000);
        }
        wprintf(L"%s window-lifecycle %s (wait=%lu, exit=%lu)\n",
                status == WAIT_OBJECT_0 && code == 0 ? L"PASS" : L"FAIL",
                cases[i], status, code);
        if (status != WAIT_OBJECT_0 || code != 0) failures++;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return failures ? 1 : 0;
}
