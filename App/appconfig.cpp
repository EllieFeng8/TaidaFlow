#include "appconfig.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>

#include <climits>
#include <cmath>
#include <cstdio>
#include <functional>

namespace {

// ---- the settings table (spec §2) -------------------------------------------------------
// Order = order in the default file. Defaults verified against the core branch code (see the
// w2-061 report): Modbus_Client.cpp/.h, Ms300FaultReader.cpp, Modbus_Server.h, core.cpp,
// HistoryExport.h, App/main.cpp, deploy/nginx + scripts/nginx-*.ps1.

enum class Kind {
    Integer,    // whole number in [min, max]
    Port,       // whole number 1..65535
    Address,    // IPv4 / IPv6 address text (QHostAddress)
    Text,       // non-empty string
    Bool,       // true / false
    Choice,     // one of `choices` (case-insensitive, stored lower case)
    IntChoice,  // one of `intChoices`
};

struct Field
{
    QString key;
    Kind kind;
    QJsonValue defaultValue;
    int min = 0;
    int max = 0;
    QStringList choices;
    QList<int> intChoices;
};

Field field(const QString &key, Kind kind, const QJsonValue &def)
{
    return Field{key, kind, def, 0, 0, {}, {}};
}

Field field(const char *key, Kind kind, const QJsonValue &def)
{
    return field(QString::fromLatin1(key), kind, def);
}

Field integerField(const QString &key, const QJsonValue &def, int min, int max)
{
    Field f = field(key, Kind::Integer, def);
    f.min = min;
    f.max = max;
    return f;
}

void addTcpDevice(QList<Field> &fields, const char *name, const char *host)
{
    const QString prefix = QStringLiteral("devices.%1.").arg(QLatin1String(name));
    fields.append(field(prefix + QLatin1String("host"), Kind::Address, QString::fromLatin1(host)));
    fields.append(field(prefix + QLatin1String("port"), Kind::Port, 502));
    // Modbus TCP unit identifier: one byte.
    fields.append(integerField(prefix + QLatin1String("unitId"), 1, 0, 255));
}

const QList<Field> &fields()
{
    static const QList<Field> table = [] {
        QList<Field> f;
        f.append(integerField("version", AppConfig::kVersion, AppConfig::kVersion, AppConfig::kVersion));
        f.append(field("dataDir", Kind::Text, QStringLiteral("C:\\TaidaFlowData")));
        addTcpDevice(f, "adam6256", "192.168.1.201");
        addTcpDevice(f, "adam6217a", "192.168.1.202");
        addTcpDevice(f, "adam6217b", "192.168.1.203");
        addTcpDevice(f, "adam6224", "192.168.1.204");
        addTcpDevice(f, "adam6022", "192.168.1.205");
        f.append(field("devices.ms300.serialPort", Kind::Text, QStringLiteral("COM2")));
        Field baud = field("devices.ms300.baudRate", Kind::IntChoice, 9600);
        baud.intChoices = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};   // QSerialPort::BaudRate
        f.append(baud);
        f.append(integerField("devices.ms300.dataBits", 8, 5, 8));
        Field parity = field("devices.ms300.parity", Kind::Choice, QStringLiteral("none"));
        parity.choices = {QStringLiteral("none"), QStringLiteral("even"), QStringLiteral("odd"),
                          QStringLiteral("space"), QStringLiteral("mark")};
        f.append(parity);
        Field stop = field("devices.ms300.stopBits", Kind::IntChoice, 1);
        stop.intChoices = {1, 2};
        f.append(stop);
        // Modbus RTU slave address.
        f.append(integerField("devices.ms300.unitId", 1, 1, 247));
        f.append(field("modbusServer.bind", Kind::Address, QStringLiteral("0.0.0.0")));
        f.append(field("modbusServer.port", Kind::Port, 502));
        f.append(integerField("modbusServer.unitId", 1, 0, 255));
        f.append(field("http.bind", Kind::Address, QStringLiteral("0.0.0.0")));
        f.append(field("http.port", Kind::Port, 8124));
        f.append(field("rest.bind", Kind::Address, QStringLiteral("127.0.0.1")));
        f.append(field("rest.port", Kind::Port, 18080));
        f.append(field("mirror.internalPort", Kind::Port, 18125));
        f.append(field("mirror.publicBind", Kind::Address, QStringLiteral("0.0.0.0")));
        f.append(field("mirror.publicPort", Kind::Port, 8125));
        f.append(field("nginx.enabled", Kind::Bool, true));
        f.append(field("nginx.port", Kind::Port, 80));
        // w2-064 (spec §2 nginx.exe; Mango: nginx is bundled in the package, <install>\nginx\):
        // relative = relative to the folder of config.json (like dataDir), absolute = as is.
        // Read by the start scripts / nginx config generation; the app itself does not start nginx.
        f.append(field("nginx.exe", Kind::Text, QStringLiteral("nginx\\nginx.exe")));
        // w2-064 (spec §2 log): the app writes its own daily log files (App/applog.h).
        // dir: relative to dataDir. keepDays: whole number >= 1 (days kept, today included).
        f.append(field("log.dir", Kind::Text, QStringLiteral("logs")));
        f.append(field("log.quiet.enabled", Kind::Bool, true));
        f.append(integerField(QStringLiteral("log.quiet.keepDays"), 60, 1, INT_MAX));
        f.append(field("log.full.enabled", Kind::Bool, true));
        f.append(integerField(QStringLiteral("log.full.keepDays"), 7, 1, INT_MAX));
        return f;
    }();
    return table;
}

