// w2-084 D1/D3: the Core's server heartbeat (Core/ServerHeartbeat.h/.cpp, wired in Core::init() /
// Core::shutdown()). Real ServerHeartbeat + real TaidaFlowProxy (the w1-083 contract); no device,
// no port, no database.
//
//  * Core configuration (default constructor = what Core::init() creates): interval
//    TaidaFlowProxy::kServerHeartbeatIntervalMs (1000 ms), wall clock. start() writes the first
//    value at once (= now), then serverHeartbeatMs changes about every second (measured with the
//    real 1 s timer over 6.5 s: 7 values, steps 1 s apart, values = epoch ms), always on the
//    thread of the Proxy (main thread).
//  * stop() (Core::shutdown() calls it first): no change any more (watched 3.5 s), idempotent.
//  * Injected clock + short interval (test build): the written values are exactly the clock's.
//  * A busy worker thread (like the SqlManager thread) does not delay the heartbeat.
//  * start() twice, start() off the Proxy's thread (refused), Proxy destroyed while running.
//  * Wiring in the sources (code review, read from the files): Core::init() creates and starts it,
//    Core::shutdown() stops it before anything else, ServerHeartbeat is desktop-only in
//    Core/CMakeLists.txt (the WebAssembly page never writes), no other C++ source writes
//    serverHeartbeatMs, and ServerHeartbeat does not use SqlManager.
#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QThread>

#include <atomic>
#include <cmath>

#include "ServerHeartbeat.h"
#include "TaidaFlowProxy.h"

#ifndef CORE_SOURCE_DIR
#error CORE_SOURCE_DIR must point to the Core folder (Core/tests/CMakeLists.txt)
#endif

