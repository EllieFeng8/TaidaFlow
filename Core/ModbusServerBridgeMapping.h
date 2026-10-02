#pragma once

#include <QtTypes>

// External Modbus Server offsets are zero-based. This is intentionally kept
// separate from ModbusMapping.h, which describes HMI process points.
namespace ModbusServerBridgeMapping {

constexpr int ServerDoStart = 0;
constexpr int ServerDoCount = 5;
constexpr int Adam6256DoStart = 16; // ADAM-6256 manual coil 00017 = DO0.

constexpr int ServerDiStart = 10;
constexpr int ServerDiCount = 3;
constexpr int Adam6224DiStart = 0;

constexpr int ServerAdam6217AInputStart = 0;
constexpr int ServerAdam6217BInputStart = 8;
constexpr int Adam6217AiCount = 8;
constexpr int ServerInputRegisterCount = 20;

constexpr int ServerAoStart = 0;
constexpr int ServerAoCount = 4;
constexpr int ServerPumpSpeedHoldingRegister = 4;
constexpr int ServerPumpSpeedHoldingRegisterCount = 1;
constexpr int Adam6022Ao0HoldingRegister = 10; // ADAM-6022 manual 40011.
constexpr quint16 Adam6224AoMaximumRawValue = 4095;

// SettingsPage values are read-only to external Modbus clients. HR11..40 are
// signed fixed-point values (the actual value is int16 / 10); HR41..50 remain
// reserved for future settings.
constexpr int ServerSettingsHoldingStart = 11;
constexpr int ServerSettingsNumericValueCount = 30;
constexpr int ServerSettingsHoldingEnd = 40;
constexpr int ServerSettingsFixedPointScale = 10;

inline bool isContainedRange(int start, qsizetype count, int rangeStart, int rangeCount)
{
    return start >= rangeStart
            && count > 0
            && static_cast<qint64>(start) + count <= rangeStart + rangeCount;
}

} // namespace ModbusServerBridgeMapping
