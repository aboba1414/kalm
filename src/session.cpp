// SPDX-License-Identifier: GPL-3.0-or-later
#include "session.hpp"

#include "i18n.hpp"
#include "probe.hpp"
#include "transport.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QMutexLocker>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace kalm {

QString ipcSocketPath() {
    QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtimeDir.isEmpty()) {
        runtimeDir = QStringLiteral("/tmp/kalm-%1").arg(::getuid());
    }
    QDir().mkpath(runtimeDir);
    return runtimeDir + QStringLiteral("/kalm.sock");
}

std::optional<QJsonObject> ipcRequest(const QJsonObject& payload, int timeoutMs) {
    const QString path = ipcSocketPath();
    if (!QFile::exists(path)) return std::nullopt;

    const QByteArray pathUtf8 = path.toUtf8();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (static_cast<size_t>(pathUtf8.size()) >= sizeof(addr.sun_path)) return std::nullopt;
    std::memcpy(addr.sun_path, pathUtf8.constData(), static_cast<size_t>(pathUtf8.size()));

    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd) return std::nullopt;

    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        return std::nullopt;
    }

    const QByteArray reqBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (!sendAll(fd.get(), reqBytes)) {
        return std::nullopt;
    }
    ::shutdown(fd.get(), SHUT_WR);

    QByteArray respBytes;
    QElapsedTimer timer;
    timer.start();
    char buf[4096];
    while (timer.elapsed() < timeoutMs) {
        const int rem = std::max(50, static_cast<int>(timeoutMs - timer.elapsed()));
        pollfd pfd{fd.get(), POLLIN, 0};
        const int pr = ::poll(&pfd, 1, rem);
        if (pr <= 0) break;
        const ssize_t n = ::recv(fd.get(), buf, sizeof(buf), 0);
        if (n <= 0) break;
        respBytes.append(buf, n);
    }

    if (respBytes.isEmpty()) return std::nullopt;
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(respBytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
    return doc.object();
}

// ---- KalmSession ----

KalmSession::KalmSession(DeviceRegistry& registry,
                         const QString& overrideMac,
                         StateCallback onStateChanged,
                         DevicesCallback onDevicesChanged,
                         ErrorCallback onError)
    : m_registry(registry),
      m_overrideMac(overrideMac.trimmed().toUpper()),
      m_onStateChanged(std::move(onStateChanged)),
      m_onDevicesChanged(std::move(onDevicesChanged)),
      m_onError(std::move(onError)),
      m_profile(selectProfile({})) {}

KalmSession::~KalmSession() { stop(); }

EarbudState KalmSession::snapshot() const {
    QMutexLocker lock(&m_lock);
    return m_state;
}

SessionCapabilities KalmSession::capabilities() const {
    SessionCapabilities caps;
    QString name;
    {
        QMutexLocker lock(&m_lock);
        name = m_modelName;
    }
    if (name.isEmpty()) {
        if (auto active = m_registry.getActive()) name = active->name;
    }

    for (const auto& a : ancLevelPresets()) caps.ancLevelList.append(a);
    for (const auto& t : transparencyPresets()) caps.transparencyList.append(t);
    for (const auto& e : audioEffectPresets()) caps.eqList.append(e);
    for (const auto& c : noiseCyclePresets()) caps.cycleList.append(c);
    caps.profileLabel = QStringLiteral("vivo / iQOO GAIA");

    if (huawei::matchesName(name)) {
        const auto& profile = huawei::profileFor(name);
        caps.protocol = DeviceProtocol::Huawei;
        caps.profileLabel = profile.label;
        caps.ancLevelList = huawei::ancLevels(profile);
        caps.transparencyList = huawei::transparencyLevels(profile);
        caps.eqList = huawei::eqPresets(profile);
        caps.cycleList.clear();
        caps.ancLevels = profile.ancLevels;
        caps.transparency = !caps.transparencyList.isEmpty();
        caps.equalizer = profile.equalizer;
        caps.gameMode = profile.lowLatency;
        caps.wearDetection = profile.autoPause;
        caps.noiseCycle = false;
        caps.multipoint = profile.dualConnect;
        caps.findDevice = false;
        caps.probe = false;
    }
    return caps;
}

void KalmSession::applyProtocolName(const QString& modelName) {
    QMutexLocker lock(&m_lock);
    m_modelName = modelName;
    if (huawei::matchesName(modelName)) {
        m_protocol = DeviceProtocol::Huawei;
        m_hwProfile = huawei::profileFor(modelName);
        m_state.protocol = QStringLiteral("huawei");
    } else {
        m_protocol = DeviceProtocol::Gaia;
        m_state.protocol = QStringLiteral("gaia");
    }
}

void KalmSession::wakeWorker() {
    QMutexLocker lock(&m_wakeMtx);
    m_woken = true;
    m_wakeCv.wakeAll();
}

void KalmSession::start() {
    m_registry.ensureInitialImport();
    if (auto active = m_registry.getActive()) {
        QMutexLocker lock(&m_lock);
        m_state.deviceName = active->displayName();
        m_state.deviceMac = active->mac;
        m_modelName = active->name;
        if (huawei::matchesName(active->name)) {
            m_protocol = DeviceProtocol::Huawei;
            m_hwProfile = huawei::profileFor(active->name);
            m_state.protocol = QStringLiteral("huawei");
        } else {
            m_state.protocol = QStringLiteral("gaia");
        }
    }
    if (m_worker.joinable()) return;
    m_stop.store(false);
    m_worker = std::thread(&KalmSession::runLoop, this);
}

