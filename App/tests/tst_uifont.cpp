// w2-082: interface font of the core branch (desktop and web) = the embedded Noto Sans TC subset.
//
// What runs here is App/main.cpp's own code, not a copy: App/tests/CMakeLists.txt cuts, at
// configure time, applyUiFont() and the two font statements of main() (the "(core only)" block and
// the HOOK block) out of main.cpp into generated .inc files that are compiled below.
//   tst_uifont           the sequence as main.cpp compiles it for the desktop
//   tst_uifont_wasmpath  the same text with main.cpp's `#if defined(Q_OS_WASM)` switched on
//                        (installCjkFallbackChain, then applyUiFont = the order on the web),
//                        run on the desktop font database (the browser itself is not used)
// Real App/embeddedfonts.cpp, real committed App/fonts/*.ttf (Qt resource like the app), real
// Windows font database (default platform plugin; no window is ever shown), real QML engine with
// the Universal style (as qtquickcontrols2.conf) for the ApplicationWindow check. No mocks.
#include <QtTest>

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QRawFont>
#include <QRegularExpression>
#include <QTextLayout>

#include <memory>

#include "embeddedfonts.h"

#ifndef UIFONT_WASM_PATH
#define UIFONT_WASM_PATH 0
#endif

namespace MainCpp {
// App/main.cpp: QString applyUiFont(const QString &requested) { ... }
#include "uifont_applyuifont.inc"
} // namespace MainCpp

namespace {

const QString kEmbeddedFamily = QStringLiteral("TaidaFlow Noto Sans TC");
const QString kHookLine = QStringLiteral(
    "const QString uiFontFamilyRequest = cjkFamily.isEmpty() ? "
    "QStringLiteral(\"Microsoft JhengHei UI\") : cjkFamily;");

struct Sequence
{
    QString cjkFamily;
    QString request;
    QStringList log;
};
Sequence g_seq;
QStringList *g_capture = nullptr;
QtMessageHandler g_previousHandler = nullptr;

void captureHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (g_capture)
        g_capture->append(msg);
    if (g_previousHandler)
        g_previousHandler(type, ctx, msg);
}

// main.cpp, in main() order: "(core only)" block ... HOOK block (see CMakeLists.txt).
void runMainCppSequence()
{
    using namespace MainCpp;
#if UIFONT_WASM_PATH
#include "uifont_sequence_wasm.inc"
#else
#include "uifont_sequence_desktop.inc"
#endif
    g_seq.cjkFamily = cjkFamily;
    g_seq.request = uiFontFamilyRequest;
}

QString readUtf8(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll()).remove(QLatin1Char('\r'));
}

bool isCjk(char32_t ucs)
{
    return QChar::script(ucs) == QChar::Script_Han || (ucs >= 0x3000 && ucs <= 0x303F)
           || (ucs >= 0xFF00 && ucs <= 0xFFEF);
}

// "family:text" for every glyph run of `text` drawn with `font`.
QStringList glyphRuns(const QFont &font, const QString &text)
{
    QTextLayout layout(text, font);
    layout.beginLayout();
    layout.createLine();
    layout.endLayout();
    QStringList out;
    for (const QGlyphRun &run : layout.glyphRuns()) {
        const QString family = run.rawFont().isValid() ? run.rawFont().familyName() : QStringLiteral("<invalid>");
        out.append(family + QLatin1Char(':') + QString::number(run.glyphIndexes().size()));
    }
    return out;
}

// Families that draw the CJK characters of `text` with `font` (one layout per character).
QStringList cjkFamilies(const QFont &font, const QString &text)
{
    QStringList families;
    for (const char32_t ucs : QStringView(text).toUcs4()) {
        if (!isCjk(ucs))
            continue;
        QTextLayout layout(QString::fromUcs4(&ucs, 1), font);
        layout.beginLayout();
        layout.createLine();
        layout.endLayout();
        for (const QGlyphRun &run : layout.glyphRuns()) {
            const QString f = run.rawFont().familyName();
            if (!families.contains(f))
                families.append(f);
        }
    }
    return families;
}

