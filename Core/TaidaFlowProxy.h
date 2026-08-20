#pragma once
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <sstream>
#include <iomanip>
#include <QVariantList>
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
    QML_NAMED_ELEMENT(Hd)
    QML_SINGLETON

    // Writable set values (SV): edited by TextField / switch controls.
    Q_PROPERTY(double m1ValueSv READ m1ValueSv WRITE setM1ValueSv NOTIFY m1ValueSvChanged)
    Q_PROPERTY(double m2ValueSv READ m2ValueSv WRITE setM2ValueSv NOTIFY m2ValueSvChanged)
    Q_PROPERTY(double m3ValueSv READ m3ValueSv WRITE setM3ValueSv NOTIFY m3ValueSvChanged)
    Q_PROPERTY(double m4ValueSv READ m4ValueSv WRITE setM4ValueSv NOTIFY m4ValueSvChanged)
    Q_PROPERTY(double pump2HzSv READ pump2HzSv WRITE setPump2HzSv NOTIFY pump2HzSvChanged)
    Q_PROPERTY(bool motorRunningSv READ motorRunningSv WRITE setMotorRunningSv NOTIFY motorRunningSvChanged)

    // Read-only process values (PV): QML can observe but cannot write.
    Q_PROPERTY(double tt01ValuePv READ tt01ValuePv NOTIFY tt01ValuePvChanged)
    Q_PROPERTY(double tt02ValuePv READ tt02ValuePv NOTIFY tt02ValuePvChanged)
    Q_PROPERTY(double tt03ValuePv READ tt03ValuePv NOTIFY tt03ValuePvChanged)
    Q_PROPERTY(double tt04ValuePv READ tt04ValuePv NOTIFY tt04ValuePvChanged)
    Q_PROPERTY(double pt01ValuePv READ pt01ValuePv NOTIFY pt01ValuePvChanged)
    Q_PROPERTY(double pt02ValuePv READ pt02ValuePv NOTIFY pt02ValuePvChanged)
    Q_PROPERTY(double pt03ValuePv READ pt03ValuePv NOTIFY pt03ValuePvChanged)
    Q_PROPERTY(double pt04ValuePv READ pt04ValuePv NOTIFY pt04ValuePvChanged)
    Q_PROPERTY(double pt05ValuePv READ pt05ValuePv NOTIFY pt05ValuePvChanged)
    Q_PROPERTY(double pt06ValuePv READ pt06ValuePv NOTIFY pt06ValuePvChanged)
    Q_PROPERTY(double pt07ValuePv READ pt07ValuePv NOTIFY pt07ValuePvChanged)
    Q_PROPERTY(double flowMeterValuePv READ flowMeterValuePv NOTIFY flowMeterValuePvChanged)

public:
    explicit TaidaFlowProxy(QObject *parent = nullptr)
        : QObject(parent)
    {
        m_processTimer.setInterval(300);
        connect(&m_processTimer, &QTimer::timeout,
                this, &TaidaFlowProxy::updateSimulatedProcess);
        m_processTimer.start();
    }

    double m1ValueSv() const { return m_m1ValueSv; }
    double m2ValueSv() const { return m_m2ValueSv; }
    double m3ValueSv() const { return m_m3ValueSv; }
    double m4ValueSv() const { return m_m4ValueSv; }
    double pump2HzSv() const { return m_pump2HzSv; }
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

    void setM1ValueSv(double value) { setWritableValue(m_m1ValueSv, value, &TaidaFlowProxy::m1ValueSvChanged); }
    void setM2ValueSv(double value) { setWritableValue(m_m2ValueSv, value, &TaidaFlowProxy::m2ValueSvChanged); }
    void setM3ValueSv(double value) { setWritableValue(m_m3ValueSv, value, &TaidaFlowProxy::m3ValueSvChanged); }
    void setM4ValueSv(double value) { setWritableValue(m_m4ValueSv, value, &TaidaFlowProxy::m4ValueSvChanged); }
    void setPump2HzSv(double value) { setWritableValue(m_pump2HzSv, value, &TaidaFlowProxy::pump2HzSvChanged); }
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
    void pump2HzSvChanged(double value);
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

    void updateSimulatedProcess()
    {
        if (m_temperatureIncreasing) {
            m_testTemperature += 1.0;
            if (m_testTemperature >= 50.0) {
                m_testTemperature = 50.0;
                m_temperatureIncreasing = false;
            }
        } else {
            m_testTemperature -= 1.0;
            if (m_testTemperature <= 20.0) {
                m_testTemperature = 20.0;
                m_temperatureIncreasing = true;
            }
        }

        setProcessValue(m_tt01ValuePv, m_testTemperature, &TaidaFlowProxy::tt01ValuePvChanged);
        setProcessValue(m_tt02ValuePv, m_testTemperature + 3.0, &TaidaFlowProxy::tt02ValuePvChanged);
        setProcessValue(m_tt03ValuePv, m_testTemperature - 2.0, &TaidaFlowProxy::tt03ValuePvChanged);
        setProcessValue(m_tt04ValuePv, m_testTemperature + 5.0, &TaidaFlowProxy::tt04ValuePvChanged);
        setProcessValue(m_flowMeterValuePv,
                        m_testTemperature <= 23.0 ? 0.0 : 20.0,
                        &TaidaFlowProxy::flowMeterValuePvChanged);
    }

    double m_m1ValueSv = 2220.0;
    double m_m2ValueSv = 20.0;
    double m_m3ValueSv = 20.0;
    double m_m4ValueSv = 20.0;
    double m_pump2HzSv = 20.0;
    bool m_motorRunningSv = true;

    double m_tt01ValuePv = 20.0;
    double m_tt02ValuePv = 23.0;
    double m_tt03ValuePv = 18.0;
    double m_tt04ValuePv = 25.0;
    double m_pt01ValuePv = 25.5;
    double m_pt02ValuePv = 25.5;
    double m_pt03ValuePv = 2.5;
    double m_pt04ValuePv = 2.5;
    double m_pt05ValuePv = 2.5;
    double m_pt06ValuePv = 2.5;
    double m_pt07ValuePv = 2.5;
    double m_flowMeterValuePv = 0.0;

    double m_testTemperature = 20.0;
    bool m_temperatureIncreasing = true;
    QTimer m_processTimer;

};