void KalmSession::stop() {
    m_stop.store(true);
    wakeWorker();
    closeSocket(false);
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

bool KalmSession::switchToDevice(const QString& mac) {
    if (!m_registry.setActive(mac)) return false;
    m_overrideMac.clear();
    const auto active = m_registry.getActive();
    closeSocket(true);
    if (active) {
        QMutexLocker lock(&m_lock);
        m_state.deviceName = active->displayName();
        m_state.deviceMac = active->mac;
    }
    emitDevices();
    emitState();
    wakeWorker();
    return true;
}

void KalmSession::notifyRegistryUpdated() {
    const auto active = m_registry.getActive();
    bool needDrop = false;
    {
        QMutexLocker lock(&m_lock);
        if (!active) {
            needDrop = !m_state.deviceMac.isEmpty();
        } else if (!m_state.deviceMac.isEmpty() && active->mac != m_state.deviceMac) {
            needDrop = true;
        } else {
            m_state.deviceName = active->displayName();
        }
    }
    if (needDrop) {
        closeSocket(true);
        if (active) {
            QMutexLocker lock(&m_lock);
            m_state.deviceName = active->displayName();
            m_state.deviceMac = active->mac;
        }
    }
    emitDevices();
    emitState();
    wakeWorker();
}

void KalmSession::forceReconnect() {
    closeSocket(false);
    wakeWorker();
}

void KalmSession::emitState() {
    if (m_onStateChanged) m_onStateChanged(snapshot());
}

void KalmSession::emitDevices() {
    if (m_onDevicesChanged) m_onDevicesChanged();
}

void KalmSession::emitError(const QString& msg) {
    if (m_onError) m_onError(msg);
}

void KalmSession::closeSocket(bool resetIdentity) {
    int fd = -1;
    bool wasConnected = false;
    {
        QMutexLocker lock(&m_lock);
        fd = m_sockFd;
        m_sockFd = -1;
        wasConnected = m_state.connected;
        m_hwDcRows.clear();
        m_hwDcCount = 0;
        if (resetIdentity) {
            m_state = EarbudState{};
        } else {
            m_state.connected = false;
            m_state.leftBattery.stale = true;
            m_state.rightBattery.stale = true;
            m_state.caseBattery.stale = true;
        }
    }
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    if (wasConnected || resetIdentity) {
        emitState();
    }
}

bool KalmSession::sendRaw(const QByteArray& data) {
    int fd = -1;
    {
        QMutexLocker lock(&m_lock);
        fd = m_sockFd;
    }
    if (fd < 0) return false;
    if (!sendAll(fd, data)) {
        closeSocket(false);
        return false;
    }
    return true;
}

std::optional<Frame> KalmSession::sendAndWait(const QByteArray& packet,
                                              std::function<bool(const Frame&)> pred,
                                              int timeoutMs) {
    auto waiter = std::make_shared<Waiter>();
    waiter->pred = std::move(pred);
    {
        QMutexLocker lock(&m_waitersMtx);
        m_waiters.append(waiter);
    }

    auto cleanup = [&]() {
        QMutexLocker lock(&m_waitersMtx);
        m_waiters.removeOne(waiter);
    };

    if (!sendRaw(packet)) {
        cleanup();
        return std::nullopt;
    }

    std::optional<Frame> out;
    {
        QMutexLocker lock(&waiter->mtx);
        if (!waiter->done) {
            waiter->cv.wait(&waiter->mtx, timeoutMs);
        }
        if (waiter->done) out = waiter->result;
    }
    cleanup();
    return out;
}

std::optional<huawei::Packet> KalmSession::sendAndWaitHuawei(
    const QList<QByteArray>& packets,
    std::function<bool(const huawei::Packet&)> pred,
    int timeoutMs) {
    auto waiter = std::make_shared<HuaweiWaiter>();
    waiter->pred = std::move(pred);
    {
        QMutexLocker lock(&m_hwWaitersMtx);
        m_hwWaiters.append(waiter);
    }

    auto cleanup = [&]() {
        QMutexLocker lock(&m_hwWaitersMtx);
        m_hwWaiters.removeOne(waiter);
    };

    if (!packets.isEmpty()) {
        bool sent = false;
        for (const QByteArray& packet : packets) {
            sent = sendRaw(packet) || sent;
        }
        if (!sent) {
            cleanup();
            return std::nullopt;
        }
    }

    std::optional<huawei::Packet> out;
    {
        QMutexLocker lock(&waiter->mtx);
        if (!waiter->done) {
            waiter->cv.wait(&waiter->mtx, timeoutMs);
        }
        if (waiter->done) out = waiter->result;
    }
    cleanup();
    return out;
}


bool KalmSession::huaweiApply(const QByteArray& setPacket,
                              const QByteArray& ackCommand,
                              std::function<bool(const huawei::Packet&)> isTarget,
                              const QByteArray& queryPacket,
                              int timeoutMs) {
    if (!sendRaw(setPacket)) return false;

    // Wait for the write acknowledgement first. Sending the follow-up query
    // immediately makes some firmwares answer with the *old* value (and the
    // stale reply then overwrites the fresh state), so pace the sequence.
    sendAndWaitHuawei({}, [&ackCommand](const huawei::Packet& packet) {
        return packet.command == ackCommand;
    }, 800);

    if (sendAndWaitHuawei({}, isTarget, 450)) return true;
    return sendAndWaitHuawei({queryPacket}, isTarget, timeoutMs).has_value();
}

bool KalmSession::refresh() {
    DeviceProtocol proto = DeviceProtocol::Gaia;
    WireProfile p;
    {
        QMutexLocker lock(&m_lock);
        proto = m_protocol;
        p = m_profile;
    }
    if (proto == DeviceProtocol::Huawei) {
        bool ok = false;
        for (const QByteArray& pkt : {
                 huawei::buildQueryBattery(),
                 huawei::buildQueryAnc(),
                 huawei::buildQueryEqualizer(),
                 huawei::buildQueryLowLatency(),
                 huawei::buildQueryAutoPause(),
                 huawei::buildQueryDualConnectEnabled(),
                 huawei::buildEnumerateDualConnect(),
             }) {
            ok = sendRaw(pkt) || ok;
        }
        return ok;
    }

    bool ok = false;
    for (const QByteArray& pkt : {
             buildQueryBattery(),
             buildQueryNoiseMode(p),
             buildQueryWearState(p),
             buildQueryWearDetection(p),
             buildQueryAudioEffect(p),
             buildQueryLowLatency(p),
             buildQueryNoiseCycle(p),
             buildQueryMultiConnect(p),
         }) {
        ok = sendRaw(pkt) || ok;
    }
    return ok;
}

bool KalmSession::setNoiseMode(NoiseMode mode,
                               std::optional<int> ancLevel,
                               std::optional<int> transEffect) {
    WireProfile p;
    std::optional<int> effAnc;
    std::optional<int> effTrans;
    {
        QMutexLocker lock(&m_lock);
        p = m_profile;
        effAnc = ancLevel.has_value() ? ancLevel : m_state.noiseEffect;
        effTrans = transEffect.has_value() ? transEffect : m_state.transparencyEffect;
    }

    DeviceProtocol proto = DeviceProtocol::Gaia;
    {
        QMutexLocker lock(&m_lock);
        proto = m_protocol;
    }
    if (proto == DeviceProtocol::Huawei) {
        const int target = huawei::toModeValue(mode);
        const auto isTarget = [target](const huawei::Packet& pkt) {
            return pkt.command == QByteArray::fromHex("2b2a") && pkt.param(1).size() >= 2 &&
                   static_cast<uint8_t>(pkt.param(1).at(1)) == target;
        };

        const auto current = snapshot().noiseMode;
        const bool sameMode = current.has_value() && *current == mode;

        // Only pass an explicit cancel/awareness level when the mode is not
        // being changed. Mode switches are sent with the 0xFF "keep current
        // level" marker, otherwise the firmware treats the write as a
        // level-only update and the audio path never switches (observed on
        // FreeBuds 6i: after "Off" ANC/transparency would not engage).
        std::optional<int> level;
        std::optional<int> requestedLevel;
        if (target == huawei::kModeAnc) {
            requestedLevel = ancLevel;
        } else if (target == huawei::kModeAwareness && transEffect.has_value()) {
            requestedLevel = 2;  // device level 2 = plain awareness
        }
        if (sameMode) level = requestedLevel;

        if (!huaweiApply(huawei::buildSetAnc(target, level),
                         QByteArray::fromHex("2b04"), isTarget, huawei::buildQueryAnc())) {
            return false;
        }
        if (!sameMode && requestedLevel.has_value()) {
            huaweiApply(huawei::buildSetAnc(target, requestedLevel),
                        QByteArray::fromHex("2b04"), isTarget, huawei::buildQueryAnc());
        }
        return true;
    }

    const auto resp = sendAndWait(
        buildSetNoiseMode(mode, p, effAnc, effTrans),
        [mode](const Frame& f) {
            return (f.command == kCmdAckNoiseMode || f.command == kCmdReportNoiseMode) &&
                   f.payload.size() >= 2 &&
                   static_cast<uint8_t>(f.payload[0]) == 0 &&
                   static_cast<uint8_t>(f.payload[1]) == static_cast<uint8_t>(mode);
        },
        2000);
    if (resp) return true;

    const auto q = sendAndWait(
        buildQueryNoiseMode(p),
        [](const Frame& f) { return f.command == kCmdReportNoiseMode; },
        1500);
    return q && q->payload.size() >= 2 &&
           static_cast<uint8_t>(q->payload[1]) == static_cast<uint8_t>(mode);
}

bool KalmSession::setAncLevel(int levelCode) {
    return setNoiseMode(NoiseMode::Anc, levelCode, std::nullopt);
}

bool KalmSession::setTransparencyEffect(int transCode) {
    return setNoiseMode(NoiseMode::Transparency, std::nullopt, transCode);
}

std::optional<NoiseMode> KalmSession::toggleNoiseMode(const QList<NoiseMode>& cycle) {
    const QList<NoiseMode> order =
        cycle.isEmpty() ? QList<NoiseMode>{NoiseMode::Anc, NoiseMode::Transparency, NoiseMode::Off}
                        : cycle;
    const auto cur = snapshot().noiseMode;
    NoiseMode nxt = order.first();
    if (cur) {
        const qsizetype idx = order.indexOf(*cur);
        if (idx >= 0) nxt = order[(idx + 1) % order.size()];
    }
    if (setNoiseMode(nxt)) return nxt;
    return std::nullopt;
}

bool KalmSession::setAudioEffect(int effectId) {
    WireProfile p;
    DeviceProtocol proto = DeviceProtocol::Gaia;
    {
        QMutexLocker lock(&m_lock);
        p = m_profile;
        proto = m_protocol;
    }
    if (proto == DeviceProtocol::Huawei) {
        return huaweiApply(
            huawei::buildSetEqualizerPreset(effectId),
            QByteArray::fromHex("2b49"),
            [effectId](const huawei::Packet& pkt) {
                return pkt.command == QByteArray::fromHex("2b4a") &&
                       huawei::eqCurrentPreset(pkt) == effectId;
            },
            huawei::buildQueryEqualizer());
    }
    return sendAndWait(
               buildSetAudioEffect(effectId, p),
               [](const Frame& f) {
                   return (f.command == kCmdAckAudioEffect || f.command == kCmdReportAudioEffect) &&
                          f.payload.size() >= 2 && static_cast<uint8_t>(f.payload[0]) == 0;
               },
               1800)
        .has_value();
}

bool KalmSession::setLowLatency(bool enabled) {
    WireProfile p;
    DeviceProtocol proto = DeviceProtocol::Gaia;
    {
        QMutexLocker lock(&m_lock);
        p = m_profile;
        proto = m_protocol;
    }
    if (proto == DeviceProtocol::Huawei) {
        return huaweiApply(
            huawei::buildSetLowLatency(enabled),
            QByteArray::fromHex("2b6c"),
            [enabled](const huawei::Packet& pkt) {
                return pkt.command == QByteArray::fromHex("2b6c") && pkt.param(2).size() == 1 &&
                       (pkt.paramByte(2) != 0) == enabled;
            },
            huawei::buildQueryLowLatency());
    }
    return sendAndWait(
               buildSetLowLatency(enabled, p),
               [](const Frame& f) {
                   return (f.command == kCmdAckLowLatency || f.command == kCmdReportLowLatency) &&
                          f.payload.size() >= 2 && static_cast<uint8_t>(f.payload[0]) == 0;
               },
               1800)
        .has_value();
}

bool KalmSession::setWearDetection(bool enabled) {
    WireProfile p;
    DeviceProtocol proto = DeviceProtocol::Gaia;
    {
        QMutexLocker lock(&m_lock);
        p = m_profile;
        proto = m_protocol;
    }
    if (proto == DeviceProtocol::Huawei) {
        return huaweiApply(
            huawei::buildSetAutoPause(enabled),
            QByteArray::fromHex("2b10"),
            [enabled](const huawei::Packet& pkt) {
                return pkt.command == QByteArray::fromHex("2b11") && pkt.param(1).size() == 1 &&
                       (pkt.paramByte(1) != 0) == enabled;
            },
            huawei::buildQueryAutoPause());
    }
    return sendAndWait(
               buildSetWearDetection(enabled, p),
               [](const Frame& f) {
                   return (f.command == kCmdAckWearDetection || f.command == kCmdReportWearDetection) &&
                          f.payload.size() >= 2 && static_cast<uint8_t>(f.payload[0]) == 0;
               },
               1800)
        .has_value();
}

bool KalmSession::setNoiseCycle(int cycleCode) {
    {
        QMutexLocker lock(&m_lock);
        if (m_protocol == DeviceProtocol::Huawei) return false;
    }
    WireProfile p;
    {
        QMutexLocker lock(&m_lock);
        p = m_profile;
    }
    return sendAndWait(
               buildSetNoiseCycle(cycleCode, cycleCode, p),
               [](const Frame& f) {
                   return (f.command == kCmdAckNoiseCycle || f.command == kCmdReportNoiseCycle) &&
                          f.payload.size() >= 4 && static_cast<uint8_t>(f.payload[0]) == 0;
               },
               1800)
        .has_value();
}

bool KalmSession::setDeviceConnection(const QString& targetMac, bool connect) {
    DeviceProtocol proto = DeviceProtocol::Gaia;
    {
        QMutexLocker lock(&m_lock);
        proto = m_protocol;
    }
    if (proto == DeviceProtocol::Huawei) {
        const QByteArray action = huawei::buildDualConnectAction(targetMac, connect);
        if (action.isEmpty()) return false;
        if (!sendRaw(action)) return false;
        ::usleep(400 * 1000);
        sendRaw(huawei::buildEnumerateDualConnect());
        return true;
    }
    const auto snap = snapshot();
    if (snap.pairedDevices.isEmpty()) return false;
    WireProfile p;
    {
        QMutexLocker lock(&m_lock);
        p = m_profile;
    }
    if (!sendRaw(buildSetMultiConnect(snap.pairedDevices, targetMac, connect, p))) return false;
    ::usleep(400 * 1000);
    sendRaw(buildQueryMultiConnect(p));
    return true;
}

bool KalmSession::findEarbuds(bool ring) {
    {
        QMutexLocker lock(&m_lock);
        if (m_protocol == DeviceProtocol::Huawei) return false;
    }
    WireProfile p;
    {
        QMutexLocker lock(&m_lock);
        p = m_profile;
    }
    return sendAndWait(
               buildFindDevice(ring, p),
               [](const Frame& f) { return f.command == kCmdAckFindDevice; },
               1800)
        .has_value();
}

QString KalmSession::runProbe(const QString& deviceQuery, int listenSeconds) {
    {
        QMutexLocker lock(&m_lock);
        if (m_protocol == DeviceProtocol::Huawei) return trKey("probe_huawei_unsupported");
    }
    m_paused.store(true);
    closeSocket(false);
    wakeWorker();
    ::usleep(300 * 1000);

    QString report;
    try {
        report = generateDiagnosticReport(m_registry, deviceQuery, listenSeconds);
    } catch (...) {
        m_paused.store(false);
        wakeWorker();
        throw;
    }

    m_paused.store(false);
    wakeWorker();
    return report;
}

void KalmSession::runLoop() {
    auto sleepInterruptible = [this](int ms) {
        QMutexLocker lock(&m_wakeMtx);
        if (!m_woken && !m_stop.load()) {
            m_wakeCv.wait(&m_wakeMtx, ms);
        }
        m_woken = false;
    };

    while (!m_stop.load()) {
        if (m_paused.load()) {
            sleepInterruptible(300);
            continue;
        }
        QMap<QString, BluezDevice> bluezMap;
        for (const auto& d : listBluezDevices(false, false)) {
            bluezMap.insert(d.mac, d);
        }

        std::optional<SavedEarbud> chosen;
        std::optional<BluezDevice> chosenBluez;

        if (!m_overrideMac.isEmpty()) {
            if (bluezMap.contains(m_overrideMac)) chosenBluez = bluezMap.value(m_overrideMac);
            chosen = m_registry.get(m_overrideMac)
                         .value_or(SavedEarbud{m_overrideMac, chosenBluez ? chosenBluez->name : m_overrideMac, {}});
        } else if (auto active = m_registry.getActive()) {
            if (bluezMap.contains(active->mac)) chosenBluez = bluezMap.value(active->mac);
            if (chosenBluez && chosenBluez->connected) {
                chosen = active;
            } else if (m_registry.autoSwitch()) {
                for (const auto& cand : m_registry.devices()) {
                    if (bluezMap.contains(cand.mac) && bluezMap.value(cand.mac).connected) {
                        m_registry.setActive(cand.mac);
                        emitDevices();
                        chosen = cand;
                        chosenBluez = bluezMap.value(cand.mac);
                        break;
                    }
                }
                if (!chosen) chosen = active;
            } else {
                chosen = active;
            }
        } else {
            for (const auto& bdev : bluezMap) {
                if (bdev.isSupportedTws() && bdev.connected) {
                    chosen = m_registry.add(bdev.mac, bdev.name, {}, true);
                    chosenBluez = bdev;
                    emitDevices();
                    break;
                }
            }
        }

        if (!chosen) {
            sleepInterruptible(3000);
            continue;
        }

        {
            QMutexLocker lock(&m_lock);
            m_state.deviceName = chosen->displayName();
            m_state.deviceMac = chosen->mac;
        }

        if (!chosenBluez || !chosenBluez->connected) {
            sleepInterruptible(2500);
            continue;
        }

        if (!chosenBluez->name.isEmpty() && chosenBluez->name != chosen->name) {
            m_registry.add(chosen->mac, chosenBluez->name, {}, false);
        }

        applyProtocolName(chosenBluez->name);
        const bool huaweiConn = huawei::matchesName(chosenBluez->name);

        RfcommConnection conn;
        try {
            conn = huaweiConn
                       ? openHuaweiRfcomm(chosen->mac, chosenBluez->path, chosen->displayName(), 2500)
                       : openGaiaRfcomm(chosen->mac, chosenBluez->path, chosen->displayName(), 2500);
        } catch (const std::exception& ex) {
            emitError(QString::fromUtf8(ex.what()));
            sleepInterruptible(3500);
            continue;
        }

        const WireProfile profile = selectProfile(chosenBluez->name);
        const int rawConnFd = conn.fd.release();
        {
            QMutexLocker lock(&m_lock);
            m_sockFd = rawConnFd;
            m_profile = profile;
            m_hwDcRows.clear();
            m_hwDcCount = 0;
            m_state.connected = true;
            m_state.deviceName = chosen->displayName();
            m_state.deviceMac = chosen->mac;
        }

        FrameDecoder decoder;
        huawei::Decoder hwDecoder;
        if (!conn.initialBytes.isEmpty()) {
            if (huaweiConn) {
                handleHuaweiPackets(hwDecoder.feed(conn.initialBytes));
            } else {
                handleFrames(decoder.feed(conn.initialBytes));
            }
        }

        if (huaweiConn) {
            for (const QByteArray& pkt : {
                     huawei::buildQueryBattery(),
                     huawei::buildQueryAnc(),
                     huawei::buildQueryEqualizer(),
                     huawei::buildQueryLowLatency(),
                     huawei::buildQueryInfo(),
                     huawei::buildQueryAutoPause(),
                     huawei::buildQueryDualConnectEnabled(),
                     huawei::buildEnumerateDualConnect(),
                 }) {
                sendRaw(pkt);
            }
        } else {
            for (const QByteArray& pkt : buildRegisterNotifications()) {
                sendRaw(pkt);
            }
            refresh();
        }
        emitDevices();
        emitState();

        QElapsedTimer pollTimer;
        pollTimer.start();
        char buf[2048];
        while (!m_stop.load()) {
            int curFd = -1;
            {
                QMutexLocker lock(&m_lock);
                curFd = m_sockFd;
            }
            if (curFd != rawConnFd) break;

            pollfd pfd{rawConnFd, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, 1000);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr > 0) {
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
                if (pfd.revents & POLLIN) {
                    const ssize_t n = ::recv(rawConnFd, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    if (huaweiConn) {
                        const auto packets = hwDecoder.feed(QByteArray(buf, n));
                        if (!packets.isEmpty()) handleHuaweiPackets(packets);
                    } else {
                        const auto frames = decoder.feed(QByteArray(buf, n));
                        if (!frames.isEmpty()) handleFrames(frames);
                    }
                }
            }

            if (pollTimer.elapsed() >= 30000) {
                pollTimer.restart();
                if (huaweiConn) {
                    refresh();
                } else {
                    sendRaw(buildQueryBattery());
                    sendRaw(buildQueryWearState(profile));
                }
            }
        }

        closeSocket(false);
        emitDevices();
        if (!m_stop.load()) {
            sleepInterruptible(2000);
        }
    }
}

void KalmSession::handleFrames(const QList<Frame>& frames) {
    bool changed = false;
    bool replyTime = false;
    WireProfile prof;
    {
        QMutexLocker lock(&m_lock);
        prof = m_profile;
        for (const Frame& f : frames) {
            const QString tag = applyFrameToState(f, m_state);
            if (tag == u"time_request") {
                replyTime = true;
            } else if (tag != u"ignored" && tag != u"unknown" && tag != u"handshake") {
                changed = true;
            }
        }
    }

    QList<std::shared_ptr<Waiter>> activeWaiters;
    {
        QMutexLocker lock(&m_waitersMtx);
        activeWaiters = m_waiters;
    }
    for (const Frame& f : frames) {
        for (const auto& w : activeWaiters) {
            QMutexLocker wl(&w->mtx);
            if (!w->done && w->pred(f)) {
                w->result = f;
                w->done = true;
                w->cv.wakeAll();
            }
        }
    }

    if (replyTime) {
        sendRaw(buildHostTimeResponse(prof));
    }
    if (changed) {
        emitState();
    }
}

void KalmSession::handleHuaweiPackets(const QList<huawei::Packet>& packets) {
    bool changed = false;
    {
        QMutexLocker lock(&m_lock);
        for (const auto& packet : packets) {
            const auto effect = huawei::applyPacket(packet, m_state, m_hwProfile);
            if (effect.stateChanged) changed = true;
            if (!effect.dualConnectRow) continue;

            if (effect.dcCount > 0) m_hwDcCount = effect.dcCount;
            m_hwDcRows.insert(effect.dcIndex, effect.dcDevice);
            if (m_hwDcCount > 0 && m_hwDcRows.size() >= m_hwDcCount) {
                QList<MultiConnectDevice> list;
                for (auto it = m_hwDcRows.cbegin(); it != m_hwDcRows.cend(); ++it) {
                    list.append(it.value());
                }
                m_state.pairedDevices = list;
                m_hwDcRows.clear();
                changed = true;
            }
        }
    }

    QList<std::shared_ptr<HuaweiWaiter>> activeWaiters;
    {
        QMutexLocker lock(&m_hwWaitersMtx);
        activeWaiters = m_hwWaiters;
    }
    for (const auto& packet : packets) {
        for (const auto& waiter : activeWaiters) {
            QMutexLocker wl(&waiter->mtx);
            if (!waiter->done && waiter->pred(packet)) {
                waiter->result = packet;
                waiter->done = true;
                waiter->cv.wakeAll();
            }
        }
    }

    if (changed) emitState();
}

// ---- IpcServer ----

IpcServer::IpcServer(KalmSession& session, std::function<void(const QString&)> onShowWindow)
    : m_session(session), m_onShowWindow(std::move(onShowWindow)), m_path(ipcSocketPath()) {}

IpcServer::~IpcServer() { stop(); }

bool IpcServer::start() {
    if (QFile::exists(m_path)) {
        const auto ping = ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("ping")}}, 400);
        if (ping && ping->value(QStringLiteral("ok")).toBool()) {
            return false;
        }
        QFile::remove(m_path);
    }

    const QByteArray pathUtf8 = m_path.toUtf8();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (static_cast<size_t>(pathUtf8.size()) >= sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, pathUtf8.constData(), static_cast<size_t>(pathUtf8.size()));

    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd) return false;
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        return false;
    }
    ::chmod(pathUtf8.constData(), 0600);
    if (::listen(fd.get(), 8) < 0) {
        return false;
    }

    m_serverFd = fd.release();
    m_stop.store(false);
    m_thread = std::thread(&IpcServer::acceptLoop, this);
    return true;
}

