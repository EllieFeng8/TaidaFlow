#pragma once

#include "Modbus_Client.h"

#include <QList>
#include <QModbusDataUnit>

#include <limits>

// This file is the single source of truth for Modbus addresses. Addresses in
// this file are zero-based Modbus protocol offsets: the manual's 40001 and
// 00017 are represented by startAddress 0 and 16 respectively.
namespace ModbusMapping {

constexpr int UnassignedAddress = -1;

enum class ProcessPoint {
    Tt01, Tt02, Tt03, Tt04,
    Pt01, Pt02, Pt03, Pt04, Pt05, Pt06, Pt07,
    FlowMeter,
    Mv1Position, Mv2Position, Mv3Position, Mv4Position
};

enum class CommandPoint {
    M1, M2, M3, M4, Pump2Hz, MotorRunning
};

enum class ValueFormat {
    Unsigned16,
    Signed16
};

struct ReadBinding {
    ProcessPoint point;
    ModbusClient::Device device = ModbusClient::Device::Unassigned;
    QModbusDataUnit::RegisterType registerType = QModbusDataUnit::Invalid;
    int startAddress = UnassignedAddress;
    quint16 valueCount = 1;
    qsizetype valueOffset = 0;
    ValueFormat valueFormat = ValueFormat::Unsigned16;
    double scale = 1.0;
    double offset = 0.0;

    bool isConfigured() const
    {
        return device != ModbusClient::Device::Unassigned
                && registerType != QModbusDataUnit::Invalid
                && startAddress >= 0
                && valueCount > 0;
    }
};

struct WriteBinding {
    CommandPoint point;
    ModbusClient::Device device = ModbusClient::Device::Unassigned;
    QModbusDataUnit::RegisterType registerType = QModbusDataUnit::Invalid;
    int startAddress = UnassignedAddress;
    double scale = 1.0;
    double offset = 0.0;
    double minimumValue = std::numeric_limits<double>::lowest();
    double maximumValue = std::numeric_limits<double>::max();

