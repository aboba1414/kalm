// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "protocol.hpp"
#include "registry.hpp"

#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QString>
#include <QWaitCondition>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace kalm {

QString ipcSocketPath();
std::optional<QJsonObject> ipcRequest(const QJsonObject& payload, int timeoutMs = 2500);

class KalmSession {
public:
    using StateCallback = std::function<void(const EarbudState&)>;
    using DevicesCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const QString&)>;

    KalmSession(DeviceRegistry& registry,
                const QString& overrideMac = {},
                StateCallback onStateChanged = {},
                DevicesCallback onDevicesChanged = {},
                ErrorCallback onError = {});
    ~KalmSession();

    DeviceRegistry& registry() { return m_registry; }
    EarbudState snapshot() const;

    void start();
    void stop();
    bool switchToDevice(const QString& mac);
    void notifyRegistryUpdated();
    void forceReconnect();

    bool refresh();
    bool setNoiseMode(NoiseMode mode,
                      std::optional<int> ancLevel = std::nullopt,
                      std::optional<int> transEffect = std::nullopt);
    bool setAncLevel(int levelCode);
    bool setTransparencyEffect(int transCode);
    std::optional<NoiseMode> toggleNoiseMode(const QList<NoiseMode>& cycle = {});
    bool setAudioEffect(int effectId);
    bool setLowLatency(bool enabled);
    bool setWearDetection(bool enabled);
    bool setNoiseCycle(int cycleCode);
    bool setDeviceConnection(const QString& targetMac, bool connect);
    bool findEarbuds(bool ring);

private:
    struct Waiter {
        std::function<bool(const Frame&)> pred;
        QMutex mtx;
        QWaitCondition cv;
        bool done = false;
        Frame result;
    };

    void runLoop();
    void handleFrames(const QList<Frame>& frames);
    void closeSocket(bool resetIdentity = false);
    bool sendRaw(const QByteArray& data);
    std::optional<Frame> sendAndWait(const QByteArray& packet,
                                     std::function<bool(const Frame&)> pred,
                                     int timeoutMs = 1800);
    void wakeWorker();

    void emitState();
    void emitDevices();
    void emitError(const QString& msg);

    DeviceRegistry& m_registry;
    QString m_overrideMac;
    StateCallback m_onStateChanged;
    DevicesCallback m_onDevicesChanged;
    ErrorCallback m_onError;

    mutable QRecursiveMutex m_lock;
    int m_sockFd = -1;
    WireProfile m_profile;
    EarbudState m_state;

    std::atomic<bool> m_stop{false};
    QMutex m_wakeMtx;
    QWaitCondition m_wakeCv;
    bool m_woken = false;
    std::thread m_worker;

    QMutex m_waitersMtx;
    QList<std::shared_ptr<Waiter>> m_waiters;
};

class IpcServer {
public:
    IpcServer(KalmSession& session, std::function<void()> onShowWindow = {});
    ~IpcServer();

    bool start();
    void stop();

private:
    void acceptLoop();
    void handleClient(int clientFd);
    QJsonObject dispatch(const QJsonObject& req);

    KalmSession& m_session;
    std::function<void()> m_onShowWindow;
    QString m_path;
    int m_serverFd = -1;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

// Standalone oneshot CLI operations when no GUI/daemon is running
QJsonObject oneshotStatus(DeviceRegistry& reg, const QString& deviceQuery = {});
QJsonObject oneshotSetNoise(DeviceRegistry& reg, NoiseMode mode,
                            std::optional<int> ancLevel = std::nullopt,
                            std::optional<int> transEffect = std::nullopt,
                            const QString& deviceQuery = {});
QJsonObject oneshotToggleNoise(DeviceRegistry& reg, const QList<NoiseMode>& cycle, const QString& deviceQuery = {});
QJsonObject oneshotSetEq(DeviceRegistry& reg, int effectId, const QString& deviceQuery = {});
QJsonObject oneshotSetGame(DeviceRegistry& reg, bool enabled, const QString& deviceQuery = {});

}  // namespace kalm
