// w2-061: AppConfig (App/appconfig.{h,cpp}) - docs/taidaflow_config_spec.md §1/§2.
// Every case uses real files in a QTemporaryDir; nothing is simulated.
#include "appconfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

namespace {

// Spec §2 verbatim (the reference the defaults are checked against).
const char *const kSpecDefaultJson = R"({
  "version": 1,
  "dataDir": "C:\\TaidaFlowData",
  "devices": {
    "adam6256":  { "host": "192.168.1.201", "port": 502, "unitId": 1 },
    "adam6217a": { "host": "192.168.1.202", "port": 502, "unitId": 1 },
    "adam6217b": { "host": "192.168.1.203", "port": 502, "unitId": 1 },
    "adam6224":  { "host": "192.168.1.204", "port": 502, "unitId": 1 },
    "adam6022":  { "host": "192.168.1.205", "port": 502, "unitId": 1 },
    "ms300": { "serialPort": "COM2", "baudRate": 9600, "dataBits": 8, "parity": "none", "stopBits": 1, "unitId": 1 }
  },
  "modbusServer": { "bind": "0.0.0.0", "port": 502, "unitId": 1 },
  "http":         { "bind": "0.0.0.0", "port": 8124 },
  "rest":         { "bind": "127.0.0.1", "port": 18080 },
  "mirror":       { "internalPort": 18125, "publicBind": "0.0.0.0", "publicPort": 8125 },
  "nginx":        { "enabled": true, "port": 80, "exe": "nginx\\nginx.exe" },
  "log":          { "dir": "logs", "quiet": { "enabled": true, "keepDays": 60 }, "full": { "enabled": true, "keepDays": 7 } }
})";

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray("<cannot read>");
    return file.readAll();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(bytes) == bytes.size();
}

QString logText(const AppConfig::LoadResult &r)
{
    QStringList lines;
    for (const AppConfig::LogEntry &e : r.log)
        lines.append((e.warning ? QStringLiteral("W ") : QStringLiteral("I ")) + e.text);
    return lines.join(QLatin1Char('\n'));
}

bool hasWarningContaining(const AppConfig::LoadResult &r, const QString &text)
{
    for (const AppConfig::LogEntry &e : r.log) {
        if (e.warning && e.text.contains(text))
            return true;
    }
    return false;
}

bool hasInfoContaining(const AppConfig::LoadResult &r, const QString &text)
{
    for (const AppConfig::LogEntry &e : r.log) {
        if (!e.warning && e.text.contains(text))
            return true;
    }
    return false;
}

// Loads `json` written as <dir>/config.json via TAIDAFLOW_CONFIG.
AppConfig::LoadResult loadJson(const QTemporaryDir &dir, const QByteArray &json)
{
    const QString path = dir.filePath(QStringLiteral("config.json"));
    writeFile(path, json);
    return AppConfig::load(path, dir.filePath(QStringLiteral("exe")), std::nullopt);
}

} // namespace

class tst_AppConfig : public QObject
{
    Q_OBJECT

private slots:
    void defaultsMatchSpec();
    void defaultGetters();
    void defaultFileFormat();
    void missingFileCreatesDefault();
    void environmentPathIsUsed();
    void environmentPathMissingCreatesThere();
    void unwritableFolderUsesInMemoryDefaults();
    void jsonErrorDoesNotOverwrite_data();
    void jsonErrorDoesNotOverwrite();
    void missingKeysUseDefaults();
    void unknownKeysIgnored();
    void invalidValuesUseDefaults_data();
    void invalidValuesUseDefaults();
    void groupNotObjectUsesDefaults();
    void logGroupNotObjectUsesDefaults();
    void logAndNginxExeFromFile();
    void logMissingKeysUseDefaults();
    void nginxExeResolution();
    void relativeLogDir();
    void validValuesAreNormalised();
    void utf8BomAccepted();
    void logListsEveryValueAndSource();
    void relativeDataDir();
    void absoluteDataDir();
    void dataDirCannotBeCreated();
    void downloadPort();
    void writeDefaultConfigCommand();
    void writeDefaultConfigCommandErrors();
};

