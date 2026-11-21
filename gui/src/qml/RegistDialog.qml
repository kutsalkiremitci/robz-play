import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

import "controls" as C

// Robz Play: registering a console, step by step. The form is on the left with its buttons
// under it; the right side says what the step is for, why it is needed and what to do on
// the console, so that someone setting it up for the first time knows the way.
DialogView {
    id: dialog
    property bool ps5: true
    property alias host: hostField.text
    // development aid (ROBZPLAY_UI_PREVIEW=regist-N): open on a given step with sample data
    property int startStep: 0
    property bool previewMode: false
    property string previewState: ""

    property int step: 0
    // registering: "" (not yet), "working", "done", "failed"
    property string regState: ""
    property string regLog: ""
    // why the console turned registering down, from its answer: "account" (0x80108b02, the
    // account is not the one of the profile that opened Link Device), "pin" (0x80108b09) or ""
    readonly property string regReason: regLog.indexOf("0x80108b02") >= 0 ? "account"
                                      : regLog.indexOf("0x80108b09") >= 0 ? "pin" : ""

    // back to the code, for a new one
    function enterCodeAgain() {
        regState = "";
        regLog = "";
        pin.text = "";
        step = 2;
    }

    title: qsTr("Register Console")
    buttonVisible: false

    readonly property color cBg: "#141110"
    readonly property color cSurface: "#1D1816"
    readonly property color cInset: "#171312"
    readonly property color cBorder: "#3B302B"
    readonly property color cText: "#FBF4EC"
    readonly property color cMuted: "#CDBFB4"
    readonly property color cFaint: "#A08F84"
    readonly property color cAccent: "#E4845E"
    readonly property color cOnAccent: "#141110"
    readonly property color cGood: "#8DB580"
    readonly property color cWarn: "#D9A65A"
    readonly property bool hidden: Chiaki.settings.streamerMode

    // the console's protocol version: PS5, or a PS4 by its system software
    readonly property int target: ps5Choice ? 1000100 : [1000, 900, 800][ps4Firmware.currentIndex]
    property bool ps5Choice: ps5
    readonly property bool needsOnlineId: target === 800
    readonly property string psnId: needsOnlineId ? onlineId.text.trim() : accountId.text.trim()
    readonly property var stepNames: [qsTr("Console"), qsTr("Account"), qsTr("Pairing code"), qsTr("Register")]

    function stepReady(i) {
        switch (i) {
        case 0: return hostField.text.trim().length > 0;
        case 1: return psnId.length > 0;
        case 2: return pin.acceptableInput && cpin.acceptableInput;
        default: return true;
        }
    }

    function goNext() {
        if (step < 3) {
            if (stepReady(step))
                step++;
            return;
        }
        if (regState === "done") {
            dialog.close();
            return;
        }
        startRegister();
    }

    function goBack() {
        if (regState === "working")
            return;
        if (step === 0) {
            dialog.close();
            return;
        }
        regState = "";
        step--;
    }

    function startRegister() {
        regLog = "";
        regState = "working";
        const ok = Chiaki.registerHost(hostField.text.trim(), psnId, pin.text.trim(), cpin.text.trim(),
                                       hostField.text.trim() == "255.255.255.255", target, function(msg, ok, done) {
            if (!done) {
                regLog += msg + "\n";
                return;
            }
            regState = ok ? "done" : "failed";
        });
        if (!ok)
            regState = "failed";
    }

    Component.onCompleted: {
        step = Math.max(0, Math.min(3, startStep));
        if (previewMode && step >= 2 && !accountId.text)
            accountId.text = "c2FtcGxlLWFjY291bnQ=";
        if (previewMode && step >= 3)
            pin.text = "12345678";
        if (previewMode && previewState)
            regState = previewState;
        // regist-6: what the console says when the code was opened on another profile
        if (previewMode && previewState === "failed")
            regLog = "[I] Regist waiting for response\n[E] Regist received HTTP code 403\n"
                   + "[E] Reported Application Reason: 0x80108b02 (Invalid PSN ID)\n[E] Regist eventually failed\n";
    }

    StackView.onActivated: {
        if (Chiaki.settings.psnAccountId)
            accountId.text = Chiaki.settings.psnAccountId;
    }

    // ------------------------------------------------------------------ the account
    // Two ways to the account ID. 0: sign in on Sony's page in the browser and bring the address
    // of the page it ends on back here (it carries a one-time code). 1: look the online ID up
    // with a free outside service (not Sony's; it does not always answer).
    property int accountWay: 0
    // "" / "working" / "error"
    property string psnState: ""
    property string psnError: ""
    property string lookupState: ""
    property string lookupError: ""
    property int lookupTry: 0
    readonly property int lookupTries: 3

    function findByOnlineId() {
        const name = lookupField.text.trim();
        if (!name.length || lookupState === "working")
            return;
        lookupState = "working";
        lookupError = "";
        lookupTry = 1;
        sendLookup(name);
    }

    function sendLookup(name) {
        const request = new XMLHttpRequest();
        request.onreadystatechange = function() {
            if (request.readyState !== XMLHttpRequest.DONE)
                return;
            let response = null;
            try {
                response = JSON.parse(request.responseText);
            } catch (e) {
                response = null;
            }
            const id = response ? response["encoded_id"] : "";
            if (id) {
                lookupState = "";
                accountId.text = id;
                return;
            }
            if (response && response["error"]) {
                lookupState = "error";
                lookupError = qsTr("Not found: %1").arg(response["error"]);
                return;
            }
            // no answer: the service is often busy, so it is asked a few times
            if (lookupTry < lookupTries) {
                lookupTry++;
                lookupRetry.lookupName = name;
                lookupRetry.start();
                return;
            }
            lookupState = "error";
            lookupError = qsTr("No answer after %1 tries (%2). Try again in a few minutes, check the privacy settings, or use the first way.")
                .arg(lookupTries).arg(request.status || qsTr("no connection"));
        };
        request.open("GET", "https://psn.flipscreen.games/search.php?username=%1".arg(encodeURIComponent(name)));
        request.send();
    }


    // ------------------------------------------------------------------ building blocks

    // a PlayStation menu path, one chip per level
    component MenuPath: Flow {
        property var items: []
        spacing: 6
        Repeater {
            model: parent.items
            Row {
                spacing: 6
                Rectangle {
                    radius: 6
                    color: dialog.cInset
                    border.color: dialog.cBorder
                    height: chipLabel.implicitHeight + 10
                    width: chipLabel.implicitWidth + 18
                    Label {
                        id: chipLabel
                        anchors.centerIn: parent
                        text: modelData
                        color: dialog.cText
                        font.pixelSize: 14
                    }
                }
                Label {
                    visible: index < parent.parent.items.length - 1
                    anchors.verticalCenter: parent.verticalCenter
                    text: "›"
                    color: dialog.cFaint
                    font.pixelSize: 16
                }
            }
        }
    }

    // a heading and a paragraph of the explanation on the right
    component InfoBlock: ColumnLayout {
        property string heading
        property string body
        Layout.fillWidth: true
        spacing: 6
        Label {
            Layout.fillWidth: true
            text: parent.heading
            color: dialog.cAccent
            font.pixelSize: 13
            font.bold: true
            font.capitalization: Font.AllUppercase
            font.letterSpacing: 1
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: parent.body
            color: dialog.cMuted
            font.pixelSize: 15
            lineHeight: 1.25
            wrapMode: Text.Wrap
            textFormat: Text.StyledText
        }
    }

    // one point of a checklist
    component Bullet: RowLayout {
        property string text
        Layout.fillWidth: true
        spacing: 10
        Rectangle {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 7
            width: 6
            height: 6
            radius: 3
            color: dialog.cAccent
        }
        Label {
            Layout.fillWidth: true
            text: parent.text
            color: dialog.cMuted
            font.pixelSize: 15
            lineHeight: 1.2
            wrapMode: Text.Wrap
            textFormat: Text.StyledText
        }
    }

    // a label above a field
    component FieldLabel: Label {
        Layout.fillWidth: true
        color: dialog.cMuted
        font.pixelSize: 14
        wrapMode: Text.Wrap
    }

    // one of a few choices (PS5 / PS4)
    component ChoiceChip: Rectangle {
        id: chip
        property string text
        property bool selected
        signal picked()
        activeFocusOnTab: true
        implicitWidth: Math.max(110, chipText.implicitWidth + 48)
        implicitHeight: 46
        radius: 10
        color: selected ? dialog.cAccent : (chipMouse.containsMouse ? "#271F1C" : dialog.cSurface)
        border.width: activeFocus ? 2 : 1
        border.color: activeFocus ? dialog.cText : (selected ? dialog.cAccent : dialog.cBorder)
        Label {
            id: chipText
            anchors.centerIn: parent
            text: chip.text
            color: chip.selected ? dialog.cOnAccent : dialog.cText
            font.pixelSize: 17
            font.bold: true
        }
        MouseArea {
            id: chipMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.picked()
        }
        Keys.onReturnPressed: picked()
        Keys.onSpacePressed: picked()
    }

    // filled like the letters of the icon (RobzPrimaryButton.qml)
    component PrimaryButton: RobzPrimaryButton {}

    // ------------------------------------------------------------------ page

    Item {
        // the helpers that are not drawn: a retry timer and the answers from Sony (DialogView
        // takes this one page item only)
        Timer {
            id: lookupRetry
            property string lookupName
            interval: 1500
            onTriggered: dialog.sendLookup(lookupName)
        }

        Connections {
            target: Chiaki
            enabled: dialog.psnState === "working"
            function onPsnLoginAccountIdDone(id) {
                dialog.psnState = "";
                signInGuide.reset();
                accountId.text = id;
            }
            function onPsnLoginAccountIdError(error) {
                dialog.psnState = "error";
                dialog.psnError = qsTr("Sony did not accept it: %1. The code is valid for a few minutes only: open the sign-in page again and bring the new address.").arg(error);
            }
        }

        RowLayout {
            id: columns
            anchors.fill: parent
            anchors.margins: 32
            spacing: 32

            // ---------------- left: the steps and the form (it scrolls when the window is short)
            Flickable {
                id: formFlick
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: columns.width * 0.56
                contentHeight: formColumn.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: formFlick.contentHeight > formFlick.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }

                ColumnLayout {
                    id: formColumn
                    width: formFlick.width - (formFlick.contentHeight > formFlick.height ? 18 : 0)
                    spacing: 22

                    // where we are: 1 Console - 2 Account - 3 Pairing code - 4 Register
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Repeater {
                            model: dialog.stepNames
                            RowLayout {
                                Layout.fillWidth: index < dialog.stepNames.length - 1
                                spacing: 10
                                readonly property bool done: index < dialog.step || (index === 3 && dialog.regState === "done")
                                readonly property bool current: index === dialog.step && !done
                                // tiles like the ones on the home screen: the current step is filled
                                // like the letters of the icon
                                Item {
                                    id: stepMark
                                    implicitWidth: 32
                                    implicitHeight: 32
                                    readonly property bool done: parent.done
                                    readonly property bool current: parent.current
                                    RobzButtonBg {
                                        anchors.fill: parent
                                        radius: 9
                                        visible: stepMark.current
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: 9
                                        visible: !stepMark.current
                                        color: stepMark.done ? dialog.cGood : "transparent"
                                        border.width: stepMark.done ? 0 : 2
                                        border.color: dialog.cBorder
                                    }
                                    Label {
                                        anchors.centerIn: parent
                                        text: stepMark.done ? "✓" : (index + 1)
                                        color: stepMark.done || stepMark.current ? "#1A110D" : dialog.cFaint
                                        font.pixelSize: 15
                                        font.family: "Segoe UI"
                                        font.weight: Font.Black
                                        font.italic: !stepMark.done
                                    }
                                }
                                Label {
                                    text: modelData
                                    color: parent.current ? dialog.cText : (parent.done ? dialog.cMuted : dialog.cFaint)
                                    font.pixelSize: 14
                                    font.bold: parent.current
                                }
                                Rectangle {
                                    visible: index < dialog.stepNames.length - 1
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 4
                                    Layout.rightMargin: 14
                                    height: 2
                                    radius: 1
                                    color: index < dialog.step ? dialog.cGood : dialog.cBorder
                                }
                            }
                        }
                    }

                    // the step's own heading
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Label {
                            Layout.fillWidth: true
                            text: [qsTr("Which console?"), qsTr("Your PlayStation account"), qsTr("Pair with the console"),
                                   dialog.regState === "done" ? qsTr("Console registered") : qsTr("Ready to register")][dialog.step]
                            color: dialog.cText
                            font.family: "Segoe UI"
                            font.weight: Font.Black
                            font.italic: true
                            font.pixelSize: 30
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            text: [qsTr("Robz Play found this console on your network. Check that the details are right."),
                                   qsTr("Find the account you use on the console. You do this only once."),
                                   qsTr("Show a code on the console and type it in here."),
                                   dialog.regState === "done"
                                       ? qsTr("You can now press Play on the console's card on the home screen.")
                                       : qsTr("Check the summary, then register. Keep the code on the console's screen until it is done.")][dialog.step]
                            color: dialog.cFaint
                            font.pixelSize: 15
                            wrapMode: Text.Wrap
                        }
                    }

                    // the forms, one per step
                    StackLayout {
                        id: pages
                        Layout.fillWidth: true
                        // as tall as the step on show, so the buttons sit right under its form
                        Layout.fillHeight: false
                        Layout.preferredHeight: children[currentIndex] ? children[currentIndex].implicitHeight : 0
                        currentIndex: dialog.step

                        // 1 - Console
                        ColumnLayout {
                            spacing: 10
                            FieldLabel { text: qsTr("Console") }
                            RowLayout {
                                spacing: 10
                                ChoiceChip {
                                    text: "PS5"
                                    selected: dialog.ps5Choice
                                    onPicked: dialog.ps5Choice = true
                                }
                                ChoiceChip {
                                    text: "PS4"
                                    selected: !dialog.ps5Choice
                                    onPicked: dialog.ps5Choice = false
                                }
                            }
                            FieldLabel {
                                visible: !dialog.ps5Choice
                                Layout.topMargin: 8
                                text: qsTr("PS4 system software")
                            }
                            C.ComboBox {
                                id: ps4Firmware
                                visible: !dialog.ps5Choice
                                Layout.preferredWidth: 360
                                model: [qsTr("8.0 or later"), qsTr("7.0 to 7.x"), qsTr("Older than 7.0")]
                            }
                            FieldLabel {
                                Layout.topMargin: 8
                                text: qsTr("Console address on your network")
                            }
                            C.TextField {
                                id: hostField
                                Layout.fillWidth: true
                                Layout.maximumWidth: 480
                                echoMode: dialog.hidden ? TextInput.Password : TextInput.Normal
                                placeholderText: qsTr("for example 192.168.1.20")
                                onAccepted: dialog.goNext()
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Filled in automatically when the console was found. Change it only if you know it is wrong.")
                                color: dialog.cFaint
                                font.pixelSize: 13
                                wrapMode: Text.Wrap
                            }
                        }

                        // 2 - Account
                        ColumnLayout {
                            spacing: 14
                            // found already
                            Rectangle {
                                visible: !dialog.needsOnlineId && accountId.text.trim().length > 0
                                Layout.fillWidth: true
                                Layout.maximumWidth: 560
                                implicitHeight: readyRow.implicitHeight + 32
                                radius: 12
                                color: "#1F2A1D"
                                border.color: "#3F5A38"
                                RowLayout {
                                    id: readyRow
                                    anchors.fill: parent
                                    anchors.margins: 16
                                    spacing: 14
                                    Label { text: "✓"; color: dialog.cGood; font.pixelSize: 26; font.bold: true }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Label {
                                            text: qsTr("Account ready")
                                            color: dialog.cText
                                            font.pixelSize: 17
                                            font.bold: true
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: dialog.hidden ? qsTr("Account ID hidden") : qsTr("Account ID %1").arg(accountId.text.trim())
                                            color: dialog.cMuted
                                            font.pixelSize: 13
                                            elide: Text.ElideRight
                                        }
                                    }
                                    C.Button {
                                        text: qsTr("Use another account")
                                        flat: true
                                        font.pixelSize: 14
                                        onClicked: {
                                            accountId.text = "";
                                            signInGuide.reset();
                                            dialog.psnState = "";
                                            dialog.lookupState = "";
                                        }
                                    }
                                }
                            }

                            // not found yet: the two ways
                            ColumnLayout {
                                visible: !dialog.needsOnlineId && accountId.text.trim().length === 0
                                Layout.fillWidth: true
                                spacing: 14

                                Flow {
                                    Layout.fillWidth: true
                                    spacing: 10
                                    ChoiceChip {
                                        text: qsTr("1 · Sign in in the browser")
                                        selected: dialog.accountWay === 0
                                        onPicked: dialog.accountWay = 0
                                    }
                                    ChoiceChip {
                                        text: qsTr("2 · Find by online ID")
                                        selected: dialog.accountWay === 1
                                        onPicked: dialog.accountWay = 1
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.topMargin: -4
                                    text: dialog.accountWay === 0
                                          ? qsTr("Recommended: it always works. You sign in on Sony's own page; Robz Play never sees your password.")
                                          : qsTr("No sign-in, but it asks a free outside service that sometimes answers only after a few tries.")
                                    color: dialog.cFaint
                                    font.pixelSize: 13
                                    wrapMode: Text.Wrap
                                }

                                // way 1: the browser, A to Z (PsnSignInGuide.qml)
                                PsnSignInGuide {
                                    id: signInGuide
                                    visible: dialog.accountWay === 0
                                    Layout.fillWidth: true
                                    acceptAccountId: true
                                    firstInFocusChain: true
                                    listening: dialog.step === 1 && dialog.accountWay === 0 && accountId.text.trim().length === 0
                                    statusKind: dialog.psnState
                                    statusText: dialog.psnState === "working" ? qsTr("Getting your account ID from Sony…") : dialog.psnError
                                    onAddressReady: (address) => {
                                        dialog.psnState = "working";
                                        dialog.psnError = "";
                                        Chiaki.handlePsnLoginRedirect(address);
                                    }
                                    onAccountIdPasted: (id) => accountId.text = id
                                    onProblem: (message) => {
                                        dialog.psnState = "error";
                                        dialog.psnError = message;
                                    }
                                }

                                // way 2: the online ID
                                ColumnLayout {
                                    visible: dialog.accountWay === 1
                                    Layout.fillWidth: true
                                    spacing: 14

                                    RobzGuideStep {
                                        number: 1
                                        title: qsTr("Let anyone find your account")
                                        note: qsTr("On the PS5, set everything to <b>Anyone</b> in:")
                                        MenuPath {
                                            Layout.fillWidth: true
                                            items: [qsTr("Settings"), qsTr("Users and Accounts"), qsTr("Privacy")]
                                        }
                                    }
                                    RobzGuideStep {
                                        number: 2
                                        title: qsTr("Type your online ID and find it")
                                        note: qsTr("Your PSN user name, as on the console; capitals matter.")
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.maximumWidth: 560
                                            spacing: 10
                                            C.TextField {
                                                id: lookupField
                                                Layout.fillWidth: true
                                                echoMode: dialog.hidden ? TextInput.Password : TextInput.Normal
                                                placeholderText: qsTr("online ID")
                                                enabled: dialog.lookupState !== "working"
                                                onAccepted: dialog.findByOnlineId()
                                            }
                                            PrimaryButton {
                                                text: qsTr("Find")
                                                enabled: lookupField.text.trim().length > 0 && dialog.lookupState !== "working"
                                                onClicked: dialog.findByOnlineId()
                                            }
                                        }
                                        RobzStatusLine {
                                            kind: dialog.lookupState
                                            text: dialog.lookupState === "working"
                                                  ? qsTr("Looking it up… (try %1 of %2)").arg(dialog.lookupTry).arg(dialog.lookupTries)
                                                  : dialog.lookupError
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("This lookup is not Sony's. It sometimes does not answer at first: Robz Play tries %1 times, and another go a few minutes later often works.").arg(dialog.lookupTries)
                                        color: dialog.cFaint
                                        font.pixelSize: 13
                                        wrapMode: Text.Wrap
                                    }
                                }
                            }

                            // where the account ID is kept (shown in the card above)
                            C.TextField {
                                id: accountId
                                visible: false
                            }
                            // PS4 before 7.0 knows accounts by their online ID
                            FieldLabel {
                                visible: dialog.needsOnlineId
                                text: qsTr("PSN online ID (case-sensitive)")
                            }
                            C.TextField {
                                id: onlineId
                                visible: dialog.needsOnlineId
                                Layout.fillWidth: true
                                Layout.maximumWidth: 480
                                echoMode: dialog.hidden ? TextInput.Password : TextInput.Normal
                                placeholderText: qsTr("username")
                                onAccepted: dialog.goNext()
                            }
                        }

                        // 3 - Pairing code
                        ColumnLayout {
                            spacing: 10
                            FieldLabel { text: qsTr("On the console, open:") }
                            MenuPath {
                                Layout.fillWidth: true
                                items: dialog.ps5Choice
                                    ? [qsTr("Settings"), qsTr("System"), qsTr("Remote Play"), qsTr("Link Device")]
                                    : [qsTr("Settings"), qsTr("Remote Play Connection Settings"), qsTr("Add Device")]
                            }
                            // the code belongs to the profile that opens it
                            Label {
                                Layout.fillWidth: true
                                Layout.maximumWidth: 560
                                text: qsTr("Open it on the PS5 profile of <b>the account you signed in with</b> (your main profile). A code opened on another profile, such as the empty second one, is refused.")
                                color: dialog.cMuted
                                font.pixelSize: 14
                                wrapMode: Text.Wrap
                                textFormat: Text.StyledText
                            }
                            FieldLabel {
                                Layout.topMargin: 14
                                text: qsTr("The 8-digit code shown on the console")
                            }
                            C.TextField {
                                id: pin
                                Layout.preferredWidth: 300
                                font.pixelSize: 26
                                font.letterSpacing: 4
                                horizontalAlignment: TextInput.AlignHCenter
                                validator: RegularExpressionValidator { regularExpression: /[0-9]{8}/ }
                                echoMode: dialog.hidden ? TextInput.Password : TextInput.Normal
                                placeholderText: "12345678"
                                firstInFocusChain: true
                                onAccepted: dialog.goNext()
                            }
                            Label {
                                visible: pin.text.length > 0 && !pin.acceptableInput
                                text: qsTr("The code has 8 digits.")
                                color: dialog.cWarn
                                font.pixelSize: 13
                            }
                            FieldLabel {
                                Layout.topMargin: 14
                                text: qsTr("Login passcode of your account on the console (optional, 4 digits)")
                            }
                            C.TextField {
                                id: cpin
                                Layout.preferredWidth: 300
                                validator: RegularExpressionValidator { regularExpression: /^$|[0-9]{4}/ }
                                echoMode: dialog.hidden ? TextInput.Password : TextInput.Normal
                                placeholderText: qsTr("leave empty if you have none")
                                onAccepted: dialog.goNext()
                            }
                        }

                        // 4 - Register
                        ColumnLayout {
                            spacing: 12
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.maximumWidth: 560
                                implicitHeight: summary.implicitHeight + 28
                                radius: 12
                                color: dialog.cSurface
                                border.color: dialog.cBorder
                                GridLayout {
                                    id: summary
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    columns: 3
                                    rowSpacing: 10
                                    columnSpacing: 16
                                    Label { text: qsTr("Console"); color: dialog.cFaint; font.pixelSize: 14 }
                                    Label {
                                        Layout.fillWidth: true
                                        text: dialog.ps5Choice ? "PS5" : "PS4 (%1)".arg(ps4Firmware.currentText)
                                        color: dialog.cText
                                        font.pixelSize: 15
                                    }
                                    Label { text: "✓"; color: dialog.cGood; font.bold: true }
                                    Label { text: qsTr("Address"); color: dialog.cFaint; font.pixelSize: 14 }
                                    Label {
                                        Layout.fillWidth: true
                                        text: dialog.hidden ? qsTr("hidden") : hostField.text.trim()
                                        color: dialog.cText
                                        font.pixelSize: 15
                                    }
                                    Label { text: dialog.stepReady(0) ? "✓" : "!"; color: dialog.stepReady(0) ? dialog.cGood : dialog.cWarn; font.bold: true }
                                    Label { text: qsTr("Account"); color: dialog.cFaint; font.pixelSize: 14 }
                                    Label {
                                        Layout.fillWidth: true
                                        text: dialog.stepReady(1) ? qsTr("ready") : qsTr("missing")
                                        color: dialog.cText
                                        font.pixelSize: 15
                                    }
                                    Label { text: dialog.stepReady(1) ? "✓" : "!"; color: dialog.stepReady(1) ? dialog.cGood : dialog.cWarn; font.bold: true }
                                    Label { text: qsTr("Pairing code"); color: dialog.cFaint; font.pixelSize: 14 }
                                    Label {
                                        Layout.fillWidth: true
                                        text: dialog.stepReady(2) ? qsTr("entered") : qsTr("missing")
                                        color: dialog.cText
                                        font.pixelSize: 15
                                    }
                                    Label { text: dialog.stepReady(2) ? "✓" : "!"; color: dialog.stepReady(2) ? dialog.cGood : dialog.cWarn; font.bold: true }
                                }
                            }

                            // what is happening, or how it went
                            RowLayout {
                                visible: dialog.regState !== ""
                                Layout.fillWidth: true
                                spacing: 12
                                BusyIndicator {
                                    visible: dialog.regState === "working"
                                    running: visible
                                    implicitWidth: 36
                                    implicitHeight: 36
                                }
                                Label {
                                    visible: dialog.regState !== "working"
                                    text: dialog.regState === "done" ? "✓" : "✕"
                                    color: dialog.regState === "done" ? dialog.cGood : dialog.cWarn
                                    font.pixelSize: 26
                                    font.bold: true
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: dialog.regState === "working" ? qsTr("Registering with the console…")
                                        : dialog.regState === "done" ? qsTr("The console is registered with this PC.")
                                        : qsTr("Registering did not work. Check the points on the right and try again.")
                                    color: dialog.regState === "failed" ? dialog.cWarn : dialog.cText
                                    font.pixelSize: 16
                                    wrapMode: Text.Wrap
                                }
                            }
                            // what the console's answer means, and the way out
                            Rectangle {
                                visible: dialog.regState === "failed" && dialog.regReason !== ""
                                Layout.fillWidth: true
                                Layout.maximumWidth: 560
                                implicitHeight: reasonColumn.implicitHeight + 32
                                radius: 12
                                color: "#2A1E16"
                                border.color: "#5C4426"
                                ColumnLayout {
                                    id: reasonColumn
                                    anchors.fill: parent
                                    anchors.margins: 16
                                    spacing: 8
                                    Label {
                                        Layout.fillWidth: true
                                        text: dialog.regReason === "account" ? qsTr("The console did not accept this account")
                                                                            : qsTr("The console did not accept the code")
                                        color: "#E8C58E"
                                        font.pixelSize: 16
                                        font.bold: true
                                        wrapMode: Text.Wrap
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: dialog.regReason === "account"
                                              ? qsTr("The code belongs to the PS5 profile that opened Link Device, and that is not the account you signed in with. On the PS5, switch to the profile of that account (your main profile), open Link Device there for a new code, and enter it.")
                                              : qsTr("The code was wrong or had run out. Open Link Device on the PS5 again for a new code, and enter it.")
                                        color: dialog.cMuted
                                        font.pixelSize: 14
                                        wrapMode: Text.Wrap
                                    }
                                    C.Button {
                                        text: qsTr("Enter a new code")
                                        font.pixelSize: 15
                                        onClicked: dialog.enterCodeAgain()
                                    }
                                }
                            }
                            // the technical details, for when something goes wrong
                            ScrollView {
                                visible: dialog.regLog.length > 0 && dialog.regState !== "done"
                                Layout.fillWidth: true
                                Layout.maximumWidth: 560
                                Layout.preferredHeight: 120
                                clip: true
                                TextArea {
                                    readOnly: true
                                    text: dialog.regLog
                                    color: dialog.cFaint
                                    font.pixelSize: 12
                                    font.family: "Consolas"
                                    wrapMode: TextEdit.Wrap
                                    background: Rectangle { color: dialog.cInset; radius: 8 }
                                }
                            }
                        }
                    }

                    // the buttons, under the form
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 10
                        spacing: 12
                        C.Button {
                            id: backButton
                            text: dialog.step === 0 ? qsTr("Cancel") : qsTr("Back")
                            flat: true
                            enabled: dialog.regState !== "working"
                            font.pixelSize: 15
                            onClicked: dialog.goBack()
                        }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                            visible: !dialog.stepReady(dialog.step) && dialog.step < 3
                            text: [qsTr("Enter the console's address to continue."), qsTr("Find your account in one of the two ways to continue."),
                                   qsTr("Enter the 8-digit code to continue."), ""][dialog.step]
                            color: dialog.cFaint
                            font.pixelSize: 13
                            wrapMode: Text.Wrap
                        }
                        Item { Layout.fillWidth: dialog.stepReady(dialog.step) || dialog.step === 3 }
                        PrimaryButton {
                            id: nextButton
                            lastInFocusChain: true
                            enabled: dialog.step < 3 ? dialog.stepReady(dialog.step)
                                                     : (dialog.regState !== "working" && dialog.stepReady(0) && dialog.stepReady(1) && dialog.stepReady(2))
                            text: dialog.step < 3 ? qsTr("Continue")
                                : dialog.regState === "done" ? qsTr("Done")
                                : dialog.regState === "failed" ? qsTr("Try again")
                                : qsTr("Register console")
                            onClicked: dialog.goNext()
                        }
                    }
                }
            }

            // ---------------- right: what this step is about
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: columns.width * 0.44 - columns.spacing
                radius: 16
                color: dialog.cSurface
                border.color: dialog.cBorder

                Flickable {
                    id: infoFlick
                    anchors.fill: parent
                    anchors.margins: 28
                    contentHeight: infoColumn.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: infoFlick.contentHeight > infoFlick.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }

                    ColumnLayout {
                        id: infoColumn
                        width: infoFlick.width - 12
                        spacing: 22

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Step %1 of %2").arg(dialog.step + 1).arg(4)
                            color: dialog.cFaint
                            font.pixelSize: 13
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: -16
                            text: [qsTr("Connecting to your console"), qsTr("Why the account?"), qsTr("What is this code?"),
                                   qsTr("What happens now?")][dialog.step]
                            color: dialog.cText
                            font.family: "Segoe UI"
                            font.weight: Font.Black
                            font.italic: true
                            font.pixelSize: 24
                            wrapMode: Text.Wrap
                        }

                        // 1 - Console
                        ColumnLayout {
                            visible: dialog.step === 0
                            Layout.fillWidth: true
                            spacing: 20
                            InfoBlock {
                                heading: qsTr("What it does")
                                body: qsTr("Robz Play brings the console's picture to this PC over Remote Play, the same connection Sony's own app uses. This step says which console that is.")
                            }
                            InfoBlock {
                                heading: qsTr("Why it is needed")
                                body: qsTr("The address was found on your home network, so there is usually nothing to type. The console type decides how Robz Play talks to it; PS5 is already chosen.")
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                InfoBlock { heading: qsTr("Before you continue"); body: "" }
                                // first the users, then Remote Play on each of them
                                Bullet { text: qsTr("The PS5 has <b>two users</b>: yours and a second, empty one. With a single user the console can send Remote Play back to its home screen. With two, it asks who plays once connected: choose the <b>second profile</b>, not your main one (with a controller connected to this PC, the main one works too). To add one:") }
                                MenuPath {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 16
                                    items: [qsTr("Profile icon"), qsTr("Switch User"), qsTr("Add User")]
                                }
                                Bullet { text: qsTr("If your profile has a <b>login passcode</b>, remove it: it can get in the way of connecting.") }
                                Bullet { text: qsTr("Remote Play is switched on, <b>on every profile</b>:") }
                                MenuPath {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 16
                                    items: [qsTr("Settings"), qsTr("System"), qsTr("Remote Play"), qsTr("Enable Remote Play")]
                                }
                                Bullet { text: qsTr("The console and this PC are on the same network (a cable to the router is best).") }
                            }
                        }

                        // 2 - Account
                        ColumnLayout {
                            visible: dialog.step === 1
                            Layout.fillWidth: true
                            spacing: 20
                            InfoBlock {
                                heading: qsTr("What it does")
                                body: qsTr("Remote Play lets a device in only on behalf of a PlayStation account that is on the console. Robz Play needs that account's ID: a code that names the account, not your password.")
                            }
                            InfoBlock {
                                heading: qsTr("Why it is needed")
                                body: qsTr("There are two ways to find it. <b>Signing in in the browser</b> always works: it happens on Sony's own page, and Robz Play never sees your password. <b>Finding by online ID</b> needs no sign-in, but asks a free outside service instead. The account ID is kept on this PC, so you do this only once.")
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                InfoBlock { heading: qsTr("Signing in in the browser"); body: "" }
                                Bullet { text: qsTr("After you sign in, the browser lands on an empty or error-like page whose address starts with <b>remoteplay.dl.playstation.net</b>. That is the page you need.") }
                                Bullet { text: qsTr("Its address holds a one-time code that is valid for a few minutes: bring it over right away. If it has run out, open the sign-in page again.") }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                InfoBlock { heading: qsTr("Finding by online ID"); body: "" }
                                Bullet { text: qsTr("It is not Sony's own service: sometimes it does not open or answer at first, and works after a few tries.") }
                                Bullet { text: qsTr("It finds you only if everything is set to <b>Anyone</b> in the console's privacy settings:") }
                                MenuPath {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 16
                                    items: [qsTr("Settings"), qsTr("Users and Accounts"), qsTr("Privacy")]
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                InfoBlock { heading: qsTr("Good to know"); body: "" }
                                Bullet { text: qsTr("Use the same account you play with on the console.") }
                                Bullet { text: qsTr("Already set up Robz Play or chiaki-ng before? The account is remembered and this step is done.") }
                            }
                        }

                        // 3 - Pairing code
                        ColumnLayout {
                            visible: dialog.step === 2
                            Layout.fillWidth: true
                            spacing: 20
                            InfoBlock {
                                heading: qsTr("What it does")
                                body: qsTr("The console shows a one-time 8-digit code. Typing it in here proves that this PC is yours; from then on the console trusts it.")
                            }
                            InfoBlock {
                                heading: qsTr("Why it is needed")
                                body: qsTr("Without pairing the console turns every connection away. You pair once; after that you only press Play.")
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                InfoBlock { heading: qsTr("Good to know"); body: "" }
                                Bullet { text: qsTr("The code belongs to the PS5 profile that opens it: open it on the profile of the account you signed in with in step 2 (your main profile).") }
                                Bullet { text: qsTr("The code is valid only for a short time. If it has run out, choose <b>Link Device</b> again for a new one.") }
                                Bullet { text: qsTr("Leave the code on the console's screen until registering is done.") }
                                Bullet { text: qsTr("The passcode is the 4-digit one you may use to sign in to your account on the console. With it, it is not asked at every connection.") }
                            }
                        }

                        // 4 - Register
                        ColumnLayout {
                            visible: dialog.step === 3
                            Layout.fillWidth: true
                            spacing: 20
                            InfoBlock {
                                heading: qsTr("What it does")
                                body: qsTr("Robz Play sends the code to the console, which answers with a key for this PC. The key is saved: from now on the console's card on the home screen has a <b>Play</b> button.")
                            }
                            ColumnLayout {
                                visible: dialog.regState !== "failed"
                                Layout.fillWidth: true
                                spacing: 10
                                InfoBlock { heading: qsTr("For later"); body: "" }
                                Bullet { text: qsTr("When you press <b>Play</b> and the PS5 asks who plays, choose the <b>second profile</b>. With a controller connected to this PC, your main profile works too.") }
                                Bullet { text: qsTr("To wake the console from rest mode with Robz Play, switch on both options in:") }
                                MenuPath {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 16
                                    items: [qsTr("Settings"), qsTr("System"), qsTr("Power Saving"), qsTr("Features Available in Rest Mode")]
                                }
                                Bullet { text: qsTr("Waking does not always work. If Play ends in a timeout, wake the PS5 with the PS button on its controller and press Play again. To keep it from going into rest mode at all, choose <b>Don't Put in Rest Mode</b> in:") }
                                MenuPath {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 16
                                    items: [qsTr("Settings"), qsTr("System"), qsTr("Power Saving"), qsTr("Set Time Until PS5 Enters Rest Mode")]
                                }
                            }
                            ColumnLayout {
                                visible: dialog.regState === "failed"
                                Layout.fillWidth: true
                                spacing: 10
                                InfoBlock { heading: qsTr("If it did not work"); body: "" }
                                Bullet { text: qsTr("The code may have run out: get a new one on the console and enter it again (Back).") }
                                Bullet { text: qsTr("The console must be on, not in rest mode, and on the same network as this PC.") }
                                Bullet { text: qsTr("The code must be opened on the PS5 profile of the account you signed in with, not on the empty second one.") }
                                Bullet { text: qsTr("Still stuck? Technical support on Discord is free: discord.gg/UyyEn7SHm4") }
                            }
                        }
                    }
                }
            }
        }

    }
}
