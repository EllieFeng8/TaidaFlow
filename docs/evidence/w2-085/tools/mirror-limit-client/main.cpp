// w2-085 test tool (dev-only, not part of the application): changes the settings-page limits of the
// running desktop Core through the Proxy Mirror (the way the settings page of a web client does: copy
// sensorSettingsSv, replace one entry, write the whole map back) and watches alarmRecords, without a
// browser. Adapted from docs/evidence/w2-084/tools/mirror-heartbeat-client/main.cpp (same hello /
// snapshot / patch handling; the property write is ProxyMirrorClient::propertyWriteReady wrapped like
// the pack's WasmMirrorProxy::sendProxyEnvelope).
//
// Scenario run1 (fresh data folder):
//   A  PT-04 upper -500 kPa enabled -> one 未處理 / 警告 row "超過上限：…" at once; 4 s later still one row;
//      upper disabled -> that row 已解除 about 2 s later.
//   B  TT-01 lower 500 °C enabled -> row "低於下限：…"; disabled (in range) and 1 s later enabled again
//      -> 3.5 s later the same row is still 未處理 and no second row; disabled -> 已解除 about 2 s later.
//   C  Filter upper -500 kPa -> row "Filter 壓差"; disabled -> 已解除 about 2 s later.
//   D  PT-05 upper -500 kPa -> row left OPEN for the restart run (prints LEAVE_OPEN id=<id>).
// Scenario run2 --open-id <id> (after a restart with the same data folder and settings file):
//   the row is still 未處理, 8 s later still the only open PT-05 row and no new PT-05 row (taken over,
//   not added again); upper disabled -> 已解除 about 2 s later.
// Every alarmRecords change of the sensors above is printed:  RECORD id=… sensor=… status=… …
// Exit code: 0 = every check passed; 2 = contract invalid; 3 = rejected by the server; 4 = a check
// failed; 6 = socket error before the snapshot.
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
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>

#include <cstdio>
#include <functional>

namespace {
constexpr int kWireProtocolVersion = 3;      // WasmMirrorProxy::WireProtocolVersion (pack 1.0.x)

void out(const QString &line)
{
    const QByteArray utf8 = (QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz "))
                             + line + QLatin1Char('\n')).toUtf8();
    std::fwrite(utf8.constData(), 1, size_t(utf8.size()), stdout);
    std::fflush(stdout);
}

struct Op
{
    enum Kind { Act, WaitFor, Sleep, Check } kind;
    QString name;
    std::function<void()> action;
    std::function<bool()> condition;
    int ms = 0;
};
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption urlOpt(QStringLiteral("url"), QStringLiteral("Mirror WebSocket URL"), QStringLiteral("url"),
                                    QStringLiteral("ws://127.0.0.1:8386/mirror"));
    const QCommandLineOption originOpt(QStringLiteral("origin"), QStringLiteral("Origin header"), QStringLiteral("origin"),
                                       QStringLiteral("http://127.0.0.1:8385"));
    const QCommandLineOption scenarioOpt(QStringLiteral("scenario"), QStringLiteral("run1 | run2"), QStringLiteral("name"),
                                         QStringLiteral("run1"));
    const QCommandLineOption openIdOpt(QStringLiteral("open-id"), QStringLiteral("run2: the row left open by run1"),
                                       QStringLiteral("id"), QStringLiteral("-1"));
    parser.addOptions({urlOpt, originOpt, scenarioOpt, openIdOpt});
    parser.process(app);
    const QString scenario = parser.value(scenarioOpt);
    const qint64 openId = parser.value(openIdOpt).toLongLong();
    const QString mirrorName = QStringLiteral("TaidaFlow");

    TaidaFlowProxy proxy;
    ProxyMirrorClient client(proxy);
    if (!client.isValid()) {
        out(QStringLiteral("contract invalid: %1").arg(client.validationError()));
        return 2;
    }
    out(QStringLiteral("contractHash=%1 scenario=%2").arg(client.contractHash(), scenario));
    client.setLocalPropertyWritesEnabled(false);
    client.beginRequestSession();
    client.requireSnapshot();

    const QString requestSessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString connectionSessionId;
    bool snapshotSeen = false;
    bool finished = false;
    int failures = 0;
    QWebSocket ws;

    auto sendJson = [&ws](const QJsonObject &object) {
        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    };
    QObject::connect(&client, &ProxyMirrorClient::propertyWriteReady, &app, [&](QJsonObject envelope) {
        envelope.insert(QStringLiteral("protocol"), kWireProtocolVersion);
        envelope.insert(QStringLiteral("mirrorName"), mirrorName);
        envelope.insert(QStringLiteral("requestSessionId"), requestSessionId);
        envelope.insert(QStringLiteral("connectionSessionId"), connectionSessionId);
        sendJson(envelope);
    });

