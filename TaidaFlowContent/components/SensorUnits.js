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
function editNumber(value) { return Number(value.toPrecision(12)).toString() }

function parseNumber(text) {
    var trimmed = text.trim()
    if (!/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$/.test(trimmed))
        return NaN
    var value = Number(trimmed)
    return isFinite(value) ? value : NaN
}
