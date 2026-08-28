#pragma once
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <sstream>
#include <iomanip>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include <string>
#include <QDir>
#include <QGuiApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>
#include <QCoreApplication>
#include <QDebug>
#include <QDateTime>
#include <QFileDialog>
#include <QTimer>
class TaidaFlowProxy : public QObject
{
    Q_OBJECT

    // Writable set values (SV): edited by TextField / switch controls.
    Q_PROPERTY(double m1ValueSv READ m1ValueSv WRITE setM1ValueSv NOTIFY m1ValueSvChanged)
    Q_PROPERTY(double m2ValueSv READ m2ValueSv WRITE setM2ValueSv NOTIFY m2ValueSvChanged)
    Q_PROPERTY(double m3ValueSv READ m3ValueSv WRITE setM3ValueSv NOTIFY m3ValueSvChanged)
    Q_PROPERTY(double m4ValueSv READ m4ValueSv WRITE setM4ValueSv NOTIFY m4ValueSvChanged)
    Q_PROPERTY(double m1ValuePv READ m1ValuePv WRITE setM1ValuePv NOTIFY m1ValuePvChanged)
    Q_PROPERTY(double m2ValuePv READ m2ValuePv WRITE setM2ValuePv NOTIFY m2ValuePvChanged)
    Q_PROPERTY(double m3ValuePv READ m3ValuePv WRITE setM3ValuePv NOTIFY m3ValuePvChanged)
    Q_PROPERTY(double m4ValuePv READ m4ValuePv WRITE setM4ValuePv NOTIFY m4ValuePvChanged)
    Q_PROPERTY(double pump2HzSv READ pump2HzSv WRITE setPump2HzSv NOTIFY pump2HzSvChanged)
    Q_PROPERTY(double pump2HzPv READ pump2HzPv WRITE setPump2HzPv NOTIFY pump2HzPvChanged)
    Q_PROPERTY(bool motorRunningSv READ motorRunningSv WRITE setMotorRunningSv NOTIFY motorRunningSvChanged)

    // Read-only process values (PV): QML can observe but cannot write.
    Q_PROPERTY(double tt01ValuePv READ tt01ValuePv WRITE setTt01ValuePv NOTIFY tt01ValuePvChanged)
    Q_PROPERTY(double tt02ValuePv READ tt02ValuePv WRITE setTt02ValuePv NOTIFY tt02ValuePvChanged)
    Q_PROPERTY(double tt03ValuePv READ tt03ValuePv WRITE setTt03ValuePv NOTIFY tt03ValuePvChanged)
    Q_PROPERTY(double tt04ValuePv READ tt04ValuePv WRITE setTt04ValuePv NOTIFY tt04ValuePvChanged)
    Q_PROPERTY(double pt01ValuePv READ pt01ValuePv WRITE setPt01ValuePv NOTIFY pt01ValuePvChanged)
    Q_PROPERTY(double pt02ValuePv READ pt02ValuePv WRITE setPt02ValuePv NOTIFY pt02ValuePvChanged)
    Q_PROPERTY(double pt03ValuePv READ pt03ValuePv WRITE setPt03ValuePv NOTIFY pt03ValuePvChanged)
    Q_PROPERTY(double pt04ValuePv READ pt04ValuePv WRITE setPt04ValuePv NOTIFY pt04ValuePvChanged)
    Q_PROPERTY(double pt05ValuePv READ pt05ValuePv WRITE setPt05ValuePv NOTIFY pt05ValuePvChanged)
    Q_PROPERTY(double pt06ValuePv READ pt06ValuePv WRITE setPt06ValuePv NOTIFY pt06ValuePvChanged)
    Q_PROPERTY(double pt07ValuePv READ pt07ValuePv WRITE setPt07ValuePv NOTIFY pt07ValuePvChanged)
    Q_PROPERTY(double flowMeterValuePv READ flowMeterValuePv WRITE setFlowMeterValuePv NOTIFY flowMeterValuePvChanged)

    // List data is owned by the authoritative side. QML only reads a local copy.
    Q_PROPERTY(QVariantList historyRecords READ historyRecords WRITE setHistoryRecords NOTIFY historyRecordsChanged)
    Q_PROPERTY(QVariantList alarmRecords READ alarmRecords WRITE setAlarmRecords NOTIFY alarmRecordsChanged)

public:
    explicit TaidaFlowProxy(QObject *parent = nullptr)
        : QObject(parent)
    {
        // m_processTimer.setInterval(300);
        // connect(&m_processTimer, &QTimer::timeout,
        //         this, &TaidaFlowProxy::updateSimulatedProcess);
        // m_processTimer.start();
        initializeListData();
    }

