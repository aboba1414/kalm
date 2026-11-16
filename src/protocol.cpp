// SPDX-License-Identifier: GPL-3.0-or-later
#include "protocol.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace kalm {

namespace {

constexpr int kCommandBytes = 4;
constexpr int kMaxFrameBytes = 65544;

constexpr uint8_t kWearLeftInCase = 0x01;
constexpr uint8_t kWearRightInCase = 0x02;
constexpr uint8_t kWearLeftWorn = 0x04;
constexpr uint8_t kWearRightWorn = 0x08;

uint8_t xorChecksum(const QByteArray& buf, qsizetype count) {
    uint8_t acc = 0;
    for (qsizetype i = 0; i < count; ++i) {
        acc ^= static_cast<uint8_t>(buf[i]);
    }
    return acc;
}

void updateBatterySlot(BatteryReading& slot, uint8_t rawPct, bool charging) {
    if (rawPct <= 100) {
        slot.level = static_cast<int>(rawPct);
        slot.charging = charging;
        slot.stale = false;
    } else {
        slot.charging = false;
        slot.stale = true;
    }
}

QString decodeEarWear(uint8_t flags, uint8_t inCaseBit, uint8_t wornBit) {
    if (flags & wornBit) return QStringLiteral("worn");
    if (flags & inCaseBit) return QStringLiteral("in_case");
    return QStringLiteral("removed");
}

std::optional<QList<MultiConnectDevice>> parseMultiConnectDevices(const QByteArray& payload) {
    if (payload.size() < 2 || static_cast<uint8_t>(payload[0]) != 0) {
        return std::nullopt;
    }
    const int count = static_cast<uint8_t>(payload[1]);
    int offset = 2;
    QList<MultiConnectDevice> out;
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        if (offset + 11 > payload.size()) return std::nullopt;
        MultiConnectDevice dev;
        dev.mac = payload.mid(offset, 6);
        dev.raw3 = payload.mid(offset + 6, 3);
        dev.status = static_cast<uint8_t>(payload[offset + 9]);
        const int nameLen = static_cast<uint8_t>(payload[offset + 10]);
        if (offset + 11 + nameLen > payload.size()) return std::nullopt;
        dev.name = QString::fromUtf8(payload.mid(offset + 11, nameLen));
        out.append(dev);
        offset += 11 + nameLen;
    }
    return out;
}

std::optional<QJsonObject> parseTelemetryPayload(const QByteArray& payload) {
    const qsizetype start = payload.indexOf('{');
    const qsizetype end = payload.lastIndexOf('}');
    if (start < 0 || end <= start) return std::nullopt;
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(payload.mid(start, end - start + 1), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
    return doc.object();
}

}  // namespace

QString noiseModeLabelRu(NoiseMode mode) {
    switch (mode) {
        case NoiseMode::Anc: return QStringLiteral("Шумоподавление");
        case NoiseMode::Off: return QStringLiteral("Выключено");
        case NoiseMode::Transparency: return QStringLiteral("Прозрачность");
    }
    return QStringLiteral("—");
}

QString noiseModeSlug(NoiseMode mode) {
    switch (mode) {
        case NoiseMode::Anc: return QStringLiteral("anc");
        case NoiseMode::Off: return QStringLiteral("off");
        case NoiseMode::Transparency: return QStringLiteral("transparency");
    }
    return QStringLiteral("off");
}

std::optional<NoiseMode> parseNoiseModeStr(const QString& text) {
    const QString k = text.trimmed().toLower();
    if (k == u"anc" || k == u"nc" || k == u"on" || k == u"noise" || k == u"0" ||
        k == u"шумодав" || k == u"шумоподавление") {
        return NoiseMode::Anc;
    }
    if (k == u"off" || k == u"normal" || k == u"1" || k == u"выкл" || k == u"выключено") {
        return NoiseMode::Off;
    }
    if (k == u"trans" || k == u"transparency" || k == u"transparent" || k == u"ambient" ||
        k == u"2" || k == u"прозрачность") {
        return NoiseMode::Transparency;
    }
    return std::nullopt;
}

