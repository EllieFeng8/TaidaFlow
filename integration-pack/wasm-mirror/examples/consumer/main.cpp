#include "exampleproxy.h"

#include "infrastructure/proxy_mirror/wasmmirrorproxy.h"

#include <QCoreApplication>
#include <QDebug>

#include <cstdlib>
#include <memory>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // 同一個 QObject 實體同時交給 QML（本範例省略 QML 註冊）與 Mirror Config。
    ExampleProxy proxy;

#if !defined(Q_OS_WASM)
    // Desktop authoritative Core 原本的 signal/slot 接線可以保持不變。
    QObject::connect(
        &proxy,
        &ExampleProxy::startRequested,
        &proxy,
        [&proxy](int seconds) {
            proxy.setCount(seconds);
            proxy.setStatus(QStringLiteral("Running"));
        });
#endif

    WasmMirrorConfig config;
    config.host = QStringLiteral("127.0.0.1");
    config.port = 8125;
    config.path = QStringLiteral("/mirror");
    config.allowedOrigins = {
        QStringLiteral("http://127.0.0.1:8123"),
        QStringLiteral("http://localhost:8123")
    };

    WasmMirrorProxyOptions options;
    options.required = true;

#if defined(Q_OS_WASM)
    // 選配的本機 UI overlay；STORED false，所以不會反向同步到 Desktop。
    // 必須在 QML 首次讀取前切成 false；Desktop 保持 Proxy 的 true 預設值。
    proxy.setTransportState(
        false, QStringLiteral("Connecting to the Qt desktop Core..."));
    options.transportStateHandler =
        [&proxy](bool ready, const QString &message) {
            proxy.setTransportState(ready, message);
        };
#endif

    if (!config.addProxy(QStringLiteral("Example"),
                         proxy,
                         std::move(options))) {
        qCritical().noquote() << config.validationError();
        return EXIT_FAILURE;
    }

    QString error;
    auto mirror = WasmMirrorProxy::create(config, &error);
    if (!mirror) {
        qCritical().noquote() << error;
        return EXIT_FAILURE;
    }

    qInfo().noquote()
        << "WASM Mirror endpoint:"
        << mirror->webSocketUrl().toString();

    // mirror 必須活到 event loop 結束；不可建立後立即丟棄 unique_ptr。
    return app.exec();
}