    // ---- helpers on the mirrored Proxy ----
    const QStringList watched{QStringLiteral("PT-04"), QStringLiteral("PT-05"), QStringLiteral("TT-01"),
                              QStringLiteral("Filter 壓差")};
    auto records = [&]() { return proxy.alarmRecords(); };
    auto statusOf = [&](qint64 id) {
        for (const QVariant &v : records()) {
            const QVariantMap m = v.toMap();
            if (m.value("id").toLongLong() == id)
                return m.value("alarmStatus").toString();
        }
        return QString();
    };
    QSet<qint64> initialIds;
    auto newRows = [&](const QString &sensor, const QString &prefix) {
        QList<QVariantMap> list;
        for (const QVariant &v : records()) {
            const QVariantMap m = v.toMap();
            if (m.value("sensorName").toString() == sensor && m.value("alarmMessage").toString().startsWith(prefix)
                    && !initialIds.contains(m.value("id").toLongLong()))
                list << m;
        }
        return list;
    };
    auto openRows = [&](const QString &sensor) {
        QList<QVariantMap> list;
        for (const QVariant &v : records()) {
            const QVariantMap m = v.toMap();
            if (m.value("sensorName").toString() == sensor && m.value("alarmStatus").toString() == QStringLiteral("未處理")
                    && (m.value("alarmMessage").toString().startsWith(QStringLiteral("超過上限"))
                        || m.value("alarmMessage").toString().startsWith(QStringLiteral("低於下限"))))
                list << m;
        }
        return list;
    };
    auto corrected = [&](const QString &key) {
        const QVariantMap s = proxy.sensorSettingsSv();
        const auto adj = [&s](double raw, const QString &k) { return raw + s.value(k).toMap().value("offset").toDouble(); };
        if (key == QLatin1String("filter"))
            return adj(proxy.pt02ValuePv(), "pt02") - adj(proxy.pt03ValuePv(), "pt03");
        return adj(proxy.property((key + QStringLiteral("ValuePv")).toLatin1().constData()).toDouble(), key);
    };
    auto writeEntry = [&](const QString &key, const QVariantMap &changes) {
        QVariantMap settings = proxy.sensorSettingsSv();          // copy, replace one entry, write back
        QVariantMap entry = settings.value(key).toMap();
        for (auto it = changes.cbegin(); it != changes.cend(); ++it)
            entry.insert(it.key(), it.value());
        settings.insert(key, entry);
        out(QStringLiteral("WRITE sensorSettingsSv.%1 = %2").arg(key, QString::fromUtf8(
                QJsonDocument(QJsonObject::fromVariantMap(entry)).toJson(QJsonDocument::Compact))));
        proxy.setSensorSettingsSv(settings);
    };
    QHash<qint64, QString> seenStatus;
    auto printRecords = [&]() {
        for (const QVariant &v : records()) {
            const QVariantMap m = v.toMap();
            const qint64 id = m.value("id").toLongLong();
            if (!watched.contains(m.value("sensorName").toString()) || initialIds.contains(id))
                continue;
            const QString st = m.value("alarmStatus").toString();
            if (seenStatus.value(id) == st)
                continue;
            seenStatus.insert(id, st);
            out(QStringLiteral("RECORD id=%1 time=%2 equipment=%3 sensorName=%4 severity=%5 alarmStatus=%6 alarmMessage=%7")
                        .arg(id).arg(m.value("alarmTime").toString(), m.value("equipment").toString(),
                                     m.value("sensorName").toString(), m.value("severity").toString(), st,
                                     m.value("alarmMessage").toString()));
        }
    };
    QObject::connect(&proxy, &TaidaFlowProxy::alarmRecordsChanged, &app, [&]() { if (snapshotSeen) printRecords(); });

