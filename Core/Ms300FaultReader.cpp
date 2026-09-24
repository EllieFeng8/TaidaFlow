#include "Ms300FaultReader.h"

#include <QDebug>
#include <QModbusDataUnit>
#include <QModbusDevice>
#include <QModbusReply>
#include <QModbusRtuSerialClient>
#include <QSerialPort>
#include <QStringList>
#include <QVariant>

namespace {
constexpr auto kMs300PortName = "COM2";
constexpr int kMs300UnitId = 1;
constexpr quint16 kMs300FaultStatusRegister = 0x2100;
constexpr int kMs300PollIntervalMs = 1000;
constexpr int kMs300TimeoutMs = 1000;

QString faultCodeText(quint8 code)
{
    switch (code) {
    case 1: return QStringLiteral("ocA - 加速中過電流");
    case 2: return QStringLiteral("ocd - 減速中過電流");
    case 3: return QStringLiteral("ocn - 恆速中過電流");
    case 4: return QStringLiteral("GFF - 接地故障");
    case 6: return QStringLiteral("ocS - 停止時過電流");
    case 7: return QStringLiteral("ovA - 加速中過電壓");
    case 8: return QStringLiteral("ovd - 減速中過電壓");
    case 9: return QStringLiteral("ovn - 恆速中過電壓");
    case 10: return QStringLiteral("ovS - 停止時過電壓");
    case 11: return QStringLiteral("LvA - 加速中低電壓");
    case 12: return QStringLiteral("Lvd - 減速中低電壓");
    case 13: return QStringLiteral("Lvn - 恆速中低電壓");
    case 14: return QStringLiteral("LvS - 停止時低電壓");
    case 15: return QStringLiteral("OrP - 輸入缺相保護");
    case 16: return QStringLiteral("oH1 - IGBT 過熱");
    case 18: return QStringLiteral("tH1o - IGBT 溫度偵測失敗");
    case 21: return QStringLiteral("oL - 變頻器過載");
    case 22: return QStringLiteral("EoL1 - 電子熱繼電器 1 保護");
    case 23: return QStringLiteral("EoL2 - 電子熱繼電器 2 保護");
    case 24: return QStringLiteral("oH3 - 馬達過熱（PTC/PT100）");
    case 26: return QStringLiteral("ot1 - 過轉矩 1");
    case 27: return QStringLiteral("ot2 - 過轉矩 2");
    case 28: return QStringLiteral("uC - 欠電流");
    case 31: return QStringLiteral("cF2 - EEPROM 讀取錯誤");
    case 33: return QStringLiteral("cd1 - U 相異常");
    case 34: return QStringLiteral("cd2 - V 相異常");
    case 35: return QStringLiteral("cd3 - W 相異常");
    case 36: return QStringLiteral("Hd0 - 電流硬體異常");
    case 37: return QStringLiteral("Hd1 - 過電流硬體異常");
    case 40: return QStringLiteral("AUE - 自動調諧異常");
    case 41: return QStringLiteral("AFE - PID 回授遺失");
    case 43: return QStringLiteral("PGF2 - PG 回授遺失");
    case 44: return QStringLiteral("PGF3 - PG 回授停滯");
    case 45: return QStringLiteral("PGF4 - PG 滑差異常");
    case 48: return QStringLiteral("ACE - 類比電流輸入遺失");
    case 49: return QStringLiteral("EF - 外部異常");
    case 50: return QStringLiteral("EF1 - 緊急停止");
    case 51: return QStringLiteral("bb - 外部 Base Block");
    case 52: return QStringLiteral("Pcod - 密碼鎖定");
    case 54: return QStringLiteral("CE1 - Modbus 非法功能碼");
    case 55: return QStringLiteral("CE2 - Modbus 非法資料位址");
    case 56: return QStringLiteral("CE3 - Modbus 非法資料值");
    case 57: return QStringLiteral("CE4 - Modbus 寫入唯讀位址");
    case 58: return QStringLiteral("CE10 - Modbus 通訊逾時");
    case 61: return QStringLiteral("ydc - Y/Δ 接線切換異常");
    case 62: return QStringLiteral("dEb - 減速能量備援異常");
    case 63: return QStringLiteral("oSL - 過滑差");
    case 68: return QStringLiteral("SdRv - 速度回授方向相反");
    case 69: return QStringLiteral("SdOr - 速度回授超速");
    case 70: return QStringLiteral("SdDe - 速度回授偏差過大");
    case 72: return QStringLiteral("STL1 - STO 遺失 1");
    case 76: return QStringLiteral("STO - 安全轉矩關斷");
    case 77: return QStringLiteral("STL2 - STO 遺失 2");
    case 78: return QStringLiteral("STL3 - STO 遺失 3");
    case 79: return QStringLiteral("Aoc - 運轉前 U 相過電流");
    case 80: return QStringLiteral("boc - 運轉前 V 相過電流");
    case 81: return QStringLiteral("coc - 運轉前 W 相過電流");
    case 82: return QStringLiteral("OPHL - U 相輸出缺相");
    case 83: return QStringLiteral("OPHL - V 相輸出缺相");
    case 84: return QStringLiteral("OPHL - W 相輸出缺相");
    case 87: return QStringLiteral("oL3 - 低頻過載保護");
    case 89: return QStringLiteral("RoPd - 轉子位置偵測異常");
    case 101: return QStringLiteral("CGdE - CANopen 守護逾時");
    case 102: return QStringLiteral("CHbE - CANopen 心跳異常");
    case 104: return QStringLiteral("CbFE - CANopen 匯流排關閉");
    case 105: return QStringLiteral("CIdE - CANopen 索引異常");
    case 106: return QStringLiteral("CAdE - CANopen 站號異常");
    case 107: return QStringLiteral("CFrE - CANopen 記憶體異常");
    case 121: return QStringLiteral("CP20 - 內部通訊異常");
    case 123: return QStringLiteral("CP22 - 內部通訊異常");
    case 124: return QStringLiteral("CP30 - 內部通訊異常");
    case 126: return QStringLiteral("CP32 - 內部通訊異常");
    case 127: return QStringLiteral("CP33 - 內部通訊異常");
    case 128: return QStringLiteral("ot3 - 過轉矩 3");
    case 129: return QStringLiteral("ot4 - 過轉矩 4");
    case 134: return QStringLiteral("EoL3 - 內部通訊異常");
    case 135: return QStringLiteral("EoL4 - 內部通訊異常");
    case 140: return QStringLiteral("Hd6 - 過電流硬體異常");
    case 141: return QStringLiteral("b4GFF - 運轉前接地故障");
    case 142: return QStringLiteral("AuE1 - 自動調諧異常 1");
    case 143: return QStringLiteral("AuE2 - 自動調諧異常 2");
    case 144: return QStringLiteral("AuE3 - 自動調諧異常 3");
    case 149: return QStringLiteral("AuE5 - 自動調諧異常 5");
    default: return QStringLiteral("未知 Fault Code");
    }
}

QString warningCodeText(quint8 code)
{
    switch (code) {
    case 1: return QStringLiteral("CE1 - Modbus 非法功能碼");
    case 2: return QStringLiteral("CE2 - Modbus 非法資料位址");
    case 3: return QStringLiteral("CE3 - Modbus 非法資料值");
    case 4: return QStringLiteral("CE4 - Modbus 寫入唯讀位址");
    case 5: return QStringLiteral("CE10 - Modbus 通訊逾時");
    case 7: return QStringLiteral("SE1 - Keypad COPY 逾時");
    case 8: return QStringLiteral("SE2 - Keypad COPY 寫入錯誤");
    case 9: return QStringLiteral("oH1 - IGBT 過熱警告");
    case 11: return QStringLiteral("PID - PID 回授遺失");
    case 12: return QStringLiteral("AnL - 類比電流輸入遺失");
    case 13: return QStringLiteral("uC - 欠電流");
    case 17: return QStringLiteral("oSPd - 超速警告");
    case 18: return QStringLiteral("dAvE - 速度偏差警告");
    case 19: return QStringLiteral("PHL - 輸入缺相警告");
    case 20: return QStringLiteral("ot1 - 過轉矩 1 警告");
    case 21: return QStringLiteral("ot2 - 過轉矩 2 警告");
    case 22: return QStringLiteral("oH3 - 馬達過熱警告");
    case 24: return QStringLiteral("oSL - 過滑差警告");
    case 25: return QStringLiteral("tUn - 自動調諧中");
    case 28: return QStringLiteral("oPHL - 輸出缺相警告");
    case 30: return QStringLiteral("SE3 - Keypad COPY 機型錯誤");
    case 31: return QStringLiteral("ot3 - 過轉矩 3 警告");
    case 32: return QStringLiteral("ot4 - 過轉矩 4 警告");
    case 36: return QStringLiteral("CGdn - CANopen 守護逾時");
    case 37: return QStringLiteral("CHbn - CANopen 心跳異常");
    case 39: return QStringLiteral("CbFn - CANopen 匯流排關閉");
    case 40: return QStringLiteral("Cidn - CANopen 索引異常");
    case 41: return QStringLiteral("CAdn - CANopen 站號異常");
    case 42: return QStringLiteral("CFrn - CANopen 記憶體異常");
    case 43: return QStringLiteral("CSdn - CANopen SDO 逾時");
    case 44: return QStringLiteral("CSbn - CANopen SDO 接收溢位");
    case 45: return QStringLiteral("Cbtn - CANopen 啟動異常");
    case 46: return QStringLiteral("CPtn - CANopen 格式異常");
    case 50: return QStringLiteral("PLod - PLC 下載異常");
    case 51: return QStringLiteral("PLSv - PLC 儲存記憶體異常");
    case 52: return QStringLiteral("PLdA - PLC 資料異常");
    case 53: return QStringLiteral("PLFn - PLC 功能碼異常");
    case 54: return QStringLiteral("PLor - PLC 暫存器溢位");
    case 55: return QStringLiteral("PLFF - PLC 功能異常");
    case 56: return QStringLiteral("PLSn - PLC checksum 異常");
    case 57: return QStringLiteral("PLEd - PLC 缺少結束命令");
    case 58: return QStringLiteral("PLCr - PLC MCR 命令異常");
    case 59: return QStringLiteral("PLdF - PLC 下載失敗");
    case 60: return QStringLiteral("PLSF - PLC 掃描時間超限");
    case 70: return QStringLiteral("ECid - 通訊卡 MAC ID 重複");
    case 71: return QStringLiteral("ECLv - 通訊卡低電壓");
    case 72: return QStringLiteral("ECtt - 通訊卡測試模式");
    case 73: return QStringLiteral("ECbF - 通訊卡 BUS-OFF");
    case 74: return QStringLiteral("ECnP - DeviceNet 無電源");
    case 75: return QStringLiteral("ECFF - 通訊卡出廠設定異常");
    case 76: return QStringLiteral("ECiF - 通訊卡內部異常");
    case 78: return QStringLiteral("ECPP - Profibus 參數資料異常");
    case 79: return QStringLiteral("ECPi - Profibus 組態資料異常");
    case 80: return QStringLiteral("ECEF - Ethernet 網路線未連接");
    case 81: return QStringLiteral("ECto - 通訊卡通訊逾時");
    case 82: return QStringLiteral("ECCS - 通訊卡 checksum 異常");
    case 83: return QStringLiteral("ECrF - 通訊卡恢復預設值");
    case 84: return QStringLiteral("ECo0 - Modbus TCP 連線數超限");
    case 85: return QStringLiteral("ECo1 - EtherNet/IP 連線數超限");
    case 86: return QStringLiteral("ECiP - IP 設定異常");
    case 87: return QStringLiteral("EC3F - 通訊卡警報郵件");
    case 88: return QStringLiteral("ECbY - 通訊卡忙碌");
    case 89: return QStringLiteral("ECCb - 通訊卡中斷");
    case 90: return QStringLiteral("CPLP - Copy PLC 密碼錯誤");
    case 91: return QStringLiteral("CPL0 - Copy PLC 讀取模式錯誤");
    case 92: return QStringLiteral("CPL1 - Copy PLC 寫入模式錯誤");
    case 93: return QStringLiteral("CPLv - Copy PLC 版本錯誤");
    case 94: return QStringLiteral("CPLS - Copy PLC 容量錯誤");
    case 95: return QStringLiteral("CPLF - Copy PLC 功能錯誤");
    case 96: return QStringLiteral("CPLt - Copy PLC 逾時");
    case 98: return QStringLiteral("Fire - 消防模式輸出");
    default: return QStringLiteral("未知 Warning Code");
    }
}
}

