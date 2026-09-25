#include "SqlManager.h"

#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QtDebug>
#include <QThread>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QFileInfo>
#include <QFile>
#include <QTextStream>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <algorithm>
#include <limits>

SqlManager* SqlManager::s_instance = nullptr;

namespace
{
    constexpr int kSensorCount = 40;
    constexpr int kHoldingCount = 100;
    const char* kSettingsConnection = "settings";
}

int SqlManager::readFrequency(QString* errMsg) const
{
    const SqlManager* selfConst = this;
    SqlManager* self = const_cast<SqlManager*>(selfConst);
    return self->runOnThread([self, errMsg]() {
        if (!self->ensureSettingsDb())
        {
            if (errMsg) *errMsg = "settings db not available";
            return -1;
        }
        QSqlDatabase db = QSqlDatabase::database(kSettingsConnection);
        QSqlQuery query(db);
        query.prepare("SELECT value FROM app_settings WHERE key = 'read_frequency'");
        if (!query.exec() || !query.next())
        {
            if (errMsg) *errMsg = query.lastError().text();
            return -1;
        }
        bool ok = false;
        int v = query.value(0).toInt(&ok);
        if (!ok)
        {
            if (errMsg) *errMsg = "invalid value";
            return -1;
        }
        return v;
    });
}

bool SqlManager::setReadFrequency(int value, QString* errMsg)
{
    return runOnThread([this, value, errMsg]() {
        if (value <= 0)
        {
            if (errMsg) *errMsg = "value must be positive";
            return false;
        }
        if (!ensureSettingsDb())
        {
            if (errMsg) *errMsg = "settings db not available";
            return false;
        }
        QSqlDatabase db = QSqlDatabase::database(kSettingsConnection);
        QSqlQuery query(db);
        query.prepare("REPLACE INTO app_settings (key, value) VALUES ('read_frequency', :val)");
        query.bindValue(":val", value);
        if (!query.exec())
        {
            if (errMsg) *errMsg = query.lastError().text();
            return false;
        }
        return true;
    });
}

bool SqlManager::queryHoldingRangeJson(qint64 from, qint64 to, QJsonArray* out, QString* errMsg)
{
    return queryHoldingRangeJsonPaged(from, to, 1, std::numeric_limits<int>::max(), out, errMsg);
}

bool SqlManager::queryHoldingRangeJsonPaged(qint64 from, qint64 to, int page, int pageSize, QJsonArray* out, QString* errMsg)
{
    return runOnThread([this, from, to, page, pageSize, out, errMsg]() {
        if (!out)
        {
            if (errMsg) *errMsg = "output array is null";
            return false;
        }
        *out = QJsonArray();
        if (to < from)
        {
            if (errMsg) *errMsg = "to < from";
            return false;
        }
        if (page <= 0 || pageSize <= 0)
        {
            if (errMsg) *errMsg = "page and pageSize must be positive";
            return false;
        }

        QDate startDate = QDateTime::fromSecsSinceEpoch(from).date();
        QDate endDate = QDateTime::fromSecsSinceEpoch(to).date();

        QDate iter = QDate(startDate.year(), startDate.month(), 1);
        QDate endIter = QDate(endDate.year(), endDate.month(), 1);

        qint64 skipRemaining = static_cast<qint64>(page - 1) * static_cast<qint64>(pageSize);
        int takeRemaining = pageSize;

        while (iter <= endIter)
        {
            if (takeRemaining <= 0)
            {
                break;
            }

            QString key = monthKey(iter);
            QString filePath = dataFileForKey(key);
            QFileInfo fi(filePath);
            if (!fi.exists())
            {
                iter = iter.addMonths(1);
                continue;
            }

            QSqlDatabase db = openDataDb(key);
            if (!db.isValid() || !db.isOpen())
            {
                if (errMsg) *errMsg = "db open failed";
                return false;
            }
            if (!ensureDataSchema(db))
            {
                if (errMsg) *errMsg = "ensure schema failed";
                return false;
            }

            QSqlQuery countQuery(db);
            countQuery.prepare("SELECT COUNT(1) FROM holding_register WHERE timestamp >= :from AND timestamp <= :to");
            countQuery.bindValue(":from", from);
            countQuery.bindValue(":to", to);
            if (!countQuery.exec() || !countQuery.next())
            {
                if (errMsg) *errMsg = countQuery.lastError().text();
                return false;
            }

            qint64 monthCount = countQuery.value(0).toLongLong();
            if (monthCount <= 0)
            {
                iter = iter.addMonths(1);
                continue;
            }

            if (skipRemaining >= monthCount)
            {
                skipRemaining -= monthCount;
                iter = iter.addMonths(1);
                continue;
            }

            QStringList columns;
            columns << "timestamp";
            for (int i = 0; i < kHoldingCount; ++i)
            {
                columns << QString("h%1").arg(i + 1);
            }

            int localOffset = static_cast<int>(skipRemaining);
            skipRemaining = 0;

            QSqlQuery query(db);
            query.prepare(QString("SELECT %1 FROM holding_register WHERE timestamp >= :from AND timestamp <= :to ORDER BY timestamp LIMIT :limit OFFSET :offset")
                              .arg(columns.join(", ")));
            query.bindValue(":from", from);
            query.bindValue(":to", to);
            query.bindValue(":limit", takeRemaining);
            query.bindValue(":offset", localOffset);
            if (!query.exec())
            {
                if (errMsg) *errMsg = query.lastError().text();
                return false;
            }

            while (query.next())
            {
                QJsonObject obj;
                obj.insert("ts", query.value(0).toLongLong());
                for (int i = 0; i < kHoldingCount; ++i)
                {
                    obj.insert(QString("h%1").arg(i + 1), QJsonValue::fromVariant(query.value(i + 1)));
                }
                out->append(obj);
                --takeRemaining;
                if (takeRemaining <= 0)
                {
                    break;
                }
            }

            iter = iter.addMonths(1);
        }
        return true;
    });
}

bool SqlManager::executeSqlFile(const QString& path, QSqlDatabase& db) const
{
    QFile file(path);
    if (!file.exists())
    {
        qWarning() << "Schema file not found" << path;
        return false;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qWarning() << "Cannot open schema file" << path << file.errorString();
        return false;
    }

    QTextStream stream(&file);
    QString content = stream.readAll();
    file.close();

    const QStringList statements = content.split(';', Qt::SkipEmptyParts);
    for (const QString& rawStmt : statements)
    {
        QString stmt = rawStmt.trimmed();
        if (stmt.isEmpty())
            continue;
        QSqlQuery query(db);
        if (!query.exec(stmt))
        {
            qWarning() << "Failed to exec schema statement" << stmt << query.lastError().text();
            return false;
        }
    }
    return true;
}

SqlManager* SqlManager::instance()
{
    static QMutex mutex;
    QMutexLocker locker(&mutex);
    if (!s_instance)
    {
        s_instance = new SqlManager(nullptr);
        s_instance->startWorkerThread();
    }
    return s_instance;
}

SqlManager::SqlManager(QObject* parent)
    : QObject(parent)
    , m_dataDir(QDir::currentPath() + "/data")
    , m_settingsPath(QDir::currentPath() + "/settings.sqlite")
    , m_settingsSchemaPath(QDir::currentPath() + "/settings_schema.sql")
    , m_dataSchemaPath(QDir::currentPath() + "/data_schema.sql")
    , m_thread(nullptr)
    , m_threadStarted(false)
{
    qRegisterMetaType<SensorHistoryPageResult>("SensorHistoryPageResult");
}

