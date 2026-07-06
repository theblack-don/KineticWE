/*
    SPDX-FileCopyrightText: 2026 KWin Tiling Fork
    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM
import org.kde.kquickcontrols as KQC
import org.kde.kwin.kcm.tiling

KCM.SimpleKCM {
    id: root

    implicitWidth: Kirigami.Units.gridUnit * 40
    implicitHeight: Kirigami.Units.gridUnit * 30

    header: QQC2.TabBar {
        id: tabBar

        QQC2.TabButton {
            text: i18nc("@title:tab", "Layouts")
        }
        QQC2.TabButton {
            text: i18nc("@title:tab", "Gaps & Per-Monitor")
        }
        QQC2.TabButton {
            text: i18nc("@title:tab", "Per-Desktop")
        }
        QQC2.TabButton {
            text: i18nc("@title:tab", "Window Borders")
        }
        QQC2.TabButton {
            text: i18nc("@title:tab", "Floating Rules")
        }
    }

    StackLayout {
        id: stackLayout
        anchors.fill: parent
        currentIndex: tabBar.currentIndex

        Kirigami.FormLayout {
            id: layoutsPage

            QQC2.CheckBox {
                id: enableTiling
                Kirigami.FormData.label: i18n("Tiling:")
                text: i18n("Enable tiling")
                checked: kcm.settings.enabled
                onToggled: kcm.settings.enabled = checked
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "enabled"
                }
            }

            Item {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18n("Layouts")
            }

            QQC2.CheckBox {
                id: enableMasterStack
                Kirigami.FormData.label: i18n("Available:")
                text: i18n("MasterStack")
                checked: kcm.settings.enabledLayouts.indexOf("MasterStack") !== -1
                onToggled: {
                    let layouts = kcm.settings.enabledLayouts.slice();
                    if (checked && layouts.indexOf("MasterStack") === -1) {
                        layouts.push("MasterStack");
                    } else if (!checked) {
                        layouts = layouts.filter(l => l !== "MasterStack");
                    }
                    kcm.settings.enabledLayouts = layouts;
                }
            }

            QQC2.CheckBox {
                id: enableStacked
                text: i18n("Stacked")
                checked: kcm.settings.enabledLayouts.indexOf("Stacked") !== -1
                onToggled: {
                    let layouts = kcm.settings.enabledLayouts.slice();
                    if (checked && layouts.indexOf("Stacked") === -1) {
                        layouts.push("Stacked");
                    } else if (!checked) {
                        layouts = layouts.filter(l => l !== "Stacked");
                    }
                    kcm.settings.enabledLayouts = layouts;
                }
            }

            QQC2.CheckBox {
                id: enableCenterTile
                text: i18n("CenterTile")
                checked: kcm.settings.enabledLayouts.indexOf("CenterTile") !== -1
                onToggled: {
                    let layouts = kcm.settings.enabledLayouts.slice();
                    if (checked && layouts.indexOf("CenterTile") === -1) {
                        layouts.push("CenterTile");
                    } else if (!checked) {
                        layouts = layouts.filter(l => l !== "CenterTile");
                    }
                    kcm.settings.enabledLayouts = layouts;
                }
            }

            QQC2.CheckBox {
                id: enableMonocle
                text: i18n("Monocle")
                checked: kcm.settings.enabledLayouts.indexOf("Monocle") !== -1
                onToggled: {
                    let layouts = kcm.settings.enabledLayouts.slice();
                    if (checked && layouts.indexOf("Monocle") === -1) {
                        layouts.push("Monocle");
                    } else if (!checked) {
                        layouts = layouts.filter(l => l !== "Monocle");
                    }
                    kcm.settings.enabledLayouts = layouts;
                }
            }

            QQC2.CheckBox {
                id: enableAutoGrid
                text: i18n("AutoGrid")
                checked: kcm.settings.enabledLayouts.indexOf("AutoGrid") !== -1
                onToggled: {
                    let layouts = kcm.settings.enabledLayouts.slice();
                    if (checked && layouts.indexOf("AutoGrid") === -1) {
                        layouts.push("AutoGrid");
                    } else if (!checked) {
                        layouts = layouts.filter(l => l !== "AutoGrid");
                    }
                    kcm.settings.enabledLayouts = layouts;
                }
            }

            QQC2.Label {
                visible: kcm.settings.enabledLayouts.length < 2
                Kirigami.FormData.label: i18nc("@info", "Note:")
                text: i18nc("@info", "Enable at least one layout. If none are enabled, the global default below is used.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.7
            }

            QQC2.ComboBox {
                id: defaultLayout
                Kirigami.FormData.label: i18n("Default layout:")
                model: [
                    { text: i18n("MasterStack"), value: "MasterStack" },
                    { text: i18n("Stacked"), value: "Stacked" },
                    { text: i18n("CenterTile"), value: "CenterTile" },
                    { text: i18n("Monocle"), value: "Monocle" },
                    { text: i18n("AutoGrid"), value: "AutoGrid" }
                ]
                textRole: "text"
                valueRole: "value"
                currentIndex: {
                    const idx = model.findIndex(item => item.value === kcm.settings.defaultLayout);
                    return idx >= 0 ? idx : 0;
                }
                onActivated: kcm.settings.defaultLayout = currentValue
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "defaultLayout"
                }
            }

            Item {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18n("Resizing")
            }

            QQC2.SpinBox {
                id: resizeStep
                Kirigami.FormData.label: i18n("Resize step (%):")
                from: 1
                to: 100
                value: Math.round(kcm.settings.resizeStep * 100)
                onValueModified: kcm.settings.resizeStep = value / 100.0
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "resizeStep"
                }
            }

            QQC2.Label {
                Kirigami.FormData.label: i18nc("@info", "Note:")
                text: xi18nc("@info", "How much each <interface>Window Grow/Shrink Horizontal/Vertical</interface> shortcut press changes a tiled window's share of its column. The KDE-standard <shortcut>Meta+Plus</shortcut> and <shortcut>Meta+Shift+Plus</shortcut> shortcuts are reused; the active window grows and its column-mates shrink to keep the layout balanced.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.7
            }
        }

        Kirigami.FormLayout {
            id: gapsPage

            // The global gap values shown here are *templates* used as the
            // default for newly-connected monitors. They are not applied at
            // runtime once a per-monitor override exists; the per-monitor
            // page below is the source of truth at runtime. Editing these
            // values is the right thing to do only when you want to change
            // the defaults that a brand-new monitor will pick up; if you
            // already have a monitor and want to change its gaps, use the
            // per-monitor section below.
            QQC2.Label {
                Kirigami.FormData.label: i18nc("@info", "Default gaps:")
                text: xi18nc("@info", "These are the default gap values used for <emphasis>new monitors</emphasis>. Existing monitors use the per-monitor values below.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                opacity: 0.7
            }

            QQC2.SpinBox {
                id: gapLeft
                Kirigami.FormData.label: i18n("Left:")
                from: 0
                to: 1000
                value: kcm.settings.gapLeft
                onValueModified: kcm.settings.gapLeft = value
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "gapLeft"
                }
            }

            QQC2.SpinBox {
                id: gapRight
                Kirigami.FormData.label: i18n("Right:")
                from: 0
                to: 1000
                value: kcm.settings.gapRight
                onValueModified: kcm.settings.gapRight = value
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "gapRight"
                }
            }

            QQC2.SpinBox {
                id: gapTop
                Kirigami.FormData.label: i18n("Top:")
                from: 0
                to: 1000
                value: kcm.settings.gapTop
                onValueModified: kcm.settings.gapTop = value
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "gapTop"
                }
            }

            QQC2.SpinBox {
                id: gapBottom
                Kirigami.FormData.label: i18n("Bottom:")
                from: 0
                to: 1000
                value: kcm.settings.gapBottom
                onValueModified: kcm.settings.gapBottom = value
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "gapBottom"
                }
            }

            QQC2.SpinBox {
                id: gapBetween
                Kirigami.FormData.label: i18n("Between tiles:")
                from: 0
                to: 1000
                value: kcm.settings.gapBetween
                onValueModified: kcm.settings.gapBetween = value
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "gapBetween"
                }
            }

            Item {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18n("Per-Monitor Settings")
            }

            QQC2.Label {
                id: perMonitorHelp
                Kirigami.FormData.label: i18nc("@info:placeholder", "Overrides:")
                text: xi18nc("@info", "Per-monitor gap and layout values override the defaults above. Monitors without an override use the defaults. Leave the values as-is to use defaults.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 30
            }

            Repeater {
                id: perMonitorRepeater
                model: kcm.gapOverridesModel

                delegate: QQC2.Frame {
                    id: monitorFrame
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    Layout.bottomMargin: Kirigami.Units.smallSpacing

                    // Pull the Repeater model data into local properties.  Inside
                    // child controls such as ComboBox the name "model" resolves to
                    // the control's own model property, so we must not reach back
                    // through `model.entry` there.
                    property var entry: model.entry
                    property string monitorName: model.name
                    property string monitorDescription: model.description

                    background: Rectangle {
                        color: "transparent"
                        border.color: Kirigami.Theme.textColor
                        border.width: 1
                        radius: Kirigami.Units.smallSpacing
                        opacity: 0.3
                    }

                    contentItem: ColumnLayout {
                        spacing: Kirigami.Units.smallSpacing

                        QQC2.Label {
                            text: monitorFrame.monitorDescription + (monitorFrame.monitorName ? "  (" + monitorFrame.monitorName + ")" : "")
                            font.bold: true
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        GridLayout {
                            columns: 2
                            columnSpacing: Kirigami.Units.largeSpacing
                            rowSpacing: Kirigami.Units.smallSpacing
                            Layout.fillWidth: true

                            QQC2.Label {
                                text: i18n("Default layout:")
                                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            }
                            QQC2.ComboBox {
                                id: perMonitorLayoutCombo
                                property var layoutOptions: {
                                    // Only show layouts the user has enabled on
                                    // the Layouts page. If nothing is enabled,
                                    // fall back to MasterStack so the combo is
                                    // never empty. The list is rebuilt whenever
                                    // enabledLayouts changes, so the per-monitor
                                    // page always reflects the current global
                                    // "Available" selection.
                                    const seen = kcm.settings.enabledLayouts.slice();
                                    const opts = [];
                                    for (let i = 0; i < seen.length; ++i) {
                                        if (seen[i] === "MasterStack"
                                            || seen[i] === "Stacked"
                                            || seen[i] === "CenterTile"
                                            || seen[i] === "Monocle"
                                            || seen[i] === "AutoGrid") {
                                            opts.push({ text: seen[i], value: seen[i] });
                                        }
                                    }
                                    if (opts.length === 0) {
                                        opts.push({ text: i18n("MasterStack"), value: "MasterStack" });
                                    }
                                    return opts;
                                }
                                model: layoutOptions
                                textRole: "text"
                                valueRole: "value"
                                currentIndex: {
                                    const cur = monitorFrame.entry ? monitorFrame.entry.defaultLayout : kcm.settings.defaultLayout;
                                    const idx = layoutOptions.findIndex(item => item.value === cur);
                                    return idx >= 0 ? idx : 0;
                                }
                                onActivated: {
                                    if (monitorFrame.entry) {
                                        monitorFrame.entry.defaultLayout = layoutOptions[currentIndex].value;
                                    }
                                }
                            }

                            QQC2.Label {
                                text: i18n("Left gap:")
                                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            }
                            QQC2.SpinBox {
                                from: 0
                                to: 1000
                                value: monitorFrame.entry ? monitorFrame.entry.gapLeft : kcm.settings.gapLeft
                                onValueModified: if (monitorFrame.entry) monitorFrame.entry.gapLeft = value
                            }

                            QQC2.Label {
                                text: i18n("Right gap:")
                                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            }
                            QQC2.SpinBox {
                                from: 0
                                to: 1000
                                value: monitorFrame.entry ? monitorFrame.entry.gapRight : kcm.settings.gapRight
                                onValueModified: if (monitorFrame.entry) monitorFrame.entry.gapRight = value
                            }

                            QQC2.Label {
                                text: i18n("Top gap:")
                                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            }
                            QQC2.SpinBox {
                                from: 0
                                to: 1000
                                value: monitorFrame.entry ? monitorFrame.entry.gapTop : kcm.settings.gapTop
                                onValueModified: if (monitorFrame.entry) monitorFrame.entry.gapTop = value
                            }

                            QQC2.Label {
                                text: i18n("Bottom gap:")
                                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            }
                            QQC2.SpinBox {
                                from: 0
                                to: 1000
                                value: monitorFrame.entry ? monitorFrame.entry.gapBottom : kcm.settings.gapBottom
                                onValueModified: if (monitorFrame.entry) monitorFrame.entry.gapBottom = value
                            }

                            QQC2.Label {
                                text: i18n("Between tiles gap:")
                                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            }
                            QQC2.SpinBox {
                                from: 0
                                to: 1000
                                value: monitorFrame.entry ? monitorFrame.entry.gapBetween : kcm.settings.gapBetween
                                onValueModified: if (monitorFrame.entry) monitorFrame.entry.gapBetween = value
                            }
                        }
                    }
                }
            }

            QQC2.Label {
                id: noMonitorsLabel
                visible: kcm.gapOverridesModel.count === 0
                Kirigami.FormData.label: i18nc("@info", "Status:")
                text: i18nc("@info", "No connected monitors detected. Per-monitor overrides will appear here once a monitor is connected.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 30
            }
        }

        Kirigami.FormLayout {
            id: perDesktopPage

            Item {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18n("Per-Desktop Layout Overrides")
            }

            QQC2.Label {
                id: perDesktopHelp
                Kirigami.FormData.label: i18nc("@info:placeholder", "Overrides:")
                text: xi18nc("@info", "Each row is a virtual desktop. Each column is a connected monitor. Select a layout to override the global default for that (desktop, monitor) pair. Leave as <interface>Use default</interface> to fall back to the per-monitor or global setting.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 40
            }

            // Table header row showing output names
            Item {
                Kirigami.FormData.isSection: true
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 40
                implicitHeight: outputHeaderRow.implicitHeight

                RowLayout {
                    id: outputHeaderRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    spacing: Kirigami.Units.smallSpacing

                    // Empty spacer matching the desktop label column width
                    Item {
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 8
                    }

                    Repeater {
                        model: kcm.desktopLayoutOverridesModel.outputNames

                        QQC2.Label {
                            text: kcm.desktopLayoutOverridesModel.outputDescriptions[index]
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            leftPadding: Kirigami.Units.smallSpacing * 2
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            wrapMode: Text.WordWrap
                            maximumLineCount: 2
                        }
                    }
                }
            }

            Repeater {
                id: desktopRepeater
                model: {
                    // Build a flat list of desktop numbers, one per desktop row
                    const nums = [];
                    const count = kcm.desktopLayoutOverridesModel.count;
                    const outputCount = kcm.desktopLayoutOverridesModel.outputNames.length;
                    if (outputCount === 0) return [];
                    for (let i = 0; i < count; i += outputCount) {
                        nums.push(i / outputCount + 1);
                    }
                    return nums;
                }

                delegate: Item {
                    id: desktopRow
                    property int desktopNumber: modelData

                    Layout.fillWidth: true
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 40
                    implicitHeight: desktopRowLayout.implicitHeight
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    Layout.bottomMargin: Kirigami.Units.smallSpacing

                    RowLayout {
                        id: desktopRowLayout
                        anchors.left: parent.left
                        anchors.right: parent.right
                        spacing: Kirigami.Units.smallSpacing

                        // Desktop label
                        QQC2.Label {
                            text: i18n("Desktop %1:", desktopRow.desktopNumber)
                            font.bold: true
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 8
                            verticalAlignment: Text.AlignVCenter
                        }

                        // One combo per output
                        Repeater {
                            id: outputRepeater
                            model: kcm.desktopLayoutOverridesModel.outputNames

                            QQC2.ComboBox {
                                id: perDesktopLayoutCombo
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                                Layout.fillWidth: true

                                property var layoutOptions: {
                                    const seen = kcm.settings.enabledLayouts.slice();
                                    const opts = [];
                                    opts.push({ text: i18n("Use default"), value: "" });
                                    for (let i = 0; i < seen.length; ++i) {
                                        if (seen[i] === "MasterStack"
                                            || seen[i] === "Stacked"
                                            || seen[i] === "CenterTile"
                                            || seen[i] === "Monocle"
                                            || seen[i] === "AutoGrid") {
                                            opts.push({ text: seen[i], value: seen[i] });
                                        }
                                    }
                                    if (opts.length === 1) {
                                        opts.push({ text: i18n("MasterStack"), value: "MasterStack" });
                                    }
                                    return opts;
                                }
                                model: layoutOptions
                                textRole: "text"
                                valueRole: "value"

                                // Find the entry for this (desktop, output) pair.
                                // desktopRow.desktopNumber captures the outer Repeater's
                                // model data so we are not affected by the inner Repeater
                                // shadowing the outer modelData.
                                property var entry: kcm.desktopLayoutOverridesModel.entryAt((desktopRow.desktopNumber - 1) * kcm.desktopLayoutOverridesModel.outputNames.length + index)

                                currentIndex: {
                                    const cur = entry ? entry.defaultLayout : "";
                                    const idx_val = layoutOptions.findIndex(item => item.value === cur);
                                    return idx_val >= 0 ? idx_val : 0;
                                }
                                onActivated: {
                                    if (entry) {
                                        entry.defaultLayout = layoutOptions[currentIndex].value;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            QQC2.Label {
                id: noDesktopsLabel
                visible: kcm.desktopLayoutOverridesModel.count === 0
                Kirigami.FormData.label: i18nc("@info", "Status:")
                text: i18nc("@info", "No virtual desktops detected.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 30
            }
        }

        Kirigami.FormLayout {
            id: bordersPage

            QQC2.ComboBox {
                id: tilingBorderMode
                Kirigami.FormData.label: i18n("Border mode:")
                model: [
                    i18n("None"),
                    i18n("All tiled windows"),
                    i18n("Active window only"),
                    i18n("All windows")
                ]
                currentIndex: kcm.settings.tilingBorderMode
                onActivated: kcm.settings.tilingBorderMode = currentIndex
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "tilingBorderMode"
                }
            }

            QQC2.SpinBox {
                id: tilingBorderThickness
                Kirigami.FormData.label: i18n("Border thickness:")
                from: 0
                to: 20
                value: kcm.settings.tilingBorderThickness
                onValueModified: kcm.settings.tilingBorderThickness = value
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "tilingBorderThickness"
                }
            }

            QQC2.ComboBox {
                id: tilingBorderColorSourceActive
                Kirigami.FormData.label: i18n("Active border source:")
                model: [
                    i18n("Custom color"),
                    i18n("System accent"),
                    i18n("Noctalia primary"),
                    i18n("Noctalia accent")
                ]
                currentIndex: kcm.settings.tilingBorderColorSourceActive
                onActivated: kcm.settings.tilingBorderColorSourceActive = currentIndex
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "tilingBorderColorSourceActive"
                }
            }

            KQC.ColorButton {
                id: tilingBorderColorActive
                Kirigami.FormData.label: i18n("Active border color:")
                visible: kcm.settings.tilingBorderColorSourceActive === 0
                color: kcm.settings.tilingBorderColorActive
                showAlphaChannel: true
                dialogTitle: i18n("Choose active border color")
                onAccepted: kcm.settings.tilingBorderColorActive = color
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "tilingBorderColorActive"
                }
            }

            QQC2.Label {
                visible: kcm.settings.tilingBorderColorSourceActive !== 0
                Kirigami.FormData.label: i18n("Active color preview:")
                text: {
                    switch (kcm.settings.tilingBorderColorSourceActive) {
                    case 1:
                        return i18nc("@info", "Follows the system accent color.");
                    case 2:
                        return i18nc("@info", "Follows the Noctalia primary color.");
                    case 3:
                        return i18nc("@info", "Follows the Noctalia accent color.");
                    default:
                        return "";
                    }
                }
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.7
            }

            QQC2.ComboBox {
                id: tilingBorderColorSourceInactive
                Kirigami.FormData.label: i18n("Inactive border source:")
                model: [
                    i18n("Custom color"),
                    i18n("System accent"),
                    i18n("System accent (faded)"),
                    i18n("Noctalia primary"),
                    i18n("Noctalia accent")
                ]
                currentIndex: kcm.settings.tilingBorderColorSourceInactive
                onActivated: kcm.settings.tilingBorderColorSourceInactive = currentIndex
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "tilingBorderColorSourceInactive"
                }
            }

            KQC.ColorButton {
                id: tilingBorderColorInactive
                Kirigami.FormData.label: i18n("Inactive border color:")
                visible: kcm.settings.tilingBorderColorSourceInactive === 0
                color: kcm.settings.tilingBorderColorInactive
                showAlphaChannel: true
                dialogTitle: i18n("Choose inactive border color")
                onAccepted: kcm.settings.tilingBorderColorInactive = color
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "tilingBorderColorInactive"
                }
            }

            QQC2.Label {
                visible: kcm.settings.tilingBorderColorSourceInactive !== 0
                Kirigami.FormData.label: i18n("Inactive color preview:")
                text: {
                    switch (kcm.settings.tilingBorderColorSourceInactive) {
                    case 1:
                        return i18nc("@info", "Follows the system accent color.");
                    case 2:
                        return i18nc("@info", "System accent at 50% alpha.");
                    case 3:
                        return i18nc("@info", "Follows the Noctalia primary color.");
                    case 4:
                        return i18nc("@info", "Follows the Noctalia accent color.");
                    default:
                        return "";
                    }
                }
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.7
            }

            QQC2.SpinBox {
                id: tilingCornerRadius
                Kirigami.FormData.label: i18n("Corner radius:")
                from: 0
                to: 50
                value: kcm.settings.tilingCornerRadius
                onValueModified: kcm.settings.tilingCornerRadius = value
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "tilingCornerRadius"
                }
            }
        }

        Kirigami.FormLayout {
            id: floatingPage

            RowLayout {
                Kirigami.FormData.label: i18n("Floating classes:")

                QQC2.TextField {
                    id: floatingClassInput
                    Layout.fillWidth: true
                    placeholderText: i18n("Add a window class...")
                    onAccepted: addFloatingClass()
                }

                QQC2.Button {
                    icon.name: "list-add"
                    enabled: floatingClassInput.text.trim().length > 0
                    onClicked: addFloatingClass()
                }
            }

            Repeater {
                model: kcm.settings.floatingClasses
                delegate: RowLayout {
                    width: parent ? parent.width : undefined
                    spacing: Kirigami.Units.smallSpacing

                    QQC2.Label {
                        text: modelData
                        Layout.fillWidth: true
                        leftPadding: Kirigami.Units.smallSpacing
                    }

                    QQC2.ToolButton {
                        icon.name: "edit-delete"
                        QQC2.ToolTip {
                            text: i18nc("@info:tooltip", "Remove this floating class")
                        }
                        onClicked: {
                            let list = kcm.settings.floatingClasses.slice();
                            const idx = list.indexOf(modelData);
                            if (idx !== -1) {
                                list.splice(idx, 1);
                                kcm.settings.floatingClasses = list;
                            }
                        }
                    }
                }
            }

            Item {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18n("Window types:")
                visible: false
            }

            QQC2.CheckBox {
                id: floatUtility
                text: i18n("Float utility windows")
                checked: kcm.settings.floatUtility
                onToggled: kcm.settings.floatUtility = checked
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "floatUtility"
                }
            }

            QQC2.CheckBox {
                id: floatDialog
                text: i18n("Float dialog windows")
                checked: kcm.settings.floatDialog
                onToggled: kcm.settings.floatDialog = checked
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "floatDialog"
                }
            }

            QQC2.CheckBox {
                id: floatTransient
                text: i18n("Float transient windows")
                checked: kcm.settings.floatTransient
                onToggled: kcm.settings.floatTransient = checked
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "floatTransient"
                }
            }

            function addFloatingClass() {
                const text = floatingClassInput.text.trim();
                if (text.length > 0) {
                    let list = kcm.settings.floatingClasses.slice();
                    if (list.indexOf(text) === -1) {
                        list.push(text);
                        kcm.settings.floatingClasses = list;
                    }
                    floatingClassInput.text = "";
                    floatingClassInput.forceActiveFocus();
                }
            }
        }
    }
}
