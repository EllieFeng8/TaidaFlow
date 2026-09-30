#include "AlarmViews.h"

#include "AlarmRecordFormat.h"
#include "HistoryExport.h"
#include "SqlManager.h"
#include "TaidaFlowProxy.h"

#include <QCoreApplication>
#include <QVariantList>
#include <QtDebug>

#include <algorithm>
#include <cmath>

namespace {
const QString kDesktop = QString::fromLatin1(AlarmViewService::kDesktopSessionId);
// Shown by AlarmPage.qml in its status text (entry "message" of state "error").
const QString kInvalidRangeMessage = QStringLiteral("日期區間錯誤");   // same wording as the export refusal
const QString kReadFailedMessage = QStringLiteral("警報紀錄讀取失敗");

QString msText(double ms)
{
    if (!std::isfinite(ms) || std::fabs(ms) >= 1.0e16)
        return QString::number(ms, 'g', 17);   // nan, inf, 1e+300 (never hundreds of digits)
    return QString::number(ms, 'f', 0);
}
} // namespace

AlarmViewService::AlarmViewService(TaidaFlowProxy *proxy, SqlManager *sql, const Options &options,
                                   QObject *parent)
    : QObject(parent), m_proxy(proxy), m_sql(sql), m_options(options)
{
    m_options.maxSessions = std::max(1, m_options.maxSessions);
    m_options.pageSize = std::max(1, m_options.pageSize);
    m_clock.start();
    if (m_proxy) {
        connect(m_proxy, &TaidaFlowProxy::alarmViewRequested, this, &AlarmViewService::handleRequest);
        m_views = m_proxy->alarmViews();   // normally empty; only the Core writes it
    }
    if (m_sql) {
        // Both come from the SqlManager thread: queued to this (main-thread) object.
        connect(m_sql, &SqlManager::alarmRangePageReady, this, &AlarmViewService::onPageReady,
                Qt::QueuedConnection);
        connect(m_sql, &SqlManager::alarmHistoryChanged, this, &AlarmViewService::handleAlarmChanged,
                Qt::QueuedConnection);
    }
    m_sweepTimer.setInterval(std::max(1, m_options.sweepIntervalMs));
    connect(&m_sweepTimer, &QTimer::timeout, this, &AlarmViewService::sweepIdle);
    m_sweepTimer.start();
    qInfo().noquote() << QStringLiteral("[Alarm] per-client views: idle removal after %1 s (never \"%2\"), "
                                        "at most %3 entries, %4 rows per page")
                                 .arg(m_options.idleTimeoutMs / 1000.0, 0, 'f', 1)
                                 .arg(kDesktop)
                                 .arg(m_options.maxSessions)
                                 .arg(m_options.pageSize);
}

AlarmViewService::~AlarmViewService()
{
    // A read may still be running on the SqlManager thread; its result is not delivered after
    // this (a result already queued for this object is removed with it).  The newest ids kept by
    // SqlManager for our sessions are released (only a mutex, also after SqlManager::shutdown).
    if (m_sql) {
        disconnect(m_sql, nullptr, this, nullptr);
        if (QCoreApplication::instance()) {
            for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it)
                m_sql->releaseAlarmSession(it.key());
        }
    }
}

qint64 AlarmViewService::now() const
{
    return m_options.clock ? m_options.clock() : m_clock.elapsed();
}

QStringList AlarmViewService::sessionIds() const
{
    QStringList ids = m_sessions.keys();
    ids.sort();
    return ids;
}

void AlarmViewService::logInvalidSessionId(const QString &sessionId)
{
    ++m_invalidIdCount;
    const qint64 t = now();
    if (m_invalidIdLoggedAt >= 0 && t - m_invalidIdLoggedAt < m_options.invalidIdLogIntervalMs) {
        ++m_invalidIdSuppressed;
        return;
    }
    QString shown = sessionId.left(40);
    for (QChar &c : shown) {
        if (c.unicode() < 0x20 || c.unicode() > 0x7e)
            c = QLatin1Char('?');
    }
    const QString more = m_invalidIdSuppressed > 0
            ? QStringLiteral("; %1 more since the previous message").arg(m_invalidIdSuppressed)
            : QString();
    qWarning().noquote() << QStringLiteral("[Alarm] view request with an invalid session id \"%1\"%2 (%3 chars) "
                                           "ignored, not stored%4 (logged at most once per %5 s)")
                                    .arg(shown, sessionId.size() > 40 ? QStringLiteral("...") : QString())
                                    .arg(sessionId.size())
                                    .arg(more)
                                    .arg(m_options.invalidIdLogIntervalMs / 1000.0, 0, 'f', 0);
    m_invalidIdSuppressed = 0;
    m_invalidIdLoggedAt = t;
}