    double m1ValueSv() const { return m_m1ValueSv; }
    double m2ValueSv() const { return m_m2ValueSv; }
    double m3ValueSv() const { return m_m3ValueSv; }
    double m4ValueSv() const { return m_m4ValueSv; }
    double m1ValuePv() const { return m_m1ValuePv; }
    double m2ValuePv() const { return m_m2ValuePv; }
    double m3ValuePv() const { return m_m3ValuePv; }
    double m4ValuePv() const { return m_m4ValuePv; }
    double pump2HzSv() const { return m_pump2HzSv; }
    double pump2HzPv() const { return m_pump2HzPv; }
    bool motorRunningSv() const { return m_motorRunningSv; }

    double tt01ValuePv() const { return m_tt01ValuePv; }
    double tt02ValuePv() const { return m_tt02ValuePv; }
    double tt03ValuePv() const { return m_tt03ValuePv; }
    double tt04ValuePv() const { return m_tt04ValuePv; }
    double pt01ValuePv() const { return m_pt01ValuePv; }
    double pt02ValuePv() const { return m_pt02ValuePv; }
    double pt03ValuePv() const { return m_pt03ValuePv; }
    double pt04ValuePv() const { return m_pt04ValuePv; }
    double pt05ValuePv() const { return m_pt05ValuePv; }
    double pt06ValuePv() const { return m_pt06ValuePv; }
    double pt07ValuePv() const { return m_pt07ValuePv; }
    double flowMeterValuePv() const { return m_flowMeterValuePv; }
    QVariantList historyRecords() const { return m_historyRecords; }
    QVariantList alarmRecords() const { return m_alarmRecords; }

    void setM1ValueSv(double value) { setWritableValue(m_m1ValueSv, value, &TaidaFlowProxy::m1ValueSvChanged); }
    void setM2ValueSv(double value) { setWritableValue(m_m2ValueSv, value, &TaidaFlowProxy::m2ValueSvChanged); }
    void setM3ValueSv(double value) { setWritableValue(m_m3ValueSv, value, &TaidaFlowProxy::m3ValueSvChanged); }
    void setM4ValueSv(double value) { setWritableValue(m_m4ValueSv, value, &TaidaFlowProxy::m4ValueSvChanged); }
    void setM1ValuePv(double value) { setWritableValue(m_m1ValuePv, value, &TaidaFlowProxy::m1ValuePvChanged); }
    void setM2ValuePv(double value) { setWritableValue(m_m2ValuePv, value, &TaidaFlowProxy::m2ValuePvChanged); }
    void setM3ValuePv(double value) { setWritableValue(m_m3ValuePv, value, &TaidaFlowProxy::m3ValuePvChanged); }
    void setM4ValuePv(double value) { setWritableValue(m_m4ValuePv, value, &TaidaFlowProxy::m4ValuePvChanged); }
    void setPump2HzSv(double value) { setWritableValue(m_pump2HzSv, value, &TaidaFlowProxy::pump2HzSvChanged); }
    void setPump2HzPv(double value) { setWritableValue(m_pump2HzPv, value, &TaidaFlowProxy::pump2HzPvChanged); }
    void setTt01ValuePv(double value) { setProcessValue(m_tt01ValuePv, value, &TaidaFlowProxy::tt01ValuePvChanged); }
    void setTt02ValuePv(double value) { setProcessValue(m_tt02ValuePv, value, &TaidaFlowProxy::tt02ValuePvChanged); }
    void setTt03ValuePv(double value) { setProcessValue(m_tt03ValuePv, value, &TaidaFlowProxy::tt03ValuePvChanged); }
    void setTt04ValuePv(double value) { setProcessValue(m_tt04ValuePv, value, &TaidaFlowProxy::tt04ValuePvChanged); }
    void setPt01ValuePv(double value) { setProcessValue(m_pt01ValuePv, value, &TaidaFlowProxy::pt01ValuePvChanged); }
    void setPt02ValuePv(double value) { setProcessValue(m_pt02ValuePv, value, &TaidaFlowProxy::pt02ValuePvChanged); }
    void setPt03ValuePv(double value) { setProcessValue(m_pt03ValuePv, value, &TaidaFlowProxy::pt03ValuePvChanged); }
    void setPt04ValuePv(double value) { setProcessValue(m_pt04ValuePv, value, &TaidaFlowProxy::pt04ValuePvChanged); }
    void setPt05ValuePv(double value) { setProcessValue(m_pt05ValuePv, value, &TaidaFlowProxy::pt05ValuePvChanged); }
    void setPt06ValuePv(double value) { setProcessValue(m_pt06ValuePv, value, &TaidaFlowProxy::pt06ValuePvChanged); }
    void setPt07ValuePv(double value) { setProcessValue(m_pt07ValuePv, value, &TaidaFlowProxy::pt07ValuePvChanged); }
    void setFlowMeterValuePv(double value) { setProcessValue(m_flowMeterValuePv, value, &TaidaFlowProxy::flowMeterValuePvChanged); }
    void setHistoryRecords(const QVariantList &records)
    {
        m_historyRecords = records;
        emit historyRecordsChanged(records);
    }
    void setAlarmRecords(const QVariantList &records)
    {
        m_alarmRecords = records;
        emit alarmRecordsChanged(records);
    }
    void setMotorRunningSv(bool value)
    {
        if (m_motorRunningSv == value) {
            return;
        }
        m_motorRunningSv = value;
        emit motorRunningSvChanged(value);
    }

