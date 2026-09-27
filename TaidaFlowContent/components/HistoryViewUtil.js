.pragma library

// Per-client history view helpers (docs/taidaflow_history_export_spec.md §2.1, revised
// 2026-09-27). The Core writes Td.historyViews, a map keyed by clientSessionId whose values
// are { fromMs, toMs, page, totalPages, totalRows, records, revision }; each client only
// reads its own key and asks for changes with
// Td.historyViewRequested(clientSessionId, fromMs, toMs, page).
// Pure functions, no QML types, so HistoryPage.qml's selection logic can be tested with
// qmltestrunner outside the app.

// Default range of a client that has not chosen one yet: the current month,
// first day 00:00:00.000 .. last day 23:59:59.999 (local time). Built with the
// local-time Date constructor, so month/year ends and DST are handled by the JS engine.
function monthRange(now) {
    var start = new Date(now.getFullYear(), now.getMonth(), 1)
    var nextMonth = new Date(now.getFullYear(), now.getMonth() + 1, 1)
    return { fromMs: start.getTime(), toMs: nextMonth.getTime() - 1 }
}

// "顯示前一周" (spec §2, revised 2026-09-25): the last 7 local calendar days
// including today, (today - 6 days) 00:00:00.000 .. today 23:59:59.999.
function lastWeekRange(now) {
    var start = new Date(now.getFullYear(), now.getMonth(), now.getDate() - 6)
    var endExclusive = new Date(now.getFullYear(), now.getMonth(), now.getDate() + 1)
    return { fromMs: start.getTime(), toMs: endExclusive.getTime() - 1 }
}

// This client's entry of historyViews, or null when there is none (not requested yet,
// or removed by the Core after 30 idle minutes / the 32-entry limit, spec §2.1).
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

// Entry -> the values HistoryPage shows. records are { timestampMs, values } (the
// format of the former shared records list) and are sorted newest first, as before.
function normalizeEntry(entry) {
    var rows = []
    var records = entry.records
    var count = records && records.length !== undefined ? records.length : 0
    for (var i = 0; i < count; ++i) {
        var record = records[i]
        if (!record)
            continue
        rows.push({ timestampMs: Number(record.timestampMs), values: record.values || [] })
    }
    rows.sort(function(a, b) { return b.timestampMs - a.timestampMs })
    var totalRows = Math.floor(Number(entry.totalRows))
    return {
        fromMs: Number(entry.fromMs),
        toMs: Number(entry.toMs),
        page: positiveInt(entry.page, 1),
        totalPages: positiveInt(entry.totalPages, 1),
        totalRows: isFinite(totalRows) && totalRows > 0 ? totalRows : 0,
        revision: entry.revision,
        rows: rows
    }
}

// What HistoryPage must do after historyViews changed. appliedRevision is the revision
// of the entry currently shown (null = none shown / forgotten).
//   { kind: "apply", view } - own entry present with a different revision: show `view`
//   { kind: "lost" }        - own entry gone while one was shown (Core removed it):
//                             forget the revision so a rebuilt entry is shown even if
//                             its revision restarts, keep the screen as is
//   { kind: "none" }        - nothing changed for this client (e.g. only other clients'
//                             entries changed, or still not loaded): the screen must not change
function viewChange(views, sessionId, appliedRevision) {
    var entry = ownEntry(views, sessionId)
    if (!entry)
        return { kind: appliedRevision === null || appliedRevision === undefined ? "none" : "lost" }
    if (appliedRevision !== null && appliedRevision !== undefined
            && String(entry.revision) === String(appliedRevision))
        return { kind: "none" }
    return { kind: "apply", view: normalizeEntry(entry) }
}
