#ifndef SCRDOCK_STATE_H
#define SCRDOCK_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCRDOCK_STATE_TEXT 128
#define SCRDOCK_STATE_COMMAND 2048

typedef enum ScrdockDeviceKind {
    SCRDOCK_DEVICE_UNKNOWN = 0,
    SCRDOCK_DEVICE_USB = 1,
    SCRDOCK_DEVICE_WIFI = 2
} ScrdockDeviceKind;

/*
 * The identity fields are intentionally separate:
 *   device_id      remains stable when a device moves from USB to WiFi;
 *   connection_id  is the current adb/scrcpy transport address.
 */
typedef struct ScrdockStateDevice {
    wchar_t device_id[SCRDOCK_STATE_TEXT];
    wchar_t connection_id[SCRDOCK_STATE_TEXT];
    wchar_t serial[SCRDOCK_STATE_TEXT];
    wchar_t ip[64];
    unsigned short port;
    wchar_t state[32];
    bool online;
} ScrdockStateDevice;

typedef struct ScrdockSnapshotState {
    ScrdockStateDevice *items;
    size_t count;
    size_t capacity;
    bool stale;
    bool probed;
    unsigned error_code;
    uint64_t revision;
} ScrdockSnapshotState;

/* Returns true when the generated value fits in the destination buffer. */
bool scrdock_make_device_id(ScrdockDeviceKind kind,
                            const wchar_t *serial,
                            const wchar_t *ip,
                            wchar_t *out,
                            size_t out_count);

/* USB uses serial; WiFi uses ip:port. */
bool scrdock_make_connection_id(ScrdockDeviceKind kind,
                                const wchar_t *serial,
                                const wchar_t *ip,
                                unsigned short port,
                                wchar_t *out,
                                size_t out_count);

void scrdock_snapshot_init(ScrdockSnapshotState *state);
void scrdock_snapshot_dispose(ScrdockSnapshotState *state);

/*
 * Replaces the snapshot and increments revision only when observable state
 * changed. The input array is copied, so callers may release it afterwards.
 */
bool scrdock_snapshot_replace(ScrdockSnapshotState *state,
                              const ScrdockStateDevice *items,
                              size_t count,
                              bool stale,
                              bool probed,
                              unsigned error_code);

typedef struct ScrdockSessionRecord {
    wchar_t device_id[SCRDOCK_STATE_TEXT];
    wchar_t connection_id[SCRDOCK_STATE_TEXT];
    wchar_t command[SCRDOCK_STATE_COMMAND];
    uint64_t started_at;
    uint64_t ended_at;
    int exit_code;
    bool running;
    bool stop_requested;
    bool reconnect_pending;
} ScrdockSessionRecord;

typedef struct ScrdockSessionTable {
    ScrdockSessionRecord *items;
    size_t count;
    size_t capacity;
    size_t max_ended;
} ScrdockSessionTable;

void scrdock_session_table_init(ScrdockSessionTable *table, size_t max_ended);
void scrdock_session_table_dispose(ScrdockSessionTable *table);
ScrdockSessionRecord *scrdock_session_find(ScrdockSessionTable *table,
                                           const wchar_t *device_id);
const ScrdockSessionRecord *scrdock_session_find_const(
    const ScrdockSessionTable *table,
    const wchar_t *device_id);

/* Starts or replaces an ended session. Returns false for a duplicate running session. */
bool scrdock_session_begin(ScrdockSessionTable *table,
                           const wchar_t *device_id,
                           const wchar_t *connection_id,
                           const wchar_t *command,
                           uint64_t started_at);

/* Marks a session exited and prunes old ended records. */
bool scrdock_session_finish(ScrdockSessionTable *table,
                            const wchar_t *device_id,
                            int exit_code,
                            uint64_t ended_at);

#ifdef __cplusplus
}
#endif

#endif
