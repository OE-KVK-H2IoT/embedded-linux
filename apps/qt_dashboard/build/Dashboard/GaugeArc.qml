/*
 * GaugeArc.qml - Reusable arc gauge component
 *
 * Draws a 270° arc with a coloured fill proportional to the
 * current value.  Used for the temperature panel.
 *
 * Copyright (C) 2025 Obuda University - Embedded Systems Lab
 * SPDX-License-Identifier: MIT
 */

import QtQuick

Item {
    id: gauge

    property real value: 0
    property real minValue: 0
    property real maxValue: 100
    property string label: ""
    property string unit: ""

    Canvas {
        id: arcCanvas
        anchors.fill: parent

        onPaint: {
            var ctx = getContext("2d");
            var w = width;
            var h = height;
            ctx.clearRect(0, 0, w, h);

            var cx = w / 2;
            var cy = h / 2;
            var radius = Math.min(w, h) / 2 - 20;
            var lineWidth = 10;

            /* Arc angles: 135° to 405° (270° sweep, opening at bottom) */
            var startAngle = 135 * Math.PI / 180;
            var endAngle   = 405 * Math.PI / 180;

            /* Background arc */
            ctx.beginPath();
            ctx.arc(cx, cy, radius, startAngle, endAngle);
            ctx.strokeStyle = "#3c3c41";
            ctx.lineWidth = lineWidth;
            ctx.lineCap = "round";
            ctx.stroke();

            /* Value arc */
            var t = (gauge.value - gauge.minValue) / (gauge.maxValue - gauge.minValue);
            t = Math.max(0, Math.min(1, t));
            var valueAngle = startAngle + t * (endAngle - startAngle);

            ctx.beginPath();
            ctx.arc(cx, cy, radius, startAngle, valueAngle);
            ctx.strokeStyle = "#00c878";
            ctx.lineWidth = lineWidth;
            ctx.lineCap = "round";
            ctx.stroke();
        }

        Connections {
            target: gauge
            function onValueChanged() { arcCanvas.requestPaint(); }
        }
    }

    /* Value text */
    Text {
        anchors.centerIn: parent
        text: gauge.value.toFixed(1) + " " + gauge.unit
        color: "#dcdcdc"
        font.pixelSize: parent.height * 0.12
        font.bold: true
    }

    /* Label */
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: parent.height * 0.18
        text: gauge.label
        color: "#646469"
        font.pixelSize: parent.height * 0.06
        font.bold: true
    }
}
