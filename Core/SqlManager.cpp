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

bool SqlManager::insertAlarm(const QDateTime& occurrence, const QString& reason, QString* errMsg)
{
    return runOnThread([this, occurrence, reason, errMsg]() {
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
        return true;
    });
}

bool SqlManager::insertAlarm(const QString& reason, QString* errMsg)
{
    return insertAlarm(QDateTime::currentDateTime(), reason, errMsg);
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
