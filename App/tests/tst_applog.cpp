// w2-064: AppLog / AppLogWriter (App/applog.{h,cpp}) - docs/taidaflow_config_spec.md §2 "log".
// Real files in a QTemporaryDir, real threads, the real Qt message handler; the only injected
// thing is the date/time source (a settable clock) so that date changes can be tested.
#include "applog.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <memory>
#include <vector>

Q_LOGGING_CATEGORY(lcAppLogTest, "taidaflow.applogtest")

namespace {

// Settable local date/time for AppLogWriter.
struct FakeClock
{
    std::shared_ptr<QDateTime> time = std::make_shared<QDateTime>();
    FakeClock() = default;
    explicit FakeClock(const QDateTime &t) { *time = t; }
    void set(const QDateTime &t) { *time = t; }
    void advanceMs(qint64 ms) { *time = time->addMSecs(ms); }
    AppLogWriter::Clock clock() const
    {
        const std::shared_ptr<QDateTime> p = time;
        return [p] { return *p; };
    }
};

QDateTime at(int y, int m, int d, int h = 10, int min = 0, int s = 0, int ms = 0)
{
    return QDateTime(QDate(y, m, d), QTime(h, min, s, ms));
}

AppConfig::LogSettings settings(const QString &dir, bool quiet = true, int quietKeep = 60, bool full = true,
                                int fullKeep = 7)
{
    AppConfig::LogSettings s;
    s.dir = dir;
    s.quiet.enabled = quiet;
    s.quiet.keepDays = quietKeep;
    s.full.enabled = full;
    s.full.keepDays = fullKeep;
    return s;
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

bool touch(const QString &path, const QByteArray &bytes = QByteArray("x"))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(bytes) == bytes.size();
}

// Lines without the CRLF; fails the check when a line break is not CRLF.
QStringList lines(const QString &path)
{
    const QByteArray bytes = readAll(path);
    if (bytes.isEmpty())
        return {};
    QString text = QString::fromUtf8(bytes);
    if (text.endsWith(QStringLiteral("\r\n")))
        text.chop(2);
    return text.split(QStringLiteral("\r\n"));
}

// Lines of messages (AppLog's own "[AppLog] ..." lines left out).
QStringList messageLines(const QString &path)
{
    QStringList result;
    for (const QString &line : lines(path)) {
        if (!line.contains(QStringLiteral("] [AppLog] ")))
            result.append(line);
    }
    return result;
}

bool crlfOnly(const QByteArray &bytes)
{
    return bytes.count('\n') == bytes.count("\r\n") && bytes.count('\r') == bytes.count("\r\n");
}

QString quietPath(const QString &dir, const QDate &date)
{
    return QDir(dir).filePath(AppLogWriter::quietFileName(date));
}

QString fullPath(const QString &dir, const QDate &date)
{
    return QDir(dir).filePath(AppLogWriter::fullFileName(date));
}

const QRegularExpression &linePattern()
{
    static const QRegularExpression re(QStringLiteral(
        "^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3} \\[(debug|info|warning|critical|fatal)\\] "));
    return re;
}

} // namespace

class tst_AppLog : public QObject
{
    Q_OBJECT

private slots:
    void fileNames();
    void formatLine();
    void bothFilesAndLevelFilter();
    void timestampAndCategory();
    void createsMissingFolder();
    void dateChangeForward();
    void dateChangeBackward();
    void appendsSameDay();
    void retentionBoundaries_data();
    void retentionBoundaries();
    void onlyMatchingFilesDeleted();
    void cleanupOnStartAndDateChange();
    void disabledModes();
    void writeFailureInvalidPath();
    void writeFailureReadOnlyFile();
    void writeFailureInvalidCharacters();
    void multithreadedWriter();
    void handlerBuffersAndWrites();
    void handlerMultithreaded();
    void cleanupTiming();
    void describe();
    void configFailureWritesFallbackLog_data();
    void configFailureWritesFallbackLog();
    void configFailureFallbackUnwritable();
    void bufferLimit();  // last: floods the QTest message counter
};

void tst_AppLog::fileNames()
{
    QCOMPARE(AppLogWriter::quietFileName(QDate(2026, 9, 28)), QStringLiteral("taidaflow-2026-09-28.log"));
    QCOMPARE(AppLogWriter::fullFileName(QDate(2026, 9, 28)), QStringLiteral("taidaflow-2026-09-28-full.log"));
    QCOMPARE(AppLogWriter::fullFileName(QDate(2027, 1, 5)), QStringLiteral("taidaflow-2027-01-05-full.log"));
    QVERIFY(AppLogWriter::goesToQuiet(QtWarningMsg));
    QVERIFY(AppLogWriter::goesToQuiet(QtCriticalMsg));
    QVERIFY(AppLogWriter::goesToQuiet(QtFatalMsg));
    QVERIFY(!AppLogWriter::goesToQuiet(QtInfoMsg));
    QVERIFY(!AppLogWriter::goesToQuiet(QtDebugMsg));
}

void tst_AppLog::formatLine()
{
    const QDateTime t = at(2026, 9, 28, 14, 3, 5, 7);
    QCOMPARE(AppLogWriter::formatLine(t, QtInfoMsg, nullptr, QStringLiteral("hello")),
             QStringLiteral("2026-09-28 14:03:05.007 [info] hello\r\n"));
    QCOMPARE(AppLogWriter::formatLine(t, QtWarningMsg, "default", QStringLiteral("x")),
             QStringLiteral("2026-09-28 14:03:05.007 [warning] x\r\n"));
    QCOMPARE(AppLogWriter::formatLine(t, QtCriticalMsg, "qt.network.ssl", QStringLiteral("bad cert")),
             QStringLiteral("2026-09-28 14:03:05.007 [critical] qt.network.ssl: bad cert\r\n"));
    QCOMPARE(AppLogWriter::formatLine(t, QtDebugMsg, "", QStringLiteral("d")),
             QStringLiteral("2026-09-28 14:03:05.007 [debug] d\r\n"));
    QCOMPARE(AppLogWriter::formatLine(t, QtFatalMsg, nullptr, QStringLiteral("f")),
             QStringLiteral("2026-09-28 14:03:05.007 [fatal] f\r\n"));
    // The text is kept whole (brackets, %1, non-ASCII, spaces); only line breaks become CRLF.
    const QString text = QStringLiteral("[Config]   a = \"%1\" 中文 ✓\ttab\nsecond\r\nthird\n");
    QCOMPARE(AppLogWriter::formatLine(t, QtInfoMsg, nullptr, text),
             QStringLiteral("2026-09-28 14:03:05.007 [info] [Config]   a = \"%1\" 中文 ✓\ttab\r\nsecond\r\nthird\r\n\r\n"));
}

