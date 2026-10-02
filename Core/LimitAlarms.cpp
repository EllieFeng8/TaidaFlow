#include "LimitAlarms.h"

#include "ModbusMapping.h"
#include "SqlManager.h"
#include "TaidaFlowProxy.h"

#include <QDate>
#include <QDebug>
#include <QJsonDocument>
#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr auto kFilterKey = "filter";

struct SensorInfo
{
    const char *key;
    const char *name;   // UTF-8, the name on the screen (alarm 'sensor' field)
    const char *unit;   // UTF-8
};

// Settings page order (Core/SensorSettings.md).  Pressures and filter in kPa, temperatures in degC,
// flow in L/min (the base units of sensorSettingsSv).
const SensorInfo kSensors[] = {
    {"pt01", "PT-01", "kPa"}, {"pt02", "PT-02", "kPa"}, {"pt03", "PT-03", "kPa"},
    {"pt04", "PT-04", "kPa"}, {"pt05", "PT-05", "kPa"}, {"pt06", "PT-06", "kPa"},
    {"pt07", "PT-07", "kPa"},
    {"tt01", "TT-01", "°C"}, {"tt02", "TT-02", "°C"}, {"tt03", "TT-03", "°C"},
    {"tt04", "TT-04", "°C"},
    {"flowMeter", "流量計", "L/min"},
    {"filter", "Filter 壓差", "kPa"},
};

const SensorInfo *sensorInfo(const QString &key)
{
    for (const SensorInfo &info : kSensors) {
        if (key == QLatin1String(info.key))
            return &info;
    }
    return nullptr;
}

bool rawValue(const TaidaFlowProxy &proxy, const QString &key, double *value)
{
    using Getter = double (TaidaFlowProxy::*)() const;
    struct Pv { const char *key; Getter get; };
    static const Pv pvs[] = {
        {"pt01", &TaidaFlowProxy::pt01ValuePv}, {"pt02", &TaidaFlowProxy::pt02ValuePv},
        {"pt03", &TaidaFlowProxy::pt03ValuePv}, {"pt04", &TaidaFlowProxy::pt04ValuePv},
        {"pt05", &TaidaFlowProxy::pt05ValuePv}, {"pt06", &TaidaFlowProxy::pt06ValuePv},
        {"pt07", &TaidaFlowProxy::pt07ValuePv},
        {"tt01", &TaidaFlowProxy::tt01ValuePv}, {"tt02", &TaidaFlowProxy::tt02ValuePv},
        {"tt03", &TaidaFlowProxy::tt03ValuePv}, {"tt04", &TaidaFlowProxy::tt04ValuePv},
        {"flowMeter", &TaidaFlowProxy::flowMeterValuePv},
    };
    for (const Pv &pv : pvs) {
        if (key == QLatin1String(pv.key)) {
            *value = (proxy.*pv.get)();
            return true;
        }
    }
    return false;
}

const char *limitKey(LimitAlarmMonitor::Limit limit)
{
    return limit == LimitAlarmMonitor::Limit::Upper ? "upper" : "lower";
}

QString limitTitle(LimitAlarmMonitor::Limit limit)
{
    return limit == LimitAlarmMonitor::Limit::Upper ? QStringLiteral("超過上限") : QStringLiteral("低於下限");
}

QString limitWord(LimitAlarmMonitor::Limit limit)
{
    return limit == LimitAlarmMonitor::Limit::Upper ? QStringLiteral("上限") : QStringLiteral("下限");
}

QString number(double value)
{
    return QString::number(value, 'f', 2);
}

QString timeText(const QDateTime &time)
{
    return time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}
} // namespace

LimitAlarmMonitor::LimitAlarmMonitor(TaidaFlowProxy *proxy, SqlManager *sql, QObject *parent)
    : LimitAlarmMonitor(proxy, sql, Options{}, parent)
{
}

