// Copyright (C) 2024 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QRandomGenerator>
#include <cstdlib>
#include <memory>
#include <utility>

#include "autogen/environment.h"
#include "Core/TaidaFlowProxy.h"
#include "infrastructure/proxy_mirror/wasmmirrorproxy.h"

#if defined(Q_OS_WASM)
#include <emscripten/val.h>
#include <string>
#else
#include <QHostAddress>
#include "lanrelay.h"
#endif

namespace {

// Public Mirror port: the port the web pages connect to (desktop: served by LanRelay).
constexpr quint16 MirrorPublicPort = 8125;
#if !defined(Q_OS_WASM)
// Internal Mirror port on the desktop, loopback only (pack 1.0.0 permits loopback binds only);
// LanRelay forwards MirrorPublicPort to it. Not meant to be reached from outside.
constexpr quint16 MirrorInternalPort = 18125;
#endif

#if defined(Q_OS_WASM)
// Host name of the page URL (window.location.hostname), e.g. "192.168.1.20" when the page
// was opened as http://192.168.1.20:8123/. Empty when it cannot be read. The Mirror
// WebSocket and the download links (spec §3.5, pageHost) go to this same host, so a page
// opened from another computer talks to the desktop that served it.
QString browserPageHostName()
{
    const emscripten::val location = emscripten::val::global("location");
    if (location.isUndefined() || location.isNull())
        return {};
    const emscripten::val hostName = location["hostname"];
    if (!hostName.isString())
        return {};
    return QString::fromStdString(hostName.as<std::string>()).trimmed();
}
#endif

} // namespace

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
#if defined(Q_OS_WASM)
    // LAN access: connect back to the host the page was loaded from (location.hostname),
    // public port 8125. Fallback 127.0.0.1 (the previous fixed host) if it cannot be read.
    QString mirrorHost = browserPageHostName();
    if (mirrorHost.isEmpty()) {
        mirrorHost = QStringLiteral("127.0.0.1");
        qWarning().noquote()
            << "location.hostname is not available; Mirror host falls back to" << mirrorHost;
    }
    mirrorConfig.host = mirrorHost;
    mirrorConfig.port = MirrorPublicPort;
#else
    // Desktop: the Mirror itself stays on loopback (internal port); LanRelay below exposes
    // it on 0.0.0.0:8125. Temporary until wasm-mirror pack 1.0.2 (see App/lanrelay.h).
    mirrorConfig.host = QStringLiteral("127.0.0.1");
    mirrorConfig.port = MirrorInternalPort;
#endif
    mirrorConfig.path = QStringLiteral("/mirror");
    // Empty list = accept every Origin (pack docs wasm-mirror-integration.md §7.5). Intranet
    // system: pages are opened from other computers under their own host names, and Mango
    // decided not to do origin/access control.
    mirrorConfig.allowedOrigins = {};

    WasmMirrorProxyOptions mirrorOptions;
    mirrorOptions.required = true;

#if defined(Q_OS_WASM)
    // Download links (spec §3.5 revised): the Core only sends the path + downloadPort, the
    // page completes it with its own host name. Same source (and fallback) as the Mirror
    // host above. pageHost is STORED false (never mirrored); desktop keeps "".
    Td->setPageHost(mirrorHost);

    // Client session id (history export spec §1): every browser tab gets its own
    // short id before the mirror exists, so export requests and export status are
    // keyed per tab. clientSessionId is STORED false (never mirrored); the desktop
    // keeps the Proxy default "desktop". Kept outside the Proxy composition block on
    // purpose: that block is replaced by core's composition when main is merged.
    Td->setClientSessionId(
        QStringLiteral("web-%1").arg(QRandomGenerator::global()->bounded(0x10000),
                                     4, 16, QLatin1Char('0')));

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

#if !defined(Q_OS_WASM)
    // LAN access (temporary, see App/lanrelay.h): 0.0.0.0:8125 -> 127.0.0.1:18125.
    // Declared after the mirror, so it is destroyed first. A failure (e.g. port 8125 in
    // use) is only logged and the desktop app keeps running; only web pages need the relay.
    LanRelay lanRelay(QHostAddress(QHostAddress::AnyIPv4), MirrorPublicPort,
                      QHostAddress(QHostAddress::LocalHost), MirrorInternalPort);
    QString lanRelayError;
    if (lanRelay.start(&lanRelayError))
        qInfo().noquote() << "LAN relay listening:" << lanRelay.description();
    else
        qWarning().noquote() << lanRelayError;
#endif
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