Ms300FaultReader::Ms300FaultReader(QObject *parent)
    : QObject(parent)
    , m_client(new QModbusRtuSerialClient(this))
{
    m_pollTimer.setInterval(kMs300PollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &Ms300FaultReader::pollFaultStatus);

    // MS300 RTU connection: COM2, 9600, N-8-1, station address 1.
    m_client->setConnectionParameter(QModbusDevice::SerialPortNameParameter,
                                     QVariant(QString::fromLatin1(kMs300PortName)));
    m_client->setConnectionParameter(QModbusDevice::SerialBaudRateParameter,
                                     QVariant::fromValue(QSerialPort::Baud9600));
    m_client->setConnectionParameter(QModbusDevice::SerialDataBitsParameter,
                                     QVariant::fromValue(QSerialPort::Data8));
    m_client->setConnectionParameter(QModbusDevice::SerialParityParameter,
                                     QVariant::fromValue(QSerialPort::NoParity));
    m_client->setConnectionParameter(QModbusDevice::SerialStopBitsParameter,
                                     QVariant::fromValue(QSerialPort::OneStop));
    m_client->setTimeout(kMs300TimeoutMs);
    m_client->setNumberOfRetries(1);

    connect(m_client, &QModbusDevice::stateChanged, this,
            [this](QModbusDevice::State state) {
        const bool connected = state == QModbusDevice::ConnectedState;
        QString detail = connected ? QStringLiteral("connected") : m_client->errorString();
        if (detail.isEmpty())
            detail = QStringLiteral("disconnected");
        qInfo().noquote()
                << QStringLiteral("[MS300] %1 COM2, Unit ID 1: %2")
                           .arg(connected ? QStringLiteral("Connected to")
                                          : QStringLiteral("Disconnected from"),
                                detail);
        emit connectionChanged(connected, detail);

        if (connected && m_running)
            pollFaultStatus();
    });

    connect(m_client, &QModbusDevice::errorOccurred, this,
            [this](QModbusDevice::Error error) {
        if (error == QModbusDevice::NoError)
            return;

        const QString message = m_client->errorString();
        qWarning().noquote() << "[MS300]" << message;
        emit readError(message);
    });
}

