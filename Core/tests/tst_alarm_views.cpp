// w2-080 D2-D4: per-client alarm views (AlarmViewService, Core/AlarmViews.h), observed through the
// real TaidaFlowProxy::alarmViews map (what AlarmPage.qml reads) with the real SqlManager on a
// temporary data folder (alarm rows written with SqlManager::insertAlarm / updateAlarmReason, the
// functions Manager uses). No device, no network port.
//
//  * rangeAcrossTwoMonths: two month files, totalCount / totalPages / activeCount, order (newest
//    first, equal times by id desc), serialNumber, paging, page clamping (past the end and < 1);
//  * boundaries / emptyRange: both ends inclusive at ms precision; empty range = totalPages 1, no rows;
//  * rowsSameAsAlarmRecords: every row equals the alarmRecords record of the same alarm (the
//    conversion Core::loadAlarmRecords uses, AlarmRecordFormat::recordsFromHistory on
//    SqlManager::getAlarmHistory) plus serialNumber; and recordsFromHistory equals the former
//    core.cpp loop (copied below from 64b4593), so alarmRecords itself is unchanged;
//  * liveUpdates: a new alarm and a resolved alarm rewrite the entries whose range holds the
//    alarm's time (range and page kept, revision changes); the other entry keeps its revision;
//  * sessionsIndependent / newestRequestOnly: two sessions with their own ranges; three
//    requests of one session in a row -> only the newest is written;
//  * invalidInput: invalid session ids ignored (log limited); non-finite / reversed / out of range
//    ranges -> state "error" + message; page < 1 -> 1;
//  * cleanup: web entries idle for 30 min removed (injected clock), desktop never; at most 32
//    entries, least recently used web entry removed first;
//  * bigMonthNotBlocking: 5000 alarms in one month are read in several SqlManager steps; the
//    request itself returns at once.
#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutex>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <cmath>
#include <limits>
#include <memory>

#include "AlarmRecordFormat.h"
#include "AlarmViews.h"
#include "SqlManager.h"
#include "TaidaFlowProxy.h"