void tst_AppLog::bothFilesAndLevelFilter()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.filePath(QStringLiteral("logs"));
    FakeClock clock(at(2026, 9, 28, 10));
    {
        AppLogWriter writer(settings(dir), clock.clock());
        writer.start();
        QVERIFY(writer.isQuietOpen());
        QVERIFY(writer.isFullOpen());
        writer.write(QtDebugMsg, nullptr, QStringLiteral("m-debug"));
        writer.write(QtInfoMsg, nullptr, QStringLiteral("m-info"));
        writer.write(QtWarningMsg, nullptr, QStringLiteral("m-warning"));
        writer.write(QtCriticalMsg, nullptr, QStringLiteral("m-critical"));
        writer.write(QtFatalMsg, nullptr, QStringLiteral("m-fatal"));
    }
    const QDate day(2026, 9, 28);
    const QByteArray quietBytes = readAll(quietPath(dir, day));
    const QByteArray fullBytes = readAll(fullPath(dir, day));
    QCOMPARE(QDir(dir).entryList(QDir::Files, QDir::Name),
             QStringList({QStringLiteral("taidaflow-2026-09-28-full.log"), QStringLiteral("taidaflow-2026-09-28.log")}));
    // UTF-8 without BOM, CRLF only.
    QVERIFY(!quietBytes.startsWith("\xEF\xBB\xBF"));
    QVERIFY(!fullBytes.startsWith("\xEF\xBB\xBF"));
    QVERIFY(crlfOnly(quietBytes));
    QVERIFY(crlfOnly(fullBytes));
    QVERIFY(fullBytes.endsWith("\r\n"));

    QCOMPARE(lines(quietPath(dir, day)),
             QStringList({QStringLiteral("2026-09-28 10:00:00.000 [warning] m-warning"),
                          QStringLiteral("2026-09-28 10:00:00.000 [critical] m-critical"),
                          QStringLiteral("2026-09-28 10:00:00.000 [fatal] m-fatal")}));   // no info/debug, no [AppLog] info
    QCOMPARE(messageLines(fullPath(dir, day)),
             QStringList({QStringLiteral("2026-09-28 10:00:00.000 [debug] m-debug"),
                          QStringLiteral("2026-09-28 10:00:00.000 [info] m-info"),
                          QStringLiteral("2026-09-28 10:00:00.000 [warning] m-warning"),
                          QStringLiteral("2026-09-28 10:00:00.000 [critical] m-critical"),
                          QStringLiteral("2026-09-28 10:00:00.000 [fatal] m-fatal")}));
    // The start-up clean-up note is in the full file (info).
    QVERIFY(QString::fromUtf8(fullBytes).contains(QStringLiteral("[info] [AppLog] clean-up of ")));
}

void tst_AppLog::timestampAndCategory()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    FakeClock clock(at(2026, 9, 28, 23, 59, 58, 999));
    AppLogWriter writer(settings(tmp.path()), clock.clock());
    writer.write(QtInfoMsg, "taidaflow.modbus", QStringLiteral("read ok"));
    clock.advanceMs(1);   // 23:59:59.000
    writer.write(QtWarningMsg, "default", QStringLiteral("plain"));
    // An explicit time (buffered message) is written instead of the clock's.
    writer.write(QtWarningMsg, "qt.qpa", QStringLiteral("early"), at(2026, 9, 28, 8, 1, 2, 30));
    const QStringList full = messageLines(fullPath(tmp.path(), QDate(2026, 9, 28)));
    QCOMPARE(full, QStringList({QStringLiteral("2026-09-28 23:59:58.999 [info] taidaflow.modbus: read ok"),
                                QStringLiteral("2026-09-28 23:59:59.000 [warning] plain"),
                                QStringLiteral("2026-09-28 08:01:02.030 [warning] qt.qpa: early")}));
    for (const QString &line : lines(fullPath(tmp.path(), QDate(2026, 9, 28))))
        QVERIFY2(linePattern().match(line).hasMatch(), qPrintable(line));
}

void tst_AppLog::createsMissingFolder()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.filePath(QStringLiteral("a/b/c logs"));
    QVERIFY(!QFileInfo::exists(dir));
    FakeClock clock(at(2026, 9, 28));
    AppLogWriter writer(settings(dir), clock.clock());
    writer.start();
    QVERIFY(QFileInfo(dir).isDir());
    QVERIFY(QFileInfo::exists(quietPath(dir, QDate(2026, 9, 28))));   // created at start, even while empty
    QVERIFY(QFileInfo::exists(fullPath(dir, QDate(2026, 9, 28))));
    QCOMPARE(QFileInfo(quietPath(dir, QDate(2026, 9, 28))).size(), qint64(0));
}

