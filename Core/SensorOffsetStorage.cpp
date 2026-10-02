#include "SensorOffsetStorage.h"

#include "ModbusServerBridgeMapping.h"
#include "TaidaFlowProxy.h"

#include <QDebug>
#include <QStringList>

#include <cmath>
#include <limits>

// w2-086: see SensorOffsetStorage.h for the rules.

SensorOffsetStorage::SensorOffsetStorage(TaidaFlowProxy *proxy,
                                         const QList<ModbusMapping::ReadBinding> &bindings,
                                         const Options &options,
                                         QObject *parent)
    : QObject(parent)
    , m_proxy(proxy)
    , m_options(options)
    , m_columns(columnsOf(bindings))
{
    QStringList table;
    for (qsizetype index = 0; index < m_columns.size(); ++index) {
        const Column &column = m_columns.at(index);
        m_columnOfRegister.insert(column.serverRegister, int(index));
        table << QStringLiteral("%1=s%2/IR%3 x%4 %5")
                         .arg(column.key)
                         .arg(column.column)
                         .arg(column.serverRegister)
                         .arg(column.scale, 0, 'g', 10)
                         .arg(column.unit);
    }
    qInfo().noquote()
            << QStringLiteral("[SensorOffset] offsets are applied to sensor_data and the Modbus server input "
                              "registers (stored = round(raw + offset / scale), clamped 0..65535); %1 columns: %2")
                       .arg(m_columns.size())
                       .arg(table.join(QStringLiteral(", ")));
    if (m_proxy) {
        connect(m_proxy, &TaidaFlowProxy::sensorSettingsSvChanged, this, &SensorOffsetStorage::logSettings);
        logSettings();
    }
}

SensorOffsetStorage::SensorOffsetStorage(TaidaFlowProxy *proxy,
                                         const QList<ModbusMapping::ReadBinding> &bindings,
                                         QObject *parent)
    : SensorOffsetStorage(proxy, bindings, Options{}, parent)
{
}

QString SensorOffsetStorage::sensorKey(ModbusMapping::ProcessPoint point)
{
    using P = ModbusMapping::ProcessPoint;
    switch (point) {
    case P::Tt01: return QStringLiteral("tt01");
    case P::Tt02: return QStringLiteral("tt02");
    case P::Tt03: return QStringLiteral("tt03");
    case P::Tt04: return QStringLiteral("tt04");
    case P::Pt01: return QStringLiteral("pt01");
    case P::Pt02: return QStringLiteral("pt02");
    case P::Pt03: return QStringLiteral("pt03");
    case P::Pt04: return QStringLiteral("pt04");
    case P::Pt05: return QStringLiteral("pt05");
    case P::Pt06: return QStringLiteral("pt06");
    case P::Pt07: return QStringLiteral("pt07");
    case P::FlowMeter: return QStringLiteral("flowMeter");
    case P::Mv1Position:
    case P::Mv2Position:
    case P::Mv3Position:
    case P::Mv4Position:
        return {};
    }
    return {};
}

QString SensorOffsetStorage::unitOf(const QString &key)
{
    if (key.startsWith(QLatin1String("pt")))
        return QStringLiteral("kPa");
    if (key.startsWith(QLatin1String("tt")))
        return QStringLiteral("°C");
    return QStringLiteral("L/min");
}

int SensorOffsetStorage::serverRegisterOf(const ModbusMapping::ReadBinding &binding)
{
    // Same mapping as Manager::mirrorClientData(): only the ADAM-6217 holding registers are mirrored,
    // register = group start + AI number, AI number = startAddress + value index.
    if (!binding.isConfigured() || binding.registerType != QModbusDataUnit::HoldingRegisters)
        return -1;
    int start = -1;
    if (binding.device == ModbusClient::Device::Adam6217_202)
        start = ModbusServerBridgeMapping::ServerAdam6217AInputStart;
    else if (binding.device == ModbusClient::Device::Adam6217_203)
        start = ModbusServerBridgeMapping::ServerAdam6217BInputStart;
    if (start < 0)
        return -1;
    const qint64 ai = qint64(binding.startAddress) + binding.valueOffset;
    if (ai < 0 || ai >= ModbusServerBridgeMapping::Adam6217AiCount)
        return -1;
    return start + int(ai);
}

QList<SensorOffsetStorage::Column> SensorOffsetStorage::columnsOf(const QList<ModbusMapping::ReadBinding> &bindings)
{
    QList<Column> columns;
    for (const ModbusMapping::ReadBinding &binding : bindings) {
        const QString key = sensorKey(binding.point);
        const int reg = serverRegisterOf(binding);
        // Counts are 0..65535 (unsigned); a scale of 0 / non-finite cannot convert an offset.
        if (key.isEmpty() || reg < 0 || binding.valueFormat != ModbusMapping::ValueFormat::Unsigned16
                || !std::isfinite(binding.scale) || binding.scale == 0.0) {
            continue;
        }
        Column column;
        column.key = key;
        column.point = binding.point;
        column.serverRegister = reg;
        column.column = reg + 1;
        column.scale = binding.scale;
        column.unit = unitOf(key);
        columns.append(column);
    }
    return columns;
}

