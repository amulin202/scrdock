/* Real Win32 windows/message loop. Each case runs in a bounded child process. */
#include "../scrdock.c"

static const wchar_t *test_mode;
static int test_result = -1;
static HWND original_manager;

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
    }
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
}

static int run_case(const wchar_t *mode)
{
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
    if (argc == 2) return run_case(argv[1]);
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return 1;
    const wchar_t *cases[] = {L"empty", L"pending", L"lost", L"docked",
                             L"activate-existing", L"activate-new"};
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
