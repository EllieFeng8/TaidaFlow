#include "infrastructure/proxy_mirror/proxymirror.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QPointer>
#include <QSet>
#include <QSignalBlocker>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

struct PropertyContract
{
    QString name;
    QMetaProperty property;
};

struct SignalContract
{
    QString signature;
    QMetaMethod method;
    QList<QMetaType> parameterTypes;
};

struct MirrorContract
{
    QList<PropertyContract> properties;
    QList<SignalContract> requestSignals;
    QJsonObject descriptor;
    QString hash;
    QString error;

    bool isValid() const { return error.isEmpty(); }
};

bool isSupportedType(const QMetaType &type)
{
    switch (type.id()) {
    case QMetaType::Bool:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Double:
    case QMetaType::QString:
    case QMetaType::QByteArray:
    case QMetaType::QStringList:
    case QMetaType::QVariantList:
    case QMetaType::QVariantMap:
        return true;
    default:
        return false;
    }
}

QJsonValue variantToJson(const QVariant &value, const QMetaType &type)
{
    switch (type.id()) {
    case QMetaType::LongLong:
        return QString::number(value.toLongLong());
    case QMetaType::ULongLong:
        return QString::number(value.toULongLong());
    case QMetaType::QByteArray:
        return QString::fromLatin1(value.toByteArray().toBase64());
    default:
        return QJsonValue::fromVariant(value);
    }
}

bool integerFromJson(const QJsonValue &json,
                     qint64 minimum,
                     quint64 maximum,
                     qint64 *signedValue,
                     quint64 *unsignedValue)
{
    if (!json.isDouble() || !std::isfinite(json.toDouble())
        || std::floor(json.toDouble()) != json.toDouble()) {
        return false;
    }
    const double number = json.toDouble();
    if (number < static_cast<double>(minimum)
        || number > static_cast<double>(maximum)) {
        return false;
    }
    if (signedValue) {
        *signedValue = static_cast<qint64>(number);
    }
    if (unsignedValue) {
        *unsignedValue = static_cast<quint64>(number);
    }
    return true;
}

bool jsonToVariant(const QJsonValue &json,
                   const QMetaType &type,
                   QVariant *result)
{
    if (!result) {
        return false;
    }
    switch (type.id()) {
    case QMetaType::Bool:
        if (!json.isBool()) return false;
        *result = json.toBool();
        return true;
    case QMetaType::Int: {
        qint64 number = 0;
        if (!integerFromJson(json, std::numeric_limits<int>::min(),
                             std::numeric_limits<int>::max(), &number, nullptr)) {
            return false;
        }
        *result = static_cast<int>(number);
        return true;
    }
    case QMetaType::UInt: {
        quint64 number = 0;
        if (!integerFromJson(json, 0, std::numeric_limits<unsigned int>::max(),
                             nullptr, &number)) {
            return false;
        }
        *result = static_cast<unsigned int>(number);
        return true;
    }
    case QMetaType::LongLong: {
        if (!json.isString()) return false;
        bool ok = false;
        const qlonglong number = json.toString().toLongLong(&ok);
        if (!ok) return false;
        *result = number;
        return true;
    }
    case QMetaType::ULongLong: {
        if (!json.isString()) return false;
        bool ok = false;
        const qulonglong number = json.toString().toULongLong(&ok);
        if (!ok) return false;
        *result = number;
        return true;
    }
    case QMetaType::Double:
        if (!json.isDouble() || !std::isfinite(json.toDouble())) return false;
        *result = json.toDouble();
        return true;
    case QMetaType::QString:
        if (!json.isString()) return false;
        *result = json.toString();
        return true;
    case QMetaType::QByteArray:
        if (!json.isString()) return false;
        *result = QByteArray::fromBase64(json.toString().toLatin1());
        return true;
    case QMetaType::QStringList: {
        if (!json.isArray()) return false;
        QStringList strings;
        for (const QJsonValue &entry : json.toArray()) {
            if (!entry.isString()) return false;
            strings.append(entry.toString());
        }
        *result = strings;
        return true;
    }
    case QMetaType::QVariantList:
        if (!json.isArray()) return false;
        *result = json.toArray().toVariantList();
        return true;
    case QMetaType::QVariantMap:
        if (!json.isObject()) return false;
        *result = json.toObject().toVariantMap();
        return true;
    default:
        return false;
    }
}

