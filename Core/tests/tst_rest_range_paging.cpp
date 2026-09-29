// w2-071 D1/D2/D5 (review D-001): the REST range routes of RESTManager, against the real
// SqlManager with a temporary data folder (two month files, 250 rows each, holding registers
// too) and a REST server on a free local port.
//
//  * all six range routes answer the paged object {page, pageSize, totalCount, totalPages,
//    hasPreviousPage, hasNextPage, items}; defaults page 1 / pageSize 200; pageSize > 1000 is
//    clamped to 1000; rows oldest first across the month boundary;
//  * page / pageSize above INT_MAX (e.g. 4294967297, which the former static_cast<int> turned
//    into 1) -> 400 on every route; page 0, text, "9223372036854775807+1" -> 400;
//  * from/to outside 0 .. 253402300799 (9999-12-31T23:59:59Z) -> 400 in well under 1 s
//    (to=9200000000000000, from=0&to=100000000000000, negative from);
//  * SqlManager itself (no REST guard): queryRangeJsonPaged / countSensorRange over
//    0 .. 9200000000000000 and 0 .. LLONG_MAX return at once (the month files are listed,
//    no calendar-month loop) with the same rows as the exact range.
#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#include <limits>

#include "RESTManager.h"
#include "SqlManager.h"

namespace {
constexpr int kRowsPerMonth = 250;

qint64 julyStart() { return QDateTime(QDate(2026, 7, 20), QTime(0, 0)).toSecsSinceEpoch(); }
qint64 augustStart() { return QDateTime(QDate(2026, 8, 10), QTime(0, 0)).toSecsSinceEpoch(); }
// The whole data: both months (local time, like the month files).
qint64 rangeFrom() { return QDateTime(QDate(2026, 7, 1), QTime(0, 0)).toSecsSinceEpoch(); }
qint64 rangeTo() { return QDateTime(QDate(2026, 8, 31), QTime(23, 59, 59)).toSecsSinceEpoch(); }

struct Reply
{
    int status = 0;
    QJsonObject object;
    QByteArray body;
    qint64 ms = 0;
};
} // namespace

class TestRestRangePaging : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void pagedFormat_data();
    void pagedFormat();
    void pageAboveIntMax_data();
    void pageAboveIntMax();
    void invalidPaging();
    void rangeOutOfBounds_data();
    void rangeOutOfBounds();
    void sameItemsAllRoutes();
    void sqlManagerHugeRangeFast();

private:
    Reply get(const QString &pathAndQuery);
    QString exactRangeQuery(bool iso) const;

    QTemporaryDir m_dir;
    QString m_oldCwd;
    std::unique_ptr<RESTManager> m_rest;
    QNetworkAccessManager m_nam;
    quint16 m_port = 0;
};

void TestRestRangePaging::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_oldCwd = QDir::currentPath();
    // SqlManager takes its schema file paths from the current folder when it is created.
    QVERIFY(QDir::setCurrent(m_dir.path()));
    SqlManager *sql = SqlManager::instance();
    sql->setDataDirectory(m_dir.filePath(QStringLiteral("data")));
    sql->setSettingsFile(m_dir.filePath(QStringLiteral("settings.sqlite")));
    QVERIFY(sql->initialize());

    for (int month = 0; month < 2; ++month) {
        const qint64 start = month == 0 ? julyStart() : augustStart();
        for (int i = 0; i < kRowsPerMonth; ++i) {
            const int n = month * kRowsPerMonth + i;
            QVector<double> readings;
            for (int s = 0; s < 40; ++s)
                readings << n + s / 100.0;
            QVector<quint16> holdings;
            for (int h = 0; h < 100; ++h)
                holdings << quint16((n + h) % 65536);
            QVERIFY(sql->saveSensorData(QDateTime::fromSecsSinceEpoch(start + i * 60), readings, holdings));
        }
    }
    QVERIFY(QFileInfo::exists(m_dir.filePath(QStringLiteral("data/sensor_202607.sqlite"))));
    QVERIFY(QFileInfo::exists(m_dir.filePath(QStringLiteral("data/sensor_202608.sqlite"))));

    m_rest = std::make_unique<RESTManager>(sql);
    QVERIFY(m_rest->start(0, QHostAddress(QHostAddress::LocalHost)));
    m_port = m_rest->serverPort();
    QVERIFY(m_port != 0);
    qInfo("REST test server on 127.0.0.1:%u, data %s", m_port, qPrintable(m_dir.path()));
}