const std::vector<PresetEntry>& audioEffectPresets() {
    static const std::vector<PresetEntry> kList = {
        {0x00, "Стандартный"},
        {0x01, "Чистый вокал"},
        {0x02, "Мегабас"},
        {0x03, "Чистые высокие"},
        {0x05, "Аудиокниги"},
        {0x0B, "Мастер звука (Hi-Fi)"},
    };
    return kList;
}

QString audioEffectLabel(int code) {
    for (const auto& p : audioEffectPresets()) {
        if (p.code == code) return QString::fromUtf8(p.label);
    }
    return {};
}

const std::vector<PresetEntry>& noiseCyclePresets() {
    static const std::vector<PresetEntry> kList = {
        {0x0B, "Шумодав + Прозрачность + Выкл"},
        {0x0A, "Шумодав + Прозрачность"},
        {0x08, "Шумодав + Выкл"},
        {0x09, "Прозрачность + Выкл"},
    };
    return kList;
}

QString noiseCycleLabel(int code) {
    for (const auto& p : noiseCyclePresets()) {
        if (p.code == code) return QString::fromUtf8(p.label);
    }
    return {};
}

WireProfile selectProfile(const QString& deviceName) {
    QString norm;
    for (QChar ch : deviceName) {
        if (ch.isLetterOrNumber()) norm.append(ch.toLower());
    }
    if (norm.contains(u"air3pro")) {
        return {QStringLiteral("vivo TWS Air3 Pro (GAIA v3)"), 3, {}, QByteArray("\x04\x00", 2)};
    }
    if (norm.contains(u"tws3e")) {
        return {QStringLiteral("vivo TWS 3e (GAIA v3)"), 3, {}, QByteArray("\x03", 1)};
    }
    return {QStringLiteral("vivo TWS 4/5 Family (GAIA v4)"), 4, QByteArray("\x00", 1), QByteArray("\x03\x01", 2)};
}

QString MultiConnectDevice::macStr() const {
    QStringList parts;
    for (char b : mac) {
        parts.append(QStringLiteral("%1").arg(static_cast<uint8_t>(b), 2, 16, QLatin1Char('0')).toUpper());
    }
    return parts.join(u':');
}

QString MultiConnectDevice::statusRu() const {
    if (status == 2) return QStringLiteral("Подключено (активно)");
    if (status == 1) return QStringLiteral("Подключено");
    return QStringLiteral("Отключено");
}

