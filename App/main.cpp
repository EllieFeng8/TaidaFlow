// Copyright (C) 2024 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QQmlApplicationEngine>
#include <QRandomGenerator>
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
#endif

#if defined(Q_OS_WASM)
#include <emscripten/val.h>
#include <string>
#include "runtimeinfo.h"
#else
#include <QDir>
#include <QHostAddress>
#include "lanrelay.h"
#include "appconfig.h"
#include "applog.h"
#include <QMessageBox>
#include <QTimer>
#endif

namespace {

// Mirror ports (docs/taidaflow_config_spec.md §2 "mirror", §3):
// - desktop: config.json mirror.internalPort (Mirror server on 127.0.0.1, default 18125) and
//   mirror.publicBind/publicPort (LanRelay, default 0.0.0.0:8125), see AppConfig;
// - WebAssembly: mirrorPublicPort from the page's own /runtime.json, fallback 8125
//   (TaidaFlowRuntime::kDefaultMirrorPublicPort).

#if !defined(Q_OS_WASM)
// config.json cannot be used (spec §1.4): tell the operator, then main exits non-zero.
// TAIDAFLOW_CONFIG_ERROR_DIALOG_TIMEOUT_MS=<ms> (unattended checks only) closes the dialog
// automatically after that time; unset = it stays until the operator closes it.
void showConfigErrorDialog(const QString &text)
{
    QMessageBox box(QMessageBox::Critical, QStringLiteral("TaidaFlow - config.json"), text,
                    QMessageBox::Ok);
    bool timeoutOk = false;
    const int timeoutMs = qEnvironmentVariableIntValue("TAIDAFLOW_CONFIG_ERROR_DIALOG_TIMEOUT_MS", &timeoutOk);
    if (timeoutOk && timeoutMs > 0) {
        QTimer::singleShot(timeoutMs, &box, [&box, timeoutMs] {
            qWarning().noquote() << "[Config] error dialog closed automatically after" << timeoutMs
                                 << "ms (TAIDAFLOW_CONFIG_ERROR_DIALOG_TIMEOUT_MS)";
            box.done(QMessageBox::Ok);
        });
    }
    qWarning().noquote() << "[Config] showing the config.json error dialog";
    box.exec();
}
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

// <page URL>/runtime.json, resolved against window.location.href (same origin as the page,
// e.g. http://192.168.1.20/runtime.json). Invalid QUrl when the location cannot be read;
// requestRuntimeInfo() then falls back to port 8125.
QUrl browserRuntimeInfoUrl()
{
    const emscripten::val location = emscripten::val::global("location");
    if (location.isUndefined() || location.isNull())
        return {};
    const emscripten::val href = location["href"];
    if (!href.isString())
        return {};
    const QUrl page(QString::fromStdString(href.as<std::string>()));
    if (!page.isValid() || page.isRelative())
        return {};
    return page.resolved(QUrl(QStringLiteral("/runtime.json")));
}
#endif

// Creates the Mirror runtime for the Td Proxy: desktop = server bound to host:port,
// WebAssembly = client connecting to host:port. Logs and returns nullptr on failure.
std::unique_ptr<WasmMirrorProxy> createMirror(TaidaFlowProxy *Td, const QString &host, quint16 port)
{
    WasmMirrorConfig mirrorConfig;
    mirrorConfig.host = host;
    mirrorConfig.port = port;
    mirrorConfig.path = QStringLiteral("/mirror");
    // Empty list = accept every Origin (pack docs wasm-mirror-integration.md §7.5). Intranet
    // system: pages are opened from other computers under their own host names, and Mango
    // decided not to do origin/access control.
    mirrorConfig.allowedOrigins = {};

    WasmMirrorProxyOptions mirrorOptions;
    mirrorOptions.required = true;
#if defined(Q_OS_WASM)
    // Keeps the transport overlay (set to "not ready" in main before QML loads) in sync
    // with the mirror client (offline / synchronizing / ready).
    mirrorOptions.transportStateHandler =
        [Td](bool ready, const QString &message) {
            Td->setTransportState(ready, message);
        };
#endif

    if (!mirrorConfig.addProxy(QStringLiteral("TaidaFlow"),
                               *Td,
                               std::move(mirrorOptions))) {
        qCritical().noquote() << mirrorConfig.validationError();
        return nullptr;
    }

    QString mirrorError;
    auto mirror = WasmMirrorProxy::create(mirrorConfig, &mirrorError);
    if (!mirror) {
        qCritical().noquote() << mirrorError;
        return nullptr;
    }

    qInfo().noquote()
        << "WASM Mirror endpoint:"
        << mirror->webSocketUrl().toString();

    // Transport diagnostics (desktop: server listening; WASM: snapshot ready /
    // offline). On WebAssembly these lines appear in the browser console.
    QObject::connect(mirror.get(), &WasmMirrorProxy::readyChanged, mirror.get(),
                     [](bool ready) {
        qInfo().noquote() << "WASM Mirror ready:" << (ready ? "true" : "false");
    });
    QObject::connect(mirror.get(), &WasmMirrorProxy::transportError, mirror.get(),
                     [](const QString &mirrorName, const QString &message) {
        qInfo().noquote() << "WASM Mirror transport:" << mirrorName << message;
    });
    return mirror;
}

// ===== UI font (w1-081) ======================================================
// One interface font for all text. QGuiApplication::setFont() makes it the default font of
// every Text / TextInput / FontMetrics that does not name a family. QML reads the same value
// as Application.font.family: App.qml puts it in its (template) ApplicationWindow font, from
// which every Qt Quick Control and popup inherits it (the Universal style alone would use
// Segoe UI), and HistoryPage names it for its text column. Only changing / aligned numbers
// keep font.family "Consolas" in QML; the family is also put first in the substitution list
// of "Consolas", so Chinese inside those numbers is drawn with the same Chinese font instead
// of the platform's monospace CJK fallback.
// Returns the family in use: `requested` when it is installed; otherwise the platform
// default is kept unchanged and returned (e.g. main on WebAssembly, which has no
// Microsoft JhengHei UI and no embedded font).
QString applyUiFont(const QString &requested)
{
    if (requested.isEmpty() || !QFontDatabase::hasFamily(requested)) {
        const QString current = QGuiApplication::font().family();
        qWarning().noquote() << "[UiFont]" << requested
                             << "is not available; keeping the default font" << current;
        return current;
    }
    QFont uiFont = QGuiApplication::font(); // keeps the platform point size
    uiFont.setFamilies({requested});
    QGuiApplication::setFont(uiFont);
    QFont::insertSubstitution(QStringLiteral("Consolas"), requested);
    qInfo().noquote() << "[UiFont] interface font:" << requested << "| Consolas ->"
                      << QFont::substitutes(QStringLiteral("Consolas")).join(QLatin1Char(','));
    return requested;
}
// ===== end of UI font ========================================================

} // namespace

