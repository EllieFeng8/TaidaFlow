.pragma library

// w1-087: pure helpers of the device-offline banner (TopNav.qml, deviceOfflineBanner).
// Input: Td.deviceStatus (Core/TaidaFlowProxy.h, "Device status" block), mirrored from the
// Core, written only by the Core:
//   { adam6256: { name, address, online, sinceMs }, adam6217a: {...}, adam6217b: {...},
//     adam6224: {...}, adam6022: {...}, ms300: {...} }
// An empty map means "unknown" (main alone, an older Core): nothing is shown.

// Fixed key order of the banner (= TaidaFlowProxy::deviceStatusKeys()).
var DEVICE_KEYS = ["adam6256", "adam6217a", "adam6217b", "adam6224", "adam6022", "ms300"]

var BANNER_PREFIX = "設備離線："
var SEPARATOR = "、"

function isNonEmptyString(value) {
    return typeof value === "string" && value.length > 0
}

// Offline devices in DEVICE_KEYS order, each { key, name, address }.
// An entry counts only when it is an object with online === false (a real boolean) and a
// non-empty string name and address. Anything else - no map, empty map, unknown keys, missing
// or malformed fields, online true / missing / not a boolean - is ignored without error.
function offlineDevices(status) {
    var list = []
    if (!status || typeof status !== "object")
        return list
    for (var i = 0; i < DEVICE_KEYS.length; ++i) {
        var entry = status[DEVICE_KEYS[i]]
        if (!entry || typeof entry !== "object")
            continue
        if (entry.online !== false)
            continue
        if (!isNonEmptyString(entry.name) || !isNonEmptyString(entry.address))
            continue
        list.push({ key: DEVICE_KEYS[i], name: entry.name, address: entry.address })
    }
    return list
}

// Banner text, e.g. "設備離線：ADAM-6217（192.168.1.203）、MS300（COM2）"; "" when no device
// is offline (the banner is hidden then).
function bannerText(status) {
    var devices = offlineDevices(status)
    if (devices.length === 0)
        return ""
    var parts = []
    for (var i = 0; i < devices.length; ++i)
        parts.push(devices[i].name + "（" + devices[i].address + "）")
    return BANNER_PREFIX + parts.join(SEPARATOR)
}
