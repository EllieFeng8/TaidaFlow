#pragma once
#include <QElapsedTimer>
#include <QObject>
#include "manager.h"
#include "TaidaFlowProxy.h"

class ModbusServer;
class SqlManager;
class HistoryExportManager;
class HistoryViewService;
class AlarmViewService;
class RESTManager;

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
    // w2-067: stops and releases every backend object (Modbus clients / MS300 / poll timer,
    // Modbus server, REST, History, export, HTTP service, SqlManager thread) while the
    // QCoreApplication still exists: on QCoreApplication::aboutToQuit (normal close), or - when
    // main() returns before app.exec() - from the post routine that ~QApplication runs first.
    // Core itself is a function-local static: ~Core runs during static destruction, after main()
    // returned and after the other function-local statics created later (AppHttpServer,
    // ModbusClient's host table) were destroyed, so it must not do this work. Idempotent.
    void shutdown(const char* reason);
    bool m_shutDown = false;
    void reportIgnoredHmiInputSettings();
    void setHistoryTitleOnce();
    void loadAlarmRecords();
    // w2-049/w2-062: web page + /exports on the AppHttpServer singleton (config.json http, default 0.0.0.0:8124); writes <web folder>/runtime.json.
    void startHttpServer();
    // w2-060/w2-062: REST API (RESTManager) on config.json rest (default 127.0.0.1:18080);
    // reached from the LAN only through nginx (http://<host>/api/...).
    void startRestServer();
    void stopRestServer();

    Manager* m_manager = nullptr;
    ModbusServer* m_modbusServer = nullptr;
    SqlManager* m_sqlManager = nullptr;
    // w2-052 (spec §2.1): one History view per client (historyViewRequested ->
    // historyViews[sessionId]); replaces the shared page / range loading of w2-039/w2-041.
    HistoryViewService* m_historyViews = nullptr;
    HistoryExportManager* m_historyExport = nullptr;   // raw CSV export + download service
    RESTManager* m_rest = nullptr;                      // w2-060: REST API (loopback only)
    AlarmViewService* m_alarmViews = nullptr;           // w2-080: one alarm view per client

};
