import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

import "controls" as C

// Robz Play's main button: filled like the letters of the icon (see RobzButtonBg).
C.Button {
    id: button
    background: RobzButtonBg {
        implicitHeight: 52
        radius: 12
        active: button.enabled
        hovered: button.hovered
        pressed: button.pressed
        focused: button.visualFocus
    }
    Material.foreground: enabled ? "#1A110D" : "#A08F84"
    font.pixelSize: 17
    font.family: "Segoe UI"
    font.weight: Font.Black
    font.italic: true
    leftPadding: 28
    rightPadding: 28
    topPadding: 14
    bottomPadding: 14
}
