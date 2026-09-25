.pragma library

// Date/time helpers of the History page range (spec docs/taidaflow_history_export_spec.md
// §2, revised 2026-09-25): fields are "YYYY/MM/DD HH:mm" (minute precision, local time).
// Shared by HistoryPage.qml (filter request, range display) and components/DateTimeField.qml
// (picker). Pure functions, no QML types, so the arithmetic can be tested outside the app.
//
// Parts object: { year, month (0-based, like Date.getMonth()), day, hour, minute }.

function pad2(value) {
    return value < 10 ? "0" + value : String(value)
}

function formatParts(year, month, day, hour, minute) {
    return year + "/" + pad2(month + 1) + "/" + pad2(day) + " " + pad2(hour) + ":" + pad2(minute)
}

// "YYYY/MM/DD HH:mm" of a Date in local time (seconds/ms are dropped, so the
// range end 23:59:59.999 shows as 23:59).
function formatDateTime(date) {
    return formatParts(date.getFullYear(), date.getMonth(), date.getDate(),
                       date.getHours(), date.getMinutes())
}

// Default time of a field when only a date is given: the start field is the
// first minute of the day (00:00), the end field the last one (23:59).
function defaultHour(isEnd) { return isEnd ? 23 : 0 }
function defaultMinute(isEnd) { return isEnd ? 59 : 0 }

// Parts of `date`'s calendar day with the field's default time (picker default:
// empty or invalid field -> today 00:00 / today 23:59).
function dayParts(date, isEnd) {
    return {
        year: date.getFullYear(), month: date.getMonth(), day: date.getDate(),
        hour: defaultHour(isEnd), minute: defaultMinute(isEnd)
    }
}

// Accepts "YYYY/MM/DD HH:mm" and the old date-only "YYYY/MM/DD" ('-' is also
// accepted as the date separator, as before). Date only: start = 00:00,
// end = 23:59. Hour 0-23, minute 0-59, and the calendar date must exist
// (2026/02/30 is rejected). Returns the parts object, or null when invalid.
function parseDateTime(text, isEnd) {
    var match = String(text).trim()
            .match(/^(\d{4})[\/-](\d{2})[\/-](\d{2})(?:\s+(\d{1,2}):(\d{2}))?$/)
    if (!match)
        return null

    var year = Number(match[1])
    var month = Number(match[2]) - 1
    var day = Number(match[3])
    var hasTime = match[4] !== undefined && match[4] !== ""
    var hour = hasTime ? Number(match[4]) : defaultHour(isEnd)
    var minute = hasTime ? Number(match[5]) : defaultMinute(isEnd)
    if (hour > 23 || minute > 59)
        return null

    var check = new Date(year, month, day)
    if (check.getFullYear() !== year || check.getMonth() !== month || check.getDate() !== day)
        return null

    return { year: year, month: month, day: day, hour: hour, minute: minute }
}

// Range ends in epoch ms (spec §2): the start is the first millisecond of the
// start minute (:00.000), the end the last millisecond of the end minute
// (:59.999). Both are built with the local-time Date constructor - no
// millisecond arithmetic - so month/year ends and DST changes are handled by
// the JS engine exactly like the calendar day boundaries before.
function startMs(parts) {
    return new Date(parts.year, parts.month, parts.day, parts.hour, parts.minute, 0, 0).getTime()
}

function endMs(parts) {
    return new Date(parts.year, parts.month, parts.day, parts.hour, parts.minute, 59, 999).getTime()
}