void tst_AppConfig::defaultsMatchSpec()
{
    QJsonParseError error;
    const QJsonDocument spec = QJsonDocument::fromJson(kSpecDefaultJson, &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    const QJsonDocument written = QJsonDocument::fromJson(AppConfig::defaultJson(), &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    QCOMPARE(written.object(), spec.object());
}

void tst_AppConfig::defaultGetters()
{
    const AppConfig c = AppConfig::defaults();
    QCOMPARE(c.version(), 1);
    QCOMPARE(c.dataDir(), QStringLiteral("C:\\TaidaFlowData"));
    QCOMPARE(c.device(AppConfig::Device::Adam6256).host, QStringLiteral("192.168.1.201"));
    QCOMPARE(c.device(AppConfig::Device::Adam6217A).host, QStringLiteral("192.168.1.202"));
    QCOMPARE(c.device(AppConfig::Device::Adam6217B).host, QStringLiteral("192.168.1.203"));
    QCOMPARE(c.device(AppConfig::Device::Adam6224).host, QStringLiteral("192.168.1.204"));
    QCOMPARE(c.device(AppConfig::Device::Adam6022).host, QStringLiteral("192.168.1.205"));
    QCOMPARE(c.device(AppConfig::Device::Adam6022).port, quint16(502));
    QCOMPARE(c.device(AppConfig::Device::Adam6022).unitId, 1);
    const AppConfig::SerialDevice ms = c.ms300();
    QCOMPARE(ms.serialPort, QStringLiteral("COM2"));
    QCOMPARE(ms.baudRate, 9600);
    QCOMPARE(ms.dataBits, 8);
    QCOMPARE(ms.parity, QStringLiteral("none"));
    QCOMPARE(ms.stopBits, 1);
    QCOMPARE(ms.unitId, 1);
    QCOMPARE(c.modbusServer().bind, QStringLiteral("0.0.0.0"));
    QCOMPARE(c.modbusServer().port, quint16(502));
    QCOMPARE(c.modbusServer().unitId, 1);
    QCOMPARE(c.http().bind, QStringLiteral("0.0.0.0"));
    QCOMPARE(c.http().port, quint16(8124));
    QCOMPARE(c.rest().bind, QStringLiteral("127.0.0.1"));
    QCOMPARE(c.rest().port, quint16(18080));
    QCOMPARE(c.mirror().internalPort, quint16(18125));
    QCOMPARE(c.mirror().publicBind, QStringLiteral("0.0.0.0"));
    QCOMPARE(c.mirror().publicPort, quint16(8125));
    QCOMPARE(c.nginx().enabled, true);
    QCOMPARE(c.nginx().port, quint16(80));
    QCOMPARE(c.nginx().exe, QStringLiteral("nginx\\nginx.exe"));
    QCOMPARE(c.downloadPort(), quint16(80));
    // w2-064: log section.
    QCOMPARE(c.logDir(), QStringLiteral("logs"));
    QCOMPARE(c.resolvedLogDir(), QStringLiteral("C:/TaidaFlowData/logs"));
    const AppConfig::LogSettings log = c.logSettings();
    QCOMPARE(log.dir, QStringLiteral("C:/TaidaFlowData/logs"));
    QCOMPARE(log.quiet.enabled, true);
    QCOMPARE(log.quiet.keepDays, 60);
    QCOMPARE(log.full.enabled, true);
    QCOMPARE(log.full.keepDays, 7);
    for (const QString &key : AppConfig::keys())
        QCOMPARE(c.source(key), AppConfig::ValueSource::Default);
}

void tst_AppConfig::defaultFileFormat()
{
    const QByteArray bytes = AppConfig::defaultJson();
    QVERIFY(!bytes.startsWith("\xEF\xBB\xBF"));                   // no BOM
    QVERIFY(bytes.startsWith("{\n  \"version\": 1,\n  \"dataDir\": \"C:\\\\TaidaFlowData\",\n"));
    QVERIFY(bytes.endsWith("}\n"));
    QVERIFY(bytes.contains("\n    \"adam6256\": {\n      \"host\": \"192.168.1.201\",\n"));   // 2-space indent
    QVERIFY(bytes.indexOf("\"devices\"") < bytes.indexOf("\"modbusServer\""));
    QVERIFY(bytes.indexOf("\"mirror\"") < bytes.indexOf("\"nginx\""));
    // w2-064: nginx.exe after nginx.port, then the log section (3 levels deep).
    QVERIFY(bytes.contains("\n  \"nginx\": {\n    \"enabled\": true,\n    \"port\": 80,\n"
                           "    \"exe\": \"nginx\\\\nginx.exe\"\n  },\n"));
    QVERIFY(bytes.contains("\n  \"log\": {\n    \"dir\": \"logs\",\n    \"quiet\": {\n      \"enabled\": true,\n"
                           "      \"keepDays\": 60\n    },\n    \"full\": {\n      \"enabled\": true,\n"
                           "      \"keepDays\": 7\n    }\n  }\n}\n"));
    QVERIFY(bytes.indexOf("\"nginx\"") < bytes.indexOf("\"log\""));
    QVERIFY(!bytes.contains('\r'));
    QCOMPARE(QString::fromUtf8(bytes).toUtf8(), bytes);           // valid UTF-8
}

void tst_AppConfig::missingFileCreatesDefault()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString exeDir = dir.filePath(QStringLiteral("app"));
    QVERIFY(QDir().mkpath(exeDir));
    const AppConfig::LoadResult r = AppConfig::load(std::nullopt, exeDir, std::nullopt);
    QVERIFY2(r.ok, qPrintable(logText(r)));
    const QString expected = QDir(exeDir).filePath(QStringLiteral("config.json"));
    QCOMPARE(r.path, QDir::cleanPath(expected));
    QCOMPARE(r.fileSource, AppConfig::FileSource::Created);
    QCOMPARE(readAll(expected), AppConfig::defaultJson());
    QVERIFY(hasInfoContaining(r, QStringLiteral("created it with the default values")));
    for (const QString &key : AppConfig::keys())
        QCOMPARE(r.config.source(key), AppConfig::ValueSource::Default);

    // Second start: the file now exists and is read (source file for every value).
    const AppConfig::LoadResult again = AppConfig::load(std::nullopt, exeDir, std::nullopt);
    QVERIFY(again.ok);
    QCOMPARE(again.fileSource, AppConfig::FileSource::BesideExecutable);
    for (const QString &key : AppConfig::keys())
        QCOMPARE(again.config.source(key), AppConfig::ValueSource::File);
    QCOMPARE(readAll(expected), AppConfig::defaultJson());
}

void tst_AppConfig::environmentPathIsUsed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString exeDir = dir.filePath(QStringLiteral("app"));
    QVERIFY(QDir().mkpath(exeDir));
    const QString path = dir.filePath(QStringLiteral("elsewhere/my-config.json"));
    QVERIFY(writeFile(path, R"({"mirror": {"internalPort": 18200, "publicBind": "127.0.0.1", "publicPort": 8200}})"));

    const AppConfig::LoadResult r = AppConfig::load(path, exeDir, std::nullopt);
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QCOMPARE(r.fileSource, AppConfig::FileSource::EnvironmentVariable);
    QCOMPARE(r.path, QDir::cleanPath(path));
    QCOMPARE(r.config.mirror().internalPort, quint16(18200));
    QCOMPARE(r.config.mirror().publicBind, QStringLiteral("127.0.0.1"));
    QCOMPARE(r.config.mirror().publicPort, quint16(8200));
    QCOMPARE(r.config.source(QStringLiteral("mirror.publicPort")), AppConfig::ValueSource::File);
    // The exe folder is not touched.
    QVERIFY(!QFileInfo::exists(QDir(exeDir).filePath(QStringLiteral("config.json"))));
    QVERIFY(hasInfoContaining(r, QStringLiteral("TAIDAFLOW_CONFIG=")));

    // Empty TAIDAFLOW_CONFIG counts as not set -> exe folder.
    const AppConfig::LoadResult empty = AppConfig::load(QStringLiteral("  "), exeDir, std::nullopt);
    QVERIFY(empty.ok);
    QCOMPARE(empty.fileSource, AppConfig::FileSource::Created);
    QVERIFY(QFileInfo::exists(QDir(exeDir).filePath(QStringLiteral("config.json"))));
}

