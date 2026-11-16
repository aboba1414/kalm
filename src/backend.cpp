// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.hpp"

#include "transport.hpp"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QMetaObject>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QProcess>
#include <QRectF>
#include <QSet>
#include <QStandardPaths>
#include <QVariantMap>
#include <thread>

namespace kalm {

namespace {

QString wearLabelRu(const QString& wear) {
    if (wear == u"worn") return QStringLiteral("В ухе");
    if (wear == u"in_case") return QStringLiteral("В кейсе");
    if (wear == u"removed") return QStringLiteral("Снят");
    return {};
}

QString formatBattValue(const BatteryReading& r) {
    if (!r.level) return QStringLiteral("—");
    return r.charging ? QStringLiteral("%1% ⚡").arg(*r.level) : QStringLiteral("%1%").arg(*r.level);
}

}  // namespace

QIcon makeKalmIcon(bool connected, std::optional<NoiseMode> mode) {
    QPixmap px(64, 64);
    px.fill(Qt::transparent);

    QPainter painter(&px);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QColor ringColor("#64748B");
    QColor fillColor(100, 116, 139, 42);
    QColor badgeColor("#475569");

    if (connected) {
        if (mode == NoiseMode::Anc) {
            ringColor = QColor("#38BDF8");
            fillColor = QColor(56, 189, 248, 55);
            badgeColor = QColor("#2563EB");
        } else if (mode == NoiseMode::Transparency) {
            ringColor = QColor("#34D399");
            fillColor = QColor(52, 211, 153, 55);
            badgeColor = QColor("#059669");
        } else {
            ringColor = QColor("#CBD5E1");
            fillColor = QColor(203, 213, 225, 40);
            badgeColor = QColor("#64748B");
        }
    }

    painter.setPen(QPen(ringColor, 3.5));
    painter.setBrush(fillColor);
    painter.drawEllipse(QRectF(5, 5, 54, 54));

    const QColor fg = connected ? QColor("#F8FAFC") : QColor("#94A3B8");
    QPen earPen(fg, 3.2);
    earPen.setCapStyle(Qt::RoundCap);
    painter.setPen(earPen);
    painter.setBrush(fg);

    painter.drawEllipse(QRectF(16, 18, 12, 11));
    painter.drawLine(20, 29, 20, 44);
    painter.drawEllipse(QRectF(36, 18, 12, 11));
    painter.drawLine(44, 29, 44, 44);

    painter.setPen(QPen(QColor("#0F172A"), 2.5));
    painter.setBrush(badgeColor);
    painter.drawEllipse(QRectF(42, 42, 16, 16));

    if (connected && mode) {
        painter.setPen(QColor("#FFFFFF"));
        painter.setFont(QFont(QStringLiteral("Sans"), 8, QFont::Bold));
        QString letter = QStringLiteral("O");
        if (*mode == NoiseMode::Anc) letter = QStringLiteral("A");
        else if (*mode == NoiseMode::Transparency) letter = QStringLiteral("T");
        painter.drawText(QRectF(42, 42, 16, 16), Qt::AlignCenter, letter);
    }

    painter.end();
    return QIcon(px);
}

// ---- KalmBackend ----

KalmBackend::KalmBackend(KalmSession& session, DeviceRegistry& registry, QObject* parent)
    : QObject(parent),
      m_session(session),
      m_registry(registry),
      m_state(session.snapshot()) {
    for (const auto& e : audioEffectPresets()) {
        m_eqModel.append(QVariantMap{
            {QStringLiteral("text"), QString::fromUtf8(e.label)},
            {QStringLiteral("value"), e.code},
        });
    }
    for (const auto& c : noiseCyclePresets()) {
        m_cycleModel.append(QVariantMap{
            {QStringLiteral("text"), QString::fromUtf8(c.label)},
            {QStringLiteral("value"), c.code},
        });
    }
    refreshSavedDevicesAsync();
}

void KalmBackend::bindWindow(QWindow* win) {
    m_window = win;
    if (m_window) {
        m_window->setIcon(makeKalmIcon(m_state.connected, m_state.noiseMode));
    }
}

void KalmBackend::pushStateFromWorker(const EarbudState& st) {
    QMetaObject::invokeMethod(
        this,
        [this, st]() {
            m_state = st;
            if (!st.connected && m_ringing) {
                m_ringing = false;
                Q_EMIT ringingChanged();
            }
            if (m_window) {
                m_window->setIcon(makeKalmIcon(st.connected, st.noiseMode));
            }
            Q_EMIT stateChanged();
        },
        Qt::QueuedConnection);
}

void KalmBackend::pushDevicesRefreshFromWorker() {
    QMetaObject::invokeMethod(this, &KalmBackend::refreshSavedDevicesAsync, Qt::QueuedConnection);
}

void KalmBackend::pushErrorFromWorker(const QString& msg) {
    QMetaObject::invokeMethod(this, [this, msg]() { setBannerMain(msg, 2); }, Qt::QueuedConnection);
}

void KalmBackend::pushShowFromWorker() {
    QMetaObject::invokeMethod(this, &KalmBackend::windowShowRequested, Qt::QueuedConnection);
}

void KalmBackend::setBannerMain(const QString& text, int type) {
    m_bannerText = text;
    m_bannerType = type;
    Q_EMIT bannerChanged();
}

QString KalmBackend::deviceName() const {
    return m_state.deviceName.isEmpty() ? QStringLiteral("Kalm") : m_state.deviceName;
}

QString KalmBackend::statusSummary() const {
    if (m_state.connected) {
        QStringList parts{QStringLiteral("Подключено")};
        if (!m_state.firmware.isEmpty()) parts.append(QStringLiteral("FW %1").arg(m_state.firmware));
        if (!m_state.deviceMac.isEmpty()) parts.append(m_state.deviceMac);
        return parts.join(QStringLiteral(" · "));
    }
    if (!m_state.deviceMac.isEmpty()) {
        return QStringLiteral("Не подключено · %1").arg(m_state.deviceMac);
    }
    return QStringLiteral("Добавьте или выберите наушники");
}

int KalmBackend::noiseModeIndex() const {
    if (!m_state.connected || !m_state.noiseMode) return -1;
    return static_cast<int>(static_cast<uint8_t>(*m_state.noiseMode));
}

QString KalmBackend::leftBatteryText() const { return formatBattValue(m_state.leftBattery); }

QString KalmBackend::leftBatterySub() const {
    const QString w = wearLabelRu(m_state.leftWear);
    return w.isEmpty() ? QStringLiteral("Левый наушник") : QStringLiteral("Левый · %1").arg(w);
}

QString KalmBackend::rightBatteryText() const { return formatBattValue(m_state.rightBattery); }

QString KalmBackend::rightBatterySub() const {
    const QString w = wearLabelRu(m_state.rightWear);
    return w.isEmpty() ? QStringLiteral("Правый наушник") : QStringLiteral("Правый · %1").arg(w);
}

QString KalmBackend::caseBatteryText() const { return formatBattValue(m_state.caseBattery); }

QString KalmBackend::caseBatterySub() const {
    if (m_state.caseBattery.level && m_state.caseBattery.stale) {
        return QStringLiteral("Кейс (кэш)");
    }
    return QStringLiteral("Зарядный кейс");
}

int KalmBackend::eqIndex() const {
    if (!m_state.audioEffect) return 0;
    const auto& list = audioEffectPresets();
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].code == *m_state.audioEffect) return static_cast<int>(i);
    }
    return 0;
}

