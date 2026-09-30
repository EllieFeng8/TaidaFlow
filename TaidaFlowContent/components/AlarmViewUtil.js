.pragma library

// Alarm page range / view helpers (w1-078, 2026-09-30). Same pattern as HistoryViewUtil.js:
// the Core writes Td.alarmViews, a map keyed by clientSessionId (entry format in
// Core/TaidaFlowProxy.h), each client reads only its own key and asks for changes with
// Td.alarmViewRequested(clientSessionId, fromMs, toMs, page). While there is no own entry
// (Core without alarm views, main alone, or the entry was removed) AlarmPage.qml filters
// Td.alarmRecords itself with localView() below, exactly as the page did before.
// Pure functions, no QML types, so AlarmPage.qml's logic can be tested with qmltestrunner.

// Rows per page, the same in the Core's entry (pageSize) and in the local fallback.
var pageSize = 9

// Default range of the alarm page: the last 24 hours, minute precision like the fields.
// From = the minute of (now - 24 h) at :00.000, to = the minute of now at :59.999 (both
// inclusive), i.e. what the former page filtered with its fields "now - 24 h" .. "now"
// ([start minute, end minute + 1 min)). The 24 h are subtracted in ms, as before.
function defaultRange(now) {
    var start = new Date(now.getTime() - 24 * 60 * 60 * 1000)
    var from = new Date(start.getFullYear(), start.getMonth(), start.getDate(),
                        start.getHours(), start.getMinutes(), 0, 0)
    var to = new Date(now.getFullYear(), now.getMonth(), now.getDate(),
                      now.getHours(), now.getMinutes(), 59, 999)
    return { fromMs: from.getTime(), toMs: to.getTime() }
}

// One alarm row with the alarmRecords field types the page shows.
function normalizeRow(row, serialNumber) {
    return {
        serialNumber: serialNumber,
        timestampMs: Number(row.timestampMs),
        alarmTime: String(row.alarmTime),
        equipment: String(row.equipment),
        sensorName: String(row.sensorName),
        alarmMessage: String(row.alarmMessage),
        severity: String(row.severity),
        alarmStatus: String(row.alarmStatus)
    }
}

function totalPagesFor(count, size) {
    return Math.max(1, Math.ceil(count / size))
}

// Fallback without an own entry: the former AlarmPage filter and paging on alarmRecords.
// Keeps the records' order (the Core sends them newest first), numbers the rows in the
// range from 1, counts "未處理" in the whole range, and returns one page of `size` rows
// (page clamped to 1..totalPages; page 1 when the range is empty).
function localView(records, fromMs, toMs, page, size) {
    var filtered = []
    var activeCount = 0
    var count = records && records.length !== undefined ? records.length : 0
    for (var i = 0; i < count; ++i) {
        var record = records[i]
        if (!record)
            continue
        var timestampMs = Number(record.timestampMs)
        if (timestampMs >= fromMs && timestampMs <= toMs) {
            var row = normalizeRow(record, filtered.length + 1)
            if (row.alarmStatus === "未處理")
                activeCount += 1
            filtered.push(row)
        }
    }
    var totalPages = totalPagesFor(filtered.length, size)
    var shownPage = filtered.length === 0 ? 1 : Math.max(1, Math.min(Math.floor(page) || 1, totalPages))
    var start = (shownPage - 1) * size
    return {
        fromMs: fromMs,
        toMs: toMs,
        page: shownPage,
        pageSize: size,
        totalCount: filtered.length,
        totalPages: totalPages,
        activeCount: activeCount,
        rows: filtered.slice(start, start + size),
        state: "ready",
        message: ""
    }
}

// This client's entry of alarmViews, or null when there is none.
function ownEntry(views, sessionId) {
    if (!views || !sessionId)
        return null
    var entry = views[sessionId]
    return entry !== undefined && entry !== null && typeof entry === "object" ? entry : null
}

function positiveInt(value, fallback) {
    var number = Math.floor(Number(value))
    return isFinite(number) && number >= 1 ? number : fallback
}

function nonNegativeInt(value) {
    var number = Math.floor(Number(value))
    return isFinite(number) && number > 0 ? number : 0
}

// Entry -> the values AlarmPage shows. rows keep the Core's order (newest first, see
// TaidaFlowProxy.h); a row without serialNumber gets its position in the whole range.
function normalizeEntry(entry) {
    var size = positiveInt(entry.pageSize, pageSize)
    var totalCount = nonNegativeInt(entry.totalCount)
    var page = positiveInt(entry.page, 1)
    var rows = []
    var source = entry.rows
    var count = source && source.length !== undefined ? source.length : 0
    for (var i = 0; i < count; ++i) {
        var row = source[i]
        if (!row)
            continue
        var serial = positiveInt(row.serialNumber, (page - 1) * size + rows.length + 1)
        rows.push(normalizeRow(row, serial))
    }
    return {
        fromMs: Number(entry.fromMs),
        toMs: Number(entry.toMs),
        page: page,
        pageSize: size,
        totalCount: totalCount,
        totalPages: positiveInt(entry.totalPages, totalPagesFor(totalCount, size)),
        activeCount: nonNegativeInt(entry.activeCount),
        rows: rows,
        state: entry.state === "error" ? "error" : "ready",
        message: entry.message !== undefined && entry.message !== null ? String(entry.message) : "",
        revision: entry.revision
    }
}

// What AlarmPage must do after alarmViews changed (same rules as HistoryViewUtil.viewChange).
// appliedRevision is the revision of the entry currently shown (null = none shown).
//   { kind: "apply", view } - own entry present with a different revision: show `view`
//   { kind: "lost" }        - own entry gone while one was shown: back to the alarmRecords
//                             fallback with the same range / page (the screen never goes blank)
//   { kind: "none" }        - nothing changed for this client: the screen must not change
function viewChange(views, sessionId, appliedRevision) {
    var entry = ownEntry(views, sessionId)
    if (!entry)
        return { kind: appliedRevision === null || appliedRevision === undefined ? "none" : "lost" }
    if (appliedRevision !== null && appliedRevision !== undefined
            && String(entry.revision) === String(appliedRevision))
        return { kind: "none" }
    return { kind: "apply", view: normalizeEntry(entry) }
}