QJsonObject EarbudState::toJson() const {
    auto optInt = [](const std::optional<int>& v) -> QJsonValue {
        return v.has_value() ? QJsonValue(*v) : QJsonValue(QJsonValue::Null);
    };
    auto optBool = [](const std::optional<bool>& v) -> QJsonValue {
        return v.has_value() ? QJsonValue(*v) : QJsonValue(QJsonValue::Null);
    };

    QJsonObject batt{
        {QStringLiteral("left"), optInt(leftBattery.level)},
        {QStringLiteral("left_charging"), leftBattery.charging},
        {QStringLiteral("left_stale"), leftBattery.stale},
        {QStringLiteral("right"), optInt(rightBattery.level)},
        {QStringLiteral("right_charging"), rightBattery.charging},
        {QStringLiteral("right_stale"), rightBattery.stale},
        {QStringLiteral("case"), optInt(caseBattery.level)},
        {QStringLiteral("case_charging"), caseBattery.charging},
        {QStringLiteral("case_stale"), caseBattery.stale},
    };

    QJsonObject wear{
        {QStringLiteral("left"), leftWear.isEmpty() ? QJsonValue() : QJsonValue(leftWear)},
        {QStringLiteral("right"), rightWear.isEmpty() ? QJsonValue() : QJsonValue(rightWear)},
        {QStringLiteral("auto_pause"), optBool(wearDetection)},
    };

    QJsonObject nc{
        {QStringLiteral("left"), optInt(noiseCycleLeft)},
        {QStringLiteral("right"), optInt(noiseCycleRight)},
        {QStringLiteral("label"), noiseCycleLeft ? QJsonValue(noiseCycleLabel(*noiseCycleLeft)) : QJsonValue()},
    };

    QJsonArray devs;
    for (const auto& d : pairedDevices) {
        devs.append(QJsonObject{
            {QStringLiteral("mac"), d.macStr()},
            {QStringLiteral("name"), d.name},
            {QStringLiteral("status"), static_cast<int>(d.status)},
            {QStringLiteral("status_label"), d.statusRu()},
            {QStringLiteral("connected"), d.connected()},
            {QStringLiteral("active"), d.active()},
        });
    }

    return QJsonObject{
        {QStringLiteral("connected"), connected},
        {QStringLiteral("device_name"), deviceName},
        {QStringLiteral("device_mac"), deviceMac},
        {QStringLiteral("firmware"), firmware.isEmpty() ? QJsonValue() : QJsonValue(firmware)},
        {QStringLiteral("noise_mode"), noiseMode ? QJsonValue(noiseModeSlug(*noiseMode)) : QJsonValue()},
        {QStringLiteral("noise_mode_label"), noiseMode ? QJsonValue(noiseModeLabelRu(*noiseMode)) : QJsonValue()},
        {QStringLiteral("battery"), batt},
        {QStringLiteral("wear"), wear},
        {QStringLiteral("audio_effect"), optInt(audioEffect)},
        {QStringLiteral("audio_effect_label"), audioEffect ? QJsonValue(audioEffectLabel(*audioEffect)) : QJsonValue()},
        {QStringLiteral("low_latency"), optBool(lowLatency)},
        {QStringLiteral("noise_cycle"), nc},
        {QStringLiteral("paired_devices"), devs},
    };
}

QByteArray encodeFrame(uint8_t version, uint16_t vendor, uint16_t command,
                       const QByteArray& payload, uint8_t flags) {
    const bool extended = (flags & kFlagLengthExtension) || (payload.size() > 254);
    if (extended) flags |= kFlagLengthExtension;
    const int headerSize = extended ? 5 : 4;
    QByteArray out(headerSize + kCommandBytes + payload.size(), Qt::Uninitialized);
    out[0] = static_cast<char>(kPreamble);
    out[1] = static_cast<char>(version);
    out[2] = static_cast<char>(flags);
    if (extended) {
        out[3] = static_cast<char>((payload.size() >> 8) & 0xFF);
        out[4] = static_cast<char>(payload.size() & 0xFF);
    } else {
        out[3] = static_cast<char>(payload.size() & 0xFF);
    }
    out[headerSize] = static_cast<char>((vendor >> 8) & 0xFF);
    out[headerSize + 1] = static_cast<char>(vendor & 0xFF);
    out[headerSize + 2] = static_cast<char>((command >> 8) & 0xFF);
    out[headerSize + 3] = static_cast<char>(command & 0xFF);
    if (!payload.isEmpty()) {
        memcpy(out.data() + headerSize + kCommandBytes, payload.constData(), static_cast<size_t>(payload.size()));
    }
    if (flags & kFlagChecksum) {
        out.append(static_cast<char>(xorChecksum(out, out.size())));
    }
    return out;
}

