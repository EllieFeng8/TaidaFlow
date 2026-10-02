// w2-086 test tool (dev-only, not part of the application): sets settings-page offsets of the running
// desktop Core through the Proxy Mirror (like the settings page of a web client: copy sensorSettingsSv,
// replace the offsets, write the whole map back) and compares what the backend stores / serves with the
// raw PVs + offsets, without a browser:
//   * the Modbus server input registers 0..15 (Modbus TCP read of the app's own server, D1b),
//   * REST /api/sensor/last (the newest sensor_data row as stored, D1 / D5),
//   * stored x scale against the UI formula raw PV + offset (SensorUnits.adjusted, D2).
// Mirror handling copied from docs/evidence/w2-085/tools/mirror-limit-client/main.cpp (same hello /
// snapshot / patch handling and property write envelope).
//
// The simulator's values may move; a comparison is made only when the PV of that sensor was the same
// in the whole window around the read (so the raw count the backend used is known exactly:
// raw = PV / scale), and only for rows / reads that lie completely before or after a settings change.
//
// Phases: P0 offsets 0 (raw expected) -> P1 pt01 +12.5 kPa, tt02 -1.25 degC, flowMeter +3.4 L/min,
// pt06 +2000 kPa (clamped 65535), tt04 -200 degC (clamped 0) -> P2 pt01 -12.5 kPa (the first row
// stored after the change must already use it) -> P3 offsets 0 again.
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
#include <QModbusReply>
#include <QModbusTcpClient>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>

#include <cmath>
#include <cstdio>
#include <functional>
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

struct Key { const char *key; int reg; double scale; const char *unit; };
// sensor_data column s<reg + 1> / Modbus server input register <reg>, scale of Core/ModbusMapping.h.
const Key kKeys[] = {
    {"tt01", 0, 100.0 / 65535.0, "°C"}, {"tt02", 1, 100.0 / 65535.0, "°C"},
    {"tt03", 2, 100.0 / 65535.0, "°C"}, {"tt04", 3, 100.0 / 65535.0, "°C"},
    {"pt01", 4, 1000.0 / 65535.0, "kPa"}, {"pt02", 5, 1000.0 / 65535.0, "kPa"},
    {"pt03", 6, 1000.0 / 65535.0, "kPa"}, {"pt04", 7, 1000.0 / 65535.0, "kPa"},
    {"pt05", 8, 1000.0 / 65535.0, "kPa"}, {"pt06", 9, 1000.0 / 65535.0, "kPa"},
    {"pt07", 10, 1000.0 / 65535.0, "kPa"}, {"flowMeter", 11, 1.0, "L/min"},
};
constexpr int kKeyCount = int(sizeof(kKeys) / sizeof(kKeys[0]));

// Core/SensorOffsetStorage.cpp apply(): round(raw + offset / scale) (half away from zero), 0..65535.
quint16 expectedStored(quint16 raw, double offset, double scale, bool *clamped)
{
    *clamped = false;
    if (offset == 0.0)
        return raw;
    const double v = std::round(double(raw) + offset / scale);
    if (v < 0.0) { *clamped = true; return 0; }
    if (v > 65535.0) { *clamped = true; return 65535; }
    return quint16(v);
}

struct Snap { qint64 ms; double pv[kKeyCount]; };
struct OffsetEpoch { qint64 fromMs; QHash<QString, double> offsets; QString phase; };

