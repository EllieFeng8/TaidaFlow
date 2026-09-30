# Sensor settings Proxy contract

`pressureUnitSv` is a writable, mirrored QString (`kPa`, `psi`, `bar`),
initially `kPa`. PT process values and all stored pressure settings are in kPa.
Changing the display unit does not modify PVs or stored limits/offsets.

`sensorSettingsSv` is a writable, mirrored QVariantMap with these keys:
`pt01`–`pt07`, `tt01`–`tt04`, `flowMeter`, `filter`.
Every entry contains:

| Field | Type | Meaning |
| --- | --- | --- |
| offset | double | Additive correction, initially zero |
| lower / upper | double | Threshold in the sensor's base unit |
| lowerEnabled / upperEnabled | bool | Whether the respective threshold is configured |

Base units: pressure/filter kPa; temperature °C; flow L/min.
Thresholds start disabled. The settings page treats empty limit fields as
disabled, permits negative offsets, and rejects non-finite input and lower > upper
when both limits are enabled. The Proxy validates the complete map atomically.
To update one sensor, copy the current map, replace that entry, and call the setter.
The existing mirror discovers both properties via READ/WRITE/NOTIFY.

The UI displays raw PV + offset; the backend must keep PVs raw to avoid applying
the offset twice. Filter pressure difference is corrected PT-02 minus corrected
PT-03; filter has no independent offset in the UI. The leakage sensor has no
settings; its binary indicator continues to display the raw digital state.
Historical display converts pressure using the stored column unit and does not
retroactively apply today's offsets. CSV export retains the backend's source units.

The Filter graphic turns red above its enabled upper limit and orange below its
enabled lower limit, otherwise gray. It compares the unrounded corrected kPa
difference; equality is normal and display-unit changes do not affect the result.

This repository supplies the Proxy/UI: settings are in-memory values available
to the backend. Backend alarm generation, hardware calibration, and persistent
storage are not implemented here. A backend consuming the thresholds should use
corrected values and honor each enabled flag.