void tst_AppConfig::environmentPathMissingCreatesThere()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("new/folder/config.json"));
    const AppConfig::LoadResult r = AppConfig::load(path, dir.filePath(QStringLiteral("app")), std::nullopt);
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QCOMPARE(r.fileSource, AppConfig::FileSource::Created);
    QCOMPARE(readAll(path), AppConfig::defaultJson());
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("app/config.json"))));
}

void tst_AppConfig::unwritableFolderUsesInMemoryDefaults()
{
    // The "folder" of config.json is a regular file: the default file cannot be created.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString notAFolder = dir.filePath(QStringLiteral("plain-file"));
    QVERIFY(writeFile(notAFolder, "x"));
    const AppConfig::LoadResult r = AppConfig::load(std::nullopt, notAFolder, std::nullopt);
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QCOMPARE(r.fileSource, AppConfig::FileSource::InMemoryDefaults);
    QVERIFY(hasWarningContaining(r, QStringLiteral("in-memory default values")));
    QCOMPARE(r.config.mirror().publicPort, quint16(8125));
    QCOMPARE(readAll(notAFolder), QByteArray("x"));
}

void tst_AppConfig::jsonErrorDoesNotOverwrite_data()
{
    QTest::addColumn<QByteArray>("json");
    QTest::addColumn<int>("line");
    QTest::addColumn<int>("column");
    // Line/column (1-based) of the offending character.
    QTest::newRow("missing value") << QByteArray("{\n  \"version\": 1,\n  \"dataDir\": }\n") << 3 << 14;
    QTest::newRow("trailing comma") << QByteArray("{\n  \"mirror\": { \"publicPort\": 8126, }\n}\n") << 2 << 35;
    QTest::newRow("empty file") << QByteArray() << 1 << 1;
    QTest::newRow("top level array") << QByteArray("[1, 2]") << 1 << 1;
    QTest::newRow("comment") << QByteArray("{\n  // my IPs\n  \"version\": 1\n}") << 2 << 3;
}

void tst_AppConfig::jsonErrorDoesNotOverwrite()
{
    QFETCH(QByteArray, json);
    QFETCH(int, line);
    QFETCH(int, column);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("config.json"));
    QVERIFY(writeFile(path, json));
    const QDateTime before = QFileInfo(path).lastModified();

    const AppConfig::LoadResult r = AppConfig::load(path, dir.path(), std::nullopt);
    QVERIFY(!r.ok);
    QCOMPARE(readAll(path), json);                               // not overwritten
    QCOMPARE(QFileInfo(path).lastModified(), before);
    QVERIFY(!r.error.isEmpty());
    QCOMPARE(r.errorLine, line);
    QCOMPARE(r.errorColumn, column);
    QVERIFY(hasWarningContaining(r, QStringLiteral("left unchanged")));
    const QString text = r.errorDialogText();
    QVERIFY2(text.contains(QDir::toNativeSeparators(QDir::cleanPath(path))), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("第 %1 行,第 %2 欄").arg(line).arg(column)), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("請修正 config.json 或刪除它讓程式重建預設值")));
    QVERIFY(text.contains(r.error));
}

void tst_AppConfig::missingKeysUseDefaults()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray json = R"({"mirror": {"publicPort": 9000}})";
    const AppConfig::LoadResult r = loadJson(dir, json);
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QCOMPARE(r.config.mirror().publicPort, quint16(9000));
    QCOMPARE(r.config.source(QStringLiteral("mirror.publicPort")), AppConfig::ValueSource::File);
    QCOMPARE(r.config.mirror().internalPort, quint16(18125));
    QCOMPARE(r.config.source(QStringLiteral("mirror.internalPort")), AppConfig::ValueSource::Default);
    QCOMPARE(r.config.device(AppConfig::Device::Adam6224).host, QStringLiteral("192.168.1.204"));
    QVERIFY(hasInfoContaining(r, QStringLiteral("\"mirror.internalPort\" is missing - using the default 18125")));
    QVERIFY(hasInfoContaining(r, QStringLiteral("\"devices.ms300.serialPort\" is missing")));
    QVERIFY(!r.hasWarning());                                    // missing keys are not warnings
    QCOMPARE(readAll(dir.filePath(QStringLiteral("config.json"))), json);   // not written back
}

