#include "HistoryViews.h"

#include "HistoryExport.h"
#include "SqlManager.h"
#include "TaidaFlowProxy.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QVariantList>

#include <algorithm>

namespace {
const QString kDesktop = QString::fromLatin1(HistoryViewService::kDesktopSessionId);

// One History page (SqlManager samples, newest first) -> the records of a historyViews
// entry: { timestampMs, values }, values = one cell per historyTitle column (moved
// unchanged from the former Core::applyHistoryPage, w2-039/w2-041).
QVariantList recordsFromSamples(const QJsonArray &samples)
{
    constexpr double adcFullScale = 65535.0;
    QVariantList records;
    records.reserve(samples.size());
    for (const QJsonValue &sampleValue : samples) {
        const QJsonObject sample = sampleValue.toObject();
        const qint64 timestamp = static_cast<qint64>(sample.value(QStringLiteral("ts")).toDouble());
        if (timestamp <= 0)
            continue;

        const auto valueAt = [&sample](int sensorIndex, double scale = 1.0) -> QVariant {
            const QJsonValue sensorValue = sample.value(QStringLiteral("s%1").arg(sensorIndex));
            if (sensorValue.isNull() || sensorValue.isUndefined())
                return {};
            bool isNumber = false;
            const double rawValue = sensorValue.toVariant().toDouble(&isNumber);
            return isNumber ? QVariant(rawValue * scale) : QVariant();
        };

        const QDateTime sampleTime = QDateTime::fromSecsSinceEpoch(timestamp);
        QVariantList values{sampleTime.toString(QStringLiteral("yyyy/MM/dd HH:mm:ss"))};
        for (int sensorIndex = 1; sensorIndex <= 4; ++sensorIndex)
            values.append(valueAt(sensorIndex, 100.0 / adcFullScale));
        for (int sensorIndex = 5; sensorIndex <= 11; ++sensorIndex)
            values.append(valueAt(sensorIndex, 1000.0 / adcFullScale));
        values.append(valueAt(12));
        for (int sensorIndex = 13; sensorIndex <= 16; ++sensorIndex)
            values.append(valueAt(sensorIndex, 100.0 / adcFullScale));

        records.append(QVariantMap{
            {QStringLiteral("timestampMs"), timestamp * 1000},
            {QStringLiteral("values"), values}
        });
    }
    return records;
}

QString msText(double ms)
{
    return QString::number(ms, 'f', 0);
}
} // namespace

HistoryViewService::HistoryViewService(TaidaFlowProxy *proxy, SqlManager *sql, const Options &options,
                                       QObject *parent)
    : QObject(parent), m_proxy(proxy), m_sql(sql), m_options(options)
{
    m_options.maxSessions = std::max(1, m_options.maxSessions);
    m_options.pageSize = std::max(1, m_options.pageSize);
    m_clock.start();
    if (m_proxy) {
        connect(m_proxy, &TaidaFlowProxy::historyViewRequested, this, &HistoryViewService::handleRequest);
        m_views = m_proxy->historyViews();   // normally empty; only the Core writes it
    }
    if (m_sql) {
        // Results come back queued from the SqlManager thread.
        connect(m_sql, &SqlManager::sensorHistoryPageReady, this, &HistoryViewService::onPageReady,
                Qt::QueuedConnection);
    }
    m_sweepTimer.setInterval(std::max(1, m_options.sweepIntervalMs));
    connect(&m_sweepTimer, &QTimer::timeout, this, &HistoryViewService::sweepIdle);
    m_sweepTimer.start();
    qInfo().noquote() << QStringLiteral("[History] per-client views: idle removal after %1 s (never \"%2\"), "
                                        "at most %3 entries, %4 rows per page")
                                 .arg(m_options.idleTimeoutMs / 1000.0, 0, 'f', 1)
                                 .arg(kDesktop)
                                 .arg(m_options.maxSessions)
                                 .arg(m_options.pageSize);
}

HistoryViewService::~HistoryViewService()
{
    // A load may still be running on the SqlManager thread; its result is not delivered
    // after this (a result already queued for this object is removed with it).  The
    // SqlManager state of our sessions (newest id, anchor) is released as well while the
    // application object still exists (not during static destruction at exit).
    if (m_sql) {
        disconnect(m_sql, nullptr, this, nullptr);
        if (QCoreApplication::instance()) {
            for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it)
                m_sql->releaseHistorySession(it.key());
        }
    }
}

QStringList HistoryViewService::sessionIds() const
{
    QStringList ids = m_sessions.keys();
    ids.sort();
    return ids;
}

