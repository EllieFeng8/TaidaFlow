// w2-084 test tool (dev-only, not part of the application): watches the desktop Core's server
// heartbeat the way the WebAssembly page does, without a browser. Adapted from the w2-080 alarm
// client (docs/evidence/w2-080/tools/mirror-alarm-client/main.cpp: same hello / snapshot / patch
// handling, request part removed).
//
// It is a Proxy Mirror client like the web page: it builds the contract from Core/TaidaFlowProxy.h
// with the pack's ProxyMirrorClient (so the contract hash must match the running desktop), connects
// to ws://<host>:<port>/mirror, sends proxy.hello, applies the snapshot / patches to a local
// TaidaFlowProxy and prints every change of serverHeartbeatMs:
//     BEAT #<n> value=<epoch ms> (<local time>) valueStep=<ms> recvStep=<ms>
// and, when the connection ends:
//     DISCONNECTED after <s> s: <close code> <reason>
// then a summary line
//     RESULT beats=<n> firstValue=<v> lastValue=<v> valueStep[min..max]=<a>..<b> recvGapMax=<ms> disconnected=<yes|no>
// Options: --url, --origin, --mirror, --duration-sec (overall limit, default 180), --min-beats
// (default 30), --expect-disconnect (the run must end by the server closing the connection).
// Exit code: 0 = at least --min-beats changes, every value step 900..1200 ms, no receive gap over
// 2500 ms, and (with --expect-disconnect) the server closed the connection; 2 = contract invalid;
// 3 = rejected by the server; 4 = a check failed; 6 = socket error before the snapshot.
#include "TaidaFlowProxy.h"
#include "infrastructure/proxy_mirror/proxymirror.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>

#include <algorithm>
#include <cstdio>
#include <limits>

namespace {
constexpr int kWireProtocolVersion = 3;      // WasmMirrorProxy::WireProtocolVersion (pack 1.0.x)

void out(const QString &line)
{
    const QByteArray utf8 = (QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz "))
                             + line + QLatin1Char('\n')).toUtf8();
    std::fwrite(utf8.constData(), 1, size_t(utf8.size()), stdout);
    std::fflush(stdout);
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
                                       QStringLiteral("origin"), QStringLiteral("http://127.0.0.1:8124"));
    const QCommandLineOption mirrorOpt(QStringLiteral("mirror"), QStringLiteral("mirror name"), QStringLiteral("name"),
                                       QStringLiteral("TaidaFlow"));
    const QCommandLineOption durationOpt(QStringLiteral("duration-sec"), QStringLiteral("overall limit"), QStringLiteral("s"),
                                         QStringLiteral("180"));
    const QCommandLineOption minBeatsOpt(QStringLiteral("min-beats"), QStringLiteral("changes needed"), QStringLiteral("n"),
                                         QStringLiteral("30"));
    const QCommandLineOption expectDisconnectOpt(QStringLiteral("expect-disconnect"),
                                                 QStringLiteral("the server must close the connection"));
    parser.addOptions({urlOpt, originOpt, mirrorOpt, durationOpt, minBeatsOpt, expectDisconnectOpt});
    parser.process(app);

    const QString mirrorName = parser.value(mirrorOpt);
    const int durationMs = parser.value(durationOpt).toInt() * 1000;
    const int minBeats = parser.value(minBeatsOpt).toInt();
    const bool expectDisconnect = parser.isSet(expectDisconnectOpt);

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
    bool snapshotSeen = false;
    bool disconnected = false;
    bool finished = false;
    int beats = 0;
    double firstValue = 0;
    double lastValue = 0;
    double minStep = std::numeric_limits<double>::max();
    double maxStep = 0;
    qint64 lastRecvMs = -1;
    qint64 maxRecvGap = 0;
    QElapsedTimer sinceConnect;
    QElapsedTimer recvClock;
    recvClock.start();

    QWebSocket ws;
    auto finish = [&](const QString &why) {
        if (finished)
            return;
        finished = true;
        const bool stepsOk = beats < 2 || (minStep >= 900 && maxStep <= 1200);
        const bool gapOk = maxRecvGap <= 2500;
        const bool ok = beats >= minBeats && stepsOk && gapOk && (!expectDisconnect || disconnected);
        out(QStringLiteral("finished: %1").arg(why));
        out(QStringLiteral("RESULT beats=%1 firstValue=%2 lastValue=%3 valueStep[min..max]=%4..%5 recvGapMax=%6 "
                           "disconnected=%7 -> %8")
                    .arg(beats).arg(qint64(firstValue)).arg(qint64(lastValue))
                    .arg(beats >= 2 ? qint64(minStep) : 0).arg(beats >= 2 ? qint64(maxStep) : 0).arg(maxRecvGap)
                    .arg(disconnected ? QStringLiteral("yes") : QStringLiteral("no"))
                    .arg(ok ? QStringLiteral("OK") : QStringLiteral("FAILED (need beats >= %1, steps 900..1200, gap <= 2500%2)")
                                                              .arg(minBeats)
                                                              .arg(expectDisconnect ? QStringLiteral(", disconnect") : QString())));
        app.exit(ok ? 0 : 4);
    };
    auto sendJson = [&ws](const QJsonObject &object) {
        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    };