void tst_AppConfig::unknownKeysIgnored()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const AppConfig::LoadResult r = loadJson(dir, R"({
        "foo": 1,
        "devices": {"adam9999": {"host": "1.2.3.4"}, "adam6256": {"host": "10.0.0.1", "colour": "red"}},
        "mirror": {"extra": true, "publicPort": 8200}
    })");
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QVERIFY(hasInfoContaining(r, QStringLiteral("unknown key \"foo\" ignored")));
    QVERIFY(hasInfoContaining(r, QStringLiteral("unknown key \"devices.adam9999\" ignored")));
    QVERIFY(hasInfoContaining(r, QStringLiteral("unknown key \"devices.adam6256.colour\" ignored")));
    QVERIFY(hasInfoContaining(r, QStringLiteral("unknown key \"mirror.extra\" ignored")));
    QCOMPARE(r.config.device(AppConfig::Device::Adam6256).host, QStringLiteral("10.0.0.1"));
    QCOMPARE(r.config.mirror().publicPort, quint16(8200));
}

void tst_AppConfig::invalidValuesUseDefaults_data()
{
    QTest::addColumn<QByteArray>("json");
    QTest::addColumn<QString>("key");
    QTest::addColumn<QString>("expectedDefault");

    QTest::newRow("port > 65535") << QByteArray(R"({"mirror": {"publicPort": 70000}})") << "mirror.publicPort" << "8125";
    QTest::newRow("port 0") << QByteArray(R"({"http": {"port": 0}})") << "http.port" << "8124";
    QTest::newRow("port as string") << QByteArray(R"({"rest": {"port": "18080"}})") << "rest.port" << "18080";
    QTest::newRow("port fraction") << QByteArray(R"({"nginx": {"port": 80.5}})") << "nginx.port" << "80";
    QTest::newRow("host bad ip") << QByteArray(R"({"devices": {"adam6224": {"host": "192.168.1.300"}}})")
                                 << "devices.adam6224.host" << "\"192.168.1.204\"";
    QTest::newRow("host number") << QByteArray(R"({"devices": {"adam6022": {"host": 12}}})")
                                 << "devices.adam6022.host" << "\"192.168.1.205\"";
    QTest::newRow("bind hostname") << QByteArray(R"({"modbusServer": {"bind": "plant-pc"}})")
                                   << "modbusServer.bind" << "\"0.0.0.0\"";
    QTest::newRow("unitId 300") << QByteArray(R"({"devices": {"adam6256": {"unitId": 300}}})")
                                << "devices.adam6256.unitId" << "1";
    QTest::newRow("ms300 unitId 0") << QByteArray(R"({"devices": {"ms300": {"unitId": 0}}})")
                                    << "devices.ms300.unitId" << "1";
    QTest::newRow("baud 1234") << QByteArray(R"({"devices": {"ms300": {"baudRate": 1234}}})")
                               << "devices.ms300.baudRate" << "9600";
    QTest::newRow("dataBits 9") << QByteArray(R"({"devices": {"ms300": {"dataBits": 9}}})")
                                << "devices.ms300.dataBits" << "8";
    QTest::newRow("parity foo") << QByteArray(R"({"devices": {"ms300": {"parity": "foo"}}})")
                                << "devices.ms300.parity" << "\"none\"";
    QTest::newRow("stopBits 3") << QByteArray(R"({"devices": {"ms300": {"stopBits": 3}}})")
                                << "devices.ms300.stopBits" << "1";
    QTest::newRow("serialPort empty") << QByteArray(R"({"devices": {"ms300": {"serialPort": ""}}})")
                                      << "devices.ms300.serialPort" << "\"COM2\"";
    QTest::newRow("enabled string") << QByteArray(R"({"nginx": {"enabled": "yes"}})") << "nginx.enabled" << "true";
    QTest::newRow("dataDir number") << QByteArray(R"({"dataDir": 5})") << "dataDir" << "\"C:\\\\TaidaFlowData\"";
    QTest::newRow("version 2") << QByteArray(R"({"version": 2})") << "version" << "1";
    // w2-064: nginx.exe and log.
    QTest::newRow("nginx.exe empty") << QByteArray(R"({"nginx": {"exe": "  "}})")
                                     << "nginx.exe" << "\"nginx\\\\nginx.exe\"";
    QTest::newRow("nginx.exe number") << QByteArray(R"({"nginx": {"exe": 1}})")
                                      << "nginx.exe" << "\"nginx\\\\nginx.exe\"";
    QTest::newRow("log.dir empty") << QByteArray(R"({"log": {"dir": ""}})") << "log.dir" << "\"logs\"";
    QTest::newRow("log.dir number") << QByteArray(R"({"log": {"dir": 3}})") << "log.dir" << "\"logs\"";
    QTest::newRow("quiet.enabled string") << QByteArray(R"({"log": {"quiet": {"enabled": "yes"}}})")
                                          << "log.quiet.enabled" << "true";
    QTest::newRow("full.enabled number") << QByteArray(R"({"log": {"full": {"enabled": 0}}})")
                                         << "log.full.enabled" << "true";
    QTest::newRow("quiet.keepDays 0") << QByteArray(R"({"log": {"quiet": {"keepDays": 0}}})")
                                      << "log.quiet.keepDays" << "60";
    QTest::newRow("quiet.keepDays -5") << QByteArray(R"({"log": {"quiet": {"keepDays": -5}}})")
                                       << "log.quiet.keepDays" << "60";
    QTest::newRow("quiet.keepDays 1.5") << QByteArray(R"({"log": {"quiet": {"keepDays": 1.5}}})")
                                        << "log.quiet.keepDays" << "60";
    QTest::newRow("quiet.keepDays string") << QByteArray(R"({"log": {"quiet": {"keepDays": "30"}}})")
                                           << "log.quiet.keepDays" << "60";
    QTest::newRow("full.keepDays 0") << QByteArray(R"({"log": {"full": {"keepDays": 0}}})")
                                     << "log.full.keepDays" << "7";
    QTest::newRow("full.keepDays huge") << QByteArray(R"({"log": {"full": {"keepDays": 1e12}}})")
                                        << "log.full.keepDays" << "7";
    QTest::newRow("full.keepDays null") << QByteArray(R"({"log": {"full": {"keepDays": null}}})")
                                        << "log.full.keepDays" << "7";
}

