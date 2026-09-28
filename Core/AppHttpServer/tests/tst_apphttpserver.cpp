// Stand-alone QTest of AppHttpServer (only ../AppHttpServer.{h,cpp} + Qt).
// Every request goes over a real TCP connection (QTcpSocket, raw HTTP/1.1) so that the
// request target reaches the server exactly as written (no client-side path clean-up).
#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QScopeGuard>
#include <QProcess>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QtHttpServer/QHttpServerRequest>
#include <QtHttpServer/QHttpServerResponder>

#include "AppHttpServer.h"

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

#include <atomic>
#include <functional>

namespace {
const QString kWork = QStringLiteral(APPHTTP_WORK_DIR);

double privateMB()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
    return pmc.PrivateUsage / 1048576.0;
#else
    return 0.0;
#endif
}

struct Reply
{
    int status = 0;
    QByteArray head;
    QByteArray body;
    QByteArray header(const QByteArray &name) const
    {
        for (const QByteArray &line : head.split('\n')) {
            const int colon = line.indexOf(':');
            if (colon > 0 && line.left(colon).trimmed().toLower() == name.toLower())
                return line.mid(colon + 1).trimmed();
        }
        return {};
    }
    bool has(const QByteArray &name) const
    {
        for (const QByteArray &line : head.split('\n')) {
            const int colon = line.indexOf(':');
            if (colon > 0 && line.left(colon).trimmed().toLower() == name.toLower())
                return true;
        }
        return false;
    }
};

// One request on its own connection ("Connection: close"); the reply is read until the
// announced Content-Length arrived or the server closed.  For HEAD / 304 it also waits
// 300 ms for (forbidden) body bytes so that a body sent by mistake would be seen.
Reply http(const QByteArray &method, const QByteArray &target, quint16 port,
           const QList<QPair<QByteArray, QByteArray>> &headers = {})
{
    Reply reply;
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(3000))
        return reply;
    QByteArray req = method + ' ' + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
    for (const auto &h : headers)
        req += h.first + ": " + h.second + "\r\n";
    socket.write(req + "\r\n");
    QByteArray data;
    QElapsedTimer t;
    t.start();
    qint64 headDoneAt = -1;
    while (t.elapsed() < 15000) {
        if (socket.waitForReadyRead(100))
            data += socket.readAll();
        if (socket.state() == QAbstractSocket::UnconnectedState) {
            data += socket.readAll();
            break;
        }
        const int sep = data.indexOf("\r\n\r\n");
        if (sep < 0)
            continue;
        const QByteArray head = data.left(sep);
        const int status = head.split(' ').value(1).toInt();
        const bool noBody = method == "HEAD" || status == 304;
        if (noBody) {
            if (headDoneAt < 0)
                headDoneAt = t.elapsed();
            if (t.elapsed() - headDoneAt >= 300)
                break;
            continue;
        }
        const auto m = QRegularExpression(QStringLiteral("(?im)^content-length:\\s*(\\d+)"))
                               .match(QString::fromLatin1(head));
        if (m.hasMatch() && data.size() - sep - 4 >= m.captured(1).toLongLong())
            break;
    }
    const int sep = data.indexOf("\r\n\r\n");
    reply.head = sep >= 0 ? data.left(sep) : data;
    reply.body = sep >= 0 ? data.mid(sep + 4) : QByteArray();
    reply.status = reply.head.split('\n').value(0).split(' ').value(1).toInt();
    return reply;
}

void writeFile(const QString &path, const QByteArray &bytes, const QDateTime &mtime = {})
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(path));
    f.write(bytes);
    if (mtime.isValid())
        QVERIFY(f.setFileTime(mtime, QFileDevice::FileModificationTime));
    f.close();
}

