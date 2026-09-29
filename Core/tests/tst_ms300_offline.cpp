// w2-072 D3 (review D-003): Ms300FaultReader while its serial port cannot be opened (a port
// name that does not exist). Real QModbusRtuSerialClient; nothing is sent anywhere.
//  * a connection is tried at most every 3 s (was every poll = every second): the attempts are
//    counted from the reader's own info lines "[MS300] <port> connection attempt #N" plus the
//    first attempt at start();
//  * warnings: the first at once, then at most one per interval with the number held back
//    (checked with the default 60 s and with a 4 s test interval).
#include <QtTest>
#include <QMutex>

#include "Ms300FaultReader.h"

namespace {
struct Line
{
    qint64 ms;
    QtMsgType type;
    QString text;
};
QMutex g_logMutex;
QList<Line> g_log;
QElapsedTimer g_clock;
QtMessageHandler g_previousHandler = nullptr;

void captureMessages(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    {
        QMutexLocker locker(&g_logMutex);
        g_log.append({g_clock.elapsed(), type, message});
    }
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

QList<Line> linesSince(qint64 since, const QString &needle, bool warningsOnly)
{
    QMutexLocker locker(&g_logMutex);
    QList<Line> lines;
    for (const Line &l : std::as_const(g_log)) {
        if (l.ms >= since && l.text.contains(needle) && (!warningsOnly || l.type == QtWarningMsg))
            lines << l;
    }
    return lines;
}

Ms300FaultReader::Settings missingPort()
{
    Ms300FaultReader::Settings settings;
    settings.serialPort = QStringLiteral("TAIDAFLOW_TEST_NO_SUCH_PORT");
    return settings;
}
} // namespace

class TestMs300Offline : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        g_clock.start();
        g_previousHandler = qInstallMessageHandler(captureMessages);
    }
    void cleanupTestCase() { qInstallMessageHandler(g_previousHandler); }
    void attemptsEvery3sOneWarning();
    void warningsHeldBackAndCounted();
};

void TestMs300Offline::attemptsEvery3sOneWarning()
{
    Ms300FaultReader reader(missingPort());
    int readErrors = 0;
    connect(&reader, &Ms300FaultReader::readError, this, [&readErrors](const QString &) { ++readErrors; });
    const qint64 start = g_clock.elapsed();
    reader.start();
    QTest::qWait(10000);
    reader.stop();

    const QList<Line> warnings = linesSince(start, QStringLiteral("[MS300]"), true);
    const QList<Line> attempts = linesSince(start, QStringLiteral("connection attempt #"), false);
    qInfo("10 s with a missing port: %lld warning line(s), %lld logged reconnect attempt(s), %d readError signal(s)",
          qint64(warnings.size()), qint64(attempts.size()), readErrors);
    for (const Line &l : warnings)
        qInfo("  warning at %lld ms: %s", l.ms - start, qPrintable(l.text));
    QCOMPARE(warnings.size(), qsizetype(1));
    // start() tries at once, then every 3 s: #2..#4 in 10 s (the poll runs every second).
    QVERIFY2(attempts.size() >= 2 && attempts.size() <= 3, qPrintable(QString::number(attempts.size())));
    qint64 previous = start;
    for (const Line &l : attempts) {
        QVERIFY2(l.ms - previous >= Ms300FaultReader::kConnectRetryMs - 100,
                 qPrintable(QStringLiteral("attempt at %1 ms, previous at %2 ms").arg(l.ms - start).arg(previous - start)));
        previous = l.ms;
    }
}

void TestMs300Offline::warningsHeldBackAndCounted()
{
    Ms300FaultReader reader(missingPort());
    reader.setWarningIntervalMs(4000);
    const qint64 start = g_clock.elapsed();
    reader.start();
    QTest::qWait(10500);
    reader.stop();
    const QList<Line> warnings = linesSince(start, QStringLiteral("[MS300]"), true);
    for (const Line &l : warnings)
        qInfo("  warning at %lld ms: %s", l.ms - start, qPrintable(l.text));
    // attempts at ~0, 3, 6, 9 s; each gives 2 warnings (errorOccurred + "Connection request
    // failed"); 4 s interval -> written at ~0, ~6 s (and possibly ~9 s? no: < 4 s after 6 s).
    QVERIFY2(warnings.size() >= 2 && warnings.size() <= 3, qPrintable(QString::number(warnings.size())));
    QVERIFY(warnings.at(1).text.contains(QLatin1String("similar warning(s) held back")));
}

QTEST_GUILESS_MAIN(TestMs300Offline)
#include "tst_ms300_offline.moc"
