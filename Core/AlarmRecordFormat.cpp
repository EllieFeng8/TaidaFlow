#include "AlarmRecordFormat.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QtDebug>

namespace AlarmRecordFormat {

UiFields uiFieldsForStatus(const QString &coreStatus, bool hasStatus, bool isResolved)
{
    const QString unhandled = QStringLiteral("未處理");
    const QString resolved = QStringLiteral("已解除");
    const QString critical = QStringLiteral("嚴重");
    const QString warning = QStringLiteral("警告");

    UiFields fields{unhandled, warning, false};
    if (!hasStatus)
        fields = {unhandled, warning, true};
    else if (coreStatus == QStringLiteral("異常"))
        fields = {unhandled, critical, true};
    else if (coreStatus == QStringLiteral("警告") || coreStatus == QStringLiteral("數值異常"))
        fields = {unhandled, warning, true};
    else if (coreStatus == QStringLiteral("正常"))
        fields = {resolved, warning, true};
    else if (coreStatus == unhandled || coreStatus == resolved)
        fields = {coreStatus, warning, true};
    if (isResolved)
        fields.alarmStatus = resolved;
    return fields;
}

Decoded decodeReason(const QString &storedReason)
{
    QJsonParseError parseError;
    const QJsonDocument reasonDocument = QJsonDocument::fromJson(storedReason.toUtf8(), &parseError);
    const QJsonObject reasonObject = parseError.error == QJsonParseError::NoError
            && reasonDocument.isObject()
            ? reasonDocument.object()
            : QJsonObject();

    // reason remains a QString in SqlManager.  New records carry JSON;
    // legacy plain text is kept as the warning message with defaults.
    Decoded d;
    d.sensor = reasonObject.value(QStringLiteral("sensor")).toString(QStringLiteral("—"));
    d.alarmMessage = reasonObject.contains(QStringLiteral("alarmMessage"))
            ? reasonObject.value(QStringLiteral("alarmMessage")).toString()
            : reasonObject.value(QStringLiteral("message")).toString(storedReason);
    d.hasStatus = reasonObject.value(QStringLiteral("status")).isString();
    d.coreStatus = reasonObject.value(QStringLiteral("status")).toString();
    d.isResolved = reasonObject.value(QStringLiteral("resolved")).toBool(false);
    d.ui = uiFieldsForStatus(d.coreStatus, d.hasStatus, d.isResolved);
    const QString storedSeverity = reasonObject.value(QStringLiteral("severity")).toString();
    if (storedSeverity == QStringLiteral("嚴重") || storedSeverity == QStringLiteral("警告"))
        d.ui.severity = storedSeverity;
    return d;
}

QVariantMap recordFromRow(qint64 id, qint64 occurrenceSec, const Decoded &decoded)
{
    return QVariantMap{
        {QStringLiteral("id"), id},
        {QStringLiteral("timestampMs"), occurrenceSec * 1000},
        {QStringLiteral("alarmTime"), QDateTime::fromSecsSinceEpoch(occurrenceSec)
                 .toString(QStringLiteral("yyyy/MM/dd HH:mm"))},
        {QStringLiteral("equipment"), QStringLiteral("系統")},
        {QStringLiteral("sensorName"), decoded.sensor},
        {QStringLiteral("alarmMessage"), decoded.alarmMessage},
        {QStringLiteral("severity"), decoded.ui.severity},
        {QStringLiteral("alarmStatus"), decoded.ui.alarmStatus},
    };
}

QVariantMap recordFromRow(qint64 id, qint64 occurrenceSec, const QString &storedReason)
{
    return recordFromRow(id, occurrenceSec, decodeReason(storedReason));
}

bool isUnhandled(const QString &storedReason)
{
    return decodeReason(storedReason).ui.alarmStatus == QStringLiteral("未處理");
}

QVariantList recordsFromHistory(const QJsonArray &history, LoadStats *stats)
{
    LoadStats local;
    LoadStats &s = stats ? *stats : local;
    s = LoadStats{};
    QVariantList records;
    records.reserve(history.size());
    // Only the newest rows are logged field by field, the rest are counted.
    constexpr qsizetype kLoggedAlarmRows = 8;
    // SqlManager returns oldest first. AlarmPage expects newest first when it
    // constructs its default date range and its "show all" range.
    for (qsizetype index = history.size(); index > 0; --index) {
        const QJsonObject alarm = history.at(index - 1).toObject();
        const qint64 occurrence = static_cast<qint64>(
                alarm.value(QStringLiteral("occurrence_time")).toDouble());
        const QString storedReason = alarm.value(QStringLiteral("reason")).toString();
        const Decoded d = decodeReason(storedReason);
        if (!d.ui.known)
            ++s.unknownStatusCount;
        if (d.ui.alarmStatus == QStringLiteral("未處理"))
            ++s.unhandledCount;
        else
            ++s.resolvedCount;
        if (d.ui.severity == QStringLiteral("嚴重"))
            ++s.criticalCount;
        else
            ++s.warningCount;

        const qint64 alarmId = static_cast<qint64>(alarm.value(QStringLiteral("id")).toDouble());
        if (records.size() < kLoggedAlarmRows) {
            qInfo().noquote()
                    << QStringLiteral("[Alarm][UI] id=%1 sensor=%2 message=%3 coreStatus=%4%5 -> alarmStatus=%6 severity=%7")
                               .arg(alarmId)
                               .arg(d.sensor, d.alarmMessage,
                                    d.hasStatus ? d.coreStatus : QStringLiteral("(none)"),
                                    d.isResolved ? QStringLiteral(" resolved=true") : QString(),
                                    d.ui.alarmStatus, d.ui.severity);
        }
        records.append(recordFromRow(alarmId, occurrence, d));
    }
    return records;
}

} // namespace AlarmRecordFormat
