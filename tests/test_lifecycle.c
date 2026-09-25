/* Real Win32 windows/message loop. Each case runs in a bounded child process. */
#include "../scrdock.c"

static const wchar_t *test_mode;
static int test_result = -1;
static HWND original_manager;
static ULONGLONG click_deadline;
static wchar_t expected_click_result[128];
static unsigned list_writes, list_deletes, list_inserts;

static LRESULT CALLBACK count_list_updates(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR id, DWORD_PTR data)
{
    (void) id; (void) data;
    if (msg == LVM_DELETEALLITEMS || msg == LVM_DELETEITEM) list_deletes++;
    if (msg == LVM_INSERTITEMW) list_inserts++;
    if (msg == LVM_SETITEMTEXTW || msg == LVM_SETITEMW
            || msg == LVM_SETITEMSTATE || msg == WM_SETREDRAW) list_writes++;
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static int run_list_refresh(void)
{
    g_hInst = GetModuleHandleW(NULL);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    register_and_create();
    g.devList = calloc(1, sizeof(*g.devList));
    if (!g.devList) return 20;
    g.devList->count = DEV_MAX;
    for (int i = 0; i < DEV_MAX; i++) {
        swprintf(g.devList->v[i].serial, 64, L"device-%d", i);
        wcscpy_s(g.devList->v[i].state, 16, L"device");
    }
    wcscpy_s(g.serial, 128, L"device-0");
    mgr_open();
    SetWindowPos(m.lv, NULL, 0, 0, 400, 100, SWP_NOMOVE | SWP_NOZORDER);
    ListView_SetItemState(m.lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(m.lv, 6, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(m.lv, 6, FALSE);
    ListView_Scroll(m.lv, 80, 0);
    int top = ListView_GetTopIndex(m.lv), scroll = GetScrollPos(m.lv, SB_HORZ);
    SetWindowTextW(m.wifiEd, L"192.0.2.10");
    SetWindowTextW(m.wifiPort, L"6000");
    SetFocus(m.wifiEd);
    if (!SetWindowSubclass(m.lv, count_list_updates, 1, 0)) return 21;
    for (int i = 0; i < 50; i++) mgr_refresh();
    wchar_t selected[64], text[80];
    if (list_writes || list_deletes || list_inserts) return 22;
    if (!mgr_selected_serial(selected, 64) || wcscmp(selected, L"device-6")) return 23;
    if (ListView_GetTopIndex(m.lv) != top || GetScrollPos(m.lv, SB_HORZ) != scroll) return 24;
    if (GetFocus() != m.wifiEd) return 25;
    GetWindowTextW(m.wifiEd, text, 80);
    if (wcscmp(text, L"192.0.2.10")) return 26;
    GetWindowTextW(m.wifiPort, text, 80);
    if (wcscmp(text, L"6000")) return 27;

    /* Metadata/state changes must update cells without replacing rows. */
    wcscpy_s(g.devList->v[6].state, 16, L"offline");
    g.devMeta.count = 1;
    wcscpy_s(g.devMeta.v[0].serial, 64, L"device-6");
    wcscpy_s(g.devMeta.v[0].model, 64, L"updated model");
    g.devMeta.v[0].battery = -1;
    mgr_refresh();
    if (list_deletes || list_inserts || !mgr_selected_serial(selected, 64)
            || wcscmp(selected, L"device-6")) return 28;
    ListView_GetItemText(m.lv, 6, 2, text, 80);
    if (wcscmp(text, L"updated model")) return 29;

    /* ADB can reorder its snapshot; user selection follows identity, not index. */
    struct DevList *reordered = malloc(sizeof(*reordered));
    if (!reordered) return 30;
    *reordered = *g.devList;
    for (int i = 0; i < DEV_MAX; i++) reordered->v[i] = g.devList->v[DEV_MAX - i - 1];
    devlist_apply(reordered);
    if (list_deletes || list_inserts || !mgr_selected_serial(selected, 64)
            || wcscmp(selected, L"device-6")) return 31;
    if (ListView_GetTopIndex(m.lv) != top || GetScrollPos(m.lv, SB_HORZ) != scroll) return 32;

    /* Removing a different row above the viewport preserves selection and anchor. */
    wchar_t anchor[64], after_anchor[64];
    ListView_GetItemText(m.lv, ListView_GetTopIndex(m.lv), 1, anchor, 64);
    g.devList->count--; /* reversed snapshot ends with device-0 */
    mgr_refresh();
    ListView_GetItemText(m.lv, ListView_GetTopIndex(m.lv), 1, after_anchor, 64);
    if (!mgr_selected_serial(selected, 64) || wcscmp(selected, L"device-6")
            || wcscmp(anchor, after_anchor) || GetScrollPos(m.lv, SB_HORZ) != scroll) return 35;

    /* Removing the selected device must not silently select another device. */
    for (int i = 1; i < g.devList->count - 1; i++) g.devList->v[i] = g.devList->v[i + 1];
    g.devList->count--;
    mgr_refresh();
    if (mgr_selected_serial(selected, 64) || list_deletes != 2 || list_inserts) return 33;
    /* New devices are appended while existing rows keep their identity/order. */
    wcscpy_s(g.devList->v[g.devList->count].serial, 64, L"new-device");
    wcscpy_s(g.devList->v[g.devList->count].state, 16, L"device");
    g.devList->count++;
    mgr_refresh();
    if (list_deletes != 2 || list_inserts != 1 || mgr_selected_serial(selected, 64)) return 34;
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
    int result = run_loop();
    free(g.devList);
    return result;
}

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
    if (wcscmp(mode, L"list-refresh") == 0) return run_list_refresh();
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
                             L"wireless-default", L"wireless-custom", L"list-refresh"};
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
