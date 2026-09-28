#include "applog.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QList>
#include <QMutexLocker>
#include <QRegularExpression>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {

QString nativePath(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

void printStderr(const QString &text)
{
    const QByteArray line = (text + QLatin1Char('\n')).toLocal8Bit();
    std::fputs(line.constData(), stderr);
    std::fflush(stderr);
}

// Exact names written by AppLogWriter (ASCII digits only; case-sensitive).
const QRegularExpression &quietNamePattern()
{
    static const QRegularExpression re(QStringLiteral("^taidaflow-([0-9]{4})-([0-9]{2})-([0-9]{2})\\.log$"));
    return re;
}

const QRegularExpression &fullNamePattern()
{
    static const QRegularExpression re(QStringLiteral("^taidaflow-([0-9]{4})-([0-9]{2})-([0-9]{2})-full\\.log$"));
    return re;
}

QDate dateOf(const QRegularExpressionMatch &match)
{
    return QDate(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt());
}

QString onOff(bool enabled)
{
    return enabled ? QStringLiteral("on") : QStringLiteral("off");
}

// ---- application-wide handler state -----------------------------------------------------
struct BufferedMessage
{
    QtMsgType type;
    QByteArray category;   // empty = no category
    QString message;
    QDateTime time;
};

QBasicMutex g_mutex;                              // guards everything below except g_previous
std::atomic<QtMessageHandler> g_previous{nullptr};
bool g_installed = false;                         // our handler is the current Qt handler
AppLogWriter *g_writer = nullptr;                 // owned; null while buffering
QList<BufferedMessage> *g_buffer = nullptr;       // owned; null once the writer exists
int g_dropped = 0;
// A message produced while this thread is already inside the handler (e.g. a Qt warning from
// the file API) is only passed on, never written: no self-deadlock, no endless recursion.
thread_local bool t_inHandler = false;

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (!t_inHandler) {
        t_inHandler = true;
        {
            QMutexLocker lock(&g_mutex);
            if (g_writer) {
                g_writer->write(type, context.category, message);
            } else if (g_buffer) {
                if (g_buffer->size() < AppLog::kMaxBufferedMessages) {
                    g_buffer->append({type, QByteArray(context.category ? context.category : ""),
                                      message, QDateTime::currentDateTime()});
                } else {
                    ++g_dropped;
                }
            }
        }
        t_inHandler = false;
    }
    // Unchanged output as before (stderr / debugger / Qt Creator, or QTest's handler).
    const QtMessageHandler previous = g_previous.load();
    if (previous)
        previous(type, context, message);
}

} // namespace

// ==== AppLogWriter ========================================================================

AppLogWriter::AppLogWriter(const AppConfig::LogSettings &settings, Clock clock)
    : m_settings(settings), m_clock(std::move(clock))
{
    m_settings.quiet.keepDays = qMax(1, m_settings.quiet.keepDays);
    m_settings.full.keepDays = qMax(1, m_settings.full.keepDays);
    m_quiet.enabled = m_settings.quiet.enabled;
    m_quiet.quiet = true;
    m_full.enabled = m_settings.full.enabled;
    m_full.quiet = false;
}

AppLogWriter::~AppLogWriter()
{
    close();
}

QDateTime AppLogWriter::now() const
{
    return m_clock ? m_clock() : QDateTime::currentDateTime();
}

void AppLogWriter::start(bool deferNotes)
{
    QMutexLocker lock(&m_mutex);
    const QDateTime time = now();
    m_deferNotes = deferNotes;
    rollOver(time.date(), time);
    if (!m_deferNotes)
        writePendingNotes();
}

void AppLogWriter::flushNotes()
{
    QMutexLocker lock(&m_mutex);
    m_deferNotes = false;
    writePendingNotes();
}