namespace {
QMutex g_logMutex;
QStringList g_log;
QtMessageHandler g_previousHandler = nullptr;

void captureMessages(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    {
        QMutexLocker locker(&g_logMutex);
        g_log << message;
    }
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

int logCount(const QString &needle)
{
    QMutexLocker locker(&g_logMutex);
    int n = 0;
    for (const QString &line : std::as_const(g_log))
        n += line.contains(needle) ? 1 : 0;
    return n;
}

QString reasonJson(const QString &sensor, const QString &message, const QString &status, bool resolved = false)
{
    QJsonObject o{{QStringLiteral("sensor"), sensor}, {QStringLiteral("alarmMessage"), message},
                  {QStringLiteral("status"), status}};
    if (resolved) {
        o.insert(QStringLiteral("resolved"), true);
        o.insert(QStringLiteral("resolvedAt"), 1);
        o.insert(QStringLiteral("resolvedDetail"), QStringLiteral("test"));
    }
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

qint64 ms(const QDateTime &t) { return t.toMSecsSinceEpoch(); }
QDateTime at(int y, int mo, int d, int h, int mi, int s) { return QDateTime(QDate(y, mo, d), QTime(h, mi, s)); }

// ---- The former Core::loadAlarmRecords loop (core.cpp at 64b4593), test-only reference -------
// Copied unchanged except for logging and names, to show that AlarmRecordFormat::recordsFromHistory
// (which Core::loadAlarmRecords calls since w2-080) produces exactly the same alarmRecords.
struct LegacyUi { QString alarmStatus; QString severity; bool known = true; };
LegacyUi legacyUiFieldsForStatus(const QString &coreStatus, bool hasStatus, bool isResolved)
{
    const QString unhandled = QStringLiteral("未處理");
    const QString resolved = QStringLiteral("已解除");
    const QString critical = QStringLiteral("嚴重");
    const QString warning = QStringLiteral("警告");
    LegacyUi fields{unhandled, warning, false};
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
QVariantList legacyRecords(const QJsonArray &history)
{
    QVariantList records;
    for (qsizetype index = history.size(); index > 0; --index) {
        const QJsonObject alarm = history.at(index - 1).toObject();
        const qint64 occurrence = static_cast<qint64>(alarm.value(QStringLiteral("occurrence_time")).toDouble());
        const QString storedReason = alarm.value(QStringLiteral("reason")).toString();
        QJsonParseError parseError;
        const QJsonDocument reasonDocument = QJsonDocument::fromJson(storedReason.toUtf8(), &parseError);
        const QJsonObject reasonObject = parseError.error == QJsonParseError::NoError && reasonDocument.isObject()
                ? reasonDocument.object() : QJsonObject();
        const QString sensor = reasonObject.value(QStringLiteral("sensor")).toString(QStringLiteral("—"));
        const QString alarmMessage = reasonObject.contains(QStringLiteral("alarmMessage"))
                ? reasonObject.value(QStringLiteral("alarmMessage")).toString()
                : reasonObject.value(QStringLiteral("message")).toString(storedReason);
        const bool hasStatus = reasonObject.value(QStringLiteral("status")).isString();
        const QString coreStatus = reasonObject.value(QStringLiteral("status")).toString();
        const bool isResolved = reasonObject.value(QStringLiteral("resolved")).toBool(false);
        LegacyUi ui = legacyUiFieldsForStatus(coreStatus, hasStatus, isResolved);
        const QString storedSeverity = reasonObject.value(QStringLiteral("severity")).toString();
        if (storedSeverity == QStringLiteral("嚴重") || storedSeverity == QStringLiteral("警告"))
            ui.severity = storedSeverity;
        records.append(QVariantMap{
            {QStringLiteral("id"), static_cast<qint64>(alarm.value(QStringLiteral("id")).toDouble())},
            {QStringLiteral("timestampMs"), occurrence * 1000},
            {QStringLiteral("alarmTime"), QDateTime::fromSecsSinceEpoch(occurrence).toString(QStringLiteral("yyyy/MM/dd HH:mm"))},
            {QStringLiteral("equipment"), QStringLiteral("系統")},
            {QStringLiteral("sensorName"), sensor},
            {QStringLiteral("alarmMessage"), alarmMessage},
            {QStringLiteral("severity"), ui.severity},
            {QStringLiteral("alarmStatus"), ui.alarmStatus},
        });
    }
    return records;
}

// One alarm written by the test, with what the page must show for it.
struct Alarm
{
    QDateTime time;
    QString reason;
    bool active;          // alarmStatus "未處理"
    qint64 id = -1;
};
} // namespace

class TestAlarmViews : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();
    void rangeAcrossTwoMonths();
    void boundaries();
    void emptyRange();
    void rowsSameAsAlarmRecords();
    void liveUpdates();
    void sessionsIndependent();
    void newestRequestOnly();
    void invalidInput();
    void cleanupIdleAndLimit();
    void bigMonthNotBlocking();

private:
    Alarm insert(const QDateTime &time, const QString &reason, bool active);
    QVariantMap entry(const QString &sid) const { return m_proxy->alarmViews().value(sid).toMap(); }
    qint64 revision(const QString &sid) const { return entry(sid).value(QStringLiteral("revision")).toLongLong(); }
    // Sends a request and waits until the session's entry has a revision above 'before'.
    QVariantMap request(const QString &sid, qint64 fromMs, qint64 toMs, int page);
    void checkPage(const QVariantMap &e, const QList<Alarm> &expectedNewestFirst, int page);
    std::unique_ptr<AlarmViewService> makeService();

    QTemporaryDir m_dir;
    SqlManager *m_sql = nullptr;
    std::unique_ptr<TaidaFlowProxy> m_proxy;
    std::unique_ptr<AlarmViewService> m_service;
    qint64 m_now = 0;                 // injected clock (ms)
    QList<Alarm> m_main;              // alarms in the main range, newest first
    qint64 m_mainFrom = 0;
    qint64 m_mainTo = 0;
};

Alarm TestAlarmViews::insert(const QDateTime &time, const QString &reason, bool active)
{
    Alarm a{time, reason, active};
    QString error;
    if (!m_sql->insertAlarm(time, reason, &error, &a.id))
        qFatal("insertAlarm failed: %s", qPrintable(error));
    return a;
}

std::unique_ptr<AlarmViewService> TestAlarmViews::makeService()
{
    AlarmViewService::Options options;
    options.clock = [this]() { return m_now; };
    return std::make_unique<AlarmViewService>(m_proxy.get(), m_sql, options);
}

QVariantMap TestAlarmViews::request(const QString &sid, qint64 fromMs, qint64 toMs, int page)
{
    // Applied = the service logged "<sid> entry written" or "<sid> entry unchanged" (an identical
    // entry keeps its revision, contract: revision changes only when the content changes).
    const QString applied = QStringLiteral("[Alarm] %1 entry ").arg(sid);
    const int before = logCount(applied);
    emit m_proxy->alarmViewRequested(sid, double(fromMs), double(toMs), page);   // the UI's signal
    [&]() { QTRY_VERIFY_WITH_TIMEOUT(logCount(applied) > before, 10000); }();
    if (QTest::currentTestFailed())
        qWarning().noquote() << "request" << sid << "page" << page << "got no new entry";
    return entry(sid);
}

void TestAlarmViews::checkPage(const QVariantMap &e, const QList<Alarm> &all, int page)
{
    const int size = 9;
    const int totalPages = std::max<int>(1, (all.size() + size - 1) / size);
    QCOMPARE(e.value("state").toString(), QStringLiteral("ready"));
    QCOMPARE(e.value("message").toString(), QString());
    QCOMPARE(e.value("pageSize").toInt(), size);
    QCOMPARE(e.value("totalCount").toLongLong(), qint64(all.size()));
    QCOMPARE(e.value("totalPages").toInt(), totalPages);
    QCOMPARE(e.value("page").toInt(), page);
    int active = 0;
    for (const Alarm &a : all)
        active += a.active ? 1 : 0;
    QCOMPARE(e.value("activeCount").toLongLong(), qint64(active));
    const QVariantList rows = e.value("rows").toList();
    const int first = (page - 1) * size;
    QCOMPARE(rows.size(), std::max(0, std::min(size, int(all.size()) - first)));
    for (int i = 0; i < rows.size(); ++i) {
        const QVariantMap row = rows.at(i).toMap();
        const Alarm &a = all.at(first + i);
        QCOMPARE(row.value("serialNumber").toLongLong(), qint64(first + i + 1));
        QCOMPARE(row.value("id").toLongLong(), a.id);
        QCOMPARE(row.value("timestampMs").toLongLong(), a.time.toSecsSinceEpoch() * 1000);
        QCOMPARE(row.value("alarmStatus").toString(), a.active ? QStringLiteral("未處理") : QStringLiteral("已解除"));
        QCOMPARE(row.value("alarmTime").toString(), a.time.toString(QStringLiteral("yyyy/MM/dd HH:mm")));
    }
}

void TestAlarmViews::initTestCase()
{
    QVERIFY(m_dir.isValid());
    g_previousHandler = qInstallMessageHandler(captureMessages);
    m_sql = SqlManager::instance();
    m_sql->setDataDirectory(m_dir.filePath(QStringLiteral("data")));
    m_sql->setSettingsFile(m_dir.filePath(QStringLiteral("settings.sqlite")));
    QVERIFY(m_sql->initialize());

    const QString crit = QStringLiteral("異常");
    // January 2026 (sensor_202601.sqlite)
    insert(at(2026, 1, 10, 8, 0, 0), reasonJson("DI1", "漏液檢出", crit), true);          // before the range
    const Alarm j2 = insert(at(2026, 1, 25, 9, 0, 0), reasonJson("PT-01", "high", QStringLiteral("數值異常")), true);
    const Alarm j3 = insert(at(2026, 1, 25, 9, 0, 0), reasonJson("100", "設備啟動", QStringLiteral("正常")), false);
    const Alarm j4 = insert(at(2026, 1, 25, 9, 0, 0), reasonJson("DI0", "相位異常", crit, true), false);
    const Alarm j5 = insert(at(2026, 1, 28, 12, 30, 15), QStringLiteral("legacy plain text"), true);
    const Alarm j6 = insert(at(2026, 1, 31, 23, 59, 59), reasonJson("X", "unknown status", QStringLiteral("strange")), true);
    // February 2026 (sensor_202602.sqlite)
    const Alarm f1 = insert(at(2026, 2, 1, 0, 0, 0), reasonJson("MS300", "warning", QStringLiteral("警告")), true);
    QList<Alarm> feb;
    for (int i = 0; i < 7; ++i) {
        const bool critical = i % 2 == 0;
        feb.prepend(insert(at(2026, 2, 3, 1 + i, 0, 0),
                           reasonJson(QStringLiteral("DI%1").arg(i), QStringLiteral("m%1").arg(i),
                                      critical ? crit : QStringLiteral("正常")), critical));
    }
    insert(at(2026, 2, 15, 0, 0, 0), reasonJson("DI2", "after", crit), true);              // after the range

    m_main = feb;                     // F8..F2
    m_main << f1 << j6 << j5 << j4 << j3 << j2;   // equal times: id descending
    QCOMPARE(m_main.size(), 13);
    m_mainFrom = ms(at(2026, 1, 20, 0, 0, 0));
    m_mainTo = ms(at(2026, 2, 10, 23, 59, 0)) + 59999;
    QVERIFY(QFileInfo::exists(m_dir.filePath("data/sensor_202601.sqlite")));
    QVERIFY(QFileInfo::exists(m_dir.filePath("data/sensor_202602.sqlite")));
}

void TestAlarmViews::cleanupTestCase()
{
    SqlManager::instance()->shutdown();
    qInstallMessageHandler(g_previousHandler);
}

void TestAlarmViews::init()
{
    m_now = 1000;
    m_proxy = std::make_unique<TaidaFlowProxy>();
    m_service = makeService();
}

void TestAlarmViews::cleanup()
{
    m_service.reset();
    m_proxy.reset();
    QCoreApplication::processEvents();
}

void TestAlarmViews::rangeAcrossTwoMonths()
{
    const QString sid = QStringLiteral("web-cross");
    QVariantMap e = request(sid, m_mainFrom, m_mainTo, 1);
    QCOMPARE(e.value("fromMs").toDouble(), double(m_mainFrom));
    QCOMPARE(e.value("toMs").toDouble(), double(m_mainTo));
    checkPage(e, m_main, 1);
    QCOMPARE(e.value("totalPages").toInt(), 2);
    QCOMPARE(e.value("activeCount").toLongLong(), 8);
    // Equal times (2026-01-25 09:00:00) by id descending: j4, j3, j2 are the last three.
    e = request(sid, m_mainFrom, m_mainTo, 2);
    checkPage(e, m_main, 2);
    const QVariantList rows = e.value("rows").toList();
    QCOMPARE(rows.size(), 4);
    QVERIFY(rows.at(1).toMap().value("id").toLongLong() > rows.at(2).toMap().value("id").toLongLong());
    QVERIFY(rows.at(2).toMap().value("id").toLongLong() > rows.at(3).toMap().value("id").toLongLong());
    QCOMPARE(rows.at(3).toMap().value("serialNumber").toLongLong(), 13);
    // Past the last page -> the last page; < 1 -> page 1.
    e = request(sid, m_mainFrom, m_mainTo, 99);
    checkPage(e, m_main, 2);
    e = request(sid, m_mainFrom, m_mainTo, 0);
    checkPage(e, m_main, 1);
    e = request(sid, m_mainFrom, m_mainTo, 2);
    checkPage(e, m_main, 2);
    e = request(sid, m_mainFrom, m_mainTo, -5);
    checkPage(e, m_main, 1);
    QVERIFY(logCount(QStringLiteral("[Alarm] web-cross")) > 0);
    QVERIFY(logCount(QStringLiteral("2 month file(s)")) > 0);
}

void TestAlarmViews::boundaries()
{
    const QString sid = QStringLiteral("web-bound");
    const qint64 t = ms(at(2026, 1, 25, 9, 0, 0));      // j2 / j3 / j4
    QVariantMap e = request(sid, t, t, 1);               // one ms: both ends inclusive
    QCOMPARE(e.value("totalCount").toLongLong(), 3);
    e = request(sid, t + 1, t + 60000, 1);               // just after: 0
    QCOMPARE(e.value("totalCount").toLongLong(), 0);
    e = request(sid, t - 60000, t - 1, 1);               // just before: 0 (next write: content differs)
    QCOMPARE(e.value("totalCount").toLongLong(), 0);
    e = request(sid, t - 60000, t + 999, 3);             // the whole second
    QCOMPARE(e.value("totalCount").toLongLong(), 3);
    QCOMPARE(e.value("page").toInt(), 1);
}

void TestAlarmViews::emptyRange()
{
    const QString sid = QStringLiteral("web-empty");
    const QVariantMap e = request(sid, ms(at(2025, 3, 1, 0, 0, 0)), ms(at(2025, 3, 31, 23, 59, 59)), 4);
    QCOMPARE(e.value("state").toString(), QStringLiteral("ready"));
    QCOMPARE(e.value("totalCount").toLongLong(), 0);
    QCOMPARE(e.value("totalPages").toInt(), 1);
    QCOMPARE(e.value("page").toInt(), 1);
    QCOMPARE(e.value("activeCount").toLongLong(), 0);
    QVERIFY(e.value("rows").toList().isEmpty());
    QCOMPARE(e.value("pageSize").toInt(), 9);
}

void TestAlarmViews::rowsSameAsAlarmRecords()
{
    // alarmRecords path: SqlManager::getAlarmHistory (what Core::loadAlarmRecords calls) +
    // AlarmRecordFormat::recordsFromHistory (its conversion since w2-080).
    QJsonArray history;
    QString error;
    QVERIFY(m_sql->getAlarmHistory(m_mainFrom / 1000, m_mainTo / 1000, &history, &error));
    const QVariantList records = AlarmRecordFormat::recordsFromHistory(history, nullptr);
    QCOMPARE(records.size(), 13);
    // ... and it is exactly the former core.cpp loop: alarmRecords unchanged.
    QCOMPARE(records, legacyRecords(history));
    // Same alarm -> same map (plus serialNumber), for every row of both pages.
    const QString sid = QStringLiteral("web-same");
    QVariantList viewRows = request(sid, m_mainFrom, m_mainTo, 1).value("rows").toList();
    viewRows += request(sid, m_mainFrom, m_mainTo, 2).value("rows").toList();
    QCOMPARE(viewRows.size(), 13);
    for (int i = 0; i < viewRows.size(); ++i) {
        QVariantMap row = viewRows.at(i).toMap();
        QCOMPARE(row.take(QStringLiteral("serialNumber")).toLongLong(), qint64(i + 1));
        // records is newest first with the same tie order as the view (index order of the file).
        QCOMPARE(row, records.at(i).toMap());
        QCOMPARE(row.keys(), (QStringList{"alarmMessage", "alarmStatus", "alarmTime", "equipment", "id",
                                          "sensorName", "severity", "timestampMs"}));
    }
    // Spot checks of the conversion (legacy text, unknown status, resolved 異常 stays 嚴重).
    const QVariantMap legacy = viewRows.at(9).toMap();
    QCOMPARE(legacy.value("alarmMessage").toString(), QStringLiteral("legacy plain text"));
    QCOMPARE(legacy.value("sensorName").toString(), QStringLiteral("—"));
    QCOMPARE(legacy.value("alarmStatus").toString(), QStringLiteral("未處理"));
    const QVariantMap resolved = viewRows.at(10).toMap();
    QCOMPARE(resolved.value("alarmStatus").toString(), QStringLiteral("已解除"));
    QCOMPARE(resolved.value("severity").toString(), QStringLiteral("嚴重"));
}

void TestAlarmViews::liveUpdates()
{
    // March 2026: 10 alarms -> 2 pages; session A shows page 2 of March, B the main range.
    QList<Alarm> march;
    for (int i = 0; i < 10; ++i)
        march.prepend(insert(at(2026, 3, 5, 10, i, 0), reasonJson("PT-0" + QString::number(i % 7 + 1), "m",
                                                                  QStringLiteral("數值異常")), true));
    const qint64 aFrom = ms(at(2026, 3, 1, 0, 0, 0));
    const qint64 aTo = ms(at(2026, 3, 31, 23, 59, 0)) + 59999;
    QVariantMap a = request(QStringLiteral("web-liveA"), aFrom, aTo, 2);
    checkPage(a, march, 2);
    const QVariantMap b = request(QStringLiteral("web-liveB"), m_mainFrom, m_mainTo, 1);
    checkPage(b, m_main, 1);
    const qint64 revA = revision("web-liveA");
    const qint64 revB = revision("web-liveB");

    // New alarm inside A's range (newest): A rewritten, still page 2, rows shifted by one.
    const Alarm added = insert(at(2026, 3, 20, 8, 0, 0), reasonJson("DI1", "漏液檢出", QStringLiteral("異常")), true);
    march.prepend(added);
    QTRY_VERIFY_WITH_TIMEOUT(revision("web-liveA") > revA, 10000);
    a = entry("web-liveA");
    checkPage(a, march, 2);
    QCOMPARE(a.value("fromMs").toDouble(), double(aFrom));
    QCOMPARE(a.value("activeCount").toLongLong(), 11);

    // Resolve it (Manager's way: updateAlarmReason with resolved = true): activeCount 10.
    const qint64 revA2 = revision("web-liveA");
    QString error;
    QVERIFY(m_sql->updateAlarmReason(added.time, added.id,
                                     reasonJson("DI1", "漏液檢出", QStringLiteral("異常"), true), &error));
    march.first().active = false;
    QTRY_VERIFY_WITH_TIMEOUT(revision("web-liveA") > revA2, 10000);
    a = entry("web-liveA");
    checkPage(a, march, 2);
    QCOMPARE(a.value("activeCount").toLongLong(), 10);

    // B's range does not hold March: never rewritten.
    QTest::qWait(300);
    QCOMPARE(revision("web-liveB"), revB);
    QCOMPARE(entry("web-liveB"), b);
    QVERIFY(logCount(QStringLiteral("1 of 2 view(s) hold that time, read again: web-liveA")) >= 2);
}

void TestAlarmViews::sessionsIndependent()
{
    const qint64 marchFrom = ms(at(2026, 3, 1, 0, 0, 0));
    const qint64 marchTo = ms(at(2026, 3, 31, 23, 59, 59));
    // Both requests go out before either result is back.
    emit m_proxy->alarmViewRequested(QStringLiteral("web-ind1"), double(m_mainFrom), double(m_mainTo), 2);
    emit m_proxy->alarmViewRequested(QStringLiteral("desktop"), double(marchFrom), double(marchTo), 1);
    QTRY_VERIFY_WITH_TIMEOUT(revision("web-ind1") > 0 && revision("desktop") > 0, 10000);
    checkPage(entry("web-ind1"), m_main, 2);
    QCOMPARE(entry("desktop").value("totalCount").toLongLong(), 11);
    QCOMPARE(entry("desktop").value("fromMs").toDouble(), double(marchFrom));
    QCOMPARE(entry("web-ind1").value("fromMs").toDouble(), double(m_mainFrom));
    // Paging one session does not touch the other.
    const QVariantMap desk = entry("desktop");
    request(QStringLiteral("web-ind1"), m_mainFrom, m_mainTo, 1);
    QCOMPARE(entry("desktop"), desk);
}

void TestAlarmViews::newestRequestOnly()
{
    const QString sid = QStringLiteral("web-newest");
    QSignalSpy spy(m_proxy.get(), &TaidaFlowProxy::alarmViewsChanged);
    const int stale = logCount(QStringLiteral("[Alarm] web-newest result"));
    // Three requests of one session in a row (no event loop in between): pages 1, 2, then 1 of
    // another range. Only the last one may be written.
    emit m_proxy->alarmViewRequested(sid, double(m_mainFrom), double(m_mainTo), 1);
    emit m_proxy->alarmViewRequested(sid, double(m_mainFrom), double(m_mainTo), 2);
    const qint64 t = ms(at(2026, 1, 25, 9, 0, 0));
    emit m_proxy->alarmViewRequested(sid, double(t), double(t + 999), 1);
    QTRY_VERIFY_WITH_TIMEOUT(revision(sid) > 0, 10000);
    QTest::qWait(300);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(entry(sid).value("totalCount").toLongLong(), 3);
    QCOMPARE(entry(sid).value("fromMs").toDouble(), double(t));
    // The two older ones were dropped (by SqlManager's stale rule or here), never written.
    QCOMPARE(logCount(QStringLiteral("[Alarm] web-newest result")) - stale, 2);
}

void TestAlarmViews::invalidInput()
{
    QSignalSpy spy(m_proxy.get(), &TaidaFlowProxy::alarmViewsChanged);
    const int lines = logCount(QStringLiteral("view request with an invalid session id"));
    // Invalid session ids: ignored, nothing stored, logged once per minute.
    const QStringList bad{QString(), QStringLiteral("a b"), QStringLiteral("../x"), QString(41, QLatin1Char('a')),
                          QStringLiteral("web-中"), QStringLiteral("web;DROP")};
    for (int round = 0; round < 50; ++round) {
        for (const QString &id : bad)
            emit m_proxy->alarmViewRequested(id, double(m_mainFrom), double(m_mainTo), 1);
    }
    QTest::qWait(200);
    QCOMPARE(spy.count(), 0);
    QVERIFY(m_proxy->alarmViews().isEmpty());
    QCOMPARE(m_service->invalidIdCount(), 300);
    QCOMPARE(logCount(QStringLiteral("view request with an invalid session id")) - lines, 1);
    m_now += 61 * 1000;
    emit m_proxy->alarmViewRequested(QStringLiteral("x y"), 0.0, 1.0, 1);
    QCOMPARE(logCount(QStringLiteral("view request with an invalid session id")) - lines, 2);
    QCOMPARE(logCount(QStringLiteral("299 more since the previous message")), 1);

    // Valid id, invalid range: state "error" + message, no crash, written at once.
    const QString sid = QStringLiteral("web-invalid");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const QList<QPair<double, double>> ranges{
        {nan, 1.0}, {0.0, nan}, {-inf, 0.0}, {0.0, inf}, {2000.0, 1000.0}, {-1.0, 1000.0},
        {0.0, AlarmViewService::kMaxRangeMs + 1.0}, {1.0e300, 1.0e301}};
    for (const auto &r : ranges) {
        const qint64 before = revision(sid);
        emit m_proxy->alarmViewRequested(sid, r.first, r.second, 3);
        const QVariantMap e = entry(sid);
        QCOMPARE(e.value("state").toString(), QStringLiteral("error"));
        QVERIFY(!e.value("message").toString().isEmpty());
        QCOMPARE(e.value("totalPages").toInt(), 1);
        QCOMPARE(e.value("page").toInt(), 1);
        QVERIFY(e.value("rows").toList().isEmpty());
        QVERIFY(std::isfinite(e.value("fromMs").toDouble()) && std::isfinite(e.value("toMs").toDouble()));
        QVERIFY(revision(sid) >= before);
    }
    // A valid request afterwards works again.
    QVariantMap e = request(sid, m_mainFrom, m_mainTo, 1);
    checkPage(e, m_main, 1);
    // An invalid request drops a query of the same session that is still running.
    emit m_proxy->alarmViewRequested(sid, double(m_mainFrom), double(m_mainTo), 2);
    emit m_proxy->alarmViewRequested(sid, nan, nan, 1);
    QTest::qWait(300);
    QCOMPARE(entry(sid).value("state").toString(), QStringLiteral("error"));
    // The largest valid range reads every month file.
    e = request(sid, 0.0, AlarmViewService::kMaxRangeMs, 1);
    QCOMPARE(e.value("state").toString(), QStringLiteral("ready"));
    QVERIFY(e.value("totalCount").toLongLong() >= 15);
}

void TestAlarmViews::cleanupIdleAndLimit()
{
    const qint64 from = ms(at(2025, 1, 1, 0, 0, 0));
    const qint64 to = ms(at(2025, 1, 2, 0, 0, 0));
    request(QStringLiteral("desktop"), from, to, 1);
    request(QStringLiteral("web-idle"), from, to, 1);
    m_now += 29 * 60 * 1000;
    request(QStringLiteral("web-busy"), from, to, 1);
    m_service->sweepIdle();
    QCOMPARE(m_service->sessionIds(), (QStringList{"desktop", "web-busy", "web-idle"}));
    m_now += 60 * 1000;                                   // web-idle: 30 min without a request
    m_service->sweepIdle();
    QCOMPARE(m_service->sessionIds(), (QStringList{"desktop", "web-busy"}));
    QCOMPARE(m_proxy->alarmViews().keys(), (QStringList{"desktop", "web-busy"}));
    m_now += 24LL * 3600 * 1000;                          // desktop is never removed
    m_service->sweepIdle();
    QCOMPARE(m_proxy->alarmViews().keys(), (QStringList{"desktop"}));
    // A removed client's next request rebuilds its entry.
    request(QStringLiteral("web-idle"), from, to, 1);
    QCOMPARE(m_proxy->alarmViews().keys(), (QStringList{"desktop", "web-idle"}));

    // At most 32 entries: desktop (least recently used of all) + 31 web; one more removes the
    // least recently used WEB entry (web-idle), never desktop.
    for (int i = 0; i < 30; ++i)
        request(QStringLiteral("web-lru%1").arg(i, 2, 10, QLatin1Char('0')), from, to, 1);
    QCOMPARE(m_proxy->alarmViews().size(), 32);
    request(QStringLiteral("web-lru00"), from, to + 1000, 1);   // used again: no longer the oldest web
    request(QStringLiteral("web-extra"), from, to, 1);
    QCOMPARE(m_proxy->alarmViews().size(), 32);
    QVERIFY(m_proxy->alarmViews().contains("desktop"));
    QVERIFY(!m_proxy->alarmViews().contains("web-idle"));
    QVERIFY(m_proxy->alarmViews().contains("web-lru00"));
    request(QStringLiteral("web-extra2"), from, to, 1);
    QVERIFY(!m_proxy->alarmViews().contains("web-lru01"));
    QVERIFY(m_proxy->alarmViews().contains("desktop"));
    QCOMPARE(m_service->sessionIds().size(), 32);
}

void TestAlarmViews::bigMonthNotBlocking()
{
    // 5000 alarms in June 2025, written with an own connection in one transaction (fast), into
    // a month file SqlManager has not opened yet.
    const QString file = m_dir.filePath(QStringLiteral("data/sensor_202506.sqlite"));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("bulk"));
        db.setDatabaseName(file);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec("CREATE TABLE alarm_history (id INTEGER PRIMARY KEY, occurrence_time INTEGER, reason VARCHAR(255))"));
        QVERIFY(q.exec("CREATE INDEX idx_alarm_history_time ON alarm_history(occurrence_time)"));
        QVERIFY(db.transaction());
        QVERIFY(q.prepare("INSERT INTO alarm_history (occurrence_time, reason) VALUES (?, ?)"));
        const qint64 base = at(2025, 6, 1, 0, 0, 0).toSecsSinceEpoch();
        for (int i = 0; i < 5000; ++i) {
            q.addBindValue(base + i * 60);
            q.addBindValue(reasonJson("DI1", "bulk", i % 5 == 0 ? QStringLiteral("正常") : QStringLiteral("異常")));
            QVERIFY(q.exec());
        }
        QVERIFY(db.commit());
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("bulk"));

    const QString sid = QStringLiteral("web-big");
    const qint64 from = ms(at(2025, 6, 1, 0, 0, 0));
    const qint64 to = ms(at(2025, 6, 30, 23, 59, 0)) + 59999;
    const qint64 before = revision(sid);
    QElapsedTimer t;
    t.start();
    emit m_proxy->alarmViewRequested(sid, double(from), double(to), 300);
    const double postMs = t.nsecsElapsed() / 1.0e6;
    QVERIFY2(revision(sid) == before, "the entry must not be written inside the request call");
    QVERIFY2(postMs < 50.0, qPrintable(QString::number(postMs)));
    QTRY_VERIFY_WITH_TIMEOUT(revision(sid) > before, 20000);
    const QVariantMap e = entry(sid);
    QCOMPARE(e.value("totalCount").toLongLong(), 5000);
    QCOMPARE(e.value("activeCount").toLongLong(), 4000);
    QCOMPARE(e.value("totalPages").toInt(), 556);          // ceil(5000 / 9)
    QCOMPARE(e.value("page").toInt(), 300);
    const QVariantList rows = e.value("rows").toList();
    QCOMPARE(rows.size(), 9);
    // Newest first: row k (0-based position p = 299*9 + k) is minute 4999 - p.
    const qint64 base = at(2025, 6, 1, 0, 0, 0).toSecsSinceEpoch();
    for (int k = 0; k < rows.size(); ++k) {
        const qint64 p = 299 * 9 + k;
        QCOMPARE(rows.at(k).toMap().value("timestampMs").toLongLong(), (base + (4999 - p) * 60) * 1000);
        QCOMPARE(rows.at(k).toMap().value("serialNumber").toLongLong(), p + 1);
    }
    // Last page = 5000 - 555*9 = 5 rows.
    const QVariantMap last = request(sid, from, to, 1000);
    QCOMPARE(last.value("page").toInt(), 556);
    QCOMPARE(last.value("rows").toList().size(), 5);
    QCOMPARE(last.value("rows").toList().last().toMap().value("serialNumber").toLongLong(), 5000);
    qInfo().noquote() << QStringLiteral("[test] request call returned in %1 ms").arg(postMs, 0, 'f', 3);
    // Read in several queued SqlManager steps (listing + 3 chunks of <= 2000 rows + end).
    int maxSteps = 0;
    {
        QMutexLocker locker(&g_logMutex);
        static const QRegularExpression re(QStringLiteral("\\[Alarm\\] web-big #\\d+ applied: .* SqlManager (\\d+) step"));
        for (const QString &line : std::as_const(g_log)) {
            const QRegularExpressionMatch m = re.match(line);
            if (m.hasMatch())
                maxSteps = std::max(maxSteps, m.captured(1).toInt());
        }
    }
    QVERIFY2(maxSteps >= 4, qPrintable(QString::number(maxSteps)));
}

QTEST_MAIN(TestAlarmViews)
#include "tst_alarm_views.moc"