MirrorContract inspectContract(QObject &proxy)
{
    MirrorContract contract;
    const QMetaObject *metaObject = proxy.metaObject();
    QSet<int> allPropertyNotifySignalIndexes;
    QHash<int, QString> valuedNotifyOwners;

    // Q_PROPERTY NOTIFY signals are state notifications even when the
    // property is CONSTANT or STORED false. They must never be classified as
    // remotely callable UI events merely because of their name.
    for (int index = metaObject->propertyOffset();
         index < metaObject->propertyCount(); ++index) {
        const QMetaProperty property = metaObject->property(index);
        if (property.hasNotifySignal()) {
            allPropertyNotifySignalIndexes.insert(
                property.notifySignalIndex());
        }
    }

    for (int index = metaObject->propertyOffset();
         index < metaObject->propertyCount(); ++index) {
        const QMetaProperty property = metaObject->property(index);
        if (property.isConstant() || !property.isStored()) {
            continue;
        }
        if (!property.isReadable() || !property.isWritable()
            || !property.hasNotifySignal()) {
            contract.error = QObject::tr(
                "Mirrored property %1 must be readable, writable and have a NOTIFY signal.")
                                 .arg(QString::fromLatin1(property.name()));
            return contract;
        }
        const QMetaMethod notifySignal = property.notifySignal();
        if (notifySignal.parameterCount() > 1) {
            contract.error = QObject::tr(
                "Mirrored property %1 NOTIFY signal must have zero arguments or one argument matching the property type.")
                                 .arg(QString::fromLatin1(property.name()));
            return contract;
        }
        if (notifySignal.parameterCount() == 1) {
            const QMetaType notifyType = notifySignal.parameterMetaType(0);
            if (notifyType != property.metaType()) {
                contract.error = QObject::tr(
                    "Mirrored property %1 NOTIFY argument type %2 does not match property type %3.")
                                     .arg(QString::fromLatin1(property.name()),
                                          QString::fromLatin1(notifyType.name()),
                                          QString::fromLatin1(property.typeName()));
                return contract;
            }
            const int signalIndex = property.notifySignalIndex();
            const auto owner = valuedNotifyOwners.constFind(signalIndex);
            if (owner != valuedNotifyOwners.constEnd()
                && owner.value() != QString::fromLatin1(property.name())) {
                contract.error = QObject::tr(
                    "Mirrored properties %1 and %2 cannot share a value-carrying NOTIFY signal.")
                                     .arg(owner.value(),
                                          QString::fromLatin1(property.name()));
                return contract;
            }
            valuedNotifyOwners.insert(signalIndex,
                                      QString::fromLatin1(property.name()));
        }
        if (!isSupportedType(property.metaType())) {
            contract.error = QObject::tr("Mirrored property %1 uses unsupported type %2.")
                                 .arg(QString::fromLatin1(property.name()),
                                      QString::fromLatin1(property.typeName()));
            return contract;
        }
        contract.properties.append(
            {QString::fromLatin1(property.name()), property});
    }

    for (int index = metaObject->methodOffset();
         index < metaObject->methodCount(); ++index) {
        const QMetaMethod method = metaObject->method(index);
        if (method.methodType() != QMetaMethod::Signal
            || allPropertyNotifySignalIndexes.contains(index)) {
            continue;
        }
        SignalContract signal;
        signal.signature = QString::fromLatin1(method.methodSignature());
        signal.method = method;
        for (int parameter = 0; parameter < method.parameterCount(); ++parameter) {
            const QMetaType parameterType = method.parameterMetaType(parameter);
            if (!isSupportedType(parameterType)) {
                contract.error = QObject::tr("Request signal %1 uses unsupported type %2.")
                                     .arg(signal.signature,
                                          QString::fromLatin1(parameterType.name()));
                return contract;
            }
            signal.parameterTypes.append(parameterType);
        }
        contract.requestSignals.append(signal);
    }

    std::sort(contract.properties.begin(), contract.properties.end(),
              [](const PropertyContract &left, const PropertyContract &right) {
        return left.name < right.name;
    });
    std::sort(contract.requestSignals.begin(), contract.requestSignals.end(),
              [](const SignalContract &left, const SignalContract &right) {
        return left.signature < right.signature;
    });

    QJsonArray properties;
    for (const PropertyContract &property : std::as_const(contract.properties)) {
        QJsonArray notifyParameterTypes;
        const QMetaMethod notifySignal = property.property.notifySignal();
        for (int index = 0; index < notifySignal.parameterCount(); ++index) {
            notifyParameterTypes.append(
                QString::fromLatin1(notifySignal.parameterMetaType(index).name()));
        }
        properties.append(QJsonObject{
            {QStringLiteral("name"), property.name},
            {QStringLiteral("type"), QString::fromLatin1(property.property.typeName())},
            {QStringLiteral("notifyParameterTypes"), notifyParameterTypes}
        });
    }
    QJsonArray requestSignals;
    for (const SignalContract &signal : std::as_const(contract.requestSignals)) {
        QJsonArray parameterTypes;
        for (const QMetaType &type : signal.parameterTypes) {
            parameterTypes.append(QString::fromLatin1(type.name()));
        }
        requestSignals.append(QJsonObject{
            {QStringLiteral("signature"), signal.signature},
            {QStringLiteral("parameterTypes"), parameterTypes}
        });
    }
    contract.descriptor = QJsonObject{
        {QStringLiteral("protocol"), ProxyMirrorHost::ProtocolVersion},
        {QStringLiteral("properties"), properties},
        {QStringLiteral("requestSignals"), requestSignals}
    };
    contract.hash = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(contract.descriptor).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
    return contract;
}

