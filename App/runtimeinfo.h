// runtimeinfo: the /runtime.json document that tells the web page (WebAssembly) which port
// the Proxy Mirror listens on (docs/taidaflow_config_spec.md §3).
//
//   GET /runtime.json  ->  { "mirrorPublicPort": 8125, "version": 1 }   (Cache-Control: no-store)
//
// - The desktop (core branch, AppHttpServer route) builds the body with buildRuntimeJson()
//   from AppConfig::mirror().publicPort.
// - The page reads it with requestRuntimeInfo() before it creates the Mirror client. Any
//   failure (no route, HTTP error, timeout, invalid content) falls back to
//   kDefaultMirrorPublicPort (8125); the caller logs a console warning.
//
// Compiled on every platform (Qt Core + Qt Network only); the parser/builder are pure
// functions covered by App/tests.
#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QtGlobal>

#include <functional>
#include <optional>

class QObject;

namespace TaidaFlowRuntime {

// Port used when /runtime.json cannot be read (the port before config.json existed).
constexpr quint16 kDefaultMirrorPublicPort = 8125;
// Format version written by buildRuntimeJson().
constexpr int kRuntimeJsonVersion = 1;
// Timeout of the page's /runtime.json request (spec §3: "e.g. 3 seconds").
constexpr int kRuntimeRequestTimeoutMs = 3000;

struct RuntimeInfo
{
    quint16 mirrorPublicPort = kDefaultMirrorPublicPort;
    int version = kRuntimeJsonVersion;
};

// Parses a /runtime.json body. Valid = a JSON object whose "mirrorPublicPort" is an
// integer 1..65535. "version" is optional (missing = 1); when present it must be an
// integer >= 1 (a newer server version is accepted as long as mirrorPublicPort is valid).
// Unknown keys are ignored. Returns std::nullopt and fills *error when invalid.
std::optional<RuntimeInfo> parseRuntimeJson(const QByteArray &body, QString *error = nullptr);

// Body of GET /runtime.json: {"mirrorPublicPort":<port>,"version":1} (compact UTF-8).
QByteArray buildRuntimeJson(quint16 mirrorPublicPort);

// Result of requestRuntimeInfo().
struct RuntimeLookup
{
    RuntimeInfo info;          // parsed values, or the defaults (port 8125) on failure
    bool fromServer = false;   // true only when a valid /runtime.json was received
    QString detail;            // why it fell back (empty when fromServer)
};

// Asynchronous GET of `url` (normally <page origin>/runtime.json). `done` is called exactly
// once, on `context`'s thread, with either the parsed document or the fallback (HTTP error,
// non-200 status, invalid content, or no answer within timeoutMs). Does not block. If
// `context` is destroyed first, `done` is not called. An invalid url falls back on the next
// event loop turn.
void requestRuntimeInfo(const QUrl &url, int timeoutMs, QObject *context,
                        std::function<void(const RuntimeLookup &)> done);

} // namespace TaidaFlowRuntime