void IpcServer::stop() {
    m_stop.store(true);
    if (m_serverFd >= 0) {
        ::shutdown(m_serverFd, SHUT_RDWR);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_serverFd >= 0) {
        ::close(m_serverFd);
        m_serverFd = -1;
    }
    QFile::remove(m_path);
}

void IpcServer::acceptLoop() {
    while (!m_stop.load() && m_serverFd >= 0) {
        pollfd pfd{m_serverFd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 400);
        if (pr <= 0) continue;
        const int clientFd = ::accept4(m_serverFd, nullptr, nullptr, SOCK_CLOEXEC);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handleClient(clientFd);
    }
}

void IpcServer::handleClient(int rawClientFd) {
    UniqueFd clientFd(rawClientFd);
    QByteArray raw;
    char buf[4096];
    pollfd pfd{clientFd.get(), POLLIN, 0};
    if (::poll(&pfd, 1, 2000) > 0 && (pfd.revents & POLLIN)) {
        const ssize_t n = ::recv(clientFd.get(), buf, sizeof(buf), 0);
        if (n > 0) raw.append(buf, n);
    }

    QJsonObject resp;
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        resp = QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Invalid JSON")}};
    } else {
        try {
            resp = dispatch(doc.object());
        } catch (const std::exception& ex) {
            resp = QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QString::fromUtf8(ex.what())}};
        }
    }

    sendAll(clientFd.get(), QJsonDocument(resp).toJson(QJsonDocument::Compact));
}

