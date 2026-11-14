#include "state.h"

#include <stdlib.h>
#include <string.h>

static bool copy_text(wchar_t *out, size_t out_count, const wchar_t *src)
{
    size_t len;

    if (!out || out_count == 0) {
        return false;
    }
    if (!src) {
        out[0] = L'\0';
        return true;
    }
    len = wcslen(src);
    if (len >= out_count) {
        out[0] = L'\0';
        return false;
    }
    if (len != 0) {
        wmemcpy(out, src, len);
    }
    out[len] = L'\0';
    return true;
}

static bool append_text(wchar_t *out,
                        size_t out_count,
                        size_t *used,
                        const wchar_t *src)
{
    size_t len;

    if (!out || !used || !src || *used >= out_count) {
        return false;
    }
    len = wcslen(src);
    if (len > out_count - *used - 1) {
        out[0] = L'\0';
        return false;
    }
    if (len != 0) {
        wmemcpy(out + *used, src, len);
    }
    *used += len;
    out[*used] = L'\0';
    return true;
}

bool scrdock_make_device_id(ScrdockDeviceKind kind,
                            const wchar_t *serial,
                            const wchar_t *ip,
                            wchar_t *out,
                            size_t out_count)
{
    const wchar_t *prefix;
    size_t used = 0;

    (void) kind;
    if (!out || out_count == 0) {
        return false;
    }
    out[0] = L'\0';

    /* A serial is the strongest identity and survives USB -> WiFi changes. */
    if (serial && serial[0]) {
        prefix = L"device:";
        return append_text(out, out_count, &used, prefix)
            && append_text(out, out_count, &used, serial);
    }
    if (kind == SCRDOCK_DEVICE_WIFI && ip && ip[0]) {
        prefix = L"wifi:";
        return append_text(out, out_count, &used, prefix)
            && append_text(out, out_count, &used, ip);
    }
    return false;
}

bool scrdock_make_connection_id(ScrdockDeviceKind kind,
                                const wchar_t *serial,
                                const wchar_t *ip,
                                unsigned short port,
                                wchar_t *out,
                                size_t out_count)
{
    size_t used = 0;
    wchar_t port_text[16];

    if (!out || out_count == 0) {
        return false;
    }
    out[0] = L'\0';
    if (kind == SCRDOCK_DEVICE_USB) {
        return serial && serial[0] && copy_text(out, out_count, serial);
    }
    if (kind == SCRDOCK_DEVICE_WIFI && ip && ip[0] && port != 0) {
        if (swprintf(port_text, sizeof(port_text) / sizeof(port_text[0]),
                     L"%u", (unsigned) port) < 0) {
            return false;
        }
        return append_text(out, out_count, &used, ip)
            && append_text(out, out_count, &used, L":")
            && append_text(out, out_count, &used, port_text);
    }
    return false;
}

static bool device_equal(const ScrdockStateDevice *a,
                         const ScrdockStateDevice *b)
{
    return wcscmp(a->device_id, b->device_id) == 0
        && wcscmp(a->connection_id, b->connection_id) == 0
        && wcscmp(a->serial, b->serial) == 0
        && wcscmp(a->ip, b->ip) == 0
        && a->port == b->port
        && wcscmp(a->state, b->state) == 0
        && a->online == b->online;
}

static bool snapshot_equal(const ScrdockSnapshotState *state,
                           const ScrdockStateDevice *items,
                           size_t count,
                           bool stale,
                           bool probed,
                           unsigned error_code)
{
    size_t i;

    if (state->count != count || state->stale != stale || state->probed != probed
        || state->error_code != error_code) {
        return false;
    }
    for (i = 0; i < count; ++i) {
        if (!device_equal(&state->items[i], &items[i])) {
            return false;
        }
    }
    return true;
}

static bool snapshot_reserve(ScrdockSnapshotState *state, size_t count)
{
    size_t next;
    ScrdockStateDevice *items;

    if (count <= state->capacity) {
        return true;
    }
    next = state->capacity == 0 ? 4 : state->capacity;
    while (next < count) {
        if (next > ((size_t) -1) / 2) {
            next = count;
            break;
        }
        next *= 2;
    }
    items = (ScrdockStateDevice *) realloc(state->items,
                                           next * sizeof(*items));
    if (!items) {
        return false;
    }
    state->items = items;
    state->capacity = next;
    return true;
}

void scrdock_snapshot_init(ScrdockSnapshotState *state)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

void scrdock_snapshot_dispose(ScrdockSnapshotState *state)
{
    if (!state) {
        return;
    }
    free(state->items);
    memset(state, 0, sizeof(*state));
}