LimitAlarmMonitor::LimitAlarmMonitor(TaidaFlowProxy *proxy, SqlManager *sql, const Options &options,
                                     QObject *parent)
    : QObject(parent)
    , m_proxy(proxy)
    , m_sql(sql)
    , m_options(options)
{
    m_elapsed.start();
    for (const SensorInfo &info : kSensors) {
        Check check;
        check.key = QString::fromUtf8(info.key);
        check.name = QString::fromUtf8(info.name);
        check.unit = QString::fromUtf8(info.unit);
        m_checks.append(check);
    }
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &LimitAlarmMonitor::processDue);
    if (m_proxy)
        connect(m_proxy, &TaidaFlowProxy::sensorSettingsSvChanged, this, &LimitAlarmMonitor::reevaluate);
    qInfo().noquote()
            << QStringLiteral("[LimitAlarm] settings-page limit alarms active for %1 sensors (status %2, "
                              "resolved after %3 ms back in range).")
                       .arg(m_checks.size())
                       .arg(QString::fromUtf8(kCoreStatus))
                       .arg(m_options.resolveAfterMs);
}

LimitAlarmMonitor::~LimitAlarmMonitor() = default;

// ---- rules ----------------------------------------------------------------------------------

QStringList LimitAlarmMonitor::sensorKeys()
{
    QStringList keys;
    for (const SensorInfo &info : kSensors)
        keys.append(QString::fromUtf8(info.key));
    return keys;
}

QString LimitAlarmMonitor::sensorName(const QString &key)
{
    const SensorInfo *info = sensorInfo(key);
    return info ? QString::fromUtf8(info->name) : QString();
}

QString LimitAlarmMonitor::unitOf(const QString &key)
{
    const SensorInfo *info = sensorInfo(key);
    return info ? QString::fromUtf8(info->unit) : QString();
}

bool LimitAlarmMonitor::correctedValue(const TaidaFlowProxy &proxy, const QString &key, double *value)
{
    const QVariantMap settings = proxy.sensorSettingsSv();
    const auto corrected = [&settings, &proxy](const QString &k, double *out) {
        double raw = 0.0;
        if (!rawValue(proxy, k, &raw))
            return false;
        // The UI's SensorUnits.adjusted(): raw + (settings[id] ? settings[id].offset : 0).
        *out = raw + settings.value(k).toMap().value(QStringLiteral("offset")).toDouble();
        return true;
    };
    double result = 0.0;
    if (key == QLatin1String(kFilterKey)) {
        double pt02 = 0.0;
        double pt03 = 0.0;
        if (!corrected(QStringLiteral("pt02"), &pt02) || !corrected(QStringLiteral("pt03"), &pt03))
            return false;
        result = pt02 - pt03;
    } else if (!corrected(key, &result)) {
        return false;
    }
    if (!std::isfinite(result))
        return false;
    *value = result;
    return true;
}

bool LimitAlarmMonitor::isAboveUpper(double value, const QVariantMap &entry)
{
    return entry.value(QStringLiteral("upperEnabled")).toBool()
            && value > entry.value(QStringLiteral("upper")).toDouble();
}

bool LimitAlarmMonitor::isBelowLower(double value, const QVariantMap &entry)
{
    return entry.value(QStringLiteral("lowerEnabled")).toBool()
            && value < entry.value(QStringLiteral("lower")).toDouble();
}

int LimitAlarmMonitor::limitState(double value, const QVariantMap &entry)
{
    // Same order as the UI rule SensorUnits.limitState() (TaidaFlowContent/components/SensorUnits.js,
    // used by the Filter graphic and, since w1-088, every main-page value): upper first, then lower.
    return isAboveUpper(value, entry) ? 1 : isBelowLower(value, entry) ? -1 : 0;
}

QString LimitAlarmMonitor::alarmMessage(Limit limit, double value, double limitValue, const QString &unit)
{
    // e.g. 超過上限：12.34 kPa（上限 10.00 kPa）
    return QStringLiteral("%1：%2 %3（%4 %5 %3）")
            .arg(limitTitle(limit), number(value), unit, limitWord(limit), number(limitValue));
}

// ---- updates ---------------------------------------------------------------------------------

