#pragma once
#include <QElapsedTimer>
#include <QObject>
#include "manager.h"
#include "TaidaFlowProxy.h"

class ModbusServer;
class SqlManager;
class HistoryExportManager;
class HistoryViewService;

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
    void loadAlarmRecords();
    // w2-049: web page + /exports on the AppHttpServer singleton (0.0.0.0:8124).
    void startHttpServer();

    Manager* m_manager = nullptr;
    ModbusServer* m_modbusServer = nullptr;
    SqlManager* m_sqlManager = nullptr;
    // w2-052 (spec §2.1): one History view per client (historyViewRequested ->
    // historyViews[sessionId]); replaces the shared page / range loading of w2-039/w2-041.
    HistoryViewService* m_historyViews = nullptr;
    HistoryExportManager* m_historyExport = nullptr;   // raw CSV export + download service

};