struct Tally
{
    int irCompared = 0, irMismatch = 0, restCompared = 0, restMismatch = 0, screenCompared = 0, screenMismatch = 0;
    int clampedSeen = 0;
};
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption urlOpt(QStringLiteral("url"), QStringLiteral("Mirror WebSocket URL"), QStringLiteral("url"),
                                    QStringLiteral("ws://127.0.0.1:8396/mirror"));
    const QCommandLineOption originOpt(QStringLiteral("origin"), QStringLiteral("Origin header"), QStringLiteral("origin"),
                                       QStringLiteral("http://127.0.0.1:8395"));
    const QCommandLineOption restOpt(QStringLiteral("rest"), QStringLiteral("REST base URL"), QStringLiteral("url"),
                                     QStringLiteral("http://127.0.0.1:18395"));
    const QCommandLineOption mbOpt(QStringLiteral("modbus-port"), QStringLiteral("the app's Modbus server port (127.0.0.1)"),
                                   QStringLiteral("port"), QStringLiteral("5396"));
    const QCommandLineOption phaseOpt(QStringLiteral("phase-ms"), QStringLiteral("length of P0 / P2 / P3 (P1 x 1.5)"),
                                      QStringLiteral("ms"), QStringLiteral("8000"));
    parser.addOptions({urlOpt, originOpt, restOpt, mbOpt, phaseOpt});
    parser.process(app);
    const QString mirrorName = QStringLiteral("TaidaFlow");
    const int phaseMs = parser.value(phaseOpt).toInt();

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
    bool finished = false;
    int failures = 0;
    QWebSocket ws;
    QElapsedTimer clock;
    clock.start();
    const qint64 epochAtStart = QDateTime::currentMSecsSinceEpoch();
    auto nowEpochMs = [&]() { return epochAtStart + clock.elapsed(); };   // monotonic, epoch based

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
    auto check = [&](bool ok, const QString &what) {
        out(QStringLiteral("%1 %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
        return ok;
    };

    // ---- PV history (raw PVs of the mirrored Proxy, every 100 ms) ----
    QList<Snap> snaps;
    auto pvOf = [&](int k) {
        return proxy.property((QString::fromLatin1(kKeys[k].key) + QStringLiteral("ValuePv")).toLatin1().constData()).toDouble();
    };
    QTimer sampler;
    QObject::connect(&sampler, &QTimer::timeout, &app, [&]() {
        if (!snapshotSeen)
            return;
        Snap s;
        s.ms = nowEpochMs();
        for (int k = 0; k < kKeyCount; ++k)
            s.pv[k] = pvOf(k);
        snaps.append(s);
        while (!snaps.isEmpty() && snaps.first().ms < s.ms - 30000)
            snaps.removeFirst();
    });
    // raw count of key k if its PV was the same in [fromMs, toMs] (and a raw count: PV = raw x scale).
    auto stableRaw = [&](int k, qint64 fromMs, qint64 toMs, quint16 *raw) {
        bool any = false;
        double value = 0.0;
        bool coversStart = false;
        for (const Snap &s : std::as_const(snaps)) {
            if (s.ms < fromMs - 150 || s.ms > toMs + 150)
                continue;
            coversStart = coversStart || s.ms <= fromMs + 150;
            if (!any) { value = s.pv[k]; any = true; }
            else if (s.pv[k] != value) return false;
        }
        if (!any || !coversStart)
            return false;
        const double counts = value / kKeys[k].scale;
        const double rounded = std::round(counts);
        if (std::abs(counts - rounded) > 1e-6 || rounded < 0 || rounded > 65535)
            return false;
        *raw = quint16(rounded);
        return true;
    };

    // ---- offsets in effect ----
    QList<OffsetEpoch> epochs;
    QString phase = QStringLiteral("P0");
    QHash<QString, Tally> tally;
    auto writeOffsets = [&](const QHash<QString, double> &offsets, const QString &newPhase) {
        QVariantMap settings = proxy.sensorSettingsSv();          // copy, replace the offsets, write back
        QHash<QString, double> all;
        for (int k = 0; k < kKeyCount; ++k) {
            const QString key = QString::fromLatin1(kKeys[k].key);
            QVariantMap entry = settings.value(key).toMap();
            const double off = offsets.value(key, 0.0);
            entry.insert(QStringLiteral("offset"), off);
            settings.insert(key, entry);
            if (off != 0.0)
                all.insert(key, off);
        }
        phase = newPhase;
        epochs.append(OffsetEpoch{nowEpochMs(), all, newPhase});
        QStringList text;
        for (auto it = all.cbegin(); it != all.cend(); ++it)
            text << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
        text.sort();
        out(QStringLiteral("WRITE %1 sensorSettingsSv offsets: %2 (epoch ms %3)")
                    .arg(newPhase, text.isEmpty() ? QStringLiteral("all 0") : text.join(' ')).arg(epochs.last().fromMs));
        proxy.setSensorSettingsSv(settings);
    };
    // The offsets in effect during the whole window [fromMs, toMs]; false if a change lies inside.
    auto offsetsFor = [&](qint64 fromMs, qint64 toMs, OffsetEpoch *epoch) {
        for (int i = epochs.size() - 1; i >= 0; --i) {
            const qint64 start = epochs.at(i).fromMs + 300;     // write -> server apply latency margin
            const qint64 end = i + 1 < epochs.size() ? epochs.at(i + 1).fromMs : std::numeric_limits<qint64>::max();
            if (fromMs >= start && toMs < end) {
                *epoch = epochs.at(i);
                return true;
            }
        }
        return false;
    };

    // ---- Modbus server input registers (D1b) ----
    QModbusTcpClient mb;
    mb.setConnectionParameter(QModbusDevice::NetworkAddressParameter, QStringLiteral("127.0.0.1"));
    mb.setConnectionParameter(QModbusDevice::NetworkPortParameter, parser.value(mbOpt).toInt());
    mb.setTimeout(2000);
    mb.setNumberOfRetries(0);
    bool irBusy = false;
    auto pollIr = [&]() {
        if (irBusy || epochs.isEmpty())
            return;
        if (mb.state() == QModbusDevice::UnconnectedState) {
            mb.connectDevice();
            return;
        }
        if (mb.state() != QModbusDevice::ConnectedState)
            return;
        const qint64 sent = nowEpochMs();
        QModbusReply *reply = mb.sendReadRequest(QModbusDataUnit(QModbusDataUnit::InputRegisters, 0, 16), 1);
        if (!reply)
            return;
        irBusy = true;
        QObject::connect(reply, &QModbusReply::finished, &app, [&, reply, sent]() {
            irBusy = false;
            reply->deleteLater();
            if (reply->error() != QModbusDevice::NoError) {
                out(QStringLiteral("IR read error: %1").arg(reply->errorString()));
                return;
            }
            const QList<quint16> ir = reply->result().values();
            const qint64 recv = nowEpochMs();
            // The register was written by the newest poll before 'sent' (at most ~1 s + reply time).
            OffsetEpoch epoch;
            if (ir.size() < 16 || !offsetsFor(sent - 1500, recv, &epoch))
                return;
            Tally &t = tally[epoch.phase];
            QStringList parts;
            for (int k = 0; k < kKeyCount; ++k) {
                quint16 raw = 0;
                if (!stableRaw(k, sent - 1500, recv, &raw))
                    continue;
                bool clamped = false;
                const double off = epoch.offsets.value(QString::fromLatin1(kKeys[k].key), 0.0);
                const quint16 expected = expectedStored(raw, off, kKeys[k].scale, &clamped);
                const quint16 got = ir.at(kKeys[k].reg);
                ++t.irCompared;
                t.clampedSeen += clamped ? 1 : 0;
                if (got != expected) {
                    ++t.irMismatch;
                    out(QStringLiteral("IR MISMATCH %1 %2 IR%3=%4 expected %5 (raw %6, offset %7)")
                                .arg(epoch.phase, QString::fromLatin1(kKeys[k].key)).arg(kKeys[k].reg).arg(got)
                                .arg(expected).arg(raw).arg(off));
                } else if (off != 0.0) {
                    parts << QStringLiteral("%1 IR%2=%3(raw %4%5)").arg(QString::fromLatin1(kKeys[k].key)).arg(kKeys[k].reg)
                                     .arg(got).arg(raw).arg(clamped ? QStringLiteral(", clamped") : QString());
                }
            }
            if (!parts.isEmpty())
                out(QStringLiteral("IR %1 ok: %2").arg(epoch.phase, parts.join(QStringLiteral("; "))));
        });
    };

    // ---- REST /api/sensor/last (D1 / D2 / D5) ----
    QNetworkAccessManager nam;
    bool restBusy = false;
    qint64 lastTs = -1;
    QHash<QString, qint64> firstRowAfter;    // phase -> ts of the first row completely after its change
    QHash<QString, bool> firstRowChecked;
    auto pollRest = [&]() {
        if (restBusy || epochs.isEmpty())
            return;
        restBusy = true;
        QNetworkReply *reply = nam.get(QNetworkRequest(QUrl(parser.value(restOpt) + QStringLiteral("/api/sensor/last"))));
        QObject::connect(reply, &QNetworkReply::finished, &app, [&, reply]() {
            restBusy = false;
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                out(QStringLiteral("REST error: %1").arg(reply->errorString()));
                return;
            }
            const QJsonObject row = QJsonDocument::fromJson(reply->readAll()).object();
            const qint64 ts = row.value(QStringLiteral("ts")).toInteger();
            if (ts == lastTs)
                return;
            lastTs = ts;
            // The row was saved during second ts (the sample taken in that second, raw from the poll
            // shortly before): the offsets must be the same in the whole window.
            const qint64 from = ts * 1000 - 1300;
            const qint64 to = ts * 1000 + 999;
            OffsetEpoch epoch;
            if (!offsetsFor(ts * 1000, to, &epoch)) {
                out(QStringLiteral("REST row ts=%1 spans a settings change - not compared").arg(ts));
                return;
            }
            Tally &t = tally[epoch.phase];
            const bool first = !firstRowAfter.contains(epoch.phase);
            if (first)
                firstRowAfter.insert(epoch.phase, ts);
            QStringList parts;
            bool firstAllOk = true;
            int firstCompared = 0;
            for (int k = 0; k < kKeyCount; ++k) {
                quint16 raw = 0;
                if (!stableRaw(k, from, to, &raw))
                    continue;
                const QString key = QString::fromLatin1(kKeys[k].key);
                bool clamped = false;
                const double off = epoch.offsets.value(key, 0.0);
                const quint16 expected = expectedStored(raw, off, kKeys[k].scale, &clamped);
                const double got = row.value(QStringLiteral("s%1").arg(kKeys[k].reg + 1)).toDouble();
                ++t.restCompared;
                if (first) ++firstCompared;
                if (got != double(expected)) {
                    ++t.restMismatch;
                    firstAllOk = false;
                    out(QStringLiteral("REST MISMATCH %1 ts=%2 %3 s%4=%5 expected %6 (raw %7, offset %8)")
                                .arg(epoch.phase).arg(ts).arg(key).arg(kKeys[k].reg + 1).arg(got).arg(expected).arg(raw).arg(off));
                    continue;
                }
                // D2: stored x scale (History / CSV / REST conversion) vs the screen value raw PV + offset.
                if (!clamped) {
                    const double history = got * kKeys[k].scale;
                    const double screen = double(raw) * kKeys[k].scale + off;
                    ++t.screenCompared;
                    if (std::abs(history - screen) > 0.5 * kKeys[k].scale + 1e-9) {
                        ++t.screenMismatch;
                        out(QStringLiteral("SCREEN MISMATCH %1 %2: history %3 vs screen %4").arg(epoch.phase, key)
                                    .arg(history, 0, 'f', 6).arg(screen, 0, 'f', 6));
                    }
                } else {
                    ++t.clampedSeen;
                }
                if (off != 0.0)
                    parts << QStringLiteral("%1 s%2=%3 (raw %4, x scale %5 %6 vs screen %7%8)").arg(key).arg(kKeys[k].reg + 1)
                                     .arg(got).arg(raw).arg(got * kKeys[k].scale, 0, 'f', 3)
                                     .arg(QString::fromUtf8(kKeys[k].unit))
                                     .arg(double(raw) * kKeys[k].scale + off, 0, 'f', 3)
                                     .arg(clamped ? QStringLiteral(", clamped") : QString());
            }
            out(QStringLiteral("REST %1 ts=%2 %3 compared: %4").arg(epoch.phase).arg(ts)
                        .arg(first ? QStringLiteral("(first row after the change)") : QString())
                        .arg(parts.isEmpty() ? QStringLiteral("(no offset columns or not stable)") : parts.join(QStringLiteral("; "))));
            if (first) {
                firstRowChecked.insert(epoch.phase, firstCompared > 0 && firstAllOk);
            }
        });
    };
    QTimer poller;
    QObject::connect(&poller, &QTimer::timeout, &app, [&]() {
        if (!snapshotSeen || finished)
            return;
        pollIr();
        pollRest();
    });

    // ---- the scenario ----
    const QHash<QString, double> p1{{"pt01", 12.5}, {"tt02", -1.25}, {"flowMeter", 3.4}, {"pt06", 2000.0}, {"tt04", -200.0}};
    QHash<QString, double> p2 = p1;
    p2.insert(QStringLiteral("pt01"), -12.5);
    struct Step { qint64 atMs; std::function<void()> action; };
    QList<Step> steps;
    qint64 t0 = 0;
    auto phaseReport = [&](const QString &p, bool needClamped) {
        const Tally t = tally.value(p);
        out(QStringLiteral("SUMMARY %1: IR compared %2 mismatch %3; REST values compared %4 mismatch %5; "
                           "history vs screen compared %6 mismatch %7; clamped values seen %8")
                    .arg(p).arg(t.irCompared).arg(t.irMismatch).arg(t.restCompared).arg(t.restMismatch)
                    .arg(t.screenCompared).arg(t.screenMismatch).arg(t.clampedSeen));
        check(t.irCompared >= 5 && t.irMismatch == 0, QStringLiteral("%1: Modbus server input registers = raw + offset (counts)").arg(p));
        check(t.restCompared >= 5 && t.restMismatch == 0, QStringLiteral("%1: REST /api/sensor/last rows = raw + offset (counts)").arg(p));
        check(t.screenCompared >= 5 && t.screenMismatch == 0,
              QStringLiteral("%1: stored x scale = raw PV + offset within half a count").arg(p));
        if (needClamped)
            check(t.clampedSeen > 0, QStringLiteral("%1: clamped columns (pt06 -> 65535, tt04 -> 0) seen").arg(p));
    };

    QTimer runner;
    int stepIndex = 0;
    auto finish = [&](int code, const QString &why) {
        if (finished)
            return;
        finished = true;
        out(QStringLiteral("RESULT %1 (%2 failure(s)) - %3").arg(code == 0 ? "OK" : "FAILED").arg(failures).arg(why));
        ws.close();
        mb.disconnectDevice();
        QTimer::singleShot(300, &app, [&app, code]() { app.exit(code); });
    };
    auto startScenario = [&]() {
        t0 = clock.elapsed();
        const qint64 p1Ms = phaseMs * 3 / 2;
        steps << Step{0, [&]() {
            QStringList nonZero;
            const QVariantMap s = proxy.sensorSettingsSv();
            for (int k = 0; k < kKeyCount; ++k) {
                const double off = s.value(QString::fromLatin1(kKeys[k].key)).toMap().value(QStringLiteral("offset")).toDouble();
                if (off != 0.0)
                    nonZero << QStringLiteral("%1=%2").arg(QString::fromLatin1(kKeys[k].key)).arg(off);
            }
            check(nonZero.isEmpty(), QStringLiteral("all offsets 0 at start (fresh settings) %1").arg(nonZero.join(' ')));
            QStringList pv;
            for (int k = 0; k < kKeyCount; ++k)
                pv << QStringLiteral("%1=%2").arg(QString::fromLatin1(kKeys[k].key)).arg(pvOf(k), 0, 'f', 3);
            out(QStringLiteral("PVs at start (raw): %1").arg(pv.join(' ')));
            writeOffsets({}, QStringLiteral("P0"));
        }};
        steps << Step{phaseMs, [&]() { phaseReport(QStringLiteral("P0"), false); writeOffsets(p1, QStringLiteral("P1")); }};
        steps << Step{phaseMs + p1Ms, [&]() { phaseReport(QStringLiteral("P1"), true); writeOffsets(p2, QStringLiteral("P2")); }};
        steps << Step{2 * phaseMs + p1Ms, [&]() {
            phaseReport(QStringLiteral("P2"), true);
            check(firstRowChecked.value(QStringLiteral("P2"), false),
                  QStringLiteral("P2: the first row stored after the change (ts=%1) already uses pt01 -12.5 kPa")
                          .arg(firstRowAfter.value(QStringLiteral("P2"), -1)));
            writeOffsets({}, QStringLiteral("P3"));
        }};
        steps << Step{3 * phaseMs + p1Ms, [&]() {
            phaseReport(QStringLiteral("P3"), false);
            check(firstRowChecked.value(QStringLiteral("P3"), false),
                  QStringLiteral("P3: the first row stored after resetting the offsets (ts=%1) is raw again")
                          .arg(firstRowAfter.value(QStringLiteral("P3"), -1)));
            finish(failures ? 4 : 0, QStringLiteral("scenario done"));
        }};
        QObject::connect(&runner, &QTimer::timeout, &app, [&]() {
            if (finished)
                return;
            while (stepIndex < steps.size() && clock.elapsed() - t0 >= steps.at(stepIndex).atMs) {
                const int i = stepIndex++;
                out(QStringLiteral("STEP %1 at %2 ms").arg(i + 1).arg(clock.elapsed() - t0));
                steps.at(i).action();
            }
        });
        runner.start(50);
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
            out(QStringLiteral("snapshot applied"));
            client.setLocalPropertyWritesEnabled(true);
            // Wait 2 s of PV history before the first phase starts.
            QTimer::singleShot(2000, &app, [&]() { startScenario(); });
        }
    });

    QNetworkRequest request{QUrl(parser.value(urlOpt))};
    request.setRawHeader("Origin", parser.value(originOpt).toUtf8());
    ws.open(request);
    sampler.start(100);
    poller.start(400);
    QTimer::singleShot(10000, &app, [&]() {
        if (!snapshotSeen) {
            out(QStringLiteral("no snapshot within 10 s"));
            finish(4, QStringLiteral("no snapshot"));
        }
    });
    QTimer::singleShot(std::max(150000, 6 * phaseMs + 30000), &app, [&]() { finish(4, QStringLiteral("overall time limit reached")); });
    return app.exec();
}
