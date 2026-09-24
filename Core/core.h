#pragma once
#include <QElapsedTimer>
#include <QObject>
#include "manager.h"
#include "TaidaFlowProxy.h"

class ModbusServer;
class SqlManager;
struct SensorHistoryPageResult;

class Core : public QObject
{
    Q_OBJECT
        QML_ELEMENT
public:
    static Core& instance();
    TaidaFlowProxy* m_proxy = nullptr;
    void init();

private:

    explicit Core(QObject* parent = nullptr) {}
    ~Core();
    void reportIgnoredHmiInputSettings();
    void setHistoryTitleOnce();
    void loadHistoryRecords(const char *reason);
    void applyHistoryPage(const SensorHistoryPageResult &result);
    void loadAlarmRecords();

    Manager* m_manager = nullptr;
    ModbusServer* m_modbusServer = nullptr;
    SqlManager* m_sqlManager = nullptr;
    // w2-039: id of the newest History request; results with another id are
    // stale and dropped.
    quint64 m_historyRequestId = 0;
    QElapsedTimer m_historyRequestClock;   // started when the newest request is posted

};