void tst_AppLog::dateChangeForward()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.path();
    // 2026-09-22 full: kept on 09-28 (7 days = 09-22..09-28), expired on 09-29.
    QVERIFY(touch(fullPath(dir, QDate(2026, 9, 22))));
    FakeClock clock(at(2026, 9, 28, 23, 59, 59, 900));
    AppLogWriter writer(settings(dir), clock.clock());
    writer.write(QtWarningMsg, nullptr, QStringLiteral("before midnight"));
    QCOMPARE(writer.currentDate(), QDate(2026, 9, 28));
    QVERIFY(QFileInfo::exists(fullPath(dir, QDate(2026, 9, 22))));

    clock.set(at(2026, 9, 29, 0, 0, 0, 100));
    writer.write(QtWarningMsg, nullptr, QStringLiteral("after midnight"));
    QCOMPARE(writer.currentDate(), QDate(2026, 9, 29));

    QCOMPARE(messageLines(quietPath(dir, QDate(2026, 9, 28))),
             QStringList({QStringLiteral("2026-09-28 23:59:59.900 [warning] before midnight")}));
    QCOMPARE(messageLines(quietPath(dir, QDate(2026, 9, 29))),
             QStringList({QStringLiteral("2026-09-29 00:00:00.100 [warning] after midnight")}));
    QCOMPARE(messageLines(fullPath(dir, QDate(2026, 9, 29))),
             QStringList({QStringLiteral("2026-09-29 00:00:00.100 [warning] after midnight")}));
    // Notes in both full files, clean-up for the new date.
    QVERIFY(QString::fromUtf8(readAll(fullPath(dir, QDate(2026, 9, 28))))
                .contains(QStringLiteral("[AppLog] the date changed from 2026-09-28 to 2026-09-29")));
    QVERIFY(QString::fromUtf8(readAll(fullPath(dir, QDate(2026, 9, 29))))
                .contains(QStringLiteral("[AppLog] continued from 2026-09-28")));
    QVERIFY(!QFileInfo::exists(fullPath(dir, QDate(2026, 9, 22))));
    QCOMPARE(writer.lastCleanup().deleted, QStringList({QStringLiteral("taidaflow-2026-09-22-full.log")}));
}

void tst_AppLog::dateChangeBackward()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.path();
    FakeClock clock(at(2026, 9, 28, 12));
    AppLogWriter writer(settings(dir, true, 60, true, 1), clock.clock());
    writer.write(QtWarningMsg, nullptr, QStringLiteral("on the 28th"));

    // System time set back 8 days: new files for 09-20; the 09-28 files (in the future now) stay,
    // even with full keepDays = 1.
    clock.set(at(2026, 9, 20, 12));
    writer.write(QtWarningMsg, nullptr, QStringLiteral("on the 20th"));
    QCOMPARE(writer.currentDate(), QDate(2026, 9, 20));
    QCOMPARE(messageLines(quietPath(dir, QDate(2026, 9, 20))),
             QStringList({QStringLiteral("2026-09-20 12:00:00.000 [warning] on the 20th")}));
    QVERIFY(QFileInfo::exists(quietPath(dir, QDate(2026, 9, 28))));
    QVERIFY(QFileInfo::exists(fullPath(dir, QDate(2026, 9, 28))));
    QVERIFY(writer.lastCleanup().deleted.isEmpty());

    // Back to the right time: appended to the existing 09-28 files; with full keepDays = 1 the
    // 09-20 full file is now expired.
    clock.set(at(2026, 9, 28, 12, 5));
    writer.write(QtWarningMsg, nullptr, QStringLiteral("28th again"));
    QCOMPARE(messageLines(quietPath(dir, QDate(2026, 9, 28))),
             QStringList({QStringLiteral("2026-09-28 12:00:00.000 [warning] on the 28th"),
                          QStringLiteral("2026-09-28 12:05:00.000 [warning] 28th again")}));
    QVERIFY(!QFileInfo::exists(fullPath(dir, QDate(2026, 9, 20))));
    QVERIFY(QFileInfo::exists(quietPath(dir, QDate(2026, 9, 20))));   // quiet keeps 60 days
}

void tst_AppLog::appendsSameDay()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    FakeClock clock(at(2026, 9, 28, 9));
    {
        AppLogWriter first(settings(tmp.path()), clock.clock());
        first.write(QtWarningMsg, nullptr, QStringLiteral("first run"));
    }
    clock.set(at(2026, 9, 28, 17));
    {
        AppLogWriter second(settings(tmp.path()), clock.clock());   // program restarted
        second.write(QtWarningMsg, nullptr, QStringLiteral("second run"));
    }
    QCOMPARE(messageLines(quietPath(tmp.path(), QDate(2026, 9, 28))),
             QStringList({QStringLiteral("2026-09-28 09:00:00.000 [warning] first run"),
                          QStringLiteral("2026-09-28 17:00:00.000 [warning] second run")}));
}

void tst_AppLog::retentionBoundaries_data()
{
    QTest::addColumn<int>("quietKeep");
    QTest::addColumn<int>("fullKeep");
    QTest::addColumn<QDate>("quietOldestKept");
    QTest::addColumn<QDate>("fullOldestKept");
    // today = 2026-09-28; oldest kept = today - (keepDays - 1)
    QTest::newRow("defaults 60/7") << 60 << 7 << QDate(2026, 7, 31) << QDate(2026, 9, 22);
    QTest::newRow("1/1 today only") << 1 << 1 << QDate(2026, 9, 28) << QDate(2026, 9, 28);
    QTest::newRow("7/60") << 7 << 60 << QDate(2026, 9, 22) << QDate(2026, 7, 31);
}

void tst_AppLog::retentionBoundaries()
{
    QFETCH(int, quietKeep);
    QFETCH(int, fullKeep);
    QFETCH(QDate, quietOldestKept);
    QFETCH(QDate, fullOldestKept);
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QDate today(2026, 9, 28);
    QCOMPARE(today.addDays(-(quietKeep - 1)), quietOldestKept);
    QCOMPARE(today.addDays(-(fullKeep - 1)), fullOldestKept);
    // today - 70 .. today + 3 (future files: system time was set back at some point)
    for (int offset = -70; offset <= 3; ++offset) {
        QVERIFY(touch(quietPath(tmp.path(), today.addDays(offset))));
        QVERIFY(touch(fullPath(tmp.path(), today.addDays(offset))));
    }
    const AppLogWriter::CleanupResult r = AppLogWriter::cleanup(tmp.path(), today, quietKeep, fullKeep);
    QVERIFY(r.failed.isEmpty());
    QCOMPARE(r.examined, 2 * 74);
    for (int offset = -70; offset <= 3; ++offset) {
        const QDate date = today.addDays(offset);
        QCOMPARE(QFileInfo::exists(quietPath(tmp.path(), date)), date >= quietOldestKept);
        QCOMPARE(QFileInfo::exists(fullPath(tmp.path(), date)), date >= fullOldestKept);
    }
    const int expectedDeleted = int(quietOldestKept.daysTo(today.addDays(-70)) * -1)
                                + int(fullOldestKept.daysTo(today.addDays(-70)) * -1);
    QCOMPARE(r.deleted.size(), expectedDeleted);
}