void HistoryViewService::handleRequest(const QString &sessionId, double fromMs, double toMs, int page)
{
    if (!HistoryExport::isValidSessionId(sessionId)) {
        qWarning().noquote() << QStringLiteral("[History] view request ignored: invalid session id \"%1\"")
                                        .arg(sessionId.left(64));
        return;
    }
    qint64 fromSec = 0;
    qint64 toSec = 0;
    if (!HistoryExport::rangeMsToSecs(fromMs, toMs, &fromSec, &toSec)) {
        qWarning().noquote() << QStringLiteral("[History] %1 view request ignored: invalid range %2 .. %3 ms")
                                        .arg(sessionId, msText(fromMs), msText(toMs));
        return;
    }
    if (page < 1) {
        qWarning().noquote() << QStringLiteral("[History] %1 view request ignored: page %2 < 1").arg(sessionId).arg(page);
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
            qWarning().noquote() << QStringLiteral("[History] %1 view request ignored: %2 entries (limit %3) and none "
                                                   "can be removed").arg(sessionId).arg(m_sessions.size())
                                            .arg(m_options.maxSessions);
            return;
        }
        it = m_sessions.insert(sessionId, Session{});
    }

    Session &session = it.value();
    session.lastRequestMs = m_clock.elapsed();
    session.lastUse = ++m_useCounter;
    session.fromMs = fromMs;
    session.toMs = toMs;
    session.requestedPage = page;
    session.clampRequest = false;
    session.reason = created ? "new view" : "request";
    post(sessionId, session);
}

void HistoryViewService::post(const QString &sessionId, Session &session)
{
    qint64 fromSec = 0;
    qint64 toSec = 0;
    HistoryExport::rangeMsToSecs(session.fromMs, session.toMs, &fromSec, &toSec);   // validated before
    session.latestRequestId = ++m_nextRequestId;
    session.clock.start();
    if (m_sql) {
        m_sql->requestSensorHistoryRangePage(sessionId, session.latestRequestId, fromSec, toSec,
                                             session.requestedPage, m_options.pageSize);
    }
    qInfo().noquote()
            << QStringLiteral("[History] %1 request #%2 (%3) page %4, range %5 .. %6 ms (%7 .. %8 s), "
                              "%9 session(s), posted in %10 us.")
                       .arg(sessionId)
                       .arg(session.latestRequestId)
                       .arg(QString::fromLatin1(session.reason))
                       .arg(session.requestedPage)
                       .arg(msText(session.fromMs), msText(session.toMs))
                       .arg(fromSec)
                       .arg(toSec)
                       .arg(m_sessions.size())
                       .arg(session.clock.nsecsElapsed() / 1000);
}

void HistoryViewService::onPageReady(const SensorHistoryPageResult &result)
{
    if (result.sessionKey.isEmpty())
        return;                                  // not a view request
    const auto it = m_sessions.find(result.sessionKey);
    if (it == m_sessions.end()) {
        qInfo().noquote() << QStringLiteral("[History] %1 result #%2 dropped: no such session here (removed, or not created by this service).")
                                     .arg(result.sessionKey).arg(result.requestId);
        return;
    }
    Session &session = it.value();
    if (result.requestId != session.latestRequestId) {
        qInfo().noquote() << QStringLiteral("[History] %1 result #%2 (page %3) dropped: stale, newest request of this "
                                            "session is #%4%5.")
                                     .arg(result.sessionKey)
                                     .arg(result.requestId)
                                     .arg(result.page)
                                     .arg(session.latestRequestId)
                                     .arg(result.superseded ? QStringLiteral(" (not executed by SqlManager)") : QString());
        return;
    }
    if (result.superseded) {
        // Only when SqlManager lost the session (released) while this id is still ours;
        // cannot happen through this class, logged for completeness.
        qWarning().noquote() << QStringLiteral("[History] %1 result #%2 superseded in SqlManager; nothing written.")
                                        .arg(result.sessionKey).arg(result.requestId);
        return;
    }
    const double roundTripMs = session.clock.nsecsElapsed() / 1.0e6;
    const QString sessionId = result.sessionKey;

    if (!result.countOk) {
        qWarning().noquote() << QStringLiteral("[SQL] %1: failed to count sensor-history rows: %2")
                                        .arg(sessionId, result.errorMessage);
        writeEntry(sessionId, session, 1, 1, 0, QVariantList{});
        return;
    }

    const qint64 pageSize = m_options.pageSize;
    const qint64 totalRows = result.totalRows;
    const int totalPages = totalRows > 0 ? static_cast<int>((totalRows + pageSize - 1) / pageSize) : 1;
    if (result.page > totalPages) {
        if (!session.clampRequest) {
            qInfo().noquote() << QStringLiteral("[History] %1 #%2 page %3 is past the last page %4; requesting page %4.")
                                         .arg(sessionId).arg(result.requestId).arg(result.page).arg(totalPages);
            session.requestedPage = totalPages;
            session.clampRequest = true;
            session.reason = "page past the end";
            post(sessionId, session);
            return;
        }
        // Rows are only ever added, so this does not happen; never loop.
        writeEntry(sessionId, session, totalPages, totalPages, totalRows, QVariantList{});
        return;
    }
    if (!result.ok) {
        qWarning().noquote() << QStringLiteral("[SQL] %1: failed to load sensor-history page: %2")
                                        .arg(sessionId, result.errorMessage);
        writeEntry(sessionId, session, result.page, totalPages, totalRows, QVariantList{});
        return;
    }

    const QVariantList records = recordsFromSamples(result.samples);
    qInfo().noquote()
            << QStringLiteral("[History] %1 #%2 applied: page %3/%4, %5 record(s), total %6 row(s) in %7 month file(s) "
                              "(%8 count(s) from cache, %9 updated by new rows); SqlManager thread count %10 ms + "
                              "page %11 ms in %12 step(s) (longest %13 ms), page read %14; request-to-apply %15 ms.")
                       .arg(sessionId)
                       .arg(result.requestId)
                       .arg(result.page)
                       .arg(totalPages)
                       .arg(records.size())
                       .arg(totalRows)
                       .arg(result.months)
                       .arg(result.countCacheHits)
                       .arg(result.countCacheDeltas)
                       .arg(result.countMs, 0, 'f', 2)
                       .arg(result.pageMs, 0, 'f', 2)
                       .arg(result.steps)
                       .arg(result.maxStepMs, 0, 'f', 2)
                       .arg(result.pageMethod)
                       .arg(roundTripMs, 0, 'f', 2);
    writeEntry(sessionId, session, result.page, totalPages, totalRows, records);
}

