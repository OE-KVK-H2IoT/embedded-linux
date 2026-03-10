/*
 * AppButton.qml - Reusable app launch button
 *
 * Rounded rectangle with icon + label, press animation,
 * and text eliding to prevent overflow on small screens.
 *
 * Copyright (C) 2025 Obuda University - Embedded Systems Lab
 * SPDX-License-Identifier: MIT
 */

import QtQuick

Item {
    id: btn

    property string icon: ""
    property string label: ""
    property color accent: "#50b4ff"

    signal clicked()

    Rectangle {
        id: bg
        anchors.fill: parent
        anchors.margins: 4
        radius: 14
        color: ma.pressed ? Qt.darker(accent, 1.6) : "#2a2a30"
        border.color: ma.pressed ? Qt.lighter(accent, 1.3) : accent
        border.width: 2
        antialiasing: true

        /* Smooth press animation */
        scale: ma.pressed ? 0.95 : 1.0
        Behavior on scale { NumberAnimation { duration: 80; easing.type: Easing.OutQuad } }
        Behavior on color { ColorAnimation { duration: 80 } }
        Behavior on border.color { ColorAnimation { duration: 80 } }

        /* Subtle accent glow behind the icon */
        Rectangle {
            anchors.centerIn: iconText
            width: iconText.paintedWidth + 24
            height: iconText.paintedHeight + 24
            radius: width / 2
            color: accent
            opacity: ma.pressed ? 0.15 : 0.06
            antialiasing: true
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        Column {
            anchors.centerIn: parent
            spacing: 6
            width: parent.width - 16

            Text {
                id: iconText
                text: btn.icon
                font.pixelSize: Math.min(bg.height * 0.32, 56)
                color: accent
                anchors.horizontalCenter: parent.horizontalCenter
                renderType: Text.NativeRendering
            }

            Text {
                text: btn.label
                font.pixelSize: Math.min(bg.height * 0.13, 18)
                font.bold: true
                font.letterSpacing: 0.5
                color: "#dcdcdc"
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                renderType: Text.NativeRendering
            }
        }

        MouseArea {
            id: ma
            anchors.fill: parent
            onClicked: btn.clicked()
        }
    }
}