// Strings of the real UI (TaidaFlowContent / Core), mixed Latin, digits, symbols and Chinese.
const QStringList kUiSamples = {
    QStringLiteral("PT-01 設備接口出口壓力 101.3 kPa"),
    QStringLiteral("設備接口入口溫度 23.5 °C"),
    QStringLiteral("流量計 FM 12.0 L/MIN"),
    QStringLiteral("匯出完成 · 1200 筆 · 已儲存：history.csv"),
    QStringLiteral("此日期區間沒有警報紀錄（−5 ≥ 0 → ✓）"),
    QStringLiteral("2026/10/01 13:45:00 – 2026/10/01 14:45:00 Main Alarm History"),
};

} // namespace

class TstUiFont : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void mainCppSource();
    void embeddedFontLoaded();
    void hookRequestsEmbeddedFamily();
    void applicationFont();
    void consolasSubstitutes();
    void otherFamiliesFallback();
    void uiTextGlyphs_data();
    void uiTextGlyphs();
    void consolasNumbersKeepChineseInEmbedded_data();
    void consolasNumbersKeepChineseInEmbedded();
    void subsetCoverage_data();
    void subsetCoverage();
    void applicationWindowFont();
};

void TstUiFont::initTestCase()
{
    qInfo().noquote() << "variant:" << (UIFONT_WASM_PATH ? "wasm path (Q_OS_WASM branch of main.cpp)" : "desktop");
    qInfo().noquote() << "platform:" << QGuiApplication::platformName();
    QStringList captured;
    g_capture = &captured;
    g_previousHandler = qInstallMessageHandler(captureHandler);
    runMainCppSequence();
    qInstallMessageHandler(g_previousHandler);
    g_capture = nullptr;
    g_seq.log = captured;
    qInfo().noquote() << "cjkFamily =" << g_seq.cjkFamily << "| uiFontFamilyRequest =" << g_seq.request;
}

// D1: only the HOOK line changed, applyUiFont runs after the embedded font is loaded (and, on the
// web, after installCjkFallbackChain) and before the QML engine.
void TstUiFont::mainCppSource()
{
    const QString src = readUtf8(QStringLiteral(UIFONT_MAIN_CPP));
    QVERIFY2(!src.isEmpty(), UIFONT_MAIN_CPP);
    const QRegularExpression hookRe(QStringLiteral("^\\s*const QString uiFontFamilyRequest = .*$"),
                                    QRegularExpression::MultilineOption);
    QStringList hookLines;
    qsizetype hook = -1; // the statement itself (the HOOK comment above it quotes the same text)
    for (auto it = hookRe.globalMatch(src); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        hookLines.append(m.captured(0).trimmed());
        hook = m.capturedStart(0);
    }
    QCOMPARE(hookLines.size(), 1);
    QCOMPARE(hookLines.constFirst(), kHookLine);
    QCOMPARE(src.count(QStringLiteral("applyUiFont(uiFontFamilyRequest);")), 1);
    QCOMPARE(src.count(QStringLiteral("TaidaFlowFonts::loadEmbeddedCjkFont();")), 1);

    const qsizetype app = src.indexOf(QStringLiteral("QApplication app(argc, argv);"));
    const qsizetype load = src.indexOf(QStringLiteral("const QString cjkFamily = TaidaFlowFonts::loadEmbeddedCjkFont();"));
    const qsizetype chain = src.indexOf(QStringLiteral("TaidaFlowFonts::installCjkFallbackChain(cjkFamily);"));
    const qsizetype apply = src.indexOf(QStringLiteral("applyUiFont(uiFontFamilyRequest);"));
    const qsizetype engine = src.indexOf(QStringLiteral("QQmlApplicationEngine engine;"));
    qInfo() << "offsets: app" << app << "load" << load << "chain" << chain << "hook" << hook << "apply" << apply
            << "engine" << engine;
    QVERIFY(app >= 0 && load > app);
    QVERIFY(chain > load);
    QVERIFY(hook > chain);
    QVERIFY(apply > hook);
    QVERIFY(engine > apply);
    // installCjkFallbackChain stays WebAssembly-only (the desktop keeps its own fallback chain).
    const qsizetype wasmIf = src.lastIndexOf(QStringLiteral("#if defined(Q_OS_WASM)"), chain);
    const qsizetype endIf = src.indexOf(QStringLiteral("#endif"), chain);
    QVERIFY(wasmIf > load && wasmIf < chain);
    QVERIFY(endIf > chain && endIf < hook);
}

