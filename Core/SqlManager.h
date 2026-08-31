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
    bool insertAlarm(const QDateTime& occurrence, const QString& reason, QString* errMsg = nullptr);
    bool insertAlarm(const QString& reason, QString* errMsg = nullptr);
    bool getAlarmHistory(qint64 from, qint64 to, QJsonArray* out, QString* errMsg = nullptr);

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