void tst_AppConfig::invalidValuesUseDefaults()
{
    QFETCH(QByteArray, json);
    QFETCH(QString, key);
    QFETCH(QString, expectedDefault);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const AppConfig::LoadResult r = loadJson(dir, json);
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QCOMPARE(r.config.source(key), AppConfig::ValueSource::Default);
    QCOMPARE(r.config.value(key), AppConfig::defaults().value(key));
    QVERIFY2(hasWarningContaining(r, QStringLiteral("\"%1\" = ").arg(key)), qPrintable(logText(r)));
    QVERIFY2(hasWarningContaining(r, QStringLiteral("using the default %1").arg(expectedDefault)), qPrintable(logText(r)));
    QCOMPARE(readAll(dir.filePath(QStringLiteral("config.json"))), json);
}

void tst_AppConfig::groupNotObjectUsesDefaults()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const AppConfig::LoadResult r = loadJson(dir, R"({"devices": 5, "mirror": {"publicPort": 8300}})");
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QVERIFY(hasWarningContaining(r, QStringLiteral("\"devices\" = 5 is not an object")));
    QCOMPARE(r.config.device(AppConfig::Device::Adam6256).host, QStringLiteral("192.168.1.201"));
    QCOMPARE(r.config.source(QStringLiteral("devices.ms300.serialPort")), AppConfig::ValueSource::Default);
    QCOMPARE(r.config.mirror().publicPort, quint16(8300));
}

void tst_AppConfig::logGroupNotObjectUsesDefaults()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    AppConfig::LoadResult r = loadJson(dir, R"({"log": "yes"})");
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QVERIFY(hasWarningContaining(r, QStringLiteral("\"log\" = \"yes\" is not an object")));
    for (const QString &key : {QStringLiteral("log.dir"), QStringLiteral("log.quiet.enabled"),
                               QStringLiteral("log.quiet.keepDays"), QStringLiteral("log.full.enabled"),
                               QStringLiteral("log.full.keepDays")}) {
        QCOMPARE(r.config.source(key), AppConfig::ValueSource::Default);
    }
    // Nested group: log.full is not an object, log.quiet from the file still counts.
    r = loadJson(dir, R"({"log": {"quiet": {"keepDays": 30}, "full": [1, 2]}})");
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QVERIFY(hasWarningContaining(r, QStringLiteral("\"log.full\" = [1,2] is not an object")));
    QCOMPARE(r.config.logSettings().quiet.keepDays, 30);
    QCOMPARE(r.config.source(QStringLiteral("log.quiet.keepDays")), AppConfig::ValueSource::File);
    QCOMPARE(r.config.logSettings().full.keepDays, 7);
    QCOMPARE(r.config.source(QStringLiteral("log.full.keepDays")), AppConfig::ValueSource::Default);
}

void tst_AppConfig::logAndNginxExeFromFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const AppConfig::LoadResult r = loadJson(dir, R"({
        "dataDir": "data",
        "nginx": {"exe": " D:\\nginx-1.26\\nginx.exe "},
        "log": {"dir": "my logs", "quiet": {"enabled": false, "keepDays": 1}, "full": {"enabled": true, "keepDays": 365.0}}
    })");
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QVERIFY2(!r.hasWarning(), qPrintable(logText(r)));
    QCOMPARE(r.config.nginx().exe, QStringLiteral("D:\\nginx-1.26\\nginx.exe"));   // trimmed
    QCOMPARE(r.config.logDir(), QStringLiteral("my logs"));
    const AppConfig::LogSettings log = r.config.logSettings();
    QCOMPARE(log.quiet.enabled, false);
    QCOMPARE(log.quiet.keepDays, 1);                      // lower limit is valid
    QCOMPARE(log.full.enabled, true);
    QCOMPARE(log.full.keepDays, 365);
    for (const QString &key : {QStringLiteral("nginx.exe"), QStringLiteral("log.dir"),
                               QStringLiteral("log.quiet.enabled"), QStringLiteral("log.quiet.keepDays"),
                               QStringLiteral("log.full.enabled"), QStringLiteral("log.full.keepDays")}) {
        QCOMPARE(r.config.source(key), AppConfig::ValueSource::File);
    }
    QVERIFY(hasInfoContaining(r, QStringLiteral("  log.quiet.keepDays = 1 (file)")));
    QVERIFY(hasInfoContaining(r, QStringLiteral("  log.full.keepDays = 365 (file)")));
    QVERIFY(hasInfoContaining(r, QStringLiteral("  nginx.exe = \"D:\\\\nginx-1.26\\\\nginx.exe\" (file)")));
}