void TstUiFont::embeddedFontLoaded()
{
    QCOMPARE(g_seq.cjkFamily, kEmbeddedFamily);
    QVERIFY(QFontDatabase::hasFamily(kEmbeddedFamily));
    const QStringList styles = QFontDatabase::styles(kEmbeddedFamily);
    qInfo() << "styles" << styles;
    QVERIFY2(styles.contains(QStringLiteral("Regular")) && styles.contains(QStringLiteral("Bold")),
             qPrintable(styles.join(QLatin1Char(','))));
}

void TstUiFont::hookRequestsEmbeddedFamily()
{
    QCOMPARE(g_seq.request, kEmbeddedFamily);
    const QString expected = QStringLiteral("[UiFont] interface font: ") + kEmbeddedFamily;
    bool logged = false;
    for (const QString &line : std::as_const(g_seq.log)) {
        qInfo().noquote() << "log:" << line;
        if (line.startsWith(expected))
            logged = true;
    }
    QVERIFY2(logged, qPrintable(expected));
}

void TstUiFont::applicationFont()
{
    const QFont appFont = QGuiApplication::font();
    QCOMPARE(appFont.family(), kEmbeddedFamily);
    QCOMPARE(appFont.families(), QStringList{kEmbeddedFamily});
    QCOMPARE(QFontInfo(appFont).family(), kEmbeddedFamily);
    QCOMPARE(QRawFont::fromFont(appFont).familyName(), kEmbeddedFamily);
    QCOMPARE(QFont().family(), kEmbeddedFamily); // what a Text without font.family gets

    QFont bold = appFont;
    bold.setBold(true);
    const QFontInfo boldInfo(bold);
    QCOMPARE(boldInfo.family(), kEmbeddedFamily);
    QVERIFY(boldInfo.bold());
    const QRawFont boldRaw = QRawFont::fromFont(bold);
    qInfo().noquote() << "regular raw:" << QRawFont::fromFont(appFont).styleName() << QRawFont::fromFont(appFont).weight()
                      << "| bold raw:" << boldRaw.styleName() << boldRaw.weight() << "| point size" << appFont.pointSizeF();
    QCOMPARE(boldRaw.familyName(), kEmbeddedFamily);
    QCOMPARE(boldRaw.weight(), int(QFont::Bold)); // the Bold file (wght 700), not a synthesized bold
    QCOMPARE(QRawFont::fromFont(appFont).weight(), int(QFont::Normal));
}

void TstUiFont::consolasSubstitutes()
{
    const QStringList subs = QFont::substitutes(QStringLiteral("Consolas"));
    qInfo().noquote() << "Consolas ->" << subs.join(QLatin1Char(','));
    QVERIFY(!subs.isEmpty());
#if UIFONT_WASM_PATH
    // Web: installCjkFallbackChain first puts a monospace Latin face (DejaVu Sans Mono in the
    // browser; here the desktop's FixedFont), then the embedded family; applyUiFont adds nothing new.
    QVERIFY(subs.contains(kEmbeddedFamily, Qt::CaseInsensitive));
    QCOMPARE(subs.last().compare(kEmbeddedFamily, Qt::CaseInsensitive), 0);
    QCOMPARE(subs.count(kEmbeddedFamily.toLower()), 1);
    // Numbers keep a monospace face in front of the embedded family (why the chain stays on the web).
    QVERIFY2(subs.size() >= 2 && QFontDatabase::isFixedPitch(subs.first()), qPrintable(subs.first()));
    QVERIFY(!QFontDatabase::isFixedPitch(kEmbeddedFamily));
#else
    QCOMPARE(subs.size(), 1);
    QCOMPARE(subs.first().compare(kEmbeddedFamily, Qt::CaseInsensitive), 0);
#endif
}

// Desktop: no fallback chain (platform fallback for families other than the UI font / Consolas);
// web: every family the UI may request falls back to the embedded family.
void TstUiFont::otherFamiliesFallback()
{
    const QStringList segoe = QFont::substitutes(QStringLiteral("Segoe UI"));
    qInfo().noquote() << "Segoe UI ->" << segoe.join(QLatin1Char(','));
#if UIFONT_WASM_PATH
    QVERIFY(segoe.contains(kEmbeddedFamily, Qt::CaseInsensitive));
    QVERIFY(QFont::substitutes(QStringLiteral("Arial")).contains(kEmbeddedFamily, Qt::CaseInsensitive));
#else
    QVERIFY(!segoe.contains(kEmbeddedFamily, Qt::CaseInsensitive));
#endif
}

