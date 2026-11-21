import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

import "controls" as C

// Signing in to PSN without a browser inside the app, A to Z: open Sony's sign-in page in the web
// browser, sign in, copy the address of the page it ends on (it carries a one-time code) and come
// back. The address is picked up from the clipboard by itself, or pasted. The page that uses this
// decides what the address is for (addressReady) and shows how that goes (statusKind, statusText).
ColumnLayout {
    id: guide

    // how the page is doing with the address: "" (nothing yet), "working", "error" or "done"
    property string statusKind: ""
    property string statusText: ""
    // take the address from the clipboard when Robz Play comes back to the front
    property bool listening: true
    // the console setup also takes an account ID pasted here (from chiaki-ng, for example)
    property bool acceptAccountId: false
    property bool firstInFocusChain: false

    property bool browserOpened: false
    property string loginLink: ""
    property bool linkCopied: false
    readonly property bool hidden: Chiaki.settings.streamerMode
    readonly property string redirectPrefix: "https://remoteplay.dl.playstation.net/remoteplay/redirect"

    // the address of the page after signing in, with its code
    signal addressReady(string address)
    signal accountIdPasted(string accountId)
    // what was given is not that address: says why
    signal problem(string message)

    function reset() {
        browserOpened = false;
        linkCopied = false;
        addressField.text = "";
    }

    function openSignIn() {
        loginLink = Chiaki.openPsnLink();
        browserOpened = true;
        linkCopied = false;
    }

    function copyLoginLink() {
        if (!loginLink)
            loginLink = Chiaki.psnLoginUrl().toString();
        clipboard.text = loginLink;
        clipboard.selectAll();
        clipboard.copy();
        clipboard.text = "";
        linkCopied = true;
    }

    function readClipboard() {
        clipboard.text = "";
        clipboard.paste();
        const text = clipboard.text.trim();
        clipboard.text = "";
        return text;
    }

    function useText(text, fromClipboard) {
        const t = text.trim();
        if (!t.length || statusKind === "working")
            return false;
        if (t.startsWith(redirectPrefix)) {
            if (t.indexOf("code=") < 0) {
                problem(qsTr("This address has no sign-in code in it. Sign in again and copy the whole address."));
                return true;
            }
            addressReady(t);
            return true;
        }
        // from the clipboard only the address is taken, nothing else that may be on it
        if (fromClipboard)
            return false;
        if (acceptAccountId && /^[A-Za-z0-9+\/]{8,}={0,2}$/.test(t)) {
            accountIdPasted(t);
            return true;
        }
        problem(qsTr("This is not the address of the page after signing in. It starts with %1").arg(redirectPrefix));
        return false;
    }

    spacing: 14

    // reads and writes the clipboard (QML has no clipboard of its own)
    TextEdit {
        id: clipboard
        visible: false
    }

    // back from the browser with the address copied: take it from the clipboard by itself
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state !== Qt.ApplicationActive || !guide.listening || !guide.visible
                    || !guide.browserOpened || guide.statusKind === "working" || guide.statusKind === "done")
                return;
            const text = guide.readClipboard();
            if (text.startsWith(guide.redirectPrefix) && text !== addressField.text.trim())
                addressField.text = text;
        }
    }

    RobzGuideStep {
        number: 1
        done: guide.browserOpened
        title: qsTr("Open the PlayStation sign-in page")
        note: qsTr("It opens in your web browser.")
        RowLayout {
            spacing: 12
            RobzPrimaryButton {
                firstInFocusChain: guide.firstInFocusChain
                text: guide.browserOpened ? qsTr("Open it again") : qsTr("Open sign-in page")
                onClicked: guide.openSignIn()
            }
            C.Button {
                visible: guide.browserOpened
                flat: true
                font.pixelSize: 14
                text: guide.linkCopied ? qsTr("Link copied: paste it into your browser")
                                       : qsTr("Browser did not open? Copy the link")
                onClicked: guide.copyLoginLink()
            }
        }
    }
    RobzGuideStep {
        number: 2
        title: qsTr("Sign in with the account you use on the console")
        note: qsTr("Your e-mail and password go only to Sony's page.")
    }
    RobzGuideStep {
        number: 3
        title: qsTr("Copy the address of the page that opens")
        note: qsTr("After signing in the browser shows an empty or error-like page: that is normal. Click the address bar, press <b>Ctrl+A</b>, then <b>Ctrl+C</b>.")
        // what the address bar shows, selected
        Rectangle {
            Layout.fillWidth: true
            Layout.maximumWidth: 520
            implicitHeight: 34
            radius: 17
            color: "#0F0C0B"
            border.color: "#3B302B"
            clip: true
            Rectangle {
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(addressSample.implicitWidth + 8, parent.width - 24)
                height: 22
                radius: 4
                color: "#5A3A2C"
            }
            Label {
                id: addressSample
                x: 16
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 32
                elide: Text.ElideRight
                text: "remoteplay.dl.playstation.net/remoteplay/redirect?code=…"
                color: "#FBF4EC"
                font.family: "Consolas"
                font.pixelSize: 13
            }
        }
    }
    RobzGuideStep {
        number: 4
        done: guide.statusKind === "done"
        title: qsTr("Come back to Robz Play")
        note: qsTr("The copied address is picked up by itself. If it is not, paste it here:")
        RowLayout {
            Layout.fillWidth: true
            Layout.maximumWidth: 560
            spacing: 10
            C.TextField {
                id: addressField
                Layout.fillWidth: true
                echoMode: guide.hidden ? TextInput.Password : TextInput.Normal
                placeholderText: "https://remoteplay.dl.playstation.net/remoteplay/redirect?code=…"
                enabled: guide.statusKind !== "working" && guide.statusKind !== "done"
                onTextChanged: {
                    const t = text.trim();
                    if (t.startsWith(guide.redirectPrefix) && t.indexOf("code=") >= 0)
                        guide.useText(t, false);
                }
                onAccepted: guide.useText(text, false)
            }
            C.Button {
                text: qsTr("Paste")
                font.pixelSize: 15
                enabled: addressField.enabled
                onClicked: {
                    const t = guide.readClipboard();
                    if (!t.length)
                        return;
                    const same = addressField.text.trim() === t;
                    addressField.text = t;
                    if (same || !t.startsWith(guide.redirectPrefix))
                        guide.useText(t, false);
                }
            }
        }
        RobzStatusLine {
            kind: guide.statusKind
            text: guide.statusText
        }
    }
}
