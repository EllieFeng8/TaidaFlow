#include "RESTManager.h"

#include <QtHttpServer/QHttpServerRequest>
#include <QtHttpServer/QHttpServerResponse>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrlQuery>
#include <QtDebug>
#include <QDateTime>
#include <QDate>
#include <QTimeZone>
#include <QUrl>
#include <QMap>
#include <QRegularExpression>
#include <QSettings>
#include <QDir>

RESTManager::RESTManager(SqlManager* sql, QObject* parent)
    : QObject(parent)
    , m_tcpServer(std::make_unique<QTcpServer>(this))
    , m_sql(sql)
{
}

QHttpServerResponse RESTManager::jsonResp(QJsonValue v, QHttpServerResponse::StatusCode code)
{
    if (v.isArray())
    {
        QHttpServerResponse resp(v.toArray(), code);
        applyCors(resp);
        return resp;
    }
    if (v.isObject())
    {
        QHttpServerResponse resp(v.toObject(), code);
        applyCors(resp);
        return resp;
    }
    QJsonObject wrapper;
    wrapper.insert("value", v);
    QHttpServerResponse resp(wrapper, code);
    applyCors(resp);
    return resp;
}

QHttpServerResponse RESTManager::errResp(int httpCode, const QString& message)
{
    QJsonObject obj;
    obj.insert("ok", false);
    obj.insert("error", message);
    auto code = static_cast<QHttpServerResponse::StatusCode>(httpCode);
    return jsonResp(obj, code);
}

void RESTManager::applyCors(QHttpServerResponse& resp) const
{
    QHttpHeaders headers = resp.headers();
    headers.append("Access-Control-Allow-Origin", "*");
    headers.append("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS");
    headers.append("Access-Control-Allow-Headers", "Content-Type");
    headers.append("Access-Control-Allow-Credentials", "false");
    resp.setHeaders(std::move(headers));
}