void AlarmViewService::handleRequest(const QString &sessionId, double fromMs, double toMs, int page)
{
    if (!HistoryExport::isValidSessionId(sessionId)) {
        logInvalidSessionId(sessionId);
        return;
    }

    auto it = m_sessions.find(sessionId);
    const bool created = it == m_sessions.end();
    if (created) {
        // Room for the new entry: remove the least recently used web sessions (never desktop).
        QStringList evict;
        while (m_sessions.size() - evict.size() >= m_options.maxSessions) {
            QString oldest;
            quint64 oldestUse = 0;
            for (auto s = m_sessions.cbegin(); s != m_sessions.cend(); ++s) {
                if (s.key() == kDesktop || evict.contains(s.key()))
                    continue;
                if (oldest.isEmpty() || s->lastUse < oldestUse) {
                    oldest = s.key();
                    oldestUse = s->lastUse;
                }
            }
            if (oldest.isEmpty())
                break;
            evict << oldest;
        }
        if (!evict.isEmpty())
            removeSessions(evict, "entry limit");
        if (m_sessions.size() >= m_options.maxSessions && sessionId != kDesktop) {
            qWarning().noquote() << QStringLiteral("[Alarm] %1 view request ignored: %2 entries (limit %3) and none "
                                                   "can be removed").arg(sessionId).arg(m_sessions.size())
                                            .arg(m_options.maxSessions);
            return;
        }
        it = m_sessions.insert(sessionId, Session{});
    }

    Session &session = it.value();
    session.lastRequestMs = now();
    session.lastUse = ++m_useCounter;
    session.page = std::max(1, page);
    session.reason = created ? "new view" : "request";

    const bool rangeOk = std::isfinite(fromMs) && std::isfinite(toMs) && fromMs <= toMs
            && fromMs >= 0.0 && toMs <= kMaxRangeMs;
    if (!rangeOk) {
        // No query: the entry says why.  A query of an earlier request still running is dropped.
        session.valid = false;
        session.fromMs = std::isfinite(fromMs) ? fromMs : 0.0;
        session.toMs = std::isfinite(toMs) ? toMs : 0.0;
        session.fromSec = 0;
        session.toSec = -1;
        session.page = 1;
        session.latestRequestId = ++m_nextRequestId;
        if (m_sql)
            m_sql->releaseAlarmSession(sessionId);
        qWarning().noquote() << QStringLiteral("[Alarm] %1 view request refused: invalid range %2 .. %3 ms "
                                               "(must be finite, 0 <= from <= to <= %4); state \"error\".")
                                        .arg(sessionId, msText(fromMs), msText(toMs), msText(kMaxRangeMs));
        writeEntry(sessionId, session, 1, 0, 0, QVariantList{}, QStringLiteral("error"), kInvalidRangeMessage);
        return;
    }

    session.valid = true;
    session.fromMs = fromMs;
    session.toMs = toMs;
    // occurrence_time is in seconds and timestampMs = occurrence_time * 1000 must lie in
    // [fromMs, toMs]: ceil / floor (from = hh:mm:00.000, to = hh:mm:59.999 -> hh:mm:00 .. hh:mm:59).
    session.fromSec = static_cast<qint64>(std::ceil(fromMs / 1000.0));
    session.toSec = static_cast<qint64>(std::floor(toMs / 1000.0));
    post(sessionId, session);
}

