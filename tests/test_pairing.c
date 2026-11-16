#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>

static bool simulate_late_scan;
static ULONGLONG test_clock(void)
{
    static ULONGLONG offset;
    if (simulate_late_scan) offset += 65000;
    return GetTickCount64() + offset;
}
#define GetTickCount64 test_clock
#include "../scrdock.c"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL pairing line %d: %s\n", __LINE__, #expr); exit(1); \
} } while (0)

static int last_event = -1;
static LRESULT CALLBACK event_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_APP_QREVENT) {
        struct QrEvent *event = (struct QrEvent *) lp;
        last_event = event->code;
        free(event);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void render_qr(int side)
{
    CHECK(qr_encode_credentials("studio-scrdock-test", "123456"));
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = side;
    info.bmiHeader.biHeight = -side;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    CHECK(bitmap && pixels);
    HGDIOBJ old = SelectObject(dc, bitmap);
    RECT box = {0, 0, side, side};
    draw_qrcode_box(dc, &box, p.qrData, p.qrSize);
    GdiFlush();
    BITMAPFILEHEADER file = {0};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + (DWORD)(side * side * 4);
    char path[100];
    snprintf(path, sizeof(path), "build/tests/qr-%d.bmp", side);
    FILE *out = fopen(path, "wb");
    CHECK(out);
    CHECK(fwrite(&file, sizeof(file), 1, out) == 1);
    CHECK(fwrite(&info.bmiHeader, sizeof(BITMAPINFOHEADER), 1, out) == 1);
    CHECK(fwrite(pixels, (size_t)side * side * 4, 1, out) == 1);
    fclose(out);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

static void check_pairing_controls(bool qr_page)
{
    HWND qr[] = {p.qrStatus, p.qrRefresh, p.qrTip};
    HWND manual[] = {p.lblAddr, p.edAddr, p.lblCode, p.edCode,
                     p.go, p.out, p.manualTip};
    for (int i = 0; i < (int)(sizeof(qr) / sizeof(qr[0])); i++) {
        CHECK(IsWindow(qr[i]));
        CHECK((IsWindowVisible(qr[i]) != FALSE) == qr_page);
    }
    for (int i = 0; i < (int)(sizeof(manual) / sizeof(manual[0])); i++) {
        CHECK(IsWindow(manual[i]));
        CHECK((IsWindowVisible(manual[i]) != FALSE) == !qr_page);
    }
}

static void click_pairing_tab(int page)
{
    RECT tab = p.tabRc[page];
    SendMessageW(p.frame, WM_LBUTTONDOWN, MK_LBUTTON,
                 MAKELPARAM((tab.left + tab.right) / 2, (tab.top + tab.bottom) / 2));
    UpdateWindow(p.frame);
    CHECK(p.tabSel == page);
    check_pairing_controls(page == PAIR_PAGE_QR);
}

static COLORREF pairing_pixel(POINT point)
{
    HDC dc = GetDC(p.frame);
    CHECK(dc);
    GdiFlush();
    COLORREF pixel = GetPixel(dc, point.x, point.y);
    ReleaseDC(p.frame, dc);
    CHECK(pixel != CLR_INVALID);
    return pixel;
}

static void send_pairing_event(unsigned generation, int code, const wchar_t *text)
{
    struct QrEvent *event = calloc(1, sizeof(*event));
    CHECK(event);
    event->gen = generation;
    event->code = code;
    wcscpy_s(event->text, 160, text);
    SendMessageW(p.frame, WM_APP_QREVENT, 0, (LPARAM)event);
}

static void test_pairing_window(void)
{
    g_hInst = GetModuleHandleW(NULL);
    g.fUi = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    m.brBg = CreateSolidBrush(RGB(32, 32, 32));
    m.brEdit = CreateSolidBrush(RGB(43, 43, 43));
    CHECK(m.brBg && m.brEdit);
    HWND owner = CreateWindowExW(0, L"STATIC", L"pairing owner", WS_POPUP,
                                 80, 80, 500, 500, NULL, NULL, g_hInst, NULL);
    CHECK(owner);

    const int stale_success_events[] = {QREV_PAIRED, QREV_CONNECTED};
    for (int i = 0; i < (int)(sizeof(stale_success_events) / sizeof(stale_success_events[0])); i++) {
        /* Existing credentials keep this UI test independent of ADB and devices. */
        CHECK(qr_encode_credentials("studio-scrdock-test", "123456"));
        p.qrGen = ++g_qrGeneration;
        pair_open(owner);
        HWND dialog = p.frame;
        CHECK(IsWindow(dialog));
        CHECK(!p.hQrThread && !p.qrStop);
        CHECK(SetWindowPos(dialog, HWND_TOPMOST, 120, 140, 0, 0,
                           SWP_NOSIZE | SWP_NOACTIVATE));
        CHECK(RedrawWindow(dialog, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
        check_pairing_controls(true);

        /* Sample the gap between manual controls, where a stale QR would remain. */
        RECT code, button;
        CHECK(GetWindowRect(p.edCode, &code));
        CHECK(GetWindowRect(p.go, &button));
        MapWindowPoints(HWND_DESKTOP, dialog, (POINT *)&code, 2);
        MapWindowPoints(HWND_DESKTOP, dialog, (POINT *)&button, 2);
        POINT point = {(p.qrBox.left + p.qrBox.right) / 2,
                       (code.bottom + button.top) / 2};
        CHECK(PtInRect(&p.qrBox, point));
        CHECK(code.bottom < point.y && point.y < button.top);
        COLORREF qr_pixel = pairing_pixel(point);
        CHECK(qr_pixel != RGB(32, 32, 32));
        click_pairing_tab(PAIR_PAGE_MANUAL);
        CHECK(pairing_pixel(point) == RGB(32, 32, 32));
        click_pairing_tab(PAIR_PAGE_QR);
        CHECK(pairing_pixel(point) == qr_pixel);

        unsigned generation = p.qrGen;
        SetWindowTextW(p.qrStatus, L"waiting");
        send_pairing_event(generation - 1, stale_success_events[i], L"stale success");
        CHECK(IsWindow(dialog) && IsWindow(owner));
        wchar_t status[160];
        GetWindowTextW(p.qrStatus, status, 160);
        CHECK(wcscmp(status, L"waiting") == 0);
        send_pairing_event(generation, QREV_FAILED, L"pairing failed");
        CHECK(IsWindow(dialog) && IsWindow(owner));
        GetWindowTextW(p.qrStatus, status, 160);
        CHECK(wcscmp(status, L"pairing failed") == 0);
        send_pairing_event(generation, QREV_CONNECTING, L"connecting");
        CHECK(IsWindow(dialog) && IsWindow(owner));
        GetWindowTextW(p.qrStatus, status, 160);
        CHECK(wcscmp(status, L"connecting") == 0);
        send_pairing_event(generation, QREV_PAIRED, L"paired, connection failed");
        CHECK(IsWindow(dialog) && IsWindow(owner));
        GetWindowTextW(p.qrStatus, status, 160);
        CHECK(wcscmp(status, L"paired, connection failed") == 0);
        send_pairing_event(generation, QREV_CONNECTED, L"paired and connected");
        CHECK(!IsWindow(dialog) && !p.frame);
        CHECK(IsWindow(owner));
    }
    CHECK(DestroyWindow(owner));
    DeleteObject(m.brBg);
    DeleteObject(m.brEdit);
    m.brBg = m.brEdit = NULL;
    puts("PASS pairing tabs clear QR pixels and restore controls and QR rendering");
    puts("PASS pairing preserves stale/failure/progress/paired states and closes only after connection");
}

int wmain(int argc, wchar_t **argv)
{
    /* ADB substitute used only by this test's worker subprocesses. */
    if (argc == 3 && wcscmp(argv[1], L"mdns") == 0) {
        if (wcscmp(argv[2], L"check") == 0) { puts("mdns daemon version test"); return 0; }
        puts("List of discovered mdns services\n"
             "unrelated _adb-tls-pairing._tcp 192.0.2.20:37000\n"
             "\"studio-scrdock-test\" _adb-tls-pairing._tcp 192.0.2.15:37111\n"
             "studio-plain-test _adb-tls-pairing._tcp 192.0.2.15:37112\n"
             "adb-stale _adb-tls-connect._tcp 192.0.2.15:38296\n"
             "adb-test _adb-tls-connect._tcp 192.0.2.15:38297");
        return 0;
    }
    if (argc == 4 && wcscmp(argv[1], L"pair") == 0) {
        bool quoted = wcscmp(argv[2], L"192.0.2.15:37111") == 0;
        bool plain = wcscmp(argv[2], L"192.0.2.15:37112") == 0;
        if ((!quoted && !plain) || wcscmp(argv[3], quoted ? L"\"123456\"" : L"123456")) return 2;
        puts("Successfully paired to 192.0.2.15:37111");
        return 0;
    }
    if (argc == 3 && wcscmp(argv[1], L"connect") == 0) {
        if (wcscmp(argv[2], L"192.0.2.15:38296") == 0) {
            puts("failed to connect to 192.0.2.15:38296: Connection refused");
            return 3;
        }
        if (wcscmp(argv[2], L"192.0.2.15:38297")) return 3;
        puts("adb diagnostic before connection");
        puts("connected to 192.0.2.15:38297");
        return 0;
    }
    if (argc == 2 && wcscmp(argv[1], L"--dialog") == 0) {
        test_pairing_window();
        return 0;
    }
    CHECK(wifi_output_connected("diagnostic\r\nconnected to 192.0.2.15:38297\r\n"));
    CHECK(wifi_output_connected("already connected to 192.0.2.15:38297\n"));
    CHECK(!wifi_output_connected("failed: not connected to 192.0.2.15:38297\n"));
    render_qr(200);
    render_qr(300);
    render_qr(400);
    wchar_t endpoint[80];
    CHECK(mdns_find_pairing_target("\"studio-scrdock-test\"\t_adb-tls-pairing._tcp\t192.0.2.15:37111\n",
                                   "studio-scrdock-test", endpoint, 80));
    CHECK(wcscmp(endpoint, L"192.0.2.15:37111") == 0);
    CHECK(mdns_find_pairing_target("studio-scrdock-test._adb-tls-pairing._tcp.local. _adb-tls-pairing._tcp 192.0.2.15:37111\n",
                                   "studio-scrdock-test", endpoint, 80));
    CHECK(!mdns_find_pairing_target("\"studio-scrdock-test-other\" _adb-tls-pairing._tcp 192.0.2.20:37000\n",
                                    "studio-scrdock-test", endpoint, 80));
    puts("PASS Android QR quoted service names and strict device matching");
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = event_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"scrdock_pairing_test";
    CHECK(RegisterClassW(&wc));
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 1, 1,
                                HWND_MESSAGE, NULL, wc.hInstance, NULL);
    CHECK(hwnd);
    for (int scenario = 0; scenario < 3; scenario++) {
        simulate_late_scan = scenario == 2;
        struct QrWork *w = calloc(1, sizeof(*w));
        CHECK(w);
        w->gen = 1;
        w->hwnd = hwnd;
        w->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE stop = w->stop;
        CHECK(stop);
        GetModuleFileNameW(NULL, w->adb, MAX_PATH);
        strcpy_s(w->service, 32, scenario == 0 ? "studio-plain-test" : "studio-scrdock-test");
        wcscpy_s(w->code, 16, L"123456");
        last_event = -1;
        qr_pair_thread(w);
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        CHECK(last_event == QREV_CONNECTED);
        CloseHandle(stop);
        puts(scenario == 2 ? "PASS QR late scan remains active"
             : scenario == 1 ? "PASS vendor QR literal-quote argv, pairing and connection"
                             : "PASS standard QR discovery, pairing and connection");
    }
    puts("PASS QR connection skips a stale endpoint on the same host");
    p.frame = hwnd;
    PostQuitMessage(17);
    qr_stop();
    MSG quit;
    CHECK(PeekMessageW(&quit, NULL, WM_QUIT, WM_QUIT, PM_REMOVE) && quit.wParam == 17);
    DestroyWindow(hwnd);
    puts("PASS QR rendering at three scales and quit-message preservation");
    test_pairing_window();
    return 0;
}
