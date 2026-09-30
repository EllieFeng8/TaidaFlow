// w2-080 (per-client alarm views): SqlManager::requestAlarmRangePage / releaseAlarmSession.
// Kept in its own file (members of SqlManager, declared in SqlManager.h) so that the existing
// SqlManager.cpp only gained the two alarmHistoryChanged emits; see SqlManager.h for the contract.
//
// Threading: requestAlarmRangePage() is called on any thread (the main thread in the app) and
// only queues the first step; every step runs on the SqlManager thread (the thread that owns the
// SQLite connections), one queued call per step, so the blocking calls of other threads (the main
// thread's saveSensorData every second, insertAlarm, ...) run between the steps.  A step reads at
// most kAlarmChunkRows rows of one month file (keyset on occurrence_time, id), so a step stays
// short also for a month with many alarms.  The result is emitted on the SqlManager thread.
#include "SqlManager.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QSqlError>
#include <QSqlQuery>
#include <QtDebug>

#include <algorithm>

namespace
{
    constexpr int kAlarmChunkRows = 2000;   // rows read per step at most

    double alarmMsSince(const QElapsedTimer& timer)
    {
        return timer.nsecsElapsed() / 1.0e6;
    }
}

struct SqlManager::AlarmRangeJob
{
    QString sessionKey;
    quint64 requestId = 0;
    qint64 from = 0;
    qint64 to = 0;
    int page = 1;                     // requested page (>= 1)
    int pageSize = 1;
    AlarmRowPredicate isActive = nullptr;
    bool listed = false;              // month files listed (first step)
    QList<SensorMonthFile> months;    // newest first
    int index = 0;                    // month file being read
    bool hasCursor = false;           // keyset inside months[index]: last row read
    qint64 cursorTs = 0;
    qint64 cursorId = 0;
    qint64 position = 0;              // rows seen so far (0-based position of the next row)
    qint64 active = 0;
    QList<AlarmRangeRow> pageRows;    // rows of the requested page
    QList<AlarmRangeRow> lastBucket;  // rows of the newest complete-or-partial bucket (= last page at the end)
    AlarmRangePageResult result;
    QElapsedTimer wall;               // started when the request was posted
};

void SqlManager::requestAlarmRangePage(const QString& sessionKey, quint64 requestId, qint64 from, qint64 to,
                                       int page, int pageSize, AlarmRowPredicate isActive)
{
    if (m_shutDown.load())
    {
        qWarning().noquote() << "[SQL] alarm range request after SqlManager shutdown ignored.";
        return;
    }
    qRegisterMetaType<AlarmRangePageResult>("AlarmRangePageResult");
    {
        // Recorded before the job is queued, so an older job of the same session still waiting
        // in the queue is dropped at its first step.
        QMutexLocker locker(&m_alarmSessionMutex);
        quint64& latest = m_alarmSessionLatestIds[sessionKey];
        if (latest < requestId)
            latest = requestId;
    }

    auto job = std::make_shared<AlarmRangeJob>();
    job->sessionKey = sessionKey;
    job->requestId = requestId;
    job->from = from;
    job->to = to;
    job->page = std::max(1, page);
    job->pageSize = std::max(1, pageSize);
    job->isActive = isActive;
    job->result.sessionKey = sessionKey;
    job->result.requestId = requestId;
    job->result.from = from;
    job->result.to = to;
    job->result.requestedPage = job->page;
    job->result.page = job->page;
    job->result.pageSize = job->pageSize;
    job->wall.start();

    // Queued, never blocking.  'this' is the context object: nothing runs once SqlManager is
    // destroyed, and nothing after shutdown() (the thread's event loop has ended).
    QMetaObject::invokeMethod(this, [this, job]() { runAlarmRangeStep(job); }, Qt::QueuedConnection);
}

void SqlManager::releaseAlarmSession(const QString& sessionKey)
{
    QMutexLocker locker(&m_alarmSessionMutex);
    m_alarmSessionLatestIds.remove(sessionKey);
}

