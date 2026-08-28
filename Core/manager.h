#pragma once

#include <qobject>
#include <Qvector>
#include <qthread>
#include "Modbus_Client.h"

class Manager : public QObject
{
    Q_OBJECT

public:
    explicit Manager(QObject* parent = nullptr){}
    ~Manager(){}
};