    // ---- the scenario ----
    QList<Op> ops;
    QElapsedTimer clock;
    clock.start();
    qint64 mark = 0;
    qint64 idA = -1, idB = -1, idC = -1, idD = -1;
    auto check = [&](bool ok, const QString &what) {
        out(QStringLiteral("%1 %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
        return ok;
    };
    auto latencyCheck = [&](const QString &what) {
        const qint64 dt = clock.elapsed() - mark;
        check(dt >= 1900 && dt <= 4000, QStringLiteral("%1: %2 ms after the change (expected about 2000)").arg(what).arg(dt));
    };
    auto firstNewOpen = [&](const QString &sensor, const QString &prefix, qint64 *id) {
        for (const QVariantMap &m : newRows(sensor, prefix)) {
            if (m.value("alarmStatus").toString() == QStringLiteral("未處理")) {
                *id = m.value("id").toLongLong();
                return true;
            }
        }
        return false;
    };
    const QVariantMap upperOn{{"upper", -500.0}, {"upperEnabled", true}};
    const QVariantMap upperOff{{"upperEnabled", false}};

    ops << Op{Op::WaitFor, "snapshot", {}, [&]() { return snapshotSeen; }, 10000};
    ops << Op{Op::Act, "initial state", [&]() {
        for (const QVariant &v : records())
            initialIds.insert(v.toMap().value("id").toLongLong());
        for (const QString &k : {QStringLiteral("pt04"), QStringLiteral("pt05"), QStringLiteral("tt01"), QStringLiteral("filter")}) {
            out(QStringLiteral("value %1 = %2 (corrected, kPa / °C); settings %3").arg(k).arg(corrected(k), 0, 'f', 3)
                        .arg(QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(
                                proxy.sensorSettingsSv().value(k).toMap())).toJson(QJsonDocument::Compact))));
        }
        out(QStringLiteral("alarmRecords at start: %1 rows").arg(initialIds.size()));
        client.setLocalPropertyWritesEnabled(true);
    }, {}, 0};

