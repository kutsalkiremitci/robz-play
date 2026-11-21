import QtQuick

// The fill of Robz Play's main buttons, after the letters of the icon: cream running into
// terracotta from left to right, inside a dark outline and a warm rim. Purely decorative;
// the button that uses it keeps its own input handling.
Item {
    id: bg

    property real radius: 12
    property bool hovered: false
    property bool pressed: false
    // keyboard or controller focus: the rim lights up
    property bool focused: false
    // false: a quiet, dark button that cannot be pressed
    property bool active: true

    // the text colour that reads on this fill
    readonly property color textColor: active ? "#1A110D" : "#A08F84"

    Rectangle {
        anchors.fill: parent
        radius: bg.radius
        color: !bg.active ? "#4E403A" : (bg.focused ? "#FFF7EE" : "#E9997A")
    }
    Rectangle {
        anchors.fill: parent
        anchors.margins: 1.5
        radius: Math.max(0, bg.radius - 1.5)
        color: "#110E0D"
    }
    Rectangle {
        anchors.fill: parent
        anchors.margins: 3.5
        radius: Math.max(0, bg.radius - 3.5)
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop {
                position: 0.0
                color: !bg.active ? "#2A221F" : (bg.pressed ? "#F2D2B8" : (bg.hovered ? "#FFF1E4" : "#FCE6D3"))
            }
            GradientStop {
                position: 0.48
                color: !bg.active ? "#2A221F" : (bg.pressed ? "#E8A57F" : (bg.hovered ? "#F8C3A0" : "#F2B48F"))
            }
            GradientStop {
                position: 1.0
                color: !bg.active ? "#2A221F" : (bg.pressed ? "#D4744F" : (bg.hovered ? "#EE946E" : "#E4845E"))
            }
        }
    }
    // the gloss on the upper half of the letters
    Rectangle {
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            margins: 3.5
        }
        height: (parent.height - 7) * 0.45
        radius: Math.max(0, bg.radius - 3.5)
        visible: bg.active
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#33FFFFFF" }
            GradientStop { position: 1.0; color: "#00FFFFFF" }
        }
    }
}
