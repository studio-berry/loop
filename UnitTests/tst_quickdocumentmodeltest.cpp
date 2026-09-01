// MIT License
#include "quickdocumentmodel.h"

#include <QSignalSpy>
#include <QtTest>

class QuickDocumentModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void emptyModelIsSafe();
    void searchResultsExposeOnlyValueRoles();
};

void QuickDocumentModelTest::emptyModelIsSafe()
{
    QuickDocumentModel model;

    QCOMPARE(model.pages()->rowCount(), 0);
    QCOMPARE(model.outline()->rowCount(), 0);
    QCOMPARE(model.searchResults()->rowCount(), 0);
    QVERIFY(!model.hasOutline());
    QVERIFY(!model.hasAttachments());
    QVERIFY(!model.hasOptionalContent());
    QVERIFY(!model.search(QStringLiteral("text")));
}

void QuickDocumentModelTest::searchResultsExposeOnlyValueRoles()
{
    QuickSearchResultModel model;
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

    model.replace({{3, QStringLiteral("match"), QStringLiteral("before match after")}},
                  QStringLiteral("match"), QStringLiteral("revision"));

    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(model.rowCount(), 1);
    const QModelIndex index = model.index(0, 0);
    QCOMPARE(model.data(index, QuickSearchResultModel::PageRole).toInt(), 3);
    QCOMPARE(model.data(index, QuickSearchResultModel::MatchedRole).toString(), QStringLiteral("match"));
    QCOMPARE(model.data(index, QuickSearchResultModel::ContextRole).toString(), QStringLiteral("before match after"));
    QCOMPARE(model.query(), QStringLiteral("match"));
    QCOMPARE(model.revision(), QStringLiteral("revision"));
}

QTEST_GUILESS_MAIN(QuickDocumentModelTest)
#include "tst_quickdocumentmodeltest.moc"
