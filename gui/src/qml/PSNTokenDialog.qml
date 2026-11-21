import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

import "controls" as C

DialogView {
    id: dialog
    property var psnurl
    property var expired
    property bool closing: false
    title: {
        if(expired)
            qsTr("Credentials Expired: Refresh PSN Remote Connection")
        else
            qsTr("Setup Automatic PSN Remote Connection")
    }
    buttonVisible: false

    // Robz Play: without a browser inside the app (the usual case on Windows) the sign-in goes
    // through the web browser, step by step (PsnSignInGuide.qml), and the PSN connection is set up
    // here on the page: "" (not yet), "working", "error" or "done"
    property string tokenState: ""
    property string tokenError: ""
    property string tokenLog: ""
    property bool showLog: false

    function startToken(address) {
        tokenState = "working";
        tokenError = "";
        tokenLog = "";
        Chiaki.initPsnAuth(address, function(msg, ok, done) {
            tokenLog += msg + "\n";
            if (!done)
                return;
            if (ok) {
                Chiaki.settings.remotePlayAsk = false;
                tokenState = "done";
            } else {
                tokenState = "error";
                tokenError = qsTr("Sony did not accept it: %1. The code is valid for a few minutes only: open the sign-in page again and bring the new address.")
                    .arg(String(msg).replace(/^\[[A-Z]\]\s*/, ""));
            }
        });
    }
    StackView.onActivated: {
        Chiaki.settings.remotePlayAsk = true;
        nativeTokenForm.visible = true;
        nativeTokenForm.forceActiveFocus(Qt.TabFocusReason);
        if(Qt.platform.os == "linux" || Qt.platform.os == "osx")
            extBrowserButton.clicked();
    }
    function close() {
        if(webView.web)
        {
            dialog.closing = true;
            if(Chiaki.settings.remotePlayAsk)
                reloadTimer.start();
            else
                cacheClearTimer.start();
        }
        else
            root.closeDialog();
    }

    Item {
        Item {
            id: nativeTokenForm
            visible: false
            Keys.onPressed: (event) => {
                if (event.modifiers)
                    return;
                switch (event.key) {
                case Qt.Key_PageUp:
                    reloadButton.clicked();
                    event.accepted = true;
                    break;
                case Qt.Key_PageDown:
                    extBrowserButton.clicked();
                    event.accepted = true;
                    break;
                }
            }
            anchors.fill: parent
            ToolBar {
                id: psnTokenToolbar
                anchors {
                    top: parent.top
                    left: parent.left
                    right: parent.right
                }
                height: 80

                RowLayout {
                    anchors {
                        fill: parent
                        leftMargin: 10
                        rightMargin: 10
                    }
                    Button {
                        id: reloadButton
                        Layout.fillHeight: true
                        Layout.preferredWidth: 350
                        flat: true
                        text: "reload + clear cookies"
                        Image {
                            anchors {
                                left: parent.left
                                verticalCenter: parent.verticalCenter
                                leftMargin: 12
                            }
                            width: 28
                            height: 28
                            sourceSize: Qt.size(width, height)
                            source: "qrc:/icons/l1.svg"
                        }
                        onClicked: reloadTimer.start()
                        Material.roundedScale: Material.SmallScale
                        focusPolicy: Qt.NoFocus
                    }
                    ProgressBar {
                        id: browserProgresss
                        Layout.fillHeight: true
                        Layout.preferredWidth: 300
                        from: 0
                        to: 100
                        value: webView.web ? webView.web.loadProgress : 0
                        Material.roundedScale: Material.SmallScale
                        focusPolicy: Qt.NoFocus
                    }
                    Button {
                        id: extBrowserButton
                        Layout.fillHeight: true
                        Layout.preferredWidth: 300
                        flat: true
                        Image {
                            anchors {
                                left: parent.left
                                verticalCenter: parent.verticalCenter
                                leftMargin: 12
                            }
                            width: 28
                            height: 28
                            sourceSize: Qt.size(width, height)
                            source: "qrc:/icons/r1.svg"
                        }
                        focusPolicy: Qt.NoFocus
                        text: qsTr("Use external browser")
                        onClicked: {
                            nativeTokenForm.visible = false;
                            psnTokenToolbar.visible = false;
                            nativeErrorGrid.visible = false;
                            webView.visible = false;
                            guideView.visible = true;
                        }
                    }
                }
            }
            Timer {
                id: reloadTimer
                interval: 0
                running: false
                onTriggered: {
                    Chiaki.clearCookies(webView.web.profile);
                    webView.web.profile.clearHttpCache();
                }
            }

            Timer {
                id: cacheClearTimer
                interval: 0
                running: false
                onTriggered: {
                    webView.web.profile.clearHttpCache();
                }
            }

            GridLayout {
                id: nativeErrorGrid
                visible: false
                anchors {
                    top: psnTokenToolbar.bottom
                    left: parent.left
                    right: parent.right
                    topMargin: 50
                }
                columns: 2
                rowSpacing: 10
                columnSpacing: 20
                Label {
                    id: nativeErrorHeader
                    text: "Retrieving PSN account ID failed with error: "
                    Layout.fillHeight: true
                    Layout.preferredWidth: 400
                    Layout.leftMargin: 20
                }

                Label {
                    id: nativeErrorLabel
                    Layout.fillHeight: true
                    Layout.preferredWidth: 400
                    Layout.leftMargin: 20
                }

                Label {
                    id: retryButton
                    text: qsTr("Retry process")
                    Layout.fillHeight: true
                    Layout.preferredWidth: 300
                    Layout.leftMargin: 20
                }
                C.Button {
                    firstInFocusChain: true
                    lastInFocusChain: true
                    text: qsTr("Retry")
                    onClicked: {
                        nativeErrorGrid.visible = false;
                        nativeErrorLabel.text = "";
                        nativeErrorLabel.visible = false;
                        webView.visible = true;
                        webView.web.url = Chiaki.psnLoginUrl();
                    }
                    Layout.preferredWidth: 200
                    Layout.fillHeight: true
                    Layout.leftMargin: 20
                }
            }
            Item {
                id: webView
                property Item web: null
                anchors {
                    top: psnTokenToolbar.bottom
                    bottom: parent.bottom
                    left: parent.left
                    right: parent.right
                    leftMargin: 10
                    rightMargin: 10
                }
                property bool started: false
                Component.onCompleted: {
                    try {
                        web = Qt.createQmlObject("
                        import QtWebEngine
                        import QtQuick.Controls
                        import org.streetpea.chiaking
                        WebEngineView {
                            profile {
                                offTheRecord: false
                                storageName: 'psn-token'
                                onClearHttpCacheCompleted: {
                                    if(dialog.closing)
                                        root.closeDialog();
                                    else
                                        webView.web.reload();
                                }
                            }
                            settings {
                                // Load larger touch icons
                                touchIconsEnabled: true
                                localContentCanAccessRemoteUrls: true
                            }

                            onContextMenuRequested: (request) => request.accepted = true;
                            onNavigationRequested: (request) => {
                                if (Chiaki.checkPsnRedirectURL(request.url)) {
                                    logDialog.open()
                                    Chiaki.initPsnAuth(request.url, function(msg, ok, done) {
                                        if (!done)
                                            logArea.text += msg + '\n';
                                        else
                                        {
                                            logArea.text += msg + '\n';
                                            logDialog.standardButtons = Dialog.Close;
                                        }
                                    });
                                    request.reject();
                                }
                                else
                                    request.accept();
                            }
                            onCertificateError: console.error(error.description);
                        }", webView, "webView");
                        Chiaki.setWebEngineHints(web.profile);
                        web.url = Chiaki.psnLoginUrl();
                        web.anchors.fill = webView;
                    } catch (error) {
                        // Robz Play: no browser inside the app (the Windows build has none) is the
                        // usual case, not an error: the sign-in goes through the web browser
                        console.info('No browser inside the app; signing in through the web browser (' + error + ')');
                        extBrowserButton.clicked();
                    }
                }
            }
        }
        // the browser and the address, A to Z
        Item {
            id: guideView
            visible: false
            anchors.fill: parent

            Flickable {
                id: guideFlick
                anchors.fill: parent
                contentHeight: guideColumn.implicitHeight + 72
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: guideFlick.contentHeight > guideFlick.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }

                ColumnLayout {
                    id: guideColumn
                    x: Math.max(32, (guideFlick.width - width) / 2)
                    y: 36
                    width: Math.min(820, guideFlick.width - 64)
                    spacing: 16

                    Label {
                        Layout.fillWidth: true
                        text: dialog.expired ? qsTr("Your PSN sign-in has run out") : qsTr("Sign in to PSN once")
                        color: "#FBF4EC"
                        font.family: "Segoe UI"
                        font.weight: Font.Black
                        font.italic: true
                        font.pixelSize: 30
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        text: dialog.expired
                              ? qsTr("Sign in again to keep playing away from home. It goes the same way as the first time.")
                              : qsTr("This lets you play away from home and finds your consoles by itself. You sign in on Sony's own page; Robz Play never sees your password.")
                        color: "#CDBFB4"
                        font.pixelSize: 15
                        wrapMode: Text.Wrap
                    }

                    PsnSignInGuide {
                        id: tokenGuide
                        Layout.fillWidth: true
                        Layout.topMargin: 10
                        firstInFocusChain: true
                        listening: guideView.visible
                        statusKind: dialog.tokenState
                        statusText: dialog.tokenState === "working" ? qsTr("Setting up the PSN connection…")
                                  : dialog.tokenState === "done" ? qsTr("The PSN connection is ready.")
                                  : dialog.tokenError
                        onAddressReady: (address) => dialog.startToken(address)
                        onProblem: (message) => {
                            dialog.tokenState = "error";
                            dialog.tokenError = message;
                        }
                    }

                    RobzPrimaryButton {
                        visible: dialog.tokenState === "done"
                        Layout.leftMargin: 40
                        text: qsTr("Done")
                        onClicked: dialog.close()
                    }

                    // what happened, for support
                    C.Button {
                        visible: dialog.tokenLog.length > 0
                        Layout.leftMargin: 32
                        flat: true
                        font.pixelSize: 14
                        text: dialog.showLog ? qsTr("Hide the details") : qsTr("Show the details")
                        onClicked: dialog.showLog = !dialog.showLog
                    }
                    ScrollView {
                        visible: dialog.showLog && dialog.tokenLog.length > 0
                        Layout.fillWidth: true
                        Layout.leftMargin: 40
                        Layout.preferredHeight: 130
                        clip: true
                        TextArea {
                            readOnly: true
                            text: dialog.tokenLog
                            color: "#A08F84"
                            font.pixelSize: 12
                            font.family: "Consolas"
                            wrapMode: TextEdit.Wrap
                            background: Rectangle { color: "#171312"; radius: 8 }
                        }
                    }
                }
            }
        }

        Item {
            Dialog {
                id: logDialog
                parent: Overlay.overlay
                x: Math.round((root.width - width) / 2)
                y: Math.round((root.height - height) / 2)
                title: qsTr("Create PSN Automatic Remote Connection Token")
                modal: true
                closePolicy: Popup.NoAutoClose
                standardButtons: Dialog.Cancel
                Material.roundedScale: Material.MediumScale
                onOpened: logArea.forceActiveFocus(Qt.TabFocusReason)
                onClosed: root.showMainView();

                Flickable {
                    id: logFlick
                    implicitWidth: 600
                    implicitHeight: 400
                    clip: true
                    contentWidth: logArea.contentWidth
                    contentHeight: logArea.contentHeight
                    flickableDirection: Flickable.AutoFlickIfNeeded
                    ScrollBar.vertical: ScrollBar {
                        id: logScrollbar
                        policy: ScrollBar.AlwaysOn
                        visible: logFlick.contentHeight > logFlick.implicitHeight
                    }

                    Label {
                        id: logArea
                        width: logFlick.width
                        wrapMode: TextEdit.Wrap
                        Keys.onReturnPressed: if (logDialog.standardButtons == Dialog.Close) logDialog.close()
                        Keys.onEscapePressed: logDialog.close()
                        Keys.onPressed: (event) => {
                            switch (event.key) {
                            case Qt.Key_Up:
                                if(logScrollbar.position > 0.001)
                                    logFlick.flick(0, 500);
                                event.accepted = true;
                                break;
                            case Qt.Key_Down:
                                if(logScrollbar.position < 1.0 - logScrollbar.size - 0.001)
                                    logFlick.flick(0, -500);
                                event.accepted = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
}