QList<Frame> FrameDecoder::feed(const QByteArray& chunk) {
    if (!chunk.isEmpty()) m_buf.append(chunk);
    QList<Frame> frames;
    while (true) {
        const qsizetype idx = m_buf.indexOf(static_cast<char>(kPreamble));
        if (idx < 0) {
            m_buf.clear();
            break;
        }
        if (idx > 0) m_buf.remove(0, idx);
        if (m_buf.size() < 4) break;

        const uint8_t ver = static_cast<uint8_t>(m_buf[1]);
        const uint8_t flags = static_cast<uint8_t>(m_buf[2]);
        if ((ver != 3 && ver != 4) || (flags & ~(kFlagChecksum | kFlagLengthExtension)) != 0) {
            m_buf.remove(0, 1);
            continue;
        }

        const bool extended = (flags & kFlagLengthExtension) != 0;
        const int headerSize = extended ? 5 : 4;
        if (m_buf.size() < headerSize + kCommandBytes) break;

        const uint16_t vendor = (static_cast<uint16_t>(static_cast<uint8_t>(m_buf[headerSize])) << 8) |
                                static_cast<uint8_t>(m_buf[headerSize + 1]);
        if (vendor != kGaiaVendor && vendor != kVivoVendor) {
            m_buf.remove(0, 1);
            continue;
        }

        const int payloadLen = extended
                                   ? ((static_cast<int>(static_cast<uint8_t>(m_buf[3])) << 8) |
                                      static_cast<uint8_t>(m_buf[4]))
                                   : static_cast<int>(static_cast<uint8_t>(m_buf[3]));
        const int checksumLen = (flags & kFlagChecksum) ? 1 : 0;
        const int totalLen = headerSize + kCommandBytes + payloadLen + checksumLen;
        if (totalLen > kMaxFrameBytes) {
            m_buf.remove(0, 1);
            continue;
        }
        if (m_buf.size() < totalLen) break;

        const QByteArray raw = m_buf.left(totalLen);
        m_buf.remove(0, totalLen);

        if (checksumLen && xorChecksum(raw, raw.size() - 1) != static_cast<uint8_t>(raw.back())) {
            continue;
        }

        const uint16_t command = (static_cast<uint16_t>(static_cast<uint8_t>(raw[headerSize + 2])) << 8) |
                                 static_cast<uint8_t>(raw[headerSize + 3]);
        Frame f;
        f.version = ver;
        f.flags = flags;
        f.vendor = vendor;
        f.command = command;
        f.payload = raw.mid(headerSize + kCommandBytes, payloadLen);
        f.raw = raw;
        frames.append(std::move(f));
    }
    return frames;
}

QByteArray buildHandshake() { return encodeFrame(4, kGaiaVendor, kCmdHandshake); }

QList<QByteArray> buildRegisterNotifications() {
    QList<QByteArray> list;
    for (uint16_t cmd : kRegisterNotifications) {
        list.append(encodeFrame(4, kVivoVendor, cmd));
    }
    return list;
}

QByteArray buildQueryBattery() { return encodeFrame(4, kVivoVendor, kCmdQueryBattery); }

QByteArray buildQueryNoiseMode(const WireProfile& profile) {
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdQueryNoiseMode, profile.noiseQueryPayload);
}

QByteArray buildSetNoiseMode(NoiseMode mode, const WireProfile& profile) {
    QByteArray p;
    p.append(static_cast<char>(static_cast<uint8_t>(mode)));
    p.append(profile.noiseSetSuffix);
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdSetNoiseMode, p);
}

QByteArray buildQueryWearState(const WireProfile& profile) {
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdQueryWearState);
}

QByteArray buildQueryWearDetection(const WireProfile& profile) {
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdQueryWearDetection);
}

QByteArray buildSetWearDetection(bool enabled, const WireProfile& profile) {
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdSetWearDetection, QByteArray(1, enabled ? '\x01' : '\x00'));
}

QByteArray buildQueryAudioEffect(const WireProfile& profile) {
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdQueryAudioEffect);
}

QByteArray buildSetAudioEffect(int effectId, const WireProfile& profile) {
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdSetAudioEffect, QByteArray(1, static_cast<char>(effectId & 0xFF)));
}

QByteArray buildQueryLowLatency(const WireProfile& profile) {
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdQueryLowLatency);
}

QByteArray buildSetLowLatency(bool enabled, const WireProfile& profile) {
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdSetLowLatency, QByteArray(1, enabled ? '\x01' : '\x00'));
}

