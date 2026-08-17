#pragma once
#include <QObject>
#include <QString>
#include <sstream>
#include <iomanip>
#include <QVariantList>
#include <string>
#include <QDir>
#include <QGuiApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>
#include <QCoreApplication>
#include <QDebug>
class TaidaFlowProxy : public QObject
{
    Q_OBJECT

    Q_PROPERTY(int m1ValueSv READ getM1ValueSv WRITE setM1ValueSv NOTIFY m1ValueSvChanged)

public:
    explicit TaidaFlowProxy(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE int getM1ValueSv() const { return m_m1ValueSv; }
    void setM1ValueSv(int value)
    {
        if (m_m1ValueSv == value) {
            return;
        }

        m_m1ValueSv = value;
        emit m1ValueSvChanged(m_m1ValueSv);
    }

signals:
    void m1ValueSvChanged(int value);

private:
    int m_m1ValueSv = 2220;

};


