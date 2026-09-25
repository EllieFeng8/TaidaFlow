#pragma once
#include <QElapsedTimer>
#include <QObject>
#include "manager.h"
#include "TaidaFlowProxy.h"

class ModbusServer;
class SqlManager;
class HistoryExportManager;
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
    void onHistoryRangeRequested(double fromMs, double toMs);
    void applyHistoryPage(const SensorHistoryPageResult &result);
    void loadAlarmRecords();
    // w2-049: web page + /exports on the AppHttpServer singleton (0.0.0.0:8124).
    void startHttpServer();

    Manager* m_manager = nullptr;
    ModbusServer* m_modbusServer = nullptr;
    SqlManager* m_sqlManager = nullptr;
    // w2-039: id of the newest History request; results with another id are
    // stale and dropped.
    quint64 m_historyRequestId = 0;
    QElapsedTimer m_historyRequestClock;   // started when the newest request is posted
    // w2-041: set while a new History range moves the page back to 1, so that
    // the load triggered by historyCurrentPageChanged is logged as a range change.
    bool m_historyRangeChangePending = false;
    HistoryExportManager* m_historyExport = nullptr;   // raw CSV export + download service

};
