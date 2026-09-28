#include "runtimeinfo.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <cmath>
#include <memory>
#include <utility>

namespace TaidaFlowRuntime {

namespace {

// JSON numbers are doubles: accept only whole numbers inside [min, max].
bool wholeNumberInRange(const QJsonValue &value, double min, double max, int *out)
{
    if (!value.isDouble())
        return false;
    const double d = value.toDouble();
    if (!std::isfinite(d) || std::floor(d) != d || d < min || d > max)
        return false;
    *out = int(d);
    return true;
}

} // namespace

std::optional<RuntimeInfo> parseRuntimeJson(const QByteArray &body, QString *error)
{
    const auto fail = [error](const QString &message) -> std::optional<RuntimeInfo> {
        if (error)
            *error = message;
        return std::nullopt;
    };

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail(QStringLiteral("not JSON (%1 at offset %2)")
                        .arg(parseError.errorString())
                        .arg(parseError.offset));
    if (!doc.isObject())
        return fail(QStringLiteral("not a JSON object"));

    const QJsonObject object = doc.object();
    RuntimeInfo info;

    const QJsonValue port = object.value(QLatin1String("mirrorPublicPort"));
    if (port.isUndefined())
        return fail(QStringLiteral("\"mirrorPublicPort\" is missing"));
    int portValue = 0;
    if (!wholeNumberInRange(port, 1, 65535, &portValue))
        return fail(QStringLiteral("\"mirrorPublicPort\" is not an integer 1..65535"));
    info.mirrorPublicPort = quint16(portValue);

    const QJsonValue version = object.value(QLatin1String("version"));
    if (!version.isUndefined()) {
        int versionValue = 0;
        if (!wholeNumberInRange(version, 1, 1000000, &versionValue))
            return fail(QStringLiteral("\"version\" is not an integer >= 1"));
        info.version = versionValue;
    }

    if (error)
        error->clear();
    return info;
}

QByteArray buildRuntimeJson(quint16 mirrorPublicPort)
{
    QJsonObject object;
    object.insert(QLatin1String("mirrorPublicPort"), int(mirrorPublicPort));
    object.insert(QLatin1String("version"), kRuntimeJsonVersion);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

void requestRuntimeInfo(const QUrl &url, int timeoutMs, QObject *context,
                        std::function<void(const RuntimeLookup &)> done)
{
    // Shared by the reply and the timeout handlers: whichever comes first reports, the other
    // one is ignored. `context` guards against reporting while it is being destroyed.
    struct State
    {
        bool reported = false;
        QPointer<QObject> context;
        std::function<void(const RuntimeLookup &)> done;
    };
    auto state = std::make_shared<State>();
    state->context = context;
    state->done = std::move(done);

    const auto report = [state](const RuntimeLookup &lookup) {
        if (state->reported || !state->context)
            return;
        state->reported = true;
        auto callback = std::move(state->done);
        state->done = nullptr;
        if (callback)
            callback(lookup);
    };

    if (!url.isValid() || url.isRelative()) {
        RuntimeLookup lookup;
        lookup.detail = QStringLiteral("no usable URL for /runtime.json (\"%1\")").arg(url.toString());
        QMetaObject::invokeMethod(context, [report, lookup] { report(lookup); }, Qt::QueuedConnection);
        return;
    }

    // Owned by `context`; removed once the request is over.
    auto *network = new QNetworkAccessManager(context);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("Cache-Control", "no-cache");
    QNetworkReply *reply = network->get(request);
    QPointer<QNetworkReply> guardedReply(reply);

    auto *timer = new QTimer(network);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, network, [report, guardedReply, network, timeoutMs] {
        RuntimeLookup lookup;
        lookup.detail = QStringLiteral("no answer within %1 ms").arg(timeoutMs);
        report(lookup);
        if (guardedReply)
            guardedReply->abort();   // its finished() is ignored (already reported)
        network->deleteLater();
    });

    QObject::connect(reply, &QNetworkReply::finished, network, [report, reply, network, timer] {
        timer->stop();
        RuntimeLookup lookup;
        const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (reply->error() != QNetworkReply::NoError) {
            lookup.detail = QStringLiteral("request failed: %1").arg(reply->errorString());
        } else if (status.toInt() != 200) {
            lookup.detail = QStringLiteral("HTTP status %1 (expected 200)")
                                .arg(status.isValid() ? status.toString() : QStringLiteral("unknown"));
        } else {
            QString error;
            const std::optional<RuntimeInfo> info = parseRuntimeJson(reply->readAll(), &error);
            if (info) {
                lookup.info = *info;
                lookup.fromServer = true;
            } else {
                lookup.detail = QStringLiteral("invalid content: %1").arg(error);
            }
        }
        reply->deleteLater();
        network->deleteLater();
        report(lookup);
    });

    timer->start(timeoutMs);
}

} // namespace TaidaFlowRuntime
