// w2-071 D4/D5 (review D-007): limits of HistoryExportManager's status map, observed through
// the real TaidaFlowProxy::historyExportStatus (what the page reads).
//
//  * 1000 requests with invalid session ids: nothing is stored, one log line (60 s limit);
//  * 100 refused requests of valid sessions (bad range): stored as error entries, but the map
//    keeps at most kMaxFinishedStatusEntries (30) of them, newest kept;
//  * sessions with a job running / queued are never removed while other entries are pruned,
//    also when their own new request is refused; they finish as "done" afterwards;
//  * the web queue holds at most kMaxQueuedJobs (20): the next request gets an error entry;
//    the map never exceeds 30 + running + queued (+ desktop).
//
// SqlManager is not needed for these rules: the manager gets none (the export writes a CSV
// with the header only into a temporary folder). The worker's "finished" reaches the manager
// queued, so while the test does not return to the event loop the jobs stay running/queued.
#include <QtTest>
#include <QMutex>
#include <QTemporaryDir>

#include "HistoryExport.h"
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

constexpr double kUnboundedToMs = 8640000000000000.0;   // UI contract: 0 .. this = unbounded

QString stateOf(const TaidaFlowProxy &proxy, const QString &sid)
{
    return proxy.historyExportStatus().value(sid).toMap().value(QStringLiteral("state")).toString();
}
} // namespace

class TestHistoryExportStatus : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();
    void invalidSessionIdsNotStored();
    void refusedRequestsPruned();
    void busyEntriesKept();
    void queueLimit();

private:
    QTemporaryDir m_dir;
    std::unique_ptr<TaidaFlowProxy> m_proxy;
    std::unique_ptr<HistoryExportManager> m_manager;
    int m_run = 0;
};

void TestHistoryExportStatus::initTestCase()
{
    QVERIFY(m_dir.isValid());
    g_previousHandler = qInstallMessageHandler(captureMessages);
}

void TestHistoryExportStatus::cleanupTestCase()
{
    qInstallMessageHandler(g_previousHandler);
}

void TestHistoryExportStatus::init()
{
    m_proxy = std::make_unique<TaidaFlowProxy>();
    HistoryExportManager::Options options;
    options.exportDir = m_dir.filePath(QStringLiteral("exports-%1").arg(++m_run));
    options.mountDownloads = false;   // no HTTP listener in this test
    m_manager = std::make_unique<HistoryExportManager>(m_proxy.get(), nullptr, options);
}

void TestHistoryExportStatus::cleanup()
{
    m_manager.reset();
    m_proxy.reset();
}

void TestHistoryExportStatus::invalidSessionIdsNotStored()
{
    const int before = logCount(QStringLiteral("invalid session id"));
    for (int i = 0; i < 1000; ++i) {
        // too long, spaces, dots, slashes, empty, non-ASCII
        const QString sid = (i % 5 == 0) ? QString(41 + i % 100, QLatin1Char('a'))
                          : (i % 5 == 1) ? QStringLiteral("bad id %1").arg(i)
                          : (i % 5 == 2) ? QStringLiteral("../x%1").arg(i)
                          : (i % 5 == 3) ? QString()
                                         : QStringLiteral("匯出%1").arg(i);
        m_manager->requestExport(sid, 0, kUnboundedToMs);
    }
    QCoreApplication::processEvents();
    QCOMPARE(m_proxy->historyExportStatus().size(), qsizetype(0));
    const int lines = logCount(QStringLiteral("invalid session id")) - before;
    qInfo("1000 invalid session ids -> status entries %lld, log lines %d",
          qint64(m_proxy->historyExportStatus().size()), lines);
    QCOMPARE(lines, 1);
}

void TestHistoryExportStatus::refusedRequestsPruned()
{
    for (int i = 0; i < 100; ++i)
        m_manager->requestExport(QStringLiteral("refused-%1").arg(i), 2000, 1000);   // from > to
    const QVariantMap status = m_proxy->historyExportStatus();
    qInfo("100 refused requests -> %lld entries", qint64(status.size()));
    QCOMPARE(status.size(), qsizetype(HistoryExport::kMaxFinishedStatusEntries));
    // The newest 30 are kept, with the UI fields and values unchanged.
    for (int i = 70; i < 100; ++i) {
        const QVariantMap entry = status.value(QStringLiteral("refused-%1").arg(i)).toMap();
        QCOMPARE(entry.value(QStringLiteral("state")).toString(), QStringLiteral("error"));
        QCOMPARE(entry.value(QStringLiteral("message")).toString(), QStringLiteral("匯出失敗:日期區間錯誤"));
        QStringList keys = entry.keys();
        keys.sort();
        QCOMPARE(keys, (QStringList{"downloadPort", "fileName", "message", "progress", "queuePosition",
                                    "rowsWritten", "savedPath", "state", "totalRows", "url"}));
    }
    QVERIFY(!status.contains(QStringLiteral("refused-69")));
}