QVariantMap readState(QObject &proxy, const MirrorContract &contract)
{
    QVariantMap state;
    for (const PropertyContract &property : contract.properties) {
        state.insert(property.name, property.property.read(&proxy));
    }
    return state;
}

QJsonObject stateToJson(const QVariantMap &state,
                        const MirrorContract &contract)
{
    QJsonObject object;
    for (const PropertyContract &property : contract.properties) {
        if (state.contains(property.name)) {
            object.insert(property.name,
                          variantToJson(state.value(property.name),
                                        property.property.metaType()));
        }
    }
    return object;
}

bool jsonToState(const QJsonObject &object,
                 const MirrorContract &contract,
                 bool requireComplete,
                 QVariantMap *state,
                 QString *error)
{
    QVariantMap parsed;
    QSet<QString> known;
    for (const PropertyContract &property : contract.properties) {
        known.insert(property.name);
        if (!object.contains(property.name)) {
            if (requireComplete) {
                if (error) {
                    *error = QObject::tr("Proxy state is missing property %1.")
                                 .arg(property.name);
                }
                return false;
            }
            continue;
        }
        QVariant converted;
        if (!jsonToVariant(object.value(property.name),
                           property.property.metaType(), &converted)) {
            if (error) {
                *error = QObject::tr("Proxy property %1 has an invalid JSON value.")
                             .arg(property.name);
            }
            return false;
        }
        parsed.insert(property.name, converted);
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!known.contains(it.key())) {
            if (error) {
                *error = QObject::tr("Proxy state contains unknown property %1.")
                             .arg(it.key());
            }
            return false;
        }
    }
    if (state) {
        *state = parsed;
    }
    return true;
}

bool writeState(QObject &proxy,
                const MirrorContract &contract,
                const QVariantMap &values,
                QString *error)
{
    struct PreviousValue {
        QMetaProperty property;
        QVariant value;
    };
    struct PendingNotification {
        QMetaMethod signal;
        QVariant value;
    };

    QList<PreviousValue> previousValues;
    QHash<int, PendingNotification> changedNotifications;
    previousValues.reserve(values.size());

    // jsonToState() 已先完成名稱與型別驗證。這裡先保存舊值並阻擋 signal，
    // 確保 snapshot/patch 全部寫入成功後，QML 才看到任何 NOTIFY。
    {
        const QSignalBlocker blocker(&proxy);
        for (const PropertyContract &entry : contract.properties) {
            if (!values.contains(entry.name)) {
                continue;
            }
            const QVariant previous = entry.property.read(&proxy);
            const QVariant next = values.value(entry.name);
            if (previous == next) {
                continue;
            }
            previousValues.append({entry.property, previous});
            if (!entry.property.write(&proxy, next)) {
                for (auto rollback = previousValues.crbegin();
                     rollback != previousValues.crend(); ++rollback) {
                    rollback->property.write(&proxy, rollback->value);
                }
                if (error) {
                    *error = QObject::tr("Could not write mirrored property %1.")
                                 .arg(entry.name);
                }
                return false;
            }
            const int notifyIndex = entry.property.notifySignalIndex();
            if (!changedNotifications.contains(notifyIndex)) {
                changedNotifications.insert(
                    notifyIndex,
                    {entry.property.notifySignal(), next});
            }
        }
    }

    // 共用 NOTIFY（例如 digitalDataChanged）只送一次，而且所有 property
    // 都已經是新值，避免 QML 看見半份 snapshot。
    QList<int> notifyIndexes(changedNotifications.keyBegin(),
                             changedNotifications.keyEnd());
    std::sort(notifyIndexes.begin(), notifyIndexes.end());
    for (const int index : std::as_const(notifyIndexes)) {
        const PendingNotification notification = changedNotifications.value(index);
        bool invoked = false;
        if (notification.signal.parameterCount() == 0) {
            invoked = notification.signal.invoke(&proxy, Qt::DirectConnection);
        } else {
            QVariant value = notification.value;
            const QMetaType parameterType = notification.signal.parameterMetaType(0);
            if (value.metaType() != parameterType && !value.convert(parameterType)) {
                if (error) {
                    *error = QObject::tr(
                        "Could not convert mirrored property notification value.");
                }
                return false;
            }
            invoked = notification.signal.invoke(
                &proxy, Qt::DirectConnection,
                QGenericArgument(parameterType.name(), value.constData()));
        }
        if (!invoked) {
            if (error) {
                *error = QObject::tr("Could not emit mirrored property notification.");
            }
            return false;
        }
    }
    return true;
}