void LimitAlarmMonitor::processPointUpdated(ModbusMapping::ProcessPoint point)
{
    switch (point) {
    case ModbusMapping::ProcessPoint::Tt01: valueUpdated(QStringLiteral("tt01")); break;
    case ModbusMapping::ProcessPoint::Tt02: valueUpdated(QStringLiteral("tt02")); break;
    case ModbusMapping::ProcessPoint::Tt03: valueUpdated(QStringLiteral("tt03")); break;
    case ModbusMapping::ProcessPoint::Tt04: valueUpdated(QStringLiteral("tt04")); break;
    case ModbusMapping::ProcessPoint::Pt01: valueUpdated(QStringLiteral("pt01")); break;
    case ModbusMapping::ProcessPoint::Pt02: valueUpdated(QStringLiteral("pt02")); break;
    case ModbusMapping::ProcessPoint::Pt03: valueUpdated(QStringLiteral("pt03")); break;
    case ModbusMapping::ProcessPoint::Pt04: valueUpdated(QStringLiteral("pt04")); break;
    case ModbusMapping::ProcessPoint::Pt05: valueUpdated(QStringLiteral("pt05")); break;
    case ModbusMapping::ProcessPoint::Pt06: valueUpdated(QStringLiteral("pt06")); break;
    case ModbusMapping::ProcessPoint::Pt07: valueUpdated(QStringLiteral("pt07")); break;
    case ModbusMapping::ProcessPoint::FlowMeter: valueUpdated(QStringLiteral("flowMeter")); break;
    case ModbusMapping::ProcessPoint::Mv1Position:
    case ModbusMapping::ProcessPoint::Mv2Position:
    case ModbusMapping::ProcessPoint::Mv3Position:
    case ModbusMapping::ProcessPoint::Mv4Position:
        break;
    }
}

void LimitAlarmMonitor::valueUpdated(const QString &sensorKey)
{
    if (sensorKey == QLatin1String(kFilterKey) || !sensorInfo(sensorKey))
        return;
    m_known.insert(sensorKey);
    m_dirty.insert(sensorKey);
    if (sensorKey == QLatin1String("pt02") || sensorKey == QLatin1String("pt03"))
        m_dirty.insert(QString::fromLatin1(kFilterKey));
    if (!m_flushQueued) {
        // All PVs of one Modbus reply are written in one loop (Manager's registersRead handler);
        // evaluating after it keeps PT-02 / PT-03 of the filter from the same reply.
        m_flushQueued = true;
        QMetaObject::invokeMethod(this, &LimitAlarmMonitor::flushPendingUpdates, Qt::QueuedConnection);
    }
}

void LimitAlarmMonitor::flushPendingUpdates()
{
    m_flushQueued = false;
    if (m_dirty.isEmpty())
        return;
    const QSet<QString> dirty = std::exchange(m_dirty, {});
    for (Check &check : m_checks) {
        if (dirty.contains(check.key))
            evaluate(check);
    }
    scheduleTimer();
}

void LimitAlarmMonitor::reevaluate()
{
    m_dirty.clear();
    QStringList known;
    for (const Check &check : std::as_const(m_checks)) {
        if (valueKnown(check.key))
            known.append(check.key);
    }
    qInfo().noquote()
            << QStringLiteral("[LimitAlarm] sensor settings changed: re-evaluating %1 sensor(s) with a value (%2).")
                       .arg(known.size())
                       .arg(known.isEmpty() ? QStringLiteral("none yet") : known.join(QLatin1Char(',')));
    for (Check &check : m_checks)
        evaluate(check);
    scheduleTimer();
}

void LimitAlarmMonitor::processDue()
{
    m_retryPending = false;
    for (Check &check : m_checks)
        evaluate(check);
    scheduleTimer();
}

bool LimitAlarmMonitor::valueKnown(const QString &key) const
{
    if (key == QLatin1String(kFilterKey))
        return m_known.contains(QStringLiteral("pt02")) && m_known.contains(QStringLiteral("pt03"));
    return m_known.contains(key);
}

qint64 LimitAlarmMonitor::now() const
{
    return m_options.clock ? m_options.clock() : m_elapsed.elapsed();
}

