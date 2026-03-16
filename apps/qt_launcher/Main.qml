/*
 * Main.qml - Car-style app launcher with swipeable pages
 *
 * Page 1: Apps     — Doom, Qt Dashboard, Ball Balance, Pong
 * Page 2: Demos    — SDL2 tutorial demos (Cube, Touch Paint, Level, Audio Viz, etc.)
 * Page 3: System   — CPU, memory, temperature, uptime, IP
 *
 * All app paths assume the embedded-linux repo is cloned to ~/embedded-linux.
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

    /* Base path — auto-detected from executable location (set by main.cpp),
     * falls back to ~/embedded-linux for manual launches */
    readonly property string repo: typeof repoBase !== "undefined" ? repoBase : "/home/linux/embedded-linux"

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

        Row {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 20
            spacing: 16

            Text {
                text: sysinfo.hostname
                color: "#dcdcdc"
                font.pixelSize: 18
                font.bold: true
                renderType: Text.NativeRendering
            }

            Text {
                text: ["\u2022 Apps", "\u2022 Demos", "\u2022 System"][swipe.currentIndex]
                color: "#50b4ff"
                font.pixelSize: 14
                anchors.baseline: parent.children[0].baseline
                renderType: Text.NativeRendering
            }
        }

        Text {
            text: sysinfo.ipAddress
            color: "#808080"
            font.pixelSize: 14
            renderType: Text.NativeRendering
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

        /* ── Page 1: Apps ────────────────────────────────────── */
        Item {
            GridLayout {
                anchors.fill: parent
                anchors.margins: 16
                columns: 2
                rowSpacing: 8
                columnSpacing: 8

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u2694"
                    label: "Doom"
                    accent: "#e05050"
                    onClicked: launcher.launch(repo + "/solutions/doom-on-pi/doom_kiosk.sh")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u2299"
                    label: "Dashboard"
                    accent: "#50b4ff"
                    onClicked: launcher.launch(repo + "/apps/qt_dashboard/build/qt_dashboard")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u25C9"
                    label: "Ball Balance"
                    accent: "#ff9832"
                    onClicked: launcher.launch("python3 /home/linux/ball_detect.py")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u25A3"
                    label: "Pong"
                    accent: "#c878ff"
                    onClicked: launcher.launch(repo + "/apps/pong-fb/pong_fb")
                }
            }
        }

        /* ── Page 2: Demos ───────────────────────────────────── */
        Item {
            GridLayout {
                anchors.fill: parent
                anchors.margins: 16
                columns: 3
                rowSpacing: 8
                columnSpacing: 8

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u25F0"
                    label: "3D Cube"
                    accent: "#00c878"
                    onClicked: launcher.launch("SDL_VIDEODRIVER=kmsdrm " + repo + "/solutions/sdl2-rotating-cube/build/step4_cube")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u270E"
                    label: "Touch Paint"
                    accent: "#ffc832"
                    onClicked: launcher.launch("SDL_VIDEODRIVER=kmsdrm " + repo + "/solutions/sdl2-touch-paint/build/sdl2_touch_paint")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u25CE"
                    label: "Level"
                    accent: "#50e0a0"
                    onClicked: launcher.launch("SDL_VIDEODRIVER=kmsdrm " + repo + "/apps/level-display/level_sdl2")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u2261"
                    label: "SDL2 Dash"
                    accent: "#50b4ff"
                    onClicked: launcher.launch("SDL_VIDEODRIVER=kmsdrm " + repo + "/apps/sdl2-dashboard/sdl2_dashboard")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u25B3"
                    label: "Triangle"
                    accent: "#ff6480"
                    onClicked: launcher.launch("SDL_VIDEODRIVER=kmsdrm " + repo + "/solutions/sdl2-rotating-cube/build/step1_triangle")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u25A1"
                    label: "Spin Square"
                    accent: "#c878ff"
                    onClicked: launcher.launch("SDL_VIDEODRIVER=kmsdrm " + repo + "/solutions/sdl2-rotating-cube/build/step3_rotating_square")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u266B"
                    label: "Audio Viz"
                    accent: "#ff5090"
                    onClicked: launcher.launch("SDL_VIDEODRIVER=kmsdrm " + repo + "/apps/i2s-audio-viz/audio_viz -d hw:1,0 -c 2 -f")
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    icon: "\u266A"
                    label: "Audio Full"
                    accent: "#c050ff"
                    onClicked: launcher.launch("SDL_VIDEODRIVER=kmsdrm " + repo + "/apps/i2s-audio-viz/audio_viz_full -d hw:1,0 -c 2 -f")
                }
            }
        }

        /* ── Page 3: System Info ─────────────────────────────── */
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

                    Text { text: "CPU"; color: "#808080"; font.pixelSize: 16; renderType: Text.NativeRendering }
                    Text { text: sysinfo.cpuPercent.toFixed(0) + "%"; color: "#dcdcdc"; font.pixelSize: 24; font.bold: true; renderType: Text.NativeRendering }

                    Text { text: "Memory"; color: "#808080"; font.pixelSize: 16; renderType: Text.NativeRendering }
                    Text { text: sysinfo.memUsedMB + " / " + sysinfo.memTotalMB + " MB"; color: "#dcdcdc"; font.pixelSize: 24; font.bold: true; renderType: Text.NativeRendering }

                    Text { text: "Temperature"; color: "#808080"; font.pixelSize: 16; renderType: Text.NativeRendering }
                    Text { text: sysinfo.temperature.toFixed(1) + " \u00B0C"; color: "#dcdcdc"; font.pixelSize: 24; font.bold: true; renderType: Text.NativeRendering }

                    Text { text: "Uptime"; color: "#808080"; font.pixelSize: 16; renderType: Text.NativeRendering }
                    Text { text: sysinfo.uptime; color: "#dcdcdc"; font.pixelSize: 24; font.bold: true; renderType: Text.NativeRendering }

                    Text { text: "IP Address"; color: "#808080"; font.pixelSize: 16; renderType: Text.NativeRendering }
                    Text { text: sysinfo.ipAddress; color: "#dcdcdc"; font.pixelSize: 24; font.bold: true; renderType: Text.NativeRendering }

                    Text { text: "Hostname"; color: "#808080"; font.pixelSize: 16; renderType: Text.NativeRendering }
                    Text { text: sysinfo.hostname; color: "#dcdcdc"; font.pixelSize: 24; font.bold: true; renderType: Text.NativeRendering }
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
            width: index === indicator.currentIndex ? 24 : 10
            height: 10
            radius: 5
            color: index === indicator.currentIndex ? "#50b4ff" : "#3c3c41"
            antialiasing: true
            Behavior on width { NumberAnimation { duration: 200; easing.type: Easing.OutQuad } }
            Behavior on color { ColorAnimation { duration: 200 } }
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
