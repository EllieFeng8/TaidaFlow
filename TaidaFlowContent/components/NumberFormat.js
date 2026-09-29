.pragma library

// Display formatting of process values on the main page (w1-056). The Proxy (Td) exposes
// raw doubles; values read back from the device or converted from raw registers can carry
// long binary fractions (e.g. 30.012970168612192). Every place that shows such a value
// formats it here, so the rule lives in one file. Pure functions, no QML types, so they can
// be tested headless with qmltestrunner.
//
// Missing values: null, undefined, "", NaN and +/-Infinity are shown as the em dash "—"
// (U+2014, already in the web font subset), so "no data" is never mistaken for a real 0.

var missingText = "—"

function toFiniteNumber(value) {
    if (value === null || value === undefined || value === "")
        return NaN
    var number = Number(value)
    return isFinite(number) ? number : NaN
}

// PV (measured value): always 1 decimal, like the TT/PT/pump Hz PVs (toFixed(1)).
// 30.012970168612192 -> "30.0", 50 -> "50.0", -0.04 -> "0.0" (no "-0.0").
function formatPv(value) {
    var number = toFiniteNumber(value)
    if (isNaN(number))
        return missingText
    var text = number.toFixed(1)
    return text === "-0.0" ? "0.0" : text
}

// SV (set value): at most 2 decimals without trailing zeros (the edit dialog's
// DoubleValidator also allows 2). 30.01 -> "30.01", 40 -> "40", 50.5 -> "50.5",
// 30.012970168612192 -> "30.01", 1e-9 -> "0".
function formatSv(value) {
    var number = toFiniteNumber(value)
    if (isNaN(number))
        return missingText
    // Number(...) drops the trailing zeros of toFixed(2); adding 0 turns -0 into 0.
    return String(Number(number.toFixed(2)) + 0)
}

// Initial text of an SV edit dialog: the same text as formatSv, but empty when there is no
// value, so the field never starts with a character its DoubleValidator rejects.
function svEditText(value) {
    return isNaN(toFiniteNumber(value)) ? "" : formatSv(value)
}

// Range check of an SV edit dialog before it writes (w1-070): M1..M4 0 ~ 100, pump Hz 0 ~ 60.
// The text must be a plain decimal number with at most 2 decimals (what the dialogs'
// DoubleValidator allows, no exponent) and lie in [minValue, maxValue], both ends included.
// Returns the number to write, or NaN when nothing may be written (empty, not a number, more
// than 2 decimals, out of range). Never clamps: 100.01 is rejected, not turned into 100.
function parseSvInRange(text, minValue, maxValue) {
    var trimmed = (text === null || text === undefined) ? "" : String(text).trim()
    if (!/^[+-]?(\d+(\.\d{0,2})?|\.\d{1,2})$/.test(trimmed))
        return NaN
    var number = Number(trimmed)
    if (!isFinite(number) || number < minValue || number > maxValue)
        return NaN
    // Adding 0 turns -0 into 0.
    return number + 0
}

// "0 ~ 100": the range shown in an SV edit dialog and in its "請輸入 ..." hint (w1-070).
function rangeText(minValue, maxValue) {
    return formatSv(minValue) + " ~ " + formatSv(maxValue)
}