void LimitAlarmMonitor::evaluate(Check &check)
{
    if (!m_proxy || !valueKnown(check.key))
        return;
    double value = 0.0;
    if (!correctedValue(*m_proxy, check.key, &value))
        return;

    const QVariantMap entry = m_proxy->sensorSettingsSv().value(check.key).toMap();
    const bool above = isAboveUpper(value, entry);
    const bool below = isBelowLower(value, entry);

    if (!check.restartChecked && !takeOverRestart(check, value, above, below))
        return;     // lookup failed or deferred: no new row for this sensor until it succeeded
    resolvePending(check);

    updateSide(check, Limit::Upper, above, value,
               entry.value(QStringLiteral("upperEnabled")).toBool(),
               entry.value(QStringLiteral("upper")).toDouble());
    updateSide(check, Limit::Lower, below, value,
               entry.value(QStringLiteral("lowerEnabled")).toBool(),
               entry.value(QStringLiteral("lower")).toDouble());
}

void LimitAlarmMonitor::updateSide(Check &check, Limit limit, bool violated, double value, bool enabled,
                                   double limitValue)
{
    Side &side = check.side[static_cast<int>(limit)];
    if (violated) {
        if (side.active) {
            if (side.normalSinceMs >= 0) {
                qInfo().noquote()
                        << QStringLiteral("[LimitAlarm] %1 %2 again (%3 %4, %5 %6 %4) %7 ms after it was back in "
                                          "range: id=%8 stays unresolved, no new row.")
                                   .arg(check.name, limitTitle(limit), number(value), check.unit,
                                        limitWord(limit), number(limitValue))
                                   .arg(now() - side.normalSinceMs)
                                   .arg(side.row.id);
                side.normalSinceMs = -1;
            }
            return;
        }
        Row row;
        if (insertRow(check, limit, alarmMessage(limit, value, limitValue, check.unit), &row)) {
            side.active = true;
            side.row = row;
            side.normalSinceMs = -1;
        } else {
            m_retryPending = true;      // retried on the next update or by the timer
        }
        return;
    }

    if (!side.active)
        return;
    const qint64 t = now();
    if (side.normalSinceMs < 0) {
        side.normalSinceMs = t;
        qInfo().noquote()
                << QStringLiteral("[LimitAlarm] %1 %2 back in range (%3 %4; %5): id=%6 is resolved in %7 ms "
                                  "unless it leaves the range again.")
                           .arg(check.name, limitTitle(limit), number(value), check.unit,
                                enabled ? QStringLiteral("%1 %2 %3").arg(limitWord(limit), number(limitValue), check.unit)
                                        : QStringLiteral("%1 disabled").arg(limitWord(limit)))
                           .arg(side.row.id)
                           .arg(m_options.resolveAfterMs);
    }
    if (t - side.normalSinceMs < m_options.resolveAfterMs)
        return;     // the timer comes back at the deadline

    const QString detail = enabled
            ? QStringLiteral("%1 解除（%2=%3 %4，%5 %6 %4，回到範圍內 %7 秒）")
                      .arg(limitTitle(limit), check.name, number(value), check.unit, limitWord(limit),
                           number(limitValue))
                      .arg(m_options.resolveAfterMs / 1000.0)
            : QStringLiteral("%1 解除（%2已停用，%3=%4 %5）")
                      .arg(limitTitle(limit), limitWord(limit), check.name, number(value), check.unit);
    if (markResolved(side.row, detail)) {
        side = Side{};
    } else {
        m_retryPending = true;          // the row stays tracked; retried after retryAfterMs
    }
}

// ---- restart (w2-053 pattern) ----------------------------------------------------------------