bool parseCounter(const QJsonValue &value, quint64 *result)
{
    if (!value.isString()) {
        return false;
    }
    bool ok = false;
    const quint64 parsed = value.toString().toULongLong(&ok);
    if (ok && result) {
        *result = parsed;
    }
    return ok;
}

QJsonObject baseEnvelope(const QString &type,
                         const QString &contractHash)
{
    return {{QStringLiteral("type"), type},
            {QStringLiteral("protocol"), ProxyMirrorHost::ProtocolVersion},
            {QStringLiteral("contractHash"), contractHash}};
}

const SignalContract *findSignal(const MirrorContract &contract,
                                 const QString &signature)
{
    const auto found = std::find_if(
        contract.requestSignals.constBegin(), contract.requestSignals.constEnd(),
        [&signature](const SignalContract &signal) {
            return signal.signature == signature;
        });
    return found == contract.requestSignals.constEnd() ? nullptr : &*found;
}

bool invokeSignal(QObject &proxy,
                  const SignalContract &signal,
                  const QVariantList &arguments)
{
    if (arguments.size() > 10) {
        return false;
    }
    QGenericArgument generic[10];
    for (qsizetype index = 0; index < arguments.size(); ++index) {
        generic[index] = QGenericArgument(
            signal.parameterTypes.at(index).name(),
            arguments.at(index).constData());
    }
    return signal.method.invoke(&proxy, Qt::DirectConnection,
                                generic[0], generic[1], generic[2], generic[3],
                                generic[4], generic[5], generic[6], generic[7],
                                generic[8], generic[9]);
}

QStringList sortedKeys(const QVariantMap &values)
{
    QStringList keys = values.keys();
    keys.sort();
    return keys;
}

} // namespace

class ProxyMirrorHost::Private
{
public:
    Private(ProxyMirrorHost *owner, QObject &target)
        : q(owner)
        , proxy(&target)
        , contract(inspectContract(target))
        , stateSessionId(QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
        if (!contract.isValid()) {
            return;
        }
        lastState = readState(*proxy, contract);
        revision = 1;
        const int slotIndex = ProxyMirrorHost::staticMetaObject.indexOfSlot(
            "handlePropertyNotification()");
        const QMetaMethod slot = ProxyMirrorHost::staticMetaObject.method(slotIndex);
        QSet<int> connectedSignals;
        for (const PropertyContract &property : contract.properties) {
            const int signalIndex = property.property.notifySignalIndex();
            if (connectedSignals.contains(signalIndex)) {
                continue;
            }
            connectedSignals.insert(signalIndex);
            QObject::connect(proxy, property.property.notifySignal(), q, slot,
                             Qt::DirectConnection);
        }
    }

    void schedulePatch()
    {
        if (patchScheduled || !contract.isValid()) {
            return;
        }
        patchScheduled = true;
        QTimer::singleShot(0, q, [this] { flushPatch(); });
    }

    void flushPatch()
    {
        if (!contract.isValid() || !proxy) {
            return;
        }
        patchScheduled = false;
        const QVariantMap current = readState(*proxy, contract);
        QVariantMap changes;
        for (auto it = current.constBegin(); it != current.constEnd(); ++it) {
            if (!lastState.contains(it.key()) || lastState.value(it.key()) != it.value()) {
                changes.insert(it.key(), it.value());
            }
        }
        if (changes.isEmpty()) {
            return;
        }
        const quint64 baseRevision = revision;
        ++revision;
        lastState = current;
        QJsonObject patch = baseEnvelope(QStringLiteral("proxy.patch"),
                                         contract.hash);
        patch.insert(QStringLiteral("stateSessionId"), stateSessionId);
        patch.insert(QStringLiteral("baseRevision"), QString::number(baseRevision));
        patch.insert(QStringLiteral("revision"), QString::number(revision));
        patch.insert(QStringLiteral("properties"),
                     stateToJson(changes, contract));
        emit q->patchReady(patch);
    }

