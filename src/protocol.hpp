// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <cstdint>
#include <optional>
#include <vector>

namespace kalm {

inline constexpr const char* kVivoGaiaUuid = "00000837-d102-11e1-9b23-00025b00a5a5";
inline constexpr const char* kGattServiceUuid = "00001100-d102-11e1-9b23-00025b00a5a5";
inline constexpr const char* kAudioSinkUuid = "0000110b-0000-1000-8000-00805f9b34fb";

inline constexpr uint8_t kPreamble = 0xFF;
inline constexpr uint8_t kFlagChecksum = 0x01;
inline constexpr uint8_t kFlagLengthExtension = 0x02;

inline constexpr uint16_t kGaiaVendor = 0x000A;
inline constexpr uint16_t kVivoVendor = 0x001B;

inline constexpr uint16_t kCmdHandshake = 0x0300;
inline constexpr uint16_t kCmdHandshakeResp = 0x8300;

inline constexpr uint16_t kRegisterNotifications[5] = {0x0202, 0x0203, 0x0204, 0x0205, 0x0206};

inline constexpr uint16_t kCmdSetWearDetection = 0x0103;
inline constexpr uint16_t kCmdQueryWearDetection = 0x0203;
inline constexpr uint16_t kCmdAckWearDetection = 0x8103;
inline constexpr uint16_t kCmdReportWearDetection = 0x8203;

inline constexpr uint16_t kCmdQueryBattery = 0x0207;
inline constexpr uint16_t kCmdReportBattery = 0x8207;

inline constexpr uint16_t kCmdQueryWearState = 0x020D;
inline constexpr uint16_t kCmdReportWearState = 0x820D;

inline constexpr uint16_t kCmdSetAudioEffect = 0x0118;
inline constexpr uint16_t kCmdQueryAudioEffect = 0x0218;
inline constexpr uint16_t kCmdAckAudioEffect = 0x8118;
inline constexpr uint16_t kCmdReportAudioEffect = 0x8218;

inline constexpr uint16_t kCmdSetFindDevice = 0x0120;
inline constexpr uint16_t kCmdAckFindDevice = 0x8120;

inline constexpr uint16_t kCmdSetNoiseMode = 0x0130;
inline constexpr uint16_t kCmdQueryNoiseMode = 0x0230;
inline constexpr uint16_t kCmdAckNoiseMode = 0x8130;
inline constexpr uint16_t kCmdReportNoiseMode = 0x8230;

inline constexpr uint16_t kCmdSetNoiseCycle = 0x0131;
inline constexpr uint16_t kCmdQueryNoiseCycle = 0x0231;
inline constexpr uint16_t kCmdAckNoiseCycle = 0x8131;
inline constexpr uint16_t kCmdReportNoiseCycle = 0x8231;
inline constexpr uint8_t kNoiseCycleSubcmd = 0x05;

inline constexpr uint16_t kCmdQueryMultiConnect = 0x0249;
inline constexpr uint16_t kCmdReportMultiConnect = 0x8249;
inline constexpr uint16_t kCmdSetMultiConnect = 0x014A;

inline constexpr uint16_t kCmdSetLowLatency = 0x0151;
inline constexpr uint16_t kCmdQueryLowLatency = 0x0251;
inline constexpr uint16_t kCmdAckLowLatency = 0x8151;
inline constexpr uint16_t kCmdReportLowLatency = 0x8251;

inline constexpr uint16_t kCmdReportTelemetry = 0x8224;
inline constexpr uint16_t kCmdPeerTimeRequest = 0x8509;
inline constexpr uint16_t kCmdHostTimeResponse = 0x0509;

enum class NoiseMode : uint8_t {
    Anc = 0,
    Off = 1,
    Transparency = 2,
};

QString noiseModeLabelRu(NoiseMode mode);
QString noiseModeSlug(NoiseMode mode);
std::optional<NoiseMode> parseNoiseModeStr(const QString& text);

struct PresetEntry {
    int code;
    const char* label;
};

const std::vector<PresetEntry>& audioEffectPresets();
QString audioEffectLabel(int code);

const std::vector<PresetEntry>& noiseCyclePresets();
QString noiseCycleLabel(int code);

struct WireProfile {
    QString label;
    uint8_t gaiaVersion;
    QByteArray noiseQueryPayload;
    QByteArray noiseSetSuffix;
};

WireProfile selectProfile(const QString& deviceName);

struct Frame {
    uint8_t version = 0;
    uint8_t flags = 0;
    uint16_t vendor = 0;
    uint16_t command = 0;
    QByteArray payload;
    QByteArray raw;
};

struct BatteryReading {
    std::optional<int> level;
    bool charging = false;
    bool stale = true;
};

struct MultiConnectDevice {
    QByteArray mac;   // 6 bytes
    QByteArray raw3;  // 3 bytes
    uint8_t status = 0;
    QString name;

    QString macStr() const;
    bool connected() const { return status != 0; }
    bool active() const { return status == 2; }
    QString statusRu() const;
};

struct EarbudState {
    bool connected = false;
    QString deviceName;
    QString deviceMac;
    QString firmware;

    std::optional<NoiseMode> noiseMode;
    std::optional<int> noiseEffect;
    std::optional<int> transparencyEffect;

    BatteryReading leftBattery;
    BatteryReading rightBattery;
    BatteryReading caseBattery;

    QString leftWear;   // "worn" | "in_case" | "removed" | ""
    QString rightWear;
    std::optional<bool> wearDetection;

    std::optional<int> audioEffect;
    std::optional<bool> lowLatency;
    std::optional<int> noiseCycleLeft;
    std::optional<int> noiseCycleRight;

    QList<MultiConnectDevice> pairedDevices;

    QJsonObject toJson() const;
};

QByteArray encodeFrame(uint8_t version, uint16_t vendor, uint16_t command,
                       const QByteArray& payload = {}, uint8_t flags = 0);

class FrameDecoder {
public:
    void reset() { m_buf.clear(); }
    QList<Frame> feed(const QByteArray& chunk);

private:
    QByteArray m_buf;
};

// Packet builders
QByteArray buildHandshake();
QList<QByteArray> buildRegisterNotifications();
QByteArray buildQueryBattery();
QByteArray buildQueryNoiseMode(const WireProfile& profile);
QByteArray buildSetNoiseMode(NoiseMode mode, const WireProfile& profile);
QByteArray buildQueryWearState(const WireProfile& profile);
QByteArray buildQueryWearDetection(const WireProfile& profile);
QByteArray buildSetWearDetection(bool enabled, const WireProfile& profile);
QByteArray buildQueryAudioEffect(const WireProfile& profile);
QByteArray buildSetAudioEffect(int effectId, const WireProfile& profile);
QByteArray buildQueryLowLatency(const WireProfile& profile);
QByteArray buildSetLowLatency(bool enabled, const WireProfile& profile);
QByteArray buildQueryNoiseCycle(const WireProfile& profile);
QByteArray buildSetNoiseCycle(int leftCode, int rightCode, const WireProfile& profile);
QByteArray buildQueryMultiConnect(const WireProfile& profile);
QByteArray buildSetMultiConnect(const QList<MultiConnectDevice>& devices,
                                const QString& targetMac, bool connect,
                                const WireProfile& profile);
QByteArray buildFindDevice(bool ring, const WireProfile& profile);
QByteArray buildHostTimeResponse(const WireProfile& profile,
                                 const QDateTime& dt = QDateTime::currentDateTime());

QString applyFrameToState(const Frame& frame, EarbudState& state);

}  // namespace kalm
