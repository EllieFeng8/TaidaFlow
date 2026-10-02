# Sensor Offset 套用範圍

> 決定日期:2026-10-02(Mango)。設定頁每支感測器的 Offset(`sensorSettingsSv[id].offset`,
> 工程單位:壓力 kPa、溫度 °C、流量 L/min)套用在哪些地方,以本表為準。
> 契約與欄位見同資料夾的 `SensorSettings.md`。

## 決定

採用「**存檔時就加上 offset**」:資料庫、歷史頁、CSV、REST 全部一致。

接受的缺點:

- 改 offset 之後,新舊資料的基準不同(歷史曲線會在改設定的時間點出現跳動)。
- 存進資料庫的是校正後的值,原始量測值找不回來。
- 已經存在資料庫的舊資料**不改寫**。

## 套用範圍

| 地方 | 套用 offset | 說明 |
|---|---|---|
| 主畫面數值(PT-01~07、TT-01~04) | ✅ | 畫面自己加:原始值 + offset(`TaidaFlowContent/components/SensorUnits.js` 的 `adjusted()` / `display()`) |
| 主畫面 Filter 壓差 | ✅ | PT-02、PT-03 各自加 offset 後再相減 |
| 主畫面流量計 | ✅ | |
| 主畫面超限變色 | ✅ | 用加上 offset 後的值判斷 |
| 資料庫儲存 | ✅ | 存檔前把 offset 依該欄位縮放比例換算成計數值加上去,四捨五入,超出 0~65535 夾到邊界 |
| 歷史頁 | ✅ | 讀資料庫,自動一致 |
| CSV 匯出 | ✅ | 讀資料庫,自動一致 |
| REST API(`/api/sensor/...`) | ✅ | 讀資料庫,自動一致 |
| 超限警報(設定頁上下限) | ✅ | 用加上 offset 後的值判斷 |
| Proxy 上的 PV(程式內部、網頁同步) | ❌ 原始值 | 畫面自己加 offset;PV 維持原始值才不會重複加兩次 |
| Modbus server 給外部 HMI 的 PV | ✅ | Mango 2026-10-02 改為套用:input register 0~15(ADAM-6217 AI 鏡像)與資料庫同一個換算(offset 換成計數值加上去,四捨五入,夾在 0~65535);MV1~4 位置、DI/DO/線圈不變;設定頁的 offset 與上下限設定值本身仍另外寫在 HR11~40(holding register,原樣) |
| 原本的「量程 90% 高限」警報 | ❌ 原始值 | 保護硬體量程用 |
| 已經存在資料庫的舊資料 | ❌ 不改寫 | |

## 實作狀態(2026-10-02)

- 已生效:主畫面數值、Filter 壓差、流量計、主畫面超限變色。
- 已生效(core 分支):
  - 超限警報(超過上限或低於下限立即記錄、嚴重程度「警告」,回到範圍內持續 2 秒才改為已解除):**已生效**(w2-085,2026-10-02)。
  - 資料庫存檔時套用 offset,歷史頁、CSV、REST 隨之一致;Modbus server 給外部 HMI 的 PV 同樣套用:**已生效**(w2-086,2026-10-02)。

### 存檔時套用的做法(w2-086)

- 程式:`Core/SensorOffsetStorage.{h,cpp}`(新檔,只編進桌面版),`Manager` 只做接線:每秒存檔
  (`saveServerInputData`)前換算整筆資料,鏡像到 Modbus server 的 input register 時換算該暫存器。
- 換算:`存入值 = round(原始計數 + offset ÷ 縮放比例)`,夾在 0~65535(夾到邊界時記 warning,每欄每 60 秒最多一行,
  下一行會附上期間被略過的次數)。四捨五入 = 0.5 進位(遠離 0)。
- 每一筆都用**當下**的設定:設定頁套用後,下一筆存檔(≤ 1 秒)與下一次 Modbus server 更新就用新的 offset。
  offset 為 0 或沒有設定 → 存原始計數。
- 縮放比例只有一個來源:`Core/ModbusMapping.h` 的 `defaultReadBindings()`(即時顯示用的那一份);歷史頁
  (`HistoryViews.cpp`)與 CSV(`HistoryExport.cpp`)的換算比例與它相同(`Core/tests/tst_offset_storage` 檢查)。

| 設定 key | 資料庫欄位 / Modbus server input register | 縮放比例(每個計數) | 單位 |
|---|---|---|---|
| tt01~tt04 | s1~s4 / IR0~IR3 | 100 ÷ 65535 ≈ 0.001526 | °C |
| pt01~pt04 | s5~s8 / IR4~IR7 | 1000 ÷ 65535 ≈ 0.015259 | kPa |
| pt05~pt07 | s9~s11 / IR8~IR10 | 1000 ÷ 65535 ≈ 0.015259 | kPa |
| flowMeter | s12 / IR11 | 1 | L/min |
| (無,MV1~4 開度回授) | s13~s16 / IR12~IR15 | 100 ÷ 65535 | % — 不套用 |
| filter | 沒有自己的欄位 | — | 由 PT-02、PT-03 各自校正後相減,自動一致 |

- 一致性:歷史頁 / CSV / REST(× 縮放比例)= 主畫面的「原始值 + offset」,誤差不超過半個計數
  (壓力 ≤ 0.0076 kPa、溫度 ≤ 0.00076 °C、流量 ≤ 0.5 L/min)。**流量計目前 1 個計數 = 1 L/min**(傳送器量程尚未提供,
  ModbusMapping.h 暫用原始值),所以流量的 offset 存檔時會四捨五入到整數 L/min(例如 offset 3.4 → 存 +3)。
- 夾到邊界時(例如原始值已接近滿刻度又加正 offset)存入值就是 0 或 65535,與主畫面顯示值不同;log 有 warning。
