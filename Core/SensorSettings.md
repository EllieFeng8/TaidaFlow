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

## Co-located sensor groups

Sensors at the same position share one pair of limits on the settings page:
outlet pressure PT-04/PT-05, outlet temperature TT-01/TT-02, inlet pressure
PT-06/PT-07, inlet temperature TT-03/TT-04. Each group is one row with a single
lower/upper pair and one Offset field per sensor. Applying the row writes
identical `lower`, `upper`, `lowerEnabled`, and `upperEnabled` values to every
key of the group, while each key keeps its own `offset`; all keys are written in
one setter call using the same copy-and-replace flow and validation as above.
If the stored limits of a group differ (older data or another writer), the row
shows the first sensor's limits with the hint 「組內上下限不一致，套用後將統一」;
the next apply unifies them. Only effective limits are compared: the enabled
flags, and the values of enabled limits. The contract is unchanged: the map
still has one entry per sensor, the Proxy still validates each entry on its own,
and the backend keeps reading limits per sensor key.

The UI displays raw PV + offset; the backend must keep PVs raw to avoid applying
the offset twice. Filter pressure difference is corrected PT-02 minus corrected
PT-03; filter has no independent offset in the UI. The leakage sensor has no
settings; its binary indicator continues to display the raw digital state.
Offsets are applied when a sample is stored (Mango 2026-10-02, core branch desktop
backend, w2-086, `Core/SensorOffsetStorage.{h,cpp}`; scope table `Core/SensorOffset.md`):
before each `sensor_data` row is written, the offset in effect at that moment is
converted to counts with the column's scale (`Core/ModbusMapping.h`), added to the raw
count, rounded and clamped to 0..65535. The History page, CSV export and REST therefore
show corrected values (History/CSV in the backend's source units: pressure kPa,
temperature °C, flow L/min), equal to the screen value raw PV + offset within half a
count. A changed offset applies only to samples stored afterwards; rows already stored
are never rewritten, and the original raw value of a corrected row cannot be recovered.
The Proxy PVs stay raw (the UI adds the offset itself). The original 90 % high-range
alarm judges raw values. The Modbus server PVs for external HMIs (input registers
0..15, ADAM-6217 mirror) carry the same correction as the database (Mango 2026-10-02);
the offset/limit settings themselves stay in HR11..40 unchanged.

The Filter graphic turns red above its enabled upper limit and orange below its
enabled lower limit, otherwise gray. It compares the unrounded corrected kPa
difference; equality is normal and display-unit changes do not affect the result.

This repository supplies the Proxy/UI: settings are in-memory values available
to the backend. Backend alarm generation, hardware calibration, and persistent
storage are not implemented here. A backend consuming the thresholds should use
corrected values and honor each enabled flag.

## 後端超限警報規則(core 分支,w2-085)

core 分支的桌面後端依本契約產生警報(`Core/LimitAlarms.{h,cpp}` 的 `LimitAlarmMonitor`,由 `Manager` 建立;
上一段「Backend alarm generation … not implemented here」指的是 main 分支只提供 Proxy/UI)。規則:

- 判斷對象:`pt01`–`pt07`、`tt01`–`tt04`、`flowMeter`、`filter` 共 13 個 key,**每個 key 各自判斷、各自記錄**
  (同組 PT-04/PT-05 等雖在設定頁共用一組上下限,仍是兩支感測器、兩筆各自的警報)。
- 判斷值 = 校正後數值 = 原始 PV + 該 key 的 `offset`(PV 本身保持原始值);`filter` = 校正後 PT-02 − 校正後 PT-03,
  用 `filter` 的上下限。壓力與上下限一律以 kPa 比較,與顯示單位 `pressureUnitSv` 無關;溫度 °C、流量 L/min。
- 超上限:`upperEnabled && 值 > upper`;低於下限:`lowerEnabled && 值 < lower`。**等於上下限屬正常**;
  停用的上下限不判斷。與主畫面 Filter 變色(`Main.qml` `filterBody.limitState`)同一個運算式
  (`Core/tests/tst_limit_alarms` 直接執行 UI 的 JavaScript 逐一比對)。
- 上限與下限是同一感測器的兩種狀態,各自新增、各自解除。超限時**立即**新增一筆警報(alarm_history,
  `status` = 「警告」→ 警報頁嚴重程度「警告」、狀態「未處理」);未解除前同一感測器同一方向不重複新增。
- 回到範圍內(含上下限被停用)**持續 2 秒**(單調時鐘、計時器,不受 PV 更新頻率影響)才把**同一筆**改為已解除
  (`resolved` / `resolvedAt` / `resolvedDetail`,與漏水 DI1 相同);2 秒內又超限 → 不解除、不新增。
- `sensorSettingsSv` 變動(含停用)時立即以新設定重新評估所有已有讀值的感測器。
- 一支感測器在本次執行中第一次收到後端寫入的 PV 之後才判斷(`filter` 需 PT-02 與 PT-03 都有);同一個
  Modbus 回覆裡的多個 PV 寫完後才一起判斷,所以 `filter` 不會用到「新 PT-02 + 舊 PT-03」。
- 警報文字:`超過上限：<值> <單位>（上限 <上限> <單位>）`、`低於下限：<值> <單位>（下限 <下限> <單位>）`,
  數值兩位小數;`sensorName` 用畫面上的名稱:`PT-01`…`PT-07`、`TT-01`…`TT-04`、`流量計`、`Filter 壓差`。
  reason JSON 另存 `"limit": "upper"|"lower"` 與 `"sensorKey"`(供重啟辨識;`alarmRecords` 欄位不變)。
- 重啟(比照 w2-053 的 DI):每支感測器第一次判斷時查本月與上月資料檔中該名稱、`status` 為「警告」且未解除的列;
  每個方向最新的一筆接手為進行中的警報(仍超限 → 不新增;已恢復 → 2 秒後解除),較舊的重複列立即解除。
  查詢失敗時該感測器不新增警報、下次更新重試(其他感測器在失敗後 500 ms 內延後);所有感測器共用 5 次失敗,
  用完後尚未查過的感測器都回到一般邏輯(資料檔被鎖住時最多 5 次等待,不會讓畫面卡住數分鐘)。
- 不影響:原本「量程 90% 高限」警報(`數值異常`,用原始 raw 值)、DI/漏水/MS300 警報、設定儲存與 Modbus server
  HR 寫入行為都不變。
