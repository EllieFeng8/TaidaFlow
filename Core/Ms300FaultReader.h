#pragma once

#include <QObject>
#include <QTimer>

class QModbusRtuSerialClient;

// Read-only Modbus RTU monitor for the Delta MS300 inverter.
class Ms300FaultReader final : public QObject
{
    Q_OBJECT

public:
    explicit Ms300FaultReader(QObject *parent = nullptr);
    ~Ms300FaultReader() override;

    void start();
    void stop();

signals:
    void connectionChanged(bool connected, const QString &detail);
    void faultStatusRead(quint8 faultCode, quint8 warningCode, const QString &message);
    void faultStatusChanged(quint8 faultCode, quint8 warningCode, const QString &message);
    void readError(const QString &message);

private:
    void pollFaultStatus();
    void publishFaultStatus(quint16 rawStatus);

    QModbusRtuSerialClient *m_client = nullptr;
    QTimer m_pollTimer;
    bool m_running = false;
    bool m_requestPending = false;
    bool m_hasLastStatus = false;
    quint16 m_lastStatus = 0;
};