    ProxyMirrorHost *q;
    QPointer<QObject> proxy;
    MirrorContract contract;
    QString stateSessionId;
    QVariantMap lastState;
    QHash<QString, quint64> lastRequestSequence;
    quint64 revision = 0;
    bool patchScheduled = false;
};

ProxyMirrorHost::ProxyMirrorHost(QObject &authoritativeProxy, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>(this, authoritativeProxy))
{
}

ProxyMirrorHost::~ProxyMirrorHost()
{
    if (d->proxy) {
        QObject::disconnect(d->proxy, nullptr, this, nullptr);
    }
}

bool ProxyMirrorHost::isValid() const
{
    return d->contract.isValid() && !d->proxy.isNull();
}
QString ProxyMirrorHost::validationError() const
{
    return d->proxy ? d->contract.error : tr("The authoritative Proxy was destroyed.");
}
QString ProxyMirrorHost::contractHash() const { return d->contract.hash; }
QJsonObject ProxyMirrorHost::contractDescriptor() const { return d->contract.descriptor; }
QString ProxyMirrorHost::stateSessionId() const { return d->stateSessionId; }
quint64 ProxyMirrorHost::revision() const { return d->revision; }

QJsonObject ProxyMirrorHost::makeSnapshot()
{
    if (!isValid()) {
        return {};
    }
    d->flushPatch();
    QJsonObject snapshot = baseEnvelope(QStringLiteral("proxy.snapshot"),
                                        d->contract.hash);
    snapshot.insert(QStringLiteral("stateSessionId"), d->stateSessionId);
    snapshot.insert(QStringLiteral("revision"), QString::number(d->revision));
    snapshot.insert(QStringLiteral("properties"),
                    stateToJson(d->lastState, d->contract));
    return snapshot;
}

ProxyMirrorApplyResult ProxyMirrorHost::applySignalEnvelope(
    const QJsonObject &envelope)
{
    ProxyMirrorApplyResult result;
    const auto reject = [this, &result](const QString &error) {
        result.code = ProxyMirrorApplyCode::Rejected;
        result.error = error;
        emit envelopeRejected(error);
        return result;
    };
    if (!isValid()) {
        return reject(validationError());
    }
    if (envelope.value(QStringLiteral("type")).toString()
            != QStringLiteral("proxy.signal")
        || envelope.value(QStringLiteral("protocol")).toInt(-1)
            != ProtocolVersion
        || envelope.value(QStringLiteral("contractHash")).toString()
            != contractHash()) {
        return reject(tr("Invalid Proxy Mirror signal envelope."));
    }
    const QString requestSessionId = envelope.value(
        QStringLiteral("requestSessionId")).toString();
    const QString observedStateSessionId = envelope.value(
        QStringLiteral("stateSessionId")).toString();
    quint64 sequence = 0;
    if (requestSessionId.isEmpty()
        || observedStateSessionId != d->stateSessionId
        || !parseCounter(envelope.value(QStringLiteral("sequence")), &sequence)
        || sequence == 0) {
        return reject(tr("Invalid or stale Proxy Mirror request session."));
    }
    result.sequence = sequence;
    const quint64 previous = d->lastRequestSequence.value(requestSessionId, 0);
    if (sequence == previous) {
        result.code = ProxyMirrorApplyCode::Duplicate;
        return result;
    }
    if (sequence < previous) {
        result.code = ProxyMirrorApplyCode::Stale;
        return result;
    }
    if (sequence != previous + 1) {
        return reject(tr("Proxy Mirror request sequence gap."));
    }

    const QString signature = envelope.value(QStringLiteral("signal")).toString();
    const SignalContract *signal = findSignal(d->contract, signature);
    const QJsonValue argumentValue = envelope.value(QStringLiteral("arguments"));
    if (!signal || !argumentValue.isArray()
        || argumentValue.toArray().size() != signal->parameterTypes.size()) {
        return reject(tr("Unknown request signal or argument count mismatch."));
    }
    QVariantList arguments;
    for (int index = 0; index < signal->parameterTypes.size(); ++index) {
        QVariant argument;
        if (!jsonToVariant(argumentValue.toArray().at(index),
                           signal->parameterTypes.at(index), &argument)) {
            return reject(tr("Request signal argument %1 has an invalid type.")
                              .arg(index));
        }
        arguments.append(argument);
    }
    if (!d->proxy || !invokeSignal(*d->proxy, *signal, arguments)) {
        return reject(tr("Could not invoke request signal %1.").arg(signature));
    }
    d->lastRequestSequence.insert(requestSessionId, sequence);
    result.code = ProxyMirrorApplyCode::Applied;
    emit requestApplied(requestSessionId, sequence, signature);
    return result;
}