const Field *findField(const QString &key)
{
    for (const Field &f : fields()) {
        if (f.key == key)
            return &f;
    }
    return nullptr;
}

// True when `path` is a group of the table ("devices", "devices.adam6256", ...).
bool isGroup(const QString &path)
{
    const QString prefix = path + QLatin1Char('.');
    for (const Field &f : fields()) {
        if (f.key.startsWith(prefix))
            return true;
    }
    return false;
}

QString jsonText(const QJsonValue &value)
{
    if (value.isUndefined())
        return QStringLiteral("(missing)");
    const QByteArray array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(array.mid(1, array.size() - 2));   // strip "[" and "]"
}

bool wholeNumber(const QJsonValue &value, int min, int max, int *out)
{
    if (!value.isDouble())
        return false;
    const double d = value.toDouble();
    if (!std::isfinite(d) || std::floor(d) != d || d < min || d > max)
        return false;
    *out = int(d);
    return true;
}

// Validates `value` for `f`. Returns the normalised value, or Undefined + *reason.
QJsonValue validate(const Field &f, const QJsonValue &value, QString *reason)
{
    int number = 0;
    switch (f.kind) {
    case Kind::Integer:
        if (wholeNumber(value, f.min, f.max, &number))
            return number;
        *reason = QStringLiteral("expected an integer %1..%2").arg(f.min).arg(f.max);
        return QJsonValue(QJsonValue::Undefined);
    case Kind::Port:
        if (wholeNumber(value, 1, 65535, &number))
            return number;
        *reason = QStringLiteral("expected a port number 1..65535");
        return QJsonValue(QJsonValue::Undefined);
    case Kind::Address:
        if (value.isString()) {
            const QString text = value.toString().trimmed();
            QHostAddress address;
            if (!text.isEmpty() && address.setAddress(text))
                return text;
        }
        *reason = QStringLiteral("expected an IP address such as \"192.168.1.201\"");
        return QJsonValue(QJsonValue::Undefined);
    case Kind::Text:
        if (value.isString() && !value.toString().trimmed().isEmpty())
            return value.toString().trimmed();
        *reason = QStringLiteral("expected a non-empty string");
        return QJsonValue(QJsonValue::Undefined);
    case Kind::Bool:
        if (value.isBool())
            return value;
        *reason = QStringLiteral("expected true or false");
        return QJsonValue(QJsonValue::Undefined);
    case Kind::Choice:
        if (value.isString()) {
            const QString text = value.toString().trimmed().toLower();
            if (f.choices.contains(text))
                return text;
        }
        *reason = QStringLiteral("expected one of: %1").arg(f.choices.join(QStringLiteral(", ")));
        return QJsonValue(QJsonValue::Undefined);
    case Kind::IntChoice:
        if (wholeNumber(value, INT_MIN, INT_MAX, &number) && f.intChoices.contains(number))
            return number;
        {
            QStringList allowed;
            for (int v : f.intChoices)
                allowed.append(QString::number(v));
            *reason = QStringLiteral("expected one of: %1").arg(allowed.join(QStringLiteral(", ")));
        }
        return QJsonValue(QJsonValue::Undefined);
    }
    *reason = QStringLiteral("unsupported setting");
    return QJsonValue(QJsonValue::Undefined);
}