Ms300FaultReader::~Ms300FaultReader()
{
    stop();
}

void Ms300FaultReader::start()
{
    if (m_running)
        return;

    m_running = true;
    m_pollTimer.start();
    pollFaultStatus();
}

void Ms300FaultReader::stop()
{
    m_running = false;
    m_pollTimer.stop();
    m_requestPending = false;
    if (m_client->state() != QModbusDevice::UnconnectedState)
        m_client->disconnectDevice();
}

void Ms300FaultReader::pollFaultStatus()
{
    if (!m_running || m_requestPending)
        return;

    if (m_client->state() != QModbusDevice::ConnectedState) {
        if (m_client->state() == QModbusDevice::UnconnectedState
                && !m_client->connectDevice()) {
            const QString message = m_client->errorString();
            qWarning().noquote() << "[MS300] Connection request failed:" << message;
            emit readError(message);
        }
        return;
    }

    // MS300 status monitor 21xx: 2100H, function 03H, one U16 register.
    // bits 7..0 are the fault code; bits 15..8 are the warning code.
    const QModbusDataUnit request(QModbusDataUnit::HoldingRegisters,
                                  kMs300FaultStatusRegister,
                                  1);
    QModbusReply *reply = m_client->sendReadRequest(request, kMs300UnitId);
    if (!reply) {
        const QString message = m_client->errorString();
        qWarning().noquote() << "[MS300] Fault-status read request failed:" << message;
        emit readError(message);
        return;
    }

    m_requestPending = true;
    const auto handleReply = [this, reply]() {
        m_requestPending = false;
        if (reply->error() != QModbusDevice::NoError) {
            const QString message = reply->errorString();
            qWarning().noquote() << "[MS300] Fault-status read failed:" << message;
            emit readError(message);
        } else {
            const QList<quint16> values = reply->result().values();
            if (values.isEmpty()) {
                const QString message = QStringLiteral("MS300 returned an empty fault-status response.");
                qWarning().noquote() << "[MS300]" << message;
                emit readError(message);
            } else {
                publishFaultStatus(values.constFirst());
            }
        }
        reply->deleteLater();
    };

    connect(reply, &QModbusReply::finished, this, handleReply);
    if (reply->isFinished())
        handleReply();
}