void HistoryViewService::writeEntry(const QString &sessionId, const Session &session, int page, int totalPages,
                                    qint64 totalRows, const QVariantList &records)
{
    QVariantMap entry{
        {QStringLiteral("fromMs"), session.fromMs},
        {QStringLiteral("toMs"), session.toMs},
        {QStringLiteral("page"), page},
        {QStringLiteral("totalPages"), std::max(1, totalPages)},
        {QStringLiteral("totalRows"), totalRows},
        {QStringLiteral("records"), records},
    };
    QVariantMap previous = m_views.value(sessionId).toMap();
    const QVariant previousRevision = previous.take(QStringLiteral("revision"));
    if (previousRevision.isValid() && previous == entry) {
        qInfo().noquote() << QStringLiteral("[History] %1 entry unchanged (page %2/%3, revision %4); historyViews not written.")
                                     .arg(sessionId).arg(page).arg(totalPages).arg(previousRevision.toLongLong());
        return;
    }
    entry.insert(QStringLiteral("revision"), ++m_revision);
    m_views.insert(sessionId, entry);
    if (m_proxy)
        m_proxy->setHistoryViews(m_views);
    qInfo().noquote() << QStringLiteral("[History] %1 entry written: page %2/%3, %4 record(s), total %5 row(s), "
                                        "revision %6; historyViews has %7 entr%8.")
                                 .arg(sessionId)
                                 .arg(page)
                                 .arg(totalPages)
                                 .arg(records.size())
                                 .arg(totalRows)
                                 .arg(m_revision)
                                 .arg(m_views.size())
                                 .arg(m_views.size() == 1 ? QStringLiteral("y") : QStringLiteral("ies"));
}

bool HistoryViewService::removeSessions(const QStringList &ids, const char *why)
{
    bool changed = false;
    for (const QString &id : ids) {
        m_sessions.remove(id);
        if (m_sql)
            m_sql->releaseHistorySession(id);
        changed = m_views.remove(id) > 0 || changed;
    }
    if (changed && m_proxy)
        m_proxy->setHistoryViews(m_views);      // once for all removed entries
    qInfo().noquote() << QStringLiteral("[History] removed %1 session(s) (%2): %3; %4 session(s) left, historyViews %5.")
                                 .arg(ids.size())
                                 .arg(QString::fromLatin1(why))
                                 .arg(ids.join(QStringLiteral(", ")))
                                 .arg(m_sessions.size())
                                 .arg(changed ? QStringLiteral("written once") : QStringLiteral("unchanged"));
    return changed;
}

void HistoryViewService::sweepIdle()
{
    const qint64 now = m_clock.elapsed();
    QStringList idle;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if (it.key() != kDesktop && now - it->lastRequestMs >= m_options.idleTimeoutMs)
            idle << it.key();
    }
    if (idle.isEmpty())
        return;
    idle.sort();
    removeSessions(idle, "idle");
}
