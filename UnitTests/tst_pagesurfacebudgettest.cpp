// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pdfpagesurfacecache_p.h"

#include <QtTest>

class PageSurfaceBudgetTest final : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void partitionsSingleLimit();
};

void PageSurfaceBudgetTest::partitionsSingleLimit()
{
    QCOMPARE(pdf::PDFPageCacheBudget::compiledPages(-1), qsizetype(0));
    QCOMPARE(pdf::PDFPageCacheBudget::pageSurfaces(-1), qsizetype(0));

    for (const qsizetype total : { qsizetype(0), qsizetype(1), qsizetype(100), qsizetype(101) })
    {
        const qsizetype compiled = pdf::PDFPageCacheBudget::compiledPages(total);
        const qsizetype surfaces = pdf::PDFPageCacheBudget::pageSurfaces(total);
        QCOMPARE(compiled + surfaces, total);
    }
}

QTEST_APPLESS_MAIN(PageSurfaceBudgetTest)
#include "tst_pagesurfacebudgettest.moc"
