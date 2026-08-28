#pragma once
#include<qsettings.h>
#include <qobject>
#include <Qvector>
#include <QTimer>
#include "manager.h"
#include "TaidaFlowProxy.h"
#include <QRandomGenerator>

class Core : public QObject
{
    Q_OBJECT
        QML_ELEMENT
public:
    static Core& instance();
    TaidaFlowProxy* m_proxy = nullptr;
    void init();

public slots:
    //proxy
    
    //manager
    
    //void on201data(QVector <quint16>);
    //void on202data(QVector <quint16>);
    //void on203data(QVector <quint16>);
    //void on204data(QVector <quint16>);
    //void on205data(QVector <quint16>);

private:

    explicit Core(QObject* parent = nullptr) {}
    ~Core();

    Manager* m_manager = nullptr;

};