// ---- ordered writer for the default file ------------------------------------------------
struct Node
{
    QString name;
    bool leaf = false;
    QJsonValue value;
    QList<Node> children;
};

void insertNode(Node &parent, const QStringList &path, int index, const QJsonValue &value)
{
    const QString &name = path.at(index);
    if (index == path.size() - 1) {
        Node leaf;
        leaf.name = name;
        leaf.leaf = true;
        leaf.value = value;
        parent.children.append(leaf);
        return;
    }
    for (Node &child : parent.children) {
        if (!child.leaf && child.name == name) {
            insertNode(child, path, index + 1, value);
            return;
        }
    }
    Node group;
    group.name = name;
    parent.children.append(group);
    insertNode(parent.children.last(), path, index + 1, value);
}

void writeNode(const Node &node, int indent, QByteArray &out)
{
    const QByteArray pad(indent, ' ');
    const QByteArray childPad(indent + 2, ' ');
    out += "{\n";
    for (int i = 0; i < node.children.size(); ++i) {
        const Node &child = node.children.at(i);
        out += childPad;
        out += jsonText(child.name).toUtf8();
        out += ": ";
        if (child.leaf)
            out += jsonText(child.value).toUtf8();
        else
            writeNode(child, indent + 2, out);
        if (i + 1 < node.children.size())
            out += ',';
        out += '\n';
    }
    out += pad;
    out += '}';
}

// 1-based line / column of a byte offset in `bytes` (column counted in characters).
void lineAndColumn(const QByteArray &bytes, int offset, int *line, int *column)
{
    offset = qBound(0, offset, int(bytes.size()));
    const QByteArray before = bytes.left(offset);
    *line = int(before.count('\n')) + 1;
    const int lineStart = int(before.lastIndexOf('\n')) + 1;
    *column = int(QString::fromUtf8(before.mid(lineStart)).size()) + 1;
}

QString nativePath(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

AppConfig &globalInstance()
{
    static AppConfig config;
    return config;
}

} // namespace

// ==== AppConfig ==========================================================================

AppConfig::AppConfig()
{
    for (const Field &f : fields()) {
        m_values.insert(f.key, f.defaultValue);
        m_sources.insert(f.key, ValueSource::Default);
    }
    m_baseDir = QDir::currentPath();
}

QStringList AppConfig::keys()
{
    QStringList list;
    for (const Field &f : fields())
        list.append(f.key);
    return list;
}

QByteArray AppConfig::defaultJson()
{
    Node root;
    for (const Field &f : fields())
        insertNode(root, f.key.split(QLatin1Char('.')), 0, f.defaultValue);
    QByteArray out;
    writeNode(root, 0, out);
    out += '\n';
    return out;
}

int AppConfig::writeDefaultFile(const QString &path, QString *message)
{
    const auto result = [message](int code, const QString &text) {
        if (message)
            *message = text;
        return code;
    };
    if (path.trimmed().isEmpty())
        return result(kWriteFailed, QStringLiteral("empty path"));
    const QFileInfo info(path);
    const QString absolute = nativePath(info.absoluteFilePath());
    if (info.exists())
        return result(kWriteTargetExists,
                      QStringLiteral("%1 already exists - not overwritten").arg(absolute));
    if (!QDir().mkpath(info.absolutePath()))
        return result(kWriteFailed, QStringLiteral("cannot create the folder %1")
                                        .arg(nativePath(info.absolutePath())));
    QSaveFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly))
        return result(kWriteFailed, QStringLiteral("cannot write %1: %2").arg(absolute, file.errorString()));
    const QByteArray bytes = defaultJson();
    if (file.write(bytes) != bytes.size() || !file.commit())
        return result(kWriteFailed, QStringLiteral("cannot write %1: %2").arg(absolute, file.errorString()));
    return result(kWriteOk, QStringLiteral("wrote the default configuration to %1").arg(absolute));
}

