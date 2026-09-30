#pragma once
// w2-080 (desktop only, never compiled into WebAssembly): the ONE conversion of an alarm_history
// row (id, occurrence_time, reason) into an alarm record of the UI contract, shared by
//   * Core::loadAlarmRecords  -> TaidaFlowProxy::alarmRecords (last 3 days, unchanged), and
//   * AlarmViewService        -> TaidaFlowProxy::alarmViews[sessionId].rows (+ serialNumber).
// Moved unchanged out of core.cpp (the vocabulary table, alarmUiFieldsForStatus and the per-row
// code of the former loadAlarmRecords loop), so both properties always show the same fields.
//
// Alarm vocabulary: Core status (stored in alarm.reason JSON 'status') ->
// UI contract fields of Td.alarmRecords (AlarmPage.qml counts and colours
// alarmStatus === "未處理"; severity === "嚴重" is red, anything else amber).
//
//  Core status   | produced by                                  | alarmStatus | severity
//  --------------+----------------------------------------------+-------------+---------
//  異常          | DI0 相位異常 / DI1 漏液檢出 / DI2 補水泵 OL    | 未處理      | 嚴重
//                | (Manager::checkDigitalInputAlarm), MS300     |             |
//                | fault code != 0 (Ms300FaultReader)           |             |
//  警告          | MS300 warning code only (fault code == 0)    | 未處理      | 警告
//  數值異常      | AI >= high limit (Manager::checkHighInputAlarm) | 未處理   | 警告
//  正常          | 設備啟動 (Core::init) - informational record  | 已解除      | 警告
//  未處理/已解除 | already UI vocabulary (pass-through)         | same        | 警告
//  (missing)     | legacy plain-text reason / JSON w/o status   | 未處理      | 警告
//  anything else | unknown                                      | 未處理      | 警告
//
// 設備啟動 is not a fault: 已解除 keeps it out of the page's "未處理" count and
// shows it green; the UI has no info level, so it takes the lower level 警告.
// Unknown or missing statuses default to 未處理 so that nothing that might
// need attention is hidden.  A valid 'severity' stored in the JSON (嚴重/警告)
// overrides the table (no producer writes one today).  The database is not
// migrated: the conversion runs on every read, so existing rows are covered.
//
// w2-037: a DI alarm row (DI0/DI1/DI2) is updated in place when the input
// returns to normal: Manager keeps 'status' (異常) and adds "resolved": true,
// "resolvedAt" (epoch s) and "resolvedDetail" (e.g. 漏液檢出 解除（DI1=0）).
// A row with resolved == true reads as alarmStatus 已解除; its severity still
// comes from 'status', so a resolved 異常 row stays 嚴重.
//
// Pure functions without shared state: safe on any thread (AlarmViewService runs isUnhandled()
// on the SqlManager thread to count activeCount).
#include <QJsonArray>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>

namespace AlarmRecordFormat {

struct UiFields
{
    QString alarmStatus;
    QString severity;
    bool known = true;
};

UiFields uiFieldsForStatus(const QString &coreStatus, bool hasStatus, bool isResolved);

// One stored row, decoded.
struct Decoded
{
    QString sensor;          // JSON 'sensor', "—" when missing
    QString alarmMessage;    // JSON 'alarmMessage' / 'message', or the plain-text reason
    QString coreStatus;      // JSON 'status' ("" when missing)
    bool hasStatus = false;
    bool isResolved = false;
    UiFields ui;             // alarmStatus / severity of the UI contract
};
Decoded decodeReason(const QString &storedReason);

// The alarm record map of the UI contract (the element type of alarmRecords):
// id, timestampMs, alarmTime ("yyyy/MM/dd HH:mm"), equipment, sensorName, alarmMessage,
// severity, alarmStatus.
QVariantMap recordFromRow(qint64 id, qint64 occurrenceSec, const Decoded &decoded);
QVariantMap recordFromRow(qint64 id, qint64 occurrenceSec, const QString &storedReason);

// true when the row reads as alarmStatus "未處理" (the rows counted by activeCount).
bool isUnhandled(const QString &storedReason);

// Core::loadAlarmRecords: SqlManager::getAlarmHistory rows (oldest first) -> alarmRecords
// (newest first), with the per-row log lines of the newest kLoggedAlarmRows rows and counts.
struct LoadStats
{
    int unhandledCount = 0;
    int resolvedCount = 0;
    int criticalCount = 0;
    int warningCount = 0;
    int unknownStatusCount = 0;
};
QVariantList recordsFromHistory(const QJsonArray &historyOldestFirst, LoadStats *stats);

} // namespace AlarmRecordFormat
