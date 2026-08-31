#pragma once
#include <QObject>
#include <QtHttpServer/QHttpServer>
#include <QtHttpServer/QHttpServerResponse>
#include <QTcpServer>
#include <QMutex>
#include <QMutexLocker>
#include <memory>
#include "SqlManager.h"

class RESTManager : public QObject
{
	Q_OBJECT
public:
    explicit RESTManager(SqlManager* sql, QObject* parent = nullptr);

    bool start(quint16 port);

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
