// w2-087 test tool (dev-only, not part of the application): reads TaidaFlowProxy::deviceStatus of the
// desktop Core the way the web page does, without a browser. Copied from the w2-084 heartbeat client
// (docs/evidence/w2-084/tools/mirror-heartbeat-client/main.cpp: same hello / snapshot / patch handling)
// and adapted: it prints deviceStatus at the snapshot and at every change, and checks the final map.
//
// It is a Proxy Mirror client like the web page: it builds the contract from Core/TaidaFlowProxy.h with
// the pack's ProxyMirrorClient (so the contract hash must match the running desktop), connects to
// ws://<host>:<port>/mirror, sends proxy.hello, applies the snapshot / patches to a local TaidaFlowProxy
// and prints
//     DEVICESTATUS #<n> [snapshot|patch] {"adam6022":{...},...}
//     BANNER <the text the UI's DeviceStatusUtil.bannerText() rule gives: offline devices in key order>
// Options: --url, --origin, --mirror, --duration-sec (the run, default 70),
//          --expect key=name|address|online (repeatable; online = true|false): at the end the map must
//          have exactly these keys with these values (sinceMs a positive epoch ms not in the future);
//          --max-changes N (default 0 = no limit): at most N changes of the map after the snapshot.
// Exit code: 0 = the final map matches every --expect (and --max-changes); 2 = contract invalid;
// 3 = rejected by the server; 4 = a check failed; 6 = socket error before the snapshot.
#include "TaidaFlowProxy.h"
#include "infrastructure/proxy_mirror/proxymirror.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
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

