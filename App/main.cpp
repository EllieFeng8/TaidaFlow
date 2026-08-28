// Copyright (C) 2024 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

#include <QApplication>
#include <QQmlApplicationEngine>
#include <cstdlib>
#include <utility>

#include "autogen/environment.h"
#include "Core/TaidaFlowProxy.h"
#include "infrastructure/proxy_mirror/wasmmirrorproxy.h"
#include "core.h"


int main(int argc, char *argv[])
{
    set_qt_environment();
    QApplication app(argc, argv);

    Core& core = Core::instance();
    core.init();
    TaidaFlowProxy* Td = core.m_proxy;
    qmlRegisterSingletonInstance<TaidaFlowProxy>("Core", 1, 0, "Td", Td);


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