QJsonObject IpcServer::dispatch(const QJsonObject& req) {
    const QString cmd = req.value(QStringLiteral("cmd")).toString();
    if (cmd == u"ping") {
        return QJsonObject{{QStringLiteral("ok"), true}};
    }
    if (cmd == u"show") {
        if (m_onShowWindow) m_onShowWindow(req.value(QStringLiteral("token")).toString());
        return QJsonObject{{QStringLiteral("ok"), true}};
    }
    if (cmd == u"status") {
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("state"), m_session.snapshot().toJson()}};
    }
    if (cmd == u"select_device") {
        const bool ok = m_session.switchToDevice(req.value(QStringLiteral("mac")).toString());
        return QJsonObject{{QStringLiteral("ok"), ok}, {QStringLiteral("state"), m_session.snapshot().toJson()}};
    }
    if (cmd == u"reload_registry") {
        m_session.registry().load();
        m_session.notifyRegistryUpdated();
        return QJsonObject{{QStringLiteral("ok"), true}};
    }
    if (cmd == u"set_noise") {
        const auto m = parseNoiseModeStr(req.value(QStringLiteral("mode")).toString());
        if (!m) return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Invalid mode")}};
        std::optional<int> ancLvl;
        std::optional<int> transEff;
        if (req.contains(QStringLiteral("anc_level")) && !req.value(QStringLiteral("anc_level")).isNull()) {
            ancLvl = req.value(QStringLiteral("anc_level")).toInt();
        }
        if (req.contains(QStringLiteral("trans_effect")) && !req.value(QStringLiteral("trans_effect")).isNull()) {
            transEff = req.value(QStringLiteral("trans_effect")).toInt();
        }
        const bool ok = m_session.setNoiseMode(*m, ancLvl, transEff);
        return QJsonObject{
            {QStringLiteral("ok"), ok},
            {QStringLiteral("error"), ok ? QJsonValue() : QJsonValue(trKey("err_mode_unconfirmed"))},
            {QStringLiteral("state"), m_session.snapshot().toJson()},
        };
    }
    if (cmd == u"toggle_noise") {
        QList<NoiseMode> cycle;
        for (const QJsonValue& v : req.value(QStringLiteral("modes")).toArray()) {
            if (auto m = parseNoiseModeStr(v.toString())) cycle.append(*m);
        }
        const auto nxt = m_session.toggleNoiseMode(cycle);
        return QJsonObject{
            {QStringLiteral("ok"), nxt.has_value()},
            {QStringLiteral("error"), nxt ? QJsonValue() : QJsonValue(trKey("err_mode_unconfirmed"))},
            {QStringLiteral("state"), m_session.snapshot().toJson()},
        };
    }
    if (cmd == u"set_eq") {
        const bool ok = m_session.setAudioEffect(req.value(QStringLiteral("effect")).toInt());
        return QJsonObject{{QStringLiteral("ok"), ok}, {QStringLiteral("state"), m_session.snapshot().toJson()}};
    }
    if (cmd == u"set_game") {
        const bool ok = m_session.setLowLatency(req.value(QStringLiteral("enabled")).toBool());
        return QJsonObject{{QStringLiteral("ok"), ok}, {QStringLiteral("state"), m_session.snapshot().toJson()}};
    }
    if (cmd == u"probe") {
        const QString q = req.value(QStringLiteral("device")).toString();
        const int listenSec = req.value(QStringLiteral("listen")).toInt(0);
        const QString report = m_session.runProbe(q, listenSec);
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("report"), report}};
    }
    return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Unknown IPC command")}};
}