    if (scenario == QLatin1String("run1")) {
        ops << Op{Op::Check, "no open limit rows of the watched sensors at start", {}, [&]() {
            return openRows("PT-04").isEmpty() && openRows("PT-05").isEmpty() && openRows("TT-01").isEmpty()
                    && openRows("Filter 壓差").isEmpty(); }, 0};
        // A: PT-04 upper
        ops << Op{Op::Act, "A: PT-04 upper -500 enabled", [&]() { mark = clock.elapsed(); writeEntry("pt04", upperOn); }, {}, 0};
        ops << Op{Op::WaitFor, "A: new 未處理 PT-04 超過上限 row", {}, [&]() { return firstNewOpen("PT-04", "超過上限", &idA); }, 5000};
        ops << Op{Op::Act, "A: raised", [&]() { out(QStringLiteral("A: id=%1 raised %2 ms after the write").arg(idA).arg(clock.elapsed() - mark)); }, {}, 0};
        ops << Op{Op::Sleep, "A: stays over the limit 4 s", {}, {}, 4000};
        ops << Op{Op::Check, "A: still exactly one PT-04 超過上限 row (no duplicate), 未處理, severity 警告", {}, [&]() {
            const auto rows = newRows("PT-04", "超過上限");
            return rows.size() == 1 && rows.first().value("id").toLongLong() == idA
                    && rows.first().value("alarmStatus").toString() == "未處理"
                    && rows.first().value("severity").toString() == "警告"
                    && rows.first().value("equipment").toString() == "系統"; }, 0};
        ops << Op{Op::Act, "A: upper disabled", [&]() { mark = clock.elapsed(); writeEntry("pt04", upperOff); }, {}, 0};
        ops << Op{Op::WaitFor, "A: row 已解除", {}, [&]() { return statusOf(idA) == "已解除"; }, 6000};
        ops << Op{Op::Act, "A: latency", [&]() { latencyCheck("A: PT-04 row 已解除"); }, {}, 0};
        // B: TT-01 lower, back in range for 1 s only
        ops << Op{Op::Act, "B: TT-01 lower 500 enabled", [&]() { mark = clock.elapsed(); writeEntry("tt01", {{"lower", 500.0}, {"lowerEnabled", true}}); }, {}, 0};
        ops << Op{Op::WaitFor, "B: new 未處理 TT-01 低於下限 row", {}, [&]() { return firstNewOpen("TT-01", "低於下限", &idB); }, 5000};
        ops << Op{Op::Act, "B: lower disabled (in range)", [&]() { writeEntry("tt01", {{"lowerEnabled", false}}); }, {}, 0};
        ops << Op{Op::Sleep, "B: 1 s in range", {}, {}, 1000};
        ops << Op{Op::Act, "B: lower enabled again within 2 s", [&]() { writeEntry("tt01", {{"lowerEnabled", true}}); }, {}, 0};
        ops << Op{Op::Sleep, "B: 3.5 s", {}, {}, 3500};
        ops << Op{Op::Check, "B: same row still 未處理, no second TT-01 row", {}, [&]() {
            return statusOf(idB) == "未處理" && newRows("TT-01", "低於下限").size() == 1; }, 0};
        ops << Op{Op::Act, "B: lower disabled", [&]() { mark = clock.elapsed(); writeEntry("tt01", {{"lowerEnabled", false}}); }, {}, 0};
        ops << Op{Op::WaitFor, "B: row 已解除", {}, [&]() { return statusOf(idB) == "已解除"; }, 6000};
        ops << Op{Op::Act, "B: latency", [&]() { latencyCheck("B: TT-01 row 已解除"); }, {}, 0};
        // C: filter
        ops << Op{Op::Act, "C: Filter upper -500 enabled", [&]() { writeEntry("filter", upperOn); }, {}, 0};
        ops << Op{Op::WaitFor, "C: new 未處理 Filter 壓差 超過上限 row", {}, [&]() { return firstNewOpen("Filter 壓差", "超過上限", &idC); }, 5000};
        ops << Op{Op::Act, "C: upper disabled", [&]() { mark = clock.elapsed(); writeEntry("filter", upperOff); }, {}, 0};
        ops << Op{Op::WaitFor, "C: row 已解除", {}, [&]() { return statusOf(idC) == "已解除"; }, 6000};
        ops << Op{Op::Act, "C: latency", [&]() { latencyCheck("C: Filter 壓差 row 已解除"); }, {}, 0};
        // D: PT-05 left open for the restart run
        ops << Op{Op::Act, "D: PT-05 upper -500 enabled", [&]() { writeEntry("pt05", upperOn); }, {}, 0};
        ops << Op{Op::WaitFor, "D: new 未處理 PT-05 超過上限 row", {}, [&]() { return firstNewOpen("PT-05", "超過上限", &idD); }, 5000};
        ops << Op{Op::Act, "D: leave open", [&]() { out(QStringLiteral("LEAVE_OPEN id=%1").arg(idD)); }, {}, 0};
        ops << Op{Op::Sleep, "D: settings saved", {}, {}, 1500};
    } else if (scenario == QLatin1String("nodevice")) {
        // No device answers: the backend never writes a PV, so nothing may be judged (no false alarm
        // from the Proxy's initial 0 values) - the settings write itself is accepted and saved.
        ops << Op{Op::Act, "nodevice: PT-04 upper -500 enabled", [&]() { writeEntry("pt04", upperOn); }, {}, 0};
        ops << Op{Op::Sleep, "nodevice: 6 s", {}, {}, 6000};
        ops << Op{Op::Check, "nodevice: no PT-04 limit row (no PV from the backend yet)", {}, [&]() {
            const QVariantMap e = proxy.sensorSettingsSv().value("pt04").toMap();
            return e.value("upperEnabled").toBool() && e.value("upper").toDouble() == -500.0
                    && newRows("PT-04", "").isEmpty(); }, 0};
        ops << Op{Op::Act, "nodevice: upper disabled again", [&]() { writeEntry("pt04", upperOff); }, {}, 0};
        ops << Op{Op::Sleep, "nodevice: 3 s", {}, {}, 3000};
        ops << Op{Op::Check, "nodevice: still no limit row of any sensor", {}, [&]() {
            for (const QVariant &v : records()) {
                const QVariantMap m = v.toMap();
                const QString msg = m.value("alarmMessage").toString();
                if (!initialIds.contains(m.value("id").toLongLong())
                        && (msg.startsWith(QStringLiteral("超過上限")) || msg.startsWith(QStringLiteral("低於下限"))))
                    return false;
            }
            return true; }, 0};
    } else {
        ops << Op{Op::Check, "run2: PT-05 upper -500 enabled restored from the settings file", {}, [&]() {
            const QVariantMap e = proxy.sensorSettingsSv().value("pt05").toMap();
            return e.value("upperEnabled").toBool() && e.value("upper").toDouble() == -500.0; }, 0};
        ops << Op{Op::Check, "run2: the row of run 1 is still 未處理", {}, [&]() { return statusOf(openId) == "未處理"; }, 0};
        ops << Op{Op::Sleep, "run2: 8 s of polls (PT-05 still over the limit)", {}, {}, 8000};
        ops << Op{Op::Check, "run2: still the only open PT-05 row, no new PT-05 row (taken over)", {}, [&]() {
            const auto open = openRows("PT-05");
            return open.size() == 1 && open.first().value("id").toLongLong() == openId
                    && newRows("PT-05", "超過上限").isEmpty() && newRows("PT-05", "低於下限").isEmpty(); }, 0};
        ops << Op{Op::Act, "run2: upper disabled", [&]() { mark = clock.elapsed(); writeEntry("pt05", upperOff); }, {}, 0};
        ops << Op{Op::WaitFor, "run2: row 已解除", {}, [&]() { return statusOf(openId) == "已解除"; }, 6000};
        ops << Op{Op::Act, "run2: latency", [&]() { latencyCheck("run2: PT-05 row of run 1 已解除"); }, {}, 0};
        ops << Op{Op::Check, "run2: no new PT-05 row at all", {}, [&]() { return newRows("PT-05", "").isEmpty(); }, 0};
    }

