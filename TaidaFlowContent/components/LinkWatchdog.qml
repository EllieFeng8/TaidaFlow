import QtQuick
import "LinkWatchdog.js" as LinkWatchdogJs

// w1-083: heartbeat watchdog of the web page (WASM only). TopNav.qml owns the one instance and
// publishes `linkAlive`; every page gates its controls with it (see TopNav.qml).
//
// Inputs:
//   backend     - Td (TaidaFlowProxy): transportReady, serverHeartbeatMs
//   pageControl - WebPageControl from App/main.cpp (App.qml -> TopNav.webPage):
//                 isWebPage, monotonicMs(), epochMs(), sessionValue(key),
//                 setSessionValue(key, value), reloadPage()
// The watchdog is active only when pageControl.isWebPage is true (WebAssembly). On the desktop,
// or without pageControl (tests / previews loading TopNav alone), it never detects anything and
// linkAlive simply follows transportReady (always true on the desktop).
//
// Detection (thresholds: LinkWatchdog.js):
//   - "armed" only after serverHeartbeatMs has CHANGED to a non-zero value at least once
//     (main alone / a Core without heartbeat keep 0: never armed); a change to 0, or the
//     transport going down, disarms it until the next change.
//   - silentMs = time without a change, counted with the local monotonic clock only while
//     transportReady is true (steps of a frozen page are not counted, see MAX_COUNTED_STEP_MS).
//   - heartbeatStale: silentMs > 5 s -> linkAlive false (banner + controls disabled);
//     the next change clears it at once.
//   - silentMs >= 15 s with transportReady still true -> pageControl.reloadPage(), after the
//     sessionStorage back-off (LinkWatchdog.js reloadWaitMs); transportReady false never reloads.
Item {
    id: watchdog
    visible: false

    property var backend: null
    property var pageControl: null

    readonly property bool active: !!pageControl && pageControl.isWebPage === true
    readonly property bool transportReady: !!backend && backend.transportReady === true
    readonly property real heartbeatMs: backend ? Number(backend.serverHeartbeatMs) || 0 : 0

    // State (written only by the functions below).
    property bool heartbeatSeen: false
    property real silentMs: 0
    property real aliveMs: 0
    property real lastTickMonoMs: -1
    property bool reloadIssued: false
    // Changes of heartbeatMs before Component.onCompleted only pick up the value the backend
    // already had (binding set-up), they are not heartbeats received by this page.
    property bool completed: false
    // Back-off state, mirrored from sessionStorage (loadBackoffState / recordReload).
    property real lastReloadEpochMs: 0
    property int reloadStreak: 0
    // Remaining time until the automatic reload (ms), -1 when none is pending.
    property real reloadInMs: -1

    readonly property bool armed: active && heartbeatSeen
    readonly property bool heartbeatStale: armed && transportReady
                                           && silentMs > LinkWatchdogJs.STALE_AFTER_MS
    // The one flag the pages use for their controls (TopNav.qml -> root.linkAlive).
    readonly property bool linkAlive: transportReady && !heartbeatStale

    function monotonicNow() {
        return Number(pageControl.monotonicMs())
    }

    function loadBackoffState() {
        if (!active)
            return
        lastReloadEpochMs = LinkWatchdogJs.parseStoredNumber(
                    pageControl.sessionValue(LinkWatchdogJs.STORAGE_LAST_RELOAD_KEY))
        reloadStreak = Math.max(0, Math.floor(LinkWatchdogJs.parseStoredNumber(
                    pageControl.sessionValue(LinkWatchdogJs.STORAGE_STREAK_KEY))))
    }

    function storeStreak(streak) {
        reloadStreak = streak
        pageControl.setSessionValue(LinkWatchdogJs.STORAGE_STREAK_KEY, String(streak))
    }

    function recordReload(nowEpochMs) {
        lastReloadEpochMs = nowEpochMs
        pageControl.setSessionValue(LinkWatchdogJs.STORAGE_LAST_RELOAD_KEY, String(nowEpochMs))
        storeStreak(reloadStreak + 1)
    }

    function disarm() {
        heartbeatSeen = false
        silentMs = 0
        aliveMs = 0
        reloadInMs = -1
    }

    // One check; called by the timer every CHECK_INTERVAL_MS (tests call it directly with a
    // controlled pageControl clock).
    function tick() {
        if (!active)
            return
        var now = monotonicNow()
        var step = lastTickMonoMs < 0 ? 0 : now - lastTickMonoMs
        lastTickMonoMs = now
        if (step < 0)
            step = 0
        else if (step > LinkWatchdogJs.MAX_COUNTED_STEP_MS)
            step = LinkWatchdogJs.CHECK_INTERVAL_MS
        if (!armed || !transportReady) {
            aliveMs = 0
            reloadInMs = -1
            return
        }
        silentMs += step
        if (!heartbeatStale) {
            aliveMs += step
            reloadInMs = -1
            if (reloadStreak > 0 && aliveMs >= LinkWatchdogJs.STREAK_RESET_AFTER_MS)
                storeStreak(0)
            return
        }
        aliveMs = 0
        var epochNow = Number(pageControl.epochMs())
        var wait = Math.max(LinkWatchdogJs.RELOAD_AFTER_MS - silentMs,
                            LinkWatchdogJs.reloadWaitMs(epochNow, lastReloadEpochMs, reloadStreak))
        reloadInMs = Math.max(0, wait)
        if (wait > 0 || reloadIssued)
            return
        reloadIssued = true
        recordReload(epochNow)
        console.warn("[LinkWatchdog] no server heartbeat for", Math.round(silentMs),
                     "ms while the transport is ready: reloading the page (automatic reload",
                     reloadStreak, "in a row)")
        pageControl.reloadPage()
    }

    onHeartbeatMsChanged: {
        if (!active || !completed)
            return
        if (heartbeatMs === 0) {
            disarm()
            return
        }
        heartbeatSeen = true
        silentMs = 0
        reloadIssued = false
        reloadInMs = -1
    }

    onTransportReadyChanged: {
        // Down: the existing offline behaviour takes over; the next connection re-arms only
        // after it has seen the heartbeat change again. Up: silence is counted from now.
        if (!transportReady)
            disarm()
        silentMs = 0
    }

    onActiveChanged: loadBackoffState()
    Component.onCompleted: {
        completed = true
        loadBackoffState()
    }

    Timer {
        interval: LinkWatchdogJs.CHECK_INTERVAL_MS
        repeat: true
        running: watchdog.active
        onTriggered: watchdog.tick()
    }
}
