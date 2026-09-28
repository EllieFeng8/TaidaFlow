// w2-061: /runtime.json parser/builder and the request with fallback
// (App/runtimeinfo.{h,cpp}, docs/taidaflow_config_spec.md §3). The request cases talk to a
// real HTTP responder on 127.0.0.1 (QTcpServer, ephemeral port).
#include "runtimeinfo.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

#include <memory>
#include <optional>

using namespace TaidaFlowRuntime;

namespace {

// Minimal HTTP/1.1 responder: answers every request with `response` (raw bytes), or never
// answers when `response` is empty (timeout case).
class HttpResponder : public QObject
{
public:
    explicit HttpResponder(QByteArray response) : m_response(std::move(response))
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    if (!buffer->contains("\r\n\r\n"))
                        return;
                    m_requests.append(*buffer);
                    if (m_response.isEmpty())
                        return;   // hold the connection open, never answer
                    socket->write(m_response);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/runtime.json").arg(m_server.serverPort()));
    }
    QList<QByteArray> requests() const { return m_requests; }

private:
    QTcpServer m_server;
    QByteArray m_response;
    QList<QByteArray> m_requests;
};

QByteArray httpResponse(int status, const QByteArray &reason, const QByteArray &body,
                        const QByteArray &type = "application/json")
{
    return "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n"
           "Content-Type: " + type + "\r\n"
           "Cache-Control: no-store\r\n"
           "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
           "Connection: close\r\n\r\n" + body;
}

// Runs requestRuntimeInfo and waits for the single callback.
std::optional<RuntimeLookup> lookup(const QUrl &url, int timeoutMs, int *calls, qint64 *elapsedMs = nullptr)
{
    QObject context;
    std::optional<RuntimeLookup> result;
    *calls = 0;
    QElapsedTimer timer;
    timer.start();
    requestRuntimeInfo(url, timeoutMs, &context, [&](const RuntimeLookup &r) {
        ++*calls;
        result = r;
    });
    if (!QTest::qWaitFor([&] { return result.has_value(); }, timeoutMs + 5000))
        return std::nullopt;
    if (elapsedMs)
        *elapsedMs = timer.elapsed();
    QTest::qWait(300);   // a second callback (bug) would show up here
    return result;
}

} // namespace

class tst_RuntimeInfo : public QObject
{
    Q_OBJECT

private slots:
    void parseValid_data();
    void parseValid();
    void parseInvalid_data();
    void parseInvalid();
    void buildRoundTrip();
    void requestValid();
    void requestHttp404FallsBack();
    void requestInvalidContentFallsBack();
    void requestTimeoutFallsBack();
    void requestRefusedFallsBack();
    void requestInvalidUrlFallsBackAsynchronously();
    void requestContextDestroyedNoCallback();
};

void tst_RuntimeInfo::parseValid_data()
{
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<int>("port");
    QTest::addColumn<int>("version");
    QTest::newRow("spec") << QByteArray(R"({ "mirrorPublicPort": 8125, "version": 1 })") << 8125 << 1;
    QTest::newRow("other port") << QByteArray(R"({"mirrorPublicPort":9125,"version":1})") << 9125 << 1;
    QTest::newRow("no version") << QByteArray(R"({"mirrorPublicPort":8126})") << 8126 << 1;
    QTest::newRow("newer version") << QByteArray(R"({"mirrorPublicPort":8127,"version":2,"x":true})") << 8127 << 2;
    QTest::newRow("max port") << QByteArray(R"({"mirrorPublicPort":65535})") << 65535 << 1;
}

void tst_RuntimeInfo::parseValid()
{
    QFETCH(QByteArray, body);
    QFETCH(int, port);
    QFETCH(int, version);
    QString error = QStringLiteral("unchanged");
    const std::optional<RuntimeInfo> info = parseRuntimeJson(body, &error);
    QVERIFY2(info.has_value(), qPrintable(error));
    QCOMPARE(int(info->mirrorPublicPort), port);
    QCOMPARE(info->version, version);
    QVERIFY(error.isEmpty());
}

void tst_RuntimeInfo::parseInvalid_data()
{
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<QString>("errorPart");
    QTest::newRow("empty") << QByteArray() << "not JSON";
    QTest::newRow("html page") << QByteArray("<!doctype html><html></html>") << "not JSON";
    QTest::newRow("array") << QByteArray("[8125]") << "not a JSON object";
    QTest::newRow("missing port") << QByteArray(R"({"version":1})") << "\"mirrorPublicPort\" is missing";
    QTest::newRow("port 0") << QByteArray(R"({"mirrorPublicPort":0})") << "1..65535";
    QTest::newRow("port 70000") << QByteArray(R"({"mirrorPublicPort":70000})") << "1..65535";
    QTest::newRow("port string") << QByteArray(R"({"mirrorPublicPort":"8125"})") << "1..65535";
    QTest::newRow("port fraction") << QByteArray(R"({"mirrorPublicPort":8125.5})") << "1..65535";
    QTest::newRow("port null") << QByteArray(R"({"mirrorPublicPort":null})") << "1..65535";
    QTest::newRow("version string") << QByteArray(R"({"mirrorPublicPort":8125,"version":"1"})") << "\"version\"";
    QTest::newRow("version 0") << QByteArray(R"({"mirrorPublicPort":8125,"version":0})") << "\"version\"";
}

void tst_RuntimeInfo::parseInvalid()
{
    QFETCH(QByteArray, body);
    QFETCH(QString, errorPart);
    QString error;
    QVERIFY(!parseRuntimeJson(body, &error).has_value());
    QVERIFY2(error.contains(errorPart), qPrintable(error));
}