void AppLogWriter::write(QtMsgType type, const char *category, const QString &message, const QDateTime &time)
{
    QMutexLocker lock(&m_mutex);
    const QDateTime current = now();
    if (current.date() != m_date)
        rollOver(current.date(), current);
    if (!m_deferNotes)
        writePendingNotes();
    const QByteArray bytes = formatLine(time.isValid() ? time : current, type, category, message).toUtf8();
    if (goesToQuiet(type))
        writeTo(m_quiet, bytes, current);
    writeTo(m_full, bytes, current);
}

void AppLogWriter::close()
{
    QMutexLocker lock(&m_mutex);
    m_quiet.file.close();
    m_full.file.close();
}

AppConfig::LogSettings AppLogWriter::settings() const
{
    QMutexLocker lock(&m_mutex);
    return m_settings;
}

QDate AppLogWriter::currentDate() const
{
    QMutexLocker lock(&m_mutex);
    return m_date;
}

AppLogWriter::CleanupResult AppLogWriter::lastCleanup() const
{
    QMutexLocker lock(&m_mutex);
    return m_lastCleanup;
}

int AppLogWriter::failureWarnings() const
{
    QMutexLocker lock(&m_mutex);
    return m_failureWarnings;
}

bool AppLogWriter::isQuietOpen() const
{
    QMutexLocker lock(&m_mutex);
    return m_quiet.file.isOpen();
}

bool AppLogWriter::isFullOpen() const
{
    QMutexLocker lock(&m_mutex);
    return m_full.file.isOpen();
}

QString AppLogWriter::quietFileName(const QDate &date)
{
    return QStringLiteral("taidaflow-%1.log").arg(date.toString(QStringLiteral("yyyy-MM-dd")));
}

QString AppLogWriter::fullFileName(const QDate &date)
{
    return QStringLiteral("taidaflow-%1-full.log").arg(date.toString(QStringLiteral("yyyy-MM-dd")));
}

bool AppLogWriter::goesToQuiet(QtMsgType type)
{
    return type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg;
}

QString AppLogWriter::levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return QStringLiteral("debug");
    case QtInfoMsg: return QStringLiteral("info");
    case QtWarningMsg: return QStringLiteral("warning");
    case QtCriticalMsg: return QStringLiteral("critical");
    case QtFatalMsg: return QStringLiteral("fatal");
    }
    return QStringLiteral("unknown");
}

QString AppLogWriter::formatLine(const QDateTime &time, QtMsgType type, const char *category,
                                 const QString &message)
{
    QString line = time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    line += QStringLiteral(" [");
    line += levelName(type);
    line += QStringLiteral("] ");
    if (category && *category && std::strcmp(category, "default") != 0) {
        line += QString::fromUtf8(category);
        line += QStringLiteral(": ");
    }
    // The text is kept as it is; only its line breaks become CRLF like the rest of the file.
    QString text = message;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    line += text;
    line += QStringLiteral("\r\n");
    return line;
}

AppLogWriter::CleanupResult AppLogWriter::cleanup(const QString &dir, const QDate &today, int quietKeepDays,
                                                  int fullKeepDays)
{
    CleanupResult result;
    QElapsedTimer timer;
    timer.start();
    const QDir folder(dir);
    if (!today.isValid() || dir.isEmpty() || !folder.exists()) {
        result.elapsedMs = timer.elapsed();
        return result;
    }
    // keepDays days including today: the oldest kept date is today - (keepDays - 1).
    const QDate quietOldestKept = today.addDays(-(qint64(qMax(1, quietKeepDays)) - 1));
    const QDate fullOldestKept = today.addDays(-(qint64(qMax(1, fullKeepDays)) - 1));
    // Files only (no sub-folders, no recursion); symbolic links are never touched.
    const QFileInfoList entries =
        folder.entryInfoList(QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDir::Name);
    result.examined = int(entries.size());
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        QDate date;
        QDate oldestKept;
        const QRegularExpressionMatch quiet = quietNamePattern().match(name);
        if (quiet.hasMatch()) {
            date = dateOf(quiet);
            oldestKept = quietOldestKept;
        } else {
            const QRegularExpressionMatch full = fullNamePattern().match(name);
            if (!full.hasMatch())
                continue;                       // not a file of this program: never touched
            date = dateOf(full);
            oldestKept = fullOldestKept;
        }
        if (!date.isValid() || date >= oldestKept)
            continue;                           // invalid date in the name, or still kept
        QFile file(entry.absoluteFilePath());
        if (file.remove())
            result.deleted.append(name);
        else
            result.failed.append(QStringLiteral("%1: %2").arg(name, file.errorString()));
    }
    result.elapsedMs = timer.elapsed();
    return result;
}