int main(int argc, char *argv[])
{
#if !defined(Q_OS_WASM)
    // Log files (docs/taidaflow_config_spec.md §2 "log", App/applog.h): from here on every
    // message is kept in memory (and still printed as before) until AppLog::install() below
    // writes it to today's files, so the config.json messages are in the files too. Declared
    // first: destroyed last (flush, close, previous message handler back).
    AppLog::Scope appLogScope;
#endif
    set_qt_environment();
    QApplication app(argc, argv);
#if !defined(Q_OS_WASM)
    // config.json (docs/taidaflow_config_spec.md §1), before anything that writes a file:
    // `--write-default-config <path>` only writes the default file and exits (scripts);
    // otherwise load it (TAIDAFLOW_CONFIG -> <exe folder>/config.json -> create defaults),
    // exit non-zero if it is not valid JSON, and switch to dataDir (relative to config.json).
    if (const std::optional<int> writeExitCode = AppConfig::runWriteDefaultConfigCommand(app.arguments()))
        return *writeExitCode;
    {
        const AppConfig::LoadResult configLoad = AppConfig::loadFromEnvironment();
        configLoad.printLog();
        if (!configLoad.ok) {
            // w2-064 A2: log files in <folder of config.json>\logs (buffered messages + the
            // failure), dialog and stderr name the log file; exit code 2 as before.
            return AppLog::handleConfigFailure(configLoad, showConfigErrorDialog);
        }
        AppConfig::setInstance(configLoad.config);
    }
    AppConfig::instance().applyDataDir();
    // log.dir (relative to dataDir, created when missing): today's quiet / full files, expired
    // files deleted; the messages buffered since the start of main() are written first.
    {
        const AppConfig::LogSettings logSettings = AppConfig::instance().logSettings();
        const bool logFilesOpen = AppLog::install(logSettings);
        qInfo().noquote() << AppLog::describe(logSettings);
        if (!logFilesOpen) {
            qWarning().noquote() << "[AppLog] not every enabled log file could be opened in"
                                 << QDir::toNativeSeparators(logSettings.dir)
                                 << "- see stderr; messages still go to stderr, the files are retried";
        }
    }
    const AppConfig::MirrorSettings mirrorPorts = AppConfig::instance().mirror();
#endif

    // (core only) Register the embedded Noto Sans TC subset (App/fonts, OFL-1.1) on every
    // platform: it is the interface font of the desktop AND the web page (HOOK below ->
    // applyUiFont(): application font, ApplicationWindow font, CJK of "Consolas" numbers).
    // WebAssembly has no system fonts, so there it is also made the fallback of every other
    // family the UI may name (Segoe UI, Arial, ..., Han / Common script) and "Consolas" becomes
    // DejaVu Sans Mono (monospace digits, bundled with Qt for WebAssembly) -> Noto Sans TC;
    // applyUiFont() runs after it and only adds the application font. The desktop needs no chain.
    const QString cjkFamily = TaidaFlowFonts::loadEmbeddedCjkFont();
#if defined(Q_OS_WASM)
    TaidaFlowFonts::installCjkFallbackChain(cjkFamily);
#endif

    // ===== Proxy composition: the ONLY branch-specific block =====================
    // core branch composition (wasm-mirror pack 1.0.1, package-integration §7,
    // guide §10): the desktop owns the authoritative Core (Modbus/MS300/REST/SQLite)
    // and uses its Proxy; WebAssembly only builds a plain Proxy, whose constructor
    // has no hardware side effect, so no MirrorReplica mode is needed.
    // The owner is declared before the mirror so that the Proxy outlives it.
    std::unique_ptr<TaidaFlowProxy> wasmProxyOwner;
    TaidaFlowProxy *Td = nullptr;
#if defined(Q_OS_WASM)
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
    // ===== end of Proxy composition ==============================================

    // The manual registration URI must not equal any qt_add_qml_module(URI ...)
    // (wasm-mirror package-integration §7, guide §8). Core/CMakeLists.txt owns URI
    // "Core", so the Td singleton lives in its own URI "TaidaFlowBackend"; QML
    // imports `TaidaFlowBackend 1.0`.
    qmlRegisterSingletonInstance<TaidaFlowProxy>("TaidaFlowBackend", 1, 0, "Td", Td);

    // Declared after the Proxy owner and before the engine: destroyed after the engine and
    // before the Proxy (pack guide §9).
    std::unique_ptr<WasmMirrorProxy> mirror;

#if defined(Q_OS_WASM)
    // LAN access: connect back to the host the page was loaded from (location.hostname).
    // Fallback 127.0.0.1 (the previous fixed host) if it cannot be read.
    QString mirrorHost = browserPageHostName();
    if (mirrorHost.isEmpty()) {
        mirrorHost = QStringLiteral("127.0.0.1");
        qWarning().noquote()
            << "location.hostname is not available; Mirror host falls back to" << mirrorHost;
    }

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
    // composition root switches it to false before QML loads, then the mirror's
    // transportStateHandler keeps it in sync with the mirror client (createMirror()).
    Td->setTransportState(
        false, QStringLiteral("Connecting to the Qt desktop Core..."));

    // Mirror port (config spec §3): asynchronous GET of the page's own /runtime.json, then
    // the Mirror client is created. QML loads meanwhile (the offline overlay stays up until
    // the mirror is ready). No route / error / >3 s / invalid content -> port 8125 + console
    // warning (main alone has no /runtime.json route: always the fallback).
    // `mirror` lives in main's scope for the whole run (WebAssembly: app.exec() never
    // returns), like the engine below.
    TaidaFlowRuntime::requestRuntimeInfo(
        browserRuntimeInfoUrl(), TaidaFlowRuntime::kRuntimeRequestTimeoutMs, &app,
        [Td, mirrorHost, &mirror](const TaidaFlowRuntime::RuntimeLookup &lookup) {
            if (lookup.fromServer) {
                qInfo().noquote() << "/runtime.json: mirrorPublicPort =" << lookup.info.mirrorPublicPort;
            } else {
                qWarning().noquote() << "/runtime.json not usable (" + lookup.detail + "); Mirror port falls back to"
                                     << lookup.info.mirrorPublicPort;
            }
            mirror = createMirror(Td, mirrorHost, lookup.info.mirrorPublicPort);
            if (!mirror) {
                Td->setTransportState(
                    false, QStringLiteral("Mirror client could not be created (see the browser console)."));
            }
        });
#else
    // Desktop: the Mirror itself stays on loopback (config.json mirror.internalPort);
    // LanRelay below exposes it on mirror.publicBind:publicPort. Temporary until wasm-mirror
    // pack 1.0.2 (see App/lanrelay.h).
    mirror = createMirror(Td, QStringLiteral("127.0.0.1"), mirrorPorts.internalPort);
    if (!mirror)
        return EXIT_FAILURE;

    // LAN access (temporary, see App/lanrelay.h): publicBind:publicPort -> 127.0.0.1:internalPort
    // (defaults 0.0.0.0:8125 -> 127.0.0.1:18125). Declared after the mirror, so it is destroyed
    // first. A failure (e.g. the port in use) is only logged and the desktop app keeps running;
    // only web pages need the relay.
    LanRelay lanRelay(QHostAddress(mirrorPorts.publicBind), mirrorPorts.publicPort,
                      QHostAddress(QHostAddress::LocalHost), mirrorPorts.internalPort);
    QString lanRelayError;
    if (lanRelay.start(&lanRelayError))
        qInfo().noquote() << "LAN relay listening:" << lanRelay.description();
    else
        qWarning().noquote() << lanRelayError;
#endif
    const auto logTransportOverlay = [Td] {
        qInfo().noquote() << "Transport overlay: transportReady ="
                          << (Td->transportReady() ? "true" : "false")
                          << "message =" << Td->transportMessage();
    };
    QObject::connect(Td, &TaidaFlowProxy::transportReadyChanged, &app, logTransportOverlay);
    QObject::connect(Td, &TaidaFlowProxy::transportMessageChanged, &app, logTransportOverlay);

    // ===== UI font (w1-081), before QML loads; see applyUiFont() =================
    // HOOK (core): main has no font file, so it asks for the Windows font below. core
    // changes ONLY this line to its embedded Noto Sans TC family, i.e.
    //   const QString uiFontFamilyRequest = cjkFamily.isEmpty() ? QStringLiteral("Microsoft JhengHei UI") : cjkFamily;
    // (cjkFamily = TaidaFlowFonts::loadEmbeddedCjkFont(), already loaded earlier in core).
    const QString uiFontFamilyRequest = cjkFamily.isEmpty() ? QStringLiteral("Microsoft JhengHei UI") : cjkFamily;
    applyUiFont(uiFontFamilyRequest); // QML reads the result as Application.font.family
    // ===== end of UI font ========================================================

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
