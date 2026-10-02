#pragma once
// w2-085 (desktop only, never compiled into WebAssembly): alarms for the upper / lower limits of the
// settings page (TaidaFlowProxy::sensorSettingsSv, contract Core/SensorSettings.md), recorded like the
// leak sensor DI1 (w2-037 / w2-053): a limit violation inserts an alarm_history row at once, and the
// same row is marked resolved (SqlManager::updateAlarmReason) when the value has been back in range
// for 2 s.
//
// Rules (Mango 2026-10-02, PM decisions of w2-085):
//  * Checked sensors (13): pt01..pt07, tt01..tt04, flowMeter, filter.  Each sensor key is judged on
//    its own (the co-located groups PT-04/PT-05 etc. share their limits on the settings page, but
//    every key is judged and recorded separately).
//  * Value = corrected value = raw PV + offset of that key (the PVs themselves stay raw).  filter =
//    corrected PT-02 - corrected PT-03, judged against the limits of the 'filter' entry.  Pressures
//    and their limits are in kPa (the display unit pressureUnitSv does not matter), temperatures in
//    degC, flow in L/min.
//  * upper alarm: upperEnabled && value > upper;  lower alarm: lowerEnabled && value < lower.
//    Equal to a limit is normal; a disabled limit is never violated.  This is the expression the
//    UI uses for the limit colours (TaidaFlowContent/components/SensorUnits.js SensorUnits.limitState,
//    called by the Filter graphic and, since w1-088, by every main-page value); the test
//    tst_limit_alarms evaluates the UI's own JavaScript against limitState() below.
//  * Upper and lower are two independent alarms of the same sensor (each inserted / resolved on its
//    own).  While a sensor's upper (lower) alarm is unresolved, no second row of that kind is added.
//  * Back in range: the row is resolved only after the value has stayed in range for resolveAfterMs
//    (2000 ms) on a monotonic clock.  A new violation within that time cancels the countdown (the
//    same row stays, no new row).  The countdown runs on a timer, so it does not depend on how often
//    the PV is updated.
//  * Settings changes (sensorSettingsSvChanged, including disabling a limit) re-evaluate every
//    sensor at once with the new settings; a disabled limit counts as in range (resolved 2 s later).
//  * A sensor is judged only after its PV was written by the backend at least once in this run
//    (Manager::updateProcessPoint -> processPointUpdated); filter needs both PT-02 and PT-03.
//    The PV updates of one Modbus reply are evaluated together, after the whole reply was applied
//    (queued), so filter never sees a new PT-02 with an old PT-03.
//  * Restart (as w2-053 for the DIs): on a sensor's first evaluation after start the unresolved
//    '警告' rows of that sensor name in this month's and last month's data file are looked up.  The
//    newest row of each kind (upper / lower) is taken over as the active alarm (no new row while the
//    sensor is still over the limit; resolved 2 s after it is back in range); older rows of the same
//    kind are resolved at once as duplicates.  A failed lookup writes no new row for that sensor and
//    is retried on the next update (the other sensors wait restartLookupDeferMs after a failure);
//    after restartLookupMaxAttempts failed lookups (shared by all sensors, so a locked file costs at
//    most that many busy waits) the normal logic runs for every sensor not checked yet.
//
// Stored row (alarm_history.reason JSON): the fields Manager::saveAlarm writes (sensor = the name on
// the screen: PT-04, TT-01, 流量計, Filter 壓差; alarmMessage; status = 警告 -> severity 警告, see
// AlarmRecordFormat.h) plus "limit": "upper"|"lower" and "sensorKey" (e.g. "pt04") that identify the
// alarm after a restart.  Resolving adds resolved / resolvedAt / resolvedDetail exactly like
// Manager::markAlarmRowResolved (w2-037), so alarmRecords shows 已解除.  alarmSaved() is emitted after
// every insert / resolve (Manager forwards it to Core::loadAlarmRecords); SqlManager's
// alarmHistoryChanged updates the alarm views (AlarmViewService) by itself.
//
// Everything runs on the thread of the Proxy (the main thread); SqlManager calls are its blocking
// calls, like Manager's.
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <functional>

class SqlManager;
class TaidaFlowProxy;
namespace ModbusMapping { enum class ProcessPoint; }

class LimitAlarmMonitor final : public QObject
{
    Q_OBJECT
public:
    struct Options
    {
        qint64 resolveAfterMs = 2000;        // in range this long -> resolved
        int restartLookupMaxAttempts = 5;    // failed restart lookups (all sensors together) before giving up
        qint64 restartLookupDeferMs = 500;   // after a failed lookup, other sensors wait this long
        qint64 retryAfterMs = 1000;          // a failed insert / resolve is retried after this
        // Monotonic time in ms; empty = an internal QElapsedTimer.  Tests inject their own clock.
        std::function<qint64()> clock;
    };