// ---- private (m_mutex held) -------------------------------------------------------------

QString AppLogWriter::path(const Target &target) const
{
    return QDir(m_settings.dir).filePath(target.quiet ? quietFileName(m_date) : fullFileName(m_date));
}

void AppLogWriter::rollOver(const QDate &today, const QDateTime &time)
{
    const QDate previous = m_date;
    if (previous.isValid()) {
        internalLine(QtInfoMsg,
                     QStringLiteral("[AppLog] the date changed from %1 to %2 - continuing in the files of %2")
                         .arg(previous.toString(Qt::ISODate), today.toString(Qt::ISODate)),
                     time);
    }
    m_quiet.file.close();
    m_full.file.close();
    m_date = today;
    // A new date = a new attempt for a file that could not be written.
    m_quiet.retryAt = QDateTime();
    m_full.retryAt = QDateTime();

    m_lastCleanup = cleanup(m_settings.dir, today, m_settings.quiet.keepDays, m_settings.full.keepDays);

    ensureOpen(m_quiet, time);
    ensureOpen(m_full, time);

    if (previous.isValid()) {
        addNote(QtInfoMsg,
                QStringLiteral("[AppLog] continued from %1 (date changed from %1 to %2)")
                    .arg(previous.toString(Qt::ISODate), today.toString(Qt::ISODate)),
                time);
    }
    const CleanupResult &c = m_lastCleanup;
    addNote(QtInfoMsg,
            QStringLiteral("[AppLog] clean-up of %1 for %2: %3 file(s) examined, %4 expired log file(s) "
                           "deleted%5 (quiet keeps %6 day(s), full keeps %7 day(s), today included), took %8 ms")
                .arg(nativePath(m_settings.dir), today.toString(Qt::ISODate), QString::number(c.examined),
                     QString::number(c.deleted.size()),
                     c.deleted.isEmpty() ? QString() : QStringLiteral(": ") + c.deleted.join(QStringLiteral(", ")),
                     QString::number(m_settings.quiet.keepDays), QString::number(m_settings.full.keepDays),
                     QString::number(c.elapsedMs)),
            time);
    if (!c.failed.isEmpty()) {
        const QString text = QStringLiteral("[AppLog] expired log file(s) could not be deleted: %1")
                                 .arg(c.failed.join(QStringLiteral("; ")));
        printStderr(text);
        addNote(QtWarningMsg, text, time);
    }
}

void AppLogWriter::addNote(QtMsgType type, const QString &text, const QDateTime &time)
{
    m_pendingNotes.append({type, text, time});
}

void AppLogWriter::writePendingNotes()
{
    const QList<Note> notes = std::exchange(m_pendingNotes, {});
    for (const Note &note : notes)
        internalLine(note.type, note.text, note.time);
}