// ---- Oneshot CLI Operations ----

namespace {

struct TargetInfo {
    QString mac;
    QString displayName;
    QString rawName;
    QString devPath;
};

TargetInfo resolveOneshotTarget(DeviceRegistry& reg, const QString& query) {
    reg.ensureInitialImport();
    if (!query.trimmed().isEmpty()) {
        const auto resolved = reg.resolveQuery(query);
        if (!resolved) {
            throw std::runtime_error(trKey("err_device_not_saved").arg(query).toStdString());
        }
        const auto bdev = findBluezByMac(resolved->mac);
        if (bdev && !bdev->connected) {
            throw std::runtime_error(
                trKey("err_not_connected_bt").arg(resolved->displayName(), resolved->mac).toStdString());
        }
        return {resolved->mac, resolved->displayName(), bdev ? bdev->name : resolved->name,
                bdev ? bdev->path : QString{}};
    }

    if (auto active = reg.getActive()) {
        const auto bdev = findBluezByMac(active->mac);
        if (bdev && bdev->connected) {
            return {active->mac, active->displayName(), bdev->name, bdev->path};
        }
        if (reg.autoSwitch()) {
            for (const auto& cand : reg.devices()) {
                const auto cb = findBluezByMac(cand.mac);
                if (cb && cb->connected) {
                    return {cand.mac, cand.displayName(), cb->name, cb->path};
                }
            }
        }
        throw std::runtime_error(
            trKey("err_not_connected_bt").arg(active->displayName(), active->mac).toStdString());
    }

    for (const auto& bdev : listBluezDevices(true, false)) {
        if (bdev.connected) {
            return {bdev.mac, bdev.name, bdev.name, bdev.path};
        }
    }
    throw std::runtime_error(trKey("err_no_vivo_found").toStdString());
}

bool recvUntil(int fd, FrameDecoder& dec, EarbudState& state, const WireProfile& profile,
               std::function<bool(const Frame&)> stopWhen, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    bool matched = false;
    char buf[2048];
    while (timer.elapsed() < timeoutMs) {
        const int rem = std::min(400, std::max(50, static_cast<int>(timeoutMs - timer.elapsed())));
        pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, rem);
        if (pr <= 0) {
            if (matched) return true;
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP)) break;
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (const Frame& f : dec.feed(QByteArray(buf, n))) {
            const QString tag = applyFrameToState(f, state);
            if (tag == u"time_request") {
                sendAll(fd, buildHostTimeResponse(profile));
            }
            if (stopWhen(f)) matched = true;
        }
        if (matched) return true;
    }
    return matched;
}