    bool isConfigured() const
    {
        return device != ModbusClient::Device::Unassigned
                && (registerType == QModbusDataUnit::Coils
                    || registerType == QModbusDataUnit::HoldingRegisters)
                && startAddress >= 0
                && scale != 0.0;
    }
};

inline QList<ReadBinding> defaultReadBindings()
{
    // ADAM-6217 AI0..AI7 uses the ordinary 16-bit AI registers at
    // 40001..40008 (offsets 0..7). For a configured 4-20 mA input, raw
    // 0..65535 represents 4..20 mA.
    constexpr double kAdcFullScale = 65535.0;
    constexpr double kTemperatureScale = 100.0 / kAdcFullScale;
    constexpr double kPressureScale = 1000.0 / kAdcFullScale;
    // ADAM-6217 .203 AI4..AI7 are 0..10 V valve-position feedbacks.
    // Their 16-bit raw range maps directly to 0..100 % for HMI display.
    constexpr double kValvePositionScale = 100.0 / kAdcFullScale;

    return {
        {ProcessPoint::Tt01, ModbusClient::Device::Adam6217_202,
         QModbusDataUnit::HoldingRegisters, 0, 8, 0,
         ValueFormat::Unsigned16, kTemperatureScale, 0.0},
        {ProcessPoint::Tt02, ModbusClient::Device::Adam6217_202,
         QModbusDataUnit::HoldingRegisters, 0, 8, 1,
         ValueFormat::Unsigned16, kTemperatureScale, 0.0},
        {ProcessPoint::Tt03, ModbusClient::Device::Adam6217_202,
         QModbusDataUnit::HoldingRegisters, 0, 8, 2,
         ValueFormat::Unsigned16, kTemperatureScale, 0.0},
        {ProcessPoint::Tt04, ModbusClient::Device::Adam6217_202,
         QModbusDataUnit::HoldingRegisters, 0, 8, 3,
         ValueFormat::Unsigned16, kTemperatureScale, 0.0},
        {ProcessPoint::Pt01, ModbusClient::Device::Adam6217_202,
         QModbusDataUnit::HoldingRegisters, 0, 8, 4,
         ValueFormat::Unsigned16, kPressureScale, 0.0},
        {ProcessPoint::Pt02, ModbusClient::Device::Adam6217_202,
         QModbusDataUnit::HoldingRegisters, 0, 8, 5,
         ValueFormat::Unsigned16, kPressureScale, 0.0},
        {ProcessPoint::Pt03, ModbusClient::Device::Adam6217_202,
         QModbusDataUnit::HoldingRegisters, 0, 8, 6,
         ValueFormat::Unsigned16, kPressureScale, 0.0},
        {ProcessPoint::Pt04, ModbusClient::Device::Adam6217_202,
         QModbusDataUnit::HoldingRegisters, 0, 8, 7,
         ValueFormat::Unsigned16, kPressureScale, 0.0},
        {ProcessPoint::Pt05, ModbusClient::Device::Adam6217_203,
         QModbusDataUnit::HoldingRegisters, 0, 8, 0,
         ValueFormat::Unsigned16, kPressureScale, 0.0},
        {ProcessPoint::Pt06, ModbusClient::Device::Adam6217_203,
         QModbusDataUnit::HoldingRegisters, 0, 8, 1,
         ValueFormat::Unsigned16, kPressureScale, 0.0},
        {ProcessPoint::Pt07, ModbusClient::Device::Adam6217_203,
         QModbusDataUnit::HoldingRegisters, 0, 8, 2,
         ValueFormat::Unsigned16, kPressureScale, 0.0},
        // Flow-meter electrical range is not yet known; do not display mA as
        // a flow rate until the transmitter's engineering range is supplied.
        {ProcessPoint::FlowMeter},
        {ProcessPoint::Mv1Position, ModbusClient::Device::Adam6217_203,
         QModbusDataUnit::HoldingRegisters, 0, 8, 4,
         ValueFormat::Unsigned16, kValvePositionScale, 0.0},
        {ProcessPoint::Mv2Position, ModbusClient::Device::Adam6217_203,
         QModbusDataUnit::HoldingRegisters, 0, 8, 5,
         ValueFormat::Unsigned16, kValvePositionScale, 0.0},
        {ProcessPoint::Mv3Position, ModbusClient::Device::Adam6217_203,
         QModbusDataUnit::HoldingRegisters, 0, 8, 6,
         ValueFormat::Unsigned16, kValvePositionScale, 0.0},
        {ProcessPoint::Mv4Position, ModbusClient::Device::Adam6217_203,
         QModbusDataUnit::HoldingRegisters, 0, 8, 7,
         ValueFormat::Unsigned16, kValvePositionScale, 0.0},
    };
}

inline QList<WriteBinding> defaultWriteBindings()
{
    // ADAM-6224 AO0..AO3 is MV1..MV4. A 12-bit raw AO value of 0..4095
    // maps to the configured 0..10 V output, which is 0..100% valve opening.
    // ADAM-6256 DO0 starts the circulation pump.
    constexpr double kValveOpeningScale = 100.0 / 4095.0;
    return {
        {CommandPoint::M1, ModbusClient::Device::Adam6224_204,
         QModbusDataUnit::HoldingRegisters, 0, kValveOpeningScale, 0.0, 0.0, 100.0},
        {CommandPoint::M2, ModbusClient::Device::Adam6224_204,
         QModbusDataUnit::HoldingRegisters, 1, kValveOpeningScale, 0.0, 0.0, 100.0},
        {CommandPoint::M3, ModbusClient::Device::Adam6224_204,
         QModbusDataUnit::HoldingRegisters, 2, kValveOpeningScale, 0.0, 0.0, 100.0},
        {CommandPoint::M4, ModbusClient::Device::Adam6224_204,
         QModbusDataUnit::HoldingRegisters, 3, kValveOpeningScale, 0.0, 0.0, 100.0},
        {CommandPoint::Pump2Hz},
        {CommandPoint::MotorRunning, ModbusClient::Device::Adam6256_201,
         QModbusDataUnit::Coils, 16},
    };
}

} // namespace ModbusMapping
