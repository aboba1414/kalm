// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard

Kirigami.ApplicationWindow {
    id: root

    title: backend.deviceName ? (backend.deviceName + " — Kalm") : "Kalm"
    width: Kirigami.Units.gridUnit * 31
    height: Kirigami.Units.gridUnit * 40
    minimumWidth: Kirigami.Units.gridUnit * 24
    minimumHeight: Kirigami.Units.gridUnit * 28

    onClosing: function(close) {
        if (!backend.quitting) {
            close.accepted = false
            root.visible = false
        }
    }

    Connections {
        target: backend
        function onWindowShowRequested() {
            root.visible = true
            root.showNormal()
            root.raise()
            root.requestActivate()
        }
    }

    // ---- Sidebar Drawer for Quick Multi-Earbud Switching ----
    globalDrawer: Kirigami.GlobalDrawer {
        id: drawer
        title: "Kalm"
        titleIcon: "audio-headset"
        isMenu: false
        modal: true

        header: ColumnLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Heading {
                level: 4
                text: "Мои наушники"
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.topMargin: Kirigami.Units.smallSpacing
            }
        }

        actions: [
            Kirigami.Action {
                text: "Добавить наушники…"
                icon.name: "list-add-symbolic"
                onTriggered: {
                    backend.scanBluetoothDevices()
                    addDeviceDialog.open()
                }
            },
            Kirigami.Action {
                text: "Настройки Bluetooth KDE…"
                icon.name: "preferences-system-bluetooth"
                onTriggered: backend.openSystemBluetoothSettings()
            },
            Kirigami.Action {
                separator: true
            },
            Kirigami.Action {
                text: "Свернуть в трей"
                icon.name: "window-minimize-symbolic"
                onTriggered: root.visible = false
            },
            Kirigami.Action {
                text: "Выход"
                icon.name: "application-exit-symbolic"
                onTriggered: backend.quitApp()
            }
        ]
    }

    // ---- Main Kirigami FormCard Page ----
    pageStack.initialPage: FormCard.FormCardPage {
        id: mainPage
        title: backend.deviceName || "Kalm"

        actions: [
            Kirigami.Action {
                text: "Добавить наушники"
                icon.name: "list-add-symbolic"
                onTriggered: {
                    backend.scanBluetoothDevices()
                    addDeviceDialog.open()
                }
            },
            Kirigami.Action {
                text: backend.connected ? "Обновить" : "Подключить"
                icon.name: "view-refresh-symbolic"
                onTriggered: backend.refreshNow()
            }
        ]

        // Inline status/error message banner
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.largeSpacing
            visible: backend.bannerText.length > 0
            text: backend.bannerText
            type: {
                if (backend.bannerType === 1) return Kirigami.MessageType.Positive
                if (backend.bannerType === 2) return Kirigami.MessageType.Warning
                if (backend.bannerType === 3) return Kirigami.MessageType.Error
                return Kirigami.MessageType.Information
            }
            showCloseButton: true
            onVisibleChanged: {
                if (!visible && backend.bannerText.length > 0) {
                    backend.clearBanner()
                }
            }
        }

        // ---- Connection Status Header Card ----
        FormCard.FormCard {
            Layout.topMargin: Kirigami.Units.largeSpacing

            FormCard.FormTextDelegate {
                text: backend.deviceName || "Наушники не выбраны"
                description: backend.statusSummary

                leading: Kirigami.Icon {
                    source: "audio-headset"
                    implicitWidth: Kirigami.Units.iconSizes.medium
                    implicitHeight: Kirigami.Units.iconSizes.medium
                    opacity: backend.connected ? 1.0 : 0.55
                }

                trailing: Controls.Button {
                    visible: !backend.connected && backend.deviceMac.length > 0
                    text: "Подключить BT"
                    icon.name: "network-bluetooth-activated-symbolic"
                    onClicked: backend.connectBluetoothDevice(backend.deviceMac)
                }
            }
        }

        // ---- Battery Info Grid (Left / Right / Case) ----
        FormCard.FormGridContainer {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing

            infoCards: [
                FormCard.FormGridContainer.InfoCard {
                    title: backend.leftBatteryText
                    subtitle: backend.leftBatterySub
                },
                FormCard.FormGridContainer.InfoCard {
                    title: backend.rightBatteryText
                    subtitle: backend.rightBatterySub
                },
                FormCard.FormGridContainer.InfoCard {
                    title: backend.caseBatteryText
                    subtitle: backend.caseBatterySub
                }
            ]
        }

        // ---- Primary Section: Noise Cancellation Selector ----
        FormCard.FormHeader {
            title: "Режим шумоподавления"
        }

        FormCard.FormCard {
            enabled: backend.connected

            FormCard.FormRadioSelectorDelegate {
                id: noiseSelector
                consistentWidth: true
                selectedIndex: backend.noiseModeIndex

                actions: [
                    Kirigami.Action {
                        text: "Шумоподавление"
                        icon.name: "microphone-sensitivity-muted-symbolic"
                        onTriggered: backend.setNoiseByIndex(0)
                    },
                    Kirigami.Action {
                        text: "Выключено"
                        icon.name: "audio-headphones-symbolic"
                        onTriggered: backend.setNoiseByIndex(1)
                    },
                    Kirigami.Action {
                        text: "Прозрачность"
                        icon.name: "microphone-sensitivity-high-symbolic"
                        onTriggered: backend.setNoiseByIndex(2)
                    }
                ]
            }
        }

        // ---- Sound & Controls Section ----
        FormCard.FormHeader {
            title: "Звук и управление"
        }

        FormCard.FormCard {
            enabled: backend.connected

            FormCard.FormComboBoxDelegate {
                id: eqCombo
                text: "Звуковой эффект DeepX"
                description: "Встроенный аудиопрофиль наушников"
                model: backend.eqModel
                textRole: "text"
                valueRole: "value"
                currentIndex: backend.eqIndex
                onActivated: function(index) {
                    backend.setEqByIndex(index)
                }
            }

            FormCard.FormDelegateSeparator { above: eqCombo; below: cycleCombo }

            FormCard.FormComboBoxDelegate {
                id: cycleCombo
                text: "Долгое нажатие на ножку"
                description: "Цикл переключения режимов на самих наушниках"
                model: backend.cycleModel
                textRole: "text"
                valueRole: "value"
                currentIndex: backend.cycleIndex
                onActivated: function(index) {
                    backend.setCycleByIndex(index)
                }
            }

            FormCard.FormDelegateSeparator { above: cycleCombo; below: gameSwitch }

            FormCard.FormSwitchDelegate {
                id: gameSwitch
                text: "Игровой режим"
                description: "Пониженная задержка звука в играх и видео"
                checked: backend.gameMode
                onToggled: backend.setGameMode(checked)
            }

            FormCard.FormDelegateSeparator { above: gameSwitch; below: wearSwitch }

            FormCard.FormSwitchDelegate {
                id: wearSwitch
                text: "Обнаружение ношения"
                description: "Автоматическая пауза при извлечении наушника из уха"
                checked: backend.wearDetection
                onToggled: backend.setWearDetection(checked)
            }
        }

        // ---- Saved Earbuds Section (Multi-Device Management) ----
        FormCard.FormHeader {
            title: "Мои наушники"
        }

        FormCard.FormCard {
            Repeater {
                model: backend.savedDevices

                delegate: ColumnLayout {
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    spacing: 0

                    FormCard.FormDelegateSeparator {
                        visible: index > 0
                    }

                    FormCard.FormTextDelegate {
                        text: modelData.displayName
                        description: modelData.mac + " · " + (modelData.btConnected ? "Подключено по Bluetooth" : "Не в сети")

                        leading: Kirigami.Icon {
                            source: modelData.active ? "emblem-checked" : "audio-headset"
                            implicitWidth: Kirigami.Units.iconSizes.smallMedium
                            implicitHeight: Kirigami.Units.iconSizes.smallMedium
                        }

                        trailing: RowLayout {
                            spacing: Kirigami.Units.smallSpacing

                            Controls.Button {
                                visible: !modelData.active
                                text: "Выбрать"
                                onClicked: backend.selectSavedDevice(modelData.mac)
                            }

                            Controls.ToolButton {
                                icon.name: "edit-rename-symbolic"
                                Controls.ToolTip.visible: hovered
                                Controls.ToolTip.text: "Переименовать"
                                onClicked: {
                                    renameDialog.targetMac = modelData.mac
                                    renameField.text = modelData.alias.length > 0 ? modelData.alias : modelData.displayName
                                    renameDialog.open()
                                }
                            }

                            Controls.ToolButton {
                                icon.name: "edit-delete-remove-symbolic"
                                Controls.ToolTip.visible: hovered
                                Controls.ToolTip.text: "Удалить из списка"
                                onClicked: backend.removeSavedDevice(modelData.mac)
                            }
                        }
                    }
                }
            }

            FormCard.FormDelegateSeparator {
                visible: backend.savedDevices.length > 0
            }

            FormCard.FormSwitchDelegate {
                text: "Автопереключение между наушниками"
                description: "Автоматически выбирать подключённую пару из списка"
                checked: backend.autoSwitch
                onToggled: backend.setAutoSwitch(checked)
            }

            FormCard.FormDelegateSeparator {}

            FormCard.FormButtonDelegate {
                text: "Добавить наушники…"
                icon.name: "list-add-symbolic"
                onClicked: {
                    backend.scanBluetoothDevices()
                    addDeviceDialog.open()
                }
            }
        }

        // ---- Dual-Connection Multipoint Section ----
        FormCard.FormHeader {
            title: "Мультипоинт (два источника)"
            visible: backend.connected && backend.multipointDevices.length > 0
        }

        FormCard.FormCard {
            visible: backend.connected && backend.multipointDevices.length > 0

            Repeater {
                model: backend.multipointDevices

                delegate: ColumnLayout {
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    spacing: 0

                    FormCard.FormDelegateSeparator {
                        visible: index > 0
                    }

                    FormCard.FormTextDelegate {
                        text: modelData.name
                        description: modelData.mac + " · " + modelData.statusText

                        leading: Kirigami.Icon {
                            source: modelData.connected ? "network-connect-symbolic" : "network-disconnect-symbolic"
                            implicitWidth: Kirigami.Units.iconSizes.smallMedium
                            implicitHeight: Kirigami.Units.iconSizes.smallMedium
                        }

                        trailing: Controls.Button {
                            visible: !modelData.active
                            text: modelData.connected ? "Отключить" : "Подключить"
                            onClicked: backend.toggleMultipointDevice(modelData.mac, !modelData.connected)
                        }
                    }
                }
            }
        }

        // ---- Find My Earbuds Section ----
        FormCard.FormHeader {
            title: "Поиск устройства"
        }

        FormCard.FormCard {
            Layout.bottomMargin: Kirigami.Units.largeSpacing
            enabled: backend.connected

            FormCard.FormButtonDelegate {
                text: backend.isRinging ? "Остановить звуковой сигнал" : "Найти наушники звуковым сигналом…"
                description: "Воспроизвести громкий сигнал на обоих наушниках"
                icon.name: backend.isRinging ? "media-playback-stop-symbolic" : "preferences-desktop-notification-bell"
                onClicked: {
                    if (backend.isRinging) {
                        backend.stopFindEarbuds()
                    } else {
                        findConfirmDialog.open()
                    }
                }
            }
        }
    }

    // ---- Dialog: Add Earbuds (Scan Bluetooth + Manual MAC) ----
    FormCard.FormCardDialog {
        id: addDeviceDialog
        title: "Добавить наушники"
        standardButtons: Controls.Dialog.Close

        FormCard.FormSectionText {
            text: "Выберите сопряжённые наушники из списка Bluetooth или укажите MAC-адрес вручную."
        }

        Repeater {
            model: backend.discoveredDevices

            delegate: ColumnLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 0

                FormCard.FormDelegateSeparator {}

                FormCard.FormTextDelegate {
                    text: modelData.name
                    description: modelData.subtitle

                    leading: Kirigami.Icon {
                        source: modelData.isVivo ? "audio-headset" : "preferences-system-bluetooth"
                        implicitWidth: Kirigami.Units.iconSizes.smallMedium
                        implicitHeight: Kirigami.Units.iconSizes.smallMedium
                    }

                    trailing: Controls.Button {
                        text: modelData.alreadySaved ? "Выбрать" : "Добавить"
                        icon.name: modelData.alreadySaved ? "dialog-ok-apply" : "list-add-symbolic"
                        onClicked: {
                            backend.addSavedDevice(modelData.mac, modelData.name, "")
                            addDeviceDialog.close()
                        }
                    }
                }
            }
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormButtonDelegate {
            text: "Обновить список Bluetooth"
            icon.name: "view-refresh-symbolic"
            onClicked: backend.scanBluetoothDevices()
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormTextFieldDelegate {
            id: manualMacField
            label: "MAC-адрес вручную (XX:XX:XX:XX:XX:XX):"
            placeholderText: "1C:BA:B8:0F:5A:0F"
        }

        FormCard.FormTextFieldDelegate {
            id: manualAliasField
            label: "Название (необязательно):"
            placeholderText: "Мои вторые vivo TWS"
        }

        FormCard.FormButtonDelegate {
            text: "Добавить по MAC-адресу"
            icon.name: "list-add-symbolic"
            enabled: manualMacField.text.trim().length >= 17
            onClicked: {
                if (backend.addSavedDevice(manualMacField.text, manualAliasField.text, manualAliasField.text)) {
                    manualMacField.text = ""
                    manualAliasField.text = ""
                    addDeviceDialog.close()
                }
            }
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormButtonDelegate {
            text: "Сопрячь новые наушники в настройках KDE…"
            icon.name: "preferences-system-bluetooth"
            onClicked: backend.openSystemBluetoothSettings()
        }
    }

    // ---- Dialog: Rename Earbuds ----
    FormCard.FormCardDialog {
        id: renameDialog
        property string targetMac: ""
        title: "Переименовать наушники"
        standardButtons: Controls.Dialog.Save | Controls.Dialog.Cancel

        onAccepted: {
            if (targetMac.length > 0) {
                backend.renameSavedDevice(targetMac, renameField.text)
            }
        }

        FormCard.FormTextFieldDelegate {
            id: renameField
            label: "Отображаемое имя:"
            placeholderText: "Оставьте пустым для заводского имени"
        }
    }

    // ---- Dialog: Find Earbuds Safety Warning ----
    FormCard.FormCardDialog {
        id: findConfirmDialog
        title: "Внимание: громкий сигнал"
        standardButtons: Controls.Dialog.Yes | Controls.Dialog.Cancel

        onAccepted: backend.startFindEarbuds()

        FormCard.FormTextDelegate {
            text: "Убедитесь, что наушники сняты из ушей!"
            description: "Оба наушника начнут издавать громкий звуковой сигнал для поиска в помещении."
            leading: Kirigami.Icon {
                source: "dialog-warning"
                implicitWidth: Kirigami.Units.iconSizes.medium
                implicitHeight: Kirigami.Units.iconSizes.medium
            }
        }
    }
}