void RESTManager::setupRoutes()
{
    auto optionsHandler = [this](const QHttpServerRequest&) {
        QHttpServerResponse resp(QHttpServerResponse::StatusCode::Ok);
        applyCors(resp);
        return resp;
    };

    // Health / status (GET)
    m_httpServer.route("/", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest&) {
        QMutexLocker locker(&device_state_mutex);
        QString state = device_state.isEmpty() ? QStringLiteral("ok") : device_state;
        return jsonResp(QJsonObject{{"status", state}});
    });

    // GET settings
    m_httpServer.route("/api/settings/sensors", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest&) {
        QJsonArray arr = m_sql->getSensorMapJson();
        return jsonResp(arr);
    });

    // PUT settings
    m_httpServer.route("/api/settings/sensors", QHttpServerRequest::Method::Put, [this](const QHttpServerRequest& req) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(req.body(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isArray())
        {
            return errResp(400, "invalid json array");
        }

        // merge with existing keys: keep old ones not provided
        QJsonArray current = m_sql->getSensorMapJson();
        QMap<QString, QString> map;
        for (const QJsonValue& v : current)
        {
            if (!v.isObject()) continue;
            QJsonObject o = v.toObject();
            map[o.value("key").toString()] = o.value("name").toString();
        }
        for (const QJsonValue& v : doc.array())
        {
            if (!v.isObject()) continue;
            QJsonObject o = v.toObject();
            QString k = o.value("key").toString();
            QString n = o.value("name").toString();
            if (!k.isEmpty())
            {
                map[k] = n;
            }
        }

        QJsonArray merged;
        for (auto it = map.cbegin(); it != map.cend(); ++it)
        {
            QJsonObject o;
            o.insert("key", it.key());
            o.insert("name", it.value());
            merged.append(o);
        }

        QString errMsg;
        if (!m_sql->updateSensorMapJson(merged, &errMsg))
        {
            return errResp(500, errMsg);
        }
        QJsonObject ok; ok.insert("ok", true);
        return jsonResp(ok);
    });

    m_httpServer.route("/api/settings/sensors", QHttpServerRequest::Method::Options, optionsHandler);

    // GET read frequency
    m_httpServer.route("/api/settings/frequency", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest&) {
        QString errMsg;
        int freq = m_sql->readFrequency(&errMsg);
        if (freq < 0)
        {
            return errResp(500, errMsg.isEmpty() ? QStringLiteral("read frequency error") : errMsg);
        }
        QJsonObject obj; obj.insert("read_frequency", freq);
        return jsonResp(obj);
    });

    m_httpServer.route("/api/settings/frequency", QHttpServerRequest::Method::Options, optionsHandler);

    // PUT modbus mode
    m_httpServer.route("/api/modbus/mode", QHttpServerRequest::Method::Put, [this](const QHttpServerRequest& req) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(req.body(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
        {
            return errResp(400, "invalid json object");
        }

        QJsonObject obj = doc.object();
        QString mode = obj.value("mode").toString().trimmed().toLower();
        if (mode.isEmpty())
        {
            return errResp(400, "mode missing");
        }

        const QStringList allowed = {"network", "standalone"};
        if (!allowed.contains(mode))
        {
            return errResp(400, "mode must be 'network' or 'standalone'");
        }

        {
            QMutexLocker locker(&m_modbusMutex);
            m_modbusMode = mode;
        }

        qInfo() << "Modbus mode changed to" << mode;
        emit modbusModeChanged(mode);

        QJsonObject ok; ok.insert("ok", true); ok.insert("mode", mode);
        return jsonResp(ok);
    });

    m_httpServer.route("/api/modbus/mode", QHttpServerRequest::Method::Options, optionsHandler);

    // GET modbus mode
    m_httpServer.route("/api/modbus/mode", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest&) {
        QString mode;
        {
            QMutexLocker locker(&m_modbusMutex);
            mode = m_modbusMode;
        }
        if (mode.isEmpty())
        {
            mode = QStringLiteral("unknown");
        }
        QJsonObject obj; obj.insert("mode", mode);
        return jsonResp(obj);
    });

    // PUT read frequency
    m_httpServer.route("/api/settings/frequency", QHttpServerRequest::Method::Put, [this](const QHttpServerRequest& req) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(req.body(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
        {
            return errResp(400, "invalid json object");
        }
        QJsonObject obj = doc.object();
        if (!obj.contains("read_frequency") || !obj.value("read_frequency").isDouble())
        {
            return errResp(400, "read_frequency missing or invalid");
        }
        int val = static_cast<int>(obj.value("read_frequency").toDouble());
        QString errMsg;
        if (!m_sql->setReadFrequency(val, &errMsg))
        {
            return errResp(500, errMsg);
        }
        QJsonObject ok; ok.insert("ok", true); ok.insert("read_frequency", val);
        return jsonResp(ok);
    });

    m_httpServer.route("/api/settings/frequency", QHttpServerRequest::Method::Options, optionsHandler);

    // GET range
    m_httpServer.route("/api/sensor/range", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest& req) {
        QUrlQuery q(req.query());
        qint64 from = 0, to = 0;
        if (!parseIntExpr(q.queryItemValue("from"), from) || !parseIntExpr(q.queryItemValue("to"), to) || to < from)
        {
            return errResp(400, "invalid from/to");
        }
        QJsonArray out;
        QString errMsg;
        if (!m_sql->queryRangeJson(from, to, &out, &errMsg))
        {
            return errResp(500, errMsg);
        }
        return jsonResp(out);
    });

    m_httpServer.route("/api/sensor/range", QHttpServerRequest::Method::Options, optionsHandler);

    // GET holding range
    m_httpServer.route("/api/holding/range", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest& req) {
        QUrlQuery q(req.query());
        qint64 from = 0, to = 0;
        if (!parseIntExpr(q.queryItemValue("from"), from) || !parseIntExpr(q.queryItemValue("to"), to) || to < from)
        {
            return errResp(400, "invalid from/to");
        }
        QJsonArray out;
        QString errMsg;
        if (!m_sql->queryHoldingRangeJson(from, to, &out, &errMsg))
        {
            return errResp(500, errMsg);
        }
        return jsonResp(out);
    });

    m_httpServer.route("/api/holding/range", QHttpServerRequest::Method::Options, optionsHandler);

    // GET device sn
    m_httpServer.route("/api/device/sn", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest&) {
        QString sn = loadDeviceSn();
        return jsonResp(QJsonObject{{"sn", sn}});
    });

    m_httpServer.route("/api/device/sn", QHttpServerRequest::Method::Options, optionsHandler);

    // GET last sample
    m_httpServer.route("/api/sensor/last", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest&) {
        auto rows = m_sql->fetchSensorData(QDate::currentDate(), 1);
        if (rows.isEmpty())
        {
            return errResp(404, "no data");
        }
        const QVariantList& r = rows.first();
        QJsonObject obj;
        obj.insert("ts", r.value(0).toLongLong());
        for (int i = 1; i < r.size(); ++i)
        {
            obj.insert(QString("s%1").arg(i), QJsonValue::fromVariant(r.at(i)));
        }
        return jsonResp(obj);
    });

    m_httpServer.route("/api/sensor/last", QHttpServerRequest::Method::Options, optionsHandler);

    // GET last holding registers
    m_httpServer.route("/api/holding/last", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest&) {
        auto rows = m_sql->fetchHoldingRegisters(QDate::currentDate(), 1);
        if (rows.isEmpty())
        {
            return errResp(404, "no data");
        }
        const QVariantList& r = rows.first();
        QJsonObject obj;
        obj.insert("ts", r.value(0).toLongLong());
        for (int i = 1; i < r.size(); ++i)
        {
            obj.insert(QString("h%1").arg(i), QJsonValue::fromVariant(r.at(i)));
        }
        return jsonResp(obj);
    });

    m_httpServer.route("/api/holding/last", QHttpServerRequest::Method::Options, optionsHandler);

    // GET range by datetime (ISO string)
    m_httpServer.route("/api/sensor/rangeDateTime", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest& req) {
        QUrlQuery q(req.query());
        qint64 from = 0, to = 0;
        if (!parseDateTimeIso(q.queryItemValue("from"), from) || !parseDateTimeIso(q.queryItemValue("to"), to))
        {
            return errResp(400, "invalid datetime");
        }
        if (to < from)
        {
            return errResp(400, "to < from");
        }
        QJsonArray out;
        QString errMsg;
        if (!m_sql->queryRangeJson(from, to, &out, &errMsg))
        {
            return errResp(500, errMsg);
        }
        return jsonResp(out);
    });

    m_httpServer.route("/api/sensor/rangeDateTime", QHttpServerRequest::Method::Options, optionsHandler);

    // GET sensor range by datetime with pagination
    m_httpServer.route("/api/sensor/rangeDateTimePage", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest& req) {
        QUrlQuery q(req.query());
        qint64 from = 0, to = 0;
        if (!parseDateTimeIso(q.queryItemValue("from"), from) || !parseDateTimeIso(q.queryItemValue("to"), to))
        {
            return errResp(400, "invalid datetime");
        }
        if (to < from)
        {
            return errResp(400, "to < from");
        }

        qint64 pageVal = 1;
        qint64 pageSizeVal = 200;
        QString pageText = q.queryItemValue("page").trimmed();
        QString pageSizeText = q.queryItemValue("pageSize").trimmed();
        if (!pageText.isEmpty() && !parseIntExpr(pageText, pageVal))
        {
            return errResp(400, "invalid page");
        }
        if (!pageSizeText.isEmpty() && !parseIntExpr(pageSizeText, pageSizeVal))
        {
            return errResp(400, "invalid pageSize");
        }
        if (pageVal <= 0 || pageSizeVal <= 0)
        {
            return errResp(400, "page and pageSize must be positive");
        }
        if (pageSizeVal > 1000)
        {
            pageSizeVal = 1000;
        }

        QJsonArray out;
        QString errMsg;
        if (!m_sql->queryRangeJsonPaged(from, to, static_cast<int>(pageVal), static_cast<int>(pageSizeVal), &out, &errMsg))
        {
            return errResp(500, errMsg);
        }

        qint64 totalCount = 0;
        if (!m_sql->countSensorRange(from, to, &totalCount, &errMsg))
        {
            return errResp(500, errMsg);
        }

        qint64 totalPages = (totalCount > 0) ? ((totalCount + pageSizeVal - 1) / pageSizeVal) : 0;
        bool hasPrevious = pageVal > 1;
        bool hasNext = pageVal < totalPages;

        QJsonObject obj;
        obj.insert("page", pageVal);
        obj.insert("pageSize", pageSizeVal);
        obj.insert("totalCount", totalCount);
        obj.insert("totalPages", totalPages);
        obj.insert("hasPreviousPage", hasPrevious);
        obj.insert("hasNextPage", hasNext);
        obj.insert("items", out);
        return jsonResp(obj);
    });

    m_httpServer.route("/api/sensor/rangeDateTimePage", QHttpServerRequest::Method::Options, optionsHandler);

    // GET holding range by datetime (ISO string)
    m_httpServer.route("/api/holding/rangeDateTime", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest& req) {
        QUrlQuery q(req.query());
        qint64 from = 0, to = 0;
        if (!parseDateTimeIso(q.queryItemValue("from"), from) || !parseDateTimeIso(q.queryItemValue("to"), to))
        {
            return errResp(400, "invalid datetime");
        }
        if (to < from)
        {
            return errResp(400, "to < from");
        }
        QJsonArray out;
        QString errMsg;
        if (!m_sql->queryHoldingRangeJson(from, to, &out, &errMsg))
        {
            return errResp(500, errMsg);
        }
        return jsonResp(out);
    });

    m_httpServer.route("/api/holding/rangeDateTime", QHttpServerRequest::Method::Options, optionsHandler);

    // GET holding range by datetime with pagination
    m_httpServer.route("/api/holding/rangeDateTimePage", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest& req) {
        QUrlQuery q(req.query());
        qint64 from = 0, to = 0;
        if (!parseDateTimeIso(q.queryItemValue("from"), from) || !parseDateTimeIso(q.queryItemValue("to"), to))
        {
            return errResp(400, "invalid datetime");
        }
        if (to < from)
        {
            return errResp(400, "to < from");
        }

        qint64 pageVal = 1;
        qint64 pageSizeVal = 200;
        QString pageText = q.queryItemValue("page").trimmed();
        QString pageSizeText = q.queryItemValue("pageSize").trimmed();
        if (!pageText.isEmpty() && !parseIntExpr(pageText, pageVal))
        {
            return errResp(400, "invalid page");
        }
        if (!pageSizeText.isEmpty() && !parseIntExpr(pageSizeText, pageSizeVal))
        {
            return errResp(400, "invalid pageSize");
        }
        if (pageVal <= 0 || pageSizeVal <= 0)
        {
            return errResp(400, "page and pageSize must be positive");
        }
        if (pageSizeVal > 1000)
        {
            pageSizeVal = 1000;
        }

        QJsonArray out;
        QString errMsg;
        if (!m_sql->queryHoldingRangeJsonPaged(from, to, static_cast<int>(pageVal), static_cast<int>(pageSizeVal), &out, &errMsg))
        {
            return errResp(500, errMsg);
        }

        qint64 totalCount = 0;
        if (!m_sql->countHoldingRange(from, to, &totalCount, &errMsg))
        {
            return errResp(500, errMsg);
        }

        qint64 totalPages = (totalCount > 0) ? ((totalCount + pageSizeVal - 1) / pageSizeVal) : 0;
        bool hasPrevious = pageVal > 1;
        bool hasNext = pageVal < totalPages;

        QJsonObject obj;
        obj.insert("page", pageVal);
        obj.insert("pageSize", pageSizeVal);
        obj.insert("totalCount", totalCount);
        obj.insert("totalPages", totalPages);
        obj.insert("hasPreviousPage", hasPrevious);
        obj.insert("hasNextPage", hasNext);
        obj.insert("items", out);
        return jsonResp(obj);
    });

    m_httpServer.route("/api/holding/rangeDateTimePage", QHttpServerRequest::Method::Options, optionsHandler);
}

