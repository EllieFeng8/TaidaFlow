// w2-052 test tool (dev-only, not part of the application): sends per-client History view
// requests the way web pages do, without a browser, and prints what comes back.
//
// Extended copy of docs/evidence/w2-050/tools/mirror-export-client.  It is a Proxy Mirror
// client like the WebAssembly page: it builds the contract from Core/TaidaFlowProxy.h with the
// pack's ProxyMirrorClient, connects to ws://<host>:8125/mirror (the desktop's LAN relay),
// sends proxy.hello, applies snapshot / patches to a local TaidaFlowProxy and then sends
//     historyViewRequested(QString sessionId, double fromMs, double toMs, int page)
// in rounds.  Every request of a round is sent back to back (no waiting in between); the
// round ends when every entry of the round shows its request.  It prints
//   PATCH  for every historyViews change: which entries got a new revision / were added / removed
//   ENTRY  per request: range, page, totals, revision, first/last row time, the first record as JSON,
//          and the SHA-1 of the whole records list (digest line)
// Usage:
//   mirror_history_client --url ws://127.0.0.1:8125/mirror
//       --round "web-c1|month|1;web-c2|week|1" --round "web-c1|month|2" ...
//   range = month | week | all | <fromMs>:<toMs>   (month / week as the page computes them)
// A request followed by another request of the same session in the same round is expected to
// be superseded (not waited for).  At the end every entry is printed (DUMP lines).
// --start-at-ms <epoch ms>: the first round is sent at that wall-clock time (after the snapshot),
// so that several client processes send their requests at the same time.
// Exit code: 0 = every round completed; 2 = contract invalid / bad arguments; 3 = rejected;
// 5 = timeout; 6 = socket error.
#include "TaidaFlowProxy.h"
#include "infrastructure/proxy_mirror/proxymirror.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
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

QString compact(const QVariant &v)
{
    const QJsonValue j = QJsonValue::fromVariant(v);
    if (j.isObject())
        return QString::fromUtf8(QJsonDocument(j.toObject()).toJson(QJsonDocument::Compact));
    if (j.isArray())
        return QString::fromUtf8(QJsonDocument(j.toArray()).toJson(QJsonDocument::Compact));
    return v.toString();
}

struct Request
{
    QString sid;
    QString rangeText;
    double fromMs = 0;
    double toMs = 0;
    int page = 1;
    qint64 revisionBefore = -1;
    bool done = false;
};

// The page's own defaults (TaidaFlowContent/components/HistoryViewUtil.js monthRange / lastWeekRange).
bool parseRange(const QString &text, double *from, double *to)
{
    const QDate today = QDate::currentDate();
    if (text == QStringLiteral("month")) {
        const QDate first(today.year(), today.month(), 1);
        *from = double(QDateTime(first, QTime(0, 0)).toMSecsSinceEpoch());
        *to = double(QDateTime(first.addMonths(1), QTime(0, 0)).toMSecsSinceEpoch() - 1);
        return true;
    }
    if (text == QStringLiteral("week")) {
        *from = double(QDateTime(today.addDays(-6), QTime(0, 0)).toMSecsSinceEpoch());
        *to = double(QDateTime(today.addDays(1), QTime(0, 0)).toMSecsSinceEpoch() - 1);
        return true;
    }
    if (text == QStringLiteral("all")) {
        *from = 0;
        *to = 8640000000000000.0;
        return true;
    }
    const QStringList parts = text.split(QLatin1Char(':'));
    bool ok1 = false;
    bool ok2 = false;
    if (parts.size() == 2) {
        *from = parts.at(0).toDouble(&ok1);
        *to = parts.at(1).toDouble(&ok2);
    }
    return ok1 && ok2;
}

// SHA-1 of the whole records list (compact JSON): pages of different runs / sessions compare equal
// row by row exactly when their digests are equal.
QString recordsDigest(const QVariantList &records)
{
    const QByteArray json = QJsonDocument(QJsonArray::fromVariantList(records)).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(json, QCryptographicHash::Sha1).toHex());
}

