#pragma once
// w2-086 (desktop only, never compiled into WebAssembly): the settings-page Offset
// (TaidaFlowProxy::sensorSettingsSv[key].offset, contract Core/SensorSettings.md, scope table
// Core/SensorOffset.md) is applied when a sample is STORED - Mango 2026-10-02: 「存檔時就加上
// offset：資料庫、歷史頁、CSV、REST 全部一致」 - and to the PVs the Modbus server gives external HMIs
// (Mango 2026-10-02: 「Modbus server 給外部 HMI 的 PV 要套用 offset」).
//
// Rules (PM decisions of w2-086):
//  * sensor_data s1..s16 / Modbus server input registers 0..15 hold ADAM-6217 raw counts (0..65535).
//    For a column with an offset, the offset (engineering units: pressure kPa, temperature degC,
//    flow L/min) is converted into counts with that column's scale and added to the raw count:
//        stored = clamp(round(raw + offset / scale), 0, 65535)      (round = half away from zero)
//    The history page / CSV / REST convert stored * scale, so they show raw * scale + offset within
//    half a count (<= 1 count) - the value the main screen shows (SensorUnits.adjusted()).
//  * Scale: ModbusMapping::defaultReadBindings() (ReadBinding::scale, the live-display table) is the
//    single source; the column / register of a binding is computed exactly like
//    Manager::mirrorClientData() does (ADAM-6217 .202 -> 0 + AI, .203 -> 8 + AI).
//  * Columns with an offset key: tt01..tt04 (s1..s4), pt01..pt04 (s5..s8), pt05..pt07 (s9..s11),
//    flowMeter (s12).  MV1..MV4 positions (s13..s16) and the unused registers 16..19 never change.
//    filter has no column (its value is corrected PT-02 - corrected PT-03, consistent by itself).
//  * The offset of the moment is used: sensorSettingsSv is read for every sample / register, so the
//    first sample after a settings change already uses the new offset.  Offset 0, no entry or a
//    non-finite offset -> the raw count unchanged.  Old rows are never rewritten.
//  * Clamped at 0 or 65535: a warning per column, rate limited (RepeatedWarningLimiter, 60 s).
//  * Not changed: the Proxy PVs (raw; the UI adds the offset itself), the 90 % high alarm (raw), the
//    settings HR11..40 of the Modbus server (Ukai0107), DI / DO / coils, the database schema.
#include "ModbusMapping.h"
#include "Modbus_Client.h"   // RepeatedWarningLimiter

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

class TaidaFlowProxy;

class SensorOffsetStorage final : public QObject
{
    Q_OBJECT
public:
    // One sensor with an offset key, its database column / Modbus server input register and scale.
    struct Column
    {
        QString key;                       // sensorSettingsSv key: tt01..tt04, pt01..pt07, flowMeter
        ModbusMapping::ProcessPoint point = ModbusMapping::ProcessPoint::Tt01;
        int serverRegister = -1;           // Modbus server input register (0-based)
        int column = 0;                    // sensor_data column number: s<column> = register + 1
        double scale = 1.0;                // engineering units per count (ReadBinding::scale)
        QString unit;                      // kPa, °C, L/min
    };

    enum class Clamp { None, Low, High };
    struct Result
    {
        quint16 value = 0;
        Clamp clamp = Clamp::None;
        double offsetCounts = 0.0;         // offset / scale
    };

    struct Options
    {
        qint64 clampWarningIntervalMs = RepeatedWarningLimiter::kDefaultIntervalMs;
    };

    SensorOffsetStorage(TaidaFlowProxy *proxy,
                        const QList<ModbusMapping::ReadBinding> &bindings,
                        const Options &options,
                        QObject *parent = nullptr);
    SensorOffsetStorage(TaidaFlowProxy *proxy,
                        const QList<ModbusMapping::ReadBinding> &bindings,
                        QObject *parent = nullptr);

    // Manager::mirrorClientData: the value for Modbus server input register 'serverRegister'.
    quint16 correctedServerRegister(int serverRegister, quint16 raw);
    // Manager::saveServerInputData: the readings handed to SqlManager::saveSensorData (one per
    // register, in register order = s1, s2, ...).
    QVector<double> correctedSample(const QVector<quint16> &rawRegisters);

    const QList<Column> &columns() const { return m_columns; }
    // Clamp warnings written / held back so far (tests).
    qint64 clampWarningsWritten() const { return m_clampWarningsWritten; }

    // ---- rules, shared with the tests ----
    static QString sensorKey(ModbusMapping::ProcessPoint point);          // empty: no offset key
    static QString unitOf(const QString &key);                            // kPa, °C, L/min
    // Modbus server input register of a read binding (as Manager::mirrorClientData); -1: none.
    static int serverRegisterOf(const ModbusMapping::ReadBinding &binding);
    static QList<Column> columnsOf(const QList<ModbusMapping::ReadBinding> &bindings);
    // sensorSettingsSv[key].offset; missing entry / field or non-finite -> 0.
    static double offsetOf(const QVariantMap &settings, const QString &key);
    static Result apply(quint16 raw, double offset, double scale);

private:
    quint16 correct(const Column &column, const QVariantMap &settings, quint16 raw, const char *where);
    void logSettings();

    TaidaFlowProxy *m_proxy = nullptr;
    Options m_options;
    QList<Column> m_columns;
    QHash<int, int> m_columnOfRegister;              // server register -> index in m_columns
    QHash<int, RepeatedWarningLimiter> m_clampWarnings;   // per server register
    qint64 m_clampWarningsWritten = 0;
    QString m_lastLoggedOffsets;
};
