// ExampleProxy 示範既有 QML-facing Proxy 如何定義可雙向同步的 Q_PROPERTY、
// 一般 UI event signal，以及不進入 Mirror contract 的本機 transport overlay。
#pragma once

#include <QObject>
#include <QString>

class ExampleProxy final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int count READ count WRITE setCount NOTIFY countChanged)
    Q_PROPERTY(QString status READ status WRITE setStatus NOTIFY statusChanged)
    Q_PROPERTY(bool transportReady READ transportReady
               NOTIFY transportReadyChanged STORED false)
    Q_PROPERTY(QString transportMessage READ transportMessage
               NOTIFY transportMessageChanged STORED false)

public:
    explicit ExampleProxy(QObject *parent = nullptr);

    int count() const;
    QString status() const;
    bool transportReady() const;
    QString transportMessage() const;

    void setCount(int value);
    void setStatus(const QString &value);
    void setTransportState(bool ready, const QString &message);

signals:
    void countChanged(int value);
    void statusChanged();
    void transportReadyChanged();
    void transportMessageChanged();

    // 非 Q_PROPERTY NOTIFY 的 public signal 會由 WASM relay 傳到 Desktop。
    void startRequested(int seconds);

private:
    int m_count = 0;
    QString m_status = QStringLiteral("Idle");
    // Desktop 沒有遠端 transport 需要等待，因此預設可操作；WASM composition
    // root 會在載入 QML 前明確切成 false，再交由 handler 維護。
    bool m_transportReady = true;
    QString m_transportMessage;
};