ProxyMirrorApplyResult ProxyMirrorHost::applyPropertyEnvelope(
    const QJsonObject &envelope)
{
    ProxyMirrorApplyResult result;
    const auto reject = [this, &result](const QString &error) {
        result.code = ProxyMirrorApplyCode::Rejected;
        result.error = error;
        emit envelopeRejected(error);
        return result;
    };
    if (!isValid()) {
        return reject(validationError());
    }
    if (envelope.value(QStringLiteral("type")).toString()
            != QStringLiteral("proxy.properties")
        || envelope.value(QStringLiteral("protocol")).toInt(-1)
            != ProtocolVersion
        || envelope.value(QStringLiteral("contractHash")).toString()
            != contractHash()) {
        return reject(tr("Invalid Proxy Mirror property envelope."));
    }
    const QString requestSessionId = envelope.value(
        QStringLiteral("requestSessionId")).toString();
    const QString observedStateSessionId = envelope.value(
        QStringLiteral("stateSessionId")).toString();
    quint64 sequence = 0;
    if (requestSessionId.isEmpty()
        || observedStateSessionId != d->stateSessionId
        || !parseCounter(envelope.value(QStringLiteral("sequence")), &sequence)
        || sequence == 0) {
        return reject(tr("Invalid or stale Proxy Mirror request session."));
    }
    result.sequence = sequence;
    const quint64 previous = d->lastRequestSequence.value(requestSessionId, 0);
    if (sequence == previous) {
        result.code = ProxyMirrorApplyCode::Duplicate;
        return result;
    }
    if (sequence < previous) {
        result.code = ProxyMirrorApplyCode::Stale;
        return result;
    }
    if (sequence != previous + 1) {
        return reject(tr("Proxy Mirror request sequence gap."));
    }

    const QJsonValue propertyValue = envelope.value(
        QStringLiteral("properties"));
    QVariantMap properties;
    QString error;
    if (!propertyValue.isObject()
        || !jsonToState(propertyValue.toObject(), d->contract, false,
                        &properties, &error)
        || properties.isEmpty()) {
        return reject(error.isEmpty()
                          ? tr("Proxy Mirror property update is empty.")
                          : error);
    }
    if (!d->proxy || !writeState(*d->proxy, d->contract, properties, &error)) {
        return reject(error.isEmpty()
                          ? tr("Could not apply Proxy Mirror property update.")
                          : error);
    }
    d->lastRequestSequence.insert(requestSessionId, sequence);
    result.code = ProxyMirrorApplyCode::Applied;
    return result;
}

void ProxyMirrorHost::releaseRequestSession(const QString &requestSessionId)
{
    d->lastRequestSequence.remove(requestSessionId);
}

void ProxyMirrorHost::handlePropertyNotification()
{
    if (d->proxy) {
        d->schedulePatch();
    }
}

class ProxyMirrorClient::Private
{
public:
    Private(ProxyMirrorClient *owner, QObject &target)
        : q(owner)
        , proxy(&target)
        , contract(inspectContract(target))
        , requestSessionId(QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
        if (!contract.isValid()) {
            return;
        }
        const int slotIndex = ProxyMirrorClient::staticMetaObject.indexOfSlot(
            "handleLocalPropertyNotification()");
        const QMetaMethod slot = ProxyMirrorClient::staticMetaObject.method(
            slotIndex);
        QSet<int> connectedSignals;
        for (const PropertyContract &property : contract.properties) {
            const int signalIndex = property.property.notifySignalIndex();
            propertyNamesByNotify[signalIndex].append(property.name);
            if (connectedSignals.contains(signalIndex)) {
                continue;
            }
            connectedSignals.insert(signalIndex);
            QObject::connect(proxy, property.property.notifySignal(), q, slot,
                             Qt::DirectConnection);
        }
    }

    ProxyMirrorClient *q;
    QPointer<QObject> proxy;
    MirrorContract contract;
    QString requestSessionId;
    QString stateSessionId;
    quint64 revision = 0;
    quint64 nextSequence = 1;
    bool hasState = false;
    bool snapshotRequired = true;
    bool applyingState = false;
    bool localPropertyWritesEnabled = true;
    QHash<int, QStringList> propertyNamesByNotify;
};

ProxyMirrorClient::ProxyMirrorClient(QObject &mirroredProxy, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>(this, mirroredProxy))
{
}

ProxyMirrorClient::~ProxyMirrorClient() = default;

