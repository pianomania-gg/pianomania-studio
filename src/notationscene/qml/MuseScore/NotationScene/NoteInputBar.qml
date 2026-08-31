/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

pragma ComponentBehavior: Bound

import QtQuick

import Muse.Ui
import Muse.UiComponents
import MuseScore.NotationScene

Item {
    id: root

    property alias orientation: gridView.orientation

    property bool floating: false

    property int maximumWidth: 0
    property int maximumHeight: 0

    width: gridView.isHorizontal ? childrenRect.width : 76
    height: !gridView.isHorizontal ? childrenRect.height : 40

    property NavigationPanel navigationPanel: NavigationPanel {
        name: "NoteInputBar"
        enabled: root.enabled && root.visible
        accessible.name: qsTrc("notation", "Note input toolbar")
    }

    NoteInputBarModel {
        id: noteInputModel
    }

    HeldNoteSettingsDialog {
        id: heldNoteSettingsDialog
        model: noteInputModel
    }

    Connections {
        target: noteInputModel
        function onHeldNoteSettingsRequested() {
            heldNoteSettingsDialog.show()
        }
    }

    QtObject {
        id: prv

        function resolveHorizontalGridViewWidth() {
            if (root.floating) {
                return gridView.contentWidth
            }

            var requiredFreeSpace = gridView.cellWidth * 3 + gridView.rowSpacing * 4

            if (root.maximumWidth - gridView.contentWidth < requiredFreeSpace) {
                return gridView.contentWidth - requiredFreeSpace
            }

            return gridView.contentWidth
        }

        function resolveVerticalGridViewHeight() {
            if (root.floating) {
                return gridView.contentHeight
            }

            var requiredFreeSpace = gridView.cellHeight * 3 + gridView.rowSpacing * 4

            if (root.maximumHeight - gridView.contentHeight < requiredFreeSpace) {
                return gridView.contentHeight - requiredFreeSpace
            }

            return gridView.contentHeight
        }
    }

    GridViewSectional {
        id: gridView

        sectionRole: "section"

        rowSpacing: 4
        columnSpacing: 4

        cellWidth: 32
        cellHeight: cellWidth

        sectionWidth: isHorizontal ? 1 : width
        sectionHeight: isHorizontal ? height : 1

        clip: true

        model: noteInputModel

        sectionDelegate: SeparatorLine {
            required property int itemIndex

            orientation: gridView.isHorizontal ? Qt.Vertical : Qt.Horizontal
            visible: itemIndex !== 0
        }

        itemDelegate: FlatButton {
            id: btn

            required property var itemModel

            readonly property MenuItem item: Boolean(itemModel) ? itemModel.item : null
            readonly property bool hasMenu: Boolean(item) && item.subitems.length !== 0

            width: gridView.cellWidth
            height: gridView.cellWidth

            enabled: noteInputModel.isInputAllowed && Boolean(item) && item.enabled

            accentButton: (Boolean(item) && item.checked) || menuLoader.isMenuOpened
            transparent: !accentButton

            icon: Boolean(item) && item.id !== "held-note" && item.id !== "shake-note"
                && item.id !== "left-hand" && item.id !== "right-hand"
                && item.id !== "hand-visuals" && item.id !== "rubato-zone"
                && item.id !== "pyro-span" && item.id !== "laser-span"
                ? item.icon
                : IconCode.NONE
            iconFont: ui.theme.toolbarIconsFont

            toolTipTitle: Boolean(item) ? item.title : ""
            toolTipDescription: Boolean(item) ? item.description : ""
            toolTipShortcut: Boolean(item) ? item.shortcuts : ""

            navigation.panel: root.navigationPanel
            navigation.name: Boolean(item) ? item.id : ""
            navigation.order: Boolean(itemModel) ? itemModel.order : 0
            isClickOnKeyNavTriggered: false
            navigation.onTriggered: {
                if (btn.hasMenu) {
                    toggleMenuOpened()
                } else {
                    handleMenuItem()
                }
            }

            function toggleMenuOpened() {
                menuLoader.toggleOpened(item.subitems)
            }

            function handleMenuItem() {
                Qt.callLater(noteInputModel.handleMenuItem, item.id)
            }

            onClicked: {
                if (btn.hasMenu) {
                    toggleMenuOpened()
                } else {
                    handleMenuItem()
                }
            }

            mouseArea.onPressAndHold: function(event) {
                if (menuLoader.isMenuOpened || !btn.hasMenu) {
                    event.accepted = false // do not suppress the click event
                    return
                }

                btn.toggleMenuOpened()
            }

            // Rainbow overlay for held-note
            Rectangle {
                anchors.fill: parent
                visible: Boolean(item) && item.id === "held-note"
                radius: 6
                border.width: accentButton ? 2 : 0
                border.color: accentButton ? ui.theme.accentColor : "transparent"
                layer.enabled: true
                layer.smooth: true
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: "red" }
                    GradientStop { position: 0.142; color: "orange" }
                    GradientStop { position: 0.284; color: "yellow" }
                    GradientStop { position: 0.426; color: "green" }
                    GradientStop { position: 0.571; color: "cyan" }
                    GradientStop { position: 0.714; color: "blue" }
                    GradientStop { position: 0.856; color: "indigo" }
                    GradientStop { position: 1.0; color: "violet" }
                }
            }

            // Violet vibration bars for shake-note (bass shake)
            Item {
                anchors.centerIn: parent
                visible: Boolean(item) && item.id === "shake-note"
                width: 20
                height: 14
                opacity: accentButton ? 1 : 0.8

                Rectangle {
                    x: 5
                    y: 0
                    width: 13
                    height: 3
                    radius: 1.5
                    color: "#D826FF"
                }

                Rectangle {
                    x: 0
                    y: 5.5
                    width: 13
                    height: 3
                    radius: 1.5
                    color: "#D826FF"
                }

                Rectangle {
                    x: 5
                    y: 11
                    width: 13
                    height: 3
                    radius: 1.5
                    color: "#D826FF"
                }
            }

            Rectangle {
                anchors.centerIn: parent
                visible: Boolean(item) && item.id === "left-hand"
                width: 18
                height: 18
                radius: width / 2
                color: "transparent"
                border.width: 2
                border.color: "#0b3d91"
                opacity: accentButton ? 1 : 0.6
            }

            Rectangle {
                anchors.centerIn: parent
                visible: Boolean(item) && item.id === "right-hand"
                width: 18
                height: 18
                radius: width / 2
                color: "transparent"
                border.width: 2
                border.color: "#c25a00"
                opacity: accentButton ? 1 : 0.6
            }

            // Purple bracket overlay for rubato-zone: top halves of [ and ]
            Item {
                anchors.centerIn: parent
                visible: Boolean(item) && item.id === "rubato-zone"
                width: 22
                height: 12
                opacity: accentButton ? 1 : 0.6

                Rectangle {
                    x: 0
                    y: 0
                    width: 22
                    height: 2
                    color: "#9B4DFF"
                }

                Rectangle {
                    x: 0
                    y: 0
                    width: 2
                    height: 8
                    color: "#9B4DFF"
                }

                Rectangle {
                    x: 20
                    y: 0
                    width: 2
                    height: 8
                    color: "#9B4DFF"
                }
            }

            // Orange bracket + flame pyramid for pyro-span
            Item {
                anchors.centerIn: parent
                visible: Boolean(item) && item.id === "pyro-span"
                width: 22
                height: 14
                opacity: accentButton ? 1 : 0.6

                Rectangle {
                    x: 0
                    y: 0
                    width: 22
                    height: 2
                    color: "#FF7A1A"
                }

                Rectangle {
                    x: 0
                    y: 0
                    width: 2
                    height: 8
                    color: "#FF7A1A"
                }

                Rectangle {
                    x: 20
                    y: 0
                    width: 2
                    height: 8
                    color: "#FF7A1A"
                }

                Rectangle {
                    x: 10
                    y: 4
                    width: 2
                    height: 2
                    color: "#FF7A1A"
                }

                Rectangle {
                    x: 8.5
                    y: 7
                    width: 5
                    height: 2
                    color: "#FF7A1A"
                }

                Rectangle {
                    x: 7
                    y: 10
                    width: 8
                    height: 2
                    color: "#FF7A1A"
                }
            }

            // Cyan bracket + fanning beams for laser-span
            Item {
                anchors.centerIn: parent
                visible: Boolean(item) && item.id === "laser-span"
                width: 22
                height: 14
                opacity: accentButton ? 1 : 0.6

                Rectangle {
                    x: 0
                    y: 0
                    width: 22
                    height: 2
                    color: "#00E5FF"
                }

                Rectangle {
                    x: 0
                    y: 0
                    width: 2
                    height: 8
                    color: "#00E5FF"
                }

                Rectangle {
                    x: 20
                    y: 0
                    width: 2
                    height: 8
                    color: "#00E5FF"
                }

                Rectangle {
                    x: 6
                    y: 4
                    width: 1.5
                    height: 10
                    color: "#00E5FF"
                    transformOrigin: Item.Top
                    rotation: -25
                }

                Rectangle {
                    x: 10.5
                    y: 4
                    width: 1.5
                    height: 10
                    color: "#00E5FF"
                    transformOrigin: Item.Top
                    rotation: 0
                }

                Rectangle {
                    x: 15
                    y: 4
                    width: 1.5
                    height: 10
                    color: "#00E5FF"
                    transformOrigin: Item.Top
                    rotation: 25
                }
            }

            Item {
                anchors.centerIn: parent
                visible: Boolean(item) && item.id === "hand-visuals"
                width: 22
                height: 12
                opacity: accentButton ? 1 : 0.6

                Rectangle {
                    width: 10
                    height: 10
                    radius: width / 2
                    color: "transparent"
                    border.width: 2
                    border.color: "#0b3d91"
                }

                Rectangle {
                    x: 12
                    width: 10
                    height: 10
                    radius: width / 2
                    color: "transparent"
                    border.width: 2
                    border.color: "#c25a00"
                }
            }

            StyledMenuLoader {
                id: menuLoader

                onHandleMenuItem: function(itemId) {
                    noteInputModel.handleMenuItem(itemId)
                }
            }
        }
    }

    FlatButton {
        id: customizeButton

        anchors.margins: 4

        width: gridView.cellWidth
        height: gridView.cellHeight

        icon: IconCode.SETTINGS_COG
        iconFont: ui.theme.toolbarIconsFont
        toolTipTitle: qsTrc("notation", "Customize toolbar")
        toolTipDescription: qsTrc("notation", "Show/hide toolbar buttons")
        transparent: true

        enabled: noteInputModel.isInputAllowed

        navigation.panel: root.navigationPanel
        navigation.order: 100
        navigation.accessible.name: qsTrc("notation", "Customize toolbar")

        onClicked: {
            customizePopup.toggleOpened()
        }

        NoteInputBarCustomisePopup {
            id: customizePopup

            anchorItem: !root.floating ? ui.rootItem : null
        }
    }

    states: [
        State {
            when: gridView.isHorizontal

            PropertyChanges {
                target: gridView
                width: prv.resolveHorizontalGridViewWidth()
                height: root.height
                sectionWidth: 1
                sectionHeight: root.height
                rows: 1
                columns: gridView.noLimit
            }

            AnchorChanges {
                target: customizeButton
                anchors.left: gridView.right
                anchors.verticalCenter: root.verticalCenter
            }
        },
        State {
            when: !gridView.isHorizontal

            PropertyChanges {
                target: gridView
                width: root.width
                height: prv.resolveVerticalGridViewHeight()
                sectionWidth: root.width
                sectionHeight: 1
                rows: gridView.noLimit
                columns: 2
            }

            AnchorChanges {
                target: customizeButton
                anchors.top: gridView.bottom
                anchors.right: parent.right
            }
        }
    ]
}