    Q_INVOKABLE QString saveHistoryCsv(const QString &csvContent)
    {
        const QString suggestedName = QDir::homePath()
                + QStringLiteral("/TaidaFlow_History_")
                + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))
                + QStringLiteral(".csv");
        const QString fileName = QFileDialog::getSaveFileName(
                nullptr,
                tr("下載歷史資料"),
                suggestedName,
                tr("CSV 檔案 (*.csv)"));

        if (fileName.isEmpty()) {
            return QString();
        }

        QFile file(fileName);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return QStringLiteral("ERROR:") + file.errorString();
        }

        // UTF-8 BOM keeps Traditional Chinese readable when opened in Excel.
        file.write("\xEF\xBB\xBF");
        file.write(csvContent.toUtf8());
        file.close();
        return fileName;
    }
signals:
    void m1ValueSvChanged(double value);
    void m2ValueSvChanged(double value);
    void m3ValueSvChanged(double value);
    void m4ValueSvChanged(double value);
    void m1ValuePvChanged(double value);
    void m2ValuePvChanged(double value);
    void m3ValuePvChanged(double value);
    void m4ValuePvChanged(double value);
    void pump2HzSvChanged(double value);
    void pump2HzPvChanged(double value);
    void motorRunningSvChanged(bool value);

    void tt01ValuePvChanged(double value);
    void tt02ValuePvChanged(double value);
    void tt03ValuePvChanged(double value);
    void tt04ValuePvChanged(double value);
    void pt01ValuePvChanged(double value);
    void pt02ValuePvChanged(double value);
    void pt03ValuePvChanged(double value);
    void pt04ValuePvChanged(double value);
    void pt05ValuePvChanged(double value);
    void pt06ValuePvChanged(double value);
    void pt07ValuePvChanged(double value);
    void flowMeterValuePvChanged(double value);
    void historyRecordsChanged(const QVariantList &records);
    void alarmRecordsChanged(const QVariantList &records);

