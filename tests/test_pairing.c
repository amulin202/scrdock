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

int wmain(int argc, wchar_t **argv)
{
    /* ADB substitute used only by this test's worker subprocesses. */
    if (argc == 3 && wcscmp(argv[1], L"mdns") == 0) {
        if (wcscmp(argv[2], L"check") == 0) { puts("mdns daemon version test"); return 0; }
        puts("List of discovered mdns services\n"
             "unrelated _adb-tls-pairing._tcp 192.0.2.20:37000\n"
             "\"studio-scrdock-test\" _adb-tls-pairing._tcp 192.0.2.15:37111\n"
             "studio-plain-test _adb-tls-pairing._tcp 192.0.2.15:37112\n"
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
        if (wcscmp(argv[2], L"192.0.2.15:38297")) return 3;
        puts("connected to 192.0.2.15:38297");
        return 0;
    }
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
    p.frame = hwnd;
    PostQuitMessage(17);
    qr_stop();
    MSG quit;
    CHECK(PeekMessageW(&quit, NULL, WM_QUIT, WM_QUIT, PM_REMOVE) && quit.wParam == 17);
    DestroyWindow(hwnd);
    puts("PASS QR rendering at three scales and quit-message preservation");
    return 0;
}