void TstUiFont::uiTextGlyphs_data()
{
    QTest::addColumn<QString>("text");
    for (const QString &s : kUiSamples)
        QTest::newRow(qPrintable(s.left(12))) << s;
}

// Every glyph (Latin, digits, symbols, Chinese) of real UI strings in the interface font comes
// from the embedded subset: nothing switches to another font for a single character.
void TstUiFont::uiTextGlyphs()
{
    QFETCH(QString, text);
    for (const bool bold : {false, true}) {
        QFont f = QGuiApplication::font();
        f.setBold(bold);
        const QStringList runs = glyphRuns(f, text);
        qInfo().noquote() << (bold ? "bold" : "regular") << text << "->" << runs.join(QLatin1String(" | "));
        QVERIFY(!runs.isEmpty());
        for (const QString &run : runs)
            QVERIFY2(run.startsWith(kEmbeddedFamily + QLatin1Char(':')), qPrintable(run));
    }
}

void TstUiFont::consolasNumbersKeepChineseInEmbedded_data()
{
    QTest::addColumn<QString>("text");
    QTest::newRow("value") << QStringLiteral("12.5 設定值");
    QTest::newRow("date") << QStringLiteral("2026/10/01（三）13:45");
    QTest::newRow("units") << QStringLiteral("101.3 壓力 kPa");
}

// Numbers keep Consolas (font.family: "Consolas" in QML); Chinese inside them is drawn with the
// embedded family, not with the platform's CJK fallback.
void TstUiFont::consolasNumbersKeepChineseInEmbedded()
{
    QFETCH(QString, text);
    const QFont consolas(QStringLiteral("Consolas"));
    const QStringList cjk = cjkFamilies(consolas, text);
    qInfo().noquote() << "Consolas" << text << "CJK ->" << cjk.join(QLatin1Char(','))
                      << "| runs" << glyphRuns(consolas, text).join(QLatin1String(" | "));
    QCOMPARE(cjk, QStringList{kEmbeddedFamily});
#if !UIFONT_WASM_PATH
    // Desktop: the digits themselves stay Consolas.
    QTextLayout layout(QStringLiteral("0123456789.:/"), consolas);
    layout.beginLayout();
    layout.createLine();
    layout.endLayout();
    const QList<QGlyphRun> runs = layout.glyphRuns();
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.first().rawFont().familyName(), QStringLiteral("Consolas"));
#endif
}

void TstUiFont::subsetCoverage_data()
{
    QTest::addColumn<QString>("file");
    QTest::addColumn<QString>("style");
    QTest::newRow("Regular") << QStringLiteral(":/fonts/TaidaFlowNotoSansTC-Regular.ttf") << QStringLiteral("Regular");
    QTest::newRow("Bold") << QStringLiteral(":/fonts/TaidaFlowNotoSansTC-Bold.ttf") << QStringLiteral("Bold");
}

// D2: printable ASCII, the symbols of the UI and every character of charset.txt are in both files.
void TstUiFont::subsetCoverage()
{
    QFETCH(QString, file);
    QFETCH(QString, style);
    QFile f(file);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray data = f.readAll();
    const QRawFont raw(data, 16);
    QVERIFY(raw.isValid());
    QCOMPARE(raw.familyName(), kEmbeddedFamily);
    QCOMPARE(raw.weight(), int(style == QLatin1String("Bold") ? QFont::Bold : QFont::Normal));

    QStringList missing;
    int checked = 0;
    const auto check = [&](char32_t ucs) {
        ++checked;
        if (!raw.supportsCharacter(ucs))
            missing.append(QStringLiteral("U+%1").arg(uint(ucs), 4, 16, QLatin1Char('0')).toUpper());
    };
    for (char32_t c = 0x20; c <= 0x7E; ++c)
        check(c);
    const int ascii = checked;
    // Symbols on screen (scan: docs/evidence/w2-082/tools/scan-ui-symbols.ps1) and the ones the
    // subset keeps for later UI edits (make_font_subset EXTRA / EXTRA_SYMBOLS / EXTRA_RANGES).
    const QString symbols = QStringLiteral("°·×−–—…‹›→↓≥✓℃Δ§　、。「」『』《》〈〉【】！％（）＋，－／：；＝？｜～"
                                           "±µ²³÷«»©®‘’“”•‧‰′″←↑≤≠≈∞℉Ωμ〔〕［］｛｝＜＞＊＃＆＠＿０１２３４５６７８９");
    for (const char32_t c : QStringView(symbols).toUcs4())
        check(c);
    for (char32_t c = 0xA0; c <= 0xFF; ++c)
        check(c);
    const QString charset = readUtf8(QStringLiteral(UIFONT_CHARSET)).trimmed();
    QVERIFY(charset.size() > 500);
    int charsetCount = 0;
    for (const char32_t c : QStringView(charset).toUcs4()) {
        check(c);
        ++charsetCount;
    }
    qInfo().noquote() << style << ": checked" << checked << "code points (ASCII" << ascii << ", charset.txt" << charsetCount
                      << "), missing" << missing.size() << missing.join(QLatin1Char(' '));
    QVERIFY2(missing.isEmpty(), qPrintable(missing.join(QLatin1Char(' '))));
}