bool AppLogWriter::ensureOpen(Target &target, const QDateTime &time)
{
    if (!target.enabled)
        return false;
    if (target.file.isOpen())
        return true;
    // Waiting for the next attempt (a clock set back before the failure retries at once).
    if (target.retryAt.isValid() && time < target.retryAt
        && time >= target.retryAt.addMSecs(-kRetryIntervalMs)) {
        return false;
    }
    if (!QDir().mkpath(m_settings.dir)) {
        fail(target, QStringLiteral("cannot create the log folder %1").arg(nativePath(m_settings.dir)), time);
        return false;
    }
    target.file.setFileName(path(target));
    // Unbuffered: every write() goes straight to the operating system, nothing is lost when
    // the program crashes after the line was written.
    if (!target.file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Unbuffered)) {
        fail(target, QStringLiteral("cannot open %1: %2").arg(nativePath(target.file.fileName()),
                                                                target.file.errorString()),
             time);
        return false;
    }
    target.retryAt = QDateTime();
    if (target.failing) {
        target.failing = false;
        const QString text = QStringLiteral("[AppLog] %1 log file %2 is writable again")
                                 .arg(target.quiet ? QStringLiteral("quiet") : QStringLiteral("full"),
                                      nativePath(target.file.fileName()));
        printStderr(text);
        internalLine(QtInfoMsg, text, time);
    }
    return true;
}

void AppLogWriter::writeTo(Target &target, const QByteArray &bytes, const QDateTime &time)
{
    if (!ensureOpen(target, time))
        return;
    if (target.file.write(bytes) != bytes.size()) {
        const QString error = target.file.errorString();
        const QString name = nativePath(target.file.fileName());
        target.file.close();
        fail(target, QStringLiteral("cannot write %1: %2").arg(name, error), time);
        return;
    }
    target.file.flush();
}

void AppLogWriter::fail(Target &target, const QString &what, const QDateTime &time)
{
    target.retryAt = time.addMSecs(kRetryIntervalMs);
    if (target.failing)
        return;                                  // one warning per failure episode
    target.failing = true;
    ++m_failureWarnings;
    const QString text = QStringLiteral("[AppLog] WARNING: %1 log: %2 - its messages go to stderr only; "
                                        "retrying every %3 s and at the next date change")
                             .arg(target.quiet ? QStringLiteral("quiet") : QStringLiteral("full"), what,
                                  QString::number(kRetryIntervalMs / 1000));
    printStderr(text);
    internalLine(QtWarningMsg, text, time);      // into the other file when that one is open
}

void AppLogWriter::internalLine(QtMsgType type, const QString &text, const QDateTime &time)
{
    const QByteArray bytes = formatLine(time, type, nullptr, text).toUtf8();
    for (Target *target : {&m_quiet, &m_full}) {
        if (!target->file.isOpen() || (target->quiet && !goesToQuiet(type)))
            continue;
        if (target->file.write(bytes) != bytes.size()) {
            // No further note here (no recursion): mark it failing, retry later.
            target->file.close();
            target->retryAt = time.addMSecs(kRetryIntervalMs);
            if (!target->failing) {
                target->failing = true;
                ++m_failureWarnings;
                printStderr(QStringLiteral("[AppLog] WARNING: cannot write %1 - retrying every %2 s")
                                .arg(nativePath(path(*target)), QString::number(kRetryIntervalMs / 1000)));
            }
        }
    }
}

// ==== AppLog ==============================================================================

void AppLog::startBuffering()
{
    {
        QMutexLocker lock(&g_mutex);
        if (g_installed)
            return;
        g_installed = true;
        if (!g_writer && !g_buffer)
            g_buffer = new QList<BufferedMessage>;
    }
    g_previous.store(qInstallMessageHandler(messageHandler));
}

