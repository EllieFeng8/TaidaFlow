#include "embeddedfonts.h"

#include <QChar>
#include <QDebug>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QStringList>

namespace TaidaFlowFonts {

namespace {

const char *const kFontFiles[] = {
    ":/fonts/TaidaFlowNotoSansTC-Regular.ttf",
    ":/fonts/TaidaFlowNotoSansTC-Bold.ttf",
};

void appendUnique(QStringList &list, const QString &value)
{
    if (!value.isEmpty() && !list.contains(value, Qt::CaseInsensitive))
        list.append(value);
}

} // namespace

QString loadEmbeddedCjkFont()
{
    QString family;
    for (const char *path : kFontFiles) {
        const int id = QFontDatabase::addApplicationFont(QString::fromLatin1(path));
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (id < 0 || families.isEmpty()) {
            qWarning().noquote() << "Embedded CJK font failed to load:" << path;
            return QString();
        }
        if (family.isEmpty())
            family = families.constFirst();
    }
    qInfo().noquote() << "Embedded CJK font loaded:" << family
                      << "styles:" << QFontDatabase::styles(family).join(QLatin1Char(','));
    return family;
}

void installCjkFallbackChain(const QString &family)
{
    if (family.isEmpty())
        return;

    const QStringList available = QFontDatabase::families();

    // Families the UI can request: the platform/application default, the
    // Qt Quick Controls Universal style font, the explicit QML family, and the
    // families used by the bundled Qt Design Studio components.
    QStringList requested;
    for (const QString &f : QGuiApplication::font().families())
        appendUnique(requested, f);
    for (const QString &f : QFontDatabase::systemFont(QFontDatabase::GeneralFont).families())
        appendUnique(requested, f);
    for (const QString &f : { QStringLiteral("Segoe UI"), QStringLiteral("Arial"),
                              QStringLiteral("Verdana"), QStringLiteral("Sans Serif") })
        appendUnique(requested, f);

    for (const QString &f : std::as_const(requested)) {
        if (f.compare(family, Qt::CaseInsensitive) != 0)
            QFont::insertSubstitutions(f, { family });
    }

    // Monospace text (Consolas in QML): keep a monospace Latin face first when
    // one exists (DejaVu Sans Mono ships with Qt for WebAssembly), then CJK.
    QStringList monoChain;
    for (const QString &f : { QStringLiteral("DejaVu Sans Mono"),
                              QFontDatabase::systemFont(QFontDatabase::FixedFont).family() }) {
        if (available.contains(f, Qt::CaseInsensitive))
            appendUnique(monoChain, f);
    }
    appendUnique(monoChain, family);
    QFont::insertSubstitutions(QStringLiteral("Consolas"), monoChain);

    // Script-level fallback for Han text in any family not listed above.
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Han, family);
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Common, family);

    qInfo().noquote() << "CJK fallback chain:" << requested.join(QLatin1Char(','))
                      << "->" << family << "| Consolas ->" << monoChain.join(QLatin1Char(','));
}

} // namespace TaidaFlowFonts