quint32 crc32(const QByteArray &data)
{
    static quint32 table[256];
    static bool init = false;
    if (!init) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    quint32 crc = 0xFFFFFFFFu;
    for (const char ch : data)
        crc = table[(crc ^ quint8(ch)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// A real gzip member (RFC 1952) around qCompress's raw deflate stream.
QByteArray gzip(const QByteArray &data)
{
    const QByteArray z = qCompress(data, 9);           // 4-byte size + zlib (2 hdr .. 4 adler)
    const QByteArray deflate = z.mid(4 + 2, z.size() - 4 - 2 - 4);
    QByteArray out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff", 10);
    out += deflate;
    const quint32 crc = crc32(data);
    const quint32 len = quint32(data.size());
    for (int i = 0; i < 4; ++i)
        out += char((crc >> (8 * i)) & 0xFF);
    for (int i = 0; i < 4; ++i)
        out += char((len >> (8 * i)) & 0xFF);
    return out;
}

bool exportName(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,40}_\\d{8}_\\d{6}\\.csv$"));
    return re.match(name).hasMatch();
}

// Reads a response and throws the body away; tracks the process private memory peak.
struct StreamResult
{
    int status = 0;
    qint64 contentLength = -1;
    qint64 bodyBytes = 0;
    double peakPrivMB = 0;
    double ms = 0;
};

StreamResult streamDownload(const QByteArray &target, quint16 port, int pauseMsAfterFirstChunk,
                            double *pausePeakMB = nullptr)
{
    StreamResult r;
    QTcpSocket socket;
    socket.setReadBufferSize(64 * 1024);               // back-pressure: the kernel window fills up
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(3000))
        return r;
    socket.write("GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    QByteArray head;
    bool headDone = false;
    bool paused = false;
    QElapsedTimer t, sample;
    t.start();
    sample.start();
    r.peakPrivMB = privateMB();
    while (t.elapsed() < 600000) {
        if (!socket.waitForReadyRead(200) && socket.state() == QAbstractSocket::UnconnectedState
            && socket.bytesAvailable() == 0)
            break;
        const QByteArray chunk = socket.read(1 << 20);
        if (!headDone) {
            head += chunk;
            const int sep = head.indexOf("\r\n\r\n");
            if (sep < 0)
                continue;
            headDone = true;
            r.status = head.split(' ').value(1).toInt();
            const auto m = QRegularExpression(QStringLiteral("(?im)^content-length:\\s*(\\d+)"))
                                   .match(QString::fromLatin1(head.left(sep)));
            r.contentLength = m.hasMatch() ? m.captured(1).toLongLong() : -1;
            r.bodyBytes = head.size() - sep - 4;
        } else {
            r.bodyBytes += chunk.size();
        }
        if (sample.elapsed() >= 20) {
            r.peakPrivMB = std::max(r.peakPrivMB, privateMB());
            sample.restart();
        }
        if (headDone && !paused && pauseMsAfterFirstChunk > 0) {
            // Slow client: stop reading; the server must wait instead of buffering the file.
            paused = true;
            double peak = privateMB();
            QElapsedTimer p;
            p.start();
            while (p.elapsed() < pauseMsAfterFirstChunk) {
                QThread::msleep(50);
                peak = std::max(peak, privateMB());
            }
            if (pausePeakMB)
                *pausePeakMB = peak;
            r.peakPrivMB = std::max(r.peakPrivMB, peak);
        }
        if (headDone && r.contentLength >= 0 && r.bodyBytes >= r.contentLength)
            break;
    }
    r.ms = t.nsecsElapsed() / 1.0e6;
    return r;
}
} // namespace

class TestAppHttpServer : public QObject
{
    Q_OBJECT

    QString m_web;
    QString m_outside;
    QString m_downloads;
    quint16 m_port = 0;
    AppHttpServer &server() { return AppHttpServer::instance(); }

    AppHttpServer::StaticOptions webOptions() const
    {
        AppHttpServer::StaticOptions o;
        o.indexFile = QStringLiteral("index.html");
        // w2-062: runtime.json is rewritten by the application at every start.
        o.fileCacheControl.insert(QStringLiteral("runtime.json"), QByteArrayLiteral("no-store"));
        return o;
    }

private slots:
    void initTestCase()
    {
        QDir().rmdir(kWork + QStringLiteral("/web/escape"));   // junction left by an aborted run
        QDir(kWork).removeRecursively();
        m_web = kWork + QStringLiteral("/web");
        m_outside = kWork + QStringLiteral("/outside");
        m_downloads = kWork + QStringLiteral("/downloads");
        QVERIFY(QDir().mkpath(m_web));
        QVERIFY(QDir().mkpath(m_outside));
        QVERIFY(QDir().mkpath(m_downloads));
        const QDateTime old = QDateTime::currentDateTime().addSecs(-3600);
        writeFile(m_web + QStringLiteral("/index.html"), "<!doctype html><title>index</title>\n", old);
        writeFile(m_web + QStringLiteral("/app.html"), "<!doctype html><title>app</title>\n", old);
        writeFile(m_web + QStringLiteral("/app.js"), "console.log('app');\n", old);
        writeFile(m_web + QStringLiteral("/mod.mjs"), "export const x = 1;\n", old);
        QByteArray wasm("\0asm\x01\0\0\0", 8);
        for (int i = 0; i < 20000; ++i)
            wasm += QByteArray::number(i % 97) + ' ';     // compressible payload
        writeFile(m_web + QStringLiteral("/app.wasm"), wasm, old);
        writeFile(m_web + QStringLiteral("/app.wasm.gz"), gzip(wasm), old.addSecs(1));
        writeFile(m_web + QStringLiteral("/style.css"), "body{}\n", old);
        writeFile(m_web + QStringLiteral("/data.json"), "{\"a\":1}\n", old);
        writeFile(m_web + QStringLiteral("/img.png"), QByteArray("\x89PNG\r\n\x1a\n", 8), old);
        writeFile(m_web + QStringLiteral("/icon.svg"), "<svg xmlns=\"http://www.w3.org/2000/svg\"/>\n", old);
        writeFile(m_web + QStringLiteral("/favicon.ico"), QByteArray("\0\0\1\0", 4), old);
        writeFile(m_web + QStringLiteral("/font.ttf"), QByteArray("\0\1\0\0", 4), old);
        writeFile(m_web + QStringLiteral("/font.woff2"), "wOF2", old);
        writeFile(m_web + QStringLiteral("/noext"), "raw", old);
        writeFile(m_web + QStringLiteral("/.hidden"), "SECRET-HIDDEN", old);
        writeFile(m_web + QStringLiteral("/sub/index.html"), "<p>sub</p>\n", old);
        QVERIFY(QDir().mkpath(m_web + QStringLiteral("/empty")));
        writeFile(m_outside + QStringLiteral("/secret.txt"), "SECRET-OUTSIDE", old);
        writeFile(m_outside + QStringLiteral("/web-x_20260101_000000.csv"), "SECRET-OUTSIDE-CSV", old);
        writeFile(kWork + QStringLiteral("/secret.txt"), "SECRET-PARENT", old);

        QVERIFY(server().mountStatic(QStringLiteral("/"), m_web, webOptions()));
        QVERIFY(server().mountDownloads(QStringLiteral("/exports"), m_downloads, exportName));
        QVERIFY(server().start(0, QHostAddress::LocalHost));   // any free port
        QVERIFY(server().isListening());
        m_port = server().port();
        QVERIFY(m_port != 0);
        qInfo() << "listening on port" << m_port << "mounts" << server().mountedPrefixes();
    }

    void mimeTypes_data()
    {
        QTest::addColumn<QByteArray>("path");
        QTest::addColumn<QByteArray>("type");
        QTest::newRow("html") << QByteArray("/app.html") << QByteArray("text/html; charset=utf-8");
        QTest::newRow("js") << QByteArray("/app.js") << QByteArray("text/javascript; charset=utf-8");
        QTest::newRow("mjs") << QByteArray("/mod.mjs") << QByteArray("text/javascript; charset=utf-8");
        QTest::newRow("wasm") << QByteArray("/app.wasm") << QByteArray("application/wasm");
        QTest::newRow("css") << QByteArray("/style.css") << QByteArray("text/css; charset=utf-8");
        QTest::newRow("json") << QByteArray("/data.json") << QByteArray("application/json");
        QTest::newRow("png") << QByteArray("/img.png") << QByteArray("image/png");
        QTest::newRow("svg") << QByteArray("/icon.svg") << QByteArray("image/svg+xml");
        QTest::newRow("ico") << QByteArray("/favicon.ico") << QByteArray("image/x-icon");
        QTest::newRow("ttf") << QByteArray("/font.ttf") << QByteArray("font/ttf");
        QTest::newRow("woff2") << QByteArray("/font.woff2") << QByteArray("font/woff2");
        QTest::newRow("no suffix") << QByteArray("/noext") << QByteArray("application/octet-stream");
    }

    void mimeTypes()
    {
        QFETCH(QByteArray, path);
        QFETCH(QByteArray, type);
        const Reply r = http("GET", path, m_port);   // no Accept-Encoding: identity
        QCOMPARE(r.status, 200);
        QCOMPARE(r.header("Content-Type"), type);
        QFile f(m_web + QString::fromLatin1(path));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(r.body, f.readAll());
        QCOMPARE(r.header("Content-Length").toLongLong(), f.size());
        QCOMPARE(r.header("Cross-Origin-Opener-Policy"), QByteArray("same-origin"));
        QCOMPARE(r.header("Cross-Origin-Embedder-Policy"), QByteArray("require-corp"));
        QCOMPARE(r.header("Cross-Origin-Resource-Policy"), QByteArray("same-origin"));
        QCOMPARE(r.header("Cache-Control"), QByteArray("no-cache"));
        QVERIFY(r.header("ETag").startsWith('"'));
        QVERIFY(r.header("Last-Modified").endsWith(" GMT"));
        QVERIFY(!r.has("Content-Encoding"));
    }

    // w2-062: StaticOptions::fileCacheControl - one file name gets its own Cache-Control
    // (runtime.json: no-store); every other file keeps the default (no-cache).
    void perFileCacheControl()
    {
        const QString path = m_web + QStringLiteral("/runtime.json");
        const QByteArray v1 = R"({"mirrorPublicPort":8125,"version":1})";
        const QByteArray v2 = R"({"mirrorPublicPort":9125,"version":1})";
        writeFile(path, v1, QDateTime::currentDateTime().addSecs(-120));
        const Reply r = http("GET", "/runtime.json", m_port);
        QCOMPARE(r.status, 200);
        QCOMPARE(r.header("Content-Type"), QByteArray("application/json"));
        QCOMPARE(r.header("Cache-Control"), QByteArray("no-store"));
        QCOMPARE(r.body, v1);
        QCOMPARE(http("HEAD", "/runtime.json", m_port).header("Cache-Control"), QByteArray("no-store"));
        // Case-insensitive file name match (Windows file system).
        QCOMPARE(http("GET", "/RUNTIME.json", m_port).header("Cache-Control"), QByteArray("no-store"));
        // Other files unchanged.
        QCOMPARE(http("GET", "/data.json", m_port).header("Cache-Control"), QByteArray("no-cache"));
        QCOMPARE(http("GET", "/app.html", m_port).header("Cache-Control"), QByteArray("no-cache"));
        // Rewritten at the next start: the new content is served at once.
        writeFile(path, v2);
        const Reply r2 = http("GET", "/runtime.json", m_port);
        QCOMPARE(r2.status, 200);
        QCOMPARE(r2.body, v2);
        QCOMPARE(r2.header("Cache-Control"), QByteArray("no-store"));
        QVERIFY(QFile::remove(path));
    }

    void conditionalRequests()
    {
        const QString path = m_web + QStringLiteral("/cond.js");
        writeFile(path, "v1();\n", QDateTime::currentDateTime().addSecs(-600));
        const Reply first = http("GET", "/cond.js", m_port);
        QCOMPARE(first.status, 200);
        const QByteArray etag = first.header("ETag");
        const QByteArray lastModified = first.header("Last-Modified");
        qInfo().noquote() << "first:" << first.status << "ETag" << etag << "Last-Modified" << lastModified;

        const Reply inm = http("GET", "/cond.js", m_port, {{"If-None-Match", etag}});
        QCOMPARE(inm.status, 304);
        QVERIFY(inm.body.isEmpty());
        QCOMPARE(inm.header("ETag"), etag);
        QCOMPARE(inm.header("Cache-Control"), QByteArray("no-cache"));
        QCOMPARE(inm.header("Cross-Origin-Embedder-Policy"), QByteArray("require-corp"));
        QCOMPARE(http("GET", "/cond.js", m_port, {{"If-None-Match", "W/" + etag}}).status, 304);
        QCOMPARE(http("GET", "/cond.js", m_port, {{"If-None-Match", "\"other\", " + etag}}).status, 304);
        QCOMPARE(http("GET", "/cond.js", m_port, {{"If-None-Match", "\"other\""}}).status, 200);
        QCOMPARE(http("HEAD", "/cond.js", m_port, {{"If-None-Match", etag}}).status, 304);
        const Reply ims = http("GET", "/cond.js", m_port, {{"If-Modified-Since", lastModified}});
        QCOMPARE(ims.status, 304);
        QVERIFY(ims.body.isEmpty());
        QCOMPARE(http("GET", "/cond.js", m_port, {{"If-Modified-Since", "Thu, 01 Jan 2015 00:00:00 GMT"}}).status, 200);
        // If-None-Match wins over If-Modified-Since (RFC 9110 §13.1.3).
        QCOMPARE(http("GET", "/cond.js", m_port, {{"If-None-Match", "\"other\""}, {"If-Modified-Since", lastModified}}).status, 200);

        // "Rebuild": new content -> new ETag -> the old validator gets 200 with the new bytes.
        writeFile(path, "v2(); // rebuilt\n");
        const Reply rebuilt = http("GET", "/cond.js", m_port, {{"If-None-Match", etag}});
        qInfo().noquote() << "after rebuild with the old ETag:" << rebuilt.status << "new ETag" << rebuilt.header("ETag");
        QCOMPARE(rebuilt.status, 200);
        QCOMPARE(rebuilt.body, QByteArray("v2(); // rebuilt\n"));
        QVERIFY(rebuilt.header("ETag") != etag);
        QCOMPARE(http("GET", "/cond.js", m_port, {{"If-Modified-Since", lastModified}}).status, 200);
    }

    void gzipVariant()
    {
        QFile plain(m_web + QStringLiteral("/app.wasm")), gz(m_web + QStringLiteral("/app.wasm.gz"));
        QVERIFY(plain.open(QIODevice::ReadOnly) && gz.open(QIODevice::ReadOnly));
        const QByteArray plainBytes = plain.readAll(), gzBytes = gz.readAll();
        const Reply g = http("GET", "/app.wasm", m_port, {{"Accept-Encoding", "gzip, deflate, br"}});
        qInfo().noquote() << "gzip:" << g.status << g.header("Content-Encoding") << "length" << g.header("Content-Length")
                          << "(plain" << plainBytes.size() << "gz" << gzBytes.size() << ")";
        QCOMPARE(g.status, 200);
        QCOMPARE(g.header("Content-Encoding"), QByteArray("gzip"));
        QCOMPARE(g.header("Content-Type"), QByteArray("application/wasm"));
        QCOMPARE(g.header("Vary"), QByteArray("Accept-Encoding"));
        QCOMPARE(g.header("Content-Length").toLongLong(), qint64(gzBytes.size()));
        QCOMPARE(g.body, gzBytes);
        const Reply id = http("GET", "/app.wasm", m_port);
        QCOMPARE(id.status, 200);
        QVERIFY(!id.has("Content-Encoding"));
        QCOMPARE(id.header("Vary"), QByteArray("Accept-Encoding"));
        QCOMPARE(id.body, plainBytes);
        QVERIFY(id.header("ETag") != g.header("ETag"));
        QCOMPARE(http("GET", "/app.wasm", m_port, {{"Accept-Encoding", "gzip;q=0, deflate"}}).header("Content-Encoding"), QByteArray());
        QCOMPARE(http("GET", "/app.wasm", m_port, {{"Accept-Encoding", "*"}}).header("Content-Encoding"), QByteArray("gzip"));
        // 304 on the gzip variant with its own ETag
        QCOMPARE(http("GET", "/app.wasm", m_port, {{"Accept-Encoding", "gzip"}, {"If-None-Match", g.header("ETag")}}).status, 304);
        QCOMPARE(http("GET", "/app.wasm", m_port, {{"If-None-Match", g.header("ETag")}}).status, 200);

        // A .gz older than its file (left over from a previous build) is never served.
        writeFile(m_web + QStringLiteral("/stale.js"), "new();\n", QDateTime::currentDateTime());
        writeFile(m_web + QStringLiteral("/stale.js.gz"), gzip("old();\n"), QDateTime::currentDateTime().addSecs(-60));
        const Reply stale = http("GET", "/stale.js", m_port, {{"Accept-Encoding", "gzip"}});
        QCOMPARE(stale.status, 200);
        QVERIFY(!stale.has("Content-Encoding"));
        QCOMPARE(stale.body, QByteArray("new();\n"));
    }

    void headRequests()
    {
        const Reply h = http("HEAD", "/app.wasm", m_port);
        QCOMPARE(h.status, 200);
        QCOMPARE(h.header("Content-Length").toLongLong(), QFileInfo(m_web + QStringLiteral("/app.wasm")).size());
        QCOMPARE(h.header("Content-Type"), QByteArray("application/wasm"));
        QVERIFY2(h.body.isEmpty(), h.body.left(20).constData());
        QCOMPARE(h.header("Cross-Origin-Opener-Policy"), QByteArray("same-origin"));
        const Reply hg = http("HEAD", "/app.wasm", m_port, {{"Accept-Encoding", "gzip"}});
        QCOMPARE(hg.header("Content-Encoding"), QByteArray("gzip"));
        QCOMPARE(hg.header("Content-Length").toLongLong(), QFileInfo(m_web + QStringLiteral("/app.wasm.gz")).size());
        QVERIFY(hg.body.isEmpty());
        QCOMPARE(http("HEAD", "/missing.js", m_port).status, 404);
    }

    void defaultPageAndNoListing()
    {
        const Reply root = http("GET", "/", m_port);
        QCOMPARE(root.status, 302);
        QCOMPARE(root.header("Location"), QByteArray("/index.html"));
        const Reply sub = http("GET", "/sub/", m_port);
        QCOMPARE(sub.status, 302);
        QCOMPARE(sub.header("Location"), QByteArray("/sub/index.html"));
        QCOMPARE(http("GET", "/sub", m_port).header("Location"), QByteArray("/sub/index.html"));
        const Reply empty = http("GET", "/empty/", m_port);
        QCOMPARE(empty.status, 404);                   // folder without default page: no listing
        QVERIFY(!empty.body.contains("secret") && !empty.body.contains("index"));
        QCOMPARE(http("GET", "/app.js/", m_port).status, 404);

        // Serve the default page directly, and a mount under a prefix.
        AppHttpServer::StaticOptions direct = webOptions();
        direct.redirectToIndex = false;
        direct.indexFile = QStringLiteral("app.html");
        direct.crossOriginIsolation = false;
        direct.crossOriginResourcePolicy = false;
        QVERIFY(server().mountStatic(QStringLiteral("/site/"), m_web, direct));
        const Reply site = http("GET", "/site/", m_port);
        QCOMPARE(site.status, 200);
        QCOMPARE(site.body, QByteArray("<!doctype html><title>app</title>\n"));
        QVERIFY(!site.has("Cross-Origin-Opener-Policy") && !site.has("Cross-Origin-Embedder-Policy")
                && !site.has("Cross-Origin-Resource-Policy"));
        QCOMPARE(http("GET", "/site/app.js", m_port).status, 200);
        QCOMPARE(http("GET", "/site/style.css", m_port).status, 200);
        // Suffix allow-list (e.g. a build folder that also holds CMakeCache.txt).
        direct.fileSuffixes = QStringList{QStringLiteral("html"), QStringLiteral("js")};
        QVERIFY(server().mountStatic(QStringLiteral("/site"), m_web, direct));
        QCOMPARE(http("GET", "/site/app.js", m_port).status, 200);
        QCOMPARE(http("GET", "/site/", m_port).status, 200);
        QCOMPARE(http("GET", "/site/style.css", m_port).status, 404);
        QCOMPARE(http("GET", "/site/noext", m_port).status, 404);
        QVERIFY(server().unmount(QStringLiteral("/site")));
        QCOMPARE(http("GET", "/site/app.js", m_port).status, 404);   // falls back to "/": no such file
        QVERIFY(!server().unmount(QStringLiteral("/site")));
    }

    void traversalRejected()
    {
        // A directory junction inside the web folder that points outside it.
        const QString junction = m_web + QStringLiteral("/escape");
        QProcess mk;
        mk.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                                             QDir::toNativeSeparators(junction), QDir::toNativeSeparators(m_outside)});
        QVERIFY(mk.waitForFinished(10000));
        qInfo().noquote() << "mklink /J:" << mk.exitCode() << mk.readAllStandardOutput().trimmed();
        QCOMPARE(mk.exitCode(), 0);
        QVERIFY(QFileInfo(junction + QStringLiteral("/secret.txt")).exists());

        const QList<QByteArray> attacks{
            "/../secret.txt",
            "/%2e%2e/secret.txt",
            "/%2E%2E/secret.txt",
            "/%2e%2e%2fsecret.txt",
            "/..%2Fsecret.txt",
            "/..%5Csecret.txt",
            "/..\\secret.txt",
            "/sub/../../secret.txt",
            "/sub/%2e%2e/%2e%2e/secret.txt",
            "/./index.html",
            "//secret.txt",
            "/C:%5CWindows%5Cwin.ini",
            "/C:/Windows/win.ini",
            "/%5C%5C127.0.0.1%5Cc$%5Cwindows%5Cwin.ini",
            "/index.html%00.txt",
            "/index.html.",
            "/index.html%20",
            "/index.html::$DATA",
            "/NUL",
            "/con.txt",
            "/.hidden",
            "/%2ehidden",
            "/escape/secret.txt",
            "/escape/",
            "/escape",
            "/exports/../secret.txt",
            "/exports/%2e%2e/outside/secret.txt",
        };
        for (const QByteArray &target : attacks) {
            const Reply r = http("GET", target, m_port);
            qInfo().noquote() << "GET" << target << "->" << r.status << r.body.left(30).trimmed();
            QVERIFY2(r.status == 400 || r.status == 403 || r.status == 404, target.constData());
            QVERIFY2(!r.body.contains("SECRET"), target.constData());
            const Reply h = http("HEAD", target, m_port);
            QVERIFY2(h.status == 400 || h.status == 403 || h.status == 404, target.constData());
        }
        QVERIFY(QDir().rmdir(junction));               // removes the junction only, never its target
        QVERIFY(QFileInfo::exists(m_outside + QStringLiteral("/secret.txt")));
    }

    void downloads()
    {
        const QByteArray csv = "\xEF\xBB\xBF\"a\",\"b\"\r\n\"1\",\"2\"\r\n";
        writeFile(m_downloads + QStringLiteral("/web-abc_20260926_101010.csv"), csv);
        const Reply ok = http("GET", "/exports/web-abc_20260926_101010.csv", m_port);
        QCOMPARE(ok.status, 200);
        QCOMPARE(ok.header("Content-Type"), QByteArray("text/csv; charset=utf-8"));
        QCOMPARE(ok.header("Content-Disposition"), QByteArray("attachment; filename=\"web-abc_20260926_101010.csv\""));
        QCOMPARE(ok.header("Access-Control-Allow-Origin"), QByteArray("*"));
        QCOMPARE(ok.header("Cache-Control"), QByteArray("no-store"));
        QCOMPARE(ok.header("Content-Length").toLongLong(), qint64(csv.size()));
        QCOMPARE(ok.body, csv);
        QVERIFY(!ok.has("ETag"));
        const Reply head = http("HEAD", "/exports/web-abc_20260926_101010.csv", m_port);
        QCOMPARE(head.status, 200);
        QCOMPARE(head.header("Content-Length").toLongLong(), qint64(csv.size()));
        QVERIFY(head.body.isEmpty());

        writeFile(m_downloads + QStringLiteral("/notes.txt"), "SECRET-NOT-WHITELISTED");
        QVERIFY(QDir().mkpath(m_downloads + QStringLiteral("/sub")));
        writeFile(m_downloads + QStringLiteral("/sub/web-sub_20260926_101010.csv"), "SECRET-SUBFOLDER");
        const QList<QPair<QByteArray, int>> cases{
            {"/exports/notes.txt", 400},
            {"/exports/web-abc_20260926_101010.csv.tmp", 400},
            {"/exports/nothere_20260101_000000.csv", 404},
            {"/exports/", 404},
            {"/exports", 404},
            {"/exports/sub/web-sub_20260926_101010.csv", 400},
            {"/exports/../outside/web-x_20260101_000000.csv", 400},
            {"/exports/..%5Coutside%5Cweb-x_20260101_000000.csv", 400},
            {"/exports/%2e%2e%2foutside%2fweb-x_20260101_000000.csv", 400},
            {"/exports//web-abc_20260926_101010.csv", 400},
        };
        for (const auto &c : cases) {
            const Reply r = http("GET", c.first, m_port);
            qInfo().noquote() << "GET" << c.first << "->" << r.status;
            QVERIFY2(r.status == c.second, c.first.constData());
            QVERIFY(!r.body.contains("SECRET"));
            QCOMPARE(r.header("Access-Control-Allow-Origin"), QByteArray("*"));
        }
        // A symbolic link with an allowed name that points outside the folder: 403.  Creating
        // file symlinks needs Developer Mode or the privilege on Windows; if that is not
        // available the case cannot be built here (junctions are covered in traversalRejected).
        const QString link = m_downloads + QStringLiteral("/web-lnk_20260926_101010.csv");
        QProcess mk;
        mk.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"), QDir::toNativeSeparators(link),
                                             QDir::toNativeSeparators(m_outside + QStringLiteral("/web-x_20260101_000000.csv"))});
        mk.waitForFinished(10000);
        if (mk.exitCode() == 0) {
            const Reply r = http("GET", "/exports/web-lnk_20260926_101010.csv", m_port);
            qInfo().noquote() << "file symlink to outside ->" << r.status;
            QCOMPARE(r.status, 403);
            QVERIFY(!r.body.contains("SECRET"));
            QFile::remove(link);
        } else {
            qInfo().noquote() << "file symlink not creatable here (" << mk.readAllStandardError().trimmed()
                              << ") - symlink case covered by the junction test";
        }

        // Other methods: 405 with Allow, nothing written.
        const Reply post = http("POST", "/exports/web-abc_20260926_101010.csv", m_port);
        QCOMPARE(post.status, 405);
        QCOMPARE(post.header("Allow"), QByteArray("GET, HEAD"));
        QCOMPARE(http("PUT", "/index.html", m_port).status, 405);
        QCOMPARE(http("DELETE", "/exports/web-abc_20260926_101010.csv", m_port).status, 405);
        QVERIFY(QFileInfo::exists(m_downloads + QStringLiteral("/web-abc_20260926_101010.csv")));

        // Unmount: gone.
        QVERIFY(server().unmount(QStringLiteral("/exports")));
        QCOMPARE(http("GET", "/exports/web-abc_20260926_101010.csv", m_port).status, 404);
        QVERIFY(server().mountDownloads(QStringLiteral("/exports"), m_downloads, exportName));
        QCOMPARE(http("GET", "/exports/web-abc_20260926_101010.csv", m_port).status, 200);
        // Invalid registrations are refused.
        QVERIFY(!server().mountDownloads(QStringLiteral("/x"), m_downloads, {}));
        QVERIFY(!server().mountStatic(QStringLiteral("relative"), m_web));
        QVERIFY(!server().mountStatic(QStringLiteral("/a/../b"), m_web));
        QVERIFY(!server().mountStatic(QStringLiteral("/ok"), QString()));
    }

    void customRoutes()
    {
        QString handlerThread;
        std::atomic_int calls{0};
        QVERIFY(server().addGetRoute(QStringLiteral("/api/ping"), [&](const QHttpServerRequest &request,
                                                                      QHttpServerResponder &responder) {
            handlerThread = QThread::currentThread()->objectName();
            ++calls;
            QJsonObject o{{QStringLiteral("pong"), true},
                          {QStringLiteral("q"), request.query().queryItemValue(QStringLiteral("q"))}};
            responder.write(QJsonDocument(o));
        }));
        // Route wins over the "/" static mount even if a file of that name existed.
        writeFile(m_web + QStringLiteral("/api/ping"), "SECRET-FILE-SHADOWED");
        const Reply r = http("GET", "/api/ping?q=42", m_port);
        qInfo().noquote() << "route:" << r.status << r.body << "handler thread" << handlerThread;
        QCOMPARE(r.status, 200);
        QCOMPARE(r.header("Content-Type"), QByteArray("application/json"));
        QCOMPARE(QJsonDocument::fromJson(r.body).object().value(QStringLiteral("q")).toString(), QStringLiteral("42"));
        QCOMPARE(handlerThread, QStringLiteral("AppHttpServerThread"));
        QVERIFY(QThread::currentThread()->objectName() != handlerThread);
        QCOMPARE(http("HEAD", "/api/ping", m_port).status, 405);
        QCOMPARE(http("POST", "/api/ping", m_port).status, 405);
        QCOMPARE(calls.load(), 1);
        QVERIFY(server().routePaths().contains(QStringLiteral("/api/ping")));
        QVERIFY(server().removeGetRoute(QStringLiteral("/api/ping")));
        QVERIFY(!server().removeGetRoute(QStringLiteral("/api/ping")));
        QCOMPARE(http("GET", "/api/ping", m_port).body, QByteArray("SECRET-FILE-SHADOWED"));   // static again
        QFile::remove(m_web + QStringLiteral("/api/ping"));
        QDir(m_web).rmdir(QStringLiteral("api"));
    }

    void bindFailureAndRestart()
    {
        // Same address and port while listening: nothing to do.
        QVERIFY(server().start(m_port, QHostAddress::LocalHost));
        QCOMPARE(server().port(), m_port);

        server().stop();
        QVERIFY(!server().isListening());
        QCOMPARE(server().port(), quint16(0));
        QTcpServer blocker;                             // someone else holds the port
        QVERIFY(blocker.listen(QHostAddress::AnyIPv4, 0));
        const quint16 taken = blocker.serverPort();
        QVERIFY(!server().start(taken));
        qInfo().noquote() << "start on busy port" << taken << "->" << server().isListening() << server().lastError();
        QVERIFY(!server().isListening());
        QVERIFY(!server().lastError().isEmpty());
        QCOMPARE(server().port(), quint16(0));
        blocker.close();
        QVERIFY(server().start(taken, QHostAddress::AnyIPv4));
        QVERIFY(server().isListening());
        QVERIFY(server().lastError().isEmpty());
        QCOMPARE(http("GET", "/app.js", taken).status, 200);            // mounts survived stop/start
        QCOMPARE(http("GET", "/exports/web-abc_20260926_101010.csv", taken).status, 200);
        // Moving to another address/port stops the old listener.
        QVERIFY(server().start(0, QHostAddress::LocalHost));
        const quint16 moved = server().port();
        QVERIFY(moved != taken);
        QCOMPARE(http("GET", "/app.js", taken).status, 0);              // nobody listens there now
        QCOMPARE(http("GET", "/app.js", moved).status, 200);
        m_port = moved;
    }

    void threadSafeRegistration()
    {
        // Registration from several threads, while stopped and while requests are served.
        // (Per-request / per-registration info lines are switched off for this test only:
        // thousands of them would bury the result.)
        QLoggingCategory::setFilterRules(QStringLiteral("apphttpserver.info=false"));
        const auto restoreLog = qScopeGuard([] { QLoggingCategory::setFilterRules(QString()); });
        server().stop();
        constexpr int kThreads = 8;
        constexpr int kRounds = 300;
        const auto worker = [this](int id, bool keepLast) {
            return QThread::create([this, id, keepLast]() {
                const QString prefix = QStringLiteral("/t%1").arg(id);
                const QString route = QStringLiteral("/r%1").arg(id);
                for (int i = 0; i < kRounds; ++i) {
                    server().mountStatic(prefix, m_web, webOptions());
                    server().mountDownloads(prefix + QStringLiteral("/dl"), m_downloads, exportName);
                    server().addGetRoute(route, [id](const QHttpServerRequest &, QHttpServerResponder &resp) {
                        resp.write(QByteArray::number(id), QByteArrayLiteral("text/plain"));
                    });
                    (void)server().mountedPrefixes();
                    (void)server().isListening();
                    if (!keepLast || i + 1 < kRounds) {
                        server().unmount(prefix + QStringLiteral("/dl"));
                        server().unmount(prefix);
                        server().removeGetRoute(route);
                    }
                }
            });
        };
        QList<QThread *> before;
        for (int i = 0; i < kThreads; ++i)
            before << worker(i, true);
        for (QThread *t : before)
            t->start();
        for (QThread *t : before) {
            QVERIFY(t->wait(60000));
            delete t;
        }
        QVERIFY(server().start(0, QHostAddress::LocalHost));   // registered before start
        m_port = server().port();
        for (int i = 0; i < kThreads; ++i) {
            QCOMPARE(http("GET", QStringLiteral("/t%1/app.js").arg(i).toLatin1(), m_port).status, 200);
            QCOMPARE(http("GET", QStringLiteral("/r%1").arg(i).toLatin1(), m_port).body, QByteArray::number(i));
            QCOMPARE(http("GET", QStringLiteral("/t%1/dl/web-abc_20260926_101010.csv").arg(i).toLatin1(), m_port).status, 200);
        }

        // Now churn while serving: registrations from 8 threads + a start() from another
        // thread (same port: no-op) + requests from this thread.
        std::atomic_bool stopRequests{false};
        QList<QThread *> during;
        for (int i = 0; i < kThreads; ++i)
            during << worker(100 + i, false);
        const quint16 port = m_port;
        during << QThread::create([this, port]() {
            for (int i = 0; i < 50; ++i) {
                server().start(port, QHostAddress::LocalHost);
                QThread::msleep(2);
            }
        });
        for (QThread *t : during)
            t->start();
        int served = 0, bad = 0;
        QElapsedTimer clock;
        clock.start();
        while (served < 200 && clock.elapsed() < 60000) {
            const Reply r = http("GET", "/app.js", m_port);
            (r.status == 200 && r.body == "console.log('app');\n") ? ++served : ++bad;
            const Reply rt = http("GET", "/r100", m_port);          // present or not, never broken
            if (!(rt.status == 200 && rt.body == "100") && rt.status != 404)
                ++bad;
        }
        for (QThread *t : during) {
            QVERIFY(t->wait(60000));
            delete t;
        }
        qInfo().noquote() << QStringLiteral("while registering from %1 threads: %2 requests served correctly, %3 bad")
                                     .arg(kThreads + 1).arg(served).arg(bad);
        QCOMPARE(bad, 0);
        QCOMPARE(served, 200);
        for (int i = 0; i < kThreads; ++i) {
            QVERIFY(!server().mountedPrefixes().contains(QStringLiteral("/t%1").arg(100 + i)));
            QVERIFY(server().mountedPrefixes().contains(QStringLiteral("/t%1").arg(i)));
            server().unmount(QStringLiteral("/t%1/dl").arg(i));
            server().unmount(QStringLiteral("/t%1").arg(i));
            server().removeGetRoute(QStringLiteral("/r%1").arg(i));
        }
        QCOMPARE(server().mountedPrefixes(), (QStringList{QStringLiteral("/exports"), QStringLiteral("/")}));
        QVERIFY(server().routePaths().isEmpty());
    }

    void largeFileStreamingMemory()
    {
#ifndef Q_OS_WIN
        QSKIP("process memory counters are measured with the Windows API");
#endif
        const qint64 size = 1024LL * 1024 * 1024;       // 1 GiB
        const QString big = m_downloads + QStringLiteral("/web-big_20260926_000000.csv");
        {
            QFile f(big);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("\"x\"\r\n");
            QVERIFY(f.resize(size));
        }
        {
            QFile f(m_web + QStringLiteral("/big.bin"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("BIN");
            QVERIFY(f.resize(size));
        }

        const double base = privateMB();
        const StreamResult dl = streamDownload("/exports/web-big_20260926_000000.csv", m_port, 0);
        qInfo().noquote() << QStringLiteral("download 1 GiB: status %1, %2 of %3 bytes in %4 ms (%5 MB/s); "
                                            "private memory before %6 MB, peak %7 MB (+%8 MB)")
                                     .arg(dl.status).arg(dl.bodyBytes).arg(dl.contentLength).arg(dl.ms, 0, 'f', 0)
                                     .arg(dl.bodyBytes / 1048576.0 / (dl.ms / 1000.0), 0, 'f', 0)
                                     .arg(base, 0, 'f', 1).arg(dl.peakPrivMB, 0, 'f', 1).arg(dl.peakPrivMB - base, 0, 'f', 1);
        QCOMPARE(dl.status, 200);
        QCOMPARE(dl.contentLength, size);
        QCOMPARE(dl.bodyBytes, size);
        QVERIFY(dl.peakPrivMB - base < 64.0);

        // Slow client: it stops reading for 3 s after the first chunk.  The server must wait
        // for the socket to drain instead of reading the file into memory.
        const double base2 = privateMB();
        double pausePeak = 0;
        const StreamResult slow = streamDownload("/big.bin", m_port, 3000, &pausePeak);
        qInfo().noquote() << QStringLiteral("static 1 GiB with a 3 s reader stall: status %1, %2 bytes; private before %3 MB, "
                                            "peak during the stall %4 MB (+%5 MB), overall peak +%6 MB")
                                     .arg(slow.status).arg(slow.bodyBytes).arg(base2, 0, 'f', 1)
                                     .arg(pausePeak, 0, 'f', 1).arg(pausePeak - base2, 0, 'f', 1)
                                     .arg(slow.peakPrivMB - base2, 0, 'f', 1);
        QCOMPARE(slow.status, 200);
        QCOMPARE(slow.bodyBytes, size);
        QVERIFY(pausePeak - base2 < 64.0);
        QVERIFY(slow.peakPrivMB - base2 < 64.0);
        QFile::remove(big);
        QFile::remove(m_web + QStringLiteral("/big.bin"));
    }

    void cleanupTestCase()
    {
        server().stop();
        QVERIFY(!server().isListening());
    }
};

QTEST_MAIN(TestAppHttpServer)
#include "tst_apphttpserver.moc"
