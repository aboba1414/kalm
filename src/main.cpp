// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.hpp"
#include "protocol.hpp"
#include "registry.hpp"
#include "session.hpp"
#include "transport.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include <csignal>
#include <iostream>

namespace {

kalm::KalmBackend* g_backendInstance = nullptr;

void handleUnixSignal(int) {
    if (g_backendInstance) {
        QMetaObject::invokeMethod(g_backendInstance, &kalm::KalmBackend::quitApp, Qt::QueuedConnection);
    }
}

void notifyDesktop(const QString& title, const QString& body) {
    const QString exe = QStandardPaths::findExecutable(QStringLiteral("notify-send"));
    if (exe.isEmpty()) return;
    QProcess::startDetached(exe, {
                                     QStringLiteral("-a"),
                                     QStringLiteral("Kalm"),
                                     QStringLiteral("-i"),
                                     QStringLiteral("audio-headset"),
                                     QStringLiteral("-h"),
                                     QStringLiteral("string:x-canonical-private-synchronous:kalm"),
                                     title,
                                     body,
                                 });
}

QString formatBattCli(const QJsonValue& levelVal, bool charging, bool stale) {
    if (levelVal.isNull() || levelVal.isUndefined()) return QStringLiteral("—");
    const int lvl = levelVal.toInt();
    const QString suffix = charging ? QStringLiteral(" ⚡") : (stale ? QStringLiteral(" (устар.)") : QString{});
    return QStringLiteral("%1%%2").arg(lvl).arg(suffix);
}

QString wearCliRu(const QString& w) {
    if (w == u"worn") return QStringLiteral("в ухе");
    if (w == u"in_case") return QStringLiteral("в кейсе");
    if (w == u"removed") return QStringLiteral("снят");
    return QStringLiteral("—");
}

void printStatusHuman(const QJsonObject& data) {
    QTextStream out(stdout);
    if (!data.value(QStringLiteral("connected")).toBool()) {
        out << "Статус: Не подключено\n";
        return;
    }

    const QString name = data.value(QStringLiteral("device_name")).toString(QStringLiteral("Наушники"));
    const QString mac = data.value(QStringLiteral("device_mac")).toString();
    const QString fw = data.value(QStringLiteral("firmware")).toString(QStringLiteral("—"));
    const QString mode = data.value(QStringLiteral("noise_mode_label")).toString(QStringLiteral("—"));
    const QJsonObject batt = data.value(QStringLiteral("battery")).toObject();
    const QJsonObject wear = data.value(QStringLiteral("wear")).toObject();

    const QString leftB = formatBattCli(batt.value(QStringLiteral("left")),
                                        batt.value(QStringLiteral("left_charging")).toBool(),
                                        batt.value(QStringLiteral("left_stale")).toBool());
    const QString rightB = formatBattCli(batt.value(QStringLiteral("right")),
                                         batt.value(QStringLiteral("right_charging")).toBool(),
                                         batt.value(QStringLiteral("right_stale")).toBool());
    const QString caseB = formatBattCli(batt.value(QStringLiteral("case")),
                                        batt.value(QStringLiteral("case_charging")).toBool(),
                                        batt.value(QStringLiteral("case_stale")).toBool());

    out << "Наушники:        " << name << " (" << mac << ")\n";
    out << "Прошивка:        " << (fw.isEmpty() ? QStringLiteral("—") : fw) << "\n";
    out << "Шумоподавление:  " << mode << "\n";
    out << "Заряд:           Л: " << leftB << "   П: " << rightB << "   Кейс: " << caseB << "\n";
    out << "Положение:       Л: " << wearCliRu(wear.value(QStringLiteral("left")).toString())
        << "   П: " << wearCliRu(wear.value(QStringLiteral("right")).toString()) << "\n";

    const QString eq = data.value(QStringLiteral("audio_effect_label")).toString();
    if (!eq.isEmpty()) out << "Звук (DeepX):    " << eq << "\n";

    if (!data.value(QStringLiteral("low_latency")).isNull()) {
        out << "Игровой режим:   "
            << (data.value(QStringLiteral("low_latency")).toBool() ? QStringLiteral("Включён") : QStringLiteral("Выключен"))
            << "\n";
    }

    const QString nc = data.value(QStringLiteral("noise_cycle")).toObject().value(QStringLiteral("label")).toString();
    if (!nc.isEmpty()) out << "Долгое нажатие:  " << nc << "\n";

    const QJsonArray devs = data.value(QStringLiteral("paired_devices")).toArray();
    if (!devs.isEmpty()) {
        out << "Мультипоинт:\n";
        for (const QJsonValue& v : devs) {
            const QJsonObject d = v.toObject();
            const QString mark = d.value(QStringLiteral("connected")).toBool() ? QStringLiteral("●") : QStringLiteral("○");
            out << "  " << mark << " " << d.value(QStringLiteral("name")).toString()
                << " (" << d.value(QStringLiteral("mac")).toString() << ") — "
                << d.value(QStringLiteral("status_label")).toString() << "\n";
        }
    }
}

QString battSummaryShort(const QJsonObject& data) {
    const QJsonObject b = data.value(QStringLiteral("battery")).toObject();
    QStringList parts;
    if (!b.value(QStringLiteral("left")).isNull()) {
        parts.append(QStringLiteral("Л %1%").arg(b.value(QStringLiteral("left")).toInt()));
    }
    if (!b.value(QStringLiteral("right")).isNull()) {
        parts.append(QStringLiteral("П %1%").arg(b.value(QStringLiteral("right")).toInt()));
    }
    if (!b.value(QStringLiteral("case")).isNull() && !b.value(QStringLiteral("case_stale")).toBool()) {
        parts.append(QStringLiteral("Кейс %1%").arg(b.value(QStringLiteral("case")).toInt()));
    }
    return parts.join(QStringLiteral(" · "));
}

std::optional<int> parseEqPreset(const QString& raw) {
    const QString k = raw.trimmed().toLower();
    if (k == u"0" || k == u"standard" || k == u"стандартный") return 0x00;
    if (k == u"1" || k == u"vocal" || k == u"voice" || k == u"вокал") return 0x01;
    if (k == u"2" || k == u"bass" || k == u"бас") return 0x02;
    if (k == u"3" || k == u"treble" || k == u"highs" || k == u"высокие") return 0x03;
    if (k == u"5" || k == u"audiobook" || k == u"книги") return 0x05;
    if (k == u"11" || k == u"master" || k == u"hifi" || k == u"мастер") return 0x0B;
    return std::nullopt;
}

void printHelp() {
    QTextStream out(stdout);
    out << "Kalm v0.1 — управление шумоподавлением и настройками наушников vivo / iQOO TWS\n\n"
        << "Использование:\n"
        << "  kalm [--minimized] [-d <MAC|имя>]            Открыть графическое приложение (Qt 6 + Kirigami)\n"
        << "  kalm status [--json] [-d <MAC|имя>]          Показать заряд, режим и статус наушников\n"
        << "  kalm anc [--notify] [-d <MAC|имя>]           Включить активное шумоподавление (ANC)\n"
        << "  kalm trans [--notify] [-d <MAC|имя>]         Включить режим прозрачности\n"
        << "  kalm off [--notify] [-d <MAC|имя>]           Выключить шумоподавление\n"
        << "  kalm toggle [--modes a,b] [--notify]         Циклически переключить режим\n"
        << "  kalm eq <standard|vocal|bass|treble|audiobook|master>\n"
        << "  kalm game <on|off>\n"
        << "  kalm devices [list|scan|add <MAC> [--alias <имя>]|use <цель>|rename <цель> <имя>|remove <цель>]\n";
}

int launchGui(const QString& targetDevice, bool startMinimized, int argc, char** argv) {
    if (targetDevice.isEmpty()) {
        const auto existing = kalm::ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("show")}}, 500);
        if (existing && existing->value(QStringLiteral("ok")).toBool()) {
            return 0;
        }
    }

    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        qputenv("QT_QUICK_CONTROLS_STYLE", "org.kde.desktop");
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Kalm"));
    app.setApplicationDisplayName(QStringLiteral("Kalm"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    app.setDesktopFileName(QStringLiteral("io.github.kalm"));
    app.setQuitOnLastWindowClosed(false);
    app.setWindowIcon(kalm::makeKalmIcon(false, std::nullopt));

    kalm::DeviceRegistry registry;
    registry.ensureInitialImport();

    QString overrideMac;
    if (!targetDevice.isEmpty()) {
        if (auto resolved = registry.resolveQuery(targetDevice)) {
            registry.setActive(resolved->mac);
            overrideMac = resolved->mac;
        }
    }

    kalm::KalmBackend* backendPtr = nullptr;
    kalm::KalmSession session(
        registry,
        overrideMac,
        [&](const kalm::EarbudState& st) {
            if (backendPtr) backendPtr->pushStateFromWorker(st);
        },
        [&]() {
            if (backendPtr) backendPtr->pushDevicesRefreshFromWorker();
        },
        [&](const QString& err) {
            if (backendPtr) backendPtr->pushErrorFromWorker(err);
        });

    kalm::KalmBackend backend(session, registry);
    backendPtr = &backend;
    g_backendInstance = &backend;

    kalm::KalmTrayController tray(session, backend);
    kalm::IpcServer ipc(session, [&]() { backend.pushShowFromWorker(); });
    ipc.start();
    session.start();

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        std::cerr << "Ошибка загрузки интерфейса Kirigami QML.\n";
        return 1;
    }

    if (auto* win = qobject_cast<QWindow*>(engine.rootObjects().first())) {
        backend.bindWindow(win);
        tray.bindWindow(win);
        win->setVisible(!startMinimized);
    }

    QTimer btTimer;
    btTimer.setInterval(6000);
    QObject::connect(&btTimer, &QTimer::timeout, &backend, &kalm::KalmBackend::refreshSavedDevicesAsync);
    btTimer.start();

    std::signal(SIGINT, handleUnixSignal);
    std::signal(SIGTERM, handleUnixSignal);

    const int rc = app.exec();
    g_backendInstance = nullptr;
    backendPtr = nullptr;
    tray.hide();
    ipc.stop();
    session.stop();
    return rc;
}

