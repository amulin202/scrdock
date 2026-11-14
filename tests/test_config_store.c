#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <wchar.h>
#include "../config_store.h"

#define CHECK(x) do { \
    if (!(x)) { \
        fwprintf(stderr, L"FAIL config line %d: %S\n", __LINE__, #x); \
        return 1; \
    } \
} while (0)

static bool make_temp_path(wchar_t out[MAX_PATH])
{
    wchar_t dir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (!n || n >= MAX_PATH) return false;
    if (!GetTempFileNameW(dir, L"sdc", 0, out)) return false;
    DeleteFileW(out);
    return true;
}

int wmain(void)
{
    wchar_t path[MAX_PATH];
    CHECK(make_temp_path(path));

    /* Legacy files have no [meta] version but keep the existing keys. */
    CHECK(WritePrivateProfileStringW(L"scrdock", L"serial", L"legacy-01", path));
    CHECK(WritePrivateProfileStringW(L"scrdock", L"control", L"adb", path));
    CHECK(WritePrivateProfileStringW(L"scrdock", L"reconnect_attempts", L"99", path));
    CHECK(WritePrivateProfileStringW(L"devices", L"usb-01", L"192.0.2.15", path));

    struct ConfigStore cfg;
    bool migrate = false;
    CHECK(config_store_load(path, &cfg, &migrate));
    CHECK(migrate);
    CHECK(wcscmp(cfg.serial, L"legacy-01") == 0);
    CHECK(cfg.control == 2);
    CHECK(cfg.reconnect_attempts == 99);

    wcscpy_s(cfg.scrcpy_args, sizeof(cfg.scrcpy_args) / sizeof(cfg.scrcpy_args[0]),
             L"--stay-awake");
    if (!config_store_save_atomic(path, &cfg)) {
        fwprintf(stderr, L"save path=%ls error=%lu\n", path, GetLastError());
        CHECK(false);
    }

    struct ConfigStore reread;
    migrate = true;
    CHECK(config_store_load(path, &reread, &migrate));
    CHECK(!migrate);
    CHECK(reread.schema_version == SCRDOCK_CONFIG_VERSION);
    CHECK(wcscmp(reread.serial, L"legacy-01") == 0);
    CHECK(wcscmp(reread.scrcpy_args, L"--stay-awake") == 0);
    CHECK(reread.control == 2);
    CHECK(reread.reconnect_attempts == 99);
    wchar_t wifi_ip[64];
    GetPrivateProfileStringW(L"devices", L"usb-01", L"", wifi_ip,
                             (DWORD)(sizeof(wifi_ip) / sizeof(wifi_ip[0])), path);
    CHECK(wcscmp(wifi_ip, L"192.0.2.15") == 0);

    DeleteFileW(path);
    fwprintf(stdout, L"config_store: OK\n");
    return 0;
}
