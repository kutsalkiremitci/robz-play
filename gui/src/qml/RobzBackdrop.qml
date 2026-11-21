import QtQuick

// The backdrop of Robz Play's connecting and loading screens: the sunset of the icon - a striped
// sun going down behind a city and dotted palms, over a grid floor - dimmed so that the text
// on top stays easy to read. Purely decorative - it takes no input and holds no state.
Item {
    id: backdrop
    clip: true

    // the sun breathes gently while the screen is up
    property bool animated: true

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#1B1514" }
            GradientStop { position: 1.0; color: "#0C0A09" }
        }
    }

    // gui/res/robz-gunbatimi.png: 1920x1080, the horizon at 81% of its height; cropped to the
    // window from the bottom up, so the horizon and the floor always show
    Image {
        id: sunset
        anchors.fill: parent
        source: "qrc:/icons/robz-gunbatimi.png"
        fillMode: Image.PreserveAspectCrop
        verticalAlignment: Image.AlignBottom
        smooth: true
        opacity: 0.6

        SequentialAnimation on opacity {
            running: backdrop.animated && backdrop.visible
            loops: Animation.Infinite
            NumberAnimation { from: 0.6; to: 0.45; duration: 2600; easing.type: Easing.InOutSine }
            NumberAnimation { from: 0.45; to: 0.6; duration: 2600; easing.type: Easing.InOutSine }
        }
    }

    // a soft darkening in the middle, where the logo and the text stand
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#00141110" }
            GradientStop { position: 0.5; color: "#59141110" }
            GradientStop { position: 1.0; color: "#00141110" }
        }
    }
}
