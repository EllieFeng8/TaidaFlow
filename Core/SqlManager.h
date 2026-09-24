#pragma once
#include <QObject>
#include <QDate>
#include <QDateTime>
#include <QSqlDatabase>
#include <QVector>
#include <QStringList>
#include <QVariantList>
#include <QJsonArray>
#include <QJsonObject>
#include <memory>
#include <QThread>
#include <type_traits>
#include <QString>
#include <QtGlobal>
#include <QHash>
#include <QList>
#include <atomic>

// w2-039: result of one History-page load (one COUNT + one newest-first page),
// produced on the SqlManager thread and delivered queued to the requester.
struct SensorHistoryPageResult
{
    quint64 requestId = 0;
    int page = 0;              // 1-based, page 1 = newest rows
    int pageSize = 0;
    qint64 totalRows = 0;      // rows of sensor_data in [from, to]
    QJsonArray samples;        // newest first; same object layout as queryRangeJsonPaged
    bool ok = false;
    bool countOk = false;
    bool superseded = false;   // skipped: a newer request was already queued
    QString errorMessage;
    double countMs = 0.0;      // measured on the SqlManager thread
    double pageMs = 0.0;
    // w2-041 (requestSensorHistoryRangePage only): month files in the range and
    // how many of their COUNTs came from the count cache.
    int months = 0;
    int countCacheHits = 0;
};
Q_DECLARE_METATYPE(SensorHistoryPageResult)

class SqlManager : public QObject
{
    Q_OBJECT

public:
    static SqlManager* instance();

    ~SqlManager();

    void setDataDirectory(const QString& path);
    void setSettingsFile(const QString& filePath);

    bool initialize();

    bool saveSensorData(const QDateTime& timestamp, const QVector<double>& readings, const QVector<quint16>& holdings = {});
    QVector<QVariantList> fetchSensorData(const QDate& date, int limit = 10);
    QVector<QVariantList> fetchHoldingRegisters(const QDate& date, int limit = 10);
    bool setSensorName(int index, const QString& name);
    QString sensorName(int index);
    QStringList sensorNames();
    QString dataFilePathForMonth(const QDate& date) const;

    // JSON helpers for REST layer
    QJsonArray getSensorMapJson() const;
    bool updateSensorMapJson(const QJsonArray& arr, QString* errMsg = nullptr);
    bool insertOneSampleJson(const QJsonObject& obj, QString* errMsg = nullptr);
    bool insertBatchSamplesJson(const QJsonArray& items, int* inserted = nullptr, QString* errMsg = nullptr);
    bool queryRangeJson(qint64 from, qint64 to, QJsonArray* out, QString* errMsg = nullptr);
    bool queryRangeJsonPaged(qint64 from, qint64 to, int page, int pageSize, QJsonArray* out, QString* errMsg = nullptr);
    bool queryHoldingRangeJson(qint64 from, qint64 to, QJsonArray* out, QString* errMsg = nullptr);
    bool queryHoldingRangeJsonPaged(qint64 from, qint64 to, int page, int pageSize, QJsonArray* out, QString* errMsg = nullptr);
    bool countSensorRange(qint64 from, qint64 to, qint64* total, QString* errMsg = nullptr);
    bool countHoldingRange(qint64 from, qint64 to, qint64* total, QString* errMsg = nullptr);
    int readFrequency(QString* errMsg = nullptr) const;
    bool setReadFrequency(int value, QString* errMsg = nullptr);

    // Alarm history helpers
    // insertedId (optional) receives the new row id.  Ids are per monthly data
    // file, so a row is identified by (occurrence month, id).
    bool insertAlarm(const QDateTime& occurrence, const QString& reason, QString* errMsg = nullptr,
                     qint64* insertedId = nullptr);
    bool insertAlarm(const QString& reason, QString* errMsg = nullptr);
    // Replaces the reason of row 'id' in the data file of 'occurrence's month.
    // Fails when no such row exists.
    bool updateAlarmReason(const QDateTime& occurrence, qint64 id, const QString& reason,
                           QString* errMsg = nullptr);
    bool getAlarmHistory(qint64 from, qint64 to, QJsonArray* out, QString* errMsg = nullptr);

