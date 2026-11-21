import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

// A line of state under a form: busy, wrong or done. Hidden while there is nothing to say.
RowLayout {
    id: statusLine
    // "" (nothing to say), "working", "error" or "done"
    property string kind: ""
    property string text
    Layout.fillWidth: true
    visible: kind.length > 0 && text.length > 0
    spacing: 10

    BusyIndicator {
        visible: statusLine.kind === "working"
        running: visible
        implicitWidth: 26
        implicitHeight: 26
    }
    Label {
        visible: statusLine.kind === "done"
        text: "✓"
        color: "#8DB580"
        font.pixelSize: 20
        font.bold: true
    }
    Label {
        Layout.fillWidth: true
        text: statusLine.text
        color: statusLine.kind === "error" ? "#D9A65A" : (statusLine.kind === "done" ? "#8DB580" : "#CDBFB4")
        font.pixelSize: 14
        wrapMode: Text.Wrap
    }
}