int handleDevicesSubcmd(const QStringList& rest) {
    QTextStream out(stdout);
    kalm::DeviceRegistry reg;
    reg.ensureInitialImport();
    const QString action = rest.isEmpty() ? QStringLiteral("list") : rest.first();

    if (action == u"list") {
        const auto devs = reg.devices();
        if (devs.isEmpty()) {
            out << "Сохранённых наушников пока нет. Используйте: kalm devices scan / kalm devices add <MAC>\n";
            return 0;
        }
        QMap<QString, kalm::BluezDevice> bluez;
        for (const auto& b : kalm::listBluezDevices(false, false)) bluez.insert(b.mac, b);
        const QString actMac = reg.activeMac();
        out << "Сохранённые наушники:\n";
        for (const auto& d : devs) {
            const QString mark = (d.mac == actMac) ? QStringLiteral("★") : QStringLiteral(" ");
            const bool hasB = bluez.contains(d.mac);
            const QString btStatus = (hasB && bluez[d.mac].connected)
                                         ? QStringLiteral("Подключено")
                                         : (hasB ? QStringLiteral("Сопряжено") : QStringLiteral("Не найдено в BT"));
            const QString aliasNote =
                (!d.alias.isEmpty() && d.alias != d.name) ? QStringLiteral(" [%1]").arg(d.name) : QString{};
            out << "  " << mark << " " << d.displayName() << aliasNote << " — " << d.mac << " (" << btStatus << ")\n";
        }
        return 0;
    }

    if (action == u"scan") {
        const auto found = kalm::listBluezDevices(false, true);
        if (found.isEmpty()) {
            out << "Аудиоустройства Bluetooth не найдены.\n";
            return 0;
        }
        QSet<QString> saved;
        for (const auto& s : reg.devices()) saved.insert(s.mac);
        out << "Доступные аудиоустройства Bluetooth:\n";
        for (const auto& b : found) {
            const QString tag = b.isVivoTws() ? QStringLiteral("vivo/iQOO GAIA") : QStringLiteral("Bluetooth Audio");
            const QString conn = b.connected ? QStringLiteral("Подключено") : QStringLiteral("Не подключено");
            const QString sv = saved.contains(b.mac) ? QStringLiteral(" [уже добавлено]") : QString{};
            out << "  • " << b.name << " — " << b.mac << " (" << tag << ", " << conn << ")" << sv << "\n";
        }
        return 0;
    }

    if (action == u"add") {
        if (rest.size() < 2) throw std::runtime_error("Укажите MAC-адрес: kalm devices add <MAC> [--alias <имя>]");
        const QString mac = rest[1];
        QString alias;
        for (qsizetype i = 2; i < rest.size(); ++i) {
            if (rest[i] == u"--alias" && i + 1 < rest.size()) {
                alias = rest[++i];
            }
        }
        const auto bdev = kalm::findBluezByMac(mac);
        const auto entry = reg.add(mac, bdev ? bdev->name : mac.toUpper(), alias, true);
        kalm::ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("reload_registry")}});
        out << "Добавлены и выбраны наушники: " << entry.displayName() << " (" << entry.mac << ")\n";
        return 0;
    }

    if (action == u"remove") {
        if (rest.size() < 2) throw std::runtime_error("Укажите цель: kalm devices remove <MAC|имя>");
        const auto target = reg.resolveQuery(rest[1]);
        if (!target || !reg.remove(target->mac)) {
            throw std::runtime_error(QStringLiteral("Наушники '%1' не найдены в списке.").arg(rest[1]).toStdString());
        }
        kalm::ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("reload_registry")}});
        out << "Удалено: " << target->displayName() << " (" << target->mac << ")\n";
        return 0;
    }

    if (action == u"use") {
        if (rest.size() < 2) throw std::runtime_error("Укажите цель: kalm devices use <MAC|имя>");
        const auto target = reg.resolveQuery(rest[1]);
        if (!target || !reg.setActive(target->mac)) {
            throw std::runtime_error(QStringLiteral("Наушники '%1' не найдены в списке.").arg(rest[1]).toStdString());
        }
        kalm::ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("select_device")},
                                     {QStringLiteral("mac"), target->mac}});
        out << "Активные наушники: " << target->displayName() << " (" << target->mac << ")\n";
        return 0;
    }

    if (action == u"rename") {
        if (rest.size() < 3) throw std::runtime_error("Укажите цель и новое имя: kalm devices rename <MAC|имя> <новое_имя>");
        const auto target = reg.resolveQuery(rest[1]);
        if (!target || !reg.rename(target->mac, rest[2])) {
            throw std::runtime_error(QStringLiteral("Наушники '%1' не найдены в списке.").arg(rest[1]).toStdString());
        }
        kalm::ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("reload_registry")}});
        out << "Переименовано: " << (rest[2].isEmpty() ? target->name : rest[2]) << " (" << target->mac << ")\n";
        return 0;
    }

    throw std::runtime_error(QStringLiteral("Неизвестное действие devices: %1").arg(action).toStdString());
}

}  // namespace

int main(int argc, char** argv) {
    QStringList rawArgs;
    for (int i = 1; i < argc; ++i) rawArgs.append(QString::fromLocal8Bit(argv[i]));

    bool startMinimized = false;
    bool jsonOut = false;
    bool notify = false;
    QString deviceQuery;
    QString modesCsv = QStringLiteral("anc,trans,off");
    QStringList positional;

    for (qsizetype i = 0; i < rawArgs.size(); ++i) {
        const QString& a = rawArgs[i];
        if (a == u"-h" || a == u"--help") {
            printHelp();
            return 0;
        }
        if (a == u"-v" || a == u"--version") {
            std::cout << "kalm 0.1.0\n";
            return 0;
        }
        if (a == u"--minimized") {
            startMinimized = true;
        } else if (a == u"--json") {
            jsonOut = true;
        } else if (a == u"--notify") {
            notify = true;
        } else if ((a == u"-d" || a == u"--device" || a == u"--mac") && i + 1 < rawArgs.size()) {
            deviceQuery = rawArgs[++i];
        } else if (a == u"--modes" && i + 1 < rawArgs.size()) {
            modesCsv = rawArgs[++i];
        } else {
            positional.append(a);
        }
    }

    const QString subcmd = positional.isEmpty() ? QStringLiteral("gui") : positional.first();
    if (subcmd == u"gui") {
        return launchGui(deviceQuery, startMinimized, argc, argv);
    }

    // Lightweight QCoreApplication for CLI D-Bus / SDP operations
    QCoreApplication coreApp(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    try {
        if (subcmd == u"devices") {
            return handleDevicesSubcmd(positional.mid(1));
        }

        kalm::DeviceRegistry reg;

        if (subcmd == u"status") {
            std::optional<QJsonObject> ipc;
            if (deviceQuery.isEmpty()) {
                ipc = kalm::ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("status")}});
            }
            const QJsonObject state =
                (ipc && ipc->value(QStringLiteral("ok")).toBool() &&
                 ipc->value(QStringLiteral("state")).toObject().value(QStringLiteral("connected")).toBool())
                    ? ipc->value(QStringLiteral("state")).toObject()
                    : kalm::oneshotStatus(reg, deviceQuery);
            if (jsonOut) {
                out << QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Indented));
            } else {
                printStatusHuman(state);
            }
            return 0;
        }

        if (subcmd == u"anc" || subcmd == u"trans" || subcmd == u"off" || subcmd == u"mode") {
            const QString rawTarget = (subcmd == u"mode")
                                          ? (positional.size() >= 2 ? positional[1] : QString{})
                                          : subcmd;
            const auto mode = kalm::parseNoiseModeStr(rawTarget);
            if (!mode) {
                throw std::runtime_error(
                    QStringLiteral("Неизвестный режим '%1'. Доступны: anc, trans, off").arg(rawTarget).toStdString());
            }
            QJsonObject state;
            const auto ipc = deviceQuery.isEmpty()
                                 ? kalm::ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("set_noise")},
                                                                {QStringLiteral("mode"), kalm::noiseModeSlug(*mode)}})
                                 : std::nullopt;
            if (ipc) {
                if (!ipc->value(QStringLiteral("ok")).toBool()) {
                    throw std::runtime_error(ipc->value(QStringLiteral("error")).toString().toStdString());
                }
                state = ipc->value(QStringLiteral("state")).toObject();
            } else {
                state = kalm::oneshotSetNoise(reg, *mode, deviceQuery);
            }
            const QString label = state.value(QStringLiteral("noise_mode_label")).toString(kalm::noiseModeLabelRu(*mode));
            out << "Режим установлен: " << label << "\n";
            if (notify) {
                const QString summary = battSummaryShort(state);
                notifyDesktop(QStringLiteral("Режим: %1").arg(label),
                              summary.isEmpty() ? state.value(QStringLiteral("device_name")).toString(QStringLiteral("Kalm")) : summary);
            }
            return 0;
        }

        if (subcmd == u"toggle") {
            QList<kalm::NoiseMode> modesList;
            QJsonArray modesJson;
            for (const QString& tok : modesCsv.split(u',', Qt::SkipEmptyParts)) {
                const auto m = kalm::parseNoiseModeStr(tok);
                if (!m) {
                    throw std::runtime_error(QStringLiteral("Неизвестный режим '%1'").arg(tok).toStdString());
                }
                modesList.append(*m);
                modesJson.append(kalm::noiseModeSlug(*m));
            }
            if (modesList.size() < 2) {
                throw std::runtime_error("Укажите минимум 2 режима для toggle (например --modes anc,trans)");
            }
            QJsonObject state;
            const auto ipc = deviceQuery.isEmpty()
                                 ? kalm::ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("toggle_noise")},
                                                                {QStringLiteral("modes"), modesJson}})
                                 : std::nullopt;
            if (ipc) {
                if (!ipc->value(QStringLiteral("ok")).toBool()) {
                    throw std::runtime_error(ipc->value(QStringLiteral("error")).toString().toStdString());
                }
                state = ipc->value(QStringLiteral("state")).toObject();
            } else {
                state = kalm::oneshotToggleNoise(reg, modesList, deviceQuery);
            }
            const QString label = state.value(QStringLiteral("noise_mode_label")).toString(QStringLiteral("—"));
            out << "Режим переключён: " << label << "\n";
            if (notify) {
                const QString summary = battSummaryShort(state);
                notifyDesktop(QStringLiteral("Режим: %1").arg(label),
                              summary.isEmpty() ? state.value(QStringLiteral("device_name")).toString(QStringLiteral("Kalm")) : summary);
            }
            return 0;
        }

        if (subcmd == u"eq") {
            if (positional.size() < 2) {
                throw std::runtime_error("Укажите пресет: standard | vocal | bass | treble | audiobook | master");
            }
            const auto eff = parseEqPreset(positional[1]);
            if (!eff) {
                throw std::runtime_error(
                    QStringLiteral("Неизвестный пресет '%1'. Доступны: standard, vocal, bass, treble, audiobook, master")
                        .arg(positional[1])
                        .toStdString());
            }
            const auto ipc = deviceQuery.isEmpty()
                                 ? kalm::ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("set_eq")},
                                                                {QStringLiteral("effect"), *eff}})
                                 : std::nullopt;
            if (!ipc || !ipc->value(QStringLiteral("ok")).toBool()) {
                kalm::oneshotSetEq(reg, *eff, deviceQuery);
            }
            out << "Эквалайзер: " << kalm::audioEffectLabel(*eff) << "\n";
            return 0;
        }

        if (subcmd == u"game") {
            if (positional.size() < 2 || (positional[1] != u"on" && positional[1] != u"off")) {
                throw std::runtime_error("Укажите состояние: kalm game on | off");
            }
            const bool en = (positional[1] == u"on");
            const auto ipc = deviceQuery.isEmpty()
                                 ? kalm::ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("set_game")},
                                                                {QStringLiteral("enabled"), en}})
                                 : std::nullopt;
            if (!ipc || !ipc->value(QStringLiteral("ok")).toBool()) {
                kalm::oneshotSetGame(reg, en, deviceQuery);
            }
            out << "Игровой режим: " << (en ? QStringLiteral("Включён") : QStringLiteral("Выключен")) << "\n";
            return 0;
        }

        throw std::runtime_error(QStringLiteral("Неизвестная команда '%1'. См. kalm --help").arg(subcmd).toStdString());
    } catch (const std::exception& ex) {
        err << "Ошибка: " << QString::fromUtf8(ex.what()) << "\n";
        return 1;
    }
}