int KalmBackend::cycleIndex() const {
    if (!m_state.noiseCycleLeft) return 0;
    const auto& list = noiseCyclePresets();
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].code == *m_state.noiseCycleLeft) return static_cast<int>(i);
    }
    return 0;
}

QVariantList KalmBackend::multipointDevices() const {
    QVariantList out;
    for (const auto& d : m_state.pairedDevices) {
        out.append(QVariantMap{
            {QStringLiteral("mac"), d.macStr()},
            {QStringLiteral("name"), d.name.isEmpty() ? d.macStr() : d.name},
            {QStringLiteral("statusText"), d.statusRu()},
            {QStringLiteral("connected"), d.connected()},
            {QStringLiteral("active"), d.active()},
        });
    }
    return out;
}

void KalmBackend::runBg(std::function<void()> fn) {
    std::thread([this, fn = std::move(fn)]() {
        try {
            fn();
        } catch (const std::exception& ex) {
            const QString err = QString::fromUtf8(ex.what());
            QMetaObject::invokeMethod(this, [this, err]() { setBannerMain(err, 3); }, Qt::QueuedConnection);
        }
    }).detach();
}

void KalmBackend::refreshSavedDevicesAsync() {
    std::thread([this]() {
        QMap<QString, BluezDevice> bmap;
        for (const auto& b : listBluezDevices(false, false)) {
            bmap.insert(b.mac, b);
        }
        const QString activeMac = m_registry.activeMac();
        const auto saved = m_registry.devices();
        QVariantList items;
        for (const auto& d : saved) {
            const bool hasB = bmap.contains(d.mac);
            const auto b = hasB ? bmap.value(d.mac) : BluezDevice{};
            items.append(QVariantMap{
                {QStringLiteral("mac"), d.mac},
                {QStringLiteral("displayName"), d.displayName()},
                {QStringLiteral("rawName"), d.name},
                {QStringLiteral("alias"), d.alias},
                {QStringLiteral("active"), d.mac == activeMac},
                {QStringLiteral("btConnected"), hasB && b.connected},
                {QStringLiteral("btPaired"), hasB && b.paired},
            });
        }
        QMetaObject::invokeMethod(
            this,
            [this, items]() {
                m_savedList = items;
                Q_EMIT devicesChanged();
            },
            Qt::QueuedConnection);
    }).detach();
}