std::optional<int> AppConfig::runWriteDefaultConfigCommand(const QStringList &arguments)
{
    const QString option = QString::fromLatin1(kWriteDefaultOption);
    const QString optionEq = option + QLatin1Char('=');
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &arg = arguments.at(i);
        QString path;
        if (arg == option) {
            if (i + 1 < arguments.size())
                path = arguments.at(i + 1);
        } else if (arg.startsWith(optionEq)) {
            path = arg.mid(optionEq.size());
        } else {
            continue;
        }
        if (path.trimmed().isEmpty()) {
            std::fprintf(stderr, "usage: TaidaFlowApp.exe %s <path of config.json>\n", kWriteDefaultOption);
            std::fflush(stderr);
            return kWriteUsage;
        }
        QString message;
        const int code = writeDefaultFile(path, &message);
        const QByteArray line = (message + QLatin1Char('\n')).toLocal8Bit();
        std::FILE *stream = code == kWriteOk ? stdout : stderr;
        std::fputs(line.constData(), stream);
        std::fflush(stream);
        return code;
    }
    return std::nullopt;
}

AppConfig::LoadResult AppConfig::load(const std::optional<QString> &configPathEnv,
                                      const QString &executableDir,
                                      const std::optional<QString> &downloadPortEnv)
{
    LoadResult r;
    const auto info = [&r](const QString &text) { r.log.append({false, text}); };
    const auto warn = [&r](const QString &text) { r.log.append({true, text}); };

    // 1-2. which file
    QString path;
    const bool fromEnvironment = configPathEnv && !configPathEnv->trimmed().isEmpty();
    if (fromEnvironment) {
        path = QFileInfo(configPathEnv->trimmed()).absoluteFilePath();
        r.fileSource = FileSource::EnvironmentVariable;
    } else {
        path = QDir(executableDir).absoluteFilePath(QString::fromLatin1(kFileName));
        r.fileSource = FileSource::BesideExecutable;
    }
    path = QDir::cleanPath(path);
    r.path = path;
    const QString origin = fromEnvironment
        ? QStringLiteral("%1=%2").arg(QLatin1String(kConfigPathEnv), configPathEnv->trimmed())
        : QStringLiteral("beside the executable");

    AppConfig config;
    config.m_baseDir = QFileInfo(path).absolutePath();

    const QFileInfo fileInfo(path);
    if (!fileInfo.exists()) {
        // 3. create the default file (or fall back to in-memory defaults)
        QString message;
        const int code = writeDefaultFile(path, &message);
        if (code == kWriteOk) {
            r.fileSource = FileSource::Created;
            info(QStringLiteral("config file %1 (%2) did not exist - created it with the default values")
                     .arg(nativePath(path), origin));
        } else {
            r.fileSource = FileSource::InMemoryDefaults;
            warn(QStringLiteral("config file %1 (%2) does not exist and cannot be created (%3) - "
                                "running with the in-memory default values")
                     .arg(nativePath(path), origin, message));
        }
    } else {
        info(QStringLiteral("config file %1 (%2)").arg(nativePath(path), origin));
        const auto fail = [&r, &warn](const QString &error, int line, int column) {
            r.ok = false;
            r.error = error;
            r.errorLine = line;
            r.errorColumn = column;
            warn(QStringLiteral("config file %1 cannot be used: %2%3 - the file is left unchanged and "
                                "the program exits (fix config.json or delete it to recreate the defaults)")
                     .arg(nativePath(r.path), error,
                          line > 0 ? QStringLiteral(" (line %1, column %2)").arg(line).arg(column)
                                   : QString()));
            return r;
        };
        if (fileInfo.isDir())
            return fail(QStringLiteral("the path is a folder, not a file"), 0, 0);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("cannot read the file: %1").arg(file.errorString()), 0, 0);
        QByteArray bytes = file.readAll();
        file.close();
        if (bytes.startsWith("\xEF\xBB\xBF")) {
            bytes.remove(0, 3);
            info(QStringLiteral("config file starts with a UTF-8 BOM - ignored"));
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            int line = 0;
            int column = 0;
            // QJsonParseError::offset points just past the offending character; report the
            // character itself.
            lineAndColumn(bytes, qMax(0, int(parseError.offset) - 1), &line, &column);
            return fail(QStringLiteral("JSON syntax error: %1").arg(parseError.errorString()), line, column);
        }
        if (!document.isObject())
            return fail(QStringLiteral("the top level is not a JSON object"), 1, 1);
        const QJsonObject root = document.object();

        // 5 + 7. every setting of the table
        QSet<QString> reportedGroups;
        for (const Field &f : fields()) {
            const QStringList segments = f.key.split(QLatin1Char('.'));
            QJsonObject object = root;
            QString groupPath;
            bool groupMissing = false;
            bool groupInvalid = false;
            for (int i = 0; i + 1 < segments.size(); ++i) {
                groupPath = groupPath.isEmpty() ? segments.at(i) : groupPath + QLatin1Char('.') + segments.at(i);
                const QJsonValue group = object.value(segments.at(i));
                if (group.isUndefined()) {
                    groupMissing = true;
                    break;
                }
                if (!group.isObject()) {
                    groupInvalid = true;
                    if (!reportedGroups.contains(groupPath)) {
                        reportedGroups.insert(groupPath);
                        warn(QStringLiteral("\"%1\" = %2 is not an object - every setting in it uses its default")
                                 .arg(groupPath, jsonText(group)));
                    }
                    break;
                }
                object = group.toObject();
            }
            const QString defaultText = jsonText(f.defaultValue);
            if (groupInvalid)
                continue;   // already reported; the default stays
            const QJsonValue raw = groupMissing ? QJsonValue(QJsonValue::Undefined)
                                                : object.value(segments.last());
            if (raw.isUndefined()) {
                info(QStringLiteral("\"%1\" is missing - using the default %2").arg(f.key, defaultText));
                continue;
            }
            QString reason;
            const QJsonValue normalised = validate(f, raw, &reason);
            if (normalised.isUndefined()) {
                warn(QStringLiteral("\"%1\" = %2 is invalid (%3) - using the default %4")
                         .arg(f.key, jsonText(raw), reason, defaultText));
                continue;
            }
            config.m_values.insert(f.key, normalised);
            config.m_sources.insert(f.key, ValueSource::File);
        }

        // 6. unknown keys
        std::function<void(const QJsonObject &, const QString &)> walk =
            [&](const QJsonObject &object, const QString &prefix) {
            for (auto it = object.begin(); it != object.end(); ++it) {
                const QString key = prefix.isEmpty() ? it.key() : prefix + QLatin1Char('.') + it.key();
                if (findField(key))
                    continue;
                if (isGroup(key)) {
                    if (it.value().isObject())
                        walk(it.value().toObject(), key);
                    continue;   // a non-object group was reported above
                }
                info(QStringLiteral("unknown key \"%1\" ignored").arg(key));
            }
        };
        walk(root, QString());
    }

    // TAIDAFLOW_DOWNLOAD_PORT: temporary override of the derived download port.
    if (downloadPortEnv && !downloadPortEnv->trimmed().isEmpty()) {
        bool ok = false;
        const uint port = downloadPortEnv->trimmed().toUInt(&ok);
        if (ok && port >= 1 && port <= 65535) {
            config.m_downloadPortOverride = quint16(port);
            info(QStringLiteral("%1=%2 overrides the download port")
                     .arg(QLatin1String(kDownloadPortEnv)).arg(port));
        } else {
            warn(QStringLiteral("%1=\"%2\" is not a port (1..65535) - ignored")
                     .arg(QLatin1String(kDownloadPortEnv), downloadPortEnv->trimmed()));
        }
    }

    // 8. effective values
    info(QStringLiteral("configuration source: %1").arg(fileSourceName(r.fileSource)));
    for (const Field &f : fields()) {
        info(QStringLiteral("  %1 = %2 (%3)")
                 .arg(f.key, jsonText(config.m_values.value(f.key)),
                      sourceName(config.m_sources.value(f.key))));
    }
    info(QStringLiteral("  dataDir (resolved) = %1").arg(nativePath(config.resolvedDataDir())));
    info(QStringLiteral("  nginx.exe (resolved) = %1").arg(nativePath(config.resolvedNginxExe())));
    info(QStringLiteral("  log.dir (resolved) = %1").arg(nativePath(config.resolvedLogDir())));
    info(QStringLiteral("  downloadPort = %1 (%2)")
             .arg(config.downloadPort())
             .arg(config.downloadPortSource() == ValueSource::Environment
                      ? QStringLiteral("environment override %1").arg(QLatin1String(kDownloadPortEnv))
                      : QStringLiteral("derived: nginx.enabled ? nginx.port : http.port")));

    r.ok = true;
    r.config = config;
    return r;
}

