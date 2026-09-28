// AppConfig: the desktop's config.json (docs/taidaflow_config_spec.md).
//
// Desktop only (never compiled for WebAssembly; the page learns the Mirror port from
// /runtime.json, see runtimeinfo.h). Depends on Qt Core only (JSON + file API), not on
// any backend, so the core branch can use it from every backend class.
//
// Loading (spec §1):
//   1. TAIDAFLOW_CONFIG=<full path>             -> that file
//   2. otherwise <folder of TaidaFlowApp.exe>/config.json
//   3. file missing -> the defaults are written there (UTF-8 without BOM, indented) and used;
//      if it cannot be written -> in-memory defaults + warning
//   4. file present but not valid JSON (or not a JSON object) -> error: nothing is written,
//      the defaults are NOT used; main shows a dialog and exits with kConfigErrorExitCode
//   5. missing key -> default (logged); 6. unknown key -> ignored (logged);
//   7. wrong type / out of range -> default (warning).
//   8. describeLog() lists the file source, every effective value and its source.
//
// Typical use (App/main.cpp does steps 1-3, the core backends only step 4):
//   1. AppConfig::LoadResult r = AppConfig::loadFromEnvironment();
//   2. r.printLog(); if (!r.ok) { dialog(r.errorDialogText()); return kConfigErrorExitCode; }
//   3. AppConfig::setInstance(r.config); AppConfig::instance().applyDataDir();
//   4. AppConfig::instance().device(AppConfig::Device::Adam6256).host ...
#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>

struct AppConfigLogEntry
{
    bool warning = false;
    QString text;
};
struct AppConfigLoadResult;

class AppConfig
{
public:
    // ---- constants ---------------------------------------------------------------------
    static constexpr int kVersion = 1;
    static constexpr const char *kConfigPathEnv = "TAIDAFLOW_CONFIG";
    // Temporary override of downloadPort() (spec §2 nginx: replaces the former setting).
    static constexpr const char *kDownloadPortEnv = "TAIDAFLOW_DOWNLOAD_PORT";
    static constexpr const char *kFileName = "config.json";
    static constexpr const char *kWriteDefaultOption = "--write-default-config";
    // Exit code of the application when config.json exists but cannot be used (spec §1.4).
    static constexpr int kConfigErrorExitCode = 2;
    // Exit codes of --write-default-config.
    static constexpr int kWriteOk = 0;
    static constexpr int kWriteFailed = 1;        // path not writable / write error
    static constexpr int kWriteTargetExists = 3;  // never overwrites an existing file
    static constexpr int kWriteUsage = 4;         // no path given

    // ---- typed values ------------------------------------------------------------------
    enum class Device { Adam6256, Adam6217A, Adam6217B, Adam6224, Adam6022 };

    struct TcpDevice      // devices.<adam...>: Modbus TCP
    {
        QString host;     // IPv4/IPv6 address
        quint16 port = 0;
        int unitId = 0;
    };
    struct SerialDevice   // devices.ms300: Modbus RTU
    {
        QString serialPort;   // e.g. "COM2"
        int baudRate = 0;     // 1200 .. 115200 (QSerialPort::BaudRate values)
        int dataBits = 0;     // 5..8
        QString parity;       // none | even | odd | space | mark
        int stopBits = 0;     // 1 | 2
        int unitId = 0;       // 1..247
    };
    struct ModbusServerSettings
    {
        QString bind;
        quint16 port = 0;
        int unitId = 0;
    };
    struct Listener       // http, rest
    {
        QString bind;
        quint16 port = 0;
    };
    struct MirrorSettings
    {
        quint16 internalPort = 0;   // Mirror server, always bound to 127.0.0.1
        QString publicBind;         // LanRelay listen address
        quint16 publicPort = 0;     // LanRelay listen port = the port the web page connects to
    };
    struct NginxSettings
    {
        bool enabled = false;
        quint16 port = 0;
        // nginx.exe (read by the start scripts / nginx config generation, not by the app):
        // exactly as configured (default "nginx\nginx.exe" = the nginx bundled in the package),
        // and resolved: relative to the folder of config.json, like dataDir.
        QString exe;
        QString resolvedExe;
    };
    // log.quiet / log.full (spec §2 "log"): one daily file each, see App/applog.h.
    struct LogFileSettings
    {
        bool enabled = true;
        int keepDays = 1;   // >= 1: files of the last keepDays days (today included) are kept
    };
    struct LogSettings
    {
        QString dir;            // absolute: log.dir resolved against resolvedDataDir()
        LogFileSettings quiet;  // taidaflow-YYYY-MM-DD.log: warning / critical / fatal
        LogFileSettings full;   // taidaflow-YYYY-MM-DD-full.log: every message
    };

    // Where a value comes from (log, diagnostics).
    enum class ValueSource { File, Default, Environment };
    // Where the configuration as a whole comes from.
    enum class FileSource { EnvironmentVariable, BesideExecutable, Created, InMemoryDefaults };

    // ---- construction ------------------------------------------------------------------
    // Default values (spec §2), no file.
    AppConfig();

