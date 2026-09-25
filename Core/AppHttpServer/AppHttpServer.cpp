#include "AppHttpServer.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpHeaders>
#include <QList>
#include <QLocale>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QReadLocker>
#include <QReadWriteLock>
#include <QTcpServer>
#include <QThread>
#include <QTimeZone>
#include <QUrl>
#include <QWriteLocker>
#include <QtHttpServer/QHttpServer>
#include <QtHttpServer/QHttpServerRequest>
#include <QtHttpServer/QHttpServerResponder>

#include <algorithm>
#include <atomic>

Q_LOGGING_CATEGORY(lcAppHttpServer, "apphttpserver")

namespace {

using StatusCode = QHttpServerResponder::StatusCode;
using WK = QHttpHeaders::WellKnownHeader;

struct Mount
{
    enum Kind { Static, Downloads };
    Kind kind = Static;
    QString prefix;                    // normalized, "/" or "/a/b"
    QStringList prefixSegments;        // empty for "/"
    QString dir;                       // absolute path
    AppHttpServer::StaticOptions staticOptions;
    AppHttpServer::DownloadOptions downloadOptions;
    AppHttpServer::FileNameValidator validator;
};
using MountPtr = std::shared_ptr<const Mount>;
using RoutePtr = std::shared_ptr<const AppHttpServer::RouteHandler>;

#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

// "/" or "/a/b": leading slash, no trailing slash, every segment a valid name.
bool isValidSegment(const QString &segment);

bool normalizePrefix(const QString &in, QString *out, QStringList *segments)
{
    if (!in.startsWith(QLatin1Char('/')))
        return false;
    QString p = in;
    while (p.size() > 1 && p.endsWith(QLatin1Char('/')))
        p.chop(1);
    QStringList segs;
    if (p != QLatin1String("/")) {
        segs = p.mid(1).split(QLatin1Char('/'));
        for (const QString &s : segs) {
            if (!isValidSegment(s))
                return false;
        }
    }
    *out = p;
    *segments = segs;
    return true;
}

bool isReservedDeviceName(const QString &segment)
{
    // Windows device names are special in every folder, with or without an extension.
    const QString base = segment.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
    static const QStringList fixed{QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
                                   QStringLiteral("NUL"), QStringLiteral("CONIN$"), QStringLiteral("CONOUT$")};
    if (fixed.contains(base))
        return true;
    if (base.size() == 4 && (base.startsWith(QLatin1String("COM")) || base.startsWith(QLatin1String("LPT")))
        && base.at(3) >= QLatin1Char('0') && base.at(3) <= QLatin1Char('9'))
        return true;
    return false;
}

// One decoded path segment that may name a file or folder directly inside its parent:
// no ".", "..", separators, drive / stream colons, wildcards, control characters,
// trailing dot or space (Windows would strip them), no device names.
bool isValidSegment(const QString &segment)
{
    if (segment.isEmpty() || segment == QLatin1String(".") || segment == QLatin1String(".."))
        return false;
    for (const QChar c : segment) {
        const ushort u = c.unicode();
        if (u < 0x20 || u == 0x7f)
            return false;
        switch (u) {
        case '/': case '\\': case ':': case '*': case '?': case '"': case '<': case '>': case '|':
            return false;
        default:
            break;
        }
    }
    if (segment.endsWith(QLatin1Char('.')) || segment.endsWith(QLatin1Char(' ')))
        return false;
    return !isReservedDeviceName(segment);
}

struct ParsedPath
{
    bool ok = false;
    QStringList segments;              // decoded
    bool trailingSlash = false;
    QString decoded;                   // "/" + segments.join("/") (+ "/")
    QString why;
};

ParsedPath parsePath(const QString &encodedPath)
{
    ParsedPath out;
    if (!encodedPath.startsWith(QLatin1Char('/'))) {
        out.why = QStringLiteral("path does not start with '/'");
        return out;
    }
    if (encodedPath.contains(QLatin1Char('\\'))) {
        out.why = QStringLiteral("backslash in path");
        return out;
    }
    const QStringList raw = encodedPath.mid(1).split(QLatin1Char('/'));
    for (int i = 0; i < raw.size(); ++i) {
        const QString &r = raw.at(i);
        if (r.isEmpty()) {
            if (i == raw.size() - 1) {
                out.trailingSlash = i > 0;             // "/a/" (but not "/" itself)
                continue;
            }
            out.why = QStringLiteral("empty path segment");
            return out;
        }
        const QString seg = QUrl::fromPercentEncoding(r.toUtf8());
        if (!isValidSegment(seg)) {
            out.why = QStringLiteral("rejected path segment \"%1\"").arg(r);
            return out;
        }
        out.segments << seg;
    }
    out.decoded = QLatin1Char('/') + out.segments.join(QLatin1Char('/'));
    if (out.trailingSlash)
        out.decoded += QLatin1Char('/');
    out.ok = true;
    return out;
}

QString encodePath(const QStringList &segments)
{
    QString out;
    for (const QString &s : segments)
        out += QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(s));
    return out.isEmpty() ? QStringLiteral("/") : out;
}

