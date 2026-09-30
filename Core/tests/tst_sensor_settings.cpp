#include "../TaidaFlowProxy.h"
#include <QtTest>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QJSValue>
#include <limits>

static QQuickItem *findVisualItem(QQuickItem *item, const QString &name)
{
    if (item->objectName() == name) return item;
    for (auto child : item->childItems())
        if (auto found = findVisualItem(child, name)) return found;
    return nullptr;
}

class SensorSettingsTest : public QObject
{
    Q_OBJECT
private slots:
    void validateProxy() {
        TaidaFlowProxy proxy;
        QCOMPARE(proxy.pressureUnitSv(), "kPa");
        QSignalSpy unitSpy(&proxy, &TaidaFlowProxy::pressureUnitSvChanged);
        proxy.setPressureUnitSv("psi");
        proxy.setPressureUnitSv("psi");
        proxy.setPressureUnitSv("Pa");
        QCOMPARE(unitSpy.count(), 1);
        QCOMPARE(proxy.pressureUnitSv(), "psi");
        auto settings = proxy.sensorSettingsSv();
        QCOMPARE(settings.size(), 13);
        QVERIFY(!settings.contains("leak"));
        auto entry = settings["pt04"].toMap();
        entry["offset"] = -3.5;
        entry["lower"] = 10.0;
        entry["upper"] = 200.0;
        entry["lowerEnabled"] = true;
        entry["upperEnabled"] = true;
        settings["pt04"] = entry;
        QSignalSpy settingsSpy(&proxy, &TaidaFlowProxy::sensorSettingsSvChanged);
        proxy.setSensorSettingsSv(settings);
        QCOMPARE(proxy.sensorSettingsSv(), settings);
        QCOMPARE(settingsSpy.count(), 1);
        proxy.setSensorSettingsSv(settings);
        QCOMPARE(settingsSpy.count(), 1);
        entry["lower"] = 201.0;
        auto invalid = settings;
        invalid["pt04"] = entry;
        proxy.setSensorSettingsSv(invalid);
        QCOMPARE(proxy.sensorSettingsSv(), settings);
        entry["lower"] = std::numeric_limits<double>::infinity();
        invalid["pt04"] = entry;
        proxy.setSensorSettingsSv(invalid);
        QCOMPARE(proxy.sensorSettingsSv(), settings);
        invalid.remove("filter");
        proxy.setSensorSettingsSv(invalid);
        QCOMPARE(proxy.sensorSettingsSv(), settings);
    }
    void conversions() {
        QFile file(QStringLiteral(SOURCE_ROOT "/TaidaFlowContent/components/SensorUnits.js"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto code = QString::fromUtf8(file.readAll());
        code.remove(".pragma library");
        QJSEngine engine;
        QVERIFY(!engine.evaluate(code).isError());
        QVERIFY(qAbs(engine.evaluate("100 * pressureFactor('psi')").toNumber() - 14.503773773) < 1e-8);
        QCOMPARE(engine.evaluate("100 * pressureFactor('bar')").toNumber(), 1.0);
        QVERIFY(engine.evaluate("isNaN(parseNumber('12abc')) && isNaN(parseNumber(''))").toBool());
        QCOMPARE(engine.evaluate("display(100, 'pt04', {pt04:{offset:5}}, 'bar')").toNumber(), 1.05);
    }
    void settingsUi() {
        TaidaFlowProxy proxy;
        qmlRegisterSingletonInstance("TaidaFlowBackend", 1, 0, "Td", &proxy);
        QQmlEngine engine;
        QQmlComponent rowComponent(&engine, QUrl::fromLocalFile(QStringLiteral(SOURCE_ROOT "/TaidaFlowContent/components/SensorSettingRow.qml")));
        QScopedPointer<QObject> row(rowComponent.createWithInitialProperties({{"sensorId", "pt04"}, {"label", "PT-04"}}));
        QVERIFY2(row, qPrintable(rowComponent.errorString()));
        auto offset = row->findChild<QObject *>("offsetField");
        auto lower = row->findChild<QObject *>("lowerField");
        auto upper = row->findChild<QObject *>("upperField");
        QVERIFY(offset && lower && upper);
        offset->setProperty("text", "10");
        lower->setProperty("text", "20");
        upper->setProperty("text", "100");
        row->setProperty("dirty", true);
        proxy.setPressureUnitSv("bar");
        QCOMPARE(offset->property("text").toString(), "0.1");
        QVERIFY(QMetaObject::invokeMethod(row.data(), "save"));
        QCOMPARE(proxy.sensorSettingsSv()["pt04"].toMap()["offset"].toDouble(), 10.0);
        QCOMPARE(proxy.sensorSettingsSv()["pt04"].toMap()["upper"].toDouble(), 100.0);
        lower->setProperty("text", "2");
        QVERIFY(QMetaObject::invokeMethod(row.data(), "save"));
        QVERIFY(!row->property("errorText").toString().isEmpty());
        QCOMPARE(proxy.sensorSettingsSv()["pt04"].toMap()["lower"].toDouble(), 20.0);

        QQmlComponent page(&engine, QUrl::fromLocalFile(QStringLiteral(SOURCE_ROOT "/Core/tests/Preview.qml")));
        QScopedPointer<QObject> window(page.create());
        QVERIFY2(window, qPrintable(page.errorString()));
        QTest::qWait(300);
        auto quickWindow = qobject_cast<QQuickWindow *>(window.data());
        QVERIFY(quickWindow);
        const auto screenshot = quickWindow->grabWindow();
        QVERIFY(!screenshot.isNull());
        QVERIFY(screenshot.save("settings-preview.png"));
        auto scroll = window->findChild<QObject *>("settingsScroll");
        QVERIFY(scroll);
        auto flickable = scroll->property("contentItem").value<QObject *>();
        QVERIFY(flickable);
        flickable->setProperty("contentY", flickable->property("contentHeight").toDouble() - flickable->property("height").toDouble());
        QTest::qWait(100);
        QVERIFY(quickWindow->grabWindow().save("settings-bottom-preview.png"));
        auto navigation = window->findChild<QObject *>("testTopNav");
        QVERIFY(navigation);
        navigation->setProperty("currentPage", 0);
        proxy.setPt02ValuePv(500);
        proxy.setPt03ValuePv(200);
        proxy.setPressureUnitSv("psi");
        QTest::qWait(100);
        QVERIFY(quickWindow->grabWindow().save("main-preview.png"));
        auto filter = window->findChild<QObject *>("filterBody");
        QVERIFY(filter);
        auto settings = proxy.sensorSettingsSv();
        auto limits = settings["filter"].toMap();
        limits["lower"] = 10.0;
        limits["upper"] = 20.0;
        limits["lowerEnabled"] = true;
        limits["upperEnabled"] = true;
        settings["filter"] = limits;
        auto pt02 = settings["pt02"].toMap();
        pt02["offset"] = 2.0;
        settings["pt02"] = pt02;
        proxy.setSensorSettingsSv(settings);
        proxy.setPt03ValuePv(0);
        proxy.setPt02ValuePv(13);
        QCOMPARE(filter->property("limitState").toInt(), 0);
        QCOMPARE(filter->property("color").value<QColor>(), QColor("#858B94"));
        proxy.setPt02ValuePv(18); // Corrected value equals upper limit.
        QCOMPARE(filter->property("limitState").toInt(), 0);
        proxy.setPt02ValuePv(8); // Corrected value equals lower limit.
        QCOMPARE(filter->property("limitState").toInt(), 0);
        proxy.setPt02ValuePv(19);
        QCOMPARE(filter->property("limitState").toInt(), 1);
        QCOMPARE(filter->property("color").value<QColor>(), QColor("#D64550"));
        proxy.setPressureUnitSv("bar");
        QCOMPARE(filter->property("limitState").toInt(), 1);
        limits["upperEnabled"] = false;
        settings["filter"] = limits;
        proxy.setSensorSettingsSv(settings);
        QCOMPARE(filter->property("limitState").toInt(), 0);
        proxy.setPt02ValuePv(7);
        QCOMPARE(filter->property("limitState").toInt(), -1);
        QCOMPARE(filter->property("color").value<QColor>(), QColor("#D98A32"));
        limits["lowerEnabled"] = false;
        settings["filter"] = limits;
        proxy.setSensorSettingsSv(settings);
        QCOMPARE(filter->property("limitState").toInt(), 0);

        // Navigation only warns for rows with unapplied edits.
        QVERIFY(QMetaObject::invokeMethod(navigation, "navigateTo", Q_ARG(QVariant, 3)));
        auto editedRow = findVisualItem(quickWindow->contentItem(), "settings-pt04");
        auto secondRow = findVisualItem(quickWindow->contentItem(), "settings-tt01");
        auto dialog = window->findChild<QObject *>("unsavedSettingsDialog");
        QVERIFY(editedRow);
        QVERIFY(secondRow);
        QVERIFY(dialog);
        auto editedField = editedRow->findChild<QObject *>("offsetField");
        auto secondField = secondRow->findChild<QObject *>("offsetField");
        QVERIFY(editedField && secondField);
        editedField->setProperty("text", "0.3");
        QVERIFY(QMetaObject::invokeMethod(editedField, "textEdited"));
        secondField->setProperty("text", "5");
        QVERIFY(QMetaObject::invokeMethod(secondField, "textEdited"));
        QVERIFY(QMetaObject::invokeMethod(editedRow, "save"));
        QVERIFY(QMetaObject::invokeMethod(navigation, "navigateTo", Q_ARG(QVariant, 1)));
        QCOMPARE(navigation->property("currentPage").toInt(), 3);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QTest::qWait(200);
        QVERIFY(quickWindow->grabWindow().save("unsaved-dialog-preview.png"));
        QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(navigation->property("currentPage").toInt(), 3);
        QCOMPARE(secondField->property("text").toString(), "5");
        QVERIFY(secondRow->property("dirty").toBool());
        QVERIFY(QMetaObject::invokeMethod(navigation, "navigateTo", Q_ARG(QVariant, 2)));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(navigation->property("currentPage").toInt(), 2);
        QVERIFY(!secondRow->property("dirty").toBool());
        QCOMPARE(secondField->property("text").toString(), "0");
        QCOMPARE(proxy.sensorSettingsSv()["tt01"].toMap()["offset"].toDouble(), 0.0);
        QCOMPARE(proxy.sensorSettingsSv()["pt04"].toMap()["offset"].toDouble(), 30.0);
        QVERIFY(QMetaObject::invokeMethod(navigation, "navigateTo", Q_ARG(QVariant, 3)));
        secondField->setProperty("text", "6");
        QVERIFY(QMetaObject::invokeMethod(secondField, "textEdited"));
        QVERIFY(QMetaObject::invokeMethod(secondRow, "save"));
        QVERIFY(QMetaObject::invokeMethod(navigation, "navigateTo", Q_ARG(QVariant, 0)));
        QCOMPARE(navigation->property("currentPage").toInt(), 0);
        QVERIFY(!dialog->property("visible").toBool());
    }
};
QTEST_MAIN(SensorSettingsTest)
#include "tst_sensor_settings.moc"