template <typename Fn>
QJsonObject runOneshot(DeviceRegistry& reg, const QString& query, Fn&& action) {
    const TargetInfo t = resolveOneshotTarget(reg, query);
    const RfcommConnection conn = openGaiaRfcomm(t.mac, t.devPath, t.displayName, 3000);
    const WireProfile profile = selectProfile(t.rawName);
    EarbudState state;
    state.connected = true;
    state.deviceName = t.displayName;
    state.deviceMac = t.mac;
    FrameDecoder dec;
    if (!conn.initialBytes.isEmpty()) {
        for (const Frame& f : dec.feed(conn.initialBytes)) {
            applyFrameToState(f, state);
        }
    }
    action(conn.fd.get(), profile, state, dec);
    return state.toJson();
}

struct HuaweiOneshot {
    RfcommConnection conn;
    huawei::ModelProfile profile;
    EarbudState state;
    huawei::Decoder decoder;
};

HuaweiOneshot openHuaweiOneshot(const TargetInfo& t) {
    HuaweiOneshot ctx;
    ctx.conn = openHuaweiRfcomm(t.mac, t.devPath, t.displayName, 3000);
    ctx.profile = huawei::profileFor(t.rawName);
    ctx.state.connected = true;
    ctx.state.deviceName = t.displayName;
    ctx.state.deviceMac = t.mac;
    if (!ctx.conn.initialBytes.isEmpty()) {
        for (const auto& packet : ctx.decoder.feed(ctx.conn.initialBytes)) {
            huawei::applyPacket(packet, ctx.state, ctx.profile);
        }
    }
    return ctx;
}