AppConfig::LoadResult AppConfig::loadFromEnvironment()
{
    std::optional<QString> configPath;
    if (qEnvironmentVariableIsSet(kConfigPathEnv))
        configPath = qEnvironmentVariable(kConfigPathEnv);
    std::optional<QString> downloadPort;
    if (qEnvironmentVariableIsSet(kDownloadPortEnv))
        downloadPort = qEnvironmentVariable(kDownloadPortEnv);
    return load(configPath, QCoreApplication::applicationDirPath(), downloadPort);
}

QString AppConfigLoadResult::errorDialogText() const
{
    QString location = errorLine > 0
        ? QStringLiteral("第 %1 行,第 %2 欄").arg(errorLine).arg(errorColumn)
        : QStringLiteral("不明");
    return QStringLiteral("設定檔 config.json 無法讀取,程式即將結束。\n\n"
                          "檔案:%1\n"
                          "位置:%2\n"
                          "錯誤:%3\n\n"
                          "請修正 config.json 或刪除它讓程式重建預設值。")
        .arg(nativePath(path), location, error);
}

void AppConfigLoadResult::printLog() const
{
    for (const AppConfigLogEntry &entry : log) {
        if (entry.warning)
            qWarning().noquote() << "[Config]" << entry.text;
        else
            qInfo().noquote() << "[Config]" << entry.text;
    }
}

