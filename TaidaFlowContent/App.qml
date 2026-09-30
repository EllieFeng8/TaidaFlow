import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import TaidaFlow

// w1-081: T.ApplicationWindow (the unstyled template, i.e. a Window plus font propagation;
// no style colors or focus frames) so that every control and popup inherits the UI font.
// Plain Text items get the same font as the application default (main.cpp applyUiFont()).
T.ApplicationWindow {
    id: appWindow

    // UI font from main.cpp (QGuiApplication::setFont). Only the family: the controls keep
    // their own sizes. Controls do not follow the application font by themselves (the
    // Universal style uses Segoe UI) and popups inherit only from an ApplicationWindow.
    font.family: Application.font.family

    // =========================================================
    // Web-only global scaling (Qt.platform.os === "wasm"). Desktop: nothing below
    // applies - the normal window is sized by topScreen (1920x1080) and
    // topScreen stays a direct child of the window, unscaled.
    // Web: the window fills the browser page (full-screen in the page container,
    // which follows the browser size), the 1920x1080 design (TopNav root) is scaled
    // uniformly by webScale = max(window width, 720) / 1920 and sits in a Flickable:
    // below 720 px the content stays 720 px wide and scrolls horizontally; when the
    // scaled height exceeds the window it scrolls vertically.
    // =========================================================
    readonly property bool webScaling: Qt.platform.os === "wasm"
    readonly property real designWidth: 1920
    readonly property real designHeight: 1080
    readonly property real minimumWebWidth: 720
    readonly property real webContentWidth: Math.max(width, minimumWebWidth)
    readonly property real webScale: webContentWidth / designWidth

    width: webScaling ? designWidth : topScreen.width
    height: webScaling ? designHeight : topScreen.height

    visible: true
    visibility: webScaling ? Window.Windowed : Window.FullScreen
    title: "TaidaFlow"
    color: "#0f192d"

    Shortcut {
        sequence: "F11"
        enabled: !appWindow.webScaling
        context: Qt.WindowShortcut
        autoRepeat: false
        onActivated: {
            if (appWindow.visibility === Window.FullScreen)
                appWindow.showNormal()
            else
                appWindow.showFullScreen()
        }
    }

    TopNav {
        id: topScreen
        // anchors.centerIn: appWindow.webScaling ? undefined : parent
    }

    // Instantiated only on the web (see Component.onCompleted), so the desktop
    // scene is unchanged.
    Component {
        id: webViewportComponent

        Flickable {
            id: webViewport
            anchors.fill: parent
            clip: true
            contentWidth: appWindow.webContentWidth
            contentHeight: appWindow.designHeight * appWindow.webScale
            // Only a direction whose content is larger than the window can be
            // dragged / wheeled; no overshoot past the design edges.
            flickableDirection: Flickable.AutoFlickIfNeeded
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.horizontal: ScrollBar {
                policy: webViewport.contentWidth > webViewport.width + 0.5
                        ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            }
            ScrollBar.vertical: ScrollBar {
                policy: webViewport.contentHeight > webViewport.height + 0.5
                        ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            }
        }
    }

    Component.onCompleted: {
        if (!webScaling)
            return
        var viewport = webViewportComponent.createObject(appWindow.contentItem)
        topScreen.parent = viewport.contentItem
        topScreen.transformOrigin = Item.TopLeft
        topScreen.scale = Qt.binding(function() { return appWindow.webScale })
        // Qt for WebAssembly: a full-screen window covers its whole screen (the
        // page's container element) and is resized with it; no browser full-screen
        // mode is involved.
        appWindow.showFullScreen()
    }
}