    int index = -1;
    qint64 opStart = 0;
    QTimer runner;
    auto finish = [&](int code, const QString &why) {
        if (finished)
            return;
        finished = true;
        out(QStringLiteral("RESULT %1 (%2 failure(s)) - %3").arg(code == 0 ? "OK" : "FAILED").arg(failures).arg(why));
        ws.close();
        QTimer::singleShot(300, &app, [&app, code]() { app.exit(code); });
    };
    auto nextOp = [&]() {
        ++index;
        opStart = clock.elapsed();
        if (index >= ops.size()) {
            finish(failures ? 4 : 0, QStringLiteral("scenario done"));
            return;
        }
        const Op &op = ops.at(index);
        out(QStringLiteral("STEP %1: %2").arg(index + 1).arg(op.name));
        if (op.kind == Op::Act && op.action)
            op.action();
    };
    QObject::connect(&runner, &QTimer::timeout, &app, [&]() {
        if (finished || index < 0 || index >= ops.size())
            return;
        const Op &op = ops.at(index);
        const qint64 elapsed = clock.elapsed() - opStart;
        switch (op.kind) {
        case Op::Act:
            nextOp();
            break;
        case Op::Sleep:
            if (elapsed >= op.ms)
                nextOp();
            break;
        case Op::Check:
            check(op.condition(), op.name);
            nextOp();
            break;
        case Op::WaitFor:
            if (op.condition()) {
                check(true, QStringLiteral("%1 (%2 ms)").arg(op.name).arg(elapsed));
                nextOp();
            } else if (elapsed > op.ms) {
                check(false, QStringLiteral("%1: not seen within %2 ms").arg(op.name).arg(op.ms));
                finish(4, QStringLiteral("timeout"));
            }
            break;
        }
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
    QObject::connect(&ws, &QWebSocket::disconnected, &app, [&]() {
        out(QStringLiteral("DISCONNECTED: close code %1 %2").arg(int(ws.closeCode())).arg(ws.closeReason()));
        if (!finished)
            finish(4, QStringLiteral("connection closed before the scenario ended"));
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
            connectionSessionId = message.value(QStringLiteral("connectionSessionId")).toString();
            out(QStringLiteral("welcome, connectionSessionId=%1").arg(connectionSessionId));
            return;
        }
        if (type == QStringLiteral("proxy.error")) {
            out(QStringLiteral("server error: %1 %2").arg(message.value(QStringLiteral("code")).toString(),
                                                         message.value(QStringLiteral("message")).toString()));
            finish(3, QStringLiteral("server error"));
            return;
        }
        if (type != QStringLiteral("proxy.snapshot") && type != QStringLiteral("proxy.patch"))
            return;
        QJsonObject normalized = message;                  // wire -> host protocol (pack normalizeEnvelope)
        normalized.insert(QStringLiteral("protocol"), ProxyMirrorHost::ProtocolVersion);
        normalized.remove(QStringLiteral("mirrorName"));
        normalized.remove(QStringLiteral("connectionSessionId"));
        const bool writes = client.localPropertyWritesEnabled();
        client.setLocalPropertyWritesEnabled(false);       // applying server state is not a local write
        const ProxyMirrorApplyResult result = client.applyStateEnvelope(normalized);
        client.setLocalPropertyWritesEnabled(writes);
        if (!result.accepted()) {
            out(QStringLiteral("state rejected: %1").arg(result.error));
            finish(3, QStringLiteral("state rejected"));
            return;
        }
        if (type == QStringLiteral("proxy.snapshot") && !snapshotSeen) {
            snapshotSeen = true;
            out(QStringLiteral("snapshot applied (alarmRecords %1 rows)").arg(proxy.alarmRecords().size()));
        }
    });

    QNetworkRequest request{QUrl(parser.value(urlOpt))};
    request.setRawHeader("Origin", parser.value(originOpt).toUtf8());
    ws.open(request);
    runner.start(50);
    nextOp();
    QTimer::singleShot(120000, &app, [&]() { finish(4, QStringLiteral("overall limit 120 s reached")); });
    return app.exec();
}
