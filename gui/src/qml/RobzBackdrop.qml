import QtQuick

// The backdrop of Robz Play's connecting and loading screens: a warm dark field with a
// glow rising from one corner and "ROBZ PLAY" written across it as a giant watermark.
// Purely decorative - it takes no input and holds no state.
Item {
    id: backdrop
    clip: true

    // gentle breathing of the watermark while the screen is up
    property bool animated: true

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#1D1B19" }
            GradientStop { position: 1.0; color: "#0F0E0D" }
        }
    }

    // glow: a wide soft band of the accent colour, tilted, fading out towards the middle
    Rectangle {
        width: backdrop.width * 3.2
        height: backdrop.height * 0.9
        x: -backdrop.width * 1.1
        y: backdrop.height * 0.45
        rotation: -24
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#00D97757" }
            GradientStop { position: 0.55; color: "#2AD97757" }
            GradientStop { position: 1.0; color: "#00D97757" }
        }
    }
    Rectangle {
        width: backdrop.width * 3.2
        height: backdrop.height * 0.6
        x: -backdrop.width * 1.1
        y: -backdrop.height * 0.30
        rotation: -24
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#00D97757" }
            GradientStop { position: 0.6; color: "#16D97757" }
            GradientStop { position: 1.0; color: "#00D97757" }
        }
    }

    // watermark: three rows of the name, the middle one across the centre of the screen
    Item {
        id: watermark
        anchors.centerIn: parent
        width: backdrop.width
        height: backdrop.height
        rotation: -9
        opacity: 1.0

        SequentialAnimation on opacity {
            running: backdrop.animated && backdrop.visible
            loops: Animation.Infinite
            NumberAnimation { from: 1.0; to: 0.62; duration: 2600; easing.type: Easing.InOutSine }
            NumberAnimation { from: 0.62; to: 1.0; duration: 2600; easing.type: Easing.InOutSine }
        }

        Repeater {
            model: 3

            Text {
                required property int index
                readonly property bool middle: index === 1
                text: "ROBZ PLAY"
                font.pixelSize: Math.round(backdrop.height * (middle ? 0.30 : 0.24))
                font.weight: Font.Black
                font.letterSpacing: backdrop.height * 0.014
                color: middle ? "#FAF9F5" : "#D97757"
                opacity: middle ? 0.075 : 0.05
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.horizontalCenterOffset: (index - 1) * backdrop.width * 0.11
                y: watermark.height * 0.5 - height * 0.5 + (index - 1) * backdrop.height * 0.34
            }
        }
    }
}