bool LimitAlarmMonitor::takeOverRestart(Check &check, double value, bool above, bool below)
{
    if (!m_sql) {
        check.restartChecked = true;
        qWarning().noquote()
                << QStringLiteral("[LimitAlarm][Restart] %1: SqlManager is unavailable; rows of earlier runs are not checked.")
                           .arg(check.name);
        return true;
    }

    if (m_restartLookupGivenUp) {
        // The attempts are shared by all sensors (13 sensors x 5 busy waits would freeze the UI for
        // minutes with a locked file); once they are used up every remaining sensor runs normally.
        check.restartChecked = true;
        qWarning().noquote()
                << QStringLiteral("[LimitAlarm][Restart] %1: restart lookups were given up after %2 failures; "
                                  "rows of earlier runs stay as they are and the normal limit logic runs.")
                           .arg(check.name)
                           .arg(m_restartLookupFailures);
        return true;
    }

    const qint64 t = now();
    if (m_lookupFailedAtMs >= 0 && t - m_lookupFailedAtMs < m_options.restartLookupDeferMs) {
        qInfo().noquote()
                << QStringLiteral("[LimitAlarm][Restart] %1 lookup deferred (a lookup failed %2 ms ago); "
                                  "no new %1 row until then.")
                           .arg(check.name)
                           .arg(t - m_lookupFailedAtMs);
        m_retryPending = true;
        return false;
    }

    const QDate today = QDate::currentDate();
    QList<SqlManager::UnresolvedAlarm> found;
    QString errorMessage;
    QElapsedTimer timer;
    timer.start();
    const bool ok = m_sql->findUnresolvedAlarms(check.name, QString::fromUtf8(kCoreStatus), today,
                                                &found, &errorMessage);
    const double lookupMs = timer.nsecsElapsed() / 1.0e6;
    if (!ok) {
        m_lookupFailedAtMs = now();
        ++check.lookupFailures;
        const int failures = ++m_restartLookupFailures;
        if (failures < m_options.restartLookupMaxAttempts) {
            qWarning().noquote()
                    << QStringLiteral("[LimitAlarm][Restart] %1 lookup of unresolved rows from earlier runs failed "
                                      "(attempt %2/%3, %4 ms): %5; retrying, no new %1 row until then.")
                               .arg(check.name)
                               .arg(failures)
                               .arg(m_options.restartLookupMaxAttempts)
                               .arg(lookupMs, 0, 'f', 1)
                               .arg(errorMessage);
            m_retryPending = true;
            return false;
        }
        qWarning().noquote()
                << QStringLiteral("[LimitAlarm][Restart] %1 lookup failed %2 times (%3); giving up: rows of earlier "
                                  "runs stay as they are and the normal limit logic runs.")
                           .arg(check.name)
                           .arg(failures)
                           .arg(errorMessage);
        check.restartChecked = true;
        m_restartLookupGivenUp = true;
        return true;
    }
    m_lookupFailedAtMs = -1;
    check.restartChecked = true;

    // Rows of this monitor carry "limit"; a row without it is classified by its message text.
    QList<Row> rows[2];
    QStringList listed;
    for (const SqlManager::UnresolvedAlarm &item : std::as_const(found)) {
        Row row;
        row.id = item.id;
        row.occurrence = QDateTime::fromSecsSinceEpoch(item.occurrenceTime);
        row.reason = QJsonDocument::fromJson(item.reason.toUtf8()).object();
        listed.append(QStringLiteral("id=%1(%2 %3)").arg(item.id).arg(item.monthKey, timeText(row.occurrence)));
        if (row.occurrence.date().toString(QStringLiteral("yyyyMM")) != item.monthKey) {
            // Rows are updated through the file of their occurrence month (SqlManager::updateAlarmReason).
            qWarning().noquote()
                    << QStringLiteral("[LimitAlarm][Restart] %1 row id=%2 in file %3 has occurrence %4 of another month; left as is.")
                               .arg(check.name)
                               .arg(item.id)
                               .arg(item.monthKey, timeText(row.occurrence));
            continue;
        }
        const QString kind = row.reason.value(QStringLiteral("limit")).toString();
        const QString message = row.reason.value(QStringLiteral("alarmMessage")).toString();
        int index = -1;
        if (kind == QLatin1String("upper") || (kind.isEmpty() && message.startsWith(limitTitle(Limit::Upper))))
            index = static_cast<int>(Limit::Upper);
        else if (kind == QLatin1String("lower") || (kind.isEmpty() && message.startsWith(limitTitle(Limit::Lower))))
            index = static_cast<int>(Limit::Lower);
        if (index < 0) {
            qWarning().noquote()
                    << QStringLiteral("[LimitAlarm][Restart] %1 row id=%2 (%3) is not an upper/lower limit alarm; left as is.")
                               .arg(check.name)
                               .arg(item.id)
                               .arg(message);
            continue;
        }
        rows[index].append(row);    // newest first (findUnresolvedAlarms order)
    }

    const QString months = QStringLiteral("%1+%2").arg(today.toString(QStringLiteral("yyyyMM")),
                                                       today.addMonths(-1).toString(QStringLiteral("yyyyMM")));
    qInfo().noquote()
            << QStringLiteral("[LimitAlarm][Restart] %1 first value after start: %2 %3 (%4); unresolved %5 rows "
                              "from earlier runs in %6: %7%8 (lookup %9 ms)")
                       .arg(check.name, number(value), check.unit,
                            above ? QStringLiteral("above the upper limit")
                                  : below ? QStringLiteral("below the lower limit") : QStringLiteral("in range"),
                            QString::fromUtf8(kCoreStatus), months)
                       .arg(found.size())
                       .arg(listed.isEmpty() ? QString() : QStringLiteral(" ") + listed.join(QLatin1Char(' ')))
                       .arg(lookupMs, 0, 'f', 1);

    for (const Limit limit : {Limit::Upper, Limit::Lower}) {
        const QList<Row> &kindRows = rows[static_cast<int>(limit)];
        if (kindRows.isEmpty())
            continue;
        Side &side = check.side[static_cast<int>(limit)];
        side.active = true;
        side.row = kindRows.first();
        side.normalSinceMs = -1;
        const bool stillViolated = limit == Limit::Upper ? above : below;
        qInfo().noquote()
                << QStringLiteral("[LimitAlarm][Restart] %1 %2: took over id=%3 (%4) as the active alarm row; %5")
                           .arg(check.name, limitTitle(limit))
                           .arg(side.row.id)
                           .arg(timeText(side.row.occurrence),
                                stillViolated ? QStringLiteral("still violated, no new row.")
                                              : QStringLiteral("now in range, resolved after %1 ms in range.")
                                                        .arg(m_options.resolveAfterMs));
        for (qsizetype index = 1; index < kindRows.size(); ++index) {
            const Row &older = kindRows.at(index);
            qInfo().noquote()
                    << QStringLiteral("[LimitAlarm][Restart] %1 %2: resolving id=%3 (%4): older duplicate of id=%5.")
                               .arg(check.name, limitTitle(limit))
                               .arg(older.id)
                               .arg(timeText(older.occurrence))
                               .arg(side.row.id);
            check.pending.append(PendingResolve{
                    older, QStringLiteral("%1 解除（重啟後由 id=%2 接手）").arg(limitTitle(limit)).arg(side.row.id)});
        }
    }
    return true;
}