SqlManager::~SqlManager()
{
    if (m_thread && m_thread->isRunning())
    {
        m_thread->quit();
        m_thread->wait();
    }
    for (const QString& name : m_dataConnectionNames)
    {
        if (QSqlDatabase::contains(name))
        {
            {
                QSqlDatabase db = QSqlDatabase::database(name);
                if (db.isValid())
                {
                    db.close();
                }
            }
            QSqlDatabase::removeDatabase(name);
        }
    }

    if (QSqlDatabase::contains(kSettingsConnection))
    {
        {
            QSqlDatabase db = QSqlDatabase::database(kSettingsConnection);
            if (db.isValid())
            {
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(kSettingsConnection);
    }
}

void SqlManager::setDataDirectory(const QString& path)
{
    runOnThread([this, path]() {
        if (!path.isEmpty())
        {
            m_dataDir = path;
            // w2-045: History counts and the page anchor belong to the old files.
            m_rangeCountCache.clear();
            m_historyAnchor = HistoryAnchor{};
            m_historySchemaChecked.clear();
        }
    });
}

void SqlManager::setSettingsFile(const QString& filePath)
{
    runOnThread([this, filePath]() {
        if (!filePath.isEmpty())
        {
            m_settingsPath = filePath;
        }
    });
}

bool SqlManager::initialize()
{
    return runOnThread([this]() {
        if (!ensureDirExists(m_dataDir))
        {
            qWarning() << "Failed to create data directory" << m_dataDir;
            return false;
        }

        if (!ensureSettingsDb())
        {
            qWarning() << "Failed to initialize settings database" << m_settingsPath;
            return false;
        }

        return true;
    });
}

bool SqlManager::saveSensorData(const QDateTime& timestamp, const QVector<double>& readings, const QVector<quint16>& holdings)
{
    return runOnThread([this, timestamp, readings, holdings]() {
        QString key = monthKey(timestamp.date());
        QSqlDatabase db = openDataDb(key);
        if (!db.isValid() || !db.isOpen())
        {
            qWarning() << "Data database is not open for key" << key;
            return false;
        }

        if (!ensureDataSchema(db))
        {
            return false;
        }

        QSqlQuery query(db);
        QStringList placeholders;
        QStringList columns;
        columns << "timestamp";
        placeholders << ":ts";
        for (int i = 0; i < kSensorCount; ++i)
        {
            columns << QString("s%1").arg(i + 1);
            placeholders << QString(":s%1").arg(i + 1);
        }

        query.prepare(QString("INSERT INTO sensor_data (%1) VALUES (%2)")
                          .arg(columns.join(", "))
                          .arg(placeholders.join(", ")));

        query.bindValue(":ts", timestamp.toSecsSinceEpoch());
        for (int i = 0; i < kSensorCount; ++i)
        {
            QString placeholder = QString(":s%1").arg(i + 1);
            if (i < readings.size())
            {
                query.bindValue(placeholder, readings.at(i));
            }
            else
            {
                query.bindValue(placeholder, QVariant());
            }
        }

        if (!query.exec())
        {
            qWarning() << "Failed to insert sensor data:" << query.lastError().text();
            return false;
        }

        // insert holding registers if provided
        if (!holdings.isEmpty())
        {
            QStringList hCols;
            QStringList hPlaceholders;
            hCols << "timestamp";
            hPlaceholders << ":ts";
            for (int i = 0; i < kHoldingCount; ++i)
            {
                hCols << QString("h%1").arg(i + 1);
                hPlaceholders << QString(":h%1").arg(i + 1);
            }

            QSqlQuery hQuery(db);
            hQuery.prepare(QString("INSERT INTO holding_register (%1) VALUES (%2)")
                               .arg(hCols.join(", "))
                               .arg(hPlaceholders.join(", ")));
            hQuery.bindValue(":ts", timestamp.toSecsSinceEpoch());
            for (int i = 0; i < kHoldingCount; ++i)
            {
                QString ph = QString(":h%1").arg(i + 1);
                if (i < holdings.size())
                {
                    hQuery.bindValue(ph, static_cast<int>(holdings.at(i)));
                }
                else
                {
                    hQuery.bindValue(ph, QVariant());
                }
            }

            if (!hQuery.exec())
            {
                qWarning() << "Failed to insert holding registers:" << hQuery.lastError().text();
                return false;
            }
        }

        return true;
    });
}

bool SqlManager::setSensorName(int index, const QString& name)
{
    return runOnThread([this, index, name]() {
        if (index < 1 || index > kSensorCount)
        {
            return false;
        }

        if (!ensureSettingsDb())
        {
            return false;
        }

        QSqlDatabase db = QSqlDatabase::database(kSettingsConnection);
        QSqlQuery query(db);
        query.prepare("REPLACE INTO sensor_config (sensor_key, sensor_name) VALUES (:key, :name)");
        query.bindValue(":key", QString("s%1").arg(index));
        query.bindValue(":name", name);
        if (!query.exec())
        {
            qWarning() << "Failed to set sensor name:" << query.lastError().text();
            return false;
        }

        return true;
    });
}

QString SqlManager::sensorName(int index)
{
    return runOnThread([this, index]() {
        if (index < 1 || index > kSensorCount)
        {
            return QString();
        }

        if (!ensureSettingsDb())
        {
            return QString();
        }

        QSqlDatabase db = QSqlDatabase::database(kSettingsConnection);
        QSqlQuery query(db);
        query.prepare("SELECT sensor_name FROM sensor_config WHERE sensor_key = :key");
        query.bindValue(":key", QString("s%1").arg(index));
        if (query.exec() && query.next())
        {
            return query.value(0).toString();
        }

        return QString();
    });
}

QStringList SqlManager::sensorNames()
{
    return runOnThread([this]() {
        QStringList names;
        if (!ensureSettingsDb())
        {
            return names;
        }
        QSqlDatabase db = QSqlDatabase::database(kSettingsConnection);
        QSqlQuery query(db);
        query.prepare("SELECT sensor_key, sensor_name FROM sensor_config ORDER BY sensor_key");
        if (query.exec())
        {
            while (query.next())
            {
                names << QString("%1=%2").arg(query.value(0).toString(), query.value(1).toString());
            }
        }
        else
        {
            qWarning() << "Failed to fetch sensor names:" << query.lastError().text();
        }

        return names;
    });
}

QJsonArray SqlManager::getSensorMapJson() const
{
    SqlManager* self = const_cast<SqlManager*>(this);
    return self->runOnThread([self]() {
        QJsonArray arr;
        if (!self->ensureSettingsDb())
        {
            return arr;
        }

        QSqlDatabase db = QSqlDatabase::database(kSettingsConnection);
        QSqlQuery query(db);
        query.prepare("SELECT sensor_key, sensor_name FROM sensor_config ORDER BY sensor_key");
        if (query.exec())
        {
            while (query.next())
            {
                QJsonObject obj;
                obj.insert("key", query.value(0).toString());
                obj.insert("name", query.value(1).toString());
                arr.append(obj);
            }
        }
        else
        {
            qWarning() << "Failed to fetch sensor map json:" << query.lastError().text();
        }
        return arr;
    });
}

bool SqlManager::updateSensorMapJson(const QJsonArray& arr, QString* errMsg)
{
    return runOnThread([this, arr, errMsg]() {
        if (!ensureSettingsDb())
        {
            if (errMsg) *errMsg = "settings db not available";
            return false;
        }

        QSqlDatabase db = QSqlDatabase::database(kSettingsConnection);
        QSqlQuery query(db);
        if (!query.exec("DELETE FROM sensor_config"))
        {
            if (errMsg) *errMsg = query.lastError().text();
            return false;
        }

        for (const QJsonValue& v : arr)
        {
            if (!v.isObject())
            {
                if (errMsg) *errMsg = "each item must be object";
                return false;
            }
            QJsonObject obj = v.toObject();
            QString key = obj.value("key").toString();
            QString name = obj.value("name").toString();
            if (key.isEmpty())
            {
                if (errMsg) *errMsg = "key missing";
                return false;
            }
            query.prepare("REPLACE INTO sensor_config (sensor_key, sensor_name) VALUES (:key, :name)");
            query.bindValue(":key", key);
            query.bindValue(":name", name);
            if (!query.exec())
            {
                if (errMsg) *errMsg = query.lastError().text();
                return false;
            }
        }
        return true;
    });
}

bool SqlManager::insertOneSampleJson(const QJsonObject& obj, QString* errMsg)
{
    return runOnThread([this, obj, errMsg]() {
        if (!obj.contains("ts") || !obj.value("ts").isDouble())
        {
            if (errMsg) *errMsg = "ts missing or invalid";
            return false;
        }
        qint64 ts = static_cast<qint64>(obj.value("ts").toDouble());
        QVector<double> readings;
        readings.reserve(kSensorCount);
        for (int i = 0; i < kSensorCount; ++i)
        {
            QString key = QString("s%1").arg(i + 1);
            if (obj.contains(key) && obj.value(key).isDouble())
            {
                readings << obj.value(key).toDouble();
            }
        }

        bool ok = saveSensorData(QDateTime::fromSecsSinceEpoch(ts), readings);
        if (!ok && errMsg)
        {
            *errMsg = "insert failed";
        }
        return ok;
    });
}

bool SqlManager::insertBatchSamplesJson(const QJsonArray& items, int* inserted, QString* errMsg)
{
    return runOnThread([this, items, inserted, errMsg]() {
        int count = 0;
        for (const QJsonValue& v : items)
        {
            if (!v.isObject())
            {
                if (errMsg) *errMsg = "batch items must be object";
                return false;
            }
            QString localErr;
            if (!insertOneSampleJson(v.toObject(), &localErr))
            {
                if (errMsg) *errMsg = localErr;
                return false;
            }
            ++count;
        }
        if (inserted) *inserted = count;
        return true;
    });
}

bool SqlManager::queryRangeJson(qint64 from, qint64 to, QJsonArray* out, QString* errMsg)
{
    return queryRangeJsonPaged(from, to, 1, std::numeric_limits<int>::max(), out, errMsg);
}

bool SqlManager::queryRangeJsonPaged(qint64 from, qint64 to, int page, int pageSize, QJsonArray* out, QString* errMsg)
{
    return runOnThread([this, from, to, page, pageSize, out, errMsg]() {
        if (!out)
        {
            if (errMsg) *errMsg = "output array is null";
            return false;
        }
        *out = QJsonArray();
        if (to < from)
        {
            if (errMsg) *errMsg = "to < from";
            return false;
        }
        if (page <= 0 || pageSize <= 0)
        {
            if (errMsg) *errMsg = "page and pageSize must be positive";
            return false;
        }

        QDate startDate = QDateTime::fromSecsSinceEpoch(from).date();
        QDate endDate = QDateTime::fromSecsSinceEpoch(to).date();

        QDate iter = QDate(startDate.year(), startDate.month(), 1);
        QDate endIter = QDate(endDate.year(), endDate.month(), 1);

        qint64 skipRemaining = static_cast<qint64>(page - 1) * static_cast<qint64>(pageSize);
        int takeRemaining = pageSize;

        while (iter <= endIter)
        {
            if (takeRemaining <= 0)
            {
                break;
            }

            QString key = monthKey(iter);
            QString filePath = dataFileForKey(key);
            QFileInfo fi(filePath);
            if (!fi.exists())
            {
                iter = iter.addMonths(1);
                continue;
            }

            QSqlDatabase db = openDataDb(key);
            if (!db.isValid() || !db.isOpen())
            {
                if (errMsg) *errMsg = "db open failed";
                return false;
            }
            if (!ensureDataSchema(db))
            {
                if (errMsg) *errMsg = "ensure schema failed";
                return false;
            }

            QSqlQuery countQuery(db);
            countQuery.prepare("SELECT COUNT(1) FROM sensor_data WHERE timestamp >= :from AND timestamp <= :to");
            countQuery.bindValue(":from", from);
            countQuery.bindValue(":to", to);
            if (!countQuery.exec() || !countQuery.next())
            {
                if (errMsg) *errMsg = countQuery.lastError().text();
                return false;
            }

            qint64 monthCount = countQuery.value(0).toLongLong();
            if (monthCount <= 0)
            {
                iter = iter.addMonths(1);
                continue;
            }

            if (skipRemaining >= monthCount)
            {
                skipRemaining -= monthCount;
                iter = iter.addMonths(1);
                continue;
            }

            QStringList columns;
            columns << "timestamp";
            for (int i = 0; i < kSensorCount; ++i)
            {
                columns << QString("s%1").arg(i + 1);
            }

            int localOffset = static_cast<int>(skipRemaining);
            skipRemaining = 0;

            QSqlQuery query(db);
            query.prepare(QString("SELECT %1 FROM sensor_data WHERE timestamp >= :from AND timestamp <= :to ORDER BY timestamp LIMIT :limit OFFSET :offset")
                              .arg(columns.join(", ")));
            query.bindValue(":from", from);
            query.bindValue(":to", to);
            query.bindValue(":limit", takeRemaining);
            query.bindValue(":offset", localOffset);
            if (!query.exec())
            {
                if (errMsg) *errMsg = query.lastError().text();
                return false;
            }

            while (query.next())
            {
                QJsonObject obj;
                obj.insert("ts", query.value(0).toLongLong());
                for (int i = 0; i < kSensorCount; ++i)
                {
                    obj.insert(QString("s%1").arg(i + 1), QJsonValue::fromVariant(query.value(i + 1)));
                }
                out->append(obj);
                --takeRemaining;
                if (takeRemaining <= 0)
                {
                    break;
                }
            }

            iter = iter.addMonths(1);
        }
        return true;
    });
}

bool SqlManager::countSensorRange(qint64 from, qint64 to, qint64* total, QString* errMsg)
{
    return runOnThread([this, from, to, total, errMsg]() {
        if (!total)
        {
            if (errMsg) *errMsg = "total is null";
            return false;
        }
        *total = 0;
        if (to < from)
        {
            if (errMsg) *errMsg = "to < from";
            return false;
        }

        QDate startDate = QDateTime::fromSecsSinceEpoch(from).date();
        QDate endDate = QDateTime::fromSecsSinceEpoch(to).date();
        QDate iter = QDate(startDate.year(), startDate.month(), 1);
        QDate endIter = QDate(endDate.year(), endDate.month(), 1);

        while (iter <= endIter)
        {
            QString key = monthKey(iter);
            QString filePath = dataFileForKey(key);
            QFileInfo fi(filePath);
            if (!fi.exists())
            {
                iter = iter.addMonths(1);
                continue;
            }

            QSqlDatabase db = openDataDb(key);
            if (!db.isValid() || !db.isOpen())
            {
                if (errMsg) *errMsg = "db open failed";
                return false;
            }
            if (!ensureDataSchema(db))
            {
                if (errMsg) *errMsg = "ensure schema failed";
                return false;
            }

            QSqlQuery query(db);
            query.prepare("SELECT COUNT(1) FROM sensor_data WHERE timestamp >= :from AND timestamp <= :to");
            query.bindValue(":from", from);
            query.bindValue(":to", to);
            if (!query.exec() || !query.next())
            {
                if (errMsg) *errMsg = query.lastError().text();
                return false;
            }
            *total += query.value(0).toLongLong();

            iter = iter.addMonths(1);
        }

        return true;
    });
}

bool SqlManager::querySensorRangeDescPaged(qint64 from, qint64 to, int page, int pageSize,
                                           QJsonArray* out, QString* errMsg)
{
    return runOnThread([this, from, to, page, pageSize, out, errMsg]() {
        if (!out)
        {
            if (errMsg) *errMsg = "output array is null";
            return false;
        }
        *out = QJsonArray();
        if (to < from)
        {
            if (errMsg) *errMsg = "to < from";
            return false;
        }
        if (page <= 0 || pageSize <= 0)
        {
            if (errMsg) *errMsg = "page and pageSize must be positive";
            return false;
        }

        const QDate startDate = QDateTime::fromSecsSinceEpoch(from).date();
        const QDate endDate = QDateTime::fromSecsSinceEpoch(to).date();
        if (startDate.year() != endDate.year() || startDate.month() != endDate.month())
        {
            if (errMsg) *errMsg = "from and to must be in the same month";
            return false;
        }

        const QString key = monthKey(startDate);
        if (!QFileInfo::exists(dataFileForKey(key)))
        {
            return true;
        }

        QSqlDatabase db = openDataDb(key);
        if (!db.isValid() || !db.isOpen())
        {
            if (errMsg) *errMsg = "db open failed";
            return false;
        }
        if (!ensureDataSchema(db))
        {
            if (errMsg) *errMsg = "ensure schema failed";
            return false;
        }

        QStringList columns;
        columns << "timestamp";
        for (int i = 0; i < kSensorCount; ++i)
        {
            columns << QString("s%1").arg(i + 1);
        }

        // Walks idx_sensor_data_ts backwards from the newest row; rowid breaks
        // ties between equal timestamps so that pages never overlap.
        QSqlQuery query(db);
        query.prepare(QString("SELECT %1 FROM sensor_data WHERE timestamp >= :from AND timestamp <= :to "
                              "ORDER BY timestamp DESC, rowid DESC LIMIT :limit OFFSET :offset")
                          .arg(columns.join(", ")));
        query.bindValue(":from", from);
        query.bindValue(":to", to);
        query.bindValue(":limit", pageSize);
        query.bindValue(":offset", static_cast<qint64>(page - 1) * static_cast<qint64>(pageSize));
        if (!query.exec())
        {
            if (errMsg) *errMsg = query.lastError().text();
            return false;
        }

        while (query.next())
        {
            QJsonObject obj;
            obj.insert("ts", query.value(0).toLongLong());
            for (int i = 0; i < kSensorCount; ++i)
            {
                obj.insert(QString("s%1").arg(i + 1), QJsonValue::fromVariant(query.value(i + 1)));
            }
            out->append(obj);
        }
        return true;
    });
}

void SqlManager::requestSensorHistoryPage(quint64 requestId, qint64 from, qint64 to,
                                          int page, int pageSize)
{
    // Remember the newest id first, so that an older request still waiting in
    // the queue is skipped instead of executed.
    quint64 latest = m_latestHistoryRequestId.load();
    while (latest < requestId
           && !m_latestHistoryRequestId.compare_exchange_weak(latest, requestId))
    {
    }

    // Queued, never blocking: the work runs later on the SqlManager thread and
    // the result goes back through the sensorHistoryPageReady signal.  'this'
    // is the context object, so nothing runs once SqlManager is destroyed.
    QMetaObject::invokeMethod(this, [this, requestId, from, to, page, pageSize]() {
        SensorHistoryPageResult result;
        result.requestId = requestId;
        result.page = page;
        result.pageSize = pageSize;

        if (requestId < m_latestHistoryRequestId.load())
        {
            result.superseded = true;
            emit sensorHistoryPageReady(result);
            return;
        }

        QElapsedTimer timer;
        timer.start();
        QString err;
        result.countOk = countSensorRange(from, to, &result.totalRows, &err);   // runs inline on this thread
        result.countMs = timer.nsecsElapsed() / 1.0e6;
        if (!result.countOk)
        {
            result.errorMessage = err;
            emit sensorHistoryPageReady(result);
            return;
        }

        timer.restart();
        const qint64 offset = static_cast<qint64>(page - 1) * static_cast<qint64>(pageSize);
        if (result.totalRows > 0 && page > 0 && offset < result.totalRows)
        {
            result.ok = querySensorRangeDescPaged(from, to, page, pageSize, &result.samples, &err);
            if (!result.ok)
                result.errorMessage = err;
        }
        else
        {
            result.ok = page > 0;      // empty month or page past the end: no page query
            if (!result.ok)
                result.errorMessage = "page must be positive";
        }
        result.pageMs = timer.nsecsElapsed() / 1.0e6;
        emit sensorHistoryPageReady(result);
    }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// w2-041: History range across months + export file list.
// ---------------------------------------------------------------------------

namespace
{
    // SQLite database header, offset 24: "file change counter" (big endian),
    // incremented by every write transaction in rollback-journal mode (the
    // mode of the sensor files).  Returns false if the header cannot be read.
    bool readSqliteChangeCounter(const QString& path, quint32* counter, qint64* size)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return false;
        *size = file.size();
        const QByteArray header = file.read(28);
        if (header.size() < 28)
            return false;
        const auto* b = reinterpret_cast<const uchar*>(header.constData()) + 24;
        *counter = (quint32(b[0]) << 24) | (quint32(b[1]) << 16) | (quint32(b[2]) << 8) | quint32(b[3]);
        return true;
    }
}

// ---------------------------------------------------------------------------
// w2-045: the History range request runs as a chain of short queued steps.
//
// The main thread saves a sample every second with a blocking call into this
// thread, so any uninterrupted work here delays the main thread by as long.
// A request is therefore cut into units (one month's cached count, one walk
// over a bounded number of index rows, one page read, ...); a step runs units until about
// kHistoryStepBudgetMs of work is done and then queues the next step behind
// whatever arrived meanwhile (for example the save).
//
// Consistency between steps: all writes to the month files happen on this
// thread, and sensor_data rows are only ever inserted (saveSensorData;
// nothing updates or deletes them), so a new row always gets a rowid above
// the current MAX(rowid).  Each month's MAX(rowid) is taken as a snapshot when
// the month is first touched by the request; COUNTs and page reads ignore rows
// above it (only when such rows exist, so the usual case has no extra filter).
// The page is therefore exactly the OFFSET page of the database as of the
// snapshots, however many saves ran between the steps.
// ---------------------------------------------------------------------------

namespace
{
    constexpr double kHistoryStepBudgetMs = 6.0;    // work per queued step (then yield)
    constexpr double kHistoryUnitTargetMs = 3.0;    // a walk unit is sized to take about this
    constexpr qint64 kHistoryDirectRows = 50000;    // a page read skipping at most this many rows is one unit
    constexpr qint64 kHistoryDeltaMaxRows = 10000;  // rows added since a cached count that are counted by rowid
    constexpr qint64 kHistoryFirstChunkRows = 50000;
    constexpr qint64 kHistoryMinChunkRows = 2000;
    constexpr qint64 kHistoryMaxChunkRows = 2000000;

    double msSince(const QElapsedTimer& timer)
    {
        return timer.nsecsElapsed() / 1.0e6;
    }

    const QString& historySelectColumns()
    {
        static const QString columns = []() {
            QStringList list{QStringLiteral("rowid"), QStringLiteral("timestamp")};
            for (int i = 0; i < kSensorCount; ++i)
                list << QStringLiteral("s%1").arg(i + 1);
            return list.join(QStringLiteral(", "));
        }();
        return columns;
    }
}

struct SqlManager::HistoryMonth
{
    SensorMonthFile file;
    qint64 lo = 0;           // the request range clamped to the month
    qint64 hi = 0;
    qint64 snap = -1;        // MAX(rowid) snapshot (-1: not taken yet)
    qint64 count = -1;       // rows in [lo, hi] with rowid <= snap
    bool dbChecked = false;  // opened and schema checked by this request
};

// Walks one month file over the timestamp index in chunks of at most 'chunk'
// rows, newest first (DESC) or oldest first (ASC).  A chunk ends at its last
// row (timestamp, rowid), found with LIMIT 1 OFFSET chunk-1; the next chunk
// starts after it (keyset).  target < 0: count the whole month.  target >= 0
// (seek): stop when the target-th row (0-based, in walk order) is less than a
// chunk away from the cursor; the page read then does the rest.  The chunk
// size adapts to kHistoryUnitTargetMs, so a unit stays short also when the
// file is not in the OS cache and whatever the gaps in the data are.
struct SqlManager::HistoryChunkWalk
{
    bool active = false;
    bool ascending = false;
    bool hasCursor = false;  // cursor = last row of the previous chunk
    HistoryAnchorRow cursor;
    qint64 chunk = kHistoryFirstChunkRows;
    qint64 counted = 0;      // rows before the cursor (inclusive), in walk order
    qint64 target = -1;
    bool found = false;
    // count mode: file stamp taken with the snapshot, stored in the cache at the end
    qint64 stampSize = -1;
    quint32 stampCounter = 0;
    bool stampOk = false;
};

struct SqlManager::HistoryRow
{
    qint64 rowid = 0;
    qint64 ts = 0;
    QJsonObject sample;
};

struct SqlManager::HistoryRangeJob
{
    enum class Phase { Start, Count, Locate, Fetch };

    quint64 requestId = 0;
    qint64 from = 0;
    qint64 to = 0;
    int page = 0;
    int pageSize = 0;
    Phase phase = Phase::Start;
    QList<HistoryMonth> months;       // newest first
    int index = 0;                    // Count: month being counted; Fetch: month being read
    HistoryChunkWalk walk;
    qint64 local = 0;                 // Fetch: DESC position in months[index] of the next row
    qint64 remaining = 0;             // Fetch: rows still to read
    qint64 target = 0;                // DESC position of the page's first row in the range
    bool methodChosen = false;
    int anchorMonth = -1;             // index in months, -1: no usable anchor
    qint64 anchorLocal = 0;           // DESC position of the anchor row in its month now
    HistoryAnchorRow anchorRow;
    QString anchorNote;               // why the anchor is (not) used, for the log
    QList<HistoryRow> rows;           // page rows, newest first
    QStringList rowKeys;              // month key of each row
    SensorHistoryPageResult result;
    QElapsedTimer wall;               // started when the request was posted
};

QList<SqlManager::SensorMonthFile> SqlManager::sensorMonthFilesInRange(qint64 from, qint64 to) const
{
    // Lists the data directory instead of walking month by month from 'from'
    // to 'to': the unbounded range (epoch 0 .. year 275760) would be millions
    // of months.  Newest month first.
    QList<SensorMonthFile> months;
    if (to < from)
        return months;
    static const QRegularExpression pattern(QStringLiteral("^sensor_(\\d{4})(\\d{2})\\.sqlite$"));
    const QStringList names = QDir(m_dataDir).entryList(QStringList{QStringLiteral("sensor_*.sqlite")},
                                                        QDir::Files, QDir::Name);
    for (const QString& name : names)
    {
        const QRegularExpressionMatch m = pattern.match(name);
        if (!m.hasMatch())
            continue;
        const QDate monthStart(m.captured(1).toInt(), m.captured(2).toInt(), 1);
        if (!monthStart.isValid())
            continue;
        SensorMonthFile month;
        month.key = monthKey(monthStart);
        month.monthFrom = QDateTime(monthStart, QTime(0, 0)).toSecsSinceEpoch();
        month.monthTo = QDateTime(monthStart.addMonths(1), QTime(0, 0)).toSecsSinceEpoch() - 1;
        if (month.monthTo < from || month.monthFrom > to)
            continue;
        months.append(month);
    }
    std::sort(months.begin(), months.end(), [](const SensorMonthFile& a, const SensorMonthFile& b) {
        return a.monthFrom > b.monthFrom;
    });
    return months;
}

void SqlManager::requestSensorHistoryRangePage(quint64 requestId, qint64 from, qint64 to,
                                               int page, int pageSize)
{
    quint64 latest = m_latestHistoryRequestId.load();
    while (latest < requestId
           && !m_latestHistoryRequestId.compare_exchange_weak(latest, requestId))
    {
    }

    auto job = std::make_shared<HistoryRangeJob>();
    job->requestId = requestId;
    job->from = from;
    job->to = to;
    job->page = page;
    job->pageSize = pageSize;
    job->result.requestId = requestId;
    job->result.page = page;
    job->result.pageSize = pageSize;
    job->wall.start();

    // Queued, never blocking: the steps run later on the SqlManager thread and
    // the result goes back through sensorHistoryPageReady.  'this' is the
    // context object, so nothing runs once SqlManager is destroyed.
    QMetaObject::invokeMethod(this, [this, job]() { runHistoryRangeStep(job); }, Qt::QueuedConnection);
}

void SqlManager::runHistoryRangeStep(const std::shared_ptr<HistoryRangeJob>& job)
{
    QElapsedTimer step;
    step.start();
    SensorHistoryPageResult& result = job->result;

    if (job->requestId < m_latestHistoryRequestId.load())
    {
        // A newer request exists: drop the remaining steps.
        result.superseded = true;
        result.totalMs = msSince(job->wall);
        emit sensorHistoryPageReady(result);
        return;
    }

    bool finished = false;
    do
    {
        QElapsedTimer unit;
        unit.start();
        const bool counting = job->phase == HistoryRangeJob::Phase::Start
                || job->phase == HistoryRangeJob::Phase::Count;
        finished = historyRangeUnit(*job);
        (counting ? result.countMs : result.pageMs) += msSince(unit);
    } while (!finished && msSince(step) < kHistoryStepBudgetMs);

    const double stepMs = msSince(step);
    ++result.steps;
    result.stepMs.append(stepMs);
    result.maxStepMs = qMax(result.maxStepMs, stepMs);

    if (finished)
    {
        result.totalMs = msSince(job->wall);
        emit sensorHistoryPageReady(result);
        return;
    }
    // Behind everything already queued (e.g. a blocking save from the main thread).
    QMetaObject::invokeMethod(this, [this, job]() { runHistoryRangeStep(job); }, Qt::QueuedConnection);
}

bool SqlManager::historyRangeUnit(HistoryRangeJob& job)
{
    switch (job.phase)
    {
    case HistoryRangeJob::Phase::Start:
    {
        if (job.page <= 0 || job.pageSize <= 0 || job.to < job.from)
        {
            historyFinish(job, false, job.to < job.from ? QStringLiteral("to < from")
                                                        : QStringLiteral("page and pageSize must be positive"));
            return true;
        }
        for (const SensorMonthFile& file : sensorMonthFilesInRange(job.from, job.to))
        {
            HistoryMonth month;
            month.file = file;
            month.lo = qMax(job.from, file.monthFrom);
            month.hi = qMin(job.to, file.monthTo);
            job.months.append(month);
        }
        job.result.months = job.months.size();
        job.phase = HistoryRangeJob::Phase::Count;
        return false;
    }
    case HistoryRangeJob::Phase::Count:
        return historyCountUnit(job);
    case HistoryRangeJob::Phase::Locate:
        return historyLocate(job);
    case HistoryRangeJob::Phase::Fetch:
        return historyFetchUnit(job);
    }
    return true;
}

bool SqlManager::historyOpenMonth(HistoryMonth& month, QSqlDatabase* db, QString* errMsg)
{
    *db = openDataDb(month.file.key);
    if (!db->isValid() || !db->isOpen())
    {
        if (errMsg) *errMsg = QStringLiteral("db open failed");
        return false;
    }
    if (!month.dbChecked && !m_historySchemaChecked.contains(month.file.key))
    {
        // Once per month file and data directory (the schema statements take
        // longer than a keyset page read).
        if (!ensureDataSchema(*db))
        {
            if (errMsg) *errMsg = QStringLiteral("ensure schema failed");
            return false;
        }
        m_historySchemaChecked.insert(month.file.key);
    }
    month.dbChecked = true;
    return true;
}

bool SqlManager::historyMaxRowid(QSqlDatabase& db, qint64* rowid, QString* errMsg)
{
    QSqlQuery query(db);
    query.setForwardOnly(true);
    if (!query.exec(QStringLiteral("SELECT MAX(rowid) FROM sensor_data")) || !query.next())
    {
        if (errMsg) *errMsg = query.lastError().text();
        return false;
    }
    *rowid = query.value(0).isNull() ? 0 : query.value(0).toLongLong();
    return true;
}

bool SqlManager::historySnapFilter(QSqlDatabase& db, const HistoryMonth& month, bool* filter, QString* errMsg)
{
    // Rows above the snapshot exist only if something was saved into this
    // month since the snapshot; only then do the queries need "+rowid <= snap"
    // (the unary + keeps SQLite on the timestamp index).
    qint64 now = 0;
    if (!historyMaxRowid(db, &now, errMsg))
        return false;
    *filter = now > month.snap;
    return true;
}

bool SqlManager::historyCountUnit(HistoryRangeJob& job)
{
    QString err;
    if (job.index >= job.months.size())
    {
        job.result.countOk = true;
        job.result.totalRows = 0;
        for (const HistoryMonth& month : std::as_const(job.months))
            job.result.totalRows += month.count;
        job.phase = HistoryRangeJob::Phase::Locate;
        return false;
    }

    HistoryMonth& month = job.months[job.index];
    const QString path = dataFileForKey(month.file.key);

    if (job.walk.active)
    {
        // Full recount in progress: one more chunk of index rows.
        QSqlDatabase db;
        bool filter = false;
        if (!historyOpenMonth(month, &db, &err) || !historySnapFilter(db, month, &filter, &err)
            || !historyWalkUnit(db, month, filter, job.walk, &err))
        {
            historyFinish(job, false, QStringLiteral("%1: %2").arg(month.file.key, err));
            return true;
        }
        if (!job.walk.active)
        {
            month.count = job.walk.counted;
            if (job.walk.stampOk)
                m_rangeCountCache.insert(month.file.key,
                                         RangeCountCacheEntry{month.lo, month.hi, job.walk.stampSize,
                                                              job.walk.stampCounter, month.count, month.snap, path});
            else
                m_rangeCountCache.remove(month.file.key);
            ++job.index;
        }
        return false;
    }

    // Start of a month: the w2-041 cache first (unchanged file: no query at all).
    quint32 counter = 0;
    qint64 size = -1;
    const bool stampOk = readSqliteChangeCounter(path, &counter, &size);
    const auto cached = m_rangeCountCache.constFind(month.file.key);
    const bool sameRange = cached != m_rangeCountCache.constEnd() && cached->from == month.lo
            && cached->to == month.hi && cached->path == path && cached->snapRowid >= 0;
    if (stampOk && sameRange && cached->fileSize == size && cached->changeCounter == counter)
    {
        month.snap = cached->snapRowid;
        month.count = cached->count;
        ++job.result.countCacheHits;
        ++job.index;
        return false;
    }

    QSqlDatabase db;
    if (!historyOpenMonth(month, &db, &err) || !historyMaxRowid(db, &month.snap, &err))
    {
        historyFinish(job, false, QStringLiteral("%1: %2").arg(month.file.key, err));
        return true;
    }

    const auto store = [&](qint64 count) {
        month.count = count;
        if (stampOk)
            m_rangeCountCache.insert(month.file.key,
                                     RangeCountCacheEntry{month.lo, month.hi, size, counter, count, month.snap, path});
        else
            m_rangeCountCache.remove(month.file.key);
        ++job.index;
    };

    if (sameRange && month.snap == cached->snapRowid)
    {
        // The file changed (e.g. an alarm row) but no sensor row was added.
        ++job.result.countCacheHits;
        store(cached->count);
        return false;
    }
    if (sameRange && month.snap > cached->snapRowid && month.snap - cached->snapRowid <= kHistoryDeltaMaxRows)
    {
        // Only the rows added since the cached count (rowid above its snapshot).
        QSqlQuery query(db);
        query.setForwardOnly(true);
        query.prepare(QStringLiteral("SELECT COUNT(1) FROM sensor_data NOT INDEXED WHERE rowid > :r0 AND rowid <= :r1 "
                                     "AND timestamp >= :lo AND timestamp <= :hi"));
        query.bindValue(QStringLiteral(":r0"), cached->snapRowid);
        query.bindValue(QStringLiteral(":r1"), month.snap);
        query.bindValue(QStringLiteral(":lo"), month.lo);
        query.bindValue(QStringLiteral(":hi"), month.hi);
        if (!query.exec() || !query.next())
        {
            historyFinish(job, false, QStringLiteral("%1: %2").arg(month.file.key, query.lastError().text()));
            return true;
        }
        ++job.result.countCacheDeltas;
        store(cached->count + query.value(0).toLongLong());
        return false;
    }
    if (month.snap == 0)
    {
        store(0);                               // empty table
        return false;
    }

    // Full count, in chunks of index rows, newest first.
    job.walk = HistoryChunkWalk{};
    job.walk.active = true;
    job.walk.stampOk = stampOk;
    job.walk.stampSize = size;
    job.walk.stampCounter = counter;
    return false;
}

bool SqlManager::historyWalkUnit(QSqlDatabase& db, const HistoryMonth& month, bool snapFilter,
                                 HistoryChunkWalk& walk, QString* errMsg)
{
    if (walk.target >= 0 && walk.target - walk.counted < walk.chunk)
    {
        walk.found = true;                      // within one chunk of the cursor
        walk.active = false;
        return true;
    }

    // The chunk-th row after the cursor (covering index scan of 'chunk' rows).
    const QString order = walk.ascending ? QStringLiteral("ASC") : QStringLiteral("DESC");
    QString where = QStringLiteral("timestamp >= :lo AND timestamp <= :hi");
    if (snapFilter)
        where += QStringLiteral(" AND +rowid <= :snap");
    if (walk.hasCursor)
        where += walk.ascending ? QStringLiteral(" AND (timestamp > :cts OR +rowid > :crid)")
                                : QStringLiteral(" AND (timestamp < :cts OR +rowid < :crid)");
    const auto bindWhere = [&](QSqlQuery& q) {
        q.bindValue(QStringLiteral(":lo"), walk.hasCursor && walk.ascending ? qMax(month.lo, walk.cursor.ts) : month.lo);
        q.bindValue(QStringLiteral(":hi"), walk.hasCursor && !walk.ascending ? qMin(month.hi, walk.cursor.ts) : month.hi);
        if (snapFilter)
            q.bindValue(QStringLiteral(":snap"), month.snap);
        if (walk.hasCursor)
        {
            q.bindValue(QStringLiteral(":cts"), walk.cursor.ts);
            q.bindValue(QStringLiteral(":crid"), walk.cursor.rowid);
        }
    };

    QElapsedTimer timer;
    timer.start();
    QSqlQuery query(db);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral("SELECT timestamp, rowid FROM sensor_data WHERE %1 ORDER BY timestamp %2, rowid %2 "
                                 "LIMIT 1 OFFSET :k").arg(where, order));
    bindWhere(query);
    query.bindValue(QStringLiteral(":k"), walk.chunk - 1);
    if (!query.exec())
    {
        if (errMsg) *errMsg = query.lastError().text();
        return false;
    }
    if (query.next())
    {
        walk.cursor.ts = query.value(0).toLongLong();
        walk.cursor.rowid = query.value(1).toLongLong();
        walk.hasCursor = true;
        walk.counted += walk.chunk;
        const double ms = msSince(timer);
        const double factor = qBound(0.25, kHistoryUnitTargetMs / qMax(ms, 0.05), 4.0);
        walk.chunk = qBound<qint64>(kHistoryMinChunkRows, qint64(double(walk.chunk) * factor), kHistoryMaxChunkRows);
        return true;
    }
    query.finish();

    // Fewer than 'chunk' rows are left.
    walk.active = false;
    if (walk.target >= 0)
        return true;                            // seek: target not found (found stays false)
    QSqlQuery count(db);
    count.setForwardOnly(true);
    count.prepare(QStringLiteral("SELECT COUNT(1) FROM sensor_data WHERE ") + where);
    bindWhere(count);
    if (!count.exec() || !count.next())
    {
        if (errMsg) *errMsg = count.lastError().text();
        return false;
    }
    walk.counted += count.value(0).toLongLong();
    return true;
}

bool SqlManager::historyFetchRows(QSqlDatabase& db, const HistoryMonth& month, bool snapFilter,
                                  qint64 lo, qint64 hi, bool ascending, HistoryKeyset keyset,
                                  const HistoryAnchorRow& anchor, qint64 offset, qint64 limit,
                                  QList<HistoryRow>* rows, QString* errMsg)
{
    // The w2-039/w2-041 page statement (ORDER BY timestamp, rowid on the
    // timestamp index) with optional snapshot and keyset conditions.
    QString sql = QStringLiteral("SELECT %1 FROM sensor_data WHERE timestamp >= :lo AND timestamp <= :hi")
                          .arg(historySelectColumns());
    if (snapFilter)
        sql += QStringLiteral(" AND +rowid <= :snap");
    if (keyset == HistoryKeyset::OlderOrEqual)       // DESC from the anchor row itself
        sql += QStringLiteral(" AND (timestamp < :ats OR +rowid <= :arid)");
    else if (keyset == HistoryKeyset::Older)         // DESC from the row after the anchor
        sql += QStringLiteral(" AND (timestamp < :ats OR +rowid < :arid)");
    else if (keyset == HistoryKeyset::Newer)         // ASC from the row after the anchor
        sql += QStringLiteral(" AND (timestamp > :ats OR +rowid > :arid)");
    sql += ascending ? QStringLiteral(" ORDER BY timestamp ASC, rowid ASC LIMIT :limit OFFSET :offset")
                     : QStringLiteral(" ORDER BY timestamp DESC, rowid DESC LIMIT :limit OFFSET :offset");

    QSqlQuery query(db);
    query.setForwardOnly(true);
    query.prepare(sql);
    query.bindValue(QStringLiteral(":lo"), keyset == HistoryKeyset::Newer ? qMax(lo, anchor.ts) : lo);
    const bool older = keyset == HistoryKeyset::OlderOrEqual || keyset == HistoryKeyset::Older;
    query.bindValue(QStringLiteral(":hi"), older ? qMin(hi, anchor.ts) : hi);
    if (snapFilter)
        query.bindValue(QStringLiteral(":snap"), month.snap);
    if (keyset != HistoryKeyset::None)
    {
        query.bindValue(QStringLiteral(":ats"), anchor.ts);
        query.bindValue(QStringLiteral(":arid"), anchor.rowid);
    }
    query.bindValue(QStringLiteral(":limit"), limit);
    query.bindValue(QStringLiteral(":offset"), offset);
    if (!query.exec())
    {
        if (errMsg) *errMsg = query.lastError().text();
        return false;
    }
    const qsizetype firstNew = rows->size();
    while (query.next())
    {
        HistoryRow row;
        row.rowid = query.value(0).toLongLong();
        row.ts = query.value(1).toLongLong();
        row.sample.insert(QStringLiteral("ts"), row.ts);
        for (int i = 0; i < kSensorCount; ++i)
            row.sample.insert(QStringLiteral("s%1").arg(i + 1), QJsonValue::fromVariant(query.value(i + 2)));
        rows->append(row);
    }
    if (ascending)
        std::reverse(rows->begin() + firstNew, rows->end());   // callers always get newest first
    return true;
}

bool SqlManager::historyEvaluateAnchor(HistoryRangeJob& job, QString* errMsg)
{
    // The previous result's first/last row is usable when the range is the
    // same, the page is the same or next to it, and every month file from the
    // anchor's month up to the newest only grew (inserts).  Its position now =
    // old position + rows added since in newer months + rows added since in
    // its own month that sort before it.
    job.anchorMonth = -1;
    const HistoryAnchor& anchor = m_historyAnchor;
    if (!anchor.valid)
    {
        job.anchorNote = QStringLiteral("no previous page");
        return true;
    }
    if (anchor.from != job.from || anchor.to != job.to || anchor.pageSize != job.pageSize)
    {
        job.anchorNote = QStringLiteral("range changed");
        return true;
    }
    if (qAbs(job.page - anchor.page) > 1)
    {
        job.anchorNote = QStringLiteral("not an adjacent page");
        return true;
    }
    const HistoryAnchorRow& row = job.page > anchor.page ? anchor.last : anchor.first;
    for (auto it = anchor.months.cbegin(); it != anchor.months.cend(); ++it)
    {
        if (it.key() < row.key)
            continue;
        const bool present = std::any_of(job.months.cbegin(), job.months.cend(),
                                         [&](const HistoryMonth& m) { return m.file.key == it.key(); });
        if (!present)
        {
            job.anchorNote = QStringLiteral("month file %1 disappeared").arg(it.key());
            return true;
        }
    }

    qint64 shift = 0;
    qint64 base = 0;
    for (int i = 0; i < job.months.size(); ++i)
    {
        HistoryMonth& month = job.months[i];
        if (month.file.key < row.key)
            break;
        const bool known = anchor.months.contains(month.file.key);
        const QPair<qint64, qint64> old = anchor.months.value(month.file.key, qMakePair(qint64(0), qint64(0)));
        if (month.snap < old.first || month.count < old.second)
        {
            job.anchorNote = QStringLiteral("%1 lost rows").arg(month.file.key);
            return true;
        }
        if (month.file.key > row.key)
        {
            shift += month.count - old.second;
            base += month.count;
            continue;
        }
        if (!known)
        {
            job.anchorNote = QStringLiteral("anchor month %1 unknown").arg(month.file.key);
            return true;
        }
        qint64 newer = 0;
        if (month.snap != old.first)
        {
            if (month.snap - old.first > kHistoryDeltaMaxRows)
            {
                job.anchorNote = QStringLiteral("%1: too many new rows").arg(month.file.key);
                return true;
            }
            QSqlDatabase db;
            if (!historyOpenMonth(month, &db, errMsg))
                return false;
            QSqlQuery query(db);
            query.setForwardOnly(true);
            query.prepare(QStringLiteral(
                    "SELECT COUNT(1), TOTAL(CASE WHEN timestamp > :ats1 OR (timestamp = :ats2 AND rowid > :arid) "
                    "THEN 1 ELSE 0 END) FROM sensor_data NOT INDEXED WHERE rowid > :r0 AND rowid <= :r1 "
                    "AND timestamp >= :lo AND timestamp <= :hi"));
            query.bindValue(QStringLiteral(":ats1"), row.ts);
            query.bindValue(QStringLiteral(":ats2"), row.ts);
            query.bindValue(QStringLiteral(":arid"), row.rowid);
            query.bindValue(QStringLiteral(":r0"), old.first);
            query.bindValue(QStringLiteral(":r1"), month.snap);
            query.bindValue(QStringLiteral(":lo"), month.lo);
            query.bindValue(QStringLiteral(":hi"), month.hi);
            if (!query.exec() || !query.next())
            {
                if (errMsg) *errMsg = query.lastError().text();
                return false;
            }
            const qint64 added = query.value(0).toLongLong();
            newer = qint64(query.value(1).toDouble());
            if (added != month.count - old.second)
            {
                job.anchorNote = QStringLiteral("%1: rows changed, not only added").arg(month.file.key);
                return true;
            }
        }
        else if (month.count != old.second)
        {
            job.anchorNote = QStringLiteral("%1: rows changed, not only added").arg(month.file.key);
            return true;
        }
        const qint64 local = row.pos + shift + newer - base;
        if (local < 0 || local >= month.count)
        {
            job.anchorNote = QStringLiteral("anchor out of range");
            return true;
        }
        job.anchorMonth = i;
        job.anchorLocal = local;
        job.anchorRow = row;
        job.anchorNote = QStringLiteral("anchor %1 row (%2, rowid %3) moved by %4")
                                 .arg(job.page > anchor.page ? QStringLiteral("last") : QStringLiteral("first"))
                                 .arg(row.ts).arg(row.rowid).arg(shift + newer);
        return true;
    }
    job.anchorNote = QStringLiteral("anchor month %1 not in range").arg(row.key);
    return true;
}

bool SqlManager::historyLocate(HistoryRangeJob& job)
{
    job.target = static_cast<qint64>(job.page - 1) * static_cast<qint64>(job.pageSize);
    if (job.target >= job.result.totalRows)
    {
        // Empty range or a page past the end: no page query (as in w2-041).
        job.result.pageMethod = QStringLiteral("none");
        historyFinish(job, true, QString());
        return true;
    }
    QString err;
    if (!historyEvaluateAnchor(job, &err))
    {
        historyFinish(job, false, err);
        return true;
    }
    qint64 base = 0;
    for (int i = 0; i < job.months.size(); ++i)
    {
        if (job.target < base + job.months.at(i).count)
        {
            job.index = i;
            job.local = job.target - base;
            break;
        }
        base += job.months.at(i).count;
    }
    job.remaining = qMin<qint64>(job.pageSize, job.result.totalRows - job.target);
    job.walk = HistoryChunkWalk{};
    job.phase = HistoryRangeJob::Phase::Fetch;
    return false;
}

bool SqlManager::historyFetchUnit(HistoryRangeJob& job)
{
    HistoryMonth& month = job.months[job.index];
    QSqlDatabase db;
    bool filter = false;
    QString err;
    const auto fail = [&](const QString& e) {
        historyFinish(job, false, QStringLiteral("%1: %2").arg(month.file.key, e));
        return true;
    };
    if (!historyOpenMonth(month, &db, &err) || !historySnapFilter(db, month, &filter, &err))
        return fail(err);

    const qint64 n = qMin(job.remaining, month.count - job.local);   // rows from this month
    QList<HistoryRow> rows;
    const HistoryAnchorRow noAnchor;

    if (job.walk.active)
    {
        if (!historyWalkUnit(db, month, filter, job.walk, &err))
            return fail(err);
        if (job.walk.active)
            return false;
        if (!job.walk.found)
            return fail(QStringLiteral("page position not found (rows changed?)"));
        // The page starts less than one chunk after the cursor row.
        const HistoryKeyset after = !job.walk.hasCursor ? HistoryKeyset::None
                : job.walk.ascending ? HistoryKeyset::Newer : HistoryKeyset::Older;
        const bool ok = historyFetchRows(db, month, filter, month.lo, month.hi, job.walk.ascending, after,
                                         job.walk.cursor, job.walk.target - job.walk.counted, n, &rows, &err);
        if (!ok)
            return fail(err);
    }
    else if (!job.methodChosen)
    {
        // First month of the page: from the anchor row when the page starts in
        // its month and near it (adjacent pages), else OFFSET from the nearer
        // end of the month file, else that OFFSET walked in chunks (seek).
        job.methodChosen = true;
        const qint64 fromNewest = job.local;
        const qint64 fromOldest = month.count - job.local - n;
        qint64 viaAnchor = std::numeric_limits<qint64>::max();
        if (job.anchorMonth == job.index)
            viaAnchor = qAbs(job.local - job.anchorLocal);
        const qint64 best = viaAnchor <= kHistoryDirectRows ? viaAnchor : qMin(fromNewest, fromOldest);
        if (best > kHistoryDirectRows)
        {
            // Too far for one query: the OFFSET from the nearer end is walked
            // in chunks of index rows, over several steps.
            job.walk = HistoryChunkWalk{};
            job.walk.active = true;
            job.walk.ascending = fromOldest < fromNewest;
            job.walk.target = job.walk.ascending ? fromOldest : fromNewest;
            job.result.pageMethod = job.walk.ascending ? QStringLiteral("seek-asc") : QStringLiteral("seek-desc");
            return false;
        }
        if (best == viaAnchor)
        {
            // Keyset from the anchor row: rows newer than it (ASC after it),
            // then the anchor and older rows (DESC from it).
            job.result.pageMethod = QStringLiteral("keyset");
            const qint64 la = job.anchorLocal;
            const qint64 newerCount = qMax<qint64>(0, qMin(la, job.local + n) - job.local);
            if (newerCount > 0
                && !historyFetchRows(db, month, filter, month.lo, month.hi, true, HistoryKeyset::Newer, job.anchorRow,
                                     la - job.local - newerCount, newerCount, &rows, &err))
                return fail(err);
            if (n - newerCount > 0
                && !historyFetchRows(db, month, filter, month.lo, month.hi, false, HistoryKeyset::OlderOrEqual,
                                     job.anchorRow, qMax(job.local, la) - la, n - newerCount, &rows, &err))
                return fail(err);
        }
        else if (fromNewest <= fromOldest)
        {
            job.result.pageMethod = QStringLiteral("offset-desc");
            if (!historyFetchRows(db, month, filter, month.lo, month.hi, false, HistoryKeyset::None, noAnchor,
                                  fromNewest, n, &rows, &err))
                return fail(err);
        }
        else
        {
            job.result.pageMethod = QStringLiteral("offset-asc");
            if (!historyFetchRows(db, month, filter, month.lo, month.hi, true, HistoryKeyset::None, noAnchor,
                                  fromOldest, n, &rows, &err))
                return fail(err);
        }
    }
    else
    {
        // The page continues at the newest row of an older month.
        if (!historyFetchRows(db, month, filter, month.lo, month.hi, false, HistoryKeyset::None, noAnchor,
                              job.local, n, &rows, &err))
            return fail(err);
    }

    for (const HistoryRow& row : std::as_const(rows))
    {
        job.rows.append(row);
        job.rowKeys.append(month.file.key);
    }
    job.remaining -= rows.size();
    // Next (older) month with rows, from its newest row.
    do
    {
        ++job.index;
    } while (job.index < job.months.size() && job.months.at(job.index).count <= 0);
    job.local = 0;
    job.walk = HistoryChunkWalk{};
    if (job.remaining <= 0 || job.index >= job.months.size())
    {
        historyFinish(job, true, QString());
        return true;
    }
    return false;
}

void SqlManager::historyFinish(HistoryRangeJob& job, bool ok, const QString& error)
{
    SensorHistoryPageResult& result = job.result;
    result.ok = ok;
    result.errorMessage = error;
    for (const HistoryMonth& month : std::as_const(job.months))
    {
        if (month.snap >= 0)
            result.snapshotRowids.insert(month.file.key, month.snap);
    }
    if (!ok || job.rows.isEmpty())
    {
        m_historyAnchor = HistoryAnchor{};
        return;
    }
    for (const HistoryRow& row : std::as_const(job.rows))
        result.samples.append(row.sample);

    HistoryAnchor anchor;
    anchor.valid = true;
    anchor.from = job.from;
    anchor.to = job.to;
    anchor.page = job.page;
    anchor.pageSize = job.pageSize;
    for (const HistoryMonth& month : std::as_const(job.months))
        anchor.months.insert(month.file.key, qMakePair(month.snap, month.count));
    anchor.first = HistoryAnchorRow{job.rowKeys.first(), job.rows.first().ts, job.rows.first().rowid, job.target};
    anchor.last = HistoryAnchorRow{job.rowKeys.last(), job.rows.last().ts, job.rows.last().rowid,
                                   job.target + job.rows.size() - 1};
    m_historyAnchor = anchor;
}

QStringList SqlManager::sensorDataFilesInRange(qint64 from, qint64 to)
{
    return runOnThread([this, from, to]() {
        QStringList files;
        for (const SensorMonthFile& month : sensorMonthFilesInRange(from, to))
            files << QFileInfo(dataFileForKey(month.key)).absoluteFilePath();
        return files;
    });
}

bool SqlManager::countHoldingRange(qint64 from, qint64 to, qint64* total, QString* errMsg)
{
    return runOnThread([this, from, to, total, errMsg]() {
        if (!total)
        {
            if (errMsg) *errMsg = "total is null";
            return false;
        }
        *total = 0;
        if (to < from)
        {
            if (errMsg) *errMsg = "to < from";
            return false;
        }

        QDate startDate = QDateTime::fromSecsSinceEpoch(from).date();
        QDate endDate = QDateTime::fromSecsSinceEpoch(to).date();
        QDate iter = QDate(startDate.year(), startDate.month(), 1);
        QDate endIter = QDate(endDate.year(), endDate.month(), 1);

        while (iter <= endIter)
        {
            QString key = monthKey(iter);
            QString filePath = dataFileForKey(key);
            QFileInfo fi(filePath);
            if (!fi.exists())
            {
                iter = iter.addMonths(1);
                continue;
            }

            QSqlDatabase db = openDataDb(key);
            if (!db.isValid() || !db.isOpen())
            {
                if (errMsg) *errMsg = "db open failed";
                return false;
            }
            if (!ensureDataSchema(db))
            {
                if (errMsg) *errMsg = "ensure schema failed";
                return false;
            }

            QSqlQuery query(db);
            query.prepare("SELECT COUNT(1) FROM holding_register WHERE timestamp >= :from AND timestamp <= :to");
            query.bindValue(":from", from);
            query.bindValue(":to", to);
            if (!query.exec() || !query.next())
            {
                if (errMsg) *errMsg = query.lastError().text();
                return false;
            }
            *total += query.value(0).toLongLong();

            iter = iter.addMonths(1);
        }

        return true;
    });
}

bool SqlManager::insertAlarm(const QDateTime& occurrence, const QString& reason, QString* errMsg,
                             qint64* insertedId)
{
    return runOnThread([this, occurrence, reason, errMsg, insertedId]() {
        QString key = monthKey(occurrence.date());
        QSqlDatabase db = openDataDb(key);
        if (!db.isValid() || !db.isOpen())
        {
            if (errMsg) *errMsg = "db open failed";
            return false;
        }

        if (!ensureDataSchema(db))
        {
            if (errMsg) *errMsg = "ensure schema failed";
            return false;
        }

        QSqlQuery query(db);
        query.prepare("INSERT INTO alarm_history (occurrence_time, reason) VALUES (:ts, :reason)");
        query.bindValue(":ts", occurrence.toSecsSinceEpoch());
        query.bindValue(":reason", reason);

        if (!query.exec())
        {
            if (errMsg) *errMsg = query.lastError().text();
            return false;
        }
        if (insertedId)
        {
            const QVariant id = query.lastInsertId();
            *insertedId = id.isValid() ? id.toLongLong() : -1;
        }
        return true;
    });
}

bool SqlManager::insertAlarm(const QString& reason, QString* errMsg)
{
    return insertAlarm(QDateTime::currentDateTime(), reason, errMsg);
}

bool SqlManager::updateAlarmReason(const QDateTime& occurrence, qint64 id, const QString& reason,
                                   QString* errMsg)
{
    return runOnThread([this, occurrence, id, reason, errMsg]() {
        const QString key = monthKey(occurrence.date());
        QSqlDatabase db = openDataDb(key);
        if (!db.isValid() || !db.isOpen())
        {
            if (errMsg) *errMsg = "db open failed";
            return false;
        }

        QSqlQuery query(db);
        query.prepare("UPDATE alarm_history SET reason = :reason WHERE id = :id");
        query.bindValue(":reason", reason);
        query.bindValue(":id", id);
        if (!query.exec())
        {
            if (errMsg) *errMsg = query.lastError().text();
            return false;
        }
        if (query.numRowsAffected() != 1)
        {
            if (errMsg) *errMsg = QStringLiteral("no alarm_history row id=%1 in %2").arg(id).arg(key);
            return false;
        }
        return true;
    });
}

bool SqlManager::getAlarmHistory(qint64 from, qint64 to, QJsonArray* out, QString* errMsg)
{
    return runOnThread([this, from, to, out, errMsg]() {
        if (!out)
        {
            if (errMsg) *errMsg = "output array is null";
            return false;
        }
        *out = QJsonArray();
        if (to < from)
        {
            if (errMsg) *errMsg = "to < from";
            return false;
        }

        QDate startDate = QDateTime::fromSecsSinceEpoch(from).date();
        QDate endDate = QDateTime::fromSecsSinceEpoch(to).date();

        QDate iter = QDate(startDate.year(), startDate.month(), 1);
        QDate endIter = QDate(endDate.year(), endDate.month(), 1);

        while (iter <= endIter)
        {
            QString key = monthKey(iter);
            QString filePath = dataFileForKey(key);
            QFileInfo fi(filePath);
            if (!fi.exists())
            {
                iter = iter.addMonths(1);
                continue;
            }

            QSqlDatabase db = openDataDb(key);
            if (!db.isValid() || !db.isOpen())
            {
                if (errMsg) *errMsg = "db open failed";
                return false;
            }
            if (!ensureDataSchema(db))
            {
                if (errMsg) *errMsg = "ensure schema failed";
                return false;
            }

            QSqlQuery query(db);
            query.prepare("SELECT id, occurrence_time, reason FROM alarm_history WHERE occurrence_time >= :from AND occurrence_time <= :to ORDER BY occurrence_time");
            query.bindValue(":from", from);
            query.bindValue(":to", to);
            if (!query.exec())
            {
                if (errMsg) *errMsg = query.lastError().text();
                return false;
            }

            while (query.next())
            {
                QJsonObject obj;
                obj.insert("id", query.value(0).toLongLong());
                obj.insert("occurrence_time", query.value(1).toLongLong());
                obj.insert("reason", query.value(2).toString());
                out->append(obj);
            }

            iter = iter.addMonths(1);
        }

        return true;
    });
}

QVector<QVariantList> SqlManager::fetchSensorData(const QDate& date, int limit)
{
    return runOnThread([this, date, limit]() {
        QVector<QVariantList> rows;
        QString key = monthKey(date);
        QSqlDatabase db = openDataDb(key);
        if (!db.isValid() || !db.isOpen())
        {
            qWarning() << "Data database is not open for key" << key;
            return rows;
        }

        if (!ensureDataSchema(db))
        {
            return rows;
        }

        QStringList columns;
        columns << "timestamp";
        for (int i = 0; i < kSensorCount; ++i)
        {
            columns << QString("s%1").arg(i + 1);
        }

        int count = limit > 0 ? limit : 10;
        QString sql = QString("SELECT %1 FROM sensor_data ORDER BY timestamp DESC LIMIT %2")
                          .arg(columns.join(", "))
                          .arg(count);

        QSqlQuery query(db);
        if (!query.exec(sql))
        {
            qWarning() << "Failed to fetch data:" << query.lastError().text();
            return rows;
        }

        while (query.next())
        {
            QVariantList row;
            for (int i = 0; i < columns.size(); ++i)
            {
                row << query.value(i);
            }
            rows << row;
        }

        return rows;
    });
}

QVector<QVariantList> SqlManager::fetchHoldingRegisters(const QDate& date, int limit)
{
    return runOnThread([this, date, limit]() {
        QVector<QVariantList> rows;
        QString key = monthKey(date);
        QSqlDatabase db = openDataDb(key);
        if (!db.isValid() || !db.isOpen())
        {
            qWarning() << "Data database is not open for key" << key;
            return rows;
        }

        if (!ensureDataSchema(db))
        {
            return rows;
        }

        QStringList columns;
        columns << "timestamp";
        for (int i = 0; i < kHoldingCount; ++i)
        {
            columns << QString("h%1").arg(i + 1);
        }

        int count = limit > 0 ? limit : 10;
        QString sql = QString("SELECT %1 FROM holding_register ORDER BY timestamp DESC LIMIT %2")
                          .arg(columns.join(", "))
                          .arg(count);

        QSqlQuery query(db);
        if (!query.exec(sql))
        {
            qWarning() << "Failed to fetch holding registers:" << query.lastError().text();
            return rows;
        }

        while (query.next())
        {
            QVariantList row;
            for (int i = 0; i < columns.size(); ++i)
            {
                row << query.value(i);
            }
            rows << row;
        }

        return rows;
    });
}

QString SqlManager::dataFilePathForMonth(const QDate& date) const
{
    return runOnThread([this, date]() { return dataFileForKey(monthKey(date)); });
}

QString SqlManager::monthKey(const QDate& date) const
{
    return date.toString("yyyyMM");
}

QString SqlManager::dataFileForKey(const QString& key) const
{
    QDir dir(m_dataDir);
    return dir.filePath(QString("sensor_%1.sqlite").arg(key));
}

QString SqlManager::dataConnectionName(const QString& key) const
{
    return QString("data_%1").arg(key);
}

bool SqlManager::ensureDirExists(const QString& dir) const
{
    QDir path(dir);
    if (path.exists())
    {
        return true;
    }
    return QDir().mkpath(dir);
}

bool SqlManager::ensureSettingsDb()
{
    QSqlDatabase db;
    if (QSqlDatabase::contains(kSettingsConnection))
    {
        db = QSqlDatabase::database(kSettingsConnection);
    }
    else
    {
        db = QSqlDatabase::addDatabase("QSQLITE", kSettingsConnection);
    }

    db.setDatabaseName(m_settingsPath);

    if (!db.isOpen() && !db.open())
    {
        qWarning() << "Failed to open settings database:" << db.lastError().text();
        return false;
    }

    // execute schema from file; fallback to built-in
    if (!executeSqlFile(m_settingsSchemaPath, db))
    {
        QSqlQuery query(db);
        if (!query.exec("CREATE TABLE IF NOT EXISTS sensor_config (sensor_key TEXT PRIMARY KEY, sensor_name TEXT)"))
        {
            qWarning() << "Failed to ensure settings schema:" << query.lastError().text();
            return false;
        }
        if (!query.exec("CREATE TABLE IF NOT EXISTS app_settings (key TEXT PRIMARY KEY, value TEXT)"))
        {
            qWarning() << "Failed to ensure app settings schema:" << query.lastError().text();
            return false;
        }

        // defaults for sensor names
        QSqlQuery qDefaults(db);
        qDefaults.prepare("INSERT OR IGNORE INTO sensor_config (sensor_key, sensor_name) VALUES "
                          "('s1','inletWaterTemp'),('s2','inletWaterPressure'),('s3','returnWaterTemp'),"
                          "('s4','returnWaterPressure'),('s5','outletWaterTemp'),('s6','outletWaterPressure'),"
                          "('s7','coolingL1'),('s8','coolingL2'),('s9','coolingR1'),('s10','coolingR2'),"
                          "('s11','inletAirTemp'),('s12','inletAirHumidity'),('s13','flowRate'),('s14','outletWaterPV'),"
                          "('s15','returnWaterPV'),('s16','fanAutoSpeed'),('s17','outletAirTemp'),('s18','pressureDifference'),"
                          "('s19','TBD'),('s20','heatExchange')");
        if (!qDefaults.exec())
        {
            qWarning() << "Failed to insert default sensor config:" << qDefaults.lastError().text();
            return false;
        }
    }

    // ensure default read frequency exists
    QSqlQuery qFreq(db);
    qFreq.prepare("INSERT OR IGNORE INTO app_settings (key, value) VALUES ('read_frequency', '1000')");
    if (!qFreq.exec())
    {
        qWarning() << "Failed to ensure read_frequency:" << qFreq.lastError().text();
        return false;
    }

    return true;
}

bool SqlManager::ensureDataSchema(QSqlDatabase& db) const
{
    // execute schema from file; fallback to built-in
    if (!executeSqlFile(m_dataSchemaPath, db))
    {
        QSqlQuery query(db);
        QStringList columns;
        columns << "timestamp INTEGER NOT NULL";
        for (int i = 0; i < kSensorCount; ++i)
        {
            columns << QString("s%1 REAL").arg(i + 1);
        }

        QString createSql = QString("CREATE TABLE IF NOT EXISTS sensor_data (%1)").arg(columns.join(", "));
        if (!query.exec(createSql))
        {
            qWarning() << "Failed to ensure data schema:" << query.lastError().text();
            return false;
        }

        if (!query.exec("CREATE INDEX IF NOT EXISTS idx_sensor_data_ts ON sensor_data(timestamp)"))
        {
            qWarning() << "Failed to ensure data index:" << query.lastError().text();
            return false;
        }

        QStringList hCols;
        hCols << "timestamp INTEGER NOT NULL";
        for (int i = 0; i < kHoldingCount; ++i)
        {
            hCols << QString("h%1 INTEGER").arg(i + 1);
        }

        QString createHolding = QString("CREATE TABLE IF NOT EXISTS holding_register (%1)").arg(hCols.join(", "));
        if (!query.exec(createHolding))
        {
            qWarning() << "Failed to ensure holding register schema:" << query.lastError().text();
            return false;
        }

        if (!query.exec("CREATE INDEX IF NOT EXISTS idx_holding_register_ts ON holding_register(timestamp)"))
        {
            qWarning() << "Failed to ensure holding register index:" << query.lastError().text();
            return false;
        }

        if (!query.exec("CREATE TABLE IF NOT EXISTS alarm_history (id INTEGER PRIMARY KEY, occurrence_time INTEGER, reason VARCHAR(255))"))
        {
            qWarning() << "Failed to ensure alarm history schema:" << query.lastError().text();
            return false;
        }

        if (!query.exec("CREATE INDEX IF NOT EXISTS idx_alarm_history_time ON alarm_history(occurrence_time)"))
        {
            qWarning() << "Failed to ensure alarm history index:" << query.lastError().text();
            return false;
        }
    }

    return true;
}

QSqlDatabase SqlManager::openDataDb(const QString& monthKey)
{
    QString connection = dataConnectionName(monthKey);
    QSqlDatabase db;
    if (QSqlDatabase::contains(connection))
    {
        db = QSqlDatabase::database(connection);
    }
    else
    {
        db = QSqlDatabase::addDatabase("QSQLITE", connection);
        m_dataConnectionNames << connection;
    }

    db.setDatabaseName(dataFileForKey(monthKey));

    if (!db.isOpen())
    {
        if (!ensureDirExists(m_dataDir))
        {
            qWarning() << "Failed to create data directory" << m_dataDir;
            return QSqlDatabase();
        }

        if (!db.open())
        {
            qWarning() << "Failed to open data database" << db.databaseName() << db.lastError().text();
            return QSqlDatabase();
        }
    }

    return db;
}

void SqlManager::startWorkerThread()
{
    if (m_threadStarted)
    {
        return;
    }
    m_threadStarted = true;
    m_thread = new QThread();
    m_thread->setObjectName("SqlManagerThread");
    this->moveToThread(m_thread);
    m_thread->start();
}

template <typename F>
auto SqlManager::runOnThread(F&& func) const -> decltype(func())
{
    using ResultType = decltype(func());
    if (QThread::currentThread() == this->thread())
    {
        return func();
    }

    if constexpr (std::is_void_v<ResultType>)
    {
        QMetaObject::invokeMethod(const_cast<SqlManager*>(this),
                                  [func]() mutable { func(); },
                                  Qt::BlockingQueuedConnection);
    }
    else
    {
        ResultType result{};
        QMetaObject::invokeMethod(const_cast<SqlManager*>(this),
                                  [&result, func]() mutable { result = func(); },
                                  Qt::BlockingQueuedConnection);
        return result;
    }
}