void tst_RuntimeInfo::buildRoundTrip()
{
    QCOMPARE(buildRuntimeJson(8125), QByteArray(R"({"mirrorPublicPort":8125,"version":1})"));
    for (quint16 port : {quint16(1), quint16(8125), quint16(18125), quint16(65535)}) {
        const std::optional<RuntimeInfo> info = parseRuntimeJson(buildRuntimeJson(port));
        QVERIFY(info.has_value());
        QCOMPARE(info->mirrorPublicPort, port);
        QCOMPARE(info->version, kRuntimeJsonVersion);
    }
}

void tst_RuntimeInfo::requestValid()
{
    HttpResponder server(httpResponse(200, "OK", buildRuntimeJson(9125)));
    QVERIFY(server.listen());
    int calls = 0;
    const std::optional<RuntimeLookup> r = lookup(server.url(), 3000, &calls);
    QVERIFY(r.has_value());
    QCOMPARE(calls, 1);
    QVERIFY2(r->fromServer, qPrintable(r->detail));
    QCOMPARE(r->info.mirrorPublicPort, quint16(9125));
    QVERIFY(r->detail.isEmpty());
    QCOMPARE(server.requests().size(), 1);
    QVERIFY(server.requests().first().startsWith("GET /runtime.json HTTP/1.1\r\n"));
}

void tst_RuntimeInfo::requestHttp404FallsBack()
{
    HttpResponder server(httpResponse(404, "Not Found", "not found", "text/plain"));
    QVERIFY(server.listen());
    int calls = 0;
    const std::optional<RuntimeLookup> r = lookup(server.url(), 3000, &calls);
    QVERIFY(r.has_value());
    QCOMPARE(calls, 1);
    QVERIFY(!r->fromServer);
    QCOMPARE(r->info.mirrorPublicPort, kDefaultMirrorPublicPort);
    QVERIFY2(!r->detail.isEmpty(), "detail explains the fallback");
}

void tst_RuntimeInfo::requestInvalidContentFallsBack()
{
    // e.g. a web server that answers every unknown path with the index page.
    HttpResponder server(httpResponse(200, "OK", "<!doctype html><title>TaidaFlow</title>", "text/html"));
    QVERIFY(server.listen());
    int calls = 0;
    const std::optional<RuntimeLookup> r = lookup(server.url(), 3000, &calls);
    QVERIFY(r.has_value());
    QCOMPARE(calls, 1);
    QVERIFY(!r->fromServer);
    QCOMPARE(r->info.mirrorPublicPort, kDefaultMirrorPublicPort);
    QVERIFY2(r->detail.startsWith(QStringLiteral("invalid content")), qPrintable(r->detail));
}

void tst_RuntimeInfo::requestTimeoutFallsBack()
{
    HttpResponder server{QByteArray()};   // accepts, never answers
    QVERIFY(server.listen());
    int calls = 0;
    qint64 elapsed = 0;
    const std::optional<RuntimeLookup> r = lookup(server.url(), 500, &calls, &elapsed);
    QVERIFY(r.has_value());
    QCOMPARE(calls, 1);
    QVERIFY(!r->fromServer);
    QCOMPARE(r->info.mirrorPublicPort, kDefaultMirrorPublicPort);
    QVERIFY2(r->detail.contains(QStringLiteral("no answer within 500 ms")), qPrintable(r->detail));
    QVERIFY2(elapsed >= 450 && elapsed < 3000, qPrintable(QString::number(elapsed)));
}

void tst_RuntimeInfo::requestRefusedFallsBack()
{
    // A port that was just free: nothing listens there.
    quint16 port = 0;
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        port = probe.serverPort();
    }
    int calls = 0;
    const std::optional<RuntimeLookup> r =
        lookup(QUrl(QStringLiteral("http://127.0.0.1:%1/runtime.json").arg(port)), 3000, &calls);
    QVERIFY(r.has_value());
    QCOMPARE(calls, 1);
    QVERIFY(!r->fromServer);
    QCOMPARE(r->info.mirrorPublicPort, kDefaultMirrorPublicPort);
    QVERIFY2(!r->detail.isEmpty(), "detail explains the fallback");
}

void tst_RuntimeInfo::requestInvalidUrlFallsBackAsynchronously()
{
    QObject context;
    int calls = 0;
    RuntimeLookup result;
    requestRuntimeInfo(QUrl(), 3000, &context, [&](const RuntimeLookup &r) {
        ++calls;
        result = r;
    });
    QCOMPARE(calls, 0);   // never synchronous: the caller finishes its setup first
    QTRY_COMPARE(calls, 1);
    QVERIFY(!result.fromServer);
    QCOMPARE(result.info.mirrorPublicPort, kDefaultMirrorPublicPort);
    QVERIFY(result.detail.contains(QStringLiteral("no usable URL")));
}

void tst_RuntimeInfo::requestContextDestroyedNoCallback()
{
    HttpResponder server{QByteArray()};
    QVERIFY(server.listen());
    int calls = 0;
    {
        auto context = std::make_unique<QObject>();
        requestRuntimeInfo(server.url(), 300, context.get(), [&](const RuntimeLookup &) { ++calls; });
        QTest::qWait(50);
    }
    QTest::qWait(700);
    QCOMPARE(calls, 0);
}

QTEST_GUILESS_MAIN(tst_RuntimeInfo)
#include "tst_runtimeinfo.moc"