bool scrdock_snapshot_replace(ScrdockSnapshotState *state,
                              const ScrdockStateDevice *items,
                              size_t count,
                              bool stale,
                              bool probed,
                              unsigned error_code)
{
    if (!state || (count != 0 && !items)) {
        return false;
    }
    if (snapshot_equal(state, items, count, stale, probed, error_code)) {
        return false;
    }
    if (!snapshot_reserve(state, count)) {
        return false;
    }
    if (count != 0) {
        memcpy(state->items, items, count * sizeof(*items));
    }
    state->count = count;
    state->stale = stale;
    state->probed = probed;
    state->error_code = error_code;
    state->revision++;
    return true;
}

static bool table_reserve(ScrdockSessionTable *table, size_t count)
{
    size_t next;
    ScrdockSessionRecord *items;

    if (count <= table->capacity) {
        return true;
    }
    next = table->capacity == 0 ? 4 : table->capacity;
    while (next < count) {
        if (next > ((size_t) -1) / 2) {
            next = count;
            break;
        }
        next *= 2;
    }
    items = (ScrdockSessionRecord *) realloc(table->items,
                                             next * sizeof(*items));
    if (!items) {
        return false;
    }
    table->items = items;
    table->capacity = next;
    return true;
}

void scrdock_session_table_init(ScrdockSessionTable *table, size_t max_ended)
{
    if (!table) {
        return;
    }
    memset(table, 0, sizeof(*table));
    table->max_ended = max_ended;
}

void scrdock_session_table_dispose(ScrdockSessionTable *table)
{
    if (!table) {
        return;
    }
    free(table->items);
    memset(table, 0, sizeof(*table));
}

ScrdockSessionRecord *scrdock_session_find(ScrdockSessionTable *table,
                                           const wchar_t *device_id)
{
    size_t i;

    if (!table || !device_id || !device_id[0]) {
        return NULL;
    }
    for (i = 0; i < table->count; ++i) {
        if (wcscmp(table->items[i].device_id, device_id) == 0) {
            return &table->items[i];
        }
    }
    return NULL;
}

const ScrdockSessionRecord *scrdock_session_find_const(
    const ScrdockSessionTable *table,
    const wchar_t *device_id)
{
    size_t i;

    if (!table || !device_id || !device_id[0]) {
        return NULL;
    }
    for (i = 0; i < table->count; ++i) {
        if (wcscmp(table->items[i].device_id, device_id) == 0) {
            return &table->items[i];
        }
    }
    return NULL;
}

static void prune_ended(ScrdockSessionTable *table)
{
    size_t ended;

    if (table->max_ended == (size_t) -1) {
        return;
    }
    for (;;) {
        size_t i;
        size_t oldest = (size_t) -1;
        uint64_t oldest_time = (uint64_t) -1;

        ended = 0;
        for (i = 0; i < table->count; ++i) {
            if (!table->items[i].running) {
                ++ended;
                if (table->items[i].ended_at < oldest_time) {
                    oldest = i;
                    oldest_time = table->items[i].ended_at;
                }
            }
        }
        if (ended <= table->max_ended || oldest == (size_t) -1) {
            return;
        }
        if (oldest + 1 < table->count) {
            memmove(&table->items[oldest], &table->items[oldest + 1],
                    (table->count - oldest - 1) * sizeof(*table->items));
        }
        --table->count;
    }
}

bool scrdock_session_begin(ScrdockSessionTable *table,
                           const wchar_t *device_id,
                           const wchar_t *connection_id,
                           const wchar_t *command,
                           uint64_t started_at)
{
    ScrdockSessionRecord *record;

    if (!table || !device_id || !device_id[0] || !connection_id || !command) {
        return false;
    }
    record = scrdock_session_find(table, device_id);
    if (record && record->running) {
        return false;
    }
    if (!record) {
        if (!table_reserve(table, table->count + 1)) {
            return false;
        }
        record = &table->items[table->count++];
    }
    memset(record, 0, sizeof(*record));
    if (!copy_text(record->device_id, SCRDOCK_STATE_TEXT, device_id)
        || !copy_text(record->connection_id, SCRDOCK_STATE_TEXT, connection_id)
        || !copy_text(record->command, SCRDOCK_STATE_COMMAND, command)) {
        memset(record, 0, sizeof(*record));
        return false;
    }
    record->started_at = started_at;
    record->exit_code = -1;
    record->running = true;
    return true;
}

bool scrdock_session_finish(ScrdockSessionTable *table,
                            const wchar_t *device_id,
                            int exit_code,
                            uint64_t ended_at)
{
    ScrdockSessionRecord *record;

    record = scrdock_session_find(table, device_id);
    if (!record || !record->running) {
        return false;
    }
    record->running = false;
    record->exit_code = exit_code;
    record->ended_at = ended_at;
    record->reconnect_pending = false;
    prune_ended(table);
    return true;
}