    // w2-039 History page (newest first).  [from, to] must lie in one calendar
    // month (one data file); no COUNT is done here.  Rows: ORDER BY timestamp
    // DESC LIMIT pageSize OFFSET (page-1)*pageSize.  Blocking like the other
    // public functions when called from another thread.
    bool querySensorRangeDescPaged(qint64 from, qint64 to, int page, int pageSize,
                                   QJsonArray* out, QString* errMsg = nullptr);

    // w2-039 asynchronous History load: returns immediately (never blocks the
    // caller).  On the SqlManager thread it runs one COUNT for [from, to] and
    // one querySensorRangeDescPaged(), then emits sensorHistoryPageReady().
    // Connect to it with a queued connection.  A request whose id is lower
    // than the newest requested id when it starts is not executed and is
    // reported with superseded = true.
    void requestSensorHistoryPage(quint64 requestId, qint64 from, qint64 to,
                                  int page, int pageSize);

    // w2-041 History range (spec §2): like requestSensorHistoryPage(), but
    // [from, to] (epoch seconds, both inclusive) may span any number of months,
    // including the "unbounded" range.  Only month files that exist in the data
    // directory are visited (newest month first); totalRows is the sum of the
    // per-month COUNTs and the page is taken newest first across months
    // (ORDER BY timestamp DESC, rowid DESC inside each month).  Per-month COUNTs
    // are cached and reused while the month file is unchanged (same size and
    // same SQLite file change counter).  Returns immediately; the result comes
    // through sensorHistoryPageReady() and shares the request-id sequence
    // (superseded handling) with requestSensorHistoryPage().
    void requestSensorHistoryRangePage(quint64 requestId, qint64 from, qint64 to,
                                       int page, int pageSize);

    // w2-041 export: absolute paths of the existing sensor_YYYYMM.sqlite files
    // whose month intersects [from, to] (epoch seconds), newest month first.
    // Only lists files (no query).  Blocking like the other public functions
    // when called from another thread.
    QStringList sensorDataFilesInRange(qint64 from, qint64 to);

signals:
    void sensorHistoryPageReady(const SensorHistoryPageResult& result);

private:
    explicit SqlManager(QObject* parent = nullptr);
    void startWorkerThread();

    template <typename F>
    auto runOnThread(F&& func) const -> decltype(func());

    QString m_dataDir;
    QString m_settingsPath;
    QStringList m_dataConnectionNames;
    QString m_settingsSchemaPath;
    QString m_dataSchemaPath;
    QThread* m_thread;
    bool m_threadStarted;
    std::atomic<quint64> m_latestHistoryRequestId{0};

    // w2-041 (SqlManager thread only)
    struct SensorMonthFile
    {
        QString key;          // yyyyMM
        qint64 monthFrom = 0; // first second of the month (local time)
        qint64 monthTo = 0;   // last second of the month
    };
    struct RangeCountCacheEntry
    {
        qint64 from = 0;
        qint64 to = 0;
        qint64 fileSize = -1;
        quint32 changeCounter = 0;
        qint64 count = 0;
    };
    QHash<QString, RangeCountCacheEntry> m_rangeCountCache;   // key = yyyyMM
    QList<SensorMonthFile> sensorMonthFilesInRange(qint64 from, qint64 to) const;
    bool countSensorMonthCached(const SensorMonthFile& month, qint64 from, qint64 to,
                                qint64* count, bool* cacheHit, QString* errMsg);
    bool querySensorDescOffset(const QString& key, qint64 from, qint64 to, qint64 offset,
                               int limit, QJsonArray* out, QString* errMsg);

    QString monthKey(const QDate& date) const;
    QString dataFileForKey(const QString& key) const;
    QString dataConnectionName(const QString& key) const;
    bool ensureDirExists(const QString& dir) const;

    bool ensureSettingsDb();
    bool ensureDataSchema(QSqlDatabase& db) const;
    QSqlDatabase openDataDb(const QString& monthKey);
    bool executeSqlFile(const QString& path, QSqlDatabase& db) const;

    static SqlManager* s_instance;
};
