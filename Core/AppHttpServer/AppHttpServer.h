#pragma once
// AppHttpServer - a reusable, process-wide HTTP server for Qt 6.8 applications.
//
// Drop-in class (AppHttpServer.h + AppHttpServer.cpp, see README.md in this folder): it
// depends on Qt Core, Network and HttpServer only and knows nothing about the application
// that uses it.  One instance per process (instance()); the QHttpServer runs on its own
// thread, so serving large files never blocks the caller's (GUI) thread.
//
//   * mountStatic(prefix, dir, options)      static files (web page / WebAssembly build):
//                                            MIME types, ETag / Last-Modified / 304,
//                                            pre-compressed <file>.gz, COOP/COEP/CORP,
//                                            HEAD, default page, no directory listing,
//                                            path traversal rejected.
//   * mountDownloads(prefix, dir, validator) one-level download folder: file names checked
//                                            by the caller's rule, Content-Disposition
//                                            attachment, Access-Control-Allow-Origin *,
//                                            Cache-Control no-store.
//   * addGetRoute(path, handler)             custom GET handler for one exact path.
//   * unmount(prefix) / removeGetRoute(path)
//
// All registration functions and start()/stop() may be called from any thread, before or
// after start(); mounts and routes survive stop()/start().  Files are always streamed from
// disk (QHttpServerResponder::write(QIODevice *)), never read into memory as a whole.
// Route handlers are called on the server thread.
#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QHttpServerRequest;
class QHttpServerResponder;

class AppHttpServer
{
public:
    struct StaticOptions
    {
        // Default page for a directory request ("/" or "<dir>/"), e.g. "index.html".
        // Empty: directory requests are answered 404 (directories are never listed).
        QString indexFile;
        // true: 302 redirect to <dir>/<indexFile> (relative URLs of the page then resolve
        // as usual); false: the default page is served under the directory URL.
        bool redirectToIndex = true;
        // Cross-Origin-Opener-Policy: same-origin + Cross-Origin-Embedder-Policy: require-corp
        // (needed by multi-threaded WebAssembly builds, harmless otherwise).
        bool crossOriginIsolation = true;
        // Cross-Origin-Resource-Policy: same-origin.
        bool crossOriginResourcePolicy = true;
        // Serve <file>.gz (Content-Encoding: gzip) when the client accepts gzip and the .gz
        // file is not older than <file>.  A stale .gz is ignored (and logged).
        bool gzipVariants = true;
        // Cache-Control of .html/.htm files and of every other file.  "no-cache" = the
        // browser keeps the file but revalidates it every time (ETag / Last-Modified -> 304),
        // so a rebuilt page never mixes old and new HTML/JS/WASM.
        QByteArray htmlCacheControl = QByteArrayLiteral("no-cache");
        QByteArray cacheControl = QByteArrayLiteral("no-cache");
        // Path segments starting with '.' (".git", ".env") are answered 404 unless true.
        bool allowHiddenFiles = false;
        // Only files with one of these suffixes (lower-case, without the dot, e.g. "html",
        // "wasm") are served; anything else is answered 404.  Empty: every suffix.  Useful
        // when the mounted folder is a build folder that also holds build files.
        QStringList fileSuffixes;
        // Additional / overriding MIME types: lower-case suffix without the dot -> type.
        QHash<QString, QByteArray> extraMimeTypes;
    };

    struct DownloadOptions
    {
        QByteArray accessControlAllowOrigin = QByteArrayLiteral("*");   // empty: header not sent
        QByteArray cacheControl = QByteArrayLiteral("no-store");
        bool attachment = true;                                          // Content-Disposition
    };

    // Returns true when fileName (one path segment, already percent-decoded) may be served.
    // Called on the server thread; must be thread-safe (pure functions are).
    using FileNameValidator = std::function<bool(const QString &fileName)>;
    // Answers one GET request.  Called on the server thread.
    using RouteHandler = std::function<void(const QHttpServerRequest &request,
                                            QHttpServerResponder &responder)>;

    static AppHttpServer &instance();

    // Binds address:port on the server thread.  Already listening on the same address and
    // port: returns true.  Listening elsewhere: stops first.  port 0 = any free port (see
    // port()).  On failure it logs a warning, keeps lastError() and returns false; the
    // application keeps running and start() may be called again later.
    bool start(quint16 port, const QHostAddress &address = QHostAddress(QHostAddress::AnyIPv4));
    // Closes the listener and every open connection and stops the server thread.  Do not
    // call it from a route handler (the server thread cannot wait for itself).
    void stop();
    bool isListening() const;
    quint16 port() const;              // bound port while listening, else 0
    QHostAddress address() const;      // bound address while listening
    QString lastError() const;         // error of the last failed start(), else empty

    // urlPrefix: "/" or "/name[/name...]" (no trailing slash, no "." / ".." segments).
    // A prefix that is already mounted is replaced.  The longest matching prefix wins; exact
    // GET routes win over mounts.  Returns false (and logs) for an invalid prefix, an empty
    // dir or an empty validator.  The folder does not have to exist yet (logged).
    bool mountStatic(const QString &urlPrefix, const QString &dir,
                     const StaticOptions &options = StaticOptions());
    bool mountDownloads(const QString &urlPrefix, const QString &dir, FileNameValidator validator,
                        const DownloadOptions &options = DownloadOptions());
    // path: exact decoded path, e.g. "/api/status".  Replaces an existing route.
    bool addGetRoute(const QString &path, RouteHandler handler);
    bool unmount(const QString &urlPrefix);           // false: nothing was mounted there
    bool removeGetRoute(const QString &path);         // false: no such route
    QStringList mountedPrefixes() const;              // longest first
    QStringList routePaths() const;

    // MIME type used for a file name (by suffix, case-insensitive; unknown ->
    // application/octet-stream).
    static QByteArray mimeTypeForFileName(const QString &fileName,
                                          const QHash<QString, QByteArray> &extra = {});

    AppHttpServer(const AppHttpServer &) = delete;
    AppHttpServer &operator=(const AppHttpServer &) = delete;

    struct Private;

private:
    AppHttpServer();
    ~AppHttpServer();

    std::unique_ptr<Private> d;
};
