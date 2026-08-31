/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 */
import QtQuick
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents
import MuseScore.NotationScene

StyledDialogView {
    id: root

    required property NoteInputBarModel model

    title: qsTrc("notation", "Held Note settings")

    contentWidth: 820
    contentHeight: root.model.heldNotePitchBendEditable ? 640 : 220
    margins: 20

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        StyledTextLabel {
            text: qsTrc("notation", "Held Note settings")
            font: ui.theme.bodyBoldFont
            horizontalAlignment: Text.AlignLeft
        }

        StyledTextLabel {
            Layout.fillWidth: true
            visible: root.model.heldNoteSettingsMessage.length > 0
            text: root.model.heldNoteSettingsMessage
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignLeft
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.model.heldNoteSettingsEditable
            spacing: 12

            RowLayout {
                Layout.fillWidth: true

                StyledTextLabel {
                    text: qsTrc("notation", "Pulse subdivision")
                }

                StyledDropdown {
                    id: pulseDropdown
                    Layout.preferredWidth: 220
                    model: {
                        var options = [
                            { text: qsTrc("notation", "None"), value: 0 },
                            { text: qsTrc("notation", "Quarter note"), value: 4 },
                            { text: qsTrc("notation", "Eighth note"), value: 8 },
                            { text: qsTrc("notation", "Sixteenth note"), value: 16 },
                            { text: qsTrc("notation", "Thirty-second note"), value: 32 },
                            { text: qsTrc("notation", "Sixty-fourth note"), value: 64 }
                        ]
                        if (root.model.heldNotePulseMixed) {
                            options.unshift({ text: qsTrc("notation", "Mixed"), value: -1 })
                        }
                        return options
                    }
                    currentIndex: root.model.heldNotePulseMixed ? 0 : indexOfValue(root.model.heldNotePulse)
                    navigation.accessible.name: qsTrc("notation", "Pulse subdivision") + ": " + currentText
                    onActivated: function(index, value) {
                        if (value >= 0) root.model.heldNotePulse = value
                    }
                }

                CheckBox {
                    text: qsTrc("notation", "Triplet")
                    enabled: root.model.heldNotePulse !== 0
                    checked: root.model.heldNotePulseTriplet
                    isIndeterminate: root.model.heldNotePulseTripletMixed
                    navigation.accessible.name: text
                    onClicked: root.model.heldNotePulseTriplet = !root.model.heldNotePulseTriplet
                }
            }

            CheckBox {
                text: qsTrc("notation", "Pitch bend")
                enabled: root.model.heldNotePitchBendEditable
                checked: root.model.heldNotePitchBendEnabled
                navigation.accessible.name: text
                onClicked: root.model.heldNotePitchBendEnabled = !root.model.heldNotePitchBendEnabled
            }

            StyledTextLabel {
                Layout.fillWidth: true
                text: qsTrc("notation", "Pitch bends require a gameplay backing track.")
                visible: root.model.heldNotePitchBendEditable
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignLeft
            }

            Rectangle {
                id: graphFrame
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: ui.theme.backgroundPrimaryColor
                border.color: ui.theme.strokeColor
                border.width: 1
                opacity: root.model.heldNotePitchBendEnabled ? 1.0 : 0.45
                visible: root.model.heldNotePitchBendEditable

                Item {
                    id: noteLabels

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    height: 28

                    property var boundaries: {
                        var result = []
                        for (var i = 0; i < root.model.heldNoteTimingGrid.length; ++i) {
                            var marker = root.model.heldNoteTimingGrid[i]
                            if (marker.writtenNoteBoundary) result.push(marker.scoreTick)
                        }
                        return result
                    }

                    Repeater {
                        model: Math.max(0, noteLabels.boundaries.length - 1)

                        StyledTextLabel {
                            x: noteLabels.boundaries[index] * noteLabels.width / root.model.heldNoteDurationTicks
                            width: (noteLabels.boundaries[index + 1] - noteLabels.boundaries[index])
                                * noteLabels.width / root.model.heldNoteDurationTicks
                            height: noteLabels.height
                            text: width >= 70 ? qsTrc("notation", "Note %1").arg(index + 1) : String(index + 1)
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }

                Canvas {
                    id: graph
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: noteLabels.bottom
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    anchors.bottomMargin: 12
                    enabled: root.model.heldNotePitchBendEnabled
                    activeFocusOnTab: true

                    property var points: root.model.heldNotePitchCurve
                    property var timingGrid: root.model.heldNoteTimingGrid
                    property int selectedPoint: 0
                    property var selectedCurvePoint: selectedPoint >= 0 && selectedPoint < points.length
                        ? points[selectedPoint] : ({ scoreTick: 0, pitch: 0, slope: 0 })

                    Accessible.role: Accessible.EditableText
                    Accessible.name: qsTrc("notation", "Held Note pitch bend curve")
                    Accessible.description: qsTrc("notation", "Use the arrow keys to edit the selected curve point. Vertical lines mark beats and written-note boundaries.")

                    onPointsChanged: {
                        selectedPoint = Math.max(0, Math.min(selectedPoint, points.length - 1))
                        requestPaint()
                    }
                    onTimingGridChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()

                    function px(scoreTick) { return scoreTick * width / root.model.heldNoteDurationTicks }
                    function py(pitch) { return height / 2 - pitch * height / 4800 }
                    function scoreTickAt(x) {
                        return Math.max(0, Math.min(root.model.heldNoteDurationTicks,
                                                   Math.round(x * root.model.heldNoteDurationTicks / width)))
                    }
                    function pitchAt(y) { return Math.max(-2400, Math.min(2400, Math.round((height / 2 - y) * 4800 / height))) }
                    function pointAt(x, y) {
                        var nearest = -1
                        var distance = 14
                        for (var i = 0; i < points.length; ++i) {
                            var dx = px(points[i].scoreTick) - x
                            var dy = py(points[i].pitch) - y
                            var candidate = Math.sqrt(dx * dx + dy * dy)
                            if (candidate < distance) {
                                nearest = i
                                distance = candidate
                            }
                        }
                        return nearest
                    }

                    function pchip(segment, scoreTick) {
                        var a = points[segment]
                        var b = points[segment + 1]
                        var h = b.scoreTick - a.scoreTick
                        var t = (scoreTick - a.scoreTick) / h
                        var t2 = t * t
                        var t3 = t2 * t
                        return (2 * t3 - 3 * t2 + 1) * a.pitch
                            + (t3 - 2 * t2 + t) * (h / root.model.scoreTicksPerQuarter) * a.slope
                            + (-2 * t3 + 3 * t2) * b.pitch
                            + (t3 - t2) * (h / root.model.scoreTicksPerQuarter) * b.slope
                    }

                    function moveSelected(deltaTime, deltaPitch) {
                        if (selectedPoint < 0 || selectedPoint >= points.length) return
                        var point = points[selectedPoint]
                        root.model.setHeldNotePitchCurvePoint(selectedPoint, point.scoreTick + deltaTime,
                                                             point.pitch + deltaPitch)
                    }

                    Keys.onLeftPressed: function(event) { moveSelected(-1, 0); event.accepted = true }
                    Keys.onRightPressed: function(event) { moveSelected(1, 0); event.accepted = true }
                    Keys.onUpPressed: function(event) { moveSelected(0, 1); event.accepted = true }
                    Keys.onDownPressed: function(event) { moveSelected(0, -1); event.accepted = true }
                    Keys.onDeletePressed: function(event) { root.model.removeHeldNotePitchCurvePoint(selectedPoint); event.accepted = true }

                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()

                        for (var markerIndex = 0; markerIndex < timingGrid.length; ++markerIndex) {
                            var marker = timingGrid[markerIndex]
                            if (marker.scoreTick <= 0 || marker.scoreTick >= root.model.heldNoteDurationTicks) continue
                            ctx.globalAlpha = marker.writtenNoteBoundary ? 0.85
                                : marker.beatBoundary ? 0.55
                                : marker.subdivision === 2 ? 0.3 : 0.18
                            ctx.strokeStyle = ui.theme.strokeColor
                            ctx.lineWidth = marker.writtenNoteBoundary || marker.beatBoundary ? 2 : 1
                            ctx.beginPath()
                            ctx.moveTo(px(marker.scoreTick), 0)
                            ctx.lineTo(px(marker.scoreTick), height)
                            ctx.stroke()
                            if (marker.beatBoundary && marker.label.length > 0) {
                                ctx.globalAlpha = 0.8
                                ctx.fillStyle = ui.theme.fontPrimaryColor
                                ctx.font = "11px " + ui.theme.bodyFont.family
                                ctx.fillText(marker.label, px(marker.scoreTick) + 4, 14)
                            }
                        }
                        ctx.globalAlpha = 1.0
                        ctx.strokeStyle = ui.theme.strokeColor
                        ctx.lineWidth = 1
                        ctx.beginPath()
                        ctx.moveTo(0, height / 2)
                        ctx.lineTo(width, height / 2)
                        ctx.stroke()
                        if (points.length < 2) return

                        ctx.strokeStyle = ui.theme.accentColor
                        ctx.lineWidth = 2
                        ctx.beginPath()
                        ctx.moveTo(px(points[0].scoreTick), py(points[0].pitch))
                        for (var segment = 0; segment < points.length - 1; ++segment) {
                            var samples = Math.max(2,
                                Math.ceil(px(points[segment + 1].scoreTick) - px(points[segment].scoreTick)))
                            for (var sample = 1; sample <= samples; ++sample) {
                                var scoreTick = points[segment].scoreTick
                                    + (points[segment + 1].scoreTick - points[segment].scoreTick) * sample / samples
                                ctx.lineTo(px(scoreTick), py(pchip(segment, scoreTick)))
                            }
                        }
                        ctx.stroke()

                        if (selectedPoint >= 0 && selectedPoint < points.length) {
                            var selected = points[selectedPoint]
                            var direction = selectedPoint === points.length - 1 ? -1 : 1
                            var handleDx = direction * Math.min(50, width / 8)
                            var handleTimeDelta = handleDx * root.model.heldNoteDurationTicks / width
                            var handlePitch = selected.pitch + selected.slope * handleTimeDelta
                                / root.model.scoreTicksPerQuarter
                            ctx.strokeStyle = ui.theme.accentColor
                            ctx.lineWidth = 1
                            ctx.beginPath()
                            ctx.moveTo(px(selected.scoreTick), py(selected.pitch))
                            ctx.lineTo(px(selected.scoreTick) + handleDx, py(handlePitch))
                            ctx.stroke()
                            ctx.fillStyle = ui.theme.accentColor
                            ctx.beginPath()
                            ctx.arc(px(selected.scoreTick) + handleDx, py(handlePitch), 4, 0, Math.PI * 2)
                            ctx.fill()
                        }

                        for (var i = 0; i < points.length; ++i) {
                            ctx.fillStyle = i === selectedPoint ? ui.theme.accentColor : ui.theme.fontPrimaryColor
                            ctx.beginPath()
                            ctx.arc(px(points[i].scoreTick), py(points[i].pitch), i === selectedPoint ? 6 : 4, 0, Math.PI * 2)
                            ctx.fill()
                        }
                    }

                    MouseArea {
                        id: graphMouseArea
                        anchors.fill: parent
                        enabled: graph.enabled
                        property bool draggingTangent: false
                        onPressed: function(mouse) {
                            graph.forceActiveFocus()
                            root.model.beginHeldNoteSettingsEdit()
                            var selected = graph.selectedCurvePoint
                            var direction = graph.selectedPoint === graph.points.length - 1 ? -1 : 1
                            var handleDx = direction * Math.min(50, graph.width / 8)
                            var handleTimeDelta = handleDx * root.model.heldNoteDurationTicks / graph.width
                            var handlePitch = selected.pitch + selected.slope * handleTimeDelta
                                / root.model.scoreTicksPerQuarter
                            var tangentDx = graph.px(selected.scoreTick) + handleDx - mouse.x
                            var tangentDy = graph.py(handlePitch) - mouse.y
                            if (Math.sqrt(tangentDx * tangentDx + tangentDy * tangentDy) < 12) {
                                draggingTangent = true
                                return
                            }
                            var nearest = graph.pointAt(mouse.x, mouse.y)
                            if (nearest >= 0) {
                                graph.selectedPoint = nearest
                            } else {
                                root.model.addHeldNotePitchCurvePoint(graph.scoreTickAt(mouse.x), graph.pitchAt(mouse.y))
                                for (var j = 0; j < graph.points.length; ++j) {
                                    if (graph.points[j].scoreTick === graph.scoreTickAt(mouse.x)) graph.selectedPoint = j
                                }
                            }
                            graph.requestPaint()
                        }
                        onDoubleClicked: function(mouse) {
                            var point = graph.pointAt(mouse.x, mouse.y)
                            if (point > 0 && point < graph.points.length - 1) {
                                graph.selectedPoint = point
                                root.model.removeHeldNotePitchCurvePoint(point)
                            }
                        }
                        onPositionChanged: function(mouse) {
                            if (!pressed) return
                            if (draggingTangent) {
                                var point = graph.selectedCurvePoint
                                var quarterDelta = (graph.scoreTickAt(mouse.x) - point.scoreTick)
                                    / root.model.scoreTicksPerQuarter
                                if (Math.abs(quarterDelta) > 0.00001) {
                                    var slope = Math.round((graph.pitchAt(mouse.y) - point.pitch) / quarterDelta)
                                    root.model.setHeldNotePitchCurvePointSlope(graph.selectedPoint,
                                        Math.max(-1000000, Math.min(1000000, slope)))
                                }
                                return
                            }
                            root.model.setHeldNotePitchCurvePoint(graph.selectedPoint, graph.scoreTickAt(mouse.x),
                                                                 graph.pitchAt(mouse.y))
                        }
                        onReleased: {
                            draggingTangent = false
                            root.model.endHeldNoteSettingsEdit()
                        }
                        onCanceled: {
                            draggingTangent = false
                            root.model.endHeldNoteSettingsEdit()
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                visible: root.model.heldNotePitchBendEditable

                StyledTextLabel {
                    text: qsTrc("notation", "Selected point")
                    font: ui.theme.bodyBoldFont
                }

                StyledTextLabel {
                    text: qsTrc("notation", "Score tick")
                }

                IncrementalPropertyControl {
                    id: selectedPointTime
                    Layout.preferredWidth: 100
                    enabled: root.model.heldNotePitchBendEnabled
                        && graph.selectedPoint > 0
                        && graph.selectedPoint < graph.points.length - 1
                    currentValue: graph.selectedCurvePoint.scoreTick
                    step: 1
                    decimals: 0
                    minValue: graph.selectedPoint > 0 ? graph.points[graph.selectedPoint - 1].scoreTick + 1 : 0
                    maxValue: graph.selectedPoint + 1 < graph.points.length
                        ? graph.points[graph.selectedPoint + 1].scoreTick - 1 : root.model.heldNoteDurationTicks
                    navigation.accessible.name: qsTrc("notation", "Selected point score tick") + ": " + currentValue
                    onValueEdited: function(newValue) {
                        root.model.setHeldNotePitchCurvePoint(graph.selectedPoint, Math.round(newValue),
                                                             graph.selectedCurvePoint.pitch)
                    }
                }

                StyledTextLabel {
                    text: qsTrc("notation", "Measure and beat")
                }

                StyledTextLabel {
                    text: root.model.heldNotePositionLabel(graph.selectedCurvePoint.scoreTick)
                    font: ui.theme.bodyBoldFont
                }

                StyledTextLabel {
                    text: qsTrc("notation", "Semitones")
                }

                IncrementalPropertyControl {
                    id: selectedPointPitch
                    Layout.preferredWidth: 120
                    enabled: root.model.heldNotePitchBendEnabled && graph.selectedPoint > 0
                    currentValue: graph.selectedCurvePoint.pitch / 100
                    step: 0.01
                    decimals: 2
                    minValue: -24
                    maxValue: 24
                    navigation.accessible.name: qsTrc("notation", "Selected point semitones") + ": " + currentValue
                    onValueEdited: function(newValue) {
                        root.model.setHeldNotePitchCurvePoint(graph.selectedPoint, graph.selectedCurvePoint.scoreTick,
                                                             Math.round(newValue * 100))
                    }
                }

            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                visible: root.model.heldNotePitchBendEditable

                StyledTextLabel {
                    text: qsTrc("notation", "Slope (semitones per quarter note)")
                }

                IncrementalPropertyControl {
                    id: selectedPointSlope
                    Layout.preferredWidth: 120
                    enabled: root.model.heldNotePitchBendEnabled && graph.selectedPoint >= 0
                    currentValue: graph.selectedCurvePoint.slope / 100
                    step: 0.01
                    decimals: 2
                    minValue: -10000
                    maxValue: 10000
                    navigation.accessible.name: qsTrc("notation", "Selected point slope in semitones per quarter note")
                        + ": " + currentValue
                    onValueEdited: function(newValue) {
                        root.model.setHeldNotePitchCurvePointSlope(graph.selectedPoint, Math.round(newValue * 100))
                    }
                }

                FlatButton {
                    text: qsTrc("notation", "Remove point")
                    enabled: root.model.heldNotePitchBendEnabled
                        && graph.selectedPoint > 0
                        && graph.selectedPoint < graph.points.length - 1
                    navigation.accessible.name: text
                    onClicked: root.model.removeHeldNotePitchCurvePoint(graph.selectedPoint)
                }

                Item { Layout.fillWidth: true }

                FlatButton {
                    text: qsTrc("global", "Undo")
                    enabled: root.model.heldNoteSettingsCanUndo
                    navigation.accessible.name: text
                    onClicked: root.model.undoHeldNoteSettingsEdit()
                }

                FlatButton {
                    text: qsTrc("global", "Redo")
                    enabled: root.model.heldNoteSettingsCanRedo
                    navigation.accessible.name: text
                    onClicked: root.model.redoHeldNoteSettingsEdit()
                }
            }

            StyledTextLabel {
                Layout.fillWidth: true
                visible: root.model.heldNotePitchBendEditable
                text: qsTrc("notation", "Score ticks 0–%1; pitch −24.00 to 24.00 semitones; 2–32 points.")
                    .arg(root.model.heldNoteDurationTicks)
                horizontalAlignment: Text.AlignLeft
            }
        }

        ButtonBox {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignRight | Qt.AlignBottom

            FlatButton {
                text: qsTrc("global", "Cancel")
                buttonRole: ButtonBoxModel.RejectRole
                onClicked: {
                    root.model.cancelHeldNoteSettings()
                    root.reject()
                }
            }

            FlatButton {
                text: qsTrc("global", "Apply")
                accentButton: true
                enabled: root.model.heldNoteSettingsEditable && root.model.heldNoteSettingsValid
                buttonRole: ButtonBoxModel.ApplyRole
                onClicked: {
                    if (root.model.applyHeldNoteSettings()) root.hide()
                }
            }
        }
    }
}