void AlarmViewService::post(const QString &sessionId, Session &session)
{
    session.latestRequestId = ++m_nextRequestId;
    session.clock.start();
    if (!m_sql) {
        writeEntry(sessionId, session, 1, 0, 0, QVariantList{}, QStringLiteral("error"), kReadFailedMessage);
        return;
    }
    m_sql->requestAlarmRangePage(sessionId, session.latestRequestId, session.fromSec, session.toSec,
                                 session.page, m_options.pageSize, &AlarmRecordFormat::isUnhandled);
    qInfo().noquote()
            << QStringLiteral("[Alarm] %1 request #%2 (%3) page %4, range %5 .. %6 ms (%7 .. %8 s), %9 session(s).")
                       .arg(sessionId)
                       .arg(session.latestRequestId)
                       .arg(QString::fromLatin1(session.reason))
                       .arg(session.page)
                       .arg(msText(session.fromMs), msText(session.toMs))
                       .arg(session.fromSec)
                       .arg(session.toSec)
                       .arg(m_sessions.size());
}

void AlarmViewService::handleAlarmChanged(qint64 occurrenceSec)
{
    QStringList hit;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if (it->valid && it->fromSec <= occurrenceSec && occurrenceSec <= it->toSec)
            hit << it.key();
    }
    hit.sort();
    qInfo().noquote() << QStringLiteral("[Alarm] alarm row written at %1 s: %2 of %3 view(s) hold that time%4")
                                 .arg(occurrenceSec)
                                 .arg(hit.size())
                                 .arg(m_sessions.size())
                                 .arg(hit.isEmpty() ? QString() : QStringLiteral(", read again: ") + hit.join(QStringLiteral(", ")));
    for (const QString &id : std::as_const(hit)) {
        Session &session = m_sessions[id];
        session.reason = "alarm changed";
        post(id, session);   // same range, same page (clamped by the result)
    }
}

void AlarmViewService::onPageReady(const AlarmRangePageResult &result)
{
    const auto it = m_sessions.find(result.sessionKey);
    if (it == m_sessions.end()) {
        qInfo().noquote() << QStringLiteral("[Alarm] %1 result #%2 dropped: no such session here (removed).")
                                     .arg(result.sessionKey).arg(result.requestId);
        return;
    }
    Session &session = it.value();
    if (result.requestId != session.latestRequestId) {
        qInfo().noquote() << QStringLiteral("[Alarm] %1 result #%2 dropped: stale, newest request of this session is #%3%4.")
                                     .arg(result.sessionKey)
                                     .arg(result.requestId)
                                     .arg(session.latestRequestId)
                                     .arg(result.superseded ? QStringLiteral(" (not finished by SqlManager)") : QString());
        return;
    }
    if (result.superseded) {
        // Only when SqlManager lost the session while this id is still ours; not through this class.
        qWarning().noquote() << QStringLiteral("[Alarm] %1 result #%2 superseded in SqlManager; nothing written.")
                                        .arg(result.sessionKey).arg(result.requestId);
        return;
    }
    const QString sessionId = result.sessionKey;
    const double roundTripMs = session.clock.nsecsElapsed() / 1.0e6;
    if (!result.ok) {
        qWarning().noquote() << QStringLiteral("[Alarm] %1 #%2: reading the alarm range failed: %3; state \"error\".")
                                        .arg(sessionId).arg(result.requestId).arg(result.errorMessage);
        writeEntry(sessionId, session, 1, 0, 0, QVariantList{}, QStringLiteral("error"), kReadFailedMessage);
        return;
    }

    QVariantList rows;
    rows.reserve(result.rows.size());
    const qint64 first = static_cast<qint64>(result.page - 1) * m_options.pageSize;
    for (qsizetype i = 0; i < result.rows.size(); ++i) {
        const AlarmRangeRow &row = result.rows.at(i);
        QVariantMap record = AlarmRecordFormat::recordFromRow(row.id, row.occurrenceTime, row.reason);
        record.insert(QStringLiteral("serialNumber"), first + i + 1);
        rows.append(record);
    }
    session.page = result.page;   // live updates keep this (clamped) page
    qInfo().noquote()
            << QStringLiteral("[Alarm] %1 #%2 applied: page %3 (requested %4), %5 row(s), total %6, active %7, "
                              "%8 month file(s); SqlManager %9 step(s) (longest %10 ms); request-to-apply %11 ms.")
                       .arg(sessionId)
                       .arg(result.requestId)
                       .arg(result.page)
                       .arg(result.requestedPage)
                       .arg(rows.size())
                       .arg(result.totalCount)
                       .arg(result.activeCount)
                       .arg(result.months)
                       .arg(result.steps)
                       .arg(result.maxStepMs, 0, 'f', 2)
                       .arg(roundTripMs, 0, 'f', 2);
    writeEntry(sessionId, session, result.page, result.totalCount, result.activeCount, rows,
               QStringLiteral("ready"), QString());
}