void TestRestRangePaging::cleanupTestCase()
{
    m_rest.reset();
    SqlManager::instance()->shutdown();
    QDir::setCurrent(m_oldCwd);
}

Reply TestRestRangePaging::get(const QString &pathAndQuery)
{
    Reply r;
    QElapsedTimer timer;
    timer.start();
    QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_port).arg(pathAndQuery)));
    QNetworkReply *reply = m_nam.get(request);
    QSignalSpy finished(reply, &QNetworkReply::finished);
    if (!reply->isFinished())
        finished.wait(30000);
    r.ms = timer.elapsed();
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    r.body = reply->readAll();
    r.object = QJsonDocument::fromJson(r.body).object();
    reply->deleteLater();
    return r;
}

QString TestRestRangePaging::exactRangeQuery(bool iso) const
{
    if (iso) {
        return QStringLiteral("from=%1&to=%2")
                .arg(QDateTime::fromSecsSinceEpoch(rangeFrom()).toString(Qt::ISODate),
                     QDateTime::fromSecsSinceEpoch(rangeTo()).toString(Qt::ISODate));
    }
    return QStringLiteral("from=%1&to=%2").arg(rangeFrom()).arg(rangeTo());
}

static const QStringList kRoutes{
    QStringLiteral("/api/sensor/range"), QStringLiteral("/api/holding/range"),
    QStringLiteral("/api/sensor/rangeDateTime"), QStringLiteral("/api/holding/rangeDateTime"),
    QStringLiteral("/api/sensor/rangeDateTimePage"), QStringLiteral("/api/holding/rangeDateTimePage")};

void TestRestRangePaging::pagedFormat_data()
{
    QTest::addColumn<QString>("route");
    QTest::addColumn<QString>("paging");
    QTest::addColumn<int>("page");
    QTest::addColumn<int>("pageSize");
    QTest::addColumn<int>("items");
    QTest::addColumn<int>("firstRow");      // 0-based row number of items[0] (oldest first)
    QTest::addColumn<bool>("hasPrevious");
    QTest::addColumn<bool>("hasNext");
    for (const QString &route : kRoutes) {
        const QByteArray r = route.toLatin1();
        QTest::addRow("%s default", r.constData()) << route << QString() << 1 << 200 << 200 << 0 << false << true;
        QTest::addRow("%s page 2 (month boundary)", r.constData()) << route << QStringLiteral("&page=2") << 2 << 200 << 200 << 200 << true << true;
        QTest::addRow("%s page 3 (last)", r.constData()) << route << QStringLiteral("&page=3") << 3 << 200 << 100 << 400 << true << false;
        QTest::addRow("%s page 4 (past the end)", r.constData()) << route << QStringLiteral("&page=4") << 4 << 200 << 0 << -1 << true << false;
        QTest::addRow("%s pageSize 5000 -> 1000", r.constData()) << route << QStringLiteral("&pageSize=5000") << 1 << 1000 << 500 << 0 << false << false;
        QTest::addRow("%s pageSize 7 page 36", r.constData()) << route << QStringLiteral("&page=36&pageSize=7") << 36 << 7 << 7 << 245 << true << true;
        QTest::addRow("%s page INT_MAX", r.constData()) << route << QStringLiteral("&page=2147483647") << 2147483647 << 200 << 0 << -1 << true << false;
    }
}

