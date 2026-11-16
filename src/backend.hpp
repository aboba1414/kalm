// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "protocol.hpp"
#include "registry.hpp"
#include "session.hpp"

#include <QAction>
#include <QActionGroup>
#include <QIcon>
#include <QMap>
#include <QMenu>
#include <QObject>
#include <QPointer>
#include <QSystemTrayIcon>
#include <QVariantList>
#include <QWindow>
#include <optional>

namespace kalm {

QIcon makeKalmIcon(bool connected, std::optional<NoiseMode> mode);

class KalmBackend : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool quitting READ quitting NOTIFY quittingChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY stateChanged)
    Q_PROPERTY(QString deviceMac READ deviceMac NOTIFY stateChanged)
    Q_PROPERTY(QString firmware READ firmware NOTIFY stateChanged)
    Q_PROPERTY(QString statusSummary READ statusSummary NOTIFY stateChanged)
    Q_PROPERTY(int noiseModeIndex READ noiseModeIndex NOTIFY stateChanged)

    Q_PROPERTY(QString leftBatteryText READ leftBatteryText NOTIFY stateChanged)
    Q_PROPERTY(QString leftBatterySub READ leftBatterySub NOTIFY stateChanged)
    Q_PROPERTY(QString rightBatteryText READ rightBatteryText NOTIFY stateChanged)
    Q_PROPERTY(QString rightBatterySub READ rightBatterySub NOTIFY stateChanged)
    Q_PROPERTY(QString caseBatteryText READ caseBatteryText NOTIFY stateChanged)
    Q_PROPERTY(QString caseBatterySub READ caseBatterySub NOTIFY stateChanged)

    Q_PROPERTY(QVariantList eqModel READ eqModel CONSTANT)
    Q_PROPERTY(int eqIndex READ eqIndex NOTIFY stateChanged)
    Q_PROPERTY(QVariantList cycleModel READ cycleModel CONSTANT)
    Q_PROPERTY(int cycleIndex READ cycleIndex NOTIFY stateChanged)

    Q_PROPERTY(bool gameMode READ gameMode NOTIFY stateChanged)
    Q_PROPERTY(bool wearDetection READ wearDetection NOTIFY stateChanged)
    Q_PROPERTY(bool isRinging READ isRinging NOTIFY ringingChanged)

    Q_PROPERTY(QVariantList multipointDevices READ multipointDevices NOTIFY stateChanged)
    Q_PROPERTY(QVariantList savedDevices READ savedDevices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList discoveredDevices READ discoveredDevices NOTIFY discoveredChanged)
    Q_PROPERTY(bool autoSwitch READ autoSwitch NOTIFY devicesChanged)

    Q_PROPERTY(QString bannerText READ bannerText NOTIFY bannerChanged)
    Q_PROPERTY(int bannerType READ bannerType NOTIFY bannerChanged)

public:
    KalmBackend(KalmSession& session, DeviceRegistry& registry, QObject* parent = nullptr);

    void bindWindow(QWindow* win);

    // Thread-safe pushers from background workers
    void pushStateFromWorker(const EarbudState& st);
    void pushDevicesRefreshFromWorker();
    void pushErrorFromWorker(const QString& msg);
    void pushShowFromWorker();

    bool quitting() const { return m_quitting; }
    bool connected() const { return m_state.connected; }
    QString deviceName() const;
    QString deviceMac() const { return m_state.deviceMac; }
    QString firmware() const { return m_state.firmware; }
    QString statusSummary() const;
    int noiseModeIndex() const;

    QString leftBatteryText() const;
    QString leftBatterySub() const;
    QString rightBatteryText() const;
    QString rightBatterySub() const;
    QString caseBatteryText() const;
    QString caseBatterySub() const;

    QVariantList eqModel() const { return m_eqModel; }
    int eqIndex() const;
    QVariantList cycleModel() const { return m_cycleModel; }
    int cycleIndex() const;

    bool gameMode() const { return m_state.lowLatency.value_or(false); }
    bool wearDetection() const { return m_state.wearDetection.value_or(false); }
    bool isRinging() const { return m_ringing; }

    QVariantList multipointDevices() const;
    QVariantList savedDevices() const { return m_savedList; }
    QVariantList discoveredDevices() const { return m_discoveredList; }
    bool autoSwitch() const { return m_registry.autoSwitch(); }

    QString bannerText() const { return m_bannerText; }
    int bannerType() const { return m_bannerType; }

public Q_SLOTS:
    void refreshSavedDevicesAsync();
    void setNoiseByIndex(int index);
    void setEqByIndex(int index);
    void setCycleByIndex(int index);
    void setGameMode(bool enabled);
    void setWearDetection(bool enabled);
    void toggleMultipointDevice(const QString& mac, bool connect);
    void startFindEarbuds();
    void stopFindEarbuds();
    void refreshNow();
    void scanBluetoothDevices();
    bool addSavedDevice(const QString& mac, const QString& name, const QString& alias);
    void removeSavedDevice(const QString& mac);
    void renameSavedDevice(const QString& mac, const QString& alias);
    void selectSavedDevice(const QString& mac);
    void connectBluetoothDevice(const QString& mac);
    void setAutoSwitch(bool enabled);
    void openSystemBluetoothSettings();
    void clearBanner();
    void quitApp();

Q_SIGNALS:
    void stateChanged();
    void devicesChanged();
    void discoveredChanged();
    void bannerChanged();
    void ringingChanged();
    void quittingChanged();
    void windowShowRequested();

private:
    void runBg(std::function<void()> fn);
    void setBannerMain(const QString& text, int type);

    KalmSession& m_session;
    DeviceRegistry& m_registry;
    EarbudState m_state;
    QVariantList m_eqModel;
    QVariantList m_cycleModel;
    QVariantList m_savedList;
    QVariantList m_discoveredList;
    QString m_bannerText;
    int m_bannerType = 0;
    bool m_ringing = false;
    bool m_quitting = false;
    QPointer<QWindow> m_window;
};

class KalmTrayController : public QObject {
    Q_OBJECT
public:
    KalmTrayController(KalmSession& session, KalmBackend& backend, QObject* parent = nullptr);

    void bindWindow(QWindow* win) { m_window = win; }
    void hide() { m_tray.hide(); }

private Q_SLOTS:
    void onActivated(QSystemTrayIcon::ActivationReason reason);
    void syncState();
    void syncDevicesMenu();

private:
    KalmSession& m_session;
    KalmBackend& m_backend;
    QPointer<QWindow> m_window;

    QSystemTrayIcon m_tray;
    QMenu m_menu;
    QAction* m_headerAct = nullptr;
    QAction* m_battAct = nullptr;
    QAction* m_actAnc = nullptr;
    QAction* m_actTrans = nullptr;
    QAction* m_actOff = nullptr;
    QMenu* m_devsMenu = nullptr;
    QActionGroup* m_devsGroup = nullptr;
    QMap<int, QAction*> m_eqActions;
    QAction* m_gameAct = nullptr;
};

}  // namespace kalm
