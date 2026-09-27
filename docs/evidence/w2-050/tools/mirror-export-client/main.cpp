// w2-050 test tool (dev-only, not part of the application): requests a history CSV export the
// way the web page does, without a browser.
//
// It is a Proxy Mirror client like the WebAssembly page: it builds the same contract from
// Core/TaidaFlowProxy.h with the pack's ProxyMirrorClient, connects to ws://<host>:8125/mirror
// (the desktop's LAN relay), sends proxy.hello, applies the snapshot / patches to a local
// TaidaFlowProxy, then sends the request signal
//     historyExportRequested(QString sessionId, double fromMs, double toMs)
// and prints every change of historyExportStatus[sessionId] until the job is done / error /
// cancelled. The last line is
//     RESULT state=<state> fileName=<name> url=<url> downloadPort=<port>
// Exit code: 0 = done; 2 = contract invalid; 3 = rejected by the server; 4 = export not done;
// 5 = timeout; 6 = socket error.
#include "TaidaFlowProxy.h"
#include "infrastructure/proxy_mirror/proxymirror.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>

#include <cstdio>

namespace {
constexpr int kWireProtocolVersion = 3;      // WasmMirrorProxy::WireProtocolVersion (pack 1.0.x)

void out(const QString &line)
{
    const QByteArray utf8 = (QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz "))
                             + line + QLatin1Char('\n')).toUtf8();
    std::fwrite(utf8.constData(), 1, size_t(utf8.size()), stdout);
    std::fflush(stdout);
}

QString compact(const QVariantMap &map)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption urlOpt(QStringLiteral("url"), QStringLiteral("Mirror WebSocket URL"), QStringLiteral("url"),
                                    QStringLiteral("ws://127.0.0.1:8125/mirror"));
    const QCommandLineOption originOpt(QStringLiteral("origin"), QStringLiteral("Origin header (the page URL origin)"),
                                       QStringLiteral("origin"), QStringLiteral("http://127.0.0.1:8123"));
    const QCommandLineOption mirrorOpt(QStringLiteral("mirror"), QStringLiteral("mirror name"), QStringLiteral("name"),
                                       QStringLiteral("TaidaFlow"));
    const QCommandLineOption sessionOpt(QStringLiteral("session"), QStringLiteral("export session id"), QStringLiteral("id"),
                                        QStringLiteral("w2050nginx"));
    const QCommandLineOption fromOpt(QStringLiteral("from-ms"), QStringLiteral("range start (ms since epoch)"), QStringLiteral("ms"));
    const QCommandLineOption toOpt(QStringLiteral("to-ms"), QStringLiteral("range end (ms since epoch)"), QStringLiteral("ms"));
    const QCommandLineOption timeoutOpt(QStringLiteral("timeout-sec"), QStringLiteral("overall timeout"), QStringLiteral("s"),
                                        QStringLiteral("120"));
    parser.addOptions({urlOpt, originOpt, mirrorOpt, sessionOpt, fromOpt, toOpt, timeoutOpt});
    parser.process(app);

    const QString mirrorName = parser.value(mirrorOpt);
    const QString sessionId = parser.value(sessionOpt);
    const double nowMs = double(QDateTime::currentMSecsSinceEpoch());
    const double toMs = parser.isSet(toOpt) ? parser.value(toOpt).toDouble() : nowMs;
    const double fromMs = parser.isSet(fromOpt) ? parser.value(fromOpt).toDouble() : toMs - 7.0 * 24 * 3600 * 1000;

    TaidaFlowProxy proxy;
    ProxyMirrorClient client(proxy);
    if (!client.isValid()) {
        out(QStringLiteral("contract invalid: %1").arg(client.validationError()));
        return 2;
    }
    out(QStringLiteral("contractHash=%1").arg(client.contractHash()));
    client.setLocalPropertyWritesEnabled(false);
    client.beginRequestSession();
    client.requireSnapshot();

    const QString requestSessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString connectionSessionId;
    bool requestSent = false;
    QString lastLine;

    QWebSocket ws;
    auto sendJson = [&ws](const QJsonObject &object) {
        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    };

