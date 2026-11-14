#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../state.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL state line %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static void device(ScrdockStateDevice *out,
                   const wchar_t *id,
                   const wchar_t *connection,
                   bool online)
{
    memset(out, 0, sizeof(*out));
    wcscpy_s(out->device_id, SCRDOCK_STATE_TEXT, id);
    wcscpy_s(out->connection_id, SCRDOCK_STATE_TEXT, connection);
    wcscpy_s(out->state, sizeof(out->state) / sizeof(out->state[0]),
             online ? L"device" : L"offline");
    out->online = online;
}

static int test_identity(void)
{
    wchar_t id[SCRDOCK_STATE_TEXT];
    wchar_t connection[SCRDOCK_STATE_TEXT];

    CHECK(scrdock_make_device_id(SCRDOCK_DEVICE_USB, L"ABC", NULL,
                                 id, SCRDOCK_STATE_TEXT));
    CHECK(wcscmp(id, L"device:ABC") == 0);
    CHECK(scrdock_make_device_id(SCRDOCK_DEVICE_WIFI, L"ABC", L"192.0.2.10",
                                 id, SCRDOCK_STATE_TEXT));
    CHECK(wcscmp(id, L"device:ABC") == 0);
    CHECK(scrdock_make_connection_id(SCRDOCK_DEVICE_WIFI, NULL, L"192.0.2.10",
                                     37111, connection, SCRDOCK_STATE_TEXT));
    CHECK(wcscmp(connection, L"192.0.2.10:37111") == 0);
    puts("PASS identity: stable device id and transport id are separate");
    return 0;
}

static int test_snapshot(void)
{
    ScrdockSnapshotState state;
    ScrdockStateDevice items[2];

    scrdock_snapshot_init(&state);
    device(&items[0], L"device:A", L"A", true);
    CHECK(scrdock_snapshot_replace(&state, items, 1, false, true, 0));
    CHECK(state.revision == 1 && state.count == 1 && !state.stale);
    CHECK(!scrdock_snapshot_replace(&state, items, 1, false, true, 0));
    CHECK(state.revision == 1);
    CHECK(scrdock_snapshot_replace(&state, items, 1, true, true, 7));
    CHECK(state.revision == 2 && state.stale && state.error_code == 7);
    device(&items[1], L"device:B", L"B", true);
    CHECK(scrdock_snapshot_replace(&state, items, 2, false, true, 0));
    CHECK(state.revision == 3 && state.count == 2);
    scrdock_snapshot_dispose(&state);
    puts("PASS snapshot: revision changes only on observable state changes");
    return 0;
}

static int test_sessions(void)
{
    ScrdockSessionTable table;
    const ScrdockSessionRecord *record;

    scrdock_session_table_init(&table, 2);
    CHECK(scrdock_session_begin(&table, L"device:A", L"A",
                                L"scrcpy -s A", 10));
    CHECK(!scrdock_session_begin(&table, L"device:A", L"A",
                                 L"scrcpy -s A", 11));
    CHECK(scrdock_session_finish(&table, L"device:A", 2, 20));
    record = scrdock_session_find_const(&table, L"device:A");
    CHECK(record && !record->running && record->exit_code == 2);

    CHECK(scrdock_session_begin(&table, L"device:B", L"B",
                                L"scrcpy -s B", 30));
    CHECK(scrdock_session_finish(&table, L"device:B", 0, 40));
    CHECK(scrdock_session_begin(&table, L"device:C", L"C",
                                L"scrcpy -s C", 50));
    CHECK(scrdock_session_finish(&table, L"device:C", 1, 60));
    CHECK(scrdock_session_find_const(&table, L"device:A") == NULL);
    CHECK(scrdock_session_find_const(&table, L"device:B") != NULL);
    CHECK(scrdock_session_find_const(&table, L"device:C") != NULL);
    scrdock_session_table_dispose(&table);
    puts("PASS sessions: duplicate running sessions rejected and old exits pruned");
    return 0;
}

int wmain(void)
{
    if (test_identity() != 0 || test_snapshot() != 0 || test_sessions() != 0) {
        return 1;
    }
    puts("PASS state module");
    return 0;
}