QString msTime(double ms)
{
    return QDateTime::fromMSecsSinceEpoch(qint64(ms)).toString(QStringLiteral("yyyy/MM/dd HH:mm:ss.zzz"));
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
    const QCommandLineOption roundOpt(QStringLiteral("round"), QStringLiteral("requests sent together: sid|range|page;..."),
                                      QStringLiteral("requests"));
    const QCommandLineOption timeoutOpt(QStringLiteral("timeout-sec"), QStringLiteral("overall timeout"), QStringLiteral("s"),
                                        QStringLiteral("120"));
    const QCommandLineOption startAtOpt(QStringLiteral("start-at-ms"),
                                        QStringLiteral("wall-clock epoch ms to send the first round at (after the snapshot), "
                                                       "so that several clients send at the same time"),
                                        QStringLiteral("ms"), QStringLiteral("0"));
    parser.addOptions({urlOpt, originOpt, mirrorOpt, roundOpt, timeoutOpt, startAtOpt});
    parser.process(app);

    QList<QList<Request>> rounds;
    for (const QString &roundText : parser.values(roundOpt)) {
        QList<Request> round;
        for (const QString &item : roundText.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const QStringList f = item.split(QLatin1Char('|'));
            Request r;
            if (f.size() != 3 || !parseRange(f.at(1), &r.fromMs, &r.toMs)) {
                out(QStringLiteral("bad request '%1' (sid|month|week|all|from:to|page)").arg(item));
                return 2;
            }
            r.sid = f.at(0);
            r.rangeText = f.at(1);
            r.page = f.at(2).toInt();
            round.append(r);
        }
        rounds.append(round);
    }
    if (rounds.isEmpty()) {
        out(QStringLiteral("no --round given"));
        return 2;
    }

    const QString mirrorName = parser.value(mirrorOpt);
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
    bool haveSnapshot = false;
    int current = -1;
    QVariantMap lastViews;

    QWebSocket ws;
    auto sendJson = [&ws](const QJsonObject &object) {
        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    };
    auto revisionOf = [](const QVariantMap &views, const QString &sid) -> qint64 {
        const QVariantMap e = views.value(sid).toMap();
        return e.isEmpty() ? -1 : e.value(QStringLiteral("revision")).toLongLong();
    };
    auto revisions = [&](const QVariantMap &views) {
        QStringList l;
        for (auto it = views.cbegin(); it != views.cend(); ++it)
            l << QStringLiteral("%1=%2").arg(it.key()).arg(it.value().toMap().value(QStringLiteral("revision")).toLongLong());
        return l.join(QLatin1Char(' '));
    };

    std::function<void()> startRound;
    std::function<void()> checkRound = [&]() {
        if (current < 0 || current >= rounds.size())
            return;
        const QVariantMap views = proxy.historyViews();
        bool all = true;
        for (int i = 0; i < rounds[current].size(); ++i) {
            Request &r = rounds[current][i];
            if (r.done)
                continue;
            // A request followed by a newer request of the SAME session in this round is
            // expected to be made stale by it (spec §2.1): it is not waited for; the round
            // still waits for the newest one, and the app log shows the dropped result.
            bool supersededInRound = false;
            for (int j = i + 1; j < rounds[current].size(); ++j)
                supersededInRound = supersededInRound || rounds[current][j].sid == r.sid;
            if (supersededInRound) {
                r.done = true;
                out(QStringLiteral("ENTRY %1: request (%2, page %3) superseded by a later request of the same session "
                                   "in this round - not waited for").arg(r.sid, r.rangeText).arg(r.page));
                continue;
            }
            const QVariantMap e = views.value(r.sid).toMap();
            const int page = e.value(QStringLiteral("page")).toInt();
            const int totalPages = e.value(QStringLiteral("totalPages")).toInt();
            const bool ok = !e.isEmpty() && e.value(QStringLiteral("revision")).toLongLong() != r.revisionBefore
                    && e.value(QStringLiteral("fromMs")).toDouble() == r.fromMs
                    && e.value(QStringLiteral("toMs")).toDouble() == r.toMs
                    && (page == r.page || (r.page > totalPages && page == totalPages));
            if (!ok) {
                all = false;
                continue;
            }
            r.done = true;
            const QVariantList records = e.value(QStringLiteral("records")).toList();
            const QVariantMap first = records.isEmpty() ? QVariantMap() : records.first().toMap();
            const QVariantMap last = records.isEmpty() ? QVariantMap() : records.last().toMap();
            out(QStringLiteral("ENTRY %1: range %2 .. %3 (%4), page %5/%6 (asked %7), totalRows %8, %9 record(s) "
                               "%10 .. %11, revision %12")
                        .arg(r.sid, msTime(r.fromMs), msTime(r.toMs), r.rangeText)
                        .arg(page).arg(totalPages).arg(r.page)
                        .arg(e.value(QStringLiteral("totalRows")).toLongLong())
                        .arg(records.size())
                        .arg(first.value(QStringLiteral("values")).toList().value(0).toString(),
                             last.value(QStringLiteral("values")).toList().value(0).toString())
                        .arg(e.value(QStringLiteral("revision")).toLongLong()));
            out(QStringLiteral("ENTRY %1 record[0] %2").arg(r.sid, compact(first)));
            out(QStringLiteral("ENTRY %1 digest page %2 rows %3 sha1 %4").arg(r.sid).arg(page).arg(records.size())
                        .arg(recordsDigest(records)));
        }
        if (!all)
            return;
        out(QStringLiteral("ROUND %1 done; revisions: %2").arg(current + 1).arg(revisions(views)));
        ++current;
        if (current >= rounds.size()) {
            // Read back every entry of historyViews as this client sees it now.
            for (auto it = views.cbegin(); it != views.cend(); ++it) {
                const QVariantMap e = it.value().toMap();
                const QVariantList records = e.value(QStringLiteral("records")).toList();
                const QVariantMap first = records.isEmpty() ? QVariantMap() : records.first().toMap();
                const QVariantMap last = records.isEmpty() ? QVariantMap() : records.last().toMap();
                out(QStringLiteral("DUMP %1: range %2 .. %3, page %4/%5, totalRows %6, %7 record(s) %8 .. %9, revision %10")
                            .arg(it.key(), msTime(e.value(QStringLiteral("fromMs")).toDouble()),
                                 msTime(e.value(QStringLiteral("toMs")).toDouble()))
                            .arg(e.value(QStringLiteral("page")).toInt())
                            .arg(e.value(QStringLiteral("totalPages")).toInt())
                            .arg(e.value(QStringLiteral("totalRows")).toLongLong())
                            .arg(records.size())
                            .arg(first.value(QStringLiteral("values")).toList().value(0).toString(),
                                 last.value(QStringLiteral("values")).toList().value(0).toString())
                            .arg(e.value(QStringLiteral("revision")).toLongLong()));
                out(QStringLiteral("DUMP %1 digest sha1 %2").arg(it.key(), recordsDigest(records)));
            }
            out(QStringLiteral("RESULT ok: %1 round(s), entries %2").arg(rounds.size())
                        .arg(QStringList(views.keys()).join(QLatin1Char(','))));
            ws.close();
            app.exit(0);
            return;
        }
        startRound();
    };
    startRound = [&]() {
        const QVariantMap views = proxy.historyViews();
        for (Request &r : rounds[current]) {
            r.revisionBefore = revisionOf(views, r.sid);
            QString error;
            QJsonObject envelope = client.makeSignalEnvelope(
                    QStringLiteral("historyViewRequested(QString,double,double,int)"),
                    QVariantList{r.sid, r.fromMs, r.toMs, r.page}, &error);
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
            out(QStringLiteral("SEND round %1: historyViewRequested(\"%2\", %3, %4, %5)  [%6]")
                        .arg(current + 1).arg(r.sid).arg(qint64(r.fromMs)).arg(qint64(r.toMs)).arg(r.page).arg(r.rangeText));
        }
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
    QObject::connect(&proxy, &TaidaFlowProxy::historyViewsChanged, &app, [&](const QVariantMap &views) {
        QStringList changed, added, removed;
        for (auto it = views.cbegin(); it != views.cend(); ++it) {
            if (!lastViews.contains(it.key()))
                added << it.key();
            else if (revisionOf(lastViews, it.key()) != revisionOf(views, it.key()))
                changed << it.key();
        }
        for (auto it = lastViews.cbegin(); it != lastViews.cend(); ++it)
            if (!views.contains(it.key()))
                removed << it.key();
        if (haveSnapshot) {
            out(QStringLiteral("PATCH historyViews: new revision [%1], added [%2], removed [%3]; now %4")
                        .arg(changed.join(QLatin1Char(',')), added.join(QLatin1Char(',')),
                             removed.join(QLatin1Char(',')), revisions(views)));
        }
        lastViews = views;
        checkRound();
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
        if (haveSnapshot || type != QStringLiteral("proxy.snapshot"))
            return;
        haveSnapshot = true;
        lastViews = proxy.historyViews();
        out(QStringLiteral("snapshot: historyViews entries [%1]; historyTitle %2 column(s)")
                    .arg(revisions(lastViews)).arg(proxy.historyTitle().size()));
        current = 0;
        const qint64 wait = parser.value(startAtOpt).toLongLong() - QDateTime::currentMSecsSinceEpoch();
        if (wait > 0) {
            out(QStringLiteral("waiting %1 ms for --start-at-ms").arg(wait));
            QTimer::singleShot(int(wait), &app, [&]() { startRound(); });
        } else {
            startRound();
        }
    });
    QTimer::singleShot(parser.value(timeoutOpt).toInt() * 1000, &app, [&]() {
        out(QStringLiteral("timeout in round %1").arg(current + 1));
        app.exit(5);
    });

    QNetworkRequest request{QUrl(parser.value(urlOpt))};
    request.setRawHeader("Origin", parser.value(originOpt).toUtf8());
    ws.open(request);
    return app.exec();
}