    QObject::connect(&ws, &QWebSocket::connected, &app, [&]() {
        out(QStringLiteral("connected to %1 (origin %2)").arg(parser.value(urlOpt), parser.value(originOpt)));
        sendJson(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("proxy.hello")},
            {QStringLiteral("protocol"), kWireProtocolVersion},
            {QStringLiteral("requestSessionId"), requestSessionId},
            {QStringLiteral("mirrors"), QJsonArray{QJsonObject{
                 {QStringLiteral("mirrorName"), mirrorName},
                 {QStringLiteral("contractHash"), client.contractHash()},
                 {QStringLiteral("required"), true}}}}});
    });
    QObject::connect(&ws, &QWebSocket::errorOccurred, &app, [&](QAbstractSocket::SocketError) {
        out(QStringLiteral("socket error: %1").arg(ws.errorString()));
        app.exit(6);
    });
    QObject::connect(&ws, &QWebSocket::textMessageReceived, &app, [&](const QString &text) {
        const QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
        const QString type = message.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("proxy.welcome")) {
            connectionSessionId = message.value(QStringLiteral("connectionSessionId")).toString();
            out(QStringLiteral("welcome, connectionSessionId=%1").arg(connectionSessionId));
            return;
        }
        if (type == QStringLiteral("proxy.error")) {
            out(QStringLiteral("server error: %1 %2").arg(message.value(QStringLiteral("code")).toString(),
                                                         message.value(QStringLiteral("message")).toString()));
            app.exit(3);
            return;
        }
        if (type != QStringLiteral("proxy.snapshot") && type != QStringLiteral("proxy.patch"))
            return;
        QJsonObject normalized = message;                  // wire -> host protocol (pack normalizeEnvelope)
        normalized.insert(QStringLiteral("protocol"), ProxyMirrorHost::ProtocolVersion);
        normalized.remove(QStringLiteral("mirrorName"));
        normalized.remove(QStringLiteral("connectionSessionId"));
        const ProxyMirrorApplyResult result = client.applyStateEnvelope(normalized);
        if (!result.accepted()) {
            out(QStringLiteral("state rejected: %1").arg(result.error));
            app.exit(3);
            return;
        }
        if (requestSent || type != QStringLiteral("proxy.snapshot"))
            return;
        QString error;
        QJsonObject envelope = client.makeSignalEnvelope(
                QStringLiteral("historyExportRequested(QString,double,double)"),
                QVariantList{sessionId, fromMs, toMs}, &error);
        if (envelope.isEmpty()) {
            out(QStringLiteral("signal envelope failed: %1").arg(error));
            app.exit(2);
            return;
        }
        envelope.insert(QStringLiteral("protocol"), kWireProtocolVersion);   // pack decorateEnvelope
        envelope.insert(QStringLiteral("mirrorName"), mirrorName);
        envelope.insert(QStringLiteral("requestSessionId"), requestSessionId);
        envelope.insert(QStringLiteral("connectionSessionId"), connectionSessionId);
        sendJson(envelope);
        requestSent = true;
        out(QStringLiteral("sent historyExportRequested(\"%1\", %2, %3) = %4 .. %5")
                    .arg(sessionId).arg(qint64(fromMs)).arg(qint64(toMs))
                    .arg(QDateTime::fromMSecsSinceEpoch(qint64(fromMs)).toString(Qt::ISODate),
                         QDateTime::fromMSecsSinceEpoch(qint64(toMs)).toString(Qt::ISODate)));
    });
    QObject::connect(&proxy, &TaidaFlowProxy::historyExportStatusChanged, &app, [&](const QVariantMap &status) {
        const QVariantMap entry = status.value(sessionId).toMap();
        if (entry.isEmpty() || !requestSent)
            return;
        const QString line = compact(entry);
        if (line != lastLine) {
            out(QStringLiteral("STATUS %1").arg(line));
            lastLine = line;
        }
        const QString state = entry.value(QStringLiteral("state")).toString();
        if (state == QStringLiteral("done") || state == QStringLiteral("error") || state == QStringLiteral("cancelled")) {
            out(QStringLiteral("RESULT state=%1 fileName=%2 url=%3 downloadPort=%4")
                        .arg(state, entry.value(QStringLiteral("fileName")).toString(),
                             entry.value(QStringLiteral("url")).toString(),
                             entry.value(QStringLiteral("downloadPort")).toString()));
            ws.close();
            app.exit(state == QStringLiteral("done") ? 0 : 4);
        }
    });
    QTimer::singleShot(parser.value(timeoutOpt).toInt() * 1000, &app, [&]() {
        out(QStringLiteral("timeout"));
        app.exit(5);
    });

    QNetworkRequest request{QUrl(parser.value(urlOpt))};
    request.setRawHeader("Origin", parser.value(originOpt).toUtf8());
    ws.open(request);
    return app.exec();
}
