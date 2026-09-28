#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

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

signals:
    void connectionChanged(bool connected, const QString &detail);
    void faultStatusRead(quint8 faultCode, quint8 warningCode, const QString &message);
    void faultStatusChanged(quint8 faultCode, quint8 warningCode, const QString &message);
    void readError(const QString &message);

private:
    void pollFaultStatus();
    void publishFaultStatus(quint16 rawStatus);

    Settings m_settings;
    QModbusRtuSerialClient *m_client = nullptr;
    QTimer m_pollTimer;
    bool m_running = false;
    bool m_requestPending = false;
    bool m_hasLastStatus = false;
    quint16 m_lastStatus = 0;
};
