#pragma once
// w2-080 (desktop only, never compiled into WebAssembly): one alarm view per client, the contract
// of TaidaFlowProxy::alarmViews / alarmViewRequested (Core/TaidaFlowProxy.h, w1-078, approved by
// Mango).  Same architecture as HistoryViewService (Core/HistoryViews.h):
//
// TaidaFlowProxy::alarmViewRequested(sessionId, fromMs, toMs, page) is the only request.
// AlarmViewService keeps one state per sessionId (the client's clientSessionId) and reads exactly
// the requested range asynchronously on the SqlManager thread (SqlManager::requestAlarmRangePage,
// sessionKey = sessionId: only a newer request of the SAME session makes a request stale; other
// sessions never do).  The result is written to TaidaFlowProxy::alarmViews[sessionId] =
//     { fromMs, toMs, page, pageSize (9), totalCount, totalPages (>= 1), activeCount,
//       rows, state ("ready" / "error"), message, revision }
// always by writing the whole map (setAlarmViews).  rows = this page, newest first
// (occurrence_time desc, id desc), each row = the alarmRecords record of the same alarm
// (AlarmRecordFormat::recordFromRow, the code Core::loadAlarmRecords uses) plus serialNumber
// = 1-based position in the whole range.  activeCount = alarms of the range whose alarmStatus is
// "未處理" (AlarmRecordFormat::isUnhandled, counted on the SqlManager thread).  A page past the
// last one is clamped to totalPages, a page < 1 to 1.  revision comes from one service-wide
// counter and changes only when the entry's content changes.
//
// Live updates: SqlManager::alarmHistoryChanged(occurrence) (emitted after every insertAlarm /
// updateAlarmReason, i.e. a new alarm or a resolved one) reads again every entry whose range
// holds that time, with its range and its current page (clamped to the new totalPages).  Other
// entries are not touched.
//
// Invalid requests: a session id that is not [A-Za-z0-9_-]{1,40} is ignored (no entry; logged at
// most once per invalidIdLogIntervalMs with the number of further ones).  With a valid id, a
// non-finite range, from > to, or a range outside 0 .. kMaxRangeMs (9999-12-31 end, UTC) writes
// the entry with state "error" and a message (no query).  A query that fails writes state
// "error" as well.
//
// Lifetime (as historyViews): a web entry (any id but "desktop") without a request for
// idleTimeoutMs (30 min) is removed; at most maxSessions (32) entries exist (desktop included),
// a new session removes the least recently used web session first; "desktop" is never removed.
// A removed client's next request rebuilds its entry.  Each removal writes the whole map once.
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <functional>

class SqlManager;
class TaidaFlowProxy;
struct AlarmRangePageResult;

class AlarmViewService : public QObject
{
    Q_OBJECT
public:
    struct Options
    {
        qint64 idleTimeoutMs = 30LL * 60 * 1000;   // web entries without a request are removed after this
        int maxSessions = 32;                      // entries kept at the same time, desktop included
        int sweepIntervalMs = 60 * 1000;           // how often idle entries are looked for
        int pageSize = 9;                          // rows per page (contract: 9)
        qint64 invalidIdLogIntervalMs = 60 * 1000; // invalid session ids: one log line per interval
        // Monotonic time in ms for the idle rule and the log limit; empty = an internal
        // QElapsedTimer.  Tests inject their own clock.
        std::function<qint64()> clock;
    };
    static constexpr auto kDesktopSessionId = "desktop";
    static constexpr double kMaxRangeMs = 253402300799999.0;   // 9999-12-31 23:59:59.999 UTC

    AlarmViewService(TaidaFlowProxy *proxy, SqlManager *sql, const Options &options,
                     QObject *parent = nullptr);
    ~AlarmViewService() override;

    // Connected to TaidaFlowProxy::alarmViewRequested.
    void handleRequest(const QString &sessionId, double fromMs, double toMs, int page);
    // Connected to SqlManager::alarmHistoryChanged (queued): reads again every valid entry whose
    // range holds occurrenceSec.
    void handleAlarmChanged(qint64 occurrenceSec);
    // Removes web sessions idle for idleTimeoutMs (also run by the sweep timer).
    void sweepIdle();

    QStringList sessionIds() const;                // sorted
    QVariantMap views() const { return m_views; }  // what was last written to alarmViews
    const Options &options() const { return m_options; }
    int invalidIdCount() const { return m_invalidIdCount; }

private:
    struct Session
    {
        quint64 latestRequestId = 0;    // results of other ids are stale
        qint64 lastRequestMs = 0;       // clock time of the newest client request (idle rule)
        quint64 lastUse = 0;            // order of client requests, for the least recently used rule
        double fromMs = 0;              // the client's range as requested, echoed into the entry
        double toMs = 0;
        qint64 fromSec = 0;             // the same range in whole seconds (occurrence_time)
        qint64 toSec = -1;
        int page = 1;                   // page to request (newest request, then the clamped result)
        bool valid = false;             // false: the newest request was invalid (error entry, no query)
        const char *reason = "";
        QElapsedTimer clock;            // started when the newest query was posted
    };

    qint64 now() const;
    void onPageReady(const AlarmRangePageResult &result);
    void post(const QString &sessionId, Session &session);
    void writeEntry(const QString &sessionId, const Session &session, int page, qint64 totalCount,
                    qint64 activeCount, const QVariantList &rows, const QString &state,
                    const QString &message);
    bool removeSessions(const QStringList &ids, const char *why);   // true if the map changed
    void logInvalidSessionId(const QString &sessionId);

    TaidaFlowProxy *m_proxy = nullptr;
    SqlManager *m_sql = nullptr;
    Options m_options;
    QHash<QString, Session> m_sessions;
    QVariantMap m_views;                // what was last written to alarmViews
    quint64 m_nextRequestId = 0;
    qint64 m_revision = 0;
    quint64 m_useCounter = 0;
    QElapsedTimer m_clock;              // default clock
    QTimer m_sweepTimer;
    // invalid session id log limit
    int m_invalidIdCount = 0;           // all ignored requests (diagnostics / tests)
    int m_invalidIdSuppressed = 0;      // not logged since the previous line
    qint64 m_invalidIdLoggedAt = -1;    // clock time of the previous line, -1 = never
};