namespace {
struct Change
{
    double value = 0;
    qint64 atMs = 0;        // QElapsedTimer (monotonic) of the test when the signal arrived
    qint64 wallMs = 0;      // wall clock of the test at that moment
    bool onProxyThread = false;
};

QString readSource(const QString &relative)
{
    QFile file(QDir(QStringLiteral(CORE_SOURCE_DIR)).filePath(relative));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

// Body of "void Core::<name>(" up to the next "\n}\n" (functions of core.cpp end with a column-0 brace).
QString functionBody(const QString &source, const QString &signature)
{
    const qsizetype start = source.indexOf(signature);
    if (start < 0)
        return QString();
    const qsizetype end = source.indexOf(QStringLiteral("\n}\n"), start);
    return end < 0 ? QString() : source.mid(start, end - start + 2);
}
} // namespace

class TestServerHeartbeat : public QObject
{
    Q_OBJECT

private slots:
    void contractDefaults();
    void startWritesAtOnce();
    void everySecondWithTheCoreConfiguration();
    void injectedClockShortInterval();
    void busyWorkerThreadDoesNotDelay();
    void startTwiceAndOffThread();
    void proxyDestroyedWhileRunning();
    void coreWiringInTheSources();
};

void TestServerHeartbeat::contractDefaults()
{
    QCOMPARE(TaidaFlowProxy::kServerHeartbeatIntervalMs, 1000);
    TaidaFlowProxy proxy;
    QCOMPARE(proxy.serverHeartbeatMs(), 0.0);           // "no heartbeat yet" until the Core starts it
    ServerHeartbeat heartbeat(&proxy);
    QCOMPARE(heartbeat.intervalMs(), TaidaFlowProxy::kServerHeartbeatIntervalMs);
    QVERIFY(!heartbeat.isActive());
    QCOMPARE(heartbeat.beatCount(), quint64(0));
    QCOMPARE(proxy.serverHeartbeatMs(), 0.0);           // constructing writes nothing
}

void TestServerHeartbeat::startWritesAtOnce()
{
    TaidaFlowProxy proxy;
    ServerHeartbeat heartbeat(&proxy);
    QSignalSpy spy(&proxy, &TaidaFlowProxy::serverHeartbeatMsChanged);
    const qint64 before = QDateTime::currentMSecsSinceEpoch();
    heartbeat.start();
    const qint64 after = QDateTime::currentMSecsSinceEpoch();
    // Synchronously, before any event loop iteration.
    QCOMPARE(spy.count(), 1);
    QCOMPARE(heartbeat.beatCount(), quint64(1));
    QVERIFY(heartbeat.isActive());
    const double value = proxy.serverHeartbeatMs();
    QVERIFY2(value >= double(before) && value <= double(after),
             qPrintable(QStringLiteral("value %1 not in [%2, %3]").arg(qint64(value)).arg(before).arg(after)));
    heartbeat.stop();
}

void TestServerHeartbeat::everySecondWithTheCoreConfiguration()
{
    TaidaFlowProxy proxy;
    ServerHeartbeat heartbeat(&proxy);                  // exactly what Core::init() creates
    QList<Change> changes;
    QElapsedTimer clock;
    clock.start();
    QObject::connect(&proxy, &TaidaFlowProxy::serverHeartbeatMsChanged, &proxy, [&]() {
        changes.append(Change{proxy.serverHeartbeatMs(), clock.elapsed(), QDateTime::currentMSecsSinceEpoch(),
                              QThread::currentThread() == proxy.thread()});
    });
    heartbeat.start();
    QTest::qWait(6500);
    // t = 0 (start), 1, 2, 3, 4, 5, 6 s.
    qInfo().noquote() << QStringLiteral("changes in 6.5 s: %1").arg(changes.size());
    for (int i = 0; i < changes.size(); ++i) {
        const Change &c = changes.at(i);
        qInfo().noquote() << QStringLiteral("  #%1 at %2 ms value %3 (%4)")
                                     .arg(i).arg(c.atMs).arg(qint64(c.value))
                                     .arg(QDateTime::fromMSecsSinceEpoch(qint64(c.value)).toString(Qt::ISODateWithMs));
    }
    QCOMPARE(changes.size(), 7);
    QCOMPARE(heartbeat.beatCount(), quint64(7));
    QVERIFY(changes.first().atMs < 100);                // first value at once
    for (int i = 0; i < changes.size(); ++i) {
        const Change &c = changes.at(i);
        QVERIFY(c.onProxyThread);
        QVERIFY2(std::abs(c.value - double(c.wallMs)) < 100.0, "value is the current epoch ms");
        if (i == 0)
            continue;
        const qint64 step = c.atMs - changes.at(i - 1).atMs;
        const double valueStep = c.value - changes.at(i - 1).value;
        QVERIFY2(step >= 900 && step <= 1200, qPrintable(QStringLiteral("step %1 ms").arg(step)));
        QVERIFY2(valueStep >= 900 && valueStep <= 1200, qPrintable(QStringLiteral("value step %1 ms").arg(valueStep)));
    }

    // Core::shutdown(): stop() first -> no change any more.
    heartbeat.stop();
    QVERIFY(!heartbeat.isActive());
    const double frozen = proxy.serverHeartbeatMs();
    const int countAtStop = changes.size();
    QTest::qWait(3500);
    QCOMPARE(changes.size(), countAtStop);
    QCOMPARE(proxy.serverHeartbeatMs(), frozen);
    QCOMPARE(heartbeat.beatCount(), quint64(countAtStop));
    heartbeat.stop();                                   // idempotent
    QVERIFY(!heartbeat.isActive());
    QCOMPARE(proxy.serverHeartbeatMs(), frozen);
}

void TestServerHeartbeat::injectedClockShortInterval()
{
    TaidaFlowProxy proxy;
    qint64 fake = 1'790'000'000'000;                    // injected wall clock
    QList<double> written;
    ServerHeartbeat heartbeat(&proxy, 20, [&fake]() { return fake += 1000; });
    QCOMPARE(heartbeat.intervalMs(), 20);
    QObject::connect(&proxy, &TaidaFlowProxy::serverHeartbeatMsChanged, &proxy,
                     [&]() { written.append(proxy.serverHeartbeatMs()); });
    heartbeat.start();
    QTRY_VERIFY_WITH_TIMEOUT(written.size() >= 50, 10000);
    heartbeat.stop();
    const int n = written.size();
    for (int i = 0; i < n; ++i)
        QCOMPARE(written.at(i), double(1'790'000'000'000 + 1000LL * (i + 1)));
    QTest::qWait(300);                                  // 15 intervals: nothing after stop()
    QCOMPARE(written.size(), n);
    QCOMPARE(heartbeat.beatCount(), quint64(n));

    // An invalid interval falls back to the contract's 1 s.
    ServerHeartbeat fallback(&proxy, 0, nullptr);
    QCOMPARE(fallback.intervalMs(), TaidaFlowProxy::kServerHeartbeatIntervalMs);
}

void TestServerHeartbeat::busyWorkerThreadDoesNotDelay()
{
    // The SqlManager work runs on its own thread; the heartbeat has no call into it. A worker thread
    // that is busy for 3.2 s (like a long database query) does not delay the heartbeat.
    TaidaFlowProxy proxy;
    ServerHeartbeat heartbeat(&proxy);
    std::atomic<bool> workerRunning{false};
    QThread *worker = QThread::create([&workerRunning]() {
        workerRunning = true;
        QThread::msleep(3200);
        workerRunning = false;
    });
    worker->start();
    QTRY_VERIFY(workerRunning.load());
    QSignalSpy spy(&proxy, &TaidaFlowProxy::serverHeartbeatMsChanged);
    heartbeat.start();
    QTest::qWait(3100);
    QVERIFY(workerRunning.load());                      // still busy during the whole window
    QCOMPARE(spy.count(), 4);                           // t = 0, 1, 2, 3 s
    heartbeat.stop();
    QVERIFY(worker->wait(5000));
    delete worker;
}

void TestServerHeartbeat::startTwiceAndOffThread()
{
    TaidaFlowProxy proxy;
    ServerHeartbeat heartbeat(&proxy);
    heartbeat.start();
    heartbeat.start();                                  // running: no second immediate write
    QCOMPARE(heartbeat.beatCount(), quint64(1));
    heartbeat.stop();

    // start() on another thread than the Proxy's is refused (the Proxy and the Mirror live on the
    // main thread).
    TaidaFlowProxy proxy2;
    ServerHeartbeat offThread(&proxy2);
    QTest::ignoreMessage(QtWarningMsg, "[Heartbeat] start() not on the Proxy's thread - server heartbeat NOT started");
    QThread *other = QThread::create([&offThread]() { offThread.start(); });
    other->start();
    QVERIFY(other->wait(5000));
    delete other;
    QVERIFY(!offThread.isActive());
    QCOMPARE(offThread.beatCount(), quint64(0));
    QCOMPARE(proxy2.serverHeartbeatMs(), 0.0);
}

void TestServerHeartbeat::proxyDestroyedWhileRunning()
{
    auto *proxy = new TaidaFlowProxy;
    ServerHeartbeat heartbeat(proxy, 20, nullptr);
    heartbeat.start();
    QCOMPARE(heartbeat.beatCount(), quint64(1));
    delete proxy;                                       // ServerHeartbeat keeps a QPointer
    QTest::qWait(200);
    QCOMPARE(heartbeat.beatCount(), quint64(1));        // no write into a destroyed Proxy
    heartbeat.stop();
}

void TestServerHeartbeat::coreWiringInTheSources()
{
    const QString core = readSource(QStringLiteral("core.cpp"));
    QVERIFY(!core.isEmpty());
    QVERIFY(core.contains(QStringLiteral("#include \"ServerHeartbeat.h\"")));

    // Core::init(): created with the Core's defaults (1 s, wall clock) and started, on the main thread.
    const QString init = functionBody(core, QStringLiteral("void Core::init()"));
    QVERIFY(!init.isEmpty());
    const qsizetype create = init.indexOf(QStringLiteral("m_heartbeat = new ServerHeartbeat(m_proxy, this);"));
    const qsizetype start = init.indexOf(QStringLiteral("m_heartbeat->start();"));
    QVERIFY(create > 0);
    QVERIFY(start > create);
    QVERIFY(init.indexOf(QStringLiteral("m_proxy = new TaidaFlowProxy(this);")) < create);

    // Core::shutdown(): the heartbeat is stopped right after the "already shut down" guard, before
    // anything else is stopped or logged.
    const QString shutdown = functionBody(core, QStringLiteral("void Core::shutdown(const char *reason)"));
    QVERIFY(!shutdown.isEmpty());
    const qsizetype guard = shutdown.indexOf(QStringLiteral("m_shutDown = true;"));
    const qsizetype stop = shutdown.indexOf(QStringLiteral("m_heartbeat->stop();"));
    QVERIFY(guard > 0);
    QVERIFY(stop > guard);
    const QString between = shutdown.mid(guard, stop - guard);
    QVERIFY2(!between.contains(QStringLiteral("m_manager")) && !between.contains(QStringLiteral("qInfo"))
                     && !between.contains(QStringLiteral("m_sqlManager")),
             "nothing between the guard and the heartbeat stop");
    for (const char *later : {"stopping the backend", "m_manager->stop()", "m_modbusServer->stop()",
                              "stopRestServer()", "AppHttpServer::instance().stop()", "m_sqlManager->shutdown()"}) {
        const qsizetype at = shutdown.indexOf(QLatin1String(later));
        QVERIFY2(at > stop, later);
    }

    // Desktop only: listed inside if(TAIDAFLOW_IS_WINDOWS_DESKTOP) of Core/CMakeLists.txt.
    const QString cmake = readSource(QStringLiteral("CMakeLists.txt"));
    const qsizetype desktopIf = cmake.indexOf(QStringLiteral("if(TAIDAFLOW_IS_WINDOWS_DESKTOP)"));
    const qsizetype desktopEnd = cmake.indexOf(QStringLiteral("endif()"), desktopIf);
    const qsizetype listed = cmake.indexOf(QStringLiteral("ServerHeartbeat.cpp"));
    QVERIFY(desktopIf > 0 && listed > desktopIf && listed < desktopEnd);
    QCOMPARE(cmake.count(QStringLiteral("ServerHeartbeat.cpp")), 1);

    // No database: ServerHeartbeat includes neither SqlManager nor anything but Qt and the Proxy.
    const QString impl = readSource(QStringLiteral("ServerHeartbeat.cpp")) + readSource(QStringLiteral("ServerHeartbeat.h"));
    QVERIFY(!impl.contains(QStringLiteral("#include \"SqlManager")));
    QVERIFY(!impl.contains(QStringLiteral("SqlManager::")));

    // The only writer: no other C++ source of Core/ or App/ calls setServerHeartbeatMs.
    QStringList writers;
    const QDir root(QStringLiteral(CORE_SOURCE_DIR "/.."));
    for (const QString &folder : {QStringLiteral("Core"), QStringLiteral("App")}) {
        QDirIterator it(root.filePath(folder), {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            if (path.contains(QStringLiteral("/tests/")) || path.contains(QStringLiteral("/autogen/")))
                continue;
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly))
                continue;
            if (QString::fromUtf8(f.readAll()).contains(QStringLiteral("setServerHeartbeatMs(")))
                writers << root.relativeFilePath(path);
        }
    }
    writers.sort();
    qInfo().noquote() << "files calling setServerHeartbeatMs(:" << writers.join(QStringLiteral(", "));
    QCOMPARE(writers, (QStringList{QStringLiteral("Core/ServerHeartbeat.cpp"), QStringLiteral("Core/TaidaFlowProxy.h")}));
}

QTEST_MAIN(TestServerHeartbeat)
#include "tst_server_heartbeat.moc"