void LimitAlarmMonitor::resolvePending(Check &check)
{
    while (!check.pending.isEmpty()) {
        // Stop at the first failure (a locked file costs one busy wait per evaluation).
        if (!markResolved(check.pending.first().row, check.pending.first().detail)) {
            m_retryPending = true;
            return;
        }
        check.pending.removeFirst();
    }
}

// ---- rows (same JSON and SqlManager calls as Manager::insertAlarmRow / markAlarmRowResolved) ----

bool LimitAlarmMonitor::insertRow(const Check &check, Limit limit, const QString &message, Row *row)
{
    if (!m_sql) {
        qWarning() << "[LimitAlarm] Insert alarm skipped: SqlManager is unavailable.";
        return false;
    }
    const QJsonObject alarm{
        {QStringLiteral("sensor"), check.name},
        {QStringLiteral("alarmMessage"), message},
        {QStringLiteral("status"), QString::fromUtf8(kCoreStatus)},
        {QStringLiteral("limit"), QLatin1String(limitKey(limit))},
        {QStringLiteral("sensorKey"), check.key},
    };
    const QString reason = QString::fromUtf8(QJsonDocument(alarm).toJson(QJsonDocument::Compact));
    const QDateTime occurrence = QDateTime::currentDateTime();
    QString errorMessage;
    qint64 id = -1;
    if (!m_sql->insertAlarm(occurrence, reason, &errorMessage, &id)) {
        qWarning().noquote()
                << QStringLiteral("[LimitAlarm] Insert alarm failed: sensor=%1 message=%2 (%3); retrying.")
                           .arg(check.name, message, errorMessage);
        return false;
    }
    qInfo().noquote()
            << QStringLiteral("[SQL] Alarm inserted: sensor=%1 message=%2 status=%3 id=%4 (limit alarm %5 %6)")
                       .arg(check.name, message, QString::fromUtf8(kCoreStatus))
                       .arg(id)
                       .arg(check.key, QLatin1String(limitKey(limit)));
    row->id = id;
    row->occurrence = occurrence;
    row->reason = alarm;
    emit alarmSaved();
    return true;
}