bool ProxyMirrorClient::isValid() const
{
    return d->contract.isValid() && !d->proxy.isNull();
}
QString ProxyMirrorClient::validationError() const
{
    return d->proxy ? d->contract.error : tr("The mirrored Proxy was destroyed.");
}
QString ProxyMirrorClient::contractHash() const { return d->contract.hash; }
QJsonObject ProxyMirrorClient::contractDescriptor() const { return d->contract.descriptor; }
QString ProxyMirrorClient::sessionId() const { return d->stateSessionId; }
quint64 ProxyMirrorClient::revision() const { return d->revision; }
bool ProxyMirrorClient::hasState() const { return d->hasState; }

void ProxyMirrorClient::setLocalPropertyWritesEnabled(bool enabled)
{
    d->localPropertyWritesEnabled = enabled;
}

bool ProxyMirrorClient::localPropertyWritesEnabled() const
{
    return d->localPropertyWritesEnabled;
}

void ProxyMirrorClient::beginRequestSession()
{
    d->requestSessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    d->nextSequence = 1;
}

QJsonObject ProxyMirrorClient::makeHello() const
{
    QJsonObject hello = baseEnvelope(QStringLiteral("proxy.hello"),
                                     d->contract.hash);
    hello.insert(QStringLiteral("requestSessionId"), d->requestSessionId);
    return hello;
}

QJsonObject ProxyMirrorClient::makeSignalEnvelope(
    const QString &signalSignature,
    const QVariantList &arguments,
    QString *errorMessage)
{
    if (!isValid() || !d->hasState || d->snapshotRequired
        || d->stateSessionId.isEmpty()) {
        const QString error = tr("A Proxy Mirror snapshot is required before sending a request.");
        if (errorMessage) *errorMessage = error;
        emit envelopeRejected(error);
        return {};
    }
    const SignalContract *signal = findSignal(d->contract, signalSignature);
    if (!signal || signal->parameterTypes.size() != arguments.size()) {
        const QString error = tr("Unknown request signal or argument count mismatch: %1")
                                  .arg(signalSignature);
        if (errorMessage) *errorMessage = error;
        emit envelopeRejected(error);
        return {};
    }
    QJsonArray jsonArguments;
    for (int index = 0; index < arguments.size(); ++index) {
        QVariant converted = arguments.at(index);
        if (!converted.convert(signal->parameterTypes.at(index))) {
            const QString error = tr("Request signal argument %1 has an invalid type.")
                                      .arg(index);
            if (errorMessage) *errorMessage = error;
            emit envelopeRejected(error);
            return {};
        }
        jsonArguments.append(variantToJson(converted,
                                           signal->parameterTypes.at(index)));
    }
    QJsonObject envelope = baseEnvelope(QStringLiteral("proxy.signal"),
                                        d->contract.hash);
    envelope.insert(QStringLiteral("requestSessionId"), d->requestSessionId);
    envelope.insert(QStringLiteral("stateSessionId"), d->stateSessionId);
    envelope.insert(QStringLiteral("sequence"), QString::number(d->nextSequence++));
    envelope.insert(QStringLiteral("signal"), signalSignature);
    envelope.insert(QStringLiteral("arguments"), jsonArguments);
    return envelope;
}