void tst_AppLog::onlyMatchingFilesDeleted()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.path();
    const QStringList keep = {
        QStringLiteral("taidaflow-20260101-120000.log"),          // old launcher logs
        QStringLiteral("taidaflow-20260101-120000.log.stdout"),
        QStringLiteral("launcher.log"),
        QStringLiteral("access.log"),                               // nginx
        QStringLiteral("error.log"),
        QStringLiteral("taidaflow-2020-01-01.log.bak"),
        QStringLiteral("taidaflow-2020-01-01-full.log.old"),
        QStringLiteral("taidaflow-2020-01-01.txt"),
        QStringLiteral("taidaflow-2020-01-01-quiet.log"),
        QStringLiteral("taidaflow-2020-1-1.log"),
        QStringLiteral("taidaflow-2020-01-01.log.log"),
        QStringLiteral("xtaidaflow-2020-01-01.log"),
        QStringLiteral("taidaflow-2020-01-01 .log"),
        QStringLiteral("Taidaflow-2020-02-01.log"),                // other case: not ours (Windows: different date, same name otherwise = same file)
        QStringLiteral("taidaflow-2020-02-01-FULL.log"),
        QStringLiteral("taidaflow-2020-13-01.log"),                // not a date
        QStringLiteral("taidaflow-2021-02-30-full.log"),           // not a date
        QStringLiteral("taidaflow-２０２０-01-01.log"),             // full-width digits
        QStringLiteral("my notes.txt"),
    };
    for (const QString &name : keep)
        QVERIFY2(touch(QDir(dir).filePath(name)), qPrintable(name));
    // Sub-folder contents and a folder with a matching name: never touched.
    QVERIFY(touch(QDir(dir).filePath(QStringLiteral("old/taidaflow-2020-01-01.log"))));
    QVERIFY(QDir(dir).mkpath(QStringLiteral("taidaflow-2020-01-02.log")));
    // Expired files of this program: deleted.
    const QStringList expired = {QStringLiteral("taidaflow-2020-01-01.log"),
                                 QStringLiteral("taidaflow-2020-01-01-full.log"),
                                 QStringLiteral("taidaflow-2026-09-27.log"),
                                 QStringLiteral("taidaflow-2026-09-27-full.log")};
    for (const QString &name : expired)
        QVERIFY(touch(QDir(dir).filePath(name)));

    const AppLogWriter::CleanupResult r = AppLogWriter::cleanup(dir, QDate(2026, 9, 28), 1, 1);
    QVERIFY(r.failed.isEmpty());
    QStringList deleted = r.deleted;
    deleted.sort();
    QStringList expectedDeleted = expired;
    expectedDeleted.sort();
    QCOMPARE(deleted, expectedDeleted);
    for (const QString &name : expired)
        QVERIFY2(!QFileInfo::exists(QDir(dir).filePath(name)), qPrintable(name));
    for (const QString &name : keep)
        QVERIFY2(QFileInfo::exists(QDir(dir).filePath(name)), qPrintable(name));
    QVERIFY(QFileInfo::exists(QDir(dir).filePath(QStringLiteral("old/taidaflow-2020-01-01.log"))));
    QVERIFY(QFileInfo(QDir(dir).filePath(QStringLiteral("taidaflow-2020-01-02.log"))).isDir());
}

void tst_AppLog::cleanupOnStartAndDateChange()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.path();
    QVERIFY(touch(quietPath(dir, QDate(2026, 7, 30))));   // 60 days: expired on 09-28
    QVERIFY(touch(quietPath(dir, QDate(2026, 7, 31))));   // kept on 09-28, expired on 09-29
    QVERIFY(touch(fullPath(dir, QDate(2026, 9, 21))));    // 7 days: expired on 09-28
    QVERIFY(touch(fullPath(dir, QDate(2026, 9, 22))));    // kept on 09-28, expired on 09-29
    FakeClock clock(at(2026, 9, 28, 8));
    AppLogWriter writer(settings(dir), clock.clock());
    writer.start();   // application start
    QVERIFY(!QFileInfo::exists(quietPath(dir, QDate(2026, 7, 30))));
    QVERIFY(QFileInfo::exists(quietPath(dir, QDate(2026, 7, 31))));
    QVERIFY(!QFileInfo::exists(fullPath(dir, QDate(2026, 9, 21))));
    QVERIFY(QFileInfo::exists(fullPath(dir, QDate(2026, 9, 22))));
    QVERIFY(QString::fromUtf8(readAll(fullPath(dir, QDate(2026, 9, 28))))
                .contains(QStringLiteral("2 expired log file(s) deleted: taidaflow-2026-07-30.log, taidaflow-2026-09-21-full.log")));

    clock.set(at(2026, 9, 29, 0, 0, 1));
    writer.write(QtInfoMsg, nullptr, QStringLiteral("next day"));
    QVERIFY(!QFileInfo::exists(quietPath(dir, QDate(2026, 7, 31))));
    QVERIFY(!QFileInfo::exists(fullPath(dir, QDate(2026, 9, 22))));
    QVERIFY(QFileInfo::exists(quietPath(dir, QDate(2026, 9, 28))));
    QVERIFY(QFileInfo::exists(fullPath(dir, QDate(2026, 9, 28))));
}

