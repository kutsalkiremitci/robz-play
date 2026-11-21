import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

// One numbered step of a guide: a little tile filled like the letters of the icon, a line
// saying what to do, a note under it and, under that, whatever the step needs (a button, a
// field). A done step shows a green tick instead of its number.
RowLayout {
    id: guideStep
    property int number
    property string title
    property string note
    property bool done: false
    default property alias extra: guideColumn.data
    Layout.fillWidth: true
    spacing: 12

    Item {
        Layout.alignment: Qt.AlignTop
        implicitWidth: 28
        implicitHeight: 28
        RobzButtonBg {
            anchors.fill: parent
            radius: 8
            visible: !guideStep.done
        }
        Rectangle {
            anchors.fill: parent
            radius: 8
            visible: guideStep.done
            color: "#8DB580"
        }
        Label {
            anchors.centerIn: parent
            text: guideStep.done ? "✓" : guideStep.number
            color: "#1A110D"
            font.pixelSize: 14
            font.family: "Segoe UI"
            font.weight: Font.Black
            font.italic: !guideStep.done
        }
    }
    ColumnLayout {
        id: guideColumn
        Layout.fillWidth: true
        spacing: 6
        Label {
            Layout.fillWidth: true
            Layout.topMargin: 3
            text: guideStep.title
            color: "#FBF4EC"
            font.pixelSize: 16
            font.bold: true
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: guideStep.note
            color: "#A08F84"
            font.pixelSize: 13
            lineHeight: 1.15
            wrapMode: Text.Wrap
            textFormat: Text.StyledText
        }
    }
}
