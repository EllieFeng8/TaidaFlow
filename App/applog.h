// AppLog: the desktop application writes its own log files (docs/taidaflow_config_spec.md §2
// "log", w2-064).
//
// Desktop only (never compiled for WebAssembly: the page has no log files; its messages stay
// in the browser console exactly as before).
//
// Every message that reaches Qt's message handler (qDebug / qInfo / qWarning / qCritical /
// qFatal and the qC* category variants) is
//   * written to up to two daily files in config.json log.dir (default <dataDir>\logs):
//       quiet: taidaflow-YYYY-MM-DD.log       warning / critical / fatal only, kept log.quiet.keepDays (60)
//       full:  taidaflow-YYYY-MM-DD-full.log  every message,                   kept log.full.keepDays (7)
//     each switchable with log.quiet.enabled / log.full.enabled (both on by default);
//   * and still passed on to the previous handler (stderr / debugger / Qt Creator), unchanged.
//
// Line format (UTF-8 without BOM, CRLF):
//   2026-09-28 14:03:05.123 [info] <message>
//   2026-09-28 14:03:05.123 [warning] qt.network.ssl: <message>     (category when not "default")
// The message text itself is written unchanged; only line breaks inside a message are written
// as CRLF so the file has one line-ending style.
//
// Date change: before every write the date of the clock (QDate::currentDate() in the
// application) is compared with the date of the open files. When it differs (midnight, or the
// system time was changed - backwards too) the files of the new date are opened and expired
// files are deleted. Also done once when the log is installed.
// Expired = the file name is exactly taidaflow-YYYY-MM-DD.log (quiet) or
// taidaflow-YYYY-MM-DD-full.log (full), the date in the name is a valid date, and
//   date < today - (keepDays - 1)
// i.e. keepDays days are kept including today (keepDays = 1: today only). Files dated after
// today (system time set back) are kept. Nothing else in the folder is touched (older launcher
// logs taidaflow-yyyyMMdd-HHmmss.log, .stdout, nginx logs, anything else), sub-folders are not
// visited. A disabled mode still has its expired files deleted with its own keepDays.
//
// Failures (folder cannot be created, file not writable, disk full) never stop the program:
// one warning is printed to stderr per failure episode, the message still goes to the previous
// handler, and opening is retried after kRetryIntervalMs and at every date change.
//
// Usage (App/main.cpp):
//   int main(...) {
//       AppLog::Scope appLog;             // 1st line: buffers every message until install()
//       QApplication app(...);
//       ... load config.json, applyDataDir() ...
//       AppLog::install(AppConfig::instance().logSettings());   // writes the buffered messages
//       ...
//   }                                     // ~Scope: flush, close, previous handler restored
#pragma once

#include "appconfig.h"

#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <functional>
#include <memory>

// Writes the two daily files. Thread-safe (internal mutex). Used by AppLog; public so the
// date change and the clean-up can be tested with an injected clock.
class AppLogWriter
{
public:
    // Local date/time source; empty = QDateTime::currentDateTime().
    using Clock = std::function<QDateTime()>;

    static constexpr int kRetryIntervalMs = 60 * 1000;

    struct CleanupResult
    {
        QStringList deleted;      // file names
        QStringList failed;       // "name: reason"
        qint64 elapsedMs = 0;
        int examined = 0;         // files (not folders) in log.dir
    };

    explicit AppLogWriter(const AppConfig::LogSettings &settings, Clock clock = Clock());
    ~AppLogWriter();
    AppLogWriter(const AppLogWriter &) = delete;
    AppLogWriter &operator=(const AppLogWriter &) = delete;

    // Creates log.dir, opens today's enabled files and deletes expired files. write() does the
    // same by itself when the date changed; start() does it right away (application start).
    // deferNotes: AppLog's own notes about it ("[AppLog] clean-up ...") wait for flushNotes()
    // (AppLog::install() first writes the buffered, older messages: the file stays chronological).
    void start(bool deferNotes = false);
    void flushNotes();

    // One message. `time` = when it was produced (buffered early messages); invalid = now.
    // `category` = QMessageLogContext::category (nullptr / "default" = not written).
    void write(QtMsgType type, const char *category, const QString &message,
               const QDateTime &time = QDateTime());

    // Closes the files (write() reopens them).
    void close();