// The banner rule of TaidaFlowContent/components/DeviceStatusUtil.js (w1-087), re-written here for the
// log only: offline devices (online === false, non-empty name and address) in key order.
QString bannerText(const QVariantMap &map)
{
    QStringList parts;
    for (const QString &key : TaidaFlowProxy::deviceStatusKeys()) {
        const QVariantMap e = map.value(key).toMap();
        const QVariant online = e.value(QStringLiteral("online"));
        const QString name = e.value(QStringLiteral("name")).toString();
        const QString address = e.value(QStringLiteral("address")).toString();
        if (online.typeId() == QMetaType::Bool && !online.toBool() && !name.isEmpty() && !address.isEmpty())
            parts.append(QStringLiteral("%1（%2）").arg(name, address));
    }
    return parts.isEmpty() ? QString() : QStringLiteral("設備離線：") + parts.join(QStringLiteral("、"));
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
    const QCommandLineOption durationOpt(QStringLiteral("duration-sec"), QStringLiteral("run time"), QStringLiteral("s"),
                                         QStringLiteral("70"));
    const QCommandLineOption expectOpt(QStringLiteral("expect"), QStringLiteral("key=name|address|online"), QStringLiteral("spec"));
    const QCommandLineOption maxChangesOpt(QStringLiteral("max-changes"), QStringLiteral("max changes after the snapshot"),
                                           QStringLiteral("n"), QStringLiteral("0"));
    parser.addOptions({urlOpt, originOpt, mirrorOpt, durationOpt, expectOpt, maxChangesOpt});
    parser.process(app);

    const QString mirrorName = parser.value(mirrorOpt);
    const int durationMs = parser.value(durationOpt).toInt() * 1000;
    const int maxChanges = parser.value(maxChangesOpt).toInt();
    QVariantMap expected;     // key -> {name, address, online}
    for (const QString &spec : parser.values(expectOpt)) {
        const QStringList kv = spec.split(QLatin1Char('='));
        const QStringList f = kv.value(1).split(QLatin1Char('|'));
        if (kv.size() != 2 || f.size() != 3) {
            out(QStringLiteral("bad --expect %1").arg(spec));
            return 4;
        }
        expected.insert(kv.at(0), QVariantMap{{QStringLiteral("name"), f.at(0)}, {QStringLiteral("address"), f.at(1)},
                                              {QStringLiteral("online"), f.at(2) == QLatin1String("true")}});
    }

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
    bool finished = false;
    int updates = 0;
    int changesAfterSnapshot = 0;

    QWebSocket ws;
    auto finish = [&](const QString &why) {
        if (finished)
            return;
        finished = true;
        const QVariantMap map = proxy.deviceStatus();
        out(QStringLiteral("finished: %1").arg(why));
        out(QStringLiteral("FINAL %1").arg(compact(map)));
        out(QStringLiteral("FINAL BANNER %1").arg(bannerText(map)));
        int failures = 0;
        const auto check = [&](bool ok, const QString &what) {
            out(QStringLiteral("%1 %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
            failures += ok ? 0 : 1;
        };
        check(snapshotSeen, QStringLiteral("snapshot received"));
        if (!expected.isEmpty()) {
            check(map.keys() == expected.keys(), QStringLiteral("keys %1 (expected %2)")
                                                     .arg(map.keys().join(QLatin1Char(',')), expected.keys().join(QLatin1Char(','))));
            const double now = double(QDateTime::currentMSecsSinceEpoch());
            for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
                const QVariantMap e = map.value(it.key()).toMap();
                const QVariantMap x = it.value().toMap();
                const QVariant online = e.value(QStringLiteral("online"));
                const double since = e.value(QStringLiteral("sinceMs")).toDouble();
                const bool ok = e.value(QStringLiteral("name")).toString() == x.value(QStringLiteral("name")).toString()
                        && e.value(QStringLiteral("address")).toString() == x.value(QStringLiteral("address")).toString()
                        && online.typeId() == QMetaType::Bool && online.toBool() == x.value(QStringLiteral("online")).toBool()
                        && since > 1.7e12 && since <= now && e.size() == 4;
                check(ok, QStringLiteral("%1 = name %2, address %3, online %4, sinceMs %5 (%6)")
                                  .arg(it.key(), e.value(QStringLiteral("name")).toString(),
                                       e.value(QStringLiteral("address")).toString(),
                                       online.toBool() ? QStringLiteral("true") : QStringLiteral("false"))
                                  .arg(qint64(since))
                                  .arg(QDateTime::fromMSecsSinceEpoch(qint64(since)).toString(QStringLiteral("HH:mm:ss.zzz"))));
            }
        }
        if (maxChanges > 0)
            check(changesAfterSnapshot <= maxChanges, QStringLiteral("changes after the snapshot %1 <= %2").arg(changesAfterSnapshot).arg(maxChanges));
        out(QStringLiteral("RESULT updates=%1 changesAfterSnapshot=%2 failures=%3 -> %4")
                    .arg(updates).arg(changesAfterSnapshot).arg(failures)
                    .arg(failures ? QStringLiteral("FAILED") : QStringLiteral("OK")));
        app.exit(failures ? 4 : 0);
    };
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
    QObject::connect(&ws, &QWebSocket::disconnected, &app, [&]() {
        out(QStringLiteral("DISCONNECTED: close code %1 %2").arg(int(ws.closeCode())).arg(ws.closeReason()));
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
        const ProxyMirrorApplyResult result = client.applyStateEnvelope(normalized);
        if (!result.accepted()) {
            out(QStringLiteral("state rejected: %1").arg(result.error));
            finished = true;
            app.exit(3);
            return;
        }
        if (isSnapshot && !snapshotSeen) {
            snapshotSeen = true;
            out(QStringLiteral("DEVICESTATUS #0 [snapshot] %1").arg(compact(proxy.deviceStatus())));
            out(QStringLiteral("BANNER %1").arg(bannerText(proxy.deviceStatus())));
        }
    });
    QObject::connect(&proxy, &TaidaFlowProxy::deviceStatusChanged, &app, [&]() {
        ++updates;
        if (snapshotSeen)
            ++changesAfterSnapshot;
        if (!snapshotSeen)
            return;                                        // printed once the snapshot is applied
        out(QStringLiteral("DEVICESTATUS #%1 [patch] %2").arg(changesAfterSnapshot).arg(compact(proxy.deviceStatus())));
        out(QStringLiteral("BANNER %1").arg(bannerText(proxy.deviceStatus())));
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