// The (template) ApplicationWindow of TaidaFlowContent/App.qml: its font.family binding, taken
// from App.qml itself, gives the embedded family to the window, to Qt Quick Controls (Universal
// style would use Segoe UI) and to popups. The rest of App.qml (pages, Td backend) is not loaded.
void TstUiFont::applicationWindowFont()
{
    const QString appQml = readUtf8(QStringLiteral(UIFONT_APP_QML));
    QVERIFY2(!appQml.isEmpty(), UIFONT_APP_QML);
    const QRegularExpression rootRe(QStringLiteral("^T\\.ApplicationWindow \\{$"), QRegularExpression::MultilineOption);
    const QRegularExpressionMatch root = rootRe.match(appQml);
    QVERIFY2(root.hasMatch(), "App.qml root object is not `T.ApplicationWindow {`");
    const QRegularExpression fontRe(QStringLiteral("^    font\\.family: (.+)$"), QRegularExpression::MultilineOption);
    const QRegularExpressionMatch fontLine = fontRe.match(appQml, root.capturedEnd());
    QVERIFY2(fontLine.hasMatch(), "App.qml root has no `font.family:` binding");
    const QString binding = fontLine.captured(1).trimmed();
    qInfo().noquote() << "App.qml root font.family:" << binding;
    QCOMPARE(binding, QStringLiteral("Application.font.family"));

    const QByteArray qml = QStringLiteral(
        "import QtQuick\n"
        "import QtQuick.Controls\n"
        "import QtQuick.Templates as T\n"
        "T.ApplicationWindow {\n"
        "    visible: false\n"
        "    font.family: %1\n"
        "    property alias label: label\n"
        "    property alias text: text\n"
        "    property alias button: button\n"
        "    property alias popupLabel: popupLabel\n"
        "    Label { id: label; text: \"警報 Alarm\" }\n"
        "    Text { id: text; text: \"設備 PT-01\" }\n"
        "    Button { id: button; text: \"匯出 CSV\" }\n"
        "    Popup { Label { id: popupLabel; text: \"日期區間錯誤\" } }\n"
        "}\n").arg(binding).toUtf8();
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(qml, QUrl(QStringLiteral("qrc:/w2082/AppWindowFont.qml")));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    const auto family = [&](const char *prop) {
        QObject *o = window->property(prop).value<QObject *>();
        return o ? o->property("font").value<QFont>().family() : QStringLiteral("<no %1>").arg(QLatin1String(prop));
    };
    const QString windowFamily = window->property("font").value<QFont>().family();
    qInfo().noquote() << "window" << windowFamily << "| Label" << family("label") << "| Text" << family("text")
                      << "| Button" << family("button") << "| Popup Label" << family("popupLabel");
    QCOMPARE(windowFamily, kEmbeddedFamily);
    QCOMPARE(family("label"), kEmbeddedFamily);
    QCOMPARE(family("text"), kEmbeddedFamily);
    QCOMPARE(family("button"), kEmbeddedFamily);
    QCOMPARE(family("popupLabel"), kEmbeddedFamily);
}

int main(int argc, char *argv[])
{
    // Same Qt Quick Controls style as the app (qtquickcontrols2.conf: Universal).
    qputenv("QT_QUICK_CONTROLS_STYLE", "Universal");
    QApplication app(argc, argv); // the app is a QApplication too (main.cpp)
    TstUiFont tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_uifont.moc"
