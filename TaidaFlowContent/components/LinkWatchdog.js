.pragma library

// w1-083: constants and pure helpers of the web page's heartbeat watchdog (LinkWatchdog.qml).
// All thresholds of the feature live here; the Core side interval is
// TaidaFlowProxy::kServerHeartbeatIntervalMs (Core/TaidaFlowProxy.h, "Server heartbeat").

// Core side (informational, the page does not use it): the Core writes Td.serverHeartbeatMs
// every 1 s. The thresholds below are multiples of it.
var HEARTBEAT_INTERVAL_MS = 1000

// Link broken: transportReady is true but serverHeartbeatMs has not changed for MORE than
// 5 s (5 missed heartbeats) -> offline banner "連線中斷，正在恢復…", every control disabled.
var STALE_AFTER_MS = 5000

// Reload: still no change after 15 s (or longer) while transportReady is true (the half-open
// connection of 2026-10-01) -> reload the page (same as F5), subject to the back-off below.
// transportReady false (desktop really closed) never reloads: the pack reconnects by itself.
var RELOAD_AFTER_MS = 15000

// Back-off of automatic reloads, kept in the browser's sessionStorage (per tab, survives the
// reload): the first automatic reload happens at once; each further one waits until at least
// 60 s after the previous one, and the wait doubles with each consecutive reload
// (60 s, 120 s, 240 s), never more than 5 min.
var RELOAD_BACKOFF_BASE_MS = 60000
var RELOAD_BACKOFF_MAX_MS = 300000

// "Consecutive" ends when the link has been alive (heartbeats arriving) for 60 s without a
// break: the stored streak is set back to 0, so the next half-open reloads at once again.
var STREAK_RESET_AFTER_MS = 60000

// The watchdog checks every 0.5 s. A step between two checks longer than 2 s means the page
// itself was not running (frozen / throttled background tab): that time is not counted as
// silence (only one normal step is), so a page coming back to the foreground is never
// reloaded just because its own timers were stopped.
var CHECK_INTERVAL_MS = 500
var MAX_COUNTED_STEP_MS = 2000

// sessionStorage keys (values are decimal strings).
var STORAGE_LAST_RELOAD_KEY = "taidaflow.autoReload.lastEpochMs"
var STORAGE_STREAK_KEY = "taidaflow.autoReload.streak"

// Stored text -> number; missing / invalid -> 0.
function parseStoredNumber(text) {
    var value = Number(text)
    return (text === undefined || text === null || text === "" || !isFinite(value)) ? 0 : value
}

// Minimum time between the previous automatic reload and the next one, for `streak` automatic
// reloads in a row so far: 0 -> 0 (reload at once), 1 -> 60 s, 2 -> 120 s, 3 -> 240 s,
// 4 and more -> 300 s.
function reloadGapMs(streak) {
    if (!(streak > 0))
        return 0
    var gap = RELOAD_BACKOFF_BASE_MS * Math.pow(2, Math.min(streak, 20) - 1)
    return Math.min(gap, RELOAD_BACKOFF_MAX_MS)
}

// How long the next automatic reload still has to wait (ms, >= 0). nowEpochMs and
// lastReloadEpochMs are wall-clock epoch ms (the monotonic clock restarts with every page
// load). A previous reload "in the future" (clock set back) counts as just now, so the wait
// is at most one gap.
function reloadWaitMs(nowEpochMs, lastReloadEpochMs, streak) {
    var gap = reloadGapMs(streak)
    if (gap <= 0 || !(lastReloadEpochMs > 0))
        return 0
    var elapsed = Math.max(0, nowEpochMs - lastReloadEpochMs)
    return Math.max(0, gap - elapsed)
}