void tst_AppLog::disabledModes()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QDate day(2026, 9, 28);
    FakeClock clock(at(2026, 9, 28));

    // quiet off: no quiet file; its expired files are still deleted with quiet.keepDays.
    const QString a = tmp.filePath(QStringLiteral("a"));
    QVERIFY(touch(quietPath(a, QDate(2026, 9, 1))));
    QVERIFY(touch(quietPath(a, QDate(2026, 9, 27))));
    {
        AppLogWriter writer(settings(a, false, 2, true, 7), clock.clock());
        writer.write(QtWarningMsg, nullptr, QStringLiteral("w"));
        QVERIFY(!writer.isQuietOpen());
    }
    QVERIFY(!QFileInfo::exists(quietPath(a, day)));
    QVERIFY(!QFileInfo::exists(quietPath(a, QDate(2026, 9, 1))));
    QVERIFY(QFileInfo::exists(quietPath(a, QDate(2026, 9, 27))));
    QCOMPARE(messageLines(fullPath(a, day)), QStringList({QStringLiteral("2026-09-28 10:00:00.000 [warning] w")}));

    // full off: no full file; expired -full files still deleted with full.keepDays.
    const QString b = tmp.filePath(QStringLiteral("b"));
    QVERIFY(touch(fullPath(b, QDate(2026, 9, 1))));
    QVERIFY(touch(fullPath(b, QDate(2026, 9, 25))));
    {
        AppLogWriter writer(settings(b, true, 60, false, 7), clock.clock());
        writer.write(QtInfoMsg, nullptr, QStringLiteral("i"));
        writer.write(QtWarningMsg, nullptr, QStringLiteral("w"));
        QVERIFY(!writer.isFullOpen());
    }
    QVERIFY(!QFileInfo::exists(fullPath(b, day)));
    QVERIFY(!QFileInfo::exists(fullPath(b, QDate(2026, 9, 1))));
    QVERIFY(QFileInfo::exists(fullPath(b, QDate(2026, 9, 25))));
    QCOMPARE(lines(quietPath(b, day)), QStringList({QStringLiteral("2026-09-28 10:00:00.000 [warning] w")}));

    // both off: no file at all, clean-up still done.
    const QString c = tmp.filePath(QStringLiteral("c"));
    QVERIFY(touch(fullPath(c, QDate(2026, 1, 1))));
    QVERIFY(touch(quietPath(c, QDate(2026, 1, 1))));
    {
        AppLogWriter writer(settings(c, false, 60, false, 7), clock.clock());
        writer.write(QtWarningMsg, nullptr, QStringLiteral("w"));
    }
    QCOMPARE(QDir(c).entryList(QDir::Files), QStringList());
}

void tst_AppLog::writeFailureInvalidPath()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    // log.dir below a FILE: the folder cannot be created.
    const QString blocker = tmp.filePath(QStringLiteral("blocker"));
    QVERIFY(touch(blocker));
    const QString dir = blocker + QStringLiteral("/logs");
    FakeClock clock(at(2026, 9, 28, 10));
    AppLogWriter writer(settings(dir), clock.clock());
    writer.start();
    QVERIFY(!writer.isQuietOpen());
    QVERIFY(!writer.isFullOpen());
    QCOMPARE(writer.failureWarnings(), 2);   // one per file
    for (int i = 0; i < 100; ++i)
        writer.write(i % 2 ? QtWarningMsg : QtInfoMsg, nullptr, QStringLiteral("lost %1").arg(i));
    QCOMPARE(writer.failureWarnings(), 2);   // no new warning per message
    clock.advanceMs(30 * 1000);
    writer.write(QtWarningMsg, nullptr, QStringLiteral("still failing"));
    QCOMPARE(writer.failureWarnings(), 2);

    // Fixed: the next attempt (after kRetryIntervalMs) succeeds.
    QVERIFY(QFile::remove(blocker));
    clock.advanceMs(AppLogWriter::kRetryIntervalMs);
    writer.write(QtWarningMsg, nullptr, QStringLiteral("works again"));
    QVERIFY(writer.isQuietOpen());
    QVERIFY(writer.isFullOpen());
    QCOMPARE(messageLines(quietPath(dir, QDate(2026, 9, 28))),
             QStringList({QStringLiteral("2026-09-28 10:01:30.000 [warning] works again")}));
    QVERIFY(QString::fromUtf8(readAll(fullPath(dir, QDate(2026, 9, 28)))).contains(QStringLiteral("is writable again")));

    // Failure again (folder removed and replaced by a file), then the next date retries at once.
    writer.close();
    QVERIFY(QDir(dir).removeRecursively());
    QVERIFY(QDir(tmp.path()).rmdir(QStringLiteral("blocker")));
    QVERIFY(touch(blocker));
    writer.write(QtWarningMsg, nullptr, QStringLiteral("lost again"));
    QCOMPARE(writer.failureWarnings(), 4);
    QVERIFY(QFile::remove(blocker));
    clock.set(at(2026, 9, 29, 0, 0, 1));   // < kRetryIntervalMs later would not matter: new date
    writer.write(QtWarningMsg, nullptr, QStringLiteral("new day"));
    QVERIFY(writer.isQuietOpen());
    QCOMPARE(messageLines(quietPath(dir, QDate(2026, 9, 29))),
             QStringList({QStringLiteral("2026-09-29 00:00:01.000 [warning] new day")}));
}