void tst_AppConfig::logMissingKeysUseDefaults()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A version-1 file written before w2-064 (no nginx.exe, no log): defaults, info only.
    const QByteArray json = R"({"nginx": {"enabled": true, "port": 80}})";
    const AppConfig::LoadResult r = loadJson(dir, json);
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QVERIFY2(!r.hasWarning(), qPrintable(logText(r)));
    QVERIFY(hasInfoContaining(r, QStringLiteral("\"nginx.exe\" is missing - using the default \"nginx\\\\nginx.exe\"")));
    QVERIFY(hasInfoContaining(r, QStringLiteral("\"log.dir\" is missing - using the default \"logs\"")));
    QVERIFY(hasInfoContaining(r, QStringLiteral("\"log.quiet.keepDays\" is missing - using the default 60")));
    QVERIFY(hasInfoContaining(r, QStringLiteral("\"log.full.keepDays\" is missing - using the default 7")));
    QCOMPARE(r.config.logSettings().quiet.keepDays, 60);
    QCOMPARE(r.config.logSettings().full.keepDays, 7);
    QCOMPARE(r.config.nginx().exe, QStringLiteral("nginx\\nginx.exe"));
    QCOMPARE(readAll(dir.filePath(QStringLiteral("config.json"))), json);   // not written back
}

void tst_AppConfig::nginxExeResolution()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString configDir = QDir::cleanPath(dir.filePath(QStringLiteral("install")));
    const QString path = QDir(configDir).filePath(QStringLiteral("config.json"));

    // Default "nginx\nginx.exe": relative to the folder of config.json (the install folder).
    QVERIFY(writeFile(path, R"({"dataDir": "data"})"));
    AppConfig::LoadResult r = AppConfig::load(path, dir.path(), std::nullopt);
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QCOMPARE(r.config.source(QStringLiteral("nginx.exe")), AppConfig::ValueSource::Default);
    const QString bundled = configDir + QStringLiteral("/nginx/nginx.exe");
    QCOMPARE(r.config.resolvedNginxExe(), bundled);
    QCOMPARE(r.config.nginx().exe, QStringLiteral("nginx\\nginx.exe"));
    QCOMPARE(r.config.nginx().resolvedExe, bundled);
    QVERIFY2(hasInfoContaining(r, QStringLiteral("  nginx.exe (resolved) = %1").arg(QDir::toNativeSeparators(bundled))),
             qPrintable(logText(r)));
    // Not relative to dataDir.
    QVERIFY(!r.config.resolvedNginxExe().startsWith(r.config.resolvedDataDir()));

    // Other relative paths (with "..", forward slashes) - same base, cleaned.
    QVERIFY(writeFile(path, R"({"nginx": {"exe": "..\\tools/nginx-1.26\\nginx.exe"}})"));
    r = AppConfig::load(path, dir.path(), std::nullopt);
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QCOMPARE(r.config.source(QStringLiteral("nginx.exe")), AppConfig::ValueSource::File);
    QCOMPARE(r.config.resolvedNginxExe(), QDir::cleanPath(dir.filePath(QStringLiteral("tools/nginx-1.26/nginx.exe"))));

    // Absolute path: used as is (only cleaned).
    QVERIFY(writeFile(path, R"({"nginx": {"exe": "C:\\tools\\nginx\\nginx.exe"}})"));
    r = AppConfig::load(path, dir.path(), std::nullopt);
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QCOMPARE(r.config.nginx().exe, QStringLiteral("C:\\tools\\nginx\\nginx.exe"));
    QCOMPARE(r.config.resolvedNginxExe(), QStringLiteral("C:/tools/nginx/nginx.exe"));
    QVERIFY(hasInfoContaining(r, QStringLiteral("  nginx.exe (resolved) = C:\\tools\\nginx\\nginx.exe")));

    // TAIDAFLOW_CONFIG missing + created: base = the folder of the created file.
    const QString created = dir.filePath(QStringLiteral("other/config.json"));
    r = AppConfig::load(created, dir.path(), std::nullopt);
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QCOMPARE(r.fileSource, AppConfig::FileSource::Created);
    QCOMPARE(r.config.resolvedNginxExe(), QDir::cleanPath(dir.filePath(QStringLiteral("other/nginx/nginx.exe"))));
}

void tst_AppConfig::relativeLogDir()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString configDir = dir.filePath(QStringLiteral("cfg"));
    const QString path = QDir(configDir).filePath(QStringLiteral("config.json"));

    // relative dataDir (to config.json) + relative log.dir (to dataDir)
    QVERIFY(writeFile(path, R"({"dataDir": "data/run", "log": {"dir": "logs/app"}})"));
    AppConfig::LoadResult r = AppConfig::load(path, dir.path(), std::nullopt);
    QVERIFY2(r.ok, qPrintable(logText(r)));
    const QString dataDir = QDir::cleanPath(QDir(configDir).filePath(QStringLiteral("data/run")));
    QCOMPARE(r.config.resolvedLogDir(), dataDir + QStringLiteral("/logs/app"));
    QCOMPARE(r.config.logSettings().dir, dataDir + QStringLiteral("/logs/app"));
    QVERIFY(hasInfoContaining(r, QStringLiteral("  log.dir (resolved) = %1")
                                     .arg(QDir::toNativeSeparators(dataDir + QStringLiteral("/logs/app")))));

    // "..": still resolved against dataDir (and cleaned)
    QVERIFY(writeFile(path, R"({"dataDir": "data/run", "log": {"dir": "../logs"}})"));
    r = AppConfig::load(path, dir.path(), std::nullopt);
    QVERIFY(r.ok);
    QCOMPARE(r.config.resolvedLogDir(), QDir::cleanPath(QDir(configDir).filePath(QStringLiteral("data/logs"))));

    // absolute log.dir: independent of dataDir
    const QString absolute = QDir::cleanPath(dir.filePath(QStringLiteral("abs-logs")));
    QVERIFY(writeFile(path, QJsonDocument(QJsonObject{
                                {QStringLiteral("dataDir"), QStringLiteral("data/run")},
                                {QStringLiteral("log"), QJsonObject{{QStringLiteral("dir"), QDir::toNativeSeparators(absolute)}}}})
                                .toJson()));
    r = AppConfig::load(path, dir.path(), std::nullopt);
    QVERIFY(r.ok);
    QCOMPARE(r.config.resolvedLogDir(), absolute);

    // resolved against the dataDir setting even before applyDataDir() changed the working folder
    QVERIFY(QDir::currentPath() != dataDir);
}