void TestHistoryExportStatus::busyEntriesKept()
{
    m_manager->requestExport(QStringLiteral("busy-run"), 0, kUnboundedToMs);
    m_manager->requestExport(QStringLiteral("busy-q1"), 0, kUnboundedToMs);
    QCOMPARE(stateOf(*m_proxy, QStringLiteral("busy-run")), QStringLiteral("running"));
    QCOMPARE(stateOf(*m_proxy, QStringLiteral("busy-q1")), QStringLiteral("queued"));

    for (int i = 0; i < 100; ++i) {
        m_manager->requestExport(QStringLiteral("other-%1").arg(i), 2000, 1000);
        if (i % 10 == 0) {
            // A refused request of a busy session keeps its running / queued entry.
            m_manager->requestExport(QStringLiteral("busy-run"), 0, kUnboundedToMs);
            m_manager->requestExport(QStringLiteral("busy-q1"), 0, kUnboundedToMs);
        }
        QVERIFY(m_proxy->historyExportStatus().size() <= HistoryExport::kMaxFinishedStatusEntries + 2);
    }
    QCOMPARE(stateOf(*m_proxy, QStringLiteral("busy-run")), QStringLiteral("running"));
    QCOMPARE(stateOf(*m_proxy, QStringLiteral("busy-q1")), QStringLiteral("queued"));
    QCOMPARE(m_proxy->historyExportStatus().size(), qsizetype(HistoryExport::kMaxFinishedStatusEntries + 2));

    // Let the jobs finish (worker thread -> queued to this thread).
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(*m_proxy, QStringLiteral("busy-run")), QStringLiteral("done"), 15000);
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(*m_proxy, QStringLiteral("busy-q1")), QStringLiteral("done"), 15000);
    const QVariantMap status = m_proxy->historyExportStatus();
    qInfo("after the busy jobs finished: %lld entries", qint64(status.size()));
    QVERIFY(status.size() <= HistoryExport::kMaxFinishedStatusEntries);
    QCOMPARE(status.value(QStringLiteral("busy-run")).toMap().value(QStringLiteral("url")).toString().left(9),
             QStringLiteral("/exports/"));
}

void TestHistoryExportStatus::queueLimit()
{
    const int requests = 1 + HistoryExport::kMaxQueuedJobs + 4;
    for (int i = 0; i < requests; ++i)
        m_manager->requestExport(QStringLiteral("q-%1").arg(i), 0, kUnboundedToMs);
    QVariantMap status = m_proxy->historyExportStatus();
    QCOMPARE(status.size(), qsizetype(requests));
    QCOMPARE(stateOf(*m_proxy, QStringLiteral("q-0")), QStringLiteral("running"));
    int queued = 0;
    int refused = 0;
    for (int i = 1; i < requests; ++i) {
        const QVariantMap entry = status.value(QStringLiteral("q-%1").arg(i)).toMap();
        if (entry.value(QStringLiteral("state")).toString() == QLatin1String("queued")) {
            ++queued;
            QCOMPARE(entry.value(QStringLiteral("queuePosition")).toInt(), i);
        } else {
            QCOMPARE(entry.value(QStringLiteral("state")).toString(), QStringLiteral("error"));
            QCOMPARE(entry.value(QStringLiteral("message")).toString(), QStringLiteral("匯出排隊已達上限,請晚點再試"));
            ++refused;
        }
    }
    QCOMPARE(queued, HistoryExport::kMaxQueuedJobs);
    QCOMPARE(refused, 4);

    // Many more refused sessions while the queue is full: map <= 30 + 1 running + 20 queued.
    for (int i = 0; i < 200; ++i)
        m_manager->requestExport(QStringLiteral("more-%1").arg(i), 0, kUnboundedToMs);
    status = m_proxy->historyExportStatus();
    qInfo("queue full + 200 more requests -> %lld entries (limit %d)", qint64(status.size()),
          HistoryExport::kMaxFinishedStatusEntries + 1 + HistoryExport::kMaxQueuedJobs);
    QCOMPARE(status.size(), qsizetype(HistoryExport::kMaxFinishedStatusEntries + 1 + HistoryExport::kMaxQueuedJobs));
    for (int i = 0; i <= HistoryExport::kMaxQueuedJobs; ++i)
        QVERIFY(status.contains(QStringLiteral("q-%1").arg(i)));   // busy entries all kept

    // Everything finishes; the finished entries are pruned to 30.
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(*m_proxy, QStringLiteral("q-%1").arg(HistoryExport::kMaxQueuedJobs)),
                              QStringLiteral("done"), 30000);
    QVERIFY(m_proxy->historyExportStatus().size() <= HistoryExport::kMaxFinishedStatusEntries);
}

QTEST_MAIN(TestHistoryExportStatus)
#include "tst_historyexport_status.moc"
