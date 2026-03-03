/*
 * main.qml - Qt6 Quick dashboard UI
 *
 * Three-panel fullscreen dashboard with property bindings to
 * SensorBackend:  temperature arc gauge, horizon strip, CPU bar.
 * Arrow keys provide keyboard fallback when sensors are absent.
 *
 * Copyright (C) 2025 Obuda University - Embedded Systems Lab
 * SPDX-License-Identifier: MIT
 */

import QtQuick
import QtQuick.Layouts

Window {
    id: root
    width: 800; height: 480
    visibility: Window.FullScreen
    color: "#19191e"
    title: "Dashboard"

    /* Keyboard fallback */
    Item {
        focus: true
        Keys.onUpPressed:    backend.adjustTemperature(0.5)
        Keys.onDownPressed:  backend.adjustTemperature(-0.5)
        Keys.onLeftPressed:  backend.adjustRoll(-2.0)
        Keys.onRightPressed: backend.adjustRoll(2.0)
        Keys.onEscapePressed: Qt.quit()
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 15

        /* --- Temperature Gauge (left) --- */
        Item {
            Layout.preferredWidth: root.height - 40
            Layout.fillHeight: true

            GaugeArc {
                anchors.centerIn: parent
                width: Math.min(parent.width, parent.height)
                height: width
                value: backend.temperature
                minValue: 15
                maxValue: 45
                label: "TEMPERATURE"
                unit: "°C"
            }
        }

        /* --- Horizon Indicator (center) --- */
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Canvas {
                id: horizonCanvas
                anchors.fill: parent

                property real roll: backend.roll
                property real pitch: backend.pitch
                onRollChanged: requestPaint()
                onPitchChanged: requestPaint()

                onPaint: {
                    var ctx = getContext("2d");
                    var w = width;
                    var h = height;
                    ctx.clearRect(0, 0, w, h);

                    var horizonY = h / 2 + pitch * 2;

                    /* Sky */
                    ctx.fillStyle = "#3282c8";
                    ctx.fillRect(0, 0, w, Math.max(0, horizonY));

                    /* Ground */
                    ctx.fillStyle = "#8c5a28";
                    if (horizonY < h)
                        ctx.fillRect(0, horizonY, w, h - horizonY);

                    /* Horizon line (rotated by roll) */
                    var rollRad = roll * Math.PI / 180;
                    var cx = w / 2;
                    var cy = horizonY;
                    var half = w * 0.45;
                    var dx = half * Math.cos(rollRad);
                    var dy = half * Math.sin(rollRad);

                    ctx.strokeStyle = "#ffffff";
                    ctx.lineWidth = 2;
                    ctx.beginPath();
                    ctx.moveTo(cx - dx, cy + dy);
                    ctx.lineTo(cx + dx, cy - dy);
                    ctx.stroke();

                    /* Center crosshair */
                    var scx = w / 2;
                    var scy = h / 2;
                    ctx.beginPath();
                    ctx.moveTo(scx - 12, scy);
                    ctx.lineTo(scx + 12, scy);
                    ctx.moveTo(scx, scy - 12);
                    ctx.lineTo(scx, scy + 12);
                    ctx.stroke();
                }
            }

            /* Labels */
            Text {
                text: "HORIZON"
                color: "#ffffff"
                font.pixelSize: 16
                font.bold: true
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 6
            }
            Text {
                text: "R:" + backend.roll.toFixed(0) + "°  P:" + backend.pitch.toFixed(0) + "°"
                color: "#dcdcdc"
                font.pixelSize: 14
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 8
            }
        }

        /* --- CPU Bar (right) --- */
        Item {
            Layout.preferredWidth: 70
            Layout.fillHeight: true

            Text {
                id: cpuLabel
                text: "CPU"
                color: "#dcdcdc"
                font.pixelSize: 16
                font.bold: true
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
            }

            Rectangle {
                id: cpuBg
                anchors.top: cpuLabel.bottom
                anchors.topMargin: 8
                anchors.bottom: cpuText.top
                anchors.bottomMargin: 8
                anchors.horizontalCenter: parent.horizontalCenter
                width: 50
                color: "#323237"
                radius: 4

                Rectangle {
                    id: cpuFill
                    width: parent.width
                    height: parent.height * backend.cpuPercent / 100
                    anchors.bottom: parent.bottom
                    color: "#50b4ff"
                    radius: 4

                    Behavior on height { NumberAnimation { duration: 150 } }
                }
            }

            Text {
                id: cpuText
                text: backend.cpuPercent.toFixed(0) + "%"
                color: "#dcdcdc"
                font.pixelSize: 18
                font.bold: true
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
            }
        }
    }
}
