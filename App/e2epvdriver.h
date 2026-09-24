#pragma once

// ---------------------------------------------------------------------------
// DEV / E2E ONLY - desktop build, inactive unless TAIDAFLOW_E2E_PV_FILE is set.
//
// On the core branch the read-only process values (…Pv) are driven by the real
// backend (Manager: ADAM Modbus reads -> TaidaFlowProxy setters). On a bench with no
// devices (192.168.1.201..205 unreachable, no COM2) nothing ever updates them, so the
// Desktop -> WASM direction of a PV cannot be observed. For the mirror E2E test this
// driver stands in for the field devices: it polls a text file with
// `propertyName=value` lines and writes the values into the *authoritative desktop*
// TaidaFlowProxy through its normal setter (QObject::setProperty).
//   * Only properties whose name ends in "Pv" are accepted, so it cannot fake operator
//     (SV) writes and never reaches Manager/Modbus (Core connects only SV NOTIFYs).
//   * double PVs take a number; bool PVs (motorRunningPv, wayValveOpenPv) take
//     true/false/1/0.
// The mirror transport under test is the real one; this only supplies the
// desktop-side stimulus. Not compiled into the WebAssembly build (main.cpp guards the
// include with !Q_OS_WASM) and never constructed unless the environment variable is set.
// ---------------------------------------------------------------------------

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QMetaProperty>
#include <QObject>
#include <QTimer>

class E2ePvDriver final : public QObject
{
public:
    E2ePvDriver(QObject *target, const QString &filePath, QObject *parent = nullptr)
        : QObject(parent), m_target(target), m_path(filePath)
    {
        m_timer.setInterval(200);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] { poll(); });
        m_timer.start();
        qInfo().noquote() << "E2E PV driver (dev-only) watching" << m_path;
    }

private:
    static bool parseBool(const QString &text, bool *ok)
    {
        const QString t = text.trimmed().toLower();
        *ok = true;
        if (t == QLatin1String("true") || t == QLatin1String("1"))
            return true;
        if (t == QLatin1String("false") || t == QLatin1String("0"))
            return false;
        *ok = false;
        return false;
    }

    void poll()
    {
        const QFileInfo info(m_path);
        if (!info.exists())
            return;
        const QDateTime stamp = info.lastModified();
        if (stamp == m_lastStamp && info.size() == m_lastSize)
            return;
        m_lastStamp = stamp;
        m_lastSize = info.size();

        QFile file(m_path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return;
        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine()).trimmed();
            const qsizetype eq = line.indexOf(QLatin1Char('='));
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || eq <= 0)
                continue;
            const QString name = line.left(eq).trimmed();
            const QString text = line.mid(eq + 1).trimmed();
            const QMetaObject *mo = m_target->metaObject();
            const int idx = mo->indexOfProperty(name.toLatin1().constData());
            if (!name.endsWith(QLatin1String("Pv")) || idx < 0) {
                qWarning().noquote() << "E2E PV driver ignored line:" << line;
                continue;
            }
            const int type = mo->property(idx).metaType().id();
            QVariant value;
            bool ok = false;
            if (type == QMetaType::Double) {
                value = text.toDouble(&ok);
            } else if (type == QMetaType::Bool) {
                value = parseBool(text, &ok);
            }
            if (!ok) {
                qWarning().noquote() << "E2E PV driver ignored line:" << line;
                continue;
            }
            m_target->setProperty(name.toLatin1().constData(), value);
            qInfo().noquote() << "E2E PV driver set" << name << "=" << value.toString();
        }
    }

    QObject *m_target;
    QString m_path;
    QTimer m_timer;
    QDateTime m_lastStamp;
    qint64 m_lastSize = -1;
};
