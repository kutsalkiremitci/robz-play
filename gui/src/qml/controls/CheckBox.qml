import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// Robz Play draws every check box of the app as an on/off switch. Behaviour, signals and
// key handling are those of the plain check box.
CheckBox {
    id: control
    property bool firstInFocusChain: false
    property bool lastInFocusChain: false
    property bool sendOutput: false

    indicator: Rectangle {
        implicitWidth: 46
        implicitHeight: 26
        x: control.text ? (control.mirrored ? control.width - width - control.rightPadding : control.leftPadding) : control.leftPadding + (control.availableWidth - width) / 2
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: height / 2
        color: control.checked ? "#E4845E" : "#4E403A"
        opacity: control.enabled ? 1.0 : 0.45
        Behavior on color { ColorAnimation { duration: 120 } }

        // shown while the switch is reached with the keyboard or a controller
        Rectangle {
            anchors {
                fill: parent
                margins: -4
            }
            radius: height / 2
            color: "transparent"
            border.width: 2
            border.color: "#FBF4EC"
            visible: control.visualFocus
        }

        Rectangle {
            width: 20
            height: 20
            radius: 10
            y: 3
            x: control.checked ? parent.width - width - 3 : 3
            color: "#FBF4EC"
            Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
        }
    }

    Keys.onPressed: (event) => {
        switch (event.key) {
        case Qt.Key_Up:
            if (!firstInFocusChain) {
                let item = nextItemInFocusChain(false);
                if (item)
                    item.forceActiveFocus(Qt.TabFocusReason);
                if(!sendOutput)
                    event.accepted = true;
            }
            break;
        case Qt.Key_Down:
            if (!lastInFocusChain) {
                let item = nextItemInFocusChain();
                if (item)
                    item.forceActiveFocus(Qt.TabFocusReason);
                if(!sendOutput)
                    event.accepted = true;
            }
            break;
        case Qt.Key_Return:
            if (visualFocus) {
                toggle();
                clicked();
            }
            event.accepted = true;
            break;
        }
    }
}
