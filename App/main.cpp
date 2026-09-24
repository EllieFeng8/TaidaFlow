// Copyright (C) 2024 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

#include <QApplication>
#include <QQmlApplicationEngine>
#include <cstdlib>
#include <memory>
#include <utility>

#include "autogen/environment.h"
#include "Core/TaidaFlowProxy.h"
#include "infrastructure/proxy_mirror/wasmmirrorproxy.h"



int main(int argc, char *argv[])
{
    set_qt_environment();
    QApplication app(argc, argv);

    // ===== Proxy composition: the ONLY branch-specific block =====================
    // main (UI branch) has no backend, so the Proxy is a plain object on every
    // platform. When main is merged into core, replace just this block with core's
    // composition (WASM: std::make_unique<TaidaFlowProxy>(); desktop:
    // Core::instance().init(); Td = core.m_proxy). Everything below is shared.
    // The owner is declared before the mirror so that the Proxy outlives it.
    std::unique_ptr<TaidaFlowProxy> proxyOwner = std::make_unique<TaidaFlowProxy>();
    TaidaFlowProxy *Td = proxyOwner.get();
    // ===== end of Proxy composition ==============================================

    // The manual registration URI must not equal any qt_add_qml_module(URI ...)
    // (wasm-mirror package-integration §7, guide §8). Core/CMakeLists.txt owns URI
    // "Core", so the Td singleton lives in its own URI "TaidaFlowBackend"; QML
    // imports `TaidaFlowBackend 1.0`.
    qmlRegisterSingletonInstance<TaidaFlowProxy>("TaidaFlowBackend", 1, 0, "Td", Td);

    WasmMirrorConfig mirrorConfig;
    mirrorConfig.host = QStringLiteral("127.0.0.1");
    mirrorConfig.port = 8125;
    mirrorConfig.path = QStringLiteral("/mirror");
    mirrorConfig.allowedOrigins = {
        QStringLiteral("http://127.0.0.1:8123"),
        QStringLiteral("http://localhost:8123")
    };

    WasmMirrorProxyOptions mirrorOptions;
    mirrorOptions.required = true;

#if defined(Q_OS_WASM)
    // Transport overlay (package-integration §9): the Proxy member defaults to
    // true so the desktop UI never waits for a remote transport. Only the WASM
    // composition root switches it to false before QML loads, then the handler
    // keeps it in sync with the mirror client (offline / synchronizing / ready).
    Td->setTransportState(
        false, QStringLiteral("Connecting to the Qt desktop Core..."));
    mirrorOptions.transportStateHandler =
        [Td](bool ready, const QString &message) {
            Td->setTransportState(ready, message);
        };
#endif

    if (!mirrorConfig.addProxy(QStringLiteral("TaidaFlow"),
                               *Td,
                               std::move(mirrorOptions))) {
        qCritical().noquote() << mirrorConfig.validationError();
        return EXIT_FAILURE;
    }

    QString mirrorError;
    auto mirror = WasmMirrorProxy::create(mirrorConfig, &mirrorError);
    if (!mirror) {
        qCritical().noquote() << mirrorError;
        return EXIT_FAILURE;
    }

    qInfo().noquote()
        << "WASM Mirror endpoint:"
        << mirror->webSocketUrl().toString();
    // Transport diagnostics (desktop: server listening; WASM: snapshot ready /
    // offline). On WebAssembly these lines appear in the browser console.
    QObject::connect(mirror.get(), &WasmMirrorProxy::readyChanged, &app,
                     [](bool ready) {
        qInfo().noquote() << "WASM Mirror ready:" << (ready ? "true" : "false");
    });
    QObject::connect(mirror.get(), &WasmMirrorProxy::transportError, &app,
                     [](const QString &mirrorName, const QString &message) {
        qInfo().noquote() << "WASM Mirror transport:" << mirrorName << message;
    });
    const auto logTransportOverlay = [Td] {
        qInfo().noquote() << "Transport overlay: transportReady ="
                          << (Td->transportReady() ? "true" : "false")
                          << "message =" << Td->transportMessage();
    };
    QObject::connect(Td, &TaidaFlowProxy::transportReadyChanged, &app, logTransportOverlay);
    QObject::connect(Td, &TaidaFlowProxy::transportMessageChanged, &app, logTransportOverlay);

    QQmlApplicationEngine engine;
    const QUrl url(mainQmlFile);
    QObject::connect(
                &engine, &QQmlApplicationEngine::objectCreated, &app,
                [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    engine.addImportPath(QCoreApplication::applicationDirPath() + "/qml");
    engine.addImportPath(":/");
    engine.load(url);

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