void tst_AppLog::writeFailureReadOnlyFile()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.path();
    const QString full = fullPath(dir, QDate(2026, 9, 28));
    QVERIFY(touch(full, QByteArray()));
    QVERIFY(QFile::setPermissions(full, QFileDevice::ReadOwner | QFileDevice::ReadUser));   // read-only attribute
    FakeClock clock(at(2026, 9, 28, 10));
    {
        AppLogWriter writer(settings(dir), clock.clock());
        writer.write(QtWarningMsg, nullptr, QStringLiteral("only quiet works"));
        QVERIFY(writer.isQuietOpen());
        QVERIFY(!writer.isFullOpen());
        QCOMPARE(writer.failureWarnings(), 1);
    }
    // The quiet file has the message and the note about the full file.
    const QStringList quiet = lines(quietPath(dir, QDate(2026, 9, 28)));
    QCOMPARE(quiet.size(), 2);
    QVERIFY(quiet.at(0).contains(QStringLiteral("[warning] [AppLog] WARNING: full log: cannot open")));
    QCOMPARE(quiet.at(1), QStringLiteral("2026-09-28 10:00:00.000 [warning] only quiet works"));
    QCOMPARE(QFileInfo(full).size(), qint64(0));
    QVERIFY(QFile::setPermissions(full, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                            | QFileDevice::ReadUser | QFileDevice::WriteUser));
}

void tst_AppLog::writeFailureInvalidCharacters()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    // Characters Windows does not allow in a folder name.
    const QString dir = tmp.filePath(QStringLiteral("bad<>|?*name"));
    FakeClock clock(at(2026, 9, 28, 10));
    AppLogWriter writer(settings(dir), clock.clock());
    for (int i = 0; i < 10; ++i)
        writer.write(QtCriticalMsg, "x", QStringLiteral("msg %1").arg(i));
    QVERIFY(!writer.isQuietOpen());
    QVERIFY(!writer.isFullOpen());
    QCOMPARE(writer.failureWarnings(), 2);
}

void tst_AppLog::multithreadedWriter()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    constexpr int kThreads = 8;
    constexpr int kLines = 1000;
    const QString payload(200, QLatin1Char('x'));
    AppLogWriter writer(settings(tmp.path()));   // real clock
    writer.start();
    const QDate day = writer.currentDate();
    std::vector<std::unique_ptr<QThread>> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back(QThread::create([&writer, t, &payload] {
            for (int n = 0; n < kLines; ++n) {
                writer.write(n % 10 == 0 ? QtWarningMsg : QtInfoMsg, "taidaflow.mt",
                             QStringLiteral("thread %1 line %2 %3 end").arg(t).arg(n).arg(payload));
            }
        }));
        threads.back()->start();
    }
    for (auto &thread : threads)
        QVERIFY(thread->wait(60000));
    QVERIFY2(writer.currentDate() == day, "the test ran over midnight - run it again");
    writer.close();

    const QRegularExpression re(QStringLiteral(
        "^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3} \\[(info|warning)\\] "
        "taidaflow\\.mt: thread ([0-9]+) line ([0-9]+) x{200} end$"));
    const QStringList full = messageLines(fullPath(tmp.path(), day));
    QCOMPARE(full.size(), kThreads * kLines);
    QSet<QString> seen;
    for (const QString &line : full) {
        const QRegularExpressionMatch m = re.match(line);
        QVERIFY2(m.hasMatch(), qPrintable(line.left(120)));   // whole line intact, not interleaved
        const bool warning = m.captured(1) == QLatin1String("warning");
        QCOMPARE(warning, m.captured(3).toInt() % 10 == 0);
        seen.insert(m.captured(2) + QLatin1Char('/') + m.captured(3));
    }
    QCOMPARE(seen.size(), kThreads * kLines);   // every line exactly once
    const QStringList quiet = lines(quietPath(tmp.path(), day));
    QCOMPARE(quiet.size(), kThreads * kLines / 10);
    for (const QString &line : quiet)
        QVERIFY2(re.match(line).hasMatch() && line.contains(QStringLiteral("[warning]")), qPrintable(line.left(120)));
    QVERIFY(crlfOnly(readAll(fullPath(tmp.path(), day))));
}

void tst_AppLog::handlerBuffersAndWrites()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.filePath(QStringLiteral("logs"));
    QVERIFY(!AppLog::isInstalled());

    AppLog::startBuffering();
    const QDateTime beforeEarly = QDateTime::currentDateTime();
    qInfo().noquote() << "[Config] early info before install";
    // Still passed on to the previous handler (here QTest's: the ignoreMessage is consumed).
    QTest::ignoreMessage(QtWarningMsg, "early warning before install");
    qWarning("early warning before install");
    QVERIFY(!QFileInfo::exists(dir));   // nothing written yet

    FakeClock clock(at(2026, 9, 28, 12));
    QVERIFY(AppLog::install(settings(dir), clock.clock()));
    QVERIFY(AppLog::isInstalled());
    qCInfo(lcAppLogTest) << "category message";
    qDebug("debug message");
    QTest::ignoreMessage(QtCriticalMsg, "critical message");
    qCritical("critical message");
    AppLog::shutdown();
    QVERIFY(!AppLog::isInstalled());
    qInfo("after shutdown - not in the file");

    const QDate day(2026, 9, 28);
    const QStringList full = messageLines(fullPath(dir, day));
    QCOMPARE(full.size(), 5);
    // Chronological: the buffered messages come first, AppLog's clean-up note (made at install) after them.
    const QStringList all = lines(fullPath(dir, day));
    QVERIFY2(all.at(0).endsWith(QStringLiteral("[info] [Config] early info before install")), qPrintable(all.at(0)));
    QVERIFY2(all.at(2).contains(QStringLiteral("[info] [AppLog] clean-up of ")), qPrintable(all.at(2)));
    // Buffered messages: written first, with the time they were produced (real clock).
    const QString earlyDate = beforeEarly.toString(QStringLiteral("yyyy-MM-dd"));
    QVERIFY2(full.at(0).startsWith(earlyDate) && full.at(0).endsWith(QStringLiteral("[info] [Config] early info before install")),
             qPrintable(full.at(0)));
    QVERIFY2(full.at(1).endsWith(QStringLiteral("[warning] early warning before install")), qPrintable(full.at(1)));
    QVERIFY(QDateTime::fromString(full.at(0).left(23), QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) >= beforeEarly.addMSecs(-1));
    QCOMPARE(full.at(2), QStringLiteral("2026-09-28 12:00:00.000 [info] taidaflow.applogtest: category message"));
    QCOMPARE(full.at(3), QStringLiteral("2026-09-28 12:00:00.000 [debug] debug message"));
    QCOMPARE(full.at(4), QStringLiteral("2026-09-28 12:00:00.000 [critical] critical message"));
    const QStringList quiet = lines(quietPath(dir, day));
    QCOMPARE(quiet.size(), 2);
    QVERIFY(quiet.at(0).endsWith(QStringLiteral("[warning] early warning before install")));
    QCOMPARE(quiet.at(1), QStringLiteral("2026-09-28 12:00:00.000 [critical] critical message"));
}