QByteArray buildQueryNoiseCycle(const WireProfile& profile) {
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdQueryNoiseCycle, QByteArray(1, static_cast<char>(kNoiseCycleSubcmd)));
}

QByteArray buildSetNoiseCycle(int leftCode, int rightCode, const WireProfile& profile) {
    QByteArray p(3, '\x00');
    p[0] = static_cast<char>(kNoiseCycleSubcmd);
    p[1] = static_cast<char>(leftCode & 0xFF);
    p[2] = static_cast<char>(rightCode & 0xFF);
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdSetNoiseCycle, p);
}

QByteArray buildQueryMultiConnect(const WireProfile& profile) {
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdQueryMultiConnect);
}

QByteArray buildSetMultiConnect(const QList<MultiConnectDevice>& devices,
                                const QString& targetMac, bool connect,
                                const WireProfile& profile) {
    const QString normTarget = targetMac.trimmed().toUpper();
    QByteArray payload;
    for (const auto& d : devices) {
        payload.append(d.mac.left(6));
        if (d.macStr().toUpper() == normTarget) {
            payload.append(static_cast<char>(connect ? 0x02 : 0x00));
        } else {
            payload.append(static_cast<char>(d.status));
        }
    }
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdSetMultiConnect, payload);
}

QByteArray buildFindDevice(bool ring, const WireProfile& profile) {
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdSetFindDevice, QByteArray(1, ring ? '\x01' : '\x00'));
}

QByteArray buildHostTimeResponse(const WireProfile& profile, const QDateTime& dt) {
    const QDate d = dt.date();
    const QTime t = dt.time();
    QByteArray p(7, '\x00');
    p[0] = static_cast<char>((d.year() / 100) & 0xFF);
    p[1] = static_cast<char>((d.year() % 100) & 0xFF);
    p[2] = static_cast<char>(d.month() & 0xFF);
    p[3] = static_cast<char>(d.day() & 0xFF);
    p[4] = static_cast<char>(t.hour() & 0xFF);
    p[5] = static_cast<char>(t.minute() & 0xFF);
    p[6] = static_cast<char>(t.second() & 0xFF);
    return encodeFrame(profile.gaiaVersion, kVivoVendor, kCmdHostTimeResponse, p);
}

