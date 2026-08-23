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

#ifndef PDFREPAIRDIFF_H
#define PDFREPAIRDIFF_H

#include "pdfdocument.h"
#include "pdfoperationcontrol.h"
#include "pdfutils.h"

#include <QJsonObject>
#include <QSet>
#include <QImage>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QStringList>
#include <QVector>

#include <functional>

namespace pdf
{

enum class PDFRepairDiffStatus
{
    Complete,
    CompleteWithWarnings,
    Incomplete,
    Failed
};

enum class PDFRepairChangeClass
{
    Expected,
    Unexpected,
    Informational
};

struct LOUPELIBCORESHARED_EXPORT PDFRepairExpectedChanges
{
    bool pageBoxes = false;
    bool pageContent = false;
    bool images = false;
    bool fonts = false;
    bool colorSpaces = false;
    bool outputIntent = false;
    bool metadata = false;
    bool annotations = false;
    bool signatures = false;
};

struct LOUPELIBCORESHARED_EXPORT PDFRepairAllowedRegion
{
    int pageIndex = -1;
    QRectF pageRect;
    QString reason;
};

struct LOUPELIBCORESHARED_EXPORT PDFRepairChangeManifest
{
    QString repairId;
    QJsonObject parameters;
    QSet<int> affectedPages;
    QStringList affectedSemanticPaths;
    QVector<PDFRepairAllowedRegion> allowedRegions;
    PDFRepairExpectedChanges expected;
};

struct LOUPELIBCORESHARED_EXPORT PDFRepairDiffOptions
{
    int renderDpi = 144;
    bool renderVisualDiff = true;
    bool compareMetadata = true;
    bool compareResources = true;
    bool compareAnnotations = true;
    int maxRenderedPages = 200;
    qint64 maxRenderPixels = 250'000'000;
    int channelTolerance = 2;
    QString renderDirectory;
    PDFRepairExpectedChanges expected;
    QVector<int> affectedPages;
    QVector<PDFRepairAllowedRegion> allowedRegions;
    const PDFOperationControl* operationControl = nullptr;
};

struct LOUPELIBCORESHARED_EXPORT PDFRepairPageVisualDiff
{
    int pageIndex = -1;
    QSize pixelSize;
    QString beforeFingerprint;
    QString afterFingerprint;
    quint64 strictChangedPixelCount = 0;
    quint64 changedPixelCount = 0;
    quint64 unexpectedChangedPixelCount = 0;
    double changedPixelRatio = 0.0;
    double meanAbsoluteDelta = 0.0;
    int maxChannelDelta = 0;
    QRect changedPixelBounds;
    bool commonRegionCompared = false;
    QStringList warnings;
    QString beforeImagePath;
    QString afterImagePath;
    QString diffImagePath;
};

struct LOUPELIBCORESHARED_EXPORT PDFRepairStructuralChange
{
    QString path;
    QString kind;
    QString beforeValue;
    QString afterValue;
    PDFRepairChangeClass classification = PDFRepairChangeClass::Informational;
};

struct LOUPELIBCORESHARED_EXPORT PDFRepairDiffReport
{
    int schemaVersion = 1;
    PDFRepairDiffStatus status = PDFRepairDiffStatus::Complete;
    QString sourceFingerprint;
    QString candidateFingerprint;
    QVector<PDFRepairPageVisualDiff> pages;
    QVector<PDFRepairStructuralChange> structuralChanges;
    QStringList warnings;
    QStringList incompleteReasons;

    QJsonObject toJson() const;
};

inline int unexpectedChangeCount(const PDFRepairDiffReport& report)
{
    int count = 0;
    for (const PDFRepairStructuralChange& change : report.structuralChanges)
    {
        count += change.classification == PDFRepairChangeClass::Unexpected;
    }
    for (const PDFRepairPageVisualDiff& page : report.pages)
    {
        count += page.unexpectedChangedPixelCount > 0;
    }
    return count;
}

/// Compares two documents without relying on indirect object numbers or xref
/// offsets. The comparison is deterministic and safe to persist as a report.
class LOUPELIBCORESHARED_EXPORT PDFRepairDiffEngine
{
public:
    static PDFOperationResult compare(const PDFDocument& before,
                                      const PDFDocument& after,
                                      const PDFRepairDiffOptions& options,
                                      PDFRepairDiffReport* report);

    /// Applies a repair to a copy, writes the copy to an isolated path, and
    /// reopens the serialized candidate. The source document is never mutated.
    static PDFOperationResult buildSerializedCandidate(
        const PDFDocument& source,
        const std::function<PDFOperationResult(PDFDocument*)>& applyRepair,
        const QString& candidatePath,
        PDFDocument* reopenedCandidate,
        QByteArray* candidateSha256 = nullptr);
};

LOUPELIBCORESHARED_EXPORT QString pdfRepairDiffStatusName(PDFRepairDiffStatus status);
LOUPELIBCORESHARED_EXPORT QString pdfRepairChangeClassName(PDFRepairChangeClass changeClass);

} // namespace pdf

#endif // PDFREPAIRDIFF_H