void tst_AppLog::handlerMultithreaded()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    FakeClock clock(at(2026, 9, 28, 12));
    AppLog::startBuffering();
    QVERIFY(AppLog::install(settings(tmp.path()), clock.clock()));
    constexpr int kThreads = 4;
    constexpr int kLines = 100;
    std::vector<std::unique_ptr<QThread>> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back(QThread::create([t] {
            for (int n = 0; n < kLines; ++n)
                qCInfo(lcAppLogTest, "handler thread %d line %d payload-payload-payload", t, n);
        }));
        threads.back()->start();
    }
    for (auto &thread : threads)
        QVERIFY(thread->wait(60000));
    AppLog::shutdown();
    const QStringList full = messageLines(fullPath(tmp.path(), QDate(2026, 9, 28)));
    QCOMPARE(full.size(), kThreads * kLines);
    const QRegularExpression re(QStringLiteral(
        "^2026-09-28 12:00:00\\.000 \\[info\\] taidaflow\\.applogtest: handler thread ([0-9]) line ([0-9]+) "
        "payload-payload-payload$"));
    QSet<QString> seen;
    for (const QString &line : full) {
        const QRegularExpressionMatch m = re.match(line);
        QVERIFY2(m.hasMatch(), qPrintable(line));
        seen.insert(m.captured(1) + QLatin1Char('/') + m.captured(2));
    }
    QCOMPARE(seen.size(), kThreads * kLines);
}

void tst_AppLog::cleanupTiming()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QDate today(2026, 9, 28);
    // 67 days of quiet + full files (134) + 20 other files: about the field steady state
    // (60 quiet + 7 full) plus one day of expired files.
    for (int offset = 0; offset < 67; ++offset) {
        QVERIFY(touch(quietPath(tmp.path(), today.addDays(-offset))));
        QVERIFY(touch(fullPath(tmp.path(), today.addDays(-offset))));
    }
    for (int i = 0; i < 20; ++i)
        QVERIFY(touch(tmp.filePath(QStringLiteral("taidaflow-20260101-%1.log").arg(100000 + i))));
    QElapsedTimer timer;
    timer.start();
    const AppLogWriter::CleanupResult r = AppLogWriter::cleanup(tmp.path(), today, 60, 7);
    const qint64 ms = timer.elapsed();
    QCOMPARE(r.examined, 154);
    QCOMPARE(r.deleted.size(), 7 + 60);   // quiet: offsets 60..66; full: offsets 7..66
    qInfo("clean-up timing: %d files examined, %d deleted, %lld ms (result.elapsedMs %lld)", r.examined,
          int(r.deleted.size()), ms, r.elapsedMs);
    QVERIFY2(ms < 1000, qPrintable(QStringLiteral("%1 ms").arg(ms)));
}

void tst_AppLog::describe()
{
    const QString text = AppLog::describe(settings(QStringLiteral("C:/TaidaFlowData/logs"), true, 60, false, 7));
    QCOMPARE(text, QStringLiteral("[AppLog] log folder C:\\TaidaFlowData\\logs; quiet taidaflow-YYYY-MM-DD.log "
                                  "(warning, critical, fatal): on, keepDays 60; full taidaflow-YYYY-MM-DD-full.log "
                                  "(every message): off, keepDays 7"));
}

void tst_AppLog::configFailureWritesFallbackLog_data()
{
    QTest::addColumn<bool>("pathIsFolder");
    QTest::newRow("JSON syntax error") << false;
    QTest::newRow("config.json is a folder") << true;
}