// Symbolic links, junctions and shortcuts BELOW the mounted folder are never followed
// (canonicalFilePath() does not resolve NTFS junctions, so every component is checked).
// The mounted folder itself may be a link.
bool noLinksBelow(const QString &root, const QStringList &segments)
{
    QString current = root;
    for (const QString &segment : segments) {
        current += QLatin1Char('/') + segment;
        const QFileInfo info(current);
        if (info.isSymLink() || info.isJunction())
            return false;
    }
    return true;
}

bool insideRoot(const QString &canonicalPath, const QString &canonicalRoot, bool allowRootItself)
{
    if (canonicalPath.isEmpty() || canonicalRoot.isEmpty())
        return false;
    if (canonicalPath.compare(canonicalRoot, kPathCase) == 0)
        return allowRootItself;
    QString root = canonicalRoot;
    if (!root.endsWith(QLatin1Char('/')))
        root += QLatin1Char('/');
    return canonicalPath.startsWith(root, kPathCase);
}

QByteArray httpDate(const QDateTime &time)
{
    return QLocale::c()
        .toString(time.toUTC(), QStringLiteral("ddd, dd MMM yyyy hh:mm:ss 'GMT'"))
        .toLatin1();
}

QDateTime parseHttpDate(QByteArrayView value)
{
    // IMF-fixdate only (RFC 9110 §5.6.7), which every current browser sends.
    QDateTime t = QLocale::c().toDateTime(QString::fromLatin1(value).trimmed(),
                                          QStringLiteral("ddd, dd MMM yyyy hh:mm:ss 'GMT'"));
    if (!t.isValid())
        return {};
    return QDateTime(t.date(), t.time(), QTimeZone::UTC);
}

bool etagMatches(const QByteArray &ifNoneMatch, const QByteArray &etag)
{
    // Weak comparison (RFC 9110 §13.1.2): W/ prefixes are ignored.
    const auto strip = [](QByteArray t) {
        t = t.trimmed();
        if (t.startsWith("W/"))
            t = t.mid(2);
        return t;
    };
    const QByteArray mine = strip(etag);
    for (const QByteArray &part : ifNoneMatch.split(',')) {
        const QByteArray t = strip(part);
        if (t == "*" || t == mine)
            return true;
    }
    return false;
}

bool acceptsGzip(const QHttpServerRequest &request)
{
    const QByteArray value = request.headers().combinedValue(WK::AcceptEncoding);
    for (const QByteArray &part : value.split(',')) {
        const QList<QByteArray> params = part.split(';');
        const QByteArray coding = params.value(0).trimmed().toLower();
        if (coding != "gzip" && coding != "*")
            continue;
        bool refused = false;
        for (int i = 1; i < params.size(); ++i) {
            const QByteArray p = params.at(i).trimmed().toLower();
            if (p.startsWith("q=")) {
                bool ok = false;
                const double q = p.mid(2).toDouble(&ok);
                refused = ok && q <= 0.0;
            }
        }
        if (coding == "gzip")
            return !refused;
        if (!refused)
            return true;                              // "*" without q=0
    }
    return false;
}

QByteArray methodName(const QHttpServerRequest &request)
{
    switch (request.method()) {
    case QHttpServerRequest::Method::Get: return "GET";
    case QHttpServerRequest::Method::Head: return "HEAD";
    case QHttpServerRequest::Method::Post: return "POST";
    case QHttpServerRequest::Method::Put: return "PUT";
    case QHttpServerRequest::Method::Delete: return "DELETE";
    case QHttpServerRequest::Method::Options: return "OPTIONS";
    case QHttpServerRequest::Method::Patch: return "PATCH";
    default: return "OTHER";
    }
}

QString target(const QHttpServerRequest &request)
{
    return request.url().toString(QUrl::RemoveScheme | QUrl::RemoveAuthority | QUrl::FullyEncoded);
}

QString who(const QHttpServerRequest &request)
{
    return request.remoteAddress().toString();
}

QHttpHeaders errorHeaders()
{
    QHttpHeaders h;
    h.append(WK::ContentType, "text/plain; charset=utf-8");
    h.append(WK::AccessControlAllowOrigin, "*");
    h.append(WK::CacheControl, "no-store");
    return h;
}

