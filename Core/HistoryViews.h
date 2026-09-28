#pragma once
// w2-052 (desktop only, never compiled into WebAssembly): one History view per client
// (docs/taidaflow_history_export_spec.md §2.1, revised 2026-09-27).
//
// TaidaFlowProxy::historyViewRequested(sessionId, fromMs, toMs, page) is the only History
// trigger.  HistoryViewService keeps one state per sessionId (the client's clientSessionId),
// pages exactly the requested range through SqlManager::requestSensorHistoryRangePage
// (sessionKey = sessionId, so one client never makes another client's request stale and
// every client has its own keyset anchor) and writes the result to
// TaidaFlowProxy::historyViews[sessionId] =
//     { fromMs, toMs, page, totalPages, totalRows, records, revision }
// always by writing the whole map (setHistoryViews).  records = this page, newest first,
// each { timestampMs, values } with values = one cell per historyTitle column
// (time "yyyy/MM/dd HH:mm:ss", TT-01..04, PT-01..07, FM-01, M1..M4 converted, null = no value).
// revision comes from one service-wide counter and changes only when the entry's content
// changes, so a rebuilt entry never repeats a revision a client already shows.
//
// Lifetime: a web entry (any id but "desktop") with no request for idleTimeoutMs (30 min)
// is removed, together with the SqlManager state of that session; at most maxSessions (32)
// entries exist (desktop included), a new session removes the least recently used web
// session first; "desktop" is never removed.  A removed client's next request rebuilds it.
// Each removal writes the whole map once.
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

class SqlManager;
class TaidaFlowProxy;
struct SensorHistoryPageResult;

class HistoryViewService : public QObject
{
    Q_OBJECT
public:
    struct Options
    {
        qint64 idleTimeoutMs = 30LL * 60 * 1000;   // web entries without a request are removed after this
        int maxSessions = 32;                      // entries kept at the same time, desktop included
        int sweepIntervalMs = 60 * 1000;           // how often idle entries are looked for
        int pageSize = 10;
    };
    static constexpr auto kDesktopSessionId = "desktop";

    HistoryViewService(TaidaFlowProxy *proxy, SqlManager *sql, const Options &options,
                       QObject *parent = nullptr);
    ~HistoryViewService() override;

    // Connected to TaidaFlowProxy::historyViewRequested.  Invalid requests (session id not
    // [A-Za-z0-9_-]{1,40}, non-finite range or from > to, page < 1) are logged and ignored.
    void handleRequest(const QString &sessionId, double fromMs, double toMs, int page);
    // Removes web sessions idle for idleTimeoutMs (also run by the sweep timer).
    void sweepIdle();

    QStringList sessionIds() const;                // sorted
    const Options &options() const { return m_options; }

private:
    struct Session
    {
        quint64 latestRequestId = 0;    // results of other ids are stale
        qint64 lastRequestMs = 0;       // m_clock time of the newest request (idle rule)
        quint64 lastUse = 0;            // order of requests, for the least recently used rule
        double fromMs = 0;              // the newest request, echoed into the entry
        double toMs = 0;
        int requestedPage = 1;
        bool clampRequest = false;      // the newest request moves a too large page to the last page
        const char *reason = "";
        QElapsedTimer clock;            // started when the newest request was posted
    };

    void onPageReady(const SensorHistoryPageResult &result);
    void post(const QString &sessionId, Session &session);
    void writeEntry(const QString &sessionId, const Session &session, int page, int totalPages,
                    qint64 totalRows, const QVariantList &records);
    bool removeSessions(const QStringList &ids, const char *why);   // true if the map changed

    TaidaFlowProxy *m_proxy = nullptr;
    SqlManager *m_sql = nullptr;
    Options m_options;
    QHash<QString, Session> m_sessions;
    QVariantMap m_views;                // what was last written to historyViews
    quint64 m_nextRequestId = 0;
    qint64 m_revision = 0;
    quint64 m_useCounter = 0;
    QElapsedTimer m_clock;              // monotonic time for the idle rule
    QTimer m_sweepTimer;
};