// w2-064 A2: config.json unusable -> the same path main() takes (AppLog::handleConfigFailure).
void tst_AppLog::configFailureWritesFallbackLog()
{
    QFETCH(bool, pathIsFolder);
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString cfgDir = QDir::cleanPath(tmp.filePath(QStringLiteral("install")));
    const QString path = cfgDir + QStringLiteral("/config.json");
    const QByteArray bad = "{\n  \"dataDir\": \"data\",\n  \"mirror\": { \"publicPort\": 8125, }\n}\n";
    if (pathIsFolder)
        QVERIFY(QDir().mkpath(path));
    else
        QVERIFY(touch(path, bad));
    const QDateTime modified = QFileInfo(path).lastModified();
    const QString logs = cfgDir + QStringLiteral("/logs");
    // Fallback folder: normal clean-up rules (expired file deleted, foreign file kept).
    QVERIFY(touch(quietPath(logs, QDate(2026, 7, 1))));
    QVERIFY(touch(QDir(logs).filePath(QStringLiteral("launcher.log"))));

    const AppConfig::LogSettings fallback = AppConfig::fallbackLogSettings(path);
    QCOMPARE(fallback.dir, logs);
    QVERIFY(fallback.quiet.enabled && fallback.full.enabled);
    QCOMPARE(fallback.quiet.keepDays, 60);
    QCOMPARE(fallback.full.keepDays, 7);

    AppLog::startBuffering();                       // first line of main()
    const AppConfig::LoadResult r = AppConfig::load(path, tmp.path(), std::nullopt);
    QVERIFY(!r.ok);
    r.printLog();                                   // main() prints the load log before the check
    QString dialog;
    int dialogCalls = 0;
    FakeClock clock(at(2026, 9, 28, 9));
    const int code = AppLog::handleConfigFailure(
        r, [&dialog, &dialogCalls](const QString &text) { dialog = text; ++dialogCalls; }, clock.clock());
    AppLog::shutdown();                             // ~AppLog::Scope when main() returns

    QCOMPARE(code, 2);
    QCOMPARE(code, AppConfig::kConfigErrorExitCode);
    QCOMPARE(dialogCalls, 1);
    // config.json untouched.
    if (pathIsFolder) {
        QVERIFY(QFileInfo(path).isDir());
    } else {
        QCOMPARE(readAll(path), bad);
        QCOMPARE(QFileInfo(path).lastModified(), modified);
    }
    const QDate day(2026, 9, 28);
    const QString nativePath = QDir::toNativeSeparators(path);
    const QString where = r.errorLine > 0
        ? QStringLiteral(" (line %1, column %2)").arg(r.errorLine).arg(r.errorColumn) : QString();
    const QString fullFile = QDir::toNativeSeparators(fullPath(logs, day));
    const QString quietFile = QDir::toNativeSeparators(quietPath(logs, day));
    const QString failureLine = QStringLiteral("[critical] [Config] config file %1 cannot be used: %2%3 - the program "
                                               "exits with code 2; log files (fallback folder beside config.json): %4, %5")
                                    .arg(nativePath, r.error, where, fullFile, quietFile);
    const QString full = QString::fromUtf8(readAll(fullPath(logs, day)));
    const QString quiet = QString::fromUtf8(readAll(quietPath(logs, day)));
    QVERIFY2(full.contains(QStringLiteral("[info] [Config] config file %1").arg(nativePath)) || pathIsFolder,
             qPrintable(full));                                    // buffered info
    QVERIFY2(full.contains(QStringLiteral("[warning] [Config] config file %1 cannot be used: ").arg(nativePath)),
             qPrintable(full));                                    // buffered warning
    QVERIFY2(full.contains(failureLine), qPrintable(full));
    QVERIFY2(quiet.contains(failureLine), qPrintable(quiet));
    QVERIFY(quiet.contains(QStringLiteral("[warning] [Config] config file %1 cannot be used: ").arg(nativePath)));
    QVERIFY(!quiet.contains(QStringLiteral("[info]")));
    if (!pathIsFolder) {
        QCOMPARE(r.errorLine, 3);
        QVERIFY(r.error.startsWith(QStringLiteral("JSON syntax error")));
        QVERIFY(full.contains(QStringLiteral("(line 3, column ")));
    }
    // Dialog: the usual text + the log file.
    QVERIFY2(dialog.startsWith(r.errorDialogText()), qPrintable(dialog));
    QVERIFY(dialog.contains(QStringLiteral("請修正 config.json 或刪除它讓程式重建預設值")));
    QVERIFY2(dialog.endsWith(QStringLiteral("\n\n記錄檔:%1").arg(fullFile)), qPrintable(dialog));
    // Clean-up of the fallback folder; nothing else created next to config.json.
    QVERIFY(!QFileInfo::exists(quietPath(logs, QDate(2026, 7, 1))));
    QVERIFY(QFileInfo::exists(QDir(logs).filePath(QStringLiteral("launcher.log"))));
    QCOMPARE(QDir(cfgDir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name),
             QStringList({QStringLiteral("config.json"), QStringLiteral("logs")}));
}

void tst_AppLog::configFailureFallbackUnwritable()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString cfgDir = QDir::cleanPath(tmp.filePath(QStringLiteral("install")));
    const QString path = cfgDir + QStringLiteral("/config.json");
    const QByteArray bad = "{ \"dataDir\": ";
    QVERIFY(touch(path, bad));
    // "logs" is a FILE: the fallback folder cannot be created.
    const QString logs = cfgDir + QStringLiteral("/logs");
    QVERIFY(touch(logs, "not a folder"));

    AppLog::startBuffering();
    const AppConfig::LoadResult r = AppConfig::load(path, tmp.path(), std::nullopt);
    QVERIFY(!r.ok);
    r.printLog();
    QString dialog;
    FakeClock clock(at(2026, 9, 28, 9));
    const int code = AppLog::handleConfigFailure(
        r, [&dialog](const QString &text) { dialog = text; }, clock.clock());
    AppLog::shutdown();

    QCOMPARE(code, AppConfig::kConfigErrorExitCode);           // still exit code 2
    QCOMPARE(readAll(path), bad);                              // config.json not written
    QCOMPARE(readAll(logs), QByteArray("not a folder"));       // the blocking file untouched
    QVERIFY2(dialog.startsWith(r.errorDialogText()), qPrintable(dialog));
    QVERIFY2(dialog.endsWith(QStringLiteral("\n\n記錄檔無法寫入:%1").arg(QDir::toNativeSeparators(logs))),
             qPrintable(dialog));
    QCOMPARE(QDir(cfgDir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name),
             QStringList({QStringLiteral("config.json"), QStringLiteral("logs")}));
}

void tst_AppLog::bufferLimit()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    AppLog::startBuffering();
    const int total = AppLog::kMaxBufferedMessages + 5;
    for (int i = 0; i < total; ++i)
        qCDebug(lcAppLogTest, "buffered %d", i);
    FakeClock clock(at(2026, 9, 28, 12));
    QVERIFY(AppLog::install(settings(tmp.path()), clock.clock()));
    AppLog::shutdown();
    const QStringList full = messageLines(fullPath(tmp.path(), QDate(2026, 9, 28)));
    QCOMPARE(full.size(), AppLog::kMaxBufferedMessages);
    QVERIFY(full.last().endsWith(QStringLiteral("buffered %1").arg(AppLog::kMaxBufferedMessages - 1)));
    const QString text = QString::fromUtf8(readAll(quietPath(tmp.path(), QDate(2026, 9, 28))));
    QVERIFY2(text.contains(QStringLiteral("[warning] [AppLog] 5 message(s) produced before the log was opened were not kept")),
             qPrintable(text));
}

QTEST_GUILESS_MAIN(tst_AppLog)
#include "tst_applog.moc"
