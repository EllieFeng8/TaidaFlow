.pragma library

function pressureFactor(unit) {
    return unit === "psi" ? 1 / 6.894757293168 : unit === "bar" ? 0.01 : 1
}

function isPressure(id) { return id.indexOf("pt") === 0 || id === "filter" }
function factor(id, unit) { return isPressure(id) ? pressureFactor(unit) : 1 }
function unit(id, pressureUnit) {
    return isPressure(id) ? pressureUnit : id.indexOf("tt") === 0 ? "°C"
         : id === "flowMeter" ? "L/min" : "0/1"
}
function adjusted(raw, id, settings) {
    return raw + (settings[id] ? settings[id].offset : 0)
}
function display(raw, id, settings, pressureUnit) {
    return adjusted(raw, id, settings) * factor(id, pressureUnit)
}
// Limit rule shared by every main-page value and the Filter graphic (w1-088):
// 1 above the enabled upper limit, -1 below the enabled lower limit, otherwise 0.
// `value` is the unrounded corrected value in the base unit (kPa / °C / L/min,
// see adjusted()); equality with a limit is normal, disabled limits are ignored,
// so the display unit never changes the result.
function limitState(value, limits) {
    return limits && limits.upperEnabled && value > limits.upper ? 1
         : limits && limits.lowerEnabled && value < limits.lower ? -1 : 0
}
function sensorLimitState(raw, id, settings) {
    return limitState(adjusted(raw, id, settings), settings[id])
}
// Colors of the limit states, shared by the Filter graphic and the value displays.
function limitFillColor(state, normalColor) {
    return state > 0 ? "#D64550" : state < 0 ? "#D98A32" : normalColor
}
function limitBorderColor(state, normalColor) {
    return state > 0 ? "#FF8A80" : state < 0 ? "#FFD166" : normalColor
}
function editNumber(value) { return Number(value.toPrecision(12)).toString() }

function parseNumber(text) {
    var trimmed = text.trim()
    if (!/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$/.test(trimmed))
        return NaN
    var value = Number(trimmed)
    return isFinite(value) ? value : NaN
}