void AlarmViewService::writeEntry(const QString &sessionId, const Session &session, int page, qint64 totalCount,
                                  qint64 activeCount, const QVariantList &rows, const QString &state,
                                  const QString &message)
{
    const qint64 pageSize = m_options.pageSize;
    const int totalPages = static_cast<int>(std::max<qint64>(1, (totalCount + pageSize - 1) / pageSize));
    QVariantMap entry{
        {QStringLiteral("fromMs"), session.fromMs},
        {QStringLiteral("toMs"), session.toMs},
        {QStringLiteral("page"), std::clamp(page, 1, totalPages)},
        {QStringLiteral("pageSize"), m_options.pageSize},
        {QStringLiteral("totalCount"), totalCount},
        {QStringLiteral("totalPages"), totalPages},
        {QStringLiteral("activeCount"), activeCount},
        {QStringLiteral("rows"), rows},
        {QStringLiteral("state"), state},
        {QStringLiteral("message"), message},
    };
    QVariantMap previous = m_views.value(sessionId).toMap();
    const QVariant previousRevision = previous.take(QStringLiteral("revision"));
    if (previousRevision.isValid() && previous == entry) {
        qInfo().noquote() << QStringLiteral("[Alarm] %1 entry unchanged (page %2/%3, revision %4); alarmViews not written.")
                                     .arg(sessionId).arg(page).arg(totalPages).arg(previousRevision.toLongLong());
        return;
    }
    entry.insert(QStringLiteral("revision"), ++m_revision);
    m_views.insert(sessionId, entry);
    if (m_proxy)
        m_proxy->setAlarmViews(m_views);
    qInfo().noquote() << QStringLiteral("[Alarm] %1 entry written: state %2, page %3/%4, %5 row(s), total %6, "
                                        "active %7, revision %8; alarmViews has %9 entr%10.")
                                 .arg(sessionId, state)
                                 .arg(entry.value(QStringLiteral("page")).toInt())
                                 .arg(totalPages)
                                 .arg(rows.size())
                                 .arg(totalCount)
                                 .arg(activeCount)
                                 .arg(m_revision)
                                 .arg(m_views.size())
                                 .arg(m_views.size() == 1 ? QStringLiteral("y") : QStringLiteral("ies"));
}

bool AlarmViewService::removeSessions(const QStringList &ids, const char *why)
{
    bool changed = false;
    for (const QString &id : ids) {
        m_sessions.remove(id);
        if (m_sql)
            m_sql->releaseAlarmSession(id);
        changed = m_views.remove(id) > 0 || changed;
    }
    if (changed && m_proxy)
        m_proxy->setAlarmViews(m_views);      // once for all removed entries
    qInfo().noquote() << QStringLiteral("[Alarm] removed %1 view session(s) (%2): %3; %4 session(s) left, alarmViews %5.")
                                 .arg(ids.size())
                                 .arg(QString::fromLatin1(why))
                                 .arg(ids.join(QStringLiteral(", ")))
                                 .arg(m_sessions.size())
                                 .arg(changed ? QStringLiteral("written once") : QStringLiteral("unchanged"));
    return changed;
}

void AlarmViewService::sweepIdle()
{
    const qint64 t = now();
    QStringList idle;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if (it.key() != kDesktop && t - it->lastRequestMs >= m_options.idleTimeoutMs)
            idle << it.key();
    }
    if (idle.isEmpty())
        return;
    idle.sort();
    removeSessions(idle, "idle");
}