void SqlManager::runAlarmRangeStep(const std::shared_ptr<AlarmRangeJob>& job)
{
    QElapsedTimer step;
    step.start();
    AlarmRangePageResult& result = job->result;

    const auto finish = [&](bool ok, const QString& error) {
        const double stepMs = alarmMsSince(step);
        ++result.steps;
        result.maxStepMs = std::max(result.maxStepMs, stepMs);
        result.ok = ok;
        result.errorMessage = error;
        result.totalMs = alarmMsSince(job->wall);
        emit alarmRangePageReady(result);
    };

    {
        QMutexLocker locker(&m_alarmSessionMutex);
        const auto it = m_alarmSessionLatestIds.constFind(job->sessionKey);
        if (it == m_alarmSessionLatestIds.cend() || it.value() != job->requestId)
        {
            locker.unlock();
            result.superseded = true;
            finish(false, QString());
            return;
        }
    }

    if (!job->listed)
    {
        // Directory listing: only the month files that exist and intersect [from, to].
        job->months = sensorMonthFilesInRange(job->from, job->to);
        job->listed = true;
        result.months = job->months.size();
    }
    else if (job->index < job->months.size())
    {
        const SensorMonthFile& month = job->months.at(job->index);
        // Read only: never create a month file (openDataDb would) or a table.
        if (!QFileInfo::exists(dataFileForKey(month.key)))
        {
            ++job->index;
            job->hasCursor = false;
            QMetaObject::invokeMethod(this, [this, job]() { runAlarmRangeStep(job); }, Qt::QueuedConnection);
            return;
        }
        QSqlDatabase db = openDataDb(month.key);
        if (!db.isValid() || !db.isOpen())
        {
            finish(false, QStringLiteral("db open failed (%1)").arg(month.key));
            return;
        }
        QSqlQuery query(db);
        query.setForwardOnly(true);
        QString sql = QStringLiteral("SELECT id, occurrence_time, reason FROM alarm_history "
                                     "WHERE occurrence_time >= :from AND occurrence_time <= :to");
        if (job->hasCursor)
            sql += QStringLiteral(" AND (occurrence_time < :cts OR (occurrence_time = :cts2 AND id < :cid))");
        sql += QStringLiteral(" ORDER BY occurrence_time DESC, id DESC LIMIT :limit");
        query.prepare(sql);
        query.bindValue(QStringLiteral(":from"), std::max(job->from, month.monthFrom));
        query.bindValue(QStringLiteral(":to"), std::min(job->to, month.monthTo));
        if (job->hasCursor)
        {
            query.bindValue(QStringLiteral(":cts"), job->cursorTs);
            query.bindValue(QStringLiteral(":cts2"), job->cursorTs);
            query.bindValue(QStringLiteral(":cid"), job->cursorId);
        }
        query.bindValue(QStringLiteral(":limit"), kAlarmChunkRows);
        if (!query.exec())
        {
            const QString error = query.lastError().text();
            if (!error.contains(QLatin1String("no such table"), Qt::CaseInsensitive))
            {
                finish(false, QStringLiteral("%1 (%2)").arg(error, month.key));
                return;
            }
            // A month file without alarm_history (never had an alarm): 0 rows.
            ++job->index;
            job->hasCursor = false;
        }
        else
        {
            const qint64 pageFirst = static_cast<qint64>(job->page - 1) * job->pageSize;
            const qint64 pageEnd = pageFirst + job->pageSize;
            int read = 0;
            while (query.next())
            {
                AlarmRangeRow row;
                row.monthKey = month.key;
                row.id = query.value(0).toLongLong();
                row.occurrenceTime = query.value(1).toLongLong();
                row.reason = query.value(2).toString();
                if (job->isActive && job->isActive(row.reason))
                    ++job->active;
                if (job->position % job->pageSize == 0)
                    job->lastBucket.clear();
                job->lastBucket.append(row);
                if (job->position >= pageFirst && job->position < pageEnd)
                    job->pageRows.append(row);
                ++job->position;
                job->hasCursor = true;
                job->cursorTs = row.occurrenceTime;
                job->cursorId = row.id;
                ++read;
            }
            if (query.lastError().isValid())
            {
                finish(false, QStringLiteral("%1 (%2)").arg(query.lastError().text(), month.key));
                return;
            }
            if (read < kAlarmChunkRows)
            {
                // This month is done; the next step reads the next (older) month file.
                ++job->index;
                job->hasCursor = false;
            }
        }
    }

    if (job->listed && job->index >= job->months.size())
    {
        result.totalCount = job->position;
        result.activeCount = job->active;
        const qint64 totalPages = std::max<qint64>(1, (job->position + job->pageSize - 1) / job->pageSize);
        if (job->page > totalPages)
        {
            result.page = static_cast<int>(totalPages);
            result.rows = job->position > 0 ? job->lastBucket : QList<AlarmRangeRow>();
        }
        else
        {
            result.page = job->page;
            result.rows = job->pageRows;
        }
        finish(true, QString());
        return;
    }

    const double stepMs = alarmMsSince(step);
    ++result.steps;
    result.maxStepMs = std::max(result.maxStepMs, stepMs);
    // Behind everything already queued (e.g. a blocking save from the main thread).
    QMetaObject::invokeMethod(this, [this, job]() { runAlarmRangeStep(job); }, Qt::QueuedConnection);
}
