#pragma once
#include <QObject>
#include <QtHttpServer/QHttpServer>
#include <QtHttpServer/QHttpServerResponse>
#include <QTcpServer>
#include <QMutex>
#include <QMutexLocker>
#include <QUrlQuery>
#include <memory>
#include "SqlManager.h"

class RESTManager : public QObject
{
	Q_OBJECT
public:
    explicit RESTManager(SqlManager* sql, QObject* parent = nullptr);

    // w2-060: 'address' added (default QHostAddress::Any = the previous behaviour).
    bool start(quint16 port, const QHostAddress& address = QHostAddress(QHostAddress::Any));

    // w2-071: the port actually listened on (start(0, ...) picks a free one; tests); 0 before start.
    quint16 serverPort() const { return m_tcpServer ? m_tcpServer->serverPort() : 0; }

    // w2-071 (review D-001): limits of the range routes (/api/sensor|holding/range,
    // .../rangeDateTime, .../rangeDateTimePage - all paged).
    static constexpr qint64 kMaxRangeEpochSecs = 253402300799LL;   // 9999-12-31T23:59:59Z
    static constexpr qint64 kDefaultPageSize = 200;
    static constexpr qint64 kMaxPageSize = 1000;                    // larger pageSize is clamped

    void setDeviceState(const QString& state)
    {
        QMutexLocker locker(&device_state_mutex);
        device_state = state;
    }

signals:
    void modbusModeChanged(const QString& mode);

private:
    QHttpServerResponse jsonResp(QJsonValue v, QHttpServerResponse::StatusCode code = QHttpServerResponse::StatusCode::Ok);
    QHttpServerResponse errResp(int httpCode, const QString& message);
    void applyCors(QHttpServerResponse& resp) const;
    static bool parseIntExpr(const QString& text, qint64& outVal);
    static bool parseDateTimeIso(const QString& text, qint64& outSecs);
    // w2-071: shared by the six range routes.
    static QString checkRangeBounds(qint64 from, qint64 to);   // empty = OK, else the 400 message
    static bool parsePaging(const QUrlQuery& q, qint64* page, qint64* pageSize, QString* error);
    QHttpServerResponse pagedRangeResponse(bool holding, qint64 from, qint64 to, const QUrlQuery& q);
    static QString loadDeviceSn();

    void setupRoutes();



private:
    QHttpServer m_httpServer;
    std::unique_ptr<QTcpServer> m_tcpServer;
    SqlManager* m_sql;
    QString device_state = "";
    QMutex device_state_mutex;
    QString m_modbusMode = QStringLiteral("network");
    QMutex m_modbusMutex;
};