bool RESTManager::parseIntExpr(const QString& text, qint64& outVal)
{
    QRegularExpression re(R"(^\s*(\d+)\s*([+-])?\s*(\d+)?\s*$)");
    QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return false;
    bool ok = false;
    qint64 base = m.captured(1).toLongLong(&ok);
    if (!ok) return false;
    if (m.captured(2).isEmpty())
    {
        outVal = base;
        return true;
    }
    qint64 offset = m.captured(3).toLongLong(&ok);
    if (!ok) return false;
    if (m.captured(2) == "-")
        offset = -offset;
    outVal = base + offset;
    return true;
}

bool RESTManager::parseDateTimeIso(const QString& text, qint64& outSecs)
{
    QString s = QUrl::fromPercentEncoding(text.toUtf8()).trimmed();

    auto tryParse = [](const QString& v) -> QDateTime {
        QDateTime dt = QDateTime::fromString(v, Qt::ISODateWithMs);
        if (!dt.isValid())
            dt = QDateTime::fromString(v, Qt::ISODate);
        if (!dt.isValid())
            dt = QDateTime::fromString(v, "yyyy-MM-dd'T'hh:mm:ss'Z'");
        if (dt.isValid() && v.endsWith('Z', Qt::CaseInsensitive))
            dt.setTimeZone(QTimeZone::utc());
        return dt;
    };

    QDateTime dt = tryParse(s);
    if (dt.isValid())
    {
        outSecs = dt.toSecsSinceEpoch();
        return true;
    }

    bool ok = false;
    qint64 seconds = text.toLongLong(&ok);
    if (ok)
    {
        outSecs = seconds;
        return true;
    }
    return false;
}

QString RESTManager::loadDeviceSn()
{
    QString path = QDir::current().filePath("device_info.ini");
    QSettings settings(path, QSettings::IniFormat);
    QString sn = settings.value("device/sn").toString();
    if (sn.isEmpty())
    {
        sn = QStringLiteral("sn000000");
        settings.setValue("device/sn", sn);
        settings.sync();
    }
    return sn;
}

bool RESTManager::start(quint16 port)
{
    if (!m_sql)
    {
        qWarning() << "RESTManager start failed: SqlManager not set";
        return false;
    }

    QString sn = loadDeviceSn();
    qInfo() << "Device SN:" << sn;

    setupRoutes();
    if (!m_tcpServer)
    {
        m_tcpServer = std::make_unique<QTcpServer>(this);
    }
    if (!m_tcpServer->listen(QHostAddress::Any, port))
    {
        qWarning() << "HTTP server listen failed on port" << port;
        return false;
    }
    if (!m_httpServer.bind(m_tcpServer.get()))
    {
        qWarning() << "HTTP server bind failed";
        return false;
    }
    qInfo() << "Running on http://127.0.0.1:" << m_tcpServer->serverPort() << "/";
    return true;
}