    enum class Limit { Upper = 0, Lower = 1 };

    static constexpr auto kCoreStatus = "警告";   // -> alarmStatus 未處理, severity 警告

    LimitAlarmMonitor(TaidaFlowProxy *proxy, SqlManager *sql, const Options &options,
                      QObject *parent = nullptr);
    LimitAlarmMonitor(TaidaFlowProxy *proxy, SqlManager *sql, QObject *parent = nullptr);
    ~LimitAlarmMonitor() override;

    // Manager::updateProcessPoint, after the Proxy PV was written (MV positions are ignored).
    void processPointUpdated(ModbusMapping::ProcessPoint point);
    // The PV of sensorKey (pt01..pt07, tt01..tt04, flowMeter) was written.  Evaluated queued,
    // together with the other updates of the same event (or by flushPendingUpdates()).
    void valueUpdated(const QString &sensorKey);
    // Evaluates the queued updates now (tests; the queued call does the same).
    void flushPendingUpdates();
    // Re-evaluates every sensor with a known value (connected to sensorSettingsSvChanged).
    void reevaluate();
    // Resolves rows whose 2 s have passed and retries failed writes (the timer calls it).
    void processDue();

    // ---- rules, shared with the tests ----
    static QStringList sensorKeys();                    // the 13 keys, settings page order
    static QString sensorName(const QString &key);      // PT-04, TT-01, 流量計, Filter 壓差
    static QString unitOf(const QString &key);          // kPa, °C, L/min
    // Corrected value of key from the Proxy (raw PV + offset; filter = PT-02 - PT-03, both corrected).
    static bool correctedValue(const TaidaFlowProxy &proxy, const QString &key, double *value);
    static bool isAboveUpper(double value, const QVariantMap &entry);
    static bool isBelowLower(double value, const QVariantMap &entry);
    // 1 above the upper limit, -1 below the lower limit, 0 normal (the UI's SensorUnits.limitState).
    static int limitState(double value, const QVariantMap &entry);
    static QString alarmMessage(Limit limit, double value, double limitValue, const QString &unit);

    // ---- diagnostics (tests) ----
    struct ActiveAlarm
    {
        QString key;
        Limit limit = Limit::Upper;
        qint64 id = -1;
        QDateTime occurrence;
        qint64 normalSinceMs = -1;   // >= 0: back in range since then (countdown running)
    };
    QList<ActiveAlarm> activeAlarms() const;
    qint64 now() const;
    bool timerActive() const { return m_timer.isActive(); }
    int timerRemainingMs() const { return m_timer.remainingTime(); }

signals:
    // A row was inserted or resolved (Manager forwards it as Manager::alarmSaved).
    void alarmSaved();

private:
    struct Row
    {
        qint64 id = -1;
        QDateTime occurrence;
        QJsonObject reason;
    };
    struct Side
    {
        bool active = false;
        Row row;
        qint64 normalSinceMs = -1;
    };
    struct PendingResolve
    {
        Row row;
        QString detail;
    };
    struct Check
    {
        QString key;
        QString name;
        QString unit;
        bool restartChecked = false;
        int lookupFailures = 0;
        Side side[2];                 // [Limit::Upper], [Limit::Lower]
        QList<PendingResolve> pending;
    };

    bool valueKnown(const QString &key) const;
    void evaluate(Check &check);
    void updateSide(Check &check, Limit limit, bool violated, double value, bool enabled, double limitValue);
    bool takeOverRestart(Check &check, double value, bool above, bool below);
    void resolvePending(Check &check);
    bool insertRow(const Check &check, Limit limit, const QString &message, Row *row);
    bool markResolved(const Row &row, const QString &detail);
    void scheduleTimer();

    TaidaFlowProxy *m_proxy = nullptr;
    SqlManager *m_sql = nullptr;
    Options m_options;
    QElapsedTimer m_elapsed;
    QList<Check> m_checks;            // sensorKeys() order
    QSet<QString> m_known;            // keys whose PV was written in this run
    QSet<QString> m_dirty;            // updated keys waiting for the queued evaluation
    bool m_flushQueued = false;
    bool m_retryPending = false;
    qint64 m_lookupFailedAtMs = -1;
    int m_restartLookupFailures = 0;      // failed restart lookups of all sensors together
    bool m_restartLookupGivenUp = false;  // restartLookupMaxAttempts reached: no more lookups
    QTimer m_timer;
};