void KalmBackend::setNoiseByIndex(int index) {
    if (index < 0 || index > 2) return;
    const auto mode = static_cast<NoiseMode>(index);
    m_state.noiseMode = mode;
    Q_EMIT stateChanged();
    runBg([this, mode]() { m_session.setNoiseMode(mode); });
}

void KalmBackend::setEqByIndex(int index) {
    const auto& list = audioEffectPresets();
    if (index < 0 || static_cast<size_t>(index) >= list.size()) return;
    const int code = list[static_cast<size_t>(index)].code;
    runBg([this, code]() { m_session.setAudioEffect(code); });
}

void KalmBackend::setCycleByIndex(int index) {
    const auto& list = noiseCyclePresets();
    if (index < 0 || static_cast<size_t>(index) >= list.size()) return;
    const int code = list[static_cast<size_t>(index)].code;
    runBg([this, code]() { m_session.setNoiseCycle(code); });
}

void KalmBackend::setGameMode(bool enabled) {
    runBg([this, enabled]() { m_session.setLowLatency(enabled); });
}

void KalmBackend::setWearDetection(bool enabled) {
    runBg([this, enabled]() { m_session.setWearDetection(enabled); });
}

void KalmBackend::toggleMultipointDevice(const QString& mac, bool connect) {
    runBg([this, mac, connect]() { m_session.setDeviceConnection(mac, connect); });
}

void KalmBackend::startFindEarbuds() {
    m_ringing = true;
    Q_EMIT ringingChanged();
    runBg([this]() { m_session.findEarbuds(true); });
}

void KalmBackend::stopFindEarbuds() {
    m_ringing = false;
    Q_EMIT ringingChanged();
    runBg([this]() { m_session.findEarbuds(false); });
}

void KalmBackend::refreshNow() {
    if (m_state.connected) {
        runBg([this]() { m_session.refresh(); });
    } else {
        m_session.forceReconnect();
    }
    refreshSavedDevicesAsync();
}

