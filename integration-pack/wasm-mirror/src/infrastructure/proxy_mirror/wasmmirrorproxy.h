// WasmMirrorProxy 提供可直接嵌入既有 Qt 專案的多 Proxy 同步執行環境。
// 呼叫端只需用 WasmMirrorConfig 登錄具名 QObject；Desktop 會開啟一個
// WebSocket Server，WebAssembly 則會用同一份設定建立可自動重連的 Client。
#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>

struct WasmMirrorProxyOptions
{
    // required Proxy 的 contract 不一致時，整條連線會在送出任何狀態前拒絕。
    bool required = true;

    // 選配的本機 UI transport overlay。一般 Proxy 不需要設定；若 UI 需要
    // 顯示 Desktop Core 是否在線，可在這裡轉呼叫自己的狀態 setter。
    std::function<void(bool ready, const QString &message)>
        transportStateHandler;
};

class WasmMirrorConfig
{
public:
    // Desktop 綁定與 WebAssembly 連線使用的 loopback host。
    QString host = QStringLiteral("127.0.0.1");

    // 建議正式專案使用固定 port。Desktop 可設 0 讓 OS 自動分配；
    // WebAssembly 無 discovery 時不可使用 0。
    quint16 port = 8124;

    // 保留給反向代理或未來多 endpoint；目前預設根路徑即可。
    QString path = QStringLiteral("/");

    // 空 Origin 允許原生測試工具；瀏覽器只允許由本機資產伺服器開啟。
    QStringList allowedOrigins{
        QStringLiteral("http://127.0.0.1:8123"),
        QStringLiteral("http://localhost:8123")
    };

    bool addProxy(const QString &mirrorName,
                  QObject &uiProxy,
                  WasmMirrorProxyOptions options = {});

    QStringList mirrorNames() const;
    QString validationError() const;

private:
    struct Registration
    {
        QString mirrorName;
        QPointer<QObject> proxy;
        WasmMirrorProxyOptions options;
    };

    QList<Registration> m_registrations;
    QString m_validationError;

    friend class WasmMirrorProxy;
};

class WasmMirrorProxy final : public QObject
{
    Q_OBJECT

public:
    static constexpr int WireProtocolVersion = 3;

    static std::unique_ptr<WasmMirrorProxy> create(
        const WasmMirrorConfig &config,
        QString *errorMessage = nullptr);

    ~WasmMirrorProxy() override;

    quint16 actualPort() const;
    QUrl webSocketUrl() const;
    QStringList mirrorNames() const;
    bool isReady() const;

signals:
    void readyChanged(bool ready);
    void proxyReadyChanged(const QString &mirrorName, bool ready);
    void transportError(const QString &mirrorName, const QString &message);

private:
    explicit WasmMirrorProxy(const WasmMirrorConfig &config,
                             QObject *parent = nullptr);

    class Private;
    std::unique_ptr<Private> d;
};
