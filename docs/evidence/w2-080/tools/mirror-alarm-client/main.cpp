// w2-080 test tool (dev-only, not part of the application): asks the desktop Core for this
// client's alarm view the way AlarmPage.qml does, without a browser. Adapted from the w2-050
// export client (docs/evidence/w2-050/tools/mirror-export-client/main.cpp).
//
// It is a Proxy Mirror client like the WebAssembly page: it builds the same contract from
// Core/TaidaFlowProxy.h with the pack's ProxyMirrorClient, connects to ws://<host>:<port>/mirror,
// sends proxy.hello, applies the snapshot / patches to a local TaidaFlowProxy, then sends the
// request signal
//     alarmViewRequested(QString sessionId, double fromMs, double toMs, int page)
// once per --req (in order; the next one after the previous answer, or after --step-sec without
// an answer = identical entry) and prints every change of alarmViews[sessionId]:
//     ENTRY rev=<r> state=<s> page=<p>/<n> total=<t> active=<a> rows=<k> range=<from>..<to> message=<m>
//     ROW <serialNumber> <alarmTime> <sensorName> <alarmStatus> <severity> <alarmMessage>
//     JSON <the whole entry>
// then keeps listening --linger-sec (live updates) and exits.
//   --req <fromMs>:<toMs>:<page>   explicit range (both inclusive, local-epoch ms)
//   --req default:<page>           the page's default "last 24 hours" (AlarmViewUtil.defaultRange)
// Exit code: 0 = every request answered (or identical) and at least one entry seen;
// 2 = contract invalid; 3 = rejected by the server; 5 = no entry at all; 6 = socket error.
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

struct Request { double fromMs = 0; double toMs = 0; int page = 1; };

