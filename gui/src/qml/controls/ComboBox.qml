import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

ComboBox {
    id: control
    property bool firstInFocusChain: false
    property bool lastInFocusChain: false
    implicitContentWidthPolicy: ComboBox.WidestText

    // a filled, rounded field instead of the outlined one
    background: Rectangle {
        implicitWidth: 120
        implicitHeight: control.Material.textFieldHeight
        radius: 10
        color: control.popup.visible ? "#271F1C" : (control.hovered ? "#1A1514" : "#171312")
        border.width: (control.visualFocus || control.popup.visible) ? 2 : 1
        border.color: (control.visualFocus || control.popup.visible) ? "#E4845E" : (control.hovered ? "#5E4E46" : "#3B302B")
        opacity: control.enabled ? 1.0 : 0.5
    }

    Keys.onPressed: (event) => {
        switch (event.key) {
        case Qt.Key_Up:
            if (!popup.visible) {
                let item = nextItemInFocusChain(false);
                if (!firstInFocusChain && item)
                    item.forceActiveFocus(Qt.TabFocusReason);
                event.accepted = true;
            }
            break;
        case Qt.Key_Down:
            if (!popup.visible) {
                let item = nextItemInFocusChain();
                if (!lastInFocusChain && item)
                    item.forceActiveFocus(Qt.TabFocusReason);
                event.accepted = true;
            }
            break;
        case Qt.Key_Return:
            if (popup.visible) {
                activated(highlightedIndex);
                popup.close();
            } else {
                popup.open();
            }
            event.accepted = true;
            break;
        }
    }

    Keys.onReleased: (event) => {
        if (event.key == Qt.Key_Return)
            event.accepted = true;
    }
}