private:
    using ValueSignal = void (TaidaFlowProxy::*)(double);

    void setWritableValue(double &target, double value, ValueSignal signal)
    {
        if (qFuzzyCompare(target + 1.0, value + 1.0)) {
            return;
        }
        target = value;
        emit (this->*signal)(value);
    }

    void setProcessValue(double &target, double value, ValueSignal signal)
    {
        if (qFuzzyCompare(target + 1.0, value + 1.0)) {
            return;
        }
        target = value;
        emit (this->*signal)(value);
    }

    void initializeListData()
    {
        static const QStringList historyDevices{
            QStringLiteral("循環泵浦 A"), QStringLiteral("循環泵浦 B"),
            QStringLiteral("主水槽"), QStringLiteral("過濾器"),
            QStringLiteral("測試設備"), QStringLiteral("加熱器")};
        static const QStringList historySensors{
            QStringLiteral("TT-01"), QStringLiteral("PT-02"),
            QStringLiteral("LS-01"), QStringLiteral("FM-01"),
            QStringLiteral("TT-03"), QStringLiteral("PT-06")};
        static const QStringList alarmDevices{
            QStringLiteral("循環泵浦 A"), QStringLiteral("主水槽"),
            QStringLiteral("過濾器"), QStringLiteral("測試設備"),
            QStringLiteral("加熱器"), QStringLiteral("循環泵浦 B")};
        static const QStringList alarmSensors{
            QStringLiteral("M1"), QStringLiteral("LS-01"),
            QStringLiteral("PT-03"), QStringLiteral("TT-04"),
            QStringLiteral("TT-03"), QStringLiteral("FM-01")};
        static const QStringList alarmMessages{
            QStringLiteral("馬達回授訊號異常"), QStringLiteral("偵測到漏水訊號"),
            QStringLiteral("壓力超過安全範圍"), QStringLiteral("出口溫度過高"),
            QStringLiteral("加熱溫度偏高"), QStringLiteral("流量低於設定值")};

        const QDateTime now = QDateTime::currentDateTime();
        for (int i = 0; i < 36; ++i) {
            const QDateTime recordTime = now.addSecs(-i * 2 * 60 * 60);
            const bool leakOn = i == 7 || i == 19;
            m_historyRecords.append(QVariantMap{
                {QStringLiteral("timestampMs"), recordTime.toMSecsSinceEpoch()},
                {QStringLiteral("recordTime"), recordTime.toString(QStringLiteral("yyyy/MM/dd HH:mm"))},
                {QStringLiteral("equipment"), historyDevices.at(i % historyDevices.size())},
                {QStringLiteral("sensorName"), historySensors.at(i % historySensors.size())},
                {QStringLiteral("switchState"), (i % 5 == 0 || leakOn) ? QStringLiteral("OFF") : QStringLiteral("ON")},
                {QStringLiteral("leakState"), leakOn ? QStringLiteral("ON") : QStringLiteral("OFF")}});
        }

        for (int i = 0; i < 14; ++i) {
            const QDateTime alarmTime = now.addSecs(-i * 37 * 60);
            const bool active = i == 0 || i == 3 || i == 8;
            m_alarmRecords.append(QVariantMap{
                {QStringLiteral("timestampMs"), alarmTime.toMSecsSinceEpoch()},
                {QStringLiteral("alarmTime"), alarmTime.toString(QStringLiteral("yyyy/MM/dd HH:mm"))},
                {QStringLiteral("equipment"), alarmDevices.at(i % alarmDevices.size())},
                {QStringLiteral("sensorName"), alarmSensors.at(i % alarmSensors.size())},
                {QStringLiteral("alarmMessage"), alarmMessages.at(i % alarmMessages.size())},
                {QStringLiteral("severity"), i % 4 == 0 ? QStringLiteral("嚴重") : QStringLiteral("警告")},
                {QStringLiteral("alarmStatus"), active ? QStringLiteral("未處理") : QStringLiteral("已解除")}});
        }
    }

    // void updateSimulatedProcess()
    // {
    //     if (m_temperatureIncreasing) {
    //         m_testTemperature += 1.0;
    //         if (m_testTemperature >= 50.0) {
    //             m_testTemperature = 50.0;
    //             m_temperatureIncreasing = false;
    //         }
    //     } else {
    //         m_testTemperature -= 1.0;
    //         if (m_testTemperature <= 20.0) {
    //             m_testTemperature = 20.0;
    //             m_temperatureIncreasing = true;
    //         }
    //     }
    //
    //     // setProcessValue(m_tt01ValuePv, m_testTemperature, &TaidaFlowProxy::tt01ValuePvChanged);
    //     // setProcessValue(m_tt02ValuePv, m_testTemperature + 3.0, &TaidaFlowProxy::tt02ValuePvChanged);
    //     // setProcessValue(m_tt03ValuePv, m_testTemperature - 2.0, &TaidaFlowProxy::tt03ValuePvChanged);
    //     // setProcessValue(m_tt04ValuePv, m_testTemperature + 5.0, &TaidaFlowProxy::tt04ValuePvChanged);
    //     // setProcessValue(m_flowMeterValuePv,
    //     //                 m_testTemperature <= 23.0 ? 0.0 : 20.0,
    //     //                 &TaidaFlowProxy::flowMeterValuePvChanged);
    // }

    double m_m1ValueSv = 0.0;
    double m_m2ValueSv = 0.0;
    double m_m3ValueSv = 0.0;
    double m_m4ValueSv = 0.0;
    double m_m1ValuePv = 10.1;
    double m_m2ValuePv = 10.1;
    double m_m3ValuePv = 10.1;
    double m_m4ValuePv = 10.1;
    double m_pump2HzSv = 0.0;
    double m_pump2HzPv = 10.1;
    bool m_motorRunningSv = false;

    double m_tt01ValuePv = 0.0;
    double m_tt02ValuePv = 0.0;
    double m_tt03ValuePv = 0.0;
    double m_tt04ValuePv = 0.0;
    double m_pt01ValuePv = 0.0;
    double m_pt02ValuePv = 0.0;
    double m_pt03ValuePv = 0.0;
    double m_pt04ValuePv = 0.0;
    double m_pt05ValuePv = 0.0;
    double m_pt06ValuePv = 0.0;
    double m_pt07ValuePv = 0.0;
    double m_flowMeterValuePv = 0.0;
    QVariantList m_historyRecords;
    QVariantList m_alarmRecords;

    double m_testTemperature = 0.0;
    bool m_temperatureIncreasing = false;
    QTimer m_processTimer;

};