    AppConfig::LogSettings settings() const;
    QDate currentDate() const;               // date of the open files (invalid before start)
    CleanupResult lastCleanup() const;
    int failureWarnings() const;             // warnings printed to stderr so far
    bool isQuietOpen() const;
    bool isFullOpen() const;

    // Pure helpers.
    static QString quietFileName(const QDate &date);   // taidaflow-YYYY-MM-DD.log
    static QString fullFileName(const QDate &date);    // taidaflow-YYYY-MM-DD-full.log
    static bool goesToQuiet(QtMsgType type);           // warning / critical / fatal
    static QString levelName(QtMsgType type);          // debug info warning critical fatal
    // One complete line including the trailing CRLF.
    static QString formatLine(const QDateTime &time, QtMsgType type, const char *category,
                              const QString &message);
    // Deletes the expired files of `dir` for `today` (rules above).
    static CleanupResult cleanup(const QString &dir, const QDate &today, int quietKeepDays,
                                 int fullKeepDays);

private:
    struct Target
    {
        bool enabled = false;
        bool quiet = false;
        QFile file;
        bool failing = false;          // a failure warning was printed, not recovered yet
        QDateTime retryAt;             // no open attempt before this time
    };

    QDateTime now() const;
    void rollOver(const QDate &today, const QDateTime &time);
    bool ensureOpen(Target &target, const QDateTime &time);
    void writeTo(Target &target, const QByteArray &bytes, const QDateTime &time);
    void fail(Target &target, const QString &what, const QDateTime &time);
    // Notes of rollOver() (clean-up result, "continued from"), written by writePendingNotes().
    void addNote(QtMsgType type, const QString &text, const QDateTime &time);
    void writePendingNotes();
    // AppLog's own lines ("[AppLog] ..."): written to the files already open, never re-entering.
    void internalLine(QtMsgType type, const QString &text, const QDateTime &time);
    QString path(const Target &target) const;

    mutable QMutex m_mutex;
    AppConfig::LogSettings m_settings;
    Clock m_clock;
    QDate m_date;
    Target m_quiet;
    Target m_full;
    CleanupResult m_lastCleanup;
    int m_failureWarnings = 0;
    struct Note
    {
        QtMsgType type;
        QString text;
        QDateTime time;
    };
    QList<Note> m_pendingNotes;
    bool m_deferNotes = false;
};

// The application-wide message handler (qInstallMessageHandler).
class AppLog
{
public:
    // Upper limit of messages kept before install() (the rest is counted and reported).
    static constexpr int kMaxBufferedMessages = 10000;

    // Installs the handler in buffering mode: messages are kept in memory (and passed on to
    // the previous handler) until install(). Call first thing in main().
    static void startBuffering();
    // Creates the writer (log.dir, today's files, clean-up), writes the buffered messages with
    // their original time stamps, then writes every new message. Returns false when neither
    // enabled file could be opened (the handler stays installed and keeps retrying).
    static bool install(const AppConfig::LogSettings &settings,
                        AppLogWriter::Clock clock = AppLogWriter::Clock());
    // Flushes and closes the files, drops the buffer and restores the previous handler.
    static void shutdown();
    static bool isInstalled();

    // RAII for main(): startBuffering() / shutdown().
    struct Scope
    {
        Scope() { AppLog::startBuffering(); }
        ~Scope() { AppLog::shutdown(); }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
    };

    // One line for the start-up log: folder, both files on/off and keep days.
    static QString describe(const AppConfig::LogSettings &settings);

    // w2-064 A2 (Mango: config.json read failures must be in a log file too). config.json
    // cannot be used (result.ok == false): installs the log in the fallback folder
    // AppConfig::fallbackLogSettings(result.path) = <folder of config.json>\logs (normal file
    // names, default keepDays, normal clean-up), writes the buffered messages and one critical
    // line with file, line/column, error and the log file locations (also on stderr), then calls
    // showDialog(errorDialogText + log file location) and returns
    // AppConfig::kConfigErrorExitCode. If the fallback folder cannot be written the handling is
    // otherwise unchanged (stderr + dialog say so; still the same exit code; config.json is
    // never written).
    static int handleConfigFailure(const AppConfig::LoadResult &result,
                                   const std::function<void(const QString &)> &showDialog,
                                   AppLogWriter::Clock clock = AppLogWriter::Clock());
};