bool LimitAlarmMonitor::markResolved(const Row &row, const QString &detail)
{
    if (!m_sql) {
        qWarning() << "[LimitAlarm] Resolve alarm skipped: SqlManager is unavailable.";
        return false;
    }
    if (row.id < 0) {
        qWarning().noquote() << QStringLiteral("[LimitAlarm] alarm without a row id cannot be resolved; dropped.");
        return true;
    }
    QJsonObject alarm = row.reason;
    const QDateTime resolvedAt = QDateTime::currentDateTime();
    alarm.insert(QStringLiteral("resolved"), true);
    alarm.insert(QStringLiteral("resolvedAt"), resolvedAt.toSecsSinceEpoch());
    alarm.insert(QStringLiteral("resolvedDetail"), detail);
    const QString reason = QString::fromUtf8(QJsonDocument(alarm).toJson(QJsonDocument::Compact));
    QString errorMessage;
    if (!m_sql->updateAlarmReason(row.occurrence, row.id, reason, &errorMessage)) {
        qWarning().noquote()
                << QStringLiteral("[SQL] Resolve alarm failed: id=%1 sensor=%2 (%3); will retry.")
                           .arg(row.id)
                           .arg(alarm.value(QStringLiteral("sensor")).toString(), errorMessage);
        return false;
    }
    qInfo().noquote()
            << QStringLiteral("[SQL] Alarm resolved: id=%1 sensor=%2 message=%3 status=%4 detail=%5 resolvedAt=%6")
                       .arg(row.id)
                       .arg(alarm.value(QStringLiteral("sensor")).toString(),
                            alarm.value(QStringLiteral("alarmMessage")).toString(),
                            alarm.value(QStringLiteral("status")).toString(),
                            detail,
                            timeText(resolvedAt));
    emit alarmSaved();
    return true;
}

void LimitAlarmMonitor::scheduleTimer()
{
    const qint64 t = now();
    qint64 due = std::numeric_limits<qint64>::max();
    for (const Check &check : std::as_const(m_checks)) {
        for (const Side &side : check.side) {
            if (side.active && side.normalSinceMs >= 0)
                due = std::min(due, side.normalSinceMs + m_options.resolveAfterMs);
        }
    }
    if (m_retryPending)
        due = std::min(due, t + m_options.retryAfterMs);
    if (due == std::numeric_limits<qint64>::max()) {
        m_timer.stop();
        return;
    }
    const qint64 wait = std::clamp<qint64>(due - t, 0, std::numeric_limits<int>::max());
    m_timer.start(static_cast<int>(wait));
}

QList<LimitAlarmMonitor::ActiveAlarm> LimitAlarmMonitor::activeAlarms() const
{
    QList<ActiveAlarm> list;
    for (const Check &check : m_checks) {
        for (const Limit limit : {Limit::Upper, Limit::Lower}) {
            const Side &side = check.side[static_cast<int>(limit)];
            if (side.active)
                list.append(ActiveAlarm{check.key, limit, side.row.id, side.row.occurrence, side.normalSinceMs});
        }
    }
    return list;
}