bool AppLog::install(const AppConfig::LogSettings &settings, AppLogWriter::Clock clock)
{
    startBuffering();   // no-op when already buffering
    auto *writer = new AppLogWriter(settings, std::move(clock));
    bool allOpen = true;
    {
        QMutexLocker lock(&g_mutex);
        const bool wasInHandler = t_inHandler;
        t_inHandler = true;   // messages of this thread meanwhile are only passed on
        writer->start(true);   // its notes (clean-up) after the buffered messages: chronological file
        if (g_buffer) {
            for (const BufferedMessage &m : std::as_const(*g_buffer))
                writer->write(m.type, m.category.isEmpty() ? nullptr : m.category.constData(), m.message, m.time);
            if (g_dropped > 0) {
                writer->write(QtWarningMsg, nullptr,
                              QStringLiteral("[AppLog] %1 message(s) produced before the log was opened were "
                                             "not kept (limit %2); they went to stderr only")
                                  .arg(QString::number(g_dropped), QString::number(kMaxBufferedMessages)));
            }
            delete g_buffer;
            g_buffer = nullptr;
            g_dropped = 0;
        }
        writer->flushNotes();
        delete g_writer;       // a second install() replaces the first writer
        g_writer = writer;
        allOpen = (!settings.quiet.enabled || writer->isQuietOpen())
                  && (!settings.full.enabled || writer->isFullOpen());
        t_inHandler = wasInHandler;
    }
    return allOpen;
}

void AppLog::shutdown()
{
    QtMessageHandler previous = nullptr;
    {
        QMutexLocker lock(&g_mutex);
        if (!g_installed)
            return;
        delete g_writer;
        g_writer = nullptr;
        delete g_buffer;
        g_buffer = nullptr;
        g_dropped = 0;
        g_installed = false;
        previous = g_previous.load();
    }
    qInstallMessageHandler(previous);
}

bool AppLog::isInstalled()
{
    QMutexLocker lock(&g_mutex);
    return g_writer != nullptr;
}

QString AppLog::describe(const AppConfig::LogSettings &settings)
{
    return QStringLiteral("[AppLog] log folder %1; quiet taidaflow-YYYY-MM-DD.log (warning, critical, fatal): %2, "
                          "keepDays %3; full taidaflow-YYYY-MM-DD-full.log (every message): %4, keepDays %5")
        .arg(nativePath(settings.dir), onOff(settings.quiet.enabled), QString::number(settings.quiet.keepDays),
             onOff(settings.full.enabled), QString::number(settings.full.keepDays));
}

int AppLog::handleConfigFailure(const AppConfig::LoadResult &result,
                                const std::function<void(const QString &)> &showDialog,
                                AppLogWriter::Clock clock)
{
    const AppConfig::LogSettings settings = AppConfig::fallbackLogSettings(result.path);
    const bool written = install(settings, std::move(clock));
    QDate date;
    {
        QMutexLocker lock(&g_mutex);
        if (g_writer)
            date = g_writer->currentDate();
    }
    const QString quietFile = nativePath(QDir(settings.dir).filePath(AppLogWriter::quietFileName(date)));
    const QString fullFile = nativePath(QDir(settings.dir).filePath(AppLogWriter::fullFileName(date)));
    const QString where = result.errorLine > 0
        ? QStringLiteral(" (line %1, column %2)").arg(result.errorLine).arg(result.errorColumn)
        : QString();
    const QString exitCode = QString::number(AppConfig::kConfigErrorExitCode);
    QString dialogNote;
    if (written) {
        qCritical().noquote() << QStringLiteral("[Config] config file %1 cannot be used: %2%3 - the program exits "
                                                "with code %4; log files (fallback folder beside config.json): %5, %6")
                                     .arg(nativePath(result.path), result.error, where, exitCode, fullFile, quietFile);
        dialogNote = QStringLiteral("\n\n記錄檔:%1").arg(fullFile);
    } else {
        qCritical().noquote() << QStringLiteral("[Config] config file %1 cannot be used: %2%3 - the program exits "
                                                "with code %4; the log files could not be written in the fallback "
                                                "folder %5 (see the warnings above) - this message is on stderr only")
                                     .arg(nativePath(result.path), result.error, where, exitCode,
                                          nativePath(settings.dir));
        dialogNote = QStringLiteral("\n\n記錄檔無法寫入:%1").arg(nativePath(settings.dir));
    }
    if (showDialog)
        showDialog(result.errorDialogText() + dialogNote);
    return AppConfig::kConfigErrorExitCode;
}