void KalmBackend::scanBluetoothDevices() {
    std::thread([this]() {
        const auto devs = listBluezDevices(false, true);
        QSet<QString> savedMacs;
        for (const auto& s : m_registry.devices()) savedMacs.insert(s.mac);
        QVariantList out;
        for (const auto& b : devs) {
            QStringList parts{b.mac};
            if (b.isVivoTws()) parts.append(QStringLiteral("vivo/iQOO GAIA"));
            parts.append(b.connected ? QStringLiteral("Подключено") : QStringLiteral("Сопряжено"));
            const bool already = savedMacs.contains(b.mac);
            if (already) parts.append(QStringLiteral("В списке"));
            out.append(QVariantMap{
                {QStringLiteral("mac"), b.mac},
                {QStringLiteral("name"), b.name},
                {QStringLiteral("isVivo"), b.isVivoTws()},
                {QStringLiteral("connected"), b.connected},
                {QStringLiteral("alreadySaved"), already},
                {QStringLiteral("subtitle"), parts.join(QStringLiteral(" · "))},
            });
        }
        QMetaObject::invokeMethod(
            this,
            [this, out]() {
                m_discoveredList = out;
                Q_EMIT discoveredChanged();
            },
            Qt::QueuedConnection);
    }).detach();
}

bool KalmBackend::addSavedDevice(const QString& mac, const QString& name, const QString& alias) {
    SavedEarbud entry;
    try {
        entry = m_registry.add(mac, name, alias, true);
    } catch (const std::exception& ex) {
        setBannerMain(QString::fromUtf8(ex.what()), 3);
        return false;
    }
    m_session.switchToDevice(entry.mac);
    refreshSavedDevicesAsync();
    setBannerMain(QStringLiteral("Выбраны наушники: %1").arg(entry.displayName()), 1);
    return true;
}

void KalmBackend::removeSavedDevice(const QString& mac) {
    m_registry.remove(mac);
    m_session.notifyRegistryUpdated();
    refreshSavedDevicesAsync();
}

void KalmBackend::renameSavedDevice(const QString& mac, const QString& alias) {
    m_registry.rename(mac, alias);
    m_session.notifyRegistryUpdated();
    refreshSavedDevicesAsync();
}

void KalmBackend::selectSavedDevice(const QString& mac) {
    m_session.switchToDevice(mac);
    refreshSavedDevicesAsync();
}

void KalmBackend::connectBluetoothDevice(const QString& mac) {
    setBannerMain(QStringLiteral("Подключение по Bluetooth к %1…").arg(mac), 0);
    std::thread([this, mac]() {
        const bool ok = connectBluezDevice(mac);
        if (ok) {
            m_session.forceReconnect();
            QMetaObject::invokeMethod(
                this,
                [this]() { setBannerMain(QStringLiteral("Bluetooth-соединение установлено"), 1); },
                Qt::QueuedConnection);
        } else {
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    setBannerMain(
                        QStringLiteral("Не удалось подключиться по Bluetooth. Проверьте, открыт ли кейс."),
                        2);
                },
                Qt::QueuedConnection);
        }
        refreshSavedDevicesAsync();
    }).detach();
}

void KalmBackend::setAutoSwitch(bool enabled) {
    m_registry.setAutoSwitch(enabled);
    Q_EMIT devicesChanged();
}

void KalmBackend::openSystemBluetoothSettings() {
    const QList<QStringList> cmds = {
        {QStringLiteral("systemsettings"), QStringLiteral("kcm_bluetooth")},
        {QStringLiteral("kcmshell6"), QStringLiteral("kcm_bluetooth")},
        {QStringLiteral("bluedevil-wizard")},
    };
    for (const QStringList& cmd : cmds) {
        if (!QStandardPaths::findExecutable(cmd.first()).isEmpty()) {
            QProcess::startDetached(cmd.first(), cmd.mid(1));
            return;
        }
    }
}