bool AppConfigLoadResult::hasWarning() const
{
    for (const AppConfigLogEntry &entry : log) {
        if (entry.warning)
            return true;
    }
    return false;
}

const AppConfig &AppConfig::instance()
{
    return globalInstance();
}

void AppConfig::setInstance(const AppConfig &config)
{
    globalInstance() = config;
}

// ---- getters ----------------------------------------------------------------------------

QString AppConfig::string(const QString &key) const
{
    return m_values.value(key).toString();
}

int AppConfig::integer(const QString &key) const
{
    return m_values.value(key).toInt();
}

int AppConfig::version() const
{
    return integer(QStringLiteral("version"));
}

QString AppConfig::dataDir() const
{
    return string(QStringLiteral("dataDir"));
}

QString AppConfig::resolvedDataDir() const
{
    return QDir::cleanPath(QDir(m_baseDir).absoluteFilePath(dataDir()));
}

bool AppConfig::applyDataDir() const
{
    const QString dir = resolvedDataDir();
    const QString previous = QDir::currentPath();
    if (!QDir().mkpath(dir)) {
        qWarning().noquote() << "[Config] dataDir" << nativePath(dir)
                             << "cannot be created - the working directory stays" << nativePath(previous);
        return false;
    }
    if (!QDir::setCurrent(dir)) {
        qWarning().noquote() << "[Config] cannot change the working directory to dataDir" << nativePath(dir)
                             << "- the working directory stays" << nativePath(previous);
        return false;
    }
    qInfo().noquote() << "[Config] working directory (dataDir):" << nativePath(QDir::currentPath());
    return true;
}

QString AppConfig::deviceKey(Device device)
{
    switch (device) {
    case Device::Adam6256: return QStringLiteral("adam6256");
    case Device::Adam6217A: return QStringLiteral("adam6217a");
    case Device::Adam6217B: return QStringLiteral("adam6217b");
    case Device::Adam6224: return QStringLiteral("adam6224");
    case Device::Adam6022: return QStringLiteral("adam6022");
    }
    return QString();
}

AppConfig::TcpDevice AppConfig::device(Device device) const
{
    const QString prefix = QStringLiteral("devices.%1.").arg(deviceKey(device));
    TcpDevice d;
    d.host = string(prefix + QLatin1String("host"));
    d.port = quint16(integer(prefix + QLatin1String("port")));
    d.unitId = integer(prefix + QLatin1String("unitId"));
    return d;
}

AppConfig::SerialDevice AppConfig::ms300() const
{
    SerialDevice d;
    d.serialPort = string(QStringLiteral("devices.ms300.serialPort"));
    d.baudRate = integer(QStringLiteral("devices.ms300.baudRate"));
    d.dataBits = integer(QStringLiteral("devices.ms300.dataBits"));
    d.parity = string(QStringLiteral("devices.ms300.parity"));
    d.stopBits = integer(QStringLiteral("devices.ms300.stopBits"));
    d.unitId = integer(QStringLiteral("devices.ms300.unitId"));
    return d;
}