// AlarmViewUtil.defaultRange(now): (now - 24 h) minute :00.000 .. now minute :59.999.
Request defaultRange(int page)
{
    const QDateTime now = QDateTime::currentDateTime();
    QDateTime start = now.addMSecs(-24LL * 3600 * 1000);
    start.setTime(QTime(start.time().hour(), start.time().minute(), 0, 0));
    QDateTime end = now;
    end.setTime(QTime(now.time().hour(), now.time().minute(), 59, 999));
    return Request{double(start.toMSecsSinceEpoch()), double(end.toMSecsSinceEpoch()), page};
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
    const QCommandLineOption sessionOpt(QStringLiteral("session"), QStringLiteral("clientSessionId"), QStringLiteral("id"),
                                        QStringLiteral("web-w2080"));
    const QCommandLineOption reqOpt(QStringLiteral("req"), QStringLiteral("request fromMs:toMs:page or default:page"),
                                    QStringLiteral("spec"));
    const QCommandLineOption stepOpt(QStringLiteral("step-sec"), QStringLiteral("wait per request"), QStringLiteral("s"),
                                     QStringLiteral("5"));
    const QCommandLineOption lingerOpt(QStringLiteral("linger-sec"), QStringLiteral("listen after the last request"),
                                       QStringLiteral("s"), QStringLiteral("0"));
    parser.addOptions({urlOpt, originOpt, mirrorOpt, sessionOpt, reqOpt, stepOpt, lingerOpt});
    parser.process(app);

    QList<Request> requests;
    const QStringList specs = parser.values(reqOpt).isEmpty() ? QStringList{QStringLiteral("default:1")}
                                                              : parser.values(reqOpt);
    for (const QString &spec : specs) {
        const QStringList parts = spec.split(QLatin1Char(':'));
        if (parts.size() == 2 && parts.at(0) == QStringLiteral("default"))
            requests << defaultRange(parts.at(1).toInt());
        else if (parts.size() == 3)
            requests << Request{parts.at(0).toDouble(), parts.at(1).toDouble(), parts.at(2).toInt()};
        else {
            out(QStringLiteral("bad --req %1").arg(spec));
            return 2;
        }
    }

    const QString mirrorName = parser.value(mirrorOpt);
    const QString sessionId = parser.value(sessionOpt);
    const int stepMs = parser.value(stepOpt).toInt() * 1000;
    const int lingerMs = parser.value(lingerOpt).toInt() * 1000;

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
    bool snapshotSeen = false;
    int next = 0;                  // index of the next request to send
    int entriesSeen = 0;
    QVariant lastRevision;
    QTimer stepTimer;
    stepTimer.setSingleShot(true);

    QWebSocket ws;
    auto sendJson = [&ws](const QJsonObject &object) {
        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    };
    std::function<void()> sendNext;
    auto finishOrLinger = [&]() {
        out(QStringLiteral("all %1 request(s) sent; listening %2 s for live updates").arg(requests.size()).arg(lingerMs / 1000));
        QTimer::singleShot(lingerMs, &app, [&]() {
            out(QStringLiteral("RESULT entries=%1").arg(entriesSeen));
            ws.close();
            app.exit(entriesSeen > 0 ? 0 : 5);
        });
    };
    sendNext = [&]() {
        if (next >= requests.size()) {
            finishOrLinger();
            return;
        }
        const Request r = requests.at(next++);
        QString error;
        QJsonObject envelope = client.makeSignalEnvelope(
                QStringLiteral("alarmViewRequested(QString,double,double,int)"),
                QVariantList{sessionId, r.fromMs, r.toMs, r.page}, &error);
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
        out(QStringLiteral("SENT alarmViewRequested(\"%1\", %2, %3, %4) = %5 .. %6")
                    .arg(sessionId).arg(qint64(r.fromMs)).arg(qint64(r.toMs)).arg(r.page)
                    .arg(QDateTime::fromMSecsSinceEpoch(qint64(r.fromMs)).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                         QDateTime::fromMSecsSinceEpoch(qint64(r.toMs)).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))));
        stepTimer.start(stepMs);
    };
    QObject::connect(&stepTimer, &QTimer::timeout, &app, [&]() {
        out(QStringLiteral("no change of the entry within %1 s (identical content keeps its revision)").arg(stepMs / 1000));
        sendNext();
    });

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
        if (type == QStringLiteral("proxy.snapshot") && !snapshotSeen) {
            snapshotSeen = true;
            out(QStringLiteral("snapshot: alarmViews has %1 entr%2 (%3); alarmRecords %4 row(s)")
                        .arg(proxy.alarmViews().size())
                        .arg(proxy.alarmViews().size() == 1 ? QStringLiteral("y") : QStringLiteral("ies"))
                        .arg(proxy.alarmViews().keys().join(QStringLiteral(", ")))
                        .arg(proxy.alarmRecords().size()));
            lastRevision = proxy.alarmViews().value(sessionId).toMap().value(QStringLiteral("revision"));
            sendNext();
        }
    });
    QObject::connect(&proxy, &TaidaFlowProxy::alarmViewsChanged, &app, [&](const QVariantMap &views) {
        const QVariantMap entry = views.value(sessionId).toMap();
        if (!snapshotSeen || entry.isEmpty())
            return;
        const QVariant revision = entry.value(QStringLiteral("revision"));
        if (revision == lastRevision)
            return;                                          // another client's entry changed
        lastRevision = revision;
        ++entriesSeen;
        out(QStringLiteral("ENTRY rev=%1 state=%2 page=%3/%4 total=%5 active=%6 rows=%7 range=%8..%9 message=%10")
                    .arg(revision.toString(), entry.value(QStringLiteral("state")).toString())
                    .arg(entry.value(QStringLiteral("page")).toInt())
                    .arg(entry.value(QStringLiteral("totalPages")).toInt())
                    .arg(entry.value(QStringLiteral("totalCount")).toLongLong())
                    .arg(entry.value(QStringLiteral("activeCount")).toLongLong())
                    .arg(entry.value(QStringLiteral("rows")).toList().size())
                    .arg(qint64(entry.value(QStringLiteral("fromMs")).toDouble()))
                    .arg(qint64(entry.value(QStringLiteral("toMs")).toDouble()))
                    .arg(entry.value(QStringLiteral("message")).toString()));
        for (const QVariant &v : entry.value(QStringLiteral("rows")).toList()) {
            const QVariantMap row = v.toMap();
            out(QStringLiteral("ROW %1 %2 %3 %4 %5 %6")
                        .arg(row.value(QStringLiteral("serialNumber")).toLongLong())
                        .arg(row.value(QStringLiteral("alarmTime")).toString(),
                             row.value(QStringLiteral("sensorName")).toString(),
                             row.value(QStringLiteral("alarmStatus")).toString(),
                             row.value(QStringLiteral("severity")).toString(),
                             row.value(QStringLiteral("alarmMessage")).toString()));
        }
        out(QStringLiteral("JSON %1").arg(compact(entry)));
        if (stepTimer.isActive()) {
            stepTimer.stop();
            sendNext();
        }
    });

    QNetworkRequest request{QUrl(parser.value(urlOpt))};
    request.setRawHeader("Origin", parser.value(originOpt).toUtf8());
    ws.open(request);
    QTimer::singleShot(10 * 60 * 1000, &app, [&]() { out(QStringLiteral("overall timeout")); app.exit(5); });
    return app.exec();
}