void tst_AppConfig::validValuesAreNormalised()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const AppConfig::LoadResult r = loadJson(dir, R"({
        "devices": {"adam6256": {"host": " 10.0.0.7 ", "port": 1502.0, "unitId": 3},
                    "ms300": {"serialPort": "COM7", "baudRate": 19200, "dataBits": 7, "parity": "EVEN", "stopBits": 2, "unitId": 5}},
        "modbusServer": {"bind": "127.0.0.1", "port": 1502, "unitId": 9},
        "http": {"bind": "::1", "port": 9124},
        "nginx": {"enabled": false, "port": 8080}
    })");
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QVERIFY2(!r.hasWarning(), qPrintable(logText(r)));
    const AppConfig::TcpDevice d = r.config.device(AppConfig::Device::Adam6256);
    QCOMPARE(d.host, QStringLiteral("10.0.0.7"));
    QCOMPARE(d.port, quint16(1502));
    QCOMPARE(d.unitId, 3);
    const AppConfig::SerialDevice ms = r.config.ms300();
    QCOMPARE(ms.serialPort, QStringLiteral("COM7"));
    QCOMPARE(ms.baudRate, 19200);
    QCOMPARE(ms.dataBits, 7);
    QCOMPARE(ms.parity, QStringLiteral("even"));
    QCOMPARE(ms.stopBits, 2);
    QCOMPARE(ms.unitId, 5);
    QCOMPARE(r.config.modbusServer().bind, QStringLiteral("127.0.0.1"));
    QCOMPARE(r.config.modbusServer().port, quint16(1502));
    QCOMPARE(r.config.modbusServer().unitId, 9);
    QCOMPARE(r.config.http().bind, QStringLiteral("::1"));
    QCOMPARE(r.config.http().port, quint16(9124));
    QCOMPARE(r.config.nginx().enabled, false);
    QCOMPARE(r.config.nginx().port, quint16(8080));
}

void tst_AppConfig::utf8BomAccepted()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const AppConfig::LoadResult r = loadJson(dir, QByteArray("\xEF\xBB\xBF") + R"({"mirror": {"publicPort": 8400}})");
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QCOMPARE(r.config.mirror().publicPort, quint16(8400));
    QVERIFY(hasInfoContaining(r, QStringLiteral("UTF-8 BOM")));
}

void tst_AppConfig::logListsEveryValueAndSource()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const AppConfig::LoadResult r = loadJson(dir, R"({"mirror": {"publicPort": 8500}})");
    QVERIFY(r.ok);
    QVERIFY(hasInfoContaining(r, QStringLiteral("configuration source: TAIDAFLOW_CONFIG")));
    QVERIFY(hasInfoContaining(r, QStringLiteral("  mirror.publicPort = 8500 (file)")));
    QVERIFY(hasInfoContaining(r, QStringLiteral("  mirror.internalPort = 18125 (default)")));
    for (const QString &key : AppConfig::keys())
        QVERIFY2(hasInfoContaining(r, QStringLiteral("  %1 = ").arg(key)), qPrintable(key));
    QVERIFY(hasInfoContaining(r, QStringLiteral("  dataDir (resolved) = ")));
    QVERIFY(hasInfoContaining(r, QStringLiteral("  downloadPort = 80 (derived")));
}

void tst_AppConfig::relativeDataDir()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString configDir = dir.filePath(QStringLiteral("cfg"));
    const QString path = QDir(configDir).filePath(QStringLiteral("config.json"));
    QVERIFY(writeFile(path, R"({"dataDir": "data/run"})"));
    const AppConfig::LoadResult r = AppConfig::load(path, dir.path(), std::nullopt);
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QCOMPARE(r.config.dataDir(), QStringLiteral("data/run"));
    const QString expected = QDir::cleanPath(QDir(configDir).filePath(QStringLiteral("data/run")));
    QCOMPARE(r.config.resolvedDataDir(), expected);

    const QString previous = QDir::currentPath();
    QVERIFY(!QFileInfo::exists(expected));
    const bool applied = r.config.applyDataDir();
    const QString now = QDir::currentPath();
    QDir::setCurrent(previous);
    QVERIFY(applied);
    QVERIFY(QFileInfo(expected).isDir());
    QCOMPARE(QFileInfo(now).canonicalFilePath(), QFileInfo(expected).canonicalFilePath());
}

void tst_AppConfig::absoluteDataDir()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString absolute = QDir::cleanPath(dir.filePath(QStringLiteral("abs-data")));
    const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("dataDir"), QDir::toNativeSeparators(absolute)}})
                                .toJson();
    const AppConfig::LoadResult r = loadJson(dir, json);
    QVERIFY2(r.ok, qPrintable(logText(r)));
    QCOMPARE(r.config.resolvedDataDir(), absolute);
    // Default (C:\TaidaFlowData) is absolute too: independent of the config folder.
    QCOMPARE(AppConfig::defaults().resolvedDataDir(), QStringLiteral("C:/TaidaFlowData"));
}

void tst_AppConfig::dataDirCannotBeCreated()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.filePath(QStringLiteral("blocker")), "x"));
    const AppConfig::LoadResult r = loadJson(dir, R"({"dataDir": "blocker/sub"})");
    QVERIFY(r.ok);
    const QString previous = QDir::currentPath();
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("cannot be created - the working directory stays")));
    QVERIFY(!r.config.applyDataDir());
    QCOMPARE(QDir::currentPath(), previous);
}

void tst_AppConfig::downloadPort()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("config.json"));

    QVERIFY(writeFile(path, R"({"nginx": {"enabled": true, "port": 8088}, "http": {"port": 9124}})"));
    AppConfig::LoadResult r = AppConfig::load(path, dir.path(), std::nullopt);
    QVERIFY(r.ok);
    QCOMPARE(r.config.downloadPort(), quint16(8088));
    QCOMPARE(r.config.downloadPortSource(), AppConfig::ValueSource::File);

    QVERIFY(writeFile(path, R"({"nginx": {"enabled": false, "port": 8088}, "http": {"port": 9124}})"));
    r = AppConfig::load(path, dir.path(), std::nullopt);
    QVERIFY(r.ok);
    QCOMPARE(r.config.downloadPort(), quint16(9124));

    r = AppConfig::load(path, dir.path(), QStringLiteral("8123"));
    QVERIFY(r.ok);
    QCOMPARE(r.config.downloadPort(), quint16(8123));
    QCOMPARE(r.config.downloadPortSource(), AppConfig::ValueSource::Environment);
    QVERIFY(hasInfoContaining(r, QStringLiteral("TAIDAFLOW_DOWNLOAD_PORT=8123 overrides")));

    r = AppConfig::load(path, dir.path(), QStringLiteral("abc"));
    QVERIFY(r.ok);
    QCOMPARE(r.config.downloadPort(), quint16(9124));
    QVERIFY(hasWarningContaining(r, QStringLiteral("TAIDAFLOW_DOWNLOAD_PORT=\"abc\" is not a port")));
}

void tst_AppConfig::writeDefaultConfigCommand()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(!AppConfig::runWriteDefaultConfigCommand({QStringLiteral("TaidaFlowApp.exe")}).has_value());
    QVERIFY(!AppConfig::runWriteDefaultConfigCommand({QStringLiteral("TaidaFlowApp.exe"), QStringLiteral("--other")})
                 .has_value());

    const QString path = dir.filePath(QStringLiteral("scripts/out/config.json"));
    std::optional<int> code = AppConfig::runWriteDefaultConfigCommand(
        {QStringLiteral("TaidaFlowApp.exe"), QStringLiteral("--write-default-config"), path});
    QVERIFY(code.has_value());
    QCOMPARE(*code, 0);
    const QByteArray written = readAll(path);
    QCOMPARE(written, AppConfig::defaultJson());
    QCOMPARE(QJsonDocument::fromJson(written).object(), QJsonDocument::fromJson(kSpecDefaultJson).object());

    // The written file loads without warnings, every value from the file = the defaults.
    const AppConfig::LoadResult r = AppConfig::load(path, dir.path(), std::nullopt);
    QVERIFY(r.ok);
    QVERIFY(!r.hasWarning());
    for (const QString &key : AppConfig::keys()) {
        QCOMPARE(r.config.source(key), AppConfig::ValueSource::File);
        QCOMPARE(r.config.value(key), AppConfig::defaults().value(key));
    }

    // "--write-default-config=<path>" form.
    const QString path2 = dir.filePath(QStringLiteral("eq.json"));
    code = AppConfig::runWriteDefaultConfigCommand(
        {QStringLiteral("TaidaFlowApp.exe"), QStringLiteral("--write-default-config=") + path2});
    QCOMPARE(code.value_or(-1), 0);
    QCOMPARE(readAll(path2), AppConfig::defaultJson());
}

void tst_AppConfig::writeDefaultConfigCommandErrors()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Existing file: never overwritten.
    const QString existing = dir.filePath(QStringLiteral("config.json"));
    QVERIFY(writeFile(existing, "{\"mine\": true}"));
    std::optional<int> code = AppConfig::runWriteDefaultConfigCommand(
        {QStringLiteral("TaidaFlowApp.exe"), QStringLiteral("--write-default-config"), existing});
    QCOMPARE(code.value_or(-1), AppConfig::kWriteTargetExists);
    QCOMPARE(readAll(existing), QByteArray("{\"mine\": true}"));

    // Not writable: the parent "folder" is a file.
    QVERIFY(writeFile(dir.filePath(QStringLiteral("file")), "x"));
    code = AppConfig::runWriteDefaultConfigCommand(
        {QStringLiteral("TaidaFlowApp.exe"), QStringLiteral("--write-default-config"),
         dir.filePath(QStringLiteral("file/config.json"))});
    QCOMPARE(code.value_or(-1), AppConfig::kWriteFailed);
    QVERIFY(code.value_or(0) != 0);

    // No path.
    code = AppConfig::runWriteDefaultConfigCommand(
        {QStringLiteral("TaidaFlowApp.exe"), QStringLiteral("--write-default-config")});
    QCOMPARE(code.value_or(-1), AppConfig::kWriteUsage);
}

QTEST_GUILESS_MAIN(tst_AppConfig)
#include "tst_appconfig.moc"
