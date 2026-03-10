/*
 * AppButton.qml - Reusable app launch button
 *
 * Rounded rectangle with icon text and label.
 * Emits clicked() on tap or mouse click.
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
        anchors.margins: 6
        radius: 16
        color: ma.pressed ? Qt.darker(accent, 1.4) : "#2a2a30"
        border.color: accent
        border.width: 2

        Behavior on color { ColorAnimation { duration: 100 } }

        Column {
            anchors.centerIn: parent
            spacing: 8

            Text {
                text: btn.icon
                font.pixelSize: btn.height * 0.28
                color: accent
                anchors.horizontalCenter: parent.horizontalCenter
            }

            Text {
                text: btn.label
                font.pixelSize: btn.height * 0.12
                font.bold: true
                color: "#dcdcdc"
                anchors.horizontalCenter: parent.horizontalCenter
            }
        }

        MouseArea {
            id: ma
            anchors.fill: parent
            onClicked: btn.clicked()
        }
    }
}
