#ifndef SCRDOCK_CONFIG_STORE_H
#define SCRDOCK_CONFIG_STORE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>

/* Versioned persistence wrapper for scrdock's existing settings.
 *
 * The on-disk format intentionally stays INI-compatible so existing users can
 * upgrade without losing hand-edited settings.  Unlike the old call sites,
 * this API uses the wide Win32 profile functions and replaces the file in one
 * rename after writing a complete temporary file.
 */
#define SCRDOCK_CONFIG_VERSION 2

struct ConfigStore {
    int schema_version;
    wchar_t serial[128];
    wchar_t scrcpy[MAX_PATH];
    wchar_t scrcpy_args[512];
    wchar_t adb[MAX_PATH];
    BOOL close_on_exit;
    int control;             /* 0=auto, 1=shortcut, 2=adb */
    int bitrate;
    int max_size;
    int max_fps;
    BOOL turn_screen_off;
    BOOL stay_awake;
    BOOL no_audio;
    BOOL show_touches;
    BOOL auto_reconnect;
    int reconnect_attempts;
    BOOL wireless;
};

void config_store_defaults(struct ConfigStore *cfg);

/* Returns false only when the arguments or profile access fail. Invalid
 * numeric values are clamped to defaults. Missing files are treated as
 * defaults and return true. needs_migration is
 * set when the file predates the version marker or carries an older version.
 */
bool config_store_load(const wchar_t *path, struct ConfigStore *cfg,
                       bool *needs_migration);

/* Writes a complete configuration to a sibling temporary file, flushes it,
 * and atomically replaces path.  The caller may continue using cfg after the
 * call returns.
 */
bool config_store_save_atomic(const wchar_t *path,
                              const struct ConfigStore *cfg);

#endif
