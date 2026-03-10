/*
 * Main.qml - Car-style app launcher with swipeable pages
 *
 * Page 1: App grid — launch Doom, Dashboard, Level Display, etc.
 * Page 2: System info — CPU, memory, temperature, uptime, IP
 *
 * Copyright (C) 2025 Obuda University - Embedded Systems Lab
 * SPDX-License-Identifier: MIT
 */

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Window {
    id: root
    width: 800; height: 480
    visibility: Window.FullScreen
    color: "#19191e"
    title: "Launcher"

    Item {
        focus: true
        Keys.onEscapePressed: Qt.quit()
    }

    /* ── Header bar ─────────────────────────────────────────── */
    Rectangle {
        id: header
        width: parent.width
        height: 48
        color: "#111115"
        z: 1

        Text {
            text: sysinfo.hostname
            color: "#dcdcdc"
            font.pixelSize: 18
            font.bold: true
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 20
        }

        Text {
            text: sysinfo.ipAddress
            color: "#808080"
            font.pixelSize: 14
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 20
        }
    }

    /* ── Swipeable pages ────────────────────────────────────── */
    SwipeView {
        id: swipe
        anchors.top: header.bottom
        anchors.bottom: indicator.top
        anchors.left: parent.left
        anchors.right: parent.right

        /* ── Page 1: App Grid ──────────────────────────────── */
        Item {
            GridLayout {
                anchors.fill: parent
                anchors.margins: 20
                columns: 3
                rowSpacing: 12
                columnSpacing: 12

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u2694"
                    label: "Doom"
                    accent: "#e05050"
                    onClicked: launcher.launch("chocolate-doom -iwad /home/linux/doom1.wad")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u2299"
                    label: "Dashboard"
                    accent: "#50b4ff"
                    onClicked: launcher.launch("/home/linux/apps/qt_dashboard/build/qt_dashboard")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u25CE"
                    label: "Level"
                    accent: "#00c878"
                    onClicked: launcher.launch("/home/linux/apps/level_sdl2")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u25C9"
                    label: "Ball Balance"
                    accent: "#ff9832"
                    onClicked: launcher.launch("python3 /home/linux/apps/ball_detection.py")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u25A3"
                    label: "Pong"
                    accent: "#c878ff"
                    onClicked: launcher.launch("/home/linux/apps/pong_fb")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u270E"
                    label: "Touch Paint"
                    accent: "#ffc832"
                    onClicked: launcher.launch("/home/linux/apps/sdl2_touch_paint")
                }
            }
        }

        /* ── Page 2: System Info ───────────────────────────── */
        Item {
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 24
                spacing: 16

                Text {
                    text: "SYSTEM"
                    color: "#808080"
                    font.pixelSize: 14
                    font.bold: true
                    Layout.alignment: Qt.AlignHCenter
                }

                GridLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    columns: 2
                    rowSpacing: 20
                    columnSpacing: 40

                    Text { text: "CPU"; color: "#808080"; font.pixelSize: 16 }
                    Text { text: sysinfo.cpuPercent.toFixed(0) + "%"; color: "#dcdcdc"; font.pixelSize: 24; font.bold: true }

                    Text { text: "Memory"; color: "#808080"; font.pixelSize: 16 }
                    Text { text: sysinfo.memUsedMB + " / " + sysinfo.memTotalMB + " MB"; color: "#dcdcdc"; font.pixelSize: 24; font.bold: true }

                    Text { text: "Temperature"; color: "#808080"; font.pixelSize: 16 }
                    Text { text: sysinfo.temperature.toFixed(1) + " \u00B0C"; color: "#dcdcdc"; font.pixelSize: 24; font.bold: true }

                    Text { text: "Uptime"; color: "#808080"; font.pixelSize: 16 }
                    Text { text: sysinfo.uptime; color: "#dcdcdc"; font.pixelSize: 24; font.bold: true }

                    Text { text: "IP Address"; color: "#808080"; font.pixelSize: 16 }
                    Text { text: sysinfo.ipAddress; color: "#dcdcdc"; font.pixelSize: 24; font.bold: true }

                    Text { text: "Hostname"; color: "#808080"; font.pixelSize: 16 }
                    Text { text: sysinfo.hostname; color: "#dcdcdc"; font.pixelSize: 24; font.bold: true }
                }
            }
        }
    }

    /* ── Page indicator dots ────────────────────────────────── */
    PageIndicator {
        id: indicator
        count: swipe.count
        currentIndex: swipe.currentIndex
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 8
        anchors.horizontalCenter: parent.horizontalCenter

        delegate: Rectangle {
            width: 10; height: 10
            radius: 5
            color: index === indicator.currentIndex ? "#50b4ff" : "#3c3c41"
        }
    }

    /* ── Overlay while child is running ─────────────────────── */
    Rectangle {
        anchors.fill: parent
        color: "#000000"
        visible: launcher.childRunning
        z: 10

        Text {
            anchors.centerIn: parent
            text: "App running..."
            color: "#808080"
            font.pixelSize: 20
        }
    }
}