void reject(const QHttpServerRequest &request, QHttpServerResponder &responder, StatusCode code,
            const QString &why, QHttpHeaders extra = {})
{
    QByteArray body;
    switch (code) {
    case StatusCode::BadRequest: body = "bad request\n"; break;
    case StatusCode::Forbidden: body = "forbidden\n"; break;
    case StatusCode::MethodNotAllowed: body = "method not allowed\n"; break;
    default: body = "not found\n"; break;
    }
    // One multi-argument arg(): a "%NN" in the URL must not be read as a place marker.
    qCInfo(lcAppHttpServer).noquote()
        << QStringLiteral("[AppHttpServer] %1 %2 from %3 -> %4 (%5)")
               .arg(QString::fromLatin1(methodName(request)), target(request), who(request),
                    QString::number(int(code)), why);
    QHttpHeaders headers = errorHeaders();
    for (qsizetype i = 0; i < extra.size(); ++i)
        headers.append(extra.nameAt(i), extra.valueAt(i));
    if (request.method() == QHttpServerRequest::Method::Head) {
        headers.append(WK::ContentLength, QByteArray::number(body.size()));
        responder.write(headers, code);
    } else {
        responder.write(body, headers, code);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Private state (shared by the caller threads and the server thread)
// ---------------------------------------------------------------------------

class AppHttpServerImpl;

struct AppHttpServer::Private
{
    QMutex lifecycle;                          // serializes start()/stop()
    QThread thread;
    AppHttpServerImpl *impl = nullptr;         // lives on thread while it runs

    mutable QMutex stateMutex;                 // guards the four fields below
    bool listening = false;
    quint16 port = 0;
    QHostAddress address;
    QString lastError;

    mutable QReadWriteLock tableLock;          // guards mounts / routes
    QList<MountPtr> mounts;                    // longest prefix first
    QHash<QString, RoutePtr> routes;

    void addMount(MountPtr mount)
    {
        QWriteLocker lock(&tableLock);
        mounts.erase(std::remove_if(mounts.begin(), mounts.end(),
                                    [&](const MountPtr &m) { return m->prefix == mount->prefix; }),
                     mounts.end());
        mounts.append(std::move(mount));
        std::stable_sort(mounts.begin(), mounts.end(), [](const MountPtr &a, const MountPtr &b) {
            return a->prefixSegments.size() > b->prefixSegments.size();
        });
    }

    // Server thread.
    void dispatch(const QHttpServerRequest &request, QHttpServerResponder &responder);
    void serveStatic(const Mount &mount, const ParsedPath &path, const QStringList &rest,
                     const QHttpServerRequest &request, QHttpServerResponder &responder);
    void serveDownload(const Mount &mount, const ParsedPath &path, const QStringList &rest,
                       const QHttpServerRequest &request, QHttpServerResponder &responder);
};

class AppHttpServerImpl : public QObject
{
public:
    explicit AppHttpServerImpl(AppHttpServer::Private *d) : m_d(d) {}

    bool listen(const QHostAddress &address, quint16 port, QString *error, quint16 *bound)
    {
        close();
        auto *tcp = new QTcpServer(this);
        if (!tcp->listen(address, port)) {
            *error = tcp->errorString();
            delete tcp;
            return false;
        }
        auto *http = new QHttpServer(this);
        http->setMissingHandler(this, [this](const QHttpServerRequest &request,
                                             QHttpServerResponder &responder) {
            m_d->dispatch(request, responder);
        });
        if (!http->bind(tcp)) {
            *error = QStringLiteral("QHttpServer::bind failed");
            delete http;
            delete tcp;
            return false;
        }
        *bound = tcp->serverPort();
        m_tcp = tcp;
        m_http = http;
        return true;
    }

    void close()
    {
        if (m_tcp)
            m_tcp->close();
        delete m_http.data();                  // also closes its connections
        delete m_tcp.data();
    }

private:
    AppHttpServer::Private *m_d;
    QPointer<QTcpServer> m_tcp;
    QPointer<QHttpServer> m_http;
};

// ---------------------------------------------------------------------------
// Request handling (server thread)
// ---------------------------------------------------------------------------

void AppHttpServer::Private::dispatch(const QHttpServerRequest &request, QHttpServerResponder &responder)
{
    const auto method = request.method();
    const bool get = method == QHttpServerRequest::Method::Get;
    const bool head = method == QHttpServerRequest::Method::Head;
    if (!get && !head) {
        QHttpHeaders allow;
        allow.append(WK::Allow, "GET, HEAD");
        reject(request, responder, StatusCode::MethodNotAllowed, QStringLiteral("only GET and HEAD"), allow);
        return;
    }
    const ParsedPath path = parsePath(request.url().path(QUrl::FullyEncoded));
    if (!path.ok) {
        reject(request, responder, StatusCode::BadRequest, path.why);
        return;
    }

    RoutePtr route;
    MountPtr mount;
    {
        QReadLocker lock(&tableLock);
        route = routes.value(path.decoded);
        if (!route) {
            for (const MountPtr &m : std::as_const(mounts)) {
                if (m->prefixSegments.size() > path.segments.size())
                    continue;
                if (std::equal(m->prefixSegments.begin(), m->prefixSegments.end(), path.segments.begin())) {
                    mount = m;
                    break;
                }
            }
        }
    }
    if (route) {
        if (!get) {
            QHttpHeaders allow;
            allow.append(WK::Allow, "GET");
            reject(request, responder, StatusCode::MethodNotAllowed, QStringLiteral("route answers GET only"), allow);
            return;
        }
        qCInfo(lcAppHttpServer).noquote() << QStringLiteral("[AppHttpServer] GET %1 from %2 -> route %3")
                                                 .arg(target(request), who(request), path.decoded);
        (*route)(request, responder);
        return;
    }
    if (!mount) {
        reject(request, responder, StatusCode::NotFound, QStringLiteral("no mount or route"));
        return;
    }
    const QStringList rest = path.segments.mid(mount->prefixSegments.size());
    if (mount->kind == Mount::Static)
        serveStatic(*mount, path, rest, request, responder);
    else
        serveDownload(*mount, path, rest, request, responder);
}

void AppHttpServer::Private::serveStatic(const Mount &mount, const ParsedPath &path, const QStringList &rest,
                                         const QHttpServerRequest &request, QHttpServerResponder &responder)
{
    const StaticOptions &o = mount.staticOptions;
    const QString canonicalRoot = QFileInfo(mount.dir).canonicalFilePath();
    if (canonicalRoot.isEmpty() || !QFileInfo(canonicalRoot).isDir()) {
        reject(request, responder, StatusCode::NotFound,
               QStringLiteral("static folder %1 does not exist").arg(QDir::toNativeSeparators(mount.dir)));
        return;
    }
    if (!o.allowHiddenFiles) {
        for (const QString &s : rest) {
            if (s.startsWith(QLatin1Char('.'))) {
                reject(request, responder, StatusCode::NotFound, QStringLiteral("hidden name"));
                return;
            }
        }
    }
    if (!noLinksBelow(mount.dir, rest)) {
        reject(request, responder, StatusCode::Forbidden, QStringLiteral("link below the mount (not followed)"));
        return;
    }
    const QString rel = rest.join(QLatin1Char('/'));
    QFileInfo info(rel.isEmpty() ? mount.dir : mount.dir + QLatin1Char('/') + rel);
    if (!info.exists()) {
        reject(request, responder, StatusCode::NotFound, QStringLiteral("no such file"));
        return;
    }
    if (info.isDir()) {
        if (!insideRoot(info.canonicalFilePath(), canonicalRoot, true)) {
            reject(request, responder, StatusCode::Forbidden, QStringLiteral("folder outside the mount"));
            return;
        }
        const QFileInfo index(info.absoluteFilePath() + QLatin1Char('/') + o.indexFile);
        if (o.indexFile.isEmpty() || !index.isFile() || !noLinksBelow(mount.dir, rest + QStringList{o.indexFile})) {
            reject(request, responder, StatusCode::NotFound, QStringLiteral("directory (no listing)"));
            return;
        }
        if (o.redirectToIndex) {
            QStringList segs = mount.prefixSegments + rest;
            segs << o.indexFile;
            const QByteArray location = encodePath(segs).toLatin1();
            QHttpHeaders h;
            h.append(WK::Location, location);
            h.append(WK::CacheControl, "no-cache");
            h.append(WK::ContentType, "text/plain; charset=utf-8");
            qCInfo(lcAppHttpServer).noquote()
                << QStringLiteral("[AppHttpServer] %1 %2 from %3 -> 302 %4")
                       .arg(QString::fromLatin1(methodName(request)), target(request), who(request),
                            QString::fromLatin1(location));
            if (request.method() == QHttpServerRequest::Method::Head) {
                h.append(WK::ContentLength, "0");
                responder.write(h, StatusCode::Found);
            } else {
                responder.write(QByteArray(), h, StatusCode::Found);
            }
            return;
        }
        info = index;
    } else if (path.trailingSlash) {
        reject(request, responder, StatusCode::NotFound, QStringLiteral("file requested as a folder"));
        return;
    }
    if (!info.isFile()) {
        reject(request, responder, StatusCode::NotFound, QStringLiteral("not a regular file"));
        return;
    }
    // Symbolic links / junctions are resolved by canonicalFilePath(): the target must still
    // be inside the mounted folder.
    if (!insideRoot(info.canonicalFilePath(), canonicalRoot, false)) {
        reject(request, responder, StatusCode::Forbidden, QStringLiteral("file outside the mount"));
        return;
    }

    const QString fileName = info.fileName();
    if (!o.fileSuffixes.isEmpty()
        && (!fileName.contains(QLatin1Char('.'))
            || !o.fileSuffixes.contains(fileName.section(QLatin1Char('.'), -1).toLower()))) {
        reject(request, responder, StatusCode::NotFound, QStringLiteral("file type not served by this mount"));
        return;
    }
    const QByteArray mime = AppHttpServer::mimeTypeForFileName(fileName, o.extraMimeTypes);
    const bool html = fileName.endsWith(QLatin1String(".html"), Qt::CaseInsensitive)
            || fileName.endsWith(QLatin1String(".htm"), Qt::CaseInsensitive);

    QFileInfo served = info;
    bool gzip = false;
    QString gzipNote;
    if (o.gzipVariants && !fileName.endsWith(QLatin1String(".gz"), Qt::CaseInsensitive)
        && acceptsGzip(request)) {
        const QFileInfo gz(info.absoluteFilePath() + QStringLiteral(".gz"));
        if (gz.isFile() && !gz.isSymLink() && !gz.isJunction()
            && insideRoot(gz.canonicalFilePath(), canonicalRoot, false)) {
            if (gz.lastModified() >= info.lastModified()) {
                served = gz;
                gzip = true;
            } else {
                gzipNote = QStringLiteral(", stale .gz ignored");
            }
        }
    }

    const QDateTime mtime = served.lastModified();
    const QByteArray etag = '"' + QByteArray::number(served.size(), 16) + '-'
            + QByteArray::number(mtime.toMSecsSinceEpoch(), 16) + (gzip ? "-gz" : "") + '"';

    QHttpHeaders h;
    h.append(WK::ETag, etag);
    h.append(WK::LastModified, httpDate(mtime));
    h.append(WK::CacheControl, html ? o.htmlCacheControl : o.cacheControl);
    if (o.gzipVariants)
        h.append(WK::Vary, "Accept-Encoding");
    if (o.crossOriginIsolation) {
        h.append(WK::CrossOriginOpenerPolicy, "same-origin");
        h.append(WK::CrossOriginEmbedderPolicy, "require-corp");
    }
    if (o.crossOriginResourcePolicy)
        h.append(WK::CrossOriginResourcePolicy, "same-origin");

    bool notModified = false;
    const QHttpHeaders &rh = request.headers();
    if (rh.contains(WK::IfNoneMatch)) {
        notModified = etagMatches(rh.combinedValue(WK::IfNoneMatch), etag);
    } else if (rh.contains(WK::IfModifiedSince)) {
        const QDateTime since = parseHttpDate(rh.value(WK::IfModifiedSince));
        notModified = since.isValid() && mtime.toSecsSinceEpoch() <= since.toSecsSinceEpoch();
    }
    const QString method = QString::fromLatin1(methodName(request));
    if (notModified) {
        qCInfo(lcAppHttpServer).noquote()
            << QStringLiteral("[AppHttpServer] %1 %2 from %3 -> 304 (%4%5)")
                   .arg(method, target(request), who(request), QString::fromLatin1(etag), gzipNote);
        responder.write(h, StatusCode::NotModified);
        return;
    }
    h.append(WK::ContentType, mime);
    if (gzip)
        h.append(WK::ContentEncoding, "gzip");

    if (request.method() == QHttpServerRequest::Method::Head) {
        h.append(WK::ContentLength, QByteArray::number(served.size()));
        qCInfo(lcAppHttpServer).noquote()
            << QStringLiteral("[AppHttpServer] HEAD %1 from %2 -> 200 (%3 byte(s)%4%5)")
                   .arg(target(request), who(request), QString::number(served.size()),
                        gzip ? QStringLiteral(", gzip") : QString(), gzipNote);
        responder.write(h, StatusCode::Ok);
        return;
    }
    auto *file = new QFile(served.absoluteFilePath());
    if (!file->open(QIODevice::ReadOnly)) {
        const QString error = file->errorString();
        delete file;
        reject(request, responder, StatusCode::NotFound, QStringLiteral("open failed: %1").arg(error));
        return;
    }
    qCInfo(lcAppHttpServer).noquote()
        << QStringLiteral("[AppHttpServer] GET %1 from %2 -> 200 (%3, %4 byte(s) streamed%5%6)")
               .arg(target(request), who(request), QString::fromLatin1(mime), QString::number(file->size()),
                    gzip ? QStringLiteral(", gzip") : QString(), gzipNote);
    // Non-sequential device: the responder sends Content-Length and reads the file in
    // chunks as the socket drains; it takes ownership of the QFile.
    responder.write(file, h, StatusCode::Ok);
}

void AppHttpServer::Private::serveDownload(const Mount &mount, const ParsedPath &path, const QStringList &rest,
                                           const QHttpServerRequest &request, QHttpServerResponder &responder)
{
    Q_UNUSED(path);
    if (rest.isEmpty()) {
        reject(request, responder, StatusCode::NotFound, QStringLiteral("no file name"));
        return;
    }
    if (rest.size() != 1 || path.trailingSlash) {
        reject(request, responder, StatusCode::BadRequest, QStringLiteral("sub folders are not served"));
        return;
    }
    const QString name = rest.first();
    if (!mount.validator || !mount.validator(name)) {
        reject(request, responder, StatusCode::BadRequest, QStringLiteral("rejected name \"%1\"").arg(name));
        return;
    }
    const QFileInfo info(mount.dir + QLatin1Char('/') + name);
    if (info.isSymLink() || info.isJunction()) {
        reject(request, responder, StatusCode::Forbidden, QStringLiteral("link in the download folder (not followed)"));
        return;
    }
    if (!info.exists() || !info.isFile()) {
        reject(request, responder, StatusCode::NotFound, QStringLiteral("no file \"%1\"").arg(name));
        return;
    }
    const QString canonicalDir = QFileInfo(mount.dir).canonicalFilePath();
    if (canonicalDir.isEmpty() || info.canonicalPath().compare(canonicalDir, kPathCase) != 0) {
        reject(request, responder, StatusCode::Forbidden, QStringLiteral("outside the download folder"));
        return;
    }
    const DownloadOptions &o = mount.downloadOptions;
    QHttpHeaders h;
    h.append(WK::ContentType, AppHttpServer::mimeTypeForFileName(name));
    if (o.attachment) {
        QByteArray ascii;
        bool plain = true;
        for (const QChar c : name) {
            const bool ok = c.unicode() >= 0x20 && c.unicode() < 0x7f && c != QLatin1Char('"')
                    && c != QLatin1Char('\\');
            plain = plain && ok;
            ascii += ok ? char(c.unicode()) : '_';
        }
        QByteArray value = "attachment; filename=\"" + ascii + '"';
        if (!plain)
            value += "; filename*=UTF-8''" + QUrl::toPercentEncoding(name);
        h.append(WK::ContentDisposition, value);
    }
    if (!o.accessControlAllowOrigin.isEmpty())
        h.append(WK::AccessControlAllowOrigin, o.accessControlAllowOrigin);
    if (!o.cacheControl.isEmpty())
        h.append(WK::CacheControl, o.cacheControl);

    if (request.method() == QHttpServerRequest::Method::Head) {
        h.append(WK::ContentLength, QByteArray::number(info.size()));
        qCInfo(lcAppHttpServer).noquote() << QStringLiteral("[AppHttpServer] HEAD %1 from %2 -> 200 (%3 byte(s))")
                                                 .arg(target(request), who(request), QString::number(info.size()));
        responder.write(h, StatusCode::Ok);
        return;
    }
    auto *file = new QFile(info.absoluteFilePath());
    if (!file->open(QIODevice::ReadOnly)) {
        const QString error = file->errorString();
        delete file;
        reject(request, responder, StatusCode::NotFound, QStringLiteral("open failed: %1").arg(error));
        return;
    }
    qCInfo(lcAppHttpServer).noquote()
        << QStringLiteral("[AppHttpServer] GET %1 from %2 -> 200, %3 byte(s) streamed from disk")
               .arg(target(request), who(request), QString::number(file->size()));
    responder.write(file, h, StatusCode::Ok);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AppHttpServer &AppHttpServer::instance()
{
    static AppHttpServer server;
    return server;
}

AppHttpServer::AppHttpServer() : d(std::make_unique<Private>())
{
    d->thread.setObjectName(QStringLiteral("AppHttpServerThread"));
}

AppHttpServer::~AppHttpServer()
{
    stop();
}

namespace {
template <typename F>
void runOn(QThread *thread, QObject *context, F &&f)
{
    if (QThread::currentThread() == thread)
        f();
    else
        QMetaObject::invokeMethod(context, std::forward<F>(f), Qt::BlockingQueuedConnection);
}
} // namespace

bool AppHttpServer::start(quint16 port, const QHostAddress &address)
{
    QMutexLocker lifecycle(&d->lifecycle);
    {
        QMutexLocker state(&d->stateMutex);
        if (d->listening && d->address == address && (port == 0 || port == d->port))
            return true;
    }
    if (d->impl && QThread::currentThread() == &d->thread) {
        qCWarning(lcAppHttpServer).noquote() << "[AppHttpServer] start() from a route handler is not supported";
        return false;
    }
    if (d->impl) {
        lifecycle.unlock();
        stop();
        lifecycle.relock();
    }
    auto *impl = new AppHttpServerImpl(d.get());
    impl->moveToThread(&d->thread);
    d->impl = impl;
    d->thread.start();

    QString error;
    quint16 bound = 0;
    bool ok = false;
    runOn(&d->thread, impl, [&]() { ok = impl->listen(address, port, &error, &bound); });
    const QString where = QStringLiteral("%1:%2").arg(address.toString()).arg(port);
    {
        QMutexLocker state(&d->stateMutex);
        d->listening = ok;
        d->port = ok ? bound : 0;
        d->address = ok ? address : QHostAddress();
        d->lastError = ok ? QString() : error;
    }
    if (ok) {
        qCInfo(lcAppHttpServer).noquote()
            << QStringLiteral("[AppHttpServer] listening on %1:%2 (thread %3); mounts: %4; routes: %5")
                   .arg(address.toString()).arg(bound).arg(d->thread.objectName(),
                        mountedPrefixes().join(QStringLiteral(", ")), routePaths().join(QStringLiteral(", ")));
    } else {
        qCWarning(lcAppHttpServer).noquote()
            << QStringLiteral("[AppHttpServer] NOT listening on %1: %2 (the application keeps running)")
                   .arg(where, error);
        runOn(&d->thread, impl, [impl]() { impl->close(); });
        d->thread.quit();
        d->thread.wait();
        delete impl;
        d->impl = nullptr;
    }
    return ok;
}

void AppHttpServer::stop()
{
    QMutexLocker lifecycle(&d->lifecycle);
    AppHttpServerImpl *impl = d->impl;
    if (!impl)
        return;
    quint16 port = 0;
    {
        QMutexLocker state(&d->stateMutex);
        port = d->port;
        d->listening = false;
        d->port = 0;
        d->address = QHostAddress();
    }
    if (QThread::currentThread() == &d->thread) {
        // Called from a route handler: close now, the thread ends when the handler returns.
        impl->close();
        d->thread.quit();
        qCWarning(lcAppHttpServer).noquote() << "[AppHttpServer] stop() called on the server thread";
        return;
    }
    runOn(&d->thread, impl, [impl]() { impl->close(); });
    d->thread.quit();
    d->thread.wait();                     // pending deleteLater()s run before the thread ends
    delete impl;
    d->impl = nullptr;
    qCInfo(lcAppHttpServer).noquote() << QStringLiteral("[AppHttpServer] stopped (was port %1)").arg(port);
}

bool AppHttpServer::isListening() const
{
    QMutexLocker state(&d->stateMutex);
    return d->listening;
}

quint16 AppHttpServer::port() const
{
    QMutexLocker state(&d->stateMutex);
    return d->port;
}

QHostAddress AppHttpServer::address() const
{
    QMutexLocker state(&d->stateMutex);
    return d->address;
}

QString AppHttpServer::lastError() const
{
    QMutexLocker state(&d->stateMutex);
    return d->lastError;
}

bool AppHttpServer::mountStatic(const QString &urlPrefix, const QString &dir, const StaticOptions &options)
{
    auto m = std::make_shared<Mount>();
    if (!normalizePrefix(urlPrefix, &m->prefix, &m->prefixSegments) || dir.isEmpty()) {
        qCWarning(lcAppHttpServer).noquote()
            << QStringLiteral("[AppHttpServer] mountStatic refused: prefix \"%1\", folder \"%2\"").arg(urlPrefix, dir);
        return false;
    }
    if (!options.indexFile.isEmpty() && !isValidSegment(options.indexFile)) {
        qCWarning(lcAppHttpServer).noquote()
            << QStringLiteral("[AppHttpServer] mountStatic refused: index file \"%1\"").arg(options.indexFile);
        return false;
    }
    m->kind = Mount::Static;
    m->dir = QDir::cleanPath(QDir(dir).absolutePath());
    m->staticOptions = options;
    const bool exists = QFileInfo(m->dir).isDir();
    qCInfo(lcAppHttpServer).noquote()
        << QStringLiteral("[AppHttpServer] static %1 -> %2%3 (index \"%4\", COOP/COEP %5, CORP %6, gzip variants %7)")
               .arg(m->prefix, QDir::toNativeSeparators(m->dir),
                    exists ? QString() : QStringLiteral(" [folder does not exist yet]"), options.indexFile,
                    options.crossOriginIsolation ? QStringLiteral("on") : QStringLiteral("off"),
                    options.crossOriginResourcePolicy ? QStringLiteral("on") : QStringLiteral("off"),
                    options.gzipVariants ? QStringLiteral("on") : QStringLiteral("off"));
    d->addMount(std::move(m));
    return true;
}

bool AppHttpServer::mountDownloads(const QString &urlPrefix, const QString &dir, FileNameValidator validator,
                                   const DownloadOptions &options)
{
    auto m = std::make_shared<Mount>();
    if (!normalizePrefix(urlPrefix, &m->prefix, &m->prefixSegments) || dir.isEmpty() || !validator) {
        qCWarning(lcAppHttpServer).noquote()
            << QStringLiteral("[AppHttpServer] mountDownloads refused: prefix \"%1\", folder \"%2\", validator %3")
                   .arg(urlPrefix, dir, validator ? QStringLiteral("set") : QStringLiteral("missing"));
        return false;
    }
    m->kind = Mount::Downloads;
    m->dir = QDir::cleanPath(QDir(dir).absolutePath());
    m->downloadOptions = options;
    m->validator = std::move(validator);
    qCInfo(lcAppHttpServer).noquote() << QStringLiteral("[AppHttpServer] downloads %1/<file> -> %2%3")
                                             .arg(m->prefix == QLatin1String("/") ? QString() : m->prefix,
                                                  QDir::toNativeSeparators(m->dir),
                                                  QFileInfo(m->dir).isDir() ? QString()
                                                                            : QStringLiteral(" [folder does not exist yet]"));
    d->addMount(std::move(m));
    return true;
}

bool AppHttpServer::addGetRoute(const QString &path, RouteHandler handler)
{
    QString normalized;
    QStringList segments;
    if (!handler || !normalizePrefix(path, &normalized, &segments)) {
        qCWarning(lcAppHttpServer).noquote() << QStringLiteral("[AppHttpServer] addGetRoute refused: \"%1\"").arg(path);
        return false;
    }
    {
        QWriteLocker lock(&d->tableLock);
        d->routes.insert(normalized, std::make_shared<const RouteHandler>(std::move(handler)));
    }
    qCInfo(lcAppHttpServer).noquote() << QStringLiteral("[AppHttpServer] route GET %1").arg(normalized);
    return true;
}

bool AppHttpServer::unmount(const QString &urlPrefix)
{
    QString normalized;
    QStringList segments;
    if (!normalizePrefix(urlPrefix, &normalized, &segments))
        return false;
    qsizetype removed = 0;
    {
        QWriteLocker lock(&d->tableLock);
        removed = d->mounts.removeIf([&](const MountPtr &m) { return m->prefix == normalized; });
    }
    if (removed)
        qCInfo(lcAppHttpServer).noquote() << QStringLiteral("[AppHttpServer] unmounted %1").arg(normalized);
    return removed > 0;
}

bool AppHttpServer::removeGetRoute(const QString &path)
{
    QString normalized;
    QStringList segments;
    if (!normalizePrefix(path, &normalized, &segments))
        return false;
    QWriteLocker lock(&d->tableLock);
    return d->routes.remove(normalized);
}

QStringList AppHttpServer::mountedPrefixes() const
{
    QReadLocker lock(&d->tableLock);
    QStringList out;
    for (const MountPtr &m : d->mounts)
        out << m->prefix;
    return out;
}

QStringList AppHttpServer::routePaths() const
{
    QReadLocker lock(&d->tableLock);
    QStringList out = d->routes.keys();
    out.sort();
    return out;
}

QByteArray AppHttpServer::mimeTypeForFileName(const QString &fileName, const QHash<QString, QByteArray> &extra)
{
    const QString suffix = fileName.section(QLatin1Char('.'), -1).toLower();
    if (!fileName.contains(QLatin1Char('.')))
        return QByteArrayLiteral("application/octet-stream");
    const auto it = extra.constFind(suffix);
    if (it != extra.constEnd())
        return it.value();
    static const QHash<QString, QByteArray> types{
        {QStringLiteral("html"), "text/html; charset=utf-8"},
        {QStringLiteral("htm"), "text/html; charset=utf-8"},
        {QStringLiteral("js"), "text/javascript; charset=utf-8"},
        {QStringLiteral("mjs"), "text/javascript; charset=utf-8"},
        {QStringLiteral("wasm"), "application/wasm"},
        {QStringLiteral("css"), "text/css; charset=utf-8"},
        {QStringLiteral("json"), "application/json"},
        {QStringLiteral("map"), "application/json"},
        {QStringLiteral("txt"), "text/plain; charset=utf-8"},
        {QStringLiteral("csv"), "text/csv; charset=utf-8"},
        {QStringLiteral("xml"), "application/xml"},
        {QStringLiteral("png"), "image/png"},
        {QStringLiteral("jpg"), "image/jpeg"},
        {QStringLiteral("jpeg"), "image/jpeg"},
        {QStringLiteral("gif"), "image/gif"},
        {QStringLiteral("webp"), "image/webp"},
        {QStringLiteral("svg"), "image/svg+xml"},
        {QStringLiteral("ico"), "image/x-icon"},
        {QStringLiteral("ttf"), "font/ttf"},
        {QStringLiteral("otf"), "font/otf"},
        {QStringLiteral("woff"), "font/woff"},
        {QStringLiteral("woff2"), "font/woff2"},
        {QStringLiteral("pdf"), "application/pdf"},
        {QStringLiteral("gz"), "application/gzip"},
        {QStringLiteral("zip"), "application/zip"},
    };
    return types.value(suffix, QByteArrayLiteral("application/octet-stream"));
}