double SensorOffsetStorage::offsetOf(const QVariantMap &settings, const QString &key)
{
    // The UI's SensorUnits.adjusted(): raw + (settings[id] ? settings[id].offset : 0).
    const auto entry = settings.constFind(key);
    if (entry == settings.cend())
        return 0.0;
    const double offset = entry->toMap().value(QStringLiteral("offset")).toDouble();
    return std::isfinite(offset) ? offset : 0.0;
}

SensorOffsetStorage::Result SensorOffsetStorage::apply(quint16 raw, double offset, double scale)
{
    Result result;
    result.value = raw;
    if (offset == 0.0 || !std::isfinite(offset) || !std::isfinite(scale) || scale == 0.0)
        return result;
    result.offsetCounts = offset / scale;
    const double corrected = std::round(double(raw) + result.offsetCounts);   // half away from zero
    constexpr double maximum = std::numeric_limits<quint16>::max();
    if (!(corrected >= 0.0)) {            // also catches NaN (cannot happen: both terms finite)
        result.value = 0;
        result.clamp = Clamp::Low;
    } else if (corrected > maximum) {
        result.value = std::numeric_limits<quint16>::max();
        result.clamp = Clamp::High;
    } else {
        result.value = quint16(corrected);
    }
    return result;
}

quint16 SensorOffsetStorage::correct(const Column &column, const QVariantMap &settings, quint16 raw,
                                     const char *where)
{
    const double offset = offsetOf(settings, column.key);
    const Result result = apply(raw, offset, column.scale);
    if (result.clamp != Clamp::None) {
        auto it = m_clampWarnings.find(column.serverRegister);
        if (it == m_clampWarnings.end())
            it = m_clampWarnings.insert(column.serverRegister, RepeatedWarningLimiter(m_options.clampWarningIntervalMs));
        qint64 heldBack = 0;
        if (it->allow(&heldBack)) {
            ++m_clampWarningsWritten;
            qWarning().noquote()
                    << QStringLiteral("[SensorOffset] %1 (s%2) raw=%3 + offset %4 %5 (%6 counts) is outside 0..65535; "
                                      "%7 clamped to %8%9")
                               .arg(column.key)
                               .arg(column.column)
                               .arg(raw)
                               .arg(offset, 0, 'f', 3)
                               .arg(column.unit)
                               .arg(result.offsetCounts, 0, 'f', 2)
                               .arg(QLatin1String(where))
                               .arg(result.value)
                               .arg(it->suffix(heldBack));
        }
    }
    return result.value;
}

quint16 SensorOffsetStorage::correctedServerRegister(int serverRegister, quint16 raw)
{
    const auto index = m_columnOfRegister.constFind(serverRegister);
    if (index == m_columnOfRegister.cend() || !m_proxy)
        return raw;
    return correct(m_columns.at(index.value()), m_proxy->sensorSettingsSv(), raw, "Modbus server PV");
}

QVector<double> SensorOffsetStorage::correctedSample(const QVector<quint16> &rawRegisters)
{
    QVector<double> readings;
    readings.reserve(rawRegisters.size());
    for (quint16 raw : rawRegisters)
        readings.append(double(raw));
    if (!m_proxy)
        return readings;

    const QVariantMap settings = m_proxy->sensorSettingsSv();   // the offsets of this moment
    QStringList changed;
    for (const Column &column : std::as_const(m_columns)) {
        if (column.serverRegister >= rawRegisters.size())
            continue;
        const quint16 raw = rawRegisters.at(column.serverRegister);
        const quint16 stored = correct(column, settings, raw, "sensor_data value");
        readings[column.serverRegister] = double(stored);
        if (offsetOf(settings, column.key) != 0.0) {
            changed << QStringLiteral("s%1 %2 raw=%3 stored=%4")
                               .arg(column.column)
                               .arg(column.key)
                               .arg(raw)
                               .arg(stored);
        }
    }
    if (!changed.isEmpty()) {
        qInfo().noquote() << QStringLiteral("[SensorOffset] sensor_data sample with offsets: %1")
                                     .arg(changed.join(QStringLiteral("; ")));
    }
    return readings;
}

void SensorOffsetStorage::logSettings()
{
    if (!m_proxy)
        return;
    const QVariantMap settings = m_proxy->sensorSettingsSv();
    QStringList parts;
    for (const Column &column : std::as_const(m_columns)) {
        const double offset = offsetOf(settings, column.key);
        if (offset != 0.0) {
            parts << QStringLiteral("%1 %2 %3 = %4 counts (s%5)")
                             .arg(column.key)
                             .arg(offset, 0, 'f', 3)
                             .arg(column.unit)
                             .arg(offset / column.scale, 0, 'f', 2)
                             .arg(column.column);
        }
    }
    const QString text = parts.isEmpty() ? QStringLiteral("none (raw counts are stored)")
                                         : parts.join(QStringLiteral(", "));
    if (text == m_lastLoggedOffsets)
        return;
    m_lastLoggedOffsets = text;
    qInfo().noquote() << QStringLiteral("[SensorOffset] offsets in use from the next sample: %1").arg(text);
}
