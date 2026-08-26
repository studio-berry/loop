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

#ifndef PAGESURFACEKEY_H
#define PAGESURFACEKEY_H

#include "interactionglobal.h"

#include "pdfdocumentcontext.h"
#include "pdfjobscheduler.h"
#include "pdfpage.h"
#include "pdfrenderer.h"

#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>

#include <memory>

namespace pdfinteraction
{

/// A generation paired with the revision it was taken against.
///
/// The two fences answer different questions and neither one is sufficient
/// alone. pdf::PDFRevisionIdentity says which document state the work was
/// requested for; the generation says whether the requester still wants it, which
/// changes on viewport demand that no document mutation accompanies. A zoom
/// change supersedes prior surface demand without touching the revision, and a
/// document replacement changes the revision without the viewport moving.
///
/// This is the shared primitive for revision-fenced transient work. P4-S4's
/// interaction state needs the same pair and must reuse this rather than declare
/// its own.
struct RevisionFencedToken
{
    quint64 generation = 0;
    pdf::PDFRevisionIdentity revision;

    bool isValid() const { return revision.isValid(); }
    bool operator==(const RevisionFencedToken& other) const = default;
};

/// Everything that can change a page's pixels, and nothing that cannot.
///
/// Two rules make this type work, and both are easy to lose:
///
/// The revision is embedded as a complete pdf::PDFRevisionIdentity value, never
/// as a subset of its fields. Comparing part of the fence and reconciling the
/// rest heuristically is what the fence exists to prevent.
///
/// Every field is exact-comparable. The device pixel ratio is stored as an
/// integer thousandth rather than a qreal, and zoom enters as a bucket index
/// rather than a scale factor, so equality cannot drift between two keys that a
/// float comparison would call different. makePageSurfaceKey() is the only place
/// those normalizations happen; a key assembled field-by-field elsewhere can
/// disagree with an equal one built here, which is exactly the admission bug the
/// full-key rule is meant to stop.
struct PageSurfaceKey
{
    pdf::PDFRevisionIdentity revision;
    int pageIndex = -1;
    pdf::PageRotation rotation = pdf::PageRotation::None;

    /// static_cast<int>(pdf::PDFRenderer::Features). Held as bits rather than the
    /// flags type so the key stays trivially comparable and orderable. It also
    /// carries the five ColorAdjust_* post-processing modes, which change pixels.
    int featureBits = 0;

    /// Identity of the colour-managed output path: the pdf::PDFCMSSettings in
    /// force plus any document output intent. It is a digest rather than the
    /// settings struct because the struct is large, and because proof and
    /// output-preview modes (P4-S9) join the same string instead of adding an
    /// enum with one live value today.
    QString colorOutputIdentity;

    /// Quantized zoom. Two nearby zoom levels share a bucket on purpose: the
    /// bucket decides which cached surface may be reused, targetPixelSize decides
    /// what a fresh render produces.
    int zoomBucket = 0;

    QSize targetPixelSize;

    /// Device pixel ratio times 1000. Integral so two keys either match or do not.
    int devicePixelRatio1000 = 1000;

    /// Tile bounds in page space. A null rect means the whole page.
    QRectF pageTileBounds;

    bool operator==(const PageSurfaceKey& other) const = default;
    bool operator<(const PageSurfaceKey& other) const;

    /// True when a surface built for this key may stand in for `desired` while a
    /// fidelity render is in flight: same document state, page, geometry and
    /// output path, differing only in resolution. zoomBucket and targetPixelSize
    /// are the two fields deliberately excluded.
    bool compatibleWith(const PageSurfaceKey& desired) const;

    bool isValid() const;
};

/// The only supported way to build a key. See PageSurfaceKey for why.
PageSurfaceKey makePageSurfaceKey(const pdf::PDFRevisionIdentity& revision,
                                  int pageIndex,
                                  pdf::PageRotation rotation,
                                  pdf::PDFRenderer::Features features,
                                  const QString& colorOutputIdentity,
                                  qreal zoom,
                                  QSize targetPixelSize,
                                  qreal devicePixelRatio,
                                  QRectF pageTileBounds = QRectF());

/// The zoom quantizer used by makePageSurfaceKey, exposed so a test can pin it
/// rather than re-derive it.
int zoomBucketFor(qreal zoom);

/// How a page surface request ended. Cancelled, Failed and BudgetExhausted are
/// distinct and none of them is success, the rule architecture invariant I05
/// pins for pdf::PDFJobScheduler. Stale is not a failure either: it is a correct
/// result for a demand nobody holds any more.
enum class SurfaceTerminalState
{
    Complete,
    Cancelled,
    Failed,
    Stale,
    BudgetExhausted
};

const char* getSurfaceTerminalStateName(SurfaceTerminalState state);

/// Rendered pixels, owned by C++ and shared immutably.
///
/// Never a QML property, JS value, QByteArray context property, or URL. It
/// crosses C++ ownership boundaries only, and the canvas consumes it after
/// admission. Held by shared_ptr<const SurfaceBuffer> everywhere so an admitted
/// snapshot cannot be edited underneath its reader.
struct SurfaceBuffer
{
    QImage image;
    qint64 byteSize = 0;
};

using SurfaceBufferPointer = std::shared_ptr<const SurfaceBuffer>;

SurfaceBufferPointer makeSurfaceBuffer(QImage image);

/// An immutable unit of work. Priority is a pdf::PDFJobPriority so there is no
/// second priority scheme; the coordinator maps viewport demand onto the classes
/// the scheduler already has.
struct PageSurfaceRequest
{
    PageSurfaceKey key;
    RevisionFencedToken token;
    pdf::PDFJobPriority priority = pdf::PDFJobPriority::VisiblePage;
};

/// A terminal outcome. `pixels` is set only for Complete.
struct PageSurfaceResult
{
    PageSurfaceKey key;
    RevisionFencedToken token;
    SurfaceTerminalState state = SurfaceTerminalState::Failed;
    SurfaceBufferPointer pixels;
    QSize pixelSize;
    qint64 byteSize = 0;

    /// A <domain>/<kebab-reason> code, never a path or document content.
    QString typedError;
};

}   // namespace pdfinteraction

Q_DECLARE_METATYPE(pdfinteraction::PageSurfaceKey)
Q_DECLARE_METATYPE(pdfinteraction::SurfaceTerminalState)

#endif   // PAGESURFACEKEY_H
