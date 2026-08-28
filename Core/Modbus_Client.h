#pragma once

#include <qobject>
#include <Qvector>
#include <QTimer>
#include <QMutex>
#include <qvariant>
#include <QModbusTcpClient>
#include <QModbusReply>
#include <QQueue>

struct readInput_Data 
{

};

class clientWorker : public QObject
{
    Q_OBJECT

public:
    clientWorker(QObject* parent = nullptr) {}
        ~clientWorker() {}

    void init() {}
};