AppConfig::ModbusServerSettings AppConfig::modbusServer() const
{
    ModbusServerSettings s;
    s.bind = string(QStringLiteral("modbusServer.bind"));
    s.port = quint16(integer(QStringLiteral("modbusServer.port")));
    s.unitId = integer(QStringLiteral("modbusServer.unitId"));
    return s;
}

AppConfig::Listener AppConfig::http() const
{
    return {string(QStringLiteral("http.bind")), quint16(integer(QStringLiteral("http.port")))};
}

AppConfig::Listener AppConfig::rest() const
{
    return {string(QStringLiteral("rest.bind")), quint16(integer(QStringLiteral("rest.port")))};
}

AppConfig::MirrorSettings AppConfig::mirror() const
{
    MirrorSettings m;
    m.internalPort = quint16(integer(QStringLiteral("mirror.internalPort")));
    m.publicBind = string(QStringLiteral("mirror.publicBind"));
    m.publicPort = quint16(integer(QStringLiteral("mirror.publicPort")));
    return m;
}

AppConfig::NginxSettings AppConfig::nginx() const
{
    NginxSettings n;
    n.enabled = m_values.value(QStringLiteral("nginx.enabled")).toBool();
    n.port = quint16(integer(QStringLiteral("nginx.port")));
    n.exe = string(QStringLiteral("nginx.exe"));
    n.resolvedExe = resolvedNginxExe();
    return n;
}

QString AppConfig::resolvedNginxExe() const
{
    return QDir::cleanPath(QDir(m_baseDir).absoluteFilePath(string(QStringLiteral("nginx.exe"))));
}

QString AppConfig::logDir() const
{
    return string(QStringLiteral("log.dir"));
}

QString AppConfig::resolvedLogDir() const
{
    return QDir::cleanPath(QDir(resolvedDataDir()).absoluteFilePath(logDir()));
}

AppConfig::LogSettings AppConfig::logSettings() const
{
    LogSettings s;
    s.dir = resolvedLogDir();
    s.quiet.enabled = m_values.value(QStringLiteral("log.quiet.enabled")).toBool();
    s.quiet.keepDays = integer(QStringLiteral("log.quiet.keepDays"));
    s.full.enabled = m_values.value(QStringLiteral("log.full.enabled")).toBool();
    s.full.keepDays = integer(QStringLiteral("log.full.keepDays"));
    return s;
}

AppConfig::LogSettings AppConfig::fallbackLogSettings(const QString &configPath)
{
    LogSettings s = defaults().logSettings();
    s.dir = QDir::cleanPath(QFileInfo(configPath).absolutePath() + QStringLiteral("/logs"));
    return s;
}

quint16 AppConfig::downloadPort() const
{
    if (m_downloadPortOverride)
        return *m_downloadPortOverride;
    const NginxSettings n = nginx();
    return n.enabled ? n.port : http().port;
}

AppConfig::ValueSource AppConfig::downloadPortSource() const
{
    if (m_downloadPortOverride)
        return ValueSource::Environment;
    const NginxSettings n = nginx();
    const QString key = n.enabled ? QStringLiteral("nginx.port") : QStringLiteral("http.port");
    const ValueSource enabledSource = source(QStringLiteral("nginx.enabled"));
    // File as soon as one of the inputs came from the file.
    return (source(key) == ValueSource::File || enabledSource == ValueSource::File)
        ? ValueSource::File : ValueSource::Default;
}

QJsonValue AppConfig::value(const QString &key) const
{
    return m_values.value(key, QJsonValue(QJsonValue::Undefined));
}

AppConfig::ValueSource AppConfig::source(const QString &key) const
{
    return m_sources.value(key, ValueSource::Default);
}

QString AppConfig::baseDir() const
{
    return m_baseDir;
}

QString AppConfig::sourceName(ValueSource source)
{
    switch (source) {
    case ValueSource::File: return QStringLiteral("file");
    case ValueSource::Default: return QStringLiteral("default");
    case ValueSource::Environment: return QStringLiteral("environment override");
    }
    return QString();
}

QString AppConfig::fileSourceName(FileSource source)
{
    switch (source) {
    case FileSource::EnvironmentVariable: return QStringLiteral("TAIDAFLOW_CONFIG");
    case FileSource::BesideExecutable: return QStringLiteral("config.json beside the executable");
    case FileSource::Created: return QStringLiteral("newly created default file");
    case FileSource::InMemoryDefaults: return QStringLiteral("in-memory defaults (no file)");
    }
    return QString();
}