    static AppConfig defaults() { return AppConfig(); }
    // The default config.json: ordered as in spec §2, 2-space indent, UTF-8, no BOM,
    // trailing newline.
    static QByteArray defaultJson();
    // Keys of every setting in file order, e.g. "devices.adam6256.host".
    static QStringList keys();

    // ---- loading -----------------------------------------------------------------------
    using LogEntry = AppConfigLogEntry;
    using LoadResult = AppConfigLoadResult;   // defined below the class

    // Explicit inputs (unit tests): configPathEnv = value of TAIDAFLOW_CONFIG (nullopt = not
    // set; an empty value counts as not set), executableDir = folder of the exe,
    // downloadPortEnv = value of TAIDAFLOW_DOWNLOAD_PORT (nullopt = not set).
    static LoadResult load(const std::optional<QString> &configPathEnv, const QString &executableDir,
                           const std::optional<QString> &downloadPortEnv);
    // The application's inputs: environment variables + QCoreApplication::applicationDirPath().
    // Needs a Q(Core)Application.
    static LoadResult loadFromEnvironment();

    // Writes the default file to `path` (QSaveFile; creates the parent folder). Never
    // overwrites an existing file. Returns kWriteOk / kWriteFailed / kWriteTargetExists.
    static int writeDefaultFile(const QString &path, QString *message);

    // `--write-default-config <path>`: when `arguments` (QCoreApplication::arguments())
    // contain the option, writes the default file, prints one line to stdout (success) or
    // stderr (failure) and returns the process exit code; otherwise std::nullopt (normal start).
    static std::optional<int> runWriteDefaultConfigCommand(const QStringList &arguments);

    // ---- application instance -----------------------------------------------------------
    // The configuration loaded by main (defaults until setInstance() is called).
    static const AppConfig &instance();
    static void setInstance(const AppConfig &config);

    // ---- getters -----------------------------------------------------------------------
    int version() const;
    // dataDir exactly as configured, and resolved against the folder of config.json.
    QString dataDir() const;
    QString resolvedDataDir() const;
    // Creates resolvedDataDir() and makes it the working directory. On failure logs a
    // warning, keeps the current working directory and returns false (spec §1.7).
    bool applyDataDir() const;

    TcpDevice device(Device device) const;
    SerialDevice ms300() const;
    ModbusServerSettings modbusServer() const;
    Listener http() const;
    Listener rest() const;
    MirrorSettings mirror() const;
    NginxSettings nginx() const;
    // nginx.exe resolved against baseDir() (absolute nginx.exe values unchanged, cleaned).
    QString resolvedNginxExe() const;
    // log.dir exactly as configured, and resolved: a relative log.dir is relative to
    // resolvedDataDir() (default "logs" -> C:/TaidaFlowData/logs).
    QString logDir() const;
    QString resolvedLogDir() const;
    // log.* with dir = resolvedLogDir() (input of AppLog::install()).
    LogSettings logSettings() const;
    // w2-064 A2: log settings when config.json cannot be used (the program exits before
    // dataDir / log.dir are known): <folder of configPath>/logs, default enabled / keepDays.
    static LogSettings fallbackLogSettings(const QString &configPath);
    // Port of the download links sent to the page: TAIDAFLOW_DOWNLOAD_PORT when set and
    // valid, else nginx.enabled ? nginx.port : http.port.
    quint16 downloadPort() const;
    ValueSource downloadPortSource() const;

    // Generic access by key (keys()).
    QJsonValue value(const QString &key) const;
    ValueSource source(const QString &key) const;
    // Folder relative dataDir values are resolved against (folder of config.json; the exe
    // folder for in-memory defaults).
    QString baseDir() const;

    static QString deviceKey(Device device);   // "adam6256", ...
    static QString sourceName(ValueSource source);
    static QString fileSourceName(FileSource source);

private:
    QString string(const QString &key) const;
    int integer(const QString &key) const;

    QHash<QString, QJsonValue> m_values;
    QHash<QString, ValueSource> m_sources;
    QString m_baseDir;
    std::optional<quint16> m_downloadPortOverride;
};

// Result of AppConfig::load() / loadFromEnvironment().
struct AppConfigLoadResult
{
    bool ok = false;               // false = config.json unusable, exit (spec §1.4)
    AppConfig config;              // valid when ok
    QString path;                  // absolute path of config.json (used or intended)
    AppConfig::FileSource fileSource = AppConfig::FileSource::InMemoryDefaults;
    // Error details when !ok.
    QString error;                 // parser message
    int errorLine = 0;             // 1-based, 0 = unknown
    int errorColumn = 0;           // 1-based, 0 = unknown
    QList<AppConfigLogEntry> log;  // everything to log, in order

    // Text for the error dialog: file path, line/column, error, and
    // "請修正 config.json 或刪除它讓程式重建預設值".
    QString errorDialogText() const;
    // Writes `log` with qInfo()/qWarning() (prefix "[Config]").
    void printLog() const;
    bool hasWarning() const;
};
