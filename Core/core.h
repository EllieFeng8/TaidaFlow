#pragma once
#include <QObject>
#include "manager.h"
#include "TaidaFlowProxy.h"

class ModbusServer;
class SqlManager;

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
    void saveHmiInputSettings();
    void loadHmiInputSettings();

    Manager* m_manager = nullptr;
    ModbusServer* m_modbusServer = nullptr;
    SqlManager* m_sqlManager = nullptr;
    bool m_loadingHmiInputSettings = false;

};
