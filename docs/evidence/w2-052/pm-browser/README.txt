PM 瀏覽器雙分頁實測(2026-09-28,in-app 瀏覽器;截圖於對話中檢視,canvas 無法存檔)
- 11:43 啟動:安全探測 SAFE、simulator profile、build\runtime-cwd、AppHttpServer 8124。
- 分頁 A(http://127.0.0.1:8124,session web-a5b7):歷史頁預設 2026/09/01 00:00–09/30 23:59,下一頁 ×2 → 第 3/6119 頁。
- 分頁 B 以區網 IP(http://192.168.0.125:8124)開啟:網頁載入,但 mirror 未連線(紅色「離線」橫幅;app log 無來自 192.168.0.125 的 LAN relay 連線)。
  in-app 瀏覽器在此步驟卡住約 3 小時(疑為非本機網站授權),原因未確認;區網路徑已由 w2-052 mirror 測試客戶端與 w2-042/043 驗證,真實第二台電腦待 Mango 實測。
- 分頁 B 改以 http://127.0.0.1:8124(session web-8845):顯示前一周 → 2026/09/22 00:00–09/28 23:59,下一頁 → 第 2/7322 頁。
- 回看分頁 A:仍為 09/01–09/30、第 3/6119 頁、表格內容不變 → 各端獨立確認。
- 分頁 A 的 session 在閒置 30 分鐘後被移除(log「removed 1 session(s) (idle): web-a5b7」,11:43 → 15:05),畫面保留最後內容,符合設計。
- 另發現(main UI):主畫面 M2/M4 的 PV 顯示未格式化(例:PV:30.012970168612192%)。
- 結束:關閉兩個分頁、CloseMainWindow 關 app(110812)與模擬器(122656),無殘留 listener;完整 app.log(51 MB)未保留,摘錄於 app-history-excerpt.log。
