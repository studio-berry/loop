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

#ifndef PDFPAGESURFACECACHE_P_H
#define PDFPAGESURFACECACHE_P_H

#include "pdfdocumentcontext.h"
#include "pdfpage.h"

#include <QImage>
#include <QtGlobal>

#include <map>
#include <optional>

namespace pdf
{

struct PDFPageSurfaceKey
{
    PDFRevisionIdentity revision;
    PDFInteger pageIndex = -1;
    PageRotation rotation = PageRotation::None;
    int featureBits = 0;
    QSize targetPixelSize;
    int devicePixelRatio1000 = 1000;

    bool operator<(const PDFPageSurfaceKey& other) const;
    bool compatibleWith(const PDFPageSurfaceKey& desired) const;
};

struct PDFPageSurfaceLookup
{
    QImage image;
    bool exact = false;
};

/// Splits the single public cache limit between compiled pages and surfaces.
struct PDFPageCacheBudget final
{
    static qsizetype total(qsizetype requested) { return qMax<qsizetype>(0, requested); }
    static qsizetype compiledPages(qsizetype requested) { return total(requested) / 2; }
    static qsizetype pageSurfaces(qsizetype requested)
    {
        const qsizetype normalized = total(requested);
        return normalized - compiledPages(normalized);
    }
};

/// Revision-fenced, bounded cache for rendered page surfaces.
class PDFPageSurfaceCache final
{
public:
    explicit PDFPageSurfaceCache(qsizetype byteBudget);

    void setByteBudget(qsizetype byteBudget);
    void clear();
    quint64 beginRequest();

    std::optional<PDFPageSurfaceLookup> lookup(const PDFPageSurfaceKey& desired);
    bool insert(const PDFPageSurfaceKey& key, quint64 generation, QImage image);

private:
    struct Entry
    {
        PDFPageSurfaceKey key;
        QImage image;
        qsizetype cost = 0;
        quint64 accessSequence = 0;
    };

    void trimToBudget();

    qsizetype m_byteBudget = 0;
    qsizetype m_currentBytes = 0;
    quint64 m_generation = 0;
    quint64 m_accessSequence = 0;
    std::map<PDFPageSurfaceKey, Entry> m_entries;
};

}   // namespace pdf

#endif   // PDFPAGESURFACECACHE_P_H