ProxyMirrorApplyResult ProxyMirrorClient::applyStateEnvelope(
    const QJsonObject &envelope)
{
    ProxyMirrorApplyResult result;
    const auto reject = [this, &result](const QString &error, bool resync) {
        result.code = ProxyMirrorApplyCode::Rejected;
        result.error = error;
        emit envelopeRejected(error);
        if (resync) {
            d->snapshotRequired = true;
            emit resyncRequired();
        }
        return result;
    };
    if (!isValid()) {
        return reject(validationError(), false);
    }
    const QString type = envelope.value(QStringLiteral("type")).toString();
    if ((type != QStringLiteral("proxy.snapshot")
         && type != QStringLiteral("proxy.patch"))
        || envelope.value(QStringLiteral("protocol")).toInt(-1)
            != ProxyMirrorHost::ProtocolVersion
        || envelope.value(QStringLiteral("contractHash")).toString()
            != contractHash()) {
        return reject(tr("Invalid Proxy Mirror state envelope."), false);
    }
    const QString sessionId = envelope.value(
        QStringLiteral("stateSessionId")).toString();
    quint64 nextRevision = 0;
    if (sessionId.isEmpty()
        || !parseCounter(envelope.value(QStringLiteral("revision")),
                         &nextRevision)
        || !envelope.value(QStringLiteral("properties")).isObject()) {
        return reject(tr("Invalid Proxy Mirror state metadata."), true);
    }
    result.revision = nextRevision;

    if (type == QStringLiteral("proxy.snapshot")) {
        if (d->hasState && !d->snapshotRequired
            && sessionId != d->stateSessionId) {
            return reject(
                tr("The Proxy Mirror state session changed without reconnecting."),
                true);
        }
        if (d->hasState && sessionId == d->stateSessionId) {
            if (!d->snapshotRequired && nextRevision == d->revision) {
                result.code = ProxyMirrorApplyCode::Duplicate;
                return result;
            }
            if (nextRevision < d->revision) {
                if (d->snapshotRequired) {
                    return reject(tr("Proxy Mirror snapshot revision moved backwards."),
                                  true);
                }
                result.code = ProxyMirrorApplyCode::Stale;
                return result;
            }
        }
        QVariantMap state;
        QString error;
        if (!jsonToState(envelope.value(QStringLiteral("properties")).toObject(),
                         d->contract, true, &state, &error)) {
            return reject(error, true);
        }
        const bool previousApplyingState = d->applyingState;
        d->applyingState = true;
        const bool applied = writeState(*d->proxy, d->contract, state, &error);
        d->applyingState = previousApplyingState;
        if (!applied) {
            return reject(error, true);
        }
        d->stateSessionId = sessionId;
        d->revision = nextRevision;
        d->hasState = true;
        d->snapshotRequired = false;
        result.code = ProxyMirrorApplyCode::Applied;
        emit stateApplied(nextRevision, sortedKeys(state));
        return result;
    }

    if (d->snapshotRequired || !d->hasState
        || sessionId != d->stateSessionId) {
        return reject(tr("A full Proxy Mirror snapshot is required."), true);
    }
    quint64 baseRevision = 0;
    if (!parseCounter(envelope.value(QStringLiteral("baseRevision")),
                      &baseRevision)) {
        return reject(tr("Invalid Proxy Mirror patch base revision."), true);
    }
    if (nextRevision == d->revision) {
        if (baseRevision + 1 == nextRevision) {
            result.code = ProxyMirrorApplyCode::Duplicate;
            return result;
        }
        return reject(tr("Invalid duplicate Proxy Mirror patch."), true);
    }
    if (nextRevision < d->revision) {
        result.code = ProxyMirrorApplyCode::Stale;
        return result;
    }
    if (baseRevision != d->revision
        || nextRevision != baseRevision + 1) {
        return reject(tr("Proxy Mirror patch revision gap."), true);
    }
    QVariantMap changes;
    QString error;
    if (!jsonToState(envelope.value(QStringLiteral("properties")).toObject(),
                     d->contract, false, &changes, &error)
        || changes.isEmpty()) {
        return reject(error.isEmpty() ? tr("Proxy Mirror patch is empty.") : error,
                      true);
    }
    const bool previousApplyingState = d->applyingState;
    d->applyingState = true;
    const bool applied = writeState(*d->proxy, d->contract, changes, &error);
    d->applyingState = previousApplyingState;
    if (!applied) {
        return reject(error, true);
    }
    d->revision = nextRevision;
    result.code = ProxyMirrorApplyCode::Applied;
    emit stateApplied(nextRevision, sortedKeys(changes));
    return result;
}

void ProxyMirrorClient::requireSnapshot()
{
    d->snapshotRequired = true;
}

void ProxyMirrorClient::handleLocalPropertyNotification()
{
    if (!isValid() || d->applyingState || !d->localPropertyWritesEnabled
        || !d->hasState
        || d->snapshotRequired || d->stateSessionId.isEmpty()) {
        return;
    }
    const QStringList propertyNames = d->propertyNamesByNotify.value(
        senderSignalIndex());
    if (propertyNames.isEmpty() || !d->proxy) {
        return;
    }

    QVariantMap values;
    for (const PropertyContract &property : d->contract.properties) {
        if (propertyNames.contains(property.name)) {
            values.insert(property.name, property.property.read(d->proxy));
        }
    }
    if (values.isEmpty()) {
        return;
    }

    QJsonObject envelope = baseEnvelope(QStringLiteral("proxy.properties"),
                                        d->contract.hash);
    envelope.insert(QStringLiteral("requestSessionId"), d->requestSessionId);
    envelope.insert(QStringLiteral("stateSessionId"), d->stateSessionId);
    envelope.insert(QStringLiteral("sequence"),
                    QString::number(d->nextSequence++));
    envelope.insert(QStringLiteral("properties"),
                    stateToJson(values, d->contract));
    emit propertyWriteReady(envelope);
}
