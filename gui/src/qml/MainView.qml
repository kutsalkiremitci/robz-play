import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

Pane {
    padding: 0
    id: consolePane
    StackView.onActivated: {
        forceActiveFocus(Qt.TabFocusReason);
        if(!Chiaki.autoConnect && !root.initialAsk && !Chiaki.window.directStream)
        {
            root.initialAsk = true;
            if(Chiaki.settings.addSteamShortcutAsk && (typeof Chiaki.createSteamShortcut === "function"))
                root.showRemindDialog(qsTr("Official Steam artwork + controller layout"), qsTr("Would you like to either create a new non-Steam game for Robz Play\nor update an existing non-Steam game with the official artwork and controller layout?") + "\n\n" + qsTr("(Note: If you select no now and want to do this later, click the button or press R3 from the main menu.)"), false, () => root.showSteamShortcutDialog(true));
            else if(Chiaki.settings.remotePlayAsk)
            {
                if(!Chiaki.settings.psnRefreshToken || !Chiaki.settings.psnAuthToken || !Chiaki.settings.psnAuthTokenExpiry || !Chiaki.settings.psnAccountId)
                    root.showRemindDialog(qsTr("Remote Play via PSN"), qsTr("Would you like to connect to PSN?\nThis enables:\n- Automatic registration\n- Playing outside of your home network without port forwarding?") + "\n\n" + qsTr("(Note: If you select no now and want to do this later, go to the Remote play section of the settings.)"), true, () => root.showPSNTokenDialog(false));
                else
                    Chiaki.settings.remotePlayAsk = false;
            }
        }
    }
    Keys.onUpPressed: {
        if(hostsView.currentItem && hostsView.currentItem.visible)
        {
            hostsView.decrementCurrentIndex()
            while(!hostsView.currentItem.visible)
                hostsView.decrementCurrentIndex()
        }
    }
    Keys.onDownPressed: {
        if(hostsView.currentItem && hostsView.currentItem.visible)
        {
            hostsView.incrementCurrentIndex()
            while(!hostsView.currentItem.visible)
                 hostsView.incrementCurrentIndex()
        }
    }
    Keys.onMenuPressed: settingsButton.clicked()
    Keys.onReturnPressed: if (hostsView.currentItem) hostsView.currentItem.connectToHost()
    Keys.onYesPressed: if (hostsView.currentItem) hostsView.currentItem.wakeUpHost()
    Keys.onNoPressed: if (hostsView.currentItem) hostsView.currentItem.deleteHost()
    Keys.onEscapePressed: root.showConfirmDialog(qsTr("Quit"), qsTr("Are you sure you want to quit?"), () => Qt.quit())
    Keys.onPressed: (event) => {
        if (event.modifiers)
            return;
        switch (event.key) {
        case Qt.Key_PageUp:
            if (hostsView.currentItem) hostsView.currentItem.setConsolePin();
            event.accepted = true;
            break;
        case Qt.Key_PageDown:
            if (Chiaki.settings.psnAuthToken) Chiaki.refreshPsnToken();
            event.accepted = true;
            break;
        case Qt.Key_F1:
            if (typeof Chiaki.createSteamShortcut === "function") root.showSteamShortcutDialog(false);
            event.accepted = true;
            break;
        case Qt.Key_F2:
            root.showManualHostDialog();
            event.accepted = true;
            break;
        }
    }

    // ------------------------------------------------------------------ look
    // Robz Play home screen: brand and navigation on the left, consoles in the middle, and
    // the frame generation panel - with a live picture of what it does - on the right.
    // Warm dark palette.
    readonly property color cBg: "#1F1E1D"
    readonly property color cSidebar: "#181716"
    readonly property color cSurface: "#262624"
    readonly property color cSurfaceHover: "#2E2D2B"
    readonly property color cInset: "#1A1918"
    readonly property color cBorder: "#3A3936"
    readonly property color cText: "#FAF9F5"
    readonly property color cMuted: "#C2C0B6"
    readonly property color cFaint: "#8F8D85"
    readonly property color cAccent: "#D97757"
    readonly property color cAccentHover: "#E38A6C"
    readonly property color cOnAccent: "#1F1E1D"
    readonly property color cGood: "#8DB580"
    readonly property color cWarn: "#D9A65A"
    readonly property string serif: "Georgia"

    // room for the wide layout; below it the sidebar and the panel slim down
    readonly property bool roomy: width >= 1320

    readonly property bool previewEmpty: Chiaki.uiPreview === "home-empty"

    readonly property int visibleHosts: {
        if (previewEmpty)
            return 0;
        let n = 0;
        const hosts = Chiaki.hosts;
        for (let i = 0; i < hosts.length; ++i) {
            if (hosts[i].display)
                n++;
        }
        return n;
    }

    // consoles that still have to be linked with the code from the console
    readonly property int hostsToSetUp: {
        let n = 0;
        const hosts = Chiaki.hosts;
        for (let i = 0; i < hosts.length; ++i) {
            if (hosts[i].display && !hosts[i].registered && !hosts[i].duid)
                n++;
        }
        return n;
    }

    // The frame rate the picture is shown at with the current switches, for a 30 FPS game.
    readonly property int shownFps: Chiaki.settings.frameGenEnabled ? Chiaki.settings.frameGenTargetFps : 30

    // Drives the two moving markers of the frame generation panel. It only runs while this
    // screen is the one on show and the app is in front.
    property real cadencePhase: 0
    NumberAnimation on cadencePhase {
        from: 0
        to: 1
        duration: 1800
        loops: Animation.Infinite
        running: consolePane.StackView.status === StackView.Active && Qt.application.state === Qt.ApplicationActive
    }

    background: Rectangle { color: consolePane.cBg }

    // A row in the sidebar.
    component NavButton: AbstractButton {
        id: nav
        property string hint: ""
        Layout.fillWidth: true
        implicitHeight: 46
        focusPolicy: Qt.NoFocus
        hoverEnabled: true
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        background: Rectangle {
            radius: 10
            color: nav.down ? "#2E2D2B" : (nav.hovered ? "#262624" : "transparent")
        }
        contentItem: RowLayout {
            spacing: 10
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                text: nav.text
                color: "#FAF9F5"
                font.pixelSize: 17
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
            Label {
                Layout.rightMargin: 14
                visible: nav.hint.length > 0
                text: nav.hint
                color: "#8F8D85"
                font.pixelSize: 13
                verticalAlignment: Text.AlignVCenter
            }
        }
    }

    // An on/off switch. `on` is bound to a setting; clicking asks the owner to change it.
    component Toggle: AbstractButton {
        id: toggle
        property bool on: false
        implicitWidth: 46
        implicitHeight: 26
        focusPolicy: Qt.NoFocus
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        background: Rectangle {
            radius: height / 2
            color: toggle.on ? "#D97757" : "#4A4946"
            Behavior on color { ColorAnimation { duration: 120 } }
            Rectangle {
                width: 20
                height: 20
                radius: 10
                y: 3
                x: toggle.on ? parent.width - width - 3 : 3
                color: "#FAF9F5"
                Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
            }
        }
    }

    // One choice of a segmented control. `active` is bound to a setting. `badge` is a small
    // note next to the text, such as "Experimental".
    component Segment: AbstractButton {
        id: segment
        property bool active: false
        property string badge: ""
        Layout.fillWidth: true
        implicitHeight: 34
        implicitWidth: Math.max(64, segmentContent.implicitWidth + 22)
        focusPolicy: Qt.NoFocus
        hoverEnabled: true
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        background: Rectangle {
            radius: 8
            color: segment.active ? "#D97757" : (segment.hovered ? "#2E2D2B" : "transparent")
        }
        contentItem: Item {
            implicitWidth: segmentContent.implicitWidth
            implicitHeight: segmentContent.implicitHeight
            Row {
                id: segmentContent
                anchors.centerIn: parent
                spacing: 7
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: segment.text
                    color: segment.active ? "#1F1E1D" : "#C2C0B6"
                    font.pixelSize: 15
                    font.weight: segment.active ? Font.DemiBold : Font.Normal
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: segment.badge.length > 0
                    width: segmentBadge.implicitWidth + 12
                    height: 18
                    radius: 9
                    color: segment.active ? "#2E1F1E1D" : "transparent"
                    border.width: segment.active ? 0 : 1
                    border.color: "#D9A65A"
                    Label {
                        id: segmentBadge
                        anchors.centerIn: parent
                        text: segment.badge
                        color: segment.active ? "#1F1E1D" : "#D9A65A"
                        font.pixelSize: 11
                    }
                }
            }
        }
    }

    // The frame around a row of segments; the segments share its width evenly.
    component SegmentGroup: Rectangle {
        default property alias segments: segmentRow.data
        Layout.fillWidth: true
        implicitWidth: segmentRow.implicitWidth + 6
        implicitHeight: 40
        radius: 10
        color: "#1A1918"
        border.color: "#3A3936"
        RowLayout {
            id: segmentRow
            anchors {
                fill: parent
                margins: 3
            }
            spacing: 2
            uniformCellSizes: true
        }
    }

    // A language choice, shown as its flag.
    component FlagButton: AbstractButton {
        id: flag
        property string code: ""
        property bool active: false
        implicitWidth: 46
        implicitHeight: 32
        focusPolicy: Qt.NoFocus
        hoverEnabled: true
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        background: Rectangle {
            radius: 8
            color: flag.active ? "#2E2D2B" : (flag.hovered ? "#262624" : "transparent")
            border.width: flag.active ? 2 : 1
            border.color: flag.active ? "#D97757" : "#3A3936"
        }
        contentItem: Item {
            Image {
                anchors.centerIn: parent
                width: 27
                height: 18
                sourceSize: Qt.size(54, 36)
                source: "qrc:/icons/flag-" + flag.code + ".svg"
                opacity: flag.active ? 1.0 : 0.55
            }
        }
    }

    // A quiet text button used for the secondary actions on a console card.
    component CardAction: AbstractButton {
        id: cardAction
        property string glyph: ""
        property bool showGlyph: false
        implicitHeight: 34
        implicitWidth: cardActionRow.implicitWidth + 24
        focusPolicy: Qt.NoFocus
        hoverEnabled: true
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        background: Rectangle {
            radius: 8
            color: cardAction.hovered ? "#1A1918" : "transparent"
            border.color: "#3A3936"
            border.width: 1
        }
        contentItem: Item {
            implicitWidth: cardActionRow.implicitWidth
            Row {
                id: cardActionRow
                anchors.centerIn: parent
                spacing: 8
                Image {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: cardAction.showGlyph && cardAction.glyph.length > 0
                    width: 20
                    height: 20
                    sourceSize: Qt.size(width, height)
                    source: visible ? cardAction.glyph : ""
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: cardAction.text
                    color: "#C2C0B6"
                    font.pixelSize: 14
                }
            }
        }
    }

    // A track with a marker that crosses it. The marker only moves `fps` times a second, so
    // two lanes side by side show how much smoother a higher frame rate looks.
    component CadenceLane: Item {
        id: lane
        property real phase: 0
        property int fps: 30
        property color tint: "#8F8D85"
        readonly property real steps: Math.max(1, 1.8 * fps)
        readonly property real shown: Math.floor(phase * steps) / steps
        Layout.fillWidth: true
        implicitHeight: 16
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 2
            radius: 1
            color: "#3A3936"
        }
        Rectangle {
            width: 16
            height: 16
            radius: 5
            color: lane.tint
            x: Math.round(lane.shown * (lane.width - width))
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // -------------------------------------------------------------- sidebar
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: consolePane.roomy ? 300 : 256
            color: consolePane.cSidebar

            Rectangle {
                anchors.right: parent.right
                width: 1
                height: parent.height
                color: consolePane.cBorder
            }

            ColumnLayout {
                anchors {
                    fill: parent
                    margins: 18
                }
                spacing: 4

                RowLayout {
                    Layout.bottomMargin: 22
                    Layout.topMargin: 6
                    Layout.leftMargin: 6
                    spacing: 12

                    Image {
                        source: "qrc:/icons/robzplay.svg"
                        sourceSize: Qt.size(46, 46)
                    }

                    ColumnLayout {
                        spacing: 0
                        Label {
                            text: "Robz Play"
                            font.family: consolePane.serif
                            font.pixelSize: 27
                            color: consolePane.cText
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Remote Play with frame generation")
                            font.pixelSize: 12
                            color: consolePane.cFaint
                            elide: Text.ElideRight
                        }
                    }
                }

                NavButton {
                    text: qsTr("Add a console")
                    hint: "F2"
                    onClicked: root.showManualHostDialog()
                }

                NavButton {
                    text: qsTr("Refresh PSN hosts")
                    visible: Chiaki.settings.psnAuthToken ? true : false
                    onClicked: Chiaki.refreshPsnToken()
                }

                NavButton {
                    text: qsTr("Steam shortcut")
                    hint: "F1"
                    visible: typeof Chiaki.createSteamShortcut === "function"
                    onClicked: root.showSteamShortcutDialog(false)
                }

                NavButton {
                    id: settingsButton
                    text: qsTr("Settings")
                    onClicked: root.showSettingsDialog()
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 12
                    Layout.bottomMargin: 12
                    implicitHeight: 1
                    color: consolePane.cBorder
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 14
                    Layout.rightMargin: 10
                    spacing: 10
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Find consoles")
                            color: consolePane.cText
                            font.pixelSize: 16
                        }
                        Label {
                            text: Chiaki.discoveryEnabled ? qsTr("Searching your network") : qsTr("Search is off")
                            color: consolePane.cFaint
                            font.pixelSize: 12
                        }
                    }
                    Toggle {
                        on: Chiaki.discoveryEnabled
                        onClicked: Chiaki.discoveryEnabled = !Chiaki.discoveryEnabled
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 14
                    Layout.leftMargin: 14
                    Layout.rightMargin: 10
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Language")
                        color: consolePane.cText
                        font.pixelSize: 16
                    }
                    FlagButton {
                        code: "tr"
                        active: Chiaki.settings.language === "tr"
                        onClicked: Chiaki.settings.language = "tr"
                    }
                    FlagButton {
                        code: "en"
                        active: Chiaki.settings.language === "en"
                        onClicked: Chiaki.settings.language = "en"
                    }
                }

                Item { Layout.fillHeight: true }

                AbstractButton {
                    id: kickBadge
                    Layout.fillWidth: true
                    implicitHeight: 58
                    focusPolicy: Qt.NoFocus
                    hoverEnabled: true
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    onClicked: Qt.openUrlExternally("https://kick.com/robzsogood")
                    background: Rectangle {
                        radius: 12
                        color: kickBadge.hovered ? consolePane.cSurfaceHover : consolePane.cSurface
                        border.width: 1
                        border.color: kickBadge.hovered ? "#53FC18" : consolePane.cBorder
                    }
                    contentItem: RowLayout {
                        spacing: 12
                        Image {
                            Layout.leftMargin: 14
                            source: "qrc:/icons/kick.svg"
                            sourceSize: Qt.size(26, 26)
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                Layout.fillWidth: true
                                text: "kick.com/robzsogood"
                                color: consolePane.cText
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Label {
                                text: qsTr("Watch live on Kick")
                                color: consolePane.cFaint
                                font.pixelSize: 12
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.topMargin: 10
                    Layout.leftMargin: 6
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Version %1").arg(Qt.application.version)
                        color: consolePane.cFaint
                        font.pixelSize: 12
                    }
                    CardAction {
                        text: qsTr("Quit")
                        onClicked: Qt.quit()
                    }
                }
            }
        }

        // -------------------------------------------------------------- content
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            RowLayout {
                id: contentRow
                anchors {
                    fill: parent
                    leftMargin: consolePane.roomy ? 36 : 24
                    rightMargin: consolePane.roomy ? 36 : 24
                    topMargin: 30
                    bottomMargin: 26
                }
                spacing: consolePane.roomy ? 28 : 20

                // ------------------------------------------------------ consoles
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 18

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Label {
                            text: qsTr("Consoles")
                            font.family: consolePane.serif
                            font.pixelSize: 36
                            color: consolePane.cText
                        }
                        Label {
                            Layout.fillWidth: true
                            text: {
                                if (consolePane.visibleHosts === 0)
                                    return Chiaki.discoveryEnabled ? qsTr("Looking for PlayStation consoles on your network…") : qsTr("Console search is switched off.");
                                if (consolePane.hostsToSetUp > 0)
                                    return qsTr("Choose Set up, then type the code the console shows under Settings › System › Remote Play › Link Device.");
                                return qsTr("Choose a console to start playing.");
                            }
                            color: consolePane.cMuted
                            font.pixelSize: 16
                            wrapMode: Text.Wrap
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        ListView {
                            id: hostsView
                            keyNavigationWraps: true
                            anchors.fill: parent
                            clip: true
                            visible: !consolePane.previewEmpty
                            model: Chiaki.hosts
                            onCountChanged: {
                                if(!hostsView.currentItem)
                                    hostsView.incrementCurrentIndex();
                                if(!hostsView.currentItem)
                                    return;
                                if(!hostsView.currentItem.visible)
                                {
                                    for(var i = 0; i < hostsView.count; i++)
                                    {
                                        hostsView.incrementCurrentIndex()
                                        if(hostsView.currentItem.visible)
                                        {
                                            break;
                                        }
                                    }
                                }
                            }
                            delegate: ItemDelegate {
                                id: delegate
                                visible: modelData.display
                                width: parent ? parent.width : 0
                                height: modelData.display ? 178 : 0
                                padding: 0
                                highlighted: ListView.isCurrentItem
                                onClicked: connectToHost()
                                HoverHandler { cursorShape: Qt.PointingHandCursor }

                                function connectToHost() {
                                    if(modelData.discovered)
                                        Chiaki.connectToHost(index, modelData.name);
                                    else
                                        Chiaki.connectToHost(index);
                                }

                                function wakeUpHost() {
                                    if(!modelData.discovered && !modelData.duid)
                                        Chiaki.wakeUpHost(index);
                                }

                                function deleteHost() {
                                    if (modelData.manual)
                                        root.showConfirmDialog(qsTr("Delete Console"), qsTr("Are you sure you want to delete this console?"), () => {Chiaki.deleteHost(index)});

                                    else if (modelData.discovered && !modelData.registered)
                                        root.showConfirmDialog(qsTr("Hide Console"), qsTr("Are you sure you want to hide this console?") + "\n\n" + qsTr("Note: You can unhide from the Consoles section of the Settings under Hidden Consoles"), () => Chiaki.hideHost(modelData.mac, modelData.name));

                                }

                                function setConsolePin() {
                                    root.showConsolePinDialog(index);
                                }

                                readonly property bool standby: modelData.state == "standby"
                                readonly property bool usable: (modelData.registered || modelData.duid) ? true : false
                                readonly property color stateColor: {
                                    if (!usable)
                                        return consolePane.cAccent;
                                    if (modelData.state == "ready")
                                        return consolePane.cGood;
                                    return standby ? consolePane.cWarn : consolePane.cFaint;
                                }

                                // where the console stands, in plain words
                                readonly property string statusText: {
                                    if (modelData.duid)
                                        return modelData.discovered ? qsTr("Automatic registration available") : qsTr("Remote connection via PSN");
                                    if (modelData.state == "ready")
                                        return modelData.registered ? qsTr("On and ready to play") : qsTr("On, not set up yet");
                                    if (standby)
                                        return modelData.registered ? qsTr("In rest mode. Play wakes it up.") : qsTr("In rest mode, not set up yet");
                                    return modelData.registered ? qsTr("Not seen on your network right now") : qsTr("Not set up yet");
                                }

                                // the facts about it, for those who need them
                                readonly property string detailText: {
                                    let parts = [];
                                    if (modelData.address)
                                        parts.push(qsTr("Address %1").arg(Chiaki.settings.streamerMode ? qsTr("hidden") : modelData.address));
                                    if (modelData.mac)
                                        parts.push(qsTr("ID %1").arg(Chiaki.settings.streamerMode ? qsTr("hidden") : modelData.mac));
                                    if (!modelData.duid) {
                                        if (modelData.discovered)
                                            parts.push(modelData.manual ? qsTr("Found on your network, also added by address") : qsTr("Found on your network"));
                                        else
                                            parts.push(qsTr("Added by address"));
                                        if (modelData.discovered) {
                                            if (modelData.app)
                                                parts.push(qsTr("Running %1").arg(modelData.app));
                                            if (modelData.titleId)
                                                parts.push(qsTr("Title ID %1").arg(modelData.titleId));
                                        }
                                    }
                                    return parts.join("     ");
                                }

                                background: Item {
                                    Rectangle {
                                        anchors {
                                            fill: parent
                                            bottomMargin: 14
                                        }
                                        radius: 16
                                        color: delegate.hovered ? consolePane.cSurfaceHover : consolePane.cSurface
                                        border.width: delegate.highlighted ? 2 : 1
                                        border.color: delegate.highlighted ? consolePane.cAccent : consolePane.cBorder
                                    }
                                }

                                contentItem: Item {
                                    RowLayout {
                                        anchors {
                                            fill: parent
                                            leftMargin: 22
                                            rightMargin: 22
                                            topMargin: 14
                                            bottomMargin: 28
                                        }
                                        spacing: 22

                                        Image {
                                            Layout.fillHeight: true
                                            Layout.preferredWidth: consolePane.roomy ? 112 : 84
                                            fillMode: Image.PreserveAspectFit
                                            source: "image://svg/console-ps" + (modelData.ps5 ? "5" : "4") + (delegate.standby ? "#light_standby" : "#light_on")
                                            sourceSize: Qt.size(width, height)
                                        }

                                        ColumnLayout {
                                            Layout.alignment: Qt.AlignVCenter
                                            Layout.fillWidth: true
                                            spacing: 5

                                            Label {
                                                Layout.fillWidth: true
                                                text: modelData.name ? modelData.name : (modelData.ps5 ? "PlayStation 5" : "PlayStation 4")
                                                color: consolePane.cText
                                                font.pixelSize: 23
                                                font.weight: Font.DemiBold
                                                elide: Text.ElideRight
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 9
                                                Rectangle {
                                                    Layout.alignment: Qt.AlignVCenter
                                                    implicitWidth: 9
                                                    implicitHeight: 9
                                                    radius: 4.5
                                                    color: delegate.stateColor
                                                }
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: delegate.statusText
                                                    color: consolePane.cMuted
                                                    font.pixelSize: 15
                                                    elide: Text.ElideRight
                                                }
                                            }

                                            Label {
                                                Layout.fillWidth: true
                                                text: delegate.detailText
                                                color: consolePane.cFaint
                                                font.pixelSize: 13
                                                elide: Text.ElideRight
                                            }

                                            RowLayout {
                                                Layout.topMargin: 5
                                                spacing: 8

                                                CardAction {
                                                    text: qsTr("Wake Up")
                                                    visible: modelData.registered && !modelData.duid && !modelData.discovered
                                                    glyph: root.controllerButton("pyramid")
                                                    showGlyph: delegate.highlighted
                                                    onClicked: delegate.wakeUpHost()
                                                }

                                                CardAction {
                                                    text: qsTr("Console PIN")
                                                    visible: modelData.registered
                                                    glyph: "qrc:/icons/l1.svg"
                                                    showGlyph: delegate.highlighted
                                                    onClicked: delegate.setConsolePin()
                                                }

                                                CardAction {
                                                    text: modelData.manual ? qsTr("Delete") : qsTr("Hide")
                                                    visible: modelData.manual || (modelData.discovered && !modelData.registered)
                                                    glyph: root.controllerButton("box")
                                                    showGlyph: delegate.highlighted
                                                    onClicked: delegate.deleteHost()
                                                }
                                            }
                                        }

                                        AbstractButton {
                                            id: playButton
                                            Layout.alignment: Qt.AlignVCenter
                                            implicitHeight: 50
                                            implicitWidth: Math.max(128, playLabel.implicitWidth + 52)
                                            focusPolicy: Qt.NoFocus
                                            hoverEnabled: true
                                            onClicked: delegate.connectToHost()
                                            background: Rectangle {
                                                radius: 12
                                                color: playButton.hovered ? consolePane.cAccentHover : consolePane.cAccent
                                            }
                                            contentItem: Label {
                                                id: playLabel
                                                text: delegate.usable ? qsTr("Play") : qsTr("Set up")
                                                color: consolePane.cOnAccent
                                                font.pixelSize: 17
                                                font.weight: Font.DemiBold
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // nothing to show yet: the three steps that get a console on this screen
                        ColumnLayout {
                            anchors {
                                left: parent.left
                                right: parent.right
                                top: parent.top
                                topMargin: 6
                            }
                            visible: consolePane.visibleHosts === 0
                            spacing: 16

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: stepsColumn.implicitHeight + 52
                                radius: 16
                                color: consolePane.cSurface
                                border.color: consolePane.cBorder

                                ColumnLayout {
                                    id: stepsColumn
                                    anchors {
                                        left: parent.left
                                        right: parent.right
                                        top: parent.top
                                        margins: 26
                                    }
                                    spacing: 16

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Connect your PlayStation")
                                        font.family: consolePane.serif
                                        font.pixelSize: 25
                                        color: consolePane.cText
                                    }

                                    Repeater {
                                        model: [
                                            qsTr("On the PS5, open Settings › System › Remote Play and switch on Enable Remote Play."),
                                            qsTr("Keep the PS5 and this PC on the same network. A cable gives the steadiest picture."),
                                            qsTr("The console then shows up here by itself. Choose Set up and type the code the PS5 shows under Link Device.")
                                        ]

                                        RowLayout {
                                            id: stepRow
                                            required property int index
                                            required property string modelData
                                            Layout.fillWidth: true
                                            spacing: 14

                                            Rectangle {
                                                Layout.alignment: Qt.AlignTop
                                                implicitWidth: 30
                                                implicitHeight: 30
                                                radius: 15
                                                color: consolePane.cInset
                                                border.color: consolePane.cBorder
                                                Label {
                                                    anchors.centerIn: parent
                                                    text: stepRow.index + 1
                                                    color: consolePane.cAccent
                                                    font.pixelSize: 15
                                                    font.weight: Font.DemiBold
                                                }
                                            }

                                            Label {
                                                Layout.fillWidth: true
                                                Layout.topMargin: 4
                                                text: stepRow.modelData
                                                color: consolePane.cMuted
                                                font.pixelSize: 16
                                                wrapMode: Text.Wrap
                                                lineHeight: 1.15
                                            }
                                        }
                                    }

                                    CardAction {
                                        Layout.topMargin: 2
                                        Layout.leftMargin: 44
                                        text: qsTr("Add a console by address")
                                        onClicked: root.showManualHostDialog()
                                    }
                                }
                            }
                        }
                    }
                }

                // ------------------------------------------------------ frame generation
                Rectangle {
                    id: frameGenPanel
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: consolePane.roomy ? 448 : 360
                    Layout.preferredHeight: Math.min(contentRow.height, frameGenColumn.implicitHeight + 48)
                    radius: 16
                    color: consolePane.cSurface
                    border.color: consolePane.cBorder

                    Flickable {
                        id: frameGenFlick
                        anchors {
                            fill: parent
                            margins: 1
                        }
                        clip: true
                        contentWidth: width
                        contentHeight: frameGenColumn.implicitHeight + 46
                        boundsBehavior: Flickable.StopAtBounds
                        interactive: contentHeight > height

                        ColumnLayout {
                            id: frameGenColumn
                            x: 23
                            y: 23
                            width: frameGenFlick.width - 46
                            spacing: 14

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 14
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Frame generation")
                                    font.family: consolePane.serif
                                    font.pixelSize: 24
                                    color: consolePane.cText
                                }
                                Toggle {
                                    Layout.alignment: Qt.AlignVCenter
                                    on: Chiaki.settings.frameGenEnabled
                                    onClicked: Chiaki.settings.frameGenEnabled = !Chiaki.settings.frameGenEnabled
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                Layout.topMargin: -6
                                text: Chiaki.settings.frameGenEnabled
                                    ? qsTr("On. This PC's graphics card draws the frames in between, so a 30 FPS game is shown at %1 FPS. Changes apply from the next stream.").arg(Chiaki.settings.frameGenTargetFps)
                                    : qsTr("Off. The stream is shown exactly as the console sends it.")
                                color: consolePane.cMuted
                                font.pixelSize: 14
                                wrapMode: Text.Wrap
                                lineHeight: 1.15
                            }

                            // what it looks like: the same motion at the console's rate and at ours
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: cadenceColumn.implicitHeight + 30
                                radius: 12
                                color: consolePane.cInset
                                border.color: consolePane.cBorder

                                ColumnLayout {
                                    id: cadenceColumn
                                    anchors {
                                        left: parent.left
                                        right: parent.right
                                        top: parent.top
                                        margins: 15
                                    }
                                    spacing: 7

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("As the console sends it")
                                            color: consolePane.cFaint
                                            font.pixelSize: 13
                                        }
                                        Label {
                                            text: qsTr("%1 FPS").arg(30)
                                            color: consolePane.cMuted
                                            font.pixelSize: 13
                                        }
                                    }
                                    CadenceLane {
                                        phase: consolePane.cadencePhase
                                        fps: 30
                                        tint: "#8F8D85"
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.topMargin: 6
                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("As Robz Play shows it")
                                            color: consolePane.cFaint
                                            font.pixelSize: 13
                                        }
                                        Label {
                                            text: qsTr("%1 FPS").arg(consolePane.shownFps)
                                            color: Chiaki.settings.frameGenEnabled ? consolePane.cAccent : consolePane.cMuted
                                            font.pixelSize: 13
                                            font.weight: Chiaki.settings.frameGenEnabled ? Font.DemiBold : Font.Normal
                                        }
                                    }
                                    CadenceLane {
                                        phase: consolePane.cadencePhase
                                        fps: consolePane.shownFps
                                        tint: Chiaki.settings.frameGenEnabled ? "#D97757" : "#8F8D85"
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 14
                                opacity: Chiaki.settings.frameGenEnabled ? 1.0 : 0.4
                                enabled: Chiaki.settings.frameGenEnabled

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Label { text: qsTr("Target"); color: consolePane.cMuted; font.pixelSize: 14 }
                                    SegmentGroup {
                                        Segment {
                                            text: qsTr("60 FPS")
                                            active: Chiaki.settings.frameGenTargetFps === 60
                                            onClicked: Chiaki.settings.frameGenTargetFps = 60
                                        }
                                        Segment {
                                            text: qsTr("120 FPS")
                                            badge: qsTr("Experimental")
                                            active: Chiaki.settings.frameGenTargetFps === 120
                                            onClicked: Chiaki.settings.frameGenTargetFps = 120
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        visible: Chiaki.settings.frameGenTargetFps === 120
                                        text: qsTr("120 FPS is experimental. It needs a fast graphics card and a 120 Hz screen, and quick motion can show more artifacts than at 60 FPS.")
                                        color: consolePane.cWarn
                                        font.pixelSize: 12
                                        wrapMode: Text.Wrap
                                        lineHeight: 1.15
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Label { text: qsTr("Motion detail"); color: consolePane.cMuted; font.pixelSize: 14 }
                                    SegmentGroup {
                                        Segment {
                                            text: qsTr("Fast")
                                            active: Chiaki.settings.frameGenFlowDivisor === 4
                                            onClicked: Chiaki.settings.frameGenFlowDivisor = 4
                                        }
                                        Segment {
                                            text: qsTr("Balanced")
                                            active: Chiaki.settings.frameGenFlowDivisor === 2
                                            onClicked: Chiaki.settings.frameGenFlowDivisor = 2
                                        }
                                        Segment {
                                            text: qsTr("High")
                                            active: Chiaki.settings.frameGenFlowDivisor === 1
                                            onClicked: Chiaki.settings.frameGenFlowDivisor = 1
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 12
                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Keep HUD and text steady")
                                        color: consolePane.cMuted
                                        font.pixelSize: 14
                                        wrapMode: Text.Wrap
                                    }
                                    Toggle {
                                        on: Chiaki.settings.frameGenProtectStatic
                                        onClicked: Chiaki.settings.frameGenProtectStatic = !Chiaki.settings.frameGenProtectStatic
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.topMargin: 2
                                Layout.bottomMargin: 2
                                implicitHeight: 1
                                color: consolePane.cBorder
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Label { text: qsTr("Sharpening"); color: consolePane.cMuted; font.pixelSize: 14 }
                                SegmentGroup {
                                    Segment {
                                        text: qsTr("Off")
                                        active: Chiaki.settings.sharpening === 0
                                        onClicked: Chiaki.settings.sharpening = 0
                                    }
                                    Segment {
                                        text: qsTr("Low")
                                        active: Chiaki.settings.sharpening === 1
                                        onClicked: Chiaki.settings.sharpening = 1
                                    }
                                    Segment {
                                        text: qsTr("Medium")
                                        active: Chiaki.settings.sharpening === 2
                                        onClicked: Chiaki.settings.sharpening = 2
                                    }
                                    Segment {
                                        text: qsTr("High")
                                        active: Chiaki.settings.sharpening === 3
                                        onClicked: Chiaki.settings.sharpening = 3
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Brings back crispness the compressed stream loses. Applies right away.")
                                    color: consolePane.cFaint
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Show an FPS counter while playing")
                                    color: consolePane.cMuted
                                    font.pixelSize: 14
                                    wrapMode: Text.Wrap
                                }
                                Toggle {
                                    on: Chiaki.settings.showFpsCounter
                                    onClicked: Chiaki.settings.showFpsCounter = !Chiaki.settings.showFpsCounter
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
