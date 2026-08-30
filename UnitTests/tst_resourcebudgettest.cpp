// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#include "pdfresourcebudget.h"

#include <QJsonObject>
#include <QtTest>

class ResourceBudgetTest final : public QObject
{
    Q_OBJECT

private slots:
    void conservativeDefaultsExposeAllPools();
    void reservationTracksResidentAndHighWater();
    void sharedReservationKeepsBudgetAlive();
    void poolAndResidentLimitsRejectWithDetails();
    void lowPriorityRejectionIsCountedAsShed();
    void jsonContainsPoolUsageAndPressure();
};

void ResourceBudgetTest::conservativeDefaultsExposeAllPools()
{
    const pdf::PDFResourceBudgetConfig config = pdf::PDFResourceBudgetConfig::conservativeDefaults();
    QCOMPARE(config.residentLimitBytes, 768 * pdf::PDFResourceBudgetConfig::MiB);
    QCOMPARE(config.limit(pdf::PDFResourcePool::ActiveDocumentModel), 256 * pdf::PDFResourceBudgetConfig::MiB);
    QCOMPARE(config.limit(pdf::PDFResourcePool::CompiledEvidenceCache), 128 * pdf::PDFResourceBudgetConfig::MiB);
    QCOMPARE(config.limit(pdf::PDFResourcePool::RasterTileCache), 128 * pdf::PDFResourceBudgetConfig::MiB);
    QCOMPARE(config.limit(pdf::PDFResourcePool::GpuTextureCache), 128 * pdf::PDFResourceBudgetConfig::MiB);
    QCOMPARE(config.limit(pdf::PDFResourcePool::DecodedStreamImageCache), 256 * pdf::PDFResourceBudgetConfig::MiB);
    QCOMPARE(config.limit(pdf::PDFResourcePool::UndoHistory), 256 * pdf::PDFResourceBudgetConfig::MiB);
    QCOMPARE(config.limit(pdf::PDFResourcePool::RollbackStorage), 2 * pdf::PDFResourceBudgetConfig::GiB);
}

void ResourceBudgetTest::reservationTracksResidentAndHighWater()
{
    pdf::PDFResourceBudgetConfig config;
    config.residentLimitBytes = 100;
    config.setLimit(pdf::PDFResourcePool::RasterTileCache, 100);
    pdf::PDFResourceBudget budget(config);

    {
        const pdf::PDFResourceReservation reservation = budget.reserve(pdf::PDFResourcePool::RasterTileCache,
                                                                       40,
                                                                       pdf::PDFResourcePriority::Visible,
                                                                       QStringLiteral("test surface"));
        Q_UNUSED(reservation);
        QCOMPARE(budget.residentBytes(), 40);
        QCOMPARE(budget.residentHighWaterBytes(), 40);
        QCOMPARE(budget.usage(pdf::PDFResourcePool::RasterTileCache).currentBytes, 40);
    }

    QCOMPARE(budget.residentBytes(), 0);
    QCOMPARE(budget.residentHighWaterBytes(), 40);
    QCOMPARE(budget.usage(pdf::PDFResourcePool::RasterTileCache).currentBytes, 0);
}

void ResourceBudgetTest::sharedReservationKeepsBudgetAlive()
{
    pdf::PDFResourceBudgetConfig config;
    config.residentLimitBytes = 100;
    config.setLimit(pdf::PDFResourcePool::RasterTileCache, 100);
    auto budget = std::make_shared<pdf::PDFResourceBudget>(config);
    const std::weak_ptr<pdf::PDFResourceBudget> weakBudget = budget;

    QVERIFY(budget->tryReserve(pdf::PDFResourcePool::RasterTileCache, 40, pdf::PDFResourcePriority::Visible));
    pdf::PDFResourceReservation reservation(budget, pdf::PDFResourcePool::RasterTileCache, 40);
    budget.reset();

    QVERIFY(!weakBudget.expired());
    QCOMPARE(reservation.bytes(), qsizetype(40));
    reservation.release();
    QVERIFY(weakBudget.expired());
}

void ResourceBudgetTest::poolAndResidentLimitsRejectWithDetails()
{
    pdf::PDFResourceBudgetConfig config;
    config.residentLimitBytes = 50;
    config.setLimit(pdf::PDFResourcePool::RasterTileCache, 30);
    config.setLimit(pdf::PDFResourcePool::DecodedStreamImageCache, 50);
    pdf::PDFResourceBudget budget(config);

    QVERIFY(budget.tryReserve(pdf::PDFResourcePool::RasterTileCache, 30, pdf::PDFResourcePriority::Visible));

    try
    {
        const auto reservation = budget.reserve(pdf::PDFResourcePool::RasterTileCache,
                                                1,
                                                pdf::PDFResourcePriority::Visible,
                                                QStringLiteral("pool overflow"));
        Q_UNUSED(reservation);
        QFAIL("a full resource pool must reject a reservation");
    }
    catch (const pdf::PDFResourceBudgetExceededException& exception)
    {
        const pdf::PDFResourceBudgetExceeded& detail = exception.detail();
        QCOMPARE(detail.pool, pdf::PDFResourcePool::RasterTileCache);
        QCOMPARE(detail.limitBytes, qsizetype(30));
        QCOMPARE(detail.currentBytes, qsizetype(30));
        QCOMPARE(detail.attemptedBytes, qsizetype(1));
        QCOMPARE(detail.context, QStringLiteral("pool overflow"));
    }

    budget.release(pdf::PDFResourcePool::RasterTileCache, 30);
    QVERIFY(budget.tryReserve(pdf::PDFResourcePool::RasterTileCache, 30, pdf::PDFResourcePriority::Visible));
    QVERIFY(!budget.tryReserve(pdf::PDFResourcePool::DecodedStreamImageCache, 21, pdf::PDFResourcePriority::Visible));
    QCOMPARE(budget.residentBytes(), qsizetype(30));
}

void ResourceBudgetTest::lowPriorityRejectionIsCountedAsShed()
{
    pdf::PDFResourceBudgetConfig config;
    config.setLimit(pdf::PDFResourcePool::RasterTileCache, 10);
    pdf::PDFResourceBudget budget(config);

    QVERIFY(budget.tryReserve(pdf::PDFResourcePool::RasterTileCache, 10, pdf::PDFResourcePriority::Visible));
    QVERIFY(!budget.tryReserve(pdf::PDFResourcePool::RasterTileCache,
                               1,
                               pdf::PDFResourcePriority::Prefetch,
                               QStringLiteral("prefetch")));
    QCOMPARE(budget.usage(pdf::PDFResourcePool::RasterTileCache).shed, qint64(1));
}

void ResourceBudgetTest::jsonContainsPoolUsageAndPressure()
{
    pdf::PDFResourceBudget budget;
    const QJsonObject json = budget.toJson();
    QVERIFY(json.value(QStringLiteral("config")).toObject().contains(QStringLiteral("pool_limits_bytes")));
    QVERIFY(json.value(QStringLiteral("pools")).toObject().contains(QStringLiteral("active-document-model")));
    QVERIFY(json.value(QStringLiteral("pools")).toObject().contains(QStringLiteral("rollback-storage")));
    QCOMPARE(json.value(QStringLiteral("resident_bytes")).toInteger(), qint64(0));
    QCOMPARE(json.value(QStringLiteral("pressure")).toString(), QStringLiteral("normal"));
}

QTEST_GUILESS_MAIN(ResourceBudgetTest)
#include "tst_resourcebudgettest.moc"