void KalmBackend::clearBanner() {
    m_bannerText.clear();
    Q_EMIT bannerChanged();
}

void KalmBackend::quitApp() {
    m_quitting = true;
    Q_EMIT quittingChanged();
    if (auto* app = QApplication::instance()) {
        app->quit();
    }
}

// ---- KalmTrayController ----

KalmTrayController::KalmTrayController(KalmSession& session, KalmBackend& backend, QObject* parent)
    : QObject(parent),
      m_session(session),
      m_backend(backend),
      m_tray(makeKalmIcon(false, std::nullopt), this) {
    m_tray.setToolTip(QStringLiteral("Kalm — Ожидание подключения"));

    m_headerAct = m_menu.addAction(QStringLiteral("Kalm"));
    m_headerAct->setEnabled(false);
    m_battAct = m_menu.addAction(QStringLiteral("Заряд: —"));
    m_battAct->setEnabled(false);
    m_menu.addSeparator();

    auto* modeGroup = new QActionGroup(&m_menu);
    modeGroup->setExclusive(true);

    m_actAnc = m_menu.addAction(QStringLiteral("Шумоподавление (ANC)"));
    m_actAnc->setCheckable(true);
    modeGroup->addAction(m_actAnc);
    connect(m_actAnc, &QAction::triggered, this, [this]() { m_backend.setNoiseByIndex(0); });

    m_actTrans = m_menu.addAction(QStringLiteral("Прозрачность"));
    m_actTrans->setCheckable(true);
    modeGroup->addAction(m_actTrans);
    connect(m_actTrans, &QAction::triggered, this, [this]() { m_backend.setNoiseByIndex(2); });

    m_actOff = m_menu.addAction(QStringLiteral("Выключено"));
    m_actOff->setCheckable(true);
    modeGroup->addAction(m_actOff);
    connect(m_actOff, &QAction::triggered, this, [this]() { m_backend.setNoiseByIndex(1); });

    m_menu.addSeparator();

    m_devsMenu = m_menu.addMenu(QStringLiteral("Мои наушники"));
    m_devsGroup = new QActionGroup(m_devsMenu);
    m_devsGroup->setExclusive(true);

    QMenu* eqMenu = m_menu.addMenu(QStringLiteral("Звук (DeepX)"));
    auto* eqGroup = new QActionGroup(eqMenu);
    eqGroup->setExclusive(true);
    const auto& presets = audioEffectPresets();
    for (size_t i = 0; i < presets.size(); ++i) {
        QAction* act = eqMenu->addAction(QString::fromUtf8(presets[i].label));
        act->setCheckable(true);
        eqGroup->addAction(act);
        const int idx = static_cast<int>(i);
        connect(act, &QAction::triggered, this, [this, idx]() { m_backend.setEqByIndex(idx); });
        m_eqActions.insert(presets[i].code, act);
    }

    m_gameAct = m_menu.addAction(QStringLiteral("Игровой режим"));
    m_gameAct->setCheckable(true);
    connect(m_gameAct, &QAction::triggered, this, [this](bool checked) { m_backend.setGameMode(checked); });

    m_menu.addSeparator();
    QAction* openAct = m_menu.addAction(QStringLiteral("Открыть Kalm"));
    connect(openAct, &QAction::triggered, &m_backend, &KalmBackend::windowShowRequested);

    QAction* quitAct = m_menu.addAction(QStringLiteral("Выход"));
    connect(quitAct, &QAction::triggered, &m_backend, &KalmBackend::quitApp);

    m_tray.setContextMenu(&m_menu);
    connect(&m_tray, &QSystemTrayIcon::activated, this, &KalmTrayController::onActivated);
    connect(&m_backend, &KalmBackend::stateChanged, this, &KalmTrayController::syncState);
    connect(&m_backend, &KalmBackend::devicesChanged, this, &KalmTrayController::syncDevicesMenu);

    m_tray.show();
    syncState();
    syncDevicesMenu();
}

