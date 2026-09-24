// Copyright (C) 2024 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

#include <QApplication>
#include <QQmlApplicationEngine>
#include <cstdlib>
#include <memory>
#include <utility>

#include "autogen/environment.h"
#include "Core/TaidaFlowProxy.h"
#include "embeddedfonts.h"
#include "infrastructure/proxy_mirror/wasmmirrorproxy.h"

#if !defined(Q_OS_WASM)
// Desktop-only authoritative Core (Modbus/MS300/REST/SQLite). Never part of WASM.
#include "core.h"
#include "e2epvdriver.h"
#endif


int main(int argc, char *argv[])
{
    set_qt_environment();
    QApplication app(argc, argv);

    // WebAssembly has no system CJK fonts: register the embedded Noto Sans TC
    // subset and make it the fallback for every UI font. The desktop keeps the
    // platform's own fallback chain so its rendering is unchanged.
    const QString cjkFamily = TaidaFlowFonts::loadEmbeddedCjkFont();
#if defined(Q_OS_WASM)
    TaidaFlowFonts::installCjkFallbackChain(cjkFamily);
#endif

    // Desktop-only Core / WASM only builds the Proxy (wasm-mirror pack 1.0.1,
    // package-integration §7, guide §10). The replica owner is declared before the
    // mirror so that it outlives it.
    std::unique_ptr<TaidaFlowProxy> wasmProxyOwner;
    TaidaFlowProxy *Td = nullptr;
#if defined(Q_OS_WASM)
    // Same concrete class and contract as the desktop; its constructor has no
    // hardware side effect, so no MirrorReplica mode is needed.
    wasmProxyOwner = std::make_unique<TaidaFlowProxy>();
    Td = wasmProxyOwner.get();
#else
    Core& core = Core::instance();
    core.init();
    Td = core.m_proxy;
    if (!Td) {
        qCritical() << "TaidaFlowProxy is null.";
        return EXIT_FAILURE;
    }
#endif
    // The manual registration URI must not equal any qt_add_qml_module(URI ...)
    // (package-integration §7, guide §8). Core/CMakeLists.txt owns URI "Core", so the
    // Td singleton lives in its own URI "TaidaFlowBackend"; QML imports
    // `TaidaFlowBackend 1.0`.
    qmlRegisterSingletonInstance<TaidaFlowProxy>("TaidaFlowBackend", 1, 0, "Td", Td);

#if !defined(Q_OS_WASM)
    // DEV/E2E only: inactive unless TAIDAFLOW_E2E_PV_FILE is set (see e2epvdriver.h).
    std::unique_ptr<E2ePvDriver> e2ePvDriver;
    if (qEnvironmentVariableIsSet("TAIDAFLOW_E2E_PV_FILE")) {
        e2ePvDriver = std::make_unique<E2ePvDriver>(
            Td, qEnvironmentVariable("TAIDAFLOW_E2E_PV_FILE"));
    }
#endif


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