    auto onValue = [&](double value, const char *source) {
        const qint64 now = recvClock.elapsed();
        ++beats;
        QString steps;
        if (beats == 1) {
            firstValue = value;
        } else {
            const double step = value - lastValue;
            minStep = std::min(minStep, step);
            maxStep = std::max(maxStep, step);
            maxRecvGap = std::max(maxRecvGap, now - lastRecvMs);
            steps = QStringLiteral(" valueStep=%1 recvStep=%2").arg(qint64(step)).arg(now - lastRecvMs);
        }
        lastValue = value;
        lastRecvMs = now;
        out(QStringLiteral("BEAT #%1 value=%2 (%3)%4 [%5]")
                    .arg(beats).arg(qint64(value))
                    .arg(QDateTime::fromMSecsSinceEpoch(qint64(value)).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                         steps, QLatin1String(source)));
    };

    QObject::connect(&ws, &QWebSocket::connected, &app, [&]() {
        sinceConnect.start();
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
    QObject::connect(&ws, &QWebSocket::disconnected, &app, [&]() {
        disconnected = true;
        out(QStringLiteral("DISCONNECTED after %1 s: close code %2 %3 (last heartbeat %4 ms ago)")
                    .arg(sinceConnect.isValid() ? sinceConnect.elapsed() / 1000.0 : 0.0, 0, 'f', 1)
                    .arg(int(ws.closeCode())).arg(ws.closeReason())
                    .arg(lastRecvMs < 0 ? -1 : recvClock.elapsed() - lastRecvMs));
        finish(QStringLiteral("connection closed by the server"));
    });
    QObject::connect(&ws, &QWebSocket::errorOccurred, &app, [&](QAbstractSocket::SocketError error) {
        out(QStringLiteral("socket error %1: %2").arg(int(error)).arg(ws.errorString()));
        if (!snapshotSeen && !finished) {
            finished = true;
            app.exit(6);
        }
    });
    QObject::connect(&ws, &QWebSocket::textMessageReceived, &app, [&](const QString &text) {
        const QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
        const QString type = message.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("proxy.welcome")) {
            out(QStringLiteral("welcome, connectionSessionId=%1")
                        .arg(message.value(QStringLiteral("connectionSessionId")).toString()));
            return;
        }
        if (type == QStringLiteral("proxy.error")) {
            out(QStringLiteral("server error: %1 %2").arg(message.value(QStringLiteral("code")).toString(),
                                                         message.value(QStringLiteral("message")).toString()));
            finished = true;
            app.exit(3);
            return;
        }
        if (type != QStringLiteral("proxy.snapshot") && type != QStringLiteral("proxy.patch"))
            return;
        QJsonObject normalized = message;                  // wire -> host protocol (pack normalizeEnvelope)
        normalized.insert(QStringLiteral("protocol"), ProxyMirrorHost::ProtocolVersion);
        normalized.remove(QStringLiteral("mirrorName"));
        normalized.remove(QStringLiteral("connectionSessionId"));
        const bool isSnapshot = type == QStringLiteral("proxy.snapshot");
        const double before = proxy.serverHeartbeatMs();
        const ProxyMirrorApplyResult result = client.applyStateEnvelope(normalized);
        if (!result.accepted()) {
            out(QStringLiteral("state rejected: %1").arg(result.error));
            finished = true;
            app.exit(3);
            return;
        }
        if (isSnapshot && !snapshotSeen) {
            snapshotSeen = true;
            out(QStringLiteral("snapshot: serverHeartbeatMs=%1 (before snapshot %2)")
                        .arg(qint64(proxy.serverHeartbeatMs())).arg(qint64(before)));
        }
    });
    QObject::connect(&proxy, &TaidaFlowProxy::serverHeartbeatMsChanged, &app, [&]() {
        onValue(proxy.serverHeartbeatMs(), snapshotSeen ? "patch" : "snapshot");
    });

    QNetworkRequest request{QUrl(parser.value(urlOpt))};
    request.setRawHeader("Origin", parser.value(originOpt).toUtf8());
    ws.open(request);
    QTimer::singleShot(durationMs, &app, [&]() {
        finish(QStringLiteral("--duration-sec reached"));
        ws.close();
    });
    return app.exec();
}