void TestRestRangePaging::pagedFormat()
{
    QFETCH(QString, route);
    QFETCH(QString, paging);
    QFETCH(int, page);
    QFETCH(int, pageSize);
    QFETCH(int, items);
    QFETCH(int, firstRow);
    QFETCH(bool, hasPrevious);
    QFETCH(bool, hasNext);

    const bool iso = route.contains(QLatin1String("DateTime"));
    const bool holding = route.contains(QLatin1String("holding"));
    const Reply r = get(route + QLatin1Char('?') + exactRangeQuery(iso) + paging);
    QCOMPARE(r.status, 200);
    QStringList keys = r.object.keys();
    keys.sort();
    QCOMPARE(keys, (QStringList{"hasNextPage", "hasPreviousPage", "items", "page", "pageSize",
                                "totalCount", "totalPages"}));
    QCOMPARE(r.object.value("page").toInteger(), qint64(page));
    QCOMPARE(r.object.value("pageSize").toInteger(), qint64(pageSize));
    QCOMPARE(r.object.value("totalCount").toInteger(), qint64(2 * kRowsPerMonth));
    QCOMPARE(r.object.value("totalPages").toInteger(), qint64((2 * kRowsPerMonth + pageSize - 1) / pageSize));
    QCOMPARE(r.object.value("hasPreviousPage").toBool(), hasPrevious);
    QCOMPARE(r.object.value("hasNextPage").toBool(), hasNext);
    const QJsonArray array = r.object.value("items").toArray();
    QCOMPARE(array.size(), qsizetype(items));
    for (qsizetype i = 0; i < array.size(); ++i) {
        const int n = firstRow + int(i);
        const qint64 ts = n < kRowsPerMonth ? julyStart() + n * 60 : augustStart() + (n - kRowsPerMonth) * 60;
        const QJsonObject row = array.at(i).toObject();
        QCOMPARE(row.value("ts").toInteger(), ts);
        if (holding) {
            QCOMPARE(row.size(), qsizetype(101));
            QCOMPARE(row.value("h1").toInteger(), qint64(n % 65536));
            QCOMPARE(row.value("h100").toInteger(), qint64((n + 99) % 65536));
        } else {
            QCOMPARE(row.size(), qsizetype(41));
            QCOMPARE(row.value("s1").toDouble(), double(n));
            QCOMPARE(row.value("s40").toDouble(), n + 39 / 100.0);
        }
    }
}

void TestRestRangePaging::pageAboveIntMax_data()
{
    QTest::addColumn<QString>("route");
    QTest::addColumn<QString>("paging");
    for (const QString &route : kRoutes) {
        const QByteArray r = route.toLatin1();
        QTest::addRow("%s page=4294967297", r.constData()) << route << QStringLiteral("&page=4294967297");
        QTest::addRow("%s page=2147483648", r.constData()) << route << QStringLiteral("&page=2147483648");
        QTest::addRow("%s pageSize=4294967297", r.constData()) << route << QStringLiteral("&pageSize=4294967297");
        QTest::addRow("%s page=4294967296+1", r.constData()) << route << QStringLiteral("&page=4294967296+1");
    }
}

void TestRestRangePaging::pageAboveIntMax()
{
    QFETCH(QString, route);
    QFETCH(QString, paging);
    const bool iso = route.contains(QLatin1String("DateTime"));
    const Reply r = get(route + QLatin1Char('?') + exactRangeQuery(iso) + paging);
    QCOMPARE(r.status, 400);
    QCOMPARE(r.object.value("ok").toBool(true), false);
    QVERIFY2(r.object.value("error").toString().contains(QLatin1String("at most 2147483647")), r.body.constData());
}

void TestRestRangePaging::invalidPaging()
{
    for (const QString &route : kRoutes) {
        const bool iso = route.contains(QLatin1String("DateTime"));
        const QString base = route + QLatin1Char('?') + exactRangeQuery(iso);
        QCOMPARE(get(base + QStringLiteral("&page=0")).status, 400);
        QCOMPARE(get(base + QStringLiteral("&pageSize=0")).status, 400);
        QCOMPARE(get(base + QStringLiteral("&page=abc")).status, 400);
        QCOMPARE(get(base + QStringLiteral("&page=-1")).status, 400);
        // Overflow of the "a+b" form: refused instead of wrapping around.
        QCOMPARE(get(base + QStringLiteral("&page=9223372036854775807+1")).status, 400);
        // (control: the "a+b" form itself still works)
        const Reply sum = get(base + QStringLiteral("&page=1+1"));
        QCOMPARE(sum.status, 200);
        QCOMPARE(sum.object.value("page").toInteger(), 2LL);
    }
}

