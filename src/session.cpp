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
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace kalm {

QString ipcSocketPath() {
#ifdef _WIN32
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (base.isEmpty()) base = QDir::tempPath();
    const QString dir = base + QStringLiteral("/kalm");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/kalm.sock");
#else
    QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtimeDir.isEmpty()) {
        runtimeDir = QStringLiteral("/tmp/kalm-%1").arg(::getuid());
    }
    QDir().mkpath(runtimeDir);
    return runtimeDir + QStringLiteral("/kalm.sock");
#endif
}

std::optional<QJsonObject> ipcRequest(const QJsonObject& payload, int timeoutMs) {
    const QString path = ipcSocketPath();
    if (!QFile::exists(path)) return std::nullopt;

    UniqueSocket fd(openLocalIpcClient(path));
    if (!fd) return std::nullopt;

    const QByteArray reqBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (!sendAll(fd.get(), reqBytes)) {
        return std::nullopt;
    }

    QByteArray respBytes;
    QElapsedTimer timer;
    timer.start();
    char buf[4096];
    while (timer.elapsed() < timeoutMs) {
        const int rem = std::max(50, static_cast<int>(timeoutMs - timer.elapsed()));
        const int pr = pollSocketRead(fd.get(), rem);
        if (pr <= 0) break;
        const qint64 n = recvSocket(fd.get(), buf, sizeof(buf));
        if (n <= 0) break;
        respBytes.append(buf, static_cast<qsizetype>(n));
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
    SocketHandle fd = kInvalidSocket;
    bool wasConnected = false;
    {
        QMutexLocker lock(&m_lock);
        fd = m_sockFd;
        m_sockFd = kInvalidSocket;
        wasConnected = m_state.connected;
        if (resetIdentity) {
            m_state = EarbudState{};
        } else {
            m_state.connected = false;
            m_state.leftBattery.stale = true;
            m_state.rightBattery.stale = true;
            m_state.caseBattery.stale = true;
        }
    }
    if (fd != kInvalidSocket) {
        shutdownSocketHandle(fd);
        closeSocketHandle(fd);
    }
    if (wasConnected || resetIdentity) {
        emitState();
    }
}

bool KalmSession::sendRaw(const QByteArray& data) {
    SocketHandle fd = kInvalidSocket;
    {
        QMutexLocker lock(&m_lock);
        fd = m_sockFd;
    }
    if (fd == kInvalidSocket) return false;
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

std::optional<RealmeOppoFrame> KalmSession::sendAndWaitRealme(
    const QByteArray& packet,
    std::function<bool(const RealmeOppoFrame&)> pred,
    int timeoutMs) {
    auto waiter = std::make_shared<RealmeWaiter>();
    waiter->pred = std::move(pred);
    {
        QMutexLocker lock(&m_waitersMtx);
        m_realmeWaiters.append(waiter);
    }

    auto cleanup = [&]() {
        QMutexLocker lock(&m_waitersMtx);
        m_realmeWaiters.removeOne(waiter);
    };

    if (!sendRaw(packet)) {
        cleanup();
        return std::nullopt;
    }

    std::optional<RealmeOppoFrame> out;
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

bool KalmSession::refresh() {
    DeviceProtocol proto;
    WireProfile p;
    {
        QMutexLocker lock(&m_lock);
        proto = m_protocol;
        p = m_profile;
    }

    if (proto == DeviceProtocol::RealmeOppoTl) {
        bool ok = false;
        for (const QByteArray& pkt : {
                 buildRealmeOppoPacket(kTlCmdGetFirmware, 0, {}),
                 buildRealmeOppoPacket(kTlCmdGetBattery, 0, {}),
                 buildRealmeOppoPacket(kTlCmdQueryNotify, 0, QByteArray("\x03", 1)),
                 buildRealmeOppoPacket(kTlCmdGetNoise, 0, {}),
                 buildRealmeOppoPacket(kTlCmdGetSwitches, 0, QByteArray("\x04\x05\x06\x07", 4)),
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
    DeviceProtocol proto;
    WireProfile p;
    std::optional<int> effAnc;
    std::optional<int> effTrans;
    {
        QMutexLocker lock(&m_lock);
        proto = m_protocol;
        p = m_profile;
        effAnc = ancLevel.has_value() ? ancLevel : m_state.noiseEffect;
        effTrans = transEffect.has_value() ? transEffect : m_state.transparencyEffect;
    }

    if (proto == DeviceProtocol::RealmeOppoTl) {
        const auto ack = sendAndWaitRealme(
            buildRealmeSetNoise(mode, 0),
            [](const RealmeOppoFrame& f) {
                return (f.command == kTlRespSetNoise && !f.payload.isEmpty() &&
                        static_cast<uint8_t>(f.payload[0]) == 0);
            },
            2200);
        if (ack) {
            {
                QMutexLocker lock(&m_lock);
                m_state.noiseMode = mode;
            }
            emitState();
            return true;
        }
        return false;
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
    {
        QMutexLocker lock(&m_lock);
        p = m_profile;
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
    {
        QMutexLocker lock(&m_lock);
        p = m_profile;
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
    {
        QMutexLocker lock(&m_lock);
        p = m_profile;
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
    const auto snap = snapshot();
    if (snap.pairedDevices.isEmpty()) return false;
    WireProfile p;
    {
        QMutexLocker lock(&m_lock);
        p = m_profile;
    }
    if (!sendRaw(buildSetMultiConnect(snap.pairedDevices, targetMac, connect, p))) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    sendRaw(buildQueryMultiConnect(p));
    return true;
}

bool KalmSession::findEarbuds(bool ring) {
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
    m_paused.store(true);
    closeSocket(false);
    wakeWorker();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

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

        RfcommConnection conn;
        try {
            conn = openEarbudRfcomm(chosen->mac, chosenBluez->path, chosen->displayName(),
                                    chosenBluez->uuids, 2500);
        } catch (const std::exception& ex) {
            emitError(QString::fromUtf8(ex.what()));
            sleepInterruptible(3500);
            continue;
        }

        const WireProfile profile = selectProfile(chosenBluez->name);
        const DeviceProtocol proto = conn.protocol;
        const SocketHandle rawConnFd = conn.fd.release();
        {
            QMutexLocker lock(&m_lock);
            m_sockFd = rawConnFd;
            m_protocol = proto;
            m_profile = profile;
            m_state.connected = true;
            m_state.protocol = proto;
            m_state.deviceName = chosen->displayName();
            m_state.deviceMac = chosen->mac;
        }

        FrameDecoder vivoDecoder;
        RealmeOppoDecoder realmeDecoder;
        if (!conn.initialBytes.isEmpty()) {
            if (proto == DeviceProtocol::RealmeOppoTl) {
                handleRealmeFrames(realmeDecoder.feed(conn.initialBytes));
            } else {
                handleFrames(vivoDecoder.feed(conn.initialBytes));
            }
        }

        if (proto == DeviceProtocol::VivoGaia) {
            for (const QByteArray& pkt : buildRegisterNotifications()) {
                sendRaw(pkt);
            }
        } else {
            // Subscribe to Battery (1), Wear (2), and Noise/ANC (3) live notifications
            sendRaw(buildRealmeOppoPacket(kTlCmdGetNotifyCaps, 0, {}));
            for (uint8_t evId : {uint8_t(1), uint8_t(2), uint8_t(3)}) {
                sendRaw(buildRealmeOppoPacket(kTlCmdRegNotify, 0, QByteArray(1, static_cast<char>(evId))));
            }
        }
        refresh();
        emitDevices();
        emitState();

        const qint64 pollIntervalMs = (proto == DeviceProtocol::RealmeOppoTl) ? 8000 : 30000;
        QElapsedTimer pollTimer;
        pollTimer.start();
        QElapsedTimer eventDebounceTimer;
        bool pendingEventRefresh = false;

        char buf[2048];
        while (!m_stop.load()) {
            SocketHandle curFd = kInvalidSocket;
            {
                QMutexLocker lock(&m_lock);
                curFd = m_sockFd;
            }
            if (curFd != rawConnFd) break;

            const int waitMs = pendingEventRefresh ? 200 : 1000;
            const int pr = pollSocketRead(rawConnFd, waitMs);
            if (pr < 0) break;
            if (pr > 0) {
                const qint64 n = recvSocket(rawConnFd, buf, sizeof(buf));
                if (n <= 0) break;
                const QByteArray chunk(buf, static_cast<qsizetype>(n));
                if (proto == DeviceProtocol::RealmeOppoTl) {
                    const auto frames = realmeDecoder.feed(chunk);
                    if (!frames.isEmpty() && handleRealmeFrames(frames)) {
                        pendingEventRefresh = true;
                        eventDebounceTimer.restart();
                    }
                } else {
                    const auto frames = vivoDecoder.feed(chunk);
                    if (!frames.isEmpty()) handleFrames(frames);
                }
            }

            if (pendingEventRefresh && eventDebounceTimer.elapsed() >= 400) {
                pendingEventRefresh = false;
                sendRaw(buildRealmeOppoPacket(kTlCmdGetBattery, 0, {}));
                sendRaw(buildRealmeOppoPacket(kTlCmdQueryNotify, 0, QByteArray("\x03", 1)));
            }

            if (pollTimer.elapsed() >= pollIntervalMs) {
                pollTimer.restart();
                if (proto == DeviceProtocol::RealmeOppoTl) {
                    sendRaw(buildRealmeOppoPacket(kTlCmdGetBattery, 0, {}));
                    sendRaw(buildRealmeOppoPacket(kTlCmdQueryNotify, 0, QByteArray("\x03", 1)));
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

bool KalmSession::handleRealmeFrames(const QList<RealmeOppoFrame>& frames) {
    bool changed = false;
    bool needEventRefresh = false;
    {
        QMutexLocker lock(&m_lock);
        for (const RealmeOppoFrame& f : frames) {
            const QString tag = applyRealmeFrameToState(f, m_state);
            if (tag == u"event_refresh") {
                needEventRefresh = true;
                changed = true;
            } else if (tag != u"ignored" && tag != u"handshake") {
                changed = true;
            }
        }
    }

    QList<std::shared_ptr<RealmeWaiter>> activeWaiters;
    {
        QMutexLocker lock(&m_waitersMtx);
        activeWaiters = m_realmeWaiters;
    }
    for (const RealmeOppoFrame& f : frames) {
        for (const auto& w : activeWaiters) {
            QMutexLocker wl(&w->mtx);
            if (!w->done && w->pred(f)) {
                w->result = f;
                w->done = true;
                w->cv.wakeAll();
            }
        }
    }

    if (changed) {
        emitState();
    }
    return needEventRefresh;
}

// ---- IpcServer ----

IpcServer::IpcServer(KalmSession& session, ShowCallback onShowWindow, NotifyCallback onNotify)
    : m_session(session),
      m_onShowWindow(std::move(onShowWindow)),
      m_onNotify(std::move(onNotify)),
      m_path(ipcSocketPath()) {}

IpcServer::~IpcServer() { stop(); }

bool IpcServer::start() {
    if (QFile::exists(m_path)) {
        const auto ping = ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("ping")}}, 400);
        if (ping && ping->value(QStringLiteral("ok")).toBool()) {
            return false;
        }
        QFile::remove(m_path);
    }

    m_serverFd = openLocalIpcServer(m_path);
    if (m_serverFd == kInvalidSocket) return false;

    m_stop.store(false);
    m_thread = std::thread(&IpcServer::acceptLoop, this);
    return true;
}

void IpcServer::stop() {
    m_stop.store(true);
    if (m_serverFd != kInvalidSocket) {
        shutdownSocketHandle(m_serverFd);
        closeSocketHandle(m_serverFd);
        m_serverFd = kInvalidSocket;
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    QFile::remove(m_path);
}

void IpcServer::acceptLoop() {
    while (!m_stop.load()) {
        const SocketHandle srv = m_serverFd;
        if (srv == kInvalidSocket) break;
        const int pr = pollSocketRead(srv, 400);
        if (pr <= 0) {
            if (pr < 0) break;
            continue;
        }
        const SocketHandle clientFd = acceptLocalIpcClient(srv);
        if (clientFd == kInvalidSocket) break;
        handleClient(clientFd);
    }
}

void IpcServer::handleClient(SocketHandle rawClientFd) {
    UniqueSocket clientFd(rawClientFd);
    QByteArray raw;
    char buf[4096];
    if (pollSocketRead(clientFd.get(), 2000) > 0) {
        const qint64 n = recvSocket(clientFd.get(), buf, sizeof(buf));
        if (n > 0) raw.append(buf, static_cast<qsizetype>(n));
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
    if (cmd == u"notify") {
        if (m_onNotify) {
            m_onNotify(req.value(QStringLiteral("title")).toString(),
                       req.value(QStringLiteral("body")).toString());
        }
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
    QStringList uuids;
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
                bdev ? bdev->path : QString{}, bdev ? bdev->uuids : QStringList{}};
    }

    if (auto active = reg.getActive()) {
        const auto bdev = findBluezByMac(active->mac);
        if (bdev && bdev->connected) {
            return {active->mac, active->displayName(), bdev->name, bdev->path, bdev->uuids};
        }
        if (reg.autoSwitch()) {
            for (const auto& cand : reg.devices()) {
                const auto cb = findBluezByMac(cand.mac);
                if (cb && cb->connected) {
                    return {cand.mac, cand.displayName(), cb->name, cb->path, cb->uuids};
                }
            }
        }
        throw std::runtime_error(
            trKey("err_not_connected_bt").arg(active->displayName(), active->mac).toStdString());
    }

    for (const auto& bdev : listBluezDevices(true, false)) {
        if (bdev.connected) {
            return {bdev.mac, bdev.name, bdev.name, bdev.path, bdev.uuids};
        }
    }
    throw std::runtime_error(trKey("err_no_vivo_found").toStdString());
}

bool recvUntil(SocketHandle fd, FrameDecoder& dec, EarbudState& state, const WireProfile& profile,
               std::function<bool(const Frame&)> stopWhen, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    bool matched = false;
    char buf[2048];
    while (timer.elapsed() < timeoutMs) {
        const int rem = std::min(400, std::max(50, static_cast<int>(timeoutMs - timer.elapsed())));
        const int pr = pollSocketRead(fd, rem);
        if (pr == 0) {
            if (matched) return true;
            continue;
        }
        if (pr < 0) break;
        const qint64 n = recvSocket(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (const Frame& f : dec.feed(QByteArray(buf, static_cast<qsizetype>(n)))) {
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

bool recvUntilRealme(SocketHandle fd, RealmeOppoDecoder& dec, EarbudState& state,
                     std::function<bool(const RealmeOppoFrame&)> stopWhen, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    bool matched = false;
    char buf[2048];
    while (timer.elapsed() < timeoutMs) {
        const int rem = std::min(400, std::max(50, static_cast<int>(timeoutMs - timer.elapsed())));
        const int pr = pollSocketRead(fd, rem);
        if (pr == 0) {
            if (matched) return true;
            continue;
        }
        if (pr < 0) break;
        const qint64 n = recvSocket(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (const RealmeOppoFrame& f : dec.feed(QByteArray(buf, static_cast<qsizetype>(n)))) {
            applyRealmeFrameToState(f, state);
            if (stopWhen(f)) matched = true;
        }
        if (matched) return true;
    }
    return matched;
}

template <typename Fn>
QJsonObject runOneshot(DeviceRegistry& reg, const QString& query, Fn&& action) {
    const TargetInfo t = resolveOneshotTarget(reg, query);
    const RfcommConnection conn = openEarbudRfcomm(t.mac, t.devPath, t.displayName, t.uuids, 3000);
    const WireProfile profile = selectProfile(t.rawName);
    EarbudState state;
    state.connected = true;
    state.protocol = conn.protocol;
    state.deviceName = t.displayName;
    state.deviceMac = t.mac;
    FrameDecoder vivoDec;
    RealmeOppoDecoder realmeDec;
    if (!conn.initialBytes.isEmpty()) {
        if (conn.protocol == DeviceProtocol::RealmeOppoTl) {
            for (const auto& f : realmeDec.feed(conn.initialBytes)) applyRealmeFrameToState(f, state);
        } else {
            for (const Frame& f : vivoDec.feed(conn.initialBytes)) applyFrameToState(f, state);
        }
    }
    action(conn.fd.get(), conn.protocol, profile, state, vivoDec, realmeDec);
    return state.toJson();
}

}  // namespace

QJsonObject oneshotStatus(DeviceRegistry& reg, const QString& deviceQuery) {
    return runOneshot(reg, deviceQuery, [](SocketHandle fd, DeviceProtocol proto, const WireProfile& profile, EarbudState& state, FrameDecoder& dec, RealmeOppoDecoder& rdec) {
        if (proto == DeviceProtocol::RealmeOppoTl) {
            sendAll(fd, buildRealmeOppoPacket(kTlCmdGetFirmware, 0, {}));
            sendAll(fd, buildRealmeOppoPacket(kTlCmdGetBattery, 0, {}));
            sendAll(fd, buildRealmeOppoPacket(kTlCmdQueryNotify, 0, QByteArray("\x03", 1)));
            sendAll(fd, buildRealmeOppoPacket(kTlCmdGetNoise, 0, {}));
            sendAll(fd, buildRealmeOppoPacket(kTlCmdGetSwitches, 0, QByteArray("\x04\x05\x06\x07", 4)));
            QList<uint16_t> seen;
            recvUntilRealme(
                fd, rdec, state,
                [&](const RealmeOppoFrame& f) {
                    if (!seen.contains(f.command)) seen.append(f.command);
                    return seen.contains(kTlRespGetBattery) &&
                           (seen.contains(kTlRespQueryNotify) || seen.contains(kTlRespGetNoise)) &&
                           seen.contains(kTlRespGetFirmware);
                },
                1200);
            return;
        }

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
    return runOneshot(reg, deviceQuery, [=](SocketHandle fd, DeviceProtocol proto, const WireProfile& profile, EarbudState& state, FrameDecoder& dec, RealmeOppoDecoder& rdec) {
        if (proto == DeviceProtocol::RealmeOppoTl) {
            sendAll(fd, buildRealmeOppoPacket(kTlCmdGetBattery, 0, {}));
            sendAll(fd, buildRealmeSetNoise(mode, 0));
            const bool ok = recvUntilRealme(
                fd, rdec, state,
                [](const RealmeOppoFrame& f) {
                    return f.command == kTlRespSetNoise && !f.payload.isEmpty() &&
                           static_cast<uint8_t>(f.payload[0]) == 0;
                },
                2200);
            if (!ok) throw std::runtime_error(trKey("err_mode_unconfirmed").toStdString());
            state.noiseMode = mode;
            return;
        }

        sendAll(fd, buildQueryBattery());
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
    return runOneshot(reg, deviceQuery, [&order](SocketHandle fd, DeviceProtocol proto, const WireProfile& profile, EarbudState& state, FrameDecoder& dec, RealmeOppoDecoder& rdec) {
        if (proto == DeviceProtocol::RealmeOppoTl) {
            sendAll(fd, buildRealmeOppoPacket(kTlCmdGetBattery, 0, {}));
            sendAll(fd, buildRealmeOppoPacket(kTlCmdQueryNotify, 0, QByteArray("\x03", 1)));
            sendAll(fd, buildRealmeOppoPacket(kTlCmdGetNoise, 0, {}));
            recvUntilRealme(
                fd, rdec, state,
                [&](const RealmeOppoFrame&) { return state.noiseMode.has_value(); },
                1200);
            NoiseMode nxt = order.first();
            if (state.noiseMode) {
                const qsizetype idx = order.indexOf(*state.noiseMode);
                if (idx >= 0) nxt = order[(idx + 1) % order.size()];
            }
            sendAll(fd, buildRealmeSetNoise(nxt, 0));
            const bool ok = recvUntilRealme(
                fd, rdec, state,
                [](const RealmeOppoFrame& f) {
                    return f.command == kTlRespSetNoise && !f.payload.isEmpty() &&
                           static_cast<uint8_t>(f.payload[0]) == 0;
                },
                2200);
            if (!ok) throw std::runtime_error(trKey("err_mode_unconfirmed").toStdString());
            state.noiseMode = nxt;
            return;
        }

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
    return runOneshot(reg, deviceQuery, [effectId](SocketHandle fd, DeviceProtocol /*proto*/, const WireProfile& profile, EarbudState& state, FrameDecoder& dec, RealmeOppoDecoder& /*rdec*/) {
        sendAll(fd, buildSetAudioEffect(effectId, profile));
        const bool ok = recvUntil(
            fd, dec, state, profile,
            [](const Frame& f) { return f.command == kCmdAckAudioEffect || f.command == kCmdReportAudioEffect; },
            1800);
        if (!ok) throw std::runtime_error("EQ change unconfirmed.");
    });
}

QJsonObject oneshotSetGame(DeviceRegistry& reg, bool enabled, const QString& deviceQuery) {
    return runOneshot(reg, deviceQuery, [enabled](SocketHandle fd, DeviceProtocol /*proto*/, const WireProfile& profile, EarbudState& state, FrameDecoder& dec, RealmeOppoDecoder& /*rdec*/) {
        sendAll(fd, buildSetLowLatency(enabled, profile));
        const bool ok = recvUntil(
            fd, dec, state, profile,
            [](const Frame& f) { return f.command == kCmdAckLowLatency || f.command == kCmdReportLowLatency; },
            1800);
        if (!ok) throw std::runtime_error("Game mode change unconfirmed.");
    });
}

}  // namespace kalm