void Ms300FaultReader::publishFaultStatus(quint16 rawStatus)
{
    const quint8 faultCode = static_cast<quint8>(rawStatus & 0x00ff);
    const quint8 warningCode = static_cast<quint8>((rawStatus >> 8) & 0x00ff);
    QStringList parts;
    if (faultCode != 0)
        parts.append(QStringLiteral("Fault %1 (0x%2): %3")
                             .arg(faultCode)
                             .arg(faultCode, 2, 16, QLatin1Char('0')).toUpper()
                             .arg(faultCodeText(faultCode)));
    if (warningCode != 0)
        parts.append(QStringLiteral("Warning %1 (0x%2): %3")
                             .arg(warningCode)
                             .arg(warningCode, 2, 16, QLatin1Char('0')).toUpper()
                             .arg(warningCodeText(warningCode)));
    const QString message = parts.isEmpty() ? QStringLiteral("正常")
                                             : parts.join(QStringLiteral("；"));

    qInfo().noquote()
            << QStringLiteral("[MS300][Fault Read] register=0x2100 raw=0x%1 fault=%2 warning=%3 message=%4")
                       .arg(rawStatus, 4, 16, QLatin1Char('0')).toUpper()
                       .arg(faultCode)
                       .arg(warningCode)
                       .arg(message);
    emit faultStatusRead(faultCode, warningCode, message);

    if (m_hasLastStatus && m_lastStatus == rawStatus)
        return;

    m_hasLastStatus = true;
    m_lastStatus = rawStatus;
    emit faultStatusChanged(faultCode, warningCode, message);
}