void TestRestRangePaging::rangeOutOfBounds_data()
{
    QTest::addColumn<QString>("route");
    QTest::addColumn<QString>("query");
    for (const QString &route : kRoutes) {
        const QByteArray r = route.toLatin1();
        QTest::addRow("%s to=9200000000000000", r.constData()) << route << QStringLiteral("from=0&to=9200000000000000");
        QTest::addRow("%s from=0&to=100000000000000", r.constData()) << route << QStringLiteral("from=0&to=100000000000000");
        QTest::addRow("%s to=253402300800", r.constData()) << route << QStringLiteral("from=0&to=253402300800");
        QTest::addRow("%s both huge", r.constData()) << route << QStringLiteral("from=9000000000000000&to=9200000000000000");
        QTest::addRow("%s from > to", r.constData()) << route << QStringLiteral("from=200&to=100");
        // DateTime routes accept negative epoch seconds (-> out of range); range routes do not parse "-5".
        QTest::addRow("%s from=-5", r.constData()) << route << QStringLiteral("from=-5&to=100");
    }
}

void TestRestRangePaging::rangeOutOfBounds()
{
    QFETCH(QString, route);
    QFETCH(QString, query);
    const Reply r = get(route + QLatin1Char('?') + query);
    QCOMPARE(r.status, 400);
    QCOMPARE(r.object.value("ok").toBool(true), false);
    qInfo("%s?%s -> %d in %lld ms: %s", qPrintable(route), qPrintable(query), r.status, r.ms, r.body.constData());
    QVERIFY2(r.ms < 1000, qPrintable(QStringLiteral("took %1 ms").arg(r.ms)));
}

void TestRestRangePaging::sameItemsAllRoutes()
{
    // The six routes return the same rows for the same range (sensor and holding separately).
    for (const QString &kind : {QStringLiteral("sensor"), QStringLiteral("holding")}) {
        QJsonArray reference;
        for (const QString &route : kRoutes) {
            if (!route.contains(kind))
                continue;
            const bool iso = route.contains(QLatin1String("DateTime"));
            const Reply r = get(route + QLatin1Char('?') + exactRangeQuery(iso) + QStringLiteral("&pageSize=1000"));
            QCOMPARE(r.status, 200);
            const QJsonArray items = r.object.value("items").toArray();
            QCOMPARE(items.size(), qsizetype(2 * kRowsPerMonth));
            if (reference.isEmpty())
                reference = items;
            else
                QVERIFY(items == reference);
        }
    }
}

void TestRestRangePaging::sqlManagerHugeRangeFast()
{
    SqlManager *sql = SqlManager::instance();
    QJsonArray exact;
    QVERIFY(sql->queryRangeJsonPaged(rangeFrom(), rangeTo(), 1, 1000, &exact));
    QCOMPARE(exact.size(), qsizetype(2 * kRowsPerMonth));

    const qint64 hugeTo[] = {9200000000000000LL, 100000000000000LL, std::numeric_limits<qint64>::max()};
    for (const qint64 to : hugeTo) {
        QElapsedTimer timer;
        timer.start();
        QJsonArray rows;
        QString error;
        QVERIFY2(sql->queryRangeJsonPaged(0, to, 1, 1000, &rows, &error), qPrintable(error));
        qint64 total = -1;
        QVERIFY2(sql->countSensorRange(0, to, &total, &error), qPrintable(error));
        qint64 holdingTotal = -1;
        QVERIFY2(sql->countHoldingRange(0, to, &holdingTotal, &error), qPrintable(error));
        QJsonArray holdingRows;
        QVERIFY2(sql->queryHoldingRangeJsonPaged(0, to, 2, 100, &holdingRows, &error), qPrintable(error));
        const qint64 ms = timer.elapsed();
        qInfo("SqlManager 0 .. %lld: %lld rows (page of %lld), holding %lld, %lld ms", to, total,
              qint64(rows.size()), holdingTotal, ms);
        QCOMPARE(total, qint64(2 * kRowsPerMonth));
        QCOMPARE(holdingTotal, qint64(2 * kRowsPerMonth));
        QVERIFY(rows == exact);
        QCOMPARE(holdingRows.size(), qsizetype(100));
        QVERIFY2(ms < 1000, qPrintable(QStringLiteral("took %1 ms").arg(ms)));
    }
}

QTEST_GUILESS_MAIN(TestRestRangePaging)
#include "tst_rest_range_paging.moc"