void KalmTrayController::onActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger) {
        if (m_window && m_window->isVisible()) {
            m_window->setVisible(false);
        } else {
            Q_EMIT m_backend.windowShowRequested();
        }
    } else if (reason == QSystemTrayIcon::MiddleClick) {
        std::thread([this]() { m_session.toggleNoiseMode(); }).detach();
    }
}

void KalmTrayController::syncDevicesMenu() {
    m_devsMenu->clear();
    for (QAction* a : m_devsGroup->actions()) {
        m_devsGroup->removeAction(a);
    }

    const QVariantList items = m_backend.savedDevices();
    if (items.isEmpty()) {
        QAction* empty = m_devsMenu->addAction(QStringLiteral("Нет сохранённых наушников"));
        empty->setEnabled(false);
        return;
    }

    for (const QVariant& v : items) {
        const QVariantMap d = v.toMap();
        const QString suffix = d.value(QStringLiteral("btConnected")).toBool() ? QStringLiteral(" (онлайн)") : QString{};
        QAction* act = m_devsMenu->addAction(d.value(QStringLiteral("displayName")).toString() + suffix);
        act->setCheckable(true);
        act->setChecked(d.value(QStringLiteral("active")).toBool());
        const QString mac = d.value(QStringLiteral("mac")).toString();
        connect(act, &QAction::triggered, this, [this, mac]() { m_backend.selectSavedDevice(mac); });
        m_devsGroup->addAction(act);
    }
}

void KalmTrayController::syncState() {
    const EarbudState st = m_session.snapshot();
    m_tray.setIcon(makeKalmIcon(st.connected, st.noiseMode));

    const QString name = st.deviceName.isEmpty() ? QStringLiteral("Kalm") : st.deviceName;
    if (!st.connected) {
        m_tray.setToolTip(QStringLiteral("%1 — Не подключено").arg(name));
        m_headerAct->setText(QStringLiteral("%1 — Не подключено").arg(name));
        m_battAct->setText(QStringLiteral("Заряд: —"));
        for (QAction* a : {m_actAnc, m_actTrans, m_actOff, m_gameAct}) a->setEnabled(false);
        return;
    }

    const QString modeTxt = st.noiseMode ? noiseModeLabelRu(*st.noiseMode) : QStringLiteral("—");
    QStringList bParts;
    if (st.leftBattery.level) bParts.append(QStringLiteral("Л: %1%").arg(*st.leftBattery.level));
    if (st.rightBattery.level) bParts.append(QStringLiteral("П: %1%").arg(*st.rightBattery.level));
    if (st.caseBattery.level && !st.caseBattery.stale) {
        bParts.append(QStringLiteral("Кейс: %1%").arg(*st.caseBattery.level));
    }
    const QString battStr = bParts.isEmpty() ? QStringLiteral("—") : bParts.join(QStringLiteral("  "));

    m_tray.setToolTip(QStringLiteral("%1\nРежим: %2\n%3").arg(name, modeTxt, battStr));
    m_headerAct->setText(QStringLiteral("%1 (%2)").arg(name, modeTxt));
    m_battAct->setText(QStringLiteral("Заряд: %1").arg(battStr));

    for (QAction* a : {m_actAnc, m_actTrans, m_actOff, m_gameAct}) a->setEnabled(true);

    if (st.noiseMode == NoiseMode::Anc) m_actAnc->setChecked(true);
    else if (st.noiseMode == NoiseMode::Off) m_actOff->setChecked(true);
    else if (st.noiseMode == NoiseMode::Transparency) m_actTrans->setChecked(true);

    if (st.audioEffect && m_eqActions.contains(*st.audioEffect)) {
        m_eqActions.value(*st.audioEffect)->setChecked(true);
    }
    if (st.lowLatency) {
        m_gameAct->setChecked(*st.lowLatency);
    }
}

}  // namespace kalm