QString applyFrameToState(const Frame& frame, EarbudState& state) {
    const QByteArray& p = frame.payload;
    if (frame.vendor == kGaiaVendor && frame.command == kCmdHandshakeResp) {
        return QStringLiteral("handshake");
    }
    if (frame.vendor != kVivoVendor) return QStringLiteral("unknown");

    const uint16_t cmd = frame.command;
    if (cmd == kCmdAckNoiseMode || cmd == kCmdReportNoiseMode) {
        if (p.size() >= 2 && static_cast<uint8_t>(p[0]) == 0) {
            const uint8_t m = static_cast<uint8_t>(p[1]);
            if (m <= 2) {
                state.noiseMode = static_cast<NoiseMode>(m);
                if (p.size() > 2) state.noiseEffect = static_cast<uint8_t>(p[2]);
                if (p.size() > 3) state.transparencyEffect = static_cast<uint8_t>(p[3]);
                return QStringLiteral("noise_mode");
            }
        }
        return QStringLiteral("ignored");
    }

    if (cmd == kCmdReportBattery) {
        if (p.size() >= 5 && static_cast<uint8_t>(p[0]) == 0) {
            const uint8_t chg = static_cast<uint8_t>(p[4]);
            updateBatterySlot(state.leftBattery, static_cast<uint8_t>(p[1]), (chg & 0x01) != 0);
            updateBatterySlot(state.rightBattery, static_cast<uint8_t>(p[2]), (chg & 0x02) != 0);
            updateBatterySlot(state.caseBattery, static_cast<uint8_t>(p[3]), (chg & 0x04) != 0);
            return QStringLiteral("battery");
        }
        return QStringLiteral("ignored");
    }

    if (cmd == kCmdReportWearState) {
        if (p.size() >= 2 && static_cast<uint8_t>(p[0]) == 0) {
            const uint8_t flags = static_cast<uint8_t>(p[1]);
            state.leftWear = decodeEarWear(flags, kWearLeftInCase, kWearLeftWorn);
            state.rightWear = decodeEarWear(flags, kWearRightInCase, kWearRightWorn);
            return QStringLiteral("wear_state");
        }
        return QStringLiteral("ignored");
    }

    if (cmd == kCmdAckWearDetection || cmd == kCmdReportWearDetection) {
        if (p.size() >= 2 && static_cast<uint8_t>(p[0]) == 0) {
            state.wearDetection = (static_cast<uint8_t>(p[1]) != 0);
            return QStringLiteral("wear_detection");
        }
        return QStringLiteral("ignored");
    }

    if (cmd == kCmdAckAudioEffect || cmd == kCmdReportAudioEffect) {
        if (p.size() >= 2 && static_cast<uint8_t>(p[0]) == 0) {
            state.audioEffect = static_cast<int>(static_cast<uint8_t>(p[1]));
            return QStringLiteral("audio_effect");
        }
        return QStringLiteral("ignored");
    }

    if (cmd == kCmdAckLowLatency || cmd == kCmdReportLowLatency) {
        if (p.size() >= 2 && static_cast<uint8_t>(p[0]) == 0 &&
            (static_cast<uint8_t>(p[1]) == 0 || static_cast<uint8_t>(p[1]) == 1)) {
            state.lowLatency = (static_cast<uint8_t>(p[1]) == 1);
            return QStringLiteral("low_latency");
        }
        return QStringLiteral("ignored");
    }

    if (cmd == kCmdAckNoiseCycle || cmd == kCmdReportNoiseCycle) {
        if (p.size() >= 4 && static_cast<uint8_t>(p[0]) == 0 &&
            static_cast<uint8_t>(p[1]) == kNoiseCycleSubcmd) {
            state.noiseCycleLeft = static_cast<int>(static_cast<uint8_t>(p[2]));
            state.noiseCycleRight = static_cast<int>(static_cast<uint8_t>(p[3]));
            return QStringLiteral("noise_cycle");
        }
        return QStringLiteral("ignored");
    }

    if (cmd == kCmdReportMultiConnect) {
        if (auto parsed = parseMultiConnectDevices(p)) {
            state.pairedDevices = std::move(*parsed);
            return QStringLiteral("multi_connect");
        }
        return QStringLiteral("ignored");
    }

    if (cmd == kCmdReportTelemetry) {
        if (auto obj = parseTelemetryPayload(p)) {
            const QString ver = obj->value(QStringLiteral("V")).toString();
            if (ver.contains(u'.')) {
                const QString clean = ver.section(u'_', 0, 0);
                if (clean != u"6.55.35") state.firmware = clean;
            }
            const QString batt = obj->value(QStringLiteral("C")).toString();
            if (batt.contains(u'_')) {
                bool okL = false, okR = false;
                const int lb = batt.section(u'_', 0, 0).toInt(&okL);
                const int rb = batt.section(u'_', 1, 1).toInt(&okR);
                if (okL && !state.leftBattery.level && lb > 0 && lb <= 100) {
                    state.leftBattery.level = lb;
                    state.leftBattery.stale = false;
                }
                if (okR && !state.rightBattery.level && rb > 0 && rb <= 100) {
                    state.rightBattery.level = rb;
                    state.rightBattery.stale = false;
                }
            }
            return QStringLiteral("telemetry");
        }
        return QStringLiteral("ignored");
    }

    if (cmd == kCmdPeerTimeRequest) return QStringLiteral("time_request");
    if (cmd == kCmdAckFindDevice) return QStringLiteral("find_device");
    return QStringLiteral("other");
}

}  // namespace kalm
