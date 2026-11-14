#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "config_store.h"

static bool file_exists_w(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static int read_int(const wchar_t *section, const wchar_t *key, int def,
                    const wchar_t *path)
{
    return GetPrivateProfileIntW(section, key, def, path);
}

static void read_string(const wchar_t *section, const wchar_t *key,
                        const wchar_t *def, wchar_t *out, size_t cch,
                        const wchar_t *path)
{
    if (!out || !cch) return;
    GetPrivateProfileStringW(section, key, def ? def : L"", out,
                             (DWORD)cch, path);
    out[cch - 1] = L'\0';
}

void config_store_defaults(struct ConfigStore *cfg)
{
    if (!cfg) return;
    ZeroMemory(cfg, sizeof(*cfg));
    cfg->schema_version = SCRDOCK_CONFIG_VERSION;
    cfg->close_on_exit = TRUE;
    cfg->control = 0;
    cfg->bitrate = 0;
    cfg->max_size = 0;
    cfg->max_fps = 0;
    cfg->turn_screen_off = FALSE;
    cfg->stay_awake = FALSE;
    cfg->no_audio = FALSE;
    cfg->show_touches = FALSE;
    cfg->auto_reconnect = TRUE;
    cfg->reconnect_attempts = 5;
    cfg->wireless = FALSE;
}

static bool valid_control(int value)
{
    return value >= 0 && value <= 2;
}

static bool valid_nonnegative(int value)
{
    return value >= 0 && value <= 1000000;
}

static int read_control(const wchar_t *path, int def)
{
    wchar_t text[32];
    read_string(L"scrdock", L"control", L"", text,
                sizeof(text) / sizeof(text[0]), path);
    if (!text[0]) {
        return def;
    }
    if (_wcsicmp(text, L"shortcut") == 0) return 1;
    if (_wcsicmp(text, L"adb") == 0) return 2;
    if (_wcsicmp(text, L"auto") == 0) return 0;
    return _wtoi(text);
}

bool config_store_load(const wchar_t *path, struct ConfigStore *cfg,
                       bool *needs_migration)
{
    if (!path || !cfg) return false;
    config_store_defaults(cfg);
    if (needs_migration) *needs_migration = false;
    if (!file_exists_w(path)) return true;

    int version = read_int(L"meta", L"version", 0, path);
    cfg->schema_version = version > 0 ? version : SCRDOCK_CONFIG_VERSION;
    if (needs_migration && version < SCRDOCK_CONFIG_VERSION) {
        *needs_migration = true;
    }

    read_string(L"scrdock", L"serial", L"", cfg->serial,
                sizeof(cfg->serial) / sizeof(cfg->serial[0]), path);
    read_string(L"scrdock", L"scrcpy", L"", cfg->scrcpy,
                sizeof(cfg->scrcpy) / sizeof(cfg->scrcpy[0]), path);
    read_string(L"scrdock", L"scrcpy_args", L"", cfg->scrcpy_args,
                sizeof(cfg->scrcpy_args) / sizeof(cfg->scrcpy_args[0]), path);
    read_string(L"scrdock", L"adb", L"", cfg->adb,
                sizeof(cfg->adb) / sizeof(cfg->adb[0]), path);

    cfg->close_on_exit = read_int(L"scrdock", L"close_scrcpy_on_exit",
                                  cfg->close_on_exit ? 1 : 0, path) != 0;
    cfg->control = read_control(path, cfg->control);
    cfg->bitrate = read_int(L"scrdock", L"bitrate", cfg->bitrate, path);
    cfg->max_size = read_int(L"scrdock", L"max_size", cfg->max_size, path);
    cfg->max_fps = read_int(L"scrdock", L"max_fps", cfg->max_fps, path);
    cfg->turn_screen_off = read_int(L"scrdock", L"turn_screen_off",
                                    cfg->turn_screen_off ? 1 : 0, path) != 0;
    cfg->stay_awake = read_int(L"scrdock", L"stay_awake",
                               cfg->stay_awake ? 1 : 0, path) != 0;
    cfg->no_audio = read_int(L"scrdock", L"no_audio",
                             cfg->no_audio ? 1 : 0, path) != 0;
    cfg->show_touches = read_int(L"scrdock", L"show_touches",
                                 cfg->show_touches ? 1 : 0, path) != 0;
    cfg->auto_reconnect = read_int(L"scrdock", L"auto_reconnect",
                                   cfg->auto_reconnect ? 1 : 0, path) != 0;
    cfg->reconnect_attempts = read_int(L"scrdock", L"reconnect_attempts",
                                       cfg->reconnect_attempts, path);
    cfg->wireless = read_int(L"scrdock", L"wireless",
                             cfg->wireless ? 1 : 0, path) != 0;

    /* Clamp corrupted hand-edited values to the same safe defaults used by
     * the legacy loader.  A malformed profile remains readable and can be
     * repaired by the next successful save. */
    if (!valid_control(cfg->control)) cfg->control = 0;
    if (!valid_nonnegative(cfg->bitrate)) cfg->bitrate = 0;
    if (!valid_nonnegative(cfg->max_size)) cfg->max_size = 0;
    if (!valid_nonnegative(cfg->max_fps)) cfg->max_fps = 0;
    if (cfg->reconnect_attempts < 0 || cfg->reconnect_attempts > 1000) {
        cfg->reconnect_attempts = 5;
    }
    cfg->schema_version = SCRDOCK_CONFIG_VERSION;
    return true;
}

static bool write_string(const wchar_t *section, const wchar_t *key,
                         const wchar_t *value, const wchar_t *path)
{
    return WritePrivateProfileStringW(section, key, value ? value : L"", path)
           != FALSE;
}

static bool write_int(const wchar_t *section, const wchar_t *key, int value,
                      const wchar_t *path)
{
    wchar_t text[32];
    _snwprintf_s(text, sizeof(text) / sizeof(text[0]), _TRUNCATE, L"%d", value);
    return write_string(section, key, text, path);
}

static bool write_control(int control, const wchar_t *path)
{
    const wchar_t *text = control == 1 ? L"shortcut"
                         : control == 2 ? L"adb" : L"auto";
    return write_string(L"scrdock", L"control", text, path);
}

static bool flush_profile(const wchar_t *path)
{
    /* NULL section/key asks the profile API to flush its cache.  Some Windows
     * builds return FALSE when that cache is already empty even though the
     * writes are present on disk, so the actual file handle flush below is the
     * authoritative check. */
    (void)WritePrivateProfileStringW(NULL, NULL, NULL, path);
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    BOOL ok = FlushFileBuffers(h);
    CloseHandle(h);
    return ok != FALSE;
}

static bool write_complete(const wchar_t *path, const struct ConfigStore *cfg)
{
    if (!write_int(L"meta", L"version", SCRDOCK_CONFIG_VERSION, path)) return false;
    if (!write_string(L"scrdock", L"serial", cfg->serial, path)) return false;
    if (!write_string(L"scrdock", L"scrcpy", cfg->scrcpy, path)) return false;
    if (!write_string(L"scrdock", L"scrcpy_args", cfg->scrcpy_args, path)) return false;
    if (!write_string(L"scrdock", L"adb", cfg->adb, path)) return false;
    if (!write_int(L"scrdock", L"close_scrcpy_on_exit", cfg->close_on_exit ? 1 : 0, path)) return false;
    if (!write_control(cfg->control, path)) return false;
    if (!write_int(L"scrdock", L"bitrate", cfg->bitrate, path)) return false;
    if (!write_int(L"scrdock", L"max_size", cfg->max_size, path)) return false;
    if (!write_int(L"scrdock", L"max_fps", cfg->max_fps, path)) return false;
    if (!write_int(L"scrdock", L"turn_screen_off", cfg->turn_screen_off ? 1 : 0, path)) return false;
    if (!write_int(L"scrdock", L"stay_awake", cfg->stay_awake ? 1 : 0, path)) return false;
    if (!write_int(L"scrdock", L"no_audio", cfg->no_audio ? 1 : 0, path)) return false;
    if (!write_int(L"scrdock", L"show_touches", cfg->show_touches ? 1 : 0, path)) return false;
    if (!write_int(L"scrdock", L"auto_reconnect", cfg->auto_reconnect ? 1 : 0, path)) return false;
    if (!write_int(L"scrdock", L"reconnect_attempts", cfg->reconnect_attempts, path)) return false;
    if (!write_int(L"scrdock", L"wireless", cfg->wireless ? 1 : 0, path)) return false;
    if (!flush_profile(path)) return false;
    return true;
}

bool config_store_save_atomic(const wchar_t *path,
                              const struct ConfigStore *cfg)
{
    if (!path || !cfg) return false;
    wchar_t temp[MAX_PATH * 2];
    wchar_t devices[32768];
    DWORD device_chars = 0;
    bool have_devices = false;

    /* The wireless map is still owned by the existing UI code. Preserve its
     * section while replacing the rest of the file atomically. */
    if (file_exists_w(path)) {
        device_chars = GetPrivateProfileSectionW(
            L"devices", devices, (DWORD)(sizeof(devices) / sizeof(devices[0])), path);
        have_devices = device_chars != 0;
    }
    _snwprintf_s(temp, sizeof(temp) / sizeof(temp[0]), _TRUNCATE, L"%s.tmp", path);
    DeleteFileW(temp);

    /* The profile APIs generally create a missing file, but older Windows
     * builds can leave their cache unflushed when the target did not exist
     * before the first write.  Create the sibling explicitly so the flush and
     * rename path is deterministic. */
    HANDLE seed = CreateFileW(temp, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (seed == INVALID_HANDLE_VALUE) return false;
    CloseHandle(seed);

    if (!write_complete(temp, cfg)) {
        /* Preserve the profile API's failure status for callers. */
        DWORD err = GetLastError();
        DeleteFileW(temp);
        SetLastError(err);
        return false;
    }
    if (have_devices
            && !WritePrivateProfileSectionW(L"devices", devices, temp)) {
        DWORD err = GetLastError();
        DeleteFileW(temp);
        SetLastError(err);
        return false;
    }
    if (!flush_profile(temp)) {
        DWORD err = GetLastError();
        DeleteFileW(temp);
        SetLastError(err);
        return false;
    }
    if (!MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        DeleteFileW(temp);
        SetLastError(err);
        return false;
    }
    return true;
}
