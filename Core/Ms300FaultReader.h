#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include "Modbus_Client.h"   // w2-072: RepeatedWarningLimiter

class QModbusRtuSerialClient;

// Read-only Modbus RTU monitor for the Delta MS300 inverter.
class Ms300FaultReader final : public QObject
{
    Q_OBJECT

public:
    // Serial parameters of the inverter (w2-062: from config.json devices.ms300, see
    // Core::init). The defaults are the former fixed values COM2, 9600, N-8-1, unit 1.
    // Timeout (1000 ms), retries (1) and the poll interval (1000 ms) stay fixed in the code.
    struct Settings {
        QString serialPort = QStringLiteral("COM2");
        int baudRate = 9600;
        int dataBits = 8;          // 5..8
        QString parity = QStringLiteral("none");   // none | even | odd | space | mark
        int stopBits = 1;          // 1 | 2
        int unitId = 1;            // Modbus RTU slave address
    };

    explicit Ms300FaultReader(QObject *parent = nullptr);
    explicit Ms300FaultReader(const Settings &settings, QObject *parent = nullptr);
    const Settings &settings() const { return m_settings; }
    ~Ms300FaultReader() override;

    void start();
    void stop();

    static constexpr int kConnectRetryMs = 3000;   // w2-072
    // Tests only: a shorter warning interval than RepeatedWarningLimiter::kDefaultIntervalMs.
    void setWarningIntervalMs(qint64 intervalMs)
    {
        m_connectionWarnings.setIntervalMs(intervalMs);
        m_readWarnings.setIntervalMs(intervalMs);
    }

signals:
    void connectionChanged(bool connected, const QString &detail);
    void faultStatusRead(quint8 faultCode, quint8 warningCode, const QString &message);
    void faultStatusChanged(quint8 faultCode, quint8 warningCode, const QString &message);
    void readError(const QString &message);
    // w2-087 (device status): the port opened (Connected) / is not open (Unconnected: open failed or
    // closed) - not emitted for the Connecting / Closing steps; one fault-status poll of an open port
    // failed (no / bad answer).
    void portOpenChanged(bool open);
    void faultStatusReadFailed();

private:
    void pollFaultStatus();
    void publishFaultStatus(quint16 rawStatus);
    void warnConnection(const QString &message);   // w2-072
    void warnRead(const QString &message);         // w2-072

    Settings m_settings;
    QModbusRtuSerialClient *m_client = nullptr;
    QTimer m_pollTimer;
    bool m_running = false;
    bool m_requestPending = false;
    bool m_hasLastStatus = false;
    quint16 m_lastStatus = 0;
    // w2-072 (review D-003): while the serial port is not open a connection is tried at most every
    // kConnectRetryMs; connection warnings and fault-status read failures are rate limited
    // (first at once, then at most one per 60 s with the number held back); the end of an outage
    // / of a series of read failures is logged once (info).
    QElapsedTimer m_lastConnectAttempt;
    QElapsedTimer m_outageClock;
    int m_connectAttempts = 0;
    RepeatedWarningLimiter m_connectionWarnings;
    RepeatedWarningLimiter m_readWarnings;
    qint64 m_readFailures = 0;
};
