#include "exampleproxy.h"

ExampleProxy::ExampleProxy(QObject *parent)
    : QObject(parent)
{
}

int ExampleProxy::count() const
{
    return m_count;
}

QString ExampleProxy::status() const
{
    return m_status;
}

bool ExampleProxy::transportReady() const
{
    return m_transportReady;
}

QString ExampleProxy::transportMessage() const
{
    return m_transportMessage;
}

void ExampleProxy::setCount(int value)
{
    if (m_count == value) {
        return;
    }
    m_count = value;
    emit countChanged(m_count);
}

void ExampleProxy::setStatus(const QString &value)
{
    if (m_status == value) {
        return;
    }
    m_status = value;
    emit statusChanged();
}

void ExampleProxy::setTransportState(bool ready, const QString &message)
{
    if (m_transportReady != ready) {
        m_transportReady = ready;
        emit transportReadyChanged();
    }
    if (m_transportMessage != message) {
        m_transportMessage = message;
        emit transportMessageChanged();
    }
}

