// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard

Kirigami.ApplicationWindow {
    id: root

    readonly property var i18n: backend.i18n
    readonly property real cardMaxW: Kirigami.Units.gridUnit * 32

    title: backend.deviceName ? (backend.deviceName + " — Kalm") : "Kalm"
    width: Kirigami.Units.gridUnit * 34
    height: Kirigami.Units.gridUnit * 32
    minimumWidth: Kirigami.Units.gridUnit * 28
    minimumHeight: Kirigami.Units.gridUnit * 24

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

    // ---- Sidebar Drawer ----
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
                text: root.i18n.window_my_earbuds
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.topMargin: Kirigami.Units.smallSpacing
            }
        }

        actions: [
            Kirigami.Action {
                text: root.i18n.action_add_earbuds_ellipsis
                icon.name: "list-add-symbolic"
                onTriggered: {
                    backend.scanBluetoothDevices()
                    addDeviceDialog.open()
                }
            },
            Kirigami.Action {
                text: root.i18n.action_kde_bt_settings
                icon.name: "preferences-system-bluetooth"
                onTriggered: backend.openSystemBluetoothSettings()
            },
            Kirigami.Action {
                separator: true
            },
            Kirigami.Action {
                text: root.i18n.action_minimize_tray
                icon.name: "window-minimize-symbolic"
                onTriggered: root.visible = false
            },
            Kirigami.Action {
                text: root.i18n.action_quit
                icon.name: "application-exit-symbolic"
                onTriggered: backend.quitApp()
            }
        ]
    }

    // ---- Main Kirigami FormCard Page ----
    pageStack.initialPage: FormCard.FormCardPage {
        id: mainPage
        title: "Kalm"
        verticalScrollBarPolicy: Controls.ScrollBar.AlwaysOff

        actions: [
            Kirigami.Action {
                text: root.i18n.action_add_earbuds
                icon.name: "list-add-symbolic"
                onTriggered: {
                    backend.scanBluetoothDevices()
                    addDeviceDialog.open()
                }
            },
            Kirigami.Action {
                text: backend.connected ? root.i18n.action_refresh : root.i18n.action_connect
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

        // ---- Connection Status Header Card (Centered) ----
        FormCard.FormCard {
            maximumWidth: root.cardMaxW
            Layout.topMargin: Kirigami.Units.largeSpacing

            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
                Layout.bottomMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: "audio-headset"
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: Kirigami.Units.iconSizes.medium
                    implicitHeight: Kirigami.Units.iconSizes.medium
                    opacity: backend.connected ? 1.0 : 0.55
                }

                Kirigami.Heading {
                    level: 2
                    text: backend.deviceName || root.i18n.status_no_device_selected
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Controls.Label {
                    text: backend.statusSummary
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    opacity: 0.75
                    Layout.fillWidth: true
                }

                Controls.Button {
                    visible: !backend.connected && backend.deviceMac.length > 0
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    text: root.i18n.action_connect_bt
                    icon.name: "network-bluetooth-activated-symbolic"
                    onClicked: backend.connectBluetoothDevice(backend.deviceMac)
                }
            }
        }

        // ---- Battery Info Grid (Left / Right / Case) ----
        FormCard.FormGridContainer {
            maximumWidth: root.cardMaxW
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

        // ---- Primary Section: Noise Cancellation Selector + ANC Level / Transparency Mode ----
        FormCard.FormHeader {
            maximumWidth: root.cardMaxW
            title: root.i18n.section_noise
            topPadding: Kirigami.Units.largeSpacing
            bottomPadding: Kirigami.Units.largeSpacing + 2
        }

        FormCard.FormCard {
            maximumWidth: root.cardMaxW
            enabled: backend.connected

            FormCard.FormRadioSelectorDelegate {
                id: noiseSelector
                consistentWidth: true
                selectedIndex: backend.noiseModeIndex

                actions: [
                    Kirigami.Action {
                        text: root.i18n.mode_anc
                        icon.name: "microphone-sensitivity-muted-symbolic"
                        onTriggered: backend.setNoiseByIndex(0)
                    },
                    Kirigami.Action {
                        text: root.i18n.mode_off
                        icon.name: "audio-headphones-symbolic"
                        onTriggered: backend.setNoiseByIndex(1)
                    },
                    Kirigami.Action {
                        text: root.i18n.mode_trans
                        icon.name: "microphone-sensitivity-high-symbolic"
                        onTriggered: backend.setNoiseByIndex(2)
                    }
                ]
            }

            FormCard.FormDelegateSeparator {
                visible: backend.noiseModeIndex === 0
            }

            FormCard.FormComboBoxDelegate {
                id: ancLevelCombo
                visible: backend.noiseModeIndex === 0
                text: root.i18n.anc_level_title
                description: root.i18n.anc_level_desc
                model: backend.ancLevelModel
                textRole: "text"
                valueRole: "value"
                currentIndex: backend.ancLevelIndex
                onActivated: function(index) {
                    backend.setAncLevelByIndex(index)
                }
            }

            FormCard.FormDelegateSeparator {
                visible: backend.noiseModeIndex === 2
            }

            FormCard.FormComboBoxDelegate {
                id: transEffectCombo
                visible: backend.noiseModeIndex === 2
                text: root.i18n.trans_effect_title
                description: root.i18n.trans_effect_desc
                model: backend.transEffectModel
                textRole: "text"
                valueRole: "value"
                currentIndex: backend.transEffectIndex
                onActivated: function(index) {
                    backend.setTransEffectByIndex(index)
                }
            }
        }

        // ---- Sound & Controls Section ----
        FormCard.FormHeader {
            maximumWidth: root.cardMaxW
            title: root.i18n.section_sound
            topPadding: Kirigami.Units.largeSpacing
            bottomPadding: Kirigami.Units.largeSpacing + 2
        }

        FormCard.FormCard {
            maximumWidth: root.cardMaxW
            enabled: backend.connected

            FormCard.FormComboBoxDelegate {
                id: eqCombo
                text: root.i18n.eq_title
                description: root.i18n.eq_desc
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
                text: root.i18n.cycle_title
                description: root.i18n.cycle_desc
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
                text: root.i18n.game_title
                description: root.i18n.game_desc
                checked: backend.gameMode
                onToggled: backend.setGameMode(checked)
            }

            FormCard.FormDelegateSeparator { above: gameSwitch; below: wearSwitch }

            FormCard.FormSwitchDelegate {
                id: wearSwitch
                text: root.i18n.wear_detect_title
                description: root.i18n.wear_detect_desc
                checked: backend.wearDetection
                onToggled: backend.setWearDetection(checked)
            }
        }

        // ---- Saved Earbuds Section (Multi-Device Management) ----
        FormCard.FormHeader {
            maximumWidth: root.cardMaxW
            title: root.i18n.section_devices
            topPadding: Kirigami.Units.largeSpacing
            bottomPadding: Kirigami.Units.largeSpacing + 2
        }

        FormCard.FormCard {
            maximumWidth: root.cardMaxW

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
                        description: modelData.mac + " · " + (modelData.btConnected ? root.i18n.status_connected_bt : root.i18n.status_offline)
                        leadingPadding: Kirigami.Units.largeSpacing * 1.5

                        leading: Kirigami.Icon {
                            source: modelData.active ? "emblem-checked" : "audio-headset"
                            implicitWidth: Kirigami.Units.iconSizes.smallMedium
                            implicitHeight: Kirigami.Units.iconSizes.smallMedium
                            Layout.leftMargin: Kirigami.Units.smallSpacing
                        }

                        trailing: RowLayout {
                            spacing: Kirigami.Units.smallSpacing

                            Controls.Button {
                                visible: !modelData.active
                                text: root.i18n.action_select
                                onClicked: backend.selectSavedDevice(modelData.mac)
                            }

                            Controls.ToolButton {
                                icon.name: "edit-rename-symbolic"
                                Controls.ToolTip.visible: hovered
                                Controls.ToolTip.text: root.i18n.action_rename
                                onClicked: {
                                    renameDialog.targetMac = modelData.mac
                                    renameField.text = modelData.alias.length > 0 ? modelData.alias : modelData.displayName
                                    renameDialog.open()
                                }
                            }

                            Controls.ToolButton {
                                icon.name: "edit-delete-remove-symbolic"
                                Controls.ToolTip.visible: hovered
                                Controls.ToolTip.text: root.i18n.action_remove_from_list
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
                text: root.i18n.auto_switch_title
                description: root.i18n.auto_switch_desc
                checked: backend.autoSwitch
                onToggled: backend.setAutoSwitch(checked)
            }

            FormCard.FormDelegateSeparator {}

            FormCard.FormButtonDelegate {
                text: root.i18n.action_add_earbuds_ellipsis
                icon.name: "list-add-symbolic"
                onClicked: {
                    backend.scanBluetoothDevices()
                    addDeviceDialog.open()
                }
            }
        }

        // ---- Dual-Connection Multipoint Section ----
        FormCard.FormHeader {
            maximumWidth: root.cardMaxW
            title: root.i18n.section_multipoint
            topPadding: Kirigami.Units.largeSpacing
            bottomPadding: Kirigami.Units.largeSpacing + 2
            visible: backend.connected && backend.multipointDevices.length > 0
        }

        FormCard.FormCard {
            maximumWidth: root.cardMaxW
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
                        leadingPadding: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing

                        leading: Kirigami.Icon {
                            source: modelData.connected ? "network-connect-symbolic" : "network-disconnect-symbolic"
                            implicitWidth: Kirigami.Units.iconSizes.smallMedium
                            implicitHeight: Kirigami.Units.iconSizes.smallMedium
                        }

                        trailing: Controls.Button {
                            visible: !modelData.active
                            text: modelData.connected ? root.i18n.action_disconnect : root.i18n.action_connect
                            onClicked: backend.toggleMultipointDevice(modelData.mac, !modelData.connected)
                        }
                    }
                }
            }
        }

        // ---- Find My Earbuds Section ----
        FormCard.FormHeader {
            maximumWidth: root.cardMaxW
            title: root.i18n.section_find
            topPadding: Kirigami.Units.largeSpacing
            bottomPadding: Kirigami.Units.largeSpacing + 2
        }

        FormCard.FormCard {
            maximumWidth: root.cardMaxW
            enabled: backend.connected

            FormCard.FormButtonDelegate {
                text: backend.isRinging ? root.i18n.find_stop : root.i18n.find_start
                description: root.i18n.find_desc
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

        // ---- Application Settings Section (Language) ----
        FormCard.FormHeader {
            maximumWidth: root.cardMaxW
            title: root.i18n.section_settings
            topPadding: Kirigami.Units.largeSpacing
            bottomPadding: Kirigami.Units.largeSpacing + 2
        }

        FormCard.FormCard {
            maximumWidth: root.cardMaxW
            Layout.bottomMargin: Kirigami.Units.largeSpacing

            FormCard.FormComboBoxDelegate {
                id: langCombo
                text: root.i18n.lang_title
                description: root.i18n.lang_desc
                model: backend.languageModel
                textRole: "text"
                valueRole: "value"
                currentIndex: backend.languageIndex
                onActivated: function(index) {
                    backend.setLanguageByIndex(index)
                }
            }
        }
    }

    // ---- Dialog: Add Earbuds ----
    FormCard.FormCardDialog {
        id: addDeviceDialog
        title: root.i18n.dlg_add_title
        standardButtons: Controls.Dialog.Close

        FormCard.FormSectionText {
            text: root.i18n.dlg_add_hint
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
                    leadingPadding: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing

                    leading: Kirigami.Icon {
                        source: modelData.isVivo ? "audio-headset" : "preferences-system-bluetooth"
                        implicitWidth: Kirigami.Units.iconSizes.smallMedium
                        implicitHeight: Kirigami.Units.iconSizes.smallMedium
                    }

                    trailing: Controls.Button {
                        text: modelData.alreadySaved ? root.i18n.action_select : root.i18n.action_add
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
            text: root.i18n.dlg_refresh_bt
            icon.name: "view-refresh-symbolic"
            onClicked: backend.scanBluetoothDevices()
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormTextFieldDelegate {
            id: manualMacField
            label: root.i18n.dlg_mac_label
            placeholderText: "1C:BA:B8:0F:5A:0F"
        }

        FormCard.FormTextFieldDelegate {
            id: manualAliasField
            label: root.i18n.dlg_alias_label
            placeholderText: root.i18n.dlg_alias_placeholder
        }

        FormCard.FormButtonDelegate {
            text: root.i18n.dlg_add_by_mac
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
            text: root.i18n.dlg_pair_kde
            icon.name: "preferences-system-bluetooth"
            onClicked: backend.openSystemBluetoothSettings()
        }
    }

    // ---- Dialog: Rename Earbuds ----
    FormCard.FormCardDialog {
        id: renameDialog
        property string targetMac: ""
        title: root.i18n.dlg_rename_title
        standardButtons: Controls.Dialog.Save | Controls.Dialog.Cancel

        onAccepted: {
            if (targetMac.length > 0) {
                backend.renameSavedDevice(targetMac, renameField.text)
            }
        }

        FormCard.FormTextFieldDelegate {
            id: renameField
            label: root.i18n.dlg_rename_label
            placeholderText: root.i18n.dlg_rename_placeholder
        }
    }

    // ---- Dialog: Find Earbuds Safety Warning ----
    FormCard.FormCardDialog {
        id: findConfirmDialog
        title: root.i18n.find_warn_title
        standardButtons: Controls.Dialog.Yes | Controls.Dialog.Cancel

        onAccepted: backend.startFindEarbuds()

        FormCard.FormTextDelegate {
            text: root.i18n.find_warn_heading
            description: root.i18n.find_warn_body
            leadingPadding: Kirigami.Units.largeSpacing
            leading: Kirigami.Icon {
                source: "dialog-warning"
                implicitWidth: Kirigami.Units.iconSizes.medium
                implicitHeight: Kirigami.Units.iconSizes.medium
            }
        }
    }
}