bool huaweiRecvUntil(int fd, huawei::Decoder& dec, EarbudState& state,
                     const huawei::ModelProfile& profile,
                     std::function<bool(const huawei::Packet&)> stopWhen, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    bool matched = false;
    char buf[2048];
    QMap<int, MultiConnectDevice> dcRows;
    int dcCount = 0;
    while (timer.elapsed() < timeoutMs) {
        const int rem = std::min(400, std::max(50, static_cast<int>(timeoutMs - timer.elapsed())));
        pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, rem);
        if (pr <= 0) {
            if (matched) return true;
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP)) break;
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (const auto& packet : dec.feed(QByteArray(buf, n))) {
            const auto effect = huawei::applyPacket(packet, state, profile);
            if (effect.dualConnectRow) {
                if (effect.dcCount > 0) dcCount = effect.dcCount;
                dcRows.insert(effect.dcIndex, effect.dcDevice);
                if (dcCount > 0 && dcRows.size() >= dcCount) {
                    QList<MultiConnectDevice> list;
                    for (auto it = dcRows.cbegin(); it != dcRows.cend(); ++it) list.append(it.value());
                    state.pairedDevices = list;
                }
            }
            if (stopWhen(packet)) matched = true;
        }
        if (matched) return true;
    }
    return matched;
}

bool targetIsHuawei(DeviceRegistry& reg, const QString& query) {
    return huawei::matchesName(resolveOneshotTarget(reg, query).rawName);
}

const QList<QByteArray>& huaweiStatusQueries() {
    static const QList<QByteArray> kList = {
        huawei::buildQueryBattery(),
        huawei::buildQueryAnc(),
        huawei::buildQueryEqualizer(),
        huawei::buildQueryLowLatency(),
        huawei::buildQueryAutoPause(),
        huawei::buildQueryInfo(),
        huawei::buildQueryDualConnectEnabled(),
        huawei::buildEnumerateDualConnect(),
    };
    return kList;
}

}  // namespace

QJsonObject oneshotStatus(DeviceRegistry& reg, const QString& deviceQuery) {
    if (targetIsHuawei(reg, deviceQuery)) {
        const TargetInfo t = resolveOneshotTarget(reg, deviceQuery);
        auto ctx = openHuaweiOneshot(t);
        for (const QByteArray& pkt : huaweiStatusQueries()) sendAll(ctx.conn.fd.get(), pkt);
        huaweiRecvUntil(ctx.conn.fd.get(), ctx.decoder, ctx.state, ctx.profile,
                        [](const huawei::Packet&) { return false; }, 1500);
        return ctx.state.toJson();
    }
    return runOneshot(reg, deviceQuery, [](int fd, const WireProfile& profile, EarbudState& state, FrameDecoder& dec) {
        for (const QByteArray& pkt : buildRegisterNotifications()) sendAll(fd, pkt);
        for (const QByteArray& pkt : {
                 buildQueryBattery(),
                 buildQueryNoiseMode(profile),
                 buildQueryWearState(profile),
                 buildQueryAudioEffect(profile),
                 buildQueryLowLatency(profile),
                 buildQueryNoiseCycle(profile),
                 buildQueryMultiConnect(profile),
             }) {
            sendAll(fd, pkt);
        }
        QList<uint16_t> seen;
        recvUntil(
            fd, dec, state, profile,
            [&](const Frame& f) {
                if ((f.command == kCmdReportBattery || f.command == kCmdReportNoiseMode ||
                     f.command == kCmdReportWearState || f.command == kCmdReportAudioEffect ||
                     f.command == kCmdReportLowLatency || f.command == kCmdReportMultiConnect) &&
                    !seen.contains(f.command)) {
                    seen.append(f.command);
                }
                return seen.size() >= 5 && !state.firmware.isEmpty();
            },
            1200);
    });
}

QJsonObject oneshotSetNoise(DeviceRegistry& reg, NoiseMode mode,
                            std::optional<int> ancLevel,
                            std::optional<int> transEffect,
                            const QString& deviceQuery) {
    if (targetIsHuawei(reg, deviceQuery)) {
        const TargetInfo t = resolveOneshotTarget(reg, deviceQuery);
        auto ctx = openHuaweiOneshot(t);
        const int target = huawei::toModeValue(mode);
        std::optional<int> level;
        if (target == huawei::kModeAnc) level = ancLevel;
        else if (target == huawei::kModeAwareness) level = 2;
        if (!sendAll(ctx.conn.fd.get(), huawei::buildSetAnc(target, level))) {
            throw std::runtime_error(trKey("err_mode_unconfirmed").toStdString());
        }
        const bool ok = huaweiRecvUntil(
            ctx.conn.fd.get(), ctx.decoder, ctx.state, ctx.profile,
            [target](const huawei::Packet& pkt) {
                return pkt.command == QByteArray::fromHex("2b2a") && pkt.param(1).size() >= 2 &&
                       static_cast<uint8_t>(pkt.param(1).at(1)) == target;
            },
            2000);
        if (!ok) throw std::runtime_error(trKey("err_mode_unconfirmed").toStdString());
        return ctx.state.toJson();
    }
    return runOneshot(reg, deviceQuery, [=](int fd, const WireProfile& profile, EarbudState& state, FrameDecoder& dec) {
        sendAll(fd, buildQueryBattery());
        // Query current noise state first so we preserve existing ancLevel / transEffect if not explicitly overridden
        if (!ancLevel.has_value() || !transEffect.has_value()) {
            sendAll(fd, buildQueryNoiseMode(profile));
            recvUntil(fd, dec, state, profile, [](const Frame& f) { return f.command == kCmdReportNoiseMode; }, 800);
        }
        const auto effAnc = ancLevel.has_value() ? ancLevel : state.noiseEffect;
        const auto effTrans = transEffect.has_value() ? transEffect : state.transparencyEffect;
        sendAll(fd, buildSetNoiseMode(mode, profile, effAnc, effTrans));
        const bool ok = recvUntil(
            fd, dec, state, profile,
            [mode](const Frame& f) {
                return (f.command == kCmdAckNoiseMode || f.command == kCmdReportNoiseMode) &&
                       f.payload.size() >= 2 && static_cast<uint8_t>(f.payload[0]) == 0 &&
                       static_cast<uint8_t>(f.payload[1]) == static_cast<uint8_t>(mode);
            },
            2000);
        if (!ok) throw std::runtime_error(trKey("err_mode_unconfirmed").toStdString());
    });
}

QJsonObject oneshotToggleNoise(DeviceRegistry& reg, const QList<NoiseMode>& cycle, const QString& deviceQuery) {
    const QList<NoiseMode> order =
        cycle.isEmpty() ? QList<NoiseMode>{NoiseMode::Anc, NoiseMode::Transparency, NoiseMode::Off} : cycle;
    if (targetIsHuawei(reg, deviceQuery)) {
        const TargetInfo t = resolveOneshotTarget(reg, deviceQuery);
        auto ctx = openHuaweiOneshot(t);
        sendAll(ctx.conn.fd.get(), huawei::buildQueryAnc());
        huaweiRecvUntil(
            ctx.conn.fd.get(), ctx.decoder, ctx.state, ctx.profile,
            [](const huawei::Packet& pkt) {
                return pkt.command == QByteArray::fromHex("2b2a") && pkt.param(1).size() >= 2;
            },
            1500);

        NoiseMode nxt = order.first();
        if (ctx.state.noiseMode) {
            const qsizetype idx = order.indexOf(*ctx.state.noiseMode);
            if (idx >= 0) nxt = order[(idx + 1) % order.size()];
        }

        const int target = huawei::toModeValue(nxt);
        std::optional<int> level;
        if (target == huawei::kModeAnc) level = ctx.state.noiseEffect;
        sendAll(ctx.conn.fd.get(), huawei::buildSetAnc(target, level));
        const bool ok = huaweiRecvUntil(
            ctx.conn.fd.get(), ctx.decoder, ctx.state, ctx.profile,
            [target](const huawei::Packet& pkt) {
                return pkt.command == QByteArray::fromHex("2b2a") && pkt.param(1).size() >= 2 &&
                       static_cast<uint8_t>(pkt.param(1).at(1)) == target;
            },
            2000);
        if (!ok) throw std::runtime_error(trKey("err_mode_unconfirmed").toStdString());
        return ctx.state.toJson();
    }
    return runOneshot(reg, deviceQuery, [&order](int fd, const WireProfile& profile, EarbudState& state, FrameDecoder& dec) {
        sendAll(fd, buildQueryBattery());
        sendAll(fd, buildQueryNoiseMode(profile));
        recvUntil(fd, dec, state, profile, [](const Frame& f) { return f.command == kCmdReportNoiseMode; }, 1500);
        NoiseMode nxt = order.first();
        if (state.noiseMode) {
            const qsizetype idx = order.indexOf(*state.noiseMode);
            if (idx >= 0) nxt = order[(idx + 1) % order.size()];
        }
        sendAll(fd, buildSetNoiseMode(nxt, profile, state.noiseEffect, state.transparencyEffect));
        const bool ok = recvUntil(
            fd, dec, state, profile,
            [nxt](const Frame& f) {
                return (f.command == kCmdAckNoiseMode || f.command == kCmdReportNoiseMode) &&
                       f.payload.size() >= 2 && static_cast<uint8_t>(f.payload[0]) == 0 &&
                       static_cast<uint8_t>(f.payload[1]) == static_cast<uint8_t>(nxt);
            },
            2000);
        if (!ok) throw std::runtime_error(trKey("err_mode_unconfirmed").toStdString());
    });
}

QJsonObject oneshotSetEq(DeviceRegistry& reg, int effectId, const QString& deviceQuery) {
    if (targetIsHuawei(reg, deviceQuery)) {
        const TargetInfo t = resolveOneshotTarget(reg, deviceQuery);
        auto ctx = openHuaweiOneshot(t);
        sendAll(ctx.conn.fd.get(), huawei::buildSetEqualizerPreset(effectId));
        const bool ok = huaweiRecvUntil(
            ctx.conn.fd.get(), ctx.decoder, ctx.state, ctx.profile,
            [effectId](const huawei::Packet& pkt) {
                return pkt.command == QByteArray::fromHex("2b4a") && huawei::eqCurrentPreset(pkt) == effectId;
            },
            2000);
        if (!ok) throw std::runtime_error("EQ change unconfirmed.");
        return ctx.state.toJson();
    }
    return runOneshot(reg, deviceQuery, [effectId](int fd, const WireProfile& profile, EarbudState& state, FrameDecoder& dec) {
        sendAll(fd, buildSetAudioEffect(effectId, profile));
        const bool ok = recvUntil(
            fd, dec, state, profile,
            [](const Frame& f) { return f.command == kCmdAckAudioEffect || f.command == kCmdReportAudioEffect; },
            1800);
        if (!ok) throw std::runtime_error("EQ change unconfirmed.");
    });
}

QJsonObject oneshotSetGame(DeviceRegistry& reg, bool enabled, const QString& deviceQuery) {
    if (targetIsHuawei(reg, deviceQuery)) {
        const TargetInfo t = resolveOneshotTarget(reg, deviceQuery);
        auto ctx = openHuaweiOneshot(t);
        sendAll(ctx.conn.fd.get(), huawei::buildSetLowLatency(enabled));
        sendAll(ctx.conn.fd.get(), huawei::buildQueryLowLatency());
        const bool ok = huaweiRecvUntil(
            ctx.conn.fd.get(), ctx.decoder, ctx.state, ctx.profile,
            [enabled](const huawei::Packet& pkt) {
                return pkt.command == QByteArray::fromHex("2b6c") && pkt.param(2).size() == 1 &&
                       (pkt.paramByte(2) != 0) == enabled;
            },
            2000);
        if (!ok) throw std::runtime_error("Game mode change unconfirmed.");
        return ctx.state.toJson();
    }
    return runOneshot(reg, deviceQuery, [enabled](int fd, const WireProfile& profile, EarbudState& state, FrameDecoder& dec) {
        sendAll(fd, buildSetLowLatency(enabled, profile));
        const bool ok = recvUntil(
            fd, dec, state, profile,
            [](const Frame& f) { return f.command == kCmdAckLowLatency || f.command == kCmdReportLowLatency; },
            1800);
        if (!ok) throw std::runtime_error("Game mode change unconfirmed.");
    });
}

}  // namespace